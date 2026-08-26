/*
 * join.h -- the bounded join, and the rule that has to go with it.
 *
 * THE PATTERN THIS FILE EXISTS TO STOP
 *
 *   Four bugs in this codebase have had one shape: a bounded wait for a
 *   thread to leave, followed by a free, with nothing in between that could
 *   tell the freeing code the wait had timed out. m4a_destroy, both WASAPI
 *   close paths, apr_source_destroy and apr_log_shutdown all had it. The
 *   mechanism that let it recur was that every one of those functions
 *   returned `void`, so a timeout one layer correctly detected could not
 *   reach the layer that frees.
 *
 *   THE RULE, stated once so it does not have to be rediscovered:
 *
 *     A BOUNDED WAIT THAT MAY TIME OUT MUST PRODUCE A VALUE, AND THE CODE
 *     THAT FREES MUST BE THE CODE THAT READS IT.
 *
 *   Two halves, and the second is the load-bearing one. A return value alone
 *   is only a report -- C cannot force a caller to read it. What makes this
 *   safe is that the DECISION TO FREE is taken at the same place the join
 *   result is known: apr_capture_destroy owns both, and apr_source_destroy
 *   owns both. Every layer above them gets an AprErr so it can say what
 *   happened, but no layer above them is trusted with memory safety. A caller
 *   that drops the returned error leaks; it cannot corrupt.
 *
 *   The other half of the rule: WHEN A JOIN IS ABANDONED, NOTHING THE
 *   ABANDONED THREAD CAN REACH IS FREED -- not the object, not its buffers,
 *   not the struct that holds them. Leaking a thread, a COM reference and
 *   96 KB of ring is survivable. A use-after-free under an audio thread is
 *   not, and it is a heap corruption whose stack trace points somewhere else
 *   entirely.
 *
 * WHY THERE IS NO "abandoned" ERROR KIND
 *
 *   APR_E_TIMEOUT already means "waited long enough", which is exactly what
 *   happened. What a caller has to do about it does not depend on telling
 *   this timeout apart from another one: any failure out of a destroy means
 *   the object was not retired, and every such path already leaks rather than
 *   frees.
 *
 * THREAD SAFETY: everything here is stateless. Nothing allocates.
 */
#ifndef APPRECORDER_JOIN_H
#define APPRECORDER_JOIN_H

#include <windows.h>

#include "err.h"

typedef enum AprJoin {
    APR_JOIN_EXITED    = 0,  /* the thread is gone; its memory may be freed */
    APR_JOIN_ABANDONED = 1   /* still running; free NOTHING it can reach */
} AprJoin;

/* Wait up to `timeout_ms` for `h` -- a thread handle, or an event a thread
 * sets as it leaves. A NULL handle is APR_JOIN_EXITED: there is nothing to
 * wait for and nothing to keep alive. Any wait result other than "signalled"
 * (including WAIT_FAILED on a handle that was closed underneath us) is
 * treated as abandonment, because the one thing that must never happen is
 * concluding a thread has exited when it has not. */
AprJoin apr_join_wait(HANDLE h, DWORD timeout_ms);

/* The same for a pair -- typically "the thread went idle" OR "the thread
 * died", where either answer means it is no longer inside the region that
 * touches the caller's memory. Either handle may be NULL. */
AprJoin apr_join_wait2(HANDLE a, HANDLE b, DWORD timeout_ms);

/* The error an abandoned join returns. `what` is an internal English noun
 * phrase naming the thread ("the capture pump"), not a user-facing string --
 * rule 6 exempts log and diagnostic text, and no screen reader ever reads
 * this: the front end wraps it in a catalog sentence. */
#define APR_ERR_ABANDONED(what)                                              \
    apr_err_make(APR_E_TIMEOUT, 0, __func__, __FILE__, __LINE__,             \
                 L"%ls would not stop within the wait; it is still running, " \
                 L"so nothing it can reach was freed", (what))

#endif /* APPRECORDER_JOIN_H */
