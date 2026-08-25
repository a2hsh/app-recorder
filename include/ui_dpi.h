/*
 * ui_dpi.h -- per-monitor DPI v2, done for real.
 *
 * ===========================================================================
 * THE RULE
 *
 *   THERE IS NO PROCESS-WIDE SCALE FACTOR. Not a global, not a cached one
 *   read at startup, not "the primary monitor's DPI". A window has a DPI, it
 *   changes while the process runs, and two windows in the same process can
 *   have different ones at the same instant.
 *
 *   Every function here therefore takes the DPI as an argument. That is not
 *   ceremony -- it is the only shape in which the arithmetic can be correct,
 *   because it makes "which monitor?" a question the caller must answer rather
 *   than one the library can get wrong on its behalf.
 *
 *   The consequence for anyone laying out a window: get the DPI from the
 *   window (apr_dpi_for_window), not from the system, and recompute -- do not
 *   rescale a previously scaled value -- whenever WM_DPICHANGED arrives.
 *
 * ===========================================================================
 * WHY V2 AND NOT V1
 *
 *   Under per-monitor v1 the OS rescales nothing for you: the non-client area
 *   (title bar, borders, system menu), dialogs, menus and comctl32's own
 *   internal metrics all stay at the DPI the process started at. A window
 *   dragged from a 96 DPI monitor to a 192 DPI one gets a correctly resized
 *   client area inside a half-size title bar. V2 fixes exactly those, which is
 *   why the manifest asks for PerMonitorV2 and why this module only has to
 *   handle the client area.
 *
 *   Awareness is declared in res/apprecorder.manifest, not called at startup:
 *   SetProcessDpiAwarenessContext has to run before the first HWND exists and
 *   a manifest cannot lose that race. apr_dpi_awareness() reports what actually
 *   took effect so a test can prove the manifest reached the binary.
 *
 * ===========================================================================
 * ROUNDING
 *
 *   apr_dpi_scale uses MulDiv, which rounds to nearest and rounds halves away
 *   from zero. That matters more than it sounds: at 150% a 1px hairline scales
 *   to 2px rather than vanishing to 0, and a border that vanishes on one
 *   monitor and not another is a real bug that only shows up on hardware the
 *   author does not own. Anything that must never round to zero should still
 *   be clamped by its caller; apr_dpi_scale_min exists for that.
 *
 * THREAD SAFETY: everything here is pure arithmetic or a stateless OS query.
 * The one-time GetProcAddress resolution is idempotent and benign to race.
 */
#ifndef APPRECORDER_UI_DPI_H
#define APPRECORDER_UI_DPI_H

#include <windows.h>

#include "err.h"

/* The DPI at which every logical measurement in this codebase is expressed.
 * "Logical pixels", "design pixels" and "pixels at 96 DPI" all mean this. */
#define APR_DPI_DEFAULT 96u

/* ---------------------------------------------------------------------------
 * Asking what the DPI is
 * ------------------------------------------------------------------------- */

/* The DPI of `hwnd`'s monitor, as the OS will use it for that window.
 *
 * Never returns 0. Falls back, in order, to the monitor's effective DPI, then
 * to the system DPI, then to APR_DPI_DEFAULT -- so a caller never has to check
 * and never has to divide by zero. A NULL hwnd yields the system DPI. */
UINT apr_dpi_for_window(HWND hwnd);

/* The effective DPI of a monitor. Never returns 0. */
UINT apr_dpi_for_monitor(HMONITOR mon);

/* The effective DPI of the monitor containing `pt` (screen coordinates). */
UINT apr_dpi_for_point(POINT pt);

/* The system (primary monitor) DPI. Never returns 0.
 *
 * Use this only for a decision that genuinely has no window yet -- sizing the
 * very first window before it is created, for instance. It is the wrong answer
 * for anything on screen. */
UINT apr_dpi_system(void);

/* ---------------------------------------------------------------------------
 * Arithmetic
 * ------------------------------------------------------------------------- */

/* `logical` (a measurement at 96 DPI) in device pixels at `dpi`.
 * dpi == 0 is treated as APR_DPI_DEFAULT. Rounds to nearest. */
int apr_dpi_scale(int logical, UINT dpi);

/* As apr_dpi_scale, but never returns less than `floor_px` for a non-zero
 * input. For hairlines, focus rings and separators, which must not round away
 * at fractional scale factors. */
int apr_dpi_scale_min(int logical, UINT dpi, int floor_px);

/* Device pixels at `dpi` back to logical pixels. The inverse of apr_dpi_scale
 * only up to rounding -- do not round-trip a value you intend to keep. */
int apr_dpi_unscale(int px, UINT dpi);

/* Device pixels at `from` expressed at `to`. Used when a stored pixel size has
 * to survive a DPI change and the logical value is no longer available. Prefer
 * recomputing from the logical value where you still have it. */
int apr_dpi_rescale(int px, UINT from, UINT to);

/* Rescale a rectangle's SIZE about its top-left corner. Position is left
 * alone, because where a window belongs after a DPI change is a layout
 * decision, not an arithmetic one. */
void apr_dpi_rescale_size(RECT *r, UINT from, UINT to);

/* The LOGFONT lfHeight for `point_size` at `dpi`. Negative, i.e. a character
 * height rather than a cell height, which is what every UI font wants. */
int apr_dpi_font_height(int point_size, UINT dpi);

/* ---------------------------------------------------------------------------
 * System metrics and window geometry at a given DPI
 * ------------------------------------------------------------------------- */

/* GetSystemMetricsForDpi, falling back to GetSystemMetrics where that entry
 * point is missing. Use this instead of GetSystemMetrics anywhere the result
 * feeds a window's layout: SM_CXVSCROLL, SM_CYMENU, SM_CXFRAME and friends are
 * all per-monitor quantities under v2. */
int apr_dpi_system_metrics(int index, UINT dpi);

/* AdjustWindowRectExForDpi, falling back to AdjustWindowRectEx. */
BOOL apr_dpi_adjust_window_rect(RECT *r, DWORD style, BOOL has_menu,
                                DWORD ex_style, UINT dpi);

/* SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS), falling back to the
 * un-scaled call. `ncm->cbSize` must already be set. */
BOOL apr_dpi_nonclient_metrics(NONCLIENTMETRICSW *ncm, UINT dpi);

/* ---------------------------------------------------------------------------
 * Proving the manifest arrived
 * ------------------------------------------------------------------------- */

typedef enum AprDpiAwareness {
    APR_DPI_UNAWARE = 0,
    APR_DPI_SYSTEM_AWARE = 1,
    APR_DPI_PER_MONITOR = 2,       /* v1 */
    APR_DPI_PER_MONITOR_V2 = 3,    /* what the manifest asks for */
    APR_DPI_UNKNOWN = 4            /* too old to ask */
} AprDpiAwareness;

/* What this process's DPI awareness actually is, right now.
 *
 * This exists so that "the manifest is embedded" is a tested property rather
 * than a hopeful one: a linker-generated manifest replacing ours, or a CMake
 * change dropping res/apprecorder.rc, both show up here and nowhere else. */
AprDpiAwareness apr_dpi_awareness(void);

/* ---------------------------------------------------------------------------
 * WM_DPICHANGED
 * ------------------------------------------------------------------------- */

/* Handle WM_DPICHANGED for a top-level window: move and resize it to the
 * rectangle the OS suggests in lParam, which is the only correct response --
 * the suggestion keeps the window under the pointer that dragged it and
 * matches the monitor it is arriving on.
 *
 * Returns the new DPI (HIWORD/LOWORD of wParam, which are equal in practice).
 * The caller still owns re-creating fonts and re-running its layout; this
 * function deliberately does not, because it does not know what a caller's
 * layout is and a half-done job here would be worse than none. */
UINT apr_dpi_handle_dpichanged(HWND hwnd, WPARAM wparam, LPARAM lparam);

#endif /* APPRECORDER_UI_DPI_H */
