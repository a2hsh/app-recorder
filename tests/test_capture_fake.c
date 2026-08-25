/*
 * test_capture_fake.c -- the synthetic source, and the capture interface
 * through it.
 *
 * capture_fake is the reason design section 4.3 can promise the core is
 * testable with no audio hardware, so it carries more weight than a stub
 * normally would: if it drifts, mixes or counts wrongly, every test above it
 * agrees with it and nobody notices. It is therefore tested for the same
 * properties clock.c is -- exactness over a multi-hour timeline, no
 * accumulation, byte-level determinism -- rather than for "it produced some
 * audio".
 *
 * No audio hardware is touched and no real time elapses except in the two
 * tests that deliberately exercise the real-time pacing thread.
 */
#include "test_runner.h"

#include "capture.h"
#include "capture/capture_fake.h"
#include "clock.h"
#include "ringbuf.h"

#include <math.h>
#include <string.h>

#define RB_FRAMES 8192u

typedef struct Fixture {
    RingBuf    *rb;
    AprCapture *c;
} Fixture;

static AprCaptureConfig fake_cfg(uint32_t rate, uint16_t ch, int32_t ppm,
                                 uint32_t tone_hz, float amp)
{
    AprCaptureConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.kind        = APR_SRC_FAKE;
    cfg.sample_rate = rate;
    cfg.channels    = ch;
    cfg.fake.rate_error_ppm = ppm;
    cfg.fake.tone_hz        = tone_hz;
    cfg.fake.amplitude      = amp;
    return cfg;
}

static int fixture_open(Fixture *fx, const AprCaptureConfig *cfg,
                        size_t rb_frames)
{
    AprErr e;
    fx->rb = NULL;
    fx->c  = NULL;
    e = rb_create(rb_frames, (size_t)cfg->channels * sizeof(float), &fx->rb);
    if (apr_failed(&e)) return 0;
    e = apr_capture_create(cfg, fx->rb, &fx->c);
    if (apr_failed(&e)) { rb_destroy(fx->rb); fx->rb = NULL; return 0; }
    return 1;
}

static void fixture_close(Fixture *fx)
{
    apr_capture_destroy(fx->c);
    rb_destroy(fx->rb);
    fx->c = NULL;
    fx->rb = NULL;
}

/* Ticks that hold `ms` milliseconds, computed the same exact way the source
 * does rather than with a floating multiply. */
static uint64_t ms_ticks(uint64_t ms)
{
    return apr_mul_div_u64(ms, apr_qpc_freq(), 1000u, NULL);
}

/* ==========================================================================
 * The interface itself
 * ======================================================================= */

TEST(create_rejects_a_ring_whose_frame_size_disagrees)
{
    /* Every source promises interleaved float32 at the session channel count.
     * A ring built for something else would shear frames silently, which is
     * exactly the class of bug that only shows up hours into a recording. */
    RingBuf *rb = NULL;
    AprCapture *c = NULL;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 0, 0, 0.0f);
    AprErr e;

    e = rb_create(1024, 2 * sizeof(short), &rb);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_capture_create(&cfg, rb, &c);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_INVALID_ARG, e.kind);
    ASSERT_NULL(c);
    rb_destroy(rb);
}

TEST(create_rejects_impossible_configs)
{
    RingBuf *rb = NULL;
    AprCapture *c = NULL;
    AprCaptureConfig cfg;
    AprErr e;

    e = rb_create(1024, 2 * sizeof(float), &rb);
    ASSERT_FALSE(apr_failed(&e));

    cfg = fake_cfg(0, 2, 0, 0, 0.0f);                 /* no sample rate */
    e = apr_capture_create(&cfg, rb, &c);
    ASSERT_TRUE(apr_failed(&e));

    cfg = fake_cfg(48000, 0, 0, 0, 0.0f);             /* no channels */
    e = apr_capture_create(&cfg, rb, &c);
    ASSERT_TRUE(apr_failed(&e));

    cfg = fake_cfg(48000, 2, 2000000, 0, 0.0f);       /* not a crystal error */
    e = apr_capture_create(&cfg, rb, &c);
    ASSERT_TRUE(apr_failed(&e));

    cfg = fake_cfg(48000, 2, -1000000, 0, 0.0f);      /* a stopped clock */
    e = apr_capture_create(&cfg, rb, &c);
    ASSERT_TRUE(apr_failed(&e));

    cfg = fake_cfg(48000, 2, 0, 440, 0.0f);
    cfg.kind = (AprSourceKind)99;                     /* not a kind at all */
    e = apr_capture_create(&cfg, rb, &c);
    ASSERT_TRUE(apr_failed(&e));

    ASSERT_NULL(c);
    rb_destroy(rb);
}

TEST(the_vtable_names_itself_and_destroy_tolerates_null)
{
    Fixture fx;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 0, 0, 0.0f);

    ASSERT_TRUE(fixture_open(&fx, &cfg, RB_FRAMES));
    ASSERT_NOT_NULL(fx.c->vt);
    ASSERT_STR_EQ("fake", fx.c->vt->kind_name);
    fixture_close(&fx);

    apr_capture_destroy(NULL);   /* must not fault */
}

TEST(status_starts_unanchored_alive_and_error_free)
{
    Fixture fx;
    AprCaptureStatus st;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 0, 440, 0.25f);

    ASSERT_TRUE(fixture_open(&fx, &cfg, RB_FRAMES));
    fx.c->vt->status(fx.c, &st);

    ASSERT_EQ_U64(0u, st.anchor_ticks);      /* zero means not yet anchored */
    ASSERT_EQ_U64(0u, st.frames_written);
    ASSERT_EQ_U64(0u, st.discontinuities);
    ASSERT_EQ_INT(1, st.alive);
    ASSERT_EQ_INT(0, st.muted);
    ASSERT_FALSE(apr_failed(&st.last_error));

    fixture_close(&fx);
}

TEST(stop_is_idempotent_and_safe_before_start)
{
    Fixture fx;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 0, 0, 0.0f);

    ASSERT_TRUE(fixture_open(&fx, &cfg, RB_FRAMES));
    fx.c->vt->stop(fx.c);
    fx.c->vt->stop(fx.c);
    fixture_close(&fx);
}

/* ==========================================================================
 * Anchoring
 * ======================================================================= */

TEST(the_first_advance_anchors_and_produces_nothing)
{
    /* A real capture anchors on the arrival of its first buffer, and cannot
     * have produced frames before that instant. The fake has to agree, or every
     * drift figure computed against it is off by the first buffer. */
    Fixture fx;
    AprCaptureStatus st;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 0, 0, 0.0f);
    const uint64_t t0 = 123456789u;
    AprErr e;

    ASSERT_TRUE(fixture_open(&fx, &cfg, RB_FRAMES));

    e = apr_capture_fake_advance(fx.c, t0);
    ASSERT_FALSE(apr_failed(&e));
    fx.c->vt->status(fx.c, &st);
    ASSERT_EQ_U64(t0, st.anchor_ticks);
    ASSERT_EQ_U64(0u, st.frames_written);

    fixture_close(&fx);
}

TEST(advancing_backwards_is_refused)
{
    Fixture fx;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 0, 0, 0.0f);
    AprErr e;

    ASSERT_TRUE(fixture_open(&fx, &cfg, RB_FRAMES));
    e = apr_capture_fake_advance(fx.c, 1000000u);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_capture_fake_advance(fx.c, 1000000u + ms_ticks(10));
    ASSERT_FALSE(apr_failed(&e));
    e = apr_capture_fake_advance(fx.c, 1000000u);
    ASSERT_TRUE(apr_failed(&e));
    fixture_close(&fx);
}

TEST(the_driven_and_real_time_modes_are_mutually_exclusive)
{
    Fixture fx;
    AprCaptureConfig cfg = fake_cfg(8000, 1, 0, 0, 0.0f);
    AprErr e;

    ASSERT_TRUE(fixture_open(&fx, &cfg, RB_FRAMES));
    e = fx.c->vt->start(fx.c);
    ASSERT_FALSE(apr_failed(&e));
    fx.c->vt->stop(fx.c);

    e = apr_capture_fake_advance(fx.c, apr_qpc_now());
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_STATE, e.kind);

    fixture_close(&fx);
}

TEST(the_driver_refuses_a_source_that_is_not_a_fake)
{
    /* Nothing above capture.h may branch on kind, so this door has to close
     * itself rather than trust its caller. */
    AprCapture bogus;
    AprErr e;
    memset(&bogus, 0, sizeof(bogus));
    e = apr_capture_fake_advance(&bogus, 1);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_U64(0u, apr_capture_fake_tick_rate(&bogus));
    ASSERT_EQ_U64(0u, apr_capture_fake_tick_rate(NULL));
}

/* ==========================================================================
 * Frame counts: exact, absolute, unaffected by how time is chopped up
 * ======================================================================= */

/* The count the source is contractually obliged to have produced. Written out
 * here in full rather than calling the same helper the implementation uses, so
 * a mistake in one does not cancel a mistake in the other. */
static uint64_t due(uint64_t elapsed_ticks, uint32_t rate, int32_t ppm)
{
    return apr_mul_div_u64(elapsed_ticks,
                           (uint64_t)rate * (uint64_t)(1000000 + ppm),
                           apr_qpc_freq() * 1000000ull, NULL);
}

TEST(frame_count_matches_elapsed_ticks_exactly)
{
    Fixture fx;
    AprCaptureStatus st;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 0, 0, 0.0f);
    const uint64_t t0 = 7777777u;
    uint64_t elapsed;

    ASSERT_TRUE(fixture_open(&fx, &cfg, RB_FRAMES));
    apr_capture_fake_advance(fx.c, t0);

    elapsed = ms_ticks(1000);
    apr_capture_fake_advance(fx.c, t0 + elapsed);
    fx.c->vt->status(fx.c, &st);
    ASSERT_EQ_U64(due(elapsed, 48000, 0), st.frames_written);

    elapsed = ms_ticks(2500);
    apr_capture_fake_advance(fx.c, t0 + elapsed);
    fx.c->vt->status(fx.c, &st);
    ASSERT_EQ_U64(due(elapsed, 48000, 0), st.frames_written);

    fixture_close(&fx);
}

TEST(stepping_finely_gives_the_same_count_as_one_leap)
{
    /* This is the accumulation trap clock.h describes, applied to the source:
     * converting per buffer and summing floors once per buffer, and over a long
     * session that is minutes of error. Ten thousand steps must land on exactly
     * the same frame as one step of the same length. */
    Fixture fine, coarse;
    AprCaptureStatus a, b;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 0, 0, 0.0f);
    const uint64_t t0 = 31415926u;
    const uint64_t span = ms_ticks(60000);
    uint64_t i;

    ASSERT_TRUE(fixture_open(&fine, &cfg, RB_FRAMES));
    ASSERT_TRUE(fixture_open(&coarse, &cfg, RB_FRAMES));

    apr_capture_fake_advance(fine.c, t0);
    apr_capture_fake_advance(coarse.c, t0);

    for (i = 1; i <= 10000u; i++)
        apr_capture_fake_advance(fine.c, t0 + span * i / 10000u);
    apr_capture_fake_advance(coarse.c, t0 + span);

    fine.c->vt->status(fine.c, &a);
    coarse.c->vt->status(coarse.c, &b);
    ASSERT_EQ_U64(b.frames_written, a.frames_written);
    ASSERT_EQ_U64(due(span, 48000, 0), a.frames_written);

    fixture_close(&fine);
    fixture_close(&coarse);
}

TEST(a_silent_source_still_advances_the_timeline)
{
    /* tone_hz 0 writes through rb_write_silence rather than a table of zeroes,
     * so the two paths have to agree about position. */
    Fixture fx;
    AprCaptureStatus st;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 0, 0 /* silence */, 0.0f);
    const uint64_t t0 = 1000000u;

    ASSERT_TRUE(fixture_open(&fx, &cfg, RB_FRAMES));
    apr_capture_fake_advance(fx.c, t0);
    apr_capture_fake_advance(fx.c, t0 + ms_ticks(100));
    fx.c->vt->status(fx.c, &st);

    ASSERT_EQ_U64(due(ms_ticks(100), 48000, 0), st.frames_written);
    ASSERT_EQ_U64(st.frames_written, rb_write_pos(fx.rb));

    fixture_close(&fx);
}

/* ==========================================================================
 * The wrong crystal -- design 5.2, the reason this source exists
 * ======================================================================= */

TEST(rate_error_ppm_shows_up_as_drift_against_qpc)
{
    Fixture fx;
    AprCaptureStatus st;
    AprClock clk;
    AprDrift d;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 30 /* ppm fast */, 0, 0.0f);
    const uint64_t t0 = 555000u;
    const uint64_t span = ms_ticks(3600000);   /* one hour */

    ASSERT_TRUE(fixture_open(&fx, &cfg, RB_FRAMES));
    apr_capture_fake_advance(fx.c, t0);
    apr_capture_fake_advance(fx.c, t0 + span);
    fx.c->vt->status(fx.c, &st);

    apr_clock_init(&clk, apr_capture_fake_tick_rate(fx.c), 48000);
    apr_clock_anchor(&clk, st.anchor_ticks);
    d = apr_clock_drift(&clk, t0 + span, st.frames_written);

    /* A source running fast delivers a surplus, so delta is negative. */
    ASSERT_LT_INT(0, d.delta_frames);
    /* One hour at 48 kHz is 172.8e6 frames; 30 ppm of that is 5184. */
    ASSERT_EQ_U64(5184u, (uint64_t)(-d.delta_frames));
    /* And the measured rate error is the one that was asked for, to the ppb. */
    ASSERT_EQ_INT(30000, (int)apr_drift_ppb(&d));

    fixture_close(&fx);
}

TEST(a_slow_crystal_reads_as_missing_frames)
{
    Fixture fx;
    AprCaptureStatus st;
    AprClock clk;
    AprDrift d;
    AprCaptureConfig cfg = fake_cfg(48000, 2, -50, 0, 0.0f);
    const uint64_t t0 = 42u;
    const uint64_t span = ms_ticks(3600000);

    ASSERT_TRUE(fixture_open(&fx, &cfg, RB_FRAMES));
    apr_capture_fake_advance(fx.c, t0);
    apr_capture_fake_advance(fx.c, t0 + span);
    fx.c->vt->status(fx.c, &st);

    apr_clock_init(&clk, apr_capture_fake_tick_rate(fx.c), 48000);
    apr_clock_anchor(&clk, st.anchor_ticks);
    d = apr_clock_drift(&clk, t0 + span, st.frames_written);

    ASSERT_EQ_U64(8640u, (uint64_t)d.delta_frames);   /* 50 ppm of 172.8e6 */
    ASSERT_EQ_INT(-50000, (int)apr_drift_ppb(&d));

    fixture_close(&fx);
}

TEST(a_three_hour_session_stays_within_one_frame)
{
    /* Design 5.2 test strategy, verbatim: "a simulated multi-hour session must
     * end with alignment error below one sample". Stepped 10 ms at a time --
     * 1.08 million steps, the same count a real session would take -- because
     * the failure mode being guarded against is per-step flooring, and a single
     * leap would not exercise it at all.
     *
     * 8 kHz mono keeps this at a second or so in a Debug build. The arithmetic
     * under test is rate-independent; the step count is what matters. */
    Fixture fx;
    AprCaptureStatus st;
    AprClock clk;
    AprDrift d;
    AprCaptureConfig cfg = fake_cfg(8000, 1, 30, 0, 0.0f);
    const uint64_t t0 = 987654321u;
    const uint64_t step = ms_ticks(10);
    const uint64_t steps = 3u * 3600u * 100u;         /* three hours of them */
    uint64_t i, expected;

    ASSERT_TRUE(fixture_open(&fx, &cfg, 4096));
    apr_capture_fake_advance(fx.c, t0);
    for (i = 1; i <= steps; i++) apr_capture_fake_advance(fx.c, t0 + step * i);

    fx.c->vt->status(fx.c, &st);
    expected = due(step * steps, 8000, 30);
    ASSERT_EQ_U64(expected, st.frames_written);

    /* And the drift the core would compute is exactly the 30 ppm asked for,
     * not 30 ppm plus a million accumulated floors. */
    apr_clock_init(&clk, apr_capture_fake_tick_rate(fx.c), 8000);
    apr_clock_anchor(&clk, st.anchor_ticks);
    d = apr_clock_drift(&clk, t0 + step * steps, st.frames_written);
    ASSERT_EQ_INT(30000, (int)apr_drift_ppb(&d));

    fixture_close(&fx);
}

/* ==========================================================================
 * Determinism and signal content
 * ======================================================================= */

/* Read everything the ring holds for a reader attached at the oldest frame. */
static size_t drain_all(RingBuf *rb, float *dst, size_t max_frames,
                        uint64_t *out_lost)
{
    RingReader rd;
    rb_reader_init(&rd, rb, RB_START_OLDEST);
    return rb_read(&rd, dst, max_frames, out_lost);
}

TEST(the_same_timeline_produces_byte_identical_audio)
{
    /* Determinism is not a nicety here: a golden-file encoder test compares
     * bytes, and it can only do that if the source is reproducible. */
    Fixture a, b;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 17, 440, 0.25f);
    static float buf_a[2048 * 2], buf_b[2048 * 2];
    const uint64_t t0 = 24680u;
    size_t na, nb;
    uint64_t i;

    ASSERT_TRUE(fixture_open(&a, &cfg, 4096));
    ASSERT_TRUE(fixture_open(&b, &cfg, 4096));

    apr_capture_fake_advance(a.c, t0);
    apr_capture_fake_advance(b.c, t0);

    /* Different chopping, same timeline: the audio must not care. */
    apr_capture_fake_advance(a.c, t0 + ms_ticks(40));
    for (i = 1; i <= 40u; i++) apr_capture_fake_advance(b.c, t0 + ms_ticks(i));

    na = drain_all(a.rb, buf_a, 2048, NULL);
    nb = drain_all(b.rb, buf_b, 2048, NULL);
    ASSERT_EQ_U64((uint64_t)na, (uint64_t)nb);
    ASSERT_TRUE(na > 1000);
    ASSERT_MEM_EQ(buf_a, buf_b, na * 2 * sizeof(float));

    fixture_close(&a);
    fixture_close(&b);
}

TEST(the_tone_is_the_requested_frequency_and_amplitude)
{
    /* Amplitude by peak, frequency by counting the zero crossings of a whole
     * number of periods. Both are properties an encoder test will lean on. */
    Fixture fx;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 0, 1000, 0.5f);
    static float buf[4800 * 2];
    const uint64_t t0 = 1u;
    size_t n, i;
    float peak = 0.0f, prev;
    double sumsq = 0.0;
    int crossings = 0;

    ASSERT_TRUE(fixture_open(&fx, &cfg, 8192));
    apr_capture_fake_advance(fx.c, t0);
    apr_capture_fake_advance(fx.c, t0 + ms_ticks(100));   /* 4800 frames */

    n = drain_all(fx.rb, buf, 4800, NULL);
    ASSERT_EQ_U64(4800u, (uint64_t)n);

    /* 4800 frames at 48 kHz is a whole number of 1000 Hz periods, so the last
     * sample is the cyclic predecessor of the first. Seeding with it counts the
     * crossing at frame 0 -- and incidentally asserts the tone really is
     * periodic rather than merely close. */
    prev = buf[(n - 1) * 2];
    for (i = 0; i < n; i++) {
        float v = buf[i * 2];
        float a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        sumsq += (double)v * (double)v;
        /* Both channels carry the same signal. */
        ASSERT_TRUE(buf[i * 2 + 1] == v);
        if (prev < 0.0f && v >= 0.0f) crossings++;
        prev = v;
    }

    ASSERT_NEAR(0.5, (double)peak, 0.001);
    ASSERT_NEAR(0.5 / 1.41421356, sqrt(sumsq / (double)n), 0.001);
    /* 1000 Hz for 100 ms is 100 rising zero crossings. */
    ASSERT_EQ_INT(100, crossings);

    fixture_close(&fx);
}

TEST(silence_is_actually_silent)
{
    Fixture fx;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 0, 0, 0.0f);
    static float buf[4800 * 2];
    size_t n, i;

    ASSERT_TRUE(fixture_open(&fx, &cfg, 8192));
    apr_capture_fake_advance(fx.c, 1u);
    apr_capture_fake_advance(fx.c, 1u + ms_ticks(50));

    n = drain_all(fx.rb, buf, 4800, NULL);
    ASSERT_TRUE(n > 0);
    for (i = 0; i < n * 2; i++) ASSERT_TRUE(buf[i] == 0.0f);

    fixture_close(&fx);
}

TEST(a_tone_at_zero_amplitude_is_silence_not_a_table_of_zeroes)
{
    Fixture fx;
    AprCaptureStatus st;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 0, 440, 0.0f);
    static float buf[960 * 2];
    size_t n, i;

    ASSERT_TRUE(fixture_open(&fx, &cfg, 4096));
    apr_capture_fake_advance(fx.c, 1u);
    apr_capture_fake_advance(fx.c, 1u + ms_ticks(20));

    fx.c->vt->status(fx.c, &st);
    ASSERT_TRUE(st.frames_written > 0);
    n = drain_all(fx.rb, buf, 960, NULL);
    for (i = 0; i < n * 2; i++) ASSERT_TRUE(buf[i] == 0.0f);

    fixture_close(&fx);
}

TEST(mono_and_eight_channel_sources_both_work)
{
    /* The channel count reaches the ring through frame_bytes, so an off-by-one
     * here shears every frame. Both ends of the supported range. */
    Fixture m, e8;
    AprCaptureConfig cm = fake_cfg(48000, 1, 0, 400, 0.25f);
    AprCaptureConfig c8 = fake_cfg(48000, 8, 0, 400, 0.25f);
    static float buf[480 * 8];
    size_t n, i;

    ASSERT_TRUE(fixture_open(&m, &cm, 4096));
    apr_capture_fake_advance(m.c, 1u);
    apr_capture_fake_advance(m.c, 1u + ms_ticks(10));
    n = drain_all(m.rb, buf, 480, NULL);
    ASSERT_TRUE(n > 0);
    fixture_close(&m);

    ASSERT_TRUE(fixture_open(&e8, &c8, 4096));
    apr_capture_fake_advance(e8.c, 1u);
    apr_capture_fake_advance(e8.c, 1u + ms_ticks(10));
    n = drain_all(e8.rb, buf, 480, NULL);
    ASSERT_TRUE(n > 0);
    for (i = 1; i < 8; i++) ASSERT_TRUE(buf[i] == buf[0]);
    fixture_close(&e8);
}

TEST(overrunning_the_ring_is_reported_as_loss_not_as_missing_frames)
{
    /* The source keeps the timeline honest even when nobody is reading: the
     * write position still counts every frame, and the reader is told exactly
     * how many it lost, which is what the drift corrector needs (ringbuf.h). */
    Fixture fx;
    AprCaptureStatus st;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 0, 440, 0.25f);
    static float buf[1024 * 2];
    RingReader rd;
    uint64_t lost = 0;
    size_t n;

    ASSERT_TRUE(fixture_open(&fx, &cfg, 1024));   /* deliberately tiny */
    apr_capture_fake_advance(fx.c, 1u);

    /* Attached before a single frame exists, so everything the ring drops is
     * genuinely this reader's loss -- a reader attached afterwards would be
     * placed at the oldest surviving frame and would have lost nothing
     * (ringbuf.h). */
    rb_reader_init(&rd, fx.rb, RB_START_LATEST);

    apr_capture_fake_advance(fx.c, 1u + ms_ticks(500));   /* 24000 frames */

    fx.c->vt->status(fx.c, &st);
    ASSERT_EQ_U64(due(ms_ticks(500), 48000, 0), st.frames_written);
    ASSERT_EQ_U64(st.frames_written, rb_write_pos(fx.rb));

    n = rb_read(&rd, buf, 1024, &lost);
    ASSERT_EQ_U64(1024u, (uint64_t)n);
    /* Every frame that did not fit is accounted for exactly: that count is
     * what the drift corrector turns back into silence (design section 5). */
    ASSERT_EQ_U64(st.frames_written - 1024u, lost);
    ASSERT_EQ_U64(st.frames_written, lost + (uint64_t)n);

    fixture_close(&fx);
}

/* ==========================================================================
 * Real-time mode -- the only tests here that let real time pass
 * ======================================================================= */

TEST(real_time_mode_produces_roughly_the_right_number_of_frames)
{
    Fixture fx;
    AprCaptureStatus st;
    AprCaptureConfig cfg = fake_cfg(48000, 2, 0, 440, 0.25f);
    AprErr e;
    uint64_t t0, elapsed, expected;

    ASSERT_TRUE(fixture_open(&fx, &cfg, 48000));

    t0 = apr_qpc_now();
    e = fx.c->vt->start(fx.c);
    ASSERT_FALSE(apr_failed(&e));
    Sleep(120);
    fx.c->vt->stop(fx.c);
    elapsed = apr_qpc_now() - t0;

    fx.c->vt->status(fx.c, &st);
    ASSERT_TRUE(st.anchor_ticks >= t0);
    expected = due(elapsed, 48000, 0);

    /* Generous: this is a scheduling test, not an arithmetic one. What is
     * being checked is that the pacing thread ran and stopped, not its jitter. */
    ASSERT_TRUE(st.frames_written > expected / 2);
    ASSERT_TRUE(st.frames_written <= expected);
    ASSERT_EQ_INT(1, st.alive);

    fixture_close(&fx);
}

TEST(start_is_idempotent_and_stop_joins_the_thread)
{
    Fixture fx;
    AprCaptureStatus before, after;
    AprCaptureConfig cfg = fake_cfg(8000, 1, 0, 0, 0.0f);
    AprErr e;

    ASSERT_TRUE(fixture_open(&fx, &cfg, 8192));
    e = fx.c->vt->start(fx.c);
    ASSERT_FALSE(apr_failed(&e));
    e = fx.c->vt->start(fx.c);          /* second start must be a no-op */
    ASSERT_FALSE(apr_failed(&e));
    Sleep(30);
    fx.c->vt->stop(fx.c);

    /* Once stop has returned, the thread is joined and nothing more arrives. */
    fx.c->vt->status(fx.c, &before);
    Sleep(30);
    fx.c->vt->status(fx.c, &after);
    ASSERT_EQ_U64(before.frames_written, after.frames_written);

    fx.c->vt->stop(fx.c);               /* still idempotent afterwards */
    fixture_close(&fx);
}
