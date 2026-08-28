/*
 * drift.c -- PI control of a device capture's resample ratio. See
 * include/drift.h for the error signal, the tuning and why process taps never
 * come near this file.
 */
#include "drift.h"

#define Q32_ONE 4294967296.0

/* Below this much elapsed time the cumulative rate ratio is dominated by the
 * quantisation of a partly-filled first buffer, so the feed-forward is held at
 * 1.0 and the PI does all the work. One second is ~100 buffers. */
#define APR_DRIFT_FF_MIN_FRAMES 48000

void apr_drift_ctl_init(AprDriftCtl *c, uint32_t sample_rate,
                        double target_backlog, double tau_seconds)
{
    if (!c) return;
    if (sample_rate == 0) sample_rate = 48000;
    if (!(tau_seconds > 0.0)) tau_seconds = APR_DRIFT_TAU_SEC;

    c->tau_frames = tau_seconds * (double)sample_rate;
    c->target     = target_backlog > 0.0 ? target_backlog : 0.0;
    c->max_trim   = APR_DRIFT_MAX_TRIM;
    c->kp         = 2.0 / c->tau_frames;
    c->ki         = 1.0 / (c->tau_frames * c->tau_frames);

    c->integ     = 0.0;
    c->error     = 0.0;
    c->trim      = 0.0;
    c->ratio_q32 = (uint64_t)1 << 32;
    c->updates   = 0;
}

/* See drift.h. The gains, the target and the clamp are configuration and stay;
 * `trim` and `ratio_q32` are the last output, and they go back to "no
 * correction" so that the first tick after a resume issues a ratio built from
 * the feed-forward alone rather than from a correction the reset just
 * invalidated. */
void apr_drift_ctl_reset(AprDriftCtl *c)
{
    if (!c) return;
    c->integ     = 0.0;
    c->error     = 0.0;
    c->trim      = 0.0;
    c->ratio_q32 = (uint64_t)1 << 32;
    c->updates   = 0;
}

uint64_t apr_drift_ctl_update(AprDriftCtl *c, const AprDrift *d,
                              double backlog, uint64_t block_frames)
{
    double ff = 1.0, trim, ratio;
    int    clamped = 0;

    if (!c) return (uint64_t)1 << 32;

    /* Feed-forward: the cumulative measured rate ratio, which is the right
     * steady-state answer by construction. Ignored until enough time has
     * elapsed for it to mean anything. */
    if (d && d->expected_frames >= APR_DRIFT_FF_MIN_FRAMES && d->actual_frames > 0) {
        ff = (double)apr_drift_ratio_q32(d) / Q32_ONE;
    }

    c->error = backlog - c->target;

    trim = c->kp * c->error + c->ki * c->integ;
    if (trim > c->max_trim)  { trim = c->max_trim;  clamped = 1; }
    if (trim < -c->max_trim) { trim = -c->max_trim; clamped = 1; }

    /* Anti-windup: an integrator that keeps charging while the output is
     * pinned takes as long to discharge, and that overshoot IS the audible
     * pitch swing this clamp exists to prevent. */
    if (!clamped) c->integ += c->error * (double)block_frames;

    ratio = ff * (1.0 + trim);
    if (ratio < 0.5)  ratio = 0.5;
    if (ratio > 2.0)  ratio = 2.0;

    c->trim      = trim;
    c->ratio_q32 = (uint64_t)(ratio * Q32_ONE + 0.5);
    c->updates++;
    return c->ratio_q32;
}

double apr_drift_ctl_error(const AprDriftCtl *c)    { return c ? c->error : 0.0; }
double apr_drift_ctl_trim_ppm(const AprDriftCtl *c) { return c ? c->trim * 1e6 : 0.0; }
