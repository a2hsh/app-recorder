/*
 * launcher.c -- apprecorder.com, the console front door.
 *
 * ===========================================================================
 * WHY THIS FILE EXISTS
 *
 *   A PE's subsystem is one field in one header, and apprecorder has one
 *   header. It is marked WINDOWS, because a CONSOLE image flashes a black
 *   window on every double-click and that is not a thing to ship.
 *
 *   The cost of that is paid by scripts, and it is worse than it looks:
 *
 *     - PowerShell does NOT wait for a WINDOWS-subsystem child. It returns in
 *       hundredths of a second with no exit code at all, so a scripted
 *       `record --duration 3600` "succeeds" instantly while the recording is
 *       still going. PowerShell is the default shell on Windows 11.
 *     - cmd.exe DOES wait -- this was measured, against the belief that it
 *       does not -- but it hands a WINDOWS-subsystem child no standard
 *       handles, so `apprecorder version > out.txt` writes an EMPTY FILE. The
 *       output is not lost, it goes to the console; it just does not follow a
 *       redirect, a pipe, or a captured variable. For a program whose command
 *       line exists to be automated, that is the whole point missed.
 *
 *   PATHEXT is searched in order and its default begins ".COM;.EXE". So a
 *   CONSOLE-subsystem image called apprecorder.com, sitting beside
 *   apprecorder.exe, is what the shell finds when somebody types
 *   "apprecorder" -- and being a console image it is handed the real
 *   stdin/stdout/stderr, which it passes straight to the child. Redirection,
 *   pipes, exit codes and waiting all come back, in both shells, under the
 *   name the user already knows.
 *
 *   THIS REPLACES apprecorder-wait.cmd, which solved the wrong half. That shim
 *   used `start /wait` to fix a wait cmd.exe was already doing, and `start` is
 *   precisely what severs the standard handles -- so it made redirection worse
 *   while requiring a second name nobody remembers.
 *
 * ===========================================================================
 * NO CRT
 *
 *   Linked with /NODEFAULTLIB against kernel32 alone, with its own entry
 *   point: 132 KB of static CRT for a program that concatenates two strings
 *   and waits on a handle is not a trade worth making when the thing it sits
 *   beside is 1 MB. Hence the hand-written string helpers below, the static
 *   buffers (a 64 KB automatic array would pull in __chkstk), and /GS- in
 *   CMakeLists.txt (the stack cookie lives in the CRT).
 *
 * ===========================================================================
 * CTRL+C IS NOT OURS TO ACT ON
 *
 *   apprecorder's central promise is that every file is finalized and playable
 *   on every exit path, which is why its own Ctrl+C is a request rather than a
 *   kill. A launcher that died on Ctrl+C would hand the prompt back while the
 *   child was still closing its files, and the next thing the user does would
 *   race a finalize.
 *
 *   The console sends the event to EVERY process attached to it, so the child
 *   gets its own copy directly. This process therefore swallows the event --
 *   returning TRUE, which stops the default handler terminating us -- and goes
 *   on waiting. The child decides when the take is over; we only report what
 *   it decided.
 */
/* A Debug build adds /RTC1, whose generated code calls into the CRT
 * (_RTC_InitBase, _RTC_Shutdown, the stack-frame checks) -- and there is no CRT
 * here to call. This pragma is the documented way to turn those checks off for
 * a translation unit, which is better than stubbing CRT internals and hoping
 * the list never grows. It costs nothing: this file has no arithmetic and no
 * arrays to overrun, and the shipped artifact is the Release one either way. */
#pragma runtime_checks("", off)

#include <windows.h>

/* THE OPTIMISER CALLS THE CRT EVEN WHEN THE SOURCE DOES NOT.
 *
 * A byte-copy loop is recognised as memcpy and a fill loop as memset, and the
 * compiler emits a CALL to them -- which does not link when there is no CRT.
 * Supplying them here is the fix. `#pragma function` is what stops the same
 * recognition firing INSIDE these two definitions and turning each into a call
 * to itself, which links perfectly and then recurses until the stack is gone. */
#pragma function(memset, memcpy)

void *__cdecl memset(void *dst, int val, size_t count)
{
    unsigned char *p = (unsigned char *)dst;
    while (count--) *p++ = (unsigned char)val;
    return dst;
}

void *__cdecl memcpy(void *dst, const void *src, size_t count)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (count--) *d++ = *s++;
    return dst;
}

/* One command line (32,767 wide chars is the documented ceiling) and one path,
 * both static: a buffer this size on the stack needs __chkstk, which lives in
 * the CRT this file is built without. */
static wchar_t g_exe[MAX_PATH];
static wchar_t g_line[32768];

static size_t wlen(const wchar_t *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Append, refusing rather than truncating. A silently shortened command line
 * would run a DIFFERENT command from the one that was typed. */
static int wcat(wchar_t *dst, size_t cap, size_t *len, const wchar_t *src)
{
    size_t n = wlen(src), i;

    if (*len + n + 1 > cap) return 0;
    for (i = 0; i < n; i++) dst[*len + i] = src[i];
    *len += n;
    dst[*len] = L'\0';
    return 1;
}

/* The command line with argv[0] removed, so the child receives exactly what
 * the user typed after the program name. */
static const wchar_t *args_after_program(const wchar_t *cl)
{
    const wchar_t *p = cl;

    if (*p == L'"') {
        p++;
        while (*p && *p != L'"') p++;
        if (*p == L'"') p++;
    } else {
        while (*p && *p != L' ' && *p != L'\t') p++;
    }
    while (*p == L' ' || *p == L'\t') p++;
    return p;
}

static void say_err(const char *msg)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD  written = 0, n = 0;

    if (h == NULL || h == INVALID_HANDLE_VALUE) return;
    while (msg[n]) n++;
    WriteFile(h, msg, n, &written, NULL);
}

/* See the header comment: swallow, do not die. */
static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    return TRUE;
}

void __stdcall apr_com_start(void)
{
    STARTUPINFOW        si;
    PROCESS_INFORMATION pi;
    DWORD  n, code = 7;
    size_t len = 0, i, cut = 0;

    n = GetModuleFileNameW(NULL, g_exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        say_err("apprecorder.com: cannot determine its own path.\r\n");
        ExitProcess(7);
    }
    /* Swap our own ".com" for ".exe" rather than appending a fixed name, so a
     * renamed pair -- someone who calls it "rec.com" and "rec.exe" -- still
     * finds its partner. The dot has to be looked for AFTER the last
     * separator, or a directory called "C:\my.tools\" eats the search. */
    for (i = 0; g_exe[i]; i++) {
        if (g_exe[i] == L'\\' || g_exe[i] == L'/') cut = i;
    }
    len = n;
    for (i = n; i > cut; i--) {
        if (g_exe[i] == L'.') { g_exe[i] = L'\0'; len = i; break; }
    }
    if (!wcat(g_exe, MAX_PATH, &len, L".exe")) {
        say_err("apprecorder.com: its own path is too long.\r\n");
        ExitProcess(7);
    }
    len = 0;   /* reused below for the command line */

    if (GetFileAttributesW(g_exe) == INVALID_FILE_ATTRIBUTES) {
        say_err("apprecorder.com: apprecorder.exe is not beside it. "
                "Keep the two files together.\r\n");
        ExitProcess(7);
    }

    if (!wcat(g_line, 32768, &len, L"\"") ||
        !wcat(g_line, 32768, &len, g_exe) ||
        !wcat(g_line, 32768, &len, L"\" ") ||
        !wcat(g_line, 32768, &len, args_after_program(GetCommandLineW()))) {
        say_err("apprecorder.com: command line too long.\r\n");
        ExitProcess(7);
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    for (i = 0; i < sizeof si; i++) ((char *)&si)[i] = 0;
    si.cb = sizeof si;
    /* THE WHOLE REASON THIS PROGRAM EXISTS. Whatever this process was given --
     * a console, a file behind a `>`, one end of a pipe -- is handed to the
     * child, which is what a WINDOWS-subsystem image never gets from cmd. */
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    if (!CreateProcessW(g_exe, g_line, NULL, NULL, TRUE, 0, NULL, NULL,
                        &si, &pi)) {
        say_err("apprecorder.com: could not start apprecorder.exe.\r\n");
        ExitProcess(7);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    /* The child's code, unchanged. The exit codes are contract (cli.h); a
     * launcher that invented one would break every script that branches. */
    ExitProcess(code);
}
