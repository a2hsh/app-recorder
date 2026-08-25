/*
 * capture_internal.h -- shared internals of the capture layer. Not public.
 *
 * Everything here exists so process, device and fake sources publish status
 * the same way and are built the same way. Nothing above include/capture.h may
 * see any of it.
 */
#ifndef APPRECORDER_CAPTURE_INTERNAL_H
#define APPRECORDER_CAPTURE_INTERNAL_H

#include <windows.h>
#include "capture.h"

/* ---------------------------------------------------------------------------
 * The published status cell.
 *
 * capture.h promises AprCaptureVTable::status never blocks and never
 * allocates, and it is read from the mixer while the capture thread is writing.
 * So:
 *
 *   - the counters are 64-bit and naturally aligned. x64 makes aligned 64-bit
 *     loads and stores atomic, and this codebase is x64-only (see ringbuf.h),
 *     so a reader can never see half of one.
 *   - last_error is 180+ bytes and cannot be. It is published through a
 *     seqlock: the writer bumps a counter to odd, copies, bumps it to even; the
 *     reader copies between two equal even reads. Errors are raised a handful
 *     of times in a session, so the retry path is effectively never taken, and
 *     the reader never waits on the writer -- it only ever re-reads.
 *
 * One writer only (the capture thread, or the opening thread before it runs).
 * ------------------------------------------------------------------------- */
typedef struct AprCapStatus {
    volatile LONG64 anchor_ticks;
    volatile LONG64 frames_written;
    volatile LONG64 discontinuities;
    volatile LONG   alive;
    volatile LONG   muted;
    volatile LONG   err_seq;      /* even = stable, odd = being written */
    AprErr          err;
} AprCapStatus;

void apr_capstat_init(AprCapStatus *s);
void apr_capstat_set_error(AprCapStatus *s, const AprErr *e);
void apr_capstat_clear_error(AprCapStatus *s);
void apr_capstat_read(const AprCapStatus *s, AprCaptureStatus *out);

/* Convenience writers. Single writer thread; plain stores, see above. */
void apr_capstat_set_alive(AprCapStatus *s, int alive);
void apr_capstat_set_muted(AprCapStatus *s, int muted);
int  apr_capstat_alive(const AprCapStatus *s);

/* ---------------------------------------------------------------------------
 * Per-kind constructors. apr_capture_create in capture.c is the only caller,
 * and the only place in the tree that switches on AprSourceKind (capture.h).
 * Each returns a vtable whose open() allocates its own impl.
 * ------------------------------------------------------------------------- */
const AprCaptureVTable *apr_capture_process_vtable(void);
const AprCaptureVTable *apr_capture_device_vtable(void);
const AprCaptureVTable *apr_capture_fake_vtable(void);

/* Shared argument checking, so the three kinds cannot disagree about it. */
AprErr apr_capture_check_common(const AprCaptureConfig *cfg, const RingBuf *rb);

#endif /* APPRECORDER_CAPTURE_INTERNAL_H */
