/*
 * test_ui_tree.c -- the structure panel, tested at both levels it can lie at.
 *
 * ===========================================================================
 * WHY TWO LEVELS
 *
 *   The panel is a pure projection (graph -> rows -> text) rendered into a
 *   standard control. Those are two different failure modes and a test that
 *   only reaches one of them misses the other entirely:
 *
 *     - The PURE half can be asserted exhaustively, in both languages, in one
 *       process, with no window. That is where "structure is source -> bus ->
 *       action and it does not flip in Arabic" is settled -- as an identity
 *       between two arrays, not as an impression of a screenshot.
 *
 *     - The LIVE half has to be asked of UI Automation, because a screen
 *       reader reads what UIA reports after comctl32's provider, the MSAA
 *       bridge and our NM_CUSTOMDRAW have all had their say. Everything short
 *       of asking UIA is a proxy, and a proxy is what lets a regression ship.
 *
 * ===========================================================================
 * THE REGRESSION THIS FILE EXISTS FOR
 *
 *   AGENTS.md rule 5: NM_CUSTOMDRAW, never owner-draw. Owner-draw replaces the
 *   control's semantics and leaves a screen reader an empty rectangle, and the
 *   symptom is invisible to anyone testing by eye -- the pixels look BETTER.
 *
 *   So custom_draw_ran_and_the_accessibility_tree_is_still_intact asserts BOTH
 *   halves, and the first half is why the panel exports a counting test seam:
 *   "custom draw did not break accessibility" passes for free on a build where
 *   custom draw never fired, which is exactly the state a regression would
 *   leave behind if someone deleted the handler.
 *
 * ===========================================================================
 * SKIPPING
 *
 *   A session with no interactive window station cannot create a window and
 *   UIA cannot be instantiated in some sandboxes. Both are reported SKIPPED
 *   rather than faked. The pure cases need neither and always run.
 */
#include "test_runner.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <initguid.h>
#include <uiautomation.h>
#include <string.h>

#include "graph.h"
#include "strings.h"
#include "ui_app.h"
#include "ui_theme.h"
#include "ui_tree_panel.h"

/* The test seam. Deliberately in no header -- see the note at the bottom of
 * src/ui/tree_panel.c. */
unsigned apr_tree_panel_test_customdraw_items(HWND panel);

static const CLSID kCLSID_CUIAutomation =
    { 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
static const IID kIID_IUIAutomation =
    { 0x30cbe57d, 0xd9d0, 0x452a, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };

#define RATE 48000u
#define MAXROWS 64

/* ==========================================================================
 * The model under test
 *
 * Deliberately a GRAPH and not a tree: "Teams" feeds two buses, so it appears
 * twice, and "Spare" feeds none, so it appears under its own group. A panel
 * that quietly assumed one parent per source would pass every other case here
 * and fail these two.
 * ======================================================================== */

typedef struct Model {
    AprGraph   *g;
    AprBusId    main_mix, teams_only;
    AprSourceId teams, mic, spare;
    wchar_t     out_path[MAX_PATH];
    int         has_action;
} Model;

static AprSourceId add_fake_cfg(AprGraph *g, const wchar_t *name,
                                const AprCaptureConfig *c)
{
    AprSourceId id = 0;
    (void)apr_graph_add_source(g, name, c, &id);
    return id;
}

static AprSourceId add_fake(AprGraph *g, const wchar_t *name)
{
    AprCaptureConfig c;

    memset(&c, 0, sizeof c);
    c.kind = APR_SRC_FAKE;          /* design 4.3: the core needs no hardware,
                                     * and AGENTS.md rule 1 means a UI test is
                                     * the last place that should change. */
    return add_fake_cfg(g, name, &c);
}

static void model_build(Model *m)
{
    AprActionConfig ac;
    wchar_t dir[MAX_PATH];
    AprErr e;

    memset(m, 0, sizeof *m);
    e = apr_graph_create(RATE, 2, &m->g);
    if (apr_failed(&e) || !m->g) return;

    (void)apr_graph_add_bus(m->g, L"Main Mix", &m->main_mix);
    (void)apr_graph_add_bus(m->g, L"Teams Only", &m->teams_only);

    m->teams = add_fake(m->g, L"Teams");
    m->mic   = add_fake(m->g, L"Chat Mic");
    m->spare = add_fake(m->g, L"Spare");

    (void)apr_graph_connect(m->g, m->teams, m->main_mix,   1.0f);
    (void)apr_graph_connect(m->g, m->teams, m->teams_only, 1.0f);
    (void)apr_graph_connect(m->g, m->mic,   m->main_mix,   1.0f);
    /* m->spare is deliberately connected to nothing. */

    if (GetTempPathW(MAX_PATH, dir) > 0) {
        wchar_t name[64];
        static LONG seq;
        wsprintfW(name, L"apr_tree_test_%lu_%ld.wav",
                  (unsigned long)GetCurrentProcessId(),
                  (long)InterlockedIncrement(&seq));
        lstrcpynW(m->out_path, dir, MAX_PATH);
        lstrcatW(m->out_path, name);

        memset(&ac, 0, sizeof ac);
        ac.out_path = m->out_path;
        e = apr_graph_add_action(m->g, m->main_mix, "wav", &ac);
        m->has_action = !apr_failed(&e);
    }
}

static void model_free(Model *m)
{
    if (m->g) apr_graph_destroy(m->g);
    if (m->out_path[0]) DeleteFileW(m->out_path);
    memset(m, 0, sizeof *m);
}

/* ==========================================================================
 * The pure half -- no window anywhere
 * ======================================================================== */

TEST(an_empty_session_still_produces_a_row_a_screen_reader_can_read)
{
    AprTreeRow rows[MAXROWS];
    wchar_t text[512];
    AprGraph *g = NULL;

    /* A panel with nothing in it must still say something. Silence is the one
     * answer a screen reader user cannot distinguish from a broken control. */
    ASSERT_EQ_INT(1, (int)apr_tree_panel_rows(NULL, rows, MAXROWS));
    ASSERT_EQ_INT((int)APR_TREE_ROW_EMPTY, (int)rows[0].sel.kind);
    ASSERT_GT_INT(0, (int)apr_tree_panel_label(NULL, &rows[0].sel, text, 512));
    ASSERT_TRUE(text[0] != 0);

    (void)apr_graph_create(RATE, 2, &g);
    ASSERT_NOT_NULL(g);
    ASSERT_EQ_INT(1, (int)apr_tree_panel_rows(g, rows, MAXROWS));
    ASSERT_EQ_INT((int)APR_TREE_ROW_EMPTY, (int)rows[0].sel.kind);
    apr_graph_destroy(g);
}

TEST(the_rows_are_source_bus_action_with_one_subtree_per_bus)
{
    Model m;
    AprTreeRow r[MAXROWS];
    size_t n;

    model_build(&m);
    ASSERT_NOT_NULL(m.g);
    ASSERT_TRUE(m.has_action);

    n = apr_tree_panel_rows(m.g, r, MAXROWS);
    ASSERT_EQ_INT(8, (int)n);

    /* Main Mix, then what feeds it, then what it writes. The order inside a
     * bus is the direction the audio travels, so reading straight down IS
     * reading the signal chain. */
    ASSERT_EQ_INT((int)APR_TREE_ROW_BUS,    (int)r[0].sel.kind);
    ASSERT_EQ_INT((int)m.main_mix,          (int)r[0].sel.bus);
    ASSERT_EQ_INT(0,                        r[0].depth);

    ASSERT_EQ_INT((int)APR_TREE_ROW_SOURCE, (int)r[1].sel.kind);
    ASSERT_EQ_INT((int)m.teams,             (int)r[1].sel.source);
    ASSERT_EQ_INT((int)m.main_mix,          (int)r[1].sel.bus);
    ASSERT_EQ_INT(1,                        r[1].depth);

    ASSERT_EQ_INT((int)APR_TREE_ROW_SOURCE, (int)r[2].sel.kind);
    ASSERT_EQ_INT((int)m.mic,               (int)r[2].sel.source);

    ASSERT_EQ_INT((int)APR_TREE_ROW_ACTION, (int)r[3].sel.kind);
    ASSERT_EQ_INT((int)m.main_mix,          (int)r[3].sel.bus);
    ASSERT_EQ_INT(1,                        r[3].depth);

    ASSERT_EQ_INT((int)APR_TREE_ROW_BUS,    (int)r[4].sel.kind);
    ASSERT_EQ_INT((int)m.teams_only,        (int)r[4].sel.bus);

    /* THE GRAPH-NESS. One source, two parents, two rows -- and the two rows
     * are distinguishable because a selection carries its bus. */
    ASSERT_EQ_INT((int)APR_TREE_ROW_SOURCE, (int)r[5].sel.kind);
    ASSERT_EQ_INT((int)m.teams,             (int)r[5].sel.source);
    ASSERT_EQ_INT((int)m.teams_only,        (int)r[5].sel.bus);

    /* A source wired to nothing records nothing, and nothing else in the UI
     * says so out loud. */
    ASSERT_EQ_INT((int)APR_TREE_ROW_UNASSIGNED, (int)r[6].sel.kind);
    ASSERT_EQ_INT((int)APR_TREE_ROW_SOURCE,     (int)r[7].sel.kind);
    ASSERT_EQ_INT((int)m.spare,                 (int)r[7].sel.source);
    ASSERT_EQ_INT(0,                            (int)r[7].sel.bus);

    model_free(&m);
}

TEST(the_hierarchy_does_not_reorder_in_an_rtl_language)
{
    Model m;
    AprTreeRow ltr[MAXROWS], rtl[MAXROWS];
    size_t n_ltr, n_rtl, i;
    AprErr e;

    model_build(&m);
    ASSERT_NOT_NULL(m.g);

    e = apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(0, apr_str_is_rtl());
    n_ltr = apr_tree_panel_rows(m.g, ltr, MAXROWS);

    e = apr_str_set_language(MAKELANGID(LANG_ARABIC, SUBLANG_ARABIC_SAUDI_ARABIA));
    ASSERT_FALSE(apr_failed(&e));
    /* Asserted so the case cannot pass by silently failing to switch, which is
     * the way an invariance test usually rots. */
    ASSERT_EQ_INT(1, apr_str_is_rtl());
    n_rtl = apr_tree_panel_rows(m.g, rtl, MAXROWS);

    ASSERT_EQ_INT((int)n_ltr, (int)n_rtl);
    for (i = 0; i < n_ltr; ++i) {
        ASSERT_EQ_INT((int)ltr[i].sel.kind,   (int)rtl[i].sel.kind);
        ASSERT_EQ_INT((int)ltr[i].sel.bus,    (int)rtl[i].sel.bus);
        ASSERT_EQ_INT((int)ltr[i].sel.source, (int)rtl[i].sel.source);
        ASSERT_EQ_INT((int)ltr[i].sel.action, (int)rtl[i].sel.action);
        ASSERT_EQ_INT(ltr[i].depth,           rtl[i].depth);
    }
    /* And byte for byte too, which only holds because apr_tree_panel_rows
       zeroes each row's padding -- it did not, and Release caught it. */
    ASSERT_MEM_EQ(ltr, rtl, n_ltr * sizeof ltr[0]);

    (void)apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    model_free(&m);
}

TEST(every_row_says_what_it_is_and_what_it_is_wired_to)
{
    Model m;
    AprTreeRow r[MAXROWS];
    wchar_t text[512], other[512];
    size_t n, i;

    /* Set here rather than trusted: an assertion failure in the RTL case above
       returns before it can restore English, and a cascade of confusing
       failures in later cases hides the one real one. */
    (void)apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));

    model_build(&m);
    ASSERT_NOT_NULL(m.g);
    n = apr_tree_panel_rows(m.g, r, MAXROWS);

    /* A row with no text is an element a screen reader lands on and can say
     * nothing about -- the single most common native-Windows a11y defect. */
    for (i = 0; i < n; ++i) {
        text[0] = 0;
        apr_tree_panel_label(m.g, &r[i].sel, text, 512);
        printf("      row %u depth %d: \"%ls\"\n",
               (unsigned)i, r[i].depth, text);
        ASSERT_TRUE(text[0] != 0);
    }

    /* The bus row carries its name and both counts. */
    apr_tree_panel_label(m.g, &r[0].sel, text, 512);
    ASSERT_NOT_NULL(wcsstr(text, L"Main Mix"));

    /* A source row carries its own name, its KIND, and the bus it feeds --
     * everything a listener needs without moving off the row. */
    apr_tree_panel_label(m.g, &r[2].sel, text, 512);
    ASSERT_NOT_NULL(wcsstr(text, L"Chat Mic"));
    ASSERT_NOT_NULL(wcsstr(text, apr_str(APR_S_SOURCE_KIND_FAKE)));
    ASSERT_NOT_NULL(wcsstr(text, L"Main Mix"));

    /* And the fact nesting CANNOT express: Teams also feeds another bus. Its
     * row must therefore differ from the row of a source that feeds only one,
     * even though both sit under Main Mix. */
    apr_tree_panel_label(m.g, &r[1].sel, other, 512);
    ASSERT_NOT_NULL(wcsstr(other, L"Teams"));
    ASSERT_NOT_NULL(wcsstr(other, L"Main Mix"));
    ASSERT_TRUE(wcscmp(text, other) != 0);
    ASSERT_TRUE(wcslen(other) > wcslen(L"Teams"));

    /* The unconnected source says so instead of naming a bus. */
    apr_tree_panel_label(m.g, &r[7].sel, text, 512);
    ASSERT_NOT_NULL(wcsstr(text, L"Spare"));
    ASSERT_NULL(wcsstr(text, L"Main Mix"));
    ASSERT_NULL(wcsstr(text, L"Teams Only"));

    /* The output row names the format AND the bus it comes from, because
     * "WAV" on its own is not an answer to "what is this". */
    apr_tree_panel_label(m.g, &r[3].sel, text, 512);
    ASSERT_NOT_NULL(wcsstr(text, L"Main Mix"));
    ASSERT_TRUE(wcslen(text) > 4);

    model_free(&m);
}

TEST(the_state_clauses_a_silent_recording_depends_on_are_in_the_catalog)
{
    /* A muted source records pure silence and an exited one keeps recording
     * silence, and BOTH look perfectly healthy from every other angle. What is
     * pinned HERE is only that the copy exists, is distinct, and is not empty:
     * if someone deletes an entry, the row falls back to the healthy wording,
     * which is the dangerous direction.
     *
     * The branches that SELECT this copy are driven through a real graph in
     * the two cases below, now that capture.h's fake source can be told to go
     * muted or to die. */
    const wchar_t *muted;
    const wchar_t *exited;

    (void)apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    muted  = apr_str(APR_S_UI_TREE_STATE_MUTED);
    exited = apr_str(APR_S_UI_TREE_STATE_EXITED);

    ASSERT_TRUE(muted[0] != 0);
    ASSERT_TRUE(exited[0] != 0);
    ASSERT_TRUE(wcscmp(muted, exited) != 0);

    /* And the four sentence shapes that carry them are four distinct entries,
     * not one entry reused -- which is what lets a translator put the state
     * clause where Arabic wants it in each. */
    ASSERT_TRUE(wcscmp(apr_str(APR_S_UI_TREE_SOURCE),
                       apr_str(APR_S_UI_TREE_SOURCE_STATE)) != 0);
    ASSERT_TRUE(wcscmp(apr_str(APR_S_UI_TREE_SOURCE_SHARED),
                       apr_str(APR_S_UI_TREE_SOURCE_STATE_SHARED)) != 0);
    ASSERT_TRUE(wcscmp(apr_str(APR_S_UI_TREE_SOURCE_UNUSED),
                       apr_str(APR_S_UI_TREE_SOURCE_UNUSED_STATE)) != 0);
}

/* A one-source, one-bus session whose source's health is whatever the caller
 * asked capture.h for. Shared by the two cases below; nothing else in this
 * file needs it. */
typedef struct StateModel {
    AprGraph   *g;
    AprSourceId src;
    AprBusId    bus;
} StateModel;

static int state_model_build(StateModel *m, const AprCaptureConfig *cfg)
{
    AprErr e;

    memset(m, 0, sizeof *m);
    e = apr_graph_create(RATE, 2, &m->g);
    if (apr_failed(&e) || !m->g) return 0;

    (void)apr_graph_add_bus(m->g, L"Main Mix", &m->bus);
    m->src = add_fake_cfg(m->g, L"Teams", cfg);
    if (m->src == 0) return 0;
    (void)apr_graph_connect(m->g, m->src, m->bus, 1.0f);

    /* The panel reads apr_source_muted()/apr_source_alive(), which are a
     * cached snapshot -- the frame owner refreshes them, so this stands in for
     * the frame's timer tick. No audio, no thread, no clock. */
    apr_source_poll(apr_graph_source(m->g, m->src), NULL);
    return 1;
}

/* The source row for a one-source session, whatever index it landed at. */
static int state_row_label(const StateModel *m, wchar_t *out, size_t cch)
{
    AprTreeRow r[MAXROWS];
    size_t n = apr_tree_panel_rows(m->g, r, MAXROWS);
    size_t i;

    for (i = 0; i < n && i < MAXROWS; i++) {
        if (r[i].sel.kind == APR_TREE_ROW_SOURCE && r[i].sel.source == m->src) {
            return apr_tree_panel_label(m->g, &r[i].sel, out, cch) > 0;
        }
    }
    return 0;
}

TEST(a_source_muted_in_the_windows_mixer_says_so_in_the_row_a_screen_reader_reads)
{
    /* Nothing else in the UI can tell you this. The recording will be pure
     * silence -- loopback is post-session-volume -- and every other indicator
     * will look perfect, so the fact has to be IN THE NAME, not in a colour. */
    StateModel m;
    AprCaptureConfig cfg;
    wchar_t muted_text[512], healthy_text[512];
    StateModel healthy;

    memset(&cfg, 0, sizeof cfg);
    cfg.kind = APR_SRC_FAKE;
    cfg.fake.start_muted = 1;

    (void)apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    ASSERT_TRUE(state_model_build(&m, &cfg));
    ASSERT_EQ_INT(1, apr_source_muted(apr_graph_source(m.g, m.src)));
    ASSERT_EQ_INT(1, apr_source_alive(apr_graph_source(m.g, m.src)));
    ASSERT_TRUE(state_row_label(&m, muted_text, 512));

    memset(&cfg, 0, sizeof cfg);
    cfg.kind = APR_SRC_FAKE;
    ASSERT_TRUE(state_model_build(&healthy, &cfg));
    ASSERT_TRUE(state_row_label(&healthy, healthy_text, 512));

    /* The state clause is present, and it is the MUTED one specifically. */
    ASSERT_NOT_NULL(wcsstr(muted_text, apr_str(APR_S_UI_TREE_STATE_MUTED)));
    ASSERT_NULL(wcsstr(muted_text, apr_str(APR_S_UI_TREE_STATE_EXITED)));
    /* Still a complete row, not just a state: name and bus survive. */
    ASSERT_NOT_NULL(wcsstr(muted_text, L"Teams"));
    ASSERT_NOT_NULL(wcsstr(muted_text, L"Main Mix"));
    /* And a healthy source is not given it by accident. */
    ASSERT_TRUE(wcscmp(muted_text, healthy_text) != 0);
    ASSERT_NULL(wcsstr(healthy_text, apr_str(APR_S_UI_TREE_STATE_MUTED)));

    apr_graph_destroy(healthy.g);
    apr_graph_destroy(m.g);
}

TEST(a_source_whose_process_exited_says_so_rather_than_looking_healthy)
{
    /* The other silent failure: loopback keeps producing buffers of zeros for
     * ever and WASAPI never reports the death, so without this clause closing
     * Teams mid-session reads as a successful recording. */
    StateModel m;
    AprCaptureConfig cfg;
    wchar_t text[512];

    memset(&cfg, 0, sizeof cfg);
    cfg.kind = APR_SRC_FAKE;
    cfg.fake.start_dead = 1;

    (void)apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    ASSERT_TRUE(state_model_build(&m, &cfg));
    ASSERT_EQ_INT(0, apr_source_alive(apr_graph_source(m.g, m.src)));
    ASSERT_TRUE(state_row_label(&m, text, 512));

    ASSERT_NOT_NULL(wcsstr(text, apr_str(APR_S_UI_TREE_STATE_EXITED)));
    ASSERT_NULL(wcsstr(text, apr_str(APR_S_UI_TREE_STATE_MUTED)));
    ASSERT_NOT_NULL(wcsstr(text, L"Teams"));

    apr_graph_destroy(m.g);
}

TEST(a_dead_source_is_reported_as_dead_even_while_it_is_also_muted)
{
    /* Both at once is a real state -- a muted app whose process then exits --
     * and the row can only carry one clause. Death is the one that has to win:
     * unmuting a dead app fixes nothing, and telling the user to check the
     * volume mixer sends them to the wrong place. */
    StateModel m;
    AprCaptureConfig cfg;
    wchar_t text[512];

    memset(&cfg, 0, sizeof cfg);
    cfg.kind = APR_SRC_FAKE;
    cfg.fake.start_muted = 1;
    cfg.fake.start_dead  = 1;

    (void)apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    ASSERT_TRUE(state_model_build(&m, &cfg));
    ASSERT_EQ_INT(1, apr_source_muted(apr_graph_source(m.g, m.src)));
    ASSERT_EQ_INT(0, apr_source_alive(apr_graph_source(m.g, m.src)));
    ASSERT_TRUE(state_row_label(&m, text, 512));

    ASSERT_NOT_NULL(wcsstr(text, apr_str(APR_S_UI_TREE_STATE_EXITED)));
    ASSERT_NULL(wcsstr(text, apr_str(APR_S_UI_TREE_STATE_MUTED)));

    apr_graph_destroy(m.g);
}

/* ==========================================================================
 * The live half
 * ======================================================================== */

typedef struct UiHost {
    HANDLE    thread;   /* the UI thread; GetGUIThreadInfo is asked of it */
    HANDLE    ready;
    AprUiApp *app;
    HWND      frame;
    int       failed;
} UiHost;

static DWORD WINAPI ui_thread(LPVOID param)
{
    UiHost *h = (UiHost *)param;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    AprErr e;

    e = apr_ui_app_create(GetModuleHandleW(NULL), &h->app);
    if (apr_failed(&e)) {
        h->failed = 1;
        SetEvent(h->ready);
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }

    h->frame = apr_ui_app_hwnd(h->app);
    apr_ui_app_show(h->app, SW_SHOWNORMAL);
    {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
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
    return h->failed ? 0 : (h->frame != NULL);
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

/* ---- TreeView, asked through its own API ------------------------------- */

static HTREEITEM tv_next(HWND tv, UINT what, HTREEITEM from)
{
    return (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, what, (LPARAM)from);
}

static int tv_row(HWND tv, HTREEITEM it)
{
    TVITEMW t;

    if (!it) return -1;
    memset(&t, 0, sizeof t);
    t.mask = TVIF_PARAM;
    t.hItem = it;
    if (!SendMessageW(tv, TVM_GETITEMW, 0, (LPARAM)&t)) return -1;
    return (int)t.lParam;
}

/* Every item in display order, as row indices. */
static size_t tv_visible_rows(HWND tv, int *out, size_t cap)
{
    HTREEITEM it = tv_next(tv, TVGN_ROOT, NULL);
    size_t n = 0;

    while (it && n < cap) {
        out[n++] = tv_row(tv, it);
        it = tv_next(tv, TVGN_NEXTVISIBLE, it);
    }
    return n;
}

/* ---- UIA --------------------------------------------------------------- */

typedef struct Uia {
    IUIAutomation           *uia;
    IUIAutomationTreeWalker *walker;
    int                      com_ok;
} Uia;

static int uia_open(Uia *c)
{
    HRESULT hr;

    memset(c, 0, sizeof *c);
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    c->com_ok = SUCCEEDED(hr);
    if (!c->com_ok) return 0;
    hr = CoCreateInstance(&kCLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                          &kIID_IUIAutomation, (void **)&c->uia);
    if (FAILED(hr) || !c->uia) return 0;
    hr = IUIAutomation_get_RawViewWalker(c->uia, &c->walker);
    return SUCCEEDED(hr) && c->walker;
}

static void uia_close(Uia *c)
{
    if (c->walker) IUIAutomationTreeWalker_Release(c->walker);
    if (c->uia)    IUIAutomation_Release(c->uia);
    if (c->com_ok) CoUninitialize();
    memset(c, 0, sizeof *c);
}

static void el_name(IUIAutomationElement *el, wchar_t *buf, size_t cch)
{
    BSTR s = NULL;
    buf[0] = 0;
    if (SUCCEEDED(IUIAutomationElement_get_CurrentName(el, &s)) && s) {
        lstrcpynW(buf, s, (int)cch);
        SysFreeString(s);
    }
}

typedef struct Seen {
    wchar_t name[MAXROWS][512];
    int     type[MAXROWS];
    int     n;
    int     unnamed;
} Seen;

static void uia_walk(Uia *c, IUIAutomationElement *el, Seen *st)
{
    IUIAutomationElement *child = NULL, *next = NULL;
    CONTROLTYPEID t = 0;
    wchar_t name[512];

    if (!el) return;
    el_name(el, name, 512);
    (void)IUIAutomationElement_get_CurrentControlType(el, &t);

    if (st->n < MAXROWS) {
        lstrcpynW(st->name[st->n], name, 512);
        st->type[st->n] = (int)t;
        st->n++;
    }
    if (name[0] == 0) st->unnamed++;

    if (FAILED(IUIAutomationTreeWalker_GetFirstChildElement(c->walker, el, &child))) {
        return;
    }
    while (child) {
        uia_walk(c, child, st);
        next = NULL;
        if (FAILED(IUIAutomationTreeWalker_GetNextSiblingElement(c->walker, child, &next))) {
            next = NULL;
        }
        IUIAutomationElement_Release(child);
        child = next;
    }
}

/* The subtree rooted at the TreeView. Element 0 is the tree itself. */
static int uia_scan_tree(Uia *c, HWND tv, Seen *st)
{
    IUIAutomationElement *root = NULL;

    memset(st, 0, sizeof *st);
    if (FAILED(IUIAutomation_ElementFromHandle(c->uia, tv, &root)) || !root) {
        return 0;
    }
    uia_walk(c, root, st);
    IUIAutomationElement_Release(root);
    return st->n > 0;
}

static int seen_has(const Seen *st, const wchar_t *name)
{
    int i;
    for (i = 0; i < st->n; ++i) {
        if (wcscmp(st->name[i], name) == 0) return 1;
    }
    return 0;
}

/* ---- the live cases ---------------------------------------------------- */

typedef struct Live {
    UiHost h;
    Model  m;
    HWND   pane;
    HWND   tv;
} Live;

static int live_start(Live *L)
{
    memset(L, 0, sizeof *L);
    if (!ui_start(&L->h)) return 0;
    L->pane = apr_ui_app_pane(L->h.app, APR_PANE_TREE);
    if (!L->pane) return 0;
    L->tv = apr_tree_panel_treeview(L->pane);
    if (!L->tv) return 0;
    model_build(&L->m);
    if (!L->m.g) return 0;
    apr_tree_panel_set_graph(L->pane, L->m.g);
    return 1;
}

static void live_stop(Live *L)
{
    if (L->pane && IsWindow(L->pane)) apr_tree_panel_set_graph(L->pane, NULL);
    ui_stop(&L->h);
    model_free(&L->m);
}

TEST(the_panel_is_a_real_treeview_and_not_a_picture_of_one)
{
    Live L;
    wchar_t cls[64];

    if (!live_start(&L)) {
        printf("      SKIPPED: no window\n");
        live_stop(&L);
        return;
    }

    /* WC_TREEVIEW is the whole argument for using a standard control: comctl32
     * ships the accessibility server, and no hand-written provider of ours
     * would be as good OR as familiar to the person driving it. */
    ASSERT_GT_INT(0, GetClassNameW(L.tv, cls, 64));
    printf("      tree class: %ls\n", cls);
    ASSERT_EQ_INT(CSTR_EQUAL,
                  CompareStringOrdinal(cls, -1, WC_TREEVIEWW, -1, TRUE));

    /* And the panel around it is ours, painted by us, so it must NOT be a
     * TreeView and must not carry the mirrored DC (ui_app.h CONVENTION 1). */
    ASSERT_TRUE((GetWindowLongPtrW(L.pane, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) == 0);

    live_stop(&L);
}

TEST(the_live_tree_exposes_the_model_hierarchy_with_every_item_named)
{
    Live L;
    AprTreeRow r[MAXROWS];
    size_t n, i;
    int rows[MAXROWS];
    size_t visible;
    Uia c;
    Seen st;

    if (!live_start(&L)) { printf("      SKIPPED: no window\n"); live_stop(&L); return; }

    n = apr_tree_panel_rows(L.m.g, r, MAXROWS);
    ASSERT_EQ_INT(8, (int)n);

    /* HIERARCHY, asked of the control. Every depth-1 row's parent must be the
     * bus (or group) row above it, and every depth-0 row must have no parent.
     * This is the structure comctl32's provider reports Level from, so getting
     * it right here is what makes the UIA level right. */
    for (i = 0; i < n; ++i) {
        HTREEITEM it, parent;
        int pr;

        it = NULL;
        {
            /* find the item whose lParam is i */
            HTREEITEM walk = tv_next(L.tv, TVGN_ROOT, NULL);
            while (walk) {
                if (tv_row(L.tv, walk) == (int)i) { it = walk; break; }
                walk = tv_next(L.tv, TVGN_NEXTVISIBLE, walk);
            }
        }
        ASSERT_NOT_NULL(it);
        parent = tv_next(L.tv, TVGN_PARENT, it);
        if (r[i].depth == 0) {
            ASSERT_NULL(parent);
        } else {
            ASSERT_NOT_NULL(parent);
            pr = tv_row(L.tv, parent);
            ASSERT_TRUE(pr >= 0 && pr < (int)i);
            ASSERT_EQ_INT(r[i].depth - 1, r[pr].depth);
        }
    }

    /* ORDER, and the fact that everything is reachable without expanding
     * anything: the panel opens expanded, so all eight rows are visible. */
    visible = tv_visible_rows(L.tv, rows, MAXROWS);
    ASSERT_EQ_INT((int)n, (int)visible);
    for (i = 0; i < visible; ++i) ASSERT_EQ_INT((int)i, rows[i]);

    /* NAMES, asked of UI Automation, because that is what a screen reader
     * reads -- not our TVITEMW, not our row array. */
    if (!uia_open(&c)) {
        printf("      SKIPPED (names): UI Automation unavailable here\n");
        uia_close(&c);
        live_stop(&L);
        return;
    }
    ASSERT_TRUE(uia_scan_tree(&c, L.tv, &st));
    printf("      UIA: %d elements under the tree, %d unnamed\n",
           st.n, st.unnamed);
    ASSERT_EQ_INT(0, st.unnamed);
    ASSERT_EQ_INT(UIA_TreeControlTypeId, st.type[0]);

    for (i = 0; i < n; ++i) {
        wchar_t want[512];
        apr_tree_panel_label(L.m.g, &r[i].sel, want, 512);
        if (!seen_has(&st, want)) {
            printf("      MISSING from the UIA tree: \"%ls\"\n", want);
        }
        ASSERT_TRUE(seen_has(&st, want));
    }

    uia_close(&c);
    live_stop(&L);
}

TEST(custom_draw_ran_and_the_accessibility_tree_is_still_intact)
{
    Live L;
    AprTreeRow r[MAXROWS];
    size_t n, i;
    unsigned drawn;
    Uia c;
    Seen st;

    if (!live_start(&L)) { printf("      SKIPPED: no window\n"); live_stop(&L); return; }

    /* RDW_UPDATENOW sends WM_PAINT to the owning thread synchronously, so the
     * control really repaints before the counter is read rather than at some
     * unspecified later time. */
    RedrawWindow(L.tv, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

    drawn = apr_tree_panel_test_customdraw_items(L.pane);
    printf("      NM_CUSTOMDRAW item stages: %u\n", drawn);

    /* HALF ONE: custom draw actually fired. Without this the rest of the case
     * passes for free on a build where the handler was deleted -- which is
     * precisely the state the regression would leave behind. */
    ASSERT_GT_INT(0, (int)drawn);

    /* HALF TWO: and the control is still a Tree full of named TreeItems.
     * THIS IS AGENTS.md RULE 5 AS AN ASSERTION. Owner-draw would replace the
     * control's semantics: the type would stop being Tree, or the items would
     * lose their names, or both -- and the pixels would look fine. */
    if (!uia_open(&c)) {
        printf("      SKIPPED (a11y half): UI Automation unavailable here\n");
        uia_close(&c);
        live_stop(&L);
        return;
    }
    ASSERT_TRUE(uia_scan_tree(&c, L.tv, &st));
    ASSERT_EQ_INT(UIA_TreeControlTypeId, st.type[0]);
    ASSERT_EQ_INT(0, st.unnamed);

    n = apr_tree_panel_rows(L.m.g, r, MAXROWS);
    for (i = 0; i < n; ++i) {
        wchar_t want[512];
        apr_tree_panel_label(L.m.g, &r[i].sel, want, 512);
        ASSERT_TRUE(seen_has(&st, want));
    }
    /* The tree element itself plus at least one element per row. (More is
     * fine and expected: the control's own scroll bars and their buttons are
     * real elements too, and they are named by the OS.) */
    ASSERT_GE_INT((int)n + 1, st.n);

    uia_close(&c);
    live_stop(&L);
}

TEST(the_keyboard_alone_reaches_every_row)
{
    Live L;
    int visited[MAXROWS];
    int i, tries, count = 0;
    size_t n;
    AprTreeRow r[MAXROWS];

    if (!live_start(&L)) { printf("      SKIPPED: no window\n"); live_stop(&L); return; }

    n = apr_tree_panel_rows(L.m.g, r, MAXROWS);
    memset(visited, 0, sizeof visited);

    /* Focus arrives the way it does for a user: the frame hands it to the
     * first pane, which hands it straight on to the tree.
     *
     * THAT SECOND HOP IS THE POINT. If focus stopped on the pane, a screen
     * reader would announce "Structure, pane" and nothing else, and the user
     * would have to guess that there is a tree inside it. */
    SendMessageTimeoutW(L.h.frame, WM_SETFOCUS, 0, 0, SMTO_ABORTIFHUNG, 3000, NULL);
    {
        GUITHREADINFO gti;
        int t;
        HWND f = NULL;
        for (t = 0; t < 40 && f != L.tv; ++t) {
            Sleep(15);
            memset(&gti, 0, sizeof gti);
            gti.cbSize = sizeof gti;
            /* GetGUIThreadInfo, not UIA's GetFocusedElement: the latter is
             * per-desktop and needs the window to be foreground, which a test
             * launched by a build system is not. */
            if (GetGUIThreadInfo(GetThreadId(L.h.thread), &gti)) f = gti.hwndFocus;
        }
        printf("      focus after the frame handed it on: %p (tree %p, pane %p)\n",
               (void *)f, (void *)L.tv, (void *)L.pane);
        ASSERT_TRUE(f == L.tv);
    }

    PostMessageW(L.tv, WM_KEYDOWN, VK_HOME, 0);
    PostMessageW(L.tv, WM_KEYUP, VK_HOME, 0);

    for (tries = 0; tries < (int)n * 3 + 8; ++tries) {
        HTREEITEM cur;
        int row;

        Sleep(15);
        cur = tv_next(L.tv, TVGN_CARET, NULL);
        row = tv_row(L.tv, cur);
        if (row >= 0 && row < MAXROWS && !visited[row]) {
            visited[row] = 1;
            count++;
        }
        PostMessageW(L.tv, WM_KEYDOWN, VK_DOWN, 0);
        PostMessageW(L.tv, WM_KEYUP, VK_DOWN, 0);
    }

    for (i = 0; i < (int)n; ++i) {
        if (!visited[i]) printf("      row %d was never reached by Down Arrow\n", i);
        ASSERT_TRUE(visited[i]);
    }
    printf("      Down Arrow reached %d of %d rows\n", count, (int)n);

    live_stop(&L);
}

/* ---- selection coherence ------------------------------------------------ */

static volatile LONG g_sink_calls;
static AprTreeSel    g_sink_last;

static void sink(HWND panel, const AprTreeSel *sel, void *user)
{
    (void)panel;
    (void)user;
    g_sink_last = *sel;
    InterlockedIncrement(&g_sink_calls);
}

TEST(selection_is_a_model_identity_and_does_not_echo)
{
    Live L;
    AprTreeSel want, got;
    LONG before;
    int tries;

    if (!live_start(&L)) { printf("      SKIPPED: no window\n"); live_stop(&L); return; }

    g_sink_calls = 0;
    memset(&g_sink_last, 0, sizeof g_sink_last);
    apr_tree_panel_set_selection_sink(L.pane, sink, NULL);

    /* THE COHERENCE CONTRACT. The canvas cannot hand the tree an HTREEITEM and
     * the tree cannot hand the canvas an HWND, so both name the same MODEL
     * object. "Teams, on Teams Only" is a different row from "Teams, on Main
     * Mix" and the bus in the selection is what distinguishes them -- it is
     * also exactly the identity of the edge the canvas draws. */
    memset(&want, 0, sizeof want);
    want.kind   = APR_TREE_ROW_SOURCE;
    want.bus    = L.m.teams_only;
    want.source = L.m.teams;

    before = g_sink_calls;
    ASSERT_EQ_INT(1, apr_tree_panel_select(L.pane, &want));
    ASSERT_EQ_INT(1, apr_tree_panel_get_selection(L.pane, &got));
    ASSERT_EQ_INT((int)want.kind,   (int)got.kind);
    ASSERT_EQ_INT((int)want.bus,    (int)got.bus);
    ASSERT_EQ_INT((int)want.source, (int)got.source);

    /* A view that echoed the selection back at whoever set it turns one user
     * action into a loop between two panes. */
    ASSERT_EQ_INT((int)before, (int)g_sink_calls);

    /* The same source under the OTHER bus is a different row, and selecting it
     * really moves. */
    want.bus = L.m.main_mix;
    ASSERT_EQ_INT(1, apr_tree_panel_select(L.pane, &want));
    ASSERT_EQ_INT(1, apr_tree_panel_get_selection(L.pane, &got));
    ASSERT_EQ_INT((int)L.m.main_mix, (int)got.bus);
    ASSERT_EQ_INT((int)before, (int)g_sink_calls);

    /* But a move the USER makes is reported, or the canvas could never follow
     * the tree. */
    PostMessageW(L.tv, WM_KEYDOWN, VK_DOWN, 0);
    PostMessageW(L.tv, WM_KEYUP, VK_DOWN, 0);
    for (tries = 0; tries < 60 && g_sink_calls == before; ++tries) Sleep(15);
    ASSERT_GT_INT((int)before, (int)g_sink_calls);
    ASSERT_TRUE(g_sink_last.kind != APR_TREE_ROW_NONE);

    /* Nothing in the graph should be selectable that the graph does not
     * contain. */
    memset(&want, 0, sizeof want);
    want.kind = APR_TREE_ROW_SOURCE;
    want.bus = 9999;
    want.source = 9999;
    ASSERT_EQ_INT(0, apr_tree_panel_select(L.pane, &want));

    apr_tree_panel_set_selection_sink(L.pane, NULL, NULL);
    live_stop(&L);
}

/* ---- direction, on a live control --------------------------------------- */

TEST(the_treeview_mirrors_but_the_row_order_does_not)
{
    Live L;
    AprErr e;
    int ltr_rows[MAXROWS], rtl_rows[MAXROWS];
    size_t n_ltr, n_rtl, i;
    LONG_PTR ex_tv, ex_pane;

    e = apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    ASSERT_FALSE(apr_failed(&e));
    if (!live_start(&L)) { printf("      SKIPPED: no window\n"); live_stop(&L); return; }
    n_ltr = tv_visible_rows(L.tv, ltr_rows, MAXROWS);
    ASSERT_TRUE((GetWindowLongPtrW(L.tv, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) == 0);
    live_stop(&L);

    e = apr_str_set_language(MAKELANGID(LANG_ARABIC, SUBLANG_ARABIC_SAUDI_ARABIA));
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(1, apr_str_is_rtl());
    if (!live_start(&L)) {
        printf("      SKIPPED: no window\n");
        live_stop(&L);
        (void)apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
        return;
    }

    ex_tv   = GetWindowLongPtrW(L.tv, GWL_EXSTYLE);
    ex_pane = GetWindowLongPtrW(L.pane, GWL_EXSTYLE);

    /* THE STANDARD CONTROL MIRRORS -- indent guides, expand buttons, scroll
     * bar side and text alignment all flip, and every one of those is
     * something we would otherwise be hand-mirroring wrong. It is only safe
     * because our NM_CUSTOMDRAW handler passes NO coordinates into the
     * mirrored DC; see the note at the top of tree_panel.c. */
    ASSERT_TRUE((ex_tv & WS_EX_LAYOUTRTL) != 0);

    /* THE PANEL AROUND IT DOES NOT. It has a WM_PAINT, and a mirrored DC would
     * reflect everything we draw into it (ui_app.h CONVENTION 1). */
    ASSERT_TRUE((ex_pane & WS_EX_LAYOUTRTL) == 0);

    /* AND THE HALF EVERYONE GETS WRONG: the ORDER is identical. A screen
     * reader user's navigation must not reverse when the interface language
     * changes. Only the painting does. */
    n_rtl = tv_visible_rows(L.tv, rtl_rows, MAXROWS);
    ASSERT_EQ_INT((int)n_ltr, (int)n_rtl);
    for (i = 0; i < n_rtl; ++i) ASSERT_EQ_INT(ltr_rows[i], rtl_rows[i]);

    live_stop(&L);
    (void)apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
}
