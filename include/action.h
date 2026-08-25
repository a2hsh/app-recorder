/* action.h — what a bus can do with mixed audio.
 *
 * Recording is the first action, not the only one. Encoders (WAV, MP3, OGG,
 * M4A) are four registrations behind this interface; transcription later is an
 * on_audio that accumulates instead of encoding and does its network work in
 * finalize. Nothing above this header knows which is which.
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
    const char    *id;            /* "wav", "mp3", "ogg", "m4a" — stable, in session files */
    const wchar_t *display_name;
    const wchar_t *extension;     /* without the dot */

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
} AprActionVTable;

/* --- registry (core/registry.c) ------------------------------------------ */

const AprActionVTable *apr_action_find(const char *id);   /* NULL if unknown */
size_t                 apr_action_count(void);
const AprActionVTable *apr_action_at(size_t index);

#ifdef __cplusplus
}
#endif
#endif /* APR_ACTION_H */
