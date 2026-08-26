/*
 * test_resample.c -- the one resampler.
 *
 * Four properties matter, and they are the four this file pins down:
 *
 *   1. Ratio 1.0 is the identity. If it is not, every process tap that ever
 *      passes through here is quietly filtered.
 *   2. Group delay is zero. A resampler with latency shifts a device source
 *      against the process taps it is mixed with -- the exact desync the
 *      subsystem exists to prevent.
 *   3. DC gain is exactly 1 at every phase. Ripple here is a slow level wobble
 *      on sustained tones that a noise-based test would never see.
 *   4. The input position is EXACT. Everything about multi-hour alignment
 *      rests on pos += ratio never rounding.
 */
#include "test_runner.h"
#include "resample.h"

#include <math.h>
#include <string.h>

#define Q32_ONE (1ull << 32)

static uint64_t ratio_ppm(int32_t ppm)
{
    /* (1 + ppm/1e6) in Q32, exactly. */
    return (uint64_t)((1000000ll + ppm) * (int64_t)Q32_ONE / 1000000ll);
}

TEST(create_rejects_impossible_channel_counts)
{
    AprResampler *r = NULL;
    AprErr e;

    e = apr_resampler_create(0, 1.01, &r);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(r);

    e = apr_resampler_create(99, 1.01, &r);
    ASSERT_TRUE(apr_failed(&e));

    e = apr_resampler_create(2, 1.01, NULL);
    ASSERT_TRUE(apr_failed(&e));
}

TEST(unity_ratio_is_the_identity)
{
    AprResampler *r = NULL;
    float in[512], out[512];
    size_t i, used = 0, n;
    AprErr e = apr_resampler_create(1, 1.01, &r);

    ASSERT_FALSE(apr_failed(&e));
    for (i = 0; i < 512; i++) in[i] = (float)sin(0.11 * (double)i) * 0.7f;

    n = apr_resample(r, in, 512, &used, out, 512);
    ASSERT_EQ_INT(512, (int)used);
    ASSERT_GT_INT(400, (int)n);          /* the tail waits for lookahead */

    /* Bit-exact, not merely close: at zero phase the kernel is a unit impulse. */
    for (i = 0; i < n; i++) {
        if (in[i] != out[i]) {
            printf("      first difference at frame %zu: %.9g vs %.9g\n",
                   i, (double)in[i], (double)out[i]);
            break;
        }
    }
    ASSERT_EQ_INT((int)n, (int)i);
    apr_resampler_destroy(r);
}

TEST(group_delay_is_zero_so_an_impulse_stays_where_it_was)
{
    AprResampler *r = NULL;
    float in[256], out[256];
    size_t used = 0, n, i, peak = 0;
    AprErr e = apr_resampler_create(1, 1.01, &r);

    ASSERT_FALSE(apr_failed(&e));
    memset(in, 0, sizeof in);
    in[0] = 1.0f;                      /* the very first frame of the stream */

    n = apr_resample(r, in, 256, &used, out, 256);
    ASSERT_GT_INT(0, (int)n);
    for (i = 0; i < n; i++) if (fabs((double)out[i]) > fabs((double)out[peak])) peak = i;
    ASSERT_EQ_INT(0, (int)peak);
    ASSERT_NEAR(1.0, out[0], 1e-6);
    apr_resampler_destroy(r);
}

TEST(dc_gain_is_exactly_one_at_every_phase)
{
    /* An awkward ratio walks the phase through the whole table. A windowed
     * sinc that is not normalised wobbles here by ~1e-4. */
    AprResampler *r = NULL;
    float in[4096], out[4096];
    size_t used = 0, n, i;
    float worst = 0.0f;
    AprErr e = apr_resampler_create(1, 2.0, &r);

    ASSERT_FALSE(apr_failed(&e));
    apr_resampler_set_ratio_q32(r, (uint64_t)(1.0173 * 4294967296.0));
    for (i = 0; i < 4096; i++) in[i] = 0.5f;

    n = apr_resample(r, in, 4096, &used, out, 4096);
    ASSERT_GT_INT(1000, (int)n);
    /* Skip the priming edge, where the kernel is legitimately truncated. */
    for (i = 32; i < n; i++) {
        float d = (float)fabs((double)out[i] - 0.5);
        if (d > worst) worst = d;
    }
    ASSERT_NEAR(0.0, worst, 1e-6);
    apr_resampler_destroy(r);
}

TEST(input_position_is_exact_over_millions_of_frames)
{
    /* No audio, no buffers -- just the accumulator. This is the arithmetic the
     * multi-hour alignment guarantee rests on. */
    AprResampler *r = NULL;
    uint64_t ratio = ratio_ppm(30);
    uint64_t expect;
    size_t   produced = 0;
    int      block;
    float    in[1024], out[1024];
    AprErr   e = apr_resampler_create(1, 1.01, &r);

    ASSERT_FALSE(apr_failed(&e));
    apr_resampler_set_ratio_q32(r, ratio);
    memset(in, 0, sizeof in);

    for (block = 0; block < 4000; block++) {
        size_t space = apr_resampler_input_space(r);
        size_t feed  = space < 1024 ? space : 1024;
        size_t used  = 0;
        apr_resample(r, in, feed, &used, out, 0);
        ASSERT_EQ_INT((int)feed, (int)used);
        produced += apr_resample(r, NULL, 0, NULL, out, 512);
    }
    expect = (uint64_t)produced * ratio;
    ASSERT_EQ_U64(expect, apr_resampler_in_pos_q32(r));
    ASSERT_GT_INT(100000, (int)produced);
    apr_resampler_destroy(r);
}

TEST(a_fast_source_yields_fewer_output_frames_than_it_gave)
{
    /* 30 ppm fast: 1000000 input frames must become 999970 output frames, to
     * within the filter's lookahead. That difference IS the drift being
     * removed rather than accumulating as desync. */
    AprResampler *r = NULL;
    float in[1024], out[1024];
    size_t fed = 0, produced = 0;
    int block;
    AprErr e = apr_resampler_create(1, 1.01, &r);

    ASSERT_FALSE(apr_failed(&e));
    apr_resampler_set_ratio_q32(r, ratio_ppm(30));
    memset(in, 0, sizeof in);

    for (block = 0; block < 1000; block++) {
        size_t used = 0;
        produced += apr_resample(r, in, 1000, &used, out, 1024);
        fed += used;
        ASSERT_EQ_INT(1000, (int)used);
    }
    ASSERT_EQ_INT(1000000, (int)fed);
    /* 999970 exactly, plus at most the kernel lookahead still in the pipe. */
    ASSERT_GE_INT(999970 - 40, (int)produced);
    ASSERT_LE_INT(999970,      (int)produced);
    apr_resampler_destroy(r);
}

TEST(a_sine_survives_a_thirty_ppm_correction)
{
    /* Amplitude and continuity, not just frame counts: a broken kernel shows
     * up here as a collapsed peak or a discontinuity at the block joins. */
    AprResampler *r = NULL;
    float  in[960], out[1024];
    double phase = 0.0, step = 2.0 * 3.14159265358979 * 1000.0 / 48000.0;
    float  peak = 0.0f, biggest_jump = 0.0f, prev = 0.0f;
    int    block, warm = 0;
    size_t i;
    AprErr e = apr_resampler_create(1, 1.01, &r);

    ASSERT_FALSE(apr_failed(&e));
    apr_resampler_set_ratio_q32(r, ratio_ppm(30));

    for (block = 0; block < 100; block++) {
        size_t used = 0, n;
        for (i = 0; i < 960; i++) { in[i] = (float)(0.5 * sin(phase)); phase += step; }
        n = apr_resample(r, in, 960, &used, out, 1024);
        ASSERT_EQ_INT(960, (int)used);
        for (i = 0; i < n; i++) {
            if (warm > 64) {
                float a = (float)fabs((double)out[i]);
                float d = (float)fabs((double)out[i] - (double)prev);
                if (a > peak) peak = a;
                if (d > biggest_jump) biggest_jump = d;
            }
            prev = out[i];
            warm++;
        }
    }
    ASSERT_NEAR(0.5, peak, 0.002);
    /* One step of a 1 kHz sine at 48 kHz is at most 0.5*2*pi*1000/48000 = 0.066 */
    ASSERT_TRUE(biggest_jump < 0.08f);
    apr_resampler_destroy(r);
}

TEST(halving_the_rate_bandlimits_rather_than_aliasing)
{
    /* 2:1 downsample of a tone above the new Nyquist. Without the cutoff
     * scaling this folds back as a loud spurious tone; with it, it is gone. */
    AprResampler *r = NULL;
    float  in[8192], out[8192];
    double phase = 0.0, step = 2.0 * 3.14159265358979 * 18000.0 / 48000.0;
    size_t i, used = 0, n;
    float  peak = 0.0f;
    AprErr e = apr_resampler_create(1, 2.0, &r);

    ASSERT_FALSE(apr_failed(&e));
    apr_resampler_set_ratio_q32(r, 2 * Q32_ONE);
    for (i = 0; i < 8192; i++) { in[i] = (float)(0.5 * sin(phase)); phase += step; }

    n = apr_resample(r, in, 8192, &used, out, 8192);
    ASSERT_GT_INT(3000, (int)n);
    for (i = 200; i < n - 200; i++) {
        float a = (float)fabs((double)out[i]);
        if (a > peak) peak = a;
    }
    ASSERT_TRUE(peak < 0.02f);   /* -34 dB or better */
    apr_resampler_destroy(r);
}

TEST(all_input_is_absorbed_when_it_fits_in_the_reported_space)
{
    AprResampler *r = NULL;
    float in[4096], out[16];
    size_t space, used = 0;
    AprErr e = apr_resampler_create(2, 1.01, &r);

    ASSERT_FALSE(apr_failed(&e));
    memset(in, 0, sizeof in);
    space = apr_resampler_input_space(r);
    ASSERT_GT_INT(0, (int)space);
    ASSERT_LE_INT(2048, (int)space);

    /* Pull nothing at all: the input must still be taken in full. */
    apr_resample(r, in, space, &used, out, 0);
    ASSERT_EQ_INT((int)space, (int)used);
    ASSERT_EQ_INT(0, (int)apr_resampler_input_space(r));
    apr_resampler_destroy(r);
}

TEST(input_needed_covers_what_the_next_pull_will_ask_for)
{
    AprResampler *r = NULL;
    float in[4096], out[512];
    size_t need, used = 0, n;
    AprErr e = apr_resampler_create(1, 1.01, &r);

    ASSERT_FALSE(apr_failed(&e));
    memset(in, 0, sizeof in);
    need = apr_resampler_input_needed(r, 480);
    ASSERT_GT_INT(480, (int)need);

    n = apr_resample(r, in, need, &used, out, 480);
    ASSERT_EQ_INT((int)need, (int)used);
    ASSERT_EQ_INT(480, (int)n);
    apr_resampler_destroy(r);
}

TEST(reset_returns_to_the_start_of_the_stream)
{
    AprResampler *r = NULL;
    float in[256], out[256];
    size_t used = 0;
    AprErr e = apr_resampler_create(1, 1.01, &r);

    ASSERT_FALSE(apr_failed(&e));
    memset(in, 0, sizeof in);
    apr_resample(r, in, 256, &used, out, 256);
    ASSERT_GT_INT(0, (long long)(apr_resampler_in_pos_q32(r) >> 32));

    apr_resampler_reset(r);
    ASSERT_EQ_U64(0, apr_resampler_in_pos_q32(r));
    ASSERT_EQ_U64(Q32_ONE, apr_resampler_ratio_q32(r));
    apr_resampler_destroy(r);
}

TEST(ratio_is_clamped_to_what_the_kernel_was_sized_for)
{
    AprResampler *r = NULL;
    AprErr e = apr_resampler_create(1, 2.0, &r);

    ASSERT_FALSE(apr_failed(&e));
    apr_resampler_set_ratio_q32(r, 100 * Q32_ONE);
    ASSERT_EQ_U64(2 * Q32_ONE, apr_resampler_ratio_q32(r));
    apr_resampler_set_ratio_q32(r, 1);
    ASSERT_EQ_U64(Q32_ONE / 2, apr_resampler_ratio_q32(r));
    apr_resampler_destroy(r);
}

TEST(null_arguments_are_survived)
{
    size_t used = 12345;
    float out[4];

    ASSERT_EQ_INT(0, (int)apr_resample(NULL, NULL, 4, &used, out, 4));
    ASSERT_EQ_INT(0, (int)used);
    ASSERT_EQ_U64(0, apr_resampler_in_pos_q32(NULL));
    ASSERT_EQ_U64(Q32_ONE, apr_resampler_ratio_q32(NULL));
    ASSERT_EQ_INT(0, (int)apr_resampler_input_space(NULL));
    ASSERT_EQ_INT(0, (int)apr_resampler_input_needed(NULL, 10));
    apr_resampler_set_ratio_q32(NULL, Q32_ONE);
    apr_resampler_reset(NULL);
    apr_resampler_destroy(NULL);
}
