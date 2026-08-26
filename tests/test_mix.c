/*
 * test_mix.c -- PCM conversion, channel mapping and summing.
 *
 * The two properties worth defending here are the ones that go wrong quietly:
 * integer round trips must be EXACT (a wrong full-scale constant is a 0.003 dB
 * error nobody traces), and a span that is short or offset must leave the rest
 * of the accumulator alone (that is how sources at different start instants
 * stay aligned).
 */
#include "test_runner.h"
#include "mix.h"

#include <string.h>

TEST(sample_bytes_are_the_obvious_ones)
{
    ASSERT_EQ_INT(4, (int)apr_pcm_sample_bytes(APR_PCM_F32));
    ASSERT_EQ_INT(2, (int)apr_pcm_sample_bytes(APR_PCM_S16));
    ASSERT_EQ_INT(3, (int)apr_pcm_sample_bytes(APR_PCM_S24));
    ASSERT_EQ_INT(4, (int)apr_pcm_sample_bytes(APR_PCM_S32));
    ASSERT_EQ_INT(1, (int)apr_pcm_sample_bytes(APR_PCM_U8));
    ASSERT_EQ_INT(0, (int)apr_pcm_sample_bytes((AprPcmFormat)99));
}

TEST(s16_round_trip_is_exact_for_every_value)
{
    /* All 65536 int16 values, not a sample of them: the whole point of scaling
     * by 32768 rather than 32767 is that none of them move. */
    int     i;
    int16_t in[256], out[256];
    float   mid[256];
    int     mismatches = 0;

    for (i = -32768; i < 32768; i += 256) {
        int k;
        for (k = 0; k < 256; k++) in[k] = (int16_t)(i + k);
        apr_pcm_to_float(APR_PCM_S16, in, mid, 256);
        apr_pcm_from_float(mid, APR_PCM_S16, out, 256);
        for (k = 0; k < 256; k++) if (in[k] != out[k]) mismatches++;
    }
    ASSERT_EQ_INT(0, mismatches);
}

TEST(s16_full_scale_maps_to_the_power_of_two)
{
    int16_t in[3] = { -32768, 0, 16384 };
    float   f[3];

    apr_pcm_to_float(APR_PCM_S16, in, f, 3);
    ASSERT_NEAR(-1.0, f[0], 0.0);
    ASSERT_NEAR(0.0,  f[1], 0.0);
    ASSERT_NEAR(0.5,  f[2], 0.0);
}

TEST(float_over_full_scale_clamps_rather_than_wrapping)
{
    float   f[4] = { 1.0f, 2.5f, -1.0f, -3.0f };
    int16_t s[4];

    apr_pcm_from_float(f, APR_PCM_S16, s, 4);
    ASSERT_EQ_INT(32767,  s[0]);   /* +1.0 is the one value that cannot survive */
    ASSERT_EQ_INT(32767,  s[1]);
    ASSERT_EQ_INT(-32768, s[2]);
    ASSERT_EQ_INT(-32768, s[3]);
}

TEST(s24_round_trip_is_exact_including_the_sign_bit)
{
    /* Values around the 24-bit sign boundary, where a missing sign-extension
     * shows up as a full-scale polarity flip rather than a small error. */
    const int32_t vals[] = { 0, 1, -1, 8388607, -8388608, 4194304, -4194304, 12345 };
    unsigned char packed[8 * 3], again[8 * 3];
    float         f[8];
    size_t        i;

    for (i = 0; i < 8; i++) {
        uint32_t u = (uint32_t)vals[i];
        packed[3 * i]     = (unsigned char)(u & 0xFFu);
        packed[3 * i + 1] = (unsigned char)((u >> 8) & 0xFFu);
        packed[3 * i + 2] = (unsigned char)((u >> 16) & 0xFFu);
    }
    apr_pcm_to_float(APR_PCM_S24, packed, f, 8);
    ASSERT_NEAR(-1.0, f[4], 0.0);
    ASSERT_NEAR(0.5,  f[5], 0.0);
    apr_pcm_from_float(f, APR_PCM_S24, again, 8);
    ASSERT_MEM_EQ(packed, again, sizeof packed);
}

TEST(s32_and_u8_round_trip)
{
    const int32_t s32in[5] = { 0, 1, -1, 2147483647, -2147483647 - 1 };
    int32_t       s32out[5];
    unsigned char u8in[5] = { 0, 1, 128, 200, 255 }, u8out[5];
    float         f[5];

    apr_pcm_to_float(APR_PCM_S32, s32in, f, 5);
    ASSERT_NEAR(-1.0, f[4], 0.0);
    apr_pcm_from_float(f, APR_PCM_S32, s32out, 5);
    ASSERT_EQ_INT(0,               s32out[0]);
    ASSERT_EQ_INT(2147483647,      s32out[3]);
    ASSERT_EQ_INT(-2147483647 - 1, s32out[4]);

    apr_pcm_to_float(APR_PCM_U8, u8in, f, 5);
    ASSERT_NEAR(-1.0, f[0], 0.0);
    ASSERT_NEAR(0.0,  f[2], 0.0);
    apr_pcm_from_float(f, APR_PCM_U8, u8out, 5);
    ASSERT_MEM_EQ(u8in, u8out, sizeof u8in);
}

/* ---- non-finite input ------------------------------------------------------
 *
 * A NaN reaching an encoder used to come out as -32768: FULL-SCALE NEGATIVE
 * audio, because the cast to integer happened before the clamp and x86's
 * cvttsd2si returns INT64_MIN for NaN and for both infinities. AGENTS.md rule
 * 1 is why this has its own tests rather than a comment.
 *
 * F32 is deliberately exempt: it has no integer domain, so it has no hazard,
 * and float32 WAV is the archival path that must stay bit-exact.
 * ------------------------------------------------------------------------- */

static float nan_f(void)
{
    /* Built, not written as a literal, so no compiler flag can fold it away. */
    volatile float z = 0.0f;
    return z / z;
}

static float inf_f(int negative)
{
    volatile float z = 0.0f, one = negative ? -1.0f : 1.0f;
    return one / z;
}

TEST(a_nan_becomes_silence_not_full_scale_in_every_integer_format)
{
    float f[3];
    int16_t s16[3];
    int32_t s32[3];
    unsigned char s24[9], u8[3];

    f[0] = nan_f(); f[1] = 0.25f; f[2] = nan_f();

    apr_pcm_from_float(f, APR_PCM_S16, s16, 3);
    ASSERT_EQ_INT(0, s16[0]);
    ASSERT_EQ_INT(8192, s16[1]);      /* the finite neighbour is untouched */
    ASSERT_EQ_INT(0, s16[2]);

    apr_pcm_from_float(f, APR_PCM_S32, s32, 3);
    ASSERT_EQ_INT(0, s32[0]);
    ASSERT_EQ_INT(0, s32[2]);

    apr_pcm_from_float(f, APR_PCM_S24, s24, 3);
    ASSERT_EQ_INT(0, s24[0]);
    ASSERT_EQ_INT(0, s24[1]);
    ASSERT_EQ_INT(0, s24[2]);

    apr_pcm_from_float(f, APR_PCM_U8, u8, 3);
    ASSERT_EQ_INT(128, u8[0]);        /* silence in offset binary */
    ASSERT_EQ_INT(128, u8[2]);
}

TEST(infinities_saturate_to_the_right_end_of_the_format)
{
    float f[2];
    int16_t s16[2];
    int32_t s32[2];
    unsigned char u8[2];

    f[0] = inf_f(0); f[1] = inf_f(1);

    apr_pcm_from_float(f, APR_PCM_S16, s16, 2);
    ASSERT_EQ_INT(32767,  s16[0]);
    ASSERT_EQ_INT(-32768, s16[1]);

    apr_pcm_from_float(f, APR_PCM_S32, s32, 2);
    ASSERT_EQ_INT(2147483647,      s32[0]);
    ASSERT_EQ_INT(-2147483647 - 1, s32[1]);

    apr_pcm_from_float(f, APR_PCM_U8, u8, 2);
    ASSERT_EQ_INT(255, u8[0]);
    ASSERT_EQ_INT(0,   u8[1]);
}

TEST(denormals_round_to_silence_rather_than_misbehaving)
{
    float f[3];
    int16_t s16[3];
    int32_t s32[3];

    f[0] = 1e-40f;      /* denormal */
    f[1] = -1e-40f;
    f[2] = 1e-9f;       /* finite but far below one LSB */

    apr_pcm_from_float(f, APR_PCM_S16, s16, 3);
    ASSERT_EQ_INT(0, s16[0]);
    ASSERT_EQ_INT(0, s16[1]);
    ASSERT_EQ_INT(0, s16[2]);

    apr_pcm_from_float(f, APR_PCM_S32, s32, 3);
    ASSERT_EQ_INT(0, s32[0]);
    ASSERT_EQ_INT(2, s32[2]);   /* 1e-9 * 2^31 = 2.1, and it IS representable */
}

TEST(float32_output_stays_bit_exact_including_non_finite_values)
{
    /* The archival path. The WAV action pushes a NaN end to end and requires
     * it to survive, so scrubbing here would be a silent regression. */
    float in[3], out[3];

    in[0] = nan_f(); in[1] = inf_f(1); in[2] = 0.5f;
    apr_pcm_from_float(in, APR_PCM_F32, out, 3);
    ASSERT_MEM_EQ(in, out, sizeof in);
}

/* ---- channel mapping ----------------------------------------------------- */

TEST(mono_source_lands_centred_on_a_stereo_bus)
{
    const float mono[3] = { 0.25f, -0.5f, 1.0f };
    float       st[6];

    apr_mix_map_channels(mono, 1, st, 2, 3);
    ASSERT_NEAR(0.25, st[0], 0.0);
    ASSERT_NEAR(0.25, st[1], 0.0);   /* not hard left */
    ASSERT_NEAR(-0.5, st[2], 0.0);
    ASSERT_NEAR(-0.5, st[3], 0.0);
}

TEST(stereo_folds_down_by_averaging_so_it_cannot_clip)
{
    const float st[4] = { 1.0f, 1.0f, 1.0f, -1.0f };
    float       mono[2];

    apr_mix_map_channels(st, 2, mono, 1, 2);
    ASSERT_NEAR(1.0, mono[0], 0.0);
    ASSERT_NEAR(0.0, mono[1], 0.0);
}

TEST(surplus_destination_channels_are_zeroed_not_left_stale)
{
    const float st[4] = { 0.5f, 0.25f, -0.5f, -0.25f };
    float       quad[8];
    size_t      i;

    for (i = 0; i < 8; i++) quad[i] = 99.0f;
    apr_mix_map_channels(st, 2, quad, 4, 2);
    ASSERT_NEAR(0.5,  quad[0], 0.0);
    ASSERT_NEAR(0.25, quad[1], 0.0);
    ASSERT_NEAR(0.0,  quad[2], 0.0);
    ASSERT_NEAR(0.0,  quad[3], 0.0);
    ASSERT_NEAR(0.0,  quad[7], 0.0);
}

/* ---- summing ------------------------------------------------------------- */

TEST(add_sums_two_sources_with_per_source_gain)
{
    float acc[8];
    const float a[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    const float b[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    AprMixSpan  sa = { a, 4, 2, 0, 0.5f };
    AprMixSpan  sb = { b, 4, 2, 0, 0.25f };

    apr_mix_silence(acc, 4, 2);
    apr_mix_add(acc, 4, 2, &sa);
    apr_mix_add(acc, 4, 2, &sb);
    ASSERT_NEAR(0.75, acc[0], 1e-7);
    ASSERT_NEAR(0.75, acc[7], 1e-7);
}

TEST(add_does_not_clamp_because_headroom_belongs_to_the_action)
{
    float acc[2];
    const float a[2] = { 0.9f, 0.9f };
    AprMixSpan  s = { a, 1, 2, 0, 1.0f };

    apr_mix_silence(acc, 1, 2);
    apr_mix_add(acc, 1, 2, &s);
    apr_mix_add(acc, 1, 2, &s);
    ASSERT_NEAR(1.8, acc[0], 1e-6);
}

TEST(a_source_starting_mid_block_leaves_the_head_untouched)
{
    float acc[10];
    const float a[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    AprMixSpan  s = { a, 4, 1, 3, 1.0f };
    size_t i;

    for (i = 0; i < 10; i++) acc[i] = 0.125f;   /* another source already here */
    apr_mix_add(acc, 10, 1, &s);
    ASSERT_NEAR(0.125, acc[0], 0.0);
    ASSERT_NEAR(0.125, acc[2], 0.0);
    ASSERT_NEAR(1.125, acc[3], 1e-6);
    ASSERT_NEAR(4.125, acc[6], 1e-6);
    ASSERT_NEAR(0.125, acc[7], 0.0);            /* short span: tail untouched */
    ASSERT_NEAR(0.125, acc[9], 0.0);
}

TEST(a_span_that_overhangs_the_block_is_truncated_not_wrapped)
{
    float acc[4];
    const float a[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    AprMixSpan  s = { a, 8, 1, 2, 1.0f };

    apr_mix_silence(acc, 4, 1);
    apr_mix_add(acc, 4, 1, &s);
    ASSERT_NEAR(0.0, acc[0], 0.0);   /* would be 1.0 if the tail had wrapped */
    ASSERT_NEAR(0.0, acc[1], 0.0);
    ASSERT_NEAR(1.0, acc[2], 0.0);
    ASSERT_NEAR(1.0, acc[3], 0.0);
}

TEST(a_span_entirely_past_the_block_contributes_nothing)
{
    float acc[4] = { 7, 7, 7, 7 };
    const float a[4] = { 1, 1, 1, 1 };
    AprMixSpan  s = { a, 4, 1, 4, 1.0f };

    apr_mix_add(acc, 4, 1, &s);
    ASSERT_NEAR(7.0, acc[0], 0.0);
    ASSERT_NEAR(7.0, acc[3], 0.0);
}

TEST(mono_source_added_to_a_stereo_bus_is_centred_and_gained)
{
    float acc[4];
    const float m[2] = { 1.0f, -1.0f };
    AprMixSpan  s = { m, 2, 1, 0, 0.5f };

    apr_mix_silence(acc, 2, 2);
    apr_mix_add(acc, 2, 2, &s);
    ASSERT_NEAR(0.5,  acc[0], 1e-7);
    ASSERT_NEAR(0.5,  acc[1], 1e-7);
    ASSERT_NEAR(-0.5, acc[2], 1e-7);
    ASSERT_NEAR(-0.5, acc[3], 1e-7);
}

TEST(stereo_source_added_to_a_mono_bus_averages)
{
    float acc[2];
    const float st[4] = { 1.0f, 0.0f, 0.5f, 0.5f };
    AprMixSpan  s = { st, 2, 2, 0, 1.0f };

    apr_mix_silence(acc, 2, 1);
    apr_mix_add(acc, 2, 1, &s);
    ASSERT_NEAR(0.5, acc[0], 1e-7);
    ASSERT_NEAR(0.5, acc[1], 1e-7);
}

TEST(scale_and_peak)
{
    float pcm[4] = { 0.25f, -0.75f, 0.5f, 0.0f };

    ASSERT_NEAR(0.75, apr_mix_peak(pcm, 2, 2), 0.0);
    apr_mix_scale(pcm, 2, 2, 2.0f);
    ASSERT_NEAR(1.5, apr_mix_peak(pcm, 2, 2), 0.0);
    ASSERT_NEAR(0.0, apr_mix_peak(NULL, 2, 2), 0.0);
}

TEST(degenerate_arguments_are_survived_not_faulted)
{
    float acc[2] = { 1.0f, 2.0f };
    AprMixSpan s = { NULL, 4, 2, 0, 1.0f };

    apr_mix_add(acc, 1, 2, &s);
    apr_mix_add(NULL, 1, 2, &s);
    apr_mix_add(acc, 1, 2, NULL);
    apr_mix_silence(NULL, 4, 2);
    apr_pcm_to_float(APR_PCM_S16, NULL, NULL, 4);
    apr_mix_map_channels(NULL, 1, NULL, 2, 4);
    ASSERT_NEAR(1.0, acc[0], 0.0);
}
