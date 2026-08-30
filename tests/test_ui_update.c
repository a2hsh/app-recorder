/*
 * test_ui_update.c -- the updater as the person at the keyboard meets it.
 *
 * ===========================================================================
 * WHAT IS BEING PROVED HERE, AND WHAT IS PROVED ELSEWHERE
 *
 *   tests/test_update.c owns the cryptography, the cadence and the file moves.
 *   This file owns the half that module cannot check about itself: rule 4 of
 *   include/update.h -- ANNOUNCED, AND KEYBOARD-REACHABLE.
 *
 *   The author is blind. An update that arrives as a dialog he cannot hear, or
 *   as a menu item that does nothing audible, or as a prompt that steals focus
 *   from a recording, is worse than no updater at all. So:
 *
 *     - every outcome that concerns a person is SAID, and reaches the
 *       notification area when the window is not in front, exactly as a
 *       source dying does;
 *     - a check nobody asked for stays quiet when it has no news, because
 *       "you are up to date" every five minutes, out loud, is unusable;
 *     - a check the USER asked for always answers, because a command that
 *       appears to do nothing is indistinguishable from a broken one;
 *     - nothing is installed without being asked;
 *     - and NOTHING IS INSTALLED DURING A RECORDING -- refused, out loud,
 *       with the take left running.
 *
 * ===========================================================================
 * SAFETY (AGENTS.md rule 1)
 *
 *   No network: every result is handed to the controller through
 *   apr_controller_test_deliver_update(), and the one case that downloads uses
 *   a fake transport that writes a few bytes to %TEMP%. Nothing here resolves
 *   a hostname.
 *
 *   No audio device: the one case that needs a live recording uses
 *   APR_SRC_FAKE and a WAV output in %TEMP%, deleted on teardown. Not one
 *   sample reaches any output endpoint.
 *
 *   No renaming of this executable: apr_controller_test_set_update_image()
 *   points the swap at a file this suite made. A bug in the staging path
 *   cannot reach the running image.
 */
#include "test_runner.h"
#include "test_wait.h"

#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

#include "bus.h"
#include "capture.h"
#include "graph.h"
#include "strings.h"
#include "ui_app.h"
#include "ui_controller.h"
#include "update.h"
#include "version.h"

#define TXT_CCH 1024

/* ==========================================================================
 * 1. Keyboard-reachable -- no window needed
 * ======================================================================== */

TEST(checking_for_updates_is_a_named_menu_operation_with_a_mnemonic)
{
    size_t i, n = apr_ui_binding_count();
    const AprUiBinding *found = NULL;

    (void)apr_str_init();

    for (i = 0; i < n; i++) {
        const AprUiBinding *b = apr_ui_binding_at(i);
        if (b && b->cmd == APR_CMD_HELP_UPDATE) found = b;
    }
    /* ON THE MENU, NOT ONLY ON A TIMER. Somebody who has just switched the
     * check back on, or who has been told a build exists, needs a way to ask
     * that is not "wait five minutes". */
    ASSERT_NOT_NULL(found);
    ASSERT_GT_INT(0, (int)found->menu_label);
    ASSERT_TRUE(apr_str(found->menu_label)[0] != L'\0');
    /* The ampersand IS the keyboard path to the item (AGENTS.md rule 5). */
    ASSERT_NOT_NULL(wcschr(apr_str(found->menu_label), L'&'));
}

/* ==========================================================================
 * 2. A window, a controller, and results handed to it
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
    _snwprintf_s(h->out_path, MAX_PATH, _TRUNCATE, L"%lsapr_uiupd_%lu.wav",
                 dir, (unsigned long)GetCurrentProcessId());
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
    /* NEVER ASK A HUMAN. Every case sets its own answer; -1 (the real dialog)
     * would hang the suite behind a modal nobody can click. */
    apr_controller_test_set_update_answer(h->ctl, 0);
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

static const wchar_t *said(UiHost *h, wchar_t *buf, size_t cch)
{
    apr_controller_last_announcement(h->ctl, buf, cch);
    return buf;
}

/* Hand the controller a result exactly as the check thread does.
 *
 * apr_controller_test_deliver_update() marshals onto the window's own thread
 * through the same APR_CTL_WM_UPDATE the real check posts, and it SENDS, so
 * the announcement has already happened when this returns. There is no poll
 * anywhere in this file for that reason. */
static void deliver(UiHost *h, const AprUpdateResult *r)
{
    apr_controller_test_deliver_update(h->ctl, r);
}

static void result_of(AprUpdateResult *r, AprUpdateOutcome o, AprUpdateWhy why,
                      const wchar_t *version)
{
    memset(r, 0, sizeof *r);
    r->outcome = o;
    r->why = why;
    r->err = apr_ok();
    if (version) lstrcpynW(r->manifest.version, version, APR_UPDATE_VERSION_CCH);
    lstrcpynW(r->manifest.asset, L"apprecorder.exe", APR_UPDATE_ASSET_CCH);
    lstrcpynW(r->manifest.notes, L"Test build.", APR_UPDATE_NOTES_CCH);
}

static void expect_said(UiHost *h, AprStrId id, const wchar_t *const *args,
                        size_t nargs)
{
    wchar_t want[TXT_CCH], got[TXT_CCH];

    /* Counted like any other assertion, so a case built only from these does
     * not report "0 assertions run" and look like a case that did nothing. */
    g_tr_case_asserts++;
    apr_str_format(id, want, TXT_CCH, args, nargs);
    said(h, got, TXT_CCH);
    if (wcscmp(want, got) != 0) {
        printf("      expected: [%ls]\n", want);
        printf("        actual: [%ls]\n", got);
        tr_fail_head(__FILE__, __LINE__, "expect_said");
    }
}

TEST(an_available_update_is_said_out_loud)
{
    UiHost h;
    AprUpdateResult r;
    const wchar_t *args[2];

    if (!ui_start(&h)) { printf("      SKIPPED: no window station\n"); ui_stop(&h); return; }

    apr_controller_test_set_foreground(h.ctl, 1);   /* the window is in front */
    apr_controller_test_set_update_answer(h.ctl, 0);
    result_of(&r, APR_UPDATE_AVAILABLE, APR_UPDATE_WHY_TIMER, L"9.9.9");
    deliver(&h, &r);

    args[0] = L"9.9.9";
    args[1] = APR_VERSION_STRING;
    expect_said(&h, APR_S_UPDATE_AVAILABLE, args, 2);
    /* IN THE FOREGROUND, NO BALLOON. A screen reader would read it twice. */
    ASSERT_EQ_INT(0, (int)apr_controller_balloon_count(h.ctl));

    ui_stop(&h);
}

TEST(an_available_update_reaches_a_hidden_window_through_the_notification_area)
{
    UiHost h;
    AprUpdateResult r;
    wchar_t balloon[TXT_CCH];
    const wchar_t *args[2];

    if (!ui_start(&h)) { printf("      SKIPPED: no window station\n"); ui_stop(&h); return; }

    /* This application spends its recordings minimised, which is exactly where
     * a status-bar live region reaches nobody. */
    apr_controller_test_set_foreground(h.ctl, 0);
    apr_controller_test_set_update_answer(h.ctl, 0);
    result_of(&r, APR_UPDATE_AVAILABLE, APR_UPDATE_WHY_TIMER, L"9.9.9");
    deliver(&h, &r);

    ASSERT_EQ_INT(1, (int)apr_controller_balloon_count(h.ctl));
    apr_controller_last_balloon(h.ctl, balloon, TXT_CCH);
    args[0] = L"9.9.9";
    args[1] = APR_VERSION_STRING;
    {
        wchar_t want[TXT_CCH];
        apr_str_format(APR_S_UPDATE_TRAY_AVAILABLE, want, TXT_CCH, args, 1);
        ASSERT_WSTR_EQ(want, balloon);
    }

    ui_stop(&h);
}

TEST(a_refused_release_is_announced_loudly_and_is_not_the_network_sentence)
{
    UiHost h;
    AprUpdateResult r;
    wchar_t got[TXT_CCH], failed_sentence[TXT_CCH];

    if (!ui_start(&h)) { printf("      SKIPPED: no window station\n"); ui_stop(&h); return; }

    apr_controller_test_set_foreground(h.ctl, 0);
    result_of(&r, APR_UPDATE_REFUSED, APR_UPDATE_WHY_TIMER, L"9.9.9");
    r.err = APR_ERR_SAY(APR_E_STATE, APR_S_ERR_UPDATE_SIGNATURE,
                        L"test: forged");
    deliver(&h, &r);

    said(&h, got, TXT_CCH);
    /* The security sentence, carrying the security reason. */
    ASSERT_NOT_NULL(wcsstr(got, apr_str(APR_S_ERR_UPDATE_SIGNATURE)));

    /* AND IT IS NOT THE ORDINARY FAILURE SENTENCE. One of these means the wifi
     * is bad and one means somebody is trying something; merging them throws
     * away the only one that matters. */
    apr_str_format(APR_S_UPDATE_FAILED, failed_sentence, TXT_CCH, NULL, 0);
    ASSERT_TRUE(wcscmp(got, failed_sentence) != 0);

    /* And it reached the channel that can carry it. */
    ASSERT_EQ_INT(1, (int)apr_controller_balloon_count(h.ctl));

    ui_stop(&h);
}

TEST(a_check_nobody_asked_for_stays_quiet_when_there_is_no_news)
{
    UiHost h;
    AprUpdateResult r;
    wchar_t before[TXT_CCH], after[TXT_CCH];

    if (!ui_start(&h)) { printf("      SKIPPED: no window station\n"); ui_stop(&h); return; }

    said(&h, before, TXT_CCH);

    /* "You are up to date", said out loud every five minutes, would make the
     * application unusable inside an hour. */
    result_of(&r, APR_UPDATE_UP_TO_DATE, APR_UPDATE_WHY_TIMER, NULL);
    deliver(&h, &r);
    ASSERT_WSTR_EQ(before, said(&h, after, TXT_CCH));

    result_of(&r, APR_UPDATE_NONE, APR_UPDATE_WHY_STARTUP, NULL);
    deliver(&h, &r);
    ASSERT_WSTR_EQ(before, said(&h, after, TXT_CCH));

    result_of(&r, APR_UPDATE_DISABLED, APR_UPDATE_WHY_RECORDING_STOPPED, NULL);
    deliver(&h, &r);
    ASSERT_WSTR_EQ(before, said(&h, after, TXT_CCH));

    ASSERT_EQ_INT(0, (int)apr_controller_balloon_count(h.ctl));

    ui_stop(&h);
}

TEST(a_check_the_user_asked_for_always_answers)
{
    UiHost h;
    AprUpdateResult r;
    const wchar_t *args[1];

    if (!ui_start(&h)) { printf("      SKIPPED: no window station\n"); ui_stop(&h); return; }

    apr_controller_test_set_foreground(h.ctl, 1);

    /* A command that appears to do nothing is indistinguishable from a broken
     * one to somebody working by ear. */
    result_of(&r, APR_UPDATE_UP_TO_DATE, APR_UPDATE_WHY_USER, NULL);
    deliver(&h, &r);
    args[0] = APR_VERSION_STRING;
    expect_said(&h, APR_S_UPDATE_UP_TO_DATE, args, 1);

    result_of(&r, APR_UPDATE_DISABLED, APR_UPDATE_WHY_USER, NULL);
    deliver(&h, &r);
    expect_said(&h, APR_S_UPDATE_OFF, NULL, 0);

    /* Offline, blocked, or no key: say the little that IS known rather than
     * nothing at all. */
    result_of(&r, APR_UPDATE_NONE, APR_UPDATE_WHY_USER, NULL);
    deliver(&h, &r);
    expect_said(&h, APR_S_UPDATE_LINE_CURRENT, args, 1);

    ui_stop(&h);
}

TEST(the_menu_command_says_it_is_looking)
{
    UiHost h;
    wchar_t got[TXT_CCH];
    int ok;

    if (!ui_start(&h)) { printf("      SKIPPED: no window station\n"); ui_stop(&h); return; }

    /* Ctrl+nothing: this is a menu item. Sent exactly as the menu sends it.
     * The check that follows runs on a worker and, with no release key
     * compiled in and no transport reachable, produces nothing -- which is the
     * point: what is asserted is that the COMMAND is audible immediately, not
     * whatever the network eventually says. */
    SendMessageW(h.frame, WM_COMMAND, MAKEWPARAM(APR_CMD_HELP_UPDATE, 0), 0);
    APR_WAIT_UNTIL(ok, wcscmp(apr_str(APR_S_UPDATE_CHECKING),
                              said(&h, got, TXT_CCH)) == 0);
    ASSERT_TRUE(ok);

    ui_stop(&h);
}

TEST(declining_the_prompt_installs_nothing)
{
    UiHost h;
    AprUpdateResult r;

    if (!ui_start(&h)) { printf("      SKIPPED: no window station\n"); ui_stop(&h); return; }

    apr_controller_test_set_foreground(h.ctl, 1);
    apr_controller_test_set_update_answer(h.ctl, 0);      /* "Not now" */
    result_of(&r, APR_UPDATE_AVAILABLE, APR_UPDATE_WHY_USER, L"9.9.9");
    deliver(&h, &r);

    /* ASKED, NEVER ASSUMED. Nothing was downloaded and nothing is waiting to
     * replace the executable. */
    ASSERT_EQ_INT(0, apr_controller_update_staged(h.ctl));

    ui_stop(&h);
}

/* ==========================================================================
 * 3. Rule 1: never during a recording
 * ======================================================================== */

typedef struct FakeHttp {
    int downloads;
} FakeHttp;

static AprErr fake_get(void *user, const wchar_t *url, const wchar_t *etag,
                       void *body, size_t cap, AprUpdateResponse *out)
{
    (void)user; (void)url; (void)etag; (void)body; (void)cap;
    memset(out, 0, sizeof *out);
    out->status = 404;
    return apr_ok();
}

static AprErr fake_download(void *user, const wchar_t *url, const wchar_t *dest)
{
    FakeHttp *f = (FakeHttp *)user;
    HANDLE    h;
    DWORD     wrote = 0;

    (void)url;
    f->downloads++;
    h = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return APR_ERR(APR_E_IO, L"test: could not write the fake payload");
    WriteFile(h, "NEW", 3, &wrote, NULL);
    CloseHandle(h);
    return apr_ok();
}

TEST(an_update_is_refused_out_loud_while_a_recording_is_running)
{
    UiHost h;
    AprUpdateResult r;
    AprUpdateHttp   http;
    FakeHttp        fake;
    HANDLE          idle;
    int ok;

    if (!ui_start(&h)) { printf("      SKIPPED: no window station\n"); ui_stop(&h); return; }

    memset(&fake, 0, sizeof fake);
    http.get = fake_get;
    http.download = fake_download;
    http.user = &fake;
    apr_controller_test_set_update_http(h.ctl, &http);
    apr_controller_test_set_foreground(h.ctl, 1);
    apr_controller_test_set_update_answer(h.ctl, 1);      /* "Download it" */

    idle = apr_controller_test_idle_event(h.ctl);
    SendMessageW(h.frame, WM_COMMAND, MAKEWPARAM(APR_CMD_RECORD_START, 1), 0);
    APR_WAIT_UNTIL(ok, apr_controller_recording(h.ctl) != 0);
    ASSERT_TRUE(ok);

    result_of(&r, APR_UPDATE_AVAILABLE, APR_UPDATE_WHY_USER, L"9.9.9");
    deliver(&h, &r);

    /* RULE 1. Refused, and said -- the take is not stopped, not paused, and
     * not interrupted, and the person is told why nothing happened rather
     * than being left with a prompt they answered that did nothing. */
    expect_said(&h, APR_S_UPDATE_BUSY_RECORDING, NULL, 0);
    ASSERT_EQ_INT(0, fake.downloads);
    ASSERT_EQ_INT(0, apr_controller_update_staged(h.ctl));
    /* And the recording is still running. */
    ASSERT_TRUE(apr_controller_recording(h.ctl) != 0);

    SendMessageW(h.frame, WM_COMMAND, MAKEWPARAM(APR_CMD_RECORD_STOP, 1), 0);
    if (idle) ASSERT_TRUE(APR_WAIT_SIGNAL(idle));

    apr_controller_test_set_update_http(h.ctl, NULL);
    ui_stop(&h);
}

TEST(accepting_stages_a_verified_build_and_says_it_installs_on_the_next_start)
{
    UiHost h;
    AprUpdateResult r;
    AprUpdateHttp   http;
    FakeHttp        fake;
    wchar_t dir[MAX_PATH], image[MAX_PATH * 2], staged[MAX_PATH * 2];
    const wchar_t *args[1];
    /* SHA-256 of the three bytes the fake transport writes. The manifest and
     * the payload agree, which is what makes the staging succeed -- and what
     * makes tests/test_update.c's mismatch case mean something. */
    static const uint8_t sha_new[APR_UPDATE_SHA256_BYTES] = {
        0xa2, 0x53, 0xff, 0x09, 0xc5, 0xa8, 0x67, 0x8e,
        0x1f, 0xd1, 0x96, 0x2b, 0x2c, 0x32, 0x92, 0x45,
        0xe1, 0x39, 0xe4, 0x5f, 0x9c, 0xc6, 0xce, 0xd4,
        0xe5, 0xd7, 0xad, 0x42, 0xc4, 0x10, 0x8f, 0xc0
    };
    DWORD n;

    if (!ui_start(&h)) { printf("      SKIPPED: no window station\n"); ui_stop(&h); return; }

    n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) { ui_stop(&h); FAIL("no temp directory"); }
    _snwprintf_s(image, MAX_PATH * 2, _TRUNCATE, L"%lsapr_uiupd_%lu.exe",
                 dir, (unsigned long)GetCurrentProcessId());
    apr_update_staged_path(image, staged, MAX_PATH * 2);
    DeleteFileW(image);
    DeleteFileW(staged);

    memset(&fake, 0, sizeof fake);
    http.get = fake_get;
    http.download = fake_download;
    http.user = &fake;
    apr_controller_test_set_update_http(h.ctl, &http);
    /* NEVER THE RUNNING EXECUTABLE. */
    apr_controller_test_set_update_image(h.ctl, image);
    apr_controller_test_set_foreground(h.ctl, 1);
    apr_controller_test_set_update_answer(h.ctl, 1);

    result_of(&r, APR_UPDATE_AVAILABLE, APR_UPDATE_WHY_USER, L"9.9.9");
    memcpy(r.manifest.sha256, sha_new, APR_UPDATE_SHA256_BYTES);
    deliver(&h, &r);

    ASSERT_EQ_INT(1, fake.downloads);
    ASSERT_EQ_INT(1, apr_controller_update_staged(h.ctl));
    ASSERT_NE_INT((int)INVALID_FILE_ATTRIBUTES, (int)GetFileAttributesW(staged));
    args[0] = L"9.9.9";
    expect_said(&h, APR_S_UPDATE_READY, args, 1);

    /* AND NOTHING HAS BEEN REPLACED YET. The swap happens on exit; until then
     * the build in place is the one that is running. */
    ASSERT_EQ_INT((int)INVALID_FILE_ATTRIBUTES, (int)GetFileAttributesW(image));

    apr_controller_test_set_update_http(h.ctl, NULL);
    apr_controller_test_set_update_image(h.ctl, NULL);
    ui_stop(&h);

    DeleteFileW(staged);
    DeleteFileW(image);
    {
        wchar_t backup[MAX_PATH * 2];
        apr_update_backup_path(image, backup, MAX_PATH * 2);
        DeleteFileW(backup);
    }
}
