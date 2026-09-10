/*
 * test_ui_pause.c -- pausing and resuming, as the person at the keyboard meets
 * it: a key, a sentence, a menu item that is greyed the right way round, and a
 * tooltip that answers the question it exists for.
 *
 * ===========================================================================
 * WHAT IS BEING PROVED
 *
 *   THE KEY IS THE KEY THE PRODUCT DOCUMENTS. There is one binding table
 *   (ui_app.h) and it is what builds the accelerator, what answers
 *   apr_ui_app_accel_command(), what labels the menu, and what Help >
 *   Keyboard Shortcuts renders. Everything in section 1 holds it to that,
 *   including the one thing a table cannot enforce on its own: that the key
 *   SPELT OUT in each menu label is the key that row really binds. That check
 *   is the reason the table exists -- "&Start Recording\tCtrl+R" was a piece
 *   of prose that nothing compared against anything.
 *
 *   BOTH TRANSITIONS ARE SAID OUT LOUD, on whichever channel can reach the
 *   user: the status bar's live region in the foreground, a notification-area
 *   balloon when the window is not in front. A pause the author cannot hear is
 *   an hour of a meeting that was never recorded and looks exactly like a
 *   recording that is going fine, so a silent transition is a failure here.
 *
 *   THE TOOLTIP SAYS PAUSED. Windows+B and the arrow keys is how a session is
 *   checked on from inside another application; an icon that goes on reading
 *   "recording" through a pause answers that question wrongly, which ui_tray.h
 *   says is worse than answering nothing.
 *
 *   A PAUSED RECORDING IS STILL A RECORDING. Editing stays refused, out loud,
 *   with the same sentence as during a running one.
 *
 * ===========================================================================
 * SAFETY (AGENTS.md rule 1)
 *
 *   APR_SRC_FAKE throughout. Nothing here opens an audio endpoint and not one
 *   sample is rendered to any output device. The single WAV output exists so
 *   that a recording can start at all; it is finalized on teardown and deleted.
 *   Tray registration is suppressed by APPRECORDER_NO_TRAY (CMake), so no icon
 *   and no balloon reaches the shell -- which is exactly why the controller
 *   carries the balloon and tray-state seams this file reads.
 */
#include "test_runner.h"
#include "test_window.h"
#include "test_wait.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

#include "action.h"
#include "bus.h"
#include "capture.h"
#include "graph.h"
#include "strings.h"
#include "ui_app.h"
#include "ui_canvas.h"
#include "ui_controller.h"
#include "ui_dialogs.h"
#include "ui_tray.h"

#define TXT_CCH 1024

/* ==========================================================================
 * 1. The binding table -- no window needed
 * ======================================================================== */

TEST(every_frame_binding_names_an_operation_and_a_menu_item)
{
    size_t i, n = apr_ui_binding_count();

    (void)apr_str_init();
    ASSERT_GT_INT(0, (int)n);

    for (i = 0; i < n; i++) {
        const AprUiBinding *b = apr_ui_binding_at(i);
        ASSERT_NOT_NULL(b);
        ASSERT_GT_INT(0, b->cmd);
        /* Every row is on a menu: an operation reachable only by a shortcut is
         * an operation a screen reader user cannot DISCOVER (ui_app.h). */
        ASSERT_GT_INT(0, (int)b->menu_label);
        ASSERT_TRUE(apr_str(b->menu_label)[0] != L'\0');
        /* Every row that HAS a key names itself for the shortcut list. */
        if (b->vk != 0) {
            ASSERT_GT_INT(0, (int)b->key_label);
            ASSERT_TRUE(apr_str(b->key_label)[0] != L'\0');
        }
    }
    ASSERT_NULL(apr_ui_binding_at(n));
}

TEST(no_two_frame_bindings_claim_the_same_keystroke)
{
    size_t i, j, n = apr_ui_binding_count();

    for (i = 0; i < n; i++) {
        const AprUiBinding *a = apr_ui_binding_at(i);
        if (!a || a->vk == 0) continue;
        for (j = i + 1; j < n; j++) {
            const AprUiBinding *b = apr_ui_binding_at(j);
            if (!b || b->vk == 0) continue;
            if (a->vk == b->vk && a->mods == b->mods) {
                printf("      %d and %d both claim vk 0x%02X mods %u\n",
                       a->cmd, b->cmd, a->vk, a->mods);
                FAIL("two commands are bound to one keystroke");
            }
        }
    }
}

TEST(the_accelerator_answers_with_the_command_the_table_binds)
{
    /* The ACCEL array is built from this table and apr_ui_app_accel_command()
     * reads it, so the two cannot disagree. They used to be two copies. */
    size_t i, n = apr_ui_binding_count();

    for (i = 0; i < n; i++) {
        const AprUiBinding *b = apr_ui_binding_at(i);
        if (!b || b->vk == 0) continue;
        ASSERT_EQ_INT(b->cmd, apr_ui_app_accel_command(b->vk, b->mods));
    }
}

TEST(ctrl_p_pauses_and_ctrl_shift_p_undoes_it)
{
    /* The keys, named. Ctrl+Shift+P mirrors Ctrl+P the same way Ctrl+Shift+3
     * mirrors Ctrl+3, and the pair sits beside Ctrl+R and Ctrl+. */
    ASSERT_EQ_INT(APR_CMD_RECORD_PAUSE,
                  apr_ui_app_accel_command('P', APR_KMOD_CTRL));
    ASSERT_EQ_INT(APR_CMD_RECORD_RESUME,
                  apr_ui_app_accel_command('P', APR_KMOD_CTRL | APR_KMOD_SHIFT));
    ASSERT_EQ_INT(APR_CMD_RECORD_START,
                  apr_ui_app_accel_command('R', APR_KMOD_CTRL));
    ASSERT_EQ_INT(APR_CMD_RECORD_STOP,
                  apr_ui_app_accel_command(VK_OEM_PERIOD, APR_KMOD_CTRL));
}

TEST(every_menu_label_spells_out_the_key_its_binding_really_binds)
{
    /* THE CHECK THE TABLE CANNOT MAKE ON ITS OWN. A menu label carries its own
     * accelerator text after a tab -- "&Pause Recording\tCtrl+P" -- and that
     * text is prose in a catalog, which nothing compared against the binding.
     * A translator, or a later edit to either half, could make the menu say one
     * key while the frame listened for another, and the only symptom would be a
     * key that silently does nothing. */
    size_t i, n = apr_ui_binding_count();
    int    checked = 0;

    (void)apr_str_init();

    for (i = 0; i < n; i++) {
        const AprUiBinding *b = apr_ui_binding_at(i);
        const wchar_t *label;
        const wchar_t *tab;
        wchar_t key[128];

        if (!b || b->vk == 0 || !b->menu_label) continue;
        label = apr_str(b->menu_label);
        tab   = wcschr(label, L'\t');
        if (!tab) {
            printf("      menu label for command %d has no accelerator text: "
                   "%ls\n", b->cmd, label);
            FAIL("a bound command's menu item does not name its key");
        }
        apr_dlg_key_name(b->vk, b->mods, key, 128);
        if (wcscmp(tab + 1, key) != 0) {
            printf("      command %d: menu says \"%ls\", binding is \"%ls\"\n",
                   b->cmd, tab + 1, key);
            FAIL("the menu names a different key from the one bound");
        }
        checked++;
    }
    printf("      %d bound commands, every menu label agreeing\n", checked);
    ASSERT_GT_INT(10, checked);
}

TEST(help_lists_the_recording_keys_including_the_new_pair)
{
    /* Help renders this same table (src/ui/dialogs.c, fill_keys). Before it
     * did, Ctrl+R and Ctrl+. were bound, named in the menu, and absent from the
     * one screen a keyboard user opens to find out what the keys are -- which
     * for somebody working by ear is an operation that does not exist. */
    size_t i, n = apr_ui_binding_count();
    int found_start = 0, found_pause = 0, found_resume = 0, found_stop = 0;

    (void)apr_str_init();

    for (i = 0; i < n; i++) {
        const AprUiBinding *b = apr_ui_binding_at(i);
        wchar_t row[TXT_CCH];
        wchar_t key[128];

        if (!b || b->vk == 0 || !b->key_label) continue;
        ASSERT_GT_INT(0, (int)apr_dlg_key_row(b->key_label, b->vk, b->mods,
                                              row, TXT_CCH));
        /* The row names BOTH halves: what it does, and what to press. */
        ASSERT_TRUE(wcsstr(row, apr_str(b->key_label)) != NULL);
        apr_dlg_key_name(b->vk, b->mods, key, 128);
        ASSERT_TRUE(wcsstr(row, key) != NULL);

        if (b->cmd == APR_CMD_RECORD_START)  found_start = 1;
        if (b->cmd == APR_CMD_RECORD_PAUSE)  found_pause = 1;
        if (b->cmd == APR_CMD_RECORD_RESUME) found_resume = 1;
        if (b->cmd == APR_CMD_RECORD_STOP)   found_stop = 1;
    }
    ASSERT_TRUE(found_start);
    ASSERT_TRUE(found_pause);
    ASSERT_TRUE(found_resume);
    ASSERT_TRUE(found_stop);
}

/* ==========================================================================
 * 2. The tooltip, as a unit
 *
 * The tray object is built here rather than reached through the controller,
 * because "PAUSED formats a tooltip at all" is a property of the tray and is
 * worth pinning on its own. That the controller REACHES it is the last case in
 * section 3.
 * ======================================================================== */

typedef struct TrayHost {
    HWND     owner;
    AprTray *tray;
} TrayHost;

static int tray_up(TrayHost *t)
{
    memset(t, 0, sizeof *t);
    t->owner = CreateWindowExW(0, L"STATIC", L"apr tray host", 0,
                               0, 0, 10, 10, HWND_MESSAGE, NULL,
                               GetModuleHandleW(NULL), NULL);
    if (!t->owner) return 0;
    {
        AprErr e = apr_tray_create(t->owner, &t->tray);
        if (apr_failed(&e)) { DestroyWindow(t->owner); t->owner = NULL; return 0; }
    }
    return 1;
}

static void tray_down(TrayHost *t)
{
    apr_tray_destroy(t->tray);
    if (t->owner) DestroyWindow(t->owner);
    memset(t, 0, sizeof *t);
}

TEST(the_tray_tooltip_can_say_paused_and_carries_the_recorded_length)
{
    TrayHost t;
    wchar_t  tip[256];
    wchar_t  want[256];
    const wchar_t *args[1];

    (void)apr_str_init();
    if (!tray_up(&t)) { SKIP("no window station"); return; }

    apr_tray_set_status(t.tray, APR_TRAY_RECORDING, L"00:00:12");
    ASSERT_EQ_INT(APR_TRAY_RECORDING, (int)apr_tray_state(t.tray));

    apr_tray_set_status(t.tray, APR_TRAY_PAUSED, L"00:00:12");
    ASSERT_EQ_INT(APR_TRAY_PAUSED, (int)apr_tray_state(t.tray));

    apr_tray_tip(t.tray, tip, 256);
    args[0] = L"00:00:12";
    apr_str_format(APR_S_UI_TRAY_TIP_PAUSED, want, 256, args, 1);
    ASSERT_WSTR_EQ(want, tip);

    /* And it is not the recording tooltip, which is the whole point: the two
     * states must be distinguishable by the sentence, not by an icon. */
    apr_str_format(APR_S_UI_TRAY_TIP_RECORDING, want, 256, args, 1);
    ASSERT_TRUE(wcscmp(want, tip) != 0);

    tray_down(&t);
}

TEST(the_tray_menu_offers_pause_and_resume)
{
    TrayHost t;
    HMENU    m;
    int      i, n, saw_pause = 0, saw_resume = 0;

    (void)apr_str_init();
    if (!tray_up(&t)) { SKIP("no window station"); return; }

    /* Recording and not paused: Pause is live, Resume is greyed. Greyed and
     * PRESENT -- an item that vanishes says nothing to a screen reader. */
    apr_tray_set_can_pause(t.tray, 1, 0);
    m = apr_tray_build_menu(t.tray);
    ASSERT_NOT_NULL(m);
    n = GetMenuItemCount(m);
    for (i = 0; i < n; ++i) {
        MENUITEMINFOW mii;
        memset(&mii, 0, sizeof mii);
        mii.cbSize = sizeof mii;
        mii.fMask  = MIIM_ID | MIIM_FTYPE | MIIM_STATE;
        if (!GetMenuItemInfoW(m, (UINT)i, TRUE, &mii)) continue;
        if (mii.fType & MFT_SEPARATOR) continue;
        if (mii.wID == APR_CMD_RECORD_PAUSE) {
            saw_pause = 1;
            ASSERT_EQ_INT(0, (int)(mii.fState & (MFS_GRAYED | MFS_DISABLED)));
        }
        if (mii.wID == APR_CMD_RECORD_RESUME) {
            saw_resume = 1;
            ASSERT_TRUE((mii.fState & (MFS_GRAYED | MFS_DISABLED)) != 0);
        }
    }
    DestroyMenu(m);
    ASSERT_TRUE(saw_pause);
    ASSERT_TRUE(saw_resume);
    tray_down(&t);
}

/* ==========================================================================
 * 3. The application, driven the way a person drives it
 *
 * A real frame and a real controller on a real STA, exactly as
 * tests/test_ui_behaviour.c does it: every command arrives as the WM_COMMAND
 * TranslateAccelerator would send, and every assertion is made from this
 * thread against what the controller SAID.
 * ======================================================================== */

typedef struct UiHost {
    HANDLE         thread;
    HANDLE         ready;
    AprUiApp      *app;
    AprController *ctl;
    AprGraph      *graph;
    HWND           frame;
    int            failed;
    AprSourceId    src;
    AprBusId       bus;
    wchar_t        out_path[MAX_PATH];
} UiHost;

static void build_graph(UiHost *h)
{
    AprCaptureConfig cfg;
    AprActionConfig  ac;
    wchar_t dir[MAX_PATH];
    DWORD   n;
    AprErr  e;

    h->graph = apr_controller_graph(h->ctl);
    if (!h->graph) { h->failed = 1; return; }

    memset(&cfg, 0, sizeof cfg);
    cfg.kind = APR_SRC_FAKE;
    cfg.fake.tone_hz   = 440;
    cfg.fake.amplitude = 0.25f;
    e = apr_controller_add_source(h->ctl, L"Teams", &cfg);
    if (apr_failed(&e)) { h->failed = 1; return; }
    h->src = apr_source_id(apr_graph_source_at(h->graph, 0));

    e = apr_controller_add_bus(h->ctl, L"Main Mix");
    if (apr_failed(&e)) { h->failed = 1; return; }
    h->bus = apr_bus_id(apr_graph_bus_at(h->graph, 0));

    e = apr_graph_connect(h->graph, h->src, h->bus, 1.0f);
    if (apr_failed(&e)) { h->failed = 1; return; }

    n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) { h->failed = 1; return; }
    /* Per fixture, not per process: the pid alone means every case in this
     * suite records to one path and relies on DeleteFileW to clear it, which
     * loses to the previous case's encoder while its handle is still open.
     * See tests/test_ui_behaviour.c, where that race reached CI. */
    {
        static LONG seq;
        _snwprintf_s(h->out_path, MAX_PATH, _TRUNCATE,
                     L"%lsapr_uipause_%lu_%ld.wav", dir,
                     (unsigned long)GetCurrentProcessId(),
                     InterlockedIncrement(&seq));
    }
    DeleteFileW(h->out_path);

    memset(&ac, 0, sizeof ac);
    ac.out_path = h->out_path;
    e = apr_graph_add_action(h->graph, h->bus, "wav", &ac);
    if (apr_failed(&e)) { h->failed = 1; return; }
    apr_controller_model_changed(h->ctl);
}

static DWORD WINAPI ui_thread(LPVOID param)
{
    UiHost *h = (UiHost *)param;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    AprErr  e;

    (void)apr_str_init();

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
    /* The fixture window is not a citizen of the desktop: it must not be
     * able to take the foreground, and no pointer may reach it. See
     * tests/test_window.h -- this runs before anything can focus it. */
    apr_test_isolate_frame(h->frame);
    build_graph(h);
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
    if (WaitForSingleObject(h->ready, APR_TEST_WAIT_MS) != WAIT_OBJECT_0) return 0;
    return !h->failed && h->frame != NULL;
}

static void ui_stop(UiHost *h)
{
    if (h->frame && IsWindow(h->frame)) PostMessageW(h->frame, WM_CLOSE, 0, 0);
    if (h->thread) {
        if (WaitForSingleObject(h->thread, APR_TEST_WAIT_MS) != WAIT_OBJECT_0) {
            printf("      WARNING: UI thread did not exit; terminating\n");
            TerminateThread(h->thread, 1);
        }
        CloseHandle(h->thread);
    }
    if (h->ready) CloseHandle(h->ready);
    if (h->out_path[0]) DeleteFileW(h->out_path);
    memset(h, 0, sizeof *h);
}

/* Exactly what TranslateAccelerator sends: the command in the low word and 1
 * in the high word. Sent, so the work is done before the next line runs. */
static void accel(UiHost *h, int cmd)
{
    SendMessageW(h->frame, WM_COMMAND, MAKEWPARAM(cmd, 1), 0);
}

static const wchar_t *said(UiHost *h, wchar_t *buf, size_t cch)
{
    apr_controller_last_announcement(h->ctl, buf, cch);
    return buf;
}

/* THE SENTENCE ARRIVES A HAIR AFTER THE STATE, and that is the product's own
 * ordering rather than a defect (see the note in controller.c's PAUSED case):
 * the flag is what anything watching learns the state from, and the sentence is
 * stored on the very next line. Waiting for it is what a screen reader does
 * too, so the test waits rather than assuming it can outrun a store. */
static int wait_said(UiHost *h, AprStrId id)
{
    wchar_t buf[TXT_CCH];
    int ok;

    APR_WAIT_UNTIL(ok, wcscmp(apr_str(id), said(h, buf, TXT_CCH)) == 0);
    return ok;
}

/* A SYNCHRONOUS ROUND TRIP TO THE WINDOW'S OWN THREAD. The state flag flips
 * inside the posted notice handler, which then goes on to re-grey the menu and
 * rebuild two views -- so an assertion made the instant the flag changes is
 * racing the rest of that handler. WM_NULL cannot be dispatched until the
 * message being handled has returned, so waiting for it is waiting for the
 * whole transition, with no sleep to tune and nothing to be flaky about. */
static void sync_ui(UiHost *h)
{
    if (h->frame && IsWindow(h->frame)) SendMessageW(h->frame, WM_NULL, 0, 0);
}

static int wait_paused(UiHost *h, int want)
{
    int ok;

    APR_WAIT_UNTIL(ok, !!apr_controller_paused(h->ctl) == !!want);
    sync_ui(h);
    return ok;
}

/* Start a recording and wait until the controller says one is running. */
static int start_recording(UiHost *h)
{
    int ok;

    accel(h, APR_CMD_RECORD_START);
    APR_WAIT_UNTIL(ok, apr_controller_recording(h->ctl));
    sync_ui(h);
    return ok;
}

/* Waits for the FILES, on the handle the controller sets when they are closed
 * (ui_controller.h) -- not on a poll of the flag against a ceiling. */
static void stop_recording(UiHost *h)
{
    HANDLE idle = apr_controller_test_idle_event(h->ctl);

    accel(h, APR_CMD_RECORD_STOP);
    if (idle) {
        (void)APR_WAIT_SIGNAL(idle);
    } else {
        int ok;
        APR_WAIT_UNTIL(ok, !apr_controller_recording(h->ctl));
        (void)ok;
    }
    sync_ui(h);
}

TEST(pausing_and_resuming_are_both_announced_and_both_change_the_state)
{
    UiHost  h;
    wchar_t buf[TXT_CCH];

    if (!ui_start(&h)) { SKIP("no window station"); ui_stop(&h); return; }

    ASSERT_TRUE(start_recording(&h));
    ASSERT_FALSE(apr_controller_paused(h.ctl));

    accel(&h, APR_CMD_RECORD_PAUSE);
    ASSERT_TRUE(wait_paused(&h, 1));

    /* STILL RECORDING. A pause is a quiet part of a take, not the end of one:
     * the files are open and the graph is still frozen. */
    ASSERT_TRUE(apr_controller_recording(h.ctl));
    ASSERT_TRUE(wait_said(&h, APR_S_UI_ANN_RECORD_PAUSED));
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_ANN_RECORD_PAUSED), said(&h, buf, TXT_CCH));

    accel(&h, APR_CMD_RECORD_RESUME);
    ASSERT_TRUE(wait_paused(&h, 0));
    ASSERT_TRUE(apr_controller_recording(h.ctl));
    ASSERT_TRUE(wait_said(&h, APR_S_UI_ANN_RECORD_RESUMED));
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_ANN_RECORD_RESUMED), said(&h, buf, TXT_CCH));

    stop_recording(&h);
    ASSERT_FALSE(apr_controller_paused(h.ctl));
    ui_stop(&h);
}

TEST(the_tray_tooltip_follows_the_recording_into_the_pause_and_out_again)
{
    /* The end-to-end half of the tooltip case above: the controller really
     * does tell the tray, so Windows+B answers "paused" while it is paused. */
    UiHost h;

    if (!ui_start(&h)) { SKIP("no window station"); ui_stop(&h); return; }

    ASSERT_EQ_INT(APR_TRAY_IDLE, (int)apr_controller_tray_state(h.ctl));
    ASSERT_TRUE(start_recording(&h));
    ASSERT_EQ_INT(APR_TRAY_RECORDING, (int)apr_controller_tray_state(h.ctl));

    accel(&h, APR_CMD_RECORD_PAUSE);
    ASSERT_TRUE(wait_paused(&h, 1));
    ASSERT_EQ_INT(APR_TRAY_PAUSED, (int)apr_controller_tray_state(h.ctl));

    accel(&h, APR_CMD_RECORD_RESUME);
    ASSERT_TRUE(wait_paused(&h, 0));
    ASSERT_EQ_INT(APR_TRAY_RECORDING, (int)apr_controller_tray_state(h.ctl));

    stop_recording(&h);
    ASSERT_EQ_INT(APR_TRAY_IDLE, (int)apr_controller_tray_state(h.ctl));
    ui_stop(&h);
}

TEST(a_pause_while_the_window_is_not_in_front_goes_out_as_a_balloon)
{
    /* The channel test. A recording started from the notification area runs
     * with the window hidden BY DEFINITION, which is where a status-bar live
     * region reaches nobody -- so a pause nobody hears is the exact failure
     * this feature must not have. */
    UiHost   h;
    unsigned before;
    wchar_t  buf[TXT_CCH];

    if (!ui_start(&h)) { SKIP("no window station"); ui_stop(&h); return; }

    ASSERT_TRUE(start_recording(&h));
    apr_controller_test_set_foreground(h.ctl, 0);   /* not in front */
    before = apr_controller_balloon_count(h.ctl);

    accel(&h, APR_CMD_RECORD_PAUSE);
    ASSERT_TRUE(wait_paused(&h, 1));
    ASSERT_TRUE(wait_said(&h, APR_S_UI_ANN_RECORD_PAUSED));
    ASSERT_GT_INT((int)before, (int)apr_controller_balloon_count(h.ctl));
    apr_controller_last_balloon(h.ctl, buf, TXT_CCH);
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_TRAY_INFO_PAUSED), buf);

    before = apr_controller_balloon_count(h.ctl);
    accel(&h, APR_CMD_RECORD_RESUME);
    ASSERT_TRUE(wait_paused(&h, 0));
    ASSERT_TRUE(wait_said(&h, APR_S_UI_ANN_RECORD_RESUMED));
    ASSERT_GT_INT((int)before, (int)apr_controller_balloon_count(h.ctl));
    apr_controller_last_balloon(h.ctl, buf, TXT_CCH);
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_TRAY_INFO_RESUMED), buf);

    apr_controller_test_set_foreground(h.ctl, -1);
    stop_recording(&h);
    ui_stop(&h);
}

TEST(a_paused_recording_still_refuses_to_be_edited_and_says_why)
{
    /* apr_graph_running() stays nonzero through a pause (graph.h): the
     * encoders are open, the captures are live and the reconnect worker is
     * running. Refused, and refused OUT LOUD -- a key that does nothing and
     * says nothing is indistinguishable from a broken application. */
    UiHost  h;
    wchar_t buf[TXT_CCH];
    size_t  buses;

    if (!ui_start(&h)) { SKIP("no window station"); ui_stop(&h); return; }

    ASSERT_TRUE(start_recording(&h));
    accel(&h, APR_CMD_RECORD_PAUSE);
    ASSERT_TRUE(wait_paused(&h, 1));

    buses = apr_graph_bus_count(h.graph);
    accel(&h, APR_CMD_ADD_BUS);
    ASSERT_EQ_U64((uint64_t)buses, (uint64_t)apr_graph_bus_count(h.graph));
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_ANN_BUSY_RECORDING), said(&h, buf, TXT_CCH));

    accel(&h, APR_CMD_RECORD_RESUME);
    ASSERT_TRUE(wait_paused(&h, 0));
    stop_recording(&h);
    ui_stop(&h);
}

TEST(the_pause_pair_is_greyed_the_right_way_round_at_every_moment)
{
    /* "Pause Recording, unavailable" is how a screen reader user learns the
     * take is already paused WITHOUT pressing anything and listening to what
     * happened. Grey is the answer to the question, not a decoration. */
    UiHost h;

    if (!ui_start(&h)) { SKIP("no window station"); ui_stop(&h); return; }

    /* Idle: neither. */
    ASSERT_FALSE(apr_ui_app_command_enabled(h.app, APR_CMD_RECORD_PAUSE));
    ASSERT_FALSE(apr_ui_app_command_enabled(h.app, APR_CMD_RECORD_RESUME));

    ASSERT_TRUE(start_recording(&h));
    ASSERT_TRUE(apr_ui_app_command_enabled(h.app, APR_CMD_RECORD_PAUSE));
    ASSERT_FALSE(apr_ui_app_command_enabled(h.app, APR_CMD_RECORD_RESUME));

    accel(&h, APR_CMD_RECORD_PAUSE);
    ASSERT_TRUE(wait_paused(&h, 1));
    ASSERT_FALSE(apr_ui_app_command_enabled(h.app, APR_CMD_RECORD_PAUSE));
    ASSERT_TRUE(apr_ui_app_command_enabled(h.app, APR_CMD_RECORD_RESUME));

    accel(&h, APR_CMD_RECORD_RESUME);
    ASSERT_TRUE(wait_paused(&h, 0));
    ASSERT_TRUE(apr_ui_app_command_enabled(h.app, APR_CMD_RECORD_PAUSE));
    ASSERT_FALSE(apr_ui_app_command_enabled(h.app, APR_CMD_RECORD_RESUME));

    stop_recording(&h);
    ASSERT_FALSE(apr_ui_app_command_enabled(h.app, APR_CMD_RECORD_PAUSE));
    ASSERT_FALSE(apr_ui_app_command_enabled(h.app, APR_CMD_RECORD_RESUME));
    ui_stop(&h);
}

TEST(the_keys_are_answered_out_loud_even_when_there_is_nothing_to_pause)
{
    /* A DISABLED command's key still reaches the handler (ui_app.h), so these
     * are the sentences a user gets for pressing Ctrl+P at the wrong moment.
     * Silence would be indistinguishable from a broken key. */
    UiHost  h;
    wchar_t buf[TXT_CCH];

    if (!ui_start(&h)) { SKIP("no window station"); ui_stop(&h); return; }

    accel(&h, APR_CMD_RECORD_PAUSE);
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_ANN_NOT_RECORDING), said(&h, buf, TXT_CCH));
    accel(&h, APR_CMD_RECORD_RESUME);
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_ANN_NOT_RECORDING), said(&h, buf, TXT_CCH));

    ASSERT_TRUE(start_recording(&h));
    accel(&h, APR_CMD_RECORD_RESUME);       /* running, not paused */
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_ANN_NOT_PAUSED), said(&h, buf, TXT_CCH));

    accel(&h, APR_CMD_RECORD_PAUSE);
    ASSERT_TRUE(wait_paused(&h, 1));
    accel(&h, APR_CMD_RECORD_PAUSE);        /* already paused */
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_ANN_ALREADY_PAUSED), said(&h, buf, TXT_CCH));
    /* And it is STILL one pause: a key pressed twice is one state. */
    ASSERT_TRUE(apr_controller_paused(h.ctl));

    accel(&h, APR_CMD_RECORD_RESUME);
    ASSERT_TRUE(wait_paused(&h, 0));
    stop_recording(&h);
    ui_stop(&h);
}

TEST(the_elapsed_clock_is_recorded_time_and_stops_while_paused)
{
    /* The number in the status bar and in the tooltip is the length of the
     * FILE. A clock that counted the pause would be describing a recording
     * that does not exist. */
    UiHost  h;
    int64_t at_pause, after;

    if (!ui_start(&h)) { SKIP("no window station"); ui_stop(&h); return; }

    ASSERT_TRUE(start_recording(&h));
    Sleep(250);
    accel(&h, APR_CMD_RECORD_PAUSE);
    ASSERT_TRUE(wait_paused(&h, 1));

    Sleep(50);
    at_pause = apr_controller_elapsed_ms(h.ctl);
    Sleep(500);
    after = apr_controller_elapsed_ms(h.ctl);

    printf("      %lld ms recorded before the pause, %lld ms after half a "
           "second of it\n", (long long)at_pause, (long long)after);
    ASSERT_GT_INT(0, (int)at_pause);
    ASSERT_TRUE(after - at_pause <= 40);

    accel(&h, APR_CMD_RECORD_RESUME);
    ASSERT_TRUE(wait_paused(&h, 0));
    stop_recording(&h);
    ui_stop(&h);
}
