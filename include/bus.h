/*
 * bus.h -- N sources in, mixed to float32, out to M actions.
 *
 * "Record Teams and my mic" is one bus with two sources and one action.
 * Recording them separately is two buses. Same code path -- there is no
 * special case anywhere for the number of either.
 *
 * THE BUS RUNS BEHIND WALL CLOCK, AND THAT IS THE WHOLE TRICK
 *
 *   Nobody listens to this in real time (design 1, non-goals), so latency is
 *   free and sync is everything. The mixer therefore renders the block that
 *   ended APR_BUS_LOOKBEHIND_MS ago rather than the one happening now.
 *
 *   That single decision pays for three things at once:
 *
 *     - Every source's data is already in its ring when the block is
 *       rendered, so a late buffer is not a dropout.
 *     - A device capture's jitter buffer costs NO alignment. Its backlog is
 *       held at exactly the lookbehind, which means the instant its buffer is
 *       full is the instant the bus reaches its true start. Without this, a
 *       mic would sit a fixed 50 ms behind the process taps it is mixed with,
 *       which is a sync error, not a latency one.
 *     - The tick rate stops mattering. The bus produces exactly the frames
 *       QPC says are due, so a tick that arrives late produces a longer block
 *       and the timeline is unchanged. Nothing accumulates.
 *
 *   Every position comes from an ABSOLUTE QPC timestamp against the bus's
 *   anchor -- never from counting ticks. See clock.h for why.
 *
 * ORDER OF OPERATIONS in one block: clear the accumulator, pull each source
 * (which places it at its own offset), then hand the sum to every action. An
 * action that fails is marked failed and skipped; the recording continues,
 * because one broken encoder must not cost a session (design 10).
 *
 * THREAD SAFETY: one bus is ticked by one thread. Sources and actions are
 * added and removed from that same thread, or with the bus stopped.
 */
#ifndef APPRECORDER_BUS_H
#define APPRECORDER_BUS_H

#include <stddef.h>
#include <stdint.h>

#include "action.h"
#include "clock.h"
#include "err.h"
#include "source.h"

#define APR_MAX_SOURCES_PER_BUS 32
#define APR_MAX_ACTIONS_PER_BUS 8

/* Frames rendered per inner block. Bounds the bus's scratch; a tick that owes
 * more than this simply loops. */
#define APR_BUS_BLOCK_FRAMES 1024u

/* How far behind wall clock the mixer runs. Comfortably more than a WASAPI
 * buffer (10 ms) and comfortably less than a source ring (250 ms). */
#define APR_BUS_LOOKBEHIND_MS 50

typedef uint32_t AprBusId;
typedef struct AprBus AprBus;

/* ---------------------------------------------------------------------------
 * Lifetime
 * ------------------------------------------------------------------------- */

AprErr apr_bus_create(AprBusId id, const wchar_t *name,
                      uint32_t sample_rate, uint16_t channels, AprBus **out);

/* Finalizes and destroys any actions still attached, closes every reader
 * (lowering each source's refcount), and frees the bus. Does not own sources. */
void apr_bus_destroy(AprBus *b);

AprBusId       apr_bus_id(const AprBus *b);
const wchar_t *apr_bus_name(const AprBus *b);
uint32_t       apr_bus_rate(const AprBus *b);
uint16_t       apr_bus_channels(const AprBus *b);

/* ---------------------------------------------------------------------------
 * Edges. A source may be on many buses at once; each attachment is its own
 * reader with its own cursor, resampler and controller.
 * ------------------------------------------------------------------------- */

AprErr apr_bus_add_source(AprBus *b, AprSource *s, float gain);
AprErr apr_bus_remove_source(AprBus *b, AprSourceId id);
int    apr_bus_has_source(const AprBus *b, AprSourceId id);

size_t     apr_bus_source_count(const AprBus *b);
AprSource *apr_bus_source_at(const AprBus *b, size_t index);

/* The reader for source `index` on this bus -- the drift diagnostics live
 * there, and a source on two buses has two independent sets. */
AprSourceReader *apr_bus_reader_at(const AprBus *b, size_t index);

/* Per-source linear gain. Non-finite values are refused. */
AprErr apr_bus_set_gain(AprBus *b, AprSourceId id, float gain);
float  apr_bus_gain(const AprBus *b, AprSourceId id);

/* ---------------------------------------------------------------------------
 * Actions
 * ------------------------------------------------------------------------- */

/* Creates the action's state immediately, so a bad path fails now rather than
 * halfway through a recording. */
AprErr apr_bus_add_action(AprBus *b, const AprActionVTable *vt,
                          const AprActionConfig *cfg);

size_t                 apr_bus_action_count(const AprBus *b);
const AprActionVTable *apr_bus_action_at(const AprBus *b, size_t index);

/* Nonzero once an action has refused audio and been dropped from the fan-out.
 * Its finalize is still called on stop -- a half-written file must still
 * open. */
int    apr_bus_action_failed(const AprBus *b, size_t index);
AprErr apr_bus_action_error(const AprBus *b, size_t index);

/* ---------------------------------------------------------------------------
 * Running
 * ------------------------------------------------------------------------- */

/* Anchors the bus timeline at `start_ticks` (QPC). Every position afterwards
 * is measured from it. */
AprErr apr_bus_start(AprBus *b, uint64_t start_ticks);

/* Render everything QPC says is due at `now_ticks`, minus the lookbehind.
 * Produces zero frames if called twice in a row with the same timestamp, and
 * catches up in full if called late. */
AprErr apr_bus_tick(AprBus *b, uint64_t now_ticks);

/* Finalizes every action, including failed ones. Idempotent. */
AprErr apr_bus_stop(AprBus *b);

int      apr_bus_running(const AprBus *b);
uint64_t apr_bus_frames_out(const AprBus *b);
float    apr_bus_peak(const AprBus *b);        /* last block, for metering */

const AprClock *apr_bus_clock(const AprBus *b);
double          apr_bus_lookbehind_frames(const AprBus *b);

#endif /* APPRECORDER_BUS_H */
