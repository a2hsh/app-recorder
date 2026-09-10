/*
 * test_wait.h -- how long a test waits, in one place.
 *
 * ===========================================================================
 * WHY THIS FILE EXISTS
 *
 *   The suites in this tree drive real threads: a message loop on one, a
 *   recording loop on another, a log drain, a reconnect worker. A test that
 *   has just asked one of them for something has to wait for the answer, and
 *   every such wait used to be written by hand at the site -- poll a flag
 *   every ten milliseconds, and fail the case if some number of seconds went
 *   by first.
 *
 *   That is not one defect but two, and the author felt both.
 *
 *   IT WAS SLOW. A ten-millisecond step goes on sleeping long after the thing
 *   it is waiting for has happened, and there are hundreds of these waits.
 *
 *   IT WAS FLAKY, which is worse. A ceiling picked at the site to be
 *   "obviously enough" -- 5 s here, 10 s there, 15, 20, 25 -- is a bet that
 *   the machine is not busy, and on a full ctest run something else in this
 *   very suite tree is always busy. One run in however many would cross one of
 *   them: a different case each time, every one of them green when re-run on
 *   its own. A green run that means nothing is worse than a red one.
 *
 *   So: ONE ceiling, and it is a minute. Nothing healthy in this tree comes
 *   within two orders of magnitude of it -- it is not a tuning parameter, it
 *   is a backstop, there so that a genuine hang fails with a message instead
 *   of wedging ctest for ever. And ONE step, and it is a millisecond, so a
 *   wait ends about when the work does.
 *
 * ===========================================================================
 * WHAT THIS IS NOT FOR
 *
 *   A case that asserts something about a RECORDING of a given length has to
 *   spend that length. The mixer renders exactly the frames wall time says are
 *   due (bus.h), so the audio costs its own duration and no seam conjures it.
 *   Those sleeps stay; they are labelled where they are, and each is as short
 *   as its assertion allows. Everything else waits for a condition, and waits
 *   for it here.
 *
 *   PREFER A HANDLE. Where the thing being waited for sets an event, wait on
 *   the event: APR_WAIT_SIGNAL ends AT the transition rather than at the next
 *   poll, and leaves nothing for load to interfere with. The polling forms
 *   below are for conditions that have no handle -- where focus ended up, what
 *   a control is displaying, what was last said.
 */
#ifndef APPRECORDER_TEST_WAIT_H
#define APPRECORDER_TEST_WAIT_H

#include <windows.h>

/* ===========================================================================
 * A STEP IS NOT A DURATION, AND COUNTING STEPS IS NOT A CLOCK.
 *
 *   These macros used to add APR_TEST_POLL_MS to a counter per iteration and
 *   call the result "milliseconds waited". It is not. Sleep(1) does not sleep
 *   one millisecond: it sleeps until the next system timer interrupt, and the
 *   default period is 15.625 ms. MEASURED, by asking the OS directly:
 *
 *       this workstation, as it is:            Sleep(1) = 13.05 ms
 *       this workstation, timeBeginPeriod(1):  Sleep(1) =  1.45 ms
 *
 *   So "ONE ceiling, and it is a minute" was a minute nowhere. 60000 counted
 *   steps at 13 ms is THIRTEEN MINUTES, and that is the whole explanation for
 *   the two numbers this tree could not account for: a case that "took 890 s
 *   under -j 8" locally, and a CI run whose 662 s were 648 s of one case
 *   burning one backstop. Neither was load; both were arithmetic.
 *
 *   Two changes, and they are separate:
 *
 *   1. THE BOUND IS READ FROM A CLOCK. GetTickCount64 across the wait, so the
 *      ceiling is the number written here whatever the timer period is and
 *      whatever else the machine is doing.
 *
 *   2. THE STEP IS MADE REAL. tests/test_runner.h raises this process's timer
 *      resolution to 1 ms for the life of the run, so a wait now ends about
 *      when the work does rather than up to 15 ms later. With hundreds of
 *      these per UI suite, that is seconds a run, and it is why the step was
 *      written as a millisecond in the first place.
 * ======================================================================== */

/* The one ceiling, and the one step. See above. */
#define APR_TEST_WAIT_MS 60000
#define APR_TEST_POLL_MS 1

/* Spin until `cond` holds; `ok` receives whether it ever did.
 *
 * A macro rather than a predicate function because every condition in these
 * suites is a couple of field reads and a comparison, and a callback plus a
 * context struct per site would bury the one line that matters. */
#define APR_WAIT_UNTIL(ok, cond) APR_WAIT_UNTIL_MS(ok, cond, APR_TEST_WAIT_MS)

/* THE BOUNDED FORM, and it exists for exactly one shape of wait: the ones
 * where the condition NOT holding is a legitimate outcome the case goes on to
 * handle -- a caret that has reached the last row and stops moving, a machine
 * with no audio session to poll, a Tab that has nowhere left to go.
 *
 * Those cannot use the minute above, because they would spend it every single
 * run. They must not be used where the condition is ASSERTED afterwards: there,
 * reaching the bound is a failure, and a bound a busy machine can reach is the
 * defect this file exists to remove. Every use of this macro sits next to a
 * comment saying which of the two it is.
 *
 * A trip count is NOT a bound. `for (i = 0; i < 40; i++) Sleep(step)` couples
 * the ceiling to the step, so making the step finer silently made the ceiling
 * shorter -- which is how a one-second wait became a forty-millisecond one and
 * a case that had never flaked started failing. Bounds are in milliseconds
 * here, and the step is nobody else's business.
 *
 * AND A COUNT OF STEPS IS NOT A BOUND EITHER, for the reason at the top of
 * this file. The elapsed time comes off a clock. */
#define APR_WAIT_UNTIL_MS(ok, cond, ms)                                       \
    do {                                                                      \
        ULONGLONG apr_t0_ = GetTickCount64();                                 \
        ULONGLONG apr_lim_ = (ULONGLONG)(ms);                                 \
        (ok) = 0;                                                             \
        for (;;) {                                                            \
            if (cond) { (ok) = 1; break; }                                    \
            if (GetTickCount64() - apr_t0_ >= apr_lim_) break;                \
            Sleep(APR_TEST_POLL_MS);                                          \
        }                                                                     \
    } while (0)

/* Wait on something another thread signals. Nonzero if it was signalled. */
#define APR_WAIT_SIGNAL(h)                                                    \
    (WaitForSingleObject((h), APR_TEST_WAIT_MS) == WAIT_OBJECT_0)

#endif /* APPRECORDER_TEST_WAIT_H */
