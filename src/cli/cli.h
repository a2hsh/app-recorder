/*
 * cli.h -- apprecorder's command line, as a testable object rather than a
 * main().
 *
 * THE SURFACE IS AN AUTOMATION SURFACE. The author's words: "a CLI is great
 * because it adds a beautiful way to automate." So the shape is
 * script-first -- every listing has a machine-readable form, every failure has
 * an exit code a shell can branch on, and --dry-run validates a whole
 * configuration without opening an audio device or writing a byte.
 *
 * THE GRAMMAR IS POSITIONAL, AND THAT IS THE POINT
 *
 *   apprecorder [command] [options]
 *
 *   --bus opens a bus. Every source and every output written AFTER it belongs
 *   to that bus, until the next --bus. Without any --bus there is one implicit
 *   bus. That is what makes several buses in one invocation ordinary rather
 *   than special, and several buses in one invocation is the entire reason
 *   this project exists (design 1): a hardware mixer gives you three, software
 *   has no such limit.
 *
 *       apprecorder --bus Mix   --exe teams.exe --device Chat --out mix.wav \
 *                   --bus Voice --device Chat               --out voice.wav
 *
 *   Two buses, one shared device source, two files, one session, one clock
 *   anchor -- so the two files line up with each other and not merely each
 *   with itself (graph.h, apr_graph_run).
 *
 * STOPPING IS THE PART THAT MUST NOT BE CLEVER
 *
 *   A recording killed part-way that leaves an unplayable file is the worst
 *   outcome this program has, and nothing outside this process can repair one
 *   once this process is gone. A container that could not survive it was
 *   deleted rather than shipped (design 8, AAC/M4A), and the formats that
 *   remain still have to be closed properly to end at a sensible frame
 *   boundary. So Ctrl+C is a request, not a kill: it sets a flag, the loop
 *   notices, and every action is finalized before the process exits. A second
 *   Ctrl+C says so and still refuses to abandon the finalize. The console
 *   close button gets the same treatment, with the handler blocking until the
 *   files are closed, because Windows gives a CTRL_CLOSE_EVENT handler a few
 *   seconds before it terminates the process regardless.
 *
 * TESTABILITY
 *
 *   Nothing here writes to stdout directly. Every line goes through AprCliIo,
 *   so tests read what the CLI would have printed instead of scraping a
 *   console, and apr_cli_request_stop() is the same entry point the console
 *   control handler uses -- a test therefore exercises the real stop path
 *   rather than an imitation of it.
 */
#ifndef APPRECORDER_CLI_H
#define APPRECORDER_CLI_H

#include <stddef.h>
#include <stdint.h>

#include "bus.h"
#include "discover.h"
#include "err.h"
#include "graph.h"
#include "log.h"

/* ---------------------------------------------------------------------------
 * Exit codes. These are contract: they are documented in the help text, they
 * are what a script branches on, and they must not be renumbered.
 * ------------------------------------------------------------------------- */
typedef enum AprCliExit {
    APR_CLI_OK         = 0,  /* finished; every file closed and playable      */
    APR_CLI_USAGE      = 1,  /* the command line could not be read            */
    APR_CLI_CONFIG     = 2,  /* read, but not a recording that can be made    */
    APR_CLI_NOT_FOUND  = 3,  /* a named process, app or device is not there   */
    APR_CLI_OUTPUT     = 4,  /* a file could not be created or closed         */
    APR_CLI_CAPTURE    = 5,  /* a source could not be opened or started       */
    APR_CLI_INCOMPLETE = 6,  /* recorded and playable, but something went wrong */
    APR_CLI_INTERNAL   = 7   /* no better description than that               */
} AprCliExit;

typedef enum AprCliCommand {
    APR_CLI_CMD_RECORD = 0,
    APR_CLI_CMD_LIST_APPS,
    APR_CLI_CMD_LIST_DEVICES,
    APR_CLI_CMD_HELP,
    APR_CLI_CMD_VERSION,
    /* Describe a recording and write it down instead of making it. Takes the
     * same positional grammar as `record` and the same --session flag; which
     * direction the file is read or written is the command's job to say, not
     * a second flag's. */
    APR_CLI_CMD_SAVE_SESSION
} AprCliCommand;

typedef enum AprCliSourceKind {
    APR_CLI_SRC_PID = 0,
    APR_CLI_SRC_EXE,
    APR_CLI_SRC_DEVICE,
    APR_CLI_SRC_FAKE,
    /* Records everything the machine plays and holds back one process tree.
     * Privacy-sensitive: never a default, never implied (design 4.1.1). */
    APR_CLI_SRC_SYSTEM_MINUS_TREE
} AprCliSourceKind;

#define APR_CLI_SPEC_CCH  APR_DISC_PATH_CCH
#define APR_CLI_MAX_OUTPUTS_PER_BUS APR_MAX_ACTIONS_PER_BUS

typedef struct AprCliSource {
    AprCliSourceKind kind;
    wchar_t spec[APR_CLI_SPEC_CCH];   /* exactly as it was typed */

    int32_t gain_db_tenths;           /* what the user asked for, for display */
    float   gain;                     /* the linear value the graph wants     */

    uint32_t fake_hz;
    int32_t  fake_ppm;
    float    fake_amp;

    /* Synthetic HEALTH, straight through to AprCaptureConfig.fake's five
     * health fields with nothing in between. capture.h owns the meaning:
     * frame indices rather than ticks, 0 means never, and frame 0 is spelled
     * as start_muted / start_dead rather than as a frame of 0.
     *
     * It is on the --fake spec and not on a flag of its own because the
     * health of a synthetic source is part of describing that source -- the
     * same reason its tone and its drift are there. */
    uint64_t fake_mute_at;
    uint64_t fake_unmute_at;
    uint64_t fake_die_at;
    int      fake_start_muted;
    int      fake_start_dead;

    /* Filled in by apr_cli_resolve. */
    uint32_t pid;
    wchar_t  endpoint_id[APR_DISC_ENDPOINT_CCH];
    wchar_t  label[APR_NAME_CCH];     /* what a person is shown */
    int      muted_now;               /* would record silence; warn, do not fail */
} AprCliSource;

typedef struct AprCliOutput {
    wchar_t path[APR_CLI_SPEC_CCH];
    char    action_id[16];            /* registry id: "wav", "mp3", "ogg" ... */
    int     bitrate_kbps;
    int     quality;
} AprCliOutput;

typedef struct AprCliBus {
    wchar_t      name[APR_NAME_CCH];
    AprCliSource sources[APR_MAX_SOURCES_PER_BUS];
    size_t       source_count;
    AprCliOutput outputs[APR_CLI_MAX_OUTPUTS_PER_BUS];
    size_t       output_count;
} AprCliBus;

/* NOT A STACK OBJECT. Every capacity in here is the graph's own maximum, so a
 * plan is a few hundred kilobytes of fixed arrays -- worth it, because a
 * command line that is being validated must not be able to fail on an
 * allocation, and because nothing here outlives the process. Give it static
 * storage; a local one overflows a default 1 MB stack. */
typedef struct AprCliPlan {
    AprCliCommand cmd;

    uint32_t rate;
    uint16_t channels;
    int64_t  duration_ms;             /* 0 = until stopped */

    int dry_run;
    int json;
    int quiet;
    int all;                          /* list-apps: include silent sessions */

    AprCliBus buses[APR_MAX_BUSES];
    size_t    bus_count;

    /* ---- session files (session.h) ------------------------------------- */

    /* Read by `record`, written by `save-session`. Empty when not given. */
    wchar_t session_file[APR_CLI_SPEC_CCH];

    /* Consent for an EXCLUDE source that a session file asks for. Design
     * 4.1.1: recording everything the machine plays must never be enabled by
     * a file, so this has to be typed. It deliberately does NOT enable
     * anything on its own -- without a session that asks for it, it does
     * nothing at all. */
    int allow_system_capture;

    /* Record without the sources that could not be found, rather than
     * stopping. The run then finishes APR_CLI_INCOMPLETE, never OK, because
     * what was recorded is not what was asked for. */
    int allow_missing;

    /* Set when a session was loaded and something was lost along the way, so
     * that the exit code says so even though every file is playable. */
    int session_incomplete;

    /* Whether the session format's own values were overridden on the command
     * line. A session carries a rate, a channel count and a duration; a flag
     * that was actually typed wins over the file, and one that was not must
     * not silently overwrite it with a default. */
    int explicit_rate;
    int explicit_channels;
    int explicit_duration;

    AprLogLevel log_level;
    wchar_t     log_file[APR_CLI_SPEC_CCH];
    wchar_t     lang[32];
} AprCliPlan;

/* ---------------------------------------------------------------------------
 * Output. One call is one complete line; the writer supplies the terminator,
 * so a test can compare against text with no line-ending question in it.
 * ------------------------------------------------------------------------- */
#define APR_CLI_STDOUT 0
#define APR_CLI_STDERR 1

typedef void (*AprCliWriteFn)(void *user, int stream, const wchar_t *line);

typedef struct AprCliIo {
    AprCliWriteFn write;
    void         *user;
} AprCliIo;

/* Is `name` one of the commands above, and which?  Nonzero when it is; `out`
 * may be NULL when only the yes/no is wanted.
 *
 * PUBLIC BECAUSE THE PROCESS ASKS IT TOO. apprecorder is one executable with
 * two front ends (frontend.h), and which one a command line wants is decided
 * by exactly this question -- `apprecorder record ...` is the command line,
 * `apprecorder` on its own is the window. A second list of command names in
 * the dispatcher would be a list that drifts: adding a command here would
 * quietly make it a name the window opened on. One owner, asked twice. */
int apr_cli_command_from_name(const wchar_t *name, AprCliCommand *out);

/* ---------------------------------------------------------------------------
 * The stages, separately callable so each can be tested on its own
 * ------------------------------------------------------------------------- */

/* argv[0] is the program name and is ignored, exactly as a shell passes it.
 * Fills `plan` with defaults first, so a partially parsed plan is still a
 * well-formed one. Reports the first thing it cannot read and stops. */
AprCliExit apr_cli_parse(int argc, const wchar_t *const *argv,
                         AprCliPlan *plan, const AprCliIo *io);

/* Turn what was typed into what exists: process ids, capture endpoint ids,
 * action ids from file extensions, and whether each output path can be
 * written. Opens NO audio device -- enumeration is a property query -- which
 * is what lets --dry-run mean something. */
AprCliExit apr_cli_resolve(AprCliPlan *plan, const AprCliIo *io);

/* Parse, resolve, then do it. The whole program, minus the console. */
AprCliExit apr_cli_run(int argc, const wchar_t *const *argv, const AprCliIo *io);

/* The real entry point: installs a console writer over apr_cli_run. */
int apr_cli_main(int argc, wchar_t **argv);

/* THE console writer -- an AprCliWriteFn, so it plugs into AprCliIo unchanged.
 * Public because apprecorder is one executable with two front ends now
 * (frontend.h), and its entry point has two sentences of its own to say before
 * apr_cli_main is reached: the Windows floor, and an argument that names
 * nothing. Both must reach the terminal in the same way every other line does
 * -- wide straight to a console, UTF-8 to a pipe or a file -- and a second
 * writer beside this one would be a second answer to the encoding question. */
void apr_cli_console_write(void *user, int stream, const wchar_t *line);

/* Ask the recording to stop and finalize. This is precisely what the console
 * control handler calls, so a test driving it drives the shipped stop path.
 * Safe from any thread, and safe before a recording has started. */
void apr_cli_request_stop(void);

/* ---------------------------------------------------------------------------
 * PAUSE
 *
 * Same shape as the stop above, and for the same reason: one entry point that
 * both the console and a test reach, so the suite exercises the shipped path
 * rather than an imitation of it.
 *
 * A PAUSE TAKES ITS TIME OUT OF THE FILE (bus.h): the take carries on as one
 * file, at one clock, with the paused span simply absent. It is not a mute --
 * nothing is written while it lasts -- and it is not a stop: nothing is
 * finalized, so `record` still produces exactly one file per output.
 *
 * WHY THE CONSOLE HAS NO CONTROL EVENT FOR IT. Windows offers a handler for
 * Ctrl+C and for the close button and nothing else, so unlike stopping there is
 * no signal to hang a pause on. The console therefore reads the keyboard
 * directly, and only when there IS a keyboard: see apr_cli_key_intent.
 *
 * Safe from any thread, safe before a recording has started, and idempotent --
 * pausing a paused recording changes nothing and says nothing. */
void apr_cli_request_pause(void);
void apr_cli_request_resume(void);

/* What a key pressed at the console during a recording means. PURE, and public
 * for exactly that reason: "P pauses" is otherwise a claim only a human at a
 * console can check, and a console is the one thing a test suite does not
 * have. */
typedef enum AprCliKey {
    APR_CLI_KEY_NONE = 0,
    APR_CLI_KEY_PAUSE_TOGGLE
} AprCliKey;

AprCliKey apr_cli_key_intent(wchar_t ch);

/* Test seam: nonzero while the console-key reader is running for the recording
 * in flight. It is started only when standard input really is a console, so a
 * `record` whose input is a pipe or a file -- which is what a script does, and
 * what the suite does -- never grows a reader thread at all. */
int apr_cli_test_console_keys(void);

/* Test seam: nonzero while the console control handler is installed. It exists
 * so that "Ctrl+C is handled" is pinned by the suite rather than by reading
 * the source. */
int apr_cli_test_ctrl_handler_installed(void);

/* Test seam: how long the CTRL_CLOSE/LOGOFF/SHUTDOWN handler waits for every
 * file to be closed. INFINITE, and the suite pins that, because "the handler
 * blocks until the files are closed" above is a promise a bounded wait
 * silently downgrades -- it does not risk a kill part-way through finalize,
 * it guarantees one. */
unsigned long apr_cli_test_close_wait_ms(void);

#endif /* APPRECORDER_CLI_H */
