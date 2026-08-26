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
 *
 *   HEALTH THAT LIES THE WAY THE REAL THING LIES. AprCaptureStatus carries
 *   `alive` and `muted` because both are real failures that look like a
 *   perfect recording: process loopback keeps emitting silence for ever after
 *   the target exits and WASAPI never says so (design 4.1 #6), and loopback is
 *   post-session-volume so an app muted in the Windows mixer records digital
 *   zeros while the engine still reports it rendering (4.1 #5). The fake
 *   reproduces BOTH, including the inconvenient half: a dead or muted fake
 *   keeps producing frames, at exactly the configured rate, and they are
 *   silent. Modelling death as "the source stops" would be tidier and would
 *   make every test built on it agree that the desync cannot happen.
 *
 *   AND IT COMES BACK. revive_at_frame is the mirror of unmute_at_frame, and
 *   it exists because a target that exits and is started again is the ordinary
 *   case for a recording that runs for hours. Without a way to express it,
 *   every test above this file agreed that a source which dies stays dead --
 *   which is the behaviour apprecorder now exists to stop having.
 *
 *   REJOINING. A capture built to REPLACE one that died (source.h,
 *   apr_source_reattach) is told the timeline it is joining, and pads the ring
 *   with exactly the silence that was missed before its first frame. Its tone
 *   is then indexed by the ring's ABSOLUTE frame, so a recovered fake is
 *   byte-identical to one that never died -- which is what lets a test tell a
 *   correct recovery from one landing at the wrong frame.
 *
 *   Health is a pure function of the absolute frame index, like the audio, so
 *   the transitions land on the same sample however finely a caller steps the
 *   timeline. Generation is clamped to the next transition so a chunk can
 *   never straddle one.
 */
#include "apr_winver.h"

#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "capture_internal.h"
#include "capture_fake.h"
#include "clock.h"
#include "join.h"
#include "log.h"

#define FAKE_CHUNK_FRAMES 512u
#define FAKE_PACE_MS        5u

/* Bounded, like every other capture's join. Short because the pacing thread
 * never blocks on anything but its own sleep: if it has not left in 250 ms it
 * is not going to, and a test proving the abandonment path should not take
 * five seconds to do it. */
#define FAKE_JOIN_MS      250u

typedef struct FakeImpl {
    AprCapStatus st;
    RingBuf     *rb;

    uint32_t sample_rate;
    uint16_t channels;
    int32_t  ppm;
    uint32_t tone_hz;
    float    amplitude;
    int      start_muted;
    int      start_dead;

    uint64_t tick_rate;      /* apr_qpc_freq() */
    uint64_t num, den;       /* frames = elapsed * num / den, exactly */

    /* Health schedule (capture.h). Frame indices; 0 means never. */
    uint64_t mute_at, unmute_at, die_at, revive_at;
    int      muted;          /* current, mirrors st.muted */
    int      dead;           /* current, mirrors !st.alive */

    /* Rejoining a timeline that was already running -- this capture replaced
     * one that died on a source that has been recording for a while. */
    AprCapResume resume;

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

    /* Test seam (capture_fake.h): behave like a pump wedged inside WASAPI --
     * refuse to stop, refuse to be retired, keep writing into the ring. */
    volatile LONG wedged;
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

/* ---------------------------------------------------------------------------
 * Health
 *
 * Both states are evaluated from the absolute frame index and nothing else, so
 * they are as deterministic as the samples are. `_at` is the index of the
 * FIRST frame in the new state.
 * ------------------------------------------------------------------------- */

static int muted_at(const FakeImpl *f, uint64_t n)
{
    int hit_mute   = f->mute_at   != 0 && n >= f->mute_at;
    int hit_unmute = f->unmute_at != 0 && n >= f->unmute_at;

    /* Both crossed: the later event is the one in force. They cannot be equal
     * -- fake_open refuses that rather than picking a winner here. */
    if (hit_mute && hit_unmute) return f->unmute_at > f->mute_at ? 0 : 1;
    if (hit_mute)   return 1;
    if (hit_unmute) return 0;
    return f->start_muted;
}

/* The mirror image of muted_at, and deliberately written the same way rather
 * than as a special case: a target that exits and is started again is the
 * ordinary case for a recording that runs for hours, and modelling death as
 * permanent is what made every test above this file agree that a source which
 * dies stays dead. */
static int dead_at(const FakeImpl *f, uint64_t n)
{
    int hit_die    = f->die_at    != 0 && n >= f->die_at;
    int hit_revive = f->revive_at != 0 && n >= f->revive_at;

    /* Both crossed: the later event is the one in force. They cannot be equal
     * -- fake_open refuses that rather than picking a winner here. */
    if (hit_die && hit_revive) return f->revive_at > f->die_at ? 0 : 1;
    if (hit_die)    return 1;
    if (hit_revive) return 0;
    return f->start_dead;
}

static void publish_health(FakeImpl *f, uint64_t n)
{
    int muted = muted_at(f, n);
    int dead  = dead_at(f, n);

    if (muted != f->muted) {
        f->muted = muted;
        apr_capstat_set_muted(&f->st, muted);
        APR_DEBUG(L"fake source: %s at frame %llu",
                  muted ? L"muted" : L"unmuted", (unsigned long long)n);
    }
    if (dead && !f->dead) {
        /* Same shape as capture_process.c's real detector, and for the same
         * reason: the error is what tells the owner this source failed, and
         * the capture deliberately keeps running (design section 10 -- one
         * source dying must not take the session down). */
        AprErr e = APR_ERR(APR_E_STATE,
                           L"fake source exited at frame %llu; like process "
                           L"loopback it keeps delivering silence",
                           (unsigned long long)n);
        f->dead = 1;
        apr_capstat_set_error(&f->st, &e);
        apr_capstat_set_alive(&f->st, 0);
    } else if (!dead && f->dead) {
        /* THE OTHER HALF, and it is the one that is easy to leave out. The
         * error is cleared as well as the flag: a source that is producing
         * audio again while still carrying the sentence that says it exited
         * is a status line that lies, and the front ends read exactly these
         * two fields to decide what to announce. */
        f->dead = 0;
        apr_capstat_set_alive(&f->st, 1);
        apr_capstat_clear_error(&f->st);
        APR_DEBUG(L"fake source: alive again at frame %llu",
                  (unsigned long long)n);
    }
}

/* Frames that may be generated in one go from `from` without straddling a
 * health transition. 0 when no transition is ahead. */
static uint64_t frames_to_next_event(const FakeImpl *f, uint64_t from)
{
    static const size_t k = 4;
    uint64_t ev[4];
    uint64_t best = 0;
    size_t   i;

    ev[0] = f->mute_at;
    ev[1] = f->unmute_at;
    ev[2] = f->die_at;
    ev[3] = f->revive_at;

    for (i = 0; i < k; i++) {
        if (ev[i] != 0 && ev[i] > from && (best == 0 || ev[i] < best))
            best = ev[i];
    }
    return best == 0 ? 0 : best - from;
}

static void generate_to(FakeImpl *f, uint64_t target_frames)
{
    while (f->frames < target_frames) {
        uint64_t left = target_frames - f->frames;
        uint64_t to_event = frames_to_next_event(f, f->frames);
        uint32_t n;

        if (to_event != 0 && to_event < left) left = to_event;
        n = (left > FAKE_CHUNK_FRAMES) ? FAKE_CHUNK_FRAMES : (uint32_t)left;

        /* A muted app and a dead one BOTH record as pure silence. Measured,
         * not assumed -- see the header comment. */
        if (f->period && !f->muted && !f->dead) {
            /* THE TONE IS A PURE FUNCTION OF THE RING'S ABSOLUTE INDEX, not of
             * this instance's own frame counter, and the `+ resume.padded` is
             * what keeps that true across a reconnection. A capture that
             * replaced a dead one starts its own counter at zero, so indexing
             * the tone by that would restart the waveform at phase 0 in the
             * middle of the file -- and a test comparing a recovered source
             * against one that never died could then no longer tell a correct
             * recovery from one landing at the wrong frame, which is the whole
             * property being checked. */
            fill_chunk(f, f->resume.padded + f->frames, n);
            rb_write(f->rb, f->chunk, n);
        } else {
            rb_write_silence(f->rb, n);
        }
        f->frames += n;
        publish_health(f, f->frames);
    }
    /* The rejoin pad is counted too: these are frames this capture really did
     * put into the ring, and a consumer reasoning about the ring's index space
     * needs them included or the two numbers disagree by exactly the size of
     * the hole. `f->frames` stays this instance's own count, because that is
     * what the health schedule and the rate error are measured against. */
    f->st.frames_written = (LONG64)(f->frames + f->resume.padded);
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

        /* This capture's frame 0 is AT the anchor -- unlike a WASAPI packet,
         * whose first frame precedes its arrival -- so this is the exact
         * instant the rejoin arithmetic wants, and it is before a single
         * frame has been written. Nothing at all happens for a source that
         * is not rejoining anything. */
        (void)apr_capresume_fill(&f->resume, f->rb, now_ticks);
        f->st.frames_written = (LONG64)f->resume.padded;
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
        if (InterlockedCompareExchange(&f->wedged, 0, 0) != 0) {
            /* stop_ev is deliberately ignored, which is what a pump stuck
             * inside GetBuffer looks like from the outside: still running,
             * still writing into a ring somebody upstairs is about to decide
             * it may free. */
            Sleep(FAKE_PACE_MS);
            continue;
        }
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
    /* Two contradictory health events on one frame. Resolving it quietly would
     * make the source's behaviour depend on the order of two lines in whatever
     * configured it, which is the one thing this file must never do. */
    if (cfg->fake.mute_at_frame != 0 &&
        cfg->fake.mute_at_frame == cfg->fake.unmute_at_frame)
        return APR_ERR(APR_E_INVALID_ARG,
                       L"mute_at_frame and unmute_at_frame are both %llu",
                       (unsigned long long)cfg->fake.mute_at_frame);
    if (cfg->fake.die_at_frame != 0 &&
        cfg->fake.die_at_frame == cfg->fake.revive_at_frame)
        return APR_ERR(APR_E_INVALID_ARG,
                       L"die_at_frame and revive_at_frame are both %llu",
                       (unsigned long long)cfg->fake.die_at_frame);

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
    f->mute_at     = cfg->fake.mute_at_frame;
    f->unmute_at   = cfg->fake.unmute_at_frame;
    f->die_at      = cfg->fake.die_at_frame;
    f->revive_at   = cfg->fake.revive_at_frame;
    f->start_muted = cfg->fake.start_muted ? 1 : 0;
    f->start_dead  = cfg->fake.start_dead ? 1 : 0;
    f->tick_rate   = apr_qpc_freq();
    apr_capresume_init(&f->resume, cfg);

    /* Published before a single frame exists: an app that was already muted,
     * or had already exited, when the session was armed is a normal outcome
     * and the UI has to be able to say so with nothing in the ring yet. */
    publish_health(f, 0);

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

    APR_DEBUG(L"fake source: %u Hz / %u ch, %+d ppm, %u Hz tone at %.6f, "
              L"mute@%llu unmute@%llu die@%llu revive@%llu resume@%llu%s%s",
              f->sample_rate, (unsigned)f->channels, f->ppm, f->tone_hz,
              (double)f->amplitude,
              (unsigned long long)f->mute_at,
              (unsigned long long)f->unmute_at,
              (unsigned long long)f->die_at,
              (unsigned long long)f->revive_at,
              (unsigned long long)f->resume.anchor_ticks,
              f->start_muted ? L" start-muted" : L"",
              f->start_dead  ? L" start-dead"  : L"");
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

    /* Wedged (test seam): the thread is ignoring stop_ev, so an INFINITE wait
     * here would hang the caller instead of reaching the bounded join in
     * fake_close. Leave the handle in place for close to judge. */
    if (InterlockedCompareExchange(&f->wedged, 0, 0) != 0) return;

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

static AprErr fake_close(AprCapture *c)
{
    FakeImpl *f = (FakeImpl *)c->impl;
    if (!f) return apr_ok();

    fake_stop(c);

    /* The same bounded join every real capture makes, and the same answer when
     * it fails: the thread is still writing into the caller's ring, so nothing
     * -- not the ring, not this struct -- may be freed. c->impl is left set so
     * a later close can retry once the thread does leave. */
    if (f->thread) {
        if (apr_join_wait(f->thread, FAKE_JOIN_MS) == APR_JOIN_ABANDONED)
            return APR_ERR_ABANDONED(L"the fake source's pacing thread");
        CloseHandle(f->thread);
        f->thread    = NULL;
        f->thread_id = 0;
    }

    if (f->stop_ev) { CloseHandle(f->stop_ev); f->stop_ev = NULL; }
    free(f->period);
    free(f->chunk);

    c->impl = NULL;
    free(f);
    return apr_ok();
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

AprErr apr_capture_fake_wedge(AprCapture *c)
{
    FakeImpl *f = as_fake(c);
    if (!f) return APR_ERR(APR_E_INVALID_ARG, L"not an open fake source");
    if (!f->thread)
        return APR_ERR(APR_E_STATE,
                       L"this fake source has no pacing thread to wedge; "
                       L"start() it first");
    InterlockedExchange(&f->wedged, 1);
    return apr_ok();
}

AprErr apr_capture_fake_unwedge(AprCapture *c)
{
    FakeImpl *f = as_fake(c);
    if (!f) return APR_ERR(APR_E_INVALID_ARG, L"not an open fake source");
    InterlockedExchange(&f->wedged, 0);
    if (f->stop_ev) SetEvent(f->stop_ev);
    return apr_ok();
}
