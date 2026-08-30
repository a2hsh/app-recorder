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
 *   created it (src/app/main.c). Both requirements are correct and they
 *   cannot both hold on one thread.
 *
 *   An earlier arrangement resolved that by calling CoInitializeEx(MTA) on
 *   whatever thread invoked open(), which made apr_capture_create() fail with
 *   RPC_E_CHANGED_MODE for every STA caller. The CLI never noticed, because its
 *   thread was already MTA; the GUI could not add a single source.
 *
 *   A capture already owns a thread, so it now owns its apartment as well.
 *   apr_wasapi_thread_start() creates that thread and puts it in the MTA once;
 *   every COM call THIS FILE makes -- activation, Initialize, the pump, and
 *   every Release -- runs on it. open/start/stop/close marshal there and wait.
 *   That is what lets include/capture.h promise what a library owes its
 *   callers: apr_capture_create() works from ANY apartment, and the old
 *   "open() and close() must share a thread" constraint is gone with it.
 *
 *   ONE THREAD PER APARTMENT, NOT ONE APARTMENT PER CAPTURE. capture_process.c
 *   now runs its mute poll on a SECOND thread with its own MTA and its own COM
 *   objects, because that poll is an unbounded cross-process RPC and it was
 *   stalling the pump between audio packets (BUGS.md M3). Nothing about the
 *   promise above changes: the rule is that the CALLER never has to think
 *   about an apartment, and no object crosses between those two threads.
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

/* Called once per pump wakeup, ON THE PUMP THREAD, after packets are drained.
 * Used for the out-of-band checks WASAPI will not do for us -- in practice,
 * "is the target process still alive" (design section 10).
 *
 * IT RUNS BETWEEN AUDIO PACKETS, so nothing in it may make a COM call, take a
 * lock the audio service holds, or block for an unbounded time. Mute polling
 * used to be here and is not any more, for exactly that reason: a stalled pump
 * makes WASAPI drop packets and set DATA_DISCONTINUITY. See capture_process.c. */
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

    /* Nonzero for a device capture. It no longer decides whether a reported
     * gap is filled -- apr_wasapi_packet_gap takes no kind argument at all,
     * because a gap the ENGINE reports is a loss on either kind and design
     * 3.1 answers a loss with alignment over content. What it still marks is
     * the source that carries real crystal drift and real endpoint removal;
     * a process tap is the reference timeline (design 5.1). */
    int      device_mode;

    AprWasapiTick tick;
    void         *tick_user;

    /* Rejoining a timeline that was already running -- this capture replaced
     * one that died on a source that has been recording for a while. Set by
     * the implementation's open() with apr_capresume_init(&s->resume, cfg),
     * AFTER apr_wasapi_stream_init (which zeroes this struct); the pump acts
     * on it once, immediately before the first frame it writes. */
    AprCapResume resume;

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

/* ---------------------------------------------------------------------------
 * The packet timeline -- pure arithmetic, no COM, deliberately testable.
 *
 * Both halves of "what does this packet mean for the clock" live here rather
 * than inline in drain(), because the answer is the whole of design 5.2 step 4
 * and it was wrong in a way no hardware-free test could see.
 * ------------------------------------------------------------------------- */

/* The QPC tick at which a packet's FIRST frame was captured.
 *
 * `arrival_ticks` is apr_qpc_now() read the instant GetBuffer handed the
 * packet over, and those frames were captured BEFORE that: a 480-frame packet
 * arriving now covers the 10 ms that just ended. Anchoring a source at arrival
 * therefore places its whole timeline one packet late, which is invisible
 * while every source has the same packet size and is a straight sync error the
 * moment one does not -- a 1024-frame endpoint mixed with a 480-frame process
 * tap lands 11 ms out and stays there.
 *
 * Saturates at 0 rather than wrapping, so a synthetic timeline that starts
 * near zero cannot produce an anchor in the far future. */
uint64_t apr_wasapi_packet_start(uint64_t arrival_ticks, uint32_t frames,
                                 uint64_t qpc_freq, uint32_t sample_rate);

/* Everything one arriving packet does to the timeline, with no COM anywhere
 * near it:
 *
 *   - anchors `clock` at the capture time of this source's frame 0 (which is
 *     this packet's first frame, less anything already delivered -- a first
 *     packet flagged TIMESTAMP_ERROR is not allowed to anchor, so `delivered`
 *     may already be nonzero by the time one may);
 *   - returns the frames of silence that must be written BEFORE this packet's
 *     audio to keep the ring's index space equal to elapsed time.
 *
 * `flags` is the DWORD from IAudioCaptureClient::GetBuffer.
 *
 * The fill is returned for a process tap as well as a device, and that is a
 * deliberate narrowing of design 5.1. "Process taps arrive perfect" is a
 * measured statement about an engine that is keeping up; it is not a claim
 * about one that has just told us it dropped audio. When
 * AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY is set on a process tap the engine is
 * reporting a loss, and design 3.1's stated policy for a loss is alignment
 * over content: emit exactly the frames that went missing and resume at the
 * true absolute position. Nothing is ever synthesized speculatively -- with no
 * discontinuity flag this returns 0 and not one sample is invented. */
uint64_t apr_wasapi_packet_gap(AprClock *clock, uint64_t delivered,
                               uint64_t arrival_ticks, uint32_t frames,
                               DWORD flags);

/* Implies stop. Runs `teardown` on the capture thread -- that is where the
 * implementation releases the COM objects only it knows about -- then releases
 * this layer's own, retires the thread and closes every handle. Does not free
 * the ring. Callable from any apartment.
 *
 * RETURNS apr_ok() ONLY WHEN THE THREAD HAS ACTUALLY EXITED. A failure means
 * the pump is wedged inside WASAPI and is still running on a struct embedded
 * in the implementation's own allocation: freeing that allocation, or the ring
 * the pump writes into, is a use-after-free under an audio thread. Leaking is
 * the survivable half -- see join.h.
 *
 * s->pump_stuck says the same thing and stays for diagnostics, but it is no
 * longer the only way to find out, which is the point: it was a flag every
 * caller had to remember to read, and the layer that owned the ring never
 * saw it at all. */
AprErr apr_wasapi_close(AprWasapiStream *s, AprWasapiJob teardown, void *user);

/* ---------------------------------------------------------------------------
 * Test seam. Not for production use; see tests/test_capture_wasapi.c.
 *
 * Called inside apr_wasapi_call() at the exact instant the "is the thread
 * pumping?" answer could go stale -- i.e. in the window a concurrent
 * apr_wasapi_start() has to win for the caller to post a job the pump will
 * never read. A test installs a hook that starts the pump there and asserts
 * the job is refused rather than posted into a queue nobody is serving.
 * ------------------------------------------------------------------------- */
typedef void (*AprWasapiRaceHook)(AprWasapiStream *s, void *user);
void apr_wasapi_test_set_race_hook(AprWasapiRaceHook hook, void *user);

#endif /* APPRECORDER_CAPTURE_WASAPI_COMMON_H */
