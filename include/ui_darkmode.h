/*
 * ui_darkmode.h -- the undocumented uxtheme dark-mode surface, quarantined.
 *
 * ===========================================================================
 * WHY THIS FILE EXISTS AT ALL
 *
 *   Windows has no public API for "make this app's system-drawn chrome dark".
 *   Painting our own surfaces dark is easy and lives in theme.c; what needs
 *   uxtheme is everything WE do not paint -- the title bar, the scroll bars
 *   inside a TreeView, the menu bar's popup backgrounds, the context menus.
 *   Those are drawn by comctl32 and DWM, and the only way to reach them is
 *   ordinals in uxtheme.dll that Microsoft has never documented and has
 *   renumbered and re-signatured between builds.
 *
 *   So this is a calculated risk, taken once, in one file. The rules that make
 *   it survivable:
 *
 *     1. NOTHING HERE RETURNS AN ERROR. Every function is void or int. There
 *        is no failure for a caller to handle, therefore no caller can handle
 *        it wrongly, therefore a Windows update that renumbers an ordinal
 *        cannot take the app down -- it can only make the title bar light.
 *
 *     2. NOTHING ELSE IN THE UI INCLUDES THIS. theme.c reads the user's dark
 *        preference from the documented registry value, not from
 *        ShouldAppsUseDarkMode, so our own painting is correct even when every
 *        ordinal here fails to resolve.
 *
 *     3. THE BUILD NUMBER IS CHECKED BEFORE ANY ORDINAL IS CALLED. The
 *        ordinals are only known-good from 1809 (17763). Below that we do
 *        nothing. Above the last build tested we still try -- refusing to try
 *        would freeze the feature at whatever build this was written on -- but
 *        every call is guarded and the whole module is one flag away from off.
 *
 *     4. THE TITLE BAR USES THE DOCUMENTED PATH WHERE ONE EXISTS.
 *        DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE) is documented
 *        from Windows 11 and is tried first; the ordinal is the fallback, not
 *        the plan.
 *
 * If this file is ever the suspect in a crash, deleting every call to it must
 * leave a working, if light-chromed, application. Keep it that way.
 */
#ifndef APPRECORDER_UI_DARKMODE_H
#define APPRECORDER_UI_DARKMODE_H

#include <windows.h>

/* Mirrors uxtheme's undocumented PreferredAppMode. Values are the ones the OS
 * uses; do not renumber. */
typedef enum AprAppMode {
    APR_APPMODE_DEFAULT     = 0,
    APR_APPMODE_ALLOW_DARK  = 1,
    APR_APPMODE_FORCE_DARK  = 2,
    APR_APPMODE_FORCE_LIGHT = 3
} AprAppMode;

/* Resolve what can be resolved. Idempotent, never fails, safe to call before
 * any window exists. Every other function calls it, so calling it explicitly
 * is optional -- do it at startup anyway, so the log line lands before the
 * first window rather than in the middle of painting one. */
void apr_darkmode_init(void);

/* Nonzero when the undocumented entry points resolved and this build is one we
 * are willing to call them on. When zero, every other function is a no-op and
 * the app is simply light-chromed. */
int apr_darkmode_available(void);

/* Nonzero when the USER has asked for dark mode.
 *
 * Read from HKCU\...\Themes\Personalize\AppsUseLightTheme -- documented
 * behaviour of a documented setting, and available whether or not the
 * ordinals resolved. This is what theme.c uses; ShouldAppsUseDarkMode is
 * deliberately not exposed. */
int apr_darkmode_system_prefers_dark(void);

/* Tell the OS how this process would like its system-drawn chrome. No-op when
 * unavailable. */
void apr_darkmode_set_app_mode(AprAppMode mode);

/* Apply to one window: the title bar (documented DWM attribute first), the
 * window's own themed parts, and the scroll bars of common controls inside it.
 *
 * Call for the frame and for any child that owns a scroll bar -- a TreeView or
 * a ListView -- after creating it and again whenever dark changes. No-op when
 * unavailable, and harmless on a window that has nothing to change. */
void apr_darkmode_apply_to_window(HWND hwnd, int dark);

/* Menus are themed per-process rather than per-window, and the flush is what
 * makes an already-open menu bar pick up a change. Call after
 * apr_darkmode_set_app_mode when dark is toggled at runtime. No-op when
 * unavailable. */
void apr_darkmode_flush_menu_theme(void);

/* Nonzero when `msg`/`lparam` is the WM_SETTINGCHANGE that announces a
 * dark-mode flip ("ImmersiveColorSet"). Pure predicate; changes nothing. */
int apr_darkmode_is_color_scheme_change(UINT msg, LPARAM lparam);

#endif /* APPRECORDER_UI_DARKMODE_H */
