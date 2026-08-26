/*
 * test_discover.c -- the "what is running and what is it" queries.
 *
 * NOTHING HERE RENDERS AUDIO and nothing here opens an audio client
 * (AGENTS.md rule 1). Every function under test is an enumeration: discover.h
 * promises that explicitly, and it is the promise that lets --dry-run mean
 * something, so this suite never has to be skipped for want of hardware.
 *
 * The window-class lookup is the reason this file exists. It moved out of
 * src/session/session_load.c, where it was a private static with one caller
 * and no direct coverage at all, so the cases below pin the behaviour that
 * moved rather than trusting the lift.
 *
 * CASE ORDER IS LOAD-BEARING IN ONE PLACE and is called out where it is: the
 * "no window" case has to run before any case creates one, because every case
 * in a suite shares one process and therefore one set of windows.
 */
#include "test_runner.h"

#include "discover.h"
#include "session.h"

#include <windows.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * A window to find
 *
 * WS_VISIBLE is required -- the lookup skips invisible windows on purpose --
 * so the window is parked far off every real desktop and made one pixel
 * across. It is visible to the API and to nobody's eyes, and it lives for a
 * few milliseconds.
 * ------------------------------------------------------------------------- */

#define TEST_CLASS L"AprDiscoverTestWindowClass"

static HWND make_window(void)
{
    WNDCLASSEXW wc;
    static int  registered;

    if (!registered) {
        memset(&wc, 0, sizeof wc);
        wc.cbSize        = sizeof wc;
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.lpszClassName = TEST_CLASS;
        if (!RegisterClassExW(&wc)) return NULL;
        registered = 1;
    }
    return CreateWindowExW(0, TEST_CLASS, L"apr test", WS_POPUP | WS_VISIBLE,
                           -32000, -32000, 1, 1, NULL, NULL,
                           GetModuleHandleW(NULL), NULL);
}

/* ===========================================================================
 * The window class of a process
 * ========================================================================= */

/* FIRST, deliberately: this test process is a console application with no
 * window of its own until a later case makes one. */
TEST(a_process_with_no_window_answers_empty_rather_than_failing)
{
    /* A console application, a tray-only one and one that is still starting
     * up all look like this, and none of them is an error: the class name is
     * a tiebreaker between two instances, and a source with one instance
     * never needs one. */
    wchar_t buf[APR_DISC_CLASS_CCH];

    memset(buf, 0xff, sizeof buf);
    ASSERT_EQ_INT(0, (int)apr_process_window_class(
                         (uint32_t)GetCurrentProcessId(), buf,
                         APR_DISC_CLASS_CCH));
    /* Empty, not merely "returned 0" -- a caller that ignores the count and
     * reads the buffer must not read whatever was there before. */
    ASSERT_EQ_INT(0, (int)buf[0]);
}

TEST(a_processs_own_window_class_comes_back_by_name)
{
    wchar_t buf[APR_DISC_CLASS_CCH];
    HWND    h = make_window();
    size_t  n;

    ASSERT_NOT_NULL(h);
    memset(buf, 0xff, sizeof buf);
    n = apr_process_window_class((uint32_t)GetCurrentProcessId(),
                                 buf, APR_DISC_CLASS_CCH);
    DestroyWindow(h);

    ASSERT_EQ_INT((int)wcslen(TEST_CLASS), (int)n);
    ASSERT_WSTR_EQ(TEST_CLASS, buf);
}

TEST(a_buffer_shorter_than_the_class_name_truncates_and_stays_terminated)
{
    /* GetClassNameW truncates rather than failing, and so does this: a class
     * name compared on its first few characters still tells two instances of
     * one application apart, which is the whole job. What must never happen
     * is an unterminated buffer. */
    wchar_t small[8];
    HWND    h = make_window();
    size_t  n;

    ASSERT_NOT_NULL(h);
    memset(small, 0xff, sizeof small);
    n = apr_process_window_class((uint32_t)GetCurrentProcessId(), small, 8);
    DestroyWindow(h);

    ASSERT_EQ_INT(7, (int)n);
    ASSERT_EQ_INT(0, (int)small[7]);
    ASSERT_EQ_INT(0, wcsncmp(small, TEST_CLASS, 7));
}

TEST(a_pid_nothing_is_running_under_answers_empty)
{
    wchar_t buf[APR_DISC_CLASS_CCH];

    memset(buf, 0xff, sizeof buf);
    ASSERT_EQ_INT(0, (int)apr_process_window_class(0x7ffffffdu, buf,
                                                   APR_DISC_CLASS_CCH));
    ASSERT_EQ_INT(0, (int)buf[0]);
}

TEST(pid_zero_is_never_a_process)
{
    wchar_t buf[APR_DISC_CLASS_CCH];

    memset(buf, 0xff, sizeof buf);
    ASSERT_EQ_INT(0, (int)apr_process_window_class(0, buf, APR_DISC_CLASS_CCH));
    ASSERT_EQ_INT(0, (int)buf[0]);
    /* The System Idle Process is not something to record, and every other
     * query here agrees about that. */
    ASSERT_FALSE(apr_process_exists(0));
}

TEST(no_buffer_at_all_is_answered_rather_than_written_through)
{
    ASSERT_EQ_INT(0, (int)apr_process_window_class(
                         (uint32_t)GetCurrentProcessId(), NULL, 64));
    ASSERT_EQ_INT(0, (int)apr_process_window_class(
                         (uint32_t)GetCurrentProcessId(), NULL, 0));
}

/* ===========================================================================
 * The constant the session file's field is sized by
 * ========================================================================= */

TEST(the_class_buffer_the_session_uses_is_the_one_discover_fills)
{
    /* Two constants that merely agreed would eventually drift, and the
     * failure mode is not a truncation: a name copied wcscpy_s over-length
     * reaches the CRT's invalid parameter handler, which is a modal dialog --
     * i.e. a hang -- in a Debug build. They are ONE constant now, and this is
     * what says so out loud. */
    ASSERT_EQ_INT(APR_DISC_CLASS_CCH, APR_SESSION_CLASS_CCH);
}

/* ===========================================================================
 * The rest of the process queries, briefly. They had no direct coverage
 * either, and every one of them is on the path a listing takes.
 * ========================================================================= */

TEST(this_process_exists_and_can_name_its_own_image)
{
    wchar_t buf[APR_DISC_NAME_CCH];

    ASSERT_TRUE(apr_process_exists((uint32_t)GetCurrentProcessId()));
    ASSERT_GT_INT(0, (int)apr_process_image_name(
                         (uint32_t)GetCurrentProcessId(), buf,
                         APR_DISC_NAME_CCH));
    /* The LEAF of the path, not the path: no separator may survive. */
    ASSERT_NULL(wcschr(buf, L'\\'));
    ASSERT_NOT_NULL(wcsstr(buf, L"test_discover"));
}

TEST(a_process_tree_contains_its_own_root_first)
{
    uint32_t pids[64];
    size_t   n = 0;
    AprErr   e = apr_enum_process_tree((uint32_t)GetCurrentProcessId(),
                                       pids, 64, &n);

    ASSERT_FALSE(apr_failed(&e));
    ASSERT_GE_INT(1, (int)n);
    ASSERT_EQ_INT((int)GetCurrentProcessId(), (int)pids[0]);
}

TEST(a_root_that_is_not_running_is_not_found)
{
    uint32_t pids[8];
    size_t   n = 0;
    AprErr   e = apr_enum_process_tree(0, pids, 8, &n);

    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(0, (int)n);
}
