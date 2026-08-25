/*
 * test_err.c -- platform/err.c, the sole owner of error reporting.
 */
#include "test_runner.h"
#include "err.h"

#include <audioclient.h>

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
