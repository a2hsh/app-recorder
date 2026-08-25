/*
 * discover.h -- what is there to record, right now.
 *
 * WHY THIS IS NOT "LIST THE PROCESSES"
 *
 *   A recorder that offers every PID on the machine offers a list nobody can
 *   use: a few hundred rows, almost all of which have never made a sound and
 *   never will. What a person actually wants is the handful of applications
 *   the audio engine currently knows about, which is a completely different
 *   query -- it walks the render endpoints' audio SESSIONS, not the process
 *   table.
 *
 *   The two states worth telling apart are already in the session:
 *   AudioSessionStateActive means audio is flowing now; Inactive means the
 *   application holds a session but is quiet at this moment. Both are
 *   capturable, and both are useful to show, so both come back with a flag.
 *   A process with no session at all is not listed, because there is nothing
 *   there to record.
 *
 * MUTE IS REPORTED HERE, NOT DISCOVERED LATER
 *
 *   Process loopback is post-session-volume (design 4.1 #5, measured): an
 *   application muted in the Windows volume mixer records as pure digital
 *   silence while the engine still reports it rendering. That is the single
 *   most likely "why is my recording empty" outcome, so the listing carries
 *   the flag and the front ends warn before the recording starts rather than
 *   after it is lost.
 *
 * THE PROCESS TREE IS PART OF THE ANSWER, NOT A DETAIL
 *
 *   Both process-loopback modes walk the target's process tree (design 4.1 #9
 *   and 4.1.1, both measured). For INCLUDE that is what makes browsers and
 *   Electron applications work at all. For EXCLUDE it is a privacy trap: the
 *   tree that gets held back includes everything the target has ever started,
 *   so naming a terminal holds back every program launched from it. A front
 *   end cannot describe that honestly without being able to enumerate the
 *   tree, which is what apr_enum_process_tree is for.
 *
 * THREADING: every function here is synchronous, allocates nothing beyond the
 * caller's array, and initialises and uninitialises COM around its own work
 * (MTA, nested initialisation is fine). None of them opens an IAudioClient, so
 * none of them starts a capture or touches an audio device's stream -- which
 * is what lets --dry-run validate a whole configuration without recording.
 */
#ifndef APPRECORDER_DISCOVER_H
#define APPRECORDER_DISCOVER_H

#include <stddef.h>
#include <stdint.h>

#include "err.h"

/* Characters, terminator included. */
#define APR_DISC_NAME_CCH     128
#define APR_DISC_PATH_CCH     260
#define APR_DISC_ENDPOINT_CCH 256

typedef struct AprAudioApp {
    uint32_t pid;
    wchar_t  exe[APR_DISC_NAME_CCH];      /* "chrome.exe", the leaf of `path` */
    wchar_t  display[APR_DISC_NAME_CCH];  /* the session's own name, or `exe` */
    wchar_t  path[APR_DISC_PATH_CCH];     /* full image path, empty if denied */

    int   active;   /* 1: audio is flowing now. 0: session held, but quiet.  */
    int   muted;    /* 1: mixer mute, or session volume exactly 0            */
    float volume;   /* session volume, 0..1                                  */
} AprAudioApp;

typedef struct AprAudioEndpoint {
    wchar_t id[APR_DISC_ENDPOINT_CCH];    /* what AprCaptureConfig wants */
    wchar_t name[APR_DISC_NAME_CCH];      /* friendly name, for a person */
    int     is_default;
} AprAudioEndpoint;

/* Applications with an audio render session, newest endpoint first, one row
 * per process id however many endpoints it is playing to.
 *
 * Writes at most `cap` rows and always reports the true total in
 * `*out_count`, so a caller can tell a full array from an exact fit. */
AprErr apr_enum_audio_apps(AprAudioApp *out, size_t cap, size_t *out_count);

/* Every capture endpoint in DEVICE_STATE_ACTIVE. Same counting rule. */
AprErr apr_enum_capture_endpoints(AprAudioEndpoint *out, size_t cap,
                                  size_t *out_count);

/* Process ids in the tree rooted at `root`, `root` itself first. Same counting
 * rule. A snapshot: a tree grows after it is taken, which is exactly the point
 * the EXCLUDE warning has to make.
 *
 * APR_E_NOT_FOUND when `root` is not a running process. */
AprErr apr_enum_process_tree(uint32_t root, uint32_t *out, size_t cap,
                             size_t *out_count);

/* Nonzero when a process with this id exists. Zero is never a process. */
int apr_process_exists(uint32_t pid);

/* The leaf of the process's image path ("chrome.exe") into `buf`. Falls back
 * to an empty string when the process cannot be opened, which is normal for
 * anything running at a higher integrity level. Returns characters written. */
size_t apr_process_image_name(uint32_t pid, wchar_t *buf, size_t cch);

#endif /* APPRECORDER_DISCOVER_H */
