/* capture.h — the one interface every audio source implements.
 *
 * Process taps, device captures and the fake source are interchangeable behind
 * this. Design section 4.3 makes that a hard requirement: the entire core must
 * be testable with no audio hardware present, so nothing above this header may
 * branch on AprSourceKind.
 *
 * Ownership model: a capture owns a thread, writes interleaved float32 frames
 * into a RingBuf it does not own, and publishes status atomically. Consumers
 * read through their own RingReader. The ring stays pure PCM; everything a
 * consumer needs to reason about time or health comes from AprCaptureStatus.
 */
#ifndef APR_CAPTURE_H
#define APR_CAPTURE_H

#include <stdint.h>
#include "err.h"
#include "ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AprSourceKind {
    APR_SRC_PROCESS,   /* WASAPI process loopback — see design 4.1 */
    APR_SRC_DEVICE,    /* real capture endpoint, real crystal, real drift */
    APR_SRC_FAKE       /* synthetic; the reason the core needs no hardware */
} AprSourceKind;

typedef struct AprCaptureConfig {
    AprSourceKind kind;
    uint32_t      sample_rate;   /* session rate; process taps cannot negotiate */
    uint16_t      channels;

    union {
        struct {
            uint32_t pid;
            /* 0 = INCLUDE_TARGET_PROCESS_TREE (capture this app).
             * 1 = EXCLUDE_TARGET_PROCESS_TREE (capture everything else).
             * EXCLUDE records whatever the machine is playing — treat it as a
             * privacy-sensitive mode and never enable it by default. */
            int      exclude;
        } process;

        struct {
            const wchar_t *endpoint_id;   /* borrowed for the call only */
        } device;

        struct {
            int32_t  rate_error_ppm;  /* stand in for a drifting crystal */
            uint32_t tone_hz;         /* 0 = silence */
            float    amplitude;
        } fake;
    };
} AprCaptureConfig;

/* Snapshot of a source's health and position. Cheap; safe to poll per mixer
 * tick. Never blocks and never allocates. */
typedef struct AprCaptureStatus {
    /* QPC tick at the first frame, from apr_qpc_now() at buffer arrival.
     * NOT pu64QPCPosition — for process taps that value is the frame counter
     * rescaled and carries no independent clock information (design 5.1).
     * Zero means not yet anchored. */
    uint64_t anchor_ticks;

    uint64_t frames_written;
    uint64_t discontinuities;   /* device sources only; process taps never gap */

    /* 0 once the source is known dead. Process loopback keeps emitting silence
     * forever after the target exits and WASAPI never reports it, so process
     * captures must detect death themselves — see design section 10. */
    int alive;

    /* Non-zero when a process source's session volume is 0. Loopback is
     * post-session-volume, so a muted app records as pure silence while still
     * appearing healthy. The UI must surface this. */
    int muted;

    AprErr last_error;          /* apr_ok() while healthy */
} AprCaptureStatus;

typedef struct AprCapture AprCapture;

typedef struct AprCaptureVTable {
    const char *kind_name;

    /* Acquires the OS resources. rb is borrowed and must outlive the capture. */
    AprErr (*open)(AprCapture *c, const AprCaptureConfig *cfg, RingBuf *rb);

    /* Spawns the capture thread. Frames begin arriving in rb. */
    AprErr (*start)(AprCapture *c);

    /* Idempotent. Joins the capture thread. Safe to call from any thread. */
    void (*stop)(AprCapture *c);

    void (*status)(const AprCapture *c, AprCaptureStatus *out);

    /* Implies stop(). Does not free the RingBuf. */
    void (*close)(AprCapture *c);
} AprCaptureVTable;

struct AprCapture {
    const AprCaptureVTable *vt;
    void                   *impl;
};

/* Builds the right implementation for cfg->kind. The only place in the codebase
 * permitted to switch on AprSourceKind. */
AprErr apr_capture_create(const AprCaptureConfig *cfg, RingBuf *rb,
                          AprCapture **out);

void apr_capture_destroy(AprCapture *c);

#ifdef __cplusplus
}
#endif
#endif /* APR_CAPTURE_H */
