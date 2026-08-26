/*
 * runner.h -- the recording loop, owned once and driven by both front ends.
 *
 * ===========================================================================
 * WHY THIS EXISTS
 *
 *   Everything below this header -- capture, graph, bus, action -- is already
 *   shared. What was NOT shared, until this file, was the twenty lines that
 *   turn a configured graph into a recording: arm, anchor at one QPC instant,
 *   tick until the duration elapses or a stop arrives, WAIT OUT THE MIXER'S
 *   LOOKBEHIND so the final block is not thrown away, then finalize every
 *   action on every exit path.
 *
 *   Those twenty lines lived inside src/cli/cli.c. A second front end copying
 *   them is exactly the DRY failure AGENTS.md rule 3 names: the copy would
 *   forget the lookbehind drain, or poll health on a different schedule, or
 *   finalize on one exit path and not another -- and the second and third of
 *   those produce a file that opens and is silently wrong.
 *
 *   So the loop lives here and the CLI is now one of its two callers. The
 *   CLI's own suite is the proof the extraction was faithful.
 *
 * ===========================================================================
 * THE THREE THINGS THE LOOP KNOWS THAT A NAIVE ONE DOES NOT
 *
 *   1. EVERY POSITION COMES FROM AN ABSOLUTE QPC TIMESTAMP. The bus renders
 *      exactly the frames elapsed wall time says are due (bus.h), so a tick
 *      that arrives late produces a longer block and nothing accumulates. The
 *      loop therefore does not care how punctual it is, which is what lets it
 *      share a thread with a stop event.
 *
 *   2. THE MIXER RUNS APR_BUS_LOOKBEHIND_MS BEHIND WALL CLOCK. Stopping at
 *      the instant the user asked would discard the last block. The loop waits
 *      for it to fall due, renders it, and only then finalizes.
 *
 *   3. A SOURCE CAN DIE OR BE MUTED WITHOUT ANYTHING FAILING. Process loopback
 *      keeps producing perfect silence forever after the target exits, and
 *      WASAPI never says so; a muted application records digital silence while
 *      the engine still reports it rendering (design 4.1, measured). Both look
 *      healthy from every other angle. The loop polls for both and reports
 *      them ONCE EACH, because a warning per tick is a warning nobody reads.
 *
 * ===========================================================================
 * THREADING
 *
 *   apr_runner_run() runs the whole recording on the CALLING thread. The CLI
 *   calls it directly. A GUI calls apr_runner_run_async(), which runs the same
 *   function on a thread the runner owns, because a message loop that blocks
 *   is a window that stops answering the accessibility tree.
 *
 *   THE OBSERVER IS CALLED ON WHICHEVER THREAD IS RUNNING THE LOOP. It may
 *   resolve catalog strings (apr_str is thread-safe) but it must not touch a
 *   window: post, do not send-and-paint. Every notice is a VALUE with its
 *   names already copied in, precisely so that it can be posted somewhere and
 *   read later without holding a pointer into a graph that may be gone.
 *
 *   Everything else here -- request_stop, elapsed, the health snapshot, the
 *   per-bus frame counts -- is safe to call from any thread at any time.
 */
#ifndef APPRECORDER_RUNNER_H
#define APPRECORDER_RUNNER_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "bus.h"
#include "err.h"
#include "graph.h"
#include "reconnect.h"
#include "source.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How often the loop wakes. Not a timeline parameter -- see note 1 above. */
#define APR_RUNNER_TICK_MS 10

/* ---------------------------------------------------------------------------
 * What the loop has to tell somebody about
 * ------------------------------------------------------------------------- */

typedef enum AprRunEvent {
    APR_RUN_EV_NONE = 0,

    /* A source failed to arm. The session continues without it (design 10),
     * and the run is marked incomplete. */
    APR_RUN_EV_ARM_FAILED,

    /* Every bus is anchored and audio is being written. Fired once, after
     * arming, before the first tick. */
    APR_RUN_EV_STARTED,

    /* The target process exited. Reported once per source -- and reported
     * AGAIN if the source comes back and dies a second time, because the
     * second loss is a second hole in the file. */
    APR_RUN_EV_SOURCE_DIED,

    /* THE OTHER HALF, AND IT IS THE ONE THAT IS EASY TO FORGET. The source is
     * producing audio again -- the application was reopened and reattached, or
     * the device was plugged back in (reconnect.h). The hole is filled with
     * exactly the silence that was missed and everything after it is at its
     * true absolute position, so this sentence is what tells the author his
     * take is intact rather than half lost.
     *
     * Reported once per recovery. Announcing only the loss leaves somebody who
     * stepped away believing a recording ended forty minutes before it did. */
    APR_RUN_EV_SOURCE_RECOVERED,

    /* AN EXCLUSION CANNOT BE HONOURED ANY MORE, so this source has been held
     * down and is recording nothing until it can be.
     *
     * EXCLUDE mode records everything the machine plays except one process
     * TREE, named by pid (design 4.1.1). When that process exits, the running
     * capture goes on excluding a number that means nothing -- so if the user
     * reopens the application it is now being recorded, silently, having been
     * explicitly excluded. apprecorder holds the capture down rather than let
     * that happen, and says so. Reported once per hold. */
    APR_RUN_EV_EXCLUSION_HELD,

    /* Session volume is zero: this source is recording digital silence.
     * Reported once per source. */
    APR_RUN_EV_SOURCE_MUTED,

    /* An action refused audio and was dropped from the fan-out. Its finalize
     * still runs -- a half-written file must still open.
     *
     * Reported AS SOON AS IT HAPPENS, once per output. An output that will not
     * even open reaches the user through this event at the start of the run
     * rather than at the end of it, which is the difference between losing a
     * second and losing an hour. */
    APR_RUN_EV_ACTION_FAILED,

    /* THE TAKE IS BEING SAVED SOMEWHERE OTHER THAN THE NAME THAT WAS ASKED
     * FOR, because that name was already a recording and apprecorder does not
     * overwrite one (outpath.h). `path` is where the audio is really going;
     * `name` is the output's display name, as for every other action event.
     *
     * This event is the honest half of the auto-increment policy. Renaming
     * silently would trade one surprise for another; the take is kept AND the
     * user is told, at the moment it happens. Reported once per output. */
    APR_RUN_EV_OUTPUT_RENAMED,

    /* The last block has been rendered; the files are about to be closed.
     * Fired before apr_graph_stop, so a front end can say "finishing" before
     * an encoder blocks on its disk. */
    APR_RUN_EV_FINISHING,

    /* Every action has been finalized. The files are closed and playable. */
    APR_RUN_EV_STOPPED
} AprRunEvent;

typedef struct AprRunNotice {
    AprRunEvent ev;

    /* Index into the GRAPH's source / bus arrays, not into any front end's
     * plan. SIZE_MAX when the notice is not about one. */
    size_t source_index;
    size_t bus_index;
    size_t action_index;

    /* Already resolved and COPIED: the source's name, the action's display
     * name, or the bus's name -- whichever the event is about. Empty for the
     * events that are about the whole run. Copied rather than borrowed so a
     * notice can be posted to another thread and read after the graph is
     * gone. */
    wchar_t name[APR_NAME_CCH];

    /* THE FILE THIS EVENT IS ABOUT, resolved and expanded -- not the template
     * the user typed. Empty for the events that are not about one. Its own
     * field rather than `name` because a path does not fit in APR_NAME_CCH and
     * truncating the one thing the user has to go and look at would be worse
     * than saying nothing. */
    wchar_t path[APR_OUT_PATH_CCH];

    AprErr err;      /* apr_ok() unless the event carries a failure */
} AprRunNotice;

typedef void (*AprRunObserverFn)(void *user, const AprRunNotice *n);

/* ---------------------------------------------------------------------------
 * The runner
 * ------------------------------------------------------------------------- */

typedef struct AprRunnerConfig {
    /* BORROWED and must outlive the runner. The runner never changes the
     * graph's SHAPE -- it arms it, ticks it and stops it. Do not add or remove
     * a node while a runner is running (graph.h). */
    AprGraph *graph;

    /* 0 = until stopped. */
    int64_t duration_ms;

    /* 0 = APR_RUNNER_TICK_MS. */
    unsigned tick_ms;

    /* RECONNECTION IS ON BY DEFAULT, because nobody is watching. A source that
     * dies is searched for and reattached at its true absolute position
     * (reconnect.h), and both the loss and the recovery are announced.
     *
     * Setting this leaves an ordinary source dead once it dies, which is what
     * this program used to do. It does NOT switch off the EXCLUDE hold: that
     * one is a privacy guarantee, not a convenience. */
    int no_reconnect;

    /* An EXISTING stop event to wait on, or NULL for the runner to make its
     * own. It is here for the CLI, whose console control handler is installed
     * before any runner exists and must be able to signal one that does not
     * yet. Manual-reset; borrowed, never closed by the runner. */
    HANDLE stop_event;

    AprRunObserverFn observer;   /* may be NULL */
    void            *user;
} AprRunnerConfig;

typedef struct AprRunner AprRunner;

/* Snapshots the graph's shape (names, ids, counts) so that everything below
 * can be answered without touching the graph from another thread. */
AprErr apr_runner_create(const AprRunnerConfig *cfg, AprRunner **out);

/* Requests a stop, waits for the loop to finish, and frees. Safe with NULL.
 * NEVER abandons a running recording: an unfinalized action is an unplayable
 * file, which is worse than a slow exit.
 *
 * IT WAITS FOR A SYNCHRONOUS RUN TOO, and it used to wait only for the thread
 * apr_runner_run_async() creates. apr_runner_run() executes the whole
 * recording on the CALLER's thread, and a runner has no handle for that
 * thread, so destroy simply did not see it: a second thread calling destroy
 * freed the runner out from under a live recording loop that was still writing
 * to r->elapsed_ms and about to finalize the files. What made it latent is
 * that today's callers happen to destroy from the same thread they ran on --
 * which is to say the safety net has never been under load.
 *
 * RETURNS apr_ok() when the runner was actually freed, and a failure when the
 * loop would not finish inside the bounded wait -- in which case NOTHING IS
 * FREED and `r` remains valid, because the loop thread still holds it (see
 * join.h). Retrying later is legal. A caller that ignores the value leaks; it
 * cannot corrupt. */
AprErr apr_runner_destroy(AprRunner *r);

/* The whole recording, on the calling thread. Returns apr_ok() even when
 * sources died or actions failed -- ask apr_runner_incomplete() for that. A
 * failure return means the run could not be started at all. */
AprErr apr_runner_run(AprRunner *r);

/* apr_runner_run() on a thread the runner owns. Returns as soon as the thread
 * is running; the observer starts firing immediately. */
AprErr apr_runner_run_async(AprRunner *r);

/* Nonzero once the loop has finished and every file is closed. `ms` may be 0
 * to poll or INFINITE to block. */
int apr_runner_wait(AprRunner *r, DWORD ms);

/* Set once the run has finished and every action has been finalized. This is
 * the handle a window-close or console-close path waits on: the process must
 * not die between the last block and the last fclose. Never NULL for a live
 * runner; owned by the runner, do not close it. */
HANDLE apr_runner_finished_event(const AprRunner *r);

/* Ask the loop to stop. Any thread, any time, including before the run has
 * started and after it has ended. Idempotent. */
void apr_runner_request_stop(AprRunner *r);

int apr_runner_running(const AprRunner *r);

/* Nonzero when the recording happened but is not what was asked for: a source
 * failed to arm, a source died mid-recording, or an action failed. Every file
 * is still playable.
 *
 * A RECOVERY DOES NOT CLEAR IT. The source came back and everything after the
 * hole is at its true position, but the hole is still in the file, and a
 * summary that said "complete" would be describing a recording that is not the
 * one that was asked for. */
int apr_runner_incomplete(const AprRunner *r);

/* Wall-clock milliseconds since the buses were anchored. 0 before the run
 * starts; frozen at its final value afterwards. */
int64_t apr_runner_elapsed_ms(const AprRunner *r);

/* ---------------------------------------------------------------------------
 * The snapshot -- everything a UI needs to draw a recording, from any thread
 * ------------------------------------------------------------------------- */

typedef struct AprRunSourceState {
    AprSourceId id;
    wchar_t     name[APR_NAME_CCH];
    int         alive;    /* 0 once the target process is known to have exited */
    int         muted;    /* nonzero: recording digital silence                */
    uint64_t    frames;   /* frames the capture has delivered                  */
} AprRunSourceState;

size_t apr_runner_source_count(const AprRunner *r);
int    apr_runner_source_state(const AprRunner *r, size_t index,
                               AprRunSourceState *out);

/* Where this source stands with the thing it was recording, as the reconnect
 * worker sees it (reconnect.h). APR_LINK_OFF when nothing is watching it --
 * reconnection switched off, or a synthetic source.
 *
 * It is here rather than only on the reconnector because `alive == 0` has two
 * quite different meanings now, and a status line that cannot tell "the app
 * exited and we are looking for it" from "we are deliberately not recording,
 * to keep an exclusion" would be reading out the wrong one half the time. */
AprLinkState apr_runner_source_link(const AprRunner *r, size_t index);

/* Times this source has been reattached to a live target during this run. */
uint32_t apr_runner_source_recoveries(const AprRunner *r, size_t index);

size_t   apr_runner_bus_count(const AprRunner *r);
AprBusId apr_runner_bus_id_at(const AprRunner *r, size_t index);

/* Frames this bus has written. Sampled every tick while running and pinned at
 * its final value when the run ends -- reading it after the fact is how a
 * front end reports the duration of each file it produced. */
uint64_t apr_runner_bus_frames(const AprRunner *r, AprBusId bus);

#ifdef __cplusplus
}
#endif
#endif /* APPRECORDER_RUNNER_H */
