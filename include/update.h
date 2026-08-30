/*
 * update.h -- finding out that a newer apprecorder exists, proving it is ours,
 * and putting it in place without a second process.
 *
 * ===========================================================================
 * 1. THE THREAT MODEL, WHICH IS THE WHOLE DESIGN
 *
 *   Releases are public GitHub releases. GitHub replaces INFRASTRUCTURE, not
 *   TRUST: it makes hosting somebody else's problem, and it makes the account
 *   the single thing standing between a stranger and a binary that every
 *   install downloads and runs. A stolen token, a phished password, a
 *   compromised CI job -- any of those publishes a release, and an updater
 *   that trusts the host runs whatever was published.
 *
 *   So the host is treated as UNTRUSTED and only the author's signing key is
 *   trusted:
 *
 *     - The manifest (release.json) is signed with ECDSA P-256. The public
 *       key is compiled into this binary; the private key lives with the
 *       author and is never in this repository.
 *     - The manifest carries the SHA-256 of the executable. ONE SIGNATURE
 *       PROTECTS BOTH, which is why the hash is inside the signed document
 *       rather than beside it: a signature over the exe alone would leave the
 *       version and the notes forgeable, and a signature over the manifest
 *       alone would leave the download swappable.
 *     - Verification happens BEFORE anything is executed, and before anything
 *       is even renamed into place.
 *     - There is no fallback. A signature that does not verify is not a
 *       degraded update, it is an attack or a corruption, and both are
 *       refused loudly and logged.
 *
 *   The strongest thing a compromised host can therefore do is FAIL TO SERVE
 *   A GOOD BUILD. It cannot serve a bad one.
 *
 *   ECDSA P-256 THROUGH BCrypt, and that choice is AGENTS.md rule 8: signature
 *   verification is in the box on every Windows this product runs on, so
 *   nothing enters vendor/ for it. No libsodium, no OpenSSL, no mbedTLS, and
 *   therefore no third-party cryptography to track for advisories in a program
 *   whose whole job is recording audio.
 *
 * ===========================================================================
 * 2. THE HALF-UPLOADED RELEASE IS A REAL STATE
 *
 *   Publishing three assets is three uploads. Between the first and the last
 *   there is a window in which release.json exists and release.json.sig does
 *   not -- and the naive updater, asked to verify a signature that 404s,
 *   decides "no signature, must be fine".
 *
 *   NO SIGNATURE IS NOT A PASS. It is exactly as fatal as a wrong one, with
 *   one difference: it is not ANNOUNCED, because it is the expected shape of a
 *   release still being uploaded rather than a sign that anything is wrong. The
 *   check produces nothing, the ETag is not remembered (so the next fetch is a
 *   real one rather than a 304), and the next cadence tick tries again.
 *
 *   The stored last-check time DOES advance, because the host answered. That is
 *   deliberate and it is the storm guard: leaving the timestamp alone would let
 *   every recording-stopped trigger re-fetch, for as long as the upload took.
 *
 * ===========================================================================
 * 3. WHY NOT THE GITHUB API
 *
 *   api.github.com is rate-limited to 60 requests per hour per IP for
 *   unauthenticated callers. A household, an office or a campus behind one NAT
 *   shares that budget with every other tool on it, so the failure mode is
 *   "updates stopped working for everyone in the building, intermittently".
 *
 *   The download redirect is not the API and is not rate limited that way:
 *
 *     https://github.com/<owner>/<repo>/releases/latest/download/<asset>
 *
 *   It 302s to the asset for whatever release is currently latest. Redirects
 *   are FOLLOWED (WinHTTP does this by default and it is not switched off) and
 *   certificate validation is NEVER relaxed -- there is no flag in this file
 *   that turns it off and none is to be added. The signature does not make TLS
 *   optional: TLS is what stops an observer learning which builds this machine
 *   is running.
 *
 * ===========================================================================
 * 4. CADENCE, AND WHY IT IS PERSISTED
 *
 *   The author asked for: at startup, every five minutes, and after a
 *   recording stops. The third one is the interesting one -- start/stop
 *   cycling while setting levels is normal, and "check after every stop" turns
 *   ten takes into ten requests -- so a stop only checks IF FIVE MINUTES HAVE
 *   ELAPSED since the last check.
 *
 *   For the same reason the last-check time is written to disk rather than
 *   held in memory: a process that starts, checks, crashes and restarts is a
 *   request storm with a memory-only timestamp, and the machine doing it is by
 *   definition one whose owner is not watching.
 *
 *   Which means startup obeys the same gate as everything else. Only an
 *   explicit "check now" from the user bypasses it, because a person who asks
 *   is owed an answer.
 *
 *   CONDITIONAL REQUESTS ARE WHAT MAKE FIVE MINUTES DEFENSIBLE. The ETag from
 *   the last successful fetch is sent back as If-None-Match, so an unchanged
 *   release answers 304 with no body. That is a few hundred bytes on the wire
 *   and no work on either end.
 *
 * ===========================================================================
 * 5. REPLACEMENT, WITHOUT A SECOND PROCESS
 *
 *   Windows permits RENAMING a running image (it forbids writing to it and
 *   deleting it, which is a different thing). So:
 *
 *     1. download beside the exe as apprecorder.exe.new
 *     2. verify signature and hash -- before anything moves
 *     3. on exit: rename apprecorder.exe -> apprecorder.exe.old
 *                 rename apprecorder.exe.new -> apprecorder.exe
 *
 *   No updater.exe, no scheduled task, no service, nothing left behind to be
 *   its own attack surface.
 *
 *   THE .old IS NOT DELETED AT SWAP TIME. It is kept until the new build has
 *   started successfully once, so a build that does not start is one rename
 *   from recovery rather than a reinstall. apr_update_startup_action() is that
 *   rule, as a pure function, and it is the reason a "pending" version is
 *   recorded next to the timestamp.
 *
 * ===========================================================================
 * 6. THE FOUR RULES THAT ARE NOT NEGOTIABLE
 *
 *   1. NEVER DURING A RECORDING. Not paused, not "it will only take a
 *      second". A take is unrepeatable and this is a convenience. Asked to
 *      apply while recording, this module refuses and the front end SAYS SO
 *      -- it does not stop or pause the recording to make room for itself.
 *   2. NEVER discard the previous version until the new one has proven it
 *      starts.
 *   3. THE CHECK IS OPT-OUT AND THE SETTING PERSISTS. It is a network callback
 *      that tells a server this machine is running apprecorder and how often;
 *      whether to make it is the user's decision, not ours.
 *   4. ANNOUNCED AND KEYBOARD-REACHABLE. The author is blind. An update prompt
 *      he cannot hear is worse than no updater at all -- it is a dialog
 *      stealing focus from a recording for no reason he can perceive. Every
 *      sentence goes through the controller's say/notify pair, which chooses
 *      the status-bar live region or a tray balloon by
 *      a_better_channel_exists(); the manual check is a menu item with a
 *      mnemonic like every other operation.
 *
 *   And one more that is not a rule so much as a temperament: DO NOT SILENTLY
 *   SELF-INSTALL. This build asks. For an initial release going to friends, a
 *   binary that swaps itself unasked is worse than one that prompts.
 *
 * ===========================================================================
 * 7. FAILING QUIETLY, AND FAILING LOUDLY
 *
 *   Offline, DNS down, a captive portal, a corporate proxy that eats it: a
 *   check that could not run is NOT an error worth telling anybody about. It
 *   goes in the log at INFO and nothing else happens.
 *
 *   A signature that does not verify, or a payload whose hash does not match
 *   the signed manifest, is the opposite. It is announced, it is logged at
 *   ERROR, and the downloaded file is deleted. Those two must never be folded
 *   into one "update failed" sentence: one of them means the network is
 *   unreliable and one of them means somebody is trying something.
 *
 * ===========================================================================
 * 8. WHY THERE IS A TRANSPORT SEAM
 *
 *   AGENTS.md rule 1 forbids a test contacting the network, and quite apart
 *   from the rule, a suite that reaches github.com is a suite that fails on an
 *   aeroplane. So every network call goes through AprUpdateHttp, the real
 *   implementation lives in update_http.c behind apr_update_http_winhttp(),
 *   and tests/test_update.c supplies its own -- including one that returns
 *   304, one that returns a manifest signed with a key the build does not
 *   trust, and one that serves a payload whose bytes do not match the hash the
 *   manifest signed for.
 *
 * ===========================================================================
 * THREADING
 *
 *   Nothing here is called on an audio path and nothing here may be. The check
 *   performs network I/O and disk I/O and is meant to run on a worker thread;
 *   apr_update_check_async() supplies one. The pure decision functions
 *   (apr_update_due, apr_update_startup_action, the version comparison in
 *   version.h) are callable from anywhere.
 */
#ifndef APPRECORDER_UPDATE_H
#define APPRECORDER_UPDATE_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "err.h"
#include "version.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Where a release lives
 * ------------------------------------------------------------------------- */

/* The stable redirect. Not the API -- see section 3. */
#define APR_UPDATE_BASE_URL \
    L"https://github.com/rockstorm/apprecorder/releases/latest/download/"

#define APR_UPDATE_MANIFEST_NAME  L"release.json"
#define APR_UPDATE_SIG_NAME       L"release.json.sig"

/* Five minutes, in milliseconds. The timer's period AND the floor a
 * recording-stopped check has to clear (section 4). One constant, because two
 * would drift and the drift would be invisible. */
#define APR_UPDATE_INTERVAL_MS 300000

/* A P-256 signature is r||s, 32 bytes each; a public key is X||Y, likewise.
 * Raw, not DER: BCrypt speaks this format natively, so there is no ASN.1
 * decoder in this program and no place for one to have a length bug. */
#define APR_UPDATE_SIG_BYTES 64
#define APR_UPDATE_PUBKEY_BYTES 64
#define APR_UPDATE_SHA256_BYTES 32

/* The manifest is a few hundred bytes. This is the structural ceiling: past
 * it the document is refused rather than allocated for. */
#define APR_UPDATE_MANIFEST_MAX 65536

#define APR_UPDATE_NOTES_CCH 512
#define APR_UPDATE_VERSION_CCH 32
#define APR_UPDATE_ETAG_CCH 128
#define APR_UPDATE_ASSET_CCH 64

/* ---------------------------------------------------------------------------
 * The manifest
 *
 * The signed document, exactly:
 *
 *   {
 *     "version": "0.0.2",
 *     "asset":   "apprecorder.exe",
 *     "sha256":  "9f86d0...",        (64 lowercase hex characters)
 *     "size":    1013760,
 *     "notes":   "What changed."
 *   }
 *
 * ASCII field names, like the CLI's --json output and for the same reason
 * (cli.c note 4): this is a wire format, not prose, and localizing it would
 * break it. `notes` is the one field a person reads, and it is the release
 * author's own text -- it is displayed, never used to decide anything.
 * ------------------------------------------------------------------------- */

typedef struct AprUpdateManifest {
    wchar_t  version[APR_UPDATE_VERSION_CCH];
    wchar_t  asset[APR_UPDATE_ASSET_CCH];
    wchar_t  notes[APR_UPDATE_NOTES_CCH];
    uint8_t  sha256[APR_UPDATE_SHA256_BYTES];
    uint64_t size;                  /* 0 when the manifest did not say */
} AprUpdateManifest;

/* ---------------------------------------------------------------------------
 * The transport seam (section 8)
 * ------------------------------------------------------------------------- */

typedef struct AprUpdateResponse {
    int    status;                          /* 200, 304, 404, ... */
    size_t len;                             /* bytes written to `body` */
    wchar_t etag[APR_UPDATE_ETAG_CCH];      /* empty when the server sent none */
} AprUpdateResponse;

typedef struct AprUpdateHttp {
    /* GET `url` into `body` (at most `cap` bytes).
     *
     * `etag_in` may be NULL or empty; when it is neither, it is sent as
     * If-None-Match and a 304 with no body is a legitimate, successful
     * outcome. A transport failure -- offline, DNS, TLS, proxy -- returns a
     * failed AprErr; an HTTP status the caller has to interpret returns
     * apr_ok() with the status filled in. Those are different things and
     * collapsing them is how "offline" ends up announced as "update failed". */
    AprErr (*get)(void *user, const wchar_t *url, const wchar_t *etag_in,
                  void *body, size_t cap, AprUpdateResponse *out);

    /* GET `url` straight to `dest_path`, overwriting. The payload is an
     * executable of a megabyte or so and there is no reason for it to pass
     * through memory. */
    AprErr (*download)(void *user, const wchar_t *url, const wchar_t *dest_path);

    void *user;
} AprUpdateHttp;

/* The real one: WinHTTP, redirects followed, certificate validation on. See
 * src/platform/update_http.c. */
const AprUpdateHttp *apr_update_http_winhttp(void);

/* ---------------------------------------------------------------------------
 * The trusted key
 * ------------------------------------------------------------------------- */

/* The compiled-in public key, X||Y. See src/platform/update_key.c. */
const uint8_t *apr_update_public_key(void);

/* TEST ONLY. The key apr_update_check() trusts, in place of the compiled-in
 * one; NULL restores it.
 *
 * IT EXISTS BECAUSE THE THING WORTH PROVING CANNOT BE PROVED WITHOUT IT. The
 * claims this feature rests on are "a tampered manifest is refused" and "a
 * tampered binary is refused", and a suite can only demonstrate those by
 * producing a signature that IS good and then breaking it -- which needs a
 * private key, which by construction is not in this repository. So the suite
 * generates a throwaway P-256 pair with BCrypt and points the checker at it.
 *
 * It is a process-local static, exactly like action_wav.c's write gate and the
 * controller's foreground override: nothing outside this process can reach it,
 * it is never read from a file or an environment variable, and no shipping code
 * path calls it. */
void apr_update_test_set_key(const uint8_t *pubkey);

/* Zero when the key is still all zeros -- i.e. the author has not generated
 * one yet.
 *
 * AN UNSET KEY DISABLES THE CHECK ENTIRELY. It does not mean "verify against
 * zeros" (which fails, loudly, on every release, teaching the user to ignore
 * the sentence) and it certainly does not mean "skip verification". A build
 * with no trusted key has no way to tell a real release from a forged one, so
 * it does not look. */
int apr_update_have_key(void);

/* ---------------------------------------------------------------------------
 * Verification -- the part that matters
 * ------------------------------------------------------------------------- */

/* Parse and verify a manifest in one call, because they must never be
 * separable: a caller that could parse without verifying would eventually do
 * exactly that.
 *
 * `json` is the raw bytes AS FETCHED. The signature covers those bytes
 * exactly -- not a re-serialization, not a normalized form -- so nothing here
 * rewrites them and nothing may.
 *
 * Fails when: the signature is not APR_UPDATE_SIG_BYTES long, the signature
 * does not verify against the compiled-in key, the document is not JSON, a
 * required field is missing or the wrong type, or the version does not parse.
 * On any failure `out` is zeroed. */
AprErr apr_update_verify_manifest(const void *json, size_t json_len,
                                  const void *sig, size_t sig_len,
                                  const uint8_t *pubkey,
                                  AprUpdateManifest *out);

/* SHA-256 of a file on disk, streamed. */
AprErr apr_update_hash_file(const wchar_t *path,
                            uint8_t out[APR_UPDATE_SHA256_BYTES]);

/* Does the file at `path` hash to what the (already verified) manifest says?
 * Returns a failure -- not a boolean -- because "the bytes on disk are not the
 * bytes that were signed" is an error with a sentence attached, and a bool
 * would let a caller drop it. */
AprErr apr_update_verify_payload(const wchar_t *path,
                                 const AprUpdateManifest *m);

/* ---------------------------------------------------------------------------
 * Persisted state
 *
 * HKCU\Software\apprecorder\Update, and NOT a file of our own: the settings
 * are three scalars and a string, the registry is per-user and roams with the
 * profile, and adding a second JSON writer beside session_save.c to store four
 * values would be the rule 3 failure that file's header warns about.
 *
 * TESTS NEVER TOUCH THE REAL KEY. apr_update_state_test_redirect() points the
 * whole module at a subkey of the caller's choosing, which is how
 * tests/test_update.c exercises persistence without leaving anything behind on
 * the author's machine, and without any wall-clock waiting.
 * ------------------------------------------------------------------------- */

typedef struct AprUpdateState {
    int      enabled;                        /* rule 3; default 1 */
    int64_t  last_check_unix;                /* 0 = never checked */
    wchar_t  etag[APR_UPDATE_ETAG_CCH];      /* for If-None-Match */
    wchar_t  pending[APR_UPDATE_VERSION_CCH];/* a swap awaiting first start */
} AprUpdateState;

/* Never fails in a way a caller must handle: a missing or unreadable key
 * yields the defaults, because "the settings could not be read" must not stop
 * the program from starting. */
void   apr_update_state_load(AprUpdateState *out);
AprErr apr_update_state_save(const AprUpdateState *st);

/* NULL restores the real key. The string is a subkey name under
 * HKCU\Software, e.g. L"apprecorder-test-1234\\Update". */
void apr_update_state_test_redirect(const wchar_t *subkey);

/* Remove the redirected key and everything under it. Tests only; refuses when
 * no redirect is in force, so it can never delete the real settings. */
AprErr apr_update_state_test_erase(void);

/* ---------------------------------------------------------------------------
 * Cadence -- pure, so it is a table in a test rather than a wait
 * ------------------------------------------------------------------------- */

typedef enum AprUpdateWhy {
    APR_UPDATE_WHY_STARTUP = 0,
    APR_UPDATE_WHY_TIMER,
    APR_UPDATE_WHY_RECORDING_STOPPED,
    /* The user asked. Bypasses the interval -- somebody who presses the menu
     * item is owed an answer, not silence -- but NOT the opt-out: a disabled
     * check stays disabled until it is re-enabled. */
    APR_UPDATE_WHY_USER
} AprUpdateWhy;

/* Nonzero when a check should happen now.
 *
 * `now_unix` and `last_unix` are seconds. They are PARAMETERS rather than
 * something this function reads, which is what lets every rule in section 4 be
 * asserted without a single Sleep(). */
int apr_update_due(AprUpdateWhy why, int enabled,
                   int64_t now_unix, int64_t last_unix);

/* Nonzero when an update may be APPLIED right now; rule 1. Separate from
 * apr_update_due on purpose -- checking during a recording is harmless and
 * costs nothing, swapping the image under one is not. */
int apr_update_may_apply(int recording);

/* ---------------------------------------------------------------------------
 * The check itself
 * ------------------------------------------------------------------------- */

typedef enum AprUpdateOutcome {
    /* Nothing happened and nothing should be said. Offline, blocked, 304, or
     * a release still half uploaded. */
    APR_UPDATE_NONE = 0,
    APR_UPDATE_UP_TO_DATE,
    APR_UPDATE_AVAILABLE,
    /* A signature or a hash did not match. LOUD. */
    APR_UPDATE_REFUSED,
    /* The check did not run because the user switched it off. */
    APR_UPDATE_DISABLED
} AprUpdateOutcome;

typedef struct AprUpdateResult {
    AprUpdateOutcome  outcome;
    AprUpdateManifest manifest;   /* meaningful when outcome is AVAILABLE */
    AprErr            err;        /* why, for the log and for REFUSED */

    /* WHAT ASKED FOR THIS CHECK, carried back so a front end can tell an
     * answer somebody is waiting for from one nobody asked for.
     *
     * It is the difference between two wrong behaviours. A timer check that
     * announced "you are up to date" would say it every five minutes, for
     * ever, out loud. A menu check that stayed silent because nothing had
     * changed would be a command that appears to do nothing -- which, to
     * somebody working by ear, is indistinguishable from a broken one. */
    AprUpdateWhy      why;
} AprUpdateResult;

/* One check, synchronously, on the calling thread.
 *
 * `http` may be NULL for apr_update_http_winhttp(). `now_unix` is passed in
 * for the same reason it is passed to apr_update_due: so that the suite can
 * drive the cadence without waiting for it.
 *
 * ADVANCES THE STORED LAST-CHECK TIME ONLY WHEN THE SERVER ACTUALLY ANSWERED
 * -- 200 or 304. A failed transport does not count as a check, or an hour
 * offline would leave the machine believing it had checked twelve times. */
AprErr apr_update_check(const AprUpdateHttp *http, int64_t now_unix,
                        AprUpdateWhy why, AprUpdateResult *out);

/* apr_update_check() on a thread this module owns; the callback runs on that
 * thread and must not touch a window (post, do not send). Returns a failure
 * only when the thread could not be started. */
typedef void (*AprUpdateDoneFn)(void *user, const AprUpdateResult *r);
AprErr apr_update_check_async(const AprUpdateHttp *http, int64_t now_unix,
                              AprUpdateWhy why,
                              AprUpdateDoneFn done, void *user);

/* ---------------------------------------------------------------------------
 * Staging and replacement (section 5)
 * ------------------------------------------------------------------------- */

/* Full path of the running image, and of the two names beside it. Exposed
 * because the tests drive the whole swap in a temporary directory, and
 * because a front end reporting where the download went should read the same
 * strings the module used. */
size_t apr_update_image_path(wchar_t *buf, size_t cch);
size_t apr_update_staged_path(const wchar_t *image, wchar_t *buf, size_t cch);
size_t apr_update_backup_path(const wchar_t *image, wchar_t *buf, size_t cch);

/* Download the manifest's asset beside `image`, then verify it against the
 * manifest's hash. A payload that does not match is DELETED before this
 * returns: a file whose only property is "we know it is wrong" has no business
 * sitting next to the executable. */
AprErr apr_update_stage(const AprUpdateHttp *http, const wchar_t *image,
                        const AprUpdateManifest *m);

/* The swap. `image` -> backup, staged -> `image`. Records `version` as the
 * pending build so the next start knows what it is looking at.
 *
 * BOTH RENAMES OR NEITHER: if the second fails, the first is undone, because
 * a machine left with no apprecorder.exe at all is the one outcome worse than
 * not updating. */
AprErr apr_update_swap(const wchar_t *image, const wchar_t *version);

typedef enum AprUpdateStartupAction {
    APR_UPDATE_STARTUP_NOTHING = 0,
    /* The pending version is the one now running: it started, so the previous
     * image can go. */
    APR_UPDATE_STARTUP_RETIRE_BACKUP,
    /* A swap was recorded but something else is running. Keep the backup and
     * say so in the log -- this is the state a person recovers from. */
    APR_UPDATE_STARTUP_KEEP_BACKUP
} AprUpdateStartupAction;

/* Pure. `pending` is what the registry recorded; `running` is
 * APR_VERSION_STRING. See section 5. */
AprUpdateStartupAction apr_update_startup_action(const wchar_t *pending,
                                                 const wchar_t *running);

/* Perform the action above and clear the pending marker when it is done with.
 * Called once, early, on every start. Never fails in a way a caller must
 * handle -- a backup that could not be deleted is a stale file, not a reason
 * to refuse to start. */
void apr_update_startup(const wchar_t *image);

#ifdef __cplusplus
}
#endif
#endif /* APPRECORDER_UPDATE_H */
