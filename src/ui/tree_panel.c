/*
 * tree_panel.c -- the structure panel.
 *
 * A real Win32 TreeView (WC_TREEVIEW) over the same AprGraph the canvas draws.
 * Why it exists and what a selection means are in ui_tree_panel.h; what
 * follows is why the code looks the way it does.
 *
 * ---------------------------------------------------------------------------
 * WHY A STANDARD CONTROL, AND WHY NM_CUSTOMDRAW IS THE ONLY WAY WE TOUCH IT
 *
 *   comctl32's TreeView ships an MSAA server that NVDA, JAWS and Narrator have
 *   had two decades to get right: item, level, expanded/collapsed, position in
 *   set, and the selection pattern all arrive for free and behave the way a
 *   user's muscle memory expects. Nothing we could write in C would be as good
 *   and none of it would be as WELL KNOWN, which is the property that actually
 *   matters to someone driving it by ear.
 *
 *   NM_CUSTOMDRAW changes how that control PAINTS. Owner-draw
 *   (LVS_OWNERDRAWFIXED and its relatives) replaces what the control IS: the
 *   accessibility server goes with it and a screen reader is handed an empty
 *   rectangle. AGENTS.md rule 5 is that line, and this file stays on the safe
 *   side of it in the strongest available way:
 *
 *     THIS FILE'S CUSTOM DRAW SETS clrText, clrTextBk AND THE FONT. IT ISSUES
 *     NO GDI COORDINATE OF ITS OWN.
 *
 *   That is not timidity, it buys two things:
 *
 *     - The item text a screen reader reads is still the item text the control
 *       drew, because we never drew it. There is no second copy to drift.
 *     - It is what makes WS_EX_LAYOUTRTL safe on this window. The style hands
 *       the control a MIRRORED DC (ui_app.h, CONVENTION 1); a painter that
 *       passed its own rectangles into that DC would get them reflected. We
 *       pass none. If anyone ever adds geometry here -- a colour bar, a badge,
 *       a progress bar -- THE LAYOUTRTL CALL BELOW HAS TO BE RECONSIDERED AT
 *       THE SAME TIME.
 *
 * ---------------------------------------------------------------------------
 * WHY THERE IS A HOST WINDOW AND NOT JUST THE TREEVIEW
 *
 *   NM_CUSTOMDRAW arrives as WM_NOTIFY at the control's PARENT. The frame
 *   (app.c) does not handle WM_NOTIFY and must not have to: it owns no model
 *   and knows nothing about panes' insides. So the pane the frame positions is
 *   a host window of ours, and the TreeView is its only child. The host is
 *   also where the panel's state lives, which is what lets every public
 *   function here marshal through SendMessageW and be callable from any
 *   thread.
 *
 *   Focus is forwarded from the host to the TreeView, so F6 lands a screen
 *   reader on the tree itself rather than on a container that has nothing to
 *   say -- EXCEPT when focus arrived FROM the TreeView, which is Shift+Tab
 *   walking backwards out of it. Forwarding that one would make Shift+Tab a
 *   trap: out to the host, straight back in, for ever.
 *
 * ---------------------------------------------------------------------------
 * WHY THE ROWS ARE BUILT BY A PURE FUNCTION
 *
 *   apr_tree_panel_rows() and apr_tree_panel_label() take a graph and return
 *   rows and text, with no window anywhere. The window is a renderer of their
 *   output. "Structure is source -> bus -> action and it does not flip in
 *   Arabic" is then a property a test asserts by calling a function twice in
 *   one process, instead of something inferred from a live control's pixels.
 */
#include "ui_tree_panel.h"

#include <commctrl.h>
#include <stdlib.h>
#include <string.h>
#include <uxtheme.h>   /* SetWindowTheme -- the light "Explorer" class */

#include "log.h"
#include "strings.h"
#include "ui_app.h"
#include "ui_darkmode.h"

#pragma comment(lib, "comctl32.lib")

#define APR_TREE_PANEL_CLASS L"AprRecorderTreePanel"
#define APR_TREE_ID_TV       0x1021

/* Longest row text. A row is a whole sentence carrying two names and a state
 * clause, and Arabic is longer than English at the same information content --
 * "never size to the English string" applies to buffers too. */
#define APR_TREE_TEXT_CCH 512

/* Private messages. Every public function marshals through one of these, so
 * the panel's state is only ever touched on the thread that owns the window
 * and a caller on any other thread still gets an answer rather than a race. */
#define APR_TPM_SET_GRAPH (WM_USER + 0x140)
#define APR_TPM_GET_GRAPH (WM_USER + 0x141)
#define APR_TPM_REFRESH   (WM_USER + 0x142)
#define APR_TPM_GET_SEL   (WM_USER + 0x143)
#define APR_TPM_SELECT    (WM_USER + 0x144)
#define APR_TPM_SET_SINK  (WM_USER + 0x145)
#define APR_TPM_GET_TV    (WM_USER + 0x146)
#define APR_TPM_FOCUS_TV  (WM_USER + 0x147)

typedef struct SinkArgs {
    AprTreeSelFn fn;
    void        *user;
} SinkArgs;

typedef struct TreePanel {
    HWND       host;
    HWND       tv;
    AprTheme  *theme;
    AprGraph  *graph;

    AprTreeSelFn sink;
    void        *sink_user;

    int        suppress;    /* inside apr_tree_panel_select: do not echo   */
    int        applying;    /* SetWindowTheme -> WM_THEMECHANGED re-entry  */

    AprTreeRow rows[APR_TREE_MAX_ROWS];
    HTREEITEM  item[APR_TREE_MAX_ROWS];
    size_t     nrows;

    /* Test seam. Counts CDDS_ITEMPREPAINT callbacks, so a test can prove that
     * custom draw actually RAN before it asserts the accessibility tree is
     * still intact. Without it, "custom draw did not break a11y" passes
     * trivially on a build where custom draw never fired. */
    unsigned   cd_items;
} TreePanel;

/* ==========================================================================
 * The projection -- pure
 * ======================================================================== */

static AprStrId source_kind_id(AprSourceKind k)
{
    switch (k) {
    case APR_SRC_DEVICE:  return APR_S_SOURCE_KIND_DEVICE;
    case APR_SRC_FAKE:    return APR_S_SOURCE_KIND_FAKE;
    case APR_SRC_PROCESS: break;
    }
    return APR_S_SOURCE_KIND_PROCESS;
}

/* The state clause, or NULL when there is nothing wrong.
 *
 * BOTH OF THESE LOOK PERFECTLY HEALTHY FROM EVERY OTHER ANGLE, which is
 * exactly why they belong in the name a screen reader reads rather than in a
 * colour or an icon:
 *
 *   - a source muted in the Windows volume mixer records PURE SILENCE. That is
 *     measured, not assumed: process loopback sits post-session-volume, and a
 *     volume-0 app still reports to the engine that it is rendering (see the
 *     capture spike in SESSION-HANDOFF.md). Nothing else in the UI can tell
 *     you, and you find out when you play the file back.
 *   - a source whose process has exited keeps producing buffers full of zeros
 *     for ever. WASAPI never signals that the target died.
 */
static AprStrId source_state_id(const AprSource *s)
{
    if (!s) return APR_S__NONE;
    if (!apr_source_alive(s)) return APR_S_UI_TREE_STATE_EXITED;
    if (apr_source_muted(s))  return APR_S_UI_TREE_STATE_MUTED;
    return APR_S__NONE;
}

static int rows_push(AprTreeRow *out, size_t cap, size_t *n,
                     AprTreeRowKind kind, AprBusId bus, AprSourceId src,
                     size_t action, int depth)
{
    if (out && *n < cap) {
        AprTreeRow *r = &out[*n];
        /* Zeroed WHOLE, padding included, before any field is assigned. The
         * row list is a value a caller may legitimately compare or hash to
         * answer "did the shape change" -- and indeterminate padding makes
         * two structurally identical lists compare unequal at random. Found by
         * tests/test_ui_tree.c's RTL invariance case, which passed in Debug
         * and failed in Release on twelve bytes of nothing. */
        memset(r, 0, sizeof *r);
        r->sel.kind   = kind;
        r->sel.bus    = bus;
        r->sel.source = src;
        r->sel.action = action;
        r->depth      = depth;
    }
    (*n)++;
    return 1;
}

size_t apr_tree_panel_rows(const AprGraph *g, AprTreeRow *out, size_t cap)
{
    size_t n = 0, nb, ns, i, j;
    AprSourceId sids[APR_MAX_SOURCES_PER_BUS];
    int any_unassigned = 0;

    if (!g) {
        rows_push(out, cap, &n, APR_TREE_ROW_EMPTY, 0, 0, 0, 0);
        return n;
    }

    nb = apr_graph_bus_count(g);
    ns = apr_graph_source_count(g);

    if (nb == 0 && ns == 0) {
        rows_push(out, cap, &n, APR_TREE_ROW_EMPTY, 0, 0, 0, 0);
        return n;
    }

    /* ONE SUBTREE PER BUS, AND INSIDE IT THE SIGNAL CHAIN IN ORDER: the
     * sources feeding the bus first, the files it writes last. That is the
     * direction the audio travels, so a screen reader user reading straight
     * down is reading the chain. It is identical in Arabic -- nothing in this
     * function asks apr_str_is_rtl(), and nothing may. */
    for (i = 0; i < nb; ++i) {
        AprBus *b = apr_graph_bus_at(g, i);
        AprBusId bid;
        size_t na, nsrc;

        if (!b) continue;
        bid = apr_bus_id(b);
        rows_push(out, cap, &n, APR_TREE_ROW_BUS, bid, 0, 0, 0);

        nsrc = apr_graph_sources_for_bus(g, bid, sids, APR_MAX_SOURCES_PER_BUS);
        if (nsrc > APR_MAX_SOURCES_PER_BUS) nsrc = APR_MAX_SOURCES_PER_BUS;
        for (j = 0; j < nsrc; ++j) {
            rows_push(out, cap, &n, APR_TREE_ROW_SOURCE, bid, sids[j], 0, 1);
        }

        na = apr_bus_action_count(b);
        for (j = 0; j < na; ++j) {
            rows_push(out, cap, &n, APR_TREE_ROW_ACTION, bid, 0, j, 1);
        }
    }

    /* A SOURCE WIRED TO NOTHING IS THE FAILURE MODE THIS PANEL EXISTS TO
     * CATCH. It is in the session, it is capturing, it costs a ring and a
     * thread, and not one byte of it reaches a file. On the canvas it is a box
     * with no edge leaving it, which is visible at a glance and invisible any
     * other way; here it gets its own group and says so in its own name. */
    for (i = 0; i < ns; ++i) {
        AprSource *s = apr_graph_source_at(g, i);
        if (!s) continue;
        if (apr_graph_buses_for_source(g, apr_source_id(s), NULL, 0) == 0) {
            any_unassigned = 1;
            break;
        }
    }
    if (any_unassigned) {
        rows_push(out, cap, &n, APR_TREE_ROW_UNASSIGNED, 0, 0, 0, 0);
        for (i = 0; i < ns; ++i) {
            AprSource *s = apr_graph_source_at(g, i);
            if (!s) continue;
            if (apr_graph_buses_for_source(g, apr_source_id(s), NULL, 0) != 0) {
                continue;
            }
            rows_push(out, cap, &n, APR_TREE_ROW_SOURCE, 0, apr_source_id(s), 0, 1);
        }
    }

    return n;
}

/* ==========================================================================
 * Row text
 *
 * NOTHING HERE GLUES TWO SENTENCES TOGETHER. Every row is one catalog entry
 * with positional inserts, and the pieces that vary -- a pluralised count, a
 * state clause -- occupy one insert each so a translator can move them.
 * strings.h explains why that is not the same thing as concatenation: the
 * inserts are whole translatable units and their ORDER is the translator's.
 * ======================================================================== */

static size_t label_bus(const AprGraph *g, AprBusId bid, wchar_t *buf, size_t cch)
{
    AprBus *b = apr_graph_bus(g, bid);
    wchar_t nsrc[64], nout[64];
    const wchar_t *args[3];

    if (!b) { if (cch) buf[0] = 0; return 0; }

    apr_str_plural_format(APR_S_N_SOURCES, (int64_t)apr_bus_source_count(b),
                          nsrc, 64, NULL, 0);
    apr_str_plural_format(APR_S_N_OUTPUTS, (int64_t)apr_bus_action_count(b),
                          nout, 64, NULL, 0);

    args[0] = apr_bus_name(b);
    args[1] = nsrc;
    args[2] = nout;
    return apr_str_format(apr_bus_running(b) ? APR_S_UI_TREE_BUS_RECORDING
                                             : APR_S_UI_TREE_BUS,
                          buf, cch, args, 3);
}

static size_t label_source(const AprGraph *g, const AprTreeSel *sel,
                           wchar_t *buf, size_t cch)
{
    AprSource *s = apr_graph_source(g, sel->source);
    AprBus *b;
    AprStrId state;
    const wchar_t *args[5];
    wchar_t others[64];
    size_t nbuses;

    if (!s) { if (cch) buf[0] = 0; return 0; }

    state  = source_state_id(s);
    nbuses = apr_graph_buses_for_source(g, sel->source, NULL, 0);

    args[0] = apr_source_name(s);
    args[1] = apr_str(source_kind_id(apr_source_kind(s)));

    if (sel->bus == 0 || nbuses == 0) {
        if (state != APR_S__NONE) {
            args[2] = apr_str(state);
            return apr_str_format(APR_S_UI_TREE_SOURCE_UNUSED_STATE,
                                  buf, cch, args, 3);
        }
        return apr_str_format(APR_S_UI_TREE_SOURCE_UNUSED, buf, cch, args, 2);
    }

    b = apr_graph_bus(g, sel->bus);
    if (!b) { if (cch) buf[0] = 0; return 0; }

    /* THE FACT A TREE CANNOT SHOW STRUCTURALLY. The model is a graph: this
     * source may also be feeding other buses, and it appears once under each
     * of them. Nesting can express one parent, so the rest goes in the name --
     * as a count rather than a list, because "and 4 other buses" is what a
     * listener needs and four more names is not. */
    if (nbuses > 1) {
        apr_str_plural_format(APR_S_N_OTHER_BUSES, (int64_t)(nbuses - 1),
                              others, 64, NULL, 0);
        if (state != APR_S__NONE) {
            args[2] = apr_str(state);
            args[3] = apr_bus_name(b);
            args[4] = others;
            return apr_str_format(APR_S_UI_TREE_SOURCE_STATE_SHARED,
                                  buf, cch, args, 5);
        }
        args[2] = apr_bus_name(b);
        args[3] = others;
        return apr_str_format(APR_S_UI_TREE_SOURCE_SHARED, buf, cch, args, 4);
    }

    if (state != APR_S__NONE) {
        args[2] = apr_str(state);
        args[3] = apr_bus_name(b);
        return apr_str_format(APR_S_UI_TREE_SOURCE_STATE, buf, cch, args, 4);
    }
    args[2] = apr_bus_name(b);
    return apr_str_format(APR_S_UI_TREE_SOURCE, buf, cch, args, 3);
}

static size_t label_action(const AprGraph *g, const AprTreeSel *sel,
                           wchar_t *buf, size_t cch)
{
    AprBus *b = apr_graph_bus(g, sel->bus);
    const AprActionVTable *vt;
    const wchar_t *args[2];

    if (!b) { if (cch) buf[0] = 0; return 0; }
    vt = apr_bus_action_at(b, sel->action);
    if (!vt) { if (cch) buf[0] = 0; return 0; }

    /* The encoder's display name is a catalog id, not a literal in its vtable
     * (AGENTS.md rule 6), so it resolves here, at the point of display, on the
     * UI thread -- which is where apr_str() is allowed to lock and allocate. */
    args[0] = vt->display_name_id ? apr_str(vt->display_name_id) : L"";
    args[1] = apr_bus_name(b);
    return apr_str_format(apr_bus_action_failed(b, sel->action)
                            ? APR_S_UI_TREE_ACTION_FAILED
                            : APR_S_UI_TREE_ACTION,
                          buf, cch, args, 2);
}

size_t apr_tree_panel_label(const AprGraph *g, const AprTreeSel *sel,
                            wchar_t *buf, size_t cch)
{
    if (!buf || cch == 0) return 0;
    buf[0] = 0;
    if (!sel) return 0;

    switch (sel->kind) {
    case APR_TREE_ROW_EMPTY:
        lstrcpynW(buf, apr_str(APR_S_UI_TREE_EMPTY), (int)cch);
        return wcslen(buf);
    case APR_TREE_ROW_UNASSIGNED:
        lstrcpynW(buf, apr_str(APR_S_UI_TREE_UNASSIGNED_GROUP), (int)cch);
        return wcslen(buf);
    case APR_TREE_ROW_BUS:
        if (!g) return 0;
        return label_bus(g, sel->bus, buf, cch);
    case APR_TREE_ROW_SOURCE:
        if (!g) return 0;
        return label_source(g, sel, buf, cch);
    case APR_TREE_ROW_ACTION:
        if (!g) return 0;
        return label_action(g, sel, buf, cch);
    case APR_TREE_ROW_NONE:
        break;
    }
    return 0;
}

/* ==========================================================================
 * Theme
 * ======================================================================== */

static void tp_apply_theme(TreePanel *st)
{
    const AprThemePalette *c;
    const AprThemeMetrics *m;
    HFONT f;
    int dark;

    if (!st || !st->tv || !st->theme) return;

    /* SetWindowTheme sends WM_THEMECHANGED to the window it themes, and this
     * function runs from the host's WM_THEMECHANGED handler. Without the guard
     * that is unbounded recursion inside a window procedure, which Windows
     * reports as STATUS_FATAL_USER_CALLBACK_EXCEPTION and does not tell you
     * which callback. app.c hit exactly this; the same trap is here. */
    if (st->applying) return;
    st->applying = 1;

    c = apr_theme_palette(st->theme);
    m = apr_theme_metrics(st->theme);
    dark = apr_theme_is_dark(st->theme);

    apr_darkmode_apply_to_window(st->tv, dark);
    if (!dark) {
        /* apr_darkmode_apply_to_window(..., 0) clears the theme class, which
         * also drops the modern hover and expando look. Put the documented
         * light one back. */
        (void)SetWindowTheme(st->tv, L"Explorer", NULL);
    }

    SendMessageW(st->tv, TVM_SETBKCOLOR,   0, (LPARAM)c->surface);
    SendMessageW(st->tv, TVM_SETTEXTCOLOR, 0, (LPARAM)c->text);

    /* METRICS ARE DEVICE PIXELS ALREADY (ui_theme.h). No DPI multiply here or
     * anywhere else in this file. Row height comes from the FONT's metrics,
     * never from a string, which is what stops an English-measured row from
     * clipping Arabic. */
    SendMessageW(st->tv, TVM_SETITEMHEIGHT,
                 (WPARAM)apr_theme_block_height(st->theme, APR_FONT_BODY, 1,
                                                m->space_xs), 0);
    SendMessageW(st->tv, TVM_SETINDENT, (WPARAM)m->space_lg, 0);

    f = apr_theme_font(st->theme, APR_FONT_BODY);
    if (f) SendMessageW(st->tv, WM_SETFONT, (WPARAM)f, TRUE);

    /* A STANDARD CONTROL WE NEVER PAINT INSIDE, so the free mirroring is
     * exactly what we want: indent guides, expand buttons, scroll bar and text
     * alignment all flip and the item ORDER does not. See the header note --
     * this call is only safe because our custom draw passes no coordinates. */
    apr_ui_apply_rtl(st->tv, apr_ui_dir());

    InvalidateRect(st->tv, NULL, TRUE);
    st->applying = 0;
}

/* ==========================================================================
 * Building the control from the rows
 * ======================================================================== */

static void tp_build(TreePanel *st)
{
    AprTreeSel keep;
    size_t n, i;
    HTREEITEM parent[8];
    int had_sel;

    if (!st || !st->tv) return;

    /* Keep the caret on the same MODEL OBJECT across a rebuild. An HTREEITEM
     * does not survive TVM_DELETEITEM but an AprTreeSel does, which is the
     * same reason selection is a model identity in the first place. */
    memset(&keep, 0, sizeof keep);
    had_sel = 0;
    {
        HTREEITEM cur = (HTREEITEM)SendMessageW(st->tv, TVM_GETNEXTITEM,
                                                TVGN_CARET, 0);
        if (cur) {
            TVITEMW it;
            memset(&it, 0, sizeof it);
            it.mask = TVIF_PARAM;
            it.hItem = cur;
            if (SendMessageW(st->tv, TVM_GETITEMW, 0, (LPARAM)&it)) {
                size_t idx = (size_t)it.lParam;
                if (idx < st->nrows) { keep = st->rows[idx].sel; had_sel = 1; }
            }
        }
    }

    /* Re-applied here, not only at creation: names are stored as ids
     * precisely so that a language change can re-resolve them (ui_app.h,
     * CONVENTION 2), and a rebuild is when that happens. */
    apr_ui_set_accessible_name(st->tv, APR_S_UI_TREE_NAME);
    apr_ui_set_accessible_description(st->tv, APR_S_UI_TREE_DESC);

    st->suppress++;
    SendMessageW(st->tv, WM_SETREDRAW, FALSE, 0);
    SendMessageW(st->tv, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);

    n = apr_tree_panel_rows(st->graph, st->rows, APR_TREE_MAX_ROWS);
    if (n > APR_TREE_MAX_ROWS) n = APR_TREE_MAX_ROWS;
    st->nrows = n;
    memset(parent, 0, sizeof parent);
    memset(st->item, 0, sizeof st->item);

    for (i = 0; i < n; ++i) {
        TVINSERTSTRUCTW ins;
        wchar_t text[APR_TREE_TEXT_CCH];
        int depth = st->rows[i].depth;

        if (depth < 0) depth = 0;
        if (depth > 7) depth = 7;

        apr_tree_panel_label(st->graph, &st->rows[i].sel, text, APR_TREE_TEXT_CCH);
        if (text[0] == 0) {
            /* A row with no text would be an element a screen reader lands on
             * and cannot describe -- the exact defect tests/test_ui_a11y.c
             * exists to catch. Drop the row instead of adding a silent one,
             * and say so in the log. */
            APR_WARN(L"tree panel: row %u produced no text (kind %d)",
                     (unsigned)i, (int)st->rows[i].sel.kind);
            st->item[i] = NULL;
            continue;
        }

        memset(&ins, 0, sizeof ins);
        ins.hParent      = depth > 0 ? parent[depth - 1] : TVI_ROOT;
        if (!ins.hParent) ins.hParent = TVI_ROOT;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask    = TVIF_TEXT | TVIF_PARAM;
        ins.item.pszText = text;
        ins.item.lParam  = (LPARAM)i;

        st->item[i] = (HTREEITEM)SendMessageW(st->tv, TVM_INSERTITEMW, 0,
                                              (LPARAM)&ins);
        parent[depth] = st->item[i];
    }

    /* EXPANDED BY DEFAULT. The panel's whole point is "the session at a
     * glance"; a collapsed tree is the session at a keystroke per bus. A user
     * who wants it collapsed presses Left, and the control remembers. */
    for (i = 0; i < n; ++i) {
        if (st->item[i] && st->rows[i].depth == 0) {
            SendMessageW(st->tv, TVM_EXPAND, TVE_EXPAND, (LPARAM)st->item[i]);
        }
    }

    SendMessageW(st->tv, WM_SETREDRAW, TRUE, 0);
    st->suppress--;

    if (had_sel) {
        (void)apr_tree_panel_select(st->host, &keep);
    }
    InvalidateRect(st->tv, NULL, TRUE);
}

/* ==========================================================================
 * Custom draw -- colours and fonts only. Read the header note before adding
 * anything to this function.
 * ======================================================================== */

static COLORREF tp_row_color(const TreePanel *st, const AprTreeRow *row)
{
    const AprThemePalette *c = apr_theme_palette(st->theme);

    switch (row->sel.kind) {
    case APR_TREE_ROW_BUS:
        return c->node_bus;
    case APR_TREE_ROW_ACTION:
        return c->node_action;
    case APR_TREE_ROW_SOURCE: {
        /* A source that is recording silence is coloured as a warning -- AND
         * ui_theme.h's rule holds: the colour is never the only carrier of the
         * fact. The row's TEXT says "muted in Windows, so it records silence",
         * which is what a screen reader reads and what survives a photocopy,
         * a colour-blind user and high contrast. */
        AprSource *s = st->graph ? apr_graph_source(st->graph, row->sel.source)
                                 : NULL;
        if (s && source_state_id(s) != APR_S__NONE) return c->warn;
        if (row->sel.bus == 0) return c->text_dim;
        return c->node_source;
    }
    case APR_TREE_ROW_UNASSIGNED:
    case APR_TREE_ROW_EMPTY:
    case APR_TREE_ROW_NONE:
        break;
    }
    return c->text_dim;
}

static LRESULT tp_customdraw(TreePanel *st, NMTVCUSTOMDRAW *cd)
{
    size_t idx;
    HFONT f;

    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;

    case CDDS_ITEMPREPAINT:
        st->cd_items++;

        if (!st->theme) return CDRF_DODEFAULT;

        /* HIGH CONTRAST IS NOT A COLOUR SCHEME (ui_theme.h). The user has told
         * the OS which colours they can see; every override below is us
         * replacing an accessibility setting with a taste preference. Hand the
         * item straight back to the control. */
        if (apr_theme_high_contrast(st->theme)) return CDRF_DODEFAULT;

        /* Selection colours belong to the control too. It knows the difference
         * between selected-and-focused and selected-while-focus-is-elsewhere,
         * and that difference is how a keyboard user finds their place after
         * F6 -- TVS_SHOWSELALWAYS is set precisely so it is visible. */
        if (cd->nmcd.uItemState & CDIS_SELECTED) return CDRF_DODEFAULT;

        idx = (size_t)cd->nmcd.lItemlParam;
        if (idx >= st->nrows) return CDRF_DODEFAULT;

        cd->clrTextBk = apr_theme_palette(st->theme)->surface;
        cd->clrText   = tp_row_color(st, &st->rows[idx]);

        /* A bus is a heading over the rows beneath it, so it is set in the
         * strong face. The font is re-selected for EVERY item, not only the
         * bold ones: leaving one item's font selected leaks it into the next. */
        f = apr_theme_font(st->theme,
                           st->rows[idx].sel.kind == APR_TREE_ROW_BUS
                             ? APR_FONT_BODY_STRONG : APR_FONT_BODY);
        if (f) {
            SelectObject(cd->nmcd.hdc, f);
            return CDRF_NEWFONT;
        }
        return CDRF_DODEFAULT;

    default:
        break;
    }
    return CDRF_DODEFAULT;
}

/* ==========================================================================
 * Notifications
 * ======================================================================== */

static void tp_selection_changed(TreePanel *st, HTREEITEM hit)
{
    AprTreeSel sel;
    TVITEMW it;

    if (!st->sink || st->suppress) return;

    memset(&sel, 0, sizeof sel);
    memset(&it, 0, sizeof it);
    it.mask = TVIF_PARAM;
    it.hItem = hit;
    if (hit && SendMessageW(st->tv, TVM_GETITEMW, 0, (LPARAM)&it)) {
        size_t idx = (size_t)it.lParam;
        if (idx < st->nrows) sel = st->rows[idx].sel;
    }
    st->sink(st->host, &sel, st->sink_user);
}

/* ==========================================================================
 * The host window
 * ======================================================================== */

static int tp_create_tree(TreePanel *st, HINSTANCE inst)
{
    st->tv = CreateWindowExW(
        0, WC_TREEVIEWW, L"",
        /* TVS_FULLROWSELECT and TVS_HASLINES are documented as mutually
         * exclusive, and the Explorer look wants neither set of lines, so:
         * buttons at the root, selection that stays visible when focus leaves
         * (which is how a keyboard user finds their place again after F6), and
         * hot tracking for the pointer. */
        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        TVS_HASBUTTONS | TVS_LINESATROOT |
        TVS_SHOWSELALWAYS | TVS_TRACKSELECT,
        0, 0, 10, 10,
        st->host, (HMENU)(UINT_PTR)APR_TREE_ID_TV, inst, NULL);
    if (!st->tv) return 0;

    /* Double buffering is a v6 extended style; without it a themed TreeView
     * flickers on every selection move, which is a real problem for anyone
     * using screen magnification. */
    SendMessageW(st->tv, TVM_SETEXTENDEDSTYLE,
                 (WPARAM)(TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS),
                 (LPARAM)(TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS));

    /* CONVENTION 2: name it, from the catalog. The pane around it is named
     * "Structure" by the frame; this is the control inside, and a screen
     * reader reads both -- so they say different, useful things rather than
     * the same word twice. The description is where the keys live, because
     * that is the one place a first-time user is guaranteed to hear it. */
    apr_ui_set_accessible_name(st->tv, APR_S_UI_TREE_NAME);
    apr_ui_set_accessible_description(st->tv, APR_S_UI_TREE_DESC);
    return 1;
}

static LRESULT CALLBACK tp_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TreePanel *st = (TreePanel *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        TreePanel *s = (TreePanel *)calloc(1, sizeof *s);
        if (!s) return FALSE;
        s->host  = hwnd;
        s->theme = (AprTheme *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        if (!st) return -1;
        if (!tp_create_tree(st, cs->hInstance)) {
            APR_WARN(L"tree panel: could not create the TreeView");
            return -1;
        }
        tp_apply_theme(st);
        tp_build(st);
        return 0;
    }

    case WM_NCDESTROY:
        free(st);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_SIZE:
        if (st && st->tv) {
            MoveWindow(st->tv, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        }
        return 0;

    case WM_SETFOCUS:
        /* Hand focus on to the tree, so F6 lands a screen reader on something
         * with items to read rather than on a container that can only say its
         * own name.
         *
         * POSTED, NOT CALLED. SetFocus() from inside WM_SETFOCUS is swallowed:
         * the outer SetFocus that delivered this message reasserts its own
         * target as it unwinds, so the nested call appears to do nothing at
         * all. Measured, not assumed -- tests/test_ui_tree.c asks
         * GetGUIThreadInfo which window really ended up with focus, and that
         * is the assertion that caught it. Posting moves the call outside the
         * nested dispatch, where it takes.
         *
         * THE HOST IS DELIBERATELY NOT WS_TABSTOP, and that is what makes
         * unconditional forwarding safe. An earlier version forwarded only
         * when focus had not come FROM the tree, to stop Shift+Tab bouncing
         * out of the pane and straight back in -- but "came from the tree" is
         * indistinguishable from "the frame handed focus to this pane while
         * the tree happened to hold it", so F6 arriving from the canvas got
         * stranded on the container. Taking the host out of the tab ring
         * removes the bounce at its source: Shift+Tab from the tree goes to
         * whatever precedes the pane, because the pane is not a stop. */
        if (st && st->tv) PostMessageW(hwnd, APR_TPM_FOCUS_TV, 0, 0);
        break;

    case APR_TPM_FOCUS_TV:
        /* Only if focus is still sitting on the host -- by the time this
         * arrives the user may have moved on, and stealing it back would be
         * the same bug from the other direction. */
        if (st && st->tv && GetFocus() == hwnd) SetFocus(st->tv);
        return 0;

    case WM_SETFONT:
        if (st && st->tv) SendMessageW(st->tv, WM_SETFONT, wp, lp);
        return 0;

    case WM_THEMECHANGED:
        /* Colours, fonts and metrics, then the rows -- because a theme change
         * usually arrives together with an apr_theme_refresh(), which is also
         * where the interface DIRECTION is re-read. A language change on its
         * own does not broadcast anything, so a caller that changes language
         * calls apr_tree_panel_refresh(); that is documented in the header
         * rather than guessed at from WM_SETTINGCHANGE, which arrives
         * constantly and for everything. */
        if (st) {
            tp_apply_theme(st);
            tp_build(st);
        }
        break;

    case WM_SETTINGCHANGE:
        if (st) tp_apply_theme(st);
        break;

    case WM_ERASEBKGND:
        return 1;   /* the TreeView covers the client area */

    case WM_PAINT: {
        /* Only ever visible in the one pixel of rounding between the host and
         * its child. Painted anyway so a resize never flashes white in dark
         * mode. */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (st && st->theme) {
            FillRect(dc, &rc,
                     apr_theme_brush(st->theme, apr_theme_palette(st->theme)->surface));
        } else {
            FillRect(dc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (!st || !nh || nh->hwndFrom != st->tv) break;
        if (nh->code == NM_CUSTOMDRAW) {
            return tp_customdraw(st, (NMTVCUSTOMDRAW *)lp);
        }
        if (nh->code == TVN_SELCHANGEDW) {
            NMTREEVIEWW *nmtv = (NMTREEVIEWW *)lp;
            tp_selection_changed(st, nmtv->itemNew.hItem);
            return 0;
        }
        break;
    }

    /* ---- the marshalled public API ---------------------------------- */

    case APR_TPM_SET_GRAPH:
        if (st) { st->graph = (AprGraph *)lp; tp_build(st); }
        return 0;

    case APR_TPM_GET_GRAPH:
        return st ? (LRESULT)(LONG_PTR)st->graph : 0;

    case APR_TPM_REFRESH:
        if (st) tp_build(st);
        return 0;

    case APR_TPM_GET_TV:
        return st ? (LRESULT)(LONG_PTR)st->tv : 0;

    case APR_TPM_SET_SINK:
        if (st && lp) {
            SinkArgs *a = (SinkArgs *)lp;
            st->sink = a->fn;
            st->sink_user = a->user;
        }
        return 0;

    case APR_TPM_GET_SEL: {
        AprTreeSel *out = (AprTreeSel *)lp;
        HTREEITEM cur;
        TVITEMW it;

        if (!out) return 0;
        memset(out, 0, sizeof *out);
        if (!st || !st->tv) return 0;
        cur = (HTREEITEM)SendMessageW(st->tv, TVM_GETNEXTITEM, TVGN_CARET, 0);
        if (!cur) return 0;
        memset(&it, 0, sizeof it);
        it.mask = TVIF_PARAM;
        it.hItem = cur;
        if (!SendMessageW(st->tv, TVM_GETITEMW, 0, (LPARAM)&it)) return 0;
        if ((size_t)it.lParam >= st->nrows) return 0;
        *out = st->rows[(size_t)it.lParam].sel;
        return out->kind != APR_TREE_ROW_NONE;
    }

    case APR_TPM_SELECT: {
        const AprTreeSel *want = (const AprTreeSel *)lp;
        size_t i;

        if (!st || !st->tv || !want) return 0;
        for (i = 0; i < st->nrows; ++i) {
            const AprTreeSel *r = &st->rows[i].sel;
            if (r->kind != want->kind) continue;
            if (r->bus != want->bus) continue;
            if (r->source != want->source) continue;
            if (r->action != want->action) continue;
            if (!st->item[i]) return 0;

            /* Suppressed: a view that echoed a selection back at whoever set
             * it turns one user action into a loop between two panes. */
            st->suppress++;
            SendMessageW(st->tv, TVM_SELECTITEM, TVGN_CARET,
                         (LPARAM)st->item[i]);
            SendMessageW(st->tv, TVM_ENSUREVISIBLE, 0, (LPARAM)st->item[i]);
            st->suppress--;
            return 1;
        }
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int tp_register(HINSTANCE inst)
{
    static int done;
    WNDCLASSEXW wc;

    if (done) return 1;

    memset(&wc, 0, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = tp_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = APR_TREE_PANEL_CLASS;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        APR_WARN(L"tree panel: RegisterClassExW failed (%lu)",
                 (unsigned long)GetLastError());
        return 0;
    }
    done = 1;
    return 1;
}

/* ==========================================================================
 * Public
 * ======================================================================== */

HWND apr_tree_panel_create(HWND parent, AprTheme *theme)
{
    HINSTANCE inst;

    if (!parent || !IsWindow(parent)) return NULL;
    inst = (HINSTANCE)(LONG_PTR)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    if (!inst) inst = GetModuleHandleW(NULL);
    if (!tp_register(inst)) return NULL;

    /* WS_EX_CONTROLPARENT is what lets IsDialogMessage descend into this
     * window and put the TreeView in the frame's tab order. Without it the
     * tree is unreachable by Tab and only F6 gets you there, which is a
     * keyboard path missing -- AGENTS.md rule 5.
     *
     * NOT WS_TABSTOP: the TreeView is the stop, not its container. See
     * WM_SETFOCUS for why a container that is also a tab stop turns Shift+Tab
     * into a bounce. Focus still REACHES this window -- F6 and the frame both
     * SetFocus() it directly -- it simply passes straight through.
     *
     * NOT WS_EX_LAYOUTRTL: this window has a WM_PAINT (ui_app.h CONVENTION 1).
     * The TreeView inside it gets the style individually, in tp_apply_theme. */
    return CreateWindowExW(
        WS_EX_CONTROLPARENT, APR_TREE_PANEL_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 10, 10, parent, NULL, inst, theme);
}

void apr_tree_panel_set_graph(HWND panel, AprGraph *g)
{
    if (panel && IsWindow(panel)) {
        SendMessageW(panel, APR_TPM_SET_GRAPH, 0, (LPARAM)g);
    }
}

AprGraph *apr_tree_panel_graph(HWND panel)
{
    if (!panel || !IsWindow(panel)) return NULL;
    return (AprGraph *)(LONG_PTR)SendMessageW(panel, APR_TPM_GET_GRAPH, 0, 0);
}

void apr_tree_panel_refresh(HWND panel)
{
    if (panel && IsWindow(panel)) SendMessageW(panel, APR_TPM_REFRESH, 0, 0);
}

int apr_tree_panel_get_selection(HWND panel, AprTreeSel *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!panel || !IsWindow(panel)) return 0;
    return (int)SendMessageW(panel, APR_TPM_GET_SEL, 0, (LPARAM)out);
}

int apr_tree_panel_select(HWND panel, const AprTreeSel *sel)
{
    if (!panel || !IsWindow(panel) || !sel) return 0;
    return (int)SendMessageW(panel, APR_TPM_SELECT, 0, (LPARAM)sel);
}

void apr_tree_panel_set_selection_sink(HWND panel, AprTreeSelFn fn, void *user)
{
    SinkArgs a;

    if (!panel || !IsWindow(panel)) return;
    a.fn = fn;
    a.user = user;
    SendMessageW(panel, APR_TPM_SET_SINK, 0, (LPARAM)&a);
}

HWND apr_tree_panel_treeview(HWND panel)
{
    if (!panel || !IsWindow(panel)) return NULL;
    return (HWND)(LONG_PTR)SendMessageW(panel, APR_TPM_GET_TV, 0, 0);
}

/* --------------------------------------------------------------------------
 * Test seam. Deliberately in no header -- tests/test_ui_tree.c declares it
 * itself, the way tests/test_action_wav.c declares apr_wav_test_write_gate.
 *
 * It answers one question: DID CUSTOM DRAW ACTUALLY RUN? "Custom draw did not
 * damage the accessibility tree" is a claim that passes for free on a build
 * where custom draw never fired, and that is the regression AGENTS.md rule 5
 * exists to prevent, so the test asserts both halves.
 * ----------------------------------------------------------------------- */
unsigned apr_tree_panel_test_customdraw_items(HWND panel)
{
    TreePanel *st;

    if (!panel || !IsWindow(panel)) return 0;
    st = (TreePanel *)GetWindowLongPtrW(panel, GWLP_USERDATA);
    return st ? st->cd_items : 0;
}
