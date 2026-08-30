/*
 * frontend.h -- one executable, two front ends, and the rule that picks one.
 *
 * ===========================================================================
 * WHY THERE IS ONLY ONE EXECUTABLE
 *
 *   apprecorder.exe and apprecorder_ui_app.exe were 839 KB and 915 KB of
 *   mostly the same bytes: both statically link capture, the graph, the mixer,
 *   the resampler, the drift controller, three encoders, LAME, libogg and
 *   libopus. The command line's own code is a rounding error next to that. So
 *   there is now one file to hand to a friend, one name to put on PATH, and
 *   one process that is a terminal program when it is given a command and a
 *   window when it is not.
 *
 * ===========================================================================
 * THE COST, STATED ONCE, HERE
 *
 *   A PE's subsystem is fixed in its header, and cmd.exe decides whether to
 *   wait for a child by reading that flag. The merged image is WINDOWS
 *   subsystem -- it has to be, or double-clicking it flashes a console -- so
 *   cmd returns to the prompt the instant the process starts:
 *
 *       apprecorder record ... && upload.ps1
 *
 *   no longer sequences, and output interleaves with the next prompt. The exit
 *   code still exists and is still correct; nothing is waiting to read it.
 *   That is what apprecorder-wait.cmd is for -- see the top of that file. The
 *   author accepted this trade knowingly; do not "fix" it by flipping the
 *   subsystem back, which un-merges the executables.
 *
 * ===========================================================================
 * THE DISPATCH RULE, AND WHY IT IS NOT "ANY ARGUMENT MEANS CLI"
 *
 *   "Any argument means the command line" is the obvious rule and it is wrong,
 *   because Explorer passes a double-clicked file as an argument. A session
 *   file opened from a folder must open the WINDOW on that session, not a
 *   command line that has never heard of it. So the question asked is not "are
 *   there arguments" but "does argv[1] name something this program's command
 *   line has":
 *
 *     no arguments                    -> the window, empty
 *     argv[1] is a CLI command        -> the command line
 *       (record, list-apps, list-devices, help, version, save-session --
 *        asked of cli.c through apr_cli_command_from_name, so there is one
 *        list of command names in this program, not two)
 *     argv[1] starts with '-'         -> the command line
 *       (options are the command line's grammar; the window takes none, and
 *        an unknown option is a sentence the command line already knows how
 *        to say)
 *     argv[1] alone, ending in .json  -> the window, opened on that session
 *     anything else                   -> UNKNOWN: a refusal, not a guess
 *
 *   THE LAST LINE IS THE POINT. A mistyped `apprecorder recrod --out x.wav`
 *   must not silently open a window, and a stray `apprecorder notes.txt` must
 *   not silently become a recording. Both are named back to the user and the
 *   process exits APR_CLI_USAGE.
 *
 *   `apprecorder my.json --bus X` is deliberately NOT the window: Explorer
 *   passes exactly one path, so a session path followed by more arguments is a
 *   command line somebody typed. It comes back as UNKNOWN and is reported as
 *   "my.json is not a command apprecorder has", which is exactly what it is --
 *   rather than a window that silently drops the rest of the line.
 *
 * ===========================================================================
 * THE APARTMENT IS A PROPERTY OF THE FRONT END, NOT OF THE PROCESS
 *
 *   This is the part that was already learned the hard way once; capture.h's
 *   apartment section is the record of it. The windowed front end MUST run its
 *   thread as an STA, because IAccPropServices -- which supplies every
 *   control's accessible name -- is valid only on the thread that created it.
 *   The command line's thread has no apartment at all and must keep having
 *   none: everything COM in the core (discover.c, wasapi_common.c,
 *   reconnect.c) enters the MTA on a thread it owns and leaves again, exactly
 *   so that no caller has to care.
 *
 *   One entry point now serves both, so the apartment cannot be decided at the
 *   top of main(): STA on the command line's thread changes what the command
 *   line is, and MTA on the window's thread loses every accessible name
 *   without crashing or logging anything. It is therefore decided AFTER the
 *   dispatch above and from its answer -- apr_frontend_apartment() -- and that
 *   pairing is pinned by tests/test_frontend.c rather than by this comment.
 */
#ifndef APPRECORDER_FRONTEND_H
#define APPRECORDER_FRONTEND_H

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AprFrontEnd {
    APR_FRONT_GUI = 0,   /* open the window */
    APR_FRONT_CLI,       /* hand the whole argv to apr_cli_main */
    APR_FRONT_UNKNOWN    /* argv[1] names nothing this program has */
} AprFrontEnd;

typedef enum AprApartment {
    /* Leave the thread's apartment alone. The capture and discovery layers own
     * theirs on threads they create (capture.h). */
    APR_APARTMENT_NONE = 0,
    /* CoInitializeEx(COINIT_APARTMENTTHREADED) on the thread that will own the
     * window, before anything accessible is built. */
    APR_APARTMENT_STA
} AprApartment;

typedef struct AprFrontEndChoice {
    AprFrontEnd    front;
    /* Borrowed from argv, valid as long as argv is. NULL unless the window is
     * to open on a session file. */
    const wchar_t *session_path;
    /* Borrowed from argv. The argument to name back to the user; set only when
     * front == APR_FRONT_UNKNOWN. */
    const wchar_t *bad_arg;
} AprFrontEndChoice;

/* PURE. Reads argv and nothing else -- no filesystem, no environment, no
 * console -- which is what lets the whole rule above be a table in a test.
 * `out` is fully written on every path. */
void apr_frontend_choose(int argc, const wchar_t *const *argv,
                         AprFrontEndChoice *out);

/* Which COM apartment the chosen front end needs on the thread that runs it.
 * See the apartment section above; this is the whole of that decision. */
AprApartment apr_frontend_apartment(AprFrontEnd front);

/* ---------------------------------------------------------------------------
 * WHEN THERE IS NOWHERE TO PRINT
 *
 * A WINDOWS-subsystem process started from Explorer, a scheduler or a shortcut
 * has no console and no redirection, so every line the command line writes is
 * dropped. Two wrong answers were available:
 *
 *   - Always allocate a console. A scheduled `record --duration 3600` would
 *     then pop a black window onto the desktop every night.
 *   - Say nothing, ever. That is the failure mode this codebase keeps
 *     having: the user's evidence is "it's all silence".
 *
 * So: a run with nowhere to print is allowed to SUCCEED in silence -- a
 * scheduled recording must be able to -- and is never allowed to FAIL in
 * silence. A non-zero exit with no output channel is explained in a message
 * box, which is a system dialog and therefore announced by a screen reader
 * with nothing needed from us.
 * ------------------------------------------------------------------------- */
int apr_frontend_should_explain(int have_output, int exit_code);

#ifdef __cplusplus
}
#endif
#endif /* APPRECORDER_FRONTEND_H */
