/*
 * test_strings.c -- the localization layer, including the build-time
 * completeness check.
 *
 * THE COMPLETENESS CHECK IS THE POINT OF THIS FILE.
 *
 * `every_id_resolves_in_every_complete_language` walks the same two X-macro
 * lists the .rc is generated from and probes the resource ACTUALLY LINKED INTO
 * THIS BINARY, one exact language at a time with no fallback. Because ctest
 * runs it, `build.cmd Debug test` fails when a language declared
 * APR_STR_COMPLETE is missing a string. That is the "must fail the build, not
 * ship as an empty string" requirement, and it is checked against the real
 * resource rather than against a parse of the .rc source -- so it also catches
 * an entry the resource compiler dropped, a mis-numbered id, and a language
 * block that silently did not get emitted.
 *
 * These tests set the language explicitly at the top of every case: the module
 * holds process-wide state and cases share a process.
 */
#include "test_runner.h"

#include "errmsg.h"
#include "strings.h"

#define EN MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)
#define AR MAKELANGID(LANG_ARABIC,  SUBLANG_ARABIC_SAUDI_ARABIA)

#define BUF 512

/* The Arabic marker in res/strings.rc, spelled out in code points.
 *
 * NOT written as a wide literal on purpose: this .c file is UTF-8 without a
 * BOM, MSVC would read it in the system ANSI code page, and the test would
 * then be comparing one mis-decoded string against another and passing. Code
 * points are the only spelling that actually pins the resource compiler's
 * UTF-8 -> UTF-16 conversion. */
static const wchar_t k_ar_app_name[] = {
    L'A', L'R', L'-', L'P', L'L', L'A', L'C', L'E', L'H', L'O', L'L', L'D',
    L'E', L'R', L' ', 0x062A, 0x062C, 0x0631, 0x0628, 0x0629, 0
};

/* Every id a catalog entry occupies: one, or six for a plural. */
static int entry_ids(const AprStrEntry *e, AprStrId *out)
{
    int i;
    if (!e->is_plural) {
        out[0] = e->id;
        return 1;
    }
    for (i = 0; i < APR_PLURAL_COUNT; i++)
        out[i] = (AprStrId)((int)e->id + i);
    return APR_PLURAL_COUNT;
}

static const char *category_name(int cat)
{
    switch (cat) {
    case APR_PLURAL_ZERO:  return "zero";
    case APR_PLURAL_ONE:   return "one";
    case APR_PLURAL_TWO:   return "two";
    case APR_PLURAL_FEW:   return "few";
    case APR_PLURAL_MANY:  return "many";
    case APR_PLURAL_OTHER: return "other";
    default:               return "?";
    }
}

/* ===========================================================================
 * The completeness check
 * ========================================================================= */

TEST(catalog_and_language_tables_are_populated)
{
    int i;

    ASSERT_GT_INT(0, apr_str_catalog_count());
    ASSERT_GT_INT(1, apr_str_language_count());   /* at least English + one */

    for (i = 0; i < apr_str_catalog_count(); i++) {
        const AprStrEntry *e = apr_str_catalog_at(i);
        ASSERT_NOT_NULL(e);
        ASSERT_NOT_NULL(e->name);
        ASSERT_GE_INT(APR_STR_ID_MIN, (int)e->id);
        /* A plural must leave room for all six forms below the ceiling. */
        ASSERT_GT_INT((int)e->id + (e->is_plural ? APR_PLURAL_OTHER : 0),
                      APR_STR_ID_MAX);
    }

    ASSERT_NULL(apr_str_catalog_at(-1));
    ASSERT_NULL(apr_str_catalog_at(apr_str_catalog_count()));
    ASSERT_NULL(apr_str_language_at(-1));
    ASSERT_NULL(apr_str_language_at(apr_str_language_count()));
}

TEST(no_two_catalog_entries_share_an_id)
{
    /* A duplicated id in APR_STR_LIST is invisible in C (both enumerators just
     * exist) and silently makes two strings collide in the resource. */
    int i, j, ni, nj, a, b;
    AprStrId ids_i[APR_PLURAL_COUNT], ids_j[APR_PLURAL_COUNT];

    for (i = 0; i < apr_str_catalog_count(); i++) {
        ni = entry_ids(apr_str_catalog_at(i), ids_i);
        for (j = i + 1; j < apr_str_catalog_count(); j++) {
            nj = entry_ids(apr_str_catalog_at(j), ids_j);
            for (a = 0; a < ni; a++)
                for (b = 0; b < nj; b++)
                    if (ids_i[a] == ids_j[b]) {
                        printf("      %s and %s both use id %d\n",
                               apr_str_catalog_at(i)->name,
                               apr_str_catalog_at(j)->name, (int)ids_i[a]);
                        FAIL("duplicate string id");
                    }
        }
    }
    ASSERT_EQ_INT(0, 0);
}

TEST(every_id_resolves_in_every_complete_language)
{
    wchar_t  buf[BUF];
    AprStrId ids[APR_PLURAL_COUNT];
    int      li, ci, k, n, missing = 0;

    for (li = 0; li < apr_str_language_count(); li++) {
        const AprStrLanguage *lang = apr_str_language_at(li);
        if (lang->coverage != APR_STR_COMPLETE)
            continue;

        for (ci = 0; ci < apr_str_catalog_count(); ci++) {
            const AprStrEntry *e = apr_str_catalog_at(ci);
            n = entry_ids(e, ids);
            for (k = 0; k < n; k++) {
                int cch = apr_str_probe(lang->langid, ids[k], buf, BUF);
                if (cch < 1) {
                    missing++;
                    printf("      MISSING [%ls] %s%s%s (id %d)\n",
                           lang->tag, e->name,
                           e->is_plural ? "." : "",
                           e->is_plural ? category_name(k) : "",
                           (int)ids[k]);
                }
            }
        }
    }

    if (missing) {
        printf("      %d string(s) missing from a language declared "
               "APR_STR_COMPLETE.\n", missing);
        printf("      Add the text to res/strings.rc, or -- if the language "
               "is not finished --\n");
        printf("      set its coverage to APR_STR_PARTIAL in "
               "src/i18n/strings.c.\n");
    }
    ASSERT_EQ_INT(0, missing);
}

/* Nonzero when `id` is a plain (non-plural) entry declared in the catalog. */
static int is_declared(AprStrId id)
{
    int ci;
    for (ci = 0; ci < apr_str_catalog_count(); ci++) {
        const AprStrEntry *e = apr_str_catalog_at(ci);
        if (!e->is_plural && e->id == id) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * The same completeness question, asked of FAILURES
 *
 * The check above proves every declared id has text. This one proves the
 * other direction for the one place the catalog can be bypassed: an error.
 * Every AprErr that reaches a user has to name a DECLARED id, or the sentence
 * a screen reader reads falls back to a literal in C -- which is BUGS.md M11,
 * and which no amount of .rc completeness would catch.
 * ------------------------------------------------------------------------- */

TEST(every_error_kind_names_a_declared_catalog_sentence)
{
    int      k, bad = 0;
    wchar_t  buf[BUF];

    apr_str_set_language(EN);

    /* Every value of the enum, plus one past it: an AprErrKind that nobody
     * has written a sentence for must still resolve, or a future kind ships
     * with nothing to say. */
    for (k = APR_OK; k <= (int)APR_E_BUSY + 1; k++) {
        AprErr   e;
        AprStrId id;

        memset(&e, 0, sizeof e);
        e.kind = (AprErrKind)k;
        e.line = __LINE__;
        e.file = __FILE__;
        e.func = __func__;

        id = apr_err_reason_id(&e);

        if (k == APR_OK) {
            /* Success is not a reason and must never render as one. */
            if (id != APR_S__NONE) {
                printf("      APR_OK resolved to id %d\n", (int)id);
                bad++;
            }
            if (apr_err_reason(&e, buf, BUF) != 0) bad++;
            continue;
        }

        if (!is_declared(id)) {
            printf("      kind %ls resolves to id %d, which is not "
                   "declared in the catalog\n",
                   apr_err_kind_name((AprErrKind)k), (int)id);
            bad++;
            continue;
        }
        if (apr_str_probe(EN, id, buf, BUF) < 1) {
            printf("      kind %ls resolves to id %d, which has no English "
                   "text\n", apr_err_kind_name((AprErrKind)k), (int)id);
            bad++;
        }
    }
    ASSERT_EQ_INT(0, bad);
}

TEST(every_hand_tabled_wasapi_code_names_a_declared_catalog_sentence)
{
    int i, bad = 0;
    wchar_t buf[BUF];

    apr_str_set_language(EN);

    /* Windows ships no message resource for facility 0x889, so these are the
     * only failure sentences apprecorder writes itself -- and therefore the
     * only ones that can be left behind in English. A row added to err.c's
     * table without a catalog id fails HERE, at build time, rather than in an
     * Arabic UI. */
    ASSERT_GT_INT(30, apr_err_hr_table_count());

    for (i = 0; i < apr_err_hr_table_count(); i++) {
        const AprErrHrEntry *row = apr_err_hr_table_at(i);
        ASSERT_NOT_NULL(row);
        if (!is_declared(row->reason)) {
            printf("      %ls has no catalog id\n", row->name);
            bad++;
            continue;
        }
        if (apr_str_probe(EN, row->reason, buf, BUF) < 1) {
            printf("      %ls resolves to id %d, which has no English text\n",
                   row->name, (int)row->reason);
            bad++;
        }
    }
    ASSERT_EQ_INT(0, bad);
}

TEST(a_partial_language_declares_exactly_what_it_has)
{
    /* Arabic is deliberately incomplete: the mechanism is wired, the copy is
     * not written. This case pins the two entries that DO exist, so that
     * "Arabic works" is proven rather than assumed, and reports the rest --
     * that count is the size of the translation pass. */
    wchar_t buf[BUF];
    int     ci, k, n, present = 0, absent = 0;
    AprStrId ids[APR_PLURAL_COUNT];

    for (ci = 0; ci < apr_str_catalog_count(); ci++) {
        const AprStrEntry *e = apr_str_catalog_at(ci);
        n = entry_ids(e, ids);
        for (k = 0; k < n; k++)
            if (apr_str_probe(AR, ids[k], buf, BUF) >= 1) present++;
            else                                          absent++;
    }

    printf("      ar-SA: %d translated, %d awaiting translation\n",
           present, absent);

    /* APP_NAME, all six forms of N_SOURCES, and one error reason.
     *
     * The third marker is ERR_HR_E_DEVICE_INVALIDATED, and it earns its place
     * the same way the other two do: it is the only way to prove that an
     * error raised on a capture pump, travelling as an identity, comes out of
     * the catalog block for the language actually in effect. Without it the
     * whole M11 mechanism is untestable in a non-English locale, because an
     * untranslated Arabic entry falls back to English and passes. */
    ASSERT_EQ_INT(8, present);
    ASSERT_GT_INT(0, absent);

    ASSERT_GE_INT(1, apr_str_probe(AR, APR_S_APP_NAME, buf, BUF));
    ASSERT_GE_INT(1, apr_str_probe(AR, APR_S_ERR_HR_E_DEVICE_INVALIDATED,
                                   buf, BUF));
    ASSERT_EQ_INT(-1, apr_str_probe(AR, APR_S_APP_TAGLINE, buf, BUF));
    ASSERT_EQ_INT(-1, apr_str_probe(AR, APR_S_ERR_FILE_OPEN, buf, BUF));
}

TEST(probing_a_language_never_falls_back_to_another)
{
    /* If this ever regresses, the completeness check above silently stops
     * checking anything: every Arabic probe would succeed by returning the
     * English string. */
    wchar_t ar[BUF], en[BUF];

    ASSERT_EQ_INT(-1, apr_str_probe(AR, APR_S_APP_TAGLINE, ar, BUF));
    ASSERT_GE_INT(1, apr_str_probe(EN, APR_S_APP_TAGLINE, en, BUF));

    /* A language with no block at all resolves to nothing, not to English. */
    ASSERT_EQ_INT(-1, apr_str_probe(MAKELANGID(LANG_FRENCH, SUBLANG_FRENCH),
                                    APR_S_APP_NAME, ar, BUF));
}

TEST(arabic_text_survives_the_utf8_to_utf16_resource_path)
{
    /* rc.exe reading a UTF-8 .rc as the system ANSI code page turns Arabic
     * into mojibake at resource-compile time, where no later test would
     * notice. Exact code points are the only honest check. The marker word is
     * placeholder text, not copy. */
    wchar_t buf[BUF];
    int     cch = apr_str_probe(AR, APR_S_APP_NAME, buf, BUF);

    ASSERT_EQ_INT(20, cch);
    ASSERT_EQ_INT(0x0041, buf[0]);   /* 'A' of "AR-PLACEHOLDER " */
    ASSERT_EQ_INT(0x062A, buf[15]);  /* TEH     */
    ASSERT_EQ_INT(0x062C, buf[16]);  /* JEEM    */
    ASSERT_EQ_INT(0x0631, buf[17]);  /* REH     */
    ASSERT_EQ_INT(0x0628, buf[18]);  /* BEH     */
    ASSERT_EQ_INT(0x0629, buf[19]);  /* TEH MARBUTA */
    ASSERT_EQ_INT(0, buf[20]);
}

/* ===========================================================================
 * Plural rules
 * ========================================================================= */

TEST(arabic_selects_all_six_cldr_categories)
{
    /* The two rules that are always got wrong are few (n%100 in 3..10) and
     * many (n%100 in 11..99), and the values that catch a wrong
     * implementation are the ones just past a hundred. */
    static const struct { int64_t n; int cat; } cases[] = {
        {   0, APR_PLURAL_ZERO  },
        {   1, APR_PLURAL_ONE   },
        {   2, APR_PLURAL_TWO   },
        {   3, APR_PLURAL_FEW   },
        {   9, APR_PLURAL_FEW   },
        {  10, APR_PLURAL_FEW   },
        {  11, APR_PLURAL_MANY  },
        {  26, APR_PLURAL_MANY  },
        {  99, APR_PLURAL_MANY  },
        { 100, APR_PLURAL_OTHER },   /* 100 % 100 == 0: neither few nor many */
        { 101, APR_PLURAL_OTHER },
        { 102, APR_PLURAL_OTHER },
        { 103, APR_PLURAL_FEW   },
        { 110, APR_PLURAL_FEW   },
        { 111, APR_PLURAL_MANY  },
        { 199, APR_PLURAL_MANY  },
        { 200, APR_PLURAL_OTHER },
        { 202, APR_PLURAL_OTHER },
        { 203, APR_PLURAL_FEW   },
        { 211, APR_PLURAL_MANY  },
        {1000, APR_PLURAL_OTHER },
        {1002, APR_PLURAL_OTHER },
        {1003, APR_PLURAL_FEW   },
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int got = (int)apr_plural_category(AR, cases[i].n);
        if (got != cases[i].cat) {
            printf("      n=%lld expected %s, got %s\n",
                   (long long)cases[i].n, category_name(cases[i].cat),
                   category_name(got));
        }
        ASSERT_EQ_INT(cases[i].cat, got);
    }
}

TEST(arabic_sweeps_the_whole_first_thousand_consistently)
{
    /* A table can be written to match a buggy implementation. This derives the
     * expectation from the CLDR text independently. */
    int64_t n;
    for (n = 0; n <= 1200; n++) {
        int64_t m = n % 100;
        int expect;
        if (n == 0)                     expect = APR_PLURAL_ZERO;
        else if (n == 1)                expect = APR_PLURAL_ONE;
        else if (n == 2)                expect = APR_PLURAL_TWO;
        else if (m >= 3 && m <= 10)     expect = APR_PLURAL_FEW;
        else if (m >= 11 && m <= 99)    expect = APR_PLURAL_MANY;
        else                            expect = APR_PLURAL_OTHER;

        if ((int)apr_plural_category(AR, n) != expect) {
            printf("      first mismatch at n=%lld\n", (long long)n);
            ASSERT_EQ_INT(expect, (int)apr_plural_category(AR, n));
            return;
        }
    }
    ASSERT_EQ_INT(APR_PLURAL_OTHER, (int)apr_plural_category(AR, 1200));
}

TEST(english_uses_two_of_the_six_slots)
{
    ASSERT_EQ_INT(APR_PLURAL_OTHER, (int)apr_plural_category(EN, 0));
    ASSERT_EQ_INT(APR_PLURAL_ONE,   (int)apr_plural_category(EN, 1));
    ASSERT_EQ_INT(APR_PLURAL_OTHER, (int)apr_plural_category(EN, 2));
    ASSERT_EQ_INT(APR_PLURAL_OTHER, (int)apr_plural_category(EN, 3));
    ASSERT_EQ_INT(APR_PLURAL_OTHER, (int)apr_plural_category(EN, 11));
    ASSERT_EQ_INT(APR_PLURAL_OTHER, (int)apr_plural_category(EN, 100));
    ASSERT_EQ_INT(APR_PLURAL_OTHER, (int)apr_plural_category(EN, 101));
    ASSERT_EQ_INT(APR_PLURAL_OTHER, (int)apr_plural_category(EN, 103));
    ASSERT_EQ_INT(APR_PLURAL_OTHER, (int)apr_plural_category(EN, 111));
}

TEST(an_undeclared_language_uses_the_one_other_rule)
{
    ASSERT_EQ_INT(APR_PLURAL_ONE,   (int)apr_plural_category(
                      MAKELANGID(LANG_FRENCH, SUBLANG_FRENCH), 1));
    ASSERT_EQ_INT(APR_PLURAL_OTHER, (int)apr_plural_category(
                      MAKELANGID(LANG_FRENCH, SUBLANG_FRENCH), 3));
}

TEST(negative_counts_use_the_absolute_value)
{
    /* CLDR's n operand is |n|. -3 items lost is still "few" in Arabic. */
    ASSERT_EQ_INT(APR_PLURAL_FEW,  (int)apr_plural_category(AR, -3));
    ASSERT_EQ_INT(APR_PLURAL_MANY, (int)apr_plural_category(AR, -11));
    ASSERT_EQ_INT(APR_PLURAL_ONE,  (int)apr_plural_category(EN, -1));
    /* INT64_MIN cannot be negated in the signed domain, so the rule has to
     * reach |n| through the unsigned one. |INT64_MIN| is
     * 9223372036854775808, whose last two digits are 08 -- CLDR few, not
     * other. Checked because getting here by negating a signed value is
     * undefined behaviour that happens to "work" until it does not. */
    ASSERT_EQ_INT(APR_PLURAL_FEW,
                  (int)apr_plural_category(AR, (int64_t)0x8000000000000000ull));
}

/* ===========================================================================
 * Plural strings against the real resources
 * ========================================================================= */

TEST(arabic_plurals_select_the_matching_resource)
{
    static const struct { int64_t n; const wchar_t *want; } cases[] = {
        {   0, L"AR-PLACEHOLDER zero %1!s!"  },
        {   1, L"AR-PLACEHOLDER one %1!s!"   },
        {   2, L"AR-PLACEHOLDER two %1!s!"   },
        {   3, L"AR-PLACEHOLDER few %1!s!"   },
        {  10, L"AR-PLACEHOLDER few %1!s!"   },
        {  11, L"AR-PLACEHOLDER many %1!s!"  },
        {  99, L"AR-PLACEHOLDER many %1!s!"  },
        { 100, L"AR-PLACEHOLDER other %1!s!" },
        { 101, L"AR-PLACEHOLDER other %1!s!" },
        { 103, L"AR-PLACEHOLDER few %1!s!"   },
        { 111, L"AR-PLACEHOLDER many %1!s!"  },
    };
    size_t i;

    apr_str_set_language(AR);
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++)
        ASSERT_WSTR_EQ(cases[i].want, apr_str_plural(APR_S_N_SOURCES, cases[i].n));
    apr_str_set_language(EN);
}

TEST(english_plurals_read_correctly)
{
    wchar_t buf[BUF];

    apr_str_set_language(EN);

    apr_str_plural_format(APR_S_N_SOURCES, 0, buf, BUF, NULL, 0);
    ASSERT_WSTR_EQ(L"0 sources", buf);
    apr_str_plural_format(APR_S_N_SOURCES, 1, buf, BUF, NULL, 0);
    ASSERT_WSTR_EQ(L"1 source", buf);
    apr_str_plural_format(APR_S_N_SOURCES, 2, buf, BUF, NULL, 0);
    ASSERT_WSTR_EQ(L"2 sources", buf);
    apr_str_plural_format(APR_S_N_SOURCES, 11, buf, BUF, NULL, 0);
    ASSERT_WSTR_EQ(L"11 sources", buf);
    apr_str_plural_format(APR_S_N_BUSES, 1, buf, BUF, NULL, 0);
    ASSERT_WSTR_EQ(L"1 bus", buf);
    apr_str_plural_format(APR_S_N_FRAMES_LOST, 480, buf, BUF, NULL, 0);
    ASSERT_WSTR_EQ(L"480 frames lost", buf);
}

TEST(plural_format_puts_the_count_in_insert_one)
{
    wchar_t buf[BUF];
    apr_str_set_language(AR);
    apr_str_plural_format(APR_S_N_SOURCES, 3, buf, BUF, NULL, 0);
    ASSERT_WSTR_EQ(L"AR-PLACEHOLDER few 3", buf);
    apr_str_set_language(EN);
}

TEST(an_untranslated_plural_falls_back_with_the_fallback_languages_rules)
{
    /* N_BUSES has no Arabic block. Selecting it with Arabic rules would index
     * the English six with, say, ZERO for n=0 -- which happens to be filled,
     * but only because the English block deliberately fills all six. The
     * module instead notices the fallback and switches to English rules. */
    wchar_t buf[BUF];

    apr_str_set_language(AR);
    apr_str_plural_format(APR_S_N_BUSES, 0, buf, BUF, NULL, 0);
    ASSERT_WSTR_EQ(L"0 buses", buf);
    apr_str_plural_format(APR_S_N_BUSES, 1, buf, BUF, NULL, 0);
    ASSERT_WSTR_EQ(L"1 bus", buf);
    apr_str_plural_format(APR_S_N_BUSES, 3, buf, BUF, NULL, 0);
    ASSERT_WSTR_EQ(L"3 buses", buf);
    apr_str_set_language(EN);
}

/* ===========================================================================
 * Positional formatting -- what FormatMessageW actually does
 * ========================================================================= */

TEST(inserts_expand_in_order)
{
    wchar_t buf[BUF];
    const wchar_t *args[2] = { L"Teams", L"Main Mix" };

    apr_str_set_language(EN);
    apr_str_format(APR_S_UIA_SOURCE_FEEDS, buf, BUF, args, 2);
    ASSERT_WSTR_EQ(L"Teams, source, feeding Main Mix.", buf);
}

TEST(a_translator_may_reorder_the_inserts)
{
    /* The whole reason the catalog uses positional inserts. Arabic word order
     * differs; the translated string moves %2 in front of %1 and the call site
     * does not change. */
    wchar_t buf[BUF];
    const wchar_t *args[2] = { L"Teams", L"Main Mix" };

    ASSERT_EQ_INT(32, (int)apr_str_format_string(
        L"%1!s!, source, feeding %2!s!.", buf, BUF, args, 2));
    ASSERT_WSTR_EQ(L"Teams, source, feeding Main Mix.", buf);

    apr_str_format_string(L"%2!s! <- %1!s!", buf, BUF, args, 2);
    ASSERT_WSTR_EQ(L"Main Mix <- Teams", buf);

    {
        const wchar_t *three[3] = { L"a", L"b", L"c" };
        apr_str_format_string(L"%3!s! %1!s! %2!s!", buf, BUF, three, 3);
        ASSERT_WSTR_EQ(L"c a b", buf);
    }
}

TEST(an_insert_may_be_used_twice)
{
    /* Inserts are random access, not a consuming stream: %1 twice expands the
     * same argument twice and consumes nothing. A catalog string in the
     * shipping set relies on this (ERR_SOURCE_ALREADY_ON_BUS repeats %2). */
    wchar_t buf[BUF];
    const wchar_t *args[2] = { L"Teams", L"Main Mix" };

    apr_str_format_string(L"%1!s! %1!s! %1!s!", buf, BUF, args, 1);
    ASSERT_WSTR_EQ(L"Teams Teams Teams", buf);

    apr_str_set_language(EN);
    apr_str_format(APR_S_ERR_SOURCE_ALREADY_ON_BUS, buf, BUF, args, 2);
    ASSERT_WSTR_EQ(L"Teams is already on Main Mix. Remove it from Main Mix "
                   L"before adding it again.", buf);
}

TEST(an_unreferenced_argument_is_ignored)
{
    /* Not an error, and load-bearing: a zero plural form that never mentions
     * the count ("No sources") is passed the count anyway, because the other
     * forms need it. FormatMessageW simply never reads argument 1. */
    wchar_t buf[BUF];
    const wchar_t *args[3] = { L"one", L"two", L"three" };

    apr_str_format_string(L"only %2!s!", buf, BUF, args, 3);
    ASSERT_WSTR_EQ(L"only two", buf);

    apr_str_format_string(L"no inserts at all", buf, BUF, args, 3);
    ASSERT_WSTR_EQ(L"no inserts at all", buf);

    /* Skipping a number entirely is fine too: %1 and %3 with %2 unused. */
    apr_str_format_string(L"%1!s! %3!s!", buf, BUF, args, 3);
    ASSERT_WSTR_EQ(L"one three", buf);
}

TEST(a_referenced_insert_with_no_argument_is_refused_not_read)
{
    /* FORMAT_MESSAGE_ARGUMENT_ARRAY indexes the array directly and the API
     * carries no argument count, so FormatMessageW cannot detect this: left to
     * it, %3 against a two-element array reads past the end and dereferences
     * whatever it finds. The wrapper counts inserts itself and refuses. */
    wchar_t buf[BUF];
    const wchar_t *args[2] = { L"one", L"two" };
    size_t n;

    n = apr_str_format_string(L"%1!s! %2!s! %3!s!", buf, BUF, args, 2);
    ASSERT_GT_INT(0, (int)n);
    ASSERT_WSTR_EQ(L"!!apr_str: not enough arguments!!", buf);

    n = apr_str_format_string(L"%1!s!", buf, BUF, NULL, 0);
    ASSERT_WSTR_EQ(L"!!apr_str: not enough arguments!!", buf);
    ASSERT_GT_INT(0, (int)n);

    /* Beyond the compiled-in ceiling as well. */
    apr_str_format_string(L"%99!s!", buf, BUF, args, 2);
    ASSERT_WSTR_EQ(L"!!apr_str: not enough arguments!!", buf);

    /* Exactly enough is accepted. */
    apr_str_format_string(L"%1!s! %2!s!", buf, BUF, args, 2);
    ASSERT_WSTR_EQ(L"one two", buf);
}

TEST(percent_escapes_are_not_counted_as_inserts)
{
    wchar_t buf[BUF];
    const wchar_t *args[1] = { L"x" };

    /* %% is a literal percent, not insert zero, and must not make the
     * insert counter demand an argument. */
    apr_str_format_string(L"100%% done", buf, BUF, NULL, 0);
    ASSERT_WSTR_EQ(L"100% done", buf);

    apr_str_format_string(L"%1!s! is 50%% there", buf, BUF, args, 1);
    ASSERT_WSTR_EQ(L"x is 50% there", buf);
}

TEST(formatting_into_a_short_buffer_truncates_safely)
{
    wchar_t small[8];
    const wchar_t *args[2] = { L"Teams", L"Main Mix" };
    size_t n;

    n = apr_str_format_string(L"%1!s!, source, feeding %2!s!.", small, 8,
                              args, 2);
    ASSERT_GT_INT(0, (int)n);
    ASSERT_LE_INT(7, (int)n);
    ASSERT_EQ_INT(0, small[7]);            /* always terminated */

    /* Degenerate buffers are refused rather than written to. */
    ASSERT_EQ_INT(0, (int)apr_str_format_string(L"x", NULL, 0, NULL, 0));
    ASSERT_EQ_INT(0, (int)apr_str_format_string(L"x", small, 0, NULL, 0));
}

TEST(a_null_format_does_not_crash)
{
    wchar_t buf[BUF];
    apr_str_format_string(NULL, buf, BUF, NULL, 0);
    ASSERT_WSTR_EQ(L"!!apr_str: format failed!!", buf);
}

/* ===========================================================================
 * Lookup safety
 * ========================================================================= */

TEST(every_catalog_id_is_non_empty_in_every_language_at_runtime)
{
    /* Distinct from the completeness check above: that one asks the resource,
     * exactly, per language. This one asks the API, which is allowed to fall
     * back -- and must still never hand the UI an empty label. */
    int li, ci, k, n;
    AprStrId ids[APR_PLURAL_COUNT];

    for (li = 0; li < apr_str_language_count(); li++) {
        apr_str_set_language(apr_str_language_at(li)->langid);
        for (ci = 0; ci < apr_str_catalog_count(); ci++) {
            n = entry_ids(apr_str_catalog_at(ci), ids);
            for (k = 0; k < n; k++) {
                const wchar_t *s = apr_str(ids[k]);
                ASSERT_NOT_NULL(s);
                ASSERT_GT_INT(0, (int)wcslen(s));
            }
        }
    }
    apr_str_set_language(EN);
}

TEST(an_out_of_range_id_is_safe)
{
    wchar_t buf[BUF];
    const wchar_t *s;

    apr_str_set_language(EN);

    /* Below the floor, above the ceiling, zero, and negative. None of these
     * may index the cache array. */
    s = apr_str((AprStrId)0);              ASSERT_NOT_NULL(s);
    ASSERT_WSTR_EQ(L"!!apr_str: id out of range!!", s);
    s = apr_str((AprStrId)(APR_STR_ID_MIN - 1));
    ASSERT_WSTR_EQ(L"!!apr_str: id out of range!!", s);
    s = apr_str((AprStrId)APR_STR_ID_MAX);
    ASSERT_WSTR_EQ(L"!!apr_str: id out of range!!", s);
    s = apr_str((AprStrId)0x7fffffff);
    ASSERT_WSTR_EQ(L"!!apr_str: id out of range!!", s);
    s = apr_str((AprStrId)(-12345));
    ASSERT_WSTR_EQ(L"!!apr_str: id out of range!!", s);

    /* In range but never declared: loud, not blank.
     *
     * The canary sits at the TOP of the range on purpose. It used to be the
     * first free id after the command line's help block, which is exactly
     * where the next help string gets added -- and adding one then failed
     * this case instead of failing something real. APR_STR_LIST grows upward
     * from 1000, so the last id below APR_STR_ID_MAX is the one that stays
     * undeclared longest. */
    s = apr_str((AprStrId)(APR_STR_ID_MAX - 1));
    ASSERT_NOT_NULL(s);
    ASSERT_GT_INT(0, (int)wcslen(s));
    ASSERT_WSTR_EQ(L"!!apr_str 1999 missing!!", s);

    /* And the same through the formatting and probing entry points. */
    ASSERT_EQ_INT(-1, apr_str_probe(EN, (AprStrId)0x7fffffff, buf, BUF));
    ASSERT_EQ_INT(-1, apr_str_probe(EN, (AprStrId)(-1), buf, BUF));
    apr_str_format((AprStrId)0x7fffffff, buf, BUF, NULL, 0);
    ASSERT_WSTR_EQ(L"!!apr_str: id out of range!!", buf);

    /* A plural base that does not exist must not walk off either. */
    s = apr_str_plural((AprStrId)0x7ffffff0, 3);
    ASSERT_NOT_NULL(s);
    ASSERT_GT_INT(0, (int)wcslen(s));
}

/* ===========================================================================
 * Language resolution and direction
 * ========================================================================= */

TEST(language_is_matched_on_the_primary_language_id)
{
    AprErr e;

    /* en-GB has no block of its own; it must select the en-US catalog rather
     * than fall off the end. Same for ar-EG against ar-SA. */
    e = apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_UK));
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(EN, (int)apr_str_language());

    e = apr_str_set_language(MAKELANGID(LANG_ARABIC, SUBLANG_ARABIC_EGYPT));
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(AR, (int)apr_str_language());
    ASSERT_WSTR_EQ(k_ar_app_name, apr_str(APR_S_APP_NAME));

    apr_str_set_language(EN);
}

TEST(an_undeclared_language_falls_back_and_says_so)
{
    AprErr e = apr_str_set_language(MAKELANGID(LANG_FRENCH, SUBLANG_FRENCH));

    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_NOT_FOUND, (int)e.kind);
    ASSERT_EQ_INT(EN, (int)apr_str_language());
    /* Usable anyway: the UI gets English, not blanks. */
    ASSERT_WSTR_EQ(L"apprecorder", apr_str(APR_S_APP_NAME));
}

TEST(direction_follows_the_language)
{
    apr_str_set_language(EN);
    ASSERT_FALSE(apr_str_is_rtl());

    apr_str_set_language(AR);
    ASSERT_TRUE(apr_str_is_rtl());

    /* Resolution by primary language must carry direction with it. */
    apr_str_set_language(MAKELANGID(LANG_ARABIC, SUBLANG_ARABIC_EGYPT));
    ASSERT_TRUE(apr_str_is_rtl());

    apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_UK));
    ASSERT_FALSE(apr_str_is_rtl());

    /* The declaration table and the query must agree. */
    ASSERT_EQ_INT(0, apr_str_language_at(0)->rtl);
    ASSERT_EQ_INT(1, apr_str_language_at(1)->rtl);

    apr_str_set_language(EN);
}

TEST(changing_language_reloads_the_text)
{
    apr_str_set_language(EN);
    ASSERT_WSTR_EQ(L"apprecorder", apr_str(APR_S_APP_NAME));
    apr_str_set_language(AR);
    ASSERT_WSTR_EQ(k_ar_app_name, apr_str(APR_S_APP_NAME));
    apr_str_set_language(EN);
    ASSERT_WSTR_EQ(L"apprecorder", apr_str(APR_S_APP_NAME));
}

TEST(numbers_render_as_western_digits)
{
    wchar_t buf[32];

    ASSERT_EQ_INT(1, (int)apr_str_number(0, buf, 32));
    ASSERT_WSTR_EQ(L"0", buf);
    ASSERT_EQ_INT(4, (int)apr_str_number(-480, buf, 32));
    ASSERT_WSTR_EQ(L"-480", buf);
    apr_str_number(9223372036854775807ll, buf, 32);
    ASSERT_WSTR_EQ(L"9223372036854775807", buf);

    /* Digit shaping must not silently depend on the UI language: the choice
     * lives in apr_str_number and design 6.2 says Western until the author
     * says otherwise. */
    apr_str_set_language(AR);
    apr_str_number(2026, buf, 32);
    ASSERT_WSTR_EQ(L"2026", buf);
    apr_str_set_language(EN);

    /* Too small a buffer is refused outright rather than truncated: "12" for
     * 123456 would be a wrong number, not a shortened one. It must also not
     * reach _i64tow_s with the caller's size, which invokes the CRT invalid
     * parameter handler -- a modal dialog, i.e. a hang, in a Debug build. */
    ASSERT_EQ_INT(0, (int)apr_str_number(123456, buf, 2));
    ASSERT_EQ_INT(0, buf[0]);
    ASSERT_EQ_INT(0, (int)apr_str_number(1, NULL, 0));
    /* Exactly enough room still works. */
    ASSERT_EQ_INT(6, (int)apr_str_number(123456, buf, 7));
    ASSERT_WSTR_EQ(L"123456", buf);
}

TEST(catalog_strings_are_whole_sentences_not_fragments)
{
    /* A cheap structural guard for the rule at the top of strings.h: a string
     * that begins or ends with a separator is almost always a fragment someone
     * intends to concatenate. Labels are exempt only by being separator-free. */
    int ci, k, n, bad = 0;
    AprStrId ids[APR_PLURAL_COUNT];
    wchar_t  buf[BUF];

    for (ci = 0; ci < apr_str_catalog_count(); ci++) {
        const AprStrEntry *e = apr_str_catalog_at(ci);
        n = entry_ids(e, ids);
        for (k = 0; k < n; k++) {
            size_t len;
            if (apr_str_probe(EN, ids[k], buf, BUF) < 1)
                continue;
            len = wcslen(buf);
            if (buf[0] == L' ' || buf[len - 1] == L' ' ||
                buf[len - 1] == L':' || buf[len - 1] == L'-') {
                printf("      %s looks like a fragment: [%ls]\n", e->name, buf);
                bad++;
            }
        }
    }
    ASSERT_EQ_INT(0, bad);
}
