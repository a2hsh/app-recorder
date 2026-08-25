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
 *   outcome this program has: an M4A whose moov atom was never written cannot
 *   be repaired from inside a process that no longer exists (SESSION-HANDOFF,
 *   action_m4a). So Ctrl+C is a request, not a kill: it sets a flag, the loop
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
    APR_CLI_CMD_VERSION
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

    /* Filled in by apr_cli_resolve. */
    uint32_t pid;
    wchar_t  endpoint_id[APR_DISC_ENDPOINT_CCH];
    wchar_t  label[APR_NAME_CCH];     /* what a person is shown */
    int      muted_now;               /* would record silence; warn, do not fail */
} AprCliSource;

typedef struct AprCliOutput {
    wchar_t path[APR_CLI_SPEC_CCH];
    char    action_id[16];            /* registry id: "wav", "mp3", "m4a" ... */
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

/* Ask the recording to stop and finalize. This is precisely what the console
 * control handler calls, so a test driving it drives the shipped stop path.
 * Safe from any thread, and safe before a recording has started. */
void apr_cli_request_stop(void);

/* Test seam: nonzero while the console control handler is installed. It exists
 * so that "Ctrl+C is handled" is pinned by the suite rather than by reading
 * the source. */
int apr_cli_test_ctrl_handler_installed(void);

#endif /* APPRECORDER_CLI_H */
