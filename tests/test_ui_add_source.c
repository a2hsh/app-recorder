/*
 * test_ui_add_source.c -- "Add Source" from the thread the user's window is on.
 *
 * WHY THIS IS A UI TEST AND NOT A CAPTURE TEST
 *
 *   tests/test_capture_apartment.c proves the capture layer is callable from an
 *   STA. This proves the specific thing the user did: a real frame, a real
 *   controller, a real graph, and a source added on the window's OWN thread --
 *   which is an STA and must stay one, because IAccPropServices supplies every
 *   control's accessible name and is valid only on the thread that created it
 *   (src/app/main.c). That combination is what failed with "process loopback
 *   requires the MTA; this thread is an STA" while all 26 suites passed.
 *
 *   apr_dlg_add_source() itself is modal and cannot be answered from the thread
 *   that opened it, so this drives the half after the chooser -- exactly what
 *   src/ui/controller.c's do_add_source() does with the dialog's answer, on
 *   exactly the thread it does it on. The chooser is the only part left out.
 *
 * NOTHING IN THIS FILE RENDERS AUDIO, and no device source is ever started.
 * See AGENTS.md rule 1. The process tap targets this test's own PID, whose
 * tree renders nothing.
 */
#include "test_runner.h"
#include "test_window.h"

#include <windows.h>
#include <objbase.h>
#include <string.h>

#include "capture.h"
#include "graph.h"
#include "strings.h"
#include "ui_app.h"
#include "ui_controller.h"

/* ==========================================================================
 * A real frame with a real controller, on a real STA.
 * ======================================================================== */

typedef struct UiHost {
    HANDLE         thread;
    HANDLE         ready;
    AprUiApp      *app;
    AprController *ctl;
    HWND           frame;

    /* what the STA thread did, read only after ready is signalled */
    int      failed;         /* could not even get a frame up */
    AprErr   create_err;
    AprErr   fake_err;
    AprErr   proc_err;
    int      fake_added;
    int      proc_added;
    size_t   source_count;
    int      sta;            /* CoInitializeEx(APARTMENTTHREADED) succeeded */
} UiHost;

static void add_sources(UiHost *h)
{
    AprGraph *g = apr_controller_graph(h->ctl);
    AprCaptureConfig cfg;
    AprSourceId id = 0;

    if (!g) {
        h->fake_err = APR_ERR(APR_E_STATE, L"the controller has no graph");
        h->proc_err = h->fake_err;
        return;
    }

    /* The plumbing, with no hardware in it. */
    memset(&cfg, 0, sizeof cfg);
    cfg.kind = APR_SRC_FAKE;
    cfg.fake.tone_hz   = 440;
    cfg.fake.amplitude = 0.25f;
    h->fake_err = apr_graph_add_source(g, L"Fake", &cfg, &id);
    if (!apr_failed(&h->fake_err)) h->fake_added = 1;

    /* And the real one -- this is the exact call do_add_source() makes for
     * "Add Source > an application". */
    memset(&cfg, 0, sizeof cfg);
    cfg.kind = APR_SRC_PROCESS;
    cfg.process.pid     = (uint32_t)GetCurrentProcessId();
    cfg.process.exclude = 0;
    id = 0;
    h->proc_err = apr_graph_add_source(g, L"This process", &cfg, &id);
    if (!apr_failed(&h->proc_err)) h->proc_added = 1;

    /* The controller's own view of the model, refreshed the way the command
     * handler refreshes it. */
    apr_controller_model_changed(h->ctl);
    h->source_count = apr_graph_source_count(g);
}

static DWORD WINAPI ui_thread(LPVOID param)
{
    UiHost *h = (UiHost *)param;
    HRESULT hr;

    /* Apartment-threaded, exactly as src/app/main.c does it and for exactly
     * the reason given there. This is the apartment the bug lived in. */
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    h->sta = SUCCEEDED(hr);

    (void)apr_str_init();

    h->create_err = apr_ui_app_create(GetModuleHandleW(NULL), &h->app);
    if (apr_failed(&h->create_err)) {
        h->failed = 1;
        SetEvent(h->ready);
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }
    h->frame = apr_ui_app_hwnd(h->app);
    /* The fixture window is not a citizen of the desktop: it must not be
     * able to take the foreground, and no pointer may reach it. See
     * tests/test_window.h -- this runs before anything can focus it. */
    apr_test_isolate_frame(h->frame);

    h->create_err = apr_controller_create(h->app, &h->ctl);
    if (apr_failed(&h->create_err)) {
        h->failed = 1;
        apr_ui_app_destroy(h->app);
        h->app = NULL;
        SetEvent(h->ready);
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }

    add_sources(h);
    SetEvent(h->ready);

    apr_ui_app_run(h->app);

    /* Controller first: it stops the recording and finalizes every action
     * before anything it depends on is torn down (src/app/main.c). */
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
    if (!h->thread) { CloseHandle(h->ready); h->ready = NULL; return 0; }
    if (WaitForSingleObject(h->ready, 30000) != WAIT_OBJECT_0) return 0;
    return 1;
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
    memset(h, 0, sizeof *h);
}

static void show(const char *what, const AprErr *e)
{
    wchar_t buf[512];
    apr_err_format(e, buf, 512);
    printf("      %s: %ls\n", what, buf);
}

/* ==========================================================================
 * Cases
 * ======================================================================== */

TEST(the_window_thread_really_is_an_sta)
{
    /* If this ever stops being true the rest of the file proves nothing --
     * and every accessible name in the application has already been lost. */
    UiHost h;
    if (!ui_start(&h)) { ui_stop(&h); FAIL("could not start the UI thread"); }
    if (h.failed) { show("create", &h.create_err); ui_stop(&h); FAIL("no frame"); }

    ASSERT_EQ_INT(1, h.sta);
    ASSERT_NOT_NULL(h.frame);
    ui_stop(&h);
}

TEST(a_fake_source_can_be_added_from_the_window_thread)
{
    UiHost h;
    if (!ui_start(&h)) { ui_stop(&h); FAIL("could not start the UI thread"); }
    if (h.failed) { show("create", &h.create_err); ui_stop(&h); FAIL("no frame"); }

    if (apr_failed(&h.fake_err)) show("add fake source", &h.fake_err);
    ASSERT_FALSE(apr_failed(&h.fake_err));
    ASSERT_EQ_INT(1, h.fake_added);
    ui_stop(&h);
}

TEST(a_process_source_can_be_added_from_the_window_thread)
{
    /* THE BUG. This used to fail with:
     *   controller.c do_add_source: process loopback requires the MTA;
     *   this thread is an STA: APR_E_STATE at capture_process.c(240)
     * while every automated suite passed, because no test ran from an STA. */
    UiHost h;
    if (!ui_start(&h)) { ui_stop(&h); FAIL("could not start the UI thread"); }
    if (h.failed) { show("create", &h.create_err); ui_stop(&h); FAIL("no frame"); }

    if (apr_failed(&h.proc_err)) {
        if (h.proc_err.kind == APR_E_STATE) {
            /* An apartment refusal is the regression itself and must never be
             * mistaken for "this machine has no audio engine". */
            show("add process source", &h.proc_err);
            ui_stop(&h);
            FAIL("the capture layer refused the window's own apartment");
        }
        show("add process source", &h.proc_err);
        printf("      SKIPPED: no process-loopback activation on this machine\n");
        ui_stop(&h);
        return;
    }

    ASSERT_EQ_INT(1, h.proc_added);
    ASSERT_EQ_INT(2, (int)h.source_count);   /* the fake one and this one */
    ui_stop(&h);
}
