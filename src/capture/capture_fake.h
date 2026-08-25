/*
 * capture_fake.h -- the test driver for the synthetic source. Not public.
 *
 * capture.h is deliberately identical for all three kinds, so it has no way to
 * say "now advance the simulated clock by two hours". This is that door, and
 * it is the reason design section 4.3 can claim the whole core is testable
 * with no audio hardware: a test drives the timeline itself, in a loop, with
 * no threads and no real time elapsing.
 *
 * TWO MODES, and they are mutually exclusive:
 *
 *   DRIVEN (this header). Never call start(). The test calls
 *   apr_capture_fake_advance() with monotonically increasing tick values and
 *   gets exactly the frames that a source of the configured rate error would
 *   have produced by then. Deterministic to the byte: the samples are a pure
 *   function of the absolute frame index, so the same tick sequence always
 *   yields the same audio, and how the sequence is chopped into calls makes no
 *   difference at all.
 *
 *   REAL TIME (capture.h start/stop). A pacing thread calls advance() against
 *   apr_qpc_now(). Used for the handful of tests that want a source behaving
 *   like a live one; useless for a three-hour session.
 *
 * Ticks are in QueryPerformanceCounter units at apr_qpc_freq(), the same units
 * AprCaptureStatus::anchor_ticks and clock.h use -- so a test can hand the very
 * same tick values to apr_clock_drift and compare.
 */
#ifndef APPRECORDER_CAPTURE_FAKE_H
#define APPRECORDER_CAPTURE_FAKE_H

#include <stdint.h>
#include "capture.h"

/* Generate every frame due at `now_ticks` and write it into the ring.
 *
 * The FIRST call anchors the source at `now_ticks` and produces no frames --
 * exactly like a real capture, whose anchor is the arrival of its first buffer.
 * Every later call produces
 *
 *     floor((now_ticks - anchor) * rate * (1e6 + ppm) / (qpc_freq * 1e6))
 *
 * frames in total, computed exactly in 128 bits with no accumulation, so a
 * three-hour timeline is off by less than one frame however finely it is
 * stepped (clock.h explains why that matters).
 *
 * Going backwards in time, or calling this on a source that has been start()ed,
 * is APR_E_STATE. Calling it on a source that is not a fake is
 * APR_E_INVALID_ARG. */
AprErr apr_capture_fake_advance(AprCapture *c, uint64_t now_ticks);

/* The tick rate the fake is using, i.e. apr_qpc_freq(). Here so a test can
 * build its timeline without assuming a value -- design 5.2 is emphatic that
 * QPF is not 10 MHz just because it was on one machine. */
uint64_t apr_capture_fake_tick_rate(const AprCapture *c);

#endif /* APPRECORDER_CAPTURE_FAKE_H */
