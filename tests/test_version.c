/*
 * test_version.c -- include/version.h and src/platform/version.c.
 *
 * SMALL, AND THE MOST LOAD-BEARING SUITE IN THE UPDATE FEATURE.
 *
 * Everything the updater does hangs on one comparison. Get it wrong in the
 * lexicographic direction and the tenth release of a line never installs --
 * silently, months later, on machines nobody is watching. That case is the
 * first one here.
 */
#include "test_runner.h"

#include "version.h"

TEST(the_string_is_built_from_the_numbers)
{
    /* The point of the stringize-then-widen in version.h: the text cannot say
     * something the integers do not.
     *
     * The expected value is FORMATTED FROM THE INTEGERS rather than written
     * out. A literal here would be exactly the second copy this module exists
     * to prevent -- and it was one, until 0.1.0: the assertion read
     * ASSERT_WSTR_EQ(L"0.0.1", ...) directly beneath a comment explaining that
     * there is only one place to edit. */
    AprVersion v = apr_version_current();
    wchar_t    want[64];

    ASSERT_EQ_INT(APR_VERSION_MAJOR, v.major);
    ASSERT_EQ_INT(APR_VERSION_MINOR, v.minor);
    ASSERT_EQ_INT(APR_VERSION_PATCH, v.patch);

    _snwprintf_s(want, 64, _TRUNCATE, L"%d.%d.%d", v.major, v.minor, v.patch);
    ASSERT_WSTR_EQ(want, APR_VERSION_STRING);
}

TEST(this_build_reports_0_1_0)
{
    /* A TRIPWIRE, ON PURPOSE, and the only hardcoded version in the suite.
     *
     * Everything else here is derived, so it survives a bump untouched. This
     * one does not, because the version is contract for the updater: it
     * decides whether every installed copy replaces itself. Making a bump cost
     * one deliberate edit in a file called test_version.c is the cheapest way
     * to ensure nobody changes it by accident, or by a careless sed.
     *
     * If you are here because this failed after a bump: check the number is
     * the one you meant, then change it. */
    AprVersion v = apr_version_current();
    ASSERT_EQ_INT(0, v.major);
    ASSERT_EQ_INT(1, v.minor);
    ASSERT_EQ_INT(0, v.patch);
}

/* ===========================================================================
 * THE CASE A STRING COMPARE GETS WRONG
 * ========================================================================= */

TEST(ten_is_newer_than_nine_which_a_string_compare_denies)
{
    AprVersion a, b;

    ASSERT_TRUE(apr_version_parse(L"0.10.0", &a));
    ASSERT_TRUE(apr_version_parse(L"0.9.0", &b));
    /* wcscmp(L"0.10.0", L"0.9.0") < 0. If this suite ever fails, an updater
     * that has been running for a year has quietly stopped updating. */
    ASSERT_GT_INT(0, apr_version_compare(&a, &b));
    ASSERT_LT_INT(0, apr_version_compare(&b, &a));

    ASSERT_TRUE(apr_version_parse(L"1.0.10", &a));
    ASSERT_TRUE(apr_version_parse(L"1.0.9", &b));
    ASSERT_GT_INT(0, apr_version_compare(&a, &b));

    ASSERT_TRUE(apr_version_parse(L"10.0.0", &a));
    ASSERT_TRUE(apr_version_parse(L"9.99.99", &b));
    ASSERT_GT_INT(0, apr_version_compare(&a, &b));
}

TEST(fields_are_compared_most_significant_first)
{
    AprVersion a, b;

    ASSERT_TRUE(apr_version_parse(L"1.0.0", &a));
    ASSERT_TRUE(apr_version_parse(L"0.99.99", &b));
    ASSERT_GT_INT(0, apr_version_compare(&a, &b));

    ASSERT_TRUE(apr_version_parse(L"0.2.0", &a));
    ASSERT_TRUE(apr_version_parse(L"0.1.99", &b));
    ASSERT_GT_INT(0, apr_version_compare(&a, &b));

    ASSERT_TRUE(apr_version_parse(L"1.2.3", &a));
    ASSERT_TRUE(apr_version_parse(L"1.2.3", &b));
    ASSERT_EQ_INT(0, apr_version_compare(&a, &b));
}

/* ===========================================================================
 * Parsing
 * ========================================================================= */

TEST(a_leading_v_is_accepted_because_that_is_how_a_tag_is_named)
{
    AprVersion a, b;

    ASSERT_TRUE(apr_version_parse(L"v1.2.3", &a));
    ASSERT_TRUE(apr_version_parse(L"1.2.3", &b));
    ASSERT_EQ_INT(0, apr_version_compare(&a, &b));

    ASSERT_TRUE(apr_version_parse(L"V0.0.1", &a));
    ASSERT_EQ_INT(1, a.patch);
}

TEST(anything_that_is_not_three_numbers_is_refused)
{
    AprVersion v;

    /* Each of these would, if accepted as 0.0.0 or as a truncation, compare
     * as OLDER than everything -- which turns a corrupt manifest into a
     * cheerful "you are up to date" instead of into a refusal. */
    ASSERT_FALSE(apr_version_parse(NULL, &v));
    ASSERT_FALSE(apr_version_parse(L"", &v));
    ASSERT_FALSE(apr_version_parse(L"1", &v));
    ASSERT_FALSE(apr_version_parse(L"1.2", &v));
    ASSERT_FALSE(apr_version_parse(L"1.2.3.4", &v));
    ASSERT_FALSE(apr_version_parse(L"1.2.3-rc1", &v));
    ASSERT_FALSE(apr_version_parse(L"1.2.3 ", &v));
    ASSERT_FALSE(apr_version_parse(L" 1.2.3", &v));
    ASSERT_FALSE(apr_version_parse(L"a.b.c", &v));
    ASSERT_FALSE(apr_version_parse(L"1..3", &v));
    ASSERT_FALSE(apr_version_parse(L"-1.2.3", &v));
    ASSERT_FALSE(apr_version_parse(L"999999999.0.0", &v));
    ASSERT_FALSE(apr_version_parse(L"v", &v));
}

TEST(a_refused_parse_zeroes_the_output)
{
    AprVersion v;

    v.major = 7; v.minor = 7; v.patch = 7;
    ASSERT_FALSE(apr_version_parse(L"not a version", &v));
    ASSERT_EQ_INT(0, v.major);
    ASSERT_EQ_INT(0, v.minor);
    ASSERT_EQ_INT(0, v.patch);
}

/* ===========================================================================
 * The one question the updater asks
 * ========================================================================= */

TEST(newer_than_current_is_the_whole_decision)
{
    /* Built from the current version rather than written out, so a bump does
     * not silently turn these into assertions about nothing. They were
     * literals until 0.1.0, and "0.0.2 is newer" quietly became false the
     * moment the minor moved -- which the suite caught, but only because it
     * was checking the literal it was about to stop being true. */
    AprVersion v = apr_version_current();
    wchar_t    up_major[64], up_minor[64], up_patch[64], with_v[64];

    _snwprintf_s(up_major, 64, _TRUNCATE, L"%d.%d.%d", v.major + 1, 0, 0);
    _snwprintf_s(up_minor, 64, _TRUNCATE, L"%d.%d.%d", v.major, v.minor + 1, 0);
    _snwprintf_s(up_patch, 64, _TRUNCATE, L"%d.%d.%d",
                 v.major, v.minor, v.patch + 1);
    /* The leading v a git tag carries, on a version ten minors ahead -- the
     * lexicographic trap, in the form the updater actually receives it. */
    _snwprintf_s(with_v, 64, _TRUNCATE, L"v%d.%d.%d",
                 v.major, v.minor + 10, 0);

    ASSERT_TRUE(apr_version_is_newer_than_current(up_patch));
    ASSERT_TRUE(apr_version_is_newer_than_current(up_minor));
    ASSERT_TRUE(apr_version_is_newer_than_current(up_major));
    ASSERT_TRUE(apr_version_is_newer_than_current(with_v));

    ASSERT_FALSE(apr_version_is_newer_than_current(APR_VERSION_STRING));
    ASSERT_FALSE(apr_version_is_newer_than_current(L"0.0.0"));

    /* UNPARSABLE IS NOT NEWER, and it is not older either -- it is simply not
     * an answer. A build that treated it as newer would offer to install
     * whatever a broken or hostile manifest said. */
    ASSERT_FALSE(apr_version_is_newer_than_current(L"latest"));
    ASSERT_FALSE(apr_version_is_newer_than_current(L""));
    ASSERT_FALSE(apr_version_is_newer_than_current(NULL));
    ASSERT_FALSE(apr_version_is_newer_than_current(L"99.99.99-beta"));
}

TEST(compare_survives_null)
{
    AprVersion v = apr_version_current();
    ASSERT_EQ_INT(0, apr_version_compare(NULL, &v));
    ASSERT_EQ_INT(0, apr_version_compare(&v, NULL));
    ASSERT_EQ_INT(0, apr_version_compare(NULL, NULL));
    ASSERT_FALSE(apr_version_parse(L"1.2.3", NULL));
}
