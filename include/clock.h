/*
 * clock.h -- QPC arithmetic and drift. Sole owner of both (design section 7).
 *
 * WHY THIS FILE IS ALL INTEGERS
 *
 *   Design section 5 makes QPC the single master timeline and demands
 *   sample-accurate alignment across sessions hours long, because alignment
 *   cannot be retrofitted into files already written.
 *
 *   Two habits destroy that, and both are avoided here by construction:
 *
 *   1. Floating point. A double holds 53 bits; a 3-hour session at 48 kHz is
 *      5.2e8 frames and 1.1e11 QPC ticks, and their product overflows a
 *      double's exact-integer range immediately. Every conversion below is
 *      done as a 128-bit multiply followed by a 64-bit divide, so the result
 *      is the exact mathematical floor -- no rounding at all, at any session
 *      length. Doubles appear in exactly one place, apr_drift_ppm, which is
 *      for humans reading a log and feeds nothing.
 *
 *   2. Accumulation. Converting per buffer and summing floors once per buffer
 *      and throws the fractions away; over 1.08 million 10 ms buffers that is
 *      minutes of error. Every function here takes an ABSOLUTE timestamp or
 *      frame index measured from the anchor and floors exactly once, so error
 *      is bounded below one frame however long the session runs. This is why
 *      there is no "advance the clock by dt" call: it would be the wrong
 *      shape, and someone would use it.
 *
 * UNITS
 *
 *   ticks   QueryPerformanceCounter units. Rate is apr_qpc_freq(). It was
 *           exactly 10,000,000 on the spike machine, which makes a tick and an
 *           hns unit coincide and hides a whole class of unit bug; design
 *           section 5.2 says never assume it, so nothing here does, and the
 *           tests deliberately run at 3,579,545 Hz to keep it that way.
 *   hns     100-nanosecond units. What WASAPI durations and pu64QPCPosition
 *           are expressed in.
 *   frames  Sample frames at a given rate. A frame is all channels of one
 *           instant, so channel count never enters this arithmetic.
 *
 * WHERE THE TIMESTAMPS COME FROM (design section 5, as corrected by the spike)
 *
 *   Anchor and sample from QueryPerformanceCounter AT CAPTURE TIME -- not from
 *   GetBuffer's pu64QPCPosition. The spike measured that field to be the
 *   process tap's own frame counter rescaled: it reports +0.00 ppm by
 *   construction, so drift computed from it can never fire. The hns
 *   conversions below are for interpreting WASAPI durations, not for building
 *   the master timeline out of.
 *
 *   The same correction means the two source kinds are not symmetric. Process
 *   taps arrive gapless at 100% fill and ARE the reference timeline; nothing
 *   below needs to run for them. apr_clock_drift exists for device captures,
 *   which ride a hardware crystal and are the only sources carrying real
 *   drift.
 *
 * THREAD SAFETY: every function here is pure but for apr_qpc_now/apr_qpc_freq,
 * which read the counter. Nothing allocates or locks, so all of it is safe on
 * a capture callback. An AprClock is a value the caller owns; concurrent
 * anchoring and reading of one AprClock is the caller's problem.
 */
#ifndef APPRECORDER_CLOCK_H
#define APPRECORDER_CLOCK_H

#include <stdint.h>

/* 100-nanosecond units in one second. */
#define APR_HNS_PER_SEC 10000000ull

/* ---------------------------------------------------------------------------
 * Exact 128-bit scaling. Every conversion in this file is built from these,
 * and so should any new one.
 * ------------------------------------------------------------------------- */

/* floor((a * b) / d), computed through a full 128-bit intermediate so nothing
 * is lost to overflow or rounding.
 *
 * `out_rem` (may be NULL) receives (a * b) mod d. The fractional part of the
 * true quotient is out_rem / d -- keep it if you need sub-frame phase.
 *
 * d == 0 yields 0 with a remainder of 0 rather than faulting. A quotient too
 * large for 64 bits saturates to UINT64_MAX rather than raising #DE, which is
 * what the underlying divide instruction would do. Neither case can arise from
 * real audio timings; both are here so a corrupt timestamp cannot take a
 * session down. */
uint64_t apr_mul_div_u64(uint64_t a, uint64_t b, uint64_t d, uint64_t *out_rem);

/* Signed form, truncating toward zero, so a timestamp before the anchor and
 * one the same distance after it give answers of equal magnitude. */
int64_t apr_mul_div_i64(int64_t a, uint64_t b, uint64_t d);

/* ---------------------------------------------------------------------------
 * The live counter
 * ------------------------------------------------------------------------- */

/* QPC ticks per second. Queried once and cached; the value is fixed at boot.
 * Thread-safe. */
uint64_t apr_qpc_freq(void);

/* The current QPC value. On x64 this is a user-mode read with no kernel
 * transition, so it is safe on the audio thread. Thread-safe. */
uint64_t apr_qpc_now(void);

/* ---------------------------------------------------------------------------
 * Unit conversions. All exact, all floor, all stateless.
 * ------------------------------------------------------------------------- */

uint64_t apr_ticks_to_hns(uint64_t ticks, uint64_t qpc_freq);
uint64_t apr_hns_to_ticks(uint64_t hns, uint64_t qpc_freq);

/* Frames wholly elapsed in `ticks` ticks at `sample_rate`. `out_rem` (may be
 * NULL) receives the remainder over `qpc_freq`. */
uint64_t apr_ticks_to_frames(uint64_t ticks, uint64_t qpc_freq,
                             uint32_t sample_rate, uint64_t *out_rem);

/* Ticks spanned by `frames` frames, floored. */
uint64_t apr_frames_to_ticks(uint64_t frames, uint64_t qpc_freq,
                             uint32_t sample_rate);

/* Frames wholly elapsed in `hns` 100 ns units. `out_rem` (may be NULL)
 * receives the remainder over APR_HNS_PER_SEC. */
uint64_t apr_hns_to_frames(uint64_t hns, uint32_t sample_rate, uint64_t *out_rem);

/* 100 ns units spanned by `frames` frames, floored. */
uint64_t apr_frames_to_hns(uint64_t frames, uint32_t sample_rate);

/* ---------------------------------------------------------------------------
 * A source's clock
 * ------------------------------------------------------------------------- */

typedef struct AprClock {
    uint64_t qpc_freq;      /* ticks per second */
    uint32_t sample_rate;   /* frames per second, nominal */
    int      anchored;      /* has the first frame been seen? */
    uint64_t anchor_ticks;  /* QPC at this source's frame 0 */
} AprClock;

/* Set up an unanchored clock. Both arguments must be nonzero. */
void apr_clock_init(AprClock *c, uint64_t qpc_freq, uint32_t sample_rate);

/* Record the QPC of the source's first frame -- read with apr_qpc_now() when
 * that buffer arrives, not taken from pu64QPCPosition. Design section 5.2
 * step 2. A buffer flagged AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR must NOT be
 * passed here: a poisoned anchor misaligns everything that follows. */
void apr_clock_anchor(AprClock *c, uint64_t anchor_ticks);

/* Nonzero once apr_clock_anchor has been called. */
int apr_clock_anchored(const AprClock *c);

/* Frames the clock says should have elapsed by `now_ticks`. Exact; negative if
 * `now_ticks` precedes the anchor. */
int64_t apr_clock_expected_frames(const AprClock *c, uint64_t now_ticks);

/* The first QPC tick at which `frame_index` frames have elapsed -- the inverse
 * of apr_clock_expected_frames, and rounded up for exactly that reason:
 * expected_frames(frame_ticks(n)) == n for every n. */
uint64_t apr_clock_frame_ticks(const AprClock *c, uint64_t frame_index);

/* ---------------------------------------------------------------------------
 * Drift and gaps -- design section 5.2 step 4.
 *
 * For DEVICE captures. A process tap is the reference timeline and needs none
 * of this; running it there measures the noise floor and nothing else.
 * ------------------------------------------------------------------------- */

typedef struct AprDrift {
    int64_t expected_frames;  /* from elapsed QPC */
    int64_t actual_frames;    /* what the source actually delivered */
    /* expected - actual. Positive means frames are MISSING: a gap to fill with
     * silence, from a silent app, a dropout, or a source running slow.
     * Negative means surplus: the source is running fast and must be
     * resampled down. Zero is a source locked to QPC. */
    int64_t delta_frames;
} AprDrift;

/* Compare the clock's expectation at `now_ticks` against the frames a source
 * has actually produced since its anchor. Exact and allocation-free. */
AprDrift apr_clock_drift(const AprClock *c, uint64_t now_ticks,
                         uint64_t actual_frames);

/* The source's rate error in parts per billion: positive when the source runs
 * FAST relative to QPC. Exact integer arithmetic; 0 when nothing has elapsed
 * yet. A steady nonzero value is clock drift and belongs to the resampler; a
 * value that jumps is a gap and belongs to the silence filler. */
int64_t apr_drift_ppb(const AprDrift *d);

/* The same figure in parts per million as a double. FOR DISPLAY ONLY -- never
 * feed this back into position arithmetic. */
double apr_drift_ppm(const AprDrift *d);

/* actual / expected as an unsigned Q32.32 fixed-point ratio: 1<<32 is exactly
 * 1.0. This is the resampling ratio a source needs to be brought onto the
 * session timeline, in a form that can be accumulated without drift. 1<<32
 * when nothing has elapsed yet. */
uint64_t apr_drift_ratio_q32(const AprDrift *d);

#endif /* APPRECORDER_CLOCK_H */
