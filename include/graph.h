/*
 * graph.h -- the model. Sources, buses, actions, and the edges between them.
 *
 * THIS IS A GRAPH, NOT A TREE, and that is the point: a source may feed
 * several buses at once ("Teams" into both a full mix and a Teams-only file).
 * Edges therefore have to be first-class -- an edge is a source id, a bus id
 * and a gain -- rather than implied by ownership.
 *
 * THE CANVAS AND THE ACCESSIBILITY TREE ARE BOTH PROJECTIONS OF THIS
 *
 *   Neither view is derived from the other and neither is a fallback for the
 *   other; both read this model (design 3, 6.1). That is why the queries below
 *   exist in both directions -- apr_graph_buses_for_source is what a node
 *   window needs to describe its own edges out loud ("Teams, source, feeding
 *   Main Mix and Teams Only"), and apr_graph_sources_for_bus is what the tree
 *   panel nests under a bus. Neither is a convenience wrapper over the other;
 *   asking a renderer to walk the model itself is how the two views drift
 *   apart.
 *
 *   Ids are stable and never reused within a session, so a UI node, an
 *   accessibility element and a session file can all hold one.
 *
 * FORMAT IS A SESSION-LEVEL DECISION (design 4.1). A process-loopback client
 * returns E_NOTIMPL from GetMixFormat -- there is no device to negotiate with,
 * so the rate and channel count are supplied to it rather than discovered.
 * They live here, once, and every source and bus takes them from the graph.
 *
 * FIXED CAPACITIES. Sources, buses and edges live in fixed arrays. A recorder
 * with 64 sources and 32 buses is already far past what any rig has, and the
 * alternative -- reallocating arrays a mixer thread walks -- buys nothing but
 * a lifetime problem.
 *
 * THREAD SAFETY: the graph is built and torn down on one thread, and ticked on
 * one thread. Do not mutate the shape while a tick is in flight.
 */
#ifndef APPRECORDER_GRAPH_H
#define APPRECORDER_GRAPH_H

#include <stddef.h>
#include <stdint.h>

#include "action.h"
#include "bus.h"
#include "capture.h"
#include "err.h"
#include "source.h"

#define APR_MAX_SOURCES 64
#define APR_MAX_BUSES   32

typedef struct AprGraph AprGraph;

/* ---------------------------------------------------------------------------
 * Lifetime
 * ------------------------------------------------------------------------- */

/* `sample_rate` and `channels` are the session format every source is opened
 * at and every bus mixes to. */
AprErr apr_graph_create(uint32_t sample_rate, uint16_t channels, AprGraph **out);

/* Stops everything, finalizes every action, destroys every bus and source. */
void apr_graph_destroy(AprGraph *g);

uint32_t apr_graph_rate(const AprGraph *g);
uint16_t apr_graph_channels(const AprGraph *g);

/* ---------------------------------------------------------------------------
 * Nodes
 * ------------------------------------------------------------------------- */

/* `cfg`'s sample_rate and channels are filled in from the graph, so a caller
 * only describes WHAT to capture, never at what format. */
AprErr apr_graph_add_source(AprGraph *g, const wchar_t *name,
                            const AprCaptureConfig *cfg, AprSourceId *out_id);

/* Disconnects the source from every bus first, so removing a node can never
 * leave a bus holding a reader into freed memory. */
AprErr apr_graph_remove_source(AprGraph *g, AprSourceId id);

AprSource *apr_graph_source(const AprGraph *g, AprSourceId id);   /* NULL if unknown */
size_t     apr_graph_source_count(const AprGraph *g);
AprSource *apr_graph_source_at(const AprGraph *g, size_t index);

AprErr  apr_graph_add_bus(AprGraph *g, const wchar_t *name, AprBusId *out_id);
AprErr  apr_graph_remove_bus(AprGraph *g, AprBusId id);
AprBus *apr_graph_bus(const AprGraph *g, AprBusId id);            /* NULL if unknown */
size_t  apr_graph_bus_count(const AprGraph *g);
AprBus *apr_graph_bus_at(const AprGraph *g, size_t index);

/* ---------------------------------------------------------------------------
 * Edges
 * ------------------------------------------------------------------------- */

/* Raises the source's refcount and gives the bus its own reader -- its own
 * ring cursor, its own resampler, its own drift controller. Two buses reading
 * one source share nothing but the ring. */
AprErr apr_graph_connect(AprGraph *g, AprSourceId source, AprBusId bus, float gain);
AprErr apr_graph_disconnect(AprGraph *g, AprSourceId source, AprBusId bus);
int    apr_graph_connected(const AprGraph *g, AprSourceId source, AprBusId bus);
size_t apr_graph_edge_count(const AprGraph *g);

/* ---------------------------------------------------------------------------
 * Projections -- what the canvas, the tree panel and a screen reader read
 * ------------------------------------------------------------------------- */

/* Buses this source feeds. Returns the count, which may exceed `cap`; at most
 * `cap` ids are written. `out` may be NULL to count only. */
size_t apr_graph_buses_for_source(const AprGraph *g, AprSourceId source,
                                  AprBusId *out, size_t cap);

/* Sources feeding this bus, in the order they were connected. */
size_t apr_graph_sources_for_bus(const AprGraph *g, AprBusId bus,
                                 AprSourceId *out, size_t cap);

/* ---------------------------------------------------------------------------
 * Actions
 * ------------------------------------------------------------------------- */

/* Looked up by registry id ("wav", "m4a", ...) -- the string a session file
 * stores. The config's format fields are filled in from the graph. */
AprErr apr_graph_add_action(AprGraph *g, AprBusId bus, const char *action_id,
                            const AprActionConfig *cfg);

/* ---------------------------------------------------------------------------
 * Running
 * ------------------------------------------------------------------------- */

/* Arms every source's capture without touching any bus timeline. Separate
 * from apr_graph_run for two reasons: a UI wants sources pre-rolling before
 * the user commits to recording, and a test driving a synthetic capture by
 * hand (design 4.3) must never let a real capture thread start. A source that
 * fails to arm is left in the graph and reported; the session continues
 * without it (design 10). */
AprErr apr_graph_arm(AprGraph *g);

/* Anchors every bus at `start_ticks` -- ONE anchor for all of them, which is
 * what makes two files from one session line up with each other and not merely
 * each with itself. */
AprErr apr_graph_run(AprGraph *g, uint64_t start_ticks);

/* apr_graph_arm followed by apr_graph_run. What the app calls. */
AprErr apr_graph_start(AprGraph *g, uint64_t start_ticks);

/* Ticks every bus. */
AprErr apr_graph_tick(AprGraph *g, uint64_t now_ticks);

/* Stops every bus (finalizing every action), then every source. Idempotent. */
AprErr apr_graph_stop(AprGraph *g);

int apr_graph_running(const AprGraph *g);

#endif /* APPRECORDER_GRAPH_H */
