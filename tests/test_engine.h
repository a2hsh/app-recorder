/*
 * test_engine.h -- "is there an audio engine here, and has it started feeding
 * us yet?", in one place.
 *
 * ===========================================================================
 * WHY THIS FILE EXISTS
 *
 *   Design section 11: nothing in CI may depend on hardware being present. So
 *   every case that needs a live audio engine skips where there is none -- and
 *   each of the four suites that does so had written that check by hand, with
 *   a different idea of what "there is none" means. One of them was wrong, and
 *   it failed on the first CI runner it ever met:
 *
 *       tests/test_capture_apartment.c(244): FAILED ASSERT_TRUE(sta.frames > 0)
 *
 *   THE RUNNER DID HAVE AN ENGINE. It has no capture endpoint at all (the
 *   device case skipped, correctly, with "Element not found"), but process
 *   loopback activated, started, and delivered frames -- the very next case in
 *   the same file asserted frames > 0 on the same runner and passed. What the
 *   failing case actually measured was that the FIRST tap on a cold engine
 *   took longer than 120 ms to produce its first packet, and 120 ms was a
 *   Sleep, not a wait. The case took 1,034 ms on the runner; the second one,
 *   with the engine now warm, took 248 ms.
 *
 *   So there are two separate questions and the old code asked neither:
 *
 *     1. HAS THE FIRST FRAME ARRIVED YET? Wait for it, do not sleep a number
 *        somebody guessed. apr_test_wait_for_frames() below.
 *     2. WILL IT EVER? If the engine never feeds this machine, skip -- loudly,
 *        naming the reason. apr_test_skip_capture() below.
 *
 * ===========================================================================
 * AND THE RULE THAT MATTERS MOST: A SKIP PREDICATE MUST NOT BE ABLE TO HIDE
 * THE DEFECT UNDER TEST.
 *
 *   The apartment suite exists because process loopback used to refuse an STA
 *   caller, which is the apartment the window is necessarily in. If "is there
 *   an engine here?" were asked FROM an STA, that refusal would come back as
 *   a failure to open, the case would call it "no engine on this machine" and
 *   skip, and the regression it was written to catch would pass silently --
 *   which is exactly how the original defect survived a green suite.
 *
 *   Therefore: the capability probe is always run on a thread that is NOT the
 *   thread under test, in the apartment the old code was already happy with.
 *   The helpers here take no apartment of their own and make no COM calls;
 *   they read a status struct. Where the apartment is the subject, the caller
 *   runs the probe itself on a reference thread -- see test_capture_apartment.c.
 *
 * Include AFTER test_runner.h (SKIP) and test_wait.h (APR_WAIT_UNTIL_MS).
 */
#ifndef APPRECORDER_TEST_ENGINE_H
#define APPRECORDER_TEST_ENGINE_H

#include "capture.h"
#include "err.h"

#include <windows.h>

/* How long a cold audio engine is given to produce its first packet before the
 * machine is declared to have none.
 *
 * NOT A TUNING KNOB, and not a number a busy machine can cross by being busy:
 * the healthy case on a warm engine is tens of milliseconds and the worst
 * MEASURED cold start is a second. This is the backstop that turns "there is
 * no engine here" into a skip instead of a hang, and the only cost of it being
 * generous is paid on a machine that was going to skip anyway. */
#define APR_TEST_ENGINE_FIRST_FRAME_MS 15000

/* Skip, naming the reason AND the HRESULT behind it. Both go into the skip log
 * (test_runner.h), so a skip on a CI runner can be read without re-running
 * anything -- "no engine" and "activation returned E_NOINTERFACE" are not the
 * same news.
 *
 * The file and line recorded are the CALLER'S, which is why this is a macro
 * over a function rather than a function on its own: a skip log that named
 * this header would point every reader at the same line of shared plumbing
 * instead of at the case that did not run. */
static void apr_test_skip_capture_at(const char *file, int line,
                                     const char *why, const AprErr *e)
{
    wchar_t buf[512];
    char    msg[768];

    if (e && apr_failed(e)) {
        apr_err_format(e, buf, 512);
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "%s -- %ls", why, buf);
        tr_skip(file, line, msg);
    } else {
        tr_skip(file, line, why);
    }
}

#define apr_test_skip_capture(why, e) \
    apr_test_skip_capture_at(__FILE__, __LINE__, (why), (e))

/* Wait, bounded, for the tap to deliver its first frame. Returns the frame
 * count, which is zero if the engine never fed it.
 *
 * BOUNDED, and zero is a legitimate outcome the caller goes on to handle --
 * that is the shape test_wait.h's bounded form is for. The caller decides
 * whether zero means "skip, no engine" or "fail, the engine is there and this
 * source is broken"; this function does not decide it. */
static uint64_t apr_test_wait_for_frames(AprCapture *c, unsigned ms)
{
    AprCaptureStatus st;
    int got;

    if (!c) return 0;
    memset(&st, 0, sizeof st);

    APR_WAIT_UNTIL_MS(got,
                      (c->vt->status(c, &st), st.frames_written > 0),
                      (int)ms);
    (void)got;
    return st.frames_written;
}

#endif /* APPRECORDER_TEST_ENGINE_H */
