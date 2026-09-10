/*
 * test_window.h -- a fixture window is not a citizen of the desktop.
 *
 * ===========================================================================
 * WHY THIS FILE EXISTS
 *
 *   Eight suites in this tree build a REAL frame: a top-level window, with
 *   real panes, on the real interactive desktop of whatever machine is running
 *   the build. That is the whole point -- focus, F6, arrow keys and the
 *   accessibility tree are only worth asserting on a control the platform is
 *   actually driving. But a window on a live desktop is reachable from
 *   OUTSIDE the test, and everything that reaches it lands in the middle of an
 *   assertion:
 *
 *   ACTIVATION CLEARS THE THREAD'S FOCUS. Keyboard focus is a property of a
 *   thread's input queue, and when a thread's window is DEACTIVATED the queue
 *   loses its focus window entirely. So `GetGUIThreadInfo(tid).hwndFocus`,
 *   which is how these suites ask "where is the keyboard", starts reading
 *   NULL -- not because the product moved focus, but because something else
 *   on the desktop took the foreground. A window shown with SW_SHOWNOACTIVATE
 *   is not safe from this: Windows hands the foreground to whatever visible
 *   top-level window is available when the previous holder exits, and a ctest
 *   run is forty-one processes appearing and exiting in a row.
 *
 *   MEASURED, not reasoned about: three concurrent runs of
 *   test_ui_behaviour's Structure-panel walk failed 91 times in 360 on
 *   `ASSERT_TRUE(focus == f.h.tv)`, every one of them with focus reading
 *   NULL. With this isolation applied the same hammer fails 0 in 360.
 *
 *   AND THE PERSON AT THE MACHINE IS AN INPUT DEVICE. If the fixture window
 *   can hold the foreground, the author's own keystrokes are delivered to it:
 *   Left collapses the tree node a walk is about to descend into, a letter
 *   jumps the caret by incremental search, and a key the canvas owns edits the
 *   model the test just measured. Those do not move focus, so the focus
 *   assertions still pass and the NEXT one -- "Down reached every row" --
 *   fails instead, for a reason nothing in the log can name. A mouse resting
 *   where the window opens does the same through hot tracking and clicks.
 *
 * ===========================================================================
 * WHAT THIS DOES, AND WHY EACH HALF IS NEEDED
 *
 *   WS_EX_NOACTIVATE  -- the window can never become the foreground window,
 *                        so it can never be deactivated, so the queue's focus
 *                        is never cleared by anybody else; and no typed key is
 *                        ever routed to it.
 *   WS_EX_TOOLWINDOW  -- out of Alt+Tab, so nobody can hand it the foreground
 *                        by accident either.
 *   Moved off-screen  -- a click or a hover is delivered to a NOACTIVATE
 *                        window just the same, so put it where no pointer is.
 *                        Just BELOW the primary monitor rather than at
 *                        -32000: displays are almost always arranged side by
 *                        side, so underneath the primary is empty desktop and
 *                        the nearest-monitor rule still gives the frame the
 *                        primary monitor's DPI -- which matters, because these
 *                        suites assert on DPI-scaled metrics.
 *
 *   WS_VISIBLE IS UNTOUCHED. app.c's cycle_pane only hands focus to a VISIBLE
 *   pane, so a fixture that hid its window could never put focus in one. This
 *   window is still shown and still laid out; it is simply somewhere nobody
 *   can reach and cannot take the foreground.
 *
 *   Call it once, on the thread that owns the frame, after the frame exists
 *   and before the message loop runs.
 */
#ifndef APPRECORDER_TEST_WINDOW_H
#define APPRECORDER_TEST_WINDOW_H

#include <windows.h>
#include <string.h>

static void apr_test_isolate_frame(HWND frame)
{
    LONG_PTR    ex;
    RECT        rc;
    MONITORINFO mi;
    HMONITOR    mon;
    int         w, h;

    if (!frame || !IsWindow(frame)) return;

    ex = GetWindowLongPtrW(frame, GWL_EXSTYLE);
    SetWindowLongPtrW(frame, GWL_EXSTYLE,
                      ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);

    if (!GetWindowRect(frame, &rc)) return;
    w = (int)(rc.right - rc.left);
    h = (int)(rc.bottom - rc.top);
    if (w <= 0 || h <= 0) return;

    mon = MonitorFromWindow(frame, MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!mon || !GetMonitorInfoW(mon, &mi)) return;

    /* SWP_NOACTIVATE so the move itself does not do the one thing this
     * function exists to prevent. HWND_BOTTOM for the same reason. */
    SetWindowPos(frame, HWND_BOTTOM, (int)mi.rcMonitor.left,
                 (int)mi.rcMonitor.bottom + 8, w, h,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

#endif /* APPRECORDER_TEST_WINDOW_H */
