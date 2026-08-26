/*
 * test_sync.c -- the whole core, end to end, on a synthetic timeline.
 *
 * NO REAL TIME ELAPSES AND NO AUDIO HARDWARE IS TOUCHED. capture_fake is
 * driven in its stepped mode, so a three-hour session is a loop over tick
 * values (design 4.3, and AGENTS.md rule 1 -- nothing here can make a sound).
 *
 * WHAT IS BEING ASSERTED, and why these and not "it sounds fine":
 *
 *   ALIGNMENT is (frames the source produced) - (frames the reader consumed) -
 *   (the target backlog). It is exact: an integer producer cursor against a
 *   Q32.32 consumer position. Below one sample after hours is the requirement,
 *   and it is a number, not a judgement.
 *
 *   FRAME COUNT is what QPC says is due, computed independently here from the
 *   bus clock. A mixer that counted ticks instead of reading the clock passes
 *   every audio test and fails this one.
 *
 * The three-hour DEVICE case is covered at controller level in test_drift.c,
 * where it costs milliseconds. Here it runs through the real resampler, which
 * is 32 taps per output frame per channel, so the session is shorter and the
 * assertion is the same one.
 */
#include "test_runner.h"

#include "graph.h"
#include "mix.h"
#include "capture/capture_fake.h"

#include <math.h>
#include <string.h>

#define RATE   48000u
#define BLOCK    480u          /* 10 ms, what WASAPI hands us in practice */
#define T0    1234567ull       /* an anchor that is not zero */

/* A lower rate for the multi-hour end-to-end run: the sinc is the cost, and
 * nothing in the loop depends on the rate. See that test for the reasoning. */
#define SLOW_RATE  8000u
#define SLOW_BLOCK   80u

/* ---------------------------------------------------------------------------
 * Rig
 * ------------------------------------------------------------------------- */

typedef struct Rig {
    AprGraph *g;
    uint64_t  qpf;
    uint64_t  now;
    uint64_t  ticks_done;
    AprSource *pump[APR_MAX_SOURCES];   /* sources still being advanced */
    size_t     pump_count;
} Rig;


static uint64_t tick_at(const Rig *r, uint64_t n)
{
    return T0 + apr_frames_to_ticks(n * BLOCK, r->qpf, RATE);
}

/* Anchor every fake at T0 and start the bus timeline there. Deliberately NOT
 * apr_graph_start: arming would spawn the fake's real-time pacing thread, and
 * a three-hour session must not take three hours. */
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

static void rig_seconds(Rig *r, double seconds)
{
    rig_advance(r, (uint64_t)(seconds * (double)RATE / (double)BLOCK));
}

/* Stop pumping one source: the app exited. WASAPI would keep handing us
 * silence forever, but the fake simply stops, which is the harsher case. */
static void rig_drop(Rig *r, AprSource *s)
{
    size_t i;
    for (i = 0; i < r->pump_count; i++) {
        if (r->pump[i] == s) {
            for (; i + 1 < r->pump_count; i++) r->pump[i] = r->pump[i + 1];
            r->pump_count--;
            return;
        }
    }
}

/* Frames the bus clock says are due right now -- computed here, independently
 * of anything bus.c did. */
static uint64_t frames_due(const Rig *r, const AprBus *b)
{
    uint64_t look = apr_frames_to_ticks((uint64_t)apr_bus_lookbehind_frames(b),
                                        r->qpf, RATE);
    int64_t  due  = apr_clock_expected_frames(apr_bus_clock(b), r->now - look);
    return due > 0 ? (uint64_t)due : 0;
}

/* Add a fake source and register it for pumping. `reference` marks it as a
 * process tap -- the timeline everything else is corrected onto -- rather than
 * a device capture, which is the only way to exercise that path without a real
 * application rendering audio. */
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

/* Everything a healthy fake needs. The health fields stay zero, which is what
 * makes a zero-initialised config a healthy source (capture.h). */
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

static AprSource *add_source(Rig *r, const wchar_t *name, int32_t ppm,
                             uint32_t tone_hz, float amplitude, int reference)
{
    AprCaptureConfig cfg = fake_cfg(ppm, tone_hz, amplitude);
    return add_source_cfg(r, name, &cfg, reference);
}

/* The same, but NOT registered for pumping: it exists in the graph and can be
 * wired up, and it produces nothing until a test hands it to rig.pump. That is
 * how a source whose capture starts late is built now that the graph refuses
 * shape changes mid-recording (BUGS.md C4). */
static AprSource *add_source_unpumped(Rig *r, const wchar_t *name, int32_t ppm,
                                      uint32_t tone_hz, float amplitude,
                                      int reference)
{
    AprCaptureConfig cfg = fake_cfg(ppm, tone_hz, amplitude);
    AprSourceId id = 0;
    AprSource  *s;

    apr_graph_add_source(r->g, name, &cfg, &id);
    s = apr_graph_source(r->g, id);
    if (s) apr_source_set_reference(s, reference);
    return s;
}

/* Seconds of this session, as a frame index -- the unit capture.h's health
 * schedule is expressed in. */
static uint64_t at_second(double sec)
{
    return (uint64_t)(sec * (double)RATE);
}

/* ---------------------------------------------------------------------------
 * The reference timeline: a process tap, consumed untouched
 * ------------------------------------------------------------------------- */

TEST(a_process_tap_is_consumed_with_no_resampler_at_all)
{
    Rig rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprSource *s;
    AprBus   *b;
    AprErr    e;
    AprActionConfig acfg;

    memset(&rig, 0, sizeof rig);
    e = apr_graph_create(RATE, 1, &g);
    ASSERT_FALSE(apr_failed(&e));
    rig.g = g;

    s = add_source(&rig, L"Teams", 0, 440, 0.5f, 1);
    ASSERT_NOT_NULL(s);
    ASSERT_TRUE(apr_source_is_reference(s));

    apr_graph_add_bus(g, L"Main", &bid);
    apr_graph_connect(g, apr_source_id(s), bid, 1.0f);
    memset(&acfg, 0, sizeof acfg);
    apr_graph_add_action(g, bid, "none", &acfg);
    b = apr_graph_bus(g, bid);

    rig_run(&rig);
    rig_seconds(&rig, 10.0);

    /* No correction was applied, because none was needed and none is even
     * allocated for a reference source. */
    ASSERT_NEAR(0.0, apr_source_reader_trim_ppm(apr_bus_reader_at(b, 0)), 0.0);
    ASSERT_EQ_U64(frames_due(&rig, b), apr_bus_frames_out(b));
    ASSERT_NEAR(0.5, apr_bus_peak(b), 0.01);

    apr_graph_destroy(g);
}

TEST(three_hours_of_process_tap_stays_exactly_aligned)
{
    /* The reference timeline over a full session. Every frame the tap produced
     * must have been consumed exactly once, at the right absolute position. */
    Rig rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprSource *s;
    AprBus   *b;
    double    err;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 1, &g);
    rig.g = g;

    s = add_source(&rig, L"Teams", 0, 0, 0.0f, 1);
    apr_graph_add_bus(g, L"Main", &bid);
    apr_graph_connect(g, apr_source_id(s), bid, 1.0f);
    b = apr_graph_bus(g, bid);

    rig_run(&rig);
    rig_seconds(&rig, 3.0 * 3600.0);

    err = apr_source_reader_error(apr_bus_reader_at(b, 0));
    printf("      3h process tap: %llu frames out, alignment error %.4f\n",
           (unsigned long long)apr_bus_frames_out(b), err);

    ASSERT_EQ_U64(frames_due(&rig, b), apr_bus_frames_out(b));
    ASSERT_TRUE(fabs(err) < 1.0);
    ASSERT_EQ_U64(apr_bus_frames_out(b),
                  apr_source_reader_out_frames(apr_bus_reader_at(b, 0)));

    apr_graph_destroy(g);
}

/* ---------------------------------------------------------------------------
 * The drifting timeline: a device capture, corrected onto it
 * ------------------------------------------------------------------------- */

TEST(a_device_capture_at_thirty_ppm_ends_inside_one_sample)
{
    Rig rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprSource *s;
    AprBus   *b;
    double    err, trim;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 1, &g);
    rig.g = g;

    s = add_source(&rig, L"Chat Mic", 30, 1000, 0.5f, 0);
    ASSERT_FALSE(apr_source_is_reference(s));

    apr_graph_add_bus(g, L"Main", &bid);
    apr_graph_connect(g, apr_source_id(s), bid, 1.0f);
    b = apr_graph_bus(g, bid);

    rig_run(&rig);
    rig_seconds(&rig, 300.0);

    err  = apr_source_reader_error(apr_bus_reader_at(b, 0));
    trim = apr_source_reader_trim_ppm(apr_bus_reader_at(b, 0));
    printf("      300 s device @ +30ppm: alignment error %.4f frames, "
           "trim %.2f ppm, peak %.4f\n", err, trim, apr_bus_peak(b));

    ASSERT_EQ_U64(frames_due(&rig, b), apr_bus_frames_out(b));
    ASSERT_TRUE(fabs(err) < 1.0);
    /* The tone survived the correction rather than being filtered away. */
    ASSERT_NEAR(0.5, apr_bus_peak(b), 0.02);
    /* And the correction is far below anything audible. */
    ASSERT_TRUE(fabs(trim) < 2000.0);

    apr_graph_destroy(g);
}

TEST(a_device_capture_running_slow_is_corrected_the_other_way)
{
    Rig rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprSource *s;
    AprBus   *b;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 1, &g);
    rig.g = g;

    s = add_source(&rig, L"Slow", -45, 0, 0.0f, 0);
    apr_graph_add_bus(g, L"Main", &bid);
    apr_graph_connect(g, apr_source_id(s), bid, 1.0f);
    b = apr_graph_bus(g, bid);

    rig_run(&rig);
    rig_seconds(&rig, 300.0);

    ASSERT_TRUE(fabs(apr_source_reader_error(apr_bus_reader_at(b, 0))) < 1.0);
    ASSERT_EQ_U64(frames_due(&rig, b), apr_bus_frames_out(b));

    apr_graph_destroy(g);
}

TEST(two_hours_of_device_capture_through_the_real_resampler)
{
    /* The end-to-end multi-hour case: a drifting crystal, a real windowed-sinc
     * resampler, a real ring, a real mixer, for two hours.
     *
     * It runs at 8 kHz rather than 48 kHz purely for time: the filter is 32
     * taps per output frame, so 48 kHz for two hours is minutes of CPU for a
     * result the arithmetic already guarantees. Nothing about the loop depends
     * on the rate -- the controller time constant is expressed in seconds and
     * the resampler position is exact at any rate -- and the two companions to
     * this test pin the parts that were traded away: test_drift.c runs three
     * hours of the controller at 48 kHz, and test_resample.c proves the input
     * position is exact over millions of frames. */
    Rig       rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprSource *s;
    AprBus   *b;
    double    err;
    uint64_t  ticks;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(SLOW_RATE, 1, &g);
    rig.g = g;

    s = add_source(&rig, L"Chat Mic", 30, 0, 0.0f, 0);
    apr_graph_add_bus(g, L"Main", &bid);
    apr_graph_connect(g, apr_source_id(s), bid, 1.0f);
    b = apr_graph_bus(g, bid);

    rig.qpf = apr_qpc_freq();
    rig.now = T0;
    apr_capture_fake_advance(apr_source_capture(s), T0);
    apr_graph_run(g, T0);

    for (ticks = 0; ticks < 2 * 3600ull * SLOW_RATE / SLOW_BLOCK; ticks++) {
        rig.ticks_done++;
        rig.now = T0 + apr_frames_to_ticks(rig.ticks_done * SLOW_BLOCK,
                                           rig.qpf, SLOW_RATE);
        apr_capture_fake_advance(apr_source_capture(s), rig.now);
        apr_graph_tick(g, rig.now);
    }

    err = apr_source_reader_error(apr_bus_reader_at(b, 0));
    printf("      2h device @ +30ppm, %u Hz: %llu frames out, "
           "alignment error %.4f, trim %.2f ppm\n",
           SLOW_RATE, (unsigned long long)apr_bus_frames_out(b), err,
           apr_source_reader_trim_ppm(apr_bus_reader_at(b, 0)));

    ASSERT_TRUE(fabs(err) < 1.0);
    {
        uint64_t look = apr_frames_to_ticks((uint64_t)apr_bus_lookbehind_frames(b),
                                            rig.qpf, SLOW_RATE);
        int64_t due = apr_clock_expected_frames(apr_bus_clock(b), rig.now - look);
        ASSERT_EQ_U64((uint64_t)due, apr_bus_frames_out(b));
    }

    apr_graph_destroy(g);
}

/* ---------------------------------------------------------------------------
 * A bus whose sources are not the same kind: the common case
 * ------------------------------------------------------------------------- */

TEST(a_mixed_bus_corrects_only_the_device_side)
{
    /* "Teams plus my mic". The device side is resampled onto the engine
     * timeline; the process side passes through untouched (design 5.2). */
    Rig rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprSource *tap, *mic;
    AprBus   *b;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 1, &g);
    rig.g = g;

    tap = add_source(&rig, L"Teams",    0,  440, 0.25f, 1);
    mic = add_source(&rig, L"Chat Mic", 50, 0, 0.0f, 0);

    apr_graph_add_bus(g, L"Teams plus mic", &bid);
    apr_graph_connect(g, apr_source_id(tap), bid, 1.0f);
    apr_graph_connect(g, apr_source_id(mic), bid, 1.0f);
    b = apr_graph_bus(g, bid);
    ASSERT_EQ_INT(2, (int)apr_bus_source_count(b));

    rig_run(&rig);
    rig_seconds(&rig, 200.0);

    /* Both stay aligned, but only one of them is being corrected. */
    ASSERT_TRUE(fabs(apr_source_reader_error(apr_bus_reader_at(b, 0))) < 1.0);
    ASSERT_TRUE(fabs(apr_source_reader_error(apr_bus_reader_at(b, 1))) < 1.0);
    ASSERT_NEAR(0.0, apr_source_reader_trim_ppm(apr_bus_reader_at(b, 0)), 0.0);
    ASSERT_EQ_U64(frames_due(&rig, b), apr_bus_frames_out(b));

    apr_graph_destroy(g);
}

TEST(two_sources_that_started_apart_stay_apart)
{
    /* The other half of alignment: not just each source against the clock, but
     * sources against EACH OTHER. Two taps anchored at the same instant must
     * land on the same bus frame; the bus clock is what makes that true. */
    Rig rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprSource *a, *c;
    AprBus   *b;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 1, &g);
    rig.g = g;

    a = add_source(&rig, L"A", 0, 440, 0.25f, 1);
    c = add_source(&rig, L"B", 0, 660, 0.25f, 1);
    apr_graph_add_bus(g, L"Main", &bid);
    apr_graph_connect(g, apr_source_id(a), bid, 1.0f);
    apr_graph_connect(g, apr_source_id(c), bid, 1.0f);
    b = apr_graph_bus(g, bid);

    rig_run(&rig);
    rig_seconds(&rig, 30.0);

    ASSERT_EQ_U64(apr_source_reader_first_bus_frame(apr_bus_reader_at(b, 0)),
                  apr_source_reader_first_bus_frame(apr_bus_reader_at(b, 1)));
    ASSERT_EQ_U64(apr_source_reader_out_frames(apr_bus_reader_at(b, 0)),
                  apr_source_reader_out_frames(apr_bus_reader_at(b, 1)));

    apr_graph_destroy(g);
}

/* ---------------------------------------------------------------------------
 * One source, several buses
 * ------------------------------------------------------------------------- */

TEST(one_source_feeding_two_buses_gives_both_the_same_audio)
{
    Rig rig;
    AprGraph *g = NULL;
    AprBusId  full = 0, only = 0;
    AprSource *s;
    AprBus   *bf, *bo;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 1, &g);
    rig.g = g;

    s = add_source(&rig, L"Teams", 0, 440, 0.5f, 1);
    apr_graph_add_bus(g, L"Full mix",  &full);
    apr_graph_add_bus(g, L"Teams only", &only);
    apr_graph_connect(g, apr_source_id(s), full, 1.0f);
    apr_graph_connect(g, apr_source_id(s), only, 1.0f);

    /* Bus bookkeeping, not buffer lifetime: the ring is never told. */
    ASSERT_EQ_INT(2, apr_source_refcount(s));

    bf = apr_graph_bus(g, full);
    bo = apr_graph_bus(g, only);

    rig_run(&rig);
    rig_seconds(&rig, 20.0);

    ASSERT_EQ_U64(apr_bus_frames_out(bf), apr_bus_frames_out(bo));
    ASSERT_NEAR(apr_bus_peak(bf), apr_bus_peak(bo), 0.0);
    ASSERT_EQ_U64(apr_source_reader_first_bus_frame(apr_bus_reader_at(bf, 0)),
                  apr_source_reader_first_bus_frame(apr_bus_reader_at(bo, 0)));
    ASSERT_TRUE(fabs(apr_source_reader_error(apr_bus_reader_at(bf, 0))) < 1.0);
    ASSERT_TRUE(fabs(apr_source_reader_error(apr_bus_reader_at(bo, 0))) < 1.0);

    apr_graph_destroy(g);
}

TEST(two_readers_on_one_source_return_identical_samples)
{
    /* Sample-level proof that the ring's per-consumer cursors really are
     * independent: two readers over the same bus range, memcmp. */
    AprGraph *g = NULL;
    Rig       rig;
    AprSource *s;
    AprSourceReader *a = NULL, *c = NULL;
    AprClock  clk;
    AprErr    e;
    float     buf_a[BLOCK], buf_c[BLOCK];
    AprSourcePull pa, pc;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 1, &g);
    rig.g = g;
    s = add_source(&rig, L"Teams", 0, 440, 0.5f, 1);

    rig.qpf = apr_qpc_freq();
    apr_capture_fake_advance(apr_source_capture(s), T0);
    apr_capture_fake_advance(apr_source_capture(s),
                             T0 + apr_frames_to_ticks(RATE / 10, rig.qpf, RATE));

    apr_clock_init(&clk, rig.qpf, RATE);
    apr_clock_anchor(&clk, T0);

    e = apr_source_reader_open(s, 0.0, &a);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_source_reader_open(s, 0.0, &c);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(2, apr_source_refcount(s));

    pa = apr_source_pull(a, &clk, 0, T0, buf_a, BLOCK);
    pc = apr_source_pull(c, &clk, 0, T0, buf_c, BLOCK);
    ASSERT_EQ_INT((int)pa.frames, (int)pc.frames);
    ASSERT_EQ_INT(0, (int)pa.lead);
    ASSERT_GT_INT(0, (int)pa.frames);
    ASSERT_MEM_EQ(buf_a, buf_c, pa.frames * sizeof(float));

    apr_source_reader_close(a);
    apr_source_reader_close(c);
    ASSERT_EQ_INT(0, apr_source_refcount(s));
    apr_graph_destroy(g);
}

/* ---------------------------------------------------------------------------
 * Sources that misbehave
 * ------------------------------------------------------------------------- */

TEST(a_silent_source_still_holds_its_place_on_the_timeline)
{
    /* The spike's finding, all the way through: a silent app delivers real
     * 0.0f samples at 100% fill, so there is nothing to synthesize and the
     * timeline must not gap. Peak zero, alignment perfect, frames exact. */
    Rig rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprSource *quiet;
    AprBus   *b;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 1, &g);
    rig.g = g;

    quiet = add_source(&rig, L"Silent app", 0, 0, 0.0f, 1);
    apr_graph_add_bus(g, L"Main", &bid);
    apr_graph_connect(g, apr_source_id(quiet), bid, 1.0f);
    b = apr_graph_bus(g, bid);

    rig_run(&rig);
    rig_seconds(&rig, 60.0);

    ASSERT_NEAR(0.0, apr_bus_peak(b), 0.0);
    ASSERT_EQ_U64(frames_due(&rig, b), apr_bus_frames_out(b));
    ASSERT_TRUE(fabs(apr_source_reader_error(apr_bus_reader_at(b, 0))) < 1.0);
    ASSERT_EQ_U64(apr_bus_frames_out(b),
                  apr_source_reader_out_frames(apr_bus_reader_at(b, 0)));

    apr_graph_destroy(g);
}

TEST(a_source_that_dies_does_not_stop_the_bus_or_move_the_others)
{
    /* Design 10: failure of one source must never take down a session. The
     * surviving source must be bit-for-bit unaffected -- if a dead source
     * could shorten a block, every other file in the session would shift. */
    Rig rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprSource *live, *dying;
    AprBus   *b;
    uint64_t  live_frames_before;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 1, &g);
    rig.g = g;

    live  = add_source(&rig, L"Alive", 0, 440, 0.5f, 1);
    dying = add_source(&rig, L"Teams", 0, 660, 0.5f, 1);
    apr_graph_add_bus(g, L"Main", &bid);
    apr_graph_connect(g, apr_source_id(live),  bid, 1.0f);
    apr_graph_connect(g, apr_source_id(dying), bid, 1.0f);
    b = apr_graph_bus(g, bid);

    rig_run(&rig);
    rig_seconds(&rig, 20.0);
    live_frames_before = apr_source_reader_out_frames(apr_bus_reader_at(b, 0));
    ASSERT_GT_INT(0, (long long)live_frames_before);

    rig_drop(&rig, dying);            /* the app exits; its ring stops filling */
    rig_seconds(&rig, 20.0);

    /* The bus produced every frame QPC asked for, throughout. */
    ASSERT_EQ_U64(frames_due(&rig, b), apr_bus_frames_out(b));
    /* The survivor kept perfect step and is still aligned. */
    ASSERT_EQ_U64(apr_bus_frames_out(b),
                  apr_source_reader_out_frames(apr_bus_reader_at(b, 0)));
    ASSERT_TRUE(fabs(apr_source_reader_error(apr_bus_reader_at(b, 0))) < 1.0);
    /* And the dead one kept its place rather than vanishing from the mix. */
    ASSERT_EQ_U64(apr_bus_frames_out(b),
                  apr_source_reader_out_frames(apr_bus_reader_at(b, 1)));

    apr_graph_destroy(g);
}

/* ---------------------------------------------------------------------------
 * The two failures that look like a successful recording
 *
 * The case above stops pumping the dying source, which is the HARSHER shape:
 * its ring simply stops filling. Real process loopback does something worse
 * and far quieter -- it keeps handing over perfectly formed buffers of zeros
 * for ever, at exactly the right rate, with no error and no flag (design 4.1
 * #6). A muted app is indistinguishable from that (4.1 #5).
 *
 * These two drive the real shape through the whole core, which capture_fake's
 * health knob is what makes possible. What they pin is design section 10: the
 * status reaches the graph, and the session does not so much as flinch.
 * ------------------------------------------------------------------------- */

TEST(a_source_that_dies_mid_session_keeps_its_place_and_the_bus_never_notices)
{
    Rig rig;
    AprGraph *g = NULL;
    AprBusId  mix = 0, solo = 0;
    AprSource *live, *dying;
    AprBus   *bmix, *bsolo;
    AprCaptureConfig cfg = fake_cfg(0, 660, 0.5f);
    AprCaptureStatus st;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 1, &g);
    rig.g = g;

    cfg.fake.die_at_frame = at_second(10.0);

    live  = add_source(&rig, L"Alive", 0, 440, 0.5f, 1);
    dying = add_source_cfg(&rig, L"Teams", &cfg, 1);
    ASSERT_NOT_NULL(live);
    ASSERT_NOT_NULL(dying);

    /* Two buses so silence is observable: the dying source has one to itself,
     * and also feeds the mix that has to carry on regardless. */
    apr_graph_add_bus(g, L"Mix", &mix);
    apr_graph_add_bus(g, L"Teams only", &solo);
    apr_graph_connect(g, apr_source_id(live),  mix,  1.0f);
    apr_graph_connect(g, apr_source_id(dying), mix,  1.0f);
    apr_graph_connect(g, apr_source_id(dying), solo, 1.0f);
    bmix  = apr_graph_bus(g, mix);
    bsolo = apr_graph_bus(g, solo);

    rig_run(&rig);
    rig_seconds(&rig, 5.0);

    /* Healthy, and audible. */
    apr_source_poll(dying, &st);
    ASSERT_EQ_INT(1, apr_source_alive(dying));
    ASSERT_FALSE(apr_failed(&st.last_error));
    ASSERT_NEAR(0.5, apr_bus_peak(bsolo), 0.01);

    rig_seconds(&rig, 15.0);          /* through the death, and well past it */

    /* The status propagated: capture -> AprSource -> everything that asks. */
    apr_source_poll(dying, &st);
    ASSERT_EQ_INT(0, apr_source_alive(dying));
    ASSERT_EQ_INT(0, st.alive);
    ASSERT_TRUE(apr_failed(&st.last_error));
    ASSERT_EQ_INT(1, apr_source_alive(live));      /* and only to the one that died */

    /* Silence, not absence. This is the whole point: the file being written
     * from `solo` is now ten seconds of digital zeros that nothing else in the
     * system would report. */
    ASSERT_NEAR(0.0, apr_bus_peak(bsolo), 0.0);
    /* The mix is untouched -- the survivor's audio is all of it now. */
    ASSERT_NEAR(0.5, apr_bus_peak(bmix), 0.01);

    /* Design 10: recording continues. Both buses produced every frame QPC
     * asked for, and the dead source is still exactly aligned -- had it
     * stopped delivering instead, this file would be short by ten seconds
     * against every other file in the session. */
    ASSERT_EQ_U64(frames_due(&rig, bmix),  apr_bus_frames_out(bmix));
    ASSERT_EQ_U64(frames_due(&rig, bsolo), apr_bus_frames_out(bsolo));
    ASSERT_EQ_U64(apr_bus_frames_out(bmix),
                  apr_source_reader_out_frames(apr_bus_reader_at(bmix, 1)));
    ASSERT_EQ_U64(apr_bus_frames_out(bsolo),
                  apr_source_reader_out_frames(apr_bus_reader_at(bsolo, 0)));
    ASSERT_TRUE(fabs(apr_source_reader_error(apr_bus_reader_at(bmix, 0))) < 1.0);
    ASSERT_TRUE(fabs(apr_source_reader_error(apr_bus_reader_at(bmix, 1))) < 1.0);
    ASSERT_TRUE(fabs(apr_source_reader_error(apr_bus_reader_at(bsolo, 0))) < 1.0);

    apr_graph_destroy(g);
}

TEST(a_source_muted_in_the_mixer_records_silence_and_recovers_when_unmuted)
{
    /* The single most likely "why is my recording empty" report, driven end to
     * end. Muted is not dead: alive stays 1 throughout, which is why the UI
     * needs two different clauses rather than one "not working" state. */
    Rig rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprSource *s;
    AprBus   *b;
    AprCaptureConfig cfg = fake_cfg(0, 440, 0.5f);
    AprCaptureStatus st;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 1, &g);
    rig.g = g;

    cfg.fake.mute_at_frame   = at_second(10.0);
    cfg.fake.unmute_at_frame = at_second(20.0);

    s = add_source_cfg(&rig, L"Teams", &cfg, 1);
    ASSERT_NOT_NULL(s);
    apr_graph_add_bus(g, L"Main", &bid);
    apr_graph_connect(g, apr_source_id(s), bid, 1.0f);
    b = apr_graph_bus(g, bid);

    rig_run(&rig);
    rig_seconds(&rig, 5.0);
    apr_source_poll(s, &st);
    ASSERT_EQ_INT(0, apr_source_muted(s));
    ASSERT_NEAR(0.5, apr_bus_peak(b), 0.01);

    rig_seconds(&rig, 10.0);          /* now 15 s: muted */
    apr_source_poll(s, &st);
    ASSERT_EQ_INT(1, apr_source_muted(s));
    ASSERT_EQ_INT(1, st.muted);
    ASSERT_EQ_INT(1, apr_source_alive(s));         /* muted is NOT dead */
    ASSERT_FALSE(apr_failed(&st.last_error));      /* and it is not an error */
    ASSERT_NEAR(0.0, apr_bus_peak(b), 0.0);

    rig_seconds(&rig, 10.0);          /* now 25 s: unmuted again */
    apr_source_poll(s, &st);
    ASSERT_EQ_INT(0, apr_source_muted(s));
    ASSERT_NEAR(0.5, apr_bus_peak(b), 0.01);

    /* And nothing moved. A gate over the timeline, not a pause in it. */
    ASSERT_EQ_U64(frames_due(&rig, b), apr_bus_frames_out(b));
    ASSERT_EQ_U64(apr_bus_frames_out(b),
                  apr_source_reader_out_frames(apr_bus_reader_at(b, 0)));
    ASSERT_TRUE(fabs(apr_source_reader_error(apr_bus_reader_at(b, 0))) < 1.0);

    apr_graph_destroy(g);
}

TEST(a_ring_overrun_costs_content_and_not_alignment)
{
    /* The mixer stalled for a full second -- four times the ring. Those frames
     * are gone and cannot come back. What must NOT happen is the reader
     * resuming from wherever the cursor landed and sliding everything after
     * the hole a second early: that would put this file a second out from
     * every other file in the session, for the rest of the session.
     *
     * So: exactly as many frames of silence as were lost, then real audio
     * again at its true absolute position. */
    Rig       rig;
    AprGraph *g = NULL;
    AprSource *s;
    AprSourceReader *rd = NULL;
    AprClock  clk;
    float     buf[BLOCK];
    uint64_t  lost_total = 0, blocks = 0, first_audio = 0;
    size_t    cap;
    AprErr    e;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 1, &g);
    rig.g   = g;
    rig.qpf = apr_qpc_freq();

    s   = add_source(&rig, L"Teams", 0, 440, 0.5f, 1);
    cap = rb_capacity_frames(apr_source_ring(s));

    e = apr_source_reader_open(s, 0.0, &rd);
    ASSERT_FALSE(apr_failed(&e));

    apr_clock_init(&clk, rig.qpf, RATE);
    apr_clock_anchor(&clk, T0);
    apr_capture_fake_advance(apr_source_capture(s), T0);

    /* One second of audio arrives while nothing reads. */
    apr_capture_fake_advance(apr_source_capture(s),
                             T0 + apr_frames_to_ticks(RATE, rig.qpf, RATE));
    ASSERT_GT_INT((long long)cap, (long long)apr_source_frames(s));

    /* Now catch up. Every pull must yield exactly BLOCK frames, silence or
     * not, and the reader must stay on the absolute timeline throughout. */
    for (blocks = 0; blocks < RATE / BLOCK; blocks++) {
        AprSourcePull p = apr_source_pull(rd, &clk, blocks * BLOCK, T0, buf, BLOCK);

        ASSERT_EQ_INT((int)BLOCK, (int)p.frames);
        ASSERT_EQ_INT(0, (int)p.lead);
        lost_total += p.lost;
        if (first_audio == 0 && apr_mix_peak(buf, BLOCK, 1) > 0.0f) {
            first_audio = blocks * BLOCK;
        }
        ASSERT_EQ_U64((blocks + 1) * BLOCK, apr_source_reader_out_frames(rd));
    }

    printf("      ring %zu frames, 1 s written: %llu frames lost, "
           "audio resumes at frame %llu\n",
           cap, (unsigned long long)lost_total,
           (unsigned long long)first_audio);

    /* Every overwritten frame was replaced by exactly one frame of silence,
     * plus at most one pull block: when rb_read discovers mid-copy that it has
     * been lapped, the frames it had already fetched belong at an index this
     * block has not reached, and they are dropped rather than misplaced. That
     * is the trade this whole path makes -- content for alignment -- and one
     * block of it after a quarter-second stall is the price. */
    ASSERT_GE_INT(RATE - (uint64_t)cap, (long long)lost_total);
    ASSERT_LE_INT(RATE - (uint64_t)cap + BLOCK, (long long)lost_total);

    /* The alignment claim itself: ring frame N came back at bus frame N. Audio
     * resumes in exactly the block that contains the first surviving frame --
     * not one block early, which is what sliding past the hole would do. */
    ASSERT_GT_INT(0, (long long)first_audio);
    ASSERT_EQ_U64((lost_total / BLOCK) * BLOCK, first_audio);

    apr_source_reader_close(rd);
    apr_graph_destroy(g);
}

TEST(a_source_that_starts_mid_session_lands_where_it_starts_not_at_the_beginning)
{
    /* A source whose first frame arrives five seconds into a session must land
     * five seconds in, not at the top of the file.
     *
     * THE EDGE IS BUILT BEFORE THE BUS STARTS, and that is not incidental any
     * more: BUGS.md C4 made the graph refuse every shape change while it is
     * running, because the runner's loop thread walks these very arrays. What
     * arrives late here is the SOURCE's first frame -- the fake anchors on its
     * first advance, so registering it for pumping at the five-second mark is
     * exactly a capture that produced nothing until then. The reader placement
     * being tested is unchanged; only the moment the edge was created moved,
     * and that was never what the arithmetic depended on. */
    Rig rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprSource *first, *late;
    AprBus   *b;
    uint64_t  late_start;
    AprErr    e;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 1, &g);
    rig.g = g;

    first = add_source(&rig, L"First", 0, 440, 0.5f, 1);
    apr_graph_add_bus(g, L"Main", &bid);
    apr_graph_connect(g, apr_source_id(first), bid, 1.0f);

    /* Created and wired now, but deliberately NOT registered for pumping, so
     * it produces nothing and never anchors. */
    late = add_source_unpumped(&rig, L"Late", 0, 660, 0.5f, 1);
    apr_graph_connect(g, apr_source_id(late), bid, 1.0f);

    b = apr_graph_bus(g, bid);

    rig_run(&rig);
    rig_seconds(&rig, 5.0);

    /* C4: the shape is frozen while the graph runs, and the refusal is
     * distinguishable rather than silent. */
    e = apr_graph_disconnect(g, apr_source_id(late), bid);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_BUSY, (int)e.kind);

    /* The late source's capture starts delivering here. */
    rig.pump[rig.pump_count++] = late;
    apr_capture_fake_advance(apr_source_capture(late), rig.now);

    rig_seconds(&rig, 5.0);

    late_start = apr_source_reader_first_bus_frame(apr_bus_reader_at(b, 1));
    printf("      late source starts at bus frame %llu (~%.2f s)\n",
           (unsigned long long)late_start, (double)late_start / (double)RATE);

    ASSERT_EQ_U64(0, apr_source_reader_first_bus_frame(apr_bus_reader_at(b, 0)));
    ASSERT_GT_INT(4 * (long long)RATE, (long long)late_start);
    ASSERT_LT_INT(6 * (long long)RATE, (long long)late_start);
    ASSERT_EQ_U64(frames_due(&rig, b), apr_bus_frames_out(b));

    apr_graph_destroy(g);
}

/* ---------------------------------------------------------------------------
 * Timing shape
 * ------------------------------------------------------------------------- */

TEST(a_late_tick_produces_a_bigger_block_and_no_drift)
{
    /* The mixer reads the clock; it does not count ticks. A scheduler hiccup
     * must change the block size and nothing else. */
    Rig rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprSource *s;
    AprBus   *b;
    uint64_t  before, after, jump;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 1, &g);
    rig.g = g;

    s = add_source(&rig, L"Teams", 0, 440, 0.5f, 1);
    apr_graph_add_bus(g, L"Main", &bid);
    apr_graph_connect(g, apr_source_id(s), bid, 1.0f);
    b = apr_graph_bus(g, bid);

    rig_run(&rig);
    rig_seconds(&rig, 2.0);
    before = apr_bus_frames_out(b);

    /* 150 ms with no tick at all, then one tick. */
    rig.ticks_done += 15;
    rig.now = tick_at(&rig, rig.ticks_done);
    apr_capture_fake_advance(apr_source_capture(s), rig.now);
    apr_graph_tick(g, rig.now);

    after = apr_bus_frames_out(b);
    jump  = after - before;
    ASSERT_EQ_U64(15 * BLOCK, jump);
    ASSERT_EQ_U64(frames_due(&rig, b), after);

    /* And a repeated tick at the same instant produces nothing at all. */
    apr_graph_tick(g, rig.now);
    ASSERT_EQ_U64(after, apr_bus_frames_out(b));

    apr_graph_destroy(g);
}

TEST(a_mono_source_reaches_a_stereo_bus_centred)
{
    Rig rig;
    AprGraph *g = NULL;
    AprBusId  bid = 0;
    AprSource *s;
    AprBus   *b;

    memset(&rig, 0, sizeof rig);
    apr_graph_create(RATE, 2, &g);      /* stereo session */
    rig.g = g;

    s = add_source(&rig, L"Mic", 0, 440, 0.5f, 1);
    ASSERT_EQ_INT(2, apr_source_channels(s));   /* the graph set the format */

    apr_graph_add_bus(g, L"Main", &bid);
    apr_graph_connect(g, apr_source_id(s), bid, 1.0f);
    b = apr_graph_bus(g, bid);

    rig_run(&rig);
    rig_seconds(&rig, 5.0);

    ASSERT_EQ_INT(2, apr_bus_channels(b));
    ASSERT_NEAR(0.5, apr_bus_peak(b), 0.01);
    ASSERT_EQ_U64(frames_due(&rig, b), apr_bus_frames_out(b));

    apr_graph_destroy(g);
}
