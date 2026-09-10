/*
 * test_capture_apartment.c -- capture.h's apartment promise, held to.
 *
 * WHY THIS FILE EXISTS
 *
 *   The whole 26-suite run passed while the windowed front end could not add a
 *   single source. Every other test runs its case on the runner's own thread,
 *   which has never called CoInitializeEx, so the capture layer's
 *   CoInitializeEx(NULL, COINIT_MULTITHREADED) always succeeded and the STA
 *   path was never once executed. The GUI's thread is an STA and has to be --
 *   IAccPropServices, which supplies every control's accessible name, is valid
 *   only on the thread that created it -- so the caller got RPC_E_CHANGED_MODE
 *   and "Add Source" failed with "process loopback requires the MTA".
 *
 *   Nothing tested the capture layer from an STA caller. That asymmetry was the
 *   defect; this file is the fix for the asymmetry, capture.h is the fix for the
 *   contract, and src/capture/wasapi_common.c is the fix for the code.
 *
 * WHAT IT ASSERTS
 *
 *   apr_capture_create + start + stop + destroy succeed from a genuine STA
 *   thread, from an explicit MTA thread, and from an uninitialised thread. All
 *   three, because "works in one apartment" is exactly the belief that shipped.
 *
 * NOTHING IN THIS FILE RENDERS AUDIO. See AGENTS.md rule 1.
 *
 * The fake source carries the plumbing, unconditionally and with no hardware.
 * The process tap is additionally exercised against THIS test process's own
 * PID -- its tree renders nothing, so there is nothing to hear and nothing to
 * capture but the engine's own silence -- and it skips rather than fails where
 * there is no audio engine.
 *
 * NO DEVICE SOURCE IS EVER STARTED HERE. Starting one records the author's
 * microphone. Device coverage stops at open/close, which reads no samples at
 * all: WASAPI hands over nothing until Start.
 */
#include "test_runner.h"
#include "test_wait.h"
#include "test_engine.h"

#include "capture.h"
#include "ringbuf.h"

#include <windows.h>
#include <objbase.h>
#include <string.h>

/* ==========================================================================
 * One capture, opened, started, stopped and destroyed -- on whatever thread
 * and in whatever apartment the caller asks for.
 * ======================================================================== */

typedef struct Attempt {
    /* in */
    DWORD            co_init;      /* COINIT_* flag, or NO_COM below */
    AprCaptureConfig cfg;
    int              do_start;

    /* out -- read only after the thread is joined (test_runner.h) */
    HRESULT hr_com;
    AprErr  open_err;
    AprErr  start_err;
    int     opened;
    int     started;
    uint64_t frames;
} Attempt;

#define NO_COM 0xFFFFFFFFu

static DWORD WINAPI attempt_thread(LPVOID param)
{
    Attempt *a = (Attempt *)param;
    RingBuf *rb = NULL;
    AprCapture *c = NULL;
    AprCaptureStatus st;
    AprErr e;

    a->hr_com = S_OK;
    if (a->co_init != NO_COM) {
        a->hr_com = CoInitializeEx(NULL, (DWORD)a->co_init);
        if (FAILED(a->hr_com)) return 1;
    }

    e = rb_create(48000, (size_t)a->cfg.channels * sizeof(float), &rb);
    if (apr_failed(&e)) { a->open_err = e; goto done; }

    a->open_err = apr_capture_create(&a->cfg, rb, &c);
    if (apr_failed(&a->open_err)) goto done;
    a->opened = 1;

    if (a->do_start) {
        a->start_err = c->vt->start(c);
        if (!apr_failed(&a->start_err)) {
            a->started = 1;
            /* WAIT for the engine's first packet; do not sleep a guess at how
             * long it takes. This used to be Sleep(120) and it is the whole
             * reason this file failed on its first CI runner: a cold engine
             * took about a second to deliver the first frame there, while the
             * NEXT case in this file -- by then warm -- passed the identical
             * assertion. See test_engine.h. */
            a->frames = apr_test_wait_for_frames(c,
                                                 APR_TEST_ENGINE_FIRST_FRAME_MS);
            c->vt->stop(c);
        }
    }

    c->vt->status(c, &st);
    if (st.frames_written > a->frames) a->frames = st.frames_written;

done:
    if (c)  apr_capture_destroy(c);
    if (rb) rb_destroy(rb);
    if (a->co_init != NO_COM && SUCCEEDED(a->hr_com)) CoUninitialize();
    return 0;
}

/* Runs the attempt on a thread of its own and joins it. Returns 0 if the thread
 * could not be created or did not finish -- never leaves it running, because a
 * thread still inside WASAPI when the case returns would fail the NEXT case. */
static int run_attempt(Attempt *a)
{
    HANDLE th = CreateThread(NULL, 0, attempt_thread, a, 0, NULL);
    if (!th) return 0;
    if (WaitForSingleObject(th, 30000) != WAIT_OBJECT_0) {
        printf("      WARNING: attempt thread did not finish\n");
        CloseHandle(th);
        return 0;
    }
    CloseHandle(th);
    return 1;
}

static void fake_config(AprCaptureConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->kind        = APR_SRC_FAKE;
    cfg->sample_rate = 48000;
    cfg->channels    = 2;
    cfg->fake.tone_hz   = 440;
    cfg->fake.amplitude = 0.25f;   /* synthetic samples; nothing renders them */
}

static void process_config(AprCaptureConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->kind        = APR_SRC_PROCESS;
    cfg->sample_rate = 48000;
    cfg->channels    = 2;
    cfg->process.pid = (uint32_t)GetCurrentProcessId();
    cfg->process.exclude = 0;      /* INCLUDE: this process tree only */
}

static void show(const char *what, const AprErr *e)
{
    wchar_t buf[512];
    apr_err_format(e, buf, 512);
    printf("      %s: %ls\n", what, buf);
}

/* ==========================================================================
 * "Can this machine tap a process at all?" -- ASKED WITHOUT AN STA IN THE
 * PICTURE, and asked all the way to a frame.
 *
 * TWO WAYS THIS PREDICATE CAN BE WRONG, and this file has now met both.
 *
 *   ASK IT FROM AN STA and an apartment refusal -- the exact regression these
 *   cases exist to catch -- comes back as "no engine here", the case skips,
 *   and the bug ships behind a green suite. That is how the original defect
 *   survived in the first place, so the reference attempt runs with NO_COM:
 *   the apartment the old code was already happy with.
 *
 *   STOP AT open() and the predicate answers a question nobody asked.
 *   Activation succeeding says the virtual loopback device exists; it does not
 *   say an engine will ever feed it. On the GitHub runner that failed this
 *   file, open() succeeded and there is no capture endpoint on the machine at
 *   all. So the reference runs the whole way -- open, start, first frame --
 *   and only then is "this machine can do it" a claim worth resting an
 *   assertion on.
 *
 * Returns 1 if the reference tap produced audio; 0 after having SKIPPED,
 * naming which of the three steps failed.
 * ======================================================================== */
static int process_loopback_works_here(void)
{
    Attempt ref;

    memset(&ref, 0, sizeof ref);
    ref.co_init  = NO_COM;      /* NOT an STA. See above. */
    ref.do_start = 1;
    process_config(&ref.cfg);

    if (!run_attempt(&ref)) {
        SKIP("the reference capture thread did not finish");
        return 0;
    }
    if (apr_failed(&ref.open_err)) {
        apr_test_skip_capture("no process-loopback activation on this machine",
                              &ref.open_err);
        return 0;
    }
    if (apr_failed(&ref.start_err)) {
        apr_test_skip_capture("process loopback would not start on this machine",
                              &ref.start_err);
        return 0;
    }
    if (ref.frames == 0) {
        apr_test_skip_capture("process loopback activates here but no audio "
                              "engine ever feeds it", NULL);
        return 0;
    }
    return 1;
}

/* ==========================================================================
 * The fake source: no hardware, so these are unconditional.
 * ======================================================================== */

TEST(a_fake_source_opens_and_runs_from_an_sta_thread)
{
    Attempt a;
    memset(&a, 0, sizeof a);
    a.co_init  = COINIT_APARTMENTTHREADED;
    a.do_start = 1;
    fake_config(&a.cfg);

    ASSERT_TRUE(run_attempt(&a));
    ASSERT_EQ_INT(0, (int)FAILED(a.hr_com));
    if (apr_failed(&a.open_err)) show("open", &a.open_err);
    ASSERT_FALSE(apr_failed(&a.open_err));
    ASSERT_EQ_INT(1, a.opened);
    if (apr_failed(&a.start_err)) show("start", &a.start_err);
    ASSERT_FALSE(apr_failed(&a.start_err));
    ASSERT_EQ_INT(1, a.started);
    ASSERT_TRUE(a.frames > 0);
}

TEST(a_fake_source_opens_and_runs_from_an_mta_thread)
{
    Attempt a;
    memset(&a, 0, sizeof a);
    a.co_init  = COINIT_MULTITHREADED;
    a.do_start = 1;
    fake_config(&a.cfg);

    ASSERT_TRUE(run_attempt(&a));
    ASSERT_FALSE(apr_failed(&a.open_err));
    ASSERT_FALSE(apr_failed(&a.start_err));
    ASSERT_TRUE(a.frames > 0);
}

TEST(a_fake_source_opens_and_runs_with_no_apartment_at_all)
{
    Attempt a;
    memset(&a, 0, sizeof a);
    a.co_init  = NO_COM;
    a.do_start = 1;
    fake_config(&a.cfg);

    ASSERT_TRUE(run_attempt(&a));
    ASSERT_FALSE(apr_failed(&a.open_err));
    ASSERT_FALSE(apr_failed(&a.start_err));
    ASSERT_TRUE(a.frames > 0);
}

/* ==========================================================================
 * The real thing. THIS is the case that was failing in the user's hands: a
 * process tap opened from the window's own STA thread.
 * ======================================================================== */

TEST(a_process_tap_opens_and_runs_from_an_sta_thread)
{
    Attempt sta;

    /* First establish that this machine can tap a process at all, all the way
     * to a frame, from a thread the old code was happy with. If it cannot,
     * there is no engine here and the STA result would prove nothing. */
    if (!process_loopback_works_here()) return;

    memset(&sta, 0, sizeof sta);
    sta.co_init  = COINIT_APARTMENTTHREADED;
    sta.do_start = 1;
    process_config(&sta.cfg);

    ASSERT_TRUE(run_attempt(&sta));
    ASSERT_EQ_INT(0, (int)FAILED(sta.hr_com));

    /* The regression, spelled out: this used to be APR_E_STATE, "process
     * loopback requires the MTA; this thread is an STA". */
    if (apr_failed(&sta.open_err)) show("open", &sta.open_err);
    ASSERT_FALSE(apr_failed(&sta.open_err));
    ASSERT_EQ_INT(1, sta.opened);

    if (apr_failed(&sta.start_err)) show("start", &sta.start_err);
    ASSERT_FALSE(apr_failed(&sta.start_err));
    ASSERT_EQ_INT(1, sta.started);

    /* And it is not merely open: the engine is really feeding it. Nothing in
     * this process tree renders, so every one of those frames is silence. */
    ASSERT_TRUE(sta.frames > 0);
}

TEST(a_process_tap_stopped_and_closed_from_an_sta_thread_leaves_nothing_behind)
{
    /* open/start/stop/close all on one STA thread, twice over. The old code
     * additionally required open() and close() to share a thread because they
     * balanced a CoInitializeEx; that requirement is gone, and running the
     * whole cycle twice in a row is the cheap proof the apartment is being
     * torn down cleanly each time rather than leaked. */
    Attempt a, b;

    /* "No audio engine here" must be established WITHOUT an STA in the
     * picture, or an apartment refusal -- the very regression under test --
     * would present itself as a skip and pass silently. That is precisely how
     * this defect survived a green suite in the first place. */
    if (!process_loopback_works_here()) return;

    memset(&a, 0, sizeof a);
    a.co_init  = COINIT_APARTMENTTHREADED;
    a.do_start = 1;
    process_config(&a.cfg);
    ASSERT_TRUE(run_attempt(&a));
    if (apr_failed(&a.open_err)) show("open (first cycle)", &a.open_err);
    ASSERT_FALSE(apr_failed(&a.open_err));
    ASSERT_FALSE(apr_failed(&a.start_err));

    memset(&b, 0, sizeof b);
    b.co_init  = COINIT_APARTMENTTHREADED;
    b.do_start = 1;
    process_config(&b.cfg);
    ASSERT_TRUE(run_attempt(&b));
    if (apr_failed(&b.open_err)) show("open (second cycle)", &b.open_err);
    ASSERT_FALSE(apr_failed(&b.open_err));
    ASSERT_FALSE(apr_failed(&b.start_err));
    ASSERT_TRUE(b.frames > 0);
}

/* ==========================================================================
 * The device path had the identical refusal. Opened and closed only -- never
 * started; see the file comment.
 * ======================================================================== */

TEST(a_device_source_opens_from_an_sta_thread)
{
    Attempt ref, sta;

    memset(&ref, 0, sizeof ref);
    ref.co_init = NO_COM;
    memset(&ref.cfg, 0, sizeof ref.cfg);
    ref.cfg.kind        = APR_SRC_DEVICE;
    ref.cfg.sample_rate = 48000;
    ref.cfg.channels    = 2;
    ref.cfg.device.endpoint_id = NULL;   /* the default */
    ASSERT_TRUE(run_attempt(&ref));
    if (apr_failed(&ref.open_err)) {
        /* Open is as far as this case ever goes -- starting a device capture
         * records the author's microphone -- so open succeeding IS the whole
         * capability here, and there is nothing further to establish. */
        apr_test_skip_capture("no capture endpoint on this machine",
                              &ref.open_err);
        return;
    }

    sta = ref;
    memset(&sta.open_err, 0, sizeof sta.open_err);
    sta.co_init  = COINIT_APARTMENTTHREADED;
    sta.do_start = 0;                    /* NEVER. This is a microphone. */
    sta.opened   = 0;
    sta.started  = 0;
    sta.frames   = 0;

    ASSERT_TRUE(run_attempt(&sta));
    if (apr_failed(&sta.open_err)) show("open", &sta.open_err);
    ASSERT_FALSE(apr_failed(&sta.open_err));
    ASSERT_EQ_INT(1, sta.opened);
    ASSERT_EQ_INT(0, sta.started);
    ASSERT_EQ_U64(0u, sta.frames);       /* nothing is read before Start */
}
