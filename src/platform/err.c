/*
 * err.c -- the one error type and the one HRESULT decoder. See include/err.h.
 */
#include "err.h"

#include <audioclient.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* WIDEN(#sym) turns a symbol into a wide literal of its own spelling, so the
 * table below cannot drift from the constants it describes. */
#define WIDEN2(x) L##x
#define WIDEN(x)  WIDEN2(x)

typedef struct HrEntry {
    HRESULT        hr;
    const wchar_t *name;
    const wchar_t *text;
} HrEntry;

#define E(sym, text) { (HRESULT)(sym), WIDEN(#sym), (text) }

/*
 * FormatMessage cannot describe facility 0x889 (FACILITY_AUDCLNT): Windows
 * ships no message resource for it, so every WASAPI failure would otherwise
 * surface as a bare hex number. This table is the reason err.c exists as the
 * single owner -- decoding these in three different callers is how two of them
 * end up wrong. tests/test_err.c asserts FormatMessage still fails on them.
 */
static const HrEntry g_hr_table[] = {
    E(AUDCLNT_E_NOT_INITIALIZED,
      L"The audio client has not been initialised."),
    E(AUDCLNT_E_ALREADY_INITIALIZED,
      L"The audio client is already initialised."),
    E(AUDCLNT_E_WRONG_ENDPOINT_TYPE,
      L"Wrong endpoint type for the requested operation (render vs capture)."),
    E(AUDCLNT_E_DEVICE_INVALIDATED,
      L"The audio endpoint went away: unplugged, disabled, or its format was "
      L"changed. The stream must be reopened."),
    E(AUDCLNT_E_NOT_STOPPED,
      L"The audio stream is still running."),
    E(AUDCLNT_E_BUFFER_TOO_LARGE,
      L"The requested buffer is larger than the free space in the endpoint."),
    E(AUDCLNT_E_OUT_OF_ORDER,
      L"A previous GetBuffer is still outstanding; ReleaseBuffer it first."),
    E(AUDCLNT_E_UNSUPPORTED_FORMAT,
      L"The endpoint does not support the requested wave format."),
    E(AUDCLNT_E_INVALID_SIZE,
      L"The buffer size passed to ReleaseBuffer is not valid."),
    E(AUDCLNT_E_DEVICE_IN_USE,
      L"The endpoint is already in use in exclusive mode."),
    E(AUDCLNT_E_BUFFER_OPERATION_PENDING,
      L"The buffer operation is still pending."),
    E(AUDCLNT_E_THREAD_NOT_REGISTERED,
      L"The calling thread is not registered with the Multimedia Class "
      L"Scheduler Service."),
    E(AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED,
      L"Exclusive mode is disabled for this endpoint."),
    E(AUDCLNT_E_ENDPOINT_CREATE_FAILED,
      L"The audio engine could not create the endpoint, usually because the "
      L"requested period or buffer size is out of range."),
    E(AUDCLNT_E_SERVICE_NOT_RUNNING,
      L"The Windows Audio service is not running."),
    E(AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED,
      L"An event handle was set on a stream not opened with "
      L"AUDCLNT_STREAMFLAGS_EVENTCALLBACK."),
    E(AUDCLNT_E_EXCLUSIVE_MODE_ONLY,
      L"The endpoint supports exclusive mode only."),
    E(AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL,
      L"In exclusive event-driven mode the buffer duration must equal the "
      L"device period."),
    E(AUDCLNT_E_EVENTHANDLE_NOT_SET,
      L"The stream was opened event-driven but no event handle was set."),
    E(AUDCLNT_E_INCORRECT_BUFFER_SIZE,
      L"The buffer size passed to GetBuffer is wrong."),
    E(AUDCLNT_E_BUFFER_SIZE_ERROR,
      L"The buffer duration is out of range for this endpoint."),
    E(AUDCLNT_E_CPUUSAGE_EXCEEDED,
      L"The audio engine refused the stream: process CPU usage exceeds its "
      L"allowance."),
    E(AUDCLNT_E_BUFFER_ERROR,
      L"GetBuffer failed to retrieve a data buffer."),
    E(AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED,
      L"The buffer size is not aligned; reopen with the aligned size the "
      L"client reports."),
    E(AUDCLNT_E_INVALID_DEVICE_PERIOD,
      L"The requested device period is not valid."),
    E(AUDCLNT_E_INVALID_STREAM_FLAG,
      L"A stream flag is not valid for this activation."),
    E(AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE,
      L"The endpoint cannot do offloaded audio."),
    E(AUDCLNT_E_OUT_OF_OFFLOAD_RESOURCES,
      L"The endpoint has no offload resources left."),
    E(AUDCLNT_E_OFFLOAD_MODE_ONLY,
      L"The endpoint supports offload mode only."),
    E(AUDCLNT_E_NONOFFLOAD_MODE_ONLY,
      L"The endpoint supports non-offload mode only."),
    E(AUDCLNT_E_RESOURCES_INVALIDATED,
      L"The stream's resources were invalidated and it must be reopened."),
    E(AUDCLNT_E_RAW_MODE_UNSUPPORTED,
      L"The endpoint does not support raw mode."),
    E(AUDCLNT_E_ENGINE_PERIODICITY_LOCKED,
      L"Another client has locked the audio engine's periodicity."),
    E(AUDCLNT_E_ENGINE_FORMAT_LOCKED,
      L"Another client has locked the audio engine's format."),
    E(AUDCLNT_E_HEADTRACKING_ENABLED,
      L"The operation is not allowed while head tracking is enabled."),
    E(AUDCLNT_E_HEADTRACKING_UNSUPPORTED,
      L"The endpoint does not support head tracking."),
    /* Added in the Windows 11 SDK; the project's floor is the 19041 SDK. */
#ifdef AUDCLNT_E_EFFECT_NOT_AVAILABLE
    E(AUDCLNT_E_EFFECT_NOT_AVAILABLE,
      L"The requested audio effect is not available on this endpoint."),
#endif
#ifdef AUDCLNT_E_EFFECT_STATE_READ_ONLY
    E(AUDCLNT_E_EFFECT_STATE_READ_ONLY,
      L"The audio effect's state is read-only."),
#endif

    E(AUDCLNT_S_BUFFER_EMPTY,
      L"No capture data was available this pass."),
    E(AUDCLNT_S_THREAD_ALREADY_REGISTERED,
      L"The thread is already registered with MMCSS."),
    E(AUDCLNT_S_POSITION_STALLED,
      L"The stream position has stalled: the endpoint produced no new frames.")
};

#undef E

/* Copy `src` into `dst` (cch chars), truncating, always NUL-terminating.
 * cch must be >= 1. */
static void wcopy(wchar_t *dst, size_t cch, const wchar_t *src)
{
    size_t n = wcslen(src);
    if (n > cch - 1) n = cch - 1;
    memcpy(dst, src, n * sizeof(wchar_t));
    dst[n] = L'\0';
}

/* Strip trailing CR/LF/space that FormatMessage leaves on its strings; without
 * this every logged error grows a blank line. */
static void wtrim_end(wchar_t *s)
{
    size_t n = wcslen(s);
    while (n > 0 && (s[n - 1] == L'\r' || s[n - 1] == L'\n' ||
                     s[n - 1] == L' '  || s[n - 1] == L'\t')) {
        s[--n] = L'\0';
    }
}

static const wchar_t *basename_w(const char *path, wchar_t *buf, size_t cch)
{
    const char *p, *base = path ? path : "?";
    size_t i = 0;

    for (p = base; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    while (base[i] && i < cch - 1) {
        buf[i] = (wchar_t)(unsigned char)base[i];
        i++;
    }
    buf[i] = L'\0';
    return buf;
}

AprErr apr_ok(void)
{
    AprErr e;
    memset(&e, 0, sizeof e);
    e.kind = APR_OK;
    return e;
}

int apr_failed(const AprErr *e)
{
    return e && e->kind != APR_OK;
}

AprErr apr_err_make(AprErrKind kind, long code,
                    const char *func, const char *file, int line,
                    const wchar_t *fmt, ...)
{
    AprErr  e;
    va_list ap;

    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.code = code;
    e.func = func;
    e.file = file;
    e.line = line;

    if (fmt) {
        va_start(ap, fmt);
        /* _TRUNCATE: never fails, never allocates, always NUL-terminates.
         * Safe to run on a capture thread. */
        (void)_vsnwprintf_s(e.context, APR_ERR_CONTEXT_CCH, _TRUNCATE, fmt, ap);
        va_end(ap);
    }
    return e;
}

const wchar_t *apr_hresult_name(HRESULT hr)
{
    size_t i;
    for (i = 0; i < sizeof g_hr_table / sizeof g_hr_table[0]; i++) {
        if (g_hr_table[i].hr == hr) return g_hr_table[i].name;
    }
    return NULL;
}

const wchar_t *apr_hresult_message(HRESULT hr, wchar_t *buf, size_t cch)
{
    size_t i;
    DWORD  n;

    if (!buf || cch == 0) return L"";
    buf[0] = L'\0';

    for (i = 0; i < sizeof g_hr_table / sizeof g_hr_table[0]; i++) {
        if (g_hr_table[i].hr == hr) {
            wcopy(buf, cch, g_hr_table[i].text);
            return buf;
        }
    }

    n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, (DWORD)hr,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       buf, (DWORD)cch, NULL);
    if (n > 0) {
        buf[cch - 1] = L'\0';
        wtrim_end(buf);
        if (buf[0] != L'\0') return buf;
    }

    (void)_snwprintf_s(buf, cch, _TRUNCATE,
                       L"Unrecognised error 0x%08X", (unsigned)hr);
    return buf;
}

const wchar_t *apr_err_kind_name(AprErrKind kind)
{
    switch (kind) {
    case APR_OK:            return L"APR_OK";
    case APR_E_HRESULT:     return L"APR_E_HRESULT";
    case APR_E_WIN32:       return L"APR_E_WIN32";
    case APR_E_ERRNO:       return L"APR_E_ERRNO";
    case APR_E_INVALID_ARG: return L"APR_E_INVALID_ARG";
    case APR_E_NO_MEMORY:   return L"APR_E_NO_MEMORY";
    case APR_E_STATE:       return L"APR_E_STATE";
    case APR_E_NOT_FOUND:   return L"APR_E_NOT_FOUND";
    case APR_E_UNSUPPORTED: return L"APR_E_UNSUPPORTED";
    case APR_E_TIMEOUT:     return L"APR_E_TIMEOUT";
    case APR_E_OVERRUN:     return L"APR_E_OVERRUN";
    case APR_E_IO:          return L"APR_E_IO";
    case APR_E_BUSY:        return L"APR_E_BUSY";
    }
    return L"APR_E_?";
}

const wchar_t *apr_err_format(const AprErr *e, wchar_t *buf, size_t cch)
{
    wchar_t msg[320];
    wchar_t file[64];
    const wchar_t *sym;

    if (!buf || cch == 0) return L"";
    buf[0] = L'\0';
    if (!e) return buf;

    if (e->kind == APR_OK) {
        wcopy(buf, cch, L"no error");
        return buf;
    }

    basename_w(e->file, file, sizeof file / sizeof file[0]);

    switch (e->kind) {
    case APR_E_HRESULT:
        apr_hresult_message((HRESULT)e->code, msg, sizeof msg / sizeof msg[0]);
        sym = apr_hresult_name((HRESULT)e->code);
        (void)_snwprintf_s(buf, cch, _TRUNCATE,
                           L"%ls: %ls (%ls0x%08X) at %ls(%d) in %hs",
                           e->context[0] ? e->context : L"(no context)",
                           msg,
                           sym ? sym : L"HRESULT ",
                           (unsigned)e->code,
                           file, e->line, e->func ? e->func : "?");
        break;

    case APR_E_WIN32:
        apr_hresult_message(HRESULT_FROM_WIN32((DWORD)e->code), msg,
                            sizeof msg / sizeof msg[0]);
        (void)_snwprintf_s(buf, cch, _TRUNCATE,
                           L"%ls: %ls (Win32 %ld) at %ls(%d) in %hs",
                           e->context[0] ? e->context : L"(no context)",
                           msg, e->code, file, e->line,
                           e->func ? e->func : "?");
        break;

    case APR_E_ERRNO:
        if (_wcserror_s(msg, sizeof msg / sizeof msg[0], (int)e->code) != 0) {
            wcopy(msg, sizeof msg / sizeof msg[0], L"unknown errno");
        }
        wtrim_end(msg);
        (void)_snwprintf_s(buf, cch, _TRUNCATE,
                           L"%ls: %ls (errno %ld) at %ls(%d) in %hs",
                           e->context[0] ? e->context : L"(no context)",
                           msg, e->code, file, e->line,
                           e->func ? e->func : "?");
        break;

    default:
        (void)_snwprintf_s(buf, cch, _TRUNCATE,
                           L"%ls: %ls at %ls(%d) in %hs",
                           e->context[0] ? e->context : L"(no context)",
                           apr_err_kind_name(e->kind), file, e->line,
                           e->func ? e->func : "?");
        break;
    }

    buf[cch - 1] = L'\0';
    return buf;
}
