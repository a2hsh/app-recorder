/*
 * canvas.c -- the signal-flow canvas.
 *
 * The load-bearing decisions are stated in ui_canvas.h -- nodes are real
 * windows, direction is a layout parameter, the keyboard model is a table.
 * What follows is why the parts that look arbitrary are the way they are.
 *
 * ---------------------------------------------------------------------------
 * THIS FILE OWNS NO MODEL AND CACHES NO GEOMETRY
 *
 *   The canvas holds a borrowed AprGraph and one array of node windows. Every
 *   name, every description and every edge is read out of the graph at the
 *   moment it is needed (graph.h: the canvas and the accessibility tree are
 *   both PROJECTIONS of the model, and neither is derived from the other).
 *   A cached copy of "what feeds what" would be a second model, and a second
 *   model is how the picture and the spoken description drift apart -- which
 *   for this application's first user means the picture silently becomes the
 *   only true one.
 *
 * ---------------------------------------------------------------------------
 * THE THREE LANES ARE ALWAYS THREE
 *
 *   Sources, buses, outputs -- in that logical order, always, in both
 *   directions. Only apr_canvas_node_rect() knows which side each lane lands
 *   on. A lane that collapsed when it was empty would slide every other node
 *   sideways as the user built the graph, which costs a magnifier user their
 *   place and tells a screen reader user nothing at all.
 *
 * ---------------------------------------------------------------------------
 * Z-ORDER IS SET EXPLICITLY, AND THAT IS AN ACCESSIBILITY DECISION
 *
 *   UI Automation enumerates a window's children in Z-ORDER. CreateWindowEx
 *   puts each new child at the top, so "created in logical order" produces a
 *   REVERSED accessibility tree. rebuild() therefore restacks the nodes after
 *   creating them, so that the order a screen reader walks is source -> bus ->
 *   output, in English and in Arabic alike. tests/test_ui_canvas.c asserts the
 *   live sibling chain, not the array, because the array is not what UIA
 *   reads.
 *
 * ---------------------------------------------------------------------------
 * EVERY EDIT ANNOUNCES, AND THE ANNOUNCEMENT IS THREE THINGS AT ONCE
 *
 *   1. The affected nodes' accessible NAME and DESCRIPTION are rebuilt, so the
 *      new edge is in the text before anything is said. A screen reader that
 *      re-reads the node afterwards must not get the old sentence.
 *   2. EVENT_OBJECT_NAMECHANGE fires for those nodes, and focus moves to the
 *      node whose meaning changed, which is what a screen reader actually
 *      speaks.
 *   3. The whole sentence goes to the announcement sink and is kept, so that
 *      "the user was told" is a property a test can assert rather than a claim
 *      a reviewer has to believe.
 *
 * ---------------------------------------------------------------------------
 * NOT DONE HERE, DELIBERATELY
 *
 *   Adding a source, a bus or an output needs a chooser dialog, and there is
 *   no dialog layer yet. The operations exist, are bound to keys, are on the
 *   menu, and SAY that they are not available yet -- which is the same
 *   "greyed, not absent" rule app.c applies to the menu, applied to speech.
 */
#include "ui_canvas.h"

#include <math.h>
#include <objbase.h>
#include <oleacc.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "log.h"
#include "source.h"
#include "ui_node.h"

#ifndef EVENT_OBJECT_LIVEREGIONCHANGED
#define EVENT_OBJECT_LIVEREGIONCHANGED 0x8019
#endif

#define APR_CANVAS_CLASS L"AprRecorderCanvas"

/* Every node the model can hold at once. Fixed, like the graph's own arrays:
 * a canvas that reallocated while a paint was walking it buys nothing. */
#define APR_CANVAS_MAX_NODES \
    (APR_MAX_SOURCES + APR_MAX_BUSES + APR_MAX_BUSES * APR_MAX_ACTIONS_PER_BUS)

#define APR_CANVAS_NODE_ID0 0x2000

#define APR_CANVAS_TEXT_CCH  512
#define APR_CANVAS_NAME_CCH  128

/* Level range and step, in tenths of a decibel so the arithmetic stays integer
 * everywhere except the one conversion to and from linear gain. */
#define APR_CANVAS_GAIN_MIN_DB10  (-600)
#define APR_CANVAS_GAIN_MAX_DB10  (120)
#define APR_CANVAS_GAIN_STEP_DB10 (10)

typedef struct CanvasNode {
    HWND        hwnd;
    AprNodeKind kind;
    uint32_t    model_id;   /* source id, or bus id (also for its outputs) */
    int         sub_id;     /* output index within its bus; 0 otherwise    */
    int         column;
    int         row;
} CanvasNode;

typedef struct CanvasState {
    HWND       hwnd;
    AprTheme  *theme;
    AprGraph  *graph;

    CanvasNode node[APR_CANVAS_MAX_NODES];
    int        count;
    int        column_count[APR_COL_COUNT];

    int        cur;      /* index of the node with focus, or -1 */
    int        pending;  /* held end of a half-made edge, or -1 */
    int        pending_is_disconnect;

    int        scroll_x;
    int        scroll_y;

    AprCanvasAnnounceFn announce;
    void               *announce_user;
    wchar_t             last[APR_CANVAS_TEXT_CCH];

    int rebuilding;
} CanvasState;

static CanvasState *state_of(HWND h)
{
    wchar_t cls[64];

    if (!h || !IsWindow(h)) return NULL;
    if (GetClassNameW(h, cls, 64) <= 0) return NULL;
    if (CompareStringOrdinal(cls, -1, APR_CANVAS_CLASS, -1, FALSE) != CSTR_EQUAL) {
        return NULL;
    }
    return (CanvasState *)GetWindowLongPtrW(h, GWLP_USERDATA);
}

/* ==========================================================================
 * Layout -- pure. No window handles, no globals, direction as an argument.
 * ======================================================================== */

static int layout_rows(const AprCanvasLayoutIn *in)
{
    int i, rows = 0;

    for (i = 0; i < APR_COL_COUNT; ++i) {
        if (in->count[i] > rows) rows = in->count[i];
    }
    return rows;
}

void apr_canvas_content_size(const AprCanvasLayoutIn *in, SIZE *out)
{
    int rows, w, h, vw, vh;

    if (!out) return;
    out->cx = 0;
    out->cy = 0;
    if (!in) return;

    rows = layout_rows(in);

    w = 2 * in->margin + APR_COL_COUNT * in->node_w +
        (APR_COL_COUNT - 1) * in->gap_x;
    h = 2 * in->margin;
    if (rows > 0) h += rows * in->node_h + (rows - 1) * in->gap_y;

    vw = (int)(in->viewport.right - in->viewport.left);
    vh = (int)(in->viewport.bottom - in->viewport.top);
    if (w < vw) w = vw;
    if (h < vh) h = vh;

    out->cx = w;
    out->cy = h;
}

int apr_canvas_node_rect(const AprCanvasLayoutIn *in, AprCanvasColumn column,
                         int index, RECT *out)
{
    SIZE content;
    RECT bounds, r;
    int col = (int)column;

    if (!out) return 0;
    SetRectEmpty(out);
    if (!in) return 0;
    if (col < 0 || col >= APR_COL_COUNT) return 0;
    if (index < 0 || index >= in->count[col]) return 0;

    apr_canvas_content_size(in, &content);

    r.left = in->margin + col * (in->node_w + in->gap_x);
    r.right = r.left + in->node_w;
    r.top = in->margin + index * (in->node_h + in->gap_y);
    r.bottom = r.top + in->node_h;

    /* THE ONE MIRRORING SITE. Not an `if (rtl) x = w - x` here and another one
     * somewhere else -- apr_ui_mirror_rect is the product's single answer to
     * "which side is that on", and it is identity in LTR so this line is
     * unconditional. */
    bounds.left = 0;
    bounds.top = 0;
    bounds.right = content.cx;
    bounds.bottom = content.cy;
    apr_ui_mirror_rect(&r, &bounds, in->dir);

    OffsetRect(&r, -in->scroll_x, -in->scroll_y);
    *out = r;
    return 1;
}

/* ==========================================================================
 * The binding table -- what dispatches a key AND what the shortcuts screen
 * lists. One table, so the two cannot disagree.
 * ======================================================================== */

static const AprCanvasBinding k_bindings[] = {
    /* op                            vk              mods                       mir plat label */
    { APR_CANVAS_OP_NEXT_NODE,       VK_TAB,         0,                           0, 0, APR_S_UI_KEY_NEXT_NODE },
    { APR_CANVAS_OP_PREV_NODE,       VK_TAB,         APR_KMOD_SHIFT,              0, 0, APR_S_UI_KEY_PREV_NODE },
    { APR_CANVAS_OP_NEXT_IN_COLUMN,  VK_DOWN,        0,                           0, 0, APR_S_UI_KEY_NEXT_IN_COLUMN },
    { APR_CANVAS_OP_PREV_IN_COLUMN,  VK_UP,          0,                           0, 0, APR_S_UI_KEY_PREV_IN_COLUMN },

    /* The geometric pair. These two MIRROR, because they are about which way
     * the picture runs: in Arabic the arrow that points downstream is Left. */
    { APR_CANVAS_OP_DOWNSTREAM,      VK_RIGHT,       0,                           1, 0, APR_S_UI_KEY_DOWNSTREAM },
    { APR_CANVAS_OP_UPSTREAM,        VK_LEFT,        0,                           1, 0, APR_S_UI_KEY_UPSTREAM },

    /* The same two moves stated LOGICALLY, and these never mirror. Both routes
     * exist so that a screen reader user never has to know which way the
     * drawing happens to face. */
    { APR_CANVAS_OP_DOWNSTREAM,      VK_DOWN,        APR_KMOD_CTRL,               0, 0, APR_S_UI_KEY_DOWNSTREAM },
    { APR_CANVAS_OP_UPSTREAM,        VK_UP,          APR_KMOD_CTRL,               0, 0, APR_S_UI_KEY_UPSTREAM },

    { APR_CANVAS_OP_FIRST,           VK_HOME,        0,                           0, 0, APR_S_UI_KEY_FIRST },
    { APR_CANVAS_OP_LAST,            VK_END,         0,                           0, 0, APR_S_UI_KEY_LAST },

    /* platform = 1: the frame's accelerator table claims these, so they arrive
     * as WM_COMMAND through apr_canvas_command() and never as a keystroke
     * here. Listed anyway, because Help > Keyboard Shortcuts must show the key
     * the user actually presses. */
    { APR_CANVAS_OP_CONNECT,         'E',            APR_KMOD_CTRL,               0, 1, APR_S_UI_KEY_CONNECT },
    { APR_CANVAS_OP_REMOVE,          VK_DELETE,      0,                           0, 1, APR_S_UI_KEY_REMOVE },
    { APR_CANVAS_OP_ADD_SOURCE,      '1',            APR_KMOD_CTRL,               0, 1, APR_S_UI_KEY_ADD_SOURCE },
    { APR_CANVAS_OP_ADD_BUS,         '2',            APR_KMOD_CTRL,               0, 1, APR_S_UI_KEY_ADD_BUS },
    { APR_CANVAS_OP_ADD_ACTION,      '3',            APR_KMOD_CTRL,               0, 1, APR_S_UI_KEY_ADD_ACTION },

    /* Ctrl+Shift+E is NOT in the frame's accelerator table, and accelerator
     * matching is exact on modifiers, so this one does reach us as a key. */
    { APR_CANVAS_OP_DISCONNECT,      'E',            APR_KMOD_CTRL | APR_KMOD_SHIFT, 0, 0, APR_S_UI_KEY_DISCONNECT },

    { APR_CANVAS_OP_GAIN_UP,         VK_OEM_PLUS,    0,                           0, 0, APR_S_UI_KEY_GAIN_UP },
    { APR_CANVAS_OP_GAIN_UP,         VK_ADD,         0,                           0, 0, APR_S_UI_KEY_GAIN_UP },
    { APR_CANVAS_OP_GAIN_DOWN,       VK_OEM_MINUS,   0,                           0, 0, APR_S_UI_KEY_GAIN_DOWN },
    { APR_CANVAS_OP_GAIN_DOWN,       VK_SUBTRACT,    0,                           0, 0, APR_S_UI_KEY_GAIN_DOWN },

    { APR_CANVAS_OP_CANCEL,          VK_ESCAPE,      0,                           0, 0, APR_S_UI_KEY_CANCEL },
    { APR_CANVAS_OP_DESCRIBE,        VK_RETURN,      0,                           0, 0, APR_S_UI_KEY_DESCRIBE },
    { APR_CANVAS_OP_DESCRIBE,        VK_SPACE,       0,                           0, 0, APR_S_UI_KEY_DESCRIBE }
};

size_t apr_canvas_binding_count(void)
{
    return sizeof k_bindings / sizeof k_bindings[0];
}

const AprCanvasBinding *apr_canvas_binding_at(size_t index)
{
    if (index >= apr_canvas_binding_count()) return NULL;
    return &k_bindings[index];
}

static UINT mirror_vk(UINT vk, AprUiDir dir)
{
    if (dir != APR_DIR_RTL) return vk;
    if (vk == VK_LEFT)  return VK_RIGHT;
    if (vk == VK_RIGHT) return VK_LEFT;
    return vk;
}

AprCanvasOp apr_canvas_op_for_key(UINT vk, UINT mods, AprUiDir dir)
{
    size_t i, n = apr_canvas_binding_count();

    for (i = 0; i < n; ++i) {
        const AprCanvasBinding *b = &k_bindings[i];
        UINT want = b->mirrored ? mirror_vk(b->vk, dir) : b->vk;
        if (want == vk && b->mods == mods) return b->op;
    }
    return APR_CANVAS_OP_NONE;
}

UINT apr_canvas_current_mods(void)
{
    UINT m = 0;

    if (GetKeyState(VK_CONTROL) & 0x8000) m |= APR_KMOD_CTRL;
    if (GetKeyState(VK_SHIFT)   & 0x8000) m |= APR_KMOD_SHIFT;
    if (GetKeyState(VK_MENU)    & 0x8000) m |= APR_KMOD_ALT;
    return m;
}

/* ==========================================================================
 * Announcements
 * ======================================================================== */

static void say(CanvasState *st, AprStrId id, const wchar_t *const *args,
                size_t nargs)
{
    if (!st) return;

    apr_str_format(id, st->last, APR_CANVAS_TEXT_CCH, args, nargs);

    if (st->announce) st->announce(st->announce_user, st->last);

    /* A live-region change is what a screen reader picks up without focus
     * having moved. It is best-effort -- not every reader honours it on an
     * MSAA-bridged window -- which is exactly why it is never the ONLY way an
     * edit is announced: the focused node's name changes too, and that is
     * universal. */
    if (st->hwnd && IsWindow(st->hwnd)) {
        NotifyWinEvent(EVENT_OBJECT_LIVEREGIONCHANGED, st->hwnd,
                       OBJID_CLIENT, CHILDID_SELF);
    }
}

size_t apr_canvas_last_announcement(HWND canvas, wchar_t *buf, size_t cch)
{
    CanvasState *st = state_of(canvas);

    if (!buf || cch == 0) return 0;
    buf[0] = 0;
    if (!st) return 0;
    lstrcpynW(buf, st->last, (int)cch);
    return wcslen(buf);
}

void apr_canvas_set_announce(HWND canvas, AprCanvasAnnounceFn fn, void *user)
{
    CanvasState *st = state_of(canvas);

    if (!st) return;
    st->announce = fn;
    st->announce_user = user;
}

/* ==========================================================================
 * Turning user data into speech
 *
 * THE ONLY SANCTIONED WAY TO JOIN A LIST IN THIS PRODUCT. Both the separator
 * and the conjunction are catalog entries (UI_LIST_MORE, UI_LIST_PAIR), so a
 * translator owns them; a comma written into this file would be a comma in
 * Arabic too, and it is the wrong character there.
 * ======================================================================== */

static void join_names(const wchar_t *const *names, size_t n,
                       wchar_t *out, size_t cch)
{
    wchar_t a[APR_CANVAS_TEXT_CCH];
    wchar_t b[APR_CANVAS_TEXT_CCH];
    const wchar_t *args[2];
    size_t i;

    if (!out || cch == 0) return;
    out[0] = 0;
    if (n == 0) return;

    if (n == 1) {
        lstrcpynW(out, names[0], (int)cch);
        return;
    }

    lstrcpynW(a, names[0], APR_CANVAS_TEXT_CCH);
    for (i = 1; i + 1 < n; ++i) {
        args[0] = a;
        args[1] = names[i];
        apr_str_format(APR_S_UI_LIST_MORE, b, APR_CANVAS_TEXT_CCH, args, 2);
        lstrcpynW(a, b, APR_CANVAS_TEXT_CCH);
    }
    args[0] = a;
    args[1] = names[n - 1];
    apr_str_format(APR_S_UI_LIST_PAIR, out, cch, args, 2);
}

/* Linear gain to tenths of a decibel, and back. The only floating point in
 * this file, kept in one pair of functions so the rounding decision is in one
 * place. */
static int gain_to_db10(float linear)
{
    double db;

    if (!(linear > 0.0f)) return APR_CANVAS_GAIN_MIN_DB10;
    db = 20.0 * log10((double)linear);
    if (db < (double)APR_CANVAS_GAIN_MIN_DB10 / 10.0) {
        return APR_CANVAS_GAIN_MIN_DB10;
    }
    if (db > (double)APR_CANVAS_GAIN_MAX_DB10 / 10.0) {
        return APR_CANVAS_GAIN_MAX_DB10;
    }
    return (int)lround(db * 10.0);
}

static float db10_to_gain(int db10)
{
    if (db10 <= APR_CANVAS_GAIN_MIN_DB10) return 0.0f;
    return (float)pow(10.0, (double)db10 / 200.0);
}

/* ==========================================================================
 * Reading the model
 * ======================================================================== */

static const wchar_t *bus_name_of(AprGraph *g, AprBusId id)
{
    AprBus *b = apr_graph_bus(g, id);
    return b ? apr_bus_name(b) : L"";
}

static const wchar_t *source_name_of(AprGraph *g, AprSourceId id)
{
    AprSource *s = apr_graph_source(g, id);
    return s ? apr_source_name(s) : L"";
}

/* The level a source is running at, taken from the first bus it feeds. A
 * source on several buses can in principle carry a different gain on each; the
 * canvas presents ONE level per source and writes it to every edge, which is
 * the Audio Hijack model and the one a person means by "turn Teams down".
 * Per-edge levels stay possible in the model and are simply not offered here
 * yet -- said out loud rather than pretended away. */
static int source_db10(AprGraph *g, AprSourceId id, int *has_edge)
{
    AprBusId buses[APR_MAX_BUSES];
    size_t n;
    AprBus *b;

    if (has_edge) *has_edge = 0;
    if (!g) return 0;

    n = apr_graph_buses_for_source(g, id, buses, APR_MAX_BUSES);
    if (n == 0) return 0;
    b = apr_graph_bus(g, buses[0]);
    if (!b) return 0;
    if (has_edge) *has_edge = 1;
    return gain_to_db10(apr_bus_gain(b, id));
}

/* ==========================================================================
 * Naming: what a screen reader says when it lands on a node
 * ======================================================================== */

static void describe_source(CanvasState *st, const CanvasNode *n,
                            wchar_t *name, size_t name_cch,
                            wchar_t *desc, size_t desc_cch,
                            wchar_t *title, size_t title_cch,
                            wchar_t *detail, size_t detail_cch)
{
    AprBusId buses[APR_MAX_BUSES];
    const wchar_t *names[APR_MAX_BUSES];
    const wchar_t *args[2];
    wchar_t list[APR_CANVAS_TEXT_CCH];
    wchar_t db[32];
    size_t i, cnt;
    int has_edge = 0, db10;

    lstrcpynW(title, source_name_of(st->graph, n->model_id), (int)title_cch);

    cnt = apr_graph_buses_for_source(st->graph, n->model_id, buses,
                                     APR_MAX_BUSES);
    if (cnt > APR_MAX_BUSES) cnt = APR_MAX_BUSES;
    for (i = 0; i < cnt; ++i) names[i] = bus_name_of(st->graph, buses[i]);

    db10 = source_db10(st->graph, n->model_id, &has_edge);
    apr_str_number_fixed(db10, 1, db, 32);

    /* Kind is in the NAME, in words. Colour carries it too, on the canvas, but
     * colour is never allowed to be the only carrier of a fact. */
    args[0] = title;
    if (has_edge && db10 != 0) {
        args[1] = db;
        apr_str_format(APR_S_UI_NODE_SOURCE_GAIN, name, name_cch, args, 2);
        args[0] = db;
        apr_str_format(APR_S_UI_NODE_LABEL_SOURCE_GAIN, detail, detail_cch,
                       args, 1);
    } else {
        apr_str_format(APR_S_UI_NODE_SOURCE, name, name_cch, args, 1);
        lstrcpynW(detail, apr_str(APR_S_UI_NODE_LABEL_SOURCE), (int)detail_cch);
    }

    /* The DESCRIPTION carries the edges. An edge is painted on the parent and
     * has no element of its own, so this sentence is the only place it exists
     * for a screen reader user. */
    if (cnt == 0) {
        lstrcpynW(desc, apr_str(APR_S_UI_NODE_DESC_SOURCE_ALONE), (int)desc_cch);
    } else {
        join_names(names, cnt, list, APR_CANVAS_TEXT_CCH);
        args[0] = list;
        apr_str_format(APR_S_UI_NODE_DESC_SOURCE_FEEDING, desc, desc_cch,
                       args, 1);
    }
}

static size_t bus_output_names(CanvasState *st, AprBusId id,
                               const wchar_t **out, size_t cap)
{
    AprBus *b = apr_graph_bus(st->graph, id);
    size_t i, n;

    if (!b) return 0;
    n = apr_bus_action_count(b);
    if (n > cap) n = cap;
    for (i = 0; i < n; ++i) {
        const AprActionVTable *vt = apr_bus_action_at(b, i);
        out[i] = (vt && vt->display_name) ? vt->display_name : L"";
    }
    return n;
}

static void describe_bus(CanvasState *st, const CanvasNode *n,
                         wchar_t *name, size_t name_cch,
                         wchar_t *desc, size_t desc_cch,
                         wchar_t *title, size_t title_cch,
                         wchar_t *detail, size_t detail_cch)
{
    AprSourceId srcs[APR_MAX_SOURCES];
    const wchar_t *snames[APR_MAX_SOURCES];
    const wchar_t *onames[APR_MAX_ACTIONS_PER_BUS];
    const wchar_t *args[2];
    wchar_t slist[APR_CANVAS_TEXT_CCH];
    wchar_t olist[APR_CANVAS_TEXT_CCH];
    size_t i, ns, no;

    lstrcpynW(title, bus_name_of(st->graph, n->model_id), (int)title_cch);
    lstrcpynW(detail, apr_str(APR_S_UI_NODE_LABEL_BUS), (int)detail_cch);

    args[0] = title;
    apr_str_format(APR_S_UI_NODE_BUS, name, name_cch, args, 1);

    ns = apr_graph_sources_for_bus(st->graph, n->model_id, srcs,
                                   APR_MAX_SOURCES);
    if (ns > APR_MAX_SOURCES) ns = APR_MAX_SOURCES;
    for (i = 0; i < ns; ++i) snames[i] = source_name_of(st->graph, srcs[i]);

    no = bus_output_names(st, n->model_id, onames, APR_MAX_ACTIONS_PER_BUS);

    join_names(snames, ns, slist, APR_CANVAS_TEXT_CCH);
    join_names(onames, no, olist, APR_CANVAS_TEXT_CCH);

    /* Four whole sentences rather than one with holes in it. A translator
     * cannot make "Mixes nothing and writes nothing" grammatical in Arabic by
     * substituting a word for "nothing", so the empty cases are their own
     * entries. */
    if (ns > 0 && no > 0) {
        args[0] = slist;
        args[1] = olist;
        apr_str_format(APR_S_UI_NODE_DESC_BUS_FULL, desc, desc_cch, args, 2);
    } else if (ns > 0) {
        args[0] = slist;
        apr_str_format(APR_S_UI_NODE_DESC_BUS_NO_OUTPUTS, desc, desc_cch,
                       args, 1);
    } else if (no > 0) {
        args[0] = olist;
        apr_str_format(APR_S_UI_NODE_DESC_BUS_NO_SOURCES, desc, desc_cch,
                       args, 1);
    } else {
        lstrcpynW(desc, apr_str(APR_S_UI_NODE_DESC_BUS_EMPTY), (int)desc_cch);
    }
}

static void describe_action(CanvasState *st, const CanvasNode *n,
                            wchar_t *name, size_t name_cch,
                            wchar_t *desc, size_t desc_cch,
                            wchar_t *title, size_t title_cch,
                            wchar_t *detail, size_t detail_cch)
{
    const wchar_t *onames[APR_MAX_ACTIONS_PER_BUS];
    const wchar_t *args[1];
    size_t no;

    no = bus_output_names(st, n->model_id, onames, APR_MAX_ACTIONS_PER_BUS);
    if (n->sub_id >= 0 && (size_t)n->sub_id < no) {
        lstrcpynW(title, onames[n->sub_id], (int)title_cch);
    } else {
        title[0] = 0;
    }
    lstrcpynW(detail, apr_str(APR_S_UI_NODE_LABEL_ACTION), (int)detail_cch);

    args[0] = title;
    apr_str_format(APR_S_UI_NODE_ACTION, name, name_cch, args, 1);

    args[0] = bus_name_of(st->graph, n->model_id);
    apr_str_format(APR_S_UI_NODE_DESC_ACTION, desc, desc_cch, args, 1);
}

static void refresh_node(CanvasState *st, int i)
{
    wchar_t name[APR_CANVAS_TEXT_CCH];
    wchar_t desc[APR_CANVAS_TEXT_CCH];
    wchar_t title[APR_CANVAS_NAME_CCH];
    wchar_t detail[APR_CANVAS_NAME_CCH];
    CanvasNode *n;

    if (!st || i < 0 || i >= st->count) return;
    n = &st->node[i];
    if (!n->hwnd || !st->graph) return;

    name[0] = desc[0] = title[0] = detail[0] = 0;

    switch (n->kind) {
    case APR_NODE_SOURCE:
        describe_source(st, n, name, APR_CANVAS_TEXT_CCH, desc,
                        APR_CANVAS_TEXT_CCH, title, APR_CANVAS_NAME_CCH,
                        detail, APR_CANVAS_NAME_CCH);
        break;
    case APR_NODE_BUS:
        describe_bus(st, n, name, APR_CANVAS_TEXT_CCH, desc,
                     APR_CANVAS_TEXT_CCH, title, APR_CANVAS_NAME_CCH,
                     detail, APR_CANVAS_NAME_CCH);
        break;
    default:
        describe_action(st, n, name, APR_CANVAS_TEXT_CCH, desc,
                        APR_CANVAS_TEXT_CCH, title, APR_CANVAS_NAME_CCH,
                        detail, APR_CANVAS_NAME_CCH);
        break;
    }

    apr_node_set_text(n->hwnd, title, detail);
    apr_node_set_accessible(n->hwnd, name, desc);
}

static void refresh_all(CanvasState *st)
{
    int i;

    for (i = 0; i < st->count; ++i) refresh_node(st, i);
}

/* ==========================================================================
 * Geometry, scrolling and placement
 * ======================================================================== */

static void layout_input(CanvasState *st, AprCanvasLayoutIn *in)
{
    const AprThemeMetrics *m;

    memset(in, 0, sizeof *in);
    GetClientRect(st->hwnd, &in->viewport);
    in->dir = apr_ui_dir();
    memcpy(in->count, st->column_count, sizeof in->count);

    /* DEVICE PIXELS, ALREADY SCALED. Nothing here multiplies by a DPI factor,
     * which is how a call site cannot forget to. */
    m = apr_theme_metrics(st->theme);
    if (m) {
        in->node_w = m->node_w;
        in->node_h = m->node_min_h;
        in->gap_x = m->node_gap_x;
        in->gap_y = m->node_gap_y;
        in->margin = m->space_lg;
    }
    in->scroll_x = st->scroll_x;
    in->scroll_y = st->scroll_y;
}

static void update_scrollbars(CanvasState *st)
{
    AprCanvasLayoutIn in;
    SCROLLINFO si;
    SIZE content;
    int vw, vh;

    layout_input(st, &in);
    in.scroll_x = 0;
    in.scroll_y = 0;
    apr_canvas_content_size(&in, &content);

    vw = (int)(in.viewport.right - in.viewport.left);
    vh = (int)(in.viewport.bottom - in.viewport.top);

    if (st->scroll_x > content.cx - vw) st->scroll_x = content.cx - vw;
    if (st->scroll_x < 0) st->scroll_x = 0;
    if (st->scroll_y > content.cy - vh) st->scroll_y = content.cy - vh;
    if (st->scroll_y < 0) st->scroll_y = 0;

    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    /* No SIF_DISABLENOSCROLL: a permanently visible, permanently disabled
     * scroll bar is an extra element in the accessibility tree that reports
     * nothing a user can act on. It appears when it is needed and not before. */
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = content.cx > 0 ? content.cx - 1 : 0;
    si.nPage = (UINT)(vw > 0 ? vw : 1);
    si.nPos = st->scroll_x;
    SetScrollInfo(st->hwnd, SB_HORZ, &si, TRUE);

    si.nMax = content.cy > 0 ? content.cy - 1 : 0;
    si.nPage = (UINT)(vh > 0 ? vh : 1);
    si.nPos = st->scroll_y;
    SetScrollInfo(st->hwnd, SB_VERT, &si, TRUE);

    /* Only show a bar that is actually needed: an always-present scroll bar is
     * an extra element in the accessibility tree that says nothing. */
    ShowScrollBar(st->hwnd, SB_HORZ, content.cx > vw);
    ShowScrollBar(st->hwnd, SB_VERT, content.cy > vh);
}

static void place_nodes(CanvasState *st)
{
    AprCanvasLayoutIn in;
    HDWP dwp;
    int i;

    if (st->count <= 0) return;

    layout_input(st, &in);

    dwp = BeginDeferWindowPos(st->count);
    if (!dwp) return;

    for (i = 0; i < st->count; ++i) {
        RECT r;
        if (!apr_canvas_node_rect(&in, (AprCanvasColumn)st->node[i].column,
                                  st->node[i].row, &r)) {
            continue;
        }
        dwp = DeferWindowPos(dwp, st->node[i].hwnd, NULL,
                             r.left, r.top, r.right - r.left, r.bottom - r.top,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!dwp) return;
    }
    EndDeferWindowPos(dwp);
    InvalidateRect(st->hwnd, NULL, TRUE);
}

static void ensure_visible(CanvasState *st, int index)
{
    AprCanvasLayoutIn in;
    RECT r;
    int vw, vh, changed = 0;

    if (index < 0 || index >= st->count) return;

    layout_input(st, &in);
    if (!apr_canvas_node_rect(&in, (AprCanvasColumn)st->node[index].column,
                              st->node[index].row, &r)) {
        return;
    }

    vw = (int)(in.viewport.right - in.viewport.left);
    vh = (int)(in.viewport.bottom - in.viewport.top);

    if (r.left < 0)        { st->scroll_x += (int)r.left - in.margin; changed = 1; }
    else if (r.right > vw) { st->scroll_x += (int)r.right - vw + in.margin; changed = 1; }
    if (r.top < 0)          { st->scroll_y += (int)r.top - in.margin; changed = 1; }
    else if (r.bottom > vh) { st->scroll_y += (int)r.bottom - vh + in.margin; changed = 1; }

    if (changed) {
        update_scrollbars(st);
        place_nodes(st);
    }
}

/* ==========================================================================
 * Rebuilding the node windows from the model
 * ======================================================================== */

static int find_node(CanvasState *st, AprNodeKind kind, uint32_t id, int sub)
{
    int i;

    for (i = 0; i < st->count; ++i) {
        if (st->node[i].kind == kind && st->node[i].model_id == id &&
            st->node[i].sub_id == sub) {
            return i;
        }
    }
    return -1;
}

static void destroy_nodes(CanvasState *st)
{
    int i;

    for (i = 0; i < st->count; ++i) {
        if (st->node[i].hwnd && IsWindow(st->node[i].hwnd)) {
            DestroyWindow(st->node[i].hwnd);
        }
    }
    st->count = 0;
    memset(st->column_count, 0, sizeof st->column_count);
}

static void add_node(CanvasState *st, AprNodeKind kind, uint32_t id, int sub,
                     AprCanvasColumn col)
{
    CanvasNode *n;

    if (st->count >= APR_CANVAS_MAX_NODES) return;

    n = &st->node[st->count];
    n->kind = kind;
    n->model_id = id;
    n->sub_id = sub;
    n->column = (int)col;
    n->row = st->column_count[col];
    n->hwnd = apr_node_create(st->hwnd, st->theme, kind, id, sub,
                              APR_CANVAS_NODE_ID0 + st->count);
    if (!n->hwnd) return;

    st->column_count[col]++;
    st->count++;
}

void apr_canvas_rebuild(HWND canvas)
{
    CanvasState *st = state_of(canvas);
    AprNodeKind keep_kind = APR_NODE_SOURCE;
    uint32_t keep_id = 0;
    int keep_sub = 0, had_focus = 0, restore;
    size_t i, j;

    if (!st || st->rebuilding) return;
    st->rebuilding = 1;

    /* Remember where the user was, by MODEL identity rather than by index: a
     * node that survives the edit must not lose focus because something above
     * it in the list went away. */
    if (st->cur >= 0 && st->cur < st->count) {
        HWND f = GetFocus();
        had_focus = (f == st->node[st->cur].hwnd);
        keep_kind = st->node[st->cur].kind;
        keep_id = st->node[st->cur].model_id;
        keep_sub = st->node[st->cur].sub_id;
    }

    destroy_nodes(st);
    st->pending = -1;
    st->pending_is_disconnect = 0;

    if (st->graph) {
        /* LOGICAL ORDER, and it is the same order in both languages: sources,
         * then buses, then each bus's outputs. Only the geometry mirrors. */
        for (i = 0; i < apr_graph_source_count(st->graph); ++i) {
            AprSource *s = apr_graph_source_at(st->graph, i);
            if (s) add_node(st, APR_NODE_SOURCE, apr_source_id(s), 0,
                            APR_COL_SOURCE);
        }
        for (i = 0; i < apr_graph_bus_count(st->graph); ++i) {
            AprBus *b = apr_graph_bus_at(st->graph, i);
            if (b) add_node(st, APR_NODE_BUS, apr_bus_id(b), 0, APR_COL_BUS);
        }
        for (i = 0; i < apr_graph_bus_count(st->graph); ++i) {
            AprBus *b = apr_graph_bus_at(st->graph, i);
            if (!b) continue;
            for (j = 0; j < apr_bus_action_count(b); ++j) {
                add_node(st, APR_NODE_ACTION, apr_bus_id(b), (int)j,
                         APR_COL_ACTION);
            }
        }
    }

    /* Z-ORDER IS THE ACCESSIBILITY ORDER. UIA enumerates children by z-order
     * and CreateWindowEx stacks each new child on top, so without this the
     * tree reads outputs first and sources last -- backwards, in both
     * languages. */
    for (i = 0; i < (size_t)st->count; ++i) {
        SetWindowPos(st->node[i].hwnd,
                     i == 0 ? HWND_TOP : st->node[i - 1].hwnd,
                     0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    refresh_all(st);
    update_scrollbars(st);
    place_nodes(st);

    restore = find_node(st, keep_kind, keep_id, keep_sub);
    if (restore < 0 && st->count > 0) restore = 0;
    st->cur = restore;
    if (had_focus && restore >= 0) SetFocus(st->node[restore].hwnd);

    st->rebuilding = 0;
}

void apr_canvas_set_graph(HWND canvas, AprGraph *g)
{
    CanvasState *st = state_of(canvas);

    if (!st) return;
    st->graph = g;
    st->cur = -1;
    apr_canvas_rebuild(canvas);
}

AprGraph *apr_canvas_graph(HWND canvas)
{
    CanvasState *st = state_of(canvas);
    return st ? st->graph : NULL;
}

size_t apr_canvas_node_count(HWND canvas)
{
    CanvasState *st = state_of(canvas);
    return st ? (size_t)st->count : 0u;
}

HWND apr_canvas_node_at(HWND canvas, size_t index)
{
    CanvasState *st = state_of(canvas);

    if (!st || index >= (size_t)st->count) return NULL;
    return st->node[index].hwnd;
}

HWND apr_canvas_focused_node(HWND canvas)
{
    CanvasState *st = state_of(canvas);

    if (!st || st->cur < 0 || st->cur >= st->count) return NULL;
    return st->node[st->cur].hwnd;
}

HWND apr_canvas_pending_node(HWND canvas)
{
    CanvasState *st = state_of(canvas);

    if (!st || st->pending < 0 || st->pending >= st->count) return NULL;
    return st->node[st->pending].hwnd;
}

static int focus_index(CanvasState *st, int i)
{
    if (i < 0 || i >= st->count) return 0;
    st->cur = i;
    ensure_visible(st, i);
    SetFocus(st->node[i].hwnd);
    return 1;
}

int apr_canvas_focus_node(HWND canvas, size_t index)
{
    CanvasState *st = state_of(canvas);

    if (!st || index >= (size_t)st->count) return 0;
    return focus_index(st, (int)index);
}

/* ==========================================================================
 * Edge following -- the operation that makes a graph navigable by ear
 * ======================================================================== */

/* The first node downstream of `i`: for a source, a bus it feeds; for a bus,
 * one of its outputs. Returns -1 with nothing to go to. */
static int downstream_of(CanvasState *st, int i)
{
    const CanvasNode *n;

    if (i < 0 || i >= st->count || !st->graph) return -1;
    n = &st->node[i];

    if (n->kind == APR_NODE_SOURCE) {
        AprBusId buses[APR_MAX_BUSES];
        size_t cnt = apr_graph_buses_for_source(st->graph, n->model_id, buses,
                                                APR_MAX_BUSES);
        if (cnt == 0) return -1;
        return find_node(st, APR_NODE_BUS, buses[0], 0);
    }
    if (n->kind == APR_NODE_BUS) {
        return find_node(st, APR_NODE_ACTION, n->model_id, 0);
    }
    return -1;
}

static int upstream_of(CanvasState *st, int i)
{
    const CanvasNode *n;

    if (i < 0 || i >= st->count || !st->graph) return -1;
    n = &st->node[i];

    if (n->kind == APR_NODE_ACTION) {
        return find_node(st, APR_NODE_BUS, n->model_id, 0);
    }
    if (n->kind == APR_NODE_BUS) {
        AprSourceId srcs[APR_MAX_SOURCES];
        size_t cnt = apr_graph_sources_for_bus(st->graph, n->model_id, srcs,
                                               APR_MAX_SOURCES);
        if (cnt == 0) return -1;
        return find_node(st, APR_NODE_SOURCE, srcs[0], 0);
    }
    return -1;
}

static int step_in_column(CanvasState *st, int i, int delta)
{
    int col, row, j;

    if (i < 0 || i >= st->count) return -1;
    col = st->node[i].column;
    row = st->node[i].row + delta;
    for (j = 0; j < st->count; ++j) {
        if (st->node[j].column == col && st->node[j].row == row) return j;
    }
    return -1;
}

/* ==========================================================================
 * Editing the graph -- and saying so
 * ======================================================================== */

/* The node's own name -- the bus name, the source name -- as USER DATA, for
 * inserting into a catalog sentence. Never a sentence itself.
 *
 * One static buffer: this is UI-thread-only, like every window procedure in
 * this file, and one live result at a time is all any caller here needs. A
 * caller that needs two at once copies the first, and the two that do say so
 * at the copy. */
static const wchar_t *node_display_name(CanvasState *st, int i)
{
    static wchar_t buf[APR_CANVAS_NAME_CCH];

    buf[0] = 0;
    if (i < 0 || i >= st->count || !st->graph) return buf;

    switch (st->node[i].kind) {
    case APR_NODE_SOURCE:
        lstrcpynW(buf, source_name_of(st->graph, st->node[i].model_id),
                  APR_CANVAS_NAME_CCH);
        break;
    case APR_NODE_BUS:
        lstrcpynW(buf, bus_name_of(st->graph, st->node[i].model_id),
                  APR_CANVAS_NAME_CCH);
        break;
    default: {
        const wchar_t *onames[APR_MAX_ACTIONS_PER_BUS];
        size_t no = bus_output_names(st, st->node[i].model_id, onames,
                                     APR_MAX_ACTIONS_PER_BUS);
        if (st->node[i].sub_id >= 0 && (size_t)st->node[i].sub_id < no) {
            lstrcpynW(buf, onames[st->node[i].sub_id], APR_CANVAS_NAME_CCH);
        }
        break;
    }
    }
    return buf;
}

static void clear_pending(CanvasState *st)
{
    if (st->pending >= 0 && st->pending < st->count) {
        apr_node_set_pending(st->node[st->pending].hwnd, 0);
    }
    st->pending = -1;
    st->pending_is_disconnect = 0;
}

/* Both halves of the two-step edge edit. `disconnect` picks which sentence is
 * spoken and which graph call is made; everything else about the two is
 * identical, which is why they are one function and not two that drift. */
static int begin_or_finish_edge(CanvasState *st, int disconnect)
{
    const wchar_t *args[2];
    wchar_t held[APR_CANVAS_NAME_CCH];
    int src_i, bus_i;
    AprErr e;

    if (st->cur < 0 || st->cur >= st->count) {
        say(st, APR_S_UI_ANN_CANVAS_EMPTY, NULL, 0);
        return 1;
    }

    if (st->pending < 0) {
        /* First press: hold this end. Announce what is now half done and how
         * to get out of it -- a mode the user cannot hear is a trap. */
        if (st->node[st->cur].kind != APR_NODE_SOURCE) {
            /* Its own sentence, not the one the SECOND press uses. Reusing
             * "…is not a bus" here would tell a user standing on a bus that
             * the bus is not a bus, which is worse than saying nothing. */
            args[0] = node_display_name(st, st->cur);
            say(st, APR_S_UI_ANN_CONNECT_NOT_SOURCE, args, 1);
            return 1;
        }
        st->pending = st->cur;
        st->pending_is_disconnect = disconnect;
        apr_node_set_pending(st->node[st->pending].hwnd, 1);
        args[0] = node_display_name(st, st->pending);
        say(st, disconnect ? APR_S_UI_ANN_DISCONNECT_START
                           : APR_S_UI_ANN_CONNECT_START, args, 1);
        return 1;
    }

    /* Second press: the other end must be a bus. */
    src_i = st->pending;
    bus_i = st->cur;
    disconnect = st->pending_is_disconnect;

    if (st->node[bus_i].kind != APR_NODE_BUS) {
        args[0] = node_display_name(st, bus_i);
        say(st, APR_S_UI_ANN_CONNECT_REFUSED, args, 1);
        return 1;
    }

    lstrcpynW(held, node_display_name(st, src_i), APR_CANVAS_NAME_CCH);
    args[0] = held;
    args[1] = node_display_name(st, bus_i);

    if (disconnect) {
        if (!apr_graph_connected(st->graph, st->node[src_i].model_id,
                                 st->node[bus_i].model_id)) {
            say(st, APR_S_UI_ANN_DISCONNECT_NONE, args, 2);
            clear_pending(st);
            return 1;
        }
        e = apr_graph_disconnect(st->graph, st->node[src_i].model_id,
                                 st->node[bus_i].model_id);
    } else {
        if (apr_graph_connected(st->graph, st->node[src_i].model_id,
                                st->node[bus_i].model_id)) {
            say(st, APR_S_UI_ANN_CONNECT_ALREADY, args, 2);
            clear_pending(st);
            return 1;
        }
        e = apr_graph_connect(st->graph, st->node[src_i].model_id,
                              st->node[bus_i].model_id, 1.0f);
    }

    if (apr_failed(&e)) {
        APR_LOG_ERR(APR_LOG_WARN, &e);
        clear_pending(st);
        say(st, APR_S_UI_ANN_NOT_YET, NULL, 0);
        return 1;
    }

    clear_pending(st);

    /* The edge changed, so every name and description that mentions it is now
     * stale. Rewrite them BEFORE saying anything, so a reader that re-reads
     * the node gets the new sentence and not the old one. */
    refresh_all(st);
    InvalidateRect(st->hwnd, NULL, TRUE);

    args[0] = held;
    args[1] = node_display_name(st, bus_i);
    say(st, disconnect ? APR_S_UI_ANN_DISCONNECT_DONE
                       : APR_S_UI_ANN_CONNECT_DONE, args, 2);

    /* And put focus on the source, whose meaning changed most, so the change
     * is spoken by the mechanism every screen reader honours: a focus event on
     * an element whose name now says what it feeds. */
    {
        int back = find_node(st, APR_NODE_SOURCE, st->node[src_i].model_id, 0);
        if (back >= 0) focus_index(st, back);
    }
    return 1;
}

static int do_remove(CanvasState *st)
{
    const wchar_t *args[1];
    wchar_t gone[APR_CANVAS_NAME_CCH];
    AprErr e;
    int i = st->cur;

    if (i < 0 || i >= st->count || !st->graph) {
        say(st, APR_S_UI_ANN_CANVAS_EMPTY, NULL, 0);
        return 1;
    }

    if (st->node[i].kind == APR_NODE_ACTION) {
        /* bus.h has no "remove one action" and inventing one here would be a
         * second owner of the bus's action list. Refused out loud. */
        say(st, APR_S_UI_ANN_REMOVE_REFUSED, NULL, 0);
        return 1;
    }

    lstrcpynW(gone, node_display_name(st, i), APR_CANVAS_NAME_CCH);

    if (st->node[i].kind == APR_NODE_SOURCE) {
        e = apr_graph_remove_source(st->graph, st->node[i].model_id);
    } else {
        e = apr_graph_remove_bus(st->graph, st->node[i].model_id);
    }
    if (apr_failed(&e)) {
        APR_LOG_ERR(APR_LOG_WARN, &e);
        say(st, APR_S_UI_ANN_NOT_YET, NULL, 0);
        return 1;
    }

    st->cur = -1;
    apr_canvas_rebuild(st->hwnd);

    args[0] = gone;
    say(st, APR_S_UI_ANN_REMOVED, args, 1);
    if (st->count > 0) focus_index(st, st->cur >= 0 ? st->cur : 0);
    return 1;
}

static int do_gain(CanvasState *st, int step_db10)
{
    AprBusId buses[APR_MAX_BUSES];
    const wchar_t *args[2];
    wchar_t db[32];
    wchar_t who[APR_CANVAS_NAME_CCH];
    size_t cnt, k;
    int i = st->cur, db10, has_edge = 0;
    float linear;

    if (i < 0 || i >= st->count || !st->graph) {
        say(st, APR_S_UI_ANN_CANVAS_EMPTY, NULL, 0);
        return 1;
    }
    if (st->node[i].kind != APR_NODE_SOURCE) {
        /* "Main Mix feeds no bus" would be a lie about a bus. Two states, two
         * sentences. */
        args[0] = node_display_name(st, i);
        say(st, APR_S_UI_ANN_GAIN_NOT_SOURCE, args, 1);
        return 1;
    }

    cnt = apr_graph_buses_for_source(st->graph, st->node[i].model_id, buses,
                                     APR_MAX_BUSES);
    if (cnt == 0) {
        args[0] = node_display_name(st, i);
        say(st, APR_S_UI_ANN_GAIN_NO_EDGE, args, 1);
        return 1;
    }

    db10 = source_db10(st->graph, st->node[i].model_id, &has_edge) + step_db10;
    if (db10 < APR_CANVAS_GAIN_MIN_DB10) db10 = APR_CANVAS_GAIN_MIN_DB10;
    if (db10 > APR_CANVAS_GAIN_MAX_DB10) db10 = APR_CANVAS_GAIN_MAX_DB10;
    linear = db10_to_gain(db10);

    for (k = 0; k < cnt && k < APR_MAX_BUSES; ++k) {
        AprBus *b = apr_graph_bus(st->graph, buses[k]);
        if (b) {
            AprErr e = apr_bus_set_gain(b, st->node[i].model_id, linear);
            if (apr_failed(&e)) APR_LOG_ERR(APR_LOG_WARN, &e);
        }
    }

    lstrcpynW(who, node_display_name(st, i), APR_CANVAS_NAME_CCH);
    apr_str_number_fixed(db10, 1, db, 32);

    refresh_node(st, i);
    args[0] = who;
    args[1] = db;
    say(st, APR_S_UI_ANN_GAIN, args, 2);
    return 1;
}

/* ==========================================================================
 * Dispatch
 * ======================================================================== */

int apr_canvas_perform(HWND canvas, AprCanvasOp op)
{
    CanvasState *st = state_of(canvas);
    int target;

    if (!st) return 0;

    if (st->count == 0) {
        switch (op) {
        case APR_CANVAS_OP_ADD_SOURCE:
        case APR_CANVAS_OP_ADD_BUS:
        case APR_CANVAS_OP_ADD_ACTION:
            say(st, APR_S_UI_ANN_NOT_YET, NULL, 0);
            return 1;
        case APR_CANVAS_OP_NONE:
            return 0;
        default:
            say(st, APR_S_UI_ANN_CANVAS_EMPTY, NULL, 0);
            return 1;
        }
    }

    if (st->cur < 0) st->cur = 0;

    switch (op) {
    case APR_CANVAS_OP_NEXT_NODE:
    case APR_CANVAS_OP_PREV_NODE: {
        int fwd = (op == APR_CANVAS_OP_NEXT_NODE);
        target = st->cur + (fwd ? 1 : -1);
        if (target >= 0 && target < st->count) return focus_index(st, target);
        /* At either end focus LEAVES the canvas, which is the whole reason
         * claiming every key here is not a focus trap. */
        {
            HWND frame = GetParent(canvas);
            HWND next = frame ? GetNextDlgTabItem(frame, canvas,
                                                  fwd ? FALSE : TRUE) : NULL;
            if (next && next != canvas) SetFocus(next);
        }
        return 1;
    }

    case APR_CANVAS_OP_NEXT_IN_COLUMN:
        target = step_in_column(st, st->cur, 1);
        if (target >= 0) return focus_index(st, target);
        return 1;

    case APR_CANVAS_OP_PREV_IN_COLUMN:
        target = step_in_column(st, st->cur, -1);
        if (target >= 0) return focus_index(st, target);
        return 1;

    case APR_CANVAS_OP_DOWNSTREAM:
        target = downstream_of(st, st->cur);
        if (target >= 0) return focus_index(st, target);
        {
            const wchar_t *a[1];
            a[0] = node_display_name(st, st->cur);
            say(st, APR_S_UI_ANN_NO_EDGE_DOWN, a, 1);
        }
        return 1;

    case APR_CANVAS_OP_UPSTREAM:
        target = upstream_of(st, st->cur);
        if (target >= 0) return focus_index(st, target);
        {
            const wchar_t *a[1];
            a[0] = node_display_name(st, st->cur);
            say(st, APR_S_UI_ANN_NO_EDGE_UP, a, 1);
        }
        return 1;

    case APR_CANVAS_OP_FIRST:
        return focus_index(st, 0);

    case APR_CANVAS_OP_LAST:
        return focus_index(st, st->count - 1);

    case APR_CANVAS_OP_CONNECT:
        return begin_or_finish_edge(st, 0);

    case APR_CANVAS_OP_DISCONNECT:
        return begin_or_finish_edge(st, 1);

    case APR_CANVAS_OP_REMOVE:
        return do_remove(st);

    case APR_CANVAS_OP_GAIN_UP:
        return do_gain(st, APR_CANVAS_GAIN_STEP_DB10);

    case APR_CANVAS_OP_GAIN_DOWN:
        return do_gain(st, -APR_CANVAS_GAIN_STEP_DB10);

    case APR_CANVAS_OP_CANCEL:
        if (st->pending >= 0) {
            clear_pending(st);
            say(st, APR_S_UI_ANN_CANCELLED, NULL, 0);
        }
        return 1;

    case APR_CANVAS_OP_DESCRIBE:
        refresh_node(st, st->cur);
        apr_node_announce_self(st->node[st->cur].hwnd);
        return 1;

    case APR_CANVAS_OP_ADD_SOURCE:
    case APR_CANVAS_OP_ADD_BUS:
    case APR_CANVAS_OP_ADD_ACTION:
        /* The keyboard path exists and is on the menu; the chooser dialog does
         * not exist yet. Saying so is the spoken form of "greyed, not
         * absent". */
        say(st, APR_S_UI_ANN_NOT_YET, NULL, 0);
        return 1;

    default:
        break;
    }
    return 0;
}

int apr_canvas_command(HWND canvas, int command_id)
{
    AprCanvasOp op;

    switch (command_id) {
    case APR_CMD_CONNECT:    op = APR_CANVAS_OP_CONNECT;    break;
    case APR_CMD_REMOVE:     op = APR_CANVAS_OP_REMOVE;     break;
    case APR_CMD_ADD_SOURCE: op = APR_CANVAS_OP_ADD_SOURCE; break;
    case APR_CMD_ADD_BUS:    op = APR_CANVAS_OP_ADD_BUS;    break;
    case APR_CMD_ADD_ACTION: op = APR_CANVAS_OP_ADD_ACTION; break;
    default:                 return 0;
    }
    return apr_canvas_perform(canvas, op);
}

/* ==========================================================================
 * Painting the edges
 *
 * Edges are drawn on the PARENT, in the gaps between the node windows, and
 * WS_CLIPCHILDREN keeps them out of the nodes themselves. They are the one
 * thing on this canvas with no window of its own, which is why every one of
 * them is also written into the nodes' descriptions.
 * ======================================================================== */

static void edge_endpoints(const RECT *a, const RECT *b, POINT *pa, POINT *pb)
{
    /* Direction-agnostic on purpose: the rectangles have ALREADY been mirrored
     * by apr_canvas_node_rect, so asking which one is further along the x axis
     * is the same question in both languages. */
    if (a->right <= b->left) {
        pa->x = a->right;
        pb->x = b->left;
    } else {
        pa->x = a->left;
        pb->x = b->right;
    }
    pa->y = (a->top + a->bottom) / 2;
    pb->y = (b->top + b->bottom) / 2;
}

static void draw_edge(CanvasState *st, HDC dc, const RECT *a, const RECT *b,
                      int active)
{
    const AprThemePalette *c = apr_theme_palette(st->theme);
    const AprThemeMetrics *m = apr_theme_metrics(st->theme);
    POINT pts[4];
    HPEN pen, old;
    int dx;

    if (!c || !m) return;

    edge_endpoints(a, b, &pts[0], &pts[3]);
    dx = (pts[3].x - pts[0].x) / 2;
    pts[1].x = pts[0].x + dx;
    pts[1].y = pts[0].y;
    pts[2].x = pts[3].x - dx;
    pts[2].y = pts[3].y;

    pen = apr_theme_pen(st->theme, active ? c->edge_active : c->edge,
                        m->edge_w);
    if (!pen) return;
    old = (HPEN)SelectObject(dc, pen);
    PolyBezier(dc, pts, 4);
    SelectObject(dc, old);
}

static int node_rect_of(CanvasState *st, const AprCanvasLayoutIn *in, int i,
                        RECT *out)
{
    if (i < 0 || i >= st->count) return 0;
    return apr_canvas_node_rect(in, (AprCanvasColumn)st->node[i].column,
                                st->node[i].row, out);
}

static void canvas_paint(CanvasState *st, HDC dc)
{
    const AprThemePalette *c;
    AprCanvasLayoutIn in;
    RECT rc, ra, rb;
    int i, j;

    GetClientRect(st->hwnd, &rc);

    if (!st->theme) {
        FillRect(dc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
        return;
    }
    c = apr_theme_palette(st->theme);
    FillRect(dc, &rc, apr_theme_brush(st->theme, c->window_bg));

    if (!st->graph || st->count == 0) return;

    layout_input(st, &in);

    for (i = 0; i < st->count; ++i) {
        if (st->node[i].kind == APR_NODE_SOURCE) {
            AprBusId buses[APR_MAX_BUSES];
            size_t k, n = apr_graph_buses_for_source(st->graph,
                                                     st->node[i].model_id,
                                                     buses, APR_MAX_BUSES);
            if (!node_rect_of(st, &in, i, &ra)) continue;
            for (k = 0; k < n && k < APR_MAX_BUSES; ++k) {
                j = find_node(st, APR_NODE_BUS, buses[k], 0);
                if (j < 0 || !node_rect_of(st, &in, j, &rb)) continue;
                draw_edge(st, dc, &ra, &rb, i == st->cur || j == st->cur);
            }
        } else if (st->node[i].kind == APR_NODE_ACTION) {
            j = find_node(st, APR_NODE_BUS, st->node[i].model_id, 0);
            if (j < 0) continue;
            if (!node_rect_of(st, &in, j, &ra)) continue;
            if (!node_rect_of(st, &in, i, &rb)) continue;
            draw_edge(st, dc, &ra, &rb, i == st->cur || j == st->cur);
        }
    }
}

/* ==========================================================================
 * Window procedure
 * ======================================================================== */

static void on_scroll(CanvasState *st, int bar, WPARAM wp)
{
    SCROLLINFO si;
    int pos, page, line;

    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_ALL;
    if (!GetScrollInfo(st->hwnd, bar, &si)) return;

    pos = si.nPos;
    page = (int)si.nPage;
    line = page / 8 > 0 ? page / 8 : 1;

    switch (LOWORD(wp)) {
    case SB_LINEUP:    pos -= line; break;
    case SB_LINEDOWN:  pos += line; break;
    case SB_PAGEUP:    pos -= page; break;
    case SB_PAGEDOWN:  pos += page; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: pos = si.nTrackPos; break;
    case SB_TOP:       pos = si.nMin; break;
    case SB_BOTTOM:    pos = si.nMax; break;
    default: return;
    }

    if (bar == SB_HORZ) st->scroll_x = pos;
    else                st->scroll_y = pos;

    update_scrollbars(st);
    place_nodes(st);
}

static LRESULT CALLBACK canvas_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CanvasState *st = (CanvasState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        CanvasState *s = (CanvasState *)calloc(1, sizeof *s);
        if (!s) return FALSE;
        s->hwnd = hwnd;
        s->theme = (AprTheme *)cs->lpCreateParams;
        s->cur = -1;
        s->pending = -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_NCDESTROY:
        if (st) {
            destroy_nodes(st);
            free(st);
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_GETDLGCODE:
        /* An EMPTY canvas is still a place the user can stand, so it takes the
         * arrow keys itself. Never DLGC_WANTTAB here: with no nodes, Tab has to
         * leave. */
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_SETFOCUS:
        /* A container hands focus on to its content. With no content it keeps
         * it, which is what keeps the pane named and focusable in an empty
         * session -- the property tests/test_ui_a11y.c asserts. */
        if (st && st->count > 0) {
            focus_index(st, st->cur >= 0 ? st->cur : 0);
            return 0;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_KEYDOWN:
        if (st) {
            AprCanvasOp op = apr_canvas_op_for_key((UINT)wp,
                                                   apr_canvas_current_mods(),
                                                   apr_ui_dir());
            if (op != APR_CANVAS_OP_NONE && apr_canvas_perform(hwnd, op)) {
                return 0;
            }
        }
        break;

    /* The message forms. See ui_canvas.h: SetFocus only works on the thread
     * that owns the window, so a caller from another thread has to arrive
     * through SendMessage or get nothing, silently. */
    case APR_CANVAS_WM_PERFORM:
        return apr_canvas_perform(hwnd, (AprCanvasOp)wp);

    case APR_CANVAS_WM_FOCUS_NODE:
        return apr_canvas_focus_node(hwnd, (size_t)wp);

    case APR_CANVAS_WM_SET_GRAPH:
        apr_canvas_set_graph(hwnd, (AprGraph *)lp);
        return 0;

    case WM_COMMAND:
        /* A node telling us it took focus -- by Tab, or by a click. Keeping
         * `cur` true here is what stops the keyboard model and the platform's
         * own focus movement from disagreeing. */
        if (st && HIWORD(wp) == BN_SETFOCUS) {
            int i;
            for (i = 0; i < st->count; ++i) {
                if (st->node[i].hwnd == (HWND)lp) {
                    st->cur = i;
                    ensure_visible(st, i);
                    InvalidateRect(hwnd, NULL, FALSE);
                    break;
                }
            }
            return 0;
        }
        break;

    case WM_PARENTNOTIFY:
        /* The mouse half of connect. A click completes a pending edge rather
         * than starting a second gesture of its own: one model, one set of
         * announcements, and a sighted user and a keyboard user get the same
         * behaviour out of it. */
        if (st && LOWORD(wp) == WM_LBUTTONDOWN && st->pending >= 0) {
            int i;
            for (i = 0; i < st->count; ++i) {
                if (st->node[i].hwnd == (HWND)lp) {
                    st->cur = i;
                    begin_or_finish_edge(st, st->pending_is_disconnect);
                    return 0;
                }
            }
        }
        break;

    case WM_MOUSEWHEEL:
        if (st) {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            st->scroll_y -= delta;
            update_scrollbars(st);
            place_nodes(st);
            return 0;
        }
        break;

    case WM_HSCROLL:
        if (st) { on_scroll(st, SB_HORZ, wp); return 0; }
        break;

    case WM_VSCROLL:
        if (st) { on_scroll(st, SB_VERT, wp); return 0; }
        break;

    case WM_SIZE:
        if (st) {
            update_scrollbars(st);
            place_nodes(st);
        }
        break;

    case WM_ERASEBKGND:
        return 1;   /* WM_PAINT covers the whole client area */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (st) canvas_paint(st, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==========================================================================
 * Creation -- the entry point app.c calls
 * ======================================================================== */

HWND apr_canvas_create(HWND parent, AprTheme *theme)
{
    static int registered;
    HINSTANCE inst;
    HWND h;

    if (!parent || !IsWindow(parent)) return NULL;
    inst = (HINSTANCE)(LONG_PTR)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    if (!inst) inst = GetModuleHandleW(NULL);

    if (!registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = canvas_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = APR_CANVAS_CLASS;
        if (!RegisterClassExW(&wc)) {
            APR_WARN(L"RegisterClassExW(canvas) failed (%lu); the frame will "
                     L"fall back to its placeholder pane",
                     (unsigned long)GetLastError());
            return NULL;
        }
        registered = 1;
    }
    if (!apr_node_register_class(inst)) return NULL;

    /* NOT WS_EX_LAYOUTRTL, and never: this is a window we paint. Mirroring
     * happens in apr_canvas_node_rect and nowhere else.
     *
     * WS_CLIPCHILDREN so the edges we paint stop at the node windows rather
     * than being drawn over by them a moment later. */
    h = CreateWindowExW(
        0, APR_CANVAS_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 10, 10, parent, NULL, inst, theme);
    if (!h) {
        APR_WARN(L"CreateWindowExW(canvas) failed (%lu)",
                 (unsigned long)GetLastError());
        return NULL;
    }

    /* ui_app.h: the frame does not name us, because only we know whether a
     * sub-control should carry the name instead. */
    apr_ui_set_accessible_name(h, APR_S_UI_PANE_CANVAS);
    apr_ui_set_accessible_description(h, APR_S_UI_DESC_CANVAS);
    /* Belt as well as braces, the same way app.c does it for its own classes:
     * with no annotation service the window text is still a name. */
    SetWindowTextW(h, apr_str(APR_S_UI_PANE_CANVAS));

    return h;
}
