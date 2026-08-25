/*
 * log.c -- the levelled log. See include/log.h for the contract and the
 * reasoning behind the ring and the overflow policy.
 *
 * Concurrency shape: a bounded MPSC ring of fixed slots. Producers reserve a
 * slot with one CAS, fill it, then publish it with a release store to the
 * slot's `ready` flag. The single consumer walks from read_pos and stops at
 * the first slot not yet published, which keeps output in reservation order.
 * A producer that finds the ring full refuses at the door, so it can never
 * write over a slot the consumer is reading.
 */
#include "log.h"
#include "err.h"

#include <windows.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define RING_MASK (APR_LOG_RING_SLOTS - 1)
#if (APR_LOG_RING_SLOTS & RING_MASK) != 0
#error APR_LOG_RING_SLOTS must be a power of two
#endif

typedef struct LogRecord {
    volatile LONG  ready;      /* 1 once the producer has finished filling */
    LONG           level;
    DWORD          thread_id;
    FILETIME       wall;       /* KUSER_SHARED_DATA read: no syscall */
    LONGLONG       qpc;        /* correlates a log line with an audio timestamp */
    const char    *file;
    const char    *func;
    int            line;
    const wchar_t *rt_fmt;     /* NULL for the formatted path */
    LONGLONG       a, b;
    wchar_t        text[APR_LOG_TEXT_CCH];
} LogRecord;

static LogRecord    g_ring[APR_LOG_RING_SLOTS];
static volatile LONG64 g_reserved;      /* next slot index to hand out */
static volatile LONG64 g_read;          /* next slot index to consume */
static volatile LONG64 g_emitted;
static volatile LONG64 g_dropped;

static volatile LONG g_level = APR_LV_OFF;   /* OFF until init: see log.h */
static volatile LONG g_running;

static AprLogSink g_sink;
static void      *g_sink_user;
static HANDLE     g_file   = INVALID_HANDLE_VALUE;
static HANDLE     g_thread;
static HANDLE     g_wake;
static int        g_to_debugger;
static int        g_to_stderr;

/* ------------------------------------------------------------------------ */

void apr_log_set_level(AprLogLevel level)
{
    InterlockedExchange(&g_level, (LONG)level);
}

AprLogLevel apr_log_get_level(void)
{
    return (AprLogLevel)g_level;   /* relaxed load; x64 aligned LONG is atomic */
}

int apr_log_enabled(AprLogLevel level)
{
    return (LONG)level >= g_level && (LONG)level < APR_LV_OFF;
}

void apr_log_stats(uint64_t *out_emitted, uint64_t *out_dropped)
{
    if (out_emitted) *out_emitted = (uint64_t)g_emitted;
    if (out_dropped) *out_dropped = (uint64_t)g_dropped;
}

void apr_log_set_sink(AprLogSink sink, void *user)
{
    g_sink_user = user;
    _ReadWriteBarrier();
    g_sink = sink;
}

/* Reserve a slot, or NULL when the ring is full (the newest record loses). */
static LogRecord *reserve(void)
{
    for (;;) {
        LONG64 w = g_reserved;
        LONG64 r = g_read;

        if (w - r >= APR_LOG_RING_SLOTS) {
            InterlockedIncrement64(&g_dropped);
            return NULL;
        }
        if (_InterlockedCompareExchange64(&g_reserved, w + 1, w) == w) {
            return &g_ring[w & RING_MASK];
        }
    }
}

static void stamp(LogRecord *rec, AprLogLevel level,
                  const char *func, const char *file, int line)
{
    LARGE_INTEGER now;

    rec->level     = (LONG)level;
    rec->thread_id = GetCurrentThreadId();
    rec->func      = func;
    rec->file      = file;
    rec->line      = line;
    GetSystemTimeAsFileTime(&rec->wall);
    QueryPerformanceCounter(&now);
    rec->qpc = now.QuadPart;
}

static void publish(LogRecord *rec)
{
    InterlockedIncrement64(&g_emitted);
    InterlockedExchange(&rec->ready, 1);      /* release */
    if (g_wake) SetEvent(g_wake);             /* never called on the RT path */
}

void apr_log_write(AprLogLevel level, const char *func, const char *file,
                   int line, const wchar_t *fmt, ...)
{
    LogRecord *rec;
    va_list    ap;

    if (!apr_log_enabled(level)) return;
    rec = reserve();
    if (!rec) return;

    stamp(rec, level, func, file, line);
    rec->rt_fmt = NULL;
    rec->a = rec->b = 0;
    va_start(ap, fmt);
    (void)_vsnwprintf_s(rec->text, APR_LOG_TEXT_CCH, _TRUNCATE, fmt, ap);
    va_end(ap);
    publish(rec);
}

void apr_log_write_rt(AprLogLevel level, const char *func, const char *file,
                      int line, const wchar_t *fmt, int64_t a, int64_t b)
{
    LogRecord *rec;

    if (!apr_log_enabled(level)) return;
    rec = reserve();
    if (!rec) return;

    stamp(rec, level, func, file, line);
    rec->rt_fmt  = fmt;          /* stored by pointer: must be a literal */
    rec->a       = (LONGLONG)a;
    rec->b       = (LONGLONG)b;
    rec->text[0] = L'\0';

    /* Deliberately no SetEvent: a real-time producer must not make a kernel
     * call. The drain thread's 250 ms timeout picks these up. */
    InterlockedIncrement64(&g_emitted);
    InterlockedExchange(&rec->ready, 1);
}

void apr_log_err(AprLogLevel level, const AprErr *e,
                 const char *func, const char *file, int line)
{
    LogRecord *rec;

    if (!apr_log_enabled(level) || !e) return;
    rec = reserve();
    if (!rec) return;

    stamp(rec, level, func, file, line);
    rec->rt_fmt = NULL;
    rec->a = rec->b = 0;
    apr_err_format(e, rec->text, APR_LOG_TEXT_CCH);
    publish(rec);
}

/* ------------------------------------------------------------------------ */

static const wchar_t *level_name(LONG level)
{
    switch (level) {
    case APR_LV_TRACE: return L"TRACE";
    case APR_LV_DEBUG: return L"DEBUG";
    case APR_LV_INFO:  return L"INFO ";
    case APR_LV_WARN:  return L"WARN ";
    case APR_LV_ERROR: return L"ERROR";
    default:           return L"?????";
    }
}

static const char *basename_a(const char *path)
{
    const char *p, *base = path ? path : "?";
    for (p = base; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    return base;
}

static void render(const LogRecord *rec, wchar_t *out, size_t cch)
{
    SYSTEMTIME st;
    FILETIME   local;
    wchar_t    body[APR_LOG_TEXT_CCH];

    if (rec->rt_fmt) {
        /* Expanded here, not on the producer's thread. The format is
         * documented to hold at most two %lld; extra arguments are harmless. */
        (void)_snwprintf_s(body, APR_LOG_TEXT_CCH, _TRUNCATE,
                           rec->rt_fmt, rec->a, rec->b);
    } else {
        wcsncpy_s(body, APR_LOG_TEXT_CCH, rec->text, _TRUNCATE);
    }

    if (!FileTimeToLocalFileTime(&rec->wall, &local) ||
        !FileTimeToSystemTime(&local, &st)) {
        memset(&st, 0, sizeof st);
    }

    (void)_snwprintf_s(out, cch, _TRUNCATE,
                       L"%02u:%02u:%02u.%03u %ls [%lu] %hs(%d) %hs: %ls",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                       level_name(rec->level), rec->thread_id,
                       basename_a(rec->file), rec->line,
                       rec->func ? rec->func : "?", body);
}

static void emit_line(const wchar_t *line)
{
    AprLogSink sink = g_sink;

    if (sink) sink(line, g_sink_user);
    if (g_to_debugger) {
        OutputDebugStringW(line);
        OutputDebugStringW(L"\r\n");
    }
    if (g_to_stderr) {
        fputws(line, stderr);
        fputws(L"\n", stderr);
    }
    if (g_file != INVALID_HANDLE_VALUE) {
        char  utf8[1024];
        int   n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8,
                                      (int)sizeof utf8 - 2, NULL, NULL);
        DWORD written = 0;
        if (n > 1) {
            utf8[n - 1] = '\r';
            utf8[n]     = '\n';
            (void)WriteFile(g_file, utf8, (DWORD)(n + 1), &written, NULL);
        }
    }
}

size_t apr_log_drain(void)
{
    wchar_t line[APR_LOG_TEXT_CCH + 128];
    size_t  n = 0;

    for (;;) {
        LONG64     r = g_read;
        LogRecord *rec;

        if (r >= g_reserved) break;
        rec = &g_ring[r & RING_MASK];
        if (!rec->ready) break;          /* producer still filling: keep order */

        render(rec, line, sizeof line / sizeof line[0]);
        emit_line(line);

        InterlockedExchange(&rec->ready, 0);
        _InterlockedExchange64(&g_read, r + 1);   /* release the slot */
        n++;
    }
    return n;
}

static DWORD WINAPI drain_thread(LPVOID unused)
{
    (void)unused;
    while (g_running) {
        (void)WaitForSingleObject(g_wake, 250);
        apr_log_drain();
    }
    apr_log_drain();
    return 0;
}

/* ------------------------------------------------------------------------ */

AprErr apr_log_init(const AprLogConfig *cfg)
{
    AprErr err = apr_ok();

    apr_log_shutdown();
    if (!cfg) return APR_ERR(APR_E_INVALID_ARG, L"apr_log_init(NULL)");

    memset((void *)g_ring, 0, sizeof g_ring);
    g_reserved    = 0;
    g_read        = 0;
    g_emitted     = 0;
    g_dropped     = 0;
    g_to_debugger = cfg->to_debugger;
    g_to_stderr   = cfg->to_stderr;

    if (cfg->path) {
        g_file = CreateFileW(cfg->path, FILE_APPEND_DATA, FILE_SHARE_READ,
                             NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_file == INVALID_HANDLE_VALUE) {
            err = APR_ERR_LAST(L"opening log file %ls", cfg->path);
        } else {
            (void)SetFilePointer(g_file, 0, NULL, FILE_END);
        }
    }

    apr_log_set_level(cfg->level);
    g_running = 1;

    if (cfg->background_drain) {
        g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_thread = CreateThread(NULL, 0, drain_thread, NULL, 0, NULL);
        if (!g_thread && !apr_failed(&err)) {
            err = APR_ERR_LAST(L"starting the log drain thread");
        }
    }
    return err;
}

void apr_log_shutdown(void)
{
    HANDLE thread = g_thread;

    g_running = 0;
    if (thread) {
        if (g_wake) SetEvent(g_wake);
        (void)WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
        g_thread = NULL;
    }
    if (g_wake) { CloseHandle(g_wake); g_wake = NULL; }

    apr_log_drain();                 /* flush whatever is still queued */

    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    g_sink        = NULL;
    g_sink_user   = NULL;
    g_to_debugger = 0;
    g_to_stderr   = 0;
    apr_log_set_level(APR_LOG_OFF);
}
