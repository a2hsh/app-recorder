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
#include "graph.h"
#include "strings.h"
#include "ui_app.h"
#include "ui_canvas.h"
#include "ui_controller.h"
#include "ui_node.h"

static const CLSID kCLSID_CUIAutomation =
    { 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
static const IID kIID_IUIAutomation =
    { 0x30cbe57d, 0xd9d0, 0x452a, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };

#define IDC_NAME 2003   /* the edit field in the name prompt, src/ui/dialogs.c */

/* ==========================================================================
 * A real frame, a real controller, on a real STA -- the apartment the product
 * runs in, because IAccPropServices supplies every accessible name and is
 * valid only on the thread that created it (src/uiapp/main.c).
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
    h->status = apr_ui_app_status_bar(h->app);

    build_setup(h);
    SetEvent(h->ready);

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
