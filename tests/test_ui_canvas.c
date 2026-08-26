/*
 * test_ui_canvas.c -- the canvas, tested the way a screen reader sees it.
 *
 * ===========================================================================
 * TWO KINDS OF TEST IN HERE, AND THE SPLIT IS DELIBERATE
 *
 *   1. PURE. apr_canvas_node_rect() and the binding table take direction as an
 *      argument and read no global, so LTR and RTL geometry are both checked
 *      in this one process, with no second run and no environment to set up.
 *      That is the entire reason those two are shaped the way they are: a
 *      mirroring bug that only appears in an Arabic build is a bug nobody
 *      here would see.
 *
 *   2. LIVE. A real frame on a real UI thread, and a real UI Automation client
 *      on another -- two threads on purpose, because a UIA client querying a
 *      window on the STA that owns it can deadlock. What UIA reports IS what a
 *      screen reader gets; anything short of asking it is a proxy.
 *
 * ===========================================================================
 * WHAT IS ASSERTED
 *
 *   - Every node window exposes a non-empty Name and the control type we meant
 *     (a Group, not a nameless Pane).
 *   - EDGES ARE DISCOVERABLE. An edge has no window, so it has to be in the
 *     nodes' text and reachable by key. Both are checked: the description
 *     names the bus, and "go to what this feeds" really lands on it.
 *   - TAB ORDER FOLLOWS LOGICAL ORDER IN BOTH DIRECTIONS -- and the live
 *     Z-ORDER chain is what is walked, not our own array, because z-order is
 *     what UIA enumerates.
 *   - Every operation has a keyboard path and a catalog label, which is how
 *     "no mouse-only paths" stops being a promise.
 *   - Every graph edit announces, and the announcement is a formatted catalog
 *     sentence rather than something assembled in C.
 *
 * ===========================================================================
 * WHY THE OPERATIONS ARE DRIVEN BY SendMessage
 *
 *   SetFocus does nothing, silently, when called from a thread that does not
 *   own the window. Calling apr_canvas_perform() from the test thread would
 *   therefore "pass" while moving no focus at all. The APR_CANVAS_WM_* messages
 *   marshal onto the UI thread, so these tests drive exactly the code path a
 *   key press drives.
 */
#include "test_runner.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <initguid.h>
#include <uiautomation.h>

#include "graph.h"
#include "strings.h"
#include "ui_app.h"
#include "ui_canvas.h"
#include "ui_node.h"

#define RATE 48000u

static const CLSID kCLSID_CUIAutomation =
    { 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
static const IID kIID_IUIAutomation =
    { 0x30cbe57d, 0xd9d0, 0x452a, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };

/* ==========================================================================
 * PURE: geometry
 * ======================================================================== */

static AprCanvasLayoutIn plan(AprUiDir dir, int sources, int buses, int actions)
{
    AprCanvasLayoutIn in;

    memset(&in, 0, sizeof in);
    SetRect(&in.viewport, 0, 0, 900, 600);
    in.dir = dir;
    in.count[APR_COL_SOURCE] = sources;
    in.count[APR_COL_BUS] = buses;
    in.count[APR_COL_ACTION] = actions;
    in.node_w = 200;
    in.node_h = 80;
    in.gap_x = 60;
    in.gap_y = 20;
    in.margin = 24;
    return in;
}

TEST(signal_flow_runs_left_to_right_in_english_and_right_to_left_in_arabic)
{
    AprCanvasLayoutIn ltr = plan(APR_DIR_LTR, 2, 1, 1);
    AprCanvasLayoutIn rtl = plan(APR_DIR_RTL, 2, 1, 1);
    RECT s, b, a;

    /* THE PROPERTY THE SPEC CALLS A REVIEW FAILURE TO GET WRONG. Sources feed
     * buses feed outputs, and the picture must read that way in the reading
     * direction of the language -- which is the opposite way round in
     * Arabic. */
    ASSERT_TRUE(apr_canvas_node_rect(&ltr, APR_COL_SOURCE, 0, &s));
    ASSERT_TRUE(apr_canvas_node_rect(&ltr, APR_COL_BUS, 0, &b));
    ASSERT_TRUE(apr_canvas_node_rect(&ltr, APR_COL_ACTION, 0, &a));
    printf("      LTR x: source %ld bus %ld output %ld\n", s.left, b.left, a.left);
    ASSERT_GT_INT((int)s.left, (int)b.left);
    ASSERT_GT_INT((int)b.left, (int)a.left);

    ASSERT_TRUE(apr_canvas_node_rect(&rtl, APR_COL_SOURCE, 0, &s));
    ASSERT_TRUE(apr_canvas_node_rect(&rtl, APR_COL_BUS, 0, &b));
    ASSERT_TRUE(apr_canvas_node_rect(&rtl, APR_COL_ACTION, 0, &a));
    printf("      RTL x: source %ld bus %ld output %ld\n", s.left, b.left, a.left);
    ASSERT_LT_INT((int)s.left, (int)b.left);
    ASSERT_LT_INT((int)b.left, (int)a.left);
}

TEST(mirroring_is_an_exact_reflection_and_nothing_else)
{
    AprCanvasLayoutIn ltr = plan(APR_DIR_LTR, 3, 2, 2);
    AprCanvasLayoutIn rtl = plan(APR_DIR_RTL, 3, 2, 2);
    SIZE size_l, size_r;
    int col, i;

    apr_canvas_content_size(&ltr, &size_l);
    apr_canvas_content_size(&rtl, &size_r);

    /* Direction changes where things are, never how big anything is. */
    ASSERT_EQ_INT((int)size_l.cx, (int)size_r.cx);
    ASSERT_EQ_INT((int)size_l.cy, (int)size_r.cy);

    for (col = 0; col < APR_COL_COUNT; ++col) {
        for (i = 0; i < ltr.count[col]; ++i) {
            RECT l, r;
            ASSERT_TRUE(apr_canvas_node_rect(&ltr, (AprCanvasColumn)col, i, &l));
            ASSERT_TRUE(apr_canvas_node_rect(&rtl, (AprCanvasColumn)col, i, &r));

            /* Reflected about the content's vertical centre line... */
            ASSERT_EQ_INT((int)(size_l.cx - l.right), (int)r.left);
            ASSERT_EQ_INT((int)(l.right - l.left), (int)(r.right - r.left));

            /* ...and the vertical axis untouched. Bottom does not become top
             * in any language. */
            ASSERT_EQ_INT((int)l.top, (int)r.top);
            ASSERT_EQ_INT((int)l.bottom, (int)r.bottom);
        }
    }
}

TEST(the_three_lanes_are_three_even_when_one_is_empty)
{
    AprCanvasLayoutIn full = plan(APR_DIR_LTR, 2, 2, 2);
    AprCanvasLayoutIn none = plan(APR_DIR_LTR, 2, 2, 0);
    RECT a, b;
    SIZE sf, sn;

    /* A lane that collapsed when empty would slide every other node sideways
     * while the user built the graph. It does not. */
    ASSERT_TRUE(apr_canvas_node_rect(&full, APR_COL_BUS, 0, &a));
    ASSERT_TRUE(apr_canvas_node_rect(&none, APR_COL_BUS, 0, &b));
    ASSERT_EQ_INT((int)a.left, (int)b.left);

    apr_canvas_content_size(&full, &sf);
    apr_canvas_content_size(&none, &sn);
    ASSERT_EQ_INT((int)sf.cx, (int)sn.cx);
}

TEST(an_index_outside_a_lane_yields_no_rectangle_rather_than_a_wrong_one)
{
    AprCanvasLayoutIn in = plan(APR_DIR_LTR, 1, 0, 0);
    RECT r;

    ASSERT_FALSE(apr_canvas_node_rect(&in, APR_COL_SOURCE, 1, &r));
    ASSERT_FALSE(apr_canvas_node_rect(&in, APR_COL_BUS, 0, &r));
    ASSERT_FALSE(apr_canvas_node_rect(&in, (AprCanvasColumn)7, 0, &r));
    ASSERT_TRUE(IsRectEmpty(&r) != FALSE);
}

TEST(content_is_never_smaller_than_the_viewport)
{
    AprCanvasLayoutIn in = plan(APR_DIR_LTR, 0, 0, 0);
    SIZE s;

    apr_canvas_content_size(&in, &s);
    ASSERT_GE_INT((int)(in.viewport.right - in.viewport.left), (int)s.cx);
    ASSERT_GE_INT((int)(in.viewport.bottom - in.viewport.top), (int)s.cy);
}

/* ==========================================================================
 * PURE: the keyboard model
 * ======================================================================== */

static const AprCanvasBinding *binding_for(AprCanvasOp op)
{
    size_t i, n = apr_canvas_binding_count();

    for (i = 0; i < n; ++i) {
        const AprCanvasBinding *b = apr_canvas_binding_at(i);
        if (b && b->op == op) return b;
    }
    return NULL;
}

TEST(every_operation_has_a_keyboard_path_and_a_name_a_user_can_read)
{
    int op;

    /* AGENTS.md rule 5: every operation reachable by keyboard, no mouse-only
     * paths, ever. This is that rule as an assertion rather than a review
     * item -- an operation added without a binding fails the build. */
    for (op = APR_CANVAS_OP_NONE + 1; op < APR_CANVAS_OP_COUNT; ++op) {
        const AprCanvasBinding *b = binding_for((AprCanvasOp)op);
        if (!b) printf("      operation %d has no key\n", op);
        ASSERT_NOT_NULL(b);
        ASSERT_TRUE(b->label != APR_S__NONE);

        /* And a label out of the catalog, not a literal: apr_str never returns
         * empty, and returns a loud placeholder for an id with no text. */
        ASSERT_TRUE(apr_str(b->label)[0] != 0);
        ASSERT_NULL(wcsstr(apr_str(b->label), L"???"));
    }
}

TEST(only_the_two_geometric_arrows_mirror)
{
    /* Left and Right are about which way the PICTURE runs, so they swap. */
    ASSERT_EQ_INT(APR_CANVAS_OP_DOWNSTREAM,
                  (int)apr_canvas_op_for_key(VK_RIGHT, 0, APR_DIR_LTR));
    ASSERT_EQ_INT(APR_CANVAS_OP_UPSTREAM,
                  (int)apr_canvas_op_for_key(VK_LEFT, 0, APR_DIR_LTR));
    ASSERT_EQ_INT(APR_CANVAS_OP_DOWNSTREAM,
                  (int)apr_canvas_op_for_key(VK_LEFT, 0, APR_DIR_RTL));
    ASSERT_EQ_INT(APR_CANVAS_OP_UPSTREAM,
                  (int)apr_canvas_op_for_key(VK_RIGHT, 0, APR_DIR_RTL));

    /* The LOGICAL statement of the same two moves does not swap, and that is
     * the route a screen reader user takes: "go to what this feeds" means the
     * same thing whichever way the drawing happens to face. */
    ASSERT_EQ_INT(APR_CANVAS_OP_DOWNSTREAM,
                  (int)apr_canvas_op_for_key(VK_DOWN, APR_KMOD_CTRL, APR_DIR_LTR));
    ASSERT_EQ_INT(APR_CANVAS_OP_DOWNSTREAM,
                  (int)apr_canvas_op_for_key(VK_DOWN, APR_KMOD_CTRL, APR_DIR_RTL));
    ASSERT_EQ_INT(APR_CANVAS_OP_UPSTREAM,
                  (int)apr_canvas_op_for_key(VK_UP, APR_KMOD_CTRL, APR_DIR_RTL));

    /* And nothing else does. Tab, the column arrows, Home/End and every editing
     * key mean the same key in both languages. */
    ASSERT_EQ_INT(APR_CANVAS_OP_NEXT_NODE,
                  (int)apr_canvas_op_for_key(VK_TAB, 0, APR_DIR_RTL));
    ASSERT_EQ_INT(APR_CANVAS_OP_PREV_NODE,
                  (int)apr_canvas_op_for_key(VK_TAB, APR_KMOD_SHIFT, APR_DIR_RTL));
    ASSERT_EQ_INT(APR_CANVAS_OP_NEXT_IN_COLUMN,
                  (int)apr_canvas_op_for_key(VK_DOWN, 0, APR_DIR_RTL));
    ASSERT_EQ_INT(APR_CANVAS_OP_CONNECT,
                  (int)apr_canvas_op_for_key('E', APR_KMOD_CTRL, APR_DIR_RTL));
    ASSERT_EQ_INT(APR_CANVAS_OP_DISCONNECT,
                  (int)apr_canvas_op_for_key('E', APR_KMOD_CTRL | APR_KMOD_SHIFT,
                                             APR_DIR_RTL));
}

TEST(no_key_means_two_different_things_in_either_direction)
{
    int d;

    for (d = 0; d < 2; ++d) {
        AprUiDir dir = d ? APR_DIR_RTL : APR_DIR_LTR;
        size_t i, j, n = apr_canvas_binding_count();

        for (i = 0; i < n; ++i) {
            const AprCanvasBinding *a = apr_canvas_binding_at(i);
            for (j = i + 1; j < n; ++j) {
                const AprCanvasBinding *b = apr_canvas_binding_at(j);
                UINT va = a->mirrored && dir == APR_DIR_RTL
                            ? (a->vk == VK_LEFT ? VK_RIGHT
                               : a->vk == VK_RIGHT ? VK_LEFT : a->vk)
                            : a->vk;
                UINT vb = b->mirrored && dir == APR_DIR_RTL
                            ? (b->vk == VK_LEFT ? VK_RIGHT
                               : b->vk == VK_RIGHT ? VK_LEFT : b->vk)
                            : b->vk;
                if (va == vb && a->mods == b->mods && a->op != b->op) {
                    printf("      vk %u collides in dir %d\n", va, d);
                }
                ASSERT_FALSE(va == vb && a->mods == b->mods && a->op != b->op);
            }
        }
    }
}

TEST(the_frames_own_accelerators_are_the_ones_marked_as_the_frames)
{
    /* Ctrl+E, Delete and Ctrl+1..3 are in the frame's accelerator table, so
     * TranslateAccelerator turns them into WM_COMMAND before any window sees
     * the keystroke. A binding that claimed to handle them here would be a lie
     * a user discovers by pressing the key. */
    size_t i, n = apr_canvas_binding_count();

    for (i = 0; i < n; ++i) {
        const AprCanvasBinding *b = apr_canvas_binding_at(i);
        switch (b->op) {
        case APR_CANVAS_OP_CONNECT:
        case APR_CANVAS_OP_REMOVE:
        case APR_CANVAS_OP_ADD_SOURCE:
        case APR_CANVAS_OP_ADD_BUS:
        case APR_CANVAS_OP_ADD_ACTION:
            ASSERT_EQ_INT(1, b->platform);
            break;
        default:
            ASSERT_EQ_INT(0, b->platform);
            break;
        }
    }
}

/* ==========================================================================
 * LIVE: the UI thread, the model, and a UIA client
 * ======================================================================== */

typedef struct UiHost {
    HANDLE    thread;
    HANDLE    ready;
    AprUiApp *app;
    HWND      frame;
    HWND      canvas;
    int       failed;
} UiHost;

static DWORD WINAPI ui_thread(LPVOID param)
{
    UiHost *h = (UiHost *)param;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    AprErr e;
    MSG msg;

    e = apr_ui_app_create(GetModuleHandleW(NULL), &h->app);
    if (apr_failed(&e)) {
        h->failed = 1;
        SetEvent(h->ready);
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }
    h->frame = apr_ui_app_hwnd(h->app);
    h->canvas = apr_ui_app_pane(h->app, APR_PANE_CANVAS);
    apr_ui_app_show(h->app, SW_SHOWNORMAL);

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    SetEvent(h->ready);

    apr_ui_app_run(h->app);
    apr_ui_app_destroy(h->app);
    h->app = NULL;
    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}

static int ui_start(UiHost *h)
{
    memset(h, 0, sizeof *h);
    h->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!h->ready) return 0;
    h->thread = CreateThread(NULL, 0, ui_thread, h, 0, NULL);
    if (!h->thread) { CloseHandle(h->ready); h->ready = NULL; return 0; }
    if (WaitForSingleObject(h->ready, 15000) != WAIT_OBJECT_0) return 0;
    return h->failed ? 0 : (h->frame != NULL && h->canvas != NULL);
}

static void ui_stop(UiHost *h)
{
    if (h->frame && IsWindow(h->frame)) PostMessageW(h->frame, WM_CLOSE, 0, 0);
    if (h->thread) {
        if (WaitForSingleObject(h->thread, 15000) != WAIT_OBJECT_0) {
            printf("      WARNING: UI thread did not exit; terminating\n");
            TerminateThread(h->thread, 1);
        }
        CloseHandle(h->thread);
    }
    if (h->ready) CloseHandle(h->ready);
    memset(h, 0, sizeof *h);
}

/* --- the model under the canvas ------------------------------------------ */

typedef struct Model {
    AprGraph   *g;
    AprSourceId teams;
    AprSourceId mic;
    AprBusId    mix;
    AprBusId    voice;
} Model;

static int build_model(Model *m)
{
    AprCaptureConfig c;
    AprActionConfig  cfg;
    AprErr e;

    memset(m, 0, sizeof *m);
    e = apr_graph_create(RATE, 2, &m->g);
    if (apr_failed(&e)) return 0;

    memset(&c, 0, sizeof c);
    c.kind = APR_SRC_FAKE;
    e = apr_graph_add_source(m->g, L"Teams", &c, &m->teams);
    if (apr_failed(&e)) return 0;
    e = apr_graph_add_source(m->g, L"Microphone", &c, &m->mic);
    if (apr_failed(&e)) return 0;
    e = apr_graph_add_bus(m->g, L"Main Mix", &m->mix);
    if (apr_failed(&e)) return 0;
    e = apr_graph_add_bus(m->g, L"Voice Only", &m->voice);
    if (apr_failed(&e)) return 0;

    /* The "none" action: a real registered encoder that opens no file, so the
     * output lane is populated without this test writing anything to disk. */
    memset(&cfg, 0, sizeof cfg);
    e = apr_graph_add_action(m->g, m->mix, "none", &cfg);
    if (apr_failed(&e)) return 0;
    return 1;
}

/* Everything that moves focus has to run on the UI thread; SendMessage is what
 * gets it there. See the file header. */
static int perform(UiHost *h, AprCanvasOp op)
{
    return (int)SendMessageW(h->canvas, APR_CANVAS_WM_PERFORM, (WPARAM)op, 0);
}

static int focus_node(UiHost *h, int index)
{
    return (int)SendMessageW(h->canvas, APR_CANVAS_WM_FOCUS_NODE,
                             (WPARAM)index, 0);
}

static void attach(UiHost *h, AprGraph *g)
{
    SendMessageW(h->canvas, APR_CANVAS_WM_SET_GRAPH, 0, (LPARAM)g);
}

static int find_node_index(UiHost *h, AprNodeKind kind, uint32_t id, int sub)
{
    size_t i, n = apr_canvas_node_count(h->canvas);

    for (i = 0; i < n; ++i) {
        HWND w = apr_canvas_node_at(h->canvas, i);
        if (apr_node_kind(w) == kind && apr_node_model_id(w) == id &&
            apr_node_sub_id(w) == sub) {
            return (int)i;
        }
    }
    return -1;
}

/* --- the UIA client ------------------------------------------------------ */

typedef struct UiaClient {
    IUIAutomation *uia;
    int            com_ok;
} UiaClient;

static int uia_open(UiaClient *c)
{
    HRESULT hr;

    memset(c, 0, sizeof *c);
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    c->com_ok = SUCCEEDED(hr);
    if (!c->com_ok) return 0;
    hr = CoCreateInstance(&kCLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                          &kIID_IUIAutomation, (void **)&c->uia);
    return SUCCEEDED(hr) && c->uia != NULL;
}

static void uia_close(UiaClient *c)
{
    if (c->uia) IUIAutomation_Release(c->uia);
    if (c->com_ok) CoUninitialize();
    memset(c, 0, sizeof *c);
}

static int uia_name(UiaClient *c, HWND w, wchar_t *buf, size_t cch)
{
    IUIAutomationElement *el = NULL;
    BSTR s = NULL;

    buf[0] = 0;
    if (FAILED(IUIAutomation_ElementFromHandle(c->uia, w, &el)) || !el) return 0;
    if (SUCCEEDED(IUIAutomationElement_get_CurrentName(el, &s)) && s) {
        lstrcpynW(buf, s, (int)cch);
        SysFreeString(s);
    }
    IUIAutomationElement_Release(el);
    return buf[0] != 0;
}

static CONTROLTYPEID uia_type(UiaClient *c, HWND w)
{
    IUIAutomationElement *el = NULL;
    CONTROLTYPEID t = 0;

    if (FAILED(IUIAutomation_ElementFromHandle(c->uia, w, &el)) || !el) return 0;
    (void)IUIAutomationElement_get_CurrentControlType(el, &t);
    IUIAutomationElement_Release(el);
    return t;
}

/* The DESCRIPTION, read the way a screen reader reads it: through the
 * LegacyIAccessible pattern, which is what the MSAA-to-UIA bridge exposes
 * IAccessible::get_accDescription as. That is the property
 * apr_ui_set_accessible_description_text actually writes, so this asks the
 * real question rather than a convenient one. */
static int uia_description(UiaClient *c, HWND w, wchar_t *buf, size_t cch)
{
    IUIAutomationElement *el = NULL;
    IUIAutomationLegacyIAccessiblePattern *pat = NULL;
    BSTR s = NULL;

    buf[0] = 0;
    if (FAILED(IUIAutomation_ElementFromHandle(c->uia, w, &el)) || !el) return 0;
    if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(
            el, UIA_LegacyIAccessiblePatternId,
            &IID_IUIAutomationLegacyIAccessiblePattern, (void **)&pat)) && pat) {
        if (SUCCEEDED(IUIAutomationLegacyIAccessiblePattern_get_CurrentDescription(
                pat, &s)) && s) {
            lstrcpynW(buf, s, (int)cch);
            SysFreeString(s);
        }
        IUIAutomationLegacyIAccessiblePattern_Release(pat);
    }
    IUIAutomationElement_Release(el);
    return buf[0] != 0;
}

/* --- fixture ------------------------------------------------------------- */

typedef struct Fixture {
    UiHost    h;
    UiaClient c;
    Model     m;
    int       have_ui;
    int       have_uia;
} Fixture;

/* The language is chosen BEFORE the window exists, so the canvas lays out in
 * that direction from its first paint rather than being flipped afterwards --
 * which is also how a real session works, since the interface language is
 * settled before the frame is created. */
static int fixture_up_lang(Fixture *f, LANGID lang)
{
    memset(f, 0, sizeof *f);
    (void)apr_str_set_language(lang);

    if (!build_model(&f->m)) {
        printf("      SKIPPED: could not build the model\n");
        return 0;
    }
    if (!ui_start(&f->h)) {
        printf("      SKIPPED: could not create a window (no interactive "
               "window station?)\n");
        return 0;
    }
    f->have_ui = 1;
    attach(&f->h, f->m.g);
    f->have_uia = uia_open(&f->c);
    return 1;
}

static int fixture_up(Fixture *f)
{
    return fixture_up_lang(f, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
}

static void fixture_down(Fixture *f)
{
    if (f->have_uia) uia_close(&f->c);
    /* Detach BEFORE the graph dies: the canvas borrows it and would otherwise
     * be holding a pointer into freed memory for the length of a teardown. */
    if (f->have_ui) attach(&f->h, NULL);
    if (f->h.thread) ui_stop(&f->h);
    if (f->m.g) apr_graph_destroy(f->m.g);
    (void)apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    memset(f, 0, sizeof *f);
}

/* ==========================================================================
 * LIVE: nodes are real, named, typed elements
 * ======================================================================== */

TEST(every_node_is_a_real_window_with_a_name_and_the_control_type_we_meant)
{
    Fixture f;
    size_t i, n;

    if (!fixture_up(&f)) { fixture_down(&f); return; }

    n = apr_canvas_node_count(f.h.canvas);
    printf("      %u nodes\n", (unsigned)n);

    /* Two sources, two buses, one output. */
    ASSERT_EQ_INT(5, (int)n);

    if (!f.have_uia) {
        printf("      SKIPPED the UIA half: no UI Automation here\n");
        fixture_down(&f);
        return;
    }

    for (i = 0; i < n; ++i) {
        HWND w = apr_canvas_node_at(f.h.canvas, i);
        wchar_t name[256];
        CONTROLTYPEID t;

        ASSERT_NOT_NULL(w);
        ASSERT_TRUE(IsWindow(w) != FALSE);

        t = uia_type(&f.c, w);
        uia_name(&f.c, w, name, 256);
        printf("      node %u: type %d \"%ls\"\n", (unsigned)i, (int)t, name);

        /* THE ASSERTION THIS FILE EXISTS FOR. An element with no Name is read
         * by a screen reader as its control type and nothing else. */
        ASSERT_TRUE(name[0] != 0);

        /* And not a nameless "pane": the role is annotated, so the type a
         * reader announces says this is a thing, not a region. */
        ASSERT_TRUE(t == UIA_GroupControlTypeId);
    }

    fixture_down(&f);
}

TEST(a_node_says_what_kind_of_thing_it_is_not_only_what_colour_it_is)
{
    Fixture f;
    wchar_t name[256], want[256];
    const wchar_t *args[1];
    int i;

    if (!fixture_up(&f)) { fixture_down(&f); return; }
    if (!f.have_uia) { printf("      SKIPPED: no UIA\n"); fixture_down(&f); return; }

    i = find_node_index(&f.h, APR_NODE_SOURCE, f.m.teams, 0);
    ASSERT_GE_INT(0, i);
    uia_name(&f.c, apr_canvas_node_at(f.h.canvas, (size_t)i), name, 256);

    /* Straight out of the catalog, with the node's own name inserted. A
     * literal in canvas.c would fail this, which is the point. */
    args[0] = L"Teams";
    apr_str_format(APR_S_UI_NODE_SOURCE, want, 256, args, 1);
    printf("      source node name: \"%ls\"\n", name);
    ASSERT_WSTR_EQ(want, name);

    i = find_node_index(&f.h, APR_NODE_BUS, f.m.mix, 0);
    ASSERT_GE_INT(0, i);
    uia_name(&f.c, apr_canvas_node_at(f.h.canvas, (size_t)i), name, 256);
    args[0] = L"Main Mix";
    apr_str_format(APR_S_UI_NODE_BUS, want, 256, args, 1);
    ASSERT_WSTR_EQ(want, name);

    fixture_down(&f);
}

/* ==========================================================================
 * LIVE: edges. They have no window, so they live in the text and in the keys.
 * ======================================================================== */

TEST(an_edge_is_in_the_nodes_description_because_it_has_no_element_of_its_own)
{
    Fixture f;
    wchar_t before[512], after[512];
    int si, bi;

    if (!fixture_up(&f)) { fixture_down(&f); return; }
    if (!f.have_uia) { printf("      SKIPPED: no UIA\n"); fixture_down(&f); return; }

    si = find_node_index(&f.h, APR_NODE_SOURCE, f.m.teams, 0);
    bi = find_node_index(&f.h, APR_NODE_BUS, f.m.mix, 0);
    ASSERT_GE_INT(0, si);
    ASSERT_GE_INT(0, bi);

    if (!uia_description(&f.c, apr_canvas_node_at(f.h.canvas, (size_t)si),
                         before, 512)) {
        printf("      SKIPPED: LegacyIAccessible description not available\n");
        fixture_down(&f);
        return;
    }
    printf("      before: \"%ls\"\n", before);
    ASSERT_NULL(wcsstr(before, L"Main Mix"));

    /* Connect, by keyboard, exactly the way a user would: mark, move, press
     * again. */
    ASSERT_TRUE(focus_node(&f.h, si));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_CONNECT));
    ASSERT_NOT_NULL(apr_canvas_pending_node(f.h.canvas));
    ASSERT_TRUE(focus_node(&f.h, bi));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_CONNECT));
    ASSERT_NULL(apr_canvas_pending_node(f.h.canvas));

    ASSERT_TRUE(apr_graph_connected(f.m.g, f.m.teams, f.m.mix) != 0);

    uia_description(&f.c, apr_canvas_node_at(f.h.canvas, (size_t)si), after, 512);
    printf("      after:  \"%ls\"\n", after);

    /* THE EDGE IS NOW SAYABLE. Nothing was painted that a screen reader user
     * could not also hear. */
    ASSERT_NOT_NULL(wcsstr(after, L"Main Mix"));

    /* And the bus says who feeds it, which is the other direction of the same
     * edge and a separate projection of the model (graph.h). */
    uia_description(&f.c, apr_canvas_node_at(f.h.canvas, (size_t)bi), after, 512);
    printf("      bus:    \"%ls\"\n", after);
    ASSERT_NOT_NULL(wcsstr(after, L"Teams"));

    fixture_down(&f);
}

TEST(the_user_is_told_when_a_connection_is_made)
{
    Fixture f;
    wchar_t said[512], want[512];
    const wchar_t *args[2];
    int si, bi;

    if (!fixture_up(&f)) { fixture_down(&f); return; }

    si = find_node_index(&f.h, APR_NODE_SOURCE, f.m.teams, 0);
    bi = find_node_index(&f.h, APR_NODE_BUS, f.m.mix, 0);

    ASSERT_TRUE(focus_node(&f.h, si));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_CONNECT));

    /* The half-done state announces itself, INCLUDING how to get out of it.
     * A mode the user cannot hear is a trap. */
    apr_canvas_last_announcement(f.h.canvas, said, 512);
    printf("      step 1: \"%ls\"\n", said);
    args[0] = L"Teams";
    apr_str_format(APR_S_UI_ANN_CONNECT_START, want, 512, args, 1);
    ASSERT_WSTR_EQ(want, said);

    ASSERT_TRUE(focus_node(&f.h, bi));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_CONNECT));

    apr_canvas_last_announcement(f.h.canvas, said, 512);
    printf("      step 2: \"%ls\"\n", said);
    args[0] = L"Teams";
    args[1] = L"Main Mix";
    apr_str_format(APR_S_UI_ANN_CONNECT_DONE, want, 512, args, 2);
    ASSERT_WSTR_EQ(want, said);

    /* Focus lands on the node whose meaning changed, which is what a screen
     * reader actually speaks. */
    ASSERT_TRUE(apr_canvas_focused_node(f.h.canvas) ==
                apr_canvas_node_at(f.h.canvas, (size_t)si));

    fixture_down(&f);
}

TEST(escape_gets_the_user_out_of_a_half_made_connection)
{
    Fixture f;
    wchar_t said[512];
    int si;

    if (!fixture_up(&f)) { fixture_down(&f); return; }

    si = find_node_index(&f.h, APR_NODE_SOURCE, f.m.teams, 0);
    ASSERT_TRUE(focus_node(&f.h, si));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_CONNECT));
    ASSERT_NOT_NULL(apr_canvas_pending_node(f.h.canvas));

    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_CANCEL));
    ASSERT_NULL(apr_canvas_pending_node(f.h.canvas));
    ASSERT_FALSE(apr_graph_connected(f.m.g, f.m.teams, f.m.mix) != 0);

    apr_canvas_last_announcement(f.h.canvas, said, 512);
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_ANN_CANCELLED), said);

    fixture_down(&f);
}

TEST(following_an_edge_by_keyboard_lands_on_the_other_end)
{
    Fixture f;
    int si, bi, ai;

    if (!fixture_up(&f)) { fixture_down(&f); return; }

    si = find_node_index(&f.h, APR_NODE_SOURCE, f.m.teams, 0);
    bi = find_node_index(&f.h, APR_NODE_BUS, f.m.mix, 0);
    ai = find_node_index(&f.h, APR_NODE_ACTION, f.m.mix, 0);
    ASSERT_GE_INT(0, ai);

    /* Nothing to follow yet, and it says so rather than moving silently. */
    ASSERT_TRUE(focus_node(&f.h, si));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_DOWNSTREAM));
    ASSERT_TRUE(apr_canvas_focused_node(f.h.canvas) ==
                apr_canvas_node_at(f.h.canvas, (size_t)si));
    {
        wchar_t said[512], want[512];
        const wchar_t *a[1];
        a[0] = L"Teams";
        apr_canvas_last_announcement(f.h.canvas, said, 512);
        apr_str_format(APR_S_UI_ANN_NO_EDGE_DOWN, want, 512, a, 1);
        ASSERT_WSTR_EQ(want, said);
    }

    /* Connect it, then walk the whole chain source -> bus -> output and back.
     * THIS is what makes a graph navigable without pixel positions. */
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_CONNECT));
    ASSERT_TRUE(focus_node(&f.h, bi));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_CONNECT));

    si = find_node_index(&f.h, APR_NODE_SOURCE, f.m.teams, 0);
    bi = find_node_index(&f.h, APR_NODE_BUS, f.m.mix, 0);
    ai = find_node_index(&f.h, APR_NODE_ACTION, f.m.mix, 0);

    ASSERT_TRUE(focus_node(&f.h, si));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_DOWNSTREAM));
    ASSERT_TRUE(apr_canvas_focused_node(f.h.canvas) ==
                apr_canvas_node_at(f.h.canvas, (size_t)bi));

    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_DOWNSTREAM));
    ASSERT_TRUE(apr_canvas_focused_node(f.h.canvas) ==
                apr_canvas_node_at(f.h.canvas, (size_t)ai));

    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_UPSTREAM));
    ASSERT_TRUE(apr_canvas_focused_node(f.h.canvas) ==
                apr_canvas_node_at(f.h.canvas, (size_t)bi));

    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_UPSTREAM));
    ASSERT_TRUE(apr_canvas_focused_node(f.h.canvas) ==
                apr_canvas_node_at(f.h.canvas, (size_t)si));

    fixture_down(&f);
}

TEST(disconnect_and_delete_are_reachable_and_announced)
{
    Fixture f;
    wchar_t said[512], want[512];
    const wchar_t *args[2];
    int si, bi;

    if (!fixture_up(&f)) { fixture_down(&f); return; }

    si = find_node_index(&f.h, APR_NODE_SOURCE, f.m.teams, 0);
    bi = find_node_index(&f.h, APR_NODE_BUS, f.m.mix, 0);
    ASSERT_TRUE(focus_node(&f.h, si));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_CONNECT));
    ASSERT_TRUE(focus_node(&f.h, bi));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_CONNECT));
    ASSERT_TRUE(apr_graph_connected(f.m.g, f.m.teams, f.m.mix) != 0);

    si = find_node_index(&f.h, APR_NODE_SOURCE, f.m.teams, 0);
    bi = find_node_index(&f.h, APR_NODE_BUS, f.m.mix, 0);
    ASSERT_TRUE(focus_node(&f.h, si));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_DISCONNECT));
    ASSERT_TRUE(focus_node(&f.h, bi));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_DISCONNECT));

    ASSERT_FALSE(apr_graph_connected(f.m.g, f.m.teams, f.m.mix) != 0);
    apr_canvas_last_announcement(f.h.canvas, said, 512);
    args[0] = L"Teams";
    args[1] = L"Main Mix";
    apr_str_format(APR_S_UI_ANN_DISCONNECT_DONE, want, 512, args, 2);
    printf("      \"%ls\"\n", said);
    ASSERT_WSTR_EQ(want, said);

    /* Delete removes the node from the MODEL, not merely from the picture. */
    si = find_node_index(&f.h, APR_NODE_SOURCE, f.m.teams, 0);
    ASSERT_TRUE(focus_node(&f.h, si));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_REMOVE));
    ASSERT_NULL(apr_graph_source(f.m.g, f.m.teams));
    ASSERT_EQ_INT(-1, find_node_index(&f.h, APR_NODE_SOURCE, f.m.teams, 0));

    apr_canvas_last_announcement(f.h.canvas, said, 512);
    args[0] = L"Teams";
    apr_str_format(APR_S_UI_ANN_REMOVED, want, 512, args, 1);
    ASSERT_WSTR_EQ(want, said);

    fixture_down(&f);
}

TEST(the_level_of_a_source_is_settable_by_keyboard_and_spoken_back)
{
    Fixture f;
    wchar_t said[512];
    int si, bi;
    AprBus *b;

    if (!fixture_up(&f)) { fixture_down(&f); return; }

    si = find_node_index(&f.h, APR_NODE_SOURCE, f.m.teams, 0);

    /* Not connected yet, so there is no level to set -- and it says so rather
     * than pretending it did something. */
    ASSERT_TRUE(focus_node(&f.h, si));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_GAIN_DOWN));
    apr_canvas_last_announcement(f.h.canvas, said, 512);
    printf("      \"%ls\"\n", said);
    ASSERT_NOT_NULL(wcsstr(said, L"Teams"));

    bi = find_node_index(&f.h, APR_NODE_BUS, f.m.mix, 0);
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_CONNECT));
    ASSERT_TRUE(focus_node(&f.h, bi));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_CONNECT));

    si = find_node_index(&f.h, APR_NODE_SOURCE, f.m.teams, 0);
    ASSERT_TRUE(focus_node(&f.h, si));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_GAIN_DOWN));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_GAIN_DOWN));

    apr_canvas_last_announcement(f.h.canvas, said, 512);
    printf("      \"%ls\"\n", said);
    ASSERT_NOT_NULL(wcsstr(said, L"-2.0"));

    b = apr_graph_bus(f.m.g, f.m.mix);
    ASSERT_NOT_NULL(b);
    ASSERT_NEAR(0.794328, (double)apr_bus_gain(b, f.m.teams), 0.001);

    fixture_down(&f);
}

TEST(a_refusal_says_what_is_actually_wrong_rather_than_the_nearest_sentence)
{
    Fixture f;
    wchar_t said[512], want[512];
    const wchar_t *args[1];
    int bi;

    if (!fixture_up(&f)) { fixture_down(&f); return; }

    bi = find_node_index(&f.h, APR_NODE_BUS, f.m.mix, 0);
    ASSERT_GE_INT(0, bi);
    ASSERT_TRUE(focus_node(&f.h, bi));

    /* Starting a connection ON a bus is not the same mistake as finishing one
     * on something that is not a bus, and telling the user their bus is not a
     * bus is worse than saying nothing at all. */
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_CONNECT));
    ASSERT_NULL(apr_canvas_pending_node(f.h.canvas));
    apr_canvas_last_announcement(f.h.canvas, said, 512);
    printf("      \"%ls\"\n", said);
    args[0] = L"Main Mix";
    apr_str_format(APR_S_UI_ANN_CONNECT_NOT_SOURCE, want, 512, args, 1);
    ASSERT_WSTR_EQ(want, said);

    /* Same again for a level: a bus has none, and "Main Mix feeds no bus" is
     * a false statement about a bus. */
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_GAIN_UP));
    apr_canvas_last_announcement(f.h.canvas, said, 512);
    printf("      \"%ls\"\n", said);
    apr_str_format(APR_S_UI_ANN_GAIN_NOT_SOURCE, want, 512, args, 1);
    ASSERT_WSTR_EQ(want, said);

    /* An output CAN now be deleted on its own. This case used to assert the
     * refusal, because bus.h had no call for it and the canvas correctly
     * refused rather than becoming a second owner of a bus's action list.
     * apr_bus_remove_action() exists now -- and it FINALIZES before it
     * detaches, so the file the output was writing is closed and playable
     * rather than abandoned (AGENTS.md rule 4). */
    ASSERT_TRUE(focus_node(&f.h,
                           (size_t)find_node_index(&f.h, APR_NODE_ACTION,
                                                   f.m.mix, 0)));
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_REMOVE));
    ASSERT_EQ_INT(-1, find_node_index(&f.h, APR_NODE_ACTION, f.m.mix, 0));
    apr_canvas_last_announcement(f.h.canvas, said, 512);
    printf("      \"%ls\"\n", said);
    ASSERT_FALSE(wcscmp(apr_str(APR_S_UI_ANN_REMOVE_REFUSED), said) == 0);
    ASSERT_TRUE(said[0] != 0);

    fixture_down(&f);
}

/* ==========================================================================
 * LIVE: order. The half of RTL that everyone gets wrong.
 * ======================================================================== */

/* The live sibling chain, top of the z-order first. This -- not our own array
 * -- is the order UI Automation enumerates children in, and therefore the
 * order a screen reader walks. */
static int zorder_index(HWND canvas, HWND node)
{
    HWND w = GetWindow(canvas, GW_CHILD);
    int i = 0;

    while (w) {
        if (!apr_node_is_node(w)) { w = GetWindow(w, GW_HWNDNEXT); continue; }
        if (w == node) return i;
        ++i;
        w = GetWindow(w, GW_HWNDNEXT);
    }
    return -1;
}

static void assert_logical_order(Fixture *f, const char *what)
{
    size_t i, n = apr_canvas_node_count(f->h.canvas);
    int last_kind = -1;

    printf("      %s: ", what);
    for (i = 0; i < n; ++i) {
        HWND w = apr_canvas_node_at(f->h.canvas, i);
        int k = (int)apr_node_kind(w);

        printf("%d ", k);

        /* Source, then bus, then output -- monotonically, in both languages.
         * The picture mirrors; this does not. */
        ASSERT_GE_INT(last_kind, k);
        last_kind = k;

        /* And the accessibility order is the same order. */
        ASSERT_EQ_INT((int)i, zorder_index(f->h.canvas, w));
    }
    printf("\n");
}

TEST(tab_order_and_the_accessibility_tree_stay_source_bus_output_in_both_directions)
{
    Fixture f;

    /* THE HALF OF RTL THAT EVERYONE GETS WRONG, and the half a screen reader
     * user actually feels: the picture is allowed to mirror, the order is
     * not. Two windows, one process, because the direction source is a
     * function of the language rather than of the build. */
    if (!fixture_up_lang(&f, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US))) {
        fixture_down(&f);
        return;
    }
    ASSERT_EQ_INT((int)APR_DIR_LTR, (int)apr_ui_dir());
    assert_logical_order(&f, "LTR");
    fixture_down(&f);

    if (!fixture_up_lang(&f, MAKELANGID(LANG_ARABIC,
                                        SUBLANG_ARABIC_SAUDI_ARABIA))) {
        fixture_down(&f);
        return;
    }
    ASSERT_EQ_INT((int)APR_DIR_RTL, (int)apr_ui_dir());
    assert_logical_order(&f, "RTL");
    fixture_down(&f);
}

TEST(the_picture_mirrors_and_the_order_does_not)
{
    Fixture f;
    RECT src, bus;
    int si, bi;
    HWND sw, bw;

    if (!fixture_up_lang(&f, MAKELANGID(LANG_ARABIC,
                                        SUBLANG_ARABIC_SAUDI_ARABIA))) {
        fixture_down(&f);
        return;
    }

    si = find_node_index(&f.h, APR_NODE_SOURCE, f.m.teams, 0);
    bi = find_node_index(&f.h, APR_NODE_BUS, f.m.mix, 0);
    ASSERT_GE_INT(0, si);
    ASSERT_GE_INT(0, bi);

    sw = apr_canvas_node_at(f.h.canvas, (size_t)si);
    bw = apr_canvas_node_at(f.h.canvas, (size_t)bi);
    GetWindowRect(sw, &src);
    GetWindowRect(bw, &bus);
    printf("      RTL: source x=[%ld,%ld] bus x=[%ld,%ld]\n",
           src.left, src.right, bus.left, bus.right);

    /* Sources on the RIGHT in Arabic. Design 6.2 calls hardcoding this a
     * review failure; this is the assertion that would catch it. */
    ASSERT_GT_INT((int)bus.left, (int)src.left);

    /* And no node window acquired the mirrored device context that would have
     * drawn its text backwards. */
    ASSERT_TRUE((GetWindowLongPtrW(sw, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) == 0);
    ASSERT_TRUE((GetWindowLongPtrW(f.h.canvas, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) == 0);

    /* ...while the order a screen reader walks is unchanged: the source is
     * still enumerated before the bus, even though it is now painted to the
     * right of it. */
    printf("      RTL z-order: source %d, bus %d\n",
           zorder_index(f.h.canvas, sw), zorder_index(f.h.canvas, bw));
    ASSERT_TRUE(zorder_index(f.h.canvas, sw) < zorder_index(f.h.canvas, bw));

    fixture_down(&f);
}

/* ==========================================================================
 * LIVE: Tab is not a trap
 * ======================================================================== */

TEST(tab_walks_the_nodes_and_then_leaves_the_pane)
{
    Fixture f;
    size_t n;
    int i;

    if (!fixture_up(&f)) { fixture_down(&f); return; }

    n = apr_canvas_node_count(f.h.canvas);
    ASSERT_GT_INT(1, (int)n);

    ASSERT_TRUE(focus_node(&f.h, 0));
    for (i = 1; i < (int)n; ++i) {
        ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_NEXT_NODE));
        ASSERT_TRUE(apr_canvas_focused_node(f.h.canvas) ==
                    apr_canvas_node_at(f.h.canvas, (size_t)i));
    }

    /* THE ESCAPE. The node windows claim every key, Tab included, so this is
     * the only thing standing between the canvas and being a focus trap. */
    ASSERT_TRUE(perform(&f.h, APR_CANVAS_OP_NEXT_NODE));
    {
        GUITHREADINFO gti;
        memset(&gti, 0, sizeof gti);
        gti.cbSize = sizeof gti;
        ASSERT_TRUE(GetGUIThreadInfo(GetThreadId(f.h.thread), &gti) != FALSE);
        printf("      focus after the last node: %p (canvas %p)\n",
               (void *)gti.hwndFocus, (void *)f.h.canvas);
        ASSERT_TRUE(gti.hwndFocus != NULL);
        ASSERT_FALSE(IsChild(f.h.canvas, gti.hwndFocus) != FALSE);
    }

    fixture_down(&f);
}

/* ==========================================================================
 * LIVE: the frame's own commands reach the canvas
 * ======================================================================== */

TEST(the_frames_connect_command_is_the_same_operation_as_the_key)
{
    Fixture f;
    int si, bi;

    if (!fixture_up(&f)) { fixture_down(&f); return; }

    si = find_node_index(&f.h, APR_NODE_SOURCE, f.m.mic, 0);
    bi = find_node_index(&f.h, APR_NODE_BUS, f.m.voice, 0);

    ASSERT_TRUE(focus_node(&f.h, si));
    ASSERT_TRUE(apr_canvas_command(f.h.canvas, APR_CMD_CONNECT));
    ASSERT_TRUE(focus_node(&f.h, bi));
    ASSERT_TRUE(apr_canvas_command(f.h.canvas, APR_CMD_CONNECT));
    ASSERT_TRUE(apr_graph_connected(f.m.g, f.m.mic, f.m.voice) != 0);

    /* A command that is not ours is refused rather than swallowed, so the
     * frame can go on offering it to the tree panel. */
    ASSERT_FALSE(apr_canvas_command(f.h.canvas, APR_CMD_FILE_SAVE) != 0);

    fixture_down(&f);
}
