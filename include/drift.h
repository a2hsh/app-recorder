/*
 * drift.h -- the PI controller that holds a device capture on the engine
 * timeline (design section 5.2 step 4).
 *
 * WHO NEEDS THIS, AND WHO DOES NOT
 *
 *   Only DEVICE captures. The spike measured process taps to be gapless, 100%
 *   fill, and locked to the audio engine's own clock; they ARE the reference
 *   timeline and are consumed untouched. Running a controller on one measures
 *   the noise floor and nothing else, and an earlier draft of the design that
 *   said otherwise was wrong. A hardware crystal is the only thing in the
 *   system that genuinely runs at its own rate.
 *
 * THE ERROR SIGNAL IS BACKLOG, AND THAT IS NOT AN ARBITRARY CHOICE
 *
 *   backlog = frames the device has produced - frames the resampler has
 *   consumed. Both are exact: the producer's cursor is an integer, the
 *   consumer's position is Q32.32 accumulated without rounding. So the error
 *   is exact too, at every session length.
 *
 *   Holding backlog at a constant target is EXACTLY the statement "input
 *   position and output position stay in a fixed relationship" -- which is
 *   sample-accurate alignment. Controlling on instantaneous rate instead would
 *   leave a position offset nothing ever corrects, and controlling on
 *   cumulative position alone would swing hard at startup.
 *
 * FEED-FORWARD PLUS TRIM
 *
 *   apr_drift_ratio_q32() already gives the cumulative measured rate ratio:
 *   frames the device really delivered over frames QPC expected, averaged over
 *   the whole session so far and therefore very low noise. That is used
 *   directly as the feed-forward term -- it is the right steady-state answer by
 *   construction, so the PI only ever has to correct the residual backlog
 *   rather than learn the rate from nothing.
 *
 *   The trim is clamped hard (0.2% by default, about 3.5 cents) and that clamp
 *   is the "applied gradually so it is never audible" requirement made
 *   structural rather than hoped for. A backlog error large enough to saturate
 *   it is worked off over seconds, not corrected in one tick.
 *
 * TUNING
 *
 *   Placing a double pole at (1 - N/tau) on the integrating plant gives
 *
 *       Kp = 2 / tau_frames        Ki = 1 / tau_frames^2   (against integ*N)
 *
 *   which is critically damped, has no overshoot to ring the backlog, and is
 *   independent of the block size -- so a bus whose tick size varies does not
 *   retune itself by accident.
 *
 * THREAD SAFETY: a value the caller owns. Pure arithmetic, no allocation.
 */
#ifndef APPRECORDER_DRIFT_H
#define APPRECORDER_DRIFT_H

#include <stdint.h>

#include "clock.h"

/* Loop time constant. Ten seconds is slow enough that the ratio never moves
 * audibly and fast enough that a startup transient is gone long before a
 * recording matters. */
#define APR_DRIFT_TAU_SEC 10.0

/* Hard ceiling on the fractional correction: 0.2%, about 3.5 cents. */
#define APR_DRIFT_MAX_TRIM 0.002

typedef struct AprDriftCtl {
    /* configuration */
    double tau_frames;    /* loop time constant, in engine frames */
    double target;        /* backlog to hold, in device frames */
    double max_trim;      /* clamp on the fractional correction */
    double kp, ki;        /* derived from tau_frames; see the header */

    /* state */
    double   integ;       /* integral of the backlog error, frame-ticks */
    double   error;       /* last backlog error, frames */
    double   trim;        /* last fractional correction applied */
    uint64_t ratio_q32;   /* last ratio issued */
    uint64_t updates;
} AprDriftCtl;

/* `target_backlog` is the backlog the loop holds, in frames. The caller picks
 * it: it is the mixer's lookbehind, because a bus running that far behind wall
 * clock has exactly that much data waiting in every ring.
 *
 * `tau_seconds` <= 0 selects APR_DRIFT_TAU_SEC. */
void apr_drift_ctl_init(AprDriftCtl *c, uint32_t sample_rate,
                        double target_backlog, double tau_seconds);

/* One tick.
 *
 *   `d`            cumulative QPC-vs-delivered comparison for this source,
 *                  from apr_clock_drift(). Supplies the feed-forward. May be
 *                  NULL before the source has anchored, which yields 1.0.
 *   `backlog`      produced frames minus consumed frames, right now.
 *   `block_frames` output frames this tick covers; scales the integrator so
 *                  the tuning does not depend on the block size.
 *
 * Returns the resample ratio in Q32.32 -- input (device) frames per output
 * (engine) frame -- ready for apr_resampler_set_ratio_q32. */
uint64_t apr_drift_ctl_update(AprDriftCtl *c, const AprDrift *d,
                              double backlog, uint64_t block_frames);

/* Last backlog error in frames: what the multi-hour alignment test asserts on.
 * Positive means the source is ahead of where the loop wants it. */
double apr_drift_ctl_error(const AprDriftCtl *c);

/* Last correction actually applied, in parts per million. For the UI and for
 * logs; never feed it back into position arithmetic. */
double apr_drift_ctl_trim_ppm(const AprDriftCtl *c);

#endif /* APPRECORDER_DRIFT_H */
