/*
 * test_log.c -- platform/log.c.
 *
 * The properties under test are the ones the design actually depends on:
 * a disabled call costs nothing and evaluates nothing, a capture thread can
 * log without allocating or blocking, and a starved drain loses records
 * countably rather than silently or corruptly.
 */
#include "test_runner.h"
#include "log.h"
#include "err.h"

#include <windows.h>

/* ---- a sink that captures into memory, so tests need no files ----------- */

#define CAP_MAX 4096

static wchar_t g_cap[CAP_MAX][256];
static long    g_cap_count;
static long    g_cap_bad;          /* records whose payload did not survive */
static CRITICAL_SECTION g_cap_lock;

static void cap_sink(const wchar_t *line, void *user)
{
    long i;
    (void)user;
    EnterCriticalSection(&g_cap_lock);
    i = g_cap_count++;
    if (i < CAP_MAX) {
        wcsncpy_s(g_cap[i], 256, line, _TRUNCATE);
    }
    LeaveCriticalSection(&g_cap_lock);
}

static void cap_reset(void)
{
    g_cap_count = 0;
    g_cap_bad   = 0;
}

static int cap_contains(const wchar_t *needle)
{
    long i;
    long n = g_cap_count < CAP_MAX ? g_cap_count : CAP_MAX;
    for (i = 0; i < n; i++) {
        if (wcsstr(g_cap[i], needle)) return 1;
    }
    return 0;
}

static void log_start(AprLogLevel level)
{
    AprLogConfig cfg;
    static int lock_ready;

    if (!lock_ready) { InitializeCriticalSection(&g_cap_lock); lock_ready = 1; }

    memset(&cfg, 0, sizeof cfg);
    cfg.level = level;
    /* No file, no debugger, no background thread: the test drives the drain so
     * nothing in this suite depends on timing. */
    apr_log_init(&cfg);
    apr_log_set_sink(cap_sink, NULL);
    cap_reset();
}

/* ---- level gating ------------------------------------------------------- */

TEST(records_below_the_level_are_not_emitted)
{
    log_start(APR_LOG_WARN);

    APR_INFO(L"chatty %d", 1);
    APR_WARN(L"important %d", 2);
    APR_ERROR(L"fatal %d", 3);
    apr_log_drain();

    ASSERT_FALSE(cap_contains(L"chatty"));
    ASSERT_TRUE(cap_contains(L"important"));
    ASSERT_TRUE(cap_contains(L"fatal"));
    apr_log_shutdown();
}

static int g_side_effects;
static int bump(void) { g_side_effects++; return g_side_effects; }

TEST(a_disabled_call_does_not_evaluate_its_arguments)
{
    log_start(APR_LOG_ERROR);
    g_side_effects = 0;

    APR_TRACE(L"trace %d", bump());
    APR_DEBUG(L"debug %d", bump());
    APR_INFO(L"info %d",  bump());
    APR_WARN(L"warn %d",  bump());
    apr_log_drain();

    ASSERT_EQ_INT(0, g_side_effects);

    APR_ERROR(L"error %d", bump());
    apr_log_drain();
    ASSERT_EQ_INT(1, g_side_effects);
    apr_log_shutdown();
}

TEST(level_can_be_changed_at_runtime)
{
    log_start(APR_LOG_ERROR);
    ASSERT_EQ_INT(APR_LOG_ERROR, apr_log_get_level());
    ASSERT_FALSE(apr_log_enabled(APR_LOG_INFO));

    apr_log_set_level(APR_LOG_TRACE);
    ASSERT_TRUE(apr_log_enabled(APR_LOG_INFO));

    APR_INFO(L"now visible");
    apr_log_drain();
    ASSERT_TRUE(cap_contains(L"now visible"));
    apr_log_shutdown();
}

/* ---- rendering ---------------------------------------------------------- */

TEST(a_line_carries_level_thread_origin_and_message)
{
    wchar_t tid[32];
    log_start(APR_LOG_TRACE);

    APR_WARN(L"buffer %ls short by %d", L"Chat Mic", 64);
    apr_log_drain();

    ASSERT_EQ_INT(1, g_cap_count);
    ASSERT_NOT_NULL(wcsstr(g_cap[0], L"WARN"));
    ASSERT_NOT_NULL(wcsstr(g_cap[0], L"buffer Chat Mic short by 64"));
    ASSERT_NOT_NULL(wcsstr(g_cap[0], L"test_log.c"));

    _snwprintf_s(tid, 32, _TRUNCATE, L"%lu", GetCurrentThreadId());
    ASSERT_NOT_NULL(wcsstr(g_cap[0], tid));
    apr_log_shutdown();
}

TEST(an_apr_error_can_be_logged_whole)
{
    AprErr e;
    log_start(APR_LOG_TRACE);

    e = APR_ERR_HR(0x88890004L /* AUDCLNT_E_DEVICE_INVALIDATED */,
                   L"starting %ls", L"Stream Mix 1");
    APR_LOG_ERR(APR_LOG_ERROR, &e);
    apr_log_drain();

    ASSERT_EQ_INT(1, g_cap_count);
    ASSERT_NOT_NULL(wcsstr(g_cap[0], L"starting Stream Mix 1"));
    ASSERT_NOT_NULL(wcsstr(g_cap[0], L"AUDCLNT_E_DEVICE_INVALIDATED"));
    apr_log_shutdown();
}

TEST(records_drain_in_the_order_they_were_logged)
{
    int i;
    log_start(APR_LOG_TRACE);

    for (i = 0; i < 32; i++) APR_INFO(L"seq %d", i);
    apr_log_drain();

    ASSERT_EQ_INT(32, g_cap_count);
    for (i = 0; i < 32; i++) {
        wchar_t want[32];
        _snwprintf_s(want, 32, _TRUNCATE, L"seq %d", i);
        if (!wcsstr(g_cap[i], want)) {
            FAIL("records drained out of order");
        }
    }
    apr_log_shutdown();
}

/* ---- the real-time path ------------------------------------------------- */

TEST(the_rt_path_defers_formatting_to_the_drain)
{
    log_start(APR_LOG_TRACE);

    APR_RT0(APR_LOG_WARN, L"capture gap");
    APR_RT1(APR_LOG_WARN, L"gap of %lld frames", 480);
    APR_RT2(APR_LOG_WARN, L"source %lld overran by %lld frames", 3, 1024);
    apr_log_drain();

    ASSERT_EQ_INT(3, g_cap_count);
    ASSERT_NOT_NULL(wcsstr(g_cap[0], L"capture gap"));
    ASSERT_NOT_NULL(wcsstr(g_cap[1], L"gap of 480 frames"));
    ASSERT_NOT_NULL(wcsstr(g_cap[2], L"source 3 overran by 1024 frames"));
    apr_log_shutdown();
}

TEST(rt_records_are_gated_by_level_like_any_other)
{
    log_start(APR_LOG_ERROR);
    APR_RT1(APR_LOG_INFO, L"quiet %lld", 1);
    APR_RT1(APR_LOG_ERROR, L"loud %lld", 2);
    apr_log_drain();

    ASSERT_EQ_INT(1, g_cap_count);
    ASSERT_NOT_NULL(wcsstr(g_cap[0], L"loud 2"));
    apr_log_shutdown();
}

/* ---- overflow ----------------------------------------------------------- */

TEST(a_full_ring_drops_countably_and_keeps_the_oldest)
{
    int      i;
    int      overshoot = 50;
    uint64_t emitted = 0, dropped = 0;

    log_start(APR_LOG_TRACE);

    for (i = 0; i < APR_LOG_RING_SLOTS + overshoot; i++) APR_INFO(L"seq %d", i);

    apr_log_stats(&emitted, &dropped);
    ASSERT_EQ_U64((uint64_t)APR_LOG_RING_SLOTS, emitted);
    ASSERT_EQ_U64((uint64_t)overshoot, dropped);

    apr_log_drain();
    ASSERT_EQ_INT(APR_LOG_RING_SLOTS, g_cap_count);

    /* Log overflow drops the NEWEST record, unlike core/ringbuf.c which drops
     * the oldest. A log ring only overflows when the drain is starved, and the
     * records already queued are the context that explains what went wrong. */
    ASSERT_NOT_NULL(wcsstr(g_cap[0], L"seq 0"));
    ASSERT_FALSE(cap_contains(L"seq 300"));
    apr_log_shutdown();
}

TEST(the_ring_recovers_after_a_drain)
{
    int i;
    log_start(APR_LOG_TRACE);

    for (i = 0; i < APR_LOG_RING_SLOTS + 10; i++) APR_INFO(L"first %d", i);
    apr_log_drain();
    cap_reset();

    for (i = 0; i < 8; i++) APR_INFO(L"second %d", i);
    apr_log_drain();
    ASSERT_EQ_INT(8, g_cap_count);
    ASSERT_TRUE(cap_contains(L"second 7"));
    apr_log_shutdown();
}

/* ---- concurrency -------------------------------------------------------- */

#define PRODUCERS      4
#define PER_PRODUCER 500

static volatile LONG g_go;

static DWORD WINAPI producer(LPVOID param)
{
    LONG_PTR which = (LONG_PTR)param;
    int i;

    while (!g_go) YieldProcessor();
    for (i = 0; i < PER_PRODUCER; i++) {
        /* b == a * 2 is the payload invariant the drain side re-checks. */
        APR_RT2(APR_LOG_INFO, L"rt a=%lld b=%lld",
                (long long)(which * PER_PRODUCER + i),
                (long long)(which * PER_PRODUCER + i) * 2);
    }
    return 0;
}

TEST(concurrent_producers_never_corrupt_a_record)
{
    HANDLE   th[PRODUCERS];
    LONG_PTR i;
    uint64_t emitted = 0, dropped = 0;
    long     n, k, checked = 0;

    log_start(APR_LOG_TRACE);
    g_go = 0;

    for (i = 0; i < PRODUCERS; i++) {
        th[i] = CreateThread(NULL, 0, producer, (LPVOID)i, 0, NULL);
        ASSERT_NOT_NULL(th[i]);
    }
    InterlockedExchange(&g_go, 1);

    /* Drain while they run: this is the interesting interleaving. */
    for (k = 0; k < 10000; k++) {
        apr_log_drain();
        if (WaitForMultipleObjects(PRODUCERS, th, TRUE, 0) == WAIT_OBJECT_0) break;
    }
    WaitForMultipleObjects(PRODUCERS, th, TRUE, 5000);
    apr_log_drain();
    for (i = 0; i < PRODUCERS; i++) CloseHandle(th[i]);

    apr_log_stats(&emitted, &dropped);
    ASSERT_EQ_U64((uint64_t)(PRODUCERS * PER_PRODUCER), emitted + dropped);
    ASSERT_EQ_U64(emitted, (uint64_t)g_cap_count);

    n = g_cap_count < CAP_MAX ? g_cap_count : CAP_MAX;
    for (k = 0; k < n; k++) {
        const wchar_t *p = wcsstr(g_cap[k], L"rt a=");
        long long a = -1, b = -1;
        if (!p || swscanf_s(p, L"rt a=%lld b=%lld", &a, &b) != 2 || b != a * 2) {
            g_cap_bad++;
            continue;
        }
        checked++;
    }
    ASSERT_EQ_INT(0, g_cap_bad);
    ASSERT_EQ_INT(n, checked);
    apr_log_shutdown();
}

TEST(shutdown_flushes_what_is_still_queued)
{
    log_start(APR_LOG_TRACE);
    APR_INFO(L"last words");
    apr_log_shutdown();               /* without an explicit drain */
    ASSERT_TRUE(cap_contains(L"last words"));
}

TEST(logging_before_init_is_harmless)
{
    apr_log_shutdown();               /* make sure we are uninitialised */
    APR_ERROR(L"into the void");
    APR_RT1(APR_LOG_ERROR, L"also void %lld", 1);
    ASSERT_EQ_U64(0, apr_log_drain());   /* nothing queued, nothing emitted */
}
