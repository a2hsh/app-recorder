/*
 * reconnect.h -- a source that dies must be able to come back.
 *
 * ===========================================================================
 * WHAT WAS WRONG WITHOUT IT
 *
 *   Close the application mid-recording and process loopback keeps handing
 *   over perfect silence for ever, with no error and no flag (design 4.1 #6).
 *   apprecorder detects that, says so once, and then does nothing at all: the
 *   rest of the take is silence even though the user reopened the app twenty
 *   seconds later and it has been playing ever since. Unplug a capture device
 *   and it is worse -- AUDCLNT_E_DEVICE_INVALIDATED kills the pump, and
 *   plugging it back in changes nothing.
 *
 *   For a recorder that runs for hours while nobody watches a status bar, a
 *   USB blip costing the remainder of the recording is the wrong behaviour.
 *   This module is the part that goes and looks for the source again.
 *
 * ===========================================================================
 * WHY IT IS A THREAD OF ITS OWN, AND WHERE THAT RULE COMES FROM
 *
 *   Re-resolving a source means enumerating every render endpoint and every
 *   audio session on the machine -- cross-process COM RPC with no bound on it.
 *   BUGS.md M3 is exactly that work happening on a capture pump: a stall past
 *   the buffer makes WASAPI drop packets on the source that IS the reference
 *   timeline, and every bus reading that tap moves against every bus that does
 *   not, permanently and silently. The fix there was not to bound the call --
 *   there is no timeout knob on a COM RPC -- but to move it to a thread that
 *   owns its own MTA and its own objects. This is that same lesson, one layer
 *   up: the search runs HERE, and the mixer loop never waits for it.
 *
 *   It is one thread for the whole graph, not one per source. The expensive
 *   part is a query about the MACHINE, so N down sources cost one enumeration.
 *
 * ===========================================================================
 * WHAT "THE SAME SOURCE" MEANS IS NOT DECIDED HERE
 *
 *   It is decided by session.h's resolver, and this module calls it rather
 *   than reimplementing it (AGENTS.md rule 3). That matters more than the
 *   duplication: the rules are image path first, executable name second,
 *   window class as the only tiebreaker between two instances, and a bare pid
 *   never. Getting them subtly different here would mean attaching to a
 *   DIFFERENT INSTANCE of the same application half way through a recording,
 *   which is worse than staying dead, and it would sound exactly like the
 *   right one being quiet.
 *
 *   THE STORED PID IS CLEARED BEFORE EVERY SEARCH. It is the identity of the
 *   instance that just died. Everything else -- exe, image path, window class
 *   -- describes the application, and an application is what came back.
 *
 * ===========================================================================
 * WHERE THE RECOVERED AUDIO LANDS
 *
 *   At the absolute frame it belongs at, never at "now". The hole is filled
 *   with exactly the silence that was missed (source.h: apr_source_pad_to and
 *   apr_source_reattach; capture.h: resume_anchor_ticks), because resuming
 *   early converts a recoverable hole into a permanent desync of every bus the
 *   source feeds. Design 3.1: alignment over content.
 *
 * ===========================================================================
 * EXCLUDE MODE IS A PRIVACY PROBLEM, NOT A CONVENIENCE ONE
 *
 *   PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE records everything the
 *   machine plays EXCEPT one process tree, named by pid. When that process
 *   exits and starts again under a new pid, the running capture goes on
 *   excluding a pid that no longer exists -- so the application the user
 *   explicitly excluded is now being recorded, silently, with nothing in the
 *   interface saying so. That is not a lost recording, it is a broken promise.
 *
 *   So an EXCLUDE source is watched even though its capture never fails, and
 *   the moment its target is gone the capture is TORN DOWN rather than left
 *   running: the source is HELD, producing silence, until the exclusion can be
 *   re-established on the successor. Holding costs the take the machine audio
 *   for a few seconds. Not holding costs the user the one thing they asked
 *   for. session.h already made this exact call at load time -- "dropping the
 *   target of an exclusion would record MORE, so failing safe means failing
 *   loudly" -- and this is the same rule at run time.
 *
 *   Consequently the hold is NOT optional and no flag turns it off, while
 *   ordinary reattachment is optional. And an EXCLUDE source's retry ceiling
 *   is much lower than an ordinary one's, because every second it waits is a
 *   second of the recording's main content, not of one track.
 *
 * ===========================================================================
 * THREADING
 *
 *   apr_reconnect_step() and everything it calls are producer-side operations
 *   on a source (source.h). They must run on ONE thread: either the worker
 *   this module owns, or the caller's -- never both. apr_reconnect_start()
 *   and apr_reconnect_stop() bracket the worker's ownership.
 *
 *   The graph's SHAPE must not change while the worker runs, exactly as for
 *   the runner (graph.h). The runner starts this worker after arming and
 *   retires it before apr_graph_stop, so a source is never started, stopped or
 *   destroyed underneath it.
 *
 *   Every query below is safe from any thread.
 */
#ifndef APPRECORDER_RECONNECT_H
#define APPRECORDER_RECONNECT_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "err.h"
#include "graph.h"
#include "session.h"
#include "source.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How long after a loss the first search happens, and how far the doubling is
 * allowed to go. A three-hour recording and a device that comes back after
 * twenty minutes must still be caught, so nothing here ever gives up; the
 * ceiling is only about how often a machine-wide enumeration is worth doing.
 *
 * Waiting costs SILENCE, never alignment -- the hole is filled either way --
 * which is what makes a ceiling of seconds acceptable at all. The EXCLUDE
 * ceiling is far lower because a held EXCLUDE source is not costing one track,
 * it is costing everything the machine plays. */
#define APR_RECONNECT_FIRST_MS       500u
#define APR_RECONNECT_MAX_MS       15000u
#define APR_RECONNECT_MAX_HELD_MS   2000u

/* How often the worker wakes to keep a detached source's ring at the frame
 * index that is due. Short, and unrelated to the retry schedule: it is one
 * memset of at most a ring's worth (source.h explains why a detached source is
 * padded at all). */
#define APR_RECONNECT_PAD_MS          25u

/* Where a source stands with the thing it was recording. */
typedef enum AprLinkState {
    /* A capture is attached and the target is there. */
    APR_LINK_LIVE = 0,

    /* The target is gone and this module is looking for it. The source is
     * detached and producing silence at exactly the right rate. */
    APR_LINK_SEARCHING,

    /* EXCLUDE ONLY: the excluded target has exited, so the exclusion can no
     * longer be honoured and the capture is held down rather than recording an
     * un-excluded machine. See the privacy note above. */
    APR_LINK_HELD,

    /* Several processes match the identity and nothing tells them apart.
     * Guessing would attach to a different instance of the same application,
     * so the source stays down and the search continues. */
    APR_LINK_REFUSED,

    /* Not watched: reconnection is off, or the source is synthetic. */
    APR_LINK_OFF
} AprLinkState;

typedef struct AprReconnectOptions {
    /* Reconnection is on by default -- the whole point is that nobody is
     * watching. Setting this stops ORDINARY sources being reattached; it does
     * NOT stop an EXCLUDE source being held, which is a privacy guarantee and
     * not a convenience.
     *
     * It exists because an author may legitimately want a source to stay dead:
     * a take being edited against a known hole, a diagnostic run, or an
     * application whose instances are genuinely interchangeable and whose
     * "recovery" would therefore be a different conversation. */
    int disabled;

    /* Watch APR_SRC_FAKE sources too. OFF by default: a fake is not a thing
     * that can be unplugged, and a fake's health knobs describe the fate of
     * one instance, so reviving one in production would make die_at_frame
     * untestable everywhere else. Tests turn it on to drive this whole
     * mechanism with no audio hardware in the room (design 4.3). */
    int synthetic;

    /* Decide and publish, change nothing. What it is for: the retry schedule
     * has to be provable over hours, and stepping a simulated clock through
     * one is the only way to do that without waiting for hours. */
    int dry_run;
} AprReconnectOptions;

typedef struct AprReconnector AprReconnector;

/* Snapshot the graph's sources and capture each one's IDENTITY from the live
 * machine while it is still running -- an image path cannot be read off a
 * process that has already exited, which is precisely when it is needed.
 *
 * A source whose identity cannot be captured is watched anyway and simply
 * never matches, so a recording is never refused over this. `opt` may be NULL
 * for the defaults. */
AprErr apr_reconnect_create(AprGraph *g, const AprReconnectOptions *opt,
                            AprReconnector **out);

void apr_reconnect_destroy(AprReconnector *rc);

/* Override what this module believes a source IS. For the caller that already
 * knows better than the live machine does -- a session file records the
 * identity as it was when the session was saved, which is a stronger answer
 * than anything that can be read back later. Call before starting the worker.
 * Only the identity fields are taken; the resolution fields are ignored.
 *
 * `pid` is read as "the instance this source is on RIGHT NOW", which is the
 * one thing an exclusion has to watch; it is then cleared, because a pid is
 * never part of matching (see the note above).
 *
 * IT ALSO SETTLES WHETHER THE SOURCE IS WATCHED, because the identity is the
 * authority on what a source is: an APR_SESSION_SRC_SYSTEM_MINUS_TREE identity
 * makes it the privacy case, and the privacy case is watched whether or not
 * ordinary reconnection was switched off. */
AprErr apr_reconnect_set_identity(AprReconnector *rc, AprSourceId id,
                                  const AprSessionSource *ident);

/* Resolve against a supplied machine instead of the live one. Same argument
 * session.h makes for AprSessionMachine, and the same shape: it is what makes
 * every outcome here reachable with no hardware and no second copy of Chrome,
 * and a front end previewing "what would happen right now" wants it too.
 * Borrowed and must outlive the reconnector. NULL restores the live machine. */
void apr_reconnect_set_machine(AprReconnector *rc, const AprSessionMachine *m);

/* One pass over every watched source at `now_ticks`: notice losses, keep
 * detached rings at the frame index that is due, and run whichever searches
 * have fallen due. Runs the caller's thread; see THREADING above.
 *
 * Ticks rather than "now" so that a test can step a three-hour schedule in a
 * loop, which is the only way the backoff is provable. */
void apr_reconnect_step(AprReconnector *rc, uint64_t now_ticks);

/* Hand the stepping to a thread this module owns. Idempotent. */
AprErr apr_reconnect_start(AprReconnector *rc);

/* Retire that thread. Idempotent, and it must be done before the graph is
 * stopped or any source is destroyed. */
void apr_reconnect_stop(AprReconnector *rc);

/* ---------------------------------------------------------------------------
 * Queries. Safe from any thread, at any time.
 * ------------------------------------------------------------------------- */

AprLinkState apr_reconnect_link(const AprReconnector *rc, AprSourceId id);

/* Times this source has been reattached to a live target. */
uint32_t apr_reconnect_recoveries(const AprReconnector *rc, AprSourceId id);

/* Searches made since the current loss. Reset by a recovery. */
uint32_t apr_reconnect_attempts(const AprReconnector *rc, AprSourceId id);

/* The current backoff, in milliseconds, and the tick the next search is due.
 * Both are 0 for a source that is not down. */
uint32_t apr_reconnect_backoff_ms(const AprReconnector *rc, AprSourceId id);
uint64_t apr_reconnect_next_due(const AprReconnector *rc, AprSourceId id);

/* The process this source is currently attached to, or -- for the privacy case
 * -- currently EXCLUDING. 0 for anything that is not a process. It is how a
 * front end says which instance is being recorded now, and how a test proves
 * an exclusion followed its application to a new one. */
uint32_t apr_reconnect_target_pid(const AprReconnector *rc, AprSourceId id);

/* Total searches this reconnector has made, over every source. The cost of the
 * retry policy, as a number a test can put a bound on. */
uint64_t apr_reconnect_searches(const AprReconnector *rc);

#ifdef __cplusplus
}
#endif
#endif /* APPRECORDER_RECONNECT_H */
