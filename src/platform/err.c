/*
 * err.c -- the one error type, the one HRESULT decoder, and the one place an
 * error becomes a sentence a user hears. See include/err.h and
 * include/errmsg.h.
 *
 * TWO RENDERINGS, AND THEY ARE NOT INTERCHANGEABLE.
 *
 *   apr_err_format()  English, allocation-free, lock-free, callable from a
 *                     capture pump because apr_log_err() does exactly that.
 *                     Carries the raise site. FOR THE LOG.
 *
 *   apr_err_reason()  The catalog, in the display language. Takes a lock and
 *                     may allocate. FOR A PERSON. Everything below the
 *                     "the user's language" banner belongs to this half, and
 *                     nothing above it may call apr_str().
 */
#include "err.h"
#include "errmsg.h"

#include <audioclient.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* WIDEN(#sym) turns a symbol into a wide literal of its own spelling, so the
 * table below cannot drift from the constants it describes. */
#define WIDEN2(x) L##x
#define WIDEN(x)  WIDEN2(x)

#define E(sym, reason, text) \
    { (HRESULT)(sym), WIDEN(#sym), (text), (AprStrId)(reason) }

/*
 * FormatMessage cannot describe facility 0x889 (FACILITY_AUDCLNT): Windows
 * ships no message resource for it, so every WASAPI failure would otherwise
 * surface as a bare hex number. This table is the reason err.c exists as the
 * single owner -- decoding these in three different callers is how two of them
 * end up wrong. tests/test_err.c asserts FormatMessage still fails on them.
 *
 * EACH ROW CARRIES BOTH SPELLINGS OF ITS SENTENCE: the English one, which the
 * log needs and which must be reachable without touching the string layer,
 * and the catalog id, which is what a user hears. That is a duplication, and
 * it is pinned rather than trusted -- tests/test_err.c asserts the English
 * catalog entry for each row equals the row's own text, so the two cannot
 * drift. Adding a row means adding an id to APR_STR_LIST_ERR_HR and its text
 * to every LANGUAGE block of res/strings.rc; a row with no id fails
 * tests/test_strings.c.
 */
static const AprErrHrEntry g_hr_table[] = {
    E(AUDCLNT_E_NOT_INITIALIZED, APR_S_ERR_HR_E_NOT_INITIALIZED,
      L"The audio client has not been initialised."),
    E(AUDCLNT_E_ALREADY_INITIALIZED, APR_S_ERR_HR_E_ALREADY_INITIALIZED,
      L"The audio client is already initialised."),
    E(AUDCLNT_E_WRONG_ENDPOINT_TYPE, APR_S_ERR_HR_E_WRONG_ENDPOINT_TYPE,
      L"Wrong endpoint type for the requested operation (render vs capture)."),
    E(AUDCLNT_E_DEVICE_INVALIDATED, APR_S_ERR_HR_E_DEVICE_INVALIDATED,
      L"The audio endpoint went away: unplugged, disabled, or its format was "
      L"changed. The stream must be reopened."),
    E(AUDCLNT_E_NOT_STOPPED, APR_S_ERR_HR_E_NOT_STOPPED,
      L"The audio stream is still running."),
    E(AUDCLNT_E_BUFFER_TOO_LARGE, APR_S_ERR_HR_E_BUFFER_TOO_LARGE,
      L"The requested buffer is larger than the free space in the endpoint."),
    E(AUDCLNT_E_OUT_OF_ORDER, APR_S_ERR_HR_E_OUT_OF_ORDER,
      L"A previous GetBuffer is still outstanding; ReleaseBuffer it first."),
    E(AUDCLNT_E_UNSUPPORTED_FORMAT, APR_S_ERR_HR_E_UNSUPPORTED_FORMAT,
      L"The endpoint does not support the requested wave format."),
    E(AUDCLNT_E_INVALID_SIZE, APR_S_ERR_HR_E_INVALID_SIZE,
      L"The buffer size passed to ReleaseBuffer is not valid."),
    E(AUDCLNT_E_DEVICE_IN_USE, APR_S_ERR_HR_E_DEVICE_IN_USE,
      L"The endpoint is already in use in exclusive mode."),
    E(AUDCLNT_E_BUFFER_OPERATION_PENDING, APR_S_ERR_HR_E_BUFFER_OPERATION_PENDING,
      L"The buffer operation is still pending."),
    E(AUDCLNT_E_THREAD_NOT_REGISTERED, APR_S_ERR_HR_E_THREAD_NOT_REGISTERED,
      L"The calling thread is not registered with the Multimedia Class "
      L"Scheduler Service."),
    E(AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED, APR_S_ERR_HR_E_EXCLUSIVE_MODE_NOT_ALLOWED,
      L"Exclusive mode is disabled for this endpoint."),
    E(AUDCLNT_E_ENDPOINT_CREATE_FAILED, APR_S_ERR_HR_E_ENDPOINT_CREATE_FAILED,
      L"The audio engine could not create the endpoint, usually because the "
      L"requested period or buffer size is out of range."),
    E(AUDCLNT_E_SERVICE_NOT_RUNNING, APR_S_ERR_HR_E_SERVICE_NOT_RUNNING,
      L"The Windows Audio service is not running."),
    E(AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED, APR_S_ERR_HR_E_EVENTHANDLE_NOT_EXPECTED,
      L"An event handle was set on a stream not opened with "
      L"AUDCLNT_STREAMFLAGS_EVENTCALLBACK."),
    E(AUDCLNT_E_EXCLUSIVE_MODE_ONLY, APR_S_ERR_HR_E_EXCLUSIVE_MODE_ONLY,
      L"The endpoint supports exclusive mode only."),
    E(AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL, APR_S_ERR_HR_E_BUFDURATION_PERIOD_NOT_EQUAL,
      L"In exclusive event-driven mode the buffer duration must equal the "
      L"device period."),
    E(AUDCLNT_E_EVENTHANDLE_NOT_SET, APR_S_ERR_HR_E_EVENTHANDLE_NOT_SET,
      L"The stream was opened event-driven but no event handle was set."),
    E(AUDCLNT_E_INCORRECT_BUFFER_SIZE, APR_S_ERR_HR_E_INCORRECT_BUFFER_SIZE,
      L"The buffer size passed to GetBuffer is wrong."),
    E(AUDCLNT_E_BUFFER_SIZE_ERROR, APR_S_ERR_HR_E_BUFFER_SIZE_ERROR,
      L"The buffer duration is out of range for this endpoint."),
    E(AUDCLNT_E_CPUUSAGE_EXCEEDED, APR_S_ERR_HR_E_CPUUSAGE_EXCEEDED,
      L"The audio engine refused the stream: process CPU usage exceeds its "
      L"allowance."),
    E(AUDCLNT_E_BUFFER_ERROR, APR_S_ERR_HR_E_BUFFER_ERROR,
      L"GetBuffer failed to retrieve a data buffer."),
    E(AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED, APR_S_ERR_HR_E_BUFFER_SIZE_NOT_ALIGNED,
      L"The buffer size is not aligned; reopen with the aligned size the "
      L"client reports."),
    E(AUDCLNT_E_INVALID_DEVICE_PERIOD, APR_S_ERR_HR_E_INVALID_DEVICE_PERIOD,
      L"The requested device period is not valid."),
    E(AUDCLNT_E_INVALID_STREAM_FLAG, APR_S_ERR_HR_E_INVALID_STREAM_FLAG,
      L"A stream flag is not valid for this activation."),
    E(AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE, APR_S_ERR_HR_E_ENDPOINT_OFFLOAD_NOT_CAPABLE,
      L"The endpoint cannot do offloaded audio."),
    E(AUDCLNT_E_OUT_OF_OFFLOAD_RESOURCES, APR_S_ERR_HR_E_OUT_OF_OFFLOAD_RESOURCES,
      L"The endpoint has no offload resources left."),
    E(AUDCLNT_E_OFFLOAD_MODE_ONLY, APR_S_ERR_HR_E_OFFLOAD_MODE_ONLY,
      L"The endpoint supports offload mode only."),
    E(AUDCLNT_E_NONOFFLOAD_MODE_ONLY, APR_S_ERR_HR_E_NONOFFLOAD_MODE_ONLY,
      L"The endpoint supports non-offload mode only."),
    E(AUDCLNT_E_RESOURCES_INVALIDATED, APR_S_ERR_HR_E_RESOURCES_INVALIDATED,
      L"The stream's resources were invalidated and it must be reopened."),
    E(AUDCLNT_E_RAW_MODE_UNSUPPORTED, APR_S_ERR_HR_E_RAW_MODE_UNSUPPORTED,
      L"The endpoint does not support raw mode."),
    E(AUDCLNT_E_ENGINE_PERIODICITY_LOCKED, APR_S_ERR_HR_E_ENGINE_PERIODICITY_LOCKED,
      L"Another client has locked the audio engine's periodicity."),
    E(AUDCLNT_E_ENGINE_FORMAT_LOCKED, APR_S_ERR_HR_E_ENGINE_FORMAT_LOCKED,
      L"Another client has locked the audio engine's format."),
    E(AUDCLNT_E_HEADTRACKING_ENABLED, APR_S_ERR_HR_E_HEADTRACKING_ENABLED,
      L"The operation is not allowed while head tracking is enabled."),
    E(AUDCLNT_E_HEADTRACKING_UNSUPPORTED, APR_S_ERR_HR_E_HEADTRACKING_UNSUPPORTED,
      L"The endpoint does not support head tracking."),
    /* Added in the Windows 11 SDK; the project's floor is the 19041 SDK. */
#ifdef AUDCLNT_E_EFFECT_NOT_AVAILABLE
    E(AUDCLNT_E_EFFECT_NOT_AVAILABLE, APR_S_ERR_HR_E_EFFECT_NOT_AVAILABLE,
      L"The requested audio effect is not available on this endpoint."),
#endif
#ifdef AUDCLNT_E_EFFECT_STATE_READ_ONLY
    E(AUDCLNT_E_EFFECT_STATE_READ_ONLY, APR_S_ERR_HR_E_EFFECT_STATE_READ_ONLY,
      L"The audio effect's state is read-only."),
#endif

    E(AUDCLNT_S_BUFFER_EMPTY, APR_S_ERR_HR_S_BUFFER_EMPTY,
      L"No capture data was available this pass."),
    E(AUDCLNT_S_THREAD_ALREADY_REGISTERED, APR_S_ERR_HR_S_THREAD_ALREADY_REGISTERED,
      L"The thread is already registered with MMCSS."),
    E(AUDCLNT_S_POSITION_STALLED, APR_S_ERR_HR_S_POSITION_STALLED,
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

/* The one place an AprErr is filled in. Both public constructors funnel here
 * so that "allocation-free, lock-free, safe on a capture thread" is a property
 * of one function rather than of two that have to stay in step. */
static AprErr make_v(AprErrKind kind, long code, int reason,
                     const char *func, const char *file, int line,
                     const wchar_t *fmt, va_list ap)
{
    AprErr e;

    memset(&e, 0, sizeof e);
    e.kind   = kind;
    e.code   = code;
    e.reason = reason;
    e.func   = func;
    e.file   = file;
    e.line   = line;

    if (fmt) {
        /* _TRUNCATE: never fails, never allocates, always NUL-terminates.
         * Safe to run on a capture thread. */
        (void)_vsnwprintf_s(e.context, APR_ERR_CONTEXT_CCH, _TRUNCATE, fmt, ap);
    }
    return e;
}

AprErr apr_err_make(AprErrKind kind, long code,
                    const char *func, const char *file, int line,
                    const wchar_t *fmt, ...)
{
    AprErr  e;
    va_list ap;

    va_start(ap, fmt);
    e = make_v(kind, code, 0, func, file, line, fmt, ap);
    va_end(ap);
    return e;
}

AprErr apr_err_make_r(AprErrKind kind, long code, int reason,
                      const char *func, const char *file, int line,
                      const wchar_t *fmt, ...)
{
    AprErr  e;
    va_list ap;

    va_start(ap, fmt);
    e = make_v(kind, code, reason, func, file, line, fmt, ap);
    va_end(ap);
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

/* ===========================================================================
 * THE USER'S LANGUAGE -- see include/errmsg.h
 *
 * EVERYTHING BELOW THIS LINE MAY LOCK AND MAY ALLOCATE, and everything above
 * it may not. apr_log_err() calls apr_err_format() on whatever thread raised
 * the error, capture pumps included; apr_err_reason() is called from the UI
 * thread and from the CLI's own thread and from nowhere else.
 * ========================================================================= */

int apr_err_hr_table_count(void)
{
    return (int)(sizeof g_hr_table / sizeof g_hr_table[0]);
}

const AprErrHrEntry *apr_err_hr_table_at(int index)
{
    if (index < 0 || index >= apr_err_hr_table_count()) return NULL;
    return &g_hr_table[index];
}

/* The sentence for a kind, when nothing more specific is known. This is the
 * FLOOR: every arm returns a real catalog id, so no failure can reach a user
 * as an English literal. */
static AprStrId kind_reason(AprErrKind kind)
{
    switch (kind) {
    case APR_OK:            return APR_S__NONE;
    case APR_E_HRESULT:     return APR_S_ERR_REASON_HRESULT;
    case APR_E_WIN32:       return APR_S_ERR_REASON_WIN32;
    case APR_E_ERRNO:       return APR_S_ERR_REASON_ERRNO;
    case APR_E_INVALID_ARG: return APR_S_ERR_REASON_INVALID_ARG;
    case APR_E_NO_MEMORY:   return APR_S_ERR_REASON_NO_MEMORY;
    case APR_E_STATE:       return APR_S_ERR_REASON_STATE;
    case APR_E_NOT_FOUND:   return APR_S_ERR_REASON_NOT_FOUND;
    case APR_E_UNSUPPORTED: return APR_S_ERR_REASON_UNSUPPORTED;
    case APR_E_TIMEOUT:     return APR_S_ERR_REASON_TIMEOUT;
    case APR_E_OVERRUN:     return APR_S_ERR_REASON_OVERRUN;
    case APR_E_IO:          return APR_S_ERR_REASON_IO;
    case APR_E_BUSY:        return APR_S_ERR_REASON_BUSY;
    }
    return APR_S_ERR_REASON_UNKNOWN;
}

AprStrId apr_err_reason_id(const AprErr *e)
{
    int i;

    if (!e || e->kind == APR_OK) return APR_S__NONE;

    /* 1. The raise site named its own sentence. It knew something (kind, code)
     *    cannot express, so it wins over everything below. */
    if (e->reason != 0) return (AprStrId)e->reason;

    /* 2. A WASAPI code Windows will not describe. */
    if (e->kind == APR_E_HRESULT) {
        for (i = 0; i < apr_err_hr_table_count(); i++) {
            if (g_hr_table[i].hr == (HRESULT)e->code)
                return g_hr_table[i].reason;
        }
    }

    /* 3. FormatMessageW is step 3 and has no catalog id -- see errmsg.h. This
     *    entry point reports what the catalog can supply, which is the floor. */
    return kind_reason(e->kind);
}

/* Windows' own message table, ASKED FOR THE DISPLAY LANGUAGE.
 *
 * Deliberately NOT the LANG_NEUTRAL that apr_hresult_message() uses. Neutral
 * means "the thread's language", which is the machine's, not apprecorder's:
 * a user running --lang ar-SA on an English Windows would be handed English
 * prose inside an Arabic sentence, which is the whole of M11 arriving through
 * a different door.
 *
 * Returns 0 when this language has no text for the code -- the MUI for it is
 * not installed, or the code is not in the system table at all. That is not a
 * failure to paper over: the caller falls back to a catalog sentence that
 * keeps the number, rather than to text in a language the reader did not ask
 * for. */
static size_t system_message(DWORD code, wchar_t *buf, size_t cch)
{
    LANGID lang = apr_str_language();
    DWORD  n;

    buf[0] = L'\0';
    n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, code,
                       MAKELANGID(PRIMARYLANGID(lang), SUBLANG_DEFAULT),
                       buf, (DWORD)cch, NULL);
    if (n == 0) { buf[0] = L'\0'; return 0; }

    buf[cch - 1] = L'\0';
    wtrim_end(buf);
    return wcslen(buf);
}

size_t apr_err_reason(const AprErr *e, wchar_t *buf, size_t cch)
{
    AprStrId       id;
    wchar_t        sys[320];
    wchar_t        code[40];
    const wchar_t *args[1];

    if (!buf || cch == 0) return 0;
    buf[0] = L'\0';
    if (!e || e->kind == APR_OK) return 0;

    id = apr_err_reason_id(e);

    /* A named reason or a tabled WASAPI code is already the whole sentence and
     * takes no insert. Only the three numeric-code fallbacks do. */
    if (id != APR_S_ERR_REASON_HRESULT &&
        id != APR_S_ERR_REASON_WIN32 &&
        id != APR_S_ERR_REASON_ERRNO) {
        return apr_str_format(id, buf, cch, NULL, 0);
    }

    /* Windows describes ordinary HRESULTs and Win32 codes in the reader's own
     * language, and that space is open-ended -- tabling it is not something
     * anyone finishes. So this branch leans on it, in the display language. */
    if (e->kind == APR_E_HRESULT || e->kind == APR_E_WIN32) {
        /* Both are looked up as the raw DWORD: FORMAT_MESSAGE_FROM_SYSTEM
         * matches a Win32 status directly and an HRESULT by its full value. */
        if (system_message((DWORD)e->code, sys, sizeof sys / sizeof sys[0]) > 0) {
            size_t n = wcslen(sys);
            if (n > cch - 1) n = cch - 1;
            memcpy(buf, sys, n * sizeof(wchar_t));
            buf[n] = L'\0';
            return n;
        }
    }

    /* Nothing in the reader's language. Keep the number rather than swallow
     * it, inside a sentence that IS in the reader's language.
     *
     * An HRESULT is rendered as hex and NOT through apr_str_number(): it is an
     * identifier a person reads back to support, the same class of thing as a
     * file name, and Arabic-Indic hex digits would be a different identifier.
     * A Win32 status and an errno are ordinary decimal numbers and do go
     * through the one function that decides digit shaping. */
    if (e->kind == APR_E_HRESULT) {
        (void)_snwprintf_s(code, sizeof code / sizeof code[0], _TRUNCATE,
                           L"0x%08X", (unsigned)e->code);
    } else if (e->kind == APR_E_WIN32) {
        /* Through DWORD first: a Win32 status above LONG_MAX is stored in a
         * signed field, and reading it back as a negative would print a
         * number that matches nothing anyone can look up. */
        apr_str_number((int64_t)(DWORD)e->code, code,
                       sizeof code / sizeof code[0]);
    } else {
        apr_str_number((int64_t)e->code, code, sizeof code / sizeof code[0]);
    }
    args[0] = code;
    return apr_str_format(id, buf, cch, args, 1);
}
