/*
 * test_harness.c -- the test harness testing itself.
 *
 * A harness that silently reports success is worse than no harness, so the
 * first thing verified in this repo is that a failing assertion is actually
 * observed and reported.
 */
#include "test_runner.h"

/* Deliberately failing assertion, isolated in a helper so the ASSERT's
 * early-return does not abandon the case that is checking it. */
static void tr_selftest_negative(void)
{
    ASSERT_EQ_INT(1, 2);
}

TEST(a_failing_assertion_is_counted_and_reported)
{
    int observed;

    printf("      note: the FAILED line below is the self-test's own bait\n");
    g_tr_case_failed = 0;
    tr_selftest_negative();
    observed = g_tr_case_failed;
    g_tr_case_failed = 0;          /* absorb the deliberate failure */

    ASSERT_EQ_INT(1, observed);
}

TEST(integer_assertions)
{
    int      i = -7;
    uint64_t u = 18446744073709551615ull;

    ASSERT_EQ_INT(-7, i);
    ASSERT_NE_INT(8, i);
    ASSERT_LT_INT(0, i);
    ASSERT_LE_INT(-7, i);
    ASSERT_GT_INT(-100, i);
    ASSERT_GE_INT(-7, i);
    ASSERT_EQ_U64(18446744073709551615ull, u);
}

TEST(boolean_assertions)
{
    int one = 1;

    ASSERT_TRUE(one + one == 2);
    ASSERT_FALSE(one + one == 3);
}

TEST(float_assertions_use_an_explicit_epsilon)
{
    double sum = 0.1 + 0.2;
    ASSERT_NEAR(0.3, sum, 1e-12);
    ASSERT_FALSE(sum == 0.3);      /* which is exactly why epsilon exists */
}

TEST(pointer_assertions)
{
    int   x   = 0;
    int  *p   = &x;
    int  *nil = NULL;

    ASSERT_NOT_NULL(p);
    ASSERT_NULL(nil);
}

TEST(memory_and_string_assertions)
{
    unsigned char a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    unsigned char b[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    ASSERT_MEM_EQ(a, b, sizeof a);
    ASSERT_STR_EQ("apprecorder", "apprecorder");
    ASSERT_WSTR_EQ(L"apprecorder", L"apprecorder");
}

TEST(every_case_in_this_file_registered)
{
    /* If self-registration breaks (e.g. /OPT:REF drops the .CRT$XCU slot in a
     * Release build) the count collapses and this catches it. */
    ASSERT_EQ_INT(7, g_tr_test_count);
}
