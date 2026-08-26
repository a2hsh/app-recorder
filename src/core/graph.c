/*
 * graph.c -- the model. See include/graph.h for why edges are first-class and
 * why both UI views read this rather than each other.
 */
#include "graph.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "mix.h"
#include "strings.h"

struct AprGraph {
    uint32_t rate;
    uint16_t channels;

    AprSource  *sources[APR_MAX_SOURCES];
    size_t      source_count;
    AprSourceId next_source_id;

    AprBus  *buses[APR_MAX_BUSES];
    size_t   bus_count;
    AprBusId next_bus_id;

    int running;
};

/* ---------------------------------------------------------------------------
 * Lifetime
 * ------------------------------------------------------------------------- */

AprErr apr_graph_create(uint32_t sample_rate, uint16_t channels, AprGraph **out)
{
    AprGraph *g;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"graph with no out slot");
    *out = NULL;
    if (sample_rate == 0 || channels == 0 || channels > APR_MAX_CHANNELS) {
        return APR_ERR(APR_E_INVALID_ARG, L"session format %u Hz / %u channels",
                       sample_rate, channels);
    }

    g = (AprGraph *)calloc(1, sizeof *g);
    if (!g) return APR_ERR(APR_E_NO_MEMORY, L"graph state");

    g->rate           = sample_rate;
    g->channels       = channels;
    g->next_source_id = 1;    /* 0 is "no source", so ids start at 1 */
    g->next_bus_id    = 1;
    *out = g;
    return apr_ok();
}

void apr_graph_destroy(AprGraph *g)
{
    size_t i;

    if (!g) return;
    apr_graph_stop(g);
    /* Buses first: each holds readers into sources and must give them back
     * before the sources go. */
    for (i = 0; i < g->bus_count; i++)    apr_bus_destroy(g->buses[i]);
    for (i = 0; i < g->source_count; i++) {
        /* A source whose capture thread would not stop leaks itself and its
         * ring, deliberately (source.h). Nothing above this point needs to
         * change its behaviour -- the graph is going away either way -- but
         * the log has to carry it, because a silently leaked audio thread is
         * how the next mystery starts. */
        AprErr e = apr_source_destroy(g->sources[i]);
        if (apr_failed(&e)) APR_LOG_ERR(APR_LOG_ERROR, &e);
    }
    free(g);
}

uint32_t apr_graph_rate(const AprGraph *g)     { return g ? g->rate : 0; }
uint16_t apr_graph_channels(const AprGraph *g) { return g ? g->channels : 0; }
int      apr_graph_running(const AprGraph *g)  { return g ? g->running : 0; }

/* ---------------------------------------------------------------------------
 * THE SHAPE IS FROZEN WHILE THE GRAPH RUNS
 *
 * graph.h has always said "do not mutate the shape while a tick is in flight",
 * and saying it was all it did. The runner's loop thread walks g->buses and
 * each bus walks its edge array; a UI thread rewriting either one underneath
 * it is a corrupted mix or a crash, in the middle of a recording, with the
 * files already open.
 *
 * The front end disables its editing commands during a recording, and that is
 * necessary and not sufficient: it is not the only caller (the CLI builds
 * graphs, sessions load into them, tests drive them), and at least one UI path
 * -- Ctrl+Shift+E disconnect -- reaches the canvas as a raw keystroke that
 * never passes the controller's busy() check at all. A guard that lives only
 * in one caller is a guard with a hole in it.
 *
 * REFUSED, NOT IGNORED, and refused with APR_E_BUSY specifically. A silent
 * no-op is the worst of the three outcomes for a screen-reader user: the
 * keystroke does nothing, says nothing, and is indistinguishable from a broken
 * app. APR_E_BUSY is distinguishable from every other APR_E_STATE so a caller
 * can answer it with "that cannot be changed while a recording is running"
 * rather than a generic failure.
 * ------------------------------------------------------------------------- */
static AprErr refuse_while_running(const AprGraph *g, const wchar_t *what)
{
    if (!g->running) return apr_ok();
    return APR_ERR(APR_E_BUSY, L"%ls while this graph is recording", what);
}

/* ---------------------------------------------------------------------------
 * Nodes
 * ------------------------------------------------------------------------- */

AprErr apr_graph_add_source(AprGraph *g, const wchar_t *name,
                            const AprCaptureConfig *cfg, AprSourceId *out_id)
{
    AprCaptureConfig local;
    AprSource       *s = NULL;
    AprErr           e;

    if (out_id) *out_id = 0;
    if (!g || !cfg) return APR_ERR(APR_E_INVALID_ARG, L"graph or config is null");
    e = refuse_while_running(g, L"a source cannot be added");
    if (apr_failed(&e)) return e;
    if (g->source_count >= APR_MAX_SOURCES) {
        /* A LIMIT IS ONE OF THE FEW REFUSALS A USER CAN ACT ON, so it names
         * its own sentence rather than being flattened into "wrong state"
         * at the point of display (err.h, APR_ERR_SAY). The format string
         * stays what it was: diagnostic, English, for the log. */
        return APR_ERR_SAY(APR_E_STATE, APR_S_ERR_REASON_TOO_MANY_SOURCES,
                           L"graph already holds %d sources", APR_MAX_SOURCES);
    }

    /* Format is a session decision, not a per-source one: a process-loopback
     * client cannot be asked what it wants (design 4.1). */
    local              = *cfg;
    local.sample_rate  = g->rate;
    local.channels     = g->channels;

    e = apr_source_create(g->next_source_id, name, &local, &s);
    if (apr_failed(&e)) return e;

    g->sources[g->source_count++] = s;
    if (out_id) *out_id = g->next_source_id;
    g->next_source_id++;
    return apr_ok();
}

static size_t source_index(const AprGraph *g, AprSourceId id)
{
    size_t i;
    for (i = 0; i < g->source_count; i++) {
        if (apr_source_id(g->sources[i]) == id) return i;
    }
    return (size_t)-1;
}

static size_t bus_index(const AprGraph *g, AprBusId id)
{
    size_t i;
    for (i = 0; i < g->bus_count; i++) {
        if (apr_bus_id(g->buses[i]) == id) return i;
    }
    return (size_t)-1;
}

AprSource *apr_graph_source(const AprGraph *g, AprSourceId id)
{
    size_t i;
    if (!g) return NULL;
    i = source_index(g, id);
    return i == (size_t)-1 ? NULL : g->sources[i];
}

size_t apr_graph_source_count(const AprGraph *g) { return g ? g->source_count : 0; }

AprSource *apr_graph_source_at(const AprGraph *g, size_t index)
{
    return (g && index < g->source_count) ? g->sources[index] : NULL;
}

AprErr apr_graph_remove_source(AprGraph *g, AprSourceId id)
{
    AprErr e;
    size_t i, k;

    if (!g) return APR_ERR(APR_E_INVALID_ARG, L"null graph");
    e = refuse_while_running(g, L"a source cannot be removed");
    if (apr_failed(&e)) return e;
    i = source_index(g, id);
    if (i == (size_t)-1) return APR_ERR(APR_E_NOT_FOUND, L"no source %u", id);

    /* Every edge goes first: a bus left holding a reader into a freed source
     * is the one lifetime bug this shape is meant to make impossible. */
    for (k = 0; k < g->bus_count; k++) {
        if (apr_bus_has_source(g->buses[k], id)) apr_bus_remove_source(g->buses[k], id);
    }

    /* The slot is released whether or not the source could be retired: a
     * source that leaks itself and its ring (source.h) is out of the graph
     * either way, and leaving a pointer to it here would only mean trying to
     * free it a second time at apr_graph_destroy. The error still travels, so
     * the caller can say a source is still running rather than pretend. */
    e = apr_source_destroy(g->sources[i]);
    for (; i + 1 < g->source_count; i++) g->sources[i] = g->sources[i + 1];
    g->source_count--;
    return e;
}

AprErr apr_graph_add_bus(AprGraph *g, const wchar_t *name, AprBusId *out_id)
{
    AprBus *b = NULL;
    AprErr  e;

    if (out_id) *out_id = 0;
    if (!g) return APR_ERR(APR_E_INVALID_ARG, L"null graph");
    e = refuse_while_running(g, L"a bus cannot be added");
    if (apr_failed(&e)) return e;
    if (g->bus_count >= APR_MAX_BUSES) {
        return APR_ERR_SAY(APR_E_STATE, APR_S_ERR_REASON_TOO_MANY_BUSES,
                           L"graph already holds %d buses", APR_MAX_BUSES);
    }

    e = apr_bus_create(g->next_bus_id, name, g->rate, g->channels, &b);
    if (apr_failed(&e)) return e;

    g->buses[g->bus_count++] = b;
    if (out_id) *out_id = g->next_bus_id;
    g->next_bus_id++;
    return apr_ok();
}

AprBus *apr_graph_bus(const AprGraph *g, AprBusId id)
{
    size_t i;
    if (!g) return NULL;
    i = bus_index(g, id);
    return i == (size_t)-1 ? NULL : g->buses[i];
}

size_t apr_graph_bus_count(const AprGraph *g) { return g ? g->bus_count : 0; }

AprBus *apr_graph_bus_at(const AprGraph *g, size_t index)
{
    return (g && index < g->bus_count) ? g->buses[index] : NULL;
}

AprErr apr_graph_remove_bus(AprGraph *g, AprBusId id)
{
    AprErr e;
    size_t i;

    if (!g) return APR_ERR(APR_E_INVALID_ARG, L"null graph");
    e = refuse_while_running(g, L"a bus cannot be removed");
    if (apr_failed(&e)) return e;
    i = bus_index(g, id);
    if (i == (size_t)-1) return APR_ERR(APR_E_NOT_FOUND, L"no bus %u", id);

    apr_bus_destroy(g->buses[i]);   /* finalizes actions, closes every reader */
    for (; i + 1 < g->bus_count; i++) g->buses[i] = g->buses[i + 1];
    g->bus_count--;
    return apr_ok();
}

/* ---------------------------------------------------------------------------
 * Edges
 * ------------------------------------------------------------------------- */

AprErr apr_graph_connect(AprGraph *g, AprSourceId source, AprBusId bus, float gain)
{
    AprSource *s;
    AprBus    *b;
    AprErr     e;

    if (!g) return APR_ERR(APR_E_INVALID_ARG, L"null graph");
    e = refuse_while_running(g, L"an edge cannot be connected");
    if (apr_failed(&e)) return e;
    s = apr_graph_source(g, source);
    b = apr_graph_bus(g, bus);
    if (!s) return APR_ERR(APR_E_NOT_FOUND, L"no source %u", source);
    if (!b) return APR_ERR(APR_E_NOT_FOUND, L"no bus %u", bus);
    return apr_bus_add_source(b, s, gain);
}

AprErr apr_graph_disconnect(AprGraph *g, AprSourceId source, AprBusId bus)
{
    AprBus *b;
    AprErr  e;

    if (!g) return APR_ERR(APR_E_INVALID_ARG, L"null graph");
    e = refuse_while_running(g, L"an edge cannot be disconnected");
    if (apr_failed(&e)) return e;
    b = apr_graph_bus(g, bus);
    if (!b) return APR_ERR(APR_E_NOT_FOUND, L"no bus %u", bus);
    return apr_bus_remove_source(b, source);
}

int apr_graph_connected(const AprGraph *g, AprSourceId source, AprBusId bus)
{
    const AprBus *b = apr_graph_bus(g, bus);
    return b && apr_bus_has_source(b, source);
}

size_t apr_graph_edge_count(const AprGraph *g)
{
    size_t i, n = 0;
    if (!g) return 0;
    for (i = 0; i < g->bus_count; i++) n += apr_bus_source_count(g->buses[i]);
    return n;
}

/* ---------------------------------------------------------------------------
 * Projections
 * ------------------------------------------------------------------------- */

size_t apr_graph_buses_for_source(const AprGraph *g, AprSourceId source,
                                  AprBusId *out, size_t cap)
{
    size_t i, n = 0;

    if (!g) return 0;
    for (i = 0; i < g->bus_count; i++) {
        if (!apr_bus_has_source(g->buses[i], source)) continue;
        if (out && n < cap) out[n] = apr_bus_id(g->buses[i]);
        n++;
    }
    return n;
}

size_t apr_graph_sources_for_bus(const AprGraph *g, AprBusId bus,
                                 AprSourceId *out, size_t cap)
{
    const AprBus *b = apr_graph_bus(g, bus);
    size_t        i, n;

    if (!b) return 0;
    n = apr_bus_source_count(b);
    for (i = 0; i < n && out && i < cap; i++) {
        out[i] = apr_source_id(apr_bus_source_at(b, i));
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * Actions
 * ------------------------------------------------------------------------- */

AprErr apr_graph_add_action(AprGraph *g, AprBusId bus, const char *action_id,
                            const AprActionConfig *cfg)
{
    const AprActionVTable *vt;
    AprActionConfig        local;
    AprBus                *b;
    AprErr                 e;

    if (!g || !cfg) return APR_ERR(APR_E_INVALID_ARG, L"graph or config is null");
    e = refuse_while_running(g, L"an output cannot be added");
    if (apr_failed(&e)) return e;
    b = apr_graph_bus(g, bus);
    if (!b) return APR_ERR(APR_E_NOT_FOUND, L"no bus %u", bus);

    vt = apr_action_find(action_id);
    if (!vt) {
        return APR_ERR(APR_E_NOT_FOUND, L"no action registered as \"%hs\"",
                       action_id ? action_id : "(null)");
    }

    local              = *cfg;
    local.sample_rate  = g->rate;
    local.channels     = g->channels;
    return apr_bus_add_action(b, vt, &local);
}

/* ---------------------------------------------------------------------------
 * Running
 * ------------------------------------------------------------------------- */

AprErr apr_graph_arm(AprGraph *g)
{
    AprErr first = apr_ok();
    size_t i;

    if (!g) return APR_ERR(APR_E_INVALID_ARG, L"null graph");

    /* A source that will not start is reported but does not stop the session:
     * it simply never anchors, and the buses reading it mix silence for it. */
    for (i = 0; i < g->source_count; i++) {
        AprErr e = apr_source_start(g->sources[i]);
        if (apr_failed(&e)) {
            APR_LOG_ERR(APR_LOG_ERROR, &e);
            if (!apr_failed(&first)) first = e;
        }
    }
    return first;
}

AprErr apr_graph_run(AprGraph *g, uint64_t start_ticks)
{
    size_t i;

    if (!g) return APR_ERR(APR_E_INVALID_ARG, L"null graph");
    if (g->running) return apr_ok();

    /* One anchor for every bus. */
    for (i = 0; i < g->bus_count; i++) apr_bus_start(g->buses[i], start_ticks);
    g->running = 1;
    return apr_ok();
}

AprErr apr_graph_start(AprGraph *g, uint64_t start_ticks)
{
    AprErr armed;

    if (!g) return APR_ERR(APR_E_INVALID_ARG, L"null graph");
    if (g->running) return apr_ok();

    /* Sources first, so their rings are already filling before any bus asks. */
    armed = apr_graph_arm(g);
    apr_graph_run(g, start_ticks);
    return armed;
}

AprErr apr_graph_tick(AprGraph *g, uint64_t now_ticks)
{
    AprErr first = apr_ok();
    size_t i;

    if (!g) return APR_ERR(APR_E_INVALID_ARG, L"null graph");
    if (!g->running) return APR_ERR(APR_E_STATE, L"graph is not running");

    for (i = 0; i < g->bus_count; i++) {
        AprErr e = apr_bus_tick(g->buses[i], now_ticks);
        if (apr_failed(&e) && !apr_failed(&first)) first = e;
    }
    return first;
}

AprErr apr_graph_stop(AprGraph *g)
{
    AprErr first = apr_ok();
    size_t i;

    if (!g) return APR_ERR(APR_E_INVALID_ARG, L"null graph");
    if (!g->running) return apr_ok();

    /* Buses first: every action is finalized to a playable file before the
     * sources feeding them go away. */
    for (i = 0; i < g->bus_count; i++) {
        AprErr e = apr_bus_stop(g->buses[i]);
        if (apr_failed(&e) && !apr_failed(&first)) first = e;
    }
    for (i = 0; i < g->source_count; i++) apr_source_stop(g->sources[i]);

    g->running = 0;
    return first;
}
