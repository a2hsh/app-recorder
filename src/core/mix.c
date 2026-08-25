/*
 * mix.c -- PCM conversion, channel mapping and summing. See include/mix.h for
 * the contract, the full-scale convention and why there is only one of these.
 */
#include "mix.h"

#include <string.h>

/* Full scale as a power of two: see the header. */
#define APR_S16_SCALE 32768.0f
#define APR_S24_SCALE 8388608.0f
#define APR_S32_SCALE 2147483648.0f
#define APR_U8_SCALE  128.0f

size_t apr_pcm_sample_bytes(AprPcmFormat fmt)
{
    switch (fmt) {
    case APR_PCM_F32: return 4;
    case APR_PCM_S16: return 2;
    case APR_PCM_S24: return 3;
    case APR_PCM_S32: return 4;
    case APR_PCM_U8:  return 1;
    default:          return 0;
    }
}

/* One float to one integer sample: scale, saturate, round half away from zero.
 *
 * THE CLAMP HAPPENS IN THE FLOAT DOMAIN, BEFORE THE CAST, AND THAT ORDERING IS
 * THE WHOLE POINT.
 *
 *   Casting a NaN or an infinity to an integer is undefined in C, and on x86
 *   cvttsd2si returns INT64_MIN for all three. Scaling first and clamping the
 *   RESULT -- the obvious way to write this, and how this function was first
 *   written -- therefore turned a NaN into INT64_MIN, which the clamp pinned
 *   to -32768: full-scale NEGATIVE audio. A value meaning "no value" came out
 *   as maximum loudness, and +Inf came out as maximum loudness of the wrong
 *   sign.
 *
 *   Every encoder in the project converts through here, so this is where it
 *   has to be right. See AGENTS.md rule 1: a full-scale click is not a
 *   cosmetic defect in this codebase.
 *
 *   NaN -> 0 (silence, the only honest answer), +Inf -> maximum,
 *   -Inf -> minimum, finite values unchanged.
 *
 * NaN fails every comparison, so it cannot reach the cast through either
 * saturation branch; it is caught first and explicitly rather than relying on
 * that. Three predictable branches, no call to isfinite, and strictly less
 * work than the int64 clamp it replaces. */
static int64_t to_int(float v, double scale, double lo, double hi)
{
    double d = (double)v * scale;

    if (!(d == d))  return 0;            /* NaN */
    if (d >= hi)    return (int64_t)hi;
    if (d <= lo)    return (int64_t)lo;
    return (int64_t)(d >= 0.0 ? d + 0.5 : d - 0.5);
}

/* ---- integer -> float ---------------------------------------------------- */

void apr_pcm_to_float(AprPcmFormat src_fmt, const void *src, float *dst, size_t samples)
{
    size_t i;

    if (!src || !dst || samples == 0) return;

    switch (src_fmt) {
    case APR_PCM_F32:
        memcpy(dst, src, samples * sizeof(float));
        return;

    case APR_PCM_S16: {
        const int16_t *p = (const int16_t *)src;
        for (i = 0; i < samples; i++) dst[i] = (float)p[i] / APR_S16_SCALE;
        return;
    }
    case APR_PCM_S24: {
        const unsigned char *p = (const unsigned char *)src;
        for (i = 0; i < samples; i++) {
            /* Sign-extend 24 bits by assembling into the TOP of an int32 and
             * shifting back down arithmetically -- no branch on bit 23. */
            int32_t v = (int32_t)((uint32_t)p[3 * i] << 8 |
                                  (uint32_t)p[3 * i + 1] << 16 |
                                  (uint32_t)p[3 * i + 2] << 24);
            dst[i] = (float)(v >> 8) / APR_S24_SCALE;
        }
        return;
    }
    case APR_PCM_S32: {
        const int32_t *p = (const int32_t *)src;
        for (i = 0; i < samples; i++) dst[i] = (float)((double)p[i] / APR_S32_SCALE);
        return;
    }
    case APR_PCM_U8: {
        const unsigned char *p = (const unsigned char *)src;
        for (i = 0; i < samples; i++) dst[i] = ((float)p[i] - APR_U8_SCALE) / APR_U8_SCALE;
        return;
    }
    default:
        memset(dst, 0, samples * sizeof(float));
        return;
    }
}

/* ---- float -> integer ---------------------------------------------------- */

void apr_pcm_from_float(const float *src, AprPcmFormat dst_fmt, void *dst, size_t samples)
{
    size_t i;

    if (!src || !dst || samples == 0) return;

    switch (dst_fmt) {
    case APR_PCM_F32:
        memcpy(dst, src, samples * sizeof(float));
        return;

    case APR_PCM_S16: {
        int16_t *p = (int16_t *)dst;
        for (i = 0; i < samples; i++) {
            p[i] = (int16_t)to_int(src[i], APR_S16_SCALE, -32768.0, 32767.0);
        }
        return;
    }
    case APR_PCM_S24: {
        unsigned char *p = (unsigned char *)dst;
        for (i = 0; i < samples; i++) {
            int64_t v = to_int(src[i], APR_S24_SCALE, -8388608.0, 8388607.0);
            uint32_t u = (uint32_t)(int32_t)v;
            p[3 * i]     = (unsigned char)(u & 0xFFu);
            p[3 * i + 1] = (unsigned char)((u >> 8) & 0xFFu);
            p[3 * i + 2] = (unsigned char)((u >> 16) & 0xFFu);
        }
        return;
    }
    case APR_PCM_S32: {
        int32_t *p = (int32_t *)dst;
        for (i = 0; i < samples; i++) {
            p[i] = (int32_t)to_int(src[i], APR_S32_SCALE,
                                   -2147483648.0, 2147483647.0);
        }
        return;
    }
    case APR_PCM_U8: {
        unsigned char *p = (unsigned char *)dst;
        for (i = 0; i < samples; i++) {
            /* Offset binary: clamp around zero, then bias, so a NaN lands on
             * 128 -- silence in this format, not full scale. */
            p[i] = (unsigned char)(to_int(src[i], APR_U8_SCALE, -128.0, 127.0) + 128);
        }
        return;
    }
    default:
        return;
    }
}

/* ---- channel mapping ----------------------------------------------------- */

void apr_mix_map_channels(const float *src, uint16_t src_channels,
                          float *dst, uint16_t dst_channels, size_t frames)
{
    size_t f, c;

    if (!src || !dst || frames == 0 || src_channels == 0 || dst_channels == 0) return;

    if (src_channels == dst_channels) {
        memcpy(dst, src, frames * src_channels * sizeof(float));
        return;
    }
    if (src_channels == 1) {
        for (f = 0; f < frames; f++) {
            float v = src[f];
            for (c = 0; c < dst_channels; c++) dst[f * dst_channels + c] = v;
        }
        return;
    }
    if (dst_channels == 1) {
        float inv = 1.0f / (float)src_channels;
        for (f = 0; f < frames; f++) {
            float sum = 0.0f;
            for (c = 0; c < src_channels; c++) sum += src[f * src_channels + c];
            dst[f] = sum * inv;
        }
        return;
    }
    {
        uint16_t n = src_channels < dst_channels ? src_channels : dst_channels;
        for (f = 0; f < frames; f++) {
            for (c = 0; c < n; c++) dst[f * dst_channels + c] = src[f * src_channels + c];
            for (; c < dst_channels; c++) dst[f * dst_channels + c] = 0.0f;
        }
    }
}

/* ---- summing ------------------------------------------------------------- */

void apr_mix_silence(float *acc, size_t frames, uint16_t channels)
{
    if (!acc || frames == 0 || channels == 0) return;
    memset(acc, 0, frames * channels * sizeof(float));
}

void apr_mix_add(float *acc, size_t acc_frames, uint16_t acc_channels,
                 const AprMixSpan *span)
{
    size_t n, f, c;
    float  g;

    if (!acc || !span || !span->pcm || acc_channels == 0) return;
    if (span->channels == 0 || span->frames == 0) return;
    if (span->offset >= acc_frames) return;          /* entirely past the block */

    /* A span longer than the room left is truncated, not wrapped. */
    n = acc_frames - span->offset;
    if (n > span->frames) n = span->frames;

    acc += span->offset * acc_channels;
    g    = span->gain;

    if (span->channels == acc_channels) {
        size_t samples = n * acc_channels;
        for (f = 0; f < samples; f++) acc[f] += span->pcm[f] * g;
        return;
    }
    if (span->channels == 1) {
        for (f = 0; f < n; f++) {
            float v = span->pcm[f] * g;
            for (c = 0; c < acc_channels; c++) acc[f * acc_channels + c] += v;
        }
        return;
    }
    if (acc_channels == 1) {
        float inv = g / (float)span->channels;
        for (f = 0; f < n; f++) {
            float sum = 0.0f;
            for (c = 0; c < span->channels; c++) sum += span->pcm[f * span->channels + c];
            acc[f] += sum * inv;
        }
        return;
    }
    {
        uint16_t m = span->channels < acc_channels ? span->channels : acc_channels;
        for (f = 0; f < n; f++) {
            for (c = 0; c < m; c++) {
                acc[f * acc_channels + c] += span->pcm[f * span->channels + c] * g;
            }
        }
    }
}

void apr_mix_scale(float *pcm, size_t frames, uint16_t channels, float gain)
{
    size_t i, samples;

    if (!pcm || frames == 0 || channels == 0) return;
    samples = frames * channels;
    for (i = 0; i < samples; i++) pcm[i] *= gain;
}

float apr_mix_peak(const float *pcm, size_t frames, uint16_t channels)
{
    size_t i, samples;
    float  peak = 0.0f;

    if (!pcm || frames == 0 || channels == 0) return 0.0f;
    samples = frames * channels;
    for (i = 0; i < samples; i++) {
        float v = pcm[i] < 0.0f ? -pcm[i] : pcm[i];
        if (v > peak) peak = v;
    }
    return peak;
}
