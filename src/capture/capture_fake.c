/*
 * capture_fake.c -- the synthetic source (design section 4.3).
 *
 * This file is load-bearing. It is not a convenience: the entire core --
 * mixing, drift correction, actions, the graph -- is tested through it, on
 * machines with no GoXLR and no audio hardware at all. If it is wrong, every
 * test above it is quietly wrong in the same direction.
 *
 * Three properties it therefore has to have, and how:
 *
 *   DETERMINISM. Sample n is a pure function of n. The tone is generated from
 *   a precomputed single period, indexed by (n mod sample_rate), so it neither
 *   accumulates phase error nor loses precision at frame 500 million, and two
 *   runs produce byte-identical audio. Nothing here reads the clock unless the
 *   caller asked for real time.
 *
 *   AN HONEST WRONG CLOCK. rate_error_ppm makes the source deliver frames at
 *   rate * (1 + ppm/1e6) against the tick timeline, which is what a hardware
 *   crystal that is 30 ppm fast actually does. The frame count is computed from
 *   an ABSOLUTE elapsed tick count in exact 128-bit integer arithmetic (via
 *   clock.c, which owns that), never by accumulating per-buffer counts -- the
 *   error clock.h warns about would otherwise swamp the drift being simulated.
 *
 *   SPEED. A three-hour timeline is stepped, not slept through. The work is a
 *   table lookup and a memcpy per chunk, so design 5.2's "simulated multi-hour
 *   session ends with alignment error below one sample" test runs in
 *   milliseconds per hour of audio rather than in hours.
 */
#include "apr_winver.h"

#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "capture_internal.h"
#include "capture_fake.h"
#include "clock.h"
#include "log.h"

#define FAKE_CHUNK_FRAMES 512u
#define FAKE_PACE_MS        5u

typedef struct FakeImpl {
    AprCapStatus st;
    RingBuf     *rb;

    uint32_t sample_rate;
    uint16_t channels;
    int32_t  ppm;
    uint32_t tone_hz;
    float    amplitude;

    uint64_t tick_rate;      /* apr_qpc_freq() */
    uint64_t num, den;       /* frames = elapsed * num / den, exactly */

    int      anchored;
    uint64_t anchor_ticks;
    uint64_t last_ticks;
    uint64_t frames;

    float   *period;         /* one cycle of the tone, sample_rate frames long */
    float   *chunk;          /* FAKE_CHUNK_FRAMES * channels scratch */

    HANDLE   thread;
    HANDLE   stop_ev;
    DWORD    thread_id;
    volatile LONG running;
    int      ever_started;   /* driven mode is refused once this is set */
} FakeImpl;

/* ---------------------------------------------------------------------------
 * Generation
 * ------------------------------------------------------------------------- */

static void fill_chunk(FakeImpl *f, uint64_t first_frame, uint32_t frames)
{
    uint32_t i, ch;
    float *dst = f->chunk;

    if (!f->period) {
        memset(dst, 0, (size_t)frames * f->channels * sizeof(float));
        return;
    }
    for (i = 0; i < frames; i++) {
        /* The tone has period exactly sample_rate frames, so the index stays
         * small and exact however long the session runs. */
        uint64_t n = (first_frame + i) % f->sample_rate;
        float v = f->period[(size_t)n];
        for (ch = 0; ch < f->channels; ch++) *dst++ = v;
    }
}

static void generate_to(FakeImpl *f, uint64_t target_frames)
{
    while (f->frames < target_frames) {
        uint64_t left = target_frames - f->frames;
        uint32_t n = (left > FAKE_CHUNK_FRAMES) ? FAKE_CHUNK_FRAMES
                                                : (uint32_t)left;
        if (f->period) {
            fill_chunk(f, f->frames, n);
            rb_write(f->rb, f->chunk, n);
        } else {
            rb_write_silence(f->rb, n);
        }
        f->frames += n;
    }
    f->st.frames_written = (LONG64)f->frames;
}

/* Frames this source should have produced by `now_ticks`. Exact, absolute,
 * no accumulation -- see clock.h. */
static uint64_t frames_due(const FakeImpl *f, uint64_t now_ticks)
{
    if (now_ticks <= f->anchor_ticks) return 0;
    return apr_mul_div_u64(now_ticks - f->anchor_ticks, f->num, f->den, NULL);
}

static void advance_locked(FakeImpl *f, uint64_t now_ticks)
{
    if (!f->anchored) {
        f->anchored     = 1;
        f->anchor_ticks = now_ticks;
        f->last_ticks   = now_ticks;
        f->st.anchor_ticks = (LONG64)now_ticks;
        return;
    }
    if (now_ticks < f->last_ticks) return;
    f->last_ticks = now_ticks;
    generate_to(f, frames_due(f, now_ticks));
}

/* ---------------------------------------------------------------------------
 * Real-time mode
 * ------------------------------------------------------------------------- */

static DWORD WINAPI fake_thread(LPVOID param)
{
    FakeImpl *f = (FakeImpl *)param;
    for (;;) {
        advance_locked(f, apr_qpc_now());
        if (WaitForSingleObject(f->stop_ev, FAKE_PACE_MS) == WAIT_OBJECT_0) break;
    }
    advance_locked(f, apr_qpc_now());
    return 0;
}

/* ---------------------------------------------------------------------------
 * VTable
 * ------------------------------------------------------------------------- */

static AprErr fake_open(AprCapture *c, const AprCaptureConfig *cfg, RingBuf *rb)
{
    FakeImpl *f;
    AprErr e;

    e = apr_capture_check_common(cfg, rb);
    if (apr_failed(&e)) return e;
    if (cfg->kind != APR_SRC_FAKE)
        return APR_ERR(APR_E_INVALID_ARG, L"not a fake source");
    /* -1e6 ppm is a stopped clock; anything beyond +-1e6 is not a crystal
     * error, it is a different sample rate, and it would overflow the exact
     * numerator below. */
    if (cfg->fake.rate_error_ppm <= -1000000 ||
        cfg->fake.rate_error_ppm >=  1000000)
        return APR_ERR(APR_E_INVALID_ARG,
                       L"rate_error_ppm %d outside +-1000000",
                       cfg->fake.rate_error_ppm);
    if (!(cfg->fake.amplitude >= -4.0f && cfg->fake.amplitude <= 4.0f))
        return APR_ERR(APR_E_INVALID_ARG, L"amplitude is not a sane finite value");

    f = (FakeImpl *)calloc(1, sizeof(*f));
    if (!f) return APR_ERR(APR_E_NO_MEMORY, L"fake capture");
    c->impl = f;

    apr_capstat_init(&f->st);
    f->rb          = rb;
    f->sample_rate = cfg->sample_rate;
    f->channels    = cfg->channels;
    f->ppm         = cfg->fake.rate_error_ppm;
    f->tone_hz     = cfg->fake.tone_hz;
    f->amplitude   = cfg->fake.amplitude;
    f->tick_rate   = apr_qpc_freq();

    /* frames = elapsed_ticks * (rate * (1e6 + ppm)) / (tick_rate * 1e6).
     * Both factors fit in 64 bits; apr_mul_div_u64 carries the product in 128,
     * so nothing is lost even at three hours. */
    f->num = (uint64_t)f->sample_rate * (uint64_t)(1000000 + f->ppm);
    f->den = f->tick_rate * 1000000ull;

    f->chunk = (float *)malloc((size_t)FAKE_CHUNK_FRAMES * f->channels *
                               sizeof(float));
    if (!f->chunk) return APR_ERR(APR_E_NO_MEMORY, L"fake chunk buffer");

    if (f->tone_hz != 0 && f->amplitude != 0.0f) {
        uint32_t i;
        f->period = (float *)malloc((size_t)f->sample_rate * sizeof(float));
        if (!f->period) return APR_ERR(APR_E_NO_MEMORY, L"fake tone table");
        for (i = 0; i < f->sample_rate; i++) {
            double ph = 6.283185307179586476925286766559 *
                        (double)f->tone_hz * (double)i / (double)f->sample_rate;
            f->period[i] = (float)((double)f->amplitude * sin(ph));
        }
    }
    /* tone_hz 0 (or amplitude 0) means silence, and silence goes through
     * rb_write_silence rather than a table of zeroes. */

    f->stop_ev = CreateEventW(NULL, TRUE /* manual reset */, FALSE, NULL);
    if (!f->stop_ev) return APR_ERR_LAST(L"CreateEvent for the fake source");

    APR_DEBUG(L"fake source: %u Hz / %u ch, %+d ppm, %u Hz tone at %.6f",
              f->sample_rate, (unsigned)f->channels, f->ppm, f->tone_hz,
              (double)f->amplitude);
    return apr_ok();
}

static AprErr fake_start(AprCapture *c)
{
    FakeImpl *f = (FakeImpl *)c->impl;
    if (!f) return APR_ERR(APR_E_STATE, L"fake capture is not open");
    if (InterlockedCompareExchange(&f->running, 1, 0) != 0) return apr_ok();

    f->ever_started = 1;
    ResetEvent(f->stop_ev);
    f->thread = CreateThread(NULL, 0, fake_thread, f, 0, &f->thread_id);
    if (!f->thread) {
        InterlockedExchange(&f->running, 0);
        return APR_ERR_LAST(L"CreateThread for the fake source");
    }
    return apr_ok();
}

static void fake_stop(AprCapture *c)
{
    FakeImpl *f = (FakeImpl *)c->impl;
    HANDLE th;
    if (!f) return;

    InterlockedExchange(&f->running, 0);
    if (f->stop_ev) SetEvent(f->stop_ev);

    if (GetCurrentThreadId() == f->thread_id) return;

    th = InterlockedExchangePointer((PVOID volatile *)&f->thread, NULL);
    if (th) {
        WaitForSingleObject(th, INFINITE);   /* it only ever sleeps; it will exit */
        CloseHandle(th);
        f->thread_id = 0;
    }
}

static void fake_status(const AprCapture *c, AprCaptureStatus *out)
{
    const FakeImpl *f = (const FakeImpl *)c->impl;
    if (!out) return;
    if (!f) { ZeroMemory(out, sizeof(*out)); out->last_error = apr_ok(); return; }
    apr_capstat_read(&f->st, out);
}

static void fake_close(AprCapture *c)
{
    FakeImpl *f = (FakeImpl *)c->impl;
    if (!f) return;

    fake_stop(c);

    if (f->stop_ev) { CloseHandle(f->stop_ev); f->stop_ev = NULL; }
    free(f->period);
    free(f->chunk);

    c->impl = NULL;
    free(f);
}

static const AprCaptureVTable g_fake_vtable = {
    "fake",
    fake_open,
    fake_start,
    fake_stop,
    fake_status,
    fake_close
};

const AprCaptureVTable *apr_capture_fake_vtable(void)
{
    return &g_fake_vtable;
}

/* ---------------------------------------------------------------------------
 * Test driver -- see capture_fake.h
 * ------------------------------------------------------------------------- */

static FakeImpl *as_fake(const AprCapture *c)
{
    if (!c || c->vt != &g_fake_vtable) return NULL;
    return (FakeImpl *)c->impl;
}

AprErr apr_capture_fake_advance(AprCapture *c, uint64_t now_ticks)
{
    FakeImpl *f = as_fake(c);
    if (!f) return APR_ERR(APR_E_INVALID_ARG, L"not an open fake source");
    if (f->ever_started)
        return APR_ERR(APR_E_STATE,
                       L"this fake source is running in real time; driven and "
                       L"real-time modes are mutually exclusive");
    if (f->anchored && now_ticks < f->last_ticks)
        return APR_ERR(APR_E_INVALID_ARG,
                       L"time went backwards: %llu after %llu",
                       (unsigned long long)now_ticks,
                       (unsigned long long)f->last_ticks);

    advance_locked(f, now_ticks);
    return apr_ok();
}

uint64_t apr_capture_fake_tick_rate(const AprCapture *c)
{
    const FakeImpl *f = as_fake(c);
    return f ? f->tick_rate : 0;
}
