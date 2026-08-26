/*
 * test_err.c -- platform/err.c, the sole owner of error reporting.
 */
#include "test_runner.h"
#include "err.h"
#include "errmsg.h"

#include <audioclient.h>

#define EN MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)
#define AR MAKELANGID(LANG_ARABIC,  SUBLANG_ARABIC_SAUDI_ARABIA)

/* The Arabic marker res/strings.rc carries for
 * APR_S_ERR_HR_E_DEVICE_INVALIDATED, spelled in code points.
 *
 * NOT a wide literal, for the reason tests/test_strings.c gives at length:
 * this .c file is UTF-8 without a BOM, MSVC reads it in the system ANSI code
 * page, and a mis-decoded literal compared against a mis-decoded resource
 * agrees with the bug. */
static const wchar_t k_ar_device_invalidated[] = {
    L'A', L'R', L'-', L'P', L'L', L'A', L'C', L'E', L'H', L'O', L'L', L'D',
    L'E', L'R', L'-', L'R', L'E', L'A', L'S', L'O', L'N', L' ',
    0x062A, 0x062C, 0x0631, 0x0628, 0x0629, 0
};

TEST(ok_is_not_a_failure)
{
    AprErr e = apr_ok();

    ASSERT_EQ_INT(APR_OK, e.kind);
    ASSERT_EQ_INT(0, e.code);
    ASSERT_FALSE(apr_failed(&e));
}

TEST(hresult_error_carries_code_context_and_origin)
{
    AprErr e = APR_ERR_HR(E_INVALIDARG, L"opening %ls", L"Teams");

    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_HRESULT, e.kind);
    ASSERT_EQ_INT((long)E_INVALIDARG, e.code);
    ASSERT_WSTR_EQ(L"opening Teams", e.context);
    ASSERT_NOT_NULL(e.file);
    ASSERT_NOT_NULL(e.func);
    ASSERT_GT_INT(0, e.line);
}

TEST(win32_error_snapshots_getlasterror)
{
    AprErr e;

    SetLastError(ERROR_FILE_NOT_FOUND);
    e = APR_ERR_LAST(L"opening session file");

    ASSERT_EQ_INT(APR_E_WIN32, e.kind);
    ASSERT_EQ_INT((long)ERROR_FILE_NOT_FOUND, e.code);
}

TEST(plain_kinds_need_no_code)
{
    AprErr e = APR_ERR(APR_E_NO_MEMORY, L"ring buffer of %u frames", 48000u);

    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_NO_MEMORY, e.kind);
    ASSERT_EQ_INT(0, e.code);
    ASSERT_WSTR_EQ(L"ring buffer of 48000 frames", e.context);
}

TEST(context_longer_than_the_buffer_truncates_and_terminates)
{
    AprErr e = APR_ERR(APR_E_IO, L"%ls",
        L"0123456789012345678901234567890123456789"
        L"0123456789012345678901234567890123456789"
        L"0123456789012345678901234567890123456789"
        L"0123456789012345678901234567890123456789"
        L"0123456789012345678901234567890123456789"
        L"0123456789012345678901234567890123456789");

    ASSERT_EQ_INT(0, e.context[APR_ERR_CONTEXT_CCH - 1]);
    ASSERT_EQ_INT(APR_ERR_CONTEXT_CCH - 1, (int)wcslen(e.context));
}

/* ---- HRESULT -> text ---------------------------------------------------- */

TEST(system_hresults_come_from_formatmessage)
{
    wchar_t buf[256];
    const wchar_t *m = apr_hresult_message(E_INVALIDARG, buf, 256);

    ASSERT_NOT_NULL(m);
    ASSERT_GT_INT(0, (int)wcslen(m));
    /* FormatMessage's text ends with CRLF; we must strip it or every log line
     * gains a stray blank line. */
    ASSERT_NE_INT(L'\n', m[wcslen(m) - 1]);
    ASSERT_NE_INT(L'\r', m[wcslen(m) - 1]);
}

/* This is the reason platform/err.c hand-tables the AUDCLNT codes. If Windows
 * ever starts describing facility 0x889, this test tells us the table can go. */
TEST(formatmessage_really_does_not_know_audclnt_codes)
{
    wchar_t buf[256];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, (DWORD)AUDCLNT_E_DEVICE_INVALIDATED, 0,
                             buf, 256, NULL);
    ASSERT_EQ_INT(0, (int)n);
}

TEST(audclnt_errors_are_named_and_described)
{
    wchar_t buf[256];

    ASSERT_WSTR_EQ(L"AUDCLNT_E_DEVICE_INVALIDATED",
                   apr_hresult_name(AUDCLNT_E_DEVICE_INVALIDATED));
    ASSERT_WSTR_EQ(L"AUDCLNT_E_UNSUPPORTED_FORMAT",
                   apr_hresult_name(AUDCLNT_E_UNSUPPORTED_FORMAT));
    ASSERT_WSTR_EQ(L"AUDCLNT_E_NOT_INITIALIZED",
                   apr_hresult_name(AUDCLNT_E_NOT_INITIALIZED));

    /* The message must be prose, not just the symbol echoed back. */
    apr_hresult_message(AUDCLNT_E_DEVICE_INVALIDATED, buf, 256);
    ASSERT_GT_INT(20, (int)wcslen(buf));
    ASSERT_NULL(wcsstr(buf, L"0x"));
}

TEST(audclnt_success_codes_are_named_too)
{
    ASSERT_WSTR_EQ(L"AUDCLNT_S_BUFFER_EMPTY",
                   apr_hresult_name(AUDCLNT_S_BUFFER_EMPTY));
    ASSERT_WSTR_EQ(L"AUDCLNT_S_POSITION_STALLED",
                   apr_hresult_name(AUDCLNT_S_POSITION_STALLED));
}

TEST(unknown_hresults_fall_back_to_hex)
{
    wchar_t buf[256];
    const wchar_t *m;

    ASSERT_NULL(apr_hresult_name((HRESULT)0x87654321L));

    m = apr_hresult_message((HRESULT)0x87654321L, buf, 256);
    ASSERT_NOT_NULL(wcsstr(m, L"0x87654321"));
}

TEST(hresult_message_never_overruns_a_short_buffer)
{
    wchar_t buf[8];
    const wchar_t *m = apr_hresult_message(AUDCLNT_E_DEVICE_INVALIDATED, buf, 8);

    ASSERT_TRUE(m == buf);
    ASSERT_EQ_INT(0, buf[7]);
    ASSERT_LE_INT(7, (int)wcslen(buf));
}

/* ---- whole-error rendering ---------------------------------------------- */

TEST(formatting_an_error_includes_context_symbol_and_origin)
{
    wchar_t  buf[512];
    AprErr   e = APR_ERR_HR(AUDCLNT_E_DEVICE_INVALIDATED, L"starting %ls", L"Chat Mic");
    const wchar_t *s = apr_err_format(&e, buf, 512);

    ASSERT_NOT_NULL(wcsstr(s, L"starting Chat Mic"));
    ASSERT_NOT_NULL(wcsstr(s, L"AUDCLNT_E_DEVICE_INVALIDATED"));
    ASSERT_NOT_NULL(wcsstr(s, L"0x88890004"));
    ASSERT_NOT_NULL(wcsstr(s, L"test_err.c"));
}

TEST(formatting_ok_says_so_rather_than_lying)
{
    wchar_t buf[128];
    AprErr  e = apr_ok();

    ASSERT_NOT_NULL(wcsstr(apr_err_format(&e, buf, 128), L"no error"));
}

TEST(kind_names_are_stable_strings)
{
    ASSERT_WSTR_EQ(L"APR_E_OVERRUN", apr_err_kind_name(APR_E_OVERRUN));
    ASSERT_WSTR_EQ(L"APR_OK", apr_err_kind_name(APR_OK));
    ASSERT_NOT_NULL(apr_err_kind_name((AprErrKind)999));
}

/* ===========================================================================
 * THE HALF A USER HEARS -- BUGS.md M11
 *
 * Everything above renders for the log. Everything below renders for a
 * person, which means it has to come out of the catalog: the author is blind,
 * this is the sentence he hears at the moment something has gone wrong, and
 * before this change it was English prose with a raise site glued to it,
 * wrapped in a translated frame.
 * ========================================================================= */

/* THE CASE THAT FAILS WITHOUT THE FIX.
 *
 * A WASAPI failure raised on a capture pump -- with an English diagnostic
 * context, exactly as every raise site in the tree writes one -- must reach
 * an Arabic user as ARABIC. Before the fix the display path called
 * apr_err_format() and this string was "starting Chat Mic: The audio endpoint
 * went away: unplugged, disabled, or its format was changed. ... at
 * test_err.c(NNN) in ...". Every assertion below fails on that string. */
TEST(a_failure_a_user_hears_is_the_catalogs_and_not_err_cs)
{
    wchar_t buf[512];
    AprErr  e = APR_ERR_HR(AUDCLNT_E_DEVICE_INVALIDATED,
                           L"starting %ls", L"Chat Mic");

    apr_str_set_language(AR);
    ASSERT_GT_INT(0, (int)apr_err_reason(&e, buf, 512));

    /* It came out of the Arabic block of the catalog, character for
     * character. Nothing else in the product can produce this string. */
    ASSERT_WSTR_EQ(k_ar_device_invalidated, buf);

    /* And none of the four English things that used to be in it are. */
    ASSERT_NULL(wcsstr(buf, L"audio endpoint"));   /* err.c's own prose      */
    ASSERT_NULL(wcsstr(buf, L"Chat Mic"));         /* the diagnostic context */
    ASSERT_NULL(wcsstr(buf, L"test_err.c"));       /* the raise site         */
    ASSERT_NULL(wcsstr(buf, L"AUDCLNT"));          /* the symbol             */

    apr_str_set_language(EN);
}

/* The same error in English resolves from the English block -- so the Arabic
 * case above is proving language selection, not proving that the catalog
 * happens to be empty. */
TEST(the_same_failure_in_english_is_the_english_catalog_entry)
{
    wchar_t buf[512], want[512];
    AprErr  e = APR_ERR_HR(AUDCLNT_E_SERVICE_NOT_RUNNING, L"opening %ls",
                           L"Teams");

    apr_str_set_language(EN);
    apr_err_reason(&e, buf, 512);

    ASSERT_GE_INT(1, apr_str_probe(EN, APR_S_ERR_HR_E_SERVICE_NOT_RUNNING,
                                   want, 512));
    ASSERT_WSTR_EQ(want, buf);
    ASSERT_NULL(wcsstr(buf, L"Teams"));
    ASSERT_NULL(wcsstr(buf, L"test_err.c"));
}

/* The duplication err.c's table carries -- English text for the log AND a
 * catalog id for the user -- pinned rather than trusted. Editing one spelling
 * of a sentence and not the other fails here. */
TEST(the_log_and_the_user_are_told_the_same_thing_in_english)
{
    wchar_t want[512];
    int     i, bad = 0;

    apr_str_set_language(EN);

    for (i = 0; i < apr_err_hr_table_count(); i++) {
        const AprErrHrEntry *row = apr_err_hr_table_at(i);
        if (apr_str_probe(EN, row->reason, want, 512) < 1) {
            printf("      %ls: no English catalog text\n", row->name);
            bad++;
            continue;
        }
        if (wcscmp(want, row->text) != 0) {
            printf("      %ls drifted:\n        table:   [%ls]\n"
                   "        catalog: [%ls]\n", row->name, row->text, want);
            bad++;
        }
    }
    ASSERT_EQ_INT(0, bad);
}

/* A raise site that knows something (kind, code) cannot express names its own
 * sentence. One integer store -- no lock, no allocation -- which is what
 * makes it legal on a capture pump. */
TEST(a_raise_site_may_name_the_sentence_a_user_hears)
{
    wchar_t buf[512], want[512];
    AprErr  e = APR_ERR_SAY(APR_E_STATE, APR_S_ERR_REASON_BUS_FULL_SOURCES,
                            L"bus %u already has %d sources", 1u, 8);

    apr_str_set_language(EN);

    /* The diagnostic half is untouched: still English, still the raise site,
     * still what the log gets. */
    ASSERT_EQ_INT(APR_E_STATE, e.kind);
    ASSERT_WSTR_EQ(L"bus 1 already has 8 sources", e.context);
    ASSERT_EQ_INT((int)APR_S_ERR_REASON_BUS_FULL_SOURCES, e.reason);
    ASSERT_EQ_INT((int)APR_S_ERR_REASON_BUS_FULL_SOURCES,
                  (int)apr_err_reason_id(&e));

    /* The user half is the named sentence, NOT the kind's generic one. */
    apr_err_reason(&e, buf, 512);
    apr_str_probe(EN, APR_S_ERR_REASON_BUS_FULL_SOURCES, want, 512);
    ASSERT_WSTR_EQ(want, buf);

    /* Without the name it would have been the coarse one -- which is the
     * whole reason the field exists. */
    e.reason = 0;
    apr_err_reason(&e, buf, 512);
    apr_str_probe(EN, APR_S_ERR_REASON_STATE, want, 512);
    ASSERT_WSTR_EQ(want, buf);
}

/* An error apprecorder does not table and Windows cannot describe still says
 * something in the reader's language, and still carries the number. Swallowing
 * the code would leave a user with a sentence nobody can act on. */
TEST(a_code_nobody_can_describe_keeps_its_number_inside_a_catalog_sentence)
{
    wchar_t buf[512], want[512];
    const wchar_t *args[1];
    AprErr  e = APR_ERR_HR((HRESULT)0x87654321L, L"activating %ls", L"Teams");

    apr_str_set_language(EN);
    apr_err_reason(&e, buf, 512);

    args[0] = L"0x87654321";
    apr_str_format(APR_S_ERR_REASON_HRESULT, want, 512, args, 1);
    ASSERT_WSTR_EQ(want, buf);
    ASSERT_NOT_NULL(wcsstr(buf, L"0x87654321"));
    ASSERT_NULL(wcsstr(buf, L"Teams"));
}

/* Success is not a reason. A caller that renders one by mistake gets nothing,
 * not "no error" read out as an explanation. */
TEST(a_success_value_has_no_reason_at_all)
{
    wchar_t buf[512];
    AprErr  e = apr_ok();

    apr_str_set_language(EN);
    ASSERT_EQ_INT(0, (int)apr_err_reason(&e, buf, 512));
    ASSERT_EQ_INT(0, buf[0]);
    ASSERT_EQ_INT((int)APR_S__NONE, (int)apr_err_reason_id(&e));
}

/* Nothing a user hears carries the raise site, in either language, for any
 * kind. This is the property, stated once over the whole enum, rather than
 * one assertion per case. */
TEST(no_reason_in_any_language_carries_the_raise_site)
{
    const LANGID langs[2] = { EN, AR };
    int li, k, bad = 0;

    for (li = 0; li < 2; li++) {
        apr_str_set_language(langs[li]);
        for (k = (int)APR_E_HRESULT; k <= (int)APR_E_BUSY; k++) {
            wchar_t buf[512];
            AprErr  e = apr_err_make((AprErrKind)k, 5,
                                     "a_function_name", __FILE__, 4242,
                                     L"a diagnostic nobody should hear");
            apr_err_reason(&e, buf, 512);
            if (wcslen(buf) == 0 ||
                wcsstr(buf, L"test_err.c") ||
                wcsstr(buf, L"a_function_name") ||
                wcsstr(buf, L"a diagnostic nobody should hear")) {
                printf("      kind %ls leaked: [%ls]\n",
                       apr_err_kind_name((AprErrKind)k), buf);
                bad++;
            }
        }
    }
    apr_str_set_language(EN);
    ASSERT_EQ_INT(0, bad);
}

/* A short buffer truncates rather than overruns, exactly as everything else
 * in this file does. */
TEST(a_reason_never_overruns_a_short_buffer)
{
    wchar_t buf[8];
    AprErr  e = APR_ERR_HR(AUDCLNT_E_DEVICE_INVALIDATED, L"starting");

    apr_str_set_language(EN);
    apr_err_reason(&e, buf, 8);
    ASSERT_EQ_INT(0, buf[7]);
    ASSERT_LE_INT(7, (int)wcslen(buf));
}
