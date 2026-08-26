/*
 * wasapi_common.h -- everything the process tap and the device capture share.
 *
 * Design section 4 gives them different activation paths and different clock
 * behaviour, but the middle of the job -- pick a format, Initialize, take an
 * event handle, run a pump thread, drain packets into a RingBuf -- is the same
 * code. It lives here once. capture_process.c and capture_device.c supply only
 * what genuinely differs: how the IAudioClient is obtained, which stream flags
 * it needs, and what to do on each pump tick.
 *
 * ===========================================================================
 * THE CAPTURE OWNS ITS APARTMENT. THE CALLER'S APARTMENT IS NOT ITS BUSINESS.
 *
 *   Process loopback requires the MTA (design 4.1 note 7) and the device path
 *   wants it for the same reason. The windowed front end's thread is an STA and
 *   has to be -- IAccPropServices, which supplies every control's accessible
 *   name, is created in that apartment and is valid only on the thread that
 *   created it (src/uiapp/main.c). Both requirements are correct and they
 *   cannot both hold on one thread.
 *
 *   An earlier arrangement resolved that by calling CoInitializeEx(MTA) on
 *   whatever thread invoked open(), which made apr_capture_create() fail with
 *   RPC_E_CHANGED_MODE for every STA caller. The CLI never noticed, because its
 *   thread was already MTA; the GUI could not add a single source.
 *
 *   A capture already owns a thread, so it now owns its apartment as well.
 *   apr_wasapi_thread_start() creates that thread and puts it in the MTA once;
 *   every COM call the capture makes -- activation, Initialize, the pump, the
 *   mute poll, and every Release -- runs on it. open/start/stop/close marshal
 *   there and wait. That is what lets include/capture.h promise what a library
 *   owes its callers: apr_capture_create() works from ANY apartment, and the
 *   old "open() and close() must share a thread" constraint is gone with it.
 * ===========================================================================
 */
#ifndef APPRECORDER_CAPTURE_WASAPI_COMMON_H
#define APPRECORDER_CAPTURE_WASAPI_COMMON_H

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <stdint.h>

#include "err.h"
#include "clock.h"
#include "ringbuf.h"
#include "capture_internal.h"

/* GUIDs, defined once in wasapi_common.c. apr_-prefixed so they cannot collide
 * with the identically-named symbols exported by uuid.lib / mmdevapi.lib. */
extern const GUID apr_clsid_MMDeviceEnumerator;
extern const GUID apr_iid_IMMDeviceEnumerator;
extern const GUID apr_iid_IAudioClient;
extern const GUID apr_iid_IAudioCaptureClient;
extern const GUID apr_iid_IAudioSessionManager2;
extern const GUID apr_iid_IAudioSessionControl2;
extern const GUID apr_iid_ISimpleAudioVolume;
extern const GUID apr_ksdataformat_subtype_ieee_float;

/* Called once per pump wakeup, on the pump thread, after packets are drained.
 * Used for the out-of-band checks WASAPI will not do for us: is the target
 * process still alive, is its session muted (design section 10). Must not
 * block for long -- it runs between audio packets. */
typedef void (*AprWasapiTick)(void *user);

/* A unit of work that must happen inside the capture's own apartment. Run by
 * apr_wasapi_call() on the capture thread; see the header comment. */
typedef AprErr (*AprWasapiJob)(void *user);

typedef struct AprWasapiStream {
    IAudioClient        *ac;      /* owned once apr_wasapi_adopt is called */
    IAudioCaptureClient *cc;
    HANDLE   ev;                  /* WASAPI event, auto-reset */
    HANDLE   stop_ev;             /* manual reset; the only way the pump exits */

    /* The capture thread. It IS the apartment, and it IS the pump: between
     * start() and stop() it runs the drain loop, and the rest of the time it
     * waits for jobs marshalled to it from whatever thread the caller is on. */
    HANDLE   thread;
    DWORD    thread_id;
    HANDLE   ready_ev;            /* manual; set once the apartment is entered */
    HANDLE   job_ev;              /* auto;   a job is posted */
    HANDLE   done_ev;             /* auto;   the posted job finished */
    HANDLE   idle_ev;             /* manual; set whenever the pump is NOT running */
    CRITICAL_SECTION lock;        /* one marshalled job at a time */
    int      lock_init;
    int      job;                 /* APR_WJ_* in wasapi_common.c */
    AprWasapiJob job_fn;
    void        *job_user;
    AprErr       job_err;
    HRESULT      com_hr;          /* CoInitializeEx on the capture thread */
    AprWasapiJob teardown;        /* the implementation's half of close() */
    void        *teardown_user;

    RingBuf      *rb;             /* borrowed; outlives the capture */
    AprCapStatus *st;             /* borrowed; owned by the implementation */

    AprClock clock;               /* anchored at the first frame */
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t frame_bytes;

    /* Nonzero for a device capture: fill gaps on DATA_DISCONTINUITY and treat
     * endpoint invalidation as death. A process tap is the reference timeline
     * (design 5.1) and gets none of that. */
    int      device_mode;

    AprWasapiTick tick;
    void         *tick_user;

    volatile LONG running;        /* 1 between start and stop */
    int      pump_stuck;          /* the pump would not join; do not free under it */
    uint64_t frames;              /* pump-thread private copy of the counter */
    uint64_t discont;             /* likewise, so the status cell is write-only */
} AprWasapiStream;

/* Fill a 32-bit-float WAVE_FORMAT_EXTENSIBLE. This is the format apprecorder
 * imposes on every source: process loopback cannot negotiate one
 * (GetMixFormat is E_NOTIMPL, design 4.1), so the session decides and every
 * other kind is made to match. */
void apr_wasapi_format_f32(WAVEFORMATEXTENSIBLE *wfx,
                           uint32_t sample_rate, uint16_t channels);

/* Zero a stream and record where its frames and status go. Must come first:
 * it zeroes the whole struct, including the thread's handles. */
void apr_wasapi_stream_init(AprWasapiStream *s, RingBuf *rb, AprCapStatus *st,
                            uint32_t sample_rate, uint16_t channels,
                            int device_mode,
                            AprWasapiTick tick, void *tick_user);

/* Create the capture thread and wait for it to enter the MTA. Call this
 * straight after apr_wasapi_stream_init and BEFORE touching COM: everything
 * after it belongs on that thread, not on the caller's.
 *
 * On failure the thread may still exist; apr_wasapi_close() retires it either
 * way, so a failed open must still reach close (which capture.c guarantees). */
AprErr apr_wasapi_thread_start(AprWasapiStream *s);

/* Run `job` on the capture thread, inside its MTA, and wait for the result.
 * Callable from any apartment -- that is the entire point of it.
 *
 * Called FROM the capture thread (i.e. from a tick callback) it simply runs the
 * job inline, because the thread cannot post to itself.
 *
 * NOT callable between start() and stop(): the thread is the pump then and is
 * not reading the job queue. That returns APR_E_STATE rather than hanging. */
AprErr apr_wasapi_call(AprWasapiStream *s, AprWasapiJob job, void *user);

/* Take ownership of an already-activated client, then Initialize it for
 * capture, attach an event handle and fetch IAudioCaptureClient.
 *
 * `extra_flags` is OR'd with AUDCLNT_STREAMFLAGS_EVENTCALLBACK -- a process tap
 * adds AUDCLNT_STREAMFLAGS_LOOPBACK, a device adds nothing (or
 * AUTOCONVERTPCM|SRC_DEFAULT_QUALITY on the retry path).
 *
 * hnsBufferDuration is always 0: GetDevicePeriod is E_NOTIMPL on a process
 * loopback client, so there is nothing to ask and 480 frames / 10 ms is what
 * comes back in practice (design 4.1).
 *
 * On failure the client is released and s->ac is NULL. A failed Initialize
 * leaves an IAudioClient unusable, so the caller must re-activate rather than
 * retry on the same pointer. */
AprErr apr_wasapi_prepare(AprWasapiStream *s, IAudioClient *ac,
                          DWORD extra_flags);

/* Start the client and hand the capture thread over to the pump. Frames begin
 * landing in the ring. Callable from any apartment. */
AprErr apr_wasapi_start(AprWasapiStream *s);

/* Idempotent, callable from any thread and any apartment, including from inside
 * the tick callback (in which case it does not try to join itself). */
void apr_wasapi_stop(AprWasapiStream *s);

/* Implies stop. Runs `teardown` on the capture thread -- that is where the
 * implementation releases the COM objects only it knows about -- then releases
 * this layer's own, retires the thread and closes every handle. Does not free
 * the ring. Callable from any apartment.
 *
 * AFTERWARDS THE CALLER MUST CHECK s->pump_stuck BEFORE FREEING ANYTHING the
 * stream points into. A pump wedged inside WASAPI is still running on a struct
 * embedded in the implementation's own allocation; freeing it is a
 * use-after-free under an audio thread. Leaking is the survivable half. */
void apr_wasapi_close(AprWasapiStream *s, AprWasapiJob teardown, void *user);

#endif /* APPRECORDER_CAPTURE_WASAPI_COMMON_H */
