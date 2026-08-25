/*
 * wasapi_common.h -- everything the process tap and the device capture share.
 *
 * Design section 4 gives them different activation paths and different clock
 * behaviour, but the middle of the job -- pick a format, Initialize, take an
 * event handle, run a pump thread, drain packets into a RingBuf -- is the same
 * code. It lives here once. capture_process.c and capture_device.c supply only
 * what genuinely differs: how the IAudioClient is obtained, which stream flags
 * it needs, and what to do on each pump tick.
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

typedef struct AprWasapiStream {
    IAudioClient        *ac;      /* owned once apr_wasapi_adopt is called */
    IAudioCaptureClient *cc;
    HANDLE   ev;                  /* WASAPI event, auto-reset */
    HANDLE   stop_ev;             /* manual reset; the only way the pump exits */
    HANDLE   thread;
    DWORD    thread_id;

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

/* Zero a stream and record where its frames and status go. */
void apr_wasapi_stream_init(AprWasapiStream *s, RingBuf *rb, AprCapStatus *st,
                            uint32_t sample_rate, uint16_t channels,
                            int device_mode,
                            AprWasapiTick tick, void *tick_user);

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

/* Start the client and the pump thread. Frames begin landing in the ring. */
AprErr apr_wasapi_start(AprWasapiStream *s);

/* Idempotent, callable from any thread, including from inside the tick
 * callback (in which case it does not try to join itself). */
void apr_wasapi_stop(AprWasapiStream *s);

/* Implies stop. Releases every COM object and handle. Does not free the ring. */
void apr_wasapi_close(AprWasapiStream *s);

#endif /* APPRECORDER_CAPTURE_WASAPI_COMMON_H */
