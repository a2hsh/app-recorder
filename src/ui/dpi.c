/*
 * dpi.c -- per-monitor DPI v2.
 *
 * Everything DPI-related that the rest of the UI is allowed to know about
 * lives here, and the header states the rule this file exists to enforce:
 * there is no process-wide scale factor.
 *
 * WHY GetProcAddress AND NOT A DIRECT CALL
 *
 *   GetDpiForWindow, GetSystemMetricsForDpi, AdjustWindowRectExForDpi and
 *   SystemParametersInfoForDpi arrived in Windows 10 1607; GetDpiForMonitor is
 *   in shcore.dll from 8.1; GetAwarenessFromDpiAwarenessContext is 1703. The
 *   product floor is Windows 10 2004 (WASAPI process loopback), so all of them
 *   are present on any machine that can run apprecorder at all -- but linking
 *   them statically would make the binary fail to LOAD on an older machine
 *   rather than fail to capture, and "this app does not start" is a much worse
 *   diagnostic than "process loopback is not available here". Resolving at
 *   runtime keeps the failure legible and costs one atomic read per call.
 *
 *   It also keeps this file the only place in the UI that knows these entry
 *   points might be missing. Nothing downstream has a fallback path.
 */
#include "ui_dpi.h"

#include <shellscalingapi.h>

#include "log.h"

/* --------------------------------------------------------------------------
 * Late-bound entry points
 * ----------------------------------------------------------------------- */

typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
typedef UINT (WINAPI *PFN_GetDpiForSystem)(void);
typedef int  (WINAPI *PFN_GetSystemMetricsForDpi)(int, UINT);
typedef BOOL (WINAPI *PFN_AdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef BOOL (WINAPI *PFN_SystemParametersInfoForDpi)(UINT, UINT, PVOID, UINT, UINT);
typedef DPI_AWARENESS_CONTEXT (WINAPI *PFN_GetThreadDpiAwarenessContext)(void);
typedef DPI_AWARENESS_CONTEXT (WINAPI *PFN_GetWindowDpiAwarenessContext)(HWND);
typedef DPI_AWARENESS (WINAPI *PFN_GetAwarenessFromDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
typedef BOOL (WINAPI *PFN_AreDpiAwarenessContextsEqual)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT *, UINT *);

typedef struct DpiApi {
    LONG                                    resolved;   /* 0 = not yet, 1 = done */
    PFN_GetDpiForWindow                     GetDpiForWindow;
    PFN_GetDpiForSystem                     GetDpiForSystem;
    PFN_GetSystemMetricsForDpi              GetSystemMetricsForDpi;
    PFN_AdjustWindowRectExForDpi            AdjustWindowRectExForDpi;
    PFN_SystemParametersInfoForDpi          SystemParametersInfoForDpi;
    PFN_GetThreadDpiAwarenessContext        GetThreadDpiAwarenessContext;
    PFN_GetWindowDpiAwarenessContext        GetWindowDpiAwarenessContext;
    PFN_GetAwarenessFromDpiAwarenessContext GetAwarenessFromDpiAwarenessContext;
    PFN_AreDpiAwarenessContextsEqual        AreDpiAwarenessContextsEqual;
    PFN_GetDpiForMonitor                    GetDpiForMonitor;
} DpiApi;

static DpiApi g_api;

static void dpi_resolve(void)
{
    HMODULE user32, shcore;

    if (InterlockedCompareExchange(&g_api.resolved, 0, 0) != 0) {
        return;
    }

    /* Racing threads both do this work and both write the same values. That is
     * benign: every field is a pointer written atomically on every target this
     * codebase supports, and the values are identical. The `resolved` flag is
     * published last, with a release so a reader that sees 1 sees the fields. */
    user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        g_api.GetDpiForWindow =
            (PFN_GetDpiForWindow)(void *)GetProcAddress(user32, "GetDpiForWindow");
        g_api.GetDpiForSystem =
            (PFN_GetDpiForSystem)(void *)GetProcAddress(user32, "GetDpiForSystem");
        g_api.GetSystemMetricsForDpi =
            (PFN_GetSystemMetricsForDpi)(void *)GetProcAddress(user32, "GetSystemMetricsForDpi");
        g_api.AdjustWindowRectExForDpi =
            (PFN_AdjustWindowRectExForDpi)(void *)GetProcAddress(user32, "AdjustWindowRectExForDpi");
        g_api.SystemParametersInfoForDpi =
            (PFN_SystemParametersInfoForDpi)(void *)GetProcAddress(user32, "SystemParametersInfoForDpi");
        g_api.GetThreadDpiAwarenessContext =
            (PFN_GetThreadDpiAwarenessContext)(void *)GetProcAddress(user32, "GetThreadDpiAwarenessContext");
        g_api.GetWindowDpiAwarenessContext =
            (PFN_GetWindowDpiAwarenessContext)(void *)GetProcAddress(user32, "GetWindowDpiAwarenessContext");
        g_api.GetAwarenessFromDpiAwarenessContext =
            (PFN_GetAwarenessFromDpiAwarenessContext)(void *)GetProcAddress(
                user32, "GetAwarenessFromDpiAwarenessContext");
        g_api.AreDpiAwarenessContextsEqual =
            (PFN_AreDpiAwarenessContextsEqual)(void *)GetProcAddress(
                user32, "AreDpiAwarenessContextsEqual");
    }

    /* shcore is not loaded by default in a lean process, so LoadLibrary rather
     * than GetModuleHandle. It is never freed: unloading a DLL whose function
     * pointer is cached in a global is a use-after-free waiting for a caller. */
    shcore = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (shcore) {
        g_api.GetDpiForMonitor =
            (PFN_GetDpiForMonitor)(void *)GetProcAddress(shcore, "GetDpiForMonitor");
    }

    InterlockedExchange(&g_api.resolved, 1);
}

/* --------------------------------------------------------------------------
 * Asking what the DPI is
 * ----------------------------------------------------------------------- */

static UINT dpi_sane(UINT dpi)
{
    /* A DPI outside this range is a driver or shim lying to us. 48 is half of
     * 96 and 960 is 1000% scaling; nothing real is outside it, and clamping
     * keeps every MulDiv below from producing a nonsense layout instead of a
     * merely ugly one. */
    if (dpi < 48u) return APR_DPI_DEFAULT;
    if (dpi > 960u) return 960u;
    return dpi;
}

UINT apr_dpi_system(void)
{
    HDC dc;
    UINT dpi = 0;

    dpi_resolve();
    if (g_api.GetDpiForSystem) {
        dpi = g_api.GetDpiForSystem();
    }
    if (dpi == 0) {
        dc = GetDC(NULL);
        if (dc) {
            dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(NULL, dc);
        }
    }
    return dpi_sane(dpi);
}

UINT apr_dpi_for_monitor(HMONITOR mon)
{
    UINT x = 0, y = 0;

    dpi_resolve();
    if (mon && g_api.GetDpiForMonitor) {
        if (SUCCEEDED(g_api.GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &x, &y)) && x != 0) {
            return dpi_sane(x);
        }
    }
    (void)y;
    return apr_dpi_system();
}

UINT apr_dpi_for_window(HWND hwnd)
{
    UINT dpi = 0;

    if (!hwnd) {
        return apr_dpi_system();
    }

    dpi_resolve();
    if (g_api.GetDpiForWindow) {
        dpi = g_api.GetDpiForWindow(hwnd);
    }
    if (dpi != 0) {
        return dpi_sane(dpi);
    }
    return apr_dpi_for_monitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
}

UINT apr_dpi_for_point(POINT pt)
{
    return apr_dpi_for_monitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
}

/* --------------------------------------------------------------------------
 * Arithmetic
 * ----------------------------------------------------------------------- */

int apr_dpi_scale(int logical, UINT dpi)
{
    if (dpi == 0) dpi = APR_DPI_DEFAULT;
    if (dpi == APR_DPI_DEFAULT) return logical;
    return MulDiv(logical, (int)dpi, (int)APR_DPI_DEFAULT);
}

int apr_dpi_scale_min(int logical, UINT dpi, int floor_px)
{
    int v = apr_dpi_scale(logical, dpi);
    if (logical > 0 && v < floor_px) return floor_px;
    if (logical < 0 && v > -floor_px) return -floor_px;
    return v;
}

int apr_dpi_unscale(int px, UINT dpi)
{
    if (dpi == 0) dpi = APR_DPI_DEFAULT;
    if (dpi == APR_DPI_DEFAULT) return px;
    return MulDiv(px, (int)APR_DPI_DEFAULT, (int)dpi);
}

int apr_dpi_rescale(int px, UINT from, UINT to)
{
    if (from == 0) from = APR_DPI_DEFAULT;
    if (to == 0) to = APR_DPI_DEFAULT;
    if (from == to) return px;
    return MulDiv(px, (int)to, (int)from);
}

void apr_dpi_rescale_size(RECT *r, UINT from, UINT to)
{
    int w, h;

    if (!r) return;
    w = apr_dpi_rescale(r->right - r->left, from, to);
    h = apr_dpi_rescale(r->bottom - r->top, from, to);
    r->right = r->left + w;
    r->bottom = r->top + h;
}

int apr_dpi_font_height(int point_size, UINT dpi)
{
    if (dpi == 0) dpi = APR_DPI_DEFAULT;
    /* Negative: LOGFONT treats a negative lfHeight as a character height,
     * which is what a point size means. A positive value is a cell height and
     * produces text noticeably smaller than asked for. */
    return -MulDiv(point_size, (int)dpi, 72);
}

/* --------------------------------------------------------------------------
 * System metrics at a DPI
 * ----------------------------------------------------------------------- */

int apr_dpi_system_metrics(int index, UINT dpi)
{
    dpi_resolve();
    if (g_api.GetSystemMetricsForDpi && dpi != 0) {
        return g_api.GetSystemMetricsForDpi(index, dpi);
    }
    /* Fallback: the system-DPI answer, rescaled. Wrong in the second decimal
     * place but never wrong by a factor of two, which is what matters. */
    return apr_dpi_rescale(GetSystemMetrics(index), apr_dpi_system(), dpi);
}

BOOL apr_dpi_adjust_window_rect(RECT *r, DWORD style, BOOL has_menu,
                                DWORD ex_style, UINT dpi)
{
    dpi_resolve();
    if (g_api.AdjustWindowRectExForDpi && dpi != 0) {
        return g_api.AdjustWindowRectExForDpi(r, style, has_menu, ex_style, dpi);
    }
    return AdjustWindowRectEx(r, style, has_menu, ex_style);
}

BOOL apr_dpi_nonclient_metrics(NONCLIENTMETRICSW *ncm, UINT dpi)
{
    if (!ncm) return FALSE;

    dpi_resolve();
    if (g_api.SystemParametersInfoForDpi && dpi != 0) {
        if (g_api.SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, ncm->cbSize,
                                             ncm, 0, dpi)) {
            return TRUE;
        }
    }
    return SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncm->cbSize, ncm, 0);
}

/* --------------------------------------------------------------------------
 * Awareness
 * ----------------------------------------------------------------------- */

AprDpiAwareness apr_dpi_awareness(void)
{
    DPI_AWARENESS_CONTEXT ctx;
    DPI_AWARENESS a;

    dpi_resolve();
    if (!g_api.GetThreadDpiAwarenessContext || !g_api.GetAwarenessFromDpiAwarenessContext) {
        return APR_DPI_UNKNOWN;
    }

    ctx = g_api.GetThreadDpiAwarenessContext();

    /* GetAwarenessFromDpiAwarenessContext cannot tell v2 from v1 -- both report
     * DPI_AWARENESS_PER_MONITOR_AWARE. AreDpiAwarenessContextsEqual against the
     * v2 pseudo-handle is the only way to separate them, and separating them is
     * the entire point of this function: v1 is the failure mode the manifest
     * exists to prevent, and it is invisible until someone drags the window. */
    if (g_api.AreDpiAwarenessContextsEqual &&
        g_api.AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return APR_DPI_PER_MONITOR_V2;
    }

    a = g_api.GetAwarenessFromDpiAwarenessContext(ctx);
    switch (a) {
    case DPI_AWARENESS_UNAWARE:             return APR_DPI_UNAWARE;
    case DPI_AWARENESS_SYSTEM_AWARE:        return APR_DPI_SYSTEM_AWARE;
    case DPI_AWARENESS_PER_MONITOR_AWARE:   return APR_DPI_PER_MONITOR;
    case DPI_AWARENESS_INVALID:             break;
    default:                                break;
    }
    return APR_DPI_UNKNOWN;
}

/* --------------------------------------------------------------------------
 * WM_DPICHANGED
 * ----------------------------------------------------------------------- */

UINT apr_dpi_handle_dpichanged(HWND hwnd, WPARAM wparam, LPARAM lparam)
{
    const RECT *suggested = (const RECT *)lparam;
    UINT dpi = (UINT)LOWORD(wparam);

    if (dpi == 0) {
        dpi = apr_dpi_for_window(hwnd);
    }

    /* The suggested rectangle is not advice. It is computed so the window stays
     * under the pointer that dragged it and so its size is the same physical
     * size on the new monitor; ignoring it and rescaling ourselves produces a
     * window that jumps out from under the mouse mid-drag. */
    if (hwnd && suggested) {
        SetWindowPos(hwnd, NULL,
                     suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    APR_DEBUG(L"WM_DPICHANGED: now %u", dpi);
    return dpi_sane(dpi);
}
