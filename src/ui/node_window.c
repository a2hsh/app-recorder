/*
 * node_window.c -- one graph node, as one real Win32 child window.
 *
 * The argument for that shape is in ui_node.h and in design 6.1 and is not
 * repeated. What follows is why the parts that look arbitrary are the way they
 * are.
 *
 * ---------------------------------------------------------------------------
 * WHY THE NODE CLAIMS EVERY KEY (DLGC_WANTALLKEYS) AND THEN IMPLEMENTS TAB
 *
 *   The obvious arrangement -- DLGC_WANTARROWS without DLGC_WANTTAB, so the
 *   dialog manager moves between nodes -- loses three keys the canvas needs,
 *   and loses them silently:
 *
 *     - IsDialogMessage eats VK_ESCAPE and turns it into WM_COMMAND(IDCANCEL)
 *       at the frame. Escape is how the user gets out of a half-made
 *       connection, so losing it strands them in a mode.
 *     - It eats VK_RETURN looking for a default push button that does not
 *       exist.
 *     - It never delivers VK_TAB to us at all, so the canvas cannot know that
 *       navigation happened and cannot announce it.
 *
 *   So the node claims everything and forwards it to the canvas, which owns
 *   Tab as an ordinary operation in its binding table: next/previous node, and
 *   at either end of the list focus leaves for the frame's next tab stop via
 *   GetNextDlgTabItem. That is the SAME escape the dialog manager would have
 *   provided, written down where it can be read, and it is the reason claiming
 *   every key does not make this pane a focus trap. F6 is unaffected either
 *   way: the frame intercepts it before IsDialogMessage runs.
 *
 * ---------------------------------------------------------------------------
 * WHY THE ROLE IS SET EXPLICITLY
 *
 *   A custom window class with no annotation is reported by UIA as a Pane --
 *   a container with no semantics. A screen reader lands on it and says
 *   "pane", which is exactly the defect tests/test_ui_a11y.c exists to catch
 *   one level up. ROLE_SYSTEM_GROUPING is the honest MSAA role for "a named
 *   box holding one thing in a diagram", and the UIA bridge turns it into a
 *   Group. tests/test_ui_canvas.c asserts the mapped control type rather than
 *   the role we asked for, because the mapping belongs to the OS.
 *
 * ---------------------------------------------------------------------------
 * NO LAYOUTRTL, EVER
 *
 *   This is a window whose WM_PAINT we write, so WS_EX_LAYOUTRTL would hand it
 *   a mirrored device context and draw its text backwards (ui_app.h,
 *   CONVENTION 1). It mirrors by asking apr_ui_dir() which edge is leading and
 *   drawing accordingly -- one branch, in one function, over a value that has
 *   exactly one source.
 */
#include "ui_node.h"

#include <objbase.h>
#include <oleacc.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "ui_app.h"

#define APR_NODE_CLASS L"AprRecorderNode"

#define APR_NODE_TITLE_CCH  128
#define APR_NODE_DETAIL_CCH 96

typedef struct NodeState {
    AprTheme   *theme;
    AprNodeKind kind;
    uint32_t    model_id;
    int         sub_id;
    int         focused;
    int         pending;
    wchar_t     title[APR_NODE_TITLE_CCH];
    wchar_t     detail[APR_NODE_DETAIL_CCH];
} NodeState;

static NodeState *state_of(HWND h)
{
    return (NodeState *)GetWindowLongPtrW(h, GWLP_USERDATA);
}

int apr_node_is_node(HWND hwnd)
{
    wchar_t cls[64];

    if (!hwnd || !IsWindow(hwnd)) return 0;
    if (GetClassNameW(hwnd, cls, 64) <= 0) return 0;
    return CompareStringOrdinal(cls, -1, APR_NODE_CLASS, -1, FALSE) == CSTR_EQUAL;
}

/* --------------------------------------------------------------------------
 * Painting
 * ----------------------------------------------------------------------- */

static COLORREF kind_color(const AprThemePalette *c, AprNodeKind k)
{
    switch (k) {
    case APR_NODE_SOURCE: return c->node_source;
    case APR_NODE_BUS:    return c->node_bus;
    case APR_NODE_ACTION: return c->node_action;
    default:              return c->accent;
    }
}

/* One place decides which edge is leading, and it asks apr_ui_dir(). */
static void node_paint(HWND hwnd, NodeState *st, HDC dc)
{
    const AprThemePalette *c;
    const AprThemeMetrics *m;
    AprUiDir dir;
    RECT rc, bar, text, line;
    HGDIOBJ old_font;
    HPEN pen, old_pen;
    HBRUSH fill;
    UINT flags;
    int stripe, cx, cy;

    GetClientRect(hwnd, &rc);

    if (!st || !st->theme) {
        FillRect(dc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
        return;
    }

    c = apr_theme_palette(st->theme);
    m = apr_theme_metrics(st->theme);
    dir = apr_ui_dir();

    /* Body. A pending node -- the held end of a half-made connection -- reads
     * as "hot" rather than as selected, because it is not a selection: it is a
     * thing the user is in the middle of doing, and Escape undoes it. */
    fill = apr_theme_brush(st->theme,
                           st->pending ? c->surface_hot
                                       : (st->focused ? c->surface_sel_bg : c->surface));
    pen = apr_theme_pen(st->theme,
                        st->pending ? c->accent : c->border,
                        st->pending ? m->border_strong : m->border);
    if (!fill || !pen) return;

    old_pen = (HPEN)SelectObject(dc, pen);
    {
        HGDIOBJ old_brush = SelectObject(dc, fill);
        if (m->corner > 0) {
            RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom,
                      m->corner, m->corner);
        } else {
            Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
        }
        SelectObject(dc, old_brush);
    }
    SelectObject(dc, old_pen);

    /* The kind stripe, on the LEADING edge. Colour is never the only carrier
     * of kind -- the accessible name says "source" / "bus" / "output" in
     * words, and so does the detail line below -- but for a sighted user it is
     * the fastest read on the canvas. */
    stripe = m->space_sm;
    bar = rc;
    bar.left = apr_ui_lead_x(&rc, m->border, stripe, dir);
    bar.right = bar.left + stripe;
    bar.top += m->border;
    bar.bottom -= m->border;
    FillRect(dc, &bar, apr_theme_brush(st->theme, kind_color(c, st->kind)));

    /* Ports: where an edge attaches. Drawn inside the node so that the line
     * the canvas paints in the gap meets something rather than stopping in
     * mid-air, and so that a mouse has a target with a real hit area. */
    cy = (rc.top + rc.bottom) / 2;
    {
        HBRUSH pb = apr_theme_brush(st->theme, c->edge);
        HGDIOBJ ob = SelectObject(dc, pb);
        HPEN pp = apr_theme_pen(st->theme, c->edge, m->edge_w);
        HGDIOBJ op = SelectObject(dc, pp);

        if (st->kind != APR_NODE_SOURCE) {
            cx = apr_ui_lead_x(&rc, 0, 0, dir);
            Ellipse(dc, cx - m->port_r, cy - m->port_r,
                        cx + m->port_r, cy + m->port_r);
        }
        if (st->kind != APR_NODE_ACTION) {
            cx = apr_ui_lead_x(&rc, rc.right - rc.left, 0, dir);
            Ellipse(dc, cx - m->port_r, cy - m->port_r,
                        cx + m->port_r, cy + m->port_r);
        }
        SelectObject(dc, op);
        SelectObject(dc, ob);
    }

    /* Text. DT_RIGHT | DT_RTLREADING rather than a mirrored DC: the glyphs
     * must not be reflected, only their alignment and reading order. */
    text = rc;
    if (dir == APR_DIR_RTL) {
        text.left += m->node_pad;
        text.right -= stripe + m->node_pad;
        flags = DT_RIGHT | DT_RTLREADING;
    } else {
        text.left += stripe + m->node_pad;
        text.right -= m->node_pad;
        flags = DT_LEFT;
    }
    text.top += m->node_pad;
    text.bottom -= m->node_pad;
    flags |= DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;

    SetBkMode(dc, TRANSPARENT);

    old_font = SelectObject(dc, apr_theme_font(st->theme, APR_FONT_BODY_STRONG));
    SetTextColor(dc, c->text);
    line = text;
    line.bottom = line.top + apr_theme_line_height(st->theme, APR_FONT_BODY_STRONG);
    DrawTextW(dc, st->title, -1, &line, flags);

    SelectObject(dc, apr_theme_font(st->theme, APR_FONT_SMALL));
    SetTextColor(dc, c->text_dim);
    line.top = line.bottom + m->space_xs;
    line.bottom = line.top + apr_theme_line_height(st->theme, APR_FONT_SMALL);
    if (line.bottom > text.bottom) line.bottom = text.bottom;
    DrawTextW(dc, st->detail, -1, &line, flags);
    SelectObject(dc, old_font);

    if (st->focused) {
        RECT f = rc;
        InflateRect(&f, -m->space_xs, -m->space_xs);
        apr_theme_draw_focus(st->theme, dc, &f);
    }
}

/* --------------------------------------------------------------------------
 * Window procedure
 * ----------------------------------------------------------------------- */

static LRESULT CALLBACK node_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    NodeState *st = state_of(hwnd);

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        NodeState *s = (NodeState *)calloc(1, sizeof *s);
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
        /* Everything. See the header note; Tab is implemented below, and F6
         * is intercepted by the frame before IsDialogMessage runs at all. */
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS |
               DLGC_WANTTAB;

    case WM_KEYDOWN:
        /* EVERY key, Tab included, goes to the canvas. It is the thing that
         * knows the graph, so it is the only thing that can say what "go to
         * what this feeds" means -- and having exactly one dispatch site is
         * what makes the binding table in ui_canvas.h the single truth about
         * which key does what. The canvas is also what hands focus OUT of the
         * pane at either end of the node list, which is why claiming Tab here
         * does not make this a focus trap. */
        return SendMessageW(GetParent(hwnd), WM_KEYDOWN, wp, lp);

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (st) st->focused = (msg == WM_SETFOCUS);
        InvalidateRect(hwnd, NULL, FALSE);
        if (msg == WM_SETFOCUS) {
            /* Tell the canvas, so it can scroll this node into view and keep
             * its idea of "the current node" true even when focus arrived by
             * Tab or by a mouse click rather than through apr_canvas_perform. */
            SendMessageW(GetParent(hwnd), WM_COMMAND,
                         MAKEWPARAM(GetDlgCtrlID(hwnd), BN_SETFOCUS),
                         (LPARAM)hwnd);
        }
        return 0;

    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        /* A click on a node is also the mouse half of connect: the canvas
         * decides, because the two-step model lives there and the mouse must
         * not get a second one. */
        SendMessageW(GetParent(hwnd), WM_PARENTNOTIFY,
                     MAKEWPARAM(WM_LBUTTONDOWN, 0), (LPARAM)hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;   /* WM_PAINT covers the whole client area */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        node_paint(hwnd, st, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --------------------------------------------------------------------------
 * Public
 * ----------------------------------------------------------------------- */

int apr_node_register_class(HINSTANCE inst)
{
    static int registered;
    WNDCLASSEXW wc;

    if (registered) return 1;
    if (!inst) inst = GetModuleHandleW(NULL);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = node_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = APR_NODE_CLASS;
    if (!RegisterClassExW(&wc)) {
        APR_WARN(L"RegisterClassExW(node) failed (%lu)",
                 (unsigned long)GetLastError());
        return 0;
    }
    registered = 1;
    return 1;
}

HWND apr_node_create(HWND canvas, AprTheme *theme, AprNodeKind kind,
                     uint32_t model_id, int sub_id, int ctrl_id)
{
    HWND h;
    NodeState *st;

    if (!canvas || !IsWindow(canvas)) return NULL;
    if (!apr_node_register_class((HINSTANCE)(LONG_PTR)
                                 GetWindowLongPtrW(canvas, GWLP_HINSTANCE))) {
        return NULL;
    }

    /* NOT WS_EX_LAYOUTRTL. Never. See the header note. WS_TABSTOP because the
     * platform's focus chain is the whole reason this is a window. */
    h = CreateWindowExW(
        0, APR_NODE_CLASS, L"",
        WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS,
        0, 0, 10, 10, canvas, (HMENU)(UINT_PTR)ctrl_id,
        (HINSTANCE)(LONG_PTR)GetWindowLongPtrW(canvas, GWLP_HINSTANCE), theme);
    if (!h) return NULL;

    st = state_of(h);
    if (st) {
        st->kind = kind;
        st->model_id = model_id;
        st->sub_id = sub_id;
    }

    /* Without this a screen reader says "pane" and stops. */
    apr_ui_set_accessible_role(h, ROLE_SYSTEM_GROUPING);
    return h;
}

AprNodeKind apr_node_kind(HWND node)
{
    NodeState *st = apr_node_is_node(node) ? state_of(node) : NULL;
    return st ? st->kind : APR_NODE_SOURCE;
}

uint32_t apr_node_model_id(HWND node)
{
    NodeState *st = apr_node_is_node(node) ? state_of(node) : NULL;
    return st ? st->model_id : 0u;
}

int apr_node_sub_id(HWND node)
{
    NodeState *st = apr_node_is_node(node) ? state_of(node) : NULL;
    return st ? st->sub_id : 0;
}

void apr_node_set_text(HWND node, const wchar_t *title, const wchar_t *detail)
{
    NodeState *st = apr_node_is_node(node) ? state_of(node) : NULL;

    if (!st) return;
    lstrcpynW(st->title, title ? title : L"", APR_NODE_TITLE_CCH);
    lstrcpynW(st->detail, detail ? detail : L"", APR_NODE_DETAIL_CCH);
    InvalidateRect(node, NULL, FALSE);
}

void apr_node_set_accessible(HWND node, const wchar_t *name, const wchar_t *desc)
{
    if (!apr_node_is_node(node)) return;

    if (name) {
        apr_ui_set_accessible_name_text(node, name);
        /* Belt as well as braces, exactly as app.c does for its own classes:
         * with no annotation service the window text is still a name. */
        SetWindowTextW(node, name);
        /* A name that changed silently is a change the user did not hear.
         * This is the event a screen reader re-reads on. */
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, node, OBJID_CLIENT, CHILDID_SELF);
    }
    if (desc) apr_ui_set_accessible_description_text(node, desc);
}

void apr_node_set_pending(HWND node, int pending)
{
    NodeState *st = apr_node_is_node(node) ? state_of(node) : NULL;

    if (!st || st->pending == !!pending) return;
    st->pending = !!pending;
    InvalidateRect(node, NULL, FALSE);
}

int apr_node_pending(HWND node)
{
    NodeState *st = apr_node_is_node(node) ? state_of(node) : NULL;
    return st ? st->pending : 0;
}

void apr_node_set_theme(HWND node, AprTheme *theme)
{
    NodeState *st = apr_node_is_node(node) ? state_of(node) : NULL;

    if (!st) return;
    st->theme = theme;
    InvalidateRect(node, NULL, TRUE);
}

void apr_node_announce_self(HWND node)
{
    if (!apr_node_is_node(node)) return;
    NotifyWinEvent(EVENT_OBJECT_FOCUS, node, OBJID_CLIENT, CHILDID_SELF);
}
