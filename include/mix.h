/*
 * mix.h -- PCM format conversion, channel mapping, and summing.
 *
 * Design section 7 names core/mix.c the SOLE OWNER of PCM format conversion.
 * Nothing else in the tree may convert between sample formats, replicate a
 * mono source across a stereo bus, or apply a gain while summing. If a caller
 * needs a shape this does not have, widen this file.
 *
 * WHY ONE OWNER
 *
 *   Every encoder needs float -> its own integer format, every capture needs
 *   its device format -> float, and every bus needs source channels -> bus
 *   channels. Four encoders and three capture kinds each writing their own
 *   clamp-and-scale is seven chances to pick a different full-scale constant,
 *   and the difference only shows up as a 0.003 dB level error nobody traces.
 *
 * FULL SCALE: 1.0f maps to 32768 (S16), 8388608 (S24), 2147483648 (S32).
 *
 *   The scale is the power of two, not the maximum representable integer, so
 *   integer -> float -> integer is EXACT for every input value. The single
 *   value that cannot survive is +1.0f itself, which clamps to 32767; that is
 *   one LSB, and it is the price of an otherwise bit-exact round trip. Scaling
 *   by 32767 instead makes every sample slightly wrong to spare that one.
 *
 * MIXING: sum first, clamp never.
 *
 *   apr_mix_add accumulates without limiting. Summing two sources at unity can
 *   exceed 1.0, and it is the ACTION that decides what to do about it: a WAV
 *   writer clamps on conversion, a float pipeline does not have to. Clamping
 *   inside the mixer would destroy headroom that a later gain stage could have
 *   recovered.
 *
 * THREAD SAFETY: every function here is pure, allocation-free and lock-free.
 * All of it is safe on a capture callback or a mixer tick.
 */
#ifndef APPRECORDER_MIX_H
#define APPRECORDER_MIX_H

#include <stddef.h>
#include <stdint.h>

/* Interleaved channels one buffer may carry. Generous for a recorder: the
 * hardware this targets tops out at stereo per endpoint. Fixed so buses can
 * size scratch at construction and never allocate on a tick. */
#define APR_MAX_CHANNELS 8

typedef enum AprPcmFormat {
    APR_PCM_F32 = 0,   /* what the engine runs on */
    APR_PCM_S16,       /* WAV, most encoders */
    APR_PCM_S24,       /* 3 bytes little-endian, signed */
    APR_PCM_S32,
    APR_PCM_U8         /* offset binary, as WAV defines 8-bit */
} AprPcmFormat;

/* Bytes one sample (one channel of one frame) occupies. 0 for a value outside
 * the enum, which is the only way this can fail. */
size_t apr_pcm_sample_bytes(AprPcmFormat fmt);

/* Convert `samples` interleaved samples (NOT frames -- channels are already
 * folded in) between an integer format and float32. Both directions clamp;
 * neither dithers, because dither belongs to an encoder that knows its target
 * bit depth, not to a converter shared by all of them. */
void apr_pcm_to_float(AprPcmFormat src_fmt, const void *src, float *dst, size_t samples);
void apr_pcm_from_float(const float *src, AprPcmFormat dst_fmt, void *dst, size_t samples);

/* ---------------------------------------------------------------------------
 * Channel mapping. The rules, in full:
 *
 *   equal          straight copy
 *   1 -> N         replicate the mono signal to every channel (a mono mic on a
 *                  stereo bus must be centred, not hard left)
 *   N -> 1         average, so a fold-down cannot clip a signal that did not
 *   N -> M         copy min(N, M) channels, zero any surplus destination
 * ------------------------------------------------------------------------- */
void apr_mix_map_channels(const float *src, uint16_t src_channels,
                          float *dst, uint16_t dst_channels, size_t frames);

/* ---------------------------------------------------------------------------
 * Summing
 * ------------------------------------------------------------------------- */

/* One source's contribution to one mixer block.
 *
 * `offset` and a short `frames` are not edge cases -- they are the normal
 * shape of the problem. Sources start at different QPC instants, so a source
 * joining mid-block contributes from `offset` onward; a source that has not
 * delivered everything the tick asked for contributes fewer frames than the
 * block is long. Both leave the untouched part of the accumulator exactly as
 * it was, which is silence for that source and whatever other sources have
 * already put there. */
typedef struct AprMixSpan {
    const float *pcm;        /* interleaved, `channels` wide */
    size_t       frames;     /* frames actually available; may be 0 */
    uint16_t     channels;   /* the SOURCE's channel count, not the bus's */
    size_t       offset;     /* destination frame the first frame lands on */
    float        gain;       /* linear, applied on the way in */
} AprMixSpan;

/* Zero an accumulator. */
void apr_mix_silence(float *acc, size_t frames, uint16_t channels);

/* acc += span, with channel mapping, gain and offset. Anything falling outside
 * [0, acc_frames) is dropped rather than wrapped or clamped into range: a span
 * that does not fit is a caller bug the mixer must survive, not paper over. */
void apr_mix_add(float *acc, size_t acc_frames, uint16_t acc_channels,
                 const AprMixSpan *span);

/* In-place gain, for a master trim. */
void apr_mix_scale(float *pcm, size_t frames, uint16_t channels, float gain);

/* Largest absolute sample. For metering and for tests that need to know a
 * source actually contributed something. */
float apr_mix_peak(const float *pcm, size_t frames, uint16_t channels);

#endif /* APPRECORDER_MIX_H */
