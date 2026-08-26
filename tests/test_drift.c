/*
 * test_drift.c -- the PI controller, simulated over sessions hours long.
 *
 * NO REAL TIME ELAPSES HERE. The whole timeline is synthetic QPC ticks, so a
 * three-hour session runs in milliseconds and can be run on every build. That
 * is the only way the sync guarantee ever gets tested at the length where it
 * actually matters.
 *
 * QPF is deliberately 3,579,545 Hz, not the 10 MHz of the development machine,
 * because at 10 MHz a QPC tick and a 100 ns unit coincide and hide an entire
 * class of unit bug (design 5.2). Nothing here can pass by accident that way.
 */
#include "test_runner.h"
#include "drift.h"
#include "clock.h"

#include <math.h>

#define QPF        3579545ull
#define RATE       48000u
#define BLOCK      480u          /* 10 ms, what WASAPI hands us in practice */
#define LOOKBEHIND 2400u         /* 50 ms: the mixer's lag behind wall clock */

/* Device frames produced `elapsed` ticks after the anchor, at a crystal
 * running `ppm` parts per million fast. Exact integer arithmetic. */
static uint64_t produced_frames(uint64_t elapsed, int32_t ppm)
{
    return apr_mul_div_u64(elapsed,
                           (uint64_t)RATE * (uint64_t)(1000000 + ppm),
                           QPF * 1000000ull, NULL);
}

typedef struct SimResult {
    double   final_error;      /* backlog minus target, frames */
    double   worst_error;
    double   worst_trim_ppm;
    double   final_ratio;
    uint64_t produced;
    uint64_t consumed_q32;
    uint64_t out_frames;
} SimResult;

/* Run `seconds` of session. `correct` off means the naive append everyone
 * writes first: consume one input frame per output frame and let the error
 * pile up. */
static SimResult simulate(int32_t ppm, double seconds, int correct)
{
    AprClock    clk;
    AprDriftCtl ctl;
    SimResult   r;
    const uint64_t t0    = 1234567ull;      /* an anchor that is not zero */
    const uint64_t ticks = (uint64_t)(seconds * (double)RATE / (double)BLOCK);
    uint64_t consumed_q32 = 0, out = 0, produced = 0, n;
    double   worst = 0.0, worst_trim = 0.0;
    uint64_t ratio = (uint64_t)1 << 32;

    apr_clock_init(&clk, QPF, RATE);
    apr_clock_anchor(&clk, t0);
    apr_drift_ctl_init(&ctl, RATE, (double)LOOKBEHIND, 0.0);

    for (n = 0; n < ticks; n++) {
        /* Wall clock at this tick: the mixer is LOOKBEHIND frames behind. */
        uint64_t now = apr_clock_frame_ticks(&clk, (n + 1) * BLOCK + LOOKBEHIND);
        AprDrift d;
        double   backlog;

        produced = produced_frames(now - t0, ppm);

        /* Consume this tick at the ratio the controller set last tick, THEN
         * measure. Measuring before consuming would leave a constant one-block
         * offset that looks like drift and is not. The reader in source.c does
         * it in this order for the same reason. */
        consumed_q32 += ratio * BLOCK;
        out          += BLOCK;
        backlog       = (double)produced - (double)consumed_q32 / 4294967296.0;

        d = apr_clock_drift(&clk, now, produced);
        if (correct) {
            ratio = apr_drift_ctl_update(&ctl, &d, backlog, BLOCK);
            if (fabs(apr_drift_ctl_error(&ctl)) > worst) worst = fabs(apr_drift_ctl_error(&ctl));
            if (fabs(apr_drift_ctl_trim_ppm(&ctl)) > worst_trim)
                worst_trim = fabs(apr_drift_ctl_trim_ppm(&ctl));
        }
    }

    r.final_error    = (double)produced - (double)consumed_q32 / 4294967296.0
                     - (double)LOOKBEHIND;
    r.worst_error    = worst;
    r.worst_trim_ppm = worst_trim;
    r.final_ratio    = (double)ratio / 4294967296.0;
    r.produced       = produced;
    r.consumed_q32   = consumed_q32;
    r.out_frames     = out;
    return r;
}

/* ------------------------------------------------------------------------- */

TEST(an_unanchored_source_gets_the_identity_ratio)
{
    AprDriftCtl c;

    apr_drift_ctl_init(&c, RATE, 2400.0, 0.0);
    ASSERT_EQ_U64((uint64_t)1 << 32, apr_drift_ctl_update(&c, NULL, 2400.0, BLOCK));
    ASSERT_NEAR(0.0, apr_drift_ctl_error(&c), 0.0);
}

TEST(a_perfect_crystal_is_left_alone)
{
    SimResult r = simulate(0, 600.0, 1);

    ASSERT_NEAR(1.0, r.final_ratio, 1e-6);
    ASSERT_TRUE(fabs(r.final_error) < 1.0);
    ASSERT_TRUE(r.worst_trim_ppm < 100.0);
}

TEST(three_hours_at_thirty_ppm_ends_inside_one_sample)
{
    /* The headline requirement: a simulated multi-hour session must finish
     * with alignment error below one sample. */
    SimResult r = simulate(30, 3.0 * 3600.0, 1);

    printf("      3h @ +30ppm: final error %.4f frames, worst %.1f, "
           "trim <= %.1f ppm, ratio %.9f\n",
           r.final_error, r.worst_error, r.worst_trim_ppm, r.final_ratio);

    ASSERT_TRUE(fabs(r.final_error) < 1.0);
    /* The ratio settles on the crystal's real rate to a few ppm. It is not
     * exactly 1.000030 and should not be: the producer's cursor is a whole
     * number of frames, so the feed-forward is quantised and the PI carries a
     * small standing trim to make up the fraction. That standing trim IS the
     * sub-sample accuracy being bought. */
    ASSERT_NEAR(1.000030, r.final_ratio, 1e-5);
}

TEST(three_hours_at_minus_thirty_ppm_ends_inside_one_sample)
{
    SimResult r = simulate(-30, 3.0 * 3600.0, 1);

    ASSERT_TRUE(fabs(r.final_error) < 1.0);
    ASSERT_NEAR(0.999970, r.final_ratio, 1e-5);
}

TEST(a_badly_out_crystal_still_converges)
{
    /* 200 ppm is far worse than any real device; if the loop only worked for
     * small errors that would be a tuning accident, not a design. */
    SimResult r = simulate(200, 3.0 * 3600.0, 1);

    ASSERT_TRUE(fabs(r.final_error) < 1.0);
    ASSERT_NEAR(1.000200, r.final_ratio, 1e-5);
}

TEST(uncorrected_the_same_session_drifts_by_a_third_of_a_second)
{
    /* The control. Without this the test above could pass on a stopped clock:
     * it proves the timeline really does diverge when nothing corrects it. */
    SimResult r = simulate(30, 3.0 * 3600.0, 0);

    printf("      3h @ +30ppm uncorrected: %.0f frames adrift (%.3f s)\n",
           r.final_error, r.final_error / (double)RATE);

    ASSERT_GT_INT(15000, (long long)r.final_error);
    ASSERT_LT_INT(16100, (long long)r.final_error);
}

TEST(the_correction_is_never_audible)
{
    /* "Applied gradually so it is never audible" is a real requirement, and
     * the clamp is what makes it structural. 2000 ppm is 3.5 cents. */
    SimResult r = simulate(200, 3600.0, 1);

    ASSERT_TRUE(r.worst_trim_ppm <= APR_DRIFT_MAX_TRIM * 1e6 + 1e-9);
}

TEST(a_startup_backlog_error_is_worked_off_not_snapped_out)
{
    /* Feed a large error and watch how fast the ratio moves. A controller that
     * corrected it in one tick would be a pitch jump. */
    AprDriftCtl c;
    int    i;
    double backlog = 2400.0 + 5000.0;    /* 100 ms too much data buffered */
    double first_trim;

    apr_drift_ctl_init(&c, RATE, 2400.0, 0.0);
    apr_drift_ctl_update(&c, NULL, backlog, BLOCK);
    first_trim = apr_drift_ctl_trim_ppm(&c);
    ASSERT_TRUE(first_trim <= APR_DRIFT_MAX_TRIM * 1e6 + 1e-9);

    /* Consume the surplus at the ratio the loop asks for, 10 ms at a time. */
    for (i = 0; i < 200; i++) {
        uint64_t ratio = apr_drift_ctl_update(&c, NULL, backlog, BLOCK);
        backlog -= ((double)ratio / 4294967296.0 - 1.0) * (double)BLOCK;
    }
    ASSERT_TRUE(backlog < 2400.0 + 5000.0);   /* moving the right way */
    ASSERT_TRUE(backlog > 2400.0 + 3000.0);   /* but only just: 2 s of work */
}

TEST(the_integrator_does_not_wind_up_while_the_trim_is_pinned)
{
    /* A saturated integrator takes as long to discharge as it took to charge,
     * and that overshoot is the audible swing the clamp exists to prevent. */
    AprDriftCtl c;
    int i;

    apr_drift_ctl_init(&c, RATE, 0.0, 0.0);
    for (i = 0; i < 20000; i++) apr_drift_ctl_update(&c, NULL, 100000.0, BLOCK);
    ASSERT_NEAR(0.0, c.integ, 0.0);

    /* Error clears: the trim must come straight back, not linger. */
    apr_drift_ctl_update(&c, NULL, 0.0, BLOCK);
    ASSERT_NEAR(0.0, apr_drift_ctl_trim_ppm(&c), 1e-6);
}

TEST(a_source_that_stops_delivering_does_not_run_the_ratio_away)
{
    /* A dead source pins the trim and stops there; the ratio must stay inside
     * something a resampler can honour. */
    AprDriftCtl c;
    int i;
    uint64_t ratio = 0;

    apr_drift_ctl_init(&c, RATE, 2400.0, 0.0);
    for (i = 0; i < 100000; i++) ratio = apr_drift_ctl_update(&c, NULL, 0.0, BLOCK);
    ASSERT_TRUE((double)ratio / 4294967296.0 > 0.99);
    ASSERT_TRUE((double)ratio / 4294967296.0 < 1.0);
}

TEST(null_controller_is_survived)
{
    ASSERT_EQ_U64((uint64_t)1 << 32, apr_drift_ctl_update(NULL, NULL, 0.0, 480));
    ASSERT_NEAR(0.0, apr_drift_ctl_error(NULL), 0.0);
    ASSERT_NEAR(0.0, apr_drift_ctl_trim_ppm(NULL), 0.0);
    apr_drift_ctl_init(NULL, RATE, 0.0, 0.0);
}
