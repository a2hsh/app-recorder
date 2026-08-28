/*
 * clock.c -- QPC arithmetic and drift. See include/clock.h for the contract
 * and for why none of this is done in floating point.
 */
#include "clock.h"

#include <windows.h>
#include <intrin.h>

/* ---- exact 128-bit scaling ---------------------------------------------- */

uint64_t apr_mul_div_u64(uint64_t a, uint64_t b, uint64_t d, uint64_t *out_rem)
{
    uint64_t hi, lo, rem = 0, q;

    if (out_rem) *out_rem = 0;
    if (d == 0) return 0;

    lo = _umul128(a, b, &hi);

    /* _udiv128 raises #DE when the quotient overflows 64 bits. Saturate
     * instead: no real timestamp reaches here, and a fault would end a
     * session that a nonsense number should only spoil one buffer of. */
    if (hi >= d) return 0xFFFFFFFFFFFFFFFFull;

    q = _udiv128(hi, lo, d, &rem);
    if (out_rem) *out_rem = rem;
    return q;
}

int64_t apr_mul_div_i64(int64_t a, uint64_t b, uint64_t d)
{
    uint64_t mag;
    int      neg = a < 0;

    /* Negating INT64_MIN is UB; take the magnitude through unsigned. */
    mag = neg ? (uint64_t)(-(a + 1)) + 1u : (uint64_t)a;
    mag = apr_mul_div_u64(mag, b, d, NULL);
    return neg ? -(int64_t)mag : (int64_t)mag;
}

/* ---- the live counter ---------------------------------------------------- */

uint64_t apr_qpc_freq(void)
{
    static uint64_t cached;      /* fixed at boot; a benign race writes the
                                  * same value twice */
    if (cached == 0) {
        LARGE_INTEGER f;
        if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) {
            cached = (uint64_t)f.QuadPart;
        } else {
            cached = APR_HNS_PER_SEC;   /* cannot fail on Windows XP or later */
        }
    }
    return cached;
}

uint64_t apr_qpc_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

/* ---- unit conversions ---------------------------------------------------- */

uint64_t apr_ticks_to_hns(uint64_t ticks, uint64_t qpc_freq)
{
    return apr_mul_div_u64(ticks, APR_HNS_PER_SEC, qpc_freq, NULL);
}

uint64_t apr_hns_to_ticks(uint64_t hns, uint64_t qpc_freq)
{
    return apr_mul_div_u64(hns, qpc_freq, APR_HNS_PER_SEC, NULL);
}

uint64_t apr_ticks_to_frames(uint64_t ticks, uint64_t qpc_freq,
                             uint32_t sample_rate, uint64_t *out_rem)
{
    return apr_mul_div_u64(ticks, (uint64_t)sample_rate, qpc_freq, out_rem);
}

uint64_t apr_frames_to_ticks(uint64_t frames, uint64_t qpc_freq,
                             uint32_t sample_rate)
{
    if (sample_rate == 0) return 0;
    return apr_mul_div_u64(frames, qpc_freq, (uint64_t)sample_rate, NULL);
}

uint64_t apr_hns_to_frames(uint64_t hns, uint32_t sample_rate, uint64_t *out_rem)
{
    return apr_mul_div_u64(hns, (uint64_t)sample_rate, APR_HNS_PER_SEC, out_rem);
}

uint64_t apr_frames_to_hns(uint64_t frames, uint32_t sample_rate)
{
    if (sample_rate == 0) return 0;
    return apr_mul_div_u64(frames, APR_HNS_PER_SEC, (uint64_t)sample_rate, NULL);
}

/* ---- a source's clock ---------------------------------------------------- */

void apr_clock_init(AprClock *c, uint64_t qpc_freq, uint32_t sample_rate)
{
    if (!c) return;
    c->qpc_freq     = qpc_freq;
    c->sample_rate  = sample_rate;
    c->anchored     = 0;
    c->anchor_ticks = 0;
}

void apr_clock_anchor(AprClock *c, uint64_t anchor_ticks)
{
    if (!c) return;
    c->anchor_ticks = anchor_ticks;
    c->anchored     = 1;
}

void apr_clock_shift_anchor(AprClock *c, uint64_t delta_ticks)
{
    /* Not anchored means there is no origin yet, and inventing one here would
     * anchor the clock at a time nothing ever happened. */
    if (!c || !c->anchored) return;
    c->anchor_ticks += delta_ticks;
}

int apr_clock_anchored(const AprClock *c)
{
    return c && c->anchored;
}

int64_t apr_clock_expected_frames(const AprClock *c, uint64_t now_ticks)
{
    int64_t elapsed;

    if (!c || c->qpc_freq == 0) return 0;

    /* One subtraction, then ONE floor. Doing this per buffer and summing is
     * the mistake the whole file is shaped to prevent. */
    if (now_ticks >= c->anchor_ticks) {
        elapsed = (int64_t)(now_ticks - c->anchor_ticks);
    } else {
        elapsed = -(int64_t)(c->anchor_ticks - now_ticks);
    }
    return apr_mul_div_i64(elapsed, (uint64_t)c->sample_rate, c->qpc_freq);
}

uint64_t apr_clock_frame_ticks(const AprClock *c, uint64_t frame_index)
{
    uint64_t ticks, rem = 0;

    if (!c || c->sample_rate == 0) return 0;

    ticks = apr_mul_div_u64(frame_index, c->qpc_freq,
                            (uint64_t)c->sample_rate, &rem);
    /* Round UP: we want the first tick at which the frame has fully elapsed,
     * so that expected_frames(frame_ticks(n)) == n exactly. */
    if (rem) ticks++;
    return c->anchor_ticks + ticks;
}

/* ---- drift --------------------------------------------------------------- */

AprDrift apr_clock_drift(const AprClock *c, uint64_t now_ticks,
                         uint64_t actual_frames)
{
    AprDrift d;

    d.expected_frames = apr_clock_expected_frames(c, now_ticks);
    d.actual_frames   = (int64_t)actual_frames;
    d.delta_frames    = d.expected_frames - d.actual_frames;
    return d;
}

int64_t apr_drift_ppb(const AprDrift *d)
{
    int64_t surplus;

    if (!d || d->expected_frames <= 0) return 0;

    /* Positive when the source runs fast, i.e. delivered more than the clock
     * expected. Exact: 128-bit multiply, then divide. */
    surplus = -d->delta_frames;
    return apr_mul_div_i64(surplus, 1000000000ull, (uint64_t)d->expected_frames);
}

double apr_drift_ppm(const AprDrift *d)
{
    if (!d || d->expected_frames <= 0) return 0.0;
    return (double)apr_drift_ppb(d) / 1000.0;
}

uint64_t apr_drift_ratio_q32(const AprDrift *d)
{
    if (!d || d->expected_frames <= 0 || d->actual_frames < 0) return 1ull << 32;

    /* (actual << 32) / expected, exactly: the shift is folded into the 128-bit
     * product so nothing is lost off the top. */
    return apr_mul_div_u64((uint64_t)d->actual_frames, 1ull << 32,
                           (uint64_t)d->expected_frames, NULL);
}
