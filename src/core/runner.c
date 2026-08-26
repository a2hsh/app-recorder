/*
 * runner.c -- the recording loop. See runner.h for why it is not in cli.c.
 *
 * The shape of this file is: a snapshot of the graph taken at create() so that
 * every query below is answerable without touching the graph from another
 * thread, then one loop, then the accessors that read the snapshot.
 *
 * WHAT IS PUBLISHED ACROSS THREADS AND HOW
 *
 *   The loop thread writes elapsed time, per-source health and per-bus frame
 *   counts; a UI thread reads them while the loop runs. All of them go through
 *   the Interlocked family rather than plain stores, including the 64-bit ones
 *   -- an aligned 64-bit store happens to be atomic on x64, but "happens to
 *   be" is not a thing to build a status line on, and Interlocked also stops
 *   the optimiser hoisting the read out of a UI timer's loop.
 *
 * WHAT IS NOT SHARED
 *
 *   The graph. The loop is its only toucher between apr_runner_run() and the
 *   moment it returns; runner.h says so, and the UI controller enforces it by
 *   disabling every editing command while a recording is under way.
 */
#include "runner.h"

#include <stdlib.h>
#include <string.h>

#include "action.h"
#include "clock.h"
#include "log.h"
#include "strings.h"

/* --------------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */

typedef struct SourceSlot {
    AprSourceId  id;
    wchar_t      name[APR_NAME_CCH];
    volatile LONG alive;
    volatile LONG muted;
    volatile LONG64 frames;
    int          reported_dead;    /* loop thread only */
    int          reported_muted;   /* loop thread only */
} SourceSlot;

typedef struct BusSlot {
    AprBusId        id;
    volatile LONG64 frames;

    /* Loop thread only. One bit per output, so that "this output failed" and
     * "this output was renamed" are each said ONCE. Without them, moving the
     * failure report into the loop -- which is what makes an output that
     * cannot even be created audible at the START of a run -- would repeat the
     * same sentence a hundred times a second. */
    unsigned char reported_failed[APR_MAX_ACTIONS_PER_BUS];
    unsigned char reported_renamed[APR_MAX_ACTIONS_PER_BUS];
} BusSlot;

struct AprRunner {
    AprGraph *graph;
    int64_t   duration_ms;
    unsigned  tick_ms;

    AprRunObserverFn observer;
    void            *user;

    HANDLE stop_event;        /* the one actually waited on */
    int    stop_event_owned;
    HANDLE finished_event;    /* manual reset, set once files are closed */
    HANDLE thread;

    volatile LONG   stop_requested;
    volatile LONG   running;
    volatile LONG   incomplete;
    volatile LONG64 elapsed_ms;

    SourceSlot source[APR_MAX_SOURCES];
    size_t     source_count;
    BusSlot    bus[APR_MAX_BUSES];
    size_t     bus_count;
};

/* --------------------------------------------------------------------------
 * Small atomics helpers. 64-bit reads go through a compare-exchange of zero
 * against zero, which is the documented way to read one atomically.
 * ----------------------------------------------------------------------- */

static int64_t load64(const volatile LONG64 *p)
{
    return (int64_t)InterlockedCompareExchange64((volatile LONG64 *)p, 0, 0);
}

static void store64(volatile LONG64 *p, int64_t v)
{
    InterlockedExchange64(p, (LONG64)v);
}

static int load32(const volatile LONG *p)
{
    return (int)InterlockedCompareExchange((volatile LONG *)p, 0, 0);
}

/* --------------------------------------------------------------------------
 * Notices
 * ----------------------------------------------------------------------- */

static void notice_path(AprRunner *r, AprRunEvent ev, size_t src, size_t bus,
                        size_t action, const wchar_t *name,
                        const wchar_t *path, const AprErr *err)
{
    AprRunNotice n;

    if (!r || !r->observer) return;

    memset(&n, 0, sizeof n);
    n.ev           = ev;
    n.source_index = src;
    n.bus_index    = bus;
    n.action_index = action;
    n.err          = err ? *err : apr_ok();
    if (name) lstrcpynW(n.name, name, (int)APR_NAME_CCH);
    if (path) lstrcpynW(n.path, path, (int)APR_OUT_PATH_CCH);

    r->observer(r->user, &n);
}

static void notice(AprRunner *r, AprRunEvent ev, size_t src, size_t bus,
                   size_t action, const wchar_t *name, const AprErr *err)
{
    notice_path(r, ev, src, bus, action, name, NULL, err);
}

/* --------------------------------------------------------------------------
 * Health
 *
 * Both clauses report ONCE. A source that died at second three has not died
 * again at second four, and a warning per tick is a warning nobody reads.
 * ----------------------------------------------------------------------- */

static void poll_sources(AprRunner *r)
{
    size_t i;

    for (i = 0; i < r->source_count; i++) {
        AprSource *s = apr_graph_source(r->graph, r->source[i].id);
        int alive, muted;

        if (!s) continue;
        apr_source_poll(s, NULL);

        alive = apr_source_alive(s);
        muted = apr_source_muted(s);

        InterlockedExchange(&r->source[i].alive, (LONG)alive);
        InterlockedExchange(&r->source[i].muted, (LONG)muted);
        store64(&r->source[i].frames, (int64_t)apr_source_frames(s));

        /* Loopback keeps emitting perfect silence forever after the target
         * exits and WASAPI never says so (design 4.1 #6). Without this a dead
         * application produces a file that looks like a success. */
        if (!alive && !r->source[i].reported_dead) {
            r->source[i].reported_dead = 1;
            InterlockedExchange(&r->incomplete, 1);
            notice(r, APR_RUN_EV_SOURCE_DIED, i, SIZE_MAX, SIZE_MAX,
                   r->source[i].name, NULL);
        }
        /* DEATH WINS. A source that has exited is not usefully described as
         * muted -- unmuting it in Windows would fix nothing -- and the two
         * are indistinguishable in the audio anyway, both being digital
         * silence at the right rate. Whoever is listening gets the one
         * sentence that is worth acting on, which is the same choice
         * src/ui/tree_panel.c makes for the row a screen reader reads.
         *
         * Only simultaneous states are affected: a source that goes silent at
         * one second and dies at three still says both things, in order. */
        if (muted && alive && !r->source[i].reported_muted) {
            r->source[i].reported_muted = 1;
            notice(r, APR_RUN_EV_SOURCE_MUTED, i, SIZE_MAX, SIZE_MAX,
                   r->source[i].name, NULL);
        }
    }
}

static void sample_bus_frames(AprRunner *r)
{
    size_t i;

    for (i = 0; i < r->bus_count; i++) {
        AprBus *b = apr_graph_bus(r->graph, r->bus[i].id);
        store64(&r->bus[i].frames, b ? (int64_t)apr_bus_frames_out(b) : 0);
    }
}

/* WHAT EACH OUTPUT IS DOING, said once each.
 *
 * Called every tick, not only at the end. An output whose file could not even
 * be created fails inside apr_bus_start(), and reporting that only at
 * apr_graph_stop() would have told the author an hour after it mattered. The
 * per-output bits above are what keep "once each" true at 100 Hz. */
static void poll_actions(AprRunner *r)
{
    size_t bi, k;

    for (bi = 0; bi < r->bus_count; bi++) {
        AprBus *b = apr_graph_bus(r->graph, r->bus[bi].id);
        if (!b) continue;
        for (k = 0; k < apr_bus_action_count(b) &&
                    k < APR_MAX_ACTIONS_PER_BUS; k++) {
            const AprActionVTable *vt = apr_bus_action_at(b, k);
            const wchar_t *who = (vt && vt->display_name_id)
                                   ? apr_str(vt->display_name_id)
                                   : apr_bus_name(b);

            /* The name that was asked for is not the name on disk: the take
             * that was already there has been kept and this one has moved.
             * Said out loud, because the alternative to a surprise here is a
             * lost recording (outpath.h). */
            if (apr_bus_action_renamed(b, k) && !r->bus[bi].reported_renamed[k]) {
                r->bus[bi].reported_renamed[k] = 1;
                notice_path(r, APR_RUN_EV_OUTPUT_RENAMED, SIZE_MAX, bi, k, who,
                            apr_bus_action_current_path(b, k), NULL);
            }

            if (apr_bus_action_failed(b, k) && !r->bus[bi].reported_failed[k]) {
                AprErr e = apr_bus_action_error(b, k);
                r->bus[bi].reported_failed[k] = 1;
                InterlockedExchange(&r->incomplete, 1);
                notice_path(r, APR_RUN_EV_ACTION_FAILED, SIZE_MAX, bi, k, who,
                            apr_bus_action_current_path(b, k), &e);
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * The loop
 * ----------------------------------------------------------------------- */

static int stop_requested(const AprRunner *r)
{
    return load32(&r->stop_requested);
}

AprErr apr_runner_run(AprRunner *r)
{
    uint64_t freq;
    uint64_t start;
    AprErr   e;

    if (!r) return APR_ERR(APR_E_INVALID_ARG, L"apr_runner_run: runner is NULL");
    if (InterlockedCompareExchange(&r->running, 1, 0) != 0) {
        return APR_ERR(APR_E_STATE, L"apr_runner_run: already running");
    }

    ResetEvent(r->finished_event);
    freq = apr_qpc_freq();

    e = apr_graph_start(r->graph, apr_qpc_now());
    if (apr_failed(&e)) {
        /* One source failing to arm does not stop a session (design 10), but
         * it has to be said out loud. */
        InterlockedExchange(&r->incomplete, 1);
        notice(r, APR_RUN_EV_ARM_FAILED, SIZE_MAX, SIZE_MAX, SIZE_MAX, NULL, &e);
    }
    start = apr_qpc_now();

    /* BEFORE "started", not after. The files exist by now -- apr_graph_start()
     * is what created them -- so an output that could not be opened, or one
     * that had to move aside for a take already on disk, is said first and the
     * "recording to ..." line that follows is then true. */
    poll_actions(r);
    notice(r, APR_RUN_EV_STARTED, SIZE_MAX, SIZE_MAX, SIZE_MAX, NULL, NULL);

    for (;;) {
        uint64_t now;
        int64_t  elapsed_ms;
        int      signalled = 0;

        /* THE EVENT ITSELF IS A STOP, not merely a wake-up. The CLI's console
         * control handler is installed before any runner exists and signals a
         * shared event rather than calling us, so a runner that only trusted
         * its own flag would wait on an event that was already set and spin
         * until the duration ran out. Manual-reset, so this cannot be missed
         * between the wait and the test. */
        if (r->stop_event) {
            signalled = WaitForSingleObject(r->stop_event, r->tick_ms)
                            == WAIT_OBJECT_0;
        } else {
            Sleep(r->tick_ms);
        }

        now = apr_qpc_now();
        (void)apr_graph_tick(r->graph, now);
        poll_sources(r);
        poll_actions(r);
        sample_bus_frames(r);

        elapsed_ms = (int64_t)apr_mul_div_u64(now - start, 1000u, freq, NULL);
        store64(&r->elapsed_ms, elapsed_ms);

        if (r->duration_ms && elapsed_ms >= r->duration_ms) break;
        if (signalled || stop_requested(r)) break;
    }

    /* The mixer deliberately runs APR_BUS_LOOKBEHIND_MS behind wall clock
     * (bus.h), so stopping at this instant would throw away the last block.
     * Wait for it to become due, render it, and only then finalize. */
    Sleep(APR_BUS_LOOKBEHIND_MS + 10);
    (void)apr_graph_tick(r->graph, apr_qpc_now());

    sample_bus_frames(r);

    notice(r, APR_RUN_EV_FINISHING, SIZE_MAX, SIZE_MAX, SIZE_MAX, NULL, NULL);

    e = apr_graph_stop(r->graph);
    poll_actions(r);
    if (apr_failed(&e)) InterlockedExchange(&r->incomplete, 1);

    InterlockedExchange(&r->running, 0);
    SetEvent(r->finished_event);

    notice(r, APR_RUN_EV_STOPPED, SIZE_MAX, SIZE_MAX, SIZE_MAX, NULL, NULL);
    return apr_ok();
}

static DWORD WINAPI run_thread(void *param)
{
    AprRunner *r = (AprRunner *)param;
    AprErr e = apr_runner_run(r);

    if (apr_failed(&e)) APR_LOG_ERR(APR_LOG_ERROR, &e);
    return 0;
}

AprErr apr_runner_run_async(AprRunner *r)
{
    if (!r) return APR_ERR(APR_E_INVALID_ARG, L"apr_runner_run_async: NULL");
    if (r->thread) return APR_ERR(APR_E_STATE, L"apr_runner_run_async: already");

    ResetEvent(r->finished_event);
    r->thread = CreateThread(NULL, 0, run_thread, r, 0, NULL);
    if (!r->thread) return APR_ERR_LAST(L"could not start the recording thread");
    return apr_ok();
}

/* --------------------------------------------------------------------------
 * Lifetime
 * ----------------------------------------------------------------------- */

AprErr apr_runner_create(const AprRunnerConfig *cfg, AprRunner **out)
{
    AprRunner *r;
    size_t i, n;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"apr_runner_create: out is NULL");
    *out = NULL;
    if (!cfg || !cfg->graph) {
        return APR_ERR(APR_E_INVALID_ARG, L"apr_runner_create: no graph");
    }

    r = (AprRunner *)calloc(1, sizeof *r);
    if (!r) return APR_ERR(APR_E_NO_MEMORY, L"apr_runner_create: out of memory");

    r->graph       = cfg->graph;
    r->duration_ms = cfg->duration_ms > 0 ? cfg->duration_ms : 0;
    r->tick_ms     = cfg->tick_ms ? cfg->tick_ms : APR_RUNNER_TICK_MS;
    r->observer    = cfg->observer;
    r->user        = cfg->user;

    if (cfg->stop_event) {
        r->stop_event = cfg->stop_event;
    } else {
        r->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        r->stop_event_owned = 1;
    }
    r->finished_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!r->stop_event || !r->finished_event) {
        AprErr e = APR_ERR_LAST(L"could not create the recorder's events");
        apr_runner_destroy(r);
        return e;
    }

    /* Snapshot the shape. Names are copied so that every query below can be
     * answered from another thread without reaching into the graph. */
    n = apr_graph_source_count(cfg->graph);
    for (i = 0; i < n && i < APR_MAX_SOURCES; i++) {
        AprSource *s = apr_graph_source_at(cfg->graph, i);
        if (!s) continue;
        r->source[r->source_count].id = apr_source_id(s);
        lstrcpynW(r->source[r->source_count].name, apr_source_name(s),
                  (int)APR_NAME_CCH);
        r->source[r->source_count].alive = 1;
        r->source_count++;
    }

    n = apr_graph_bus_count(cfg->graph);
    for (i = 0; i < n && i < APR_MAX_BUSES; i++) {
        AprBus *b = apr_graph_bus_at(cfg->graph, i);
        if (!b) continue;
        r->bus[r->bus_count].id = apr_bus_id(b);
        r->bus_count++;
    }

    *out = r;
    return apr_ok();
}

void apr_runner_destroy(AprRunner *r)
{
    if (!r) return;

    /* NEVER abandon a running recording. An action that is not finalized is an
     * unplayable file; a slow destroy is merely slow. */
    apr_runner_request_stop(r);
    if (r->thread) {
        WaitForSingleObject(r->thread, INFINITE);
        CloseHandle(r->thread);
        r->thread = NULL;
    }

    if (r->stop_event && r->stop_event_owned) CloseHandle(r->stop_event);
    if (r->finished_event) CloseHandle(r->finished_event);
    free(r);
}

/* --------------------------------------------------------------------------
 * Control and queries
 * ----------------------------------------------------------------------- */

void apr_runner_request_stop(AprRunner *r)
{
    if (!r) return;
    InterlockedExchange(&r->stop_requested, 1);
    if (r->stop_event) SetEvent(r->stop_event);
}

int apr_runner_wait(AprRunner *r, DWORD ms)
{
    if (!r) return 1;
    if (!r->finished_event) return 1;
    return WaitForSingleObject(r->finished_event, ms) == WAIT_OBJECT_0;
}

HANDLE apr_runner_finished_event(const AprRunner *r)
{
    return r ? r->finished_event : NULL;
}

int apr_runner_running(const AprRunner *r)
{
    return r ? load32(&r->running) : 0;
}

int apr_runner_incomplete(const AprRunner *r)
{
    return r ? load32(&r->incomplete) : 0;
}

int64_t apr_runner_elapsed_ms(const AprRunner *r)
{
    return r ? load64(&r->elapsed_ms) : 0;
}

size_t apr_runner_source_count(const AprRunner *r)
{
    return r ? r->source_count : 0;
}

int apr_runner_source_state(const AprRunner *r, size_t index,
                            AprRunSourceState *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!r || index >= r->source_count) return 0;

    out->id = r->source[index].id;
    lstrcpynW(out->name, r->source[index].name, (int)APR_NAME_CCH);
    out->alive  = load32(&r->source[index].alive);
    out->muted  = load32(&r->source[index].muted);
    out->frames = (uint64_t)load64(&r->source[index].frames);
    return 1;
}

size_t apr_runner_bus_count(const AprRunner *r)
{
    return r ? r->bus_count : 0;
}

AprBusId apr_runner_bus_id_at(const AprRunner *r, size_t index)
{
    if (!r || index >= r->bus_count) return 0;
    return r->bus[index].id;
}

uint64_t apr_runner_bus_frames(const AprRunner *r, AprBusId bus)
{
    size_t i;

    if (!r) return 0;
    for (i = 0; i < r->bus_count; i++) {
        if (r->bus[i].id == bus) return (uint64_t)load64(&r->bus[i].frames);
    }
    return 0;
}
