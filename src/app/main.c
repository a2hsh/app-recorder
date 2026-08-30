/*
 * main.c -- apprecorder's ONE entry point.
 *
 * There were two executables until this file existed: apprecorder.exe (839 KB)
 * and apprecorder_ui_app.exe (915 KB), 1.75 MB of mostly identical bytes,
 * because each statically linked the whole core -- capture, the graph, the
 * mixer, the resampler, the drift controller, three encoders, LAME, libogg and
 * libopus. There is now one file, one name on PATH, and one process that is a
 * terminal program when it is given a command and a window when it is not.
 *
 * frontend.h holds the reasoning: the dispatch rule, the apartment rule, and
 * the cost of a WINDOWS-subsystem image (cmd.exe does not wait for one; that
 * is what apprecorder-wait.cmd is for). This file is the mechanism.
 *
 * ---------------------------------------------------------------------------
 * THE ORDER HERE IS LOAD-BEARING
 *
 *   1. Read argv and choose a front end. Nothing else can be decided first:
 *      the apartment depends on the answer, and so does where a message goes.
 *   2. If it is a command line, attach to the console that launched us and
 *      point the standard handles at it -- WITHOUT clobbering a redirection
 *      the parent already set up, which is the whole of `apprecorder record
 *      ... > out.txt`.
 *   3. Load the string catalog, so every sentence below comes from it.
 *   4. Check the Windows floor. It runs on BOTH paths and must be able to
 *      report before any window exists, which is why report() below is the
 *      only thing it needs.
 *   5. Enter the apartment the chosen front end asked for -- and only then.
 *   6. Run.
 */
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "cli/cli.h"
#include "frontend.h"
#include "log.h"
#include "strings.h"
#include "ui_app.h"
#include "ui_controller.h"
#include "winver.h"

/* ---------------------------------------------------------------------------
 * The console a WINDOWS-subsystem process does not get for free
 * ------------------------------------------------------------------------- */

static int g_have_output;   /* is there anywhere for a line to go at all? */

static int handle_live(HANDLE h)
{
    return h != NULL && h != INVALID_HANDLE_VALUE;
}

/* Point one standard handle at the console we just attached to -- but only if
 * it is not already pointing somewhere.
 *
 * THAT GUARD IS THE WHOLE FUNCTION. A parent hands its child the standard
 * handles through STARTUPINFO whatever the child's subsystem is, so
 * `apprecorder record ... > out.txt` arrives with STD_OUTPUT already bound to
 * the file and STD_ERROR unbound. Reopening CONOUT$ over the first would send
 * the output to the screen and leave the file empty, which is a redirection
 * that silently does nothing -- and silently doing nothing is the failure mode
 * this program is least allowed to have.
 *
 * GENERIC_READ as well as GENERIC_WRITE on CONOUT$: GetConsoleMode() needs
 * read access, and cli.c's writer uses exactly that call to decide between
 * WriteConsoleW (wide, straight to the console, no code page in the way) and
 * UTF-8 bytes for a pipe. Open it write-only and every line would take the
 * pipe path and arrive at the console as mojibake. */
static void adopt_std(DWORD which, const wchar_t *device)
{
    HANDLE h;

    if (handle_live(GetStdHandle(which))) return;

    h = CreateFileW(device, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        if (!SetStdHandle(which, h)) CloseHandle(h);
    }
}

/* Returns non-zero when a line written from here would reach a human.
 *
 * Note what is NOT done: no AllocConsole. A process started from Explorer, a
 * shortcut or the task scheduler has no console and never gets one -- see the
 * "nowhere to print" section of frontend.h for why conjuring one is the wrong
 * answer, and what happens instead. */
static int console_attach(void)
{
    int had_out = handle_live(GetStdHandle(STD_OUTPUT_HANDLE));
    int had_err = handle_live(GetStdHandle(STD_ERROR_HANDLE));

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        adopt_std(STD_OUTPUT_HANDLE, L"CONOUT$");
        adopt_std(STD_ERROR_HANDLE,  L"CONOUT$");
        /* CONIN$ too: cli.c starts its pause-key reader only when standard
         * input really is a console, so without this a `record` run from a
         * terminal would silently lose the P key. */
        adopt_std(STD_INPUT_HANDLE,  L"CONIN$");

        /* The CRT's own streams are unbound in a windowed image, so anything
         * reaching for a FILE* -- log.h's to_stderr sink, an assert -- writes
         * into nothing. Bind the ones we just created; never the ones the
         * parent redirected, which the CRT already has. */
        {
            FILE *f = NULL;
            if (!had_out) (void)_wfreopen_s(&f, L"CONOUT$", L"w", stdout);
            if (!had_err) (void)_wfreopen_s(&f, L"CONOUT$", L"w", stderr);
        }
    }

    /* UTF-8 for the byte path, and it must be set before the first line rather
     * than inside apr_cli_main: the Windows-floor refusal below is printed
     * before the command line is ever entered. */
    SetConsoleOutputCP(CP_UTF8);

    return handle_live(GetStdHandle(STD_OUTPUT_HANDLE)) ||
           handle_live(GetStdHandle(STD_ERROR_HANDLE));
}

/* One complete sentence, to whichever channel exists.
 *
 * The console writer is cli.c's own -- the same one every other line of output
 * goes through -- so there is exactly one answer in this program to "wide text,
 * console or pipe?". MessageBoxW is the fallback rather than a window of ours
 * because this runs before the frame, the theme and the dialog builder exist,
 * and because a system dialog is announced by a screen reader with nothing
 * needed from us. */
static void report(const wchar_t *text)
{
    if (g_have_output) {
        apr_cli_console_write(NULL, APR_CLI_STDERR, text);
    } else {
        MessageBoxW(NULL, text, apr_str(APR_S_APP_NAME),
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
}

/* THE FLOOR, CHECKED BEFORE ANYTHING IS BUILT, ON BOTH PATHS.
 *
 * Below Windows 10 2004 there is no process-loopback API, so there is no
 * product: every source would fail to open, one confusing message at a time.
 * Refusing once and clearly is kinder than degrading. The sentence names BOTH
 * build numbers -- "your Windows is too old" leaves a person nowhere to go. */
static int floor_refused(void)
{
    wchar_t text[512], need[32], have[32];
    const wchar_t *args[2];

    if (apr_win_meets_floor()) return 0;

    apr_str_number((int64_t)APR_WIN_MIN_BUILD, need, 32);
    apr_str_number((int64_t)apr_win_build(),   have, 32);
    args[0] = need;
    args[1] = have;
    apr_str_format(APR_S_ERR_WINDOWS_TOO_OLD, text, 512, args, 2);

    APR_ERROR(L"refusing to start: build %u is below the floor of %u",
              (unsigned)apr_win_build(), (unsigned)APR_WIN_MIN_BUILD);
    report(text);
    return 1;
}

/* ---------------------------------------------------------------------------
 * The windowed front end
 * ------------------------------------------------------------------------- */

/* Opt-in file logging. A windowed process has nowhere to print, so without
 * this every APR_WARN goes to OutputDebugString and is invisible unless a
 * debugger is already attached -- which is exactly the position we were in
 * when a dialog silently refused to open and the only evidence available was
 * the user saying "it's all silence".
 *
 *     set APPRECORDER_LOG=%TEMP%\apprecorder.log
 */
static void log_from_environment(void)
{
    wchar_t path[1024];
    DWORD   n = GetEnvironmentVariableW(L"APPRECORDER_LOG", path,
                                        (DWORD)(sizeof path / sizeof path[0]));
    AprLogConfig lc;

    if (n == 0 || n >= sizeof path / sizeof path[0]) return;

    memset(&lc, 0, sizeof lc);
    lc.path             = path;
    lc.level            = APR_LOG_DEBUG;
    lc.to_debugger      = 1;
    lc.background_drain = 1;   /* or nothing reaches the file */
    (void)apr_log_init(&lc);
    apr_log_set_level(APR_LOG_DEBUG);
}

/* WHY THE PROCESS DOES NOT JUST EXIT WHEN THE WINDOW CLOSES
 *
 *   It does -- but only after apr_controller_destroy(), which stops any
 *   recording and finalizes every action first. That ordering is the whole
 *   reason the controller is destroyed before the app rather than after. */
static int run_window(HINSTANCE inst, int show, const wchar_t *session_path)
{
    AprUiApp      *app = NULL;
    AprController *ctl = NULL;
    AprErr         e;
    int            rc;

    e = apr_ui_app_create(inst, &app);
    if (apr_failed(&e)) {
        APR_LOG_ERR(APR_LOG_ERROR, &e);
        return 1;
    }

    e = apr_controller_create(app, &ctl);
    if (apr_failed(&e)) {
        /* The window without a controller can navigate but not record, which
         * is worse than not starting: a recorder that silently cannot record
         * is the failure this application is least allowed to have. */
        APR_LOG_ERR(APR_LOG_ERROR, &e);
        apr_ui_app_destroy(app);
        return 1;
    }

    apr_ui_app_show(app, show);

    /* A double-clicked session, opened AFTER the window is up so that the
     * resolve report and the consent question have a real parent to be modal
     * to and a real place in the accessibility tree. The controller's own
     * open-and-say path is used, so what a person hears is word for word what
     * File > Open would have said. */
    if (session_path && session_path[0]) {
        (void)apr_controller_open_session_and_report(ctl, session_path);
    }

    rc = apr_ui_app_run(app);

    /* Controller first: it stops the recording and finalizes every action
     * before anything it depends on is torn down. */
    apr_controller_destroy(ctl);
    apr_ui_app_destroy(app);
    return rc;
}

/* ---------------------------------------------------------------------------
 * The entry point
 * ------------------------------------------------------------------------- */

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    AprFrontEndChoice choice;
    LPWSTR           *argv = NULL;
    int               argc = 0;
    int               com_entered = 0;
    int               rc;

    (void)prev;
    (void)cmd;   /* the raw tail; the split form is what the rule reads */

    /* CommandLineToArgvW rather than the CRT's __wargv: wWinMain is handed a
     * single unsplit string, and the one splitter that agrees with what every
     * other program on the machine sees is the shell's own. A failure here is
     * treated as "no arguments", which opens the window -- the safe direction,
     * because the alternative is running a command line we could not read. */
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) argc = 0;

    apr_frontend_choose(argc, (const wchar_t *const *)argv, &choice);

    /* Only a command line goes looking for a console. Attaching on the
     * windowed path would put this process into the terminal's console group,
     * so a Ctrl+C typed at that prompt afterwards would arrive at a window
     * that has no handler for it. */
    if (choice.front != APR_FRONT_GUI) g_have_output = console_attach();

    /* Windowed only, exactly as before: the command line has --log-file and
     * --log-level and installs its own sink, and two initialisers racing for
     * one log is not a thing to introduce while merging two executables. */
    if (choice.front == APR_FRONT_GUI) log_from_environment();

    (void)apr_str_init();

    if (floor_refused()) {
        if (argv) LocalFree(argv);
        /* APR_CLI_CONFIG: "read, but not a recording that can be made." The
         * exit codes are contract and documented in the help text, so this
         * reuses the one that already means exactly this. On the windowed path
         * nobody reads it, and it costs nothing to be right. */
        return (int)APR_CLI_CONFIG;
    }

    if (apr_frontend_apartment(choice.front) == APR_APARTMENT_STA) {
        /* apr_ui_app_create() requires an apartment-threaded apartment on the
         * thread that will own the window: IAccPropServices -- the thing that
         * gives every control its accessible name -- is created in it and is
         * valid only on the creating thread. Getting this wrong does not
         * crash; it silently loses every accessible name in the application,
         * which is the exact class of defect tests/test_ui_a11y.c exists to
         * catch. The command line's thread deliberately gets none: see the
         * apartment section of capture.h, which is the record of what
         * happened the last time one thread's apartment was made another
         * layer's business. */
        /* S_FALSE means the apartment was already entered and still owes a
         * CoUninitialize, so the flag records the CALL rather than the mode. */
        com_entered = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
    }

    switch (choice.front) {
    case APR_FRONT_GUI:
        rc = run_window(inst, show, choice.session_path);
        break;

    case APR_FRONT_UNKNOWN: {
        /* Named back, never guessed at. The catalog sentence is the command
         * line's own, so a mistyped command reads the same whether it was
         * typed at a prompt or double-clicked into existence. */
        wchar_t        text[512];
        const wchar_t *args[1];

        args[0] = choice.bad_arg ? choice.bad_arg : L"";
        apr_str_format(APR_S_ERR_UNKNOWN_COMMAND, text, 512, args, 1);
        report(text);
        rc = (int)APR_CLI_USAGE;
        break;
    }

    default:
        rc = apr_cli_main(argc, argv);

        /* Nowhere to print and it did not work. Everything it tried to say
         * was dropped, so say the one thing that is still worth saying: that
         * there was output, and where to go to see it. */
        if (apr_frontend_should_explain(g_have_output, rc)) {
            wchar_t        text[512], code[32];
            const wchar_t *args[1];

            apr_str_number((int64_t)rc, code, 32);
            args[0] = code;
            apr_str_format(APR_S_ERR_NO_CONSOLE, text, 512, args, 1);
            MessageBoxW(NULL, text, apr_str(APR_S_APP_NAME),
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        break;
    }

    if (com_entered) CoUninitialize();
    if (argv) LocalFree(argv);
    return rc;
}
