/*
 * test_ui_behaviour.c -- the application driven the way a person drives it,
 * asserting what a person GETS.
 *
 * ===========================================================================
 * WHY THIS SUITE EXISTS
 *
 *   Three defects reached the author with every suite green: every dialog
 *   failed to open, no source could be added, and the canvas described a graph
 *   that was not there. They share one cause. The suites asserted STRUCTURE --
 *   the element exists, it is named, it is focusable, the description is not
 *   empty -- and never BEHAVIOUR: press this, and did the model change, and
 *   was the user told.
 *
 *   So every case here does the same three things:
 *
 *     1. SEND THE REAL MESSAGE. A WM_COMMAND at the frame with 1 in the high
 *        word is literally what TranslateAccelerator sends; a WM_KEYDOWN at the
 *        focused node is literally what DispatchMessage delivers. Nothing here
 *        calls a handler directly, because a test of a parallel path proves
 *        nothing about the path the user is on.
 *     2. ASSERT THE MODEL CHANGED -- source and bus counts, apr_graph_connected,
 *        the gain on the edge.
 *     3. ASSERT WHAT WAS ANNOUNCED, against the catalog sentence with the
 *        catalog's own inserts. AN OPERATION THAT CHANGES THE MODEL AND SAYS
 *        NOTHING IS A FAILURE HERE, because for this author that is
 *        indistinguishable from a broken feature.
 *
 *   And one thing no other suite checks at all: that the sentence reaches the
 *   ACCESSIBILITY TREE rather than merely a buffer we can read back. See the
 *   first two cases -- a live region whose element is named "Status" is a live
 *   region that speaks the word "Status", forever, whatever we wrote into it.
 *
 * ===========================================================================
 * EVERY MODEL CALL HAPPENS ON THE WINDOW'S OWN THREAD
 *
 *   Editing the graph publishes into two views, which creates and destroys real
 *   child windows. Doing that from the asserting thread would build windows
 *   owned by the wrong thread and prove nothing about the product, so the setup
 *   runs inside the UI thread before the message loop starts, and everything
 *   after that arrives as a message. The only calls made from here are reads.
 *
 * ===========================================================================
 * SAFETY (AGENTS.md rule 1)
 *
 *   APR_SRC_FAKE throughout. No endpoint is opened by anything in this file, no
 *   capture is ever started, and not one sample is rendered to any output
 *   device. The dialogs that would enumerate hardware are opened and CANCELLED,
 *   never accepted. The one WAV output exists so that two dialogs have
 *   something to act on; it is finalized on teardown and the file is deleted.
 */
#include "test_runner.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <initguid.h>
#include <uiautomation.h>
#include <stdio.h>

#include "action.h"
#include "bus.h"
#include "capture.h"
#include "errmsg.h"
#include "graph.h"
#include "outpath.h"
#include "strings.h"
#include "session.h"
#include "ui_app.h"
#include "ui_canvas.h"
#include "ui_controller.h"
#include "ui_dialogs.h"
#include "ui_node.h"
#include "ui_tree_panel.h"

static const CLSID kCLSID_CUIAutomation =
    { 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
static const IID kIID_IUIAutomation =
    { 0x30cbe57d, 0xd9d0, 0x452a, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };

#define IDC_NAME 2003   /* the edit field in the name prompt, src/ui/dialogs.c */

/* ==========================================================================
 * A real frame, a real controller, on a real STA -- the apartment the product
 * runs in, because IAccPropServices supplies every accessible name and is
 * valid only on the thread that created it (src/app/main.c).
 * ======================================================================== */

typedef struct Setup {
    /* what to build, before the message loop starts */
    int      want_source;
    int      want_bus;
    int      want_edge;
    int      want_output;

    /* what came back */
    AprSourceId src;
    AprBusId    bus;
    int         built;
    AprErr      err;
    wchar_t     out_path[MAX_PATH];

    /* A SESSION OPENED AFTER THE FIXTURE IS READY BUT BEFORE THE LOOP RUNS.
     *
     * apr_controller_open_session is modal when it has anything to report, and
     * a modal owns the thread that opened it -- so the only way to answer one
     * is from another thread, which means the asserting thread has to be
     * running by then. The UI thread therefore signals `ready` FIRST and opens
     * the session second; the test finds the dialog and answers it. */
    wchar_t open_path[MAX_PATH];
    AprErr  open_err;
    volatile LONG open_done;
} Setup;

typedef struct UiHost {
    HANDLE         thread;
    DWORD          tid;
    HANDLE         ready;
    AprUiApp      *app;
    AprController *ctl;
    AprGraph      *graph;
    HWND           frame;
    HWND           canvas;
    HWND           tree;     /* the Structure PANE (our host window) */
    HWND           tv;       /* the TreeView inside it                */
    HWND           status;
    Setup          setup;
    int            failed;
    AprErr         create_err;
} UiHost;

static void build_setup(UiHost *h)
{
    Setup *s = &h->setup;
    AprCaptureConfig cfg;
    AprErr e;

    h->graph = apr_controller_graph(h->ctl);
    if (!h->graph) {
        s->err = APR_ERR(APR_E_STATE, L"the controller has no graph");
        return;
    }

    if (s->want_source) {
        memset(&cfg, 0, sizeof cfg);
        cfg.kind = APR_SRC_FAKE;
        cfg.fake.tone_hz   = 440;
        cfg.fake.amplitude = 0.25f;
        e = apr_controller_add_source(h->ctl, L"Teams", &cfg);
        if (apr_failed(&e)) { s->err = e; return; }
        s->src = apr_source_id(apr_graph_source_at(h->graph, 0));
    }
    if (s->want_bus) {
        e = apr_controller_add_bus(h->ctl, L"Main Mix");
        if (apr_failed(&e)) { s->err = e; return; }
        s->bus = apr_bus_id(apr_graph_bus_at(h->graph, 0));
    }
    if (s->want_edge) {
        e = apr_graph_connect(h->graph, s->src, s->bus, 1.0f);
        if (apr_failed(&e)) { s->err = e; return; }
        apr_controller_model_changed(h->ctl);
    }
    if (s->want_output) {
        AprActionConfig ac;
        wchar_t dir[MAX_PATH];
        DWORD n = GetTempPathW(MAX_PATH, dir);

        if (n == 0 || n >= MAX_PATH) { s->err = APR_ERR(APR_E_IO, L"no temp dir"); return; }
        swprintf(s->out_path, MAX_PATH, L"%sapr_behaviour_%lu.wav", dir,
                 (unsigned long)GetCurrentProcessId());
        /* The name is fixed per process, so anything an earlier case left
         * behind would be found by the collision policy and push this take
         * to a different name. Start from a clean folder. */
        DeleteFileW(s->out_path);
        memset(&ac, 0, sizeof ac);
        ac.out_path = s->out_path;
        e = apr_graph_add_action(h->graph, s->bus, "wav", &ac);
        if (apr_failed(&e)) { s->err = e; return; }
        apr_controller_model_changed(h->ctl);
    }
    s->built = 1;
}

static DWORD WINAPI ui_thread(LPVOID param)
{
    UiHost *h = (UiHost *)param;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    (void)apr_str_init();

    h->create_err = apr_ui_app_create(GetModuleHandleW(NULL), &h->app);
    if (apr_failed(&h->create_err)) {
        h->failed = 1;
        SetEvent(h->ready);
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }

    h->create_err = apr_controller_create(h->app, &h->ctl);
    if (apr_failed(&h->create_err)) {
        h->failed = 1;
        apr_ui_app_destroy(h->app);
        h->app = NULL;
        SetEvent(h->ready);
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }

    h->frame  = apr_ui_app_hwnd(h->app);
    h->canvas = apr_ui_app_pane(h->app, APR_PANE_CANVAS);
    h->tree   = apr_ui_app_pane(h->app, APR_PANE_TREE);
    h->tv     = h->tree ? apr_tree_panel_treeview(h->tree) : NULL;
    h->status = apr_ui_app_status_bar(h->app);

    build_setup(h);
    SetEvent(h->ready);

    /* See Setup::open_path. */
    if (h->setup.open_path[0]) {
        h->setup.open_err =
            apr_controller_open_session(h->ctl, h->setup.open_path, 1);
        InterlockedIncrement(&h->setup.open_done);
    }

    apr_ui_app_run(h->app);

    apr_controller_destroy(h->ctl);
    h->ctl = NULL;
    apr_ui_app_destroy(h->app);
    h->app = NULL;
    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}

static int ui_start(UiHost *h)
{
    h->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!h->ready) return 0;
    h->thread = CreateThread(NULL, 0, ui_thread, h, 0, &h->tid);
    if (!h->thread) return 0;
    if (WaitForSingleObject(h->ready, 30000) != WAIT_OBJECT_0) return 0;
    return !h->failed && h->frame != NULL && h->canvas != NULL;
}

static void ui_stop(UiHost *h)
{
    if (h->frame && IsWindow(h->frame)) PostMessageW(h->frame, WM_CLOSE, 0, 0);
    if (h->thread) {
        if (WaitForSingleObject(h->thread, 30000) != WAIT_OBJECT_0) {
            printf("      WARNING: UI thread did not exit; terminating\n");
            TerminateThread(h->thread, 1);
        }
        CloseHandle(h->thread);
    }
    if (h->ready) CloseHandle(h->ready);
    if (h->setup.out_path[0]) DeleteFileW(h->setup.out_path);
    memset(h, 0, sizeof *h);
}

/* ==========================================================================
 * Driving it the way the user does
 * ======================================================================== */

/* THE ACCELERATOR, EXACTLY. TranslateAccelerator sends WM_COMMAND with the
 * command in the low word and 1 in the high word; that 1 is how a window tells
 * an accelerator from a menu pick. Sent rather than posted, so the assertion
 * after it cannot run before the work does -- and sent from here means it runs
 * on the window's own thread, which is the whole point. */
static void accel(UiHost *h, int cmd)
{
    SendMessageW(h->frame, WM_COMMAND, MAKEWPARAM(cmd, 1), 0);
}

/* For a command that opens a modal: the frame will not return until the dialog
 * is answered, so this one cannot wait. */
static void accel_async(UiHost *h, int cmd)
{
    PostMessageW(h->frame, WM_COMMAND, MAKEWPARAM(cmd, 1), 0);
}

/* A KEYSTROKE, EXACTLY. DispatchMessage delivers WM_KEYDOWN to the focused
 * window, which on this pane is a node; node_window.c forwards every key to
 * the canvas, the single dispatch site for the binding table. */
static void keydown(HWND focused, UINT vk)
{
    SendMessageW(focused, WM_KEYDOWN, (WPARAM)vk, 0);
}

static int node_index(UiHost *h, AprNodeKind kind, uint32_t id, int sub)
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

/* Focus a node the way the platform does: a real SetFocus on the window's own
 * thread, which is what makes the node send BN_SETFOCUS back to the canvas and
 * what keeps its idea of "the current node" true. Cross-thread, so it goes
 * through the canvas's message form (ui_canvas.h); calling SetFocus from here
 * would do nothing, silently. */
static int focus_node(UiHost *h, int index)
{
    if (index < 0) return 0;
    return (int)SendMessageW(h->canvas, APR_CANVAS_WM_FOCUS_NODE,
                             (WPARAM)index, 0);
}

static const wchar_t *said(UiHost *h, wchar_t *buf, size_t cch)
{
    apr_controller_last_announcement(h->ctl, buf, cch);
    return buf;
}

/* The catalog sentence with the catalog's own inserts -- never a literal, and
 * never a substring check where a whole sentence can be compared. */
static const wchar_t *sentence(AprStrId id, const wchar_t *a1, const wchar_t *a2,
                               wchar_t *buf, size_t cch)
{
    const wchar_t *args[2];
    size_t n = 0;

    if (a1) args[n++] = a1;
    if (a2) args[n++] = a2;
    apr_str_format(id, buf, cch, args, n);
    return buf;
}

/* ==========================================================================
 * Modal dialogs, answered from OUTSIDE the thread that owns them
 *
 * A modal dialog owns the thread that opened it, so the only place it can be
 * answered from is another thread. This is not synthetic input: a posted
 * WM_COMMAND with BN_CLICKED is exactly what the dialog's own button sends.
 * ======================================================================== */

typedef struct FindDlg { HWND frame; HWND found; } FindDlg;

static BOOL CALLBACK find_dlg_cb(HWND w, LPARAM lp)
{
    FindDlg *f = (FindDlg *)lp;
    wchar_t cls[64];

    if (w == f->frame) return TRUE;
    if (!IsWindowVisible(w)) return TRUE;
    if (GetClassNameW(w, cls, 64) <= 0) return TRUE;
    if (CompareStringOrdinal(cls, -1, L"#32770", -1, FALSE) != CSTR_EQUAL) return TRUE;
    f->found = w;
    return FALSE;
}

static HWND find_dialog(UiHost *h)
{
    FindDlg f;

    f.frame = h->frame;
    f.found = NULL;
    EnumThreadWindows(h->tid, find_dlg_cb, (LPARAM)&f);
    return f.found;
}

static HWND wait_for_dialog(UiHost *h, int ms)
{
    int waited = 0;

    for (;;) {
        HWND d = find_dialog(h);
        if (d) return d;
        if (waited >= ms) return NULL;
        Sleep(10);
        waited += 10;
    }
}

static int wait_for_no_dialog(UiHost *h, int ms)
{
    int waited = 0;

    for (;;) {
        if (!find_dialog(h)) return 1;
        if (waited >= ms) return 0;
        Sleep(10);
        waited += 10;
    }
}

/* ==========================================================================
 * The UIA client -- the accessibility tree as a client sees it
 * ======================================================================== */

typedef struct UiaClient { IUIAutomation *uia; int com_ok; } UiaClient;

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

/* UIA_LiveSettingPropertyId, read as a raw property because
 * IUIAutomationElement has no accessor for it. -1 means the element does not
 * carry the property at all, which is the answer for every window whose
 * provider is the MSAA bridge -- and the reason the live SETTING cannot be the
 * mechanism this product relies on. See the first case. */
static int uia_live_setting(UiaClient *c, HWND w)
{
    IUIAutomationElement *el = NULL;
    VARIANT v;
    int out = -1;

    if (FAILED(IUIAutomation_ElementFromHandle(c->uia, w, &el)) || !el) return -1;
    VariantInit(&v);
    if (SUCCEEDED(IUIAutomationElement_GetCurrentPropertyValue(
            el, UIA_LiveSettingPropertyId, &v))) {
        if (v.vt == VT_I4) out = (int)v.lVal;
    }
    VariantClear(&v);
    IUIAutomationElement_Release(el);
    return out;
}

/* ==========================================================================
 * A WinEvent listener -- does the event we believe we raise actually go out,
 * and does a client reading it find the sentence?
 *
 * Out of context, in this process, delivered on the thread that installed it,
 * which is why the wait below pumps. This is the channel a screen reader
 * listens on for an MSAA-bridged window, and reading accName inside the
 * callback is what such a reader does with the event.
 * ======================================================================== */

static HWND     g_watch;
static DWORD    g_want_event;
static volatile LONG g_hits;
static wchar_t  g_hit_name[512];

static void CALLBACK win_event_cb(HWINEVENTHOOK hook, DWORD ev, HWND w,
                                  LONG obj, LONG child, DWORD tid, DWORD t)
{
    IAccessible *acc = NULL;
    VARIANT self;
    BSTR name = NULL;

    (void)hook; (void)tid; (void)t;
    if (w != g_watch || ev != g_want_event) return;
    if (obj != OBJID_CLIENT || child != CHILDID_SELF) return;

    if (SUCCEEDED(AccessibleObjectFromWindow(w, (DWORD)OBJID_CLIENT,
                                             &IID_IAccessible, (void **)&acc))
        && acc) {
        VariantInit(&self);
        self.vt = VT_I4;
        self.lVal = CHILDID_SELF;
        if (SUCCEEDED(IAccessible_get_accName(acc, self, &name)) && name) {
            lstrcpynW(g_hit_name, name, 512);
            SysFreeString(name);
        }
        IAccessible_Release(acc);
    }
    InterlockedIncrement(&g_hits);
}

static HWINEVENTHOOK watch_begin(HWND w, DWORD ev)
{
    g_watch = w;
    g_want_event = ev;
    g_hits = 0;
    g_hit_name[0] = 0;
    return SetWinEventHook(ev, ev, NULL, win_event_cb,
                           GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
}

static int watch_wait(int ms)
{
    int waited = 0;
    MSG msg;

    for (;;) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_hits > 0) return 1;
        if (waited >= ms) return 0;
        Sleep(5);
        waited += 5;
    }
}

static void watch_end(HWINEVENTHOOK hook)
{
    if (hook) UnhookWinEvent(hook);
    g_watch = NULL;
    g_want_event = 0;
}

/* ==========================================================================
 * The fixture
 * ======================================================================== */

typedef struct Fix {
    UiHost    h;
    AprGraph *g;
    int       up;
} Fix;

static int fix_up(Fix *f, int source, int bus, int edge, int output)
{
    memset(f, 0, sizeof *f);
    f->h.setup.want_source = source;
    f->h.setup.want_bus    = bus;
    f->h.setup.want_edge   = edge;
    f->h.setup.want_output = output;

    if (!ui_start(&f->h)) {
        printf("      SKIPPED: could not create a window (no interactive "
               "window station?)\n");
        return 0;
    }
    f->up = 1;
    if (!f->h.setup.built) {
        wchar_t why[512];
        apr_err_format(&f->h.setup.err, why, 512);
        printf("      could not build the fixture: %ls\n", why);
        return 0;
    }
    f->g = f->h.graph;
    return f->g != NULL;
}

static void fix_down(Fix *f)
{
    if (f->up) ui_stop(&f->h);
    memset(f, 0, sizeof *f);
}

static AprSourceId SRC(Fix *f) { return f->h.setup.src; }
static AprBusId    BUS(Fix *f) { return f->h.setup.bus; }

/* One announcement with a sentence nothing else in the session produces, made
 * without touching the model: stand on the source and begin a connection. */
static void announce_something(Fix *f)
{
    int si = node_index(&f->h, APR_NODE_SOURCE, SRC(f), 0);

    if (si >= 0 && focus_node(&f->h, si)) accel(&f->h, APR_CMD_CONNECT);
}

/* ==========================================================================
 * Does an announcement reach the accessibility tree at all?
 * ======================================================================== */

TEST(the_status_bar_carries_the_last_announcement_as_its_accessible_name)
{
    /* THE QUESTION NO OTHER SUITE ASKS.
     *
     * say() writes the sentence into the status bar and raises
     * EVENT_OBJECT_LIVEREGIONCHANGED. A reader that honours that event reads
     * the element's NAME -- NVDA's handler for it is ui.message(self.name) --
     * so if the name is a fixed noun like "Status", every announcement this
     * product makes is inert: the event fires, the reader says a noun, and the
     * sentence is never heard. That is indistinguishable from silence, which
     * is what the author reported for three separate defects.
     *
     * Reading the buffer back proves nothing about this. Only the tree does. */
    Fix f;
    UiaClient c;
    wchar_t want[512], name[512];
    int live;

    if (!fix_up(&f, 1, 0, 0, 0)) { fix_down(&f); return; }

    if (!uia_open(&c)) {
        printf("      SKIPPED: no UI Automation on this machine\n");
        fix_down(&f);
        return;
    }
    ASSERT_NOT_NULL(f.h.status);

    live = uia_live_setting(&c, f.h.status);
    printf("      UIA_LiveSettingPropertyId on the status bar: %d "
           "(-1 = the element does not carry it)\n", live);

    announce_something(&f);

    sentence(APR_S_UI_ANN_CONNECT_START, L"Teams", NULL, want, 512);
    uia_name(&c, f.h.status, name, 512);
    printf("      status bar name: \"%ls\"\n", name);
    printf("      the sentence:    \"%ls\"\n", want);
    ASSERT_WSTR_EQ(want, name);

    uia_close(&c);
    fix_down(&f);
}

TEST(an_announcement_raises_a_live_region_event_carrying_the_sentence)
{
    /* And the event itself: raised, on the status bar, with the sentence
     * readable through MSAA at the moment it fires. Both halves matter -- an
     * event carrying the wrong text is silence with extra steps. */
    Fix f;
    HWINEVENTHOOK hook;
    wchar_t want[512];

    if (!fix_up(&f, 1, 0, 0, 0)) { fix_down(&f); return; }

    hook = watch_begin(f.h.status, EVENT_OBJECT_LIVEREGIONCHANGED);
    if (!hook) {
        printf("      SKIPPED: SetWinEventHook refused\n");
        fix_down(&f);
        return;
    }

    announce_something(&f);

    ASSERT_TRUE(watch_wait(5000));
    printf("      %ld live-region event(s); the name a reader would speak: "
           "\"%ls\"\n", (long)g_hits, g_hit_name);

    sentence(APR_S_UI_ANN_CONNECT_START, L"Teams", NULL, want, 512);
    ASSERT_WSTR_EQ(want, g_hit_name);

    watch_end(hook);
    fix_down(&f);
}

/* ==========================================================================
 * The operations, driven and heard
 * ======================================================================== */

TEST(adding_a_source_changes_the_model_and_names_what_was_added)
{
    Fix f;
    wchar_t got[512], want[512];

    if (!fix_up(&f, 1, 0, 0, 0)) { fix_down(&f); return; }

    ASSERT_EQ_INT(1, (int)apr_graph_source_count(f.g));
    /* And the canvas grew a node for it without anyone asking it to. */
    ASSERT_EQ_INT(1, (int)apr_canvas_node_count(f.h.canvas));

    sentence(APR_S_UI_DLG_SOURCE_ADDED, L"Teams", NULL, want, 512);
    printf("      said: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);

    fix_down(&f);
}

TEST(adding_a_bus_through_its_dialog_changes_the_model_and_names_it)
{
    /* The whole route: the accelerator, the dialog template that returned -1
     * for a fortnight while every call site read that as "cancelled", the edit
     * field, OK, the model, and the sentence. No test had ever opened a dialog
     * at all. */
    Fix f;
    HWND dlg;
    wchar_t got[512], want[512];

    if (!fix_up(&f, 0, 0, 0, 0)) { fix_down(&f); return; }

    accel_async(&f.h, APR_CMD_ADD_BUS);
    dlg = wait_for_dialog(&f.h, 10000);
    if (!dlg) { fix_down(&f); FAIL("Ctrl+2 opened no dialog"); }

    ASSERT_TRUE(SetDlgItemTextW(dlg, IDC_NAME, L"Main Mix") != 0);
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    ASSERT_TRUE(wait_for_no_dialog(&f.h, 10000));

    /* The command handler runs after EndDialog returns; one round trip through
     * the frame is enough to know it has. */
    (void)SendMessageW(f.h.frame, WM_NULL, 0, 0);

    ASSERT_EQ_INT(1, (int)apr_graph_bus_count(f.g));
    sentence(APR_S_UI_DLG_BUS_ADDED, L"Main Mix", NULL, want, 512);
    printf("      said: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);

    fix_down(&f);
}

TEST(the_two_step_connect_gesture_connects_what_the_user_started_from)
{
    /* THE AUTHOR'S REPORT, EXACTLY: "tried ctrl+e on a source, then move to
     * the bus, then ctrl+e, nothing happens."
     *
     * Ctrl+E is a FRAME accelerator, so both presses go frame -> controller ->
     * canvas, and the controller refreshes both views in between. That is the
     * only difference between this and the canvas suite's own connect case,
     * which calls the canvas directly and never sees the refresh -- and it is
     * the whole defect. */
    Fix f;
    wchar_t got[512], want[512];
    int si, bi;

    if (!fix_up(&f, 1, 1, 0, 0)) { fix_down(&f); return; }

    si = node_index(&f.h, APR_NODE_SOURCE, SRC(&f), 0);
    ASSERT_GE_INT(0, si);
    ASSERT_TRUE(focus_node(&f.h, si));
    accel(&f.h, APR_CMD_CONNECT);

    sentence(APR_S_UI_ANN_CONNECT_START, L"Teams", NULL, want, 512);
    printf("      press 1: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);

    /* The half-made gesture has to SURVIVE whatever the frame does to its
     * views between the two presses. If it does not, the second press is a
     * first press on a bus and the connection never happens -- silently, from
     * the user's side. */
    ASSERT_NOT_NULL(apr_canvas_pending_node(f.h.canvas));

    /* Focus moves, exactly as the author moved it. */
    bi = node_index(&f.h, APR_NODE_BUS, BUS(&f), 0);
    ASSERT_GE_INT(0, bi);
    ASSERT_TRUE(focus_node(&f.h, bi));
    ASSERT_TRUE(apr_canvas_focused_node(f.h.canvas) ==
                apr_canvas_node_at(f.h.canvas, (size_t)bi));

    accel(&f.h, APR_CMD_CONNECT);

    ASSERT_TRUE(apr_graph_connected(f.g, SRC(&f), BUS(&f)) != 0);
    sentence(APR_S_UI_ANN_CONNECT_DONE, L"Teams", L"Main Mix", want, 512);
    printf("      press 2: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);

    fix_down(&f);
}

TEST(the_two_step_disconnect_gesture_takes_the_edge_away_and_says_so)
{
    Fix f;
    wchar_t got[512], want[512];
    int si, bi;

    if (!fix_up(&f, 1, 1, 1, 0)) { fix_down(&f); return; }
    ASSERT_TRUE(apr_graph_connected(f.g, SRC(&f), BUS(&f)) != 0);

    si = node_index(&f.h, APR_NODE_SOURCE, SRC(&f), 0);
    ASSERT_TRUE(focus_node(&f.h, si));
    accel(&f.h, APR_CMD_DISCONNECT);

    sentence(APR_S_UI_ANN_DISCONNECT_START, L"Teams", NULL, want, 512);
    printf("      press 1: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);
    ASSERT_NOT_NULL(apr_canvas_pending_node(f.h.canvas));

    bi = node_index(&f.h, APR_NODE_BUS, BUS(&f), 0);
    ASSERT_TRUE(focus_node(&f.h, bi));
    accel(&f.h, APR_CMD_DISCONNECT);

    ASSERT_FALSE(apr_graph_connected(f.g, SRC(&f), BUS(&f)) != 0);
    sentence(APR_S_UI_ANN_DISCONNECT_DONE, L"Teams", L"Main Mix", want, 512);
    printf("      press 2: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);

    fix_down(&f);
}

TEST(the_plus_key_raises_the_level_on_the_edge_and_says_the_new_level)
{
    /* A real WM_KEYDOWN at the focused node, which is what the message loop
     * delivers. + and - reach no accelerator table, so this is the only route
     * there is -- and the announcement is the only feedback there is, because
     * a level has no other shape a listener can perceive. */
    Fix f;
    wchar_t got[512], want[512], db[32];
    AprBus *b;
    float g0, g1;
    int si;

    if (!fix_up(&f, 1, 1, 1, 0)) { fix_down(&f); return; }

    b = apr_graph_bus(f.g, BUS(&f));
    ASSERT_NOT_NULL(b);
    g0 = apr_bus_gain(b, SRC(&f));

    si = node_index(&f.h, APR_NODE_SOURCE, SRC(&f), 0);
    ASSERT_TRUE(focus_node(&f.h, si));
    keydown(apr_canvas_focused_node(f.h.canvas), VK_ADD);

    g1 = apr_bus_gain(b, SRC(&f));
    printf("      gain %f -> %f\n", (double)g0, (double)g1);
    ASSERT_TRUE(g1 > g0);

    /* One press from unity is exactly one step up, and the sentence says so. */
    apr_str_number_fixed(APR_CANVAS_GAIN_STEP_DB10, 1, db, 32);
    sentence(APR_S_UI_ANN_GAIN, L"Teams", db, want, 512);
    printf("      said: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);

    fix_down(&f);
}

TEST(delete_removes_the_focused_node_and_names_what_went)
{
    Fix f;
    wchar_t got[512], want[512];
    int si;

    if (!fix_up(&f, 1, 1, 0, 0)) { fix_down(&f); return; }

    si = node_index(&f.h, APR_NODE_SOURCE, SRC(&f), 0);
    ASSERT_TRUE(focus_node(&f.h, si));
    accel(&f.h, APR_CMD_REMOVE);

    ASSERT_EQ_INT(0, (int)apr_graph_source_count(f.g));
    ASSERT_EQ_INT(1, (int)apr_canvas_node_count(f.h.canvas));   /* the bus */

    sentence(APR_S_UI_ANN_REMOVED, L"Teams", NULL, want, 512);
    printf("      said: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);

    fix_down(&f);
}

/* ==========================================================================
 * RECORDING TWICE, WHICH IS WHERE THE AUTHOR LOST A FILE
 *
 *   "I recorded a file called test.mp3, stopped, listened to it, and then
 *   recorded again and stopped. Turns out the file was not updated with the
 *   new recording, so I deleted it and recorded again, but the new file
 *   wasn't there."
 *
 * Every case in this suite before these ones asserted that pressing a key
 * changed the model and said something. None of them ever pressed RECORD --
 * so the one operation the product exists for was the one nothing drove, and
 * a defect that cost the author two takes walked straight through.
 *
 * SAFETY (AGENTS.md rule 1): the source is APR_SRC_FAKE, the output is a WAV
 * file under TEMP, and no audio endpoint is opened in either direction. Every
 * file made here is deleted before the case returns.
 * ======================================================================== */

/* The same playability check test_cli.c makes: RIFF/WAVE, a data chunk, and a
 * data size that is non-zero and consistent with the file on disk. */
static int wav_is_playable(const wchar_t *path, unsigned *out_data_bytes)
{
    unsigned char buf[4096];
    HANDLE   h;
    DWORD    got = 0;
    uint64_t size;
    size_t   i;
    WIN32_FILE_ATTRIBUTE_DATA fa;

    if (out_data_bytes) *out_data_bytes = 0;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return 0;
    size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, buf, (DWORD)sizeof buf, &got, NULL)) got = 0;
    CloseHandle(h);
    if (got < 64) return 0;
    if (memcmp(buf, "RIFF", 4) != 0 && memcmp(buf, "RF64", 4) != 0) return 0;
    if (memcmp(buf + 8, "WAVE", 4) != 0) return 0;

    for (i = 12; i + 8 <= (size_t)got; i += 4) {
        if (memcmp(buf + i, "data", 4) == 0) {
            unsigned n = (unsigned)buf[i + 4] | ((unsigned)buf[i + 5] << 8) |
                         ((unsigned)buf[i + 6] << 16) | ((unsigned)buf[i + 7] << 24);
            if (out_data_bytes) *out_data_bytes = n;
            return n > 0 && (uint64_t)n + i + 8 <= size;
        }
    }
    return 0;
}

/* "mix.wav" -> "mix-2.wav": what the collision policy does with a name that is
 * already a recording (outpath.h). */
static void sibling_take(const wchar_t *path, int take, wchar_t *out, size_t cch)
{
    const wchar_t *dot = wcsrchr(path, L'.');
    size_t stem = dot ? (size_t)(dot - path) : wcslen(path);
    _snwprintf_s(out, cch, _TRUNCATE, L"%.*ls-%d%ls", (int)stem, path, take,
                 dot ? dot : L"");
}

/* One take: press Record, let it run, press Stop, wait for the files to close.
 * Both presses are the real accelerator; the wait is on the controller's own
 * "am I recording" state rather than on a sleep, because the runner closes the
 * files on a thread of its own and a take that is still being finalized is a
 * file that is not finished. */
static int one_take(Fix *f, int ms)
{
    int waited = 0;

    accel(&f->h, APR_CMD_RECORD_START);
    if (!apr_controller_recording(f->h.ctl)) return 0;

    Sleep((DWORD)ms);
    accel(&f->h, APR_CMD_RECORD_STOP);

    /* The controller clears `recording` from APR_RUN_EV_STOPPED, which arrives
     * on the window's own thread -- so this really is "the files are closed"
     * and not "the stop was requested". */
    while (apr_controller_recording(f->h.ctl)) {
        if (waited >= 20000) return 0;
        Sleep(10);
        waited += 10;
    }
    return 1;
}

TEST(recording_twice_through_one_graph_leaves_two_playable_files)
{
    /* BUG 1 AND BUG 2 TOGETHER. Before the lifetime fix the encoder was created
     * when the OUTPUT was added and finalized for good at the first stop, so
     * take two wrote nothing at all. With the lifetime fixed but nothing else,
     * take two would have landed on top of take one. */
    Fix f;
    wchar_t second[MAX_PATH];
    unsigned a = 0, b = 0;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }
    sibling_take(f.h.setup.out_path, 2, second, MAX_PATH);
    DeleteFileW(second);

    /* Adding the output created NOTHING: the file appears when recording
     * starts, which is the whole of the lifetime fix. */
    ASSERT_EQ_INT(1, (int)apr_bus_action_count(apr_graph_bus(f.g, BUS(&f))));
    ASSERT_EQ_INT((int)INVALID_FILE_ATTRIBUTES,
                  (int)GetFileAttributesW(f.h.setup.out_path));

    if (!one_take(&f, 400)) { fix_down(&f); DeleteFileW(second); FAIL("take one did not run"); }
    ASSERT_TRUE(wav_is_playable(f.h.setup.out_path, &a));
    printf("      take 1: [%ls] %u bytes\n", f.h.setup.out_path, a);

    if (!one_take(&f, 400)) { fix_down(&f); DeleteFileW(second); FAIL("take two did not run"); }

    /* Two files. Take one still holds take one. */
    ASSERT_TRUE(wav_is_playable(f.h.setup.out_path, &a));
    ASSERT_TRUE(wav_is_playable(second, &b));
    printf("      take 2: [%ls] %u bytes\n", second, b);
    ASSERT_GT_INT(40000, (int)b);

    /* And the graph says where the second one went, which is what the tree
     * panel and the announcement read. */
    ASSERT_WSTR_EQ(second,
        apr_bus_action_current_path(apr_graph_bus(f.g, BUS(&f)), 0));
    /* While what the user CONFIGURED is unchanged -- that is what a session
     * save writes down. */
    ASSERT_WSTR_EQ(f.h.setup.out_path,
        apr_bus_action_path(apr_graph_bus(f.g, BUS(&f)), 0));

    fix_down(&f);
    DeleteFileW(second);
}

TEST(a_take_deleted_between_recordings_comes_back)
{
    /* The third act of the report, and the strangest-looking one: deleting the
     * file did not help. Windows keeps an open handle valid with no directory
     * entry, so the next recording went on writing to a file with no name.
     * Nothing is open between recordings now, so the name is simply free. */
    Fix f;
    wchar_t second[MAX_PATH];
    unsigned bytes = 0;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }
    sibling_take(f.h.setup.out_path, 2, second, MAX_PATH);
    DeleteFileW(second);

    if (!one_take(&f, 400)) { fix_down(&f); DeleteFileW(second); FAIL("take one did not run"); }
    ASSERT_TRUE(wav_is_playable(f.h.setup.out_path, &bytes));

    /* THE DELETE HAS TO SUCCEED, and before the fix it could not have: a
     * handle was still open on it. */
    ASSERT_TRUE(DeleteFileW(f.h.setup.out_path) != 0);

    if (!one_take(&f, 400)) { fix_down(&f); DeleteFileW(second); FAIL("take two did not run"); }
    ASSERT_TRUE(wav_is_playable(f.h.setup.out_path, &bytes));
    printf("      back:   [%ls] %u bytes\n", f.h.setup.out_path, bytes);
    ASSERT_GT_INT(40000, (int)bytes);
    /* The name was free, so no take number was needed. */
    ASSERT_EQ_INT((int)INVALID_FILE_ATTRIBUTES, (int)GetFileAttributesW(second));

    fix_down(&f);
    DeleteFileW(second);
}

TEST(a_take_that_moved_aside_is_announced_while_it_is_happening)
{
    /* AUTO-INCREMENT IS ONLY HONEST IF THE USER HEARS ABOUT IT. The sentence
     * is caught DURING the second take rather than after it: the stop
     * announcement is the last thing said, so looking afterwards would find
     * that instead and prove nothing.
     *
     * Compared against the catalog sentence with the catalog's own insert,
     * like every other announcement in this file. */
    Fix f;
    wchar_t second[MAX_PATH], want[512], got[512];
    int waited = 0, heard = 0;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }
    sibling_take(f.h.setup.out_path, 2, second, MAX_PATH);
    DeleteFileW(second);

    if (!one_take(&f, 200)) { fix_down(&f); DeleteFileW(second); FAIL("take one did not run"); }

    sentence(APR_S_UI_ANN_OUTPUT_RENAMED, second, NULL, want, 512);
    printf("      wanted: \"%ls\"\n", want);

    accel(&f.h, APR_CMD_RECORD_START);
    while (waited < 5000) {
        if (wcscmp(want, said(&f.h, got, 512)) == 0) { heard = 1; break; }
        Sleep(10);
        waited += 10;
    }
    printf("      said:   \"%ls\"\n", got);

    accel(&f.h, APR_CMD_RECORD_STOP);
    while (apr_controller_recording(f.h.ctl) && waited < 25000) {
        Sleep(10);
        waited += 10;
    }
    ASSERT_TRUE(heard);

    fix_down(&f);
    DeleteFileW(second);
}

TEST(an_output_whose_folder_is_not_there_is_refused_when_it_is_added)
{
    /* THE EARLY CHECK, AND WHY IT HAD TO MOVE RATHER THAN GO. Opening the file
     * used to be what caught an unwritable path, and it happened at add time.
     * The open now happens at record time, so the CHECK was kept where it was:
     * apr_bus_add_action validates the name through apr_out_validate (bus.h).
     * The person who typed the name still finds out while they are standing
     * there, and nothing is created either way. */
    Fix f;
    AprActionConfig ac;
    wchar_t dir[MAX_PATH], bad[MAX_PATH];
    AprErr e;
    DWORD n;

    if (!fix_up(&f, 1, 1, 1, 0)) { fix_down(&f); return; }

    n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) { fix_down(&f); FAIL("no temp folder"); }
    _snwprintf_s(bad, MAX_PATH, _TRUNCATE, L"%lsapr_no_such_dir_%lu\\{bus}.{ext}",
                 dir, GetCurrentProcessId());

    memset(&ac, 0, sizeof ac);
    ac.out_path = bad;
    e = apr_graph_add_action(f.g, BUS(&f), "wav", &ac);
    {
        wchar_t why[512];
        printf("      refused: %ls\n", apr_err_format(&e, why, 512));
    }
    ASSERT_TRUE(apr_failed(&e));

    /* Refused means refused: no output on the bus, and nothing on disk. */
    ASSERT_EQ_INT(0, (int)apr_bus_action_count(apr_graph_bus(f.g, BUS(&f))));
    ASSERT_EQ_INT((int)INVALID_FILE_ATTRIBUTES, (int)GetFileAttributesW(bad));

    fix_down(&f);
}

TEST(a_writable_name_is_accepted_at_add_time_and_still_creates_nothing)
{
    /* The other half of the same rule. Validation must not be an open: adding
     * an output leaves the folder exactly as it found it, so a session can
     * carry ten outputs without ten empty files appearing beside it. */
    Fix f;
    AprActionConfig ac;
    wchar_t dir[MAX_PATH], tmpl[MAX_PATH], expect[MAX_PATH];
    AprOutContext ctx;
    AprErr e;
    DWORD n;

    if (!fix_up(&f, 1, 1, 1, 0)) { fix_down(&f); return; }

    n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) { fix_down(&f); FAIL("no temp folder"); }
    _snwprintf_s(tmpl, MAX_PATH, _TRUNCATE, L"%lsapr_add_%lu_{bus}.{ext}",
                 dir, GetCurrentProcessId());

    memset(&ac, 0, sizeof ac);
    ac.out_path = tmpl;
    e = apr_graph_add_action(f.g, BUS(&f), "wav", &ac);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(1, (int)apr_bus_action_count(apr_graph_bus(f.g, BUS(&f))));

    ctx.bus_name  = L"Main Mix";
    ctx.extension = L"wav";
    e = apr_out_expand(tmpl, &ctx, expect, MAX_PATH);
    ASSERT_FALSE(apr_failed(&e));
    printf("      would be: [%ls]\n", expect);
    /* Neither the template nor its expansion exists yet. */
    ASSERT_EQ_INT((int)INVALID_FILE_ATTRIBUTES, (int)GetFileAttributesW(tmpl));
    ASSERT_EQ_INT((int)INVALID_FILE_ATTRIBUTES, (int)GetFileAttributesW(expect));

    fix_down(&f);
    DeleteFileW(expect);
}


/* ==========================================================================
 * THE STRUCTURE PANEL, DRIVEN BY THE KEYBOARD, WITH A REAL CONTROLLER BEHIND IT
 *
 * Two sweeps found this independently and no suite could have caught it:
 * test_ui_tree.c drives a live tree with NO controller wired to it, and this
 * suite had never opened the tree at all. The wire between the two was the one
 * thing nothing tested, and the defect is in the wire.
 * ======================================================================== */

/* Focus, as the thread that owns the window sees it. GetGUIThreadInfo rather
 * than UIA's GetFocusedElement: the latter is per-desktop and needs the window
 * to be foreground, which a test launched by a build system is not. */
static HWND focus_on_ui_thread(UiHost *h)
{
    GUITHREADINFO gti;

    memset(&gti, 0, sizeof gti);
    gti.cbSize = sizeof gti;
    if (!GetGUIThreadInfo(GetThreadId(h->thread), &gti)) return NULL;
    return gti.hwndFocus;
}

/* Hand focus to the frame and wait for it to reach the tree, the way it does
 * for a user arriving at the window. */
static int focus_the_tree(UiHost *h)
{
    int round, t;

    /* THE FRAME HAS TO BE ON SCREEN FIRST. cycle_pane only hands focus to a
     * VISIBLE pane, and a child of a window that was never shown is not one --
     * so a fixture that never calls ShowWindow can never put focus in a pane,
     * which is exactly the state this suite was in. SW_SHOWNOACTIVATE: real
     * and visible, without taking the foreground from whoever is at the
     * machine. */
    ShowWindow(h->frame, SW_SHOWNOACTIVATE);

    /* F6, WHICH IS THE AUTHOR'S OWN GESTURE. Posted into the UI thread's queue
     * so it travels through apr_ui_app_run's pre-translate filter and reaches
     * cycle_pane on the thread that owns the windows -- a SetFocus from here
     * would do nothing, silently, and a synthetic WM_SETFOCUS does not move
     * focus at all. F6 cycles, so a few presses reach whichever pane. */
    for (round = 0; round < APR_PANE_COUNT + 1; ++round) {
        PostMessageW(h->frame, WM_KEYDOWN, VK_F6, 0);
        PostMessageW(h->frame, WM_KEYUP, VK_F6, 0);
        for (t = 0; t < 40; ++t) {
            if (focus_on_ui_thread(h) == h->tv) return 1;
            Sleep(15);
        }
    }
    return 0;
}

/* The row the caret is on, as its lParam index, or -1. */
static int tv_caret_row(HWND tv)
{
    HTREEITEM cur = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_CARET, 0);
    TVITEMW it;

    if (!cur) return -1;
    memset(&it, 0, sizeof it);
    it.mask = TVIF_PARAM;
    it.hItem = cur;
    if (!SendMessageW(tv, TVM_GETITEMW, 0, (LPARAM)&it)) return -1;
    return (int)it.lParam;
}

/* Every item's text, in display order, so the LIVE control can be compared
 * against what the model says it should be saying. */
static size_t tv_texts(HWND tv, wchar_t out[][512], size_t cap)
{
    HTREEITEM stack[256];
    size_t n = 0, top = 0;
    HTREEITEM it;

    it = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_ROOT, 0);
    while (it || top > 0) {
        TVITEMW q;
        HTREEITEM kid, next;

        if (!it) { it = stack[--top]; continue; }

        memset(&q, 0, sizeof q);
        q.mask = TVIF_TEXT;
        q.hItem = it;
        q.pszText = out[n < cap ? n : 0];
        q.cchTextMax = 512;
        if (n < cap && SendMessageW(tv, TVM_GETITEMW, 0, (LPARAM)&q)) n++;

        next = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_NEXT,
                                       (LPARAM)it);
        kid = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_CHILD,
                                      (LPARAM)it);
        if (kid) {
            if (next && top < 256) stack[top++] = next;
            it = kid;
        } else {
            it = next;
        }
    }
    return n;
}

TEST(arrowing_down_the_structure_panel_does_not_yank_focus_out_of_it)
{
    /* THE AUTHOR CANNOT REACH PAST THE FIRST ROW.
     *
     * The tree's selection sink fires on EVERY caret move, and the controller
     * answered it with an unconditional SetFocus on the matching canvas node.
     * So: F6 into the tree, press Down, and focus is yanked to the canvas --
     * the reader announces the canvas node instead of the row, and the next
     * Down drives the canvas. Everything past the first row is unreachable,
     * and these rows' sentences are the entire purpose of the panel. */
    Fix f;
    int visited[APR_TREE_MAX_ROWS];
    size_t want_rows;
    int i, tries, reached = 0;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }
    if (!f.h.tv) {
        printf("      SKIPPED: no tree panel in this build\n");
        fix_down(&f);
        return;
    }

    want_rows = apr_tree_panel_rows(f.g, NULL, 0);
    ASSERT_GT_INT(1, (int)want_rows);   /* a one-row tree would prove nothing */
    memset(visited, 0, sizeof visited);

    if (!focus_the_tree(&f.h)) {
        printf("      SKIPPED: focus never reached the tree control\n");
        fix_down(&f);
        return;
    }

    SendMessageW(f.h.tv, WM_KEYDOWN, VK_HOME, 0);
    SendMessageW(f.h.tv, WM_KEYUP, VK_HOME, 0);

    for (tries = 0; tries < (int)want_rows * 3 + 8; ++tries) {
        HWND focus = focus_on_ui_thread(&f.h);
        int row;

        /* THE ASSERTION THAT FAILS WITHOUT THE FIX, and it fails on the very
         * first Down: after moving the caret, the keyboard must still be in
         * the tree. */
        if (focus != f.h.tv) {
            printf("      focus left the tree after %d moves: %p "
                   "(tree %p, canvas %p)\n", tries, (void *)focus,
                   (void *)f.h.tv, (void *)f.h.canvas);
        }
        ASSERT_TRUE(focus == f.h.tv);

        row = tv_caret_row(f.h.tv);
        if (row >= 0 && row < APR_TREE_MAX_ROWS && !visited[row]) {
            visited[row] = 1;
            reached++;
        }
        SendMessageW(f.h.tv, WM_KEYDOWN, VK_DOWN, 0);
        SendMessageW(f.h.tv, WM_KEYUP, VK_DOWN, 0);
    }

    printf("      Down Arrow reached %d of %d rows without losing focus\n",
           reached, (int)want_rows);
    for (i = 0; i < (int)want_rows; ++i) ASSERT_TRUE(visited[i]);

    fix_down(&f);
}

TEST(the_caret_moves_the_canvas_quietly_and_only_enter_takes_the_keyboard_there)
{
    /* The other half of the same fix. Moving the caret must still keep the two
     * views AGREEING -- the canvas's current node follows the tree, so F6 back
     * to the canvas lands where the user was standing -- and ACTIVATING a row
     * is what actually goes there. */
    Fix f;
    AprTreeSel sel;
    int bus_node, t;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }
    if (!f.h.tv) { printf("      SKIPPED: no tree panel\n"); fix_down(&f); return; }
    if (!focus_the_tree(&f.h)) {
        printf("      SKIPPED: focus never reached the tree control\n");
        fix_down(&f);
        return;
    }

    bus_node = node_index(&f.h, APR_NODE_BUS, BUS(&f), 0);
    ASSERT_GE_INT(0, bus_node);

    /* The caret on the first row, which apr_tree_panel_rows makes the bus. */
    SendMessageW(f.h.tv, WM_KEYDOWN, VK_HOME, 0);
    SendMessageW(f.h.tv, WM_KEYUP, VK_HOME, 0);
    Sleep(50);

    ASSERT_EQ_INT(1, apr_tree_panel_get_selection(f.h.tree, &sel));
    ASSERT_EQ_INT((int)APR_TREE_ROW_BUS, (int)sel.kind);

    /* The canvas agrees -- without having taken the keyboard. */
    ASSERT_TRUE(apr_canvas_focused_node(f.h.canvas) ==
                apr_canvas_node_at(f.h.canvas, (size_t)bus_node));
    ASSERT_TRUE(focus_on_ui_thread(&f.h) == f.h.tv);

    /* NOW activate it. IsDialogMessage eats Enter unless the control claims
     * it, so this also proves the claim works. */
    SendMessageW(f.h.tv, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(f.h.tv, WM_KEYUP, VK_RETURN, 0);
    for (t = 0; t < 60; ++t) {
        if (focus_on_ui_thread(&f.h) ==
            apr_canvas_node_at(f.h.canvas, (size_t)bus_node)) {
            break;
        }
        Sleep(15);
    }
    printf("      after Enter, focus is %p (the bus node is %p)\n",
           (void *)focus_on_ui_thread(&f.h),
           (void *)apr_canvas_node_at(f.h.canvas, (size_t)bus_node));
    ASSERT_TRUE(focus_on_ui_thread(&f.h) ==
                apr_canvas_node_at(f.h.canvas, (size_t)bus_node));

    fix_down(&f);
}

/* ==========================================================================
 * "GREYED AND SPOKEN" -- THE SPOKEN HALF, WHICH HAD NEVER RUN
 * ======================================================================== */

TEST(an_editing_key_pressed_while_recording_says_why_it_was_refused)
{
    /* THE DEFECT CLASS THAT BURNED THIS AUTHOR FOUR TIMES THIS WEEK: the
     * design was right and the delivery was silently absent.
     *
     * ui_controller.h promises that an editing command refused during a
     * recording is "greyed AND spoken, because grey alone says nothing to this
     * application's first user". TranslateAccelerator does not send WM_COMMAND
     * for an accelerator whose menu item is disabled -- it swallows the key and
     * delivers nothing -- so busy() never ran, and the sentence at
     * UI_ANN_BUSY_RECORDING had never once played. Mid-recording the key was
     * TOTAL SILENCE, which for this author is a broken application.
     *
     * The key is POSTED into the UI thread's own queue, so it travels through
     * apr_ui_app_run and the real pre-translate filter -- not as a WM_COMMAND,
     * which is precisely the message that never arrived. F2 and Delete carry
     * no modifier, and a synthetic message does not move the keyboard state,
     * so the modifier read is 0 and deterministic. */
    Fix f;
    wchar_t got[512], want[512];
    int waited;
    static const struct { UINT vk; int cmd; const char *what; } keys[] = {
        { VK_F2,     APR_CMD_RENAME_BUS, "F2 (rename bus)" },
        { VK_DELETE, APR_CMD_REMOVE,     "Delete (remove node)" }
    };
    size_t k;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }

    /* The binding and the menu state are separate questions now, and both have
     * to hold: the key IS bound, and the item IS greyed. */
    for (k = 0; k < sizeof keys / sizeof keys[0]; ++k) {
        ASSERT_EQ_INT(keys[k].cmd, apr_ui_app_accel_command(keys[k].vk, 0));
    }

    accel(&f.h, APR_CMD_RECORD_START);
    if (!apr_controller_recording(f.h.ctl)) {
        fix_down(&f);
        FAIL("the recording did not start");
    }

    apr_str_format(APR_S_UI_ANN_BUSY_RECORDING, want, 512, NULL, 0);

    for (k = 0; k < sizeof keys / sizeof keys[0]; ++k) {
        /* Greyed. */
        ASSERT_FALSE(apr_ui_app_command_enabled(f.h.app, keys[k].cmd));

        /* And SPOKEN. Something else is said first, so a stale sentence
         * cannot pass for this one. */
        SendMessageW(f.h.frame, WM_COMMAND,
                     MAKEWPARAM(APR_CMD_RECORD_START, 1), 0);

        PostMessageW(f.h.frame, WM_KEYDOWN, (WPARAM)keys[k].vk, 0);
        PostMessageW(f.h.frame, WM_KEYUP, (WPARAM)keys[k].vk, 0);

        for (waited = 0; waited < 5000; waited += 10) {
            if (wcscmp(want, said(&f.h, got, 512)) == 0) break;
            Sleep(10);
        }
        printf("      %-22s said: \"%ls\"\n", keys[k].what, got);
        ASSERT_WSTR_EQ(want, got);
    }

    accel(&f.h, APR_CMD_RECORD_STOP);
    for (waited = 0; waited < 25000 && apr_controller_recording(f.h.ctl);
         waited += 10) {
        Sleep(10);
    }

    fix_down(&f);
}

TEST(a_canvas_key_that_would_rewire_a_running_graph_is_refused_in_words)
{
    /* C4's UI half. Disconnect is deliberately NOT a frame accelerator -- the
     * canvas claims Ctrl+Shift+E through its own binding table -- so it
     * bypassed the controller's busy() check entirely and reached the model as
     * a raw keystroke while the runner was iterating the same edge arrays.
     * This asserts the user HEARS the refusal rather than watching the gesture
     * quietly do nothing.
     *
     * Driven through the canvas's own message form (ui_canvas.h), which runs
     * exactly the function a keystroke runs, on the thread it requires. */
    Fix f;
    wchar_t got[512], want[512];
    int si, waited;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }
    ASSERT_TRUE(apr_graph_connected(f.g, SRC(&f), BUS(&f)) != 0);

    si = node_index(&f.h, APR_NODE_SOURCE, SRC(&f), 0);
    ASSERT_TRUE(focus_node(&f.h, si));

    accel(&f.h, APR_CMD_RECORD_START);
    if (!apr_controller_recording(f.h.ctl)) {
        fix_down(&f);
        FAIL("the recording did not start");
    }

    SendMessageW(f.h.canvas, APR_CANVAS_WM_PERFORM,
                 (WPARAM)APR_CANVAS_OP_DISCONNECT, 0);

    apr_str_format(APR_S_UI_ANN_BUSY_RECORDING, want, 512, NULL, 0);
    printf("      disconnect said: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);
    /* Refused means refused: no half-made gesture left behind, and nothing in
     * the model moved. */
    ASSERT_TRUE(apr_canvas_pending_node(f.h.canvas) == NULL);
    ASSERT_TRUE(apr_graph_connected(f.g, SRC(&f), BUS(&f)) != 0);

    /* The level keys take the same route and get the same answer. */
    SendMessageW(f.h.canvas, APR_CANVAS_WM_PERFORM,
                 (WPARAM)APR_CANVAS_OP_GAIN_UP, 0);
    printf("      plus said:       \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);

    accel(&f.h, APR_CMD_RECORD_STOP);
    for (waited = 0; waited < 25000 && apr_controller_recording(f.h.ctl);
         waited += 10) {
        Sleep(10);
    }

    fix_down(&f);
}

/* ==========================================================================
 * THE CLOSE THAT LEFT THE PROCESS BEHIND
 * ======================================================================== */

TEST(stop_recording_and_close_actually_ends_the_process)
{
    /* THE NORMAL CASE OF THE CLOSE PATH, AND IT NEVER EXITED.
     *
     * "Stop recording and close" pumps messages while the encoders flush --
     * which is right, because a frozen window is a window a screen reader
     * cannot read. But the drain dispatched EVERY message with no WM_QUIT
     * check: recording_finished posts WM_CLOSE, the same drain dispatches it,
     * DestroyWindow runs, WM_DESTROY calls PostQuitMessage -- and then the
     * drain retrieved that WM_QUIT and dropped it. Window gone, process alive,
     * apr_controller_destroy never run, tray icon ghosted, and a second launch
     * coexisting with the first.
     *
     * The assertion is simply: does the UI thread END. */
    Fix f;
    HWND dlg;
    DWORD wait;
    int waited;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }

    accel(&f.h, APR_CMD_RECORD_START);
    if (!apr_controller_recording(f.h.ctl)) {
        fix_down(&f);
        FAIL("the recording did not start");
    }
    Sleep(200);

    PostMessageW(f.h.frame, WM_CLOSE, 0, 0);
    dlg = wait_for_dialog(&f.h, 15000);
    if (!dlg) {
        accel(&f.h, APR_CMD_RECORD_STOP);
        for (waited = 0; waited < 25000 && apr_controller_recording(f.h.ctl);
             waited += 10) {
            Sleep(10);
        }
        fix_down(&f);
        FAIL("closing while recording asked no question");
    }

    /* "Stop the recording and close" is the default button (dialogs.c). */
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);

    wait = WaitForSingleObject(f.h.thread, 30000);
    printf("      the UI thread ended: %s\n",
           wait == WAIT_OBJECT_0 ? "yes"
                                 : "NO -- the process would be a zombie");
    if (wait != WAIT_OBJECT_0) {
        fix_down(&f);
        FAIL("the window closed but the message loop never ended");
    }

    /* And the take is still a real file: closing must never cost a recording. */
    ASSERT_TRUE(wav_is_playable(f.h.setup.out_path, NULL));

    fix_down(&f);
}

/* ==========================================================================
 * THE TREE PANEL AND THE MODEL, KEPT IN STEP
 * ======================================================================== */

/* The live control's rows, against what the model says they are. */
static int tree_agrees_with_model(UiHost *h, AprGraph *g)
{
    static wchar_t got[APR_TREE_MAX_ROWS][512];
    static AprTreeRow rows[APR_TREE_MAX_ROWS];
    size_t n_model, n_live, i;

    n_model = apr_tree_panel_rows(g, rows, APR_TREE_MAX_ROWS);
    n_live  = tv_texts(h->tv, got, APR_TREE_MAX_ROWS);

    if (n_model != n_live) {
        printf("      the tree shows %d rows; the model has %d\n",
               (int)n_live, (int)n_model);
        return 0;
    }
    for (i = 0; i < n_model; ++i) {
        wchar_t want[512];
        apr_tree_panel_label(g, &rows[i].sel, want, 512);
        if (wcscmp(want, got[i]) != 0) {
            printf("      row %d says    \"%ls\"\n", (int)i, got[i]);
            printf("      the model says \"%ls\"\n", want);
            return 0;
        }
    }
    return 1;
}

TEST(an_edit_the_canvas_makes_by_itself_reaches_the_tree_panel)
{
    /* apr_controller_model_changed's own header says that without it "the tree
     * panel would still be showing the old shape", and that is exactly what
     * happened: nothing in the product ever called it. Ctrl+Shift+E and a
     * mouse click completing an edge edit the graph inside the canvas's own
     * window procedure -- no accelerator, no WM_COMMAND, nothing reaching the
     * controller -- so the tree went on describing an edge that had been cut.
     * The view used to AUDIT a session reported a connection that no longer
     * existed. */
    Fix f;
    int si, bi;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }
    if (!f.h.tv) { printf("      SKIPPED: no tree panel\n"); fix_down(&f); return; }

    ASSERT_TRUE(tree_agrees_with_model(&f.h, f.g));

    /* Disconnect through the canvas's own two-step gesture -- the route that
     * never touches the controller's command handler. */
    si = node_index(&f.h, APR_NODE_SOURCE, SRC(&f), 0);
    bi = node_index(&f.h, APR_NODE_BUS, BUS(&f), 0);
    ASSERT_TRUE(focus_node(&f.h, si));
    SendMessageW(f.h.canvas, APR_CANVAS_WM_PERFORM,
                 (WPARAM)APR_CANVAS_OP_DISCONNECT, 0);
    ASSERT_TRUE(focus_node(&f.h, bi));
    SendMessageW(f.h.canvas, APR_CANVAS_WM_PERFORM,
                 (WPARAM)APR_CANVAS_OP_DISCONNECT, 0);

    ASSERT_FALSE(apr_graph_connected(f.g, SRC(&f), BUS(&f)) != 0);
    ASSERT_TRUE(tree_agrees_with_model(&f.h, f.g));

    fix_down(&f);
}

TEST(the_tree_says_a_bus_is_recording_while_it_is_recording)
{
    /* THE SENTENCES EXISTED, WERE QUEUED FOR TRANSLATION, AND COULD NEVER BE
     * HEARD WHEN TRUE. Every bus row picks UI_TREE_BUS_RECORDING over
     * UI_TREE_BUS from apr_bus_running(), and nothing rebuilt the rows when a
     * run started -- so F6 into the panel mid-session and every bus read as
     * idle for the whole recording. */
    Fix f;
    static wchar_t live[APR_TREE_MAX_ROWS][512];
    wchar_t want[512];
    AprTreeSel bus_row;
    size_t n;
    int waited, heard = 0;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }
    if (!f.h.tv) { printf("      SKIPPED: no tree panel\n"); fix_down(&f); return; }

    memset(&bus_row, 0, sizeof bus_row);
    bus_row.kind = APR_TREE_ROW_BUS;
    bus_row.bus  = BUS(&f);

    accel(&f.h, APR_CMD_RECORD_START);
    if (!apr_controller_recording(f.h.ctl)) {
        fix_down(&f);
        FAIL("the recording did not start");
    }

    /* The sentence the model produces WHILE the bus is running -- read from
     * the catalog through the panel's own pure projection, never a literal. */
    apr_tree_panel_label(f.g, &bus_row, want, 512);
    printf("      the model says: \"%ls\"\n", want);

    for (waited = 0; waited < 5000 && !heard; waited += 20) {
        size_t i;
        n = tv_texts(f.h.tv, live, APR_TREE_MAX_ROWS);
        for (i = 0; i < n; ++i) {
            if (wcscmp(want, live[i]) == 0) { heard = 1; break; }
        }
        if (!heard) Sleep(20);
    }
    if (!heard) {
        size_t i;
        n = tv_texts(f.h.tv, live, APR_TREE_MAX_ROWS);
        for (i = 0; i < n; ++i) printf("      the tree says:  \"%ls\"\n", live[i]);
    }

    accel(&f.h, APR_CMD_RECORD_STOP);
    for (waited = 0; waited < 25000 && apr_controller_recording(f.h.ctl);
         waited += 10) {
        Sleep(10);
    }

    ASSERT_TRUE(heard);
    fix_down(&f);
}

/* ==========================================================================
 * THE NOTIFICATION AREA IS A CHANNEL, NOT A DECORATION
 * ======================================================================== */

TEST(a_take_that_moved_aside_is_told_to_a_window_that_is_not_in_front)
{
    /* The honest-collision policy insists the user be TOLD when a take moves
     * aside -- and the telling landed on a status bar's live region, which no
     * reader announces on a background window. A recording started from the
     * notification area starts with the window hidden BY DEFINITION, so this
     * was the one fact the policy insists on, delivered on the one channel
     * that could not carry it.
     *
     * fg_override forces "the window is not in front", because a test cannot
     * make itself foreground reliably (ui_controller.h). */
    Fix f;
    wchar_t second[MAX_PATH], want[512], got[512];
    int waited, heard = 0;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }
    sibling_take(f.h.setup.out_path, 2, second, MAX_PATH);
    DeleteFileW(second);

    if (!one_take(&f, 200)) {
        fix_down(&f);
        DeleteFileW(second);
        FAIL("take one did not run");
    }

    apr_controller_test_set_foreground(f.h.ctl, 0);   /* hidden, or behind */
    sentence(APR_S_UI_TRAY_INFO_OUTPUT_RENAMED, second, NULL, want, 512);
    printf("      wanted on the tray channel: \"%ls\"\n", want);

    accel(&f.h, APR_CMD_RECORD_START);
    for (waited = 0; waited < 5000; waited += 10) {
        apr_controller_last_balloon(f.h.ctl, got, 512);
        if (wcscmp(want, got) == 0) { heard = 1; break; }
        Sleep(10);
    }
    printf("      last balloon:               \"%ls\"\n", got);

    accel(&f.h, APR_CMD_RECORD_STOP);
    for (waited = 0; waited < 25000 && apr_controller_recording(f.h.ctl);
         waited += 10) {
        Sleep(10);
    }
    apr_controller_test_set_foreground(f.h.ctl, -1);

    ASSERT_TRUE(heard);
    fix_down(&f);
    DeleteFileW(second);
}

TEST(nothing_balloons_while_the_window_is_in_front)
{
    /* The other half of "INSTEAD, not as well". A balloon raised while the
     * window is in front is read by a screen reader on top of the live region
     * that already said it -- every event heard twice -- and one test run
     * buried the author's notification centre during a meeting. */
    Fix f;
    unsigned before;
    wchar_t second[MAX_PATH];

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }
    sibling_take(f.h.setup.out_path, 2, second, MAX_PATH);
    DeleteFileW(second);

    apr_controller_test_set_foreground(f.h.ctl, 1);   /* in front */
    before = apr_controller_balloon_count(f.h.ctl);

    if (!one_take(&f, 200)) {
        fix_down(&f);
        DeleteFileW(second);
        FAIL("the take did not run");
    }
    Sleep(200);

    printf("      balloons raised in the foreground: %u\n",
           apr_controller_balloon_count(f.h.ctl) - before);
    ASSERT_EQ_INT((int)before, (int)apr_controller_balloon_count(f.h.ctl));

    apr_controller_test_set_foreground(f.h.ctl, -1);
    fix_down(&f);
    DeleteFileW(second);
}

TEST(hiding_the_window_is_refused_when_there_is_no_icon_to_hide_into)
{
    /* Shell_NotifyIcon(NIM_ADD) is allowed to fail, and that is deliberately
     * not fatal -- but both hide paths called ShowWindow(SW_HIDE) regardless.
     * With no icon the window then vanishes with no surface AT ALL: Win+B
     * finds nothing, Alt+Tab finds nothing, and it is still recording.
     *
     * Every test runs with APPRECORDER_NO_TRAY set (AGENTS.md rule 1, wired in
     * CMake), so this suite is permanently in exactly that state. */
    Fix f;
    wchar_t got[512], want[512];

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }

    /* The fixture never shows its frame, and "it did not disappear" needs a
     * window that was there to begin with. SW_SHOWNOACTIVATE: visible, but it
     * does not take the foreground away from whoever is at the machine. */
    ShowWindow(f.h.frame, SW_SHOWNOACTIVATE);
    if (!IsWindowVisible(f.h.frame)) {
        printf("      SKIPPED: the frame would not become visible\n");
        fix_down(&f);
        return;
    }

    accel(&f.h, APR_CMD_HIDE_TO_TRAY);

    apr_str_format(APR_S_UI_ANN_NO_TRAY, want, 512, NULL, 0);
    printf("      said: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);
    /* And the window is still there, which is the whole point. */
    ASSERT_TRUE(IsWindowVisible(f.h.frame) != 0);

    ShowWindow(f.h.frame, SW_HIDE);
    fix_down(&f);
}

/* ==========================================================================
 * SMALLER THINGS THAT WERE STILL SILENCE
 * ======================================================================== */

TEST(removing_an_output_from_a_bus_that_has_none_talks_about_outputs)
{
    /* "There is no bus to record yet" told a user standing on a bus that they
     * had no bus, and sent them off to add the thing they already had. */
    Fix f;
    wchar_t got[512], want[512];

    if (!fix_up(&f, 1, 1, 1, 0)) { fix_down(&f); return; }   /* bus, no output */

    accel(&f.h, APR_CMD_REMOVE_OUTPUT);

    apr_str_format(APR_S_UI_DLG_NO_OUTPUTS, want, 512, NULL, 0);
    printf("      said: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);

    fix_down(&f);
}

TEST(a_window_that_could_not_be_created_is_announced_rather_than_logged)
{
    /* A two-byte misalignment in the template builder made EVERY dialog fail
     * to be created for a fortnight. Every call site folded that into "the
     * user cancelled", so the key did nothing, no window appeared, and NOTHING
     * WAS SAID -- the only evidence was a warning in a log file nobody had
     * open, and the author's report was "it's all silence". */
    Fix f;
    HWND dlg;
    wchar_t got[512], want[512];

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }

    apr_dlg_test_fail_next(1);
    accel(&f.h, APR_CMD_ADD_BUS);
    apr_dlg_test_fail_next(0);

    apr_str_format(APR_S_UI_DLG_CREATE_FAILED, want, 512, NULL, 0);
    printf("      said: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);

    /* And a real cancel still says nothing, because the user meant it. */
    accel_async(&f.h, APR_CMD_ADD_BUS);
    dlg = wait_for_dialog(&f.h, 10000);
    if (!dlg) { fix_down(&f); FAIL("the dialog did not open after the gate"); }
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    ASSERT_TRUE(wait_for_no_dialog(&f.h, 10000));
    (void)SendMessageW(f.h.frame, WM_NULL, 0, 0);
    ASSERT_EQ_INT(0, apr_dlg_last_failed());

    fix_down(&f);
}

TEST(a_session_with_unsaved_changes_is_not_discarded_without_asking)
{
    /* File > New and File > Open threw an hour of routing away with no prompt.
     * Ctrl+N is one slip from Ctrl+B on any layout, and the session file is the
     * artifact you rely on to reproduce a recording. */
    Fix f;
    HWND dlg;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }
    ASSERT_EQ_INT(1, (int)apr_graph_source_count(f.g));

    accel_async(&f.h, APR_CMD_FILE_NEW);
    dlg = wait_for_dialog(&f.h, 10000);
    if (!dlg) { fix_down(&f); FAIL("File > New discarded the session silently"); }

    /* Say no, and nothing moves. */
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    ASSERT_TRUE(wait_for_no_dialog(&f.h, 10000));
    (void)SendMessageW(f.h.frame, WM_NULL, 0, 0);
    ASSERT_EQ_INT(1, (int)apr_graph_source_count(apr_controller_graph(f.h.ctl)));

    /* Say yes, and it does. */
    accel_async(&f.h, APR_CMD_FILE_NEW);
    dlg = wait_for_dialog(&f.h, 10000);
    if (!dlg) { fix_down(&f); FAIL("the prompt did not come back"); }
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    ASSERT_TRUE(wait_for_no_dialog(&f.h, 10000));
    (void)SendMessageW(f.h.frame, WM_NULL, 0, 0);
    ASSERT_EQ_INT(0, (int)apr_graph_source_count(apr_controller_graph(f.h.ctl)));

    fix_down(&f);
}

TEST(the_frame_title_is_the_catalogs_sentence_and_not_a_bare_path)
{
    /* UI_TITLE_SESSION exists precisely so a window title can be written the
     * way a language writes document titles. Putting the raw path in bypassed
     * it -- and the title is also the frame's accessible NAME, so what a reader
     * announced on arriving at the window was a file path with no indication of
     * which application it belonged to. */
    Fix f;
    wchar_t dir[MAX_PATH], path[MAX_PATH], want[512], got[512];
    AprErr e;
    DWORD n;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }

    n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) { fix_down(&f); FAIL("no temp folder"); }
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsapr_title_%lu.aprsession",
                 dir, GetCurrentProcessId());
    DeleteFileW(path);

    e = apr_controller_save_session(f.h.ctl, path);
    ASSERT_FALSE(apr_failed(&e));

    sentence(APR_S_UI_TITLE_SESSION, path, NULL, want, 512);
    got[0] = 0;
    GetWindowTextW(f.h.frame, got, 512);
    printf("      title: \"%ls\"\n", got);
    ASSERT_WSTR_EQ(want, got);

    DeleteFileW(path);
    fix_down(&f);
}


TEST(cancelling_a_session_load_is_not_reported_as_a_failure)
{
    /* "That session could not be loaded: the user declined this session."
     *
     * Three faults in one sentence: a cancel is not a failure, the reason was
     * an untranslatable English literal from inside the code, and it referred
     * to the person who had just pressed Cancel in the third person.
     *
     * The whole File > Open route is driven here, picker included, because the
     * sentence lives in the half BELOW the picker and nothing could reach it. */
    Fix f;
    HWND dlg;
    wchar_t dir[MAX_PATH], path[MAX_PATH], want[512], got[512];
    AprSession *s;
    AprErr e;
    DWORD n;
    int waited;

    /* A session naming a program that is certainly not running, so resolve has
     * something to report and the report is shown. */
    s = (AprSession *)calloc(1, sizeof *s);
    if (!s) FAIL("out of memory");
    apr_session_init(s);
    s->source_count = 1;
    strncpy_s(s->sources[0].key, sizeof s->sources[0].key, "s0", _TRUNCATE);
    s->sources[0].kind = APR_SESSION_SRC_PROCESS;
    lstrcpynW(s->sources[0].name, L"Nothing Like This", APR_NAME_CCH);
    lstrcpynW(s->sources[0].exe, L"apr_no_such_program_xyz.exe", APR_NAME_CCH);
    s->bus_count = 1;
    lstrcpynW(s->buses[0].name, L"Main Mix", APR_NAME_CCH);
    s->buses[0].edge_count = 1;
    strncpy_s(s->buses[0].edges[0].key, sizeof s->buses[0].edges[0].key,
              "s0", _TRUNCATE);
    s->buses[0].edges[0].source_index = 0;

    n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) { free(s); FAIL("no temp folder"); }
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsapr_declined_%lu.json",
                 dir, GetCurrentProcessId());
    e = apr_session_save(s, path);
    free(s);
    if (apr_failed(&e)) {
        wchar_t why[512];
        printf("      SKIPPED: could not write the session: %ls\n",
               apr_err_format(&e, why, 512));
        DeleteFileW(path);
        return;
    }

    /* An EMPTY session, so File > Open has nothing unsaved to ask about
     * first. */
    if (!fix_up(&f, 0, 0, 0, 0)) { fix_down(&f); DeleteFileW(path); return; }

    apr_dlg_test_set_session_path(path);
    accel_async(&f.h, APR_CMD_FILE_OPEN);

    dlg = wait_for_dialog(&f.h, 15000);
    if (!dlg) {
        printf("      SKIPPED: the resolve report did not open (this session "
               "resolved cleanly on this machine)\n");
        apr_dlg_test_set_session_path(NULL);
        fix_down(&f);
        DeleteFileW(path);
        return;
    }
    /* CANCEL. Deliberately, knowing exactly what it means. */
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    ASSERT_TRUE(wait_for_no_dialog(&f.h, 10000));
    (void)SendMessageW(f.h.frame, WM_NULL, 0, 0);

    apr_str_format(APR_S_UI_DLG_SESSION_CANCELLED, want, 512, NULL, 0);
    for (waited = 0; waited < 5000; waited += 10) {
        if (wcscmp(want, said(&f.h, got, 512)) == 0) break;
        Sleep(10);
    }
    printf("      said: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);

    /* And nothing was adopted: the session the user had is untouched. */
    ASSERT_EQ_INT(0, (int)apr_graph_bus_count(apr_controller_graph(f.h.ctl)));

    apr_dlg_test_set_session_path(NULL);
    fix_down(&f);
    DeleteFileW(path);
}

TEST(ctrl_t_with_focus_on_the_divider_does_not_strand_it_in_a_hidden_window)
{
    /* Ctrl+T hides the structure panel AND the splitter. The focus rescue
     * checked the panel and not the splitter, so Tab to "Panel divider" and
     * press Ctrl+T and focus stayed on an invisible window: the reader went
     * quiet, and Left and Right silently resized a panel nobody could see. */
    Fix f;
    HWND split = NULL, w;
    int t;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }

    /* The splitter by its window class. It is a private class of the frame's
     * and the frame publishes no handle for it -- which is part of why the
     * focus rescue forgot it existed. */
    for (w = GetWindow(f.h.frame, GW_CHILD); w; w = GetWindow(w, GW_HWNDNEXT)) {
        wchar_t cls[64];
        cls[0] = 0;
        if (GetClassNameW(w, cls, 64) <= 0) continue;
        if (CompareStringOrdinal(cls, -1, L"AprRecorderSplitter", -1,
                                 FALSE) == CSTR_EQUAL) {
            split = w;
            break;
        }
    }
    if (!split) {
        printf("      SKIPPED: no splitter window in this build\n");
        fix_down(&f);
        return;
    }

    /* Tab to it, exactly as the author does: focus into a pane with F6 first,
     * then Tab until the divider has it. A SetFocus from this thread would do
     * nothing at all, silently. */
    if (!focus_the_tree(&f.h)) {
        printf("      SKIPPED: focus never reached the tree control\n");
        fix_down(&f);
        return;
    }
    for (t = 0; t < 12 && focus_on_ui_thread(&f.h) != split; ++t) {
        /* Posted to the window that actually HAS focus, which is what the
         * keyboard does: IsDialogMessage navigates from the focused control,
         * and a Tab addressed to the frame is not the same message. */
        HWND ff = focus_on_ui_thread(&f.h);
        if (!ff) break;
        PostMessageW(ff, WM_KEYDOWN, VK_TAB, 0);
        PostMessageW(ff, WM_KEYUP, VK_TAB, 0);
        Sleep(60);
    }
    if (focus_on_ui_thread(&f.h) != split) {
        printf("      SKIPPED: Tab never landed on the splitter\n");
        fix_down(&f);
        return;
    }

    accel(&f.h, APR_CMD_VIEW_TREE);   /* hide the structure panel */

    for (t = 0; t < 60; ++t) {
        HWND ff = focus_on_ui_thread(&f.h);
        if (ff && IsWindowVisible(ff)) break;
        Sleep(15);
    }
    {
        HWND ff = focus_on_ui_thread(&f.h);
        printf("      splitter visible after Ctrl+T: %s; focus is on %p "
               "(visible: %s)\n",
               IsWindowVisible(split) ? "yes" : "no", (void *)ff,
               (ff && IsWindowVisible(ff)) ? "yes" : "NO");
        ASSERT_FALSE(IsWindowVisible(split) != 0);
        ASSERT_NOT_NULL(ff);
        ASSERT_TRUE(IsWindowVisible(ff) != 0);
    }

    accel(&f.h, APR_CMD_VIEW_TREE);   /* put it back */
    fix_down(&f);
}


TEST(a_close_that_runs_out_of_patience_says_that_rather_than_repeating_itself)
{
    /* The wait for the encoders is bounded, because a genuinely hung disk must
     * not leave a window that cannot be closed at all. When it expired the
     * handler said "the window will close once the files are written" -- the
     * same sentence it had said on the way IN. Hearing that twice and then
     * having nothing close is indistinguishable from a hung application. */
    Fix f;
    HWND dlg;
    wchar_t got[512], want[512];
    int waited;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }

    /* Give up immediately, so the path is reached without a stalled disk. */
    apr_controller_test_set_close_wait_ms(f.h.ctl, 0);

    accel(&f.h, APR_CMD_RECORD_START);
    if (!apr_controller_recording(f.h.ctl)) {
        fix_down(&f);
        FAIL("the recording did not start");
    }

    PostMessageW(f.h.frame, WM_CLOSE, 0, 0);
    dlg = wait_for_dialog(&f.h, 15000);
    if (!dlg) {
        accel(&f.h, APR_CMD_RECORD_STOP);
        for (waited = 0; waited < 25000 && apr_controller_recording(f.h.ctl);
             waited += 10) {
            Sleep(10);
        }
        fix_down(&f);
        FAIL("closing while recording asked no question");
    }
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);

    apr_str_format(APR_S_UI_ANN_CLOSE_TIMEOUT, want, 512, NULL, 0);
    for (waited = 0; waited < 10000; waited += 10) {
        if (wcscmp(want, said(&f.h, got, 512)) == 0) break;
        Sleep(10);
    }
    printf("      said: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);

    /* And the window is still there to be closed again, which is the other
     * half of the promise. */
    ASSERT_TRUE(IsWindow(f.h.frame) != 0);

    apr_controller_test_set_close_wait_ms(f.h.ctl, -1);
    accel(&f.h, APR_CMD_RECORD_STOP);
    for (waited = 0; waited < 25000 && apr_controller_recording(f.h.ctl);
         waited += 10) {
        Sleep(10);
    }
    fix_down(&f);
}

TEST(an_edit_the_model_refuses_is_announced_with_the_reason_the_model_gave)
{
    /* "That is not available yet" describes a feature nobody has written. It
     * was what the canvas said for EVERY refusal the model handed back, with
     * the AprErr thrown away -- so a real, explicable refusal reached the user
     * as a statement about their own session that was simply untrue, and the
     * only diagnostic went in the bin.
     *
     * A bus holds APR_MAX_SOURCES_PER_BUS sources; the one after that is
     * refused, with a reason. */
    Fix f;
    AprCaptureConfig cfg;
    wchar_t got[512], want[512];
    AprSourceId extra = 0;
    size_t i;
    int si, bi;
    AprErr e;

    if (!fix_up(&f, 0, 1, 0, 0)) { fix_down(&f); return; }

    memset(&cfg, 0, sizeof cfg);
    cfg.kind = APR_SRC_FAKE;
    cfg.fake.tone_hz   = 440;
    cfg.fake.amplitude = 0.25f;

    for (i = 0; i < APR_MAX_SOURCES_PER_BUS + 1; ++i) {
        wchar_t name[APR_NAME_CCH];
        AprSourceId id;
        _snwprintf_s(name, APR_NAME_CCH, _TRUNCATE, L"Source %d", (int)i);
        e = apr_graph_add_source(f.g, name, &cfg, &id);
        if (apr_failed(&e)) { fix_down(&f); FAIL("could not fill the graph"); }
        if (i < APR_MAX_SOURCES_PER_BUS) {
            e = apr_graph_connect(f.g, id, BUS(&f), 1.0f);
            if (apr_failed(&e)) { fix_down(&f); FAIL("could not fill the bus"); }
        } else {
            extra = id;
        }
    }
    apr_controller_model_changed(f.h.ctl);

    /* The reason the model itself gives for refusing the next one -- read from
     * the model, never guessed, and never written down here as prose. */
    e = apr_graph_connect(f.g, extra, BUS(&f), 1.0f);
    if (!apr_failed(&e)) {
        printf("      SKIPPED: this build accepts more than %d sources on a "
               "bus\n", (int)APR_MAX_SOURCES_PER_BUS);
        fix_down(&f);
        return;
    }
    {
        const wchar_t *args[1];
        wchar_t why[512];
        /* apr_err_reason, NOT apr_err_format: the frame is a catalog sentence
         * and so is the reason it carries, or the announcement is half
         * translated the day Arabic ships (BUGS.md M11). This expectation used
         * to be built from apr_err_format and therefore asserted the bug --
         * "bus 1 already has 32 sources: APR_E_STATE at bus.c(226) in
         * apr_bus_add_source", read out loud, to a blind user. */
        apr_err_reason(&e, why, 512);
        args[0] = why;
        apr_str_format(APR_S_UI_ANN_EDIT_FAILED, want, 512, args, 1);

        /* And the reason really is the catalog's, not err.c's. */
        ASSERT_NULL(wcsstr(want, L"bus.c"));
        ASSERT_NULL(wcsstr(want, L"APR_E_"));
    }

    si = node_index(&f.h, APR_NODE_SOURCE, extra, 0);
    bi = node_index(&f.h, APR_NODE_BUS, BUS(&f), 0);
    ASSERT_GE_INT(0, si);
    ASSERT_GE_INT(0, bi);

    ASSERT_TRUE(focus_node(&f.h, si));
    SendMessageW(f.h.canvas, APR_CANVAS_WM_PERFORM,
                 (WPARAM)APR_CANVAS_OP_CONNECT, 0);
    ASSERT_TRUE(focus_node(&f.h, bi));
    SendMessageW(f.h.canvas, APR_CANVAS_WM_PERFORM,
                 (WPARAM)APR_CANVAS_OP_CONNECT, 0);

    printf("      said:   \"%ls\"\n", said(&f.h, got, 512));
    printf("      wanted: \"%ls\"\n", want);
    ASSERT_WSTR_EQ(want, got);

    fix_down(&f);
}

TEST(a_refusal_from_the_controller_is_a_catalog_sentence_end_to_end)
{
    /* THE OTHER DISPLAY SITE. The canvas has its own frame
     * (UI_ANN_EDIT_FAILED); the controller has report_failure(), which is what
     * every dialog route and every session route lands on, and it fed the same
     * English prose into UI_DLG_ADD_FAILED. Same bug, second door.
     *
     * The graph holds APR_MAX_SOURCES; the one after that is refused, and
     * graph.c names the sentence for it (APR_ERR_SAY) because "wrong state" is
     * true of a full graph and of six other things. */
    Fix f;
    AprCaptureConfig cfg;
    wchar_t got[512], want[512], reason[512];
    const wchar_t *args[1];
    size_t i;
    AprErr e = apr_ok();

    if (!fix_up(&f, 0, 0, 0, 0)) { fix_down(&f); return; }

    memset(&cfg, 0, sizeof cfg);
    cfg.kind = APR_SRC_FAKE;
    cfg.fake.tone_hz   = 440;
    cfg.fake.amplitude = 0.25f;

    for (i = 0; i < APR_MAX_SOURCES + 1; ++i) {
        wchar_t name[APR_NAME_CCH];
        _snwprintf_s(name, APR_NAME_CCH, _TRUNCATE, L"Source %d", (int)i);
        e = apr_controller_add_source(f.h.ctl, name, &cfg);
        if (apr_failed(&e)) break;
    }
    if (!apr_failed(&e)) {
        printf("      SKIPPED: this build accepts more than %d sources\n",
               (int)APR_MAX_SOURCES);
        fix_down(&f);
        return;
    }

    /* The sentence the user got, and the sentence the catalog says they
     * should have got. Every character of both comes out of the .rc. */
    apr_str_probe(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                  APR_S_ERR_REASON_TOO_MANY_SOURCES, reason, 512);
    args[0] = reason;
    apr_str_format(APR_S_UI_DLG_ADD_FAILED, want, 512, args, 1);

    printf("      said:   \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);

    /* Not the raise site, not the internal count, not the enum name. */
    ASSERT_NULL(wcsstr(got, L"graph.c"));
    ASSERT_NULL(wcsstr(got, L"APR_E_"));

    fix_down(&f);
}

TEST(a_half_made_connection_whose_end_disappears_says_it_has_been_cancelled)
{
    /* A mode the user is IN that ends without a word leaves them pressing the
     * second half of a gesture that no longer exists -- which is exactly how
     * "Ctrl+E twice does nothing" was reported the first time. */
    Fix f;
    wchar_t got[512], want[512];
    int si;

    if (!fix_up(&f, 1, 1, 0, 0)) { fix_down(&f); return; }

    si = node_index(&f.h, APR_NODE_SOURCE, SRC(&f), 0);
    ASSERT_TRUE(focus_node(&f.h, si));
    SendMessageW(f.h.canvas, APR_CANVAS_WM_PERFORM,
                 (WPARAM)APR_CANVAS_OP_CONNECT, 0);
    ASSERT_NOT_NULL(apr_canvas_pending_node(f.h.canvas));

    /* The held end leaves the model behind the canvas's back -- which is the
     * case apr_controller_model_changed exists for. */
    {
        AprErr e = apr_graph_remove_source(f.g, SRC(&f));
        ASSERT_FALSE(apr_failed(&e));
    }
    apr_controller_model_changed(f.h.ctl);

    ASSERT_TRUE(apr_canvas_pending_node(f.h.canvas) == NULL);
    apr_str_format(APR_S_UI_ANN_CANCELLED, want, 512, NULL, 0);
    printf("      said: \"%ls\"\n", said(&f.h, got, 512));
    ASSERT_WSTR_EQ(want, got);

    fix_down(&f);
}

TEST(focus_arriving_at_the_canvas_lands_on_a_node_and_not_on_the_pane)
{
    /* SetFocus() from inside WM_SETFOCUS is SWALLOWED -- the outer SetFocus
     * reasserts its own target as it unwinds (design 6.3). Every proxy check
     * passes; only asking the platform which window really ended up with focus
     * catches it. A canvas that keeps focus on itself leaves a screen reader
     * reading the pane's name and nothing else, forever. */
    Fix f;
    int t;
    HWND got = NULL;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }

    ShowWindow(f.h.frame, SW_SHOWNOACTIVATE);
    if (!IsWindowVisible(f.h.frame)) {
        printf("      SKIPPED: the frame would not become visible\n");
        fix_down(&f);
        return;
    }

    /* F6 until the canvas pane has been entered. */
    for (t = 0; t < 8 && !got; ++t) {
        int w;
        PostMessageW(f.h.frame, WM_KEYDOWN, VK_F6, 0);
        PostMessageW(f.h.frame, WM_KEYUP, VK_F6, 0);
        for (w = 0; w < 30; ++w) {
            HWND ff = focus_on_ui_thread(&f.h);
            if (ff && IsChild(f.h.canvas, ff)) { got = ff; break; }
            Sleep(15);
        }
    }

    printf("      focus inside the canvas: %p (the canvas itself is %p)\n",
           (void *)got, (void *)f.h.canvas);
    ASSERT_NOT_NULL(got);
    ASSERT_TRUE(got != f.h.canvas);
    /* And it is a real node, which is what carries a name worth reading. */
    ASSERT_TRUE(got == apr_canvas_focused_node(f.h.canvas));

    fix_down(&f);
}

TEST(a_session_opened_with_nobody_to_ask_reports_the_sources_it_dropped)
{
    /* ui_controller.h: non-interactive "takes what resolved and reports the
     * rest through the return value". It returned ok -- so a caller with
     * nobody to ask was told a session had loaded cleanly while sources had
     * been silently dropped out of it. */
    Fix f;
    AprSession *s;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    AprErr e;
    DWORD n;

    s = (AprSession *)calloc(1, sizeof *s);
    if (!s) FAIL("out of memory");
    apr_session_init(s);
    s->source_count = 1;
    strncpy_s(s->sources[0].key, sizeof s->sources[0].key, "s0", _TRUNCATE);
    s->sources[0].kind = APR_SESSION_SRC_PROCESS;
    lstrcpynW(s->sources[0].name, L"Nothing Like This", APR_NAME_CCH);
    lstrcpynW(s->sources[0].exe, L"apr_no_such_program_xyz.exe", APR_NAME_CCH);
    s->bus_count = 1;
    lstrcpynW(s->buses[0].name, L"Main Mix", APR_NAME_CCH);
    s->buses[0].edge_count = 1;
    strncpy_s(s->buses[0].edges[0].key, sizeof s->buses[0].edges[0].key,
              "s0", _TRUNCATE);
    s->buses[0].edges[0].source_index = 0;

    n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) { free(s); FAIL("no temp folder"); }
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsapr_headless_%lu.json",
                 dir, GetCurrentProcessId());
    e = apr_session_save(s, path);
    free(s);
    if (apr_failed(&e)) {
        printf("      SKIPPED: could not write the session\n");
        DeleteFileW(path);
        return;
    }

    if (!fix_up(&f, 0, 0, 0, 0)) { fix_down(&f); DeleteFileW(path); return; }

    e = apr_controller_open_session(f.h.ctl, path, 0);
    {
        wchar_t why[512];
        printf("      returned: %ls\n",
               apr_failed(&e) ? apr_err_format(&e, why, 512) : L"ok");
    }
    ASSERT_TRUE(apr_failed(&e));

    /* The graph IS adopted either way -- what changes is that the caller finds
     * out what is missing from it. */
    ASSERT_EQ_INT(1, (int)apr_graph_bus_count(apr_controller_graph(f.h.ctl)));
    ASSERT_EQ_INT(0, (int)apr_graph_source_count(apr_controller_graph(f.h.ctl)));

    fix_down(&f);
    DeleteFileW(path);
}

TEST(add_output_with_no_bus_to_put_it_on_says_so_instead_of_opening_empty)
{
    /* m6 SAID "OK can silently do nothing when the format combo is empty", and
     * the shape of that defect was real: every `return TRUE` on an empty combo
     * was a button press that changed nothing, closed nothing and said
     * nothing. For a listener a dead OK reads as a broken application.
     *
     * Each of those branches now names what is missing and puts focus on it.
     * The FORMAT one cannot be reached from a build that has encoders, and the
     * BUS one turns out to be guarded a step earlier -- which is what this
     * asserts, because it is the path a person actually takes: ask for an
     * output with nowhere to put it, and be told, rather than being handed an
     * empty picker whose OK does nothing. */
    Fix f;
    HWND dlg;
    wchar_t body[512];

    /* A source but NO bus. */
    if (!fix_up(&f, 1, 0, 0, 0)) { fix_down(&f); return; }

    accel_async(&f.h, APR_CMD_ADD_ACTION);
    dlg = wait_for_dialog(&f.h, 15000);
    if (!dlg) { fix_down(&f); FAIL("Add Output said nothing and opened nothing"); }

    body[0] = 0;
    GetDlgItemTextW(dlg, 2015 /* IDC_BODY, src/ui/dialogs.c */, body, 512);
    printf("      said: \"%ls\"\n", body);
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_DLG_NO_BUSES), body);

    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    ASSERT_TRUE(wait_for_no_dialog(&f.h, 10000));
    (void)SendMessageW(f.h.frame, WM_NULL, 0, 0);

    /* And nothing was created out of a choice that could not be made. */
    ASSERT_EQ_INT(0, (int)apr_graph_bus_count(apr_controller_graph(f.h.ctl)));

    fix_down(&f);
}

TEST(the_close_dialogs_buttons_are_wide_enough_for_their_own_captions)
{
    /* NEVER SIZE A CONTROL TO FIT ITS ENGLISH STRING (AGENTS.md rule 6). These
     * were a fixed 96 dialog units, which clips "Stop the recording and close"
     * in ENGLISH -- and Arabic needs more room again at the same point size,
     * so the Arabic pass would have shipped three unreadable buttons. */
    Fix f;
    HWND dlg;
    int waited, checked = 0;
    static const int ids[] = { IDOK, IDCANCEL };
    size_t k;

    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }

    accel(&f.h, APR_CMD_RECORD_START);
    if (!apr_controller_recording(f.h.ctl)) {
        fix_down(&f);
        FAIL("the recording did not start");
    }

    PostMessageW(f.h.frame, WM_CLOSE, 0, 0);
    dlg = wait_for_dialog(&f.h, 15000);
    if (!dlg) {
        accel(&f.h, APR_CMD_RECORD_STOP);
        for (waited = 0; waited < 25000 && apr_controller_recording(f.h.ctl);
             waited += 10) {
            Sleep(10);
        }
        fix_down(&f);
        FAIL("closing while recording asked no question");
    }

    for (k = 0; k < sizeof ids / sizeof ids[0]; ++k) {
        HWND b = GetDlgItem(dlg, ids[k]);
        wchar_t text[256];
        RECT rc;
        HDC dc;
        HFONT font, old;
        SIZE sz;

        if (!b) continue;
        text[0] = 0;
        GetWindowTextW(b, text, 256);
        if (!text[0]) continue;
        GetClientRect(b, &rc);

        dc = GetDC(b);
        if (!dc) continue;
        font = (HFONT)SendMessageW(b, WM_GETFONT, 0, 0);
        old = font ? (HFONT)SelectObject(dc, font) : NULL;
        if (GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz)) {
            printf("      \"%ls\": button %ld px, text %ld px\n",
                   text, (long)(rc.right - rc.left), (long)sz.cx);
            ASSERT_GE_INT((int)sz.cx, (int)(rc.right - rc.left));
            checked++;
        }
        if (old) SelectObject(dc, old);
        ReleaseDC(b, dc);
    }
    ASSERT_GT_INT(0, checked);

    /* "Keep recording" -- the answer that changes nothing. */
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    (void)wait_for_no_dialog(&f.h, 10000);

    accel(&f.h, APR_CMD_RECORD_STOP);
    for (waited = 0; waited < 25000 && apr_controller_recording(f.h.ctl);
         waited += 10) {
        Sleep(10);
    }
    fix_down(&f);
}

/* ==========================================================================
 * Every dialog opens, is named, and cancels without touching the model
 * ======================================================================== */

typedef struct DlgCase { int cmd; const char *what; } DlgCase;

TEST(every_dialog_opens_and_cancels_and_leaves_the_model_alone)
{
    /* THE DEFECT THIS EXISTS FOR: a two-byte misalignment in the template
     * builder made DialogBoxIndirectParamW return -1 for EVERY dialog, and
     * every call site folded -1 into "the user cancelled". Nothing opened,
     * nothing was said, and every suite was green -- because no test had ever
     * opened a dialog. */
    static const DlgCase cases[] = {
        { APR_CMD_ADD_SOURCE,    "Add Source" },
        { APR_CMD_ADD_BUS,       "Add Bus" },
        { APR_CMD_ADD_ACTION,    "Add Output" },
        { APR_CMD_RENAME_BUS,    "Rename Bus" },
        { APR_CMD_REMOVE_OUTPUT, "Remove Output" },
        { APR_CMD_HELP_KEYS,     "Keyboard Shortcuts" },
        { APR_CMD_HELP_ABOUT,    "About" }
    };
    Fix f;
    size_t i;

    /* A source, a bus and one output, so that no command refuses on an empty
     * model before it ever reaches its dialog. */
    if (!fix_up(&f, 1, 1, 1, 1)) { fix_down(&f); return; }

    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        HWND dlg;
        wchar_t title[256];

        accel_async(&f.h, cases[i].cmd);
        dlg = wait_for_dialog(&f.h, 15000);
        if (!dlg) {
            printf("      %-20s -> NOTHING OPENED\n", cases[i].what);
            fix_down(&f);
            FAIL("a command that opens a dialog opened nothing");
        }
        title[0] = 0;
        GetWindowTextW(dlg, title, 256);
        printf("      %-20s -> \"%ls\"\n", cases[i].what, title);
        /* A dialog with no title is a dialog a screen reader announces as
         * nothing when it opens. */
        ASSERT_TRUE(title[0] != 0);

        PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        if (!wait_for_no_dialog(&f.h, 10000)) {
            fix_down(&f);
            FAIL("a cancelled dialog did not close");
        }
        (void)SendMessageW(f.h.frame, WM_NULL, 0, 0);
    }

    /* Cancelling changed nothing. */
    ASSERT_EQ_INT(1, (int)apr_graph_source_count(f.g));
    ASSERT_EQ_INT(1, (int)apr_graph_bus_count(f.g));
    ASSERT_TRUE(apr_graph_connected(f.g, SRC(&f), BUS(&f)) != 0);

    fix_down(&f);
}
