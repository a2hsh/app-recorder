/*
 * test_ui_dialogs.c -- the surfaces that turn "navigate a graph" into "build
 * and record one": the dialog layer, the notification area, and the
 * controller that owns both.
 *
 * ===========================================================================
 * WHAT IS ASSERTED, AND WHY IN THIS SHAPE
 *
 *   THE PURE HALF, DIRECTLY. Every row a person hears -- an application in the
 *   picker, a capture device, a keyboard shortcut, a line of a session's
 *   resolve report -- is produced by a pure function that takes no window. So
 *   "the row says the application is muted" is asserted on the text itself,
 *   not inferred from pixels, and both directions of the interface can be
 *   driven in one process.
 *
 *   THE TRAY, AS A MENU AND A SENTENCE. The context menu is a real HMENU, so
 *   the same properties the frame's menu bar has to satisfy are checkable the
 *   same way: every item named, every item carrying a mnemonic, no duplicates.
 *   And the tooltip is a status READOUT that Windows+B reaches, so what it
 *   says in each state is asserted rather than reviewed.
 *
 *   THE CONTROLLER, THROUGH THE REAL MESSAGE LOOP. Recording is driven by
 *   posting WM_COMMAND at the frame -- the same route the menu, the
 *   accelerator table and the tray's context menu all take. A test that called
 *   the handler directly would be testing a parallel path; this one cannot,
 *   because there is no parallel path to call.
 *
 * ===========================================================================
 * SAFETY (AGENTS.md rule 1)
 *
 *   The recording cases use APR_SRC_FAKE and nothing else. No audio endpoint
 *   is opened and not one sample is rendered to any output device. The
 *   recordings go to temporary files that each case deletes.
 *
 * ===========================================================================
 * WHAT THIS SUITE CANNOT DO
 *
 *   A modal dialog owns the thread that opened it, so apr_dlg_add_source() and
 *   its siblings cannot be entered and answered from the same thread that is
 *   asserting. Their CONTENT is tested through the pure row builders they are
 *   made of; their keyboard reachability is tested through the menu and the
 *   accelerator table that reach them. Driving a live modal would mean a
 *   second thread posting synthetic keystrokes, which tests the input queue
 *   more than it tests the dialog.
 */
#include "test_runner.h"
#include "test_wait.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <stdio.h>
#include <wctype.h>

#include "action.h"
#include "capture.h"
#include "graph.h"
#include "strings.h"
#include "ui_app.h"
#include "ui_canvas.h"
#include "ui_controller.h"
#include "ui_dialogs.h"
#include "ui_tray.h"

/* ==========================================================================
 * The pure half
 * ======================================================================== */

TEST(an_application_row_says_which_state_would_change_the_recording)
{
    AprAudioApp a;
    wchar_t row[512];

    memset(&a, 0, sizeof a);
    wcscpy_s(a.exe, APR_DISC_NAME_CCH, L"chrome.exe");
    wcscpy_s(a.display, APR_DISC_NAME_CCH, L"Chrome");
    a.pid = 8412;
    a.active = 1;

    ASSERT_GT_INT(0, (int)apr_dlg_app_row(&a, row, 512));
    printf("      active: \"%ls\"\n", row);
    ASSERT_NOT_NULL(wcsstr(row, L"Chrome"));
    ASSERT_NOT_NULL(wcsstr(row, L"8412"));
    ASSERT_NOT_NULL(wcsstr(row, apr_str(APR_S_UI_DLG_APP_STATE_ACTIVE)));

    /* MUTED BEATS ACTIVE. Loopback is post-session-volume, so an application
     * that is playing and muted records digital silence -- and that, not
     * "playing audio now", is the fact that changes what the file contains.
     * A picker that said the cheerful half would be the reason a user records
     * an hour of nothing. */
    a.muted = 1;
    ASSERT_GT_INT(0, (int)apr_dlg_app_row(&a, row, 512));
    printf("      muted:  \"%ls\"\n", row);
    ASSERT_NOT_NULL(wcsstr(row, apr_str(APR_S_UI_DLG_APP_STATE_MUTED)));
    ASSERT_NULL(wcsstr(row, apr_str(APR_S_UI_DLG_APP_STATE_ACTIVE)));

    a.muted = 0;
    a.active = 0;
    ASSERT_GT_INT(0, (int)apr_dlg_app_row(&a, row, 512));
    printf("      idle:   \"%ls\"\n", row);
    ASSERT_NOT_NULL(wcsstr(row, apr_str(APR_S_UI_DLG_APP_STATE_IDLE)));
}

TEST(an_application_with_no_session_name_is_still_named)
{
    AprAudioApp a;
    wchar_t row[512];

    memset(&a, 0, sizeof a);
    wcscpy_s(a.exe, APR_DISC_NAME_CCH, L"weird.exe");
    a.pid = 7;

    /* A nameless row is a row a screen reader reads as nothing at all, so the
     * executable stands in when the audio session supplies no display name. */
    ASSERT_GT_INT(0, (int)apr_dlg_app_row(&a, row, 512));
    ASSERT_NOT_NULL(wcsstr(row, L"weird.exe"));
}

TEST(a_capture_device_row_names_the_default_one_as_the_default)
{
    AprAudioEndpoint e;
    wchar_t row[512];

    memset(&e, 0, sizeof e);
    wcscpy_s(e.name, APR_DISC_NAME_CCH, L"Chat Mic (GoXLR)");

    ASSERT_GT_INT(0, (int)apr_dlg_endpoint_row(&e, row, 512));
    ASSERT_NOT_NULL(wcsstr(row, L"Chat Mic (GoXLR)"));

    /* The default device gets a DIFFERENT sentence, not the same one with a
     * suffix glued on -- the two are separate catalog entries so a translator
     * can put the qualifier wherever it belongs in their language. */
    e.is_default = 1;
    ASSERT_GT_INT(0, (int)apr_dlg_endpoint_row(&e, row, 512));
    printf("      \"%ls\"\n", row);
    ASSERT_NOT_NULL(wcsstr(row, L"Chat Mic (GoXLR)"));
    ASSERT_GT_INT((int)wcslen(L"Chat Mic (GoXLR)"), (int)wcslen(row));
}

TEST(a_key_name_is_built_from_the_catalog_not_from_a_plus_in_code)
{
    wchar_t k[128];

    /* THE CATALOG, NOT A LITERAL -- in the test as well as in the code. An
     * assertion written as L"Delete" agrees with a hardcoded L"Delete" in
     * dialogs.c and would have gone on passing while the Help window read half
     * in the interface language and half in English. */
    ASSERT_GT_INT(0, (int)apr_dlg_key_name(VK_DELETE, 0, k, 128));
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_KEYNAME_DELETE), k);

    ASSERT_GT_INT(0, (int)apr_dlg_key_name('E', APR_KMOD_CTRL, k, 128));
    printf("      \"%ls\"\n", k);
    ASSERT_NOT_NULL(wcsstr(k, apr_str(APR_S_UI_DLG_KEY_CTRL)));
    ASSERT_NOT_NULL(wcsstr(k, L"E"));

    ASSERT_GT_INT(0, (int)apr_dlg_key_name('E', APR_KMOD_CTRL | APR_KMOD_SHIFT,
                                           k, 128));
    printf("      \"%ls\"\n", k);
    ASSERT_NOT_NULL(wcsstr(k, apr_str(APR_S_UI_DLG_KEY_CTRL)));
    ASSERT_NOT_NULL(wcsstr(k, apr_str(APR_S_UI_DLG_KEY_SHIFT)));

    {
        /* "F%1!s!", with the number through apr_str_number() like every other
         * digit this product shows. */
        const wchar_t *args[1];
        wchar_t want[128], one[24];
        apr_str_number(1, one, 24);
        args[0] = one;
        apr_str_format(APR_S_UI_KEYNAME_FUNCTION, want, 128, args, 1);
        ASSERT_GT_INT(0, (int)apr_dlg_key_name(VK_F1, 0, k, 128));
        ASSERT_WSTR_EQ(want, k);
    }
}

TEST(every_named_key_takes_its_name_from_the_catalog)
{
    /* AGENTS.md rule 6 has no exception for key names, and the argument that
     * kept them as C literals -- "a translated Delete that no keyboard sends
     * is worse than an English one that matches the keycap" -- may well be the
     * right ANSWER, but it is a TRANSLATION decision. Frozen in C it produced
     * a shortcut rendered half in the interface language (the modifiers, which
     * were already in the catalog) and half in English. A translator who
     * agrees with the old argument writes the English word in the Arabic block
     * and gets exactly the old behaviour; nobody else has to. */
    static const struct { UINT vk; AprStrId id; } named[] = {
        { VK_TAB,        APR_S_UI_KEYNAME_TAB },
        { VK_RETURN,     APR_S_UI_KEYNAME_ENTER },
        { VK_SPACE,      APR_S_UI_KEYNAME_SPACE },
        { VK_DELETE,     APR_S_UI_KEYNAME_DELETE },
        { VK_ESCAPE,     APR_S_UI_KEYNAME_ESCAPE },
        { VK_HOME,       APR_S_UI_KEYNAME_HOME },
        { VK_END,        APR_S_UI_KEYNAME_END },
        { VK_LEFT,       APR_S_UI_KEYNAME_LEFT },
        { VK_RIGHT,      APR_S_UI_KEYNAME_RIGHT },
        { VK_UP,         APR_S_UI_KEYNAME_UP },
        { VK_DOWN,       APR_S_UI_KEYNAME_DOWN },
        { VK_OEM_PLUS,   APR_S_UI_KEYNAME_PLUS },
        { VK_OEM_MINUS,  APR_S_UI_KEYNAME_MINUS },
        { VK_ADD,        APR_S_UI_KEYNAME_NUM_PLUS },
        { VK_SUBTRACT,   APR_S_UI_KEYNAME_NUM_MINUS },
        { VK_OEM_PERIOD, APR_S_UI_KEYNAME_PERIOD }
    };
    size_t i;

    for (i = 0; i < sizeof named / sizeof named[0]; ++i) {
        wchar_t k[128];
        const wchar_t *want = apr_str(named[i].id);
        ASSERT_GT_INT(0, (int)apr_dlg_key_name(named[i].vk, 0, k, 128));
        printf("      0x%02X -> \"%ls\"\n", named[i].vk, k);
        ASSERT_TRUE(want[0] != 0);   /* an empty catalog entry is a defect */
        ASSERT_WSTR_EQ(want, k);
    }
}

TEST(a_button_is_sized_to_its_own_caption_and_not_to_the_english_one)
{
    /* NEVER SIZE A CONTROL TO FIT ITS ENGLISH STRING (AGENTS.md rule 6).
     * The dialog buttons were a fixed 96 dialog units, which is not enough for
     * "Stop the recording and close" in ENGLISH -- and Arabic needs more room
     * again at the same point size, so the Arabic pass would have shipped
     * captions that could not be read.
     *
     * Pure, so the rule is a property a test can hold rather than something a
     * reviewer has to notice. */
    short small_ = apr_dlg_button_width(L"OK");
    short big = apr_dlg_button_width(apr_str(APR_S_UI_CLOSE_STOP_AND_EXIT));
    short longer = apr_dlg_button_width(
        L"a caption considerably longer than the one before it");

    printf("      \"OK\" -> %d du; \"%ls\" -> %d du\n",
           (int)small_, apr_str(APR_S_UI_CLOSE_STOP_AND_EXIT), (int)big);

    /* A floor, so a two-letter caption still gets a button worth aiming at. */
    ASSERT_GE_INT(APR_DLG_BUTTON_MIN_DU, (int)small_);

    /* And it GROWS -- the whole of the fix. 96 was the old fixed width. */
    ASSERT_GT_INT(96, (int)big);
    ASSERT_GT_INT((int)big, (int)longer);

    /* NULL is a caption too, as far as not crashing goes. */
    ASSERT_GE_INT(APR_DLG_BUTTON_MIN_DU, (int)apr_dlg_button_width(NULL));
}

/* Help > Keyboard Shortcuts renders the canvas's ONE binding table, so a
 * shortcut cannot be documented as one key and implemented as another. This
 * asserts the render, not the table -- test_ui_canvas.c owns the table. */
TEST(every_canvas_operation_renders_a_shortcut_row_with_a_key_in_it)
{
    size_t i, n = apr_canvas_binding_count();

    ASSERT_GT_INT(0, (int)n);
    for (i = 0; i < n; ++i) {
        const AprCanvasBinding *b = apr_canvas_binding_at(i);
        wchar_t row[512];
        wchar_t key[128];

        ASSERT_NOT_NULL(b);
        ASSERT_GT_INT(0, (int)apr_dlg_binding_row(b, row, 512));

        /* The label is what the operation IS and the key is what performs it.
         * Both have to be in the row, or the screen is a list of half
         * sentences. */
        ASSERT_NOT_NULL(wcsstr(row, apr_str(b->label)));
        apr_dlg_key_name(b->vk, b->mods, key, 128);
        ASSERT_TRUE(key[0] != 0);
        ASSERT_NOT_NULL(wcsstr(row, key));
    }
    printf("      %u shortcut rows, all with a label and a key\n", (unsigned)n);
}

/* session.h returns what was asked for, what was substituted and every rival
 * candidate specifically so a UI can show it. The row builder must therefore
 * name BOTH halves for a substitution -- "Teams could not be found" is the
 * sentence that module exists to prevent. */
TEST(a_resolution_row_names_both_what_was_asked_for_and_what_was_used)
{
    AprSessionResolution r;
    wchar_t row[1024];

    memset(&r, 0, sizeof r);
    wcscpy_s(r.name, APR_NAME_CCH, L"Teams");
    wcscpy_s(r.wanted, APR_DISC_PATH_CCH, L"C:\\Old\\Teams.exe");
    wcscpy_s(r.substituted, APR_DISC_PATH_CCH, L"D:\\New\\Teams.exe");
    r.status = APR_SESSION_MOVED;

    ASSERT_GT_INT(0, (int)apr_dlg_resolution_row(&r, row, 1024));
    printf("      \"%ls\"\n", row);
    ASSERT_NOT_NULL(wcsstr(row, L"Teams"));
    ASSERT_NOT_NULL(wcsstr(row, L"D:\\New\\Teams.exe"));

    /* An exact match still gets a row. A report that only listed problems
     * would leave a screen reader user counting to work out which sources were
     * fine. */
    memset(&r, 0, sizeof r);
    wcscpy_s(r.name, APR_NAME_CCH, L"Chat Mic");
    r.status = APR_SESSION_DEVICE_EXACT;
    ASSERT_GT_INT(0, (int)apr_dlg_resolution_row(&r, row, 1024));
    printf("      \"%ls\"\n", row);
    ASSERT_NOT_NULL(wcsstr(row, L"Chat Mic"));

    /* And the one that must never be silent. */
    memset(&r, 0, sizeof r);
    wcscpy_s(r.name, APR_NAME_CCH, L"Discord");
    r.status = APR_SESSION_NEEDS_CONSENT;
    ASSERT_GT_INT(0, (int)apr_dlg_resolution_row(&r, row, 1024));
    printf("      \"%ls\"\n", row);
    ASSERT_TRUE(row[0] != 0);
}

TEST(every_row_builder_survives_a_null_and_a_tiny_buffer)
{
    wchar_t tiny[1];

    ASSERT_EQ_INT(0, (int)apr_dlg_app_row(NULL, tiny, 1));
    ASSERT_EQ_INT(0, (int)apr_dlg_endpoint_row(NULL, tiny, 1));
    ASSERT_EQ_INT(0, (int)apr_dlg_resolution_row(NULL, tiny, 1));
    ASSERT_EQ_INT(0, (int)apr_dlg_binding_row(NULL, tiny, 1));
    ASSERT_EQ_INT(0, (int)apr_dlg_key_name('A', 0, NULL, 0));
    ASSERT_EQ_INT(0, tiny[0]);
}

/* ==========================================================================
 * Elapsed time
 * ======================================================================== */

TEST(elapsed_time_is_two_digit_hours_minutes_and_seconds)
{
    wchar_t buf[64];

    ASSERT_GT_INT(0, (int)apr_controller_format_elapsed(0, buf, 64));
    printf("      0 ms -> \"%ls\"\n", buf);
    ASSERT_WSTR_EQ(L"00:00:00", buf);

    apr_controller_format_elapsed(1000, buf, 64);
    ASSERT_WSTR_EQ(L"00:00:01", buf);

    apr_controller_format_elapsed(59 * 1000, buf, 64);
    ASSERT_WSTR_EQ(L"00:00:59", buf);

    apr_controller_format_elapsed(60 * 1000, buf, 64);
    ASSERT_WSTR_EQ(L"00:01:00", buf);

    /* An hour and twelve and a half minutes. This is the sentence the tray
     * tooltip carries, and it is what a Windows+B check reads out. */
    apr_controller_format_elapsed((3600 + 12 * 60 + 30) * 1000, buf, 64);
    printf("      \"%ls\"\n", buf);
    ASSERT_WSTR_EQ(L"01:12:30", buf);

    /* Past a day it keeps counting rather than wrapping: a recorder that ran
     * overnight must not claim it ran for four minutes. */
    apr_controller_format_elapsed((25 * 3600) * 1000, buf, 64);
    ASSERT_WSTR_EQ(L"25:00:00", buf);

    ASSERT_EQ_INT(0, (int)apr_controller_format_elapsed(0, NULL, 0));
}

/* ==========================================================================
 * A live frame
 * ======================================================================== */

typedef struct UiHost {
    HANDLE         thread;
    HANDLE         ready;
    AprUiApp      *app;
    AprController *ctl;
    HWND           frame;
    int            failed;
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

    e = apr_controller_create(h->app, &h->ctl);
    if (apr_failed(&e)) {
        h->failed = 1;
        apr_ui_app_destroy(h->app);
        h->app = NULL;
        SetEvent(h->ready);
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }

    h->frame = apr_ui_app_hwnd(h->app);
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
    memset(h, 0, sizeof *h);
    h->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!h->ready) return 0;
    h->thread = CreateThread(NULL, 0, ui_thread, h, 0, NULL);
    if (!h->thread) return 0;
    WaitForSingleObject(h->ready, APR_TEST_WAIT_MS);
    return !h->failed && h->frame != NULL;
}

static void ui_stop(UiHost *h)
{
    if (h->frame) PostMessageW(h->frame, WM_CLOSE, 0, 0);
    if (h->thread) {
        if (WaitForSingleObject(h->thread, APR_TEST_WAIT_MS) != WAIT_OBJECT_0) {
            printf("      WARNING: UI thread did not exit\n");
            TerminateThread(h->thread, 1);
        }
        CloseHandle(h->thread);
    }
    if (h->ready) CloseHandle(h->ready);
    memset(h, 0, sizeof *h);
}

/* Drive a command exactly as the menu, the accelerator table and the tray's
 * context menu all do: a WM_COMMAND at the frame. There is no second path to
 * call, which is the point. */
static void command(UiHost *h, int cmd)
{
    PostMessageW(h->frame, WM_COMMAND, (WPARAM)cmd, 0);
}

/* No ceiling argument: see tests/test_wait.h for why there is one number
 * and it is not tuned per site. */
static int wait_until(int (*pred)(UiHost *), UiHost *h)
{
    int ok;

    APR_WAIT_UNTIL(ok, pred(h));
    return ok;
}

static int is_recording(UiHost *h) { return apr_controller_recording(h->ctl); }
static int not_recording(UiHost *h) { return !apr_controller_recording(h->ctl); }

/* Wait until the controller has SAID something containing `needle`.
 *
 * Not the same wait as "is it still recording". The flag drops as soon as the
 * files are closed, and the sentence about it -- which is the part the author
 * actually receives -- is published a few instructions later. Polling for the
 * state and then reading the sentence is a race the test loses about one run
 * in three, and losing it would have looked like a defect in the
 * announcement rather than in the test. */
static int wait_for_said(UiHost *h, const wchar_t *needle)
{
    wchar_t said[1024];
    int ok;

    APR_WAIT_UNTIL(ok, (apr_controller_last_announcement(h->ctl, said, 1024),
                        wcsstr(said, needle) != NULL));
    return ok;
}

/* ==========================================================================
 * The controller, live
 * ======================================================================== */

TEST(a_new_window_starts_with_an_empty_graph_and_nothing_to_record)
{
    UiHost h;
    wchar_t said[1024];

    if (!ui_start(&h)) { printf("      SKIPPED: no window\n"); ui_stop(&h); return; }

    ASSERT_NOT_NULL(apr_controller_graph(h.ctl));
    ASSERT_EQ_INT(0, (int)apr_graph_bus_count(apr_controller_graph(h.ctl)));
    ASSERT_FALSE(apr_controller_recording(h.ctl));

    /* Asking to record with nothing configured is REFUSED OUT LOUD. A greyed
     * menu item says nothing at all to someone who cannot see it, so the
     * refusal is a sentence, and this is what asserts it exists. */
    command(&h, APR_CMD_RECORD_START);
    (void)wait_for_said(&h, apr_str(APR_S_UI_ANN_NOTHING_TO_RECORD));
    apr_controller_last_announcement(h.ctl, said, 1024);
    printf("      \"%ls\"\n", said);
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_ANN_NOTHING_TO_RECORD), said);
    ASSERT_FALSE(apr_controller_recording(h.ctl));

    ui_stop(&h);
}

TEST(recording_starts_stops_and_announces_both)
{
    UiHost h;
    AprGraph *g;
    AprCaptureConfig cfg;
    AprActionConfig acfg;
    AprSourceId sid = 0;
    AprBusId bus = 0;
    wchar_t path[MAX_PATH];
    wchar_t dir[MAX_PATH];
    wchar_t said[1024];
    AprErr e;
    DWORD n;

    if (!ui_start(&h)) { printf("      SKIPPED: no window\n"); ui_stop(&h); return; }

    n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsapr_ui_rec_%lu.wav",
                 dir, GetCurrentProcessId());

    /* AGENTS.md rule 1: a synthetic source, so nothing opens an endpoint and
     * nothing is rendered anywhere. */
    g = apr_controller_graph(h.ctl);
    memset(&cfg, 0, sizeof cfg);
    cfg.kind = APR_SRC_FAKE;
    cfg.fake.tone_hz = 440;
    cfg.fake.amplitude = 0.25f;
    e = apr_graph_add_source(g, L"synthetic", &cfg, &sid);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_graph_add_bus(g, L"Mix", &bus);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_graph_connect(g, sid, bus, 1.0f);
    ASSERT_FALSE(apr_failed(&e));

    memset(&acfg, 0, sizeof acfg);
    acfg.out_path = path;
    acfg.sample_rate = apr_graph_rate(g);
    acfg.channels = apr_graph_channels(g);
    e = apr_graph_add_action(g, bus, "wav", &acfg);
    ASSERT_FALSE(apr_failed(&e));

    command(&h, APR_CMD_RECORD_START);
    ASSERT_TRUE(wait_until(is_recording, &h));

    /* wait_for_said, for the reason its own comment gives about the STOP case:
     * the flag and the sentence are published a few instructions apart, so
     * polling the state and then reading the sentence is a race the TEST
     * loses -- and losing it looks exactly like a defect in the announcement.
     * The controller publishes the two adjacently now; this is still the
     * honest way to ask. */
    ASSERT_TRUE(wait_for_said(&h, apr_str(APR_S_UI_ANN_RECORD_STARTED)));
    apr_controller_last_announcement(h.ctl, said, 1024);
    printf("      start: \"%ls\"\n", said);
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_ANN_RECORD_STARTED), said);

    /* The clock is sampled by the loop every tick, so wait for it to move
     * rather than for four hundred milliseconds to pass. */
    {
        int ticked;
        APR_WAIT_UNTIL(ticked, apr_controller_elapsed_ms(h.ctl) > 0);
        ASSERT_TRUE(ticked);
    }
    ASSERT_GT_INT(0, (int)apr_controller_elapsed_ms(h.ctl));

    command(&h, APR_CMD_RECORD_STOP);
    ASSERT_TRUE(wait_until(not_recording, &h));

    /* The stop sentence carries the duration, so it is a whole sentence with
     * an insert rather than a bare "Stopped." */
    ASSERT_TRUE(wait_for_said(&h, L"00:00:0"));
    apr_controller_last_announcement(h.ctl, said, 1024);
    printf("      stop:  \"%ls\"\n", said);

    /* And it left a file behind, finalized, on the ordinary exit path. */
    ASSERT_TRUE(GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES);

    /* ui_stop BEFORE the delete: an action holds its file open until the graph
     * is destroyed, and the graph lives until the controller does. Deleting
     * first fails silently on Windows and leaves debris in %TEMP%. */
    ui_stop(&h);
    DeleteFileW(path);
}

TEST(editing_is_refused_out_loud_while_a_recording_runs)
{
    UiHost h;
    AprGraph *g;
    AprCaptureConfig cfg;
    AprActionConfig acfg;
    AprSourceId sid = 0;
    AprBusId bus = 0;
    wchar_t path[MAX_PATH], dir[MAX_PATH], said[1024];
    AprErr e;
    DWORD n;

    if (!ui_start(&h)) { printf("      SKIPPED: no window\n"); ui_stop(&h); return; }

    n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsapr_ui_busy_%lu.wav",
                 dir, GetCurrentProcessId());

    g = apr_controller_graph(h.ctl);
    memset(&cfg, 0, sizeof cfg);
    cfg.kind = APR_SRC_FAKE;
    cfg.fake.amplitude = 0.25f;
    e = apr_graph_add_source(g, L"synthetic", &cfg, &sid);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_graph_add_bus(g, L"Mix", &bus);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_graph_connect(g, sid, bus, 1.0f);
    ASSERT_FALSE(apr_failed(&e));
    memset(&acfg, 0, sizeof acfg);
    acfg.out_path = path;
    acfg.sample_rate = apr_graph_rate(g);
    acfg.channels = apr_graph_channels(g);
    e = apr_graph_add_action(g, bus, "wav", &acfg);
    ASSERT_FALSE(apr_failed(&e));

    command(&h, APR_CMD_RECORD_START);
    ASSERT_TRUE(wait_until(is_recording, &h));

    /* graph.h: the shape must not change while a tick is in flight. The menu
     * items are greyed, and the command STILL says why when it arrives by
     * accelerator -- because grey is not a message this application's first
     * user receives. */
    command(&h, APR_CMD_ADD_BUS);
    (void)wait_for_said(&h, apr_str(APR_S_UI_ANN_BUSY_RECORDING));
    apr_controller_last_announcement(h.ctl, said, 1024);
    printf("      \"%ls\"\n", said);
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_ANN_BUSY_RECORDING), said);
    ASSERT_EQ_INT(1, (int)apr_graph_bus_count(g));

    command(&h, APR_CMD_RECORD_STOP);
    ASSERT_TRUE(wait_until(not_recording, &h));
    ui_stop(&h);
    DeleteFileW(path);
}

/* The window can be closed while a recording is running only by a path that
 * finalizes first. This drives the OTHER half: with nothing recording, a close
 * is not questioned at all. The recording half needs a modal answer and is
 * covered by the controller's own logic plus test_run_loop.c's proof that
 * destroy finalizes. */
TEST(closing_an_idle_window_is_not_questioned)
{
    UiHost h;

    if (!ui_start(&h)) { printf("      SKIPPED: no window\n"); ui_stop(&h); return; }
    ASSERT_FALSE(apr_controller_recording(h.ctl));
    PostMessageW(h.frame, WM_CLOSE, 0, 0);
    ASSERT_EQ_INT(WAIT_OBJECT_0, (int)WaitForSingleObject(h.thread, APR_TEST_WAIT_MS));
    CloseHandle(h.thread);
    h.thread = NULL;
    h.frame = NULL;
    ui_stop(&h);
}

TEST(the_controller_enables_recording_only_once_there_is_an_output)
{
    UiHost h;
    AprGraph *g;
    HMENU bar;
    AprBusId bus = 0;
    AprCaptureConfig cfg;
    AprActionConfig acfg;
    AprSourceId sid = 0;
    wchar_t path[MAX_PATH], dir[MAX_PATH];
    AprErr e;
    DWORD n;

    if (!ui_start(&h)) { printf("      SKIPPED: no window\n"); ui_stop(&h); return; }

    bar = GetMenu(h.frame);
    ASSERT_NOT_NULL(bar);

    /* Empty graph: greyed, but present and named -- the rule the frame set and
     * the controller must not break. */
    ASSERT_TRUE((GetMenuState(bar, (UINT)APR_CMD_RECORD_START, MF_BYCOMMAND)
                 & MF_GRAYED) != 0);

    n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsapr_ui_menu_%lu.wav",
                 dir, GetCurrentProcessId());

    g = apr_controller_graph(h.ctl);
    memset(&cfg, 0, sizeof cfg);
    cfg.kind = APR_SRC_FAKE;
    e = apr_graph_add_source(g, L"synthetic", &cfg, &sid);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_graph_add_bus(g, L"Mix", &bus);
    ASSERT_FALSE(apr_failed(&e));
    memset(&acfg, 0, sizeof acfg);
    acfg.out_path = path;
    acfg.sample_rate = apr_graph_rate(g);
    acfg.channels = apr_graph_channels(g);
    e = apr_graph_add_action(g, bus, "wav", &acfg);
    ASSERT_FALSE(apr_failed(&e));

    /* The graph was edited behind the controller's back, which is exactly the
     * case apr_controller_model_changed() exists for -- the canvas can do the
     * same thing with a keystroke the frame's accelerator table never sees. */
    apr_controller_model_changed(h.ctl);
    ASSERT_TRUE((GetMenuState(bar, (UINT)APR_CMD_RECORD_START, MF_BYCOMMAND)
                 & MF_GRAYED) == 0);

    ui_stop(&h);
    DeleteFileW(path);
}

/* ==========================================================================
 * The notification area
 * ======================================================================== */

typedef struct TrayHost {
    HWND     wnd;
    AprTray *tray;
} TrayHost;

static LRESULT CALLBACK tray_host_proc(HWND w, UINT m, WPARAM a, LPARAM b)
{
    return DefWindowProcW(w, m, a, b);
}

static int tray_up(TrayHost *t)
{
    WNDCLASSEXW wc;
    static int registered;
    AprErr e;

    memset(t, 0, sizeof *t);
    if (!registered) {
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = tray_host_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"AprTrayTestHost";
        if (!RegisterClassExW(&wc)) return 0;
        registered = 1;
    }
    /* Hidden: the icon is what is under test, not a window. It exists for a
     * fraction of a second and is removed by apr_tray_destroy. */
    t->wnd = CreateWindowExW(0, L"AprTrayTestHost", L"", WS_OVERLAPPED,
                             0, 0, 10, 10, NULL, NULL,
                             GetModuleHandleW(NULL), NULL);
    if (!t->wnd) return 0;

    e = apr_tray_create(t->wnd, &t->tray);
    if (apr_failed(&e)) { DestroyWindow(t->wnd); t->wnd = NULL; return 0; }
    return 1;
}

static void tray_down(TrayHost *t)
{
    apr_tray_destroy(t->tray);
    if (t->wnd) DestroyWindow(t->wnd);
    memset(t, 0, sizeof *t);
}

TEST(the_tray_tooltip_is_a_status_readout_and_changes_with_the_state)
{
    TrayHost t;
    wchar_t tip[256];

    if (!tray_up(&t)) { printf("      SKIPPED: no window station\n"); return; }

    /* Windows+B then the arrow keys reaches this text, and a screen reader
     * reads it. That is why it is a sentence and why it has to be right in
     * every state rather than only when the window is open. */
    ASSERT_GT_INT(0, (int)apr_tray_tip(t.tray, tip, 256));
    printf("      idle:      \"%ls\"\n", tip);
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_TRAY_TIP_IDLE), tip);
    ASSERT_EQ_INT(APR_TRAY_IDLE, (int)apr_tray_state(t.tray));

    apr_tray_set_status(t.tray, APR_TRAY_RECORDING, L"01:12:30");
    ASSERT_GT_INT(0, (int)apr_tray_tip(t.tray, tip, 256));
    printf("      recording: \"%ls\"\n", tip);
    ASSERT_NOT_NULL(wcsstr(tip, L"01:12:30"));
    ASSERT_EQ_INT(APR_TRAY_RECORDING, (int)apr_tray_state(t.tray));

    apr_tray_set_status(t.tray, APR_TRAY_FINISHING, NULL);
    apr_tray_tip(t.tray, tip, 256);
    printf("      closing:   \"%ls\"\n", tip);
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_TRAY_TIP_FINISHING), tip);

    apr_tray_set_status(t.tray, APR_TRAY_IDLE, NULL);
    apr_tray_tip(t.tray, tip, 256);
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_TRAY_TIP_IDLE), tip);

    tray_down(&t);
}

TEST(the_tray_menu_is_named_mnemonic_and_has_no_duplicate_keys)
{
    TrayHost t;
    HMENU m;
    int i, n, used_n = 0;
    wchar_t used[32];

    if (!tray_up(&t)) { printf("      SKIPPED: no window station\n"); return; }

    apr_tray_set_can_record(t.tray, 1, 0);
    m = apr_tray_build_menu(t.tray);
    ASSERT_NOT_NULL(m);

    n = GetMenuItemCount(m);
    ASSERT_GE_INT(5, n);

    for (i = 0; i < n; ++i) {
        wchar_t item[192];
        const wchar_t *amp;
        int len = GetMenuStringW(m, (UINT)i, item, 192, MF_BYPOSITION);

        if (len == 0) continue;   /* separator */

        /* Same two properties the frame's menu bar has to satisfy: a name a
         * screen reader can read, and a mnemonic that is the keyboard path to
         * the item. Shift+F10 on the icon opens this menu, so "mouse only" is
         * not an option here either. */
        ASSERT_TRUE(item[0] != 0);
        amp = wcschr(item, L'&');
        if (!amp || amp[1] == 0) printf("      no mnemonic: \"%ls\"\n", item);
        ASSERT_NOT_NULL(amp);
        ASSERT_TRUE(amp[1] != 0);

        {
            wchar_t c = (wchar_t)towlower(amp[1]);
            int k;
            for (k = 0; k < used_n; ++k) {
                if (used[k] == c) printf("      duplicate '%lc' on \"%ls\"\n", c, item);
                ASSERT_TRUE(used[k] != c);
            }
            if (used_n < 32) used[used_n++] = c;
        }
        printf("      \"%ls\"\n", item);
    }

    /* Start is offered, stop is not -- because the tray was told there is
     * nothing running. Greyed, never absent. */
    ASSERT_TRUE((GetMenuState(m, (UINT)APR_CMD_RECORD_START, MF_BYCOMMAND)
                 & MF_GRAYED) == 0);
    ASSERT_TRUE((GetMenuState(m, (UINT)APR_CMD_RECORD_STOP, MF_BYCOMMAND)
                 & MF_GRAYED) != 0);

    DestroyMenu(m);
    tray_down(&t);
}

TEST(the_tray_menu_offers_stop_once_something_is_running)
{
    TrayHost t;
    HMENU m;

    if (!tray_up(&t)) { printf("      SKIPPED: no window station\n"); return; }

    apr_tray_set_can_record(t.tray, 0, 1);
    m = apr_tray_build_menu(t.tray);
    ASSERT_NOT_NULL(m);
    ASSERT_TRUE((GetMenuState(m, (UINT)APR_CMD_RECORD_STOP, MF_BYCOMMAND)
                 & MF_GRAYED) == 0);
    ASSERT_TRUE((GetMenuState(m, (UINT)APR_CMD_RECORD_START, MF_BYCOMMAND)
                 & MF_GRAYED) != 0);
    DestroyMenu(m);
    tray_down(&t);
}

TEST(every_tray_menu_command_is_one_the_frame_already_answers)
{
    TrayHost t;
    HMENU m;
    int i, n;

    if (!tray_up(&t)) { printf("      SKIPPED: no window station\n"); return; }

    m = apr_tray_build_menu(t.tray);
    ASSERT_NOT_NULL(m);
    n = GetMenuItemCount(m);

    /* The tray raises ordinary APR_CMD_* values as WM_COMMAND, so it adds no
     * second dispatch path and an operation cannot behave differently
     * depending on which surface invoked it. An id outside the known set would
     * be a silent no-op from the notification area. */
    for (i = 0; i < n; ++i) {
        MENUITEMINFOW mii;
        memset(&mii, 0, sizeof mii);
        mii.cbSize = sizeof mii;
        mii.fMask = MIIM_ID | MIIM_FTYPE;
        ASSERT_TRUE(GetMenuItemInfoW(m, (UINT)i, TRUE, &mii) != FALSE);
        if (mii.fType & MFT_SEPARATOR) continue;
        ASSERT_TRUE(mii.wID == APR_CMD_SHOW_WINDOW ||
                    mii.wID == APR_CMD_RECORD_START ||
                    mii.wID == APR_CMD_RECORD_PAUSE ||
                    mii.wID == APR_CMD_RECORD_RESUME ||
                    mii.wID == APR_CMD_RECORD_STOP ||
                    mii.wID == APR_CMD_FILE_OPEN ||
                    mii.wID == APR_CMD_FILE_EXIT);
    }
    DestroyMenu(m);
    tray_down(&t);
}

TEST(a_tray_with_no_icon_still_answers_every_query)
{
    wchar_t tip[64];

    /* Every function tolerates NULL, because the shell can refuse to add an
     * icon and the window still has to work. */
    ASSERT_EQ_INT(0, (int)apr_tray_tip(NULL, tip, 64));
    ASSERT_EQ_INT(APR_TRAY_IDLE, (int)apr_tray_state(NULL));
    {
        HMENU m = apr_tray_build_menu(NULL);
        if (m) DestroyMenu(m);
    }
    apr_tray_set_status(NULL, APR_TRAY_RECORDING, L"x");
    apr_tray_notify(NULL, APR_S_UI_TRAY_INFO_TITLE, L"x");
    apr_tray_set_can_record(NULL, 1, 1);
    ASSERT_EQ_INT(0, apr_tray_on_message(NULL, 0, 0));
    ASSERT_EQ_INT(0, apr_tray_on_taskbar_created(NULL, WM_NULL));
    apr_tray_destroy(NULL);
}


/* ==========================================================================
 * Sessions, through the controller
 *
 * The DIALOG chooses the file and the CONTROLLER does the work, which is why
 * this is testable at all: a modal file picker cannot be answered from the
 * thread that opened it, but the verb behind it can be called directly. That
 * split is not a test hook -- it is the shape a scripting surface wants too.
 * ======================================================================== */

TEST(a_session_saved_from_the_window_reopens_as_the_same_graph)
{
    UiHost h;
    AprGraph *g;
    AprCaptureConfig cfg;
    AprActionConfig acfg;
    AprSourceId sid = 0;
    AprBusId bus = 0;
    wchar_t sess[MAX_PATH], out[MAX_PATH], dir[MAX_PATH];
    AprErr e;
    DWORD n;

    if (!ui_start(&h)) { printf("      SKIPPED: no window\n"); ui_stop(&h); return; }

    n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(sess, MAX_PATH, _TRUNCATE, L"%lsapr_ui_sess_%lu.json",
                 dir, GetCurrentProcessId());
    _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%lsapr_ui_sess_%lu.wav",
                 dir, GetCurrentProcessId());

    /* A synthetic source, because a session that stored a real process id
     * would resolve differently on the way back in and this case is about the
     * round trip, not about the matcher -- test_session.c owns that. */
    g = apr_controller_graph(h.ctl);
    memset(&cfg, 0, sizeof cfg);
    cfg.kind = APR_SRC_FAKE;
    cfg.fake.tone_hz = 440;
    cfg.fake.rate_error_ppm = 30;
    cfg.fake.amplitude = 0.25f;
    e = apr_graph_add_source(g, L"test tone", &cfg, &sid);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_graph_add_bus(g, L"VoiceOnly", &bus);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_graph_connect(g, sid, bus, 0.5f);   /* about -6 dB */
    ASSERT_FALSE(apr_failed(&e));
    memset(&acfg, 0, sizeof acfg);
    acfg.out_path = out;
    acfg.sample_rate = apr_graph_rate(g);
    acfg.channels = apr_graph_channels(g);
    e = apr_graph_add_action(g, bus, "wav", &acfg);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_controller_save_session(h.ctl, sess);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_TRUE(GetFileAttributesW(sess) != INVALID_FILE_ATTRIBUTES);
    ASSERT_WSTR_EQ(sess, apr_controller_session_path(h.ctl));

    /* Throw the model away and read it back. Anything the save did not write
     * down is gone at this point, which is the property under test: a session
     * that remembered only the NAMES would come back as an empty bus. */
    ASSERT_TRUE(apr_controller_command(h.ctl, APR_CMD_FILE_NEW));
    ASSERT_EQ_INT(0, (int)apr_graph_bus_count(apr_controller_graph(h.ctl)));

    e = apr_controller_open_session(h.ctl, sess, 0);
    if (apr_failed(&e)) {
        wchar_t why[512];
        printf("      load failed: %ls\n", apr_err_format(&e, why, 512));
    }
    ASSERT_FALSE(apr_failed(&e));

    g = apr_controller_graph(h.ctl);
    ASSERT_EQ_INT(1, (int)apr_graph_bus_count(g));
    ASSERT_EQ_INT(1, (int)apr_graph_source_count(g));
    ASSERT_EQ_INT(1, (int)apr_graph_edge_count(g));

    {
        AprBus *b = apr_graph_bus_at(g, 0);
        AprSource *src = apr_graph_source_at(g, 0);
        ASSERT_NOT_NULL(b);
        ASSERT_NOT_NULL(src);
        ASSERT_WSTR_EQ(L"VoiceOnly", apr_bus_name(b));
        ASSERT_WSTR_EQ(L"test tone", apr_source_name(src));

        /* The OUTPUT and its path came back too -- the half a session that
         * stored only the graph's shape would silently drop, leaving a
         * reopened session that records nothing anywhere. */
        ASSERT_EQ_INT(1, (int)apr_bus_action_count(b));
        ASSERT_WSTR_EQ(out, apr_bus_action_path(b, 0));

        /* And the per-edge gain, which is per EDGE and not per source
         * (design 3.2). Stored in tenths of a dB, so a round trip through the
         * text costs at most that. */
        printf("      gain back: %.4f (saved 0.5)\n",
               (double)apr_bus_gain(b, apr_source_id(src)));
        ASSERT_TRUE(apr_bus_gain(b, apr_source_id(src)) > 0.49f);
        ASSERT_TRUE(apr_bus_gain(b, apr_source_id(src)) < 0.51f);
    }

    ui_stop(&h);
    DeleteFileW(sess);
    DeleteFileW(out);
}

TEST(a_session_that_is_not_there_fails_with_something_to_say)
{
    UiHost h;
    AprErr e;

    if (!ui_start(&h)) { printf("      SKIPPED: no window\n"); ui_stop(&h); return; }

    e = apr_controller_open_session(h.ctl, L"Z:\\nope\\missing.json", 0);
    ASSERT_TRUE(apr_failed(&e));

    /* And the model is untouched: a load that failed must not leave the
     * window holding half a session. */
    ASSERT_NOT_NULL(apr_controller_graph(h.ctl));
    ASSERT_EQ_INT(0, (int)apr_graph_bus_count(apr_controller_graph(h.ctl)));

    ui_stop(&h);
}

/* Design 4.1.1, and the reason it is worth a case of its own: EXCLUDE mode
 * records EVERYTHING the machine is playing. A file asking for it is not
 * consent, so a non-interactive load -- one with nobody to ask -- must refuse
 * rather than proceed. Failing loudly here is the SAFE direction: dropping an
 * ordinary source records less than was asked for, enabling this one records
 * more. */
TEST(a_session_asking_for_system_wide_capture_is_refused_with_nobody_to_ask)
{
    UiHost h;
    wchar_t sess[MAX_PATH], dir[MAX_PATH];
    AprErr e;
    DWORD n;
    HANDLE f;
    DWORD wrote = 0;
    static const char json[] =
        "{\"apprecorder\":{\"version\":1,\"minReader\":1},"
        "\"session\":{\"sampleRate\":48000,\"channels\":2},"
        "\"sources\":[{\"key\":\"s0\",\"kind\":\"systemMinusTree\","
        "\"name\":\"everything but the shell\",\"pid\":4,"
        "\"exe\":\"explorer.exe\"}],"
        "\"buses\":[{\"name\":\"All\","
        "\"sources\":[{\"key\":\"s0\",\"gainDb\":0.0}],"
        "\"outputs\":[{\"format\":\"wav\",\"path\":\"x.wav\"}]}]}";

    if (!ui_start(&h)) { printf("      SKIPPED: no window\n"); ui_stop(&h); return; }

    n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(sess, MAX_PATH, _TRUNCATE, L"%lsapr_ui_excl_%lu.json",
                 dir, GetCurrentProcessId());

    f = CreateFileW(sess, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ASSERT_TRUE(f != INVALID_HANDLE_VALUE);
    WriteFile(f, json, (DWORD)(sizeof json - 1), &wrote, NULL);
    CloseHandle(f);

    e = apr_controller_open_session(h.ctl, sess, 0);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(0, (int)apr_graph_bus_count(apr_controller_graph(h.ctl)));

    ui_stop(&h);
    DeleteFileW(sess);
}
