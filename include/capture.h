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
 *
 * ===========================================================================
 * APARTMENTS: THE CAPTURE OWNS ITS OWN. YOU MAY CALL FROM ANY.
 *
 *   apr_capture_create(), open(), start(), stop(), status() and close() are
 *   callable from an STA, from the MTA, and from a thread that has never called
 *   CoInitializeEx at all. They neither require nor change the caller's
 *   apartment, and none of them has to be called from the same thread as any
 *   other -- stop() has always said so, and now the rest do too.
 *
 *   A capture owns a thread, so it owns its apartment as well: WASAPI process
 *   loopback requires the MTA (design 4.1 note 7), so the implementation makes
 *   one on its own thread and performs every COM call there -- activation,
 *   IAudioClient setup, the pump, and every Release. (A process tap owns a
 *   SECOND such thread for its mute poll, which is an unbounded cross-process
 *   RPC and must not sit between two audio packets. Same promise: the caller's
 *   apartment is still nobody's business but the capture's.)
 *
 *   THIS IS A PROMISE, NOT AN IMPLEMENTATION DETAIL, and it was learned the
 *   hard way. An earlier version called CoInitializeEx(MTA) on the caller's
 *   thread, which meant every source failed with RPC_E_CHANGED_MODE for any
 *   caller already in an STA. The command line never noticed because its thread
 *   is MTA; the windowed front end could not add a single source, because its
 *   thread MUST be an STA (IAccPropServices, which supplies every control's
 *   accessible name, is valid only on the thread that created it). A library
 *   that pushes a COM constraint onto its callers has moved the problem, not
 *   solved it -- and the next caller falls into the same trap.
 *
 *   tests/test_capture_apartment.c holds this to it from a genuine STA thread.
 * ===========================================================================
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

            /* HEALTH. The two ways a recording comes back silent while every
             * other indicator says it went fine (design 4.1 #5/#6 and section
             * 10) are the two conditions AprCaptureStatus reports below, and
             * neither could be reached from a test without these.
             *
             * Frame indices, not ticks: exact, independent of QPF, and
             * independent of rate_error_ppm, so a health transition lands on
             * the same sample however finely the timeline is stepped.
             * `_at_frame` is the index of the FIRST frame in the new state.
             *
             * 0 means "never" -- start_muted / start_dead are how you ask for
             * frame 0, which is what frees 0 to mean never and keeps a
             * zero-initialised config healthy.
             *
             * BOTH STATES STILL PRODUCE FRAMES, AT EXACTLY THE SAME RATE, AND
             * THOSE FRAMES ARE SILENT. That is not a simplification: loopback
             * is post-session-volume, so a muted app records digital zeros,
             * and after the target process exits loopback keeps handing over
             * zeros for ever with no error and no flag. The fake reproduces
             * the confusing behaviour rather than a tidied version of it,
             * because the confusion is the thing under test.
             *
             * Death is one-way. mute_at_frame == unmute_at_frame is refused
             * rather than silently resolved. */
            uint64_t mute_at_frame;
            uint64_t unmute_at_frame;
            uint64_t die_at_frame;
            int      start_muted;
            int      start_dead;
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
    /* Gaps the ENGINE reported (AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY).
     * Overwhelmingly a device thing -- a process tap was measured never to
     * raise one in ~190 s (design 5.1) -- but not "device sources only": if
     * the engine says it dropped audio on a process tap, that is counted here
     * and filled, because a silent shear on the reference timeline is the one
     * failure the clock design exists to prevent. */
    uint64_t discontinuities;

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

/* Every entry point below is callable from any thread and any apartment; see
 * the apartment note at the top of this file. */
typedef struct AprCaptureVTable {
    const char *kind_name;

    /* Acquires the OS resources, including the capture's own thread and the
     * apartment that thread lives in. rb is borrowed and must outlive the
     * capture. */
    AprErr (*open)(AprCapture *c, const AprCaptureConfig *cfg, RingBuf *rb);

    /* Hands the capture thread to the pump. Frames begin arriving in rb. */
    AprErr (*start)(AprCapture *c);

    /* Idempotent. Returns once the pump has stopped, or once the bounded wait
     * for it gave up. Safe to call from any thread, including from inside the
     * capture's own callbacks.
     *
     * Deliberately still void: stop() frees nothing, so a wedged pump has
     * nothing to warn this caller about that close() will not say again, at
     * the one place where it matters. */
    void (*stop)(AprCapture *c);

    /* Never blocks and never allocates -- safe on a mixer tick. */
    void (*status)(const AprCapture *c, AprCaptureStatus *out);

    /* Implies stop(), then retires the capture thread. Does not free the
     * RingBuf. Need not be called from the thread that called open().
     *
     * RETURNS apr_ok() ONLY WHEN THE CAPTURE IS FULLY RETIRED -- when every
     * thread it owns has exited and the caller may now free the ring buffer
     * it lent us. See the abandonment note on apr_capture_destroy below; this
     * return value is the whole reason that note can be honoured. */
    AprErr (*close)(AprCapture *c);
} AprCaptureVTable;

struct AprCapture {
    const AprCaptureVTable *vt;
    void                   *impl;
};

/* Builds the right implementation for cfg->kind. The only place in the codebase
 * permitted to switch on AprSourceKind.
 *
 * ON FAILURE `*out` IS NORMALLY NULL -- AND IS NOT ALWAYS. A capture that
 * fails to open has already created its own thread (the WASAPI kinds make it
 * before they touch COM), so retiring that half-open capture is a join like
 * any other and can be abandoned like any other. In that one case `*out` is
 * left holding the abandoned capture even though the call failed, because a
 * non-NULL pointer here is how the caller learns THE RING IT LENT US IS STILL
 * BEING WRITTEN INTO and must not be freed. Check `*out`, not just the error:
 *
 *     e = apr_capture_create(cfg, rb, &cap);
 *     if (apr_failed(&e)) { if (!cap) rb_destroy(rb); return e; }
 *
 * Retrying apr_capture_destroy on that pointer later is how the leak is
 * recovered if the wedged thread ever unwedges. */
AprErr apr_capture_create(const AprCaptureConfig *cfg, RingBuf *rb,
                          AprCapture **out);

/* Closes the capture and frees it.
 *
 * ===========================================================================
 * IT CAN FAIL, AND THE CALLER OWNS A RING BUFFER THAT DEPENDS ON THE ANSWER.
 *
 *   apr_ok()  -- every thread this capture owned has exited. Nothing is left
 *                running, and the RingBuf passed to open() may now be freed.
 *
 *   failure   -- a bounded wait for one of those threads timed out (a pump
 *                wedged inside WASAPI is the case this exists for; see
 *                join.h). NOTHING WAS FREED: not the implementation, not the
 *                AprCapture, and above all NOT the caller's ring, which that
 *                thread may still be memcpy-ing into. Leaking a thread, a COM
 *                reference and 96 KB is survivable; a use-after-free from an
 *                audio thread is heap corruption whose stack trace points
 *                somewhere else entirely.
 *
 *   Freeing the ring after a failure here is the bug this signature exists to
 *   make impossible to write by accident. apr_source_destroy() reads it and
 *   leaks in step; a front end that only wants to report may ignore it and
 *   leak, but cannot corrupt.
 * ===========================================================================
 *
 * A NULL capture is apr_ok() -- there was nothing to retire. */
AprErr apr_capture_destroy(AprCapture *c);

#ifdef __cplusplus
}
#endif
#endif /* APR_CAPTURE_H */
