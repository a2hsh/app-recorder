/*
 * reconnect.c -- going and looking for a source that died.
 *
 * See include/reconnect.h for why this is a thread of its own, why the
 * identity rules are borrowed from session.h rather than written again, and
 * why an EXCLUDE source is held down rather than left running.
 *
 * The shape of this file is: one slot per watched source holding its identity
 * and its retry schedule, one pass (apr_reconnect_step) that is a pure
 * function of the clock and the machine, and a thread whose entire body is
 * that pass in a loop. Everything interesting is in the pass, which is why the
 * pass takes a tick value instead of reading the clock -- a retry schedule
 * that has to hold up over three hours is only provable if a test can step it.
 */
#include "reconnect.h"

#include <stdlib.h>
#include <string.h>

#include <objbase.h>

#include "clock.h"
#include "discover.h"
#include "log.h"

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

typedef struct Slot {
    AprSourceId  id;
    int          watched;
    int          exclude;        /* the privacy case; see reconnect.h */

    AprSessionSource ident;      /* what this source IS, in session.h's terms */
    int              have_ident;

    volatile LONG   state;       /* AprLinkState */
    volatile LONG   attempts;
    volatile LONG   recoveries;
    volatile LONG   backoff_ms;
    volatile LONG64 next_due;    /* tick of the next search */

    uint32_t last_pid;           /* the pid currently being captured/excluded */
} Slot;

struct AprReconnector {
    AprGraph *graph;
    AprReconnectOptions opt;

    Slot   slot[APR_MAX_SOURCES];
    size_t slot_count;
    size_t watched_count;

    const AprSessionMachine *machine;   /* NULL = the live one */

    /* Reused across passes: both of these are a few hundred kilobytes of fixed
     * arrays (session.h says to give an AprSession static storage, and this is
     * that), and a search must not be able to fail on an allocation. */
    AprSession              *probe;
    AprSessionResolveReport *report;

    HANDLE thread;
    HANDLE stop_ev;
    volatile LONG running;
    volatile LONG64 searches;
};

static int64_t load64(const volatile LONG64 *p)
{
    return (int64_t)InterlockedCompareExchange64((volatile LONG64 *)p, 0, 0);
}

static int load32(const volatile LONG *p)
{
    return (int)InterlockedCompareExchange((volatile LONG *)p, 0, 0);
}

static uint64_t ticks_for_ms(uint32_t ms)
{
    return apr_mul_div_u64(ms, apr_qpc_freq(), 1000u, NULL);
}

/* THE SOURCE IS LOOKED UP BY ID EVERY PASS, never cached. A cached pointer
 * would be a dangling one the moment a source was removed between the
 * reconnector being built and the run starting -- and that is a use-after-free
 * on a worker thread, i.e. a crash with a stack trace pointing somewhere else.
 * The lookup is a scan of at most APR_MAX_SOURCES ids, forty times a second. */
static AprSource *slot_source(AprReconnector *rc, const Slot *k)
{
    return apr_graph_source(rc->graph, k->id);
}

static Slot *slot_for(AprReconnector *rc, AprSourceId id)
{
    size_t i;
    for (i = 0; i < rc->slot_count; i++)
        if (rc->slot[i].id == id) return &rc->slot[i];
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Identity
 *
 * Captured while the source is still running, because an image path cannot be
 * read off a process that has already exited -- which is exactly the moment it
 * is needed. session.h owns both the fields and the act of filling them in
 * (AGENTS.md rule 3); nothing here decides what a source's identity is.
 * ------------------------------------------------------------------------- */

static void describe(Slot *k, const AprCaptureConfig *cfg, const wchar_t *name)
{
    AprErr e;

    memset(&k->ident, 0, sizeof k->ident);
    lstrcpynW(k->ident.name, name ? name : L"", (int)APR_NAME_CCH);

    switch (cfg->kind) {
    case APR_SRC_PROCESS:
        k->ident.kind = cfg->process.exclude ? APR_SESSION_SRC_SYSTEM_MINUS_TREE
                                             : APR_SESSION_SRC_PROCESS;
        e = apr_session_describe_process(&k->ident, cfg->process.pid);
        k->have_ident = !apr_failed(&e);
        if (!k->have_ident) {
            APR_WARN(L"no identity for the process behind source \"%ls\"; it "
                     L"cannot be reconnected if it exits", k->ident.name);
        }
        break;

    case APR_SRC_DEVICE:
        k->ident.kind = APR_SESSION_SRC_DEVICE;
        e = apr_session_describe_device(&k->ident, cfg->device.endpoint_id);
        /* A missing FRIENDLY NAME is not a missing identity: the endpoint id
         * alone still matches the same device coming back on the same port,
         * which is the common case. Only the "new port, new GUID" recovery
         * needs the name. */
        if (apr_failed(&e) && cfg->device.endpoint_id) {
            lstrcpynW(k->ident.endpoint_id, cfg->device.endpoint_id,
                      (int)APR_DISC_ENDPOINT_CCH);
        }
        k->have_ident = k->ident.endpoint_id[0] != L'\0';
        break;

    case APR_SRC_FAKE:
    default:
        k->ident.kind = APR_SESSION_SRC_FAKE;
        k->have_ident = 1;
        break;
    }

    /* THE PID IS NOT PART OF THE ANSWER, and this line is the reason the whole
     * search is honest. It is the identity of the instance that just died: a
     * successor has a different one, and matching on it would only ever fire
     * for a recycled number. Everything else -- exe, image path, window class
     * -- describes the APPLICATION, and an application is what came back. */
    k->ident.pid = 0;
}

/* ---------------------------------------------------------------------------
 * Health, read without disturbing anybody
 *
 * Deliberately NOT apr_source_poll: that refreshes the source's cached status
 * from the mixer's thread every tick, and two writers into one struct is the
 * kind of race that shows up once a fortnight. capture.h promises status()
 * never blocks, never allocates and is callable from any thread, so this asks
 * the capture directly and touches nothing the mixer owns.
 * ------------------------------------------------------------------------- */
static int target_is_alive(AprSource *s)
{
    AprCapture      *c = apr_source_capture(s);
    AprCaptureStatus st;

    if (!c) return 0;                     /* detached: waiting on us */
    c->vt->status(c, &st);
    return st.alive != 0;
}

/* An EXCLUDE capture never fails and never reports a death -- the process it
 * names is the one thing it is NOT recording, so WASAPI has nothing to say
 * about it. The staleness of the EXCLUSION is what matters, and that is a
 * question about a pid, answered without COM and without enumeration. */
static int exclusion_is_stale(const Slot *k, const AprReconnector *rc)
{
    if (k->last_pid == 0) return 0;
    if (rc->machine && rc->machine->process_exists)
        return !rc->machine->process_exists(rc->machine->user, k->last_pid);
    return !apr_process_exists(k->last_pid);
}

/* ---------------------------------------------------------------------------
 * The search
 *
 * One resolution per pass, covering every source that is down, because the
 * expensive half is a question about the MACHINE and asking it once serves all
 * of them.
 * ------------------------------------------------------------------------- */

static void run_search(AprReconnector *rc, uint64_t now)
{
    AprSessionResolveOptions opt;
    size_t i, n = 0;
    size_t map[APR_MAX_SOURCES];

    if (!rc->probe || !rc->report) return;

    memset(&opt, 0, sizeof opt);

    /* CONSENT IS ALREADY GIVEN, and only for the exclusion that is already
     * running. An EXCLUDE source exists in this graph at all only because
     * somebody typed --allow-system-capture or clicked a confirmation
     * (session.h); re-establishing the SAME exclusion on the SAME application
     * is not a widening of that scope, and refusing here would leave the
     * machine recorded with nothing held back, which is the outcome the
     * consent gate exists to prevent. */
    opt.allow_system_capture = 1;

    /* Read per source from the report below; the aggregate return is not the
     * question this module is asking. */
    opt.allow_missing = 1;

    /* NEVER. Two instances of one application are told apart by window class
     * or not at all, and picking one at random mid-recording attaches to a
     * different conversation. Staying down is the safe half. */
    opt.pick_when_ambiguous = 0;

    apr_session_init(rc->probe);
    for (i = 0; i < rc->slot_count && n < APR_MAX_SOURCES; i++) {
        Slot *k = &rc->slot[i];
        int st = load32(&k->state);

        if (!k->watched || !k->have_ident) continue;
        if (st != APR_LINK_SEARCHING && st != APR_LINK_HELD &&
            st != APR_LINK_REFUSED) continue;
        if ((uint64_t)load64(&k->next_due) > now) continue;

        rc->probe->sources[n] = k->ident;
        map[n] = i;
        n++;
    }
    if (n == 0) return;

    rc->probe->source_count = n;
    InterlockedIncrement64(&rc->searches);

    if (rc->machine)
        (void)apr_session_resolve_against(rc->probe, &opt, rc->machine, rc->report);
    else
        (void)apr_session_resolve(rc->probe, &opt, rc->report);

    for (i = 0; i < rc->report->count && i < n; i++) {
        const AprSessionResolution *r  = &rc->report->items[i];
        AprSessionSource           *ps = &rc->probe->sources[r->source_index];
        Slot *k = &rc->slot[map[r->source_index]];
        AprSource *src;
        AprCaptureConfig cfg;
        AprErr e;
        int    refused = (r->status == APR_SESSION_AMBIGUOUS);
        uint32_t ceiling = k->exclude ? APR_RECONNECT_MAX_HELD_MS
                                      : APR_RECONNECT_MAX_MS;

        InterlockedIncrement(&k->attempts);

        if (!ps->resolved) {
            InterlockedExchange(&k->state,
                                refused ? APR_LINK_REFUSED
                                        : (k->exclude ? APR_LINK_HELD
                                                      : APR_LINK_SEARCHING));
            goto backoff;
        }

        /* Identity, not fate. What is carried over is everything that says
         * WHAT this source records; what is replaced is the one field that
         * named the INSTANCE that died.
         *
         * Switched on the CAPTURE's kind and not the identity's, because it is
         * the capture kind that says which member of that union is the live
         * one. Writing a pid into a device config would be reinterpreting an
         * endpoint pointer as an integer. */
        src = slot_source(rc, k);
        if (!src) continue;
        cfg = *apr_source_config(src);
        switch (cfg.kind) {
        case APR_SRC_DEVICE:
            cfg.device.endpoint_id = ps->resolved_endpoint_id;
            break;
        case APR_SRC_PROCESS:
            cfg.process.pid = ps->resolved_pid;
            break;
        case APR_SRC_FAKE:
        default:
            /* A synthetic source's health schedule describes the fate of ONE
             * instance, exactly as a pid does. A replacement starts healthy,
             * or the reconnection it is standing in for could never be
             * observed at all. */
            cfg.fake.start_dead      = 0;
            cfg.fake.die_at_frame    = 0;
            cfg.fake.revive_at_frame = 0;
            break;
        }

        if (rc->opt.dry_run) {
            /* Decided, published, nothing touched -- and decided ONCE: the
             * source is still down (nothing was reattached), so leaving it
             * watched would rediscover the same loss on the next pass and
             * reset the very schedule a dry run exists to measure. */
            InterlockedExchange(&k->state, APR_LINK_LIVE);
            InterlockedIncrement(&k->recoveries);
            InterlockedExchange64(&k->next_due, 0);
            k->last_pid = ps->resolved_pid;
            k->watched  = 0;
            continue;
        }

        e = apr_source_reattach(src, &cfg);
        if (apr_failed(&e)) {
            /* The target is there and we could not take it -- a permission
             * change, an endpoint that vanished again between the enumeration
             * and the activation. Treat it exactly like not finding it: keep
             * looking, keep the ring aligned. */
            APR_LOG_ERR(APR_LOG_WARN, &e);
            InterlockedExchange(&k->state,
                                k->exclude ? APR_LINK_HELD : APR_LINK_SEARCHING);
            goto backoff;
        }

        k->last_pid = ps->resolved_pid;
        InterlockedExchange(&k->state, APR_LINK_LIVE);
        InterlockedIncrement(&k->recoveries);
        APR_DEBUG(L"source \"%ls\" reconnected after %u attempt(s)",
                  k->ident.name, (unsigned)load32(&k->attempts));
        InterlockedExchange(&k->attempts, 0);
        InterlockedExchange(&k->backoff_ms, 0);
        InterlockedExchange64(&k->next_due, 0);
        continue;

    backoff:
        {
            uint32_t ms = (uint32_t)load32(&k->backoff_ms);
            ms = ms ? ms * 2u : APR_RECONNECT_FIRST_MS;
            if (ms > ceiling) ms = ceiling;
            InterlockedExchange(&k->backoff_ms, (LONG)ms);
            InterlockedExchange64(&k->next_due, (LONG64)(now + ticks_for_ms(ms)));
        }
    }
}

/* ---------------------------------------------------------------------------
 * One pass
 * ------------------------------------------------------------------------- */

void apr_reconnect_step(AprReconnector *rc, uint64_t now_ticks)
{
    size_t i;
    int    any_due = 0;

    if (!rc) return;

    for (i = 0; i < rc->slot_count; i++) {
        Slot      *k = &rc->slot[i];
        AprSource *src;
        int        st;

        if (!k->watched) continue;
        src = slot_source(rc, k);
        if (!src) continue;
        st = load32(&k->state);

        if (st == APR_LINK_LIVE) {
            int down = k->exclude ? exclusion_is_stale(k, rc)
                                  : !target_is_alive(src);
            if (!down) continue;

            /* DETACHED AT ONCE, both kinds, and for two different reasons.
             *
             * An ordinary dead source: process loopback would go on emitting
             * silence for ever and a dead device pump writes nothing at all,
             * so tearing it down and padding the ring ourselves is the one
             * behaviour that is the same for both -- and it frees the thread
             * and the COM references of a capture that will never produce
             * another useful frame.
             *
             * An EXCLUDE source: this is the privacy hold. Its capture is
             * still perfectly healthy and is recording the machine with an
             * exclusion that no longer applies to anything. See reconnect.h. */
            if (!rc->opt.dry_run) {
                AprErr e = apr_source_detach(src);
                if (apr_failed(&e)) {
                    /* The pump would not join. Nothing may attach to that ring
                     * and nothing may pad it either; try again next pass. */
                    APR_LOG_ERR(APR_LOG_WARN, &e);
                    continue;
                }
            }
            InterlockedExchange(&k->state,
                                k->exclude ? APR_LINK_HELD : APR_LINK_SEARCHING);
            InterlockedExchange(&k->attempts, 0);
            InterlockedExchange(&k->backoff_ms, (LONG)APR_RECONNECT_FIRST_MS);
            InterlockedExchange64(&k->next_due,
                (LONG64)(now_ticks + ticks_for_ms(APR_RECONNECT_FIRST_MS)));

            APR_WARN(L"source \"%ls\" is down; searching for it", k->ident.name);
            continue;
        }

        /* Down. Hold the ring at the frame index that is due, so that the
         * absence is a hole of exactly the right size and every controller
         * downstream keeps seeing a source that is merely silent. */
        (void)apr_source_pad_to(src, now_ticks);

        if ((uint64_t)load64(&k->next_due) <= now_ticks) any_due = 1;
    }

    if (any_due) run_search(rc, now_ticks);
}

/* ---------------------------------------------------------------------------
 * The worker
 * ------------------------------------------------------------------------- */

static DWORD WINAPI reconnect_thread(LPVOID param)
{
    AprReconnector *rc = (AprReconnector *)param;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    /* Its own apartment, exactly like the mute poller M3 created: everything
     * it enumerates is created, used and released here. discover.c is happy in
     * either apartment and initialises COM around its own work anyway, so a
     * refusal here is survivable and worth a line rather than a failure. */
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        APR_WARN(L"the reconnect worker could not enter the MTA (0x%08lx); "
                 L"searching from the caller's apartment instead",
                 (unsigned long)hr);
    }

    for (;;) {
        apr_reconnect_step(rc, apr_qpc_now());
        if (WaitForSingleObject(rc->stop_ev, APR_RECONNECT_PAD_MS) ==
            WAIT_OBJECT_0)
            break;
    }

    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}

AprErr apr_reconnect_start(AprReconnector *rc)
{
    if (!rc) return APR_ERR(APR_E_INVALID_ARG, L"apr_reconnect_start: NULL");
    /* Nothing to watch: no thread, and no apology for it. A graph of synthetic
     * sources has nothing that can be unplugged. */
    if (rc->watched_count == 0) return apr_ok();
    if (InterlockedCompareExchange(&rc->running, 1, 0) != 0) return apr_ok();

    ResetEvent(rc->stop_ev);
    rc->thread = CreateThread(NULL, 0, reconnect_thread, rc, 0, NULL);
    if (!rc->thread) {
        InterlockedExchange(&rc->running, 0);
        return APR_ERR_LAST(L"could not start the reconnect worker");
    }
    return apr_ok();
}

void apr_reconnect_stop(AprReconnector *rc)
{
    if (!rc || !rc->thread) return;
    InterlockedExchange(&rc->running, 0);
    SetEvent(rc->stop_ev);
    /* An unbounded wait, and it is the right one: this thread's longest block
     * is one COM enumeration, and abandoning it would leave a second producer
     * loose on a ring the graph is about to stop and free. Slow beats
     * corrupt. */
    WaitForSingleObject(rc->thread, INFINITE);
    CloseHandle(rc->thread);
    rc->thread = NULL;
}

/* ---------------------------------------------------------------------------
 * Lifetime
 * ------------------------------------------------------------------------- */

AprErr apr_reconnect_create(AprGraph *g, const AprReconnectOptions *opt,
                            AprReconnector **out)
{
    AprReconnector *rc;
    size_t i, n;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"apr_reconnect_create: out");
    *out = NULL;
    if (!g) return APR_ERR(APR_E_INVALID_ARG, L"apr_reconnect_create: no graph");

    rc = (AprReconnector *)calloc(1, sizeof *rc);
    if (!rc) return APR_ERR(APR_E_NO_MEMORY, L"reconnector");

    rc->graph  = g;
    if (opt) rc->opt = *opt;

    rc->stop_ev = CreateEventW(NULL, TRUE /* manual */, FALSE, NULL);
    if (!rc->stop_ev) {
        AprErr e = APR_ERR_LAST(L"CreateEvent for the reconnect worker");
        apr_reconnect_destroy(rc);
        return e;
    }

    n = apr_graph_source_count(g);
    for (i = 0; i < n && i < APR_MAX_SOURCES; i++) {
        AprSource *s = apr_graph_source_at(g, i);
        const AprCaptureConfig *cfg;
        Slot *k;

        if (!s) continue;
        cfg = apr_source_config(s);
        if (!cfg) continue;

        k = &rc->slot[rc->slot_count++];
        k->id    = apr_source_id(s);
        k->state = APR_LINK_OFF;

        if (cfg->kind == APR_SRC_PROCESS && cfg->process.exclude) {
            /* NOT subject to `disabled`. Holding an exclusion whose target has
             * gone is a privacy guarantee, and a convenience switch does not
             * get to turn one of those off (reconnect.h). */
            k->watched  = 1;
            k->exclude  = 1;
            k->last_pid = cfg->process.pid;
        } else if (rc->opt.disabled) {
            k->watched = 0;
        } else if (cfg->kind == APR_SRC_FAKE) {
            k->watched = rc->opt.synthetic ? 1 : 0;
        } else {
            k->watched  = 1;
            k->last_pid = (cfg->kind == APR_SRC_PROCESS) ? cfg->process.pid : 0;
        }

        if (!k->watched) continue;
        rc->watched_count++;
        k->state = APR_LINK_LIVE;
        describe(k, cfg, apr_source_name(s));
    }

    /* The scratch is a few hundred kilobytes of fixed arrays, so it is bought
     * only when there is something to search for -- which a graph of synthetic
     * sources, i.e. most of the test suite, never has. Bought HERE and not at
     * the moment of need, because a source that has just died is the worst
     * possible time to discover there is no memory to look for it with. */
    if (rc->watched_count) {
        rc->probe  = (AprSession *)calloc(1, sizeof *rc->probe);
        rc->report = (AprSessionResolveReport *)calloc(1, sizeof *rc->report);
        if (!rc->probe || !rc->report) {
            AprErr e = APR_ERR(APR_E_NO_MEMORY, L"reconnect scratch");
            apr_reconnect_destroy(rc);
            return e;
        }
    }

    *out = rc;
    return apr_ok();
}

void apr_reconnect_destroy(AprReconnector *rc)
{
    if (!rc) return;
    apr_reconnect_stop(rc);
    if (rc->stop_ev) CloseHandle(rc->stop_ev);
    free(rc->probe);
    free(rc->report);
    free(rc);
}

AprErr apr_reconnect_set_identity(AprReconnector *rc, AprSourceId id,
                                  const AprSessionSource *ident)
{
    Slot *k;

    if (!rc || !ident)
        return APR_ERR(APR_E_INVALID_ARG, L"apr_reconnect_set_identity");
    k = slot_for(rc, id);
    if (!k) return APR_ERR(APR_E_NOT_FOUND, L"source %u is not watched", id);

    k->ident = *ident;

    /* `pid` on the way IN means "the instance this source is on right now",
     * which is the one fact an exclusion has to watch. On the way OUT it is
     * cleared, because it is never part of matching: see describe(). */
    if (ident->pid) k->last_pid = ident->pid;
    k->ident.pid      = 0;
    k->ident.resolved = 0;
    k->have_ident = 1;

    /* The identity is the authority on WHAT a source is, so it settles the two
     * things that follow from that: an exclusion is watched whether or not
     * ordinary reconnection was switched off, because holding one is a privacy
     * guarantee rather than a convenience (reconnect.h). */
    k->exclude = (ident->kind == APR_SESSION_SRC_SYSTEM_MINUS_TREE) ? 1 : 0;
    if (k->exclude && !k->watched) {
        k->watched = 1;
        rc->watched_count++;
        InterlockedExchange(&k->state, APR_LINK_LIVE);
    }
    if (k->watched && !rc->probe) {
        rc->probe  = (AprSession *)calloc(1, sizeof *rc->probe);
        rc->report = (AprSessionResolveReport *)calloc(1, sizeof *rc->report);
        if (!rc->probe || !rc->report)
            return APR_ERR(APR_E_NO_MEMORY, L"reconnect scratch");
    }
    return apr_ok();
}

void apr_reconnect_set_machine(AprReconnector *rc, const AprSessionMachine *m)
{
    if (rc) rc->machine = m;
}

/* ---------------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------------- */

AprLinkState apr_reconnect_link(const AprReconnector *rc, AprSourceId id)
{
    const Slot *k = rc ? slot_for((AprReconnector *)rc, id) : NULL;
    return k ? (AprLinkState)load32(&k->state) : APR_LINK_OFF;
}

uint32_t apr_reconnect_recoveries(const AprReconnector *rc, AprSourceId id)
{
    const Slot *k = rc ? slot_for((AprReconnector *)rc, id) : NULL;
    return k ? (uint32_t)load32(&k->recoveries) : 0;
}

uint32_t apr_reconnect_attempts(const AprReconnector *rc, AprSourceId id)
{
    const Slot *k = rc ? slot_for((AprReconnector *)rc, id) : NULL;
    return k ? (uint32_t)load32(&k->attempts) : 0;
}

uint32_t apr_reconnect_backoff_ms(const AprReconnector *rc, AprSourceId id)
{
    const Slot *k = rc ? slot_for((AprReconnector *)rc, id) : NULL;
    return k ? (uint32_t)load32(&k->backoff_ms) : 0;
}

uint64_t apr_reconnect_next_due(const AprReconnector *rc, AprSourceId id)
{
    const Slot *k = rc ? slot_for((AprReconnector *)rc, id) : NULL;
    return k ? (uint64_t)load64(&k->next_due) : 0;
}

uint32_t apr_reconnect_target_pid(const AprReconnector *rc, AprSourceId id)
{
    const Slot *k = rc ? slot_for((AprReconnector *)rc, id) : NULL;
    return k ? k->last_pid : 0;
}

uint64_t apr_reconnect_searches(const AprReconnector *rc)
{
    return rc ? (uint64_t)load64(&rc->searches) : 0;
}
