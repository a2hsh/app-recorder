/* action.h — what a bus can do with mixed audio.
 *
 * Recording is the first action, not the only one. Encoders (WAV, MP3, OGG)
 * are three registrations behind this interface; transcription later is an
 * on_audio that accumulates instead of encoding and does its network work in
 * finalize. Nothing above this header knows which is which.
 *
 * AAC/M4A was written, shipped and then deleted — see design section 8 for why.
 * The short version: MP4 keeps its index in the moov atom, written only at
 * finalize, so a killed recording is an unplayable file that no surviving
 * process can repair. WAV, MP3 and OGG all degrade to a shorter but playable
 * file. Do not re-add AAC without solving that first.
 *
 * Registration is a static array in core/registry.c — no plugin system, no
 * dynamic loading, no ABI to version. That is deliberate and is what keeps the
 * binary small (design 3.3).
 */
#ifndef APR_ACTION_H
#define APR_ACTION_H

#include <stddef.h>
#include <stdint.h>
#include "err.h"
#include "strings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AprActionConfig {
    const wchar_t *out_path;      /* borrowed for the create() call only */
    uint32_t       sample_rate;
    uint16_t       channels;

    int bitrate_kbps;             /* lossy formats; 0 = implementation default */
    int quality;                  /* codec-specific; 0 = default */
} AprActionConfig;

typedef struct AprActionVTable {
    const char    *id;            /* "wav", "mp3", "ogg" — stable, in session files */

    /* The name a person sees, as a catalog id — NOT a literal. A display name
     * is prose, and prose in code is the one thing AGENTS.md rule 6 forbids;
     * this field was a `const wchar_t *` until it was not, and every call site
     * now resolves it with apr_str() at the point of display.
     *
     * apr_str() locks and may allocate, so resolve on the UI thread or a
     * worker — never inside on_audio. */
    AprStrId       display_name_id;

    const wchar_t *extension;     /* without the dot; matched against paths */

    AprErr (*create)(const AprActionConfig *cfg, void **out_state);

    /* Called from the mixer thread with interleaved float32 at the session
     * format. MUST NOT BLOCK ON DISK: each action owns its own write-behind
     * buffering, because a slow encoder must never back-pressure a ring shared
     * by every other bus (design 3.1, ring sizing).
     *
     * qpc is the QPC tick of the first frame in this call, for actions that
     * care about wall-clock position. Encoders can ignore it. */
    AprErr (*on_audio)(void *state, const float *pcm, size_t frames, uint64_t qpc);

    /* Flush and close to a playable file. Called on EVERY exit path, including
     * error paths — a half-written recording must still open in a player. */
    AprErr (*finalize)(void *state);

    void (*destroy)(void *state);

    /* CAN THIS FORMAT BE WRITTEN WITH THESE SETTINGS? Asked with no file and
     * no side effects, so a front end can refuse a plan BEFORE a recording
     * starts. `cfg->out_path` is not consulted and may be NULL.
     *
     * THIS FIELD EXISTS BECAUSE A WHOLE TAKE WAS ONCE LOST TO ITS ABSENCE.
     * `--bitrate 400 --out meeting.mp3 --duration 3600` parsed, passed
     * --dry-run, and then recorded for an hour into nothing: LAME refuses 400
     * kbps, but the only place that refusal could happen was create(), which
     * runs at apr_bus_start() -- after the recording has begun, where a failed
     * action is downgraded to a per-output skip. An encoder's limits are
     * knowable from the numbers alone; the only thing missing was somewhere to
     * ask.
     *
     * Every implementation is the SAME function its own create() calls for
     * these checks, never a second copy of the numbers -- a copy that drifted
     * would put the refusal back where it was.
     *
     * NULL means "nothing to ask": the format accepts every config create()
     * would. It is LAST in this struct on purpose, so that a vtable written
     * before this existed still compiles and still means exactly that.
     *
     * Cheap and synchronous. Never resolves a string and never touches disk. */
    AprErr (*check_config)(const AprActionConfig *cfg);
} AprActionVTable;

/* --- registry (core/registry.c) ------------------------------------------ */

/* Matched on `id`, case-insensitively: `id` is a stable ASCII wire value, and
 * "WAV" is the same format as "wav" whether it was typed after --format or
 * hand-edited into a session file. The canonical spelling is the vtable's own
 * and is what a caller should store. NULL if unknown. */
const AprActionVTable *apr_action_find(const char *id);
size_t                 apr_action_count(void);
const AprActionVTable *apr_action_at(size_t index);

/* Ask an action whether it can be written with `cfg` -- the registry's side of
 * the vtable's check_config, so that no caller has to know that the hook is
 * optional. An action with no hook accepts anything create() would.
 *
 * Call this at plan time (apr_cli_resolve, --dry-run, an Add Output dialog).
 * Getting a refusal here is the difference between a message and a lost
 * recording. */
AprErr apr_action_check_config(const AprActionVTable *vt,
                               const AprActionConfig *cfg);

#ifdef __cplusplus
}
#endif
#endif /* APR_ACTION_H */
