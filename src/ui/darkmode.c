/*
 * darkmode.c -- the undocumented uxtheme surface. The whole of it.
 *
 * The header states the four rules that make this survivable. This file's job
 * is to obey them, and the most important one is that NOTHING HERE CAN FAIL
 * UPWARDS: there is no AprErr in this translation unit, on purpose.
 *
 * ---------------------------------------------------------------------------
 * WHAT THE ORDINALS ARE, AND WHY THEY ARE ORDINALS
 *
 *   uxtheme.dll exports these without names. They are matched by ORDINAL only,
 *   which is why a signature change between builds is silent -- there is no
 *   symbol to fail to find, just an address that now points at something else.
 *   That is the entire risk of this file, and it is why every call sits behind
 *   a build-number floor and a resolved-successfully flag.
 *
 *     104  RefreshImmersiveColorPolicyState()
 *     132  ShouldAppsUseDarkMode()               -- deliberately NOT exposed
 *     133  AllowDarkModeForWindow(HWND, BOOL)
 *     135  AllowDarkModeForApp(BOOL)             -- 1809 only
 *     135  SetPreferredAppMode(PreferredAppMode) -- 1903 and later, SAME
 *                                                  ordinal, DIFFERENT
 *                                                  signature. This is the
 *                                                  concrete example of why
 *                                                  the build number is
 *                                                  checked rather than the
 *                                                  export.
 *     136  FlushMenuThemes()
 *
 *   ShouldAppsUseDarkMode (132) is resolved but never used by anything outside
 *   this file: theme.c reads the documented registry value instead, so our own
 *   painting stays correct even if every ordinal here goes missing. The one
 *   place we would want 132 -- deciding whether the user prefers dark -- is
 *   exactly the place where a wrong answer is most visible, so it uses the
 *   path that cannot break.
 *
 * ---------------------------------------------------------------------------
 * VERSION DETECTION
 *
 *   Asked of platform/winver.c, which owns it for the whole product -- the
 *   console front end needs the same number for its own floor. Why it is not
 *   GetVersionExW, and why the answer must be the OS's rather than our
 *   compatibility manifest's, is documented there. What matters here is only
 *   that the number is the running OS's, because the question this file asks
 *   is whether the uxtheme ORDINALS are the ones we expect.
 */
#include "ui_darkmode.h"

#include <dwmapi.h>
#include <uxtheme.h>

#include "log.h"
#include "winver.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

/* The floor: dark mode support arrived in 1809 / build 17763. */
#define APR_DARK_MIN_BUILD 17763u
/* SetPreferredAppMode replaced AllowDarkModeForApp at ordinal 135 in 1903. */
#define APR_DARK_SPAM_BUILD 18362u

/* Documented from the Windows 11 SDK; defined here so the build does not
 * depend on which SDK happens to be installed. 20 is the value that shipped;
 * 19 was the pre-release spelling and is tried as a fallback. */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#define APR_DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19

typedef void (WINAPI *PFN_RefreshImmersiveColorPolicyState)(void);
typedef BOOL (WINAPI *PFN_ShouldAppsUseDarkMode)(void);
typedef BOOL (WINAPI *PFN_AllowDarkModeForWindow)(HWND, BOOL);
typedef BOOL (WINAPI *PFN_AllowDarkModeForApp)(BOOL);
typedef int  (WINAPI *PFN_SetPreferredAppMode)(int);
typedef void (WINAPI *PFN_FlushMenuThemes)(void);

typedef struct DarkApi {
    LONG state;              /* 0 = untried, 1 = ready, 2 = unavailable */
    DWORD build;
    PFN_RefreshImmersiveColorPolicyState RefreshImmersiveColorPolicyState;
    PFN_ShouldAppsUseDarkMode            ShouldAppsUseDarkMode;
    PFN_AllowDarkModeForWindow           AllowDarkModeForWindow;
    PFN_AllowDarkModeForApp              AllowDarkModeForApp;
    PFN_SetPreferredAppMode              SetPreferredAppMode;
    PFN_FlushMenuThemes                  FlushMenuThemes;
} DarkApi;

static DarkApi g_dark;

/* The build number now has one owner, platform/winver.c, because the console
 * front end needs the same answer for its own floor and two copies of "what
 * Windows is this" is exactly the duplication AGENTS.md rule 3 forbids. The
 * reasoning that used to live here -- why not GetVersionExW, why not
 * VerifyVersionInfo -- moved there with it.
 *
 * The major-version test that used to be here is gone rather than lost: it
 * asked "is this Windows 10 or later" and returned 0 otherwise, but the only
 * caller immediately compares against APR_DARK_MIN_BUILD (17763), which no
 * pre-10 build can reach. */
static DWORD dark_os_build(void)
{
    return (DWORD)apr_win_build();
}

void apr_darkmode_init(void)
{
    HMODULE ux;
    DWORD build;

    if (InterlockedCompareExchange(&g_dark.state, 0, 0) != 0) return;

    build = dark_os_build();
    g_dark.build = build;

    if (build < APR_DARK_MIN_BUILD) {
        APR_INFO(L"darkmode: OS build %lu is below %u; system chrome stays light",
                 (unsigned long)build, (unsigned)APR_DARK_MIN_BUILD);
        InterlockedExchange(&g_dark.state, 2);
        return;
    }

    ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) {
        APR_WARN(L"darkmode: uxtheme.dll would not load; system chrome stays light");
        InterlockedExchange(&g_dark.state, 2);
        return;
    }
    /* Never FreeLibrary: the pointers below outlive this function and uxtheme
     * is loaded in every GUI process anyway. */

    g_dark.RefreshImmersiveColorPolicyState =
        (PFN_RefreshImmersiveColorPolicyState)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(104));
    g_dark.ShouldAppsUseDarkMode =
        (PFN_ShouldAppsUseDarkMode)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(132));
    g_dark.AllowDarkModeForWindow =
        (PFN_AllowDarkModeForWindow)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    g_dark.FlushMenuThemes =
        (PFN_FlushMenuThemes)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));

    /* Ordinal 135 is the one that changed meaning. Bind it to whichever
     * signature this build actually has -- calling the 1903 signature on 1809
     * would pass an int where a BOOL is expected, which happens to be
     * harmless, and calling the 1809 signature on 1903 would set app mode 1
     * ("allow dark") when asked for FORCE_LIGHT. Neither is a crash; both are
     * wrong, and being wrong quietly is the thing this file is built to
     * avoid. */
    if (build >= APR_DARK_SPAM_BUILD) {
        g_dark.SetPreferredAppMode =
            (PFN_SetPreferredAppMode)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    } else {
        g_dark.AllowDarkModeForApp =
            (PFN_AllowDarkModeForApp)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    }

    /* AllowDarkModeForWindow is the minimum useful set: without it nothing
     * per-window can be themed and the rest is not worth calling. */
    if (!g_dark.AllowDarkModeForWindow ||
        (!g_dark.SetPreferredAppMode && !g_dark.AllowDarkModeForApp)) {
        APR_WARN(L"darkmode: uxtheme ordinals did not resolve on build %lu; "
                 L"system chrome stays light",
                 (unsigned long)build);
        InterlockedExchange(&g_dark.state, 2);
        return;
    }

    APR_INFO(L"darkmode: available on build %lu", (unsigned long)build);
    InterlockedExchange(&g_dark.state, 1);
}

int apr_darkmode_available(void)
{
    apr_darkmode_init();
    return InterlockedCompareExchange(&g_dark.state, 0, 0) == 1;
}

int apr_darkmode_system_prefers_dark(void)
{
    HKEY key;
    DWORD value = 1, cb = sizeof value, type = 0;
    int dark = 0;

    /* Documented setting, documented location, no ordinals involved. This is
     * the answer the product acts on; the uxtheme surface only styles chrome
     * we do not paint. */
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, L"AppsUseLightTheme", NULL, &type,
                             (LPBYTE)&value, &cb) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            dark = (value == 0);
        }
        RegCloseKey(key);
    }
    return dark;
}

void apr_darkmode_set_app_mode(AprAppMode mode)
{
    if (!apr_darkmode_available()) return;

    if (g_dark.SetPreferredAppMode) {
        g_dark.SetPreferredAppMode((int)mode);
    } else if (g_dark.AllowDarkModeForApp) {
        g_dark.AllowDarkModeForApp(mode == APR_APPMODE_ALLOW_DARK ||
                                   mode == APR_APPMODE_FORCE_DARK);
    }
    if (g_dark.RefreshImmersiveColorPolicyState) {
        g_dark.RefreshImmersiveColorPolicyState();
    }
}

void apr_darkmode_apply_to_window(HWND hwnd, int dark)
{
    BOOL on = dark ? TRUE : FALSE;

    if (!hwnd || !IsWindow(hwnd)) return;

    /* Documented path first. On Windows 11 this alone darkens the title bar,
     * and it works whether or not a single ordinal resolved. Attribute 20 is
     * the shipped spelling; 19 was the 20H1 pre-release one and is tried when
     * 20 is rejected, which is what an older build does. */
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &on, sizeof on))) {
        (void)DwmSetWindowAttribute(hwnd, APR_DWMWA_USE_IMMERSIVE_DARK_MODE_OLD,
                                    &on, sizeof on);
    }

    if (!apr_darkmode_available()) return;

    g_dark.AllowDarkModeForWindow(hwnd, on);

    /* "DarkMode_Explorer" is the theme class comctl32 checks when deciding how
     * to draw the pieces we never touch -- most visibly a TreeView's or
     * ListView's scroll bars, which stay bright white inside an otherwise dark
     * pane without it. Passing NULL restores the default. SetWindowTheme is
     * documented; only the class NAME is undocumented, and a name the OS does
     * not recognise is ignored rather than fatal. */
    (void)SetWindowTheme(hwnd, on ? L"DarkMode_Explorer" : NULL, NULL);

    /* Force the non-client area to repaint with the new frame colour. */
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_FRAMECHANGED);
}

void apr_darkmode_flush_menu_theme(void)
{
    if (!apr_darkmode_available()) return;
    if (g_dark.FlushMenuThemes) g_dark.FlushMenuThemes();
    if (g_dark.RefreshImmersiveColorPolicyState) {
        g_dark.RefreshImmersiveColorPolicyState();
    }
}

int apr_darkmode_is_color_scheme_change(UINT msg, LPARAM lparam)
{
    const wchar_t *area;

    if (msg != WM_SETTINGCHANGE) return 0;
    area = (const wchar_t *)lparam;
    if (!area) return 0;
    /* The OS sends this with lParam = "ImmersiveColorSet" when the dark-mode
     * toggle flips. It also sends unrelated WM_SETTINGCHANGEs constantly, so
     * the string comparison is what keeps a theme rebuild from running on
     * every mouse-speed change. */
    return CompareStringOrdinal(area, -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL;
}
