/* winver.c -- the one place that asks the OS what build it is.
 *
 * See winver.h for why the question is asked this way rather than through
 * GetVersionExW or VerifyVersionInfo.
 */
#include "winver.h"

#include <windows.h>

#include "log.h"

typedef void (WINAPI *PFN_RtlGetNtVersionNumbers)(DWORD *, DWORD *, DWORD *);

/* 0 means "not asked yet"; a real build number is never 0, so no separate
 * flag is needed. A benign race just asks ntdll twice and stores the same
 * answer, which is why this needs no lock. */
static volatile LONG g_build;

uint32_t apr_win_build(void)
{
    LONG cached = InterlockedCompareExchange(&g_build, 0, 0);
    HMODULE ntdll;
    PFN_RtlGetNtVersionNumbers fn;
    DWORD major = 0, minor = 0, build = 0;

    if (cached != 0) return (uint32_t)cached;

    /* GetModuleHandle, not LoadLibrary: ntdll is in every process already, and
     * a version check has no business raising the reference count of the one
     * module that can never be unloaded. */
    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        APR_WARN(L"winver: ntdll is not loaded; cannot read the build number");
        return 0;
    }

    fn = (PFN_RtlGetNtVersionNumbers)(void *)
             GetProcAddress(ntdll, "RtlGetNtVersionNumbers");
    if (!fn) {
        APR_WARN(L"winver: RtlGetNtVersionNumbers is missing");
        return 0;
    }

    fn(&major, &minor, &build);
    /* The top bits are a checked/free flag, not part of the number. */
    build &= 0x0FFFFFFFu;

    InterlockedExchange(&g_build, (LONG)build);
    APR_INFO(L"winver: Windows %lu.%lu build %lu (floor is %u)",
             (unsigned long)major, (unsigned long)minor,
             (unsigned long)build, (unsigned)APR_WIN_MIN_BUILD);
    return (uint32_t)build;
}

int apr_win_meets_floor(void)
{
    uint32_t b = apr_win_build();
    /* A machine that cannot answer is not a machine to start recording on:
     * every path below this depends on an API that may simply not be there. */
    return b != 0 && b >= APR_WIN_MIN_BUILD;
}
