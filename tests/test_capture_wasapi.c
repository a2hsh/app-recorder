/*
 * test_capture_wasapi.c -- the two real capture kinds.
 *
 * Design section 11: "nothing in CI depends on hardware being present". So the
 * argument checking here is unconditional, and everything that needs a live
 * audio engine reports SKIPPED rather than failing when there is none.
 *
 * NOTHING IN THIS FILE RENDERS AUDIO. See AGENTS.md rule 1.
 *
 * And nothing in this file ever calls start() on a DEVICE source: a device
 * capture that has been started is recording the author's microphone, and an
 * automated test is not the place to decide that is acceptable. Device coverage
 * stops at open/close, which reads no samples at all -- WASAPI hands over
 * nothing until Start.
 *
 * The process tap IS started, against this test process's own PID. Its tree
 * renders nothing, so what gets captured is the audio engine's own silence and
 * nothing else. That is also the cheapest possible check of the spike's central
 * finding: a process that never rendered a sample still produces a continuous,
 * gapless stream.
 */
#include "test_runner.h"

#include "capture.h"
#include "clock.h"
#include "ringbuf.h"

#include <windows.h>
#include <string.h>

static void skip(const char *why, const AprErr *e)
{
    wchar_t buf[512];
    printf("      SKIPPED: %s\n", why);
    if (e) {
        apr_err_format(e, buf, 512);
        printf("      reason: %ls\n", buf);
    }
}

/* ==========================================================================
 * Argument checking -- no hardware involved
 * ======================================================================= */

TEST(a_process_source_refuses_pid_zero)
{
    RingBuf *rb = NULL;
    AprCapture *c = NULL;
    AprCaptureConfig cfg;
    AprErr e;

    memset(&cfg, 0, sizeof(cfg));
    cfg.kind = APR_SRC_PROCESS;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.process.pid = 0;

    e = rb_create(4096, 2 * sizeof(float), &rb);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_capture_create(&cfg, rb, &c);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_INVALID_ARG, e.kind);
    ASSERT_NULL(c);

    rb_destroy(rb);
}

TEST(a_process_source_refuses_a_pid_that_does_not_exist)
{
    /* Activation itself is happy to tap a PID that is not there -- the spike
     * measured a perfect gapless stream from a process that never rendered.
     * The death detector is what notices, and it has to notice at open, not
     * after an hour of silence has been written to disk. */
    RingBuf *rb = NULL;
    AprCapture *c = NULL;
    AprCaptureConfig cfg;
    AprErr e;

    memset(&cfg, 0, sizeof(cfg));
    cfg.kind = APR_SRC_PROCESS;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.process.pid = 0x7ffffffeu;   /* not a plausible live PID */

    e = rb_create(4096, 2 * sizeof(float), &rb);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_capture_create(&cfg, rb, &c);
    if (!apr_failed(&e)) {
        /* If some future Windows starts accepting it, say so rather than
         * silently passing. */
        FAIL("a nonexistent pid was accepted");
    }
    ASSERT_NULL(c);
    rb_destroy(rb);
}

TEST(a_device_source_refuses_an_endpoint_id_that_does_not_exist)
{
    RingBuf *rb = NULL;
    AprCapture *c = NULL;
    AprCaptureConfig cfg;
    AprErr e;

    memset(&cfg, 0, sizeof(cfg));
    cfg.kind = APR_SRC_DEVICE;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.device.endpoint_id = L"{not-an-endpoint-id}";

    e = rb_create(4096, 2 * sizeof(float), &rb);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_capture_create(&cfg, rb, &c);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(c);

    rb_destroy(rb);
}

/* ==========================================================================
 * Live audio engine -- skipped when there is none
 * ======================================================================= */

TEST(a_process_tap_on_this_very_process_is_gapless_and_silent)
{
    RingBuf *rb = NULL;
    AprCapture *c = NULL;
    AprCaptureConfig cfg;
    AprCaptureStatus st;
    AprClock clk;
    AprDrift d;
    AprErr e;
    uint64_t t_start, t_stop;

    memset(&cfg, 0, sizeof(cfg));
    cfg.kind = APR_SRC_PROCESS;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.process.pid = (uint32_t)GetCurrentProcessId();
    cfg.process.exclude = 0;    /* INCLUDE: this process tree only */

    e = rb_create(48000, 2 * sizeof(float), &rb);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_capture_create(&cfg, rb, &c);
    if (apr_failed(&e)) {
        skip("no process-loopback activation on this machine", &e);
        rb_destroy(rb);
        return;
    }

    t_start = apr_qpc_now();
    e = c->vt->start(c);
    if (apr_failed(&e)) {
        skip("process loopback would not start", &e);
        apr_capture_destroy(c);
        rb_destroy(rb);
        return;
    }
    Sleep(400);
    c->vt->stop(c);
    t_stop = apr_qpc_now();

    c->vt->status(c, &st);

    /* The spike's central finding, retested cheaply: a process that has never
     * rendered a sample still produces a continuous stream. */
    ASSERT_TRUE(st.frames_written > 0);
    ASSERT_TRUE(st.anchor_ticks >= t_start);
    ASSERT_EQ_U64(0u, st.discontinuities);   /* design 5.1: process taps never gap */
    ASSERT_EQ_INT(1, st.alive);              /* we are, demonstrably, alive */
    ASSERT_FALSE(apr_failed(&st.last_error));

    /* And it is locked to the engine, not merely approximately right: over
     * 400 ms the frame count must be inside a couple of buffers of what QPC
     * expected. (100% fill was measured over 120 s; this is the cheap version.) */
    apr_clock_init(&clk, apr_qpc_freq(), 48000);
    apr_clock_anchor(&clk, st.anchor_ticks);
    d = apr_clock_drift(&clk, t_stop, st.frames_written);
    ASSERT_TRUE(d.delta_frames < 2000 && d.delta_frames > -2000);

    /* Nothing in this process rendered anything, so every frame must be zero.
     * If this ever fails, loopback is not per-process-tree and design 4.1 is
     * wrong. */
    {
        static float buf[4096 * 2];
        RingReader rd;
        size_t n, i;
        int nonzero = 0;
        rb_reader_init(&rd, rb, RB_START_OLDEST);
        while ((n = rb_read(&rd, buf, 4096, NULL)) > 0) {
            for (i = 0; i < n * 2; i++) if (buf[i] != 0.0f) nonzero++;
        }
        ASSERT_EQ_INT(0, nonzero);
    }

    apr_capture_destroy(c);
    rb_destroy(rb);
}

TEST(a_process_tap_stops_cleanly_and_twice)
{
    RingBuf *rb = NULL;
    AprCapture *c = NULL;
    AprCaptureConfig cfg;
    AprErr e;

    memset(&cfg, 0, sizeof(cfg));
    cfg.kind = APR_SRC_PROCESS;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.process.pid = (uint32_t)GetCurrentProcessId();

    e = rb_create(4096, 2 * sizeof(float), &rb);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_capture_create(&cfg, rb, &c);
    if (apr_failed(&e)) { skip("no process-loopback activation", &e); rb_destroy(rb); return; }

    e = c->vt->start(c);
    if (apr_failed(&e)) { skip("would not start", &e); apr_capture_destroy(c); rb_destroy(rb); return; }

    Sleep(50);
    c->vt->stop(c);
    c->vt->stop(c);          /* idempotent */
    apr_capture_destroy(c);  /* close implies stop a third time */
    rb_destroy(rb);
}

TEST(the_default_capture_endpoint_opens_at_the_session_format)
{
    /* Opens and closes. Never starts, so not one sample of the author's
     * microphone is read -- see the file comment. What this proves is that the
     * device path negotiates a format at all, which is the part that differs
     * from a process tap. */
    RingBuf *rb = NULL;
    AprCapture *c = NULL;
    AprCaptureConfig cfg;
    AprCaptureStatus st;
    AprErr e;

    memset(&cfg, 0, sizeof(cfg));
    cfg.kind = APR_SRC_DEVICE;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.device.endpoint_id = NULL;    /* the default */

    e = rb_create(4096, 2 * sizeof(float), &rb);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_capture_create(&cfg, rb, &c);
    if (apr_failed(&e)) {
        skip("no capture endpoint on this machine", &e);
        rb_destroy(rb);
        return;
    }

    ASSERT_STR_EQ("device-capture", c->vt->kind_name);
    c->vt->status(c, &st);
    ASSERT_EQ_INT(1, st.alive);
    ASSERT_EQ_U64(0u, st.frames_written);
    ASSERT_EQ_U64(0u, st.anchor_ticks);

    apr_capture_destroy(c);
    rb_destroy(rb);
}
