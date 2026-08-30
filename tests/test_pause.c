/*
 * test_pause.c -- a recording paused for N seconds must contain no trace of
 * those N seconds, and every bus must still be sample-aligned afterwards.
 *
 * ===========================================================================
 * WHAT IS BEING PROVED, AND WHY EACH ONE IS A NUMBER
 *
 *   THE PAUSED SPAN IS ABSENT, NOT SILENT. That is the whole difference
 *   between pause and mute, and it is the one property a "looks fine" test
 *   cannot tell apart: a file with the paused duration written as zeros plays
 *   perfectly, opens in every editor, and is wrong. So the assertion is made
 *   against a CONTROL that was never paused and ran on the same tick grid:
 *
 *     - before the cut, the two files are IDENTICAL, sample for sample;
 *     - after it, the paused file matches the control at an offset of exactly
 *       the paused duration -- i.e. it holds the audio that really happened at
 *       those instants, not the audio that would have been next;
 *     - and it does NOT match the control at the same index, which is what
 *       stops this from being two empty buffers agreeing with each other.
 *
 *   The fake's samples are a pure function of the absolute frame index and the
 *   tone table is exactly one second long, so the pause is deliberately 3.030 s
 *   rather than 3 s: a whole number of seconds would put the waveform back at
 *   the same phase and every assertion in the paragraph above would pass for
 *   the wrong reason. The tone is 997 Hz for the same class of reason -- see
 *   the note on TONE below, and the capture_fake bug that found.
 *
 *   NOTHING WAS FILLED IN. Counted directly rather than inferred: the longest
 *   run of consecutive zero samples anywhere in the output. The tone table
 *   holds exactly one true zero, so the answer is 1 -- and an implementation
 *   that reported the discarded audio to the drift corrector as loss would
 *   answer with the paused duration instead.
 *
 *   EVERY BUS RESUMES AT THE SAME ORIGIN. Two buses reading one source through
 *   two independent readers come out sample-identical to each other, and a
 *   third bus on a drifting device source is still inside one sample of its
 *   target backlog. Alignment BETWEEN buses is the property a pause can destroy
 *   silently and permanently; mapping to wall clock is not.
 *
 *   THE TAKE IS ONE FILE. A pause finalizes nothing, so a paused recording
 *   produces exactly one WAV, of the RECORDED length and not the wall-clock
 *   length -- and the graph can still record a second time afterwards, which is
 *   the one-recording action lifetime this must not break.
 *
 *   A SOURCE THAT DIES OR REJOINS WHILE PAUSED needs no pause-specific code,
 *   and that claim is tested rather than asserted: the reconnect worker runs
 *   its ordinary detach / pad / reattach across the paused span and the audio
 *   after the resume is still the tone that belongs at those absolute indices.
 *
 * ===========================================================================
 * SAFETY (AGENTS.md rule 1)
 *
 *   EVERY SOURCE IN THIS FILE IS APR_SRC_FAKE. Nothing here opens an audio
 *   endpoint, activates an IAudioClient, or renders one sample to any output
 *   device. NO REAL TIME ELAPSES except in the two runner cases at the end,
 *   which need a live loop to have notices to count; everything else steps a
 *   tick value, so a three-second pause costs microseconds.
 */
#include "test_runner.h"
#include "test_wait.h"

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "action.h"
#include "clock.h"
#include "graph.h"
#include "drift.h"
#include "reconnect.h"
#include "runner.h"
#include "source.h"

#include "capture/capture_fake.h"

#define RATE   48000u
#define BLOCK    480u          /* 10 ms, what WASAPI hands us in practice */
#define T0    1234567ull       /* an anchor that is not zero */
/* 997 Hz, and the exact number matters. capture_fake's tone table is one
 * second long and indexed by (absolute frame mod sample_rate), so the sample
 * SEQUENCE repeats every 48000 / gcd(tone_hz, 48000) frames -- and at 440 Hz
 * that is 1200 frames, i.e. 25 ms. A recovered stream landing 25 ms early
 * would then be bit-identical to one that landed correctly, and every sample
 * comparison in this file would pass while the file was silently out of sync.
 * 997 is prime to 48000, so the sequence does not repeat inside a second and
 * any offset at all shows up as a waveform that no longer matches. */
#define TONE     997u

#define ASSERT_OK(call) do { AprErr e_ = (call); ASSERT_FALSE(apr_failed(&e_)); } while (0)

/* MSVC in C mode: an assignment is not an lvalue, so &(e = f(...)) does not
 * compile. This is the same test for a call whose result is not being kept. */
static int bad(AprErr e) { return apr_failed(&e); }

/* ==========================================================================
 * A sink that keeps the mix instead of encoding it
 *
 * apr_bus_add_action() takes a vtable POINTER, not a registry id, so a test can
 * supply its own -- which is what makes "what did this bus actually write"
 * answerable sample by sample with nothing on disk and no encoder in the way.
 * An action with no `extension` writes no file, so none of the path machinery
 * in bus.c is involved (see action_writes_a_file there).
 *
 * The storage is a static pool rather than the action's own allocation because
 * every assertion here happens AFTER apr_bus_stop, which finalizes and destroys
 * every action: state freed at destroy would take the evidence with it.
 * ======================================================================== */

#define SINK_SLOTS       4u
#define SINK_MAX_FRAMES  (RATE * 13u)

typedef struct Sink {
    float  *pcm;
    size_t  frames;
    int     finalize_calls;
    int     open;
} Sink;

static float g_pool[SINK_SLOTS][SINK_MAX_FRAMES];
static Sink  g_sink[SINK_SLOTS];
static size_t g_sink_used;

static void sink_reset_all(void)
{
    memset(g_sink, 0, sizeof g_sink);
    memset(g_pool, 0, sizeof g_pool);
    g_sink_used = 0;
}

static AprErr sink_create(const AprActionConfig *cfg, void **out_state)
{
    Sink *s;

    if (!out_state) return APR_ERR(APR_E_INVALID_ARG, L"sink: no out slot");
    *out_state = NULL;
    if (!cfg || cfg->channels != 1) {
        return APR_ERR(APR_E_INVALID_ARG, L"sink: mono only");
    }
    if (g_sink_used >= SINK_SLOTS) {
        return APR_ERR(APR_E_STATE, L"sink: out of slots");
    }

    s = &g_sink[g_sink_used];
    s->pcm            = g_pool[g_sink_used];
    s->frames         = 0;
    s->finalize_calls = 0;
    s->open           = 1;
    g_sink_used++;

    *out_state = s;
    return apr_ok();
}

static AprErr sink_on_audio(void *state, const float *pcm, size_t frames,
                            uint64_t qpc)
{
    Sink *s = (Sink *)state;
    size_t room;

    (void)qpc;
    if (!s || !pcm) return APR_ERR(APR_E_INVALID_ARG, L"sink: null");
    room = SINK_MAX_FRAMES - s->frames;
    if (frames > room) frames = room;
    memcpy(s->pcm + s->frames, pcm, frames * sizeof(float));
    s->frames += frames;
    return apr_ok();
}

static AprErr sink_finalize(void *state)
{
    Sink *s = (Sink *)state;
    if (s) { s->finalize_calls++; s->open = 0; }
    return apr_ok();
}

static void sink_destroy(void *state) { (void)state; }

static const AprActionVTable k_sink = {
    "testsink",     /* id: never reaches the registry, this is passed by hand */
    0,              /* display_name_id: nothing displays it */
    NULL,           /* extension: writes no file */
    sink_create,
    sink_on_audio,
    sink_finalize,
    sink_destroy,
    NULL            /* check_config */
};

/* ==========================================================================
 * The rig -- test_sync.c's, with a pause in it
 * ======================================================================== */

typedef struct Rig {
    AprGraph  *g;
    uint64_t   qpf;
    uint64_t   now;
    uint64_t   ticks_done;
    AprSource *pump[APR_MAX_SOURCES];
    size_t     pump_count;
} Rig;

static uint64_t tick_at(const Rig *r, uint64_t n)
{
    return T0 + apr_frames_to_ticks(n * BLOCK, r->qpf, RATE);
}

static uint64_t frames_to_ticks_(uint64_t frames)
{
    return apr_frames_to_ticks(frames, apr_qpc_freq(), RATE);
}

/* Anchor every fake at T0 and start the bus timeline there. Deliberately NOT
 * apr_graph_start: arming would spawn the fake's real-time pacing thread. */
static void rig_run(Rig *r)
{
    size_t i;

    r->qpf = apr_qpc_freq();
    r->now = T0;
    for (i = 0; i < r->pump_count; i++) {
        apr_capture_fake_advance(apr_source_capture(r->pump[i]), T0);
    }
    apr_graph_run(r->g, T0);
}

/* THE FAKES ARE PUMPED WHETHER OR NOT THE RECORDING IS PAUSED, because that is
 * what a real capture does: a pause stops the mixer, never the source. The
 * 250 ms ring therefore laps repeatedly during a pause, which is precisely the
 * condition that used to turn into an overrun report. */
static void rig_advance(Rig *r, uint64_t ticks)
{
    uint64_t k;
    size_t   i;

    for (k = 0; k < ticks; k++) {
        r->ticks_done++;
        r->now = tick_at(r, r->ticks_done);
        for (i = 0; i < r->pump_count; i++) {
            apr_capture_fake_advance(apr_source_capture(r->pump[i]), r->now);
        }
        apr_graph_tick(r->g, r->now);
    }
}

static void rig_blocks(Rig *r, uint64_t blocks) { rig_advance(r, blocks); }

/* Frames the bus clock says are due right now -- computed here, independently
 * of anything bus.c did, and therefore reading the SHIFTED origin after a
 * resume without being told about it. */
static uint64_t frames_due(const Rig *r, const AprBus *b)
{
    uint64_t look = apr_frames_to_ticks((uint64_t)apr_bus_lookbehind_frames(b),
                                        r->qpf, RATE);
    int64_t  due  = apr_clock_expected_frames(apr_bus_clock(b), r->now - look);
    return due > 0 ? (uint64_t)due : 0;
}

static AprCaptureConfig fake_cfg(int32_t ppm, uint32_t tone_hz, float amplitude)
{
    AprCaptureConfig cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.kind                = APR_SRC_FAKE;
    cfg.sample_rate         = RATE;      /* the graph overrides the format */
    cfg.channels            = 1;
    cfg.fake.rate_error_ppm = ppm;
    cfg.fake.tone_hz        = tone_hz;
    cfg.fake.amplitude      = amplitude;
    return cfg;
}

static AprSource *add_source_cfg(Rig *r, const wchar_t *name,
                                 const AprCaptureConfig *cfg, int reference)
{
    AprSourceId id = 0;
    AprSource  *s;

    apr_graph_add_source(r->g, name, cfg, &id);
    s = apr_graph_source(r->g, id);
    if (!s) return NULL;
    apr_source_set_reference(s, reference);
    r->pump[r->pump_count++] = s;
    return s;
}

static AprSource *add_source(Rig *r, const wchar_t *name, int32_t ppm,
                             uint32_t tone_hz, float amplitude, int reference)
{
    AprCaptureConfig cfg = fake_cfg(ppm, tone_hz, amplitude);
    return add_source_cfg(r, name, &cfg, reference);
}

static AprBusId add_bus_with_sink(Rig *r, const wchar_t *name)
{
    AprBusId        bid = 0;
    AprActionConfig acfg;
    AprBus         *b;

    apr_graph_add_bus(r->g, name, &bid);
    b = apr_graph_bus(r->g, bid);
    if (!b) return 0;
    memset(&acfg, 0, sizeof acfg);
    apr_bus_add_action(b, &k_sink, &acfg);
    return bid;
}

/* The longest run of consecutive exactly-zero samples. The direct measurement
 * of "nothing was filled in": a 440 Hz table crosses zero once, so the answer
 * for a healthy run is 1, and an implementation that treated the paused span
 * as an overrun would answer with the paused duration. */
static size_t longest_zero_run(const float *pcm, size_t n)
{
    size_t i, run = 0, best = 0;

    for (i = 0; i < n; i++) {
        if (pcm[i] == 0.0f) {
            run++;
            if (run > best) best = run;
        } else {
            run = 0;
        }
    }
    return best;
}

static size_t first_difference(const float *a, const float *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return i;
    return n;
}

/* THE SHIFT, IN FRAMES, DERIVED THE SAME WAY THE PRODUCT DERIVES IT rather
 * than assumed from the block count. Every conversion in clock.h floors, and
 * QueryPerformanceFrequency is not 10 MHz just because it usually is (design
 * 5.2), so a test that hard-coded blocks * BLOCK would be asserting an identity
 * that only holds on one machine. This is the number apr_bus_resume feeds into
 * the origin, so the sample comparisons below are exact wherever they run. */
static uint64_t shift_frames(uint64_t from_ticks, uint64_t to_ticks)
{
    return apr_ticks_to_frames(to_ticks - from_ticks, apr_qpc_freq(), RATE, NULL);
}

/* ==========================================================================
 * 1. The paused span is absent from the output
 * ======================================================================== */

/* Three seconds and thirty milliseconds. NOT three seconds: the fake's tone
 * table is exactly one second long, so a whole-number pause would leave the
 * waveform at the same phase and "the file changed" would be unprovable. */
#define PAUSE_BLOCKS  303u
#define PAUSE_FRAMES  (PAUSE_BLOCKS * BLOCK)

TEST(a_pause_excises_exactly_the_paused_span_and_leaves_no_silence)
{
    Rig      control, paused;
    AprGraph *gc = NULL, *gp = NULL;
    AprBusId  bc = 0, bp = 0;
    AprBus   *busc, *busp;
    const Sink *sc, *sp;
    uint64_t  cut;              /* bus frame at which the pause took effect */
    uint64_t  t_pause = 0, shift = 0;
    size_t    i, tail;

    sink_reset_all();

    /* ---- the control: ten seconds, never paused ------------------------ */
    memset(&control, 0, sizeof control);
    ASSERT_OK(apr_graph_create(RATE, 1, &gc));
    control.g = gc;
    add_source(&control, L"Teams", 0, TONE, 0.5f, 1);
    bc   = add_bus_with_sink(&control, L"Control");
    busc = apr_graph_bus(gc, bc);
    ASSERT_NOT_NULL(busc);
    ASSERT_OK(apr_graph_connect(gc, apr_source_id(control.pump[0]), bc, 1.0f));

    rig_run(&control);
    /* THE SAME NUMBER OF TICKS AS THE TAKE, not the same number of recorded
     * seconds. Both runs cover 10.00 s of wall clock; the take spends 3.03 s of
     * it paused, and the difference between the two files is the assertion. */
    rig_blocks(&control, 1000);

    /* ---- the take: 2 s, pause 3.03 s, then 4.97 s ---------------------- */
    memset(&paused, 0, sizeof paused);
    ASSERT_OK(apr_graph_create(RATE, 1, &gp));
    paused.g = gp;
    add_source(&paused, L"Teams", 0, TONE, 0.5f, 1);
    bp   = add_bus_with_sink(&paused, L"Take");
    busp = apr_graph_bus(gp, bp);
    ASSERT_NOT_NULL(busp);
    ASSERT_OK(apr_graph_connect(gp, apr_source_id(paused.pump[0]), bp, 1.0f));

    rig_run(&paused);
    rig_blocks(&paused, 200);            /* 2.00 s of recording */

    ASSERT_OK(apr_graph_pause(gp, paused.now));
    ASSERT_TRUE(apr_graph_paused(gp));
    ASSERT_TRUE(apr_bus_paused(busp));
    cut     = apr_bus_frames_out(busp);
    t_pause = paused.now;
    ASSERT_GT_INT(0, (int)cut);

    /* The pause. The source keeps producing the whole time -- its 250 ms ring
     * laps twelve times over -- and the bus produces nothing. */
    rig_blocks(&paused, PAUSE_BLOCKS);
    ASSERT_EQ_U64(cut, apr_bus_frames_out(busp));

    ASSERT_OK(apr_graph_resume(gp, paused.now));
    ASSERT_FALSE(apr_graph_paused(gp));
    ASSERT_FALSE(apr_bus_paused(busp));
    shift = shift_frames(t_pause, paused.now);

    rig_blocks(&paused, 497);            /* 4.97 s more of recording */

    /* Both graphs must be stopped before the sinks are read: stop is what
     * finalizes, and a pause must not have done that already. */
    sc = &g_sink[0];
    sp = &g_sink[1];
    ASSERT_EQ_INT(0, sc->finalize_calls);
    ASSERT_EQ_INT(0, sp->finalize_calls);
    ASSERT_OK(apr_graph_stop(gc));
    ASSERT_OK(apr_graph_stop(gp));
    ASSERT_EQ_INT(1, sc->finalize_calls);
    ASSERT_EQ_INT(1, sp->finalize_calls);

    printf("      control %llu frames, paused take %llu frames, cut at %llu, "
           "excised %llu (asked for %u)\n",
           (unsigned long long)sc->frames, (unsigned long long)sp->frames,
           (unsigned long long)cut,
           (unsigned long long)(sc->frames - sp->frames),
           (unsigned)PAUSE_FRAMES);

    /* THE LENGTH IS RECORDED TIME. Ten seconds of wall clock, seven of
     * recording. The two files differ by the excised span and by nothing else.
     * The bound is one frame because the two lengths are two independent floors
     * of the same division (clock.h) -- and one frame is the tolerance this
     * whole design is built to hold anyway. */
    ASSERT_EQ_U64(PAUSE_FRAMES, shift);
    ASSERT_NEAR((double)shift, (double)(sc->frames - sp->frames), 1.0);
    ASSERT_EQ_U64(frames_due(&paused, busp), apr_bus_frames_out(busp));
    ASSERT_EQ_U64(sp->frames, apr_bus_frames_out(busp));

    /* BEFORE THE CUT the two files are the same file. */
    ASSERT_EQ_U64((uint64_t)cut,
                  (uint64_t)first_difference(sc->pcm, sp->pcm, (size_t)cut));

    /* AFTER IT the take holds the audio that really happened at those
     * instants: control index (cut + shift + j), not (cut + j). */
    tail = sp->frames - (size_t)cut;
    ASSERT_TRUE((size_t)cut + (size_t)shift + tail <= sc->frames);
    ASSERT_EQ_U64((uint64_t)tail,
                  (uint64_t)first_difference(sp->pcm + cut,
                                             sc->pcm + cut + shift,
                                             tail));

    /* AND THE ABSENCE IS REAL. Two empty buffers would agree with each other;
     * these two carry a tone, and they disagree the moment the cut lands. */
    ASSERT_TRUE(first_difference(sp->pcm + cut, sc->pcm + cut, tail) < tail);
    for (i = 0; i < 4; i++) {
        ASSERT_TRUE(fabs((double)sp->pcm[cut + BLOCK + i]) > 0.0);
    }

    /* NOTHING WAS FILLED IN. A run of PAUSE_FRAMES zeros is what an
     * implementation that reported the discarded audio as loss would leave. */
    printf("      longest zero run: %u sample(s)\n",
           (unsigned)longest_zero_run(sp->pcm, sp->frames));
    ASSERT_LT_INT(4, (int)longest_zero_run(sp->pcm, sp->frames));

    apr_graph_destroy(gc);
    apr_graph_destroy(gp);
}

TEST(a_take_paused_twice_loses_exactly_both_spans_and_nothing_else)
{
    /* The shift accumulates, and each pause is measured from its own origin.
     * A second pause that re-derived the shift from the ORIGINAL anchor would
     * pass every single-pause assertion above and be wrong here. */
    Rig      control, paused;
    AprGraph *gc = NULL, *gp = NULL;
    AprBusId  bc = 0, bp = 0;
    AprBus   *busp;
    const Sink *sc, *sp;
    uint64_t  cut1, cut2, at1 = 0, at2 = 0, shift1 = 0, shift2 = 0;
    size_t    tail;

    sink_reset_all();

    memset(&control, 0, sizeof control);
    ASSERT_OK(apr_graph_create(RATE, 1, &gc));
    control.g = gc;
    add_source(&control, L"Teams", 0, TONE, 0.5f, 1);
    bc = add_bus_with_sink(&control, L"Control");
    ASSERT_NOT_NULL(apr_graph_bus(gc, bc));
    ASSERT_OK(apr_graph_connect(gc, apr_source_id(control.pump[0]), bc, 1.0f));
    rig_run(&control);
    rig_blocks(&control, 1000);

    memset(&paused, 0, sizeof paused);
    ASSERT_OK(apr_graph_create(RATE, 1, &gp));
    paused.g = gp;
    add_source(&paused, L"Teams", 0, TONE, 0.5f, 1);
    bp   = add_bus_with_sink(&paused, L"Take");
    busp = apr_graph_bus(gp, bp);
    ASSERT_NOT_NULL(busp);
    ASSERT_OK(apr_graph_connect(gp, apr_source_id(paused.pump[0]), bp, 1.0f));

    rig_run(&paused);
    rig_blocks(&paused, 150);
    ASSERT_OK(apr_graph_pause(gp, paused.now));
    cut1 = apr_bus_frames_out(busp);
    at1  = paused.now;
    rig_blocks(&paused, 111);                       /* 1.11 s */
    ASSERT_OK(apr_graph_resume(gp, paused.now));
    shift1 = shift_frames(at1, paused.now);

    rig_blocks(&paused, 150);
    ASSERT_OK(apr_graph_pause(gp, paused.now));
    cut2 = apr_bus_frames_out(busp);
    at2  = paused.now;
    rig_blocks(&paused, 192);                       /* 1.92 s */
    ASSERT_OK(apr_graph_resume(gp, paused.now));
    shift2 = shift_frames(at2, paused.now);

    rig_blocks(&paused, 397);
    ASSERT_OK(apr_graph_stop(gc));
    ASSERT_OK(apr_graph_stop(gp));

    sc = &g_sink[0];
    sp = &g_sink[1];

    /* Two spans gone, 3.03 s in total, and the rest present exactly once. */
    ASSERT_EQ_U64(111u * BLOCK, shift1);
    ASSERT_EQ_U64(192u * BLOCK, shift2);
    ASSERT_NEAR((double)(shift1 + shift2),
                (double)(sc->frames - sp->frames), 1.0);
    ASSERT_EQ_U64((uint64_t)cut1,
                  (uint64_t)first_difference(sc->pcm, sp->pcm, (size_t)cut1));
    /* The middle stretch sits one shift along; the tail sits two. A second
     * pause that re-derived its shift from the ORIGINAL anchor would put the
     * tail at shift2 and pass every assertion in the previous test. */
    ASSERT_EQ_U64((uint64_t)(cut2 - cut1),
                  (uint64_t)first_difference(sp->pcm + cut1,
                                             sc->pcm + cut1 + shift1,
                                             (size_t)(cut2 - cut1)));
    tail = sp->frames - (size_t)cut2;
    ASSERT_EQ_U64((uint64_t)tail,
                  (uint64_t)first_difference(
                      sp->pcm + cut2,
                      sc->pcm + cut2 + shift1 + shift2, tail));
    ASSERT_LT_INT(4, (int)longest_zero_run(sp->pcm, sp->frames));

    apr_graph_destroy(gc);
    apr_graph_destroy(gp);
}

/* ==========================================================================
 * 2. Every bus resumes at the SAME new origin
 *
 * Wall-clock mapping is given up on purpose. Alignment between the files is
 * not, and it is the one a pause can destroy silently: two buses handed
 * different shifts stay plausible for ever and never line up again.
 * ======================================================================== */

TEST(every_bus_resumes_at_one_common_origin_and_stays_sample_aligned)
{
    Rig       rig;
    AprGraph *g = NULL;
    AprBusId  a = 0, b = 0, c = 0;
    AprBus   *ba, *bb, *bc;
    AprSource *tap, *mic;
    const Sink *sa, *sb;
    double     err;

    sink_reset_all();
    memset(&rig, 0, sizeof rig);
    ASSERT_OK(apr_graph_create(RATE, 1, &g));
    rig.g = g;

    /* A process tap (the reference timeline) and a device capture at +30 ppm,
     * which is the only source kind carrying a resampler and a controller. */
    tap = add_source(&rig, L"Teams", 0, TONE, 0.5f, 1);
    mic = add_source(&rig, L"Chat Mic", 30, 1000, 0.5f, 0);
    ASSERT_NOT_NULL(tap);
    ASSERT_NOT_NULL(mic);
    ASSERT_TRUE(apr_source_is_reference(tap));
    ASSERT_FALSE(apr_source_is_reference(mic));

    a = add_bus_with_sink(&rig, L"A");
    b = add_bus_with_sink(&rig, L"B");
    c = add_bus_with_sink(&rig, L"C");
    ba = apr_graph_bus(g, a);
    bb = apr_graph_bus(g, b);
    bc = apr_graph_bus(g, c);

    /* One source, TWO buses: two readers, two cursors, two re-bases. */
    ASSERT_OK(apr_graph_connect(g, apr_source_id(tap), a, 1.0f));
    ASSERT_OK(apr_graph_connect(g, apr_source_id(tap), b, 1.0f));
    ASSERT_OK(apr_graph_connect(g, apr_source_id(mic), c, 1.0f));

    rig_run(&rig);
    rig_blocks(&rig, 300);
    ASSERT_OK(apr_graph_pause(g, rig.now));
    rig_blocks(&rig, PAUSE_BLOCKS);
    ASSERT_OK(apr_graph_resume(g, rig.now));
    rig_blocks(&rig, 400);

    /* THE SAME LENGTH, to the frame, from one shift applied at one instant. */
    ASSERT_EQ_U64(apr_bus_frames_out(ba), apr_bus_frames_out(bb));
    ASSERT_EQ_U64(apr_bus_frames_out(ba), apr_bus_frames_out(bc));
    ASSERT_EQ_U64(frames_due(&rig, ba), apr_bus_frames_out(ba));
    ASSERT_EQ_U64(frames_due(&rig, bc), apr_bus_frames_out(bc));

    ASSERT_OK(apr_graph_stop(g));

    /* AND THE SAME AUDIO. Two independent readers of one source, re-based
     * independently, land on the same frames -- one sample of divergence and
     * these two files would never line up again. */
    sa = &g_sink[0];
    sb = &g_sink[1];
    ASSERT_EQ_U64((uint64_t)sa->frames, (uint64_t)sb->frames);
    ASSERT_EQ_U64((uint64_t)sa->frames,
                  (uint64_t)first_difference(sa->pcm, sb->pcm, sa->frames));
    ASSERT_LT_INT(4, (int)longest_zero_run(sa->pcm, sa->frames));

    /* The device edge is back at its setpoint. Its controller forgot a
     * position error that was never real; it did not forget the crystal. */
    err = apr_source_reader_error(apr_bus_reader_at(bc, 0));
    printf("      device edge after a %.2f s pause: %.4f frames of error, "
           "trim %.2f ppm\n",
           (double)PAUSE_FRAMES / (double)RATE, err,
           apr_source_reader_trim_ppm(apr_bus_reader_at(bc, 0)));
    ASSERT_TRUE(fabs(err) < 1.0);

    apr_graph_destroy(g);
}

/* ==========================================================================
 * 2b. The drift controller across a pause
 *
 * AT CONTROLLER LEVEL, deliberately, because the property is about the
 * controller's own memory and the end-to-end case above cannot show it: by the
 * time a healthy session pauses, the loop has converged and the integrator is
 * near zero, so resetting it or not changes nothing anyone can measure. What it
 * changes is the case that matters -- a pause that arrives while a position
 * error is still being worked off.
 * ======================================================================== */

TEST(the_controller_forgets_a_position_error_across_a_pause_and_keeps_the_rate)
{
    AprDriftCtl held, freed;
    AprDrift    d;
    int         i;

    /* Two identical controllers, wound up by three seconds of a backlog 300
     * frames below its setpoint -- what one looks like part way through
     * working a disturbance off. */
    apr_drift_ctl_init(&held,  RATE, 2400.0, 0.0);
    apr_drift_ctl_init(&freed, RATE, 2400.0, 0.0);

    memset(&d, 0, sizeof d);
    d.expected_frames = 10u * RATE;                  /* ten seconds elapsed  */
    d.actual_frames   = d.expected_frames + 14;      /* a +30 ppm crystal    */
    d.delta_frames    = d.expected_frames - d.actual_frames;

    for (i = 0; i < 300; i++) {
        (void)apr_drift_ctl_update(&held,  &d, 2400.0 - 300.0, BLOCK);
        (void)apr_drift_ctl_update(&freed, &d, 2400.0 - 300.0, BLOCK);
    }
    ASSERT_TRUE(fabs(apr_drift_ctl_trim_ppm(&held)) > 100.0);

    /* The pause happens here. The re-base hands both controllers a backlog
     * EXACTLY at the setpoint on the first tick after it (source.h), because
     * the reader restarts at the frame its target says it should. */
    apr_drift_ctl_reset(&freed);

    (void)apr_drift_ctl_update(&held,  &d, 2400.0, BLOCK);
    (void)apr_drift_ctl_update(&freed, &d, 2400.0, BLOCK);

    printf("      after the resume: held %.1f ppm of correction, reset %.1f\n",
           apr_drift_ctl_trim_ppm(&held), apr_drift_ctl_trim_ppm(&freed));

    /* THE ONE THAT KEPT ITS INTEGRAL is still pushing hard against an error
     * that no longer exists, and it will spend the next several seconds
     * unwinding that into the audio after the resume. */
    ASSERT_TRUE(fabs(apr_drift_ctl_trim_ppm(&held)) > 50.0);
    /* THE ONE THAT LET IT GO issues the feed-forward and nothing else. */
    ASSERT_TRUE(fabs(apr_drift_ctl_trim_ppm(&freed)) < 1.0);

    /* AND THE RATE SURVIVED. The crystal has not changed, and the reset did
     * not forget it: the feed-forward is recomputed every tick from the
     * SOURCE's own clock, which a pause never touches (drift.h). +30 ppm,
     * still there, in the ratio the reset controller just issued. */
    ASSERT_NEAR(30.0,
                ((double)apr_drift_ctl_update(&freed, &d, 2400.0, BLOCK) /
                 4294967296.0 - 1.0) * 1e6,
                2.0);
}

/* ==========================================================================
 * 3. Pause is not stop
 * ======================================================================== */

TEST(a_pause_finalizes_nothing_and_the_take_stays_one_file)
{
    Rig       rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprBus   *b;
    const Sink *s;

    sink_reset_all();
    memset(&rig, 0, sizeof rig);
    ASSERT_OK(apr_graph_create(RATE, 1, &g));
    rig.g = g;

    add_source(&rig, L"Teams", 0, TONE, 0.5f, 1);
    bid = add_bus_with_sink(&rig, L"Take");
    b   = apr_graph_bus(g, bid);
    apr_graph_connect(g, apr_source_id(rig.pump[0]), bid, 1.0f);

    rig_run(&rig);
    rig_blocks(&rig, 100);

    /* ONE action was created, and it is open. */
    ASSERT_EQ_U64(1, (uint64_t)g_sink_used);
    s = &g_sink[0];
    ASSERT_TRUE(s->open);

    apr_graph_pause(g, rig.now);
    rig_blocks(&rig, 100);
    apr_graph_resume(g, rig.now);
    rig_blocks(&rig, 100);

    /* Still one action, still open, still never finalized. A pause that
     * finalized would have produced a second file at the resume and left the
     * first half unplayable in some containers. */
    ASSERT_EQ_U64(1, (uint64_t)g_sink_used);
    ASSERT_TRUE(s->open);
    ASSERT_EQ_INT(0, s->finalize_calls);
    ASSERT_EQ_INT(1, apr_bus_running(b));

    ASSERT_OK(apr_graph_stop(g));
    ASSERT_EQ_INT(1, s->finalize_calls);

    /* AND THE GRAPH CAN RECORD AGAIN, which is the one-recording action
     * lifetime bus.h describes: a second run creates a second action. */
    apr_graph_run(g, rig.now + frames_to_ticks_(RATE));
    ASSERT_EQ_U64(2, (uint64_t)g_sink_used);
    ASSERT_TRUE(g_sink[1].open);
    ASSERT_OK(apr_graph_stop(g));
    ASSERT_EQ_INT(1, g_sink[1].finalize_calls);

    apr_graph_destroy(g);
}

TEST(a_paused_graph_is_still_recording_and_still_refuses_to_be_edited)
{
    /* The shape stays frozen: the encoders are open, the captures are live and
     * the reconnect worker is running, so a pause is a quiet part of a
     * recording rather than a gap in one. Refused OUT LOUD -- APR_E_BUSY, the
     * code a front end answers with a sentence rather than a generic
     * failure. */
    Rig       rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0, other = 0;
    AprCaptureConfig cfg = fake_cfg(0, TONE, 0.5f);
    AprSourceId sid = 0;
    AprErr    e;

    sink_reset_all();
    memset(&rig, 0, sizeof rig);
    ASSERT_OK(apr_graph_create(RATE, 1, &g));
    rig.g = g;

    add_source(&rig, L"Teams", 0, TONE, 0.5f, 1);
    bid = add_bus_with_sink(&rig, L"Take");
    apr_graph_connect(g, apr_source_id(rig.pump[0]), bid, 1.0f);

    rig_run(&rig);
    rig_blocks(&rig, 50);
    ASSERT_OK(apr_graph_pause(g, rig.now));

    ASSERT_TRUE(apr_graph_running(g));

    e = apr_graph_add_source(g, L"late", &cfg, &sid);
    ASSERT_EQ_INT(APR_E_BUSY, (int)e.kind);
    e = apr_graph_add_bus(g, L"late", &other);
    ASSERT_EQ_INT(APR_E_BUSY, (int)e.kind);
    e = apr_graph_disconnect(g, apr_source_id(rig.pump[0]), bid);
    ASSERT_EQ_INT(APR_E_BUSY, (int)e.kind);
    e = apr_bus_set_gain(apr_graph_bus(g, bid), apr_source_id(rig.pump[0]), 0.5f);
    ASSERT_EQ_INT(APR_E_BUSY, (int)e.kind);

    ASSERT_OK(apr_graph_resume(g, rig.now));
    ASSERT_OK(apr_graph_stop(g));
    apr_graph_destroy(g);
}

TEST(pause_and_resume_are_idempotent_and_refused_on_an_idle_graph)
{
    Rig       rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprBus   *b;
    AprErr    e;
    uint64_t  before;

    sink_reset_all();
    memset(&rig, 0, sizeof rig);
    ASSERT_OK(apr_graph_create(RATE, 1, &g));
    rig.g = g;
    add_source(&rig, L"Teams", 0, TONE, 0.5f, 1);
    bid = add_bus_with_sink(&rig, L"Take");
    b   = apr_graph_bus(g, bid);
    apr_graph_connect(g, apr_source_id(rig.pump[0]), bid, 1.0f);

    /* Nothing is running: there is no timeline to take a span out of. */
    e = apr_graph_pause(g, T0);
    ASSERT_EQ_INT(APR_E_STATE, (int)e.kind);
    e = apr_graph_resume(g, T0);
    ASSERT_EQ_INT(APR_E_STATE, (int)e.kind);

    rig_run(&rig);
    rig_blocks(&rig, 100);

    /* A resume with no pause changes nothing rather than shifting the origin
     * by the whole session -- which is what an unguarded subtraction from a
     * zero pause timestamp would do. */
    before = apr_bus_frames_out(b);
    ASSERT_OK(apr_graph_resume(g, rig.now));
    rig_blocks(&rig, 1);
    ASSERT_EQ_U64(frames_due(&rig, b), apr_bus_frames_out(b));
    ASSERT_TRUE(apr_bus_frames_out(b) > before);

    /* Pausing twice is one pause: a key pressed twice is one state. */
    ASSERT_OK(apr_graph_pause(g, rig.now));
    before = apr_bus_frames_out(b);
    rig_blocks(&rig, 50);
    ASSERT_OK(apr_graph_pause(g, rig.now));
    rig_blocks(&rig, 50);
    ASSERT_EQ_U64(before, apr_bus_frames_out(b));

    /* And resuming twice does not excise a second span. */
    ASSERT_OK(apr_graph_resume(g, rig.now));
    ASSERT_OK(apr_graph_resume(g, rig.now));
    rig_blocks(&rig, 100);
    ASSERT_EQ_U64(frames_due(&rig, b), apr_bus_frames_out(b));

    ASSERT_OK(apr_graph_stop(g));
    apr_graph_destroy(g);
}

TEST(paused_ticks_measure_the_pause_and_nothing_else)
{
    Rig       rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    uint64_t  half, whole;

    sink_reset_all();
    memset(&rig, 0, sizeof rig);
    ASSERT_OK(apr_graph_create(RATE, 1, &g));
    rig.g = g;
    add_source(&rig, L"Teams", 0, TONE, 0.5f, 1);
    bid = add_bus_with_sink(&rig, L"Take");
    apr_graph_connect(g, apr_source_id(rig.pump[0]), bid, 1.0f);

    rig_run(&rig);
    rig_blocks(&rig, 100);
    ASSERT_EQ_U64(0, apr_graph_paused_ticks(g, rig.now));

    apr_graph_pause(g, rig.now);
    rig_blocks(&rig, 100);
    /* Readable WHILE paused -- a status line that only learned the figure at
     * the resume would freeze the wrong number for the length of the pause. */
    half = apr_graph_paused_ticks(g, rig.now);
    ASSERT_EQ_U64(frames_to_ticks_(100u * BLOCK), half);

    rig_blocks(&rig, 100);
    apr_graph_resume(g, rig.now);
    whole = apr_graph_paused_ticks(g, rig.now);
    ASSERT_EQ_U64(frames_to_ticks_(200u * BLOCK), whole);

    /* It stops climbing the instant the recording resumes. */
    rig_blocks(&rig, 100);
    ASSERT_EQ_U64(whole, apr_graph_paused_ticks(g, rig.now));

    ASSERT_OK(apr_graph_stop(g));
    apr_graph_destroy(g);
}

/* ==========================================================================
 * 4. A source that dies, or comes back, while the recording is paused
 *
 * The claim being tested is a NEGATIVE one: none of this needed a line of
 * pause-specific code, because a source's ring is real time and a pause is a
 * property of the output.
 * ======================================================================== */

TEST(a_source_that_dies_and_returns_inside_the_pause_leaves_no_hole)
{
    Rig       control, take;
    AprGraph *gc = NULL, *gp = NULL;
    AprBusId  bc = 0, bp = 0;
    AprBus   *busp;
    AprCaptureConfig cfg = fake_cfg(0, TONE, 0.5f);
    const Sink *sc, *sp;
    uint64_t  cut, at = 0, shift = 0;
    size_t    tail;

    sink_reset_all();

    memset(&control, 0, sizeof control);
    ASSERT_OK(apr_graph_create(RATE, 1, &gc));
    control.g = gc;
    add_source(&control, L"Teams", 0, TONE, 0.5f, 1);
    bc = add_bus_with_sink(&control, L"Control");
    apr_graph_connect(gc, apr_source_id(control.pump[0]), bc, 1.0f);
    rig_run(&control);
    rig_blocks(&control, 1000);

    /* Dies at 2.5 s and returns at 4.5 s -- both inside a pause that runs from
     * 2.00 s to 5.03 s of wall clock. */
    cfg.fake.die_at_frame    = (uint64_t)(RATE * 5u) / 2u;
    cfg.fake.revive_at_frame = (uint64_t)(RATE * 9u) / 2u;

    memset(&take, 0, sizeof take);
    ASSERT_OK(apr_graph_create(RATE, 1, &gp));
    take.g = gp;
    add_source_cfg(&take, L"Teams", &cfg, 1);
    bp   = add_bus_with_sink(&take, L"Take");
    busp = apr_graph_bus(gp, bp);
    apr_graph_connect(gp, apr_source_id(take.pump[0]), bp, 1.0f);

    rig_run(&take);
    rig_blocks(&take, 200);
    ASSERT_OK(apr_graph_pause(gp, take.now));
    cut = apr_bus_frames_out(busp);
    at  = take.now;
    rig_blocks(&take, PAUSE_BLOCKS);
    ASSERT_OK(apr_graph_resume(gp, take.now));
    shift = shift_frames(at, take.now);
    rig_blocks(&take, 497);

    ASSERT_OK(apr_graph_stop(gc));
    ASSERT_OK(apr_graph_stop(gp));

    sc = &g_sink[0];
    sp = &g_sink[1];

    /* The whole death happened while nobody was recording, so the take is
     * indistinguishable from one where it never happened. */
    ASSERT_NEAR((double)shift, (double)(sc->frames - sp->frames), 1.0);
    tail = sp->frames - (size_t)cut;
    ASSERT_EQ_U64((uint64_t)tail,
                  (uint64_t)first_difference(sp->pcm + cut,
                                             sc->pcm + cut + shift,
                                             tail));
    ASSERT_LT_INT(4, (int)longest_zero_run(sp->pcm, sp->frames));

    apr_graph_destroy(gc);
    apr_graph_destroy(gp);
}

TEST(a_source_that_dies_during_a_pause_and_stays_dead_still_holds_its_place)
{
    /* Silence after the resume is the RIGHT answer -- the application really
     * is gone. What must not change is the position: the bus is still exactly
     * as long as the clock says, so every other bus in the session still lines
     * up with this one. */
    Rig       take;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprBus   *b;
    AprCaptureConfig cfg = fake_cfg(0, TONE, 0.5f);
    const Sink *s;
    uint64_t  cut;
    size_t    i, nonzero = 0;

    sink_reset_all();
    cfg.fake.die_at_frame = (uint64_t)(RATE * 5u) / 2u;   /* 2.5 s: in the pause */

    memset(&take, 0, sizeof take);
    ASSERT_OK(apr_graph_create(RATE, 1, &g));
    take.g = g;
    add_source_cfg(&take, L"Teams", &cfg, 1);
    bid = add_bus_with_sink(&take, L"Take");
    b   = apr_graph_bus(g, bid);
    apr_graph_connect(g, apr_source_id(take.pump[0]), bid, 1.0f);

    rig_run(&take);
    rig_blocks(&take, 200);
    ASSERT_OK(apr_graph_pause(g, take.now));
    cut = apr_bus_frames_out(b);
    rig_blocks(&take, PAUSE_BLOCKS);
    ASSERT_OK(apr_graph_resume(g, take.now));
    rig_blocks(&take, 300);
    ASSERT_OK(apr_graph_stop(g));

    s = &g_sink[0];
    ASSERT_EQ_U64(frames_due(&take, b), (uint64_t)s->frames);

    /* Before the pause: audio. After it: the silence a dead process really is
     * producing, at the right length. */
    for (i = 0; i < (size_t)cut; i++) if (s->pcm[i] != 0.0f) nonzero++;
    ASSERT_GT_INT((int)cut / 2, (int)nonzero);
    for (i = (size_t)cut + BLOCK; i < s->frames; i++) {
        ASSERT_TRUE(s->pcm[i] == 0.0f);
    }

    apr_graph_destroy(g);
}

TEST(a_source_reconnected_while_paused_resumes_at_its_true_absolute_frame)
{
    /* The reconnect worker's ordinary detach / pad / reattach, driven across a
     * paused span. It is told nothing about the pause and needs to be told
     * nothing: it works in the source's own real-time frame index, which a
     * pause does not touch. */
    Rig       control, take;
    AprGraph *gc = NULL, *gp = NULL;
    AprBusId  bc = 0, bp = 0;
    AprBus   *busp;
    AprReconnector     *rc = NULL;
    AprReconnectOptions ro;
    AprCaptureConfig    cfg = fake_cfg(0, TONE, 0.5f);
    AprSource *s;
    const Sink *sctl, *sp;
    uint64_t   cut, at = 0, shift = 0;
    size_t     tail, from;

    sink_reset_all();

    memset(&control, 0, sizeof control);
    ASSERT_OK(apr_graph_create(RATE, 1, &gc));
    control.g = gc;
    add_source(&control, L"Teams", 0, TONE, 0.5f, 1);
    bc = add_bus_with_sink(&control, L"Control");
    apr_graph_connect(gc, apr_source_id(control.pump[0]), bc, 1.0f);
    rig_run(&control);
    rig_blocks(&control, 1000);

    memset(&take, 0, sizeof take);
    ASSERT_OK(apr_graph_create(RATE, 1, &gp));
    take.g = gp;
    cfg.fake.die_at_frame = (uint64_t)(RATE * 5u) / 2u;   /* 2.5 s: in the pause */
    s = add_source_cfg(&take, L"Teams", &cfg, 1);
    bp   = add_bus_with_sink(&take, L"Take");
    busp = apr_graph_bus(gp, bp);
    apr_graph_connect(gp, apr_source_id(s), bp, 1.0f);

    memset(&ro, 0, sizeof ro);
    ro.synthetic = 1;                       /* drive the real path with a fake */
    ASSERT_OK(apr_reconnect_create(gp, &ro, &rc));

    rig_run(&take);
    rig_blocks(&take, 200);
    ASSERT_OK(apr_graph_pause(gp, take.now));
    cut = apr_bus_frames_out(busp);
    at  = take.now;

    /* Into the pause far enough for the source to have died. */
    rig_blocks(&take, 60);
    apr_source_poll(s, NULL);
    ASSERT_FALSE(apr_source_alive(s));

    /* Pass one detaches it; pass two, after the first backoff, finds it. */
    apr_reconnect_step(rc, take.now);
    ASSERT_EQ_INT(APR_LINK_SEARCHING, (int)apr_reconnect_link(rc, apr_source_id(s)));
    ASSERT_FALSE(apr_source_attached(s));

    rig_blocks(&take, 80);                  /* 800 ms > APR_RECONNECT_FIRST_MS */
    apr_reconnect_step(rc, take.now);
    ASSERT_EQ_INT(APR_LINK_LIVE, (int)apr_reconnect_link(rc, apr_source_id(s)));
    ASSERT_TRUE(apr_source_attached(s));
    ASSERT_EQ_INT(2, (int)apr_source_generation(s));

    /* The replacement is in DRIVEN mode too, so it is pumped by the rig like
     * any other -- rig_advance re-reads apr_source_capture every tick. */
    rig_blocks(&take, PAUSE_BLOCKS - 140);
    ASSERT_OK(apr_graph_resume(gp, take.now));
    shift = shift_frames(at, take.now);
    rig_blocks(&take, 497);

    apr_reconnect_destroy(rc);
    ASSERT_OK(apr_graph_stop(gc));
    ASSERT_OK(apr_graph_stop(gp));

    sctl = &g_sink[0];
    sp   = &g_sink[1];

    ASSERT_NEAR((double)shift, (double)(sctl->frames - sp->frames), 1.0);

    /* The whole loss and the whole recovery happened inside the excised span,
     * so the take carries the tone that belongs at those absolute indices. One
     * block of slack after the cut for the replacement's own first buffer. */
    from = (size_t)cut + BLOCK;
    tail = sp->frames - from;
    ASSERT_EQ_U64((uint64_t)tail,
                  (uint64_t)first_difference(sp->pcm + from,
                                             sctl->pcm + from + shift,
                                             tail));

    apr_graph_destroy(gc);
    apr_graph_destroy(gp);
}

/* ==========================================================================
 * 5. The runner: the two sentences, and the clock that must not lie
 *
 * The only cases in this file that run in real time, because a notice is
 * something the loop produces and there has to be a loop. Every source is
 * still APR_SRC_FAKE and the take goes to a temporary file each case deletes.
 * ======================================================================== */

typedef struct Tally {
    int count[16];
    int order[32];
    int order_n;
} Tally;

static void tally(void *user, const AprRunNotice *n)
{
    Tally *t = (Tally *)user;
    if ((int)n->ev >= 0 && (int)n->ev < 16) t->count[n->ev]++;
    if (t->order_n < 32) t->order[t->order_n++] = (int)n->ev;
}

static void tmp_path(wchar_t *buf, size_t cch, const wchar_t *tag)
{
    wchar_t dir[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, dir);
    static LONG counter;

    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(buf, cch, _TRUNCATE, L"%lsapr_pause_%ls_%lu_%ld.wav",
                 dir, tag, GetCurrentProcessId(),
                 InterlockedIncrement(&counter));
}

static uint32_t wav_data_bytes(const wchar_t *path)
{
    HANDLE h;
    unsigned char buf[512];
    DWORD got = 0;
    size_t i;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ReadFile(h, buf, sizeof buf, &got, NULL);
    CloseHandle(h);
    if (got < 44) return 0;
    if (memcmp(buf, "RIFF", 4) != 0 && memcmp(buf, "RF64", 4) != 0) return 0;
    if (memcmp(buf + 8, "WAVE", 4) != 0) return 0;
    for (i = 12; i + 8 <= got; i += 2) {
        if (memcmp(buf + i, "data", 4) == 0) {
            uint32_t sz;
            memcpy(&sz, buf + i + 4, 4);
            return sz;
        }
    }
    return 0;
}

typedef struct Fixture {
    AprGraph *g;
    AprBusId  bus;
    wchar_t   path[MAX_PATH];
} Fixture;

static int fixture_up(Fixture *f, const wchar_t *tag)
{
    AprCaptureConfig cfg;
    AprActionConfig  acfg;
    AprSourceId      sid = 0;

    memset(f, 0, sizeof *f);
    tmp_path(f->path, MAX_PATH, tag);

    if (bad(apr_graph_create(RATE, 2, &f->g))) return 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.kind = APR_SRC_FAKE;
    cfg.fake.tone_hz   = TONE;
    cfg.fake.amplitude = 0.25f;
    if (bad(apr_graph_add_source(f->g, L"synthetic", &cfg, &sid))) return 0;
    if (bad(apr_graph_add_bus(f->g, L"Mix", &f->bus))) return 0;
    if (bad(apr_graph_connect(f->g, sid, f->bus, 1.0f))) return 0;

    memset(&acfg, 0, sizeof acfg);
    acfg.out_path    = f->path;
    acfg.sample_rate = RATE;
    acfg.channels    = 2;
    if (bad(apr_graph_add_action(f->g, f->bus, "wav", &acfg))) return 0;
    return 1;
}

static void fixture_down(Fixture *f)
{
    apr_graph_destroy(f->g);
    f->g = NULL;
    DeleteFileW(f->path);
}

/* Wait for a condition the recording thread will make true. One ceiling and
 * one step, both tests/test_wait.h's. */
static int wait_until(int (*pred)(AprRunner *), AprRunner *r)
{
    int ok;

    APR_WAIT_UNTIL(ok, pred(r));
    return ok;
}

static int is_running(AprRunner *r) { return apr_runner_running(r); }
static int is_paused(AprRunner *r)  { return apr_runner_paused(r); }
static int not_paused(AprRunner *r) { return !apr_runner_paused(r); }

TEST(the_runner_announces_the_pause_and_the_resume_in_that_order)
{
    /* A state the author cannot hear is a trap, and the resume is the half
     * that is easy to forget: somebody who heard "paused" and never hears
     * anything else believes the take ended there. */
    Fixture f;
    AprRunnerConfig cfg;
    AprRunner *r = NULL;
    Tally t;
    int i, saw_pause = -1, saw_resume = -1;

    ASSERT_TRUE(fixture_up(&f, L"notice"));
    memset(&t, 0, sizeof t);
    memset(&cfg, 0, sizeof cfg);
    cfg.graph    = f.g;
    cfg.observer = tally;
    cfg.user     = &t;

    ASSERT_OK(apr_runner_create(&cfg, &r));
    ASSERT_FALSE(apr_runner_paused(r));
    ASSERT_OK(apr_runner_run_async(r));
    /* WHAT IS ASSERTED HERE IS AN EVENT COUNT AND AN ORDER, not a duration:
     * one PAUSED, one RESUMED, in that order, however long each state lasted.
     * So each span is a handful of the loop's ten-millisecond ticks rather
     * than a fifth of a second, and the transitions themselves are waited for
     * rather than slept through. */
    ASSERT_TRUE(wait_until(is_running, r));
    Sleep(50);

    apr_runner_request_pause(r);
    ASSERT_TRUE(wait_until(is_paused, r));
    Sleep(60);

    /* Asked for twice is one state and one sentence. Six ticks is long enough
     * for a second notice to have arrived if one were ever going to. */
    apr_runner_request_pause(r);
    Sleep(60);

    apr_runner_request_resume(r);
    ASSERT_TRUE(wait_until(not_paused, r));
    Sleep(50);

    apr_runner_request_stop(r);
    ASSERT_TRUE(apr_runner_wait(r, APR_TEST_WAIT_MS));

    ASSERT_EQ_INT(1, t.count[APR_RUN_EV_PAUSED]);
    ASSERT_EQ_INT(1, t.count[APR_RUN_EV_RESUMED]);
    for (i = 0; i < t.order_n; i++) {
        if (t.order[i] == APR_RUN_EV_PAUSED  && saw_pause  < 0) saw_pause = i;
        if (t.order[i] == APR_RUN_EV_RESUMED && saw_resume < 0) saw_resume = i;
    }
    ASSERT_GT_INT(-1, saw_pause);
    ASSERT_TRUE(saw_resume > saw_pause);

    /* And the file was never closed in between. */
    ASSERT_EQ_INT(1, t.count[APR_RUN_EV_STOPPED]);
    ASSERT_EQ_INT(1, t.count[APR_RUN_EV_FINISHING]);
    ASSERT_FALSE(apr_runner_paused(r));

    apr_runner_destroy(r);
    fixture_down(&f);
}

TEST(elapsed_time_is_recorded_time_and_stops_while_paused)
{
    /* The clock in the status bar and in the tray tooltip is this number. A
     * clock that counted the pause would describe a file that does not
     * exist. */
    Fixture f;
    AprRunnerConfig cfg;
    AprRunner *r = NULL;
    int64_t at_pause, after_pause, final_ms, paused_ms;
    DWORD   t0, wall;

    ASSERT_TRUE(fixture_up(&f, L"elapsed"));
    memset(&cfg, 0, sizeof cfg);
    cfg.graph = f.g;

    ASSERT_OK(apr_runner_create(&cfg, &r));
    t0 = GetTickCount();
    ASSERT_OK(apr_runner_run_async(r));
    ASSERT_TRUE(wait_until(is_running, r));
    Sleep(300);

    apr_runner_request_pause(r);
    ASSERT_TRUE(wait_until(is_paused, r));
    Sleep(50);
    at_pause = apr_runner_elapsed_ms(r);

    Sleep(600);
    after_pause = apr_runner_elapsed_ms(r);

    apr_runner_request_resume(r);
    ASSERT_TRUE(wait_until(not_paused, r));
    Sleep(300);

    apr_runner_request_stop(r);
    ASSERT_TRUE(apr_runner_wait(r, APR_TEST_WAIT_MS));
    wall = GetTickCount() - t0;

    final_ms  = apr_runner_elapsed_ms(r);
    paused_ms = apr_runner_paused_ms(r);
    printf("      %ld ms of wall clock, %lld ms recorded, %lld ms paused\n",
           (long)wall, (long long)final_ms, (long long)paused_ms);

    /* FROZEN while paused: 600 ms of wall clock passed and the clock did not
     * move by more than one tick of the loop. */
    ASSERT_TRUE(after_pause - at_pause <= 2 * APR_RUNNER_TICK_MS);

    /* At least half a second of it was a pause, and the recorded length is
     * shorter than the wall clock by about that much. */
    ASSERT_GT_INT(500, (int)paused_ms);
    ASSERT_TRUE(final_ms < (int64_t)wall - 400);
    ASSERT_GT_INT(400, (int)final_ms);

    apr_runner_destroy(r);
    fixture_down(&f);
}

TEST(a_paused_recording_still_produces_one_playable_file_of_recorded_length)
{
    /* Two runs of the SAME wall-clock length, one of them paused for a third
     * of it. The paused take must be materially SHORTER on disk -- which is
     * the file-level statement of everything above. */
    Fixture plain, held;
    AprRunnerConfig cfg;
    AprRunner *r = NULL;
    uint32_t plain_bytes, held_bytes;

    /* TWO RUNS OF THE SAME WALL CLOCK, one of them paused for half of it.
     * The numbers are halved from the six-hundred-millisecond version they
     * were written at; every ratio below is unchanged, because both sides
     * moved together. */
    ASSERT_TRUE(fixture_up(&plain, L"plain"));
    memset(&cfg, 0, sizeof cfg);
    cfg.graph       = plain.g;
    cfg.duration_ms = 300;
    ASSERT_OK(apr_runner_create(&cfg, &r));
    ASSERT_OK(apr_runner_run(r));
    apr_runner_destroy(r);
    r = NULL;
    plain_bytes = wav_data_bytes(plain.path);
    ASSERT_GT_INT(0, (int)plain_bytes);

    ASSERT_TRUE(fixture_up(&held, L"held"));
    memset(&cfg, 0, sizeof cfg);
    cfg.graph = held.g;               /* stopped by hand, not by duration */
    ASSERT_OK(apr_runner_create(&cfg, &r));
    ASSERT_OK(apr_runner_run_async(r));
    ASSERT_TRUE(wait_until(is_running, r));
    Sleep(150);
    apr_runner_request_pause(r);
    ASSERT_TRUE(wait_until(is_paused, r));
    Sleep(300);
    apr_runner_request_resume(r);
    ASSERT_TRUE(wait_until(not_paused, r));
    Sleep(150);
    apr_runner_request_stop(r);
    ASSERT_TRUE(apr_runner_wait(r, APR_TEST_WAIT_MS));
    apr_runner_destroy(r);

    held_bytes = wav_data_bytes(held.path);
    printf("      300 ms unpaused: %lu bytes; 600 ms with a 300 ms pause: "
           "%lu bytes\n", (unsigned long)plain_bytes, (unsigned long)held_bytes);

    /* Playable, non-empty, and NOT the length of the wall clock: a mute would
     * have produced roughly double. */
    ASSERT_GT_INT(0, (int)held_bytes);
    ASSERT_TRUE(held_bytes < plain_bytes * 3u / 2u);

    fixture_down(&plain);
    fixture_down(&held);
}

TEST(stopping_while_paused_still_closes_the_files)
{
    /* The one case where the loop must NOT wait out the lookbehind: a paused
     * bus renders nothing however long it is ticked for, so the drain would be
     * a wait for something that cannot happen -- and resuming to drain it would
     * splice the paused audio onto the end of the take. */
    Fixture f;
    AprRunnerConfig cfg;
    AprRunner *r = NULL;
    Tally t;

    ASSERT_TRUE(fixture_up(&f, L"stopheld"));
    memset(&t, 0, sizeof t);
    memset(&cfg, 0, sizeof cfg);
    cfg.graph    = f.g;
    cfg.observer = tally;
    cfg.user     = &t;

    ASSERT_OK(apr_runner_create(&cfg, &r));
    ASSERT_OK(apr_runner_run_async(r));
    ASSERT_TRUE(wait_until(is_running, r));
    Sleep(150);   /* enough audio that "the file is not empty" means something */
    apr_runner_request_pause(r);
    ASSERT_TRUE(wait_until(is_paused, r));

    apr_runner_request_stop(r);
    ASSERT_TRUE(apr_runner_wait(r, APR_TEST_WAIT_MS));

    ASSERT_EQ_INT(1, t.count[APR_RUN_EV_STOPPED]);
    ASSERT_FALSE(apr_runner_paused(r));
    ASSERT_GT_INT(0, (int)wav_data_bytes(f.path));

    apr_runner_destroy(r);
    fixture_down(&f);
}
