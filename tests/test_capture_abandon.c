/*
 * test_capture_abandon.c -- what happens when a capture thread will not stop.
 *
 * THE BUG THIS FILE EXISTS FOR (BUGS.md C1)
 *
 *   The capture layer already detected a wedged pump and correctly declined to
 *   free ITS OWN allocation. But proc_close, dev_close, apr_capture_destroy
 *   and apr_source_destroy all returned void, so the detection could not reach
 *   the layer that owns the RING -- and apr_source_destroy called rb_destroy()
 *   regardless. The pump then unwedged and memcpy'd into freed memory: heap
 *   corruption from an audio thread, surfacing minutes later somewhere else
 *   entirely.
 *
 *   The fix is a value, and the value has to be read by the code that frees.
 *   Both halves are asserted here.
 *
 * WHY A SEAM RATHER THAN A REAL WEDGE
 *
 *   Reproducing it for real needs WASAPI to hang inside GetBuffer, which is
 *   precisely the thing nobody can arrange on demand -- which is why this path
 *   had never once executed. apr_capture_fake_wedge() makes the fake source's
 *   pacing thread ignore its stop event and keep writing into the ring. That
 *   is not a simulation of the failure; it IS the failure, minus the part that
 *   needs a broken audio driver.
 *
 * NOTHING IN THIS FILE RENDERS AUDIO. See AGENTS.md rule 1. The fake source
 * writes to a ring buffer and nothing else; the one real capture opened below
 * is a process tap on this test's own PID, whose tree renders nothing.
 */
#include "test_runner.h"
#include "test_wait.h"

#include "capture/capture_fake.h"
#include "capture/capture_process.h"
#include "capture/wasapi_common.h"
#include "capture.h"
#include "clock.h"
#include "graph.h"
#include "ringbuf.h"
#include "source.h"

#include <windows.h>
#include <string.h>

/* MSVC in C mode: an assignment is not an lvalue, so the &(e = f(...)) idiom
 * does not compile. This is the same assertion without the trick. */
#define ASSERT_OK(call) do { AprErr e_ = (call); ASSERT_FALSE(apr_failed(&e_)); } while (0)

static void fake_cfg(AprCaptureConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->kind        = APR_SRC_FAKE;
    cfg->sample_rate = 48000;
    cfg->channels    = 2;
    cfg->fake.tone_hz = 0;  /* silence: nothing renders, nothing is heard */
}

/* Let the wedged pacing thread run long enough to have written again. */
static void let_it_run(void) { Sleep(60); }

/* ==========================================================================
 * C1 -- the ring is not freed under a live capture thread
 * ======================================================================= */

TEST(a_wedged_capture_makes_destroy_fail_instead_of_freeing_the_ring)
{
    AprCaptureConfig cfg;
    AprSource *s   = NULL;
    RingBuf   *rb  = NULL;
    uint64_t   pos_a, pos_b;
    AprErr     e;

    fake_cfg(&cfg);
    e = apr_source_create(1, L"wedged", &cfg, &s);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_NOT_NULL(s);

    e = apr_source_start(s);
    ASSERT_FALSE(apr_failed(&e));

    rb = apr_source_ring(s);
    ASSERT_NOT_NULL(rb);

    e = apr_capture_fake_wedge(apr_source_capture(s));
    ASSERT_FALSE(apr_failed(&e));

    /* THE ASSERTION THE WHOLE MECHANISM IS FOR. Before the fix this returned
     * void and freed the ring anyway. */
    e = apr_source_destroy(s);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_TIMEOUT, e.kind);

    /* ... and the ring is still there, still being written into. Reading it
     * here is only safe BECAUSE nothing was freed: that is the point of the
     * test, and under the old behaviour these two lines are a use-after-free
     * against a live producer. */
    pos_a = rb_write_pos(rb);
    let_it_run();
    pos_b = rb_write_pos(rb);
    ASSERT_GT_INT((long long)pos_a, (long long)pos_b);

    /* Recover the deliberate leak: once the thread does leave, the same
     * pointer can be destroyed again and this time it succeeds. */
    e = apr_capture_fake_unwedge(apr_source_capture(s));
    ASSERT_FALSE(apr_failed(&e));
    e = apr_source_destroy(s);
    ASSERT_FALSE(apr_failed(&e));
}

TEST(a_capture_that_stops_cleanly_still_destroys_and_reports_ok)
{
    /* The mechanism must not have made the ordinary path noisy. */
    AprCaptureConfig cfg;
    AprSource *s = NULL;
    AprErr     e;

    fake_cfg(&cfg);
    e = apr_source_create(1, L"ordinary", &cfg, &s);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_OK(apr_source_start(s));

    e = apr_source_destroy(s);
    ASSERT_FALSE(apr_failed(&e));
}

TEST(destroying_a_null_source_or_capture_is_success_not_a_fault)
{
    AprErr e = apr_source_destroy(NULL);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_capture_destroy(NULL);
    ASSERT_FALSE(apr_failed(&e));
}

TEST(the_graph_reports_a_source_it_could_not_retire)
{
    /* The value has to survive the trip up through the graph, or a front end
     * announces a clean removal of a source that is still running. */
    AprGraph        *g = NULL;
    AprCaptureConfig cfg;
    AprSourceId      id = 0;
    AprSource       *s;
    AprErr           e;

    ASSERT_OK(apr_graph_create(48000, 2, &g));

    fake_cfg(&cfg);
    ASSERT_OK(apr_graph_add_source(g, L"wedged", &cfg, &id));

    /* arm() starts every capture without anchoring a bus, so the graph is not
     * "running" and the shape guard (C4) does not apply. */
    ASSERT_OK(apr_graph_arm(g));

    s = apr_graph_source(g, id);
    ASSERT_NOT_NULL(s);
    ASSERT_OK(apr_capture_fake_wedge(apr_source_capture(s)));

    e = apr_graph_remove_source(g, id);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_TIMEOUT, e.kind);

    /* The slot is released either way -- the source is out of the graph, it is
     * simply leaked rather than freed. */
    ASSERT_EQ_INT(0, (long long)apr_graph_source_count(g));

    apr_graph_destroy(g);

    /* `s` outlived the graph on purpose; clean it up now that it can be. */
    ASSERT_OK(apr_capture_fake_unwedge(apr_source_capture(s)));
    ASSERT_OK(apr_source_destroy(s));
}

TEST(a_graph_destroyed_around_a_wedged_source_does_not_free_it)
{
    /* apr_graph_destroy is void -- it is the top of the tree and has nobody to
     * report to -- but it must still not free what it cannot retire. */
    AprGraph        *g = NULL;
    AprCaptureConfig cfg;
    AprSourceId      id = 0;
    AprSource       *s;
    RingBuf         *rb;
    uint64_t         pos_a, pos_b;

    ASSERT_OK(apr_graph_create(48000, 2, &g));
    fake_cfg(&cfg);
    ASSERT_OK(apr_graph_add_source(g, L"wedged", &cfg, &id));
    ASSERT_OK(apr_graph_arm(g));

    s  = apr_graph_source(g, id);
    rb = apr_source_ring(s);
    ASSERT_OK(apr_capture_fake_wedge(apr_source_capture(s)));

    apr_graph_destroy(g);

    pos_a = rb_write_pos(rb);
    let_it_run();
    pos_b = rb_write_pos(rb);
    ASSERT_GT_INT((long long)pos_a, (long long)pos_b);

    ASSERT_OK(apr_capture_fake_unwedge(apr_source_capture(s)));
    ASSERT_OK(apr_source_destroy(s));
}

TEST(wedging_a_source_with_no_thread_is_refused_rather_than_ignored)
{
    AprCaptureConfig cfg;
    AprSource *s = NULL;
    AprErr     e;

    fake_cfg(&cfg);
    ASSERT_OK(apr_source_create(1, L"driven", &cfg, &s));

    e = apr_capture_fake_wedge(apr_source_capture(s));   /* never started */
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_STATE, e.kind);

    ASSERT_OK(apr_source_destroy(s));
}

/* ==========================================================================
 * m27 -- apr_wasapi_call must not lose a race with apr_wasapi_start
 *
 * The window: apr_wasapi_call read `running` OUTSIDE the lock, so start()
 * could raise it and take the lock first. The job was then posted into a queue
 * the thread was no longer reading, and post() waits INFINITE -- by design,
 * because an accepted job always completes. The result is a hang, not a
 * refusal.
 *
 * The hook fires at exactly that instant. No WASAPI, no client, no audio: the
 * capture thread here is only ever the job-servicing thread, which is all the
 * race needs.
 * ======================================================================= */

typedef struct RaceState {
    AprWasapiStream *s;
    volatile LONG    job_ran;
} RaceState;

static AprErr race_job(void *user)
{
    InterlockedIncrement(&((RaceState *)user)->job_ran);
    return apr_ok();
}

static void race_hook(AprWasapiStream *s, void *user)
{
    RaceState *rs = (RaceState *)user;
    if (rs->s == s) InterlockedExchange(&s->running, 1);   /* start() wins */
}

TEST(a_job_is_refused_when_start_wins_the_race_not_posted_into_a_dead_queue)
{
    AprWasapiStream s;
    AprCapStatus    st;
    RaceState       rs;
    AprErr          e;

    memset(&st, 0, sizeof(st));
    apr_wasapi_stream_init(&s, NULL, &st, 48000, 2, 0, NULL, NULL);
    e = apr_wasapi_thread_start(&s);
    ASSERT_FALSE(apr_failed(&e));

    rs.s = &s;
    rs.job_ran = 0;
    apr_wasapi_test_set_race_hook(race_hook, &rs);

    e = apr_wasapi_call(&s, race_job, &rs);

    apr_wasapi_test_set_race_hook(NULL, NULL);

    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_STATE, e.kind);
    ASSERT_EQ_INT(0, rs.job_ran);

    InterlockedExchange(&s.running, 0);
    ASSERT_OK(apr_wasapi_close(&s, NULL, NULL));
}

TEST(a_job_still_runs_when_nothing_is_racing_it)
{
    /* The refusal above must be the race, not the mechanism being broken. */
    AprWasapiStream s;
    AprCapStatus    st;
    RaceState       rs;
    AprErr          e;

    memset(&st, 0, sizeof(st));
    apr_wasapi_stream_init(&s, NULL, &st, 48000, 2, 0, NULL, NULL);
    ASSERT_OK(apr_wasapi_thread_start(&s));

    rs.s = &s;
    rs.job_ran = 0;
    e = apr_wasapi_call(&s, race_job, &rs);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(1, rs.job_ran);

    ASSERT_OK(apr_wasapi_close(&s, NULL, NULL));
}

/* ==========================================================================
 * M3 -- mute polling must not run on the pump thread
 *
 * Needs a live audio engine, so it skips rather than fails where there is
 * none (design section 11). The tap is on this test's own PID: its tree
 * renders nothing, so what is captured is the engine's own silence.
 * ======================================================================= */

TEST(mute_polling_does_not_run_on_the_pump_thread)
{
    RingBuf         *rb  = NULL;
    AprCapture      *c   = NULL;
    AprCaptureConfig cfg;
    AprProcProbe     probe;
    AprErr           e;

    memset(&cfg, 0, sizeof(cfg));
    cfg.kind        = APR_SRC_PROCESS;
    cfg.sample_rate = 48000;
    cfg.channels    = 2;
    cfg.process.pid = GetCurrentProcessId();

    ASSERT_OK(rb_create(48000, 2 * sizeof(float), &rb));

    e = apr_capture_create(&cfg, rb, &c);
    if (apr_failed(&e)) {
        wchar_t buf[512];
        apr_err_format(&e, buf, 512);
        printf("      SKIPPED: no process-loopback capture here\n");
        printf("      reason: %ls\n", buf);
        if (c) (void)apr_capture_destroy(c);
        rb_destroy(rb);
        return;
    }

    e = c->vt->start(c);
    if (apr_failed(&e)) {
        printf("      SKIPPED: the tap would not start\n");
        (void)apr_capture_destroy(c);
        rb_destroy(rb);
        return;
    }

    /* Two mute polls, so the poller has certainly run whether or not it got a
     * session on the first try -- WAITED FOR rather than slept through. It
     * used to be a flat 1.2 s, which is both longer than the healthy case
     * needs and, on a machine where the session takes longer to appear,
     * shorter than the unhealthy one does. */
    {
        int polled;
        /* BOUNDED: on a machine with no session to find this never becomes
         * true, and the assertions below are what report that -- so it must
         * not spend a minute getting there. Five seconds is four times the
         * flat 1.2 s this replaced. */
        APR_WAIT_UNTIL_MS(polled,
                          apr_capture_process_probe(c, &probe) != 0 &&
                          probe.mute_polls >= 2 && probe.mute_thread_id != 0,
                          5000);
        (void)polled;
    }

    ASSERT_TRUE(apr_capture_process_probe(c, &probe) != 0);
    ASSERT_NE_INT(0, (long long)probe.pump_thread_id);

    /* RED before the fix: the poller WAS the pump, so these two were equal and
     * an unbounded COM enumeration sat between two audio packets on the source
     * that is the reference timeline. */
    ASSERT_NE_INT(0, (long long)probe.mute_thread_id);
    ASSERT_NE_INT((long long)probe.pump_thread_id, (long long)probe.mute_thread_id);
    ASSERT_GT_INT(0, (long long)probe.mute_polls);

    c->vt->stop(c);
    ASSERT_OK(apr_capture_destroy(c));
    rb_destroy(rb);
}
