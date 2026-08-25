/*
 * session.h -- a recording setup, written down and read back.
 *
 * The positional command line (cli.h) is right for a one-off invocation and
 * wrong for the thing the author actually has: several buses, several sources
 * each, per-source gain, formats and paths, wanted again tomorrow. A session
 * file is that configuration as a document -- the whole graph, not a preset of
 * a few fields.
 *
 * Format is JSON: hand-rolled writer, jsmn for reading (design section 9).
 * The schema is at the bottom of this header, and it is the contract.
 *
 * ===========================================================================
 * WHY THIS FILE IS MOSTLY ABOUT IDENTITY
 *
 *   Saving is easy. The hard half is that A PROCESS ID IS NOT A NAME. It is a
 *   number the kernel hands out and takes back; after a reboot it means
 *   nothing, and the same number may well belong to something else. So a
 *   session cannot store "record pid 8412" and expect that to survive lunch,
 *   let alone a reboot.
 *
 *   Design 9 says to match on executable path and window class and fall back
 *   to prompting. That is right, and every one of its failure modes is SILENT
 *   if it is handled by a boolean:
 *
 *     - The application is not running. A bus that quietly loses a source
 *       still records, still finalizes, still exits 0, and produces a file
 *       missing the thing it was made for. Nobody finds out until they listen.
 *     - Several instances of the same executable. Picking one at random gets
 *       the wrong Chrome window, which sounds exactly like the right one being
 *       quiet.
 *     - The executable has moved -- an update, a reinstall, a different drive.
 *       Matching only on full path fails; matching only on the leaf name is
 *       how you record the wrong copy of a program.
 *     - The device is unplugged. An endpoint id is a GUID pair; "capture
 *       device {0.0.1.00000000}.{7b1c...} was not found" is not a sentence a
 *       person can act on. That is why BOTH the id and the friendly name are
 *       stored, and why the name is what an absent-device message says.
 *
 *   THEREFORE RESOLUTION RETURNS A REPORT, NOT A BOOLEAN. Every source comes
 *   back with a status, what the file asked for, what was used instead, and --
 *   when several processes matched -- the whole candidate list with the pids.
 *   The CLI prints it; a UI will use the same structure to populate the
 *   "which one did you mean" prompt design 9 asks for. Neither front end gets
 *   to invent its own idea of what went wrong.
 *
 * ===========================================================================
 * EXCLUDE MODE IS GATED, AND A SESSION FILE IS NOT CONSENT
 *
 *   APR_SESSION_SRC_SYSTEM_MINUS_TREE is
 *   PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE: it records EVERYTHING
 *   THE MACHINE IS PLAYING except one process tree. Design 4.1.1: "It must
 *   never be the default, never be silently enabled by a session file without
 *   confirmation."
 *
 *   So loading a file that contains one is not enough to turn it on. The
 *   caller must pass allow_system_capture, and until it does, such a source
 *   resolves to APR_SESSION_NEEDS_CONSENT and the report's
 *   needs_system_capture_consent is set. The command line makes that flag
 *   explicit and typed (--allow-system-capture); a UI must make it a
 *   confirmation the person actually sees.
 *
 *   Two consequences of that, both deliberate:
 *
 *     1. Consent is asked for even when everything else in the file resolved
 *        perfectly. Convenience is not a reason to widen a privacy scope.
 *     2. An EXCLUDE source that CANNOT be resolved is NEVER droppable, not
 *        even under allow_missing. Dropping an ordinary source records less
 *        than was asked for; dropping the target of an exclusion would record
 *        MORE -- everything, with nothing held back. Failing safe here means
 *        failing loudly.
 *
 * ===========================================================================
 * VERSIONING: TWO NUMBERS, BECAUSE ONE CANNOT SAY BOTH THINGS
 *
 *   A file carries "version" (what wrote it) and "minReader" (the oldest
 *   reader that can still be trusted with it).
 *
 *     - minReader > ours          -> refuse. The file uses something we would
 *                                    silently ignore, and ignoring it would
 *                                    change what gets recorded.
 *     - version > ours, minReader
 *       <= ours                   -> load it, report from_newer_writer, and
 *                                    ignore the keys we do not know. This is
 *                                    the lane a future release uses when it
 *                                    only ADDS optional settings.
 *     - version < our minimum     -> refuse; we no longer know that dialect.
 *
 *   Unknown keys are counted and reported, never fatal -- that is what makes
 *   the middle lane work. Values of the WRONG TYPE are fatal, because a
 *   "gainDb" of "loud" is not a setting from the future, it is a broken file,
 *   and quietly defaulting it would change the recording.
 *
 * ===========================================================================
 * NOT A STACK OBJECT
 *
 *   Every capacity here is the graph's own maximum, so an AprSession is a few
 *   hundred kilobytes of fixed arrays -- the same trade AprCliPlan makes, for
 *   the same reason: loading a file must not be able to fail on an allocation,
 *   and nothing here outlives the process. Give it static storage.
 *
 * THREADING: none of this is thread-safe and none of it needs to be. Load,
 * resolve and save happen on whichever thread owns the front end. Nothing here
 * may be called from a capture pump or an on_audio (apr_str() alone forbids
 * it, and so does the file I/O).
 */
#ifndef APPRECORDER_SESSION_H
#define APPRECORDER_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "bus.h"
#include "discover.h"
#include "err.h"
#include "graph.h"
#include "source.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Versions
 * ------------------------------------------------------------------------- */

/* What this build writes into "version". */
#define APR_SESSION_FORMAT_VERSION 1

/* The oldest "version" this build will still read. */
#define APR_SESSION_MIN_VERSION 1

/* What this build writes into "minReader": the oldest reader that can be
 * trusted with a file we produce. It stays at 1 for as long as every field we
 * add is optional; it is raised only by a change that a version-1 reader would
 * MISREAD rather than merely ignore. */
#define APR_SESSION_MIN_READER 1

/* ---------------------------------------------------------------------------
 * Sizes
 * ------------------------------------------------------------------------- */

/* A source's key inside one file. ASCII, generated on save ("s0", "s1", ...),
 * but read back as whatever the file says, because a human may have edited it
 * into something meaningful. */
#define APR_SESSION_KEY_CCH 32

/* Win32 window class names are capped at 256 characters by RegisterClassW. */
#define APR_SESSION_CLASS_CCH 64

/* How many rival processes one resolution reports by name. Past this the
 * count is still exact; only the list is cut. */
#define APR_SESSION_MAX_CANDIDATES 8

/* Where a load failed, as a JSON path ("buses[2].sources[0].gainDb"). */
#define APR_SESSION_WHERE_CCH 128

/* Largest session file this build will read, in bytes. A session is a few
 * kilobytes; this is four orders of magnitude of headroom and exists so that
 * pointing --session at a 4 GB WAV fails in a sentence rather than in the
 * allocator. */
#define APR_SESSION_MAX_BYTES (1u << 20)

/* ---------------------------------------------------------------------------
 * The model
 * ------------------------------------------------------------------------- */

typedef enum AprSessionSrcKind {
    APR_SESSION_SRC_PROCESS = 0,        /* wire: "process"          */
    APR_SESSION_SRC_SYSTEM_MINUS_TREE,  /* wire: "systemMinusTree"  */
    APR_SESSION_SRC_DEVICE,             /* wire: "device"           */
    APR_SESSION_SRC_FAKE                /* wire: "fake"             */
} AprSessionSrcKind;

/* One capture, described so that it can be found again on a different day.
 *
 * The identity fields are all stored TOGETHER and on purpose. `path` is the
 * strong key and `exe` is the one that survives an update; `window_class`
 * exists only to tell instances apart, which is the case neither of the other
 * two can answer. `pid` is a HINT -- written down because it is free and
 * because it settles a same-boot reload instantly, and never trusted on its
 * own, because a reused pid is indistinguishable from the original by number.
 */
typedef struct AprSessionSource {
    char              key[APR_SESSION_KEY_CCH];
    AprSessionSrcKind kind;
    wchar_t           name[APR_NAME_CCH];   /* what a person is shown */

    /* --- process identity (PROCESS, SYSTEM_MINUS_TREE) ------------------- */
    uint32_t pid;                                   /* hint only */
    wchar_t  exe[APR_DISC_NAME_CCH];                /* "teams.exe" */
    wchar_t  path[APR_DISC_PATH_CCH];               /* image path at save time */
    wchar_t  window_class[APR_SESSION_CLASS_CCH];   /* empty when unknown */

    /* --- device identity (DEVICE) ---------------------------------------- */
    /* BOTH, always. The id is what the capture layer wants; the name is the
     * only half of it a person can be told about. */
    wchar_t endpoint_id[APR_DISC_ENDPOINT_CCH];
    wchar_t endpoint_name[APR_DISC_NAME_CCH];

    /* --- synthetic identity (FAKE) --------------------------------------- */
    uint32_t fake_hz;
    int32_t  fake_ppm;
    float    fake_amp;

    /* --- filled in by apr_session_resolve -------------------------------- */
    int      resolved;      /* nonzero once this source can actually be opened */
    uint32_t resolved_pid;
    wchar_t  resolved_endpoint_id[APR_DISC_ENDPOINT_CCH];
    wchar_t  resolved_label[APR_NAME_CCH];  /* what the machine calls it now */
    int      muted_now;     /* would record silence; a warning, not a failure */
} AprSessionSource;

/* A bus's edge to a source. Gain is PER EDGE, not per source, because this is
 * a graph: one source may sit at -6 dB on the full mix and at unity on its own
 * file (design 3.2). Stored in tenths of a dB so no float ever round-trips
 * through the text. */
typedef struct AprSessionEdge {
    char    key[APR_SESSION_KEY_CCH];   /* names an entry in sources[] */
    int32_t gain_db_tenths;

    size_t  source_index;               /* filled by load; SIZE_MAX if dangling */
} AprSessionEdge;

typedef struct AprSessionAction {
    char    id[16];                     /* registry id: "wav", "mp3", "m4a" */
    wchar_t path[APR_DISC_PATH_CCH];
    int     bitrate_kbps;               /* 0 = the encoder's default */
    int     quality;                    /* 0 = the encoder's default */
} AprSessionAction;

typedef struct AprSessionBus {
    wchar_t          name[APR_NAME_CCH];
    AprSessionEdge   edges[APR_MAX_SOURCES_PER_BUS];
    size_t           edge_count;
    AprSessionAction actions[APR_MAX_ACTIONS_PER_BUS];
    size_t           action_count;
} AprSessionBus;

typedef struct AprSession {
    uint32_t sample_rate;
    uint16_t channels;
    int64_t  duration_ms;               /* 0 = until stopped */

    AprSessionSource sources[APR_MAX_SOURCES];
    size_t           source_count;
    AprSessionBus    buses[APR_MAX_BUSES];
    size_t           bus_count;
} AprSession;

/* ---------------------------------------------------------------------------
 * Loading: what can be wrong with a file
 *
 * A fault code rather than a message, so that the front end picks the catalog
 * string (AGENTS.md rule 6) and the session module never composes a sentence.
 * ------------------------------------------------------------------------- */

typedef enum AprSessionFault {
    APR_SESSION_FAULT_NONE = 0,
    APR_SESSION_FAULT_FILE_MISSING,    /* no such file                        */
    APR_SESSION_FAULT_FILE_UNREADABLE, /* there, but the read failed          */
    APR_SESSION_FAULT_TOO_LARGE,       /* past APR_SESSION_MAX_BYTES          */
    APR_SESSION_FAULT_NOT_JSON,        /* malformed                           */
    APR_SESSION_FAULT_TRUNCATED,       /* well-formed as far as it goes       */
    APR_SESSION_FAULT_NOT_A_SESSION,   /* JSON, but not one of ours           */
    APR_SESSION_FAULT_TOO_NEW,         /* minReader above this build          */
    APR_SESSION_FAULT_TOO_OLD,         /* version below APR_SESSION_MIN_VERSION */
    APR_SESSION_FAULT_BAD_TYPE,        /* a known key of the wrong JSON type  */
    APR_SESSION_FAULT_BAD_VALUE,       /* right type, impossible value        */
    APR_SESSION_FAULT_TOO_MANY,        /* more than the fixed arrays hold     */
    APR_SESSION_FAULT_DANGLING_REF     /* a bus names a source key that is not there */
} AprSessionFault;

typedef struct AprSessionLoadReport {
    AprSessionFault fault;
    wchar_t         where[APR_SESSION_WHERE_CCH];  /* JSON path, or the key   */

    int    version;             /* what the file claimed */
    int    min_reader;
    int    from_newer_writer;   /* readable, but written by a later build */

    size_t unknown_keys;        /* ignored on purpose; see the header comment */
    wchar_t first_unknown_key[64];

    size_t source_count;
    size_t bus_count;
} AprSessionLoadReport;

/* ---------------------------------------------------------------------------
 * Resolution: what happened to each source
 * ------------------------------------------------------------------------- */

typedef enum AprSessionResolveStatus {
    /* --- usable, exactly as the file asked ------------------------------- */
    APR_SESSION_EXACT = 0,        /* image path matched, one candidate       */
    APR_SESSION_SAME_PROCESS,     /* the stored pid is still that executable */
    APR_SESSION_DEVICE_EXACT,     /* endpoint id matched                     */
    APR_SESSION_SYNTHETIC,        /* a fake source; nothing to look up       */

    /* --- usable, but NOT the way the file asked. Report these out loud. --- */
    APR_SESSION_MOVED,            /* same executable name, different path    */
    APR_SESSION_BY_WINDOW_CLASS,  /* several instances; the class chose one  */
    APR_SESSION_FIRST_OF_MANY,    /* several instances, nothing to choose by */
    APR_SESSION_DEVICE_BY_NAME,   /* endpoint id changed; friendly name matched */

    /* --- not usable ------------------------------------------------------ */
    APR_SESSION_NOT_RUNNING,      /* no process matched at all               */
    APR_SESSION_AMBIGUOUS,        /* several matched and guessing is refused */
    APR_SESSION_DEVICE_ABSENT,    /* neither id nor name is present          */
    APR_SESSION_NEEDS_CONSENT     /* EXCLUDE source, and consent was not given */
} AprSessionResolveStatus;

/* One candidate process, so that "which one did you mean" can be asked with
 * enough detail to answer. */
typedef struct AprSessionCandidate {
    uint32_t pid;
    wchar_t  path[APR_DISC_PATH_CCH];
    wchar_t  window_class[APR_SESSION_CLASS_CCH];
} AprSessionCandidate;

typedef struct AprSessionResolution {
    size_t                  source_index;
    AprSessionResolveStatus status;
    wchar_t                 name[APR_NAME_CCH];

    /* What the file asked for, and what was used instead. `substituted` is
     * empty unless something really was swapped, so a front end can print the
     * pair only when there is a pair to print. */
    wchar_t wanted[APR_DISC_PATH_CCH];
    wchar_t substituted[APR_DISC_PATH_CCH];

    uint32_t chosen_pid;

    /* The true number of rivals, even when only the first
     * APR_SESSION_MAX_CANDIDATES are listed. */
    size_t              candidate_count;
    size_t              candidates_listed;
    AprSessionCandidate candidates[APR_SESSION_MAX_CANDIDATES];
} AprSessionResolution;

typedef struct AprSessionResolveReport {
    AprSessionResolution items[APR_MAX_SOURCES];
    size_t               count;

    size_t ok;            /* usable, as asked                */
    size_t substituted;   /* usable, but not as asked         */
    size_t failed;        /* not usable                       */

    /* Nonzero when the file contains at least one EXCLUDE source. Set whether
     * or not consent was given, so a caller can say what the flag is FOR. */
    int system_capture_sources;
    /* Nonzero when at least one of those was refused for want of consent. */
    int needs_system_capture_consent;
} AprSessionResolveReport;

typedef struct AprSessionResolveOptions {
    /* Explicit consent for EXCLUDE sources. See the header comment: a session
     * file is not consent, and this must come from a typed flag or a
     * confirmation the person saw. */
    int allow_system_capture;

    /* A source that cannot be found is dropped and reported rather than
     * failing the load. NEVER applies to an EXCLUDE source, whose failure
     * would widen what gets recorded rather than narrow it. */
    int allow_missing;

    /* When several processes match and nothing tells them apart, take the
     * lowest pid and report APR_SESSION_FIRST_OF_MANY instead of refusing
     * with APR_SESSION_AMBIGUOUS.
     *
     * Off by default, deliberately. The command line refuses, exactly as
     * --exe already does, and names every candidate so the user can pin one
     * with --pid; there is nobody to prompt in a script. A UI that CAN prompt
     * wants the candidate list, not this flag either. It exists for the
     * caller who has genuinely decided that any instance will do. */
    int pick_when_ambiguous;
} AprSessionResolveOptions;

/* The machine to resolve against.
 *
 * Passing this explicitly rather than reaching for the live system is what
 * makes every failure mode above testable with no hardware and no second
 * copy of Chrome (design 4.3, same argument). It is not a test-only hook: a
 * UI showing "what would this session do right now" wants exactly this, and a
 * preview that queried the machine itself would be a second implementation.
 */
typedef struct AprSessionMachine {
    const AprAudioApp      *apps;
    size_t                  app_count;
    const AprAudioEndpoint *endpoints;
    size_t                  endpoint_count;

    /* The window class of `pid`'s top-level window, or NULL/empty when there
     * is none to be had -- a console application, a process at a higher
     * integrity level, a window not created yet. Never required: it is a
     * tiebreaker, and a source with one instance resolves without it. */
    const wchar_t *(*window_class)(void *user, uint32_t pid);

    /* Nonzero when a process with this id exists. Separate from `apps`
     * because an application that is running but has not made a sound yet
     * holds no audio session and so is absent from `apps` -- which for a
     * session saved "for the next meeting" is the normal state of affairs. */
    int (*process_exists)(void *user, uint32_t pid);

    /* The leaf of that process's image path ("teams.exe"), empty when it
     * cannot be read. Used ONLY together with process_exists, never alone: a
     * pid on its own is worthless here, but a pid whose image name still
     * matches what the file recorded is two facts agreeing. */
    void (*image_name)(void *user, uint32_t pid, wchar_t *buf, size_t cch);

    void *user;
} AprSessionMachine;

/* ---------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Zero it and set the defaults (48 kHz, stereo). Always call this before
 * filling one in by hand; apr_session_load calls it for you. */
void apr_session_init(AprSession *s);

/* Write `s` to `path` as UTF-8 JSON. Writes to a temporary beside the target
 * and renames over it, so an interrupted save cannot leave a half-written
 * session where a whole one used to be. */
AprErr apr_session_save(const AprSession *s, const wchar_t *path);

/* Render `s` as UTF-8 JSON into caller storage. `out_len` receives the byte
 * count excluding the terminator. APR_E_NO_MEMORY when it does not fit. This
 * is the whole writer; apr_session_save is this plus a file. */
AprErr apr_session_write_utf8(const AprSession *s, char *buf, size_t cap,
                              size_t *out_len);

/* Read a session. `rep` is optional but is the only way to learn WHY a load
 * failed, or that a readable file came from a newer writer.
 *
 * Parses and validates the document; does NOT look at the machine. Nothing in
 * here opens an audio device, enumerates a process, or touches a path -- a
 * load is a pure function of the bytes. */
AprErr apr_session_load(const wchar_t *path, AprSession *out,
                        AprSessionLoadReport *rep);

/* The same, from bytes already in hand. `len` may be 0 for a NUL-terminated
 * buffer. */
AprErr apr_session_load_utf8(const char *json, size_t len, AprSession *out,
                             AprSessionLoadReport *rep);

/* Turn stored identity into something openable, against a supplied machine.
 *
 * Fills each source's resolved_* fields and appends one AprSessionResolution
 * per source to `rep`. `rep` is NOT optional -- resolution without a report is
 * exactly the silent failure this module exists to prevent, so passing NULL
 * returns APR_E_INVALID_ARG.
 *
 * Returns apr_ok() when every source is usable under `opt`. Otherwise it
 * returns a failure and `rep` says which sources and how. A failure still
 * leaves every resolvable source resolved, so a caller honouring
 * allow_missing can proceed with what it has. */
AprErr apr_session_resolve_against(AprSession *s,
                                   const AprSessionResolveOptions *opt,
                                   const AprSessionMachine *m,
                                   AprSessionResolveReport *rep);

/* apr_session_resolve_against() against the live machine: the audio engine's
 * session list, the capture endpoint list, and this process's window
 * enumeration. Enumeration only -- it opens no audio client, so it is as safe
 * inside --dry-run as apr_cli_resolve is. */
AprErr apr_session_resolve(AprSession *s, const AprSessionResolveOptions *opt,
                           AprSessionResolveReport *rep);

/* ---------------------------------------------------------------------------
 * Capturing identity, for saving
 *
 * The inverse of resolution, and it lives here for the same reason: this
 * module decides what a source's identity IS, so it must also be what writes
 * one down. A front end that gathered the fields itself would be free to
 * gather a different set from the one the matcher looks at, and the two would
 * drift the first time either changed.
 * ------------------------------------------------------------------------- */

/* Fill `s`'s process identity -- exe, image path, window class -- from the
 * live machine, given the pid that is being captured right now. Leaves the
 * kind, key and name alone. Missing pieces are left empty rather than
 * guessed: matching degrades gracefully, and a wrong path is worse than none. */
AprErr apr_session_describe_process(AprSessionSource *s, uint32_t pid);

/* Fill `s`'s device identity -- BOTH the endpoint id and the friendly name --
 * from the live capture endpoint list. A session that stored only the id
 * could not name an absent device in a sentence a person can act on. */
AprErr apr_session_describe_device(AprSessionSource *s, const wchar_t *endpoint_id);

/* ---------------------------------------------------------------------------
 * Small helpers a front end needs to say what happened
 * ------------------------------------------------------------------------- */

/* Nonzero when this status means the source can be opened. */
int apr_session_status_usable(AprSessionResolveStatus st);

/* Nonzero when this status means the source is usable but NOT the way the file
 * asked -- i.e. the front end owes the user a sentence about it. */
int apr_session_status_substituted(AprSessionResolveStatus st);

/* The wire name of a kind ("process", "device", ...). Static, never NULL. */
const char *apr_session_kind_wire(AprSessionSrcKind k);

/* ===========================================================================
 * THE SCHEMA
 *
 * {
 *   "apprecorder": { "version": 1, "minReader": 1, "writer": "0.1.0" },
 *   "session":     { "sampleRate": 48000, "channels": 2, "durationMs": 0 },
 *   "sources": [
 *     { "key": "s0", "kind": "process", "name": "Teams",
 *       "pid": 8412,
 *       "exe": "Teams.exe",
 *       "path": "C:\\Users\\me\\AppData\\Local\\Microsoft\\Teams\\Teams.exe",
 *       "windowClass": "Chrome_WidgetWin_1" },
 *     { "key": "s1", "kind": "device", "name": "Chat Mic",
 *       "endpointId": "{0.0.1.00000000}.{7b1c...}",
 *       "endpointName": "Chat Mic (TC-Helicon GoXLR)" },
 *     { "key": "s2", "kind": "fake", "name": "test tone",
 *       "toneHz": 440, "ratePpm": 30, "amplitude": 0.25 }
 *   ],
 *   "buses": [
 *     { "name": "Mix",
 *       "sources": [ { "key": "s0", "gainDb": -6.0 },
 *                    { "key": "s1", "gainDb": 0.0 } ],
 *       "outputs": [ { "format": "wav", "path": "mix.wav",
 *                      "bitrateKbps": 0, "quality": 0 } ] }
 *   ]
 * }
 *
 * Notes that are contract rather than illustration:
 *
 *   - SOURCES ARE A TOP-LEVEL LIST AND BUSES REFERENCE THEM BY KEY. That is
 *     what makes the file a graph rather than a tree: one source feeding two
 *     buses is one entry and two references, with its own gain on each. A
 *     format that nested sources inside buses could not say that at all.
 *   - Keys are file-local. They are not ids, they do not survive a save, and
 *     nothing outside the file may hold one.
 *   - `pid` is written for the same-boot case and is never sufficient on its
 *     own; a reader that trusted it would open whatever inherited the number.
 *   - `gainDb` is dB, so a human editing the file writes -6, not 0.501187.
 *   - `format` is the registry id, NOT the file extension. They coincide today
 *     and are allowed to diverge (an ADTS ".aac" action would be a different
 *     id with the same extension family).
 *   - Field names are ASCII and are NOT localized, for the same reason the
 *     --json output is not: a file a script writes must not change shape
 *     because the interface language did.
 * ========================================================================= */

#ifdef __cplusplus
}
#endif
#endif /* APPRECORDER_SESSION_H */
