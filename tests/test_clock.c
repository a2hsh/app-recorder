/*
 * test_clock.c -- core/clock.c: QPC arithmetic and drift.
 *
 * Design section 5 makes sample-accurate alignment the whole point of this
 * codebase, and says alignment cannot be retrofitted into files already
 * written. So the bar here is not "close enough": every conversion must be
 * EXACT, and must stay exact across a multi-hour session.
 *
 * No real time elapses in this file. Every timeline is synthetic tick values.
 */
#include "test_runner.h"
#include "clock.h"

/* An independent 64x64->128 multiply, written from scratch in 32-bit limbs, so
 * the exactness checks below do not lean on the same intrinsic clock.c uses. */
static void ref_mul128(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t a0 = a & 0xffffffffu, a1 = a >> 32;
    uint64_t b0 = b & 0xffffffffu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);

    *lo = (p00 & 0xffffffffu) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/* q*d + r == a*b, in full 128-bit precision. */
static int ref_exact(uint64_t a, uint64_t b, uint64_t d, uint64_t q, uint64_t r)
{
    uint64_t phi, plo, qhi, qlo, sum;

    ref_mul128(a, b, &phi, &plo);
    ref_mul128(q, d, &qhi, &qlo);
    sum = qlo + r;
    if (sum < qlo) qhi++;
    return qhi == phi && sum == plo;
}

/* ---- the 128-bit primitive ---------------------------------------------- */

TEST(mul_div_is_exact_and_reports_the_remainder)
{
    static const struct { uint64_t a, b, d; } cases[] = {
        { 0, 12345, 7 },
        { 1, 1, 1 },
        { 48000, 10000000ull, 3579545ull },
        { 108000000000ull, 48000ull, 10000000ull },   /* 3 h at 10 MHz */
        { 0xFFFFFFFFFFFFFFFFull, 1, 3 },
        { 0xFFFFFFFFull, 0xFFFFFFFFull, 7 },
        { 1234567890123456789ull, 999999937ull, 1000000007ull }
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint64_t rem = 0;
        uint64_t q = apr_mul_div_u64(cases[i].a, cases[i].b, cases[i].d, &rem);

        if (rem >= cases[i].d) FAIL("remainder is not less than the divisor");
        if (!ref_exact(cases[i].a, cases[i].b, cases[i].d, q, rem)) {
            FAIL("q*d + r != a*b: the conversion is not exact");
        }
    }
    ASSERT_EQ_INT(0, 0);
}

TEST(mul_div_saturates_rather_than_faulting_on_overflow)
{
    /* _udiv128 raises #DE when the quotient will not fit in 64 bits. A crash
     * in the drift path would take a whole session down. */
    uint64_t q = apr_mul_div_u64(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 1, NULL);
    ASSERT_EQ_U64(0xFFFFFFFFFFFFFFFFull, q);
}

TEST(mul_div_by_zero_is_defined_not_fatal)
{
    uint64_t rem = 7;
    ASSERT_EQ_U64(0, apr_mul_div_u64(100, 100, 0, &rem));
    ASSERT_EQ_U64(0, rem);
}

TEST(signed_mul_div_truncates_toward_zero_symmetrically)
{
    ASSERT_EQ_INT(-479, apr_mul_div_i64(-35795, 48000, 3579545));
    ASSERT_EQ_INT(479, apr_mul_div_i64(35795, 48000, 3579545));
    ASSERT_EQ_INT(0, apr_mul_div_i64(0, 48000, 3579545));
}

/* ---- unit conversions ---------------------------------------------------- */

TEST(ticks_and_hundred_nanosecond_units_convert_both_ways)
{
    /* Modern Windows reports 10 MHz, which makes a tick exactly one hns unit;
     * pu64QPCPosition then needs no scaling at all. */
    ASSERT_EQ_U64(1234567, apr_ticks_to_hns(1234567, 10000000ull));
    ASSERT_EQ_U64(1234567, apr_hns_to_ticks(1234567, 10000000ull));

    /* And on a machine that does not: 1 second either way. */
    ASSERT_EQ_U64(10000000ull, apr_ticks_to_hns(3579545ull, 3579545ull));
    ASSERT_EQ_U64(3579545ull, apr_hns_to_ticks(10000000ull, 3579545ull));
}

TEST(frames_and_hundred_nanosecond_units_convert_both_ways)
{
    ASSERT_EQ_U64(48000, apr_hns_to_frames(10000000ull, 48000, NULL));
    ASSERT_EQ_U64(10000000ull, apr_frames_to_hns(48000, 48000));
    ASSERT_EQ_U64(44100, apr_hns_to_frames(10000000ull, 44100, NULL));
}

TEST(a_conversion_hands_back_the_sub_frame_remainder)
{
    uint64_t rem = 0;

    /* 312 hns is 1.4976 frames at 48 kHz. The result floors to 1; the 0.4976
     * must be recoverable, or a resampler cannot hold its phase. The remainder
     * is the numerator over the conversion's divisor -- APR_HNS_PER_SEC here. */
    ASSERT_EQ_U64(1, apr_hns_to_frames(312, 48000, &rem));
    ASSERT_EQ_U64(4976000, rem);
    ASSERT_NEAR(0.4976, (double)rem / (double)APR_HNS_PER_SEC, 1e-12);
}

TEST(ticks_to_frames_is_exact_at_an_awkward_qpc_frequency)
{
    uint64_t rem = 0;
    uint64_t f   = apr_ticks_to_frames(3579545ull, 3579545ull, 48000, &rem);

    ASSERT_EQ_U64(48000, f);
    ASSERT_EQ_U64(0, rem);
}

/* ---- the multi-hour timeline -------------------------------------------- */

#define QPF_AWKWARD 3579545ull       /* the legacy ACPI timer rate */
#define RATE        48000u
#define HOURS       3
#define SESSION_TICKS (QPF_AWKWARD * 3600ull * HOURS)

TEST(expected_frames_stays_exact_across_a_three_hour_session)
{
    AprClock c;
    uint64_t t;
    uint64_t step = SESSION_TICKS / 20000;   /* 20k checkpoints over 3 hours */

    apr_clock_init(&c, QPF_AWKWARD, RATE);
    apr_clock_anchor(&c, 0);

    for (t = 0; t <= SESSION_TICKS; t += step) {
        /* Independent reference: t * 48000 peaks at 1.9e15, well inside 64
         * bits, so this needs no 128-bit help and shares no code with the
         * implementation. */
        int64_t want = (int64_t)((t * (uint64_t)RATE) / QPF_AWKWARD);
        if (apr_clock_expected_frames(&c, t) != want) {
            FAIL("expected_frames diverged from the exact value");
        }
    }
    ASSERT_EQ_INT(0, 0);
}

TEST(an_anchor_other_than_zero_shifts_nothing_else)
{
    AprClock c;
    uint64_t anchor = 987654321987ull;
    uint64_t t;

    apr_clock_init(&c, QPF_AWKWARD, RATE);
    apr_clock_anchor(&c, anchor);

    for (t = 0; t <= SESSION_TICKS; t += SESSION_TICKS / 5000) {
        int64_t want = (int64_t)((t * (uint64_t)RATE) / QPF_AWKWARD);
        if (apr_clock_expected_frames(&c, anchor + t) != want) {
            FAIL("anchoring changed the arithmetic");
        }
    }
    ASSERT_EQ_INT(0, 0);
}

TEST(a_timestamp_before_the_anchor_goes_negative_rather_than_wrapping)
{
    AprClock c;
    apr_clock_init(&c, 10000000ull, 48000);
    apr_clock_anchor(&c, 1000000000ull);

    ASSERT_EQ_INT(-48000, apr_clock_expected_frames(&c, 1000000000ull - 10000000ull));
    ASSERT_EQ_INT(0, apr_clock_expected_frames(&c, 1000000000ull));
}

/*
 * The reason apr_clock_expected_frames takes an absolute timestamp rather than
 * a delta: summing per-buffer conversions floors 1.08 million times and the
 * discarded fractions never come back. Over three hours that is nearly nine
 * minutes of error. Converting from the anchor floors exactly once, so the
 * error is bounded below one frame no matter how long the session runs.
 */
TEST(absolute_conversion_beats_summing_per_buffer_conversions)
{
    AprClock c;
    uint64_t t = 0, dt = 35795;         /* ~one 480-frame buffer at 3.58 MHz */
    int64_t  incremental = 0;
    int      k, buffers = 1080000;      /* three hours of 10 ms buffers */
    int64_t  exact;

    apr_clock_init(&c, QPF_AWKWARD, RATE);
    apr_clock_anchor(&c, 0);

    for (k = 0; k < buffers; k++) {
        incremental += (int64_t)apr_ticks_to_frames(dt, QPF_AWKWARD, RATE, NULL);
        t += dt;
    }
    exact = apr_clock_expected_frames(&c, t);

    ASSERT_EQ_INT((int64_t)((t * (uint64_t)RATE) / QPF_AWKWARD), exact);
    /* Same timeline, naive method: hundreds of thousands of frames adrift. */
    ASSERT_GT_INT(100000, exact - incremental);
}

TEST(a_frame_index_converts_back_to_the_tick_it_started_at)
{
    AprClock c;
    uint64_t frame;

    apr_clock_init(&c, QPF_AWKWARD, RATE);
    apr_clock_anchor(&c, 500);

    for (frame = 0; frame < 518400000ull; frame += 518400000ull / 5000) {
        uint64_t tick = apr_clock_frame_ticks(&c, frame);
        /* Round-tripping must land on the same frame: the tick returned is the
         * first tick at which that frame has elapsed. */
        if (apr_clock_expected_frames(&c, tick) != (int64_t)frame) {
            FAIL("frame -> tick -> frame did not round-trip");
        }
    }
    ASSERT_EQ_INT(0, 0);
}

/* ---- drift --------------------------------------------------------------- */

TEST(a_source_running_at_the_nominal_rate_shows_no_drift)
{
    AprClock c;
    AprDrift d;
    uint64_t t = QPF_AWKWARD * 3600ull;          /* one hour in */
    uint64_t actual = (t * (uint64_t)RATE) / QPF_AWKWARD;

    apr_clock_init(&c, QPF_AWKWARD, RATE);
    apr_clock_anchor(&c, 0);

    d = apr_clock_drift(&c, t, actual);
    ASSERT_EQ_INT(0, d.delta_frames);
    ASSERT_EQ_INT(0, apr_drift_ppb(&d));
}

/* +30 ppm, the case design section 5.2 names for a DEVICE capture -- the only
 * source kind that carries real drift, now that the spike has shown process
 * taps to be the reference timeline. The source is modelled as an exact
 * rational rate so the expected answer can be stated, not approximated. */
TEST(a_source_thirty_ppm_fast_is_measured_as_thirty_ppm)
{
    AprClock c;
    AprDrift d;
    uint64_t t      = SESSION_TICKS;                       /* three hours */
    uint64_t actual = apr_mul_div_u64(t, 4800144ull, QPF_AWKWARD * 100ull, NULL);
    int64_t  expected_surplus;

    apr_clock_init(&c, QPF_AWKWARD, RATE);
    apr_clock_anchor(&c, 0);
    d = apr_clock_drift(&c, t, actual);

    /* 3 h x 48000 x 30e-6 = 15552 frames of surplus, about a third of a
     * second: audible as a lip-sync error, invisible frame by frame. */
    expected_surplus = 15552;
    ASSERT_EQ_INT(518400000, d.expected_frames);
    ASSERT_LE_INT(1, (long long)llabs(d.delta_frames + expected_surplus));
    ASSERT_TRUE(d.delta_frames < 0);          /* surplus, not a gap */

    ASSERT_LE_INT(3, llabs(apr_drift_ppb(&d) - 30000));
    ASSERT_NEAR(30.0, apr_drift_ppm(&d), 0.01);
}

TEST(a_source_thirty_ppm_slow_is_measured_the_other_way)
{
    AprClock c;
    AprDrift d;
    uint64_t t      = SESSION_TICKS;
    uint64_t actual = apr_mul_div_u64(t, 4799856ull, QPF_AWKWARD * 100ull, NULL);

    apr_clock_init(&c, QPF_AWKWARD, RATE);
    apr_clock_anchor(&c, 0);
    d = apr_clock_drift(&c, t, actual);

    ASSERT_TRUE(d.delta_frames > 0);          /* frames missing */
    ASSERT_LE_INT(1, llabs(d.delta_frames - 15552));
    ASSERT_LE_INT(3, llabs(apr_drift_ppb(&d) + 30000));
}

TEST(a_source_that_produced_nothing_reads_as_a_gap_of_the_whole_interval)
{
    AprClock c;
    AprDrift d;
    uint64_t t = QPF_AWKWARD * 5ull;     /* five seconds of dropout */

    apr_clock_init(&c, QPF_AWKWARD, RATE);
    apr_clock_anchor(&c, 0);
    d = apr_clock_drift(&c, t, 0);

    /* A device capture that dropped out (design section 5.2 step 4): this is
     * exactly the number of frames of silence to synthesise to keep the track
     * aligned. Note the spike disproved the old claim that a *process* tap can
     * do this -- it delivers real zeroes at 100% fill instead. */
    ASSERT_EQ_INT(240000, d.delta_frames);
    ASSERT_EQ_INT(240000, d.expected_frames);
    ASSERT_EQ_INT(0, d.actual_frames);
}

TEST(drift_before_any_frames_have_elapsed_does_not_divide_by_zero)
{
    AprClock c;
    AprDrift d;

    apr_clock_init(&c, QPF_AWKWARD, RATE);
    apr_clock_anchor(&c, 1000);

    d = apr_clock_drift(&c, 1000, 0);
    ASSERT_EQ_INT(0, d.expected_frames);
    ASSERT_EQ_INT(0, apr_drift_ppb(&d));
    ASSERT_NEAR(0.0, apr_drift_ppm(&d), 0.0);
}

TEST(the_resample_ratio_is_exact_fixed_point)
{
    AprClock c;
    AprDrift d;
    uint64_t q32;

    apr_clock_init(&c, 10000000ull, 48000);
    apr_clock_anchor(&c, 0);

    /* Source produced exactly half the frames the clock expected. */
    d = apr_clock_drift(&c, 10000000ull, 24000);
    q32 = apr_drift_ratio_q32(&d);
    ASSERT_EQ_U64(1ull << 31, q32);            /* 0.5 in Q32.32 */

    d = apr_clock_drift(&c, 10000000ull, 48000);
    ASSERT_EQ_U64(1ull << 32, apr_drift_ratio_q32(&d));   /* exactly 1.0 */
}

/* ---- the live clock ------------------------------------------------------ */

TEST(the_process_wide_qpc_frequency_is_available_and_stable)
{
    uint64_t f = apr_qpc_freq();
    ASSERT_GT_INT(0, (long long)f);
    ASSERT_EQ_U64(f, apr_qpc_freq());          /* cached, not re-queried */
}

TEST(qpc_now_moves_forward)
{
    uint64_t a = apr_qpc_now();
    uint64_t b = apr_qpc_now();
    ASSERT_GE_INT((long long)a, (long long)b);
}

TEST(a_clock_reports_whether_it_has_been_anchored)
{
    AprClock c;
    apr_clock_init(&c, 10000000ull, 48000);
    ASSERT_FALSE(apr_clock_anchored(&c));
    apr_clock_anchor(&c, 12345);
    ASSERT_TRUE(apr_clock_anchored(&c));
    ASSERT_EQ_U64(12345, c.anchor_ticks);
}
