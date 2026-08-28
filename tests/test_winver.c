/*
 * test_winver.c -- the Windows floor.
 *
 * The interesting half of the version check is here rather than in either
 * front end: reading the REAL build number and comparing it to the floor.
 * What the front ends add is a print and a return.
 *
 * There is no way to make this machine report a different build, so the tests
 * assert the properties that hold whatever it reports -- plus the one fact
 * that matters most and is easy to get wrong: the number must be the OS's,
 * not the compatibility manifest's.
 */
#include "test_runner.h"

#include <windows.h>

#include "winver.h"

TEST(the_floor_is_the_build_that_gained_process_loopback)
{
    /* Not a preference. 19041 is Windows 10 version 2004, where
     * ActivateAudioInterfaceAsync gained
     * AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK (design 4.1). If this
     * number ever changes, something has been misunderstood. */
    ASSERT_EQ_INT(19041, (int)APR_WIN_MIN_BUILD);
}

TEST(the_build_number_is_a_real_windows_build)
{
    uint32_t b = apr_win_build();

    /* Not zero (ntdll answered), and inside the range of numbers Windows has
     * actually used -- a bad cast or a checked/free flag left in the high bits
     * would land far outside it. */
    ASSERT_TRUE(b != 0);
    ASSERT_TRUE(b >= 10240u);        /* the first Windows 10 build */
    ASSERT_TRUE(b < 1000000u);       /* no flag bits survived */
}

TEST(the_build_number_is_the_os_and_not_the_manifest)
{
    /* THE POINT OF THE WHOLE MODULE. GetVersionExW reports what the
     * compatibility manifest claims; RtlGetNtVersionNumbers reports the
     * machine. They agree only when the manifest happens to list the running
     * OS, so this asserts the weaker thing that is always true: our number is
     * never SMALLER than the one the manifest-filtered API admits to.
     *
     * On a machine newer than the manifest's newest supportedOS entry, the
     * two differ and this is the assertion that would catch a switch back to
     * GetVersionExW. */
    OSVERSIONINFOEXW osv;
    memset(&osv, 0, sizeof osv);
    osv.dwOSVersionInfoSize = sizeof osv;

#pragma warning(push)
#pragma warning(disable : 4996)   /* GetVersionExW is deprecated; that is the subject */
    if (GetVersionExW((OSVERSIONINFOW *)&osv)) {
#pragma warning(pop)
        ASSERT_TRUE(apr_win_build() >= osv.dwBuildNumber);
    }
}

TEST(meeting_the_floor_agrees_with_the_build_number)
{
    uint32_t b = apr_win_build();
    int met = apr_win_meets_floor();

    if (b >= APR_WIN_MIN_BUILD) {
        ASSERT_TRUE(met);
    } else {
        ASSERT_FALSE(met);
    }
}

TEST(the_answer_is_stable_across_calls)
{
    /* Cached after the first call, and the cache is what makes it safe to ask
     * from any thread without a lock. A second answer that differed would mean
     * the cache is not doing its job. */
    uint32_t a = apr_win_build();
    uint32_t b = apr_win_build();
    uint32_t c = apr_win_build();

    ASSERT_EQ_INT((int)a, (int)b);
    ASSERT_EQ_INT((int)b, (int)c);
}

TEST(this_machine_can_actually_run_apprecorder)
{
    /* Not a tautology: if this ever fails, the suite is running somewhere the
     * product refuses to start, and every capture test below is measuring
     * something the user could never reach. */
    ASSERT_TRUE(apr_win_meets_floor());
}
