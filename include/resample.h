/*
 * resample.h -- windowed-sinc sample rate conversion.
 *
 * Design section 7 names core/resample.c the SOLE OWNER of sample-rate
 * conversion. There is ONE implementation and every path uses it: the drift
 * corrector nudging a device capture by 30 ppm, and a future 44.1 -> 48 kHz
 * conversion, are the same code with a different ratio. Per-source variants
 * are a review failure.
 *
 * THE RATIO IS INPUT FRAMES PER OUTPUT FRAME, Q32.32
 *
 *   1 << 32 is exactly 1.0. Above that the input is consumed faster than
 *   output is produced (the source runs FAST and is being slowed down); below,
 *   the opposite.
 *
 *   This is deliberately the same quantity apr_drift_ratio_q32() returns:
 *   actual frames delivered divided by frames the QPC timeline expected. A
 *   device running 30 ppm fast delivers 1.00003 input frames for every engine
 *   frame, so its ratio is (1 << 32) * 1.00003 and the surplus is resampled
 *   away instead of accumulating as desync.
 *
 *   The position accumulates in exact 64-bit fixed point -- pos += ratio, once
 *   per output frame, no floating point anywhere in the position arithmetic.
 *   Over a three-hour session that is 5.2e8 additions with zero rounding, so
 *   the input position the resampler reports is exact to 2^-32 of a frame.
 *
 * ZERO GROUP DELAY, ON PURPOSE
 *
 *   The filter is primed with silence so that output frame 0 is centred on
 *   input frame 0. A resampler with latency would shift a device source
 *   against the process taps it is being mixed with, which is precisely the
 *   desync this whole subsystem exists to prevent. At ratio 1.0 with zero
 *   phase the kernel collapses to a unit impulse and the output is the input,
 *   bit for bit.
 *
 * DC GAIN IS EXACTLY 1
 *
 *   Every output is normalised by the sum of the taps that produced it. A
 *   windowed sinc evaluated at an arbitrary fractional phase does not sum to
 *   one, and the residual ripple would appear as a slow level wobble as the
 *   phase walks -- audible on sustained tones, and invisible in a spectrum
 *   plot of white noise. Normalising also makes the truncated kernel at the
 *   very start of a stream harmless.
 *
 * THREAD SAFETY: one AprResampler belongs to one thread. Different resamplers
 * are independent -- which is why every bus reading a shared source gets its
 * own, since each consumes that source at its own pace. Nothing here allocates
 * after create, so apr_resample is safe on a mixer tick.
 */
#ifndef APPRECORDER_RESAMPLE_H
#define APPRECORDER_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

#include "err.h"

/* Largest ratio the anti-alias kernel is sized for. Downsampling widens the
 * filter support by exactly this factor, so it bounds the internal buffer. */
#define APR_RESAMPLE_MAX_RATIO 8.0

typedef struct AprResampler AprResampler;

/* `max_ratio` is the largest ratio that will ever be set (clamped into
 * [1.0, APR_RESAMPLE_MAX_RATIO]); it sizes the kernel support and the internal
 * buffer. Drift correction wants 1.01; a 96 -> 48 kHz conversion wants 2.
 * The ratio itself starts at exactly 1.0. */
AprErr apr_resampler_create(uint16_t channels, double max_ratio, AprResampler **out);
void   apr_resampler_destroy(AprResampler *r);

/* Discard all history and return to input position 0 with the filter primed.
 * The ratio is left alone. */
void apr_resampler_reset(AprResampler *r);

/* Set the ratio. Safe to call between any two apr_resample calls -- the
 * position is untouched, so a controller may nudge this every tick. Values are
 * clamped to [1/max_ratio, max_ratio]. */
void     apr_resampler_set_ratio_q32(AprResampler *r, uint64_t ratio_q32);
uint64_t apr_resampler_ratio_q32(const AprResampler *r);

/* Exact fractional input position, Q32.32, counted from the first frame ever
 * pushed. THIS is the number the drift controller compares against the
 * producer's write cursor: their difference is the backlog, and holding that
 * constant is what keeps a device source sample-aligned for hours. */
uint64_t apr_resampler_in_pos_q32(const AprResampler *r);

/* Input frames of lookahead the kernel needs past the current position. */
size_t apr_resampler_lookahead(const AprResampler *r);

/* Input frames that can be pushed right now without any being refused. */
size_t apr_resampler_input_space(const AprResampler *r);

/* Upper bound on the input frames still needed to produce `out_frames` more
 * output frames, given what is already buffered. */
size_t apr_resampler_input_needed(const AprResampler *r, size_t out_frames);

/* Consume from `in` and produce into `out`. Returns output frames written;
 * *in_used (may be NULL) receives input frames absorbed.
 *
 * GUARANTEE: when in_frames <= apr_resampler_input_space(), every input frame
 * is absorbed, whether or not `out` filled. Callers that respect that never
 * have to carry a remainder, which is why there is no carry buffer here.
 *
 * Both buffers are interleaved at the channel count given to create. */
size_t apr_resample(AprResampler *r, const float *in, size_t in_frames, size_t *in_used,
                    float *out, size_t out_frames);

#endif /* APPRECORDER_RESAMPLE_H */
