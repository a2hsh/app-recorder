/*
 * main.c -- the windowed front end's entry point, and nothing else.
 *
 * WHY THIS FILE IS FOUR LINES OF WORK AND LIVES ON ITS OWN
 *
 *   apprecorder_ui is a static library with no entry point, deliberately: a
 *   WinMain inside it would collide with every test's main(), and which front
 *   end owns the process is not that layer's decision (ui_app.h). This is that
 *   decision, taken once, in the one translation unit the executable adds.
 *
 * WHY THE APARTMENT IS SET HERE
 *
 *   apr_ui_app_create() requires an apartment-threaded COM apartment on the
 *   thread that will own the window: IAccPropServices -- the thing that gives
 *   every control its accessible name -- is created in that apartment and is
 *   only valid on the thread that created it. Getting this wrong does not
 *   crash; it silently loses every accessible name in the application, which
 *   is the exact class of defect tests/test_ui_a11y.c exists to catch.
 *
 * WHY THE PROCESS DOES NOT JUST EXIT WHEN THE WINDOW CLOSES
 *
 *   It does -- but only after apr_controller_destroy(), which stops any
 *   recording and finalizes every action first. That ordering is the whole
 *   reason the controller is destroyed before the app rather than after.
 */
#include <windows.h>
#include <objbase.h>
#include <string.h>

#include "log.h"
#include "strings.h"
#include "ui_app.h"
#include "ui_controller.h"

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    AprUiApp      *app = NULL;
    AprController *ctl = NULL;
    AprErr         e;
    HRESULT        hr;
    int            rc;

    (void)prev;
    (void)cmd;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    /* A windowed process has nowhere to print. Without this, every APR_WARN in
     * the application goes to OutputDebugString and is invisible unless someone
     * is already attached with a debugger -- which is exactly the position we
     * were in when a dialog silently refused to open and the only evidence
     * available was the user saying "it's all silence".
     *
     * Opt-in by environment variable so the shipping default writes nothing:
     *     set APPRECORDER_LOG=%TEMP%\apprecorder.log
     */
    {
        wchar_t path[1024];
        DWORD n = GetEnvironmentVariableW(L"APPRECORDER_LOG", path,
                                          (DWORD)(sizeof path / sizeof path[0]));
        if (n > 0 && n < sizeof path / sizeof path[0]) {
            AprLogConfig lc;
            memset(&lc, 0, sizeof lc);
            lc.path             = path;
            lc.level            = APR_LOG_DEBUG;
            lc.to_debugger      = 1;
            lc.background_drain = 1;   /* or nothing reaches the file */
            (void)apr_log_init(&lc);
            apr_log_set_level(APR_LOG_DEBUG);
        }
    }

    (void)apr_str_init();

    e = apr_ui_app_create(inst, &app);
    if (apr_failed(&e)) {
        APR_LOG_ERR(APR_LOG_ERROR, &e);
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }

    e = apr_controller_create(app, &ctl);
    if (apr_failed(&e)) {
        /* The window without a controller can navigate but not record, which
         * is worse than not starting: a recorder that silently cannot record
         * is the failure this application is least allowed to have. */
        APR_LOG_ERR(APR_LOG_ERROR, &e);
        apr_ui_app_destroy(app);
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }

    apr_ui_app_show(app, show);
    rc = apr_ui_app_run(app);

    /* Controller first: it stops the recording and finalizes every action
     * before anything it depends on is torn down. */
    apr_controller_destroy(ctl);
    apr_ui_app_destroy(app);

    if (SUCCEEDED(hr)) CoUninitialize();
    return rc;
}
