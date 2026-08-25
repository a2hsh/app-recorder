/*
 * resample.c -- the one resampler. See include/resample.h for the contract,
 * the ratio convention and why the group delay is zero.
 *
 * SHAPE: a prototype windowed-sinc sampled APR_RS_PHASES times per zero
 * crossing, shared by every instance, plus a per-instance input FIFO and an
 * exact Q32.32 read position.
 *
 * Downsampling (ratio > 1) narrows the kernel's cutoff to 1/ratio and widens
 * its support by the same factor, which is what stops a 96 -> 48 conversion
 * folding everything above 24 kHz back into the audible band. At the ratios
 * drift correction uses (1 +/- 100 ppm) the cutoff is 1.0 and the filter is
 * transparent.
 */
#include "resample.h"

#include <intrin.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mix.h"   /* APR_MAX_CHANNELS */

/* Zero crossings either side of the centre at unity ratio: a 32-tap filter. */
#define APR_RS_HALF   16
/* Sub-positions tabulated per zero crossing. Linear interpolation between two
 * of them is what makes an arbitrary fractional phase cheap; 512 keeps the
 * interpolation error well under the 32-tap kernel's own stopband. */
#define APR_RS_PHASES 512
#define APR_RS_TABLE  (APR_RS_HALF * APR_RS_PHASES + 2)

/* Input frames ingested before the output loop is re-entered. Bounds the
 * internal buffer; nothing about the result depends on it. */
#define APR_RS_SLACK  1024

#define APR_RS_ONE    (1ull << 32)

/* ---------------------------------------------------------------------------
 * The prototype filter. One copy for the whole process.
 * ------------------------------------------------------------------------- */

static float         g_tab[APR_RS_TABLE];
static volatile long g_tab_state;   /* 0 unbuilt, 1 building, 2 ready */

static void build_table(void)
{
    int i;

    for (i = 0; i < APR_RS_TABLE; i++) {
        double x = (double)i / (double)APR_RS_PHASES;   /* zero crossings */
        double w, s;

        if (x >= (double)APR_RS_HALF) { g_tab[i] = 0.0f; continue; }

        /* sin(pi*n) is not exactly zero in double for integer n, and the
         * residue would cost the ratio-1.0 identity. Force the zeros. */
        if (i % APR_RS_PHASES == 0) {
            s = (i == 0) ? 1.0 : 0.0;
        } else {
            s = sin(3.14159265358979323846 * x) / (3.14159265358979323846 * x);
        }
        /* Blackman, centred: 0 at |x| == HALF, so the kernel ends cleanly. */
        w = 0.42
          + 0.50 * cos(3.14159265358979323846 * x / (double)APR_RS_HALF)
          + 0.08 * cos(2.0 * 3.14159265358979323846 * x / (double)APR_RS_HALF);
        g_tab[i] = (float)(s * w);
    }
}

static void ensure_table(void)
{
    if (g_tab_state == 2) { _ReadWriteBarrier(); return; }

    if (_InterlockedCompareExchange(&g_tab_state, 1, 0) == 0) {
        build_table();
        _InterlockedExchange(&g_tab_state, 2);
        return;
    }
    while (g_tab_state != 2) _mm_pause();
    _ReadWriteBarrier();
}

/* Kernel value at |x| zero crossings, linearly interpolated between phases. */
static double kernel(double ax)
{
    double idx = ax * (double)APR_RS_PHASES;
    size_t i;
    double f;

    if (idx >= (double)(APR_RS_HALF * APR_RS_PHASES)) return 0.0;
    i = (size_t)idx;
    f = idx - (double)i;
    return (double)g_tab[i] + f * ((double)g_tab[i + 1] - (double)g_tab[i]);
}

/* ---------------------------------------------------------------------------
 * Instance
 * ------------------------------------------------------------------------- */

struct AprResampler {
    uint16_t channels;
    size_t   half_max;    /* support at max_ratio: also the priming length */
    size_t   half;        /* support at the current ratio */
    double   cutoff;      /* 1.0 upsampling, 1/ratio downsampling */
    uint64_t ratio_q32;
    uint64_t min_ratio_q32;
    uint64_t max_ratio_q32;

    float   *buf;         /* cap frames x channels, interleaved */
    size_t   cap;
    size_t   fill;        /* frames held */
    uint64_t pos_q32;     /* read position within buf */
    uint64_t in_pos_q32;  /* stream position, from the first frame pushed */
};

static void apply_ratio(AprResampler *r)
{
    double ratio = (double)r->ratio_q32 / 4294967296.0;

    if (ratio > 1.0) {
        r->cutoff = 1.0 / ratio;
        r->half   = (size_t)ceil((double)APR_RS_HALF * ratio);
        if (r->half > r->half_max) r->half = r->half_max;
    } else {
        r->cutoff = 1.0;
        r->half   = APR_RS_HALF;
    }
}

AprErr apr_resampler_create(uint16_t channels, double max_ratio, AprResampler **out)
{
    AprResampler *r;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"resampler with no out slot");
    *out = NULL;
    if (channels == 0 || channels > APR_MAX_CHANNELS) {
        return APR_ERR(APR_E_INVALID_ARG, L"resampler for %u channels", channels);
    }
    if (!(max_ratio >= 1.0)) max_ratio = 1.0;              /* also catches NaN */
    if (max_ratio > APR_RESAMPLE_MAX_RATIO) max_ratio = APR_RESAMPLE_MAX_RATIO;

    ensure_table();

    r = (AprResampler *)calloc(1, sizeof *r);
    if (!r) return APR_ERR(APR_E_NO_MEMORY, L"resampler state");

    r->channels      = channels;
    r->half_max      = (size_t)ceil((double)APR_RS_HALF * max_ratio);
    r->ratio_q32     = APR_RS_ONE;
    r->max_ratio_q32 = (uint64_t)(max_ratio * 4294967296.0);
    r->min_ratio_q32 = (uint64_t)(4294967296.0 / max_ratio);
    r->cap           = 2 * r->half_max + APR_RS_SLACK;

    r->buf = (float *)calloc(r->cap * channels, sizeof(float));
    if (!r->buf) { free(r); return APR_ERR(APR_E_NO_MEMORY, L"resampler buffer"); }

    apply_ratio(r);
    apr_resampler_reset(r);
    *out = r;
    return apr_ok();
}

void apr_resampler_destroy(AprResampler *r)
{
    if (!r) return;
    free(r->buf);
    free(r);
}

void apr_resampler_reset(AprResampler *r)
{
    if (!r) return;
    memset(r->buf, 0, r->cap * r->channels * sizeof(float));
    /* Prime with half_max frames of silence so output 0 is centred on input 0:
     * zero group delay, see the header. */
    r->fill       = r->half_max;
    r->pos_q32    = (uint64_t)r->half_max << 32;
    r->in_pos_q32 = 0;
}

void apr_resampler_set_ratio_q32(AprResampler *r, uint64_t ratio_q32)
{
    if (!r) return;
    if (ratio_q32 < r->min_ratio_q32) ratio_q32 = r->min_ratio_q32;
    if (ratio_q32 > r->max_ratio_q32) ratio_q32 = r->max_ratio_q32;
    r->ratio_q32 = ratio_q32;
    apply_ratio(r);
}

uint64_t apr_resampler_ratio_q32(const AprResampler *r) { return r ? r->ratio_q32 : APR_RS_ONE; }
uint64_t apr_resampler_in_pos_q32(const AprResampler *r) { return r ? r->in_pos_q32 : 0; }
size_t   apr_resampler_lookahead(const AprResampler *r)  { return r ? r->half : 0; }

size_t apr_resampler_input_space(const AprResampler *r)
{
    size_t pi, keep;

    if (!r) return 0;
    pi = (size_t)(r->pos_q32 >> 32);
    /* Retained: the left context behind the read position, plus everything
     * ahead of it that has not been used yet. */
    keep = r->half + (r->fill - pi);
    return r->cap > keep ? r->cap - keep : 0;
}

size_t apr_resampler_input_needed(const AprResampler *r, size_t out_frames)
{
    uint64_t end;
    size_t   last;

    if (!r || out_frames == 0) return 0;
    end  = r->pos_q32 + (uint64_t)out_frames * r->ratio_q32;
    last = (size_t)(end >> 32) + r->half;      /* highest index the kernel touches */
    return last + 1 > r->fill ? last + 1 - r->fill : 0;
}

/* Slide the buffer down so `pos` sits just past the left context. */
static void compact(AprResampler *r)
{
    size_t pi = (size_t)(r->pos_q32 >> 32);
    size_t drop;

    if (pi <= r->half) return;
    drop = pi - r->half;
    if (drop > r->fill) drop = r->fill;
    memmove(r->buf, r->buf + drop * r->channels,
            (r->fill - drop) * r->channels * sizeof(float));
    r->fill    -= drop;
    r->pos_q32 -= (uint64_t)drop << 32;
}

static void emit(AprResampler *r, float *out)
{
    size_t   pi  = (size_t)(r->pos_q32 >> 32);
    double   fr  = (double)(uint32_t)(r->pos_q32 & 0xFFFFFFFFu) / 4294967296.0;
    uint16_t ch  = r->channels;
    int64_t  i0  = (int64_t)pi - (int64_t)r->half + 1;
    int64_t  i1  = (int64_t)pi + (int64_t)r->half;
    double   acc[APR_MAX_CHANNELS];
    double   wsum = 0.0;
    int64_t  i;
    uint16_t c;

    if (i0 < 0) i0 = 0;
    if (i1 > (int64_t)r->fill - 1) i1 = (int64_t)r->fill - 1;
    for (c = 0; c < ch; c++) acc[c] = 0.0;

    for (i = i0; i <= i1; i++) {
        double x = ((double)pi + fr) - (double)i;
        double w = kernel((x < 0.0 ? -x : x) * r->cutoff);
        const float *s;

        if (w == 0.0) continue;
        wsum += w;
        s = r->buf + (size_t)i * ch;
        for (c = 0; c < ch; c++) acc[c] += w * (double)s[c];
    }

    if (wsum != 0.0) {
        double inv = 1.0 / wsum;                 /* DC gain exactly 1 */
        for (c = 0; c < ch; c++) out[c] = (float)(acc[c] * inv);
    } else {
        for (c = 0; c < ch; c++) out[c] = 0.0f;
    }
}

size_t apr_resample(AprResampler *r, const float *in, size_t in_frames, size_t *in_used,
                    float *out, size_t out_frames)
{
    size_t used = 0, done = 0;
    uint16_t ch;

    if (in_used) *in_used = 0;
    if (!r) return 0;
    if (in_frames && !in) in_frames = 0;
    ch = r->channels;

    for (;;) {
        size_t pi, room;
        int    progress = 0;

        /* 1. take input */
        if (used < in_frames) {
            if (r->fill == r->cap) compact(r);
            room = r->cap - r->fill;
            if (room > 0) {
                size_t take = in_frames - used;
                if (take > room) take = room;
                memcpy(r->buf + r->fill * ch, in + used * ch,
                       take * ch * sizeof(float));
                r->fill += take;
                used    += take;
                progress = 1;
            }
        }

        /* 2. produce while the kernel's right edge is covered */
        while (done < out_frames) {
            pi = (size_t)(r->pos_q32 >> 32);
            if (pi + r->half >= r->fill) break;      /* need more input */
            if (out) emit(r, out + done * ch);
            r->pos_q32    += r->ratio_q32;
            r->in_pos_q32 += r->ratio_q32;
            done++;
            progress = 1;
        }

        if (used >= in_frames && done >= out_frames) break;
        if (!progress) break;
    }

    if (in_used) *in_used = used;
    return done;
}
