/*
 * app.c -- the frame.
 *
 * Window class, menu, status bar, panes, layout, keyboard, message loop. The
 * two conventions this file establishes for everything built on top of it --
 * how direction is threaded through, and how every control gets a name -- are
 * stated in ui_app.h and are not repeated here; what follows are the reasons
 * behind the parts that look arbitrary.
 *
 * ---------------------------------------------------------------------------
 * WHY THE FRAME HAS A MENU BAR AT ALL
 *
 *   A canvas app does not obviously need one. It needs one here because the
 *   menu bar is the only surface that makes an operation DISCOVERABLE without
 *   sight: Alt opens it, arrow keys walk it, and a screen reader reads every
 *   item, its state and its shortcut. An operation reachable only by a canvas
 *   gesture is, for this app's first user, an operation that does not exist.
 *
 *   That is also why unimplemented commands are greyed rather than absent.
 *   Grey says "later" to someone who can see it; absent says "never" to
 *   someone who cannot.
 *
 * ---------------------------------------------------------------------------
 * WHY THE PANES ARE REAL CHILD WINDOWS
 *
 *   Design 6.1: real HWNDs get MSAA/UIA, focus and tab order from the
 *   platform. The same argument applies one level up -- if the two panes were
 *   regions of the frame's client area rather than windows, there would be
 *   nothing for a screen reader to land on when the user pressed F6, and
 *   nothing for the a11y test to find. So the frame's client area is two
 *   windows and a splitter, even before either pane has any content.
 *
 * ---------------------------------------------------------------------------
 * THE MESSAGE LOOP IS NOT BOILERPLATE
 *
 *   TranslateAccelerator before IsDialogMessage before TranslateMessage, in
 *   that order, and each one matters:
 *
 *     - accelerators first, or Ctrl+S is consumed as a keystroke by whichever
 *       control has focus;
 *     - IsDialogMessage next, which is what makes Tab, Shift+Tab and the arrow
 *       keys move between WS_TABSTOP children of a plain window -- without it
 *       Tab does nothing at all and the app is unusable by keyboard;
 *     - F6 is intercepted BEFORE IsDialogMessage, because IsDialogMessage
 *       swallows it.
 */
#include "ui_app.h"

#include <commctrl.h>
#include <windowsx.h>   /* GET_X_LPARAM / GET_Y_LPARAM */
#include <objbase.h>
/* initguid.h makes the DEFINE_GUID declarations in oleacc.h emit actual
 * definitions in this translation unit. Without it CLSID_AccPropServices
 * and the PROPID_ACC_* property ids are unresolved externals: they are not
 * in uuid.lib, unlike most Windows GUIDs. Must precede oleacc.h. */
#include <initguid.h>
#include <oleacc.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "ui_darkmode.h"
#include "ui_dpi.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "oleacc.lib")

#define APR_FRAME_CLASS L"AprRecorderFrame"
#define APR_PANE_CLASS  L"AprRecorderPane"

#define APR_ID_STATUS   0x1001
#define APR_ID_SPLITTER 0x1002
#define APR_ID_PANE_0   0x1010   /* + slot */

/* --------------------------------------------------------------------------
 * Direction
 * ----------------------------------------------------------------------- */

AprUiDir apr_ui_dir(void)
{
    return apr_str_is_rtl() ? APR_DIR_RTL : APR_DIR_LTR;
}

void apr_ui_mirror_rect(RECT *r, const RECT *bounds, AprUiDir dir)
{
    LONG l, w;

    if (!r || !bounds || dir != APR_DIR_RTL) return;
    w = r->right - r->left;
    l = bounds->left + (bounds->right - r->right);
    r->left = l;
    r->right = l + w;
}

int apr_ui_lead_x(const RECT *bounds, int lead, int w, AprUiDir dir)
{
    if (!bounds) return 0;
    if (dir == APR_DIR_RTL) return (int)bounds->right - lead - w;
    return (int)bounds->left + lead;
}

void apr_ui_apply_rtl(HWND control, AprUiDir dir)
{
    LONG_PTR ex;

    if (!control || !IsWindow(control)) return;
    ex = GetWindowLongPtrW(control, GWL_EXSTYLE);
    if (dir == APR_DIR_RTL) {
        ex |= (LONG_PTR)(WS_EX_LAYOUTRTL | WS_EX_RTLREADING);
    } else {
        ex &= ~(LONG_PTR)(WS_EX_LAYOUTRTL | WS_EX_RTLREADING);
    }
    SetWindowLongPtrW(control, GWL_EXSTYLE, ex);
    /* The style is read at paint time, so a repaint is required for an already
     * visible control. */
    SetWindowPos(control, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_FRAMECHANGED);
    InvalidateRect(control, NULL, TRUE);
}

/* --------------------------------------------------------------------------
 * Layout -- pure
 * ----------------------------------------------------------------------- */

void apr_ui_layout(const AprUiLayoutIn *in, AprUiRects *out)
{
    RECT body;
    int cw, ch, tree_w, split_w, status_h;

    if (!in || !out) return;
    memset(out, 0, sizeof *out);

    cw = (int)(in->client.right - in->client.left);
    ch = (int)(in->client.bottom - in->client.top);
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;

    status_h = in->status_h;
    if (status_h < 0) status_h = 0;
    if (status_h > ch) status_h = ch;

    /* The status bar spans the full width and does not mirror: it is the same
     * strip in both directions, and only the text inside it reads right to
     * left (which the control does itself, given WS_EX_LAYOUTRTL). */
    out->status.left   = in->client.left;
    out->status.right  = in->client.right;
    out->status.top    = in->client.bottom - status_h;
    out->status.bottom = in->client.bottom;

    body = in->client;
    body.bottom -= status_h;
    if (body.bottom < body.top) body.bottom = body.top;

    if (!in->tree_visible) {
        out->canvas = body;
        return;
    }

    tree_w = in->tree_w;
    if (tree_w < in->pane_min_w) tree_w = in->pane_min_w;
    split_w = in->splitter_w;
    if (split_w < 0) split_w = 0;

    /* If there is not room for both panes at their minimum, the canvas keeps
     * what is left and the panel shrinks -- never the other way round, because
     * a canvas narrower than a node is useless while a narrow tree is merely
     * cramped. Below the point where even that fails, the panel is dropped
     * from the layout and the canvas takes everything: a zero-width window
     * would still be in the tab order and in the accessibility tree, which is
     * a focus trap with nothing in it. */
    if (tree_w + split_w + in->pane_min_w > (int)(body.right - body.left)) {
        tree_w = (int)(body.right - body.left) - split_w - in->pane_min_w;
    }
    if (tree_w < in->pane_min_w / 2 || tree_w <= 0) {
        out->canvas = body;
        return;
    }

    /* Computed against the LEADING edge, then mirrored. One mirroring site. */
    out->tree = body;
    out->tree.left  = body.left;
    out->tree.right = body.left + tree_w;

    out->splitter = body;
    out->splitter.left  = out->tree.right;
    out->splitter.right = out->splitter.left + split_w;

    out->canvas = body;
    out->canvas.left  = out->splitter.right;
    out->canvas.right = body.right;

    apr_ui_mirror_rect(&out->tree, &body, in->dir);
    apr_ui_mirror_rect(&out->splitter, &body, in->dir);
    apr_ui_mirror_rect(&out->canvas, &body, in->dir);
}

/* --------------------------------------------------------------------------
 * Accessible names
 * ----------------------------------------------------------------------- */

/* THREAD-LOCAL, NOT GLOBAL, AND THAT IS A CORRECTNESS POINT.
 *
 * IAccPropServices is created in the calling thread's apartment. A window
 * thread is apartment-threaded, so the pointer is only valid on the thread
 * that created it; caching it in a plain global would hand a second UI thread
 * a raw pointer into an apartment that may already have been torn down. One
 * instance per thread, released by apr_ui_app_destroy on the same thread that
 * created it. */
static __declspec(thread) IAccPropServices *t_acc_props;
static __declspec(thread) int t_acc_tried;

static IAccPropServices *acc_props(void)
{
    HRESULT hr;

    if (!t_acc_tried) {
        t_acc_tried = 1;
        hr = CoCreateInstance(&CLSID_AccPropServices, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IAccPropServices, (void **)&t_acc_props);
        if (FAILED(hr) || !t_acc_props) {
            t_acc_props = NULL;
            /* Not fatal. Window text still carries a name for most controls;
             * what is lost is the ability to name a control whose text is its
             * content. Say so once, loudly enough to find in a log. */
            APR_WARN(L"a11y: IAccPropServices unavailable (hr=0x%08lX); "
                     L"accessible names fall back to window text",
                     (unsigned long)hr);
        }
    }
    return t_acc_props;
}

static void acc_release_for_thread(void)
{
    if (t_acc_props) {
        IAccPropServices_Release(t_acc_props);
        t_acc_props = NULL;
    }
    t_acc_tried = 0;
}

static void acc_set(HWND hwnd, const MSAAPROPID *prop, const wchar_t *text)
{
    IAccPropServices *p;

    if (!hwnd || !IsWindow(hwnd) || !text) return;
    p = acc_props();
    if (!p) return;
    (void)IAccPropServices_SetHwndPropStr(p, hwnd, (DWORD)OBJID_CLIENT,
                                          (DWORD)CHILDID_SELF, *prop, text);
}

void apr_ui_set_accessible_name_text(HWND hwnd, const wchar_t *text)
{
    acc_set(hwnd, &PROPID_ACC_NAME, text);
}

void apr_ui_set_accessible_name(HWND hwnd, AprStrId id)
{
    const wchar_t *text = apr_str(id);
    wchar_t cls[64];

    apr_ui_set_accessible_name_text(hwnd, text);

    /* Belt as well as braces: for a window whose caption IS its name -- our
     * own pane class, and the frame -- put the text there too, so the name
     * survives even with no annotation service. Never for a status bar, whose
     * text is its CONTENT. */
    if (hwnd && IsWindow(hwnd) && GetClassNameW(hwnd, cls, 64) > 0) {
        if (CompareStringOrdinal(cls, -1, APR_PANE_CLASS, -1, FALSE) == CSTR_EQUAL ||
            CompareStringOrdinal(cls, -1, APR_FRAME_CLASS, -1, FALSE) == CSTR_EQUAL) {
            SetWindowTextW(hwnd, text);
        }
    }
}

void apr_ui_set_accessible_description(HWND hwnd, AprStrId id)
{
    acc_set(hwnd, &PROPID_ACC_DESCRIPTION, apr_str(id));
}

void apr_ui_set_accessible_description_text(HWND hwnd, const wchar_t *text)
{
    acc_set(hwnd, &PROPID_ACC_DESCRIPTION, text);
}

void apr_ui_set_accessible_role(HWND hwnd, long msaa_role)
{
    IAccPropServices *p;
    VARIANT v;

    if (!hwnd || !IsWindow(hwnd)) return;
    p = acc_props();
    if (!p) return;

    /* PROPID_ACC_ROLE is a VT_I4, not a string, so this cannot go through
     * acc_set. Everything else about it is the same annotation mechanism. */
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = msaa_role;
    (void)IAccPropServices_SetHwndProp(p, hwnd, (DWORD)OBJID_CLIENT,
                                       (DWORD)CHILDID_SELF, PROPID_ACC_ROLE, v);
}

/* --------------------------------------------------------------------------
 * The app
 * ----------------------------------------------------------------------- */

struct AprUiApp {
    HINSTANCE inst;
    HWND      frame;
    HWND      status;
    HWND      splitter;
    HWND      pane[APR_PANE_COUNT];
    AprStrId  pane_name[APR_PANE_COUNT];
    AprStrId  pane_desc[APR_PANE_COUNT];
    HMENU     menu;
    int       menu_owned_by_window;   /* set once CreateWindowEx took it */
    HACCEL    accel;
    AprTheme *theme;

    AprUiCommandFn cmd_fn;
    void          *cmd_user;

    int  tree_visible;
    int  applying_theme;  /* re-entrancy guard, see apply_dark */
    int  tree_w;          /* device px */
    UINT dpi;
    int  exit_code;
};

/* --------------------------------------------------------------------------
 * The placeholder pane
 *
 * Replaced by canvas.c and tree_panel.c. It exists so that the frame is a
 * complete, navigable, correctly named accessibility tree before either of
 * those is written -- which is what makes tests/test_ui_a11y.c a real test now
 * rather than a promise.
 * ----------------------------------------------------------------------- */

typedef struct PaneState {
    AprTheme *theme;
    int       focused;
} PaneState;

static LRESULT CALLBACK pane_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PaneState *st = (PaneState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        PaneState *s = (PaneState *)calloc(1, sizeof *s);
        if (!s) return FALSE;
        s->theme = (AprTheme *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_NCDESTROY:
        free(st);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_GETDLGCODE:
        /* Claim the arrow keys so a pane can navigate its own content, but let
         * Tab through to the dialog manager so the pane never becomes a focus
         * trap. WANTARROWS without WANTTAB is exactly that pair. */
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (st) st->focused = (msg == WM_SETFOCUS);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;   /* WM_PAINT covers the whole client area */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        const AprThemePalette *c;
        const AprThemeMetrics *m;

        GetClientRect(hwnd, &rc);
        if (st && st->theme) {
            c = apr_theme_palette(st->theme);
            m = apr_theme_metrics(st->theme);
            FillRect(dc, &rc, apr_theme_brush(st->theme, c->surface));
            FrameRect(dc, &rc, apr_theme_brush(st->theme, c->border));
            if (st->focused) {
                RECT f = rc;
                InflateRect(&f, -m->space_xs, -m->space_xs);
                apr_theme_draw_focus(st->theme, dc, &f);
            }
        } else {
            FillRect(dc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --------------------------------------------------------------------------
 * Splitter
 * ----------------------------------------------------------------------- */

static LRESULT CALLBACK splitter_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* --------------------------------------------------------------------------
 * Menu
 * ----------------------------------------------------------------------- */

typedef struct MenuItem { int cmd; AprStrId label; } MenuItem;

static void menu_append(HMENU m, const MenuItem *items, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (items[i].cmd == 0) {
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        } else {
            AppendMenuW(m, MF_STRING, (UINT_PTR)items[i].cmd, apr_str(items[i].label));
        }
    }
}

static HMENU build_menu(void)
{
    HMENU bar, sub;

    static const MenuItem file[] = {
        { APR_CMD_FILE_NEW,     APR_S_UI_MENU_FILE_NEW },
        { APR_CMD_FILE_OPEN,    APR_S_UI_MENU_FILE_OPEN },
        { 0, 0 },
        { APR_CMD_FILE_SAVE,    APR_S_UI_MENU_FILE_SAVE },
        { APR_CMD_FILE_SAVE_AS, APR_S_UI_MENU_FILE_SAVE_AS },
        { 0, 0 },
        { APR_CMD_FILE_EXIT,    APR_S_UI_MENU_FILE_EXIT }
    };
    static const MenuItem edit[] = {
        { APR_CMD_ADD_SOURCE, APR_S_UI_MENU_ADD_SOURCE },
        { APR_CMD_ADD_BUS,    APR_S_UI_MENU_ADD_BUS },
        { APR_CMD_ADD_ACTION, APR_S_UI_MENU_ADD_ACTION },
        { 0, 0 },
        { APR_CMD_CONNECT,    APR_S_UI_MENU_CONNECT },
        { APR_CMD_REMOVE,     APR_S_UI_MENU_REMOVE }
    };
    static const MenuItem rec[] = {
        { APR_CMD_RECORD_START, APR_S_UI_MENU_RECORD_START },
        { APR_CMD_RECORD_STOP,  APR_S_UI_MENU_RECORD_STOP }
    };
    static const MenuItem view[] = {
        { APR_CMD_VIEW_TREE, APR_S_UI_MENU_VIEW_TREE },
        { APR_CMD_VIEW_DARK, APR_S_UI_MENU_VIEW_DARK },
        { 0, 0 },
        { APR_CMD_NEXT_PANE, APR_S_UI_MENU_VIEW_NEXT_PANE }
    };
    static const MenuItem help[] = {
        { APR_CMD_HELP_KEYS,  APR_S_UI_MENU_HELP_KEYS },
        { APR_CMD_HELP_ABOUT, APR_S_UI_MENU_HELP_ABOUT }
    };

    bar = CreateMenu();
    if (!bar) return NULL;

#define APR_SUBMENU(items, title)                                              \
    do {                                                                       \
        sub = CreatePopupMenu();                                                \
        if (!sub) { DestroyMenu(bar); return NULL; }                            \
        menu_append(sub, (items), sizeof (items) / sizeof (items)[0]);          \
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)sub, apr_str(title));              \
    } while (0)

    APR_SUBMENU(file, APR_S_UI_MENU_FILE);
    APR_SUBMENU(edit, APR_S_UI_MENU_EDIT);
    APR_SUBMENU(rec,  APR_S_UI_MENU_RECORDING);
    APR_SUBMENU(view, APR_S_UI_MENU_VIEW);
    APR_SUBMENU(help, APR_S_UI_MENU_HELP);

#undef APR_SUBMENU
    return bar;
}

/* Accelerators. Not localized -- the key a user presses does not change with
 * the interface language, and a translator changing Ctrl+R to Ctrl+ت would
 * produce a shortcut no keyboard can send. The TEXT beside the menu item is in
 * the catalog so it can be rendered in the local convention; the binding is
 * here. */
static HACCEL build_accelerators(void)
{
    static ACCEL a[] = {
        { FVIRTKEY | FCONTROL,            'N', APR_CMD_FILE_NEW },
        { FVIRTKEY | FCONTROL,            'O', APR_CMD_FILE_OPEN },
        { FVIRTKEY | FCONTROL,            'S', APR_CMD_FILE_SAVE },
        { FVIRTKEY | FCONTROL | FSHIFT,   'S', APR_CMD_FILE_SAVE_AS },
        { FVIRTKEY | FCONTROL,            '1', APR_CMD_ADD_SOURCE },
        { FVIRTKEY | FCONTROL,            '2', APR_CMD_ADD_BUS },
        { FVIRTKEY | FCONTROL,            '3', APR_CMD_ADD_ACTION },
        { FVIRTKEY | FCONTROL,            'E', APR_CMD_CONNECT },
        { FVIRTKEY,                  VK_DELETE, APR_CMD_REMOVE },
        { FVIRTKEY | FCONTROL,            'R', APR_CMD_RECORD_START },
        { FVIRTKEY | FCONTROL,   VK_OEM_PERIOD, APR_CMD_RECORD_STOP },
        { FVIRTKEY | FCONTROL,            'T', APR_CMD_VIEW_TREE },
        { FVIRTKEY | FCONTROL,            'D', APR_CMD_VIEW_DARK },
        { FVIRTKEY,                      VK_F1, APR_CMD_HELP_KEYS }
    };
    return CreateAcceleratorTableW(a, (int)(sizeof a / sizeof a[0]));
}

/* --------------------------------------------------------------------------
 * Frame helpers
 * ----------------------------------------------------------------------- */

static void apply_fonts(AprUiApp *app)
{
    HFONT f = apr_theme_font(app->theme, APR_FONT_BODY);
    int i;

    if (!f) return;
    SendMessageW(app->frame, WM_SETFONT, (WPARAM)f, TRUE);
    if (app->status) SendMessageW(app->status, WM_SETFONT, (WPARAM)f, TRUE);
    for (i = 0; i < APR_PANE_COUNT; ++i) {
        if (app->pane[i]) SendMessageW(app->pane[i], WM_SETFONT, (WPARAM)f, TRUE);
    }
}

static void apply_direction(AprUiApp *app)
{
    AprUiDir dir = apr_ui_dir();

    /* Standard controls only. The frame and the panes are painted by us and
     * must never carry WS_EX_LAYOUTRTL -- see CONVENTION 1 in ui_app.h. */
    if (app->status) apr_ui_apply_rtl(app->status, dir);

    /* The menu bar mirrors with the frame's RTLREADING, which affects reading
     * order and menu drop direction without mirroring any DC we draw into. */
    {
        LONG_PTR ex = GetWindowLongPtrW(app->frame, GWL_EXSTYLE);
        if (dir == APR_DIR_RTL) ex |= WS_EX_RTLREADING;
        else                    ex &= ~(LONG_PTR)WS_EX_RTLREADING;
        SetWindowLongPtrW(app->frame, GWL_EXSTYLE, ex);
    }
}

static void apply_dark(AprUiApp *app)
{
    int dark;
    int i;

    /* RE-ENTRANCY GUARD, AND IT IS LOAD-BEARING.
     *
     * apr_darkmode_apply_to_window calls SetWindowTheme, which SENDS
     * WM_THEMECHANGED to the window. frame_proc handles WM_THEMECHANGED by
     * re-applying the theme, which calls back in here, which calls
     * SetWindowTheme again. That recursion is unbounded and it is not a slow
     * leak: it overflows the stack inside a window procedure, which Windows
     * reports as STATUS_FATAL_USER_CALLBACK_EXCEPTION (0xC000041D) with no
     * clue as to which callback. Found exactly that way.
     *
     * The same trap catches SWP_FRAMECHANGED and any future call that nudges
     * the theme, so the guard sits here rather than in the message handler. */
    if (!app || app->applying_theme) return;
    app->applying_theme = 1;

    dark = apr_theme_is_dark(app->theme);

    apr_darkmode_set_app_mode(dark ? APR_APPMODE_FORCE_DARK : APR_APPMODE_FORCE_LIGHT);
    apr_darkmode_apply_to_window(app->frame, dark);
    if (app->status) apr_darkmode_apply_to_window(app->status, dark);
    for (i = 0; i < APR_PANE_COUNT; ++i) {
        if (app->pane[i]) apr_darkmode_apply_to_window(app->pane[i], dark);
    }
    apr_darkmode_flush_menu_theme();
    DrawMenuBar(app->frame);
    InvalidateRect(app->frame, NULL, TRUE);

    app->applying_theme = 0;
}

static void reapply_names(AprUiApp *app)
{
    int i;

    apr_ui_set_accessible_name(app->frame, APR_S_UI_TITLE_UNTITLED);
    if (app->status) {
        apr_ui_set_accessible_name(app->status, APR_S_UI_PANE_STATUS);
    }
    if (app->splitter) {
        apr_ui_set_accessible_name(app->splitter, APR_S_UI_PANE_SPLITTER);
        apr_ui_set_accessible_description(app->splitter, APR_S_UI_DESC_SPLITTER);
    }
    for (i = 0; i < APR_PANE_COUNT; ++i) {
        if (!app->pane[i]) continue;
        apr_ui_set_accessible_name(app->pane[i], app->pane_name[i]);
        apr_ui_set_accessible_description(app->pane[i], app->pane_desc[i]);
    }
}

void apr_ui_app_relayout(AprUiApp *app)
{
    AprUiLayoutIn in;
    AprUiRects r;
    const AprThemeMetrics *m;
    HDWP dwp;

    if (!app || !app->frame || !IsWindow(app->frame)) return;
    m = apr_theme_metrics(app->theme);

    memset(&in, 0, sizeof in);
    GetClientRect(app->frame, &in.client);
    in.dir = apr_ui_dir();
    in.tree_w = app->tree_w > 0 ? app->tree_w : m->tree_w;
    in.pane_min_w = m->pane_min_w;
    in.splitter_w = m->splitter_w;
    in.status_h = m->status_h;
    in.tree_visible = app->tree_visible;

    apr_ui_layout(&in, &r);

    /* The status bar sizes itself from WM_SIZE, but only if it is told; a
     * status bar left to its own devices uses the system-DPI height. */
    if (app->status) {
        MoveWindow(app->status, r.status.left, r.status.top,
                   r.status.right - r.status.left,
                   r.status.bottom - r.status.top, TRUE);
    }

    dwp = BeginDeferWindowPos(APR_PANE_COUNT + 1);
    if (!dwp) return;

#define APR_PLACE(hwnd, rect, show)                                            \
    do {                                                                       \
        if (hwnd) {                                                            \
            dwp = DeferWindowPos(dwp, (hwnd), NULL, (rect).left, (rect).top,    \
                                 (rect).right - (rect).left,                    \
                                 (rect).bottom - (rect).top,                    \
                                 SWP_NOZORDER | SWP_NOACTIVATE |                \
                                 ((show) ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));   \
            if (!dwp) return;                                                   \
        }                                                                       \
    } while (0)

    APR_PLACE(app->pane[APR_PANE_TREE], r.tree,
              app->tree_visible && r.tree.right > r.tree.left);
    APR_PLACE(app->splitter, r.splitter,
              app->tree_visible && r.splitter.right > r.splitter.left);
    APR_PLACE(app->pane[APR_PANE_CANVAS], r.canvas, 1);

#undef APR_PLACE
    EndDeferWindowPos(dwp);
}

/* Move focus to the next (or previous) pane. The standard F6 idiom: it steps
 * between top-level regions while Tab stays meaningful inside one. */
static void cycle_pane(AprUiApp *app, int forward)
{
    HWND focus = GetFocus();
    int start = -1, i, n;

    for (i = 0; i < APR_PANE_COUNT; ++i) {
        if (app->pane[i] &&
            (app->pane[i] == focus || IsChild(app->pane[i], focus))) {
            start = i;
            break;
        }
    }

    for (n = 1; n <= APR_PANE_COUNT; ++n) {
        int idx = start < 0
                    ? (forward ? 0 : APR_PANE_COUNT - 1)
                    : ((start + (forward ? n : APR_PANE_COUNT - n)) % APR_PANE_COUNT);
        HWND target = app->pane[idx];
        if (target && IsWindowVisible(target) && IsWindowEnabled(target)) {
            SetFocus(target);
            return;
        }
        if (start < 0) break;
    }
}

static int dispatch_command(AprUiApp *app, int cmd)
{
    switch (cmd) {
    case APR_CMD_FILE_EXIT:
        PostMessageW(app->frame, WM_CLOSE, 0, 0);
        return 1;
    case APR_CMD_NEXT_PANE:
        cycle_pane(app, 1);
        return 1;
    case APR_CMD_PREV_PANE:
        cycle_pane(app, 0);
        return 1;
    case APR_CMD_VIEW_TREE:
        app->tree_visible = !app->tree_visible;
        CheckMenuItem(app->menu, APR_CMD_VIEW_TREE,
                      MF_BYCOMMAND | (app->tree_visible ? MF_CHECKED : MF_UNCHECKED));
        apr_ui_app_relayout(app);
        /* Focus must not be left inside a window we just hid. */
        if (!app->tree_visible && app->pane[APR_PANE_CANVAS]) {
            HWND f = GetFocus();
            if (f && app->pane[APR_PANE_TREE] &&
                (f == app->pane[APR_PANE_TREE] || IsChild(app->pane[APR_PANE_TREE], f))) {
                SetFocus(app->pane[APR_PANE_CANVAS]);
            }
        }
        return 1;
    case APR_CMD_VIEW_DARK: {
        int now = !apr_theme_is_dark(app->theme);
        AprErr e = apr_theme_set_dark(app->theme, now);
        if (apr_failed(&e)) APR_LOG_ERR(APR_LOG_WARN, &e);
        CheckMenuItem(app->menu, APR_CMD_VIEW_DARK,
                      MF_BYCOMMAND | (apr_theme_is_dark(app->theme)
                                        ? MF_CHECKED : MF_UNCHECKED));
        apply_dark(app);
        return 1;
    }
    default:
        break;
    }
    if (app->cmd_fn) return app->cmd_fn(app, cmd, app->cmd_user);
    return 0;
}

/* --------------------------------------------------------------------------
 * Splitter
 * ----------------------------------------------------------------------- */

typedef struct SplitState {
    AprUiApp *app;
    int       dragging;
    int       focused;
} SplitState;

/* Move the divider by `delta` device pixels in the LEADING direction, i.e. the
 * direction that makes the structure panel wider, whichever way the interface
 * runs. Shared by the mouse and the keyboard so the two cannot disagree. */
static void splitter_set_width(AprUiApp *a, int w)
{
    const AprThemeMetrics *m = apr_theme_metrics(a->theme);
    RECT fc;
    int max_w;

    GetClientRect(a->frame, &fc);
    max_w = (int)(fc.right - fc.left) - m->splitter_w - m->pane_min_w;
    if (w < m->pane_min_w) w = m->pane_min_w;
    if (max_w >= m->pane_min_w && w > max_w) w = max_w;
    a->tree_w = w;
    apr_ui_app_relayout(a);
}

static LRESULT CALLBACK splitter_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SplitState *st = (SplitState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SplitState *s = (SplitState *)calloc(1, sizeof *s);
        if (!s) return FALSE;
        s->app = (AprUiApp *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_NCDESTROY:
        free(st);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_GETDLGCODE:
        /* Arrow keys belong to the splitter; Tab must still pass through to
         * the dialog manager or this becomes a focus trap. */
        return DLGC_WANTARROWS;

    case WM_KEYDOWN: {
        /* A DIVIDER THAT ONLY RESPONDS TO A DRAG IS A MOUSE-ONLY PATH, which
         * AGENTS.md rule 5 does not allow. One nudge per press, a larger one
         * with Ctrl, and Home/End for the extremes.
         *
         * "Leading" rather than "left": in an RTL interface the panel is on
         * the right, so the key that makes it wider is the one pointing away
         * from it. Pressing the arrow that points at the panel always shrinks
         * it, in both directions -- which is what a user means by the gesture. */
        AprUiApp *a = st ? st->app : NULL;
        const AprThemeMetrics *m;
        int step, dir_sign;

        if (!a) break;
        m = apr_theme_metrics(a->theme);
        step = (GetKeyState(VK_CONTROL) & 0x8000) ? m->space_xl : m->space_sm;
        dir_sign = (apr_ui_dir() == APR_DIR_RTL) ? -1 : 1;

        if (wp == VK_LEFT) {
            splitter_set_width(a, a->tree_w - step * dir_sign);
            return 0;
        }
        if (wp == VK_RIGHT) {
            splitter_set_width(a, a->tree_w + step * dir_sign);
            return 0;
        }
        if (wp == VK_HOME) { splitter_set_width(a, m->pane_min_w); return 0; }
        if (wp == VK_END)  { splitter_set_width(a, 1 << 20); return 0; }
        break;
    }

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
        return TRUE;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (st) st->focused = (msg == WM_SETFOCUS);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        if (st) { st->dragging = 1; SetCapture(hwnd); }
        return 0;

    case WM_MOUSEMOVE:
        if (st && st->dragging) {
            POINT pt;
            RECT fc;
            AprUiApp *a = st->app;

            pt.x = GET_X_LPARAM(lp);
            pt.y = GET_Y_LPARAM(lp);
            ClientToScreen(hwnd, &pt);
            ScreenToClient(a->frame, &pt);
            GetClientRect(a->frame, &fc);

            splitter_set_width(a, (apr_ui_dir() == APR_DIR_RTL)
                                    ? (int)(fc.right - pt.x)
                                    : (int)(pt.x - fc.left));
        }
        return 0;

    case WM_LBUTTONUP:
        if (st && st->dragging) { st->dragging = 0; ReleaseCapture(); }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (st && st->app && st->app->theme) {
            AprTheme *t = st->app->theme;
            FillRect(dc, &rc, apr_theme_brush(t, apr_theme_palette(t)->window_bg));
            if (st->focused) apr_theme_draw_focus(t, dc, &rc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --------------------------------------------------------------------------
 * Frame window procedure
 * ----------------------------------------------------------------------- */

static LRESULT CALLBACK frame_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    AprUiApp *app = (AprUiApp *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_SIZE:
        if (app) apr_ui_app_relayout(app);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        UINT dpi = apr_dpi_for_window(hwnd);
        mmi->ptMinTrackSize.x = apr_dpi_scale(480, dpi);
        mmi->ptMinTrackSize.y = apr_dpi_scale(320, dpi);
        return 0;
    }

    case WM_DPICHANGED: {
        UINT dpi = apr_dpi_handle_dpichanged(hwnd, wp, lp);
        if (app) {
            AprErr e = apr_theme_set_dpi(app->theme, dpi);
            if (apr_failed(&e)) APR_LOG_ERR(APR_LOG_WARN, &e);
            /* The stored splitter position is in device pixels, so it has to
             * move with the DPI or the panel jumps to a different physical
             * width on the new monitor. */
            if (app->tree_w > 0) {
                app->tree_w = apr_dpi_rescale(app->tree_w, app->dpi, dpi);
            }
            app->dpi = dpi;
            apply_fonts(app);
            apr_ui_app_relayout(app);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    }

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        if (app) {
            /* WM_SETTINGCHANGE arrives constantly and for everything -- mouse
             * speed, environment variables, policy. Refreshing the theme on
             * each one is cheap and is how high contrast and the non-client
             * metrics get picked up, so that always runs. Re-applying DARK is
             * not cheap and reaches into uxtheme, so it is gated on the change
             * actually being a colour-scheme change (lParam "ImmersiveColorSet")
             * or on WM_THEMECHANGED. */
            AprErr e = apr_theme_refresh(app->theme);
            if (apr_failed(&e)) APR_LOG_ERR(APR_LOG_WARN, &e);
            apply_direction(app);
            apply_fonts(app);
            if (msg == WM_THEMECHANGED ||
                apr_darkmode_is_color_scheme_change(msg, lp)) {
                CheckMenuItem(app->menu, APR_CMD_VIEW_DARK,
                              MF_BYCOMMAND | (apr_theme_is_dark(app->theme)
                                                ? MF_CHECKED : MF_UNCHECKED));
                apply_dark(app);
            }
            apr_ui_app_relayout(app);
        }
        return 0;

    case WM_COMMAND:
        if (app && HIWORD(wp) <= 1) {
            if (dispatch_command(app, (int)LOWORD(wp))) return 0;
        }
        break;

    case WM_SETFOCUS:
        /* The frame itself is not a useful focus target; hand focus to a pane
         * so that arriving at the window by Alt+Tab puts a screen reader
         * somewhere it can read. */
        if (app) {
            int i;
            for (i = 0; i < APR_PANE_COUNT; ++i) {
                if (app->pane[i] && IsWindowVisible(app->pane[i])) {
                    SetFocus(app->pane[i]);
                    return 0;
                }
            }
        }
        break;

    case WM_ERASEBKGND:
        if (app) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc,
                     apr_theme_brush(app->theme, apr_theme_palette(app->theme)->window_bg));
            return 1;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (app) app->frame = NULL;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --------------------------------------------------------------------------
 * Class registration
 * ----------------------------------------------------------------------- */

static AprErr register_classes(HINSTANCE inst)
{
    WNDCLASSEXW wc;
    static LONG done;

    if (InterlockedCompareExchange(&done, 1, 0) != 0) return apr_ok();

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = frame_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;   /* WM_ERASEBKGND paints from the theme */
    wc.lpszClassName = APR_FRAME_CLASS;
    if (!RegisterClassExW(&wc)) {
        InterlockedExchange(&done, 0);
        return APR_ERR_LAST(L"RegisterClassExW(frame) failed");
    }

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = pane_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = APR_PANE_CLASS;
    if (!RegisterClassExW(&wc)) {
        return APR_ERR_LAST(L"RegisterClassExW(pane) failed");
    }

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = splitter_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_SIZEWE);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"AprRecorderSplitter";
    if (!RegisterClassExW(&wc)) {
        return APR_ERR_LAST(L"RegisterClassExW(splitter) failed");
    }

    return apr_ok();
}

/* --------------------------------------------------------------------------
 * Panes: the real thing where it exists, a named placeholder where it does not
 * ----------------------------------------------------------------------- */

static HWND make_placeholder(AprUiApp *app, int slot)
{
    return CreateWindowExW(
        0, APR_PANE_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN,
        0, 0, 10, 10,
        app->frame, (HMENU)(UINT_PTR)(APR_ID_PANE_0 + slot), app->inst,
        app->theme);
}

static void create_panes(AprUiApp *app)
{
    app->pane_name[APR_PANE_TREE]   = APR_S_UI_PANE_TREE;
    app->pane_desc[APR_PANE_TREE]   = APR_S_UI_DESC_TREE;
    app->pane_name[APR_PANE_CANVAS] = APR_S_UI_PANE_CANVAS;
    app->pane_desc[APR_PANE_CANVAS] = APR_S_UI_DESC_CANVAS;

    /* Creation order IS tab order, and tab order is the accessibility reading
     * order. Structure panel first, canvas second, in BOTH directions -- see
     * CONVENTION 1: only painting mirrors. */
#ifdef APR_HAVE_UI_TREE_PANEL
    app->pane[APR_PANE_TREE] = apr_tree_panel_create(app->frame, app->theme);
#endif
    if (!app->pane[APR_PANE_TREE]) {
        app->pane[APR_PANE_TREE] = make_placeholder(app, APR_PANE_TREE);
    }

    /* WS_TABSTOP: the divider is a control the user operates, so it is in the
     * tab order and it is named. Before it was, tests/test_ui_a11y.c found it
     * in the live tree as an unnamed focusable Pane -- a thing a screen reader
     * lands on and can say nothing about. */
    app->splitter = CreateWindowExW(
        0, L"AprRecorderSplitter", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 10, 10,
        app->frame, (HMENU)(UINT_PTR)APR_ID_SPLITTER, app->inst, app);

#ifdef APR_HAVE_UI_CANVAS
    app->pane[APR_PANE_CANVAS] = apr_canvas_create(app->frame, app->theme);
#endif
    if (!app->pane[APR_PANE_CANVAS]) {
        app->pane[APR_PANE_CANVAS] = make_placeholder(app, APR_PANE_CANVAS);
    }
}

/* --------------------------------------------------------------------------
 * Public
 * ----------------------------------------------------------------------- */

AprErr apr_ui_app_create(HINSTANCE inst, AprUiApp **out)
{
    AprUiApp *app;
    AprErr e;
    INITCOMMONCONTROLSEX icc;
    RECT want;
    const AprThemeMetrics *m;
    int i;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"apr_ui_app_create: out is NULL");
    *out = NULL;

    if (!inst) inst = GetModuleHandleW(NULL);

    /* Without this the process gets whatever comctl32 the activation context
     * happens to give it. With the manifest in place it is v6; the call still
     * has to happen to load the DLL and register the classes. */
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES |
                ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    if (!InitCommonControlsEx(&icc)) {
        APR_WARN(L"InitCommonControlsEx failed; controls may be version 5");
    }

    apr_darkmode_init();

    e = register_classes(inst);
    if (apr_failed(&e)) return e;

    app = (AprUiApp *)calloc(1, sizeof *app);
    if (!app) return APR_ERR(APR_E_NO_MEMORY, L"apr_ui_app_create: out of memory");

    app->inst = inst;
    app->tree_visible = 1;
    app->dpi = apr_dpi_system();

    e = apr_theme_create(app->dpi, &app->theme);
    if (apr_failed(&e)) { free(app); return e; }
    m = apr_theme_metrics(app->theme);
    app->tree_w = m->tree_w;

    app->menu = build_menu();
    if (!app->menu) {
        apr_theme_destroy(app->theme);
        free(app);
        return APR_ERR_LAST(L"could not build the menu bar");
    }

    /* Size the frame from a logical client size, adjusted for the real chrome
     * at the real DPI -- not from a magic pixel count, which would be wrong on
     * every monitor but one. */
    want.left = 0;
    want.top = 0;
    want.right = apr_dpi_scale(1100, app->dpi);
    want.bottom = apr_dpi_scale(680, app->dpi);
    apr_dpi_adjust_window_rect(&want, WS_OVERLAPPEDWINDOW, TRUE, 0, app->dpi);

    app->frame = CreateWindowExW(
        /* NOINHERITLAYOUT so that no child can ever acquire WS_EX_LAYOUTRTL by
         * inheritance -- the frame does not have it, but a future maintainer
         * adding it "for RTL" would otherwise silently mirror every custom
         * paint in the product. This makes that mistake impossible rather than
         * merely documented. */
        WS_EX_NOINHERITLAYOUT,
        APR_FRAME_CLASS, apr_str(APR_S_UI_TITLE_UNTITLED),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        want.right - want.left, want.bottom - want.top,
        NULL, app->menu, inst, app);
    if (!app->frame) {
        e = APR_ERR_LAST(L"CreateWindowExW(frame) failed");
        DestroyMenu(app->menu);
        apr_theme_destroy(app->theme);
        free(app);
        return e;
    }

    app->menu_owned_by_window = 1;
    app->dpi = apr_dpi_for_window(app->frame);
    (void)apr_theme_set_dpi(app->theme, app->dpi);

    app->status = CreateWindowExW(
        0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, app->frame, (HMENU)(UINT_PTR)APR_ID_STATUS, inst, NULL);

    create_panes(app);

    app->accel = build_accelerators();
    if (!app->accel) APR_WARN(L"accelerator table could not be created");

    CheckMenuItem(app->menu, APR_CMD_VIEW_TREE, MF_BYCOMMAND | MF_CHECKED);
    CheckMenuItem(app->menu, APR_CMD_VIEW_DARK,
                  MF_BYCOMMAND | (apr_theme_is_dark(app->theme) ? MF_CHECKED : MF_UNCHECKED));

    /* Everything the frame does not implement itself starts disabled, named
     * and present. See AprUiCommandFn. */
    {
        static const int owned_by_model[] = {
            APR_CMD_FILE_NEW, APR_CMD_FILE_OPEN, APR_CMD_FILE_SAVE,
            APR_CMD_FILE_SAVE_AS, APR_CMD_ADD_SOURCE, APR_CMD_ADD_BUS,
            APR_CMD_ADD_ACTION, APR_CMD_CONNECT, APR_CMD_REMOVE,
            APR_CMD_RECORD_START, APR_CMD_RECORD_STOP,
            APR_CMD_HELP_KEYS, APR_CMD_HELP_ABOUT
        };
        for (i = 0; i < (int)(sizeof owned_by_model / sizeof owned_by_model[0]); ++i) {
            EnableMenuItem(app->menu, (UINT)owned_by_model[i],
                           MF_BYCOMMAND | MF_GRAYED);
        }
    }

    apply_fonts(app);
    apply_direction(app);
    reapply_names(app);
    apply_dark(app);
    apr_ui_app_set_status(app, APR_S_UI_STATUS_READY);
    apr_ui_app_relayout(app);

    APR_INFO(L"ui: frame created, dpi=%u, awareness=%d, rtl=%d",
             app->dpi, (int)apr_dpi_awareness(), (int)apr_ui_dir());

    *out = app;
    return apr_ok();
}

void apr_ui_app_destroy(AprUiApp *app)
{
    if (!app) return;
    if (app->frame && IsWindow(app->frame)) DestroyWindow(app->frame);
    if (app->accel) DestroyAcceleratorTable(app->accel);
    /* DestroyWindow destroys the menu with the window. Testing `!app->frame`
     * here would be a DOUBLE FREE, not a guard: WM_DESTROY has already set
     * app->frame to NULL by this point, so the condition is true precisely
     * when the menu is already gone. The flag records what actually happened. */
    if (app->menu && !app->menu_owned_by_window) DestroyMenu(app->menu);
    apr_theme_destroy(app->theme);
    acc_release_for_thread();
    free(app);
}

HWND apr_ui_app_hwnd(const AprUiApp *app) { return app ? app->frame : NULL; }

HWND apr_ui_app_pane(const AprUiApp *app, AprUiPaneSlot slot)
{
    if (!app || slot < 0 || slot >= APR_PANE_COUNT) return NULL;
    return app->pane[slot];
}

HWND apr_ui_app_status_bar(const AprUiApp *app) { return app ? app->status : NULL; }

AprTheme *apr_ui_app_theme(AprUiApp *app) { return app ? app->theme : NULL; }

void apr_ui_app_set_command_handler(AprUiApp *app, AprUiCommandFn fn, void *user)
{
    if (!app) return;
    app->cmd_fn = fn;
    app->cmd_user = user;
}

void apr_ui_app_enable_command(AprUiApp *app, int command_id, int enabled)
{
    if (!app || !app->menu) return;
    EnableMenuItem(app->menu, (UINT)command_id,
                   MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    DrawMenuBar(app->frame);
}

void apr_ui_app_show(AprUiApp *app, int cmd_show)
{
    if (!app || !app->frame) return;
    ShowWindow(app->frame, cmd_show);
    UpdateWindow(app->frame);
}

void apr_ui_app_set_status(AprUiApp *app, AprStrId id)
{
    if (!app || !app->status) return;
    SendMessageW(app->status, SB_SETTEXTW, 0, (LPARAM)apr_str(id));
}

void apr_ui_app_request_close(const AprUiApp *app)
{
    if (app && app->frame) PostMessageW(app->frame, WM_CLOSE, 0, 0);
}

int apr_ui_app_run(AprUiApp *app)
{
    MSG msg;

    if (!app) return 1;
    memset(&msg, 0, sizeof msg);   /* GetMessage returning -1 leaves it alone */

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* F6 first: IsDialogMessage consumes it, and pane cycling is the one
         * navigation key a screen reader user relies on to leave a pane whose
         * Tab order is long. */
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F6 && app->frame) {
            cycle_pane(app, (GetKeyState(VK_SHIFT) & 0x8000) ? 0 : 1);
            continue;
        }
        if (app->accel && app->frame &&
            TranslateAcceleratorW(app->frame, app->accel, &msg)) {
            continue;
        }
        if (app->frame && IsDialogMessageW(app->frame, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    app->exit_code = (int)msg.wParam;
    return app->exit_code;
}
