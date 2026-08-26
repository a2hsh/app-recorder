/*
 * test_run_loop.c -- the shared recording loop (core/runner.c).
 *
 * ===========================================================================
 * WHY THIS SUITE EXISTS SEPARATELY FROM test_cli.c
 *
 *   The loop used to live inside src/cli/cli.c, and tests/test_cli.c is what
 *   proves the extraction into core/runner.c was faithful: the same command
 *   lines still produce the same files and the same sentences in the same
 *   order. That suite is the regression net.
 *
 *   This one is the other half. It drives the runner DIRECTLY, which is how
 *   the UI drives it, and asserts the three properties the UI depends on and
 *   the command line never exercises:
 *
 *     - the loop runs off the caller's thread and can be stopped from another;
 *     - health notices arrive, once each, for the two failures that look
 *       exactly like success (a muted source and a dead one);
 *     - destroying a runner mid-recording FINALIZES rather than abandons.
 *
 * ===========================================================================
 * SAFETY (AGENTS.md rule 1)
 *
 *   Every source here is APR_SRC_FAKE. Nothing in this file opens an audio
 *   endpoint, and nothing renders a single sample to any output device. The
 *   fake synthesises into a ring buffer and the recording goes to a temporary
 *   file that each case deletes.
 */
#include "test_runner.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "action.h"
#include "clock.h"
#include "graph.h"
#include "runner.h"
#include "strings.h"

/* MSVC in C mode: an assignment is not an lvalue, so the &(e = f(...)) idiom
 * does not compile. This is the same assertion without the trick. */
#define ASSERT_OK(call) do { AprErr e_ = (call); ASSERT_FALSE(apr_failed(&e_)); } while (0)

/* ==========================================================================
 * Scaffolding
 * ======================================================================== */

static void tmp_path(wchar_t *buf, size_t cch, const wchar_t *tag)
{
    wchar_t dir[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, dir);
    static LONG counter;

    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(buf, cch, _TRUNCATE, L"%lsapr_run_%ls_%lu_%ld.wav",
                 dir, tag, GetCurrentProcessId(),
                 InterlockedIncrement(&counter));
}

/* A WAV that opens: RIFF/WAVE, a data chunk, and a data size that is not a
 * lie. This is the property "finalize leaves a playable file" reduces to for
 * the reference encoder. */
static int wav_is_playable(const wchar_t *path, uint32_t *out_data_bytes)
{
    HANDLE h;
    unsigned char buf[512];
    DWORD got = 0;
    size_t i;

    if (out_data_bytes) *out_data_bytes = 0;
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
            if (out_data_bytes) *out_data_bytes = sz;
            return 1;
        }
    }
    return 0;
}

/* One fake source, one bus, one WAV. The whole product in four calls. */
typedef struct Fixture {
    AprGraph *g;
    AprBusId  bus;
    wchar_t   path[MAX_PATH];
} Fixture;

static int fixture_up(Fixture *f, const wchar_t *tag,
                      const AprCaptureConfig *src_cfg)
{
    AprCaptureConfig cfg;
    AprActionConfig  acfg;
    AprSourceId      sid = 0;
    AprErr           e;

    memset(f, 0, sizeof *f);
    tmp_path(f->path, MAX_PATH, tag);

    e = apr_graph_create(48000, 2, &f->g);
    if (apr_failed(&e)) return 0;

    if (src_cfg) {
        cfg = *src_cfg;
    } else {
        memset(&cfg, 0, sizeof cfg);
        cfg.kind = APR_SRC_FAKE;
        cfg.fake.tone_hz   = 440;
        cfg.fake.amplitude = 0.25f;
    }
    cfg.kind = APR_SRC_FAKE;

    e = apr_graph_add_source(f->g, L"synthetic", &cfg, &sid);
    if (apr_failed(&e)) return 0;

    e = apr_graph_add_bus(f->g, L"Mix", &f->bus);
    if (apr_failed(&e)) return 0;

    e = apr_graph_connect(f->g, sid, f->bus, 1.0f);
    if (apr_failed(&e)) return 0;

    memset(&acfg, 0, sizeof acfg);
    acfg.out_path    = f->path;
    acfg.sample_rate = 48000;
    acfg.channels    = 2;
    e = apr_graph_add_action(f->g, f->bus, "wav", &acfg);
    if (apr_failed(&e)) return 0;

    return 1;
}

static void fixture_down(Fixture *f)
{
    apr_graph_destroy(f->g);
    f->g = NULL;
    DeleteFileW(f->path);
}

/* Counts every notice by event, which is how "reported once" becomes a number
 * a test can assert rather than an impression. */
typedef struct Tally {
    int  count[16];
    wchar_t last_name[APR_NAME_CCH];
    LONG order;
    int  started_before_first_tick;
} Tally;

static void tally(void *user, const AprRunNotice *n)
{
    Tally *t = (Tally *)user;
    if ((int)n->ev >= 0 && (int)n->ev < 16) t->count[n->ev]++;
    if (n->name[0]) lstrcpynW(t->last_name, n->name, APR_NAME_CCH);
}

/* ==========================================================================
 * The ordinary case
 * ======================================================================== */

TEST(a_short_recording_writes_a_playable_file)
{
    Fixture f;
    AprRunnerConfig cfg;
    AprRunner *r = NULL;
    Tally t;
    uint32_t data = 0;
    AprErr e;

    ASSERT_TRUE(fixture_up(&f, L"basic", NULL));
    memset(&t, 0, sizeof t);
    memset(&cfg, 0, sizeof cfg);
    cfg.graph = f.g;
    cfg.duration_ms = 400;
    cfg.observer = tally;
    cfg.user = &t;

    e = apr_runner_create(&cfg, &r);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_runner_run(r);
    ASSERT_FALSE(apr_failed(&e));

    ASSERT_TRUE(wav_is_playable(f.path, &data));
    ASSERT_GT_INT(0, (int)data);

    /* STARTED once, FINISHING once, STOPPED once. The front ends hang their
     * "recording to..." and "finishing..." lines on exactly these, so a
     * duplicate would print the list twice. */
    ASSERT_EQ_INT(1, t.count[APR_RUN_EV_STARTED]);
    ASSERT_EQ_INT(1, t.count[APR_RUN_EV_FINISHING]);
    ASSERT_EQ_INT(1, t.count[APR_RUN_EV_STOPPED]);
    ASSERT_FALSE(apr_runner_incomplete(r));

    apr_runner_destroy(r);
    fixture_down(&f);
}

/* The mixer runs APR_BUS_LOOKBEHIND_MS behind wall clock, so a loop that
 * stopped at the instant the duration expired would throw away the last block.
 * The loop waits for it, renders it, and only then finalizes -- which is
 * exactly the detail a second, copied implementation would drop, and drop
 * silently, because a recording one block short still plays. */
TEST(the_final_block_is_rendered_rather_than_thrown_away)
{
    Fixture f;
    AprRunnerConfig cfg;
    AprRunner *r = NULL;
    uint64_t frames;
    AprErr e;

    ASSERT_TRUE(fixture_up(&f, L"tail", NULL));
    memset(&cfg, 0, sizeof cfg);
    cfg.graph = f.g;
    cfg.duration_ms = 500;

    e = apr_runner_create(&cfg, &r);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_runner_run(r);
    ASSERT_FALSE(apr_failed(&e));

    frames = apr_runner_bus_frames(r, f.bus);
    printf("      %llu frames for a 500 ms run at 48 kHz\n",
           (unsigned long long)frames);

    /* 500 ms is 24,000 frames. Allow the loop's own 10 ms granularity either
     * way, but require substantially more than 500 ms minus the 50 ms
     * lookbehind (22,080) -- which is what a loop that skipped the drain would
     * produce. */
    ASSERT_GT_INT(23000, (int)frames);

    apr_runner_destroy(r);
    fixture_down(&f);
}

TEST(elapsed_time_is_readable_and_advances)
{
    Fixture f;
    AprRunnerConfig cfg;
    AprRunner *r = NULL;
    AprErr e;

    ASSERT_TRUE(fixture_up(&f, L"elapsed", NULL));
    memset(&cfg, 0, sizeof cfg);
    cfg.graph = f.g;
    cfg.duration_ms = 300;

    e = apr_runner_create(&cfg, &r);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(0, (int)apr_runner_elapsed_ms(r));

    e = apr_runner_run(r);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_GT_INT(250, (int)apr_runner_elapsed_ms(r));

    apr_runner_destroy(r);
    fixture_down(&f);
}

/* ==========================================================================
 * Off the caller's thread -- the property the UI exists on
 * ======================================================================== */

TEST(a_recording_runs_on_its_own_thread_and_stops_from_another)
{
    Fixture f;
    AprRunnerConfig cfg;
    AprRunner *r = NULL;
    AprErr e;
    int i;

    ASSERT_TRUE(fixture_up(&f, L"async", NULL));
    memset(&cfg, 0, sizeof cfg);
    cfg.graph = f.g;      /* no duration: runs until stopped */

    e = apr_runner_create(&cfg, &r);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_runner_run_async(r);
    ASSERT_FALSE(apr_failed(&e));

    /* The call returned immediately -- that is the whole point. Wait for the
     * loop to genuinely be running before stopping it, so this is not testing
     * a race. */
    for (i = 0; i < 200 && !apr_runner_running(r); i++) Sleep(5);
    ASSERT_TRUE(apr_runner_running(r));
    Sleep(150);

    apr_runner_request_stop(r);
    ASSERT_TRUE(apr_runner_wait(r, 15000));
    ASSERT_FALSE(apr_runner_running(r));

    ASSERT_TRUE(wav_is_playable(f.path, NULL));

    apr_runner_destroy(r);
    fixture_down(&f);
}

/* An unfinalized action is an unplayable file, so destroying a runner that is
 * still recording must stop it and wait -- never abandon it. This is what
 * makes the window-close path safe by construction rather than by remembering
 * to call something first. */
TEST(destroying_a_running_recorder_finalizes_rather_than_abandoning)
{
    Fixture f;
    AprRunnerConfig cfg;
    AprRunner *r = NULL;
    int i;
    AprErr e;

    ASSERT_TRUE(fixture_up(&f, L"destroy", NULL));
    memset(&cfg, 0, sizeof cfg);
    cfg.graph = f.g;

    e = apr_runner_create(&cfg, &r);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_runner_run_async(r);
    ASSERT_FALSE(apr_failed(&e));
    for (i = 0; i < 200 && !apr_runner_running(r); i++) Sleep(5);
    ASSERT_TRUE(apr_runner_running(r));
    Sleep(120);

    apr_runner_destroy(r);          /* no explicit stop, no explicit wait */
    ASSERT_TRUE(wav_is_playable(f.path, NULL));

    fixture_down(&f);
}

TEST(stopping_a_runner_that_never_ran_is_harmless)
{
    Fixture f;
    AprRunnerConfig cfg;
    AprRunner *r = NULL;
    AprErr e;

    ASSERT_TRUE(fixture_up(&f, L"prestop", NULL));
    memset(&cfg, 0, sizeof cfg);
    cfg.graph = f.g;

    e = apr_runner_create(&cfg, &r);
    ASSERT_FALSE(apr_failed(&e));
    apr_runner_request_stop(r);
    apr_runner_request_stop(r);     /* idempotent */
    ASSERT_FALSE(apr_runner_running(r));
    apr_runner_destroy(r);
    fixture_down(&f);
}

/* ==========================================================================
 * Health -- the two failures that look exactly like success
 *
 * A muted application records digital silence while the engine still reports
 * it rendering, and a process that has exited keeps handing loopback perfect
 * zeros for ever with no error and no flag (design 4.1 #5/#6, both measured).
 * Neither is visible in the file, in an HRESULT, or in a frame count. The only
 * way a user finds out is if the recorder says so.
 * ======================================================================== */

TEST(a_muted_source_is_reported_and_reported_only_once)
{
    Fixture f;
    AprCaptureConfig src;
    AprRunnerConfig cfg;
    AprRunner *r = NULL;
    Tally t;
    AprRunSourceState st;
    AprErr e;

    memset(&src, 0, sizeof src);
    src.kind = APR_SRC_FAKE;
    src.fake.tone_hz     = 440;
    src.fake.amplitude   = 0.25f;
    src.fake.start_muted = 1;

    ASSERT_TRUE(fixture_up(&f, L"muted", &src));
    memset(&t, 0, sizeof t);
    memset(&cfg, 0, sizeof cfg);
    cfg.graph = f.g;
    cfg.duration_ms = 400;
    cfg.observer = tally;
    cfg.user = &t;

    e = apr_runner_create(&cfg, &r);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_runner_run(r);
    ASSERT_FALSE(apr_failed(&e));

    /* ONCE. The loop polls every 10 ms, so a naive implementation would warn
     * forty times in a 400 ms recording, and a warning per tick is a warning
     * nobody reads. */
    ASSERT_EQ_INT(1, t.count[APR_RUN_EV_SOURCE_MUTED]);
    ASSERT_WSTR_EQ(L"synthetic", t.last_name);

    /* And the snapshot says so too, which is what a UI reads rather than
     * reaching into the graph from its own thread. */
    ASSERT_TRUE(apr_runner_source_state(r, 0, &st));
    ASSERT_TRUE(st.muted);
    ASSERT_TRUE(st.alive);
    ASSERT_WSTR_EQ(L"synthetic", st.name);

    /* Muted is a warning, not a failure: the recording is exactly what was
     * asked for, it is simply silent. */
    ASSERT_FALSE(apr_runner_incomplete(r));

    apr_runner_destroy(r);
    fixture_down(&f);
}

TEST(a_source_that_dies_is_reported_and_the_run_is_incomplete)
{
    Fixture f;
    AprCaptureConfig src;
    AprRunnerConfig cfg;
    AprRunner *r = NULL;
    Tally t;
    AprRunSourceState st;
    AprErr e;

    memset(&src, 0, sizeof src);
    src.kind = APR_SRC_FAKE;
    src.fake.tone_hz      = 440;
    src.fake.amplitude    = 0.25f;
    src.fake.die_at_frame = 4800;   /* 100 ms in */

    ASSERT_TRUE(fixture_up(&f, L"died", &src));
    memset(&t, 0, sizeof t);
    memset(&cfg, 0, sizeof cfg);
    cfg.graph = f.g;
    cfg.duration_ms = 500;
    cfg.observer = tally;
    cfg.user = &t;

    e = apr_runner_create(&cfg, &r);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_runner_run(r);
    ASSERT_FALSE(apr_failed(&e));

    ASSERT_EQ_INT(1, t.count[APR_RUN_EV_SOURCE_DIED]);
    ASSERT_TRUE(apr_runner_source_state(r, 0, &st));
    ASSERT_FALSE(st.alive);

    /* UNLIKE muted: the session is not what was asked for any more, so the
     * command line exits 6 and the UI says so out loud. */
    ASSERT_TRUE(apr_runner_incomplete(r));

    /* The file is still playable. A source dying is not a reason to lose what
     * was captured before it did. */
    ASSERT_TRUE(wav_is_playable(f.path, NULL));

    apr_runner_destroy(r);
    fixture_down(&f);
}

/* ==========================================================================
 * The snapshot
 * ======================================================================== */

TEST(the_snapshot_describes_the_graph_without_touching_it)
{
    Fixture f;
    AprRunnerConfig cfg;
    AprRunner *r = NULL;
    AprRunSourceState st;
    AprErr e;

    ASSERT_TRUE(fixture_up(&f, L"snapshot", NULL));
    memset(&cfg, 0, sizeof cfg);
    cfg.graph = f.g;

    e = apr_runner_create(&cfg, &r);
    ASSERT_FALSE(apr_failed(&e));

    /* Taken at create, so it is answerable before the first tick and from any
     * thread -- which is the whole reason it is a copy and not a pointer. */
    ASSERT_EQ_INT(1, (int)apr_runner_source_count(r));
    ASSERT_EQ_INT(1, (int)apr_runner_bus_count(r));
    ASSERT_EQ_INT((int)f.bus, (int)apr_runner_bus_id_at(r, 0));

    ASSERT_TRUE(apr_runner_source_state(r, 0, &st));
    ASSERT_WSTR_EQ(L"synthetic", st.name);
    ASSERT_TRUE(st.alive);
    ASSERT_FALSE(st.muted);

    ASSERT_FALSE(apr_runner_source_state(r, 99, &st));

    apr_runner_destroy(r);
    fixture_down(&f);
}

TEST(a_runner_with_no_graph_is_refused_rather_than_crashing)
{
    AprRunnerConfig cfg;
    AprRunner *r = (AprRunner *)(void *)1;
    AprErr e;

    memset(&cfg, 0, sizeof cfg);
    e = apr_runner_create(&cfg, &r);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(r);

    /* And every accessor tolerates NULL, because a front end that failed to
     * create one still has to draw itself. */
    ASSERT_EQ_INT(0, apr_runner_running(NULL));
    ASSERT_EQ_INT(0, apr_runner_incomplete(NULL));
    ASSERT_EQ_INT(0, (int)apr_runner_elapsed_ms(NULL));
    ASSERT_EQ_INT(0, (int)apr_runner_source_count(NULL));
    apr_runner_request_stop(NULL);
    apr_runner_destroy(NULL);
}

/* ==========================================================================
 * m28 -- destroy must wait for a SYNCHRONOUS run too
 *
 * apr_runner_destroy() joined the thread apr_runner_run_async() creates, and
 * that was the whole of it. apr_runner_run() executes the recording on the
 * CALLER's thread, for which the runner holds no handle at all -- so destroy
 * saw nothing to wait for and fell straight through to free(), while a live
 * loop on another thread was still writing into that allocation and had not
 * finalized a single file.
 *
 * It stayed latent because today's callers happen to destroy on the same
 * thread they ran on, which is to say this safety net has never been under
 * load. The test puts it under load.
 * ======================================================================= */

typedef struct SyncRun {
    AprRunner *r;
    HANDLE     started;      /* set once the loop is definitely in flight */
    volatile LONG finished;  /* 1 once apr_runner_run() has RETURNED */
} SyncRun;

static DWORD WINAPI sync_run_thread(void *param)
{
    SyncRun *sr = (SyncRun *)param;
    AprErr   e;

    SetEvent(sr->started);
    e = apr_runner_run(sr->r);      /* the whole recording, on THIS thread */
    (void)e;
    InterlockedExchange(&sr->finished, 1);
    return 0;
}

TEST(destroy_waits_for_a_run_on_someone_elses_thread)
{
    Fixture          f;
    AprRunnerConfig  cfg;
    AprRunner       *r = NULL;
    SyncRun          sr;
    HANDLE           th;
    AprErr           e;

    ASSERT_TRUE(fixture_up(&f, L"m28sync", NULL));

    memset(&cfg, 0, sizeof cfg);
    cfg.graph       = f.g;
    cfg.duration_ms = 400;          /* long enough to still be running below */
    cfg.tick_ms     = 10;
    ASSERT_OK(apr_runner_create(&cfg, &r));

    memset(&sr, 0, sizeof sr);
    sr.r       = r;
    sr.started = CreateEventW(NULL, TRUE, FALSE, NULL);
    ASSERT_NOT_NULL(sr.started);

    th = CreateThread(NULL, 0, sync_run_thread, &sr, 0, NULL);
    ASSERT_NOT_NULL(th);
    ASSERT_EQ_INT(WAIT_OBJECT_0, (long long)WaitForSingleObject(sr.started, 5000));

    /* Wait until the loop is genuinely inside itself, so that destroy really
     * has something to wait for rather than arriving before or after. */
    while (!apr_runner_running(r)) Sleep(1);

    /* RED before the fix: this returned immediately, freed `r`, and left
     * sync_run_thread writing into it. */
    e = apr_runner_destroy(r);
    ASSERT_FALSE(apr_failed(&e));

    /* The ordering assertion. If destroy returned while the loop was still
     * running, this is 0 -- and everything the loop touched afterwards was
     * freed memory. */
    ASSERT_EQ_INT(1, (long long)sr.finished);

    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    CloseHandle(sr.started);
    fixture_down(&f);
}

TEST(destroy_of_a_runner_that_never_ran_is_immediate_and_ok)
{
    Fixture         f;
    AprRunnerConfig cfg;
    AprRunner      *r = NULL;
    AprErr          e;

    ASSERT_TRUE(fixture_up(&f, L"m28idle", NULL));

    memset(&cfg, 0, sizeof cfg);
    cfg.graph   = f.g;
    cfg.tick_ms = 10;
    ASSERT_OK(apr_runner_create(&cfg, &r));

    e = apr_runner_destroy(r);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_runner_destroy(NULL);
    ASSERT_FALSE(apr_failed(&e));

    fixture_down(&f);
}
