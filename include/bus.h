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
#include "outpath.h"
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

/* Rename. Returns 0 for an empty name, which is refused rather than accepted:
 * a bus with no name is a row a screen reader reads as nothing at all, and
 * every projection of this model reads the name (graph.h). The id does not
 * change, so nothing holding one is affected. */
int            apr_bus_set_name(AprBus *b, const wchar_t *name);
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

/* ---------------------------------------------------------------------------
 * AN ACTION'S LIFETIME IS ONE RECORDING, NOT THE GRAPH'S
 *
 *   This used to call vt->create() here, so the output file was opened the
 *   moment the user ADDED the output and closed for good at the first stop.
 *   The consequence, reported by the author and reproducible every time:
 *
 *     record -> stop -> record again -> THE SECOND RECORDING WROTE NOTHING.
 *
 *   finalize had closed the encoder permanently, on_audio refused everything
 *   afterwards, and deleting the stale file did not help either -- Windows
 *   keeps an open handle valid with no directory entry, so the third take went
 *   to a file with no name. Three recordings, one of them kept. That is data
 *   loss, and it came from the lifetime, not from any encoder.
 *
 *   So A BUS HOLDS A SPEC, NOT AN OPEN ENCODER: the vtable plus the config it
 *   would be created with. apr_bus_start() creates every action's state;
 *   apr_bus_stop() finalizes and destroys it. Record twice and you get two
 *   files. Delete the file and record again and it comes back.
 *
 *   WHAT THAT MOVED, AND WHERE IT MOVED TO. A bad path used to be caught by
 *   create() at add time, which was worth having: the person who typed the
 *   name is still there to fix it. Opening the file at record time would have
 *   lost that, so the CHECK stayed where it was and only the OPEN moved --
 *   apr_bus_add_action() validates through apr_out_validate() (outpath.h),
 *   which expands the name and asks the folder whether something may be
 *   created in it, leaving nothing behind. Validate at add, open at record.
 * ------------------------------------------------------------------------- */

/* Record what this output IS. Creates no file and opens no encoder -- but
 * validates the path now, so an unwritable folder is refused while there is
 * still somebody to tell. */
AprErr apr_bus_add_action(AprBus *b, const AprActionVTable *vt,
                          const AprActionConfig *cfg);

size_t                 apr_bus_action_count(const AprBus *b);
const AprActionVTable *apr_bus_action_at(const AprBus *b, size_t index);

/* WHAT THE USER ASKED FOR: the configured path for output `index`, exactly as
 * it was given, tokens and all. This is the template, not a filename -- see
 * outpath.h -- and it is what a session file stores, because reopening a
 * session tomorrow must record to tomorrow's name and not to yesterday's.
 *
 * The bus keeps it because the bus is what owns the output; a front end
 * keeping its own copy would be a second answer to the same question. Never
 * NULL. */
const wchar_t *apr_bus_action_path(const AprBus *b, size_t index);

/* WHERE THE AUDIO ACTUALLY WENT: the expanded, collision-resolved path of the
 * recording in progress, or of the last one this bus made. Empty until the
 * first apr_bus_start().
 *
 * Both are needed and neither substitutes for the other. "{bus} {date}.wav" is
 * what to save; "Main Mix 2026-08-26-2.wav" is what to tell the user, and it
 * is the only honest answer once the collision policy has renamed a take. */
const wchar_t *apr_bus_action_current_path(const AprBus *b, size_t index);

/* Nonzero when the current recording had to be saved under a different name
 * because the one that was asked for was already a recording. The collision
 * policy never overwrites and never refuses (outpath.h); this is how a front
 * end knows there is a sentence it owes the user. */
int apr_bus_action_renamed(const AprBus *b, size_t index);

/* The rest of the spec, so that saving a session round-trips the whole output
 * rather than its path alone. 0 means the encoder's own default. */
int apr_bus_action_bitrate(const AprBus *b, size_t index);
int apr_bus_action_quality(const AprBus *b, size_t index);

/* Remove one output.
 *
 * IF A RECORDING IS OPEN ON IT, IT IS FINALIZED FIRST, ALWAYS -- detaching an
 * encoder without finalizing it is exactly the "unplayable file" outcome
 * AGENTS.md rule 4 exists to prevent, and a user removing an output mid-session
 * is the case where it would happen. Then destroy, then close the gap.
 *
 * An output that is NOT recording has nothing open on it any more (see the
 * lifetime note above), so removing one from an idle graph is now what it
 * always looked like: forgetting a plan. No file is touched.
 *
 * Later actions shift down by one, so an index held across this call names a
 * different output afterwards. That is the same contract the array already
 * had; nothing outside a bus may hold an action index across a mutation.
 *
 * This function did not exist while the UI could only navigate an existing
 * graph, which is why the canvas used to refuse Delete on an output row out
 * loud. It exists now because a front end that can add an output must be able
 * to take it away again -- and inventing a private removal inside the UI would
 * have made the UI a second owner of this array. */
AprErr apr_bus_remove_action(AprBus *b, size_t index);

/* Nonzero once an action has refused audio and been dropped from the fan-out.
 * Its finalize is still called on stop -- a half-written file must still
 * open. */
int    apr_bus_action_failed(const AprBus *b, size_t index);
AprErr apr_bus_action_error(const AprBus *b, size_t index);

/* ---------------------------------------------------------------------------
 * Running
 * ------------------------------------------------------------------------- */

/* Anchors the bus timeline at `start_ticks` (QPC), and CREATES EVERY ACTION:
 * each spec's path is expanded and collision-resolved (outpath.h) and its
 * encoder is opened. This is the moment a file appears on disk.
 *
 * An action that will not open is marked failed and skipped, exactly as one
 * that refuses audio mid-recording is: one broken encoder must not cost a
 * session (design 10). The failure is readable through apr_bus_action_error()
 * and the runner announces it. */
AprErr apr_bus_start(AprBus *b, uint64_t start_ticks);

/* Render everything QPC says is due at `now_ticks`, minus the lookbehind.
 * Produces zero frames if called twice in a row with the same timestamp, and
 * catches up in full if called late. */
AprErr apr_bus_tick(AprBus *b, uint64_t now_ticks);

/* ---------------------------------------------------------------------------
 * PAUSE -- THE OUTPUT POSITION STOPS MAPPING TO WALL CLOCK
 *
 *   Everything above this line derives a position from an absolute QPC
 *   timestamp against one anchor, and that identity between elapsed time and
 *   output position is what buys sub-frame alignment over three hours. A pause
 *   breaks it deliberately, and how it is broken is the whole design:
 *
 *   PAUSED TIME IS EXCISED, NOT FILLED. That is the difference between pause
 *   and mute, and it is why this cannot borrow the gap-fill path: the gap-fill
 *   path exists to keep output position equal to elapsed time, which is exactly
 *   what a pause must stop doing. Writing silence for the paused span would
 *   produce a file as long as the wall clock, which is a mute.
 *
 *   THE ORIGIN MOVES. apr_bus_resume shifts this bus's anchor LATER by the
 *   paused duration (clock.h), so the very next tick is due at the frame the
 *   last tick before the pause reached. frames_out is continuous, no block is
 *   rendered twice, and nothing accumulates -- the arithmetic is the same
 *   absolute-position arithmetic as before, measured from a new origin.
 *
 *   EVERY BUS TAKES THE SAME SHIFT, AT ONE INSTANT. apr_graph_pause and
 *   apr_graph_resume are what callers use, for the same reason apr_graph_run
 *   anchors every bus at one timestamp: alignment BETWEEN buses is the property
 *   that matters, and it is the only one a pause can quietly destroy. Two buses
 *   given different shifts are two files that no longer line up, silently, for
 *   the rest of the session.
 *
 *   THE SOURCES ARE NOT TOLD, and that is not an omission. A source's ring is
 *   an index of real time; its capture keeps running, its 250 ms ring laps
 *   several times over, and its clock, its drift figure and the reconnect
 *   worker's padding all stay correct BECAUSE they were never asked to model
 *   the pause. Only each reader is re-based onto the new origin
 *   (apr_source_reader_rebase), which is also where the paused audio is
 *   discarded rather than reported as loss -- see source.h for why that
 *   decision can only live there.
 *
 *   THE ENCODERS STAY OPEN. A pause is not a stop: no action is finalized, no
 *   file is closed, and the take is one file. apr_bus_stop is still the only
 *   thing that ends an action's life (see the lifetime note above), so pausing
 *   and resuming cannot produce a second file and cannot lose one.
 *
 *   WHAT IT COSTS: the mixer runs APR_BUS_LOOKBEHIND_MS behind wall clock, so
 *   the cut is that far before the keystroke and the resume that far before the
 *   one after it. The excised span is EXACTLY the paused duration either way --
 *   nothing is duplicated and nothing is dropped -- it simply begins and ends
 *   50 ms earlier than the fingers did. The one place that is visible is a stop
 *   made WHILE paused, which ends the file 50 ms before the pause keystroke,
 *   because that audio was never rendered and is long gone from the ring.
 * ------------------------------------------------------------------------- */

/* Stop rendering. Ticks become no-ops; nothing is finalized and no file is
 * closed. Idempotent, and a no-op on a bus that is not running. */
AprErr apr_bus_pause(AprBus *b);

/* Resume, moving this bus's origin later by `delta_ticks` -- the wall time that
 * elapsed while it was paused. Every reader is re-based onto the new origin.
 * Idempotent, and a no-op on a bus that is not paused. */
AprErr apr_bus_resume(AprBus *b, uint64_t delta_ticks);

int apr_bus_paused(const AprBus *b);

/* Finalizes every action, including failed ones, THEN DESTROYS IT: the
 * encoder's life ends with the recording it was made for. Idempotent, and the
 * failed flags survive it so a caller can still ask what went wrong. */
AprErr apr_bus_stop(AprBus *b);

int      apr_bus_running(const AprBus *b);
uint64_t apr_bus_frames_out(const AprBus *b);
float    apr_bus_peak(const AprBus *b);        /* last block, for metering */

const AprClock *apr_bus_clock(const AprBus *b);
double          apr_bus_lookbehind_frames(const AprBus *b);

#endif /* APPRECORDER_BUS_H */
