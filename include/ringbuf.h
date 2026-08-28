/*
 * ringbuf.h -- the one ring buffer in apprecorder (design section 7).
 *
 * Nothing else in this tree may implement its own. If a caller needs a shape
 * this does not have, widen this file.
 *
 * SHAPE: single producer, many consumers, each with its own cursor
 *
 *   One Source has one capture thread filling one RingBuf, and every Bus that
 *   consumes that Source holds its own RingReader (design section 3.1). The
 *   producer never looks at any reader, so adding or removing a bus mid-session
 *   costs the capture thread nothing and cannot stall it.
 *
 *   Reading does not consume. A reader only advances its own cursor.
 *
 * PRODUCER GUARANTEES
 *
 *   rb_write and rb_write_silence never block, never fail, never allocate and
 *   never take a lock. They are safe to call from a WASAPI callback. Storage is
 *   allocated once by rb_create and never grows.
 *
 * OVERRUN POLICY: the producer always wins; the consumer is always told
 *
 *   Capacity is fixed, so a consumer that stops reading for longer than the
 *   ring holds WILL lose data. The three candidate policies:
 *
 *     - Block the producer until the consumer catches up. Rejected outright:
 *       the producer is an audio callback. Stalling it drops live audio for
 *       every bus, to protect one slow one.
 *     - Refuse the write (drop the newest). Rejected: the newest frames are
 *       the live ones. Dropping them puts a hole at the head of every other
 *       consumer's stream too, punishing readers that were keeping up.
 *     - Overwrite the oldest. Chosen. The loss is confined to the consumer
 *       that fell behind, and it is exactly the data that was already stalest.
 *
 *   Corruption is never acceptable, only loss. A reader that has been lapped
 *   discovers it before it copies anything: its cursor is force-advanced to the
 *   oldest frame still held, and the exact number of frames that passed it by
 *   is reported through `out_lost` and accumulated in rb_reader_lost(). That
 *   count is not a diagnostic -- it is the input the drift corrector needs to
 *   synthesise exactly that many frames of silence and keep the timeline
 *   sample-accurate (design section 5). A gap you can measure is recoverable;
 *   a gap you cannot is a permanently desynced file.
 *
 *   If the producer laps a reader *during* its copy, the copied bytes are
 *   discarded rather than returned (they would be a mixture of two epochs) and
 *   the read is retried. After a few failed attempts the reader is placed half
 *   a buffer behind the writer and everything skipped is reported as loss, so
 *   the call always terminates.
 *
 *   Sizing follows from this: give the ring several times the worst tolerable
 *   consumer stall. At 48 kHz stereo float32, one second is 384 KB.
 *
 * MEMORY MODEL: x64 only, which is what apprecorder targets. Cursors are
 * 64-bit and monotonic -- they are never taken modulo anything except to index
 * storage -- so there is no ambiguity between "empty" and "exactly full", and
 * no wrap to reason about for 12 million years at 48 kHz.
 */
#ifndef APPRECORDER_RINGBUF_H
#define APPRECORDER_RINGBUF_H

#include <stddef.h>
#include <stdint.h>

#include "err.h"

typedef struct RingBuf RingBuf;

typedef enum RbStart {
    /* Attach at the write cursor: the reader sees only frames written from
     * now on. What a bus joining a live source wants. */
    RB_START_LATEST = 0,
    /* Attach at the oldest frame the ring still holds. Frames already
     * overwritten are not counted as loss -- this reader never had them. */
    RB_START_OLDEST
} RbStart;

/* A consumer's cursor. Public so a Bus can embed one and avoid an allocation;
 * treat the fields as private and use the accessors. One RingReader belongs to
 * exactly one thread at a time. */
typedef struct RingReader {
    RingBuf *rb;
    uint64_t pos;    /* absolute index of the next frame to read */
    uint64_t lost;   /* frames overwritten before this reader saw them */
} RingReader;

/* ---------------------------------------------------------------------------
 * Lifetime. Not thread-safe: create and destroy with no capture running.
 * ------------------------------------------------------------------------- */

/* Allocate a ring for at least `capacity_frames` frames of `frame_bytes` each.
 * The capacity is rounded UP to a power of two (rb_capacity_frames reports what
 * you got). Both arguments must be nonzero.
 *
 * On success *out receives the ring, owned by the caller and released with
 * rb_destroy. On failure *out is set to NULL and the returned AprErr says why.
 * This is the only allocation in this module's whole lifetime. */
AprErr rb_create(size_t capacity_frames, size_t frame_bytes, RingBuf **out);

/* Free a ring. NULL is allowed. All readers must be finished first: this does
 * not, and cannot, check. */
void rb_destroy(RingBuf *rb);

/* Fixed properties. Thread-safe (they never change). */
size_t rb_capacity_frames(const RingBuf *rb);
size_t rb_frame_bytes(const RingBuf *rb);

/* Total frames ever written -- the absolute index the next frame will take.
 * Safe to call from any thread. */
uint64_t rb_write_pos(const RingBuf *rb);

/* ---------------------------------------------------------------------------
 * Producer. ONE thread only. Lock-free, allocation-free, never blocks.
 * ------------------------------------------------------------------------- */

/* Append `frames` frames read from `src` (frames * frame_bytes bytes; must not
 * be NULL when frames > 0). Overwrites the oldest data when the ring is full.
 * A single write longer than the capacity keeps only its last `capacity`
 * frames, and the write cursor still advances by the full count so no
 * consumer's timeline is distorted by the truncation. */
void rb_write(RingBuf *rb, const void *src, size_t frames);

/* Append `frames` of zeroes. This is how a gap detected from timestamps is
 * filled (design section 5, "a silent app produces no buffers at all").
 * Same guarantees as rb_write. */
void rb_write_silence(RingBuf *rb, size_t frames);

/* ---------------------------------------------------------------------------
 * Consumers. Each RingReader is used by one thread; different readers may be
 * used concurrently by different threads with no coordination between them.
 * All of these are allocation-free and never block.
 * ------------------------------------------------------------------------- */

/* Point `rd` at `rb`, starting where `start` says. Resets the loss counter. */
void rb_reader_init(RingReader *rd, RingBuf *rb, RbStart start);

/* Frames this reader could read right now, capped at the capacity (anything
 * beyond that has already been overwritten and will be reported as loss). */
uint64_t rb_reader_available(const RingReader *rd);

/* Absolute index of the next frame this reader will return. Together with the
 * loss reported by rb_read, this is what places a block of frames on the
 * session timeline. */
uint64_t rb_reader_pos(const RingReader *rd);

/* Frames lost to overrun by this reader since rb_reader_init. */
uint64_t rb_reader_lost(const RingReader *rd);

/* Copy up to `max_frames` frames into `dst` and advance the cursor by however
 * many were copied. Returns the frame count copied, which may be 0.
 *
 * `out_lost` (may be NULL) receives the frames skipped by THIS call because
 * they had been overwritten. They sit immediately before the returned frames
 * on the timeline: the first frame returned has absolute index
 * (rb_reader_pos() before the call) + *out_lost. Fill that many frames of
 * silence to keep alignment.
 *
 * `dst` must have room for max_frames * frame_bytes bytes. */
size_t rb_read(RingReader *rd, void *dst, size_t max_frames, uint64_t *out_lost);

/* Advance the cursor by up to `max_frames` without copying. Returns the number
 * skipped deliberately; `out_lost` reports frames skipped because they were
 * already overwritten. */
size_t rb_skip(RingReader *rd, size_t max_frames, uint64_t *out_lost);

/* PLACE the cursor at absolute frame index `pos`, clamped into what the ring
 * can still serve: no earlier than the oldest frame it still holds, no later
 * than the write cursor. Returns the index actually reached.
 *
 * NOTHING IS COUNTED AS LOSS, and that is the entire difference between this
 * and rb_skip. Loss means "frames that belonged to this reader's timeline went
 * past it"; a seek means "this reader's timeline moved", which is a statement
 * about the consumer and not about the data. A pause is exactly that: the
 * frames captured while the recording was paused are DELIBERATELY not wanted,
 * and reporting them as loss would make the drift corrector synthesise the
 * paused duration back into the file as silence -- which is the one outcome
 * pause exists to avoid (bus.h).
 *
 * rb_skip cannot do this job. It reaps an overrun BEFORE it applies the caller's
 * frame count, so a request computed from a lapped cursor overshoots the target
 * and the reader lands past where it asked for -- which the next read then reads
 * back as a hole. Seeking states the destination instead of the distance.
 *
 * Consumer side only, like everything else in this section. */
uint64_t rb_reader_seek(RingReader *rd, uint64_t pos);

#endif /* APPRECORDER_RINGBUF_H */
