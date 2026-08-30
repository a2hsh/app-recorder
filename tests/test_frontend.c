/*
 * test_frontend.c -- one executable, two front ends.
 *
 * Two halves, and they are different KINDS of test on purpose.
 *
 *   The rule (frontend.h) is a pure function over argv, so the whole dispatch
 *   table is asserted here in-process, one row at a time, with no files, no
 *   console and no window.
 *
 *   Everything else about a merged image is a property of the IMAGE and cannot
 *   be seen from inside the process that has it: the subsystem byte in the PE
 *   header, whether output survives a redirection the parent set up, whether a
 *   non-ASCII path comes back the same bytes, and whether the .cmd shim hands
 *   an exit code back. Those cases run the real apprecorder.exe. Every wait is
 *   bounded and every child is terminated if it overruns, because the failure
 *   this file exists to catch -- a command line that opens a window instead --
 *   is otherwise a hang.
 *
 * SAFETY (AGENTS.md rule 1): nothing here renders audio. The only recording
 * command used is --dry-run, which opens no device and writes no file, and the
 * only source named is --fake, which reads no hardware and plays nothing.
 */
#include "test_runner.h"

#include <windows.h>

#include "cli/cli.h"
#include "frontend.h"
#include "session.h"

/* ===========================================================================
 * The rule
 * ========================================================================= */

static AprFrontEndChoice choose(const wchar_t **argv, int argc)
{
    AprFrontEndChoice c;
    memset(&c, 0xAB, sizeof c);          /* every field must be written */
    apr_frontend_choose(argc, (const wchar_t *const *)argv, &c);
    return c;
}

TEST(no_arguments_opens_the_window)
{
    const wchar_t *argv[] = { L"apprecorder.exe" };
    AprFrontEndChoice c = choose(argv, 1);

    ASSERT_EQ_INT(APR_FRONT_GUI, (int)c.front);
    ASSERT_NULL(c.session_path);
    ASSERT_NULL(c.bad_arg);
}

TEST(an_empty_first_argument_opens_the_window)
{
    /* CommandLineToArgvW can and does hand back an empty argument for a
     * trailing "". Treating it as a mistyped command would refuse to start on
     * a stray pair of quotes. */
    const wchar_t *argv[] = { L"apprecorder.exe", L"" };
    AprFrontEndChoice c = choose(argv, 2);

    ASSERT_EQ_INT(APR_FRONT_GUI, (int)c.front);
}

TEST(every_cli_command_takes_the_command_line)
{
    static const wchar_t *const names[] = {
        L"record", L"list-apps", L"list-devices",
        L"help", L"version", L"save-session"
    };
    size_t i;

    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        const wchar_t *argv[] = { L"apprecorder.exe", NULL };
        AprFrontEndChoice c;
        argv[1] = names[i];
        c = choose(argv, 2);
        if (c.front != APR_FRONT_CLI) printf("  command: %ls\n", names[i]);
        ASSERT_EQ_INT(APR_FRONT_CLI, (int)c.front);
        ASSERT_NULL(c.session_path);
    }
}

TEST(the_command_names_come_from_the_command_line_itself)
{
    /* The dispatcher must not own a second list. If a new command is added to
     * cli.c and this lookup does not know it, `apprecorder <newcommand>` opens
     * a window -- silently, and only for the new command. */
    AprCliCommand cmd = (AprCliCommand)-1;

    ASSERT_TRUE(apr_cli_command_from_name(L"record", &cmd));
    ASSERT_EQ_INT(APR_CLI_CMD_RECORD, (int)cmd);
    ASSERT_TRUE(apr_cli_command_from_name(L"save-session", &cmd));
    ASSERT_EQ_INT(APR_CLI_CMD_SAVE_SESSION, (int)cmd);
    ASSERT_TRUE(apr_cli_command_from_name(L"version", NULL));

    ASSERT_FALSE(apr_cli_command_from_name(L"recrod", NULL));
    ASSERT_FALSE(apr_cli_command_from_name(L"", NULL));
    ASSERT_FALSE(apr_cli_command_from_name(NULL, NULL));
    /* Case-sensitive: the grammar is, and guessing is worse than saying so. */
    ASSERT_FALSE(apr_cli_command_from_name(L"Record", NULL));
}

TEST(an_option_first_takes_the_command_line)
{
    static const wchar_t *const opts[] = { L"--help", L"-h", L"-?", L"--json",
                                           L"--nonsense" };
    size_t i;

    for (i = 0; i < sizeof opts / sizeof opts[0]; i++) {
        const wchar_t *argv[] = { L"apprecorder.exe", NULL };
        AprFrontEndChoice c;
        argv[1] = opts[i];
        c = choose(argv, 2);
        if (c.front != APR_FRONT_CLI) printf("  option: %ls\n", opts[i]);
        /* Even --nonsense: an option the command line does not know is a
         * sentence the command line already knows how to say. */
        ASSERT_EQ_INT(APR_FRONT_CLI, (int)c.front);
    }
}

TEST(a_lone_session_file_opens_the_window_on_it)
{
    /* THE REASON THE RULE IS NOT "ANY ARGUMENT MEANS CLI". Explorer passes a
     * double-clicked file as argv[1]. */
    const wchar_t *argv[] = { L"apprecorder.exe", L"C:\\takes\\friday.json" };
    AprFrontEndChoice c = choose(argv, 2);

    ASSERT_EQ_INT(APR_FRONT_GUI, (int)c.front);
    ASSERT_NOT_NULL(c.session_path);
    ASSERT_WSTR_EQ(L"C:\\takes\\friday.json", c.session_path);
    ASSERT_NULL(c.bad_arg);
}

TEST(the_session_extension_is_matched_without_case)
{
    /* Windows file systems do not care about case and Explorer hands back
     * whatever is on disk. The COMMAND names above are case-sensitive; a path
     * is not a command. */
    const wchar_t *argv[] = { L"apprecorder.exe", L"Friday.JSON" };
    AprFrontEndChoice c = choose(argv, 2);

    ASSERT_EQ_INT(APR_FRONT_GUI, (int)c.front);
    ASSERT_NOT_NULL(c.session_path);
}

TEST(the_session_extension_has_one_owner)
{
    /* session.h owns the format, so it owns the extension. If this stops
     * matching, the dispatcher and the save dialog have drifted apart. */
    ASSERT_WSTR_EQ(L"json", APR_SESSION_EXT);
}

TEST(a_session_file_with_more_arguments_is_a_typed_command_line)
{
    /* Explorer passes exactly one path. Something else typed this, and the
     * honest answer is that "my.json" is not a command -- not a window that
     * silently drops the rest. */
    const wchar_t *argv[] = { L"apprecorder.exe", L"my.json", L"--bus", L"Mix" };
    AprFrontEndChoice c = choose(argv, 4);

    ASSERT_EQ_INT(APR_FRONT_UNKNOWN, (int)c.front);
    ASSERT_NOT_NULL(c.bad_arg);
    ASSERT_WSTR_EQ(L"my.json", c.bad_arg);
    ASSERT_NULL(c.session_path);
}

TEST(a_mistyped_command_is_refused_and_not_guessed_at)
{
    /* Without this the whole feature is a trap: `apprecorder recrod --exe
     * teams.exe --out x.wav` opens an empty window and loses the rest. */
    const wchar_t *argv[] = { L"apprecorder.exe", L"recrod", L"--out", L"x.wav" };
    AprFrontEndChoice c = choose(argv, 4);

    ASSERT_EQ_INT(APR_FRONT_UNKNOWN, (int)c.front);
    ASSERT_WSTR_EQ(L"recrod", c.bad_arg);
}

TEST(a_file_that_is_not_a_session_is_refused)
{
    const wchar_t *argv[] = { L"apprecorder.exe", L"notes.txt" };
    AprFrontEndChoice c = choose(argv, 2);

    ASSERT_EQ_INT(APR_FRONT_UNKNOWN, (int)c.front);
    ASSERT_WSTR_EQ(L"notes.txt", c.bad_arg);
}

TEST(a_null_argv_opens_the_window)
{
    /* CommandLineToArgvW failing is reported to the rule as argc 0. Opening
     * the window is the safe direction: the alternative is running a command
     * line nobody could read. */
    AprFrontEndChoice c = choose(NULL, 0);
    ASSERT_EQ_INT(APR_FRONT_GUI, (int)c.front);
}

/* ===========================================================================
 * The apartment
 * ========================================================================= */

TEST(only_the_window_gets_an_apartment)
{
    /* capture.h's apartment section is the record of what happened last time
     * one thread's apartment became another layer's business: STA on the
     * command line's thread and every source fails with RPC_E_CHANGED_MODE;
     * MTA on the window's thread and every accessible name disappears without
     * a crash or a log line. One entry point serves both now, so this pairing
     * is the whole of that decision. */
    ASSERT_EQ_INT(APR_APARTMENT_STA,  (int)apr_frontend_apartment(APR_FRONT_GUI));
    ASSERT_EQ_INT(APR_APARTMENT_NONE, (int)apr_frontend_apartment(APR_FRONT_CLI));
    ASSERT_EQ_INT(APR_APARTMENT_NONE,
                  (int)apr_frontend_apartment(APR_FRONT_UNKNOWN));
}

/* ===========================================================================
 * Nowhere to print
 * ========================================================================= */

TEST(silence_is_allowed_for_a_run_that_worked_and_never_for_one_that_did_not)
{
    /* A scheduled `record --duration 3600` has no console and must not raise a
     * dialog every night. A double-clicked mistake must never be silent. */
    ASSERT_FALSE(apr_frontend_should_explain(0, 0));   /* worked, silent: fine */
    ASSERT_TRUE (apr_frontend_should_explain(0, 1));   /* failed, silent: no  */
    ASSERT_TRUE (apr_frontend_should_explain(0, (int)APR_CLI_CAPTURE));
    ASSERT_FALSE(apr_frontend_should_explain(1, 1));   /* it was already said */
    ASSERT_FALSE(apr_frontend_should_explain(1, 0));
}

/* ===========================================================================
 * The console handle the merged image has to open for itself
 * ========================================================================= */

TEST(getconsolemode_needs_a_readable_conout)
{
    /* src/app/main.c opens CONOUT$ with GENERIC_READ *and* GENERIC_WRITE, and
     * the read half is load-bearing rather than tidy: cli.c decides between
     * WriteConsoleW and UTF-8 bytes by whether GetConsoleMode succeeds on the
     * handle. Open it write-only and every line takes the pipe path and
     * arrives at a console as mojibake -- which is a bug nobody would think to
     * look for in an access mask. */
    HANDLE rw, wo;
    DWORD  mode = 0;

    if (!GetConsoleWindow()) {
        printf("  (no console attached to this run; skipped)\n");
        return;
    }

    rw = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                     OPEN_EXISTING, 0, NULL);
    ASSERT_TRUE(rw != INVALID_HANDLE_VALUE);
    ASSERT_TRUE(GetConsoleMode(rw, &mode) != 0);
    CloseHandle(rw);

    wo = CreateFileW(L"CONOUT$", GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                     OPEN_EXISTING, 0, NULL);
    if (wo != INVALID_HANDLE_VALUE) {
        ASSERT_TRUE(GetConsoleMode(wo, &mode) == 0);
        CloseHandle(wo);
    }
}

/* ===========================================================================
 * The real image
 * ========================================================================= */

#ifdef APR_APP_EXE

#define CAP_BYTES 65536

/* Run a command line, capture stdout AND stderr into `out`, return the exit
 * code through `code`. Returns 0 if the child had to be killed.
 *
 * Handles are passed through STARTUPINFO, which is exactly the shape of
 * `apprecorder record ... > out.txt`: the parent binds the child's standard
 * output before it starts, and the merged image must not reopen CONOUT$ over
 * the top of it. If it did, everything below would read an empty pipe.
 *
 * THE PIPE IS DRAINED WHILE THE CHILD RUNS, not after it exits. Waiting first
 * and reading second deadlocks the moment the child writes more than one pipe
 * buffer -- `apprecorder --help` is several kilobytes and did exactly that,
 * and a deadlock there looks identical to the hang this file exists to catch. */
static int run_capture(const wchar_t *cmdline, DWORD *code, char *out)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW        si;
    PROCESS_INFORMATION pi;
    HANDLE  rd = NULL, wr = NULL;
    wchar_t line[2048];
    size_t  used   = 0;
    int     alive  = 1;
    DWORD   waited = 0;

    out[0] = '\0';
    *code  = 0xFFFFFFFFu;

    memset(&sa, 0, sizeof sa);
    sa.nLength        = sizeof sa;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, CAP_BYTES)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof si);
    si.cb         = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = NULL;
    memset(&pi, 0, sizeof pi);

    wcscpy_s(line, 2048, cmdline);
    if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        printf("  CreateProcessW failed (%lu) for: %ls\n",
               (unsigned long)GetLastError(), cmdline);
        CloseHandle(rd);
        CloseHandle(wr);
        return 0;
    }
    CloseHandle(wr);   /* our copy, or the drain below never sees EOF */

    /* BOUNDED. A dispatch bug that opened a window here would hang forever. */
    for (;;) {
        DWORD avail = 0, got = 0, room;

        room = (DWORD)(CAP_BYTES - 1 - used);
        if (room == 0) break;

        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            if (avail > room) avail = room;
            if (!ReadFile(rd, out + used, avail, &got, NULL) || got == 0) break;
            used += got;
            continue;                   /* more may already be waiting */
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            /* Exited. One last sweep for anything written just before it. */
            while (used + 1 < CAP_BYTES &&
                   PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0 &&
                   ReadFile(rd, out + used, (DWORD)(CAP_BYTES - 1 - used),
                            &got, NULL) && got > 0) {
                used += got;
            }
            break;
        }
        waited += 50;
        if (waited >= 30000) {
            printf("  child overran 30s and was terminated: %ls\n", cmdline);
            TerminateProcess(pi.hProcess, 0xDEAD);
            WaitForSingleObject(pi.hProcess, 5000);
            alive = 0;
            break;
        }
    }
    out[used] = '\0';

    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return alive;
}

static char *g_cap;   /* CAP_BYTES; heap, because it is far too big for a stack */

static char *cap(void)
{
    if (!g_cap) g_cap = (char *)malloc(CAP_BYTES);
    return g_cap;
}

TEST(the_shipped_image_is_windows_subsystem)
{
    /* THE ONE BYTE THAT MAKES ONE EXECUTABLE POSSIBLE. A CONSOLE image flashes
     * a black window on every double-click; the subsystem is fixed in the PE
     * header and there is only one header now. Its cost -- cmd.exe not waiting
     * -- is real, documented in frontend.h, accepted, and answered by
     * apprecorder-wait.cmd. A well-meaning switch back to CONSOLE fails here
     * rather than in somebody's shell three weeks later. */
    HANDLE f;
    DWORD  got = 0;
    unsigned char dos[64];
    unsigned char nt[96];
    LONG   e_lfanew;
    LARGE_INTEGER at;
    WORD   subsystem;

    f = CreateFileW(APR_APP_EXE, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    ASSERT_TRUE(f != INVALID_HANDLE_VALUE);
    if (f == INVALID_HANDLE_VALUE) return;

    ASSERT_TRUE(ReadFile(f, dos, sizeof dos, &got, NULL) && got == sizeof dos);
    ASSERT_EQ_INT('M', dos[0]);
    ASSERT_EQ_INT('Z', dos[1]);
    memcpy(&e_lfanew, dos + 0x3C, sizeof e_lfanew);

    at.QuadPart = e_lfanew;
    SetFilePointerEx(f, at, NULL, FILE_BEGIN);
    ASSERT_TRUE(ReadFile(f, nt, sizeof nt, &got, NULL) && got == sizeof nt);
    ASSERT_EQ_INT('P', nt[0]);
    ASSERT_EQ_INT('E', nt[1]);

    /* Signature 4 + IMAGE_FILE_HEADER 20 = 24; Subsystem sits at offset 68 of
     * the PE32+ optional header. */
    memcpy(&subsystem, nt + 24 + 68, sizeof subsystem);
    ASSERT_EQ_INT(IMAGE_SUBSYSTEM_WINDOWS_GUI, (int)subsystem);

    CloseHandle(f);
}

TEST(a_command_reaches_the_command_line_and_its_output_reaches_the_pipe)
{
    /* The merged image is WINDOWS subsystem, so it starts with no standard
     * handles of its own. Everything below -- an exit code of 0, and text in
     * the pipe -- is downstream of AttachConsole and SetStdHandle in
     * src/app/main.c leaving the parent's redirection alone. */
    wchar_t cmd[1200];
    DWORD   code = 0;

    _snwprintf_s(cmd, 1200, _TRUNCATE, L"\"%ls\" version", APR_APP_EXE);
    ASSERT_TRUE(run_capture(cmd, &code, cap()));
    ASSERT_EQ_INT((int)APR_CLI_OK, (int)code);
    ASSERT_TRUE(strstr(cap(), "apprecorder") != NULL);
}

TEST(an_unknown_first_argument_is_named_back_and_exits_usage)
{
    wchar_t cmd[1200];
    DWORD   code = 0;

    _snwprintf_s(cmd, 1200, _TRUNCATE, L"\"%ls\" recrod --out x.wav",
                 APR_APP_EXE);
    ASSERT_TRUE(run_capture(cmd, &code, cap()));
    ASSERT_EQ_INT((int)APR_CLI_USAGE, (int)code);
    /* Named, not guessed at. */
    ASSERT_TRUE(strstr(cap(), "recrod") != NULL);
}

TEST(help_still_works_through_the_merged_image)
{
    wchar_t cmd[1200];
    DWORD   code = 0;

    _snwprintf_s(cmd, 1200, _TRUNCATE, L"\"%ls\" --help", APR_APP_EXE);
    ASSERT_TRUE(run_capture(cmd, &code, cap()));
    ASSERT_EQ_INT((int)APR_CLI_OK, (int)code);
    ASSERT_TRUE(strstr(cap(), "record") != NULL);
}

TEST(a_non_ascii_output_path_survives_the_round_trip)
{
    /* The output is wide and the pipe is bytes, so something has to choose an
     * encoding. cli.c chose UTF-8 for the byte path; this pins that the merged
     * image did not quietly change the answer by handing it a different kind
     * of handle. Arabic and Japanese together, because a code page that
     * happened to cover one would still lose the other.
     *
     * --dry-run: opens no audio device and writes no file (cli.h). */
    wchar_t cmd[1600];
    wchar_t path[MAX_PATH];
    wchar_t tmp[MAX_PATH];
    char    utf8[MAX_PATH * 4];
    DWORD   code = 0;
    int     n;

    if (!GetTempPathW(MAX_PATH, tmp)) { ASSERT_TRUE(0); return; }
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\x0645\x0644\x0641_\x97f3.wav",
                 tmp);

    n = WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof utf8, NULL, NULL);
    ASSERT_TRUE(n > 0);

    _snwprintf_s(cmd, 1600, _TRUNCATE,
                 L"\"%ls\" record --fake 440 --out \"%ls\" --dry-run",
                 APR_APP_EXE, path);
    ASSERT_TRUE(run_capture(cmd, &code, cap()));
    ASSERT_EQ_INT((int)APR_CLI_OK, (int)code);
    ASSERT_TRUE(strstr(cap(), utf8) != NULL);
}

/* ---------------------------------------------------------------------------
 * The scripting shim
 * ------------------------------------------------------------------------- */
#ifdef APR_APP_SHIM

TEST(the_shim_hands_back_the_exit_code)
{
    /* cmd.exe does not wait for a WINDOWS-subsystem image, so `apprecorder
     * record ... && upload.ps1` stops sequencing. apprecorder-wait.cmd is the
     * answer, and the ONLY thing it has to get right is the exit code: a shim
     * that always returns 0 is worse than no shim, because every && in every
     * script would then run whatever the recording did.
     *
     * Skipped when this run has no console of its own: the child would then
     * have nowhere to print, and a failing command with nowhere to print
     * raises a modal dialog by design (frontend.h). A test must never leave
     * one of those on the author's screen. */
    wchar_t shim[MAX_PATH];
    wchar_t cmd[1600];
    DWORD   code = 0;
    size_t  i;

    if (!GetConsoleWindow()) {
        printf("  (no console attached to this run; skipped)\n");
        return;
    }

    /* cmd.exe's `start` wants backslashes. */
    wcscpy_s(shim, MAX_PATH, APR_APP_SHIM);
    for (i = 0; shim[i]; i++) if (shim[i] == L'/') shim[i] = L'\\';

    /* /s makes cmd strip exactly the outermost pair of quotes, which is the
     * only reliable way to pass a quoted path plus arguments after /c. */
    _snwprintf_s(cmd, 1600, _TRUNCATE, L"cmd.exe /s /c \"\"%ls\" version\"",
                 shim);
    ASSERT_TRUE(run_capture(cmd, &code, cap()));
    ASSERT_EQ_INT((int)APR_CLI_OK, (int)code);

    _snwprintf_s(cmd, 1600, _TRUNCATE, L"cmd.exe /s /c \"\"%ls\" recrod\"",
                 shim);
    ASSERT_TRUE(run_capture(cmd, &code, cap()));
    ASSERT_EQ_INT((int)APR_CLI_USAGE, (int)code);
}

TEST(the_shim_propagates_a_non_zero_code_it_produces_itself)
{
    /* The case above needs a console; this one never does, and between them
     * `exit /b %errorlevel%` is exercised with two different values.
     *
     * The shim finds the executable through %~dp0, so a copy of it on its own
     * in a temp directory takes its own guard path: one line to stderr -- which
     * IS inherited here, because nothing has gone through `start` yet -- and
     * exit code 7. A shim that ended with a bare `exit /b`, or with no `exit`
     * at all, returns 0 from this and every script downstream carries on as if
     * the recording had worked. */
    wchar_t dir[MAX_PATH], copy[MAX_PATH], cmd[1600];
    DWORD   code = 0;

    if (!GetTempPathW(MAX_PATH, dir)) { ASSERT_TRUE(0); return; }
    wcscat_s(dir, MAX_PATH, L"apprecorder_shim_test");
    CreateDirectoryW(dir, NULL);
    _snwprintf_s(copy, MAX_PATH, _TRUNCATE, L"%ls\\apprecorder-wait.cmd", dir);

    {   /* APR_APP_SHIM comes from CMake with forward slashes. */
        wchar_t src[MAX_PATH];
        size_t  i;
        wcscpy_s(src, MAX_PATH, APR_APP_SHIM);
        for (i = 0; src[i]; i++) if (src[i] == L'/') src[i] = L'\\';
        ASSERT_TRUE(CopyFileW(src, copy, FALSE) != 0);
    }

    _snwprintf_s(cmd, 1600, _TRUNCATE, L"cmd.exe /s /c \"\"%ls\" version\"",
                 copy);
    ASSERT_TRUE(run_capture(cmd, &code, cap()));
    ASSERT_EQ_INT((int)APR_CLI_INTERNAL, (int)code);
    ASSERT_TRUE(strstr(cap(), "apprecorder.exe") != NULL);

    DeleteFileW(copy);
    RemoveDirectoryW(dir);
}

#endif /* APR_APP_SHIM */
#endif /* APR_APP_EXE */
