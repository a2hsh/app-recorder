/*
 * registry.c -- the static table of actions (design 3.3).
 *
 * A STATIC ARRAY, ON PURPOSE. No plugin system, no dynamic loading, no ABI to
 * version, no self-registration through CRT initialisers. Adding Opus is
 * adding one file and one array entry, the linker drops whatever no bus uses,
 * and there is nothing to keep the binary under 1 MB except this staying dull.
 *
 * HOW TO ADD AN ACTION
 *
 *   1. Write src/actions/action_<id>.c exporting one
 *      `const AprActionVTable apr_action_<id>`.
 *   2. Add the extern and the array entry below, inside the matching
 *      APR_HAVE_ACTION_<ID> guard.
 *
 *   CMakeLists.txt defines APR_HAVE_ACTION_<ID> from the presence of the file,
 *   so a half-finished encoder cannot break the link for everyone else and a
 *   finished one needs no build-system edit. That is the only reason the
 *   guards exist -- they are not a configuration knob.
 *
 * THE "none" ACTION is built in here rather than living in src/actions,
 * because it encodes nothing: it is the sink a bus has while it is only being
 * metered, and it is what lets the graph be tested with no encoder present.
 * Delete it the day something else guarantees the table is never empty.
 */
#include "action.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * The built-in discard sink
 * ------------------------------------------------------------------------- */

typedef struct NoneState {
    uint64_t frames;
    int      finalized;
} NoneState;

static AprErr none_create(const AprActionConfig *cfg, void **out_state)
{
    NoneState *st;

    if (!out_state) return APR_ERR(APR_E_INVALID_ARG, L"none action with no out slot");
    *out_state = NULL;
    if (!cfg || cfg->sample_rate == 0 || cfg->channels == 0) {
        return APR_ERR(APR_E_INVALID_ARG, L"none action needs a format");
    }
    st = (NoneState *)calloc(1, sizeof *st);
    if (!st) return APR_ERR(APR_E_NO_MEMORY, L"none action state");
    *out_state = st;
    return apr_ok();
}

static AprErr none_on_audio(void *state, const float *pcm, size_t frames, uint64_t qpc)
{
    NoneState *st = (NoneState *)state;

    (void)pcm; (void)qpc;
    if (!st) return APR_ERR(APR_E_INVALID_ARG, L"none action with no state");
    if (st->finalized) return APR_ERR(APR_E_STATE, L"audio after finalize");
    st->frames += frames;
    return apr_ok();
}

static AprErr none_finalize(void *state)
{
    NoneState *st = (NoneState *)state;

    if (!st) return APR_ERR(APR_E_INVALID_ARG, L"none action with no state");
    st->finalized = 1;
    return apr_ok();
}

static void none_destroy(void *state)
{
    free(state);
}

/* It writes no file, encodes nothing and has no numbers to refuse, so it has
 * no check_config and the field is left off entirely -- which is exactly what
 * "NULL means nothing to ask" is for (action.h). */
static const AprActionVTable apr_action_none = {
    "none", APR_S_ACTION_NAME_NONE, L"",
    none_create, none_on_audio, none_finalize, none_destroy
};

/* ---------------------------------------------------------------------------
 * The table
 * ------------------------------------------------------------------------- */

#ifdef APR_HAVE_ACTION_WAV
extern const AprActionVTable apr_action_wav;   /* src/actions/action_wav.c */
#endif
#ifdef APR_HAVE_ACTION_MP3
extern const AprActionVTable apr_action_mp3;          /* src/actions/action_mp3.c */
#endif
#ifdef APR_HAVE_ACTION_OGG
extern const AprActionVTable apr_action_ogg;          /* src/actions/action_ogg.c */
#endif

/* Order is the order the UI offers them in, so it is editorial, not
 * alphabetical: lossless first, then lossy by ubiquity. */
static const AprActionVTable *const g_actions[] = {
#ifdef APR_HAVE_ACTION_WAV
    &apr_action_wav,
#endif
#ifdef APR_HAVE_ACTION_MP3
    &apr_action_mp3,
#endif
#ifdef APR_HAVE_ACTION_OGG
    &apr_action_ogg,
#endif
    &apr_action_none
};

size_t apr_action_count(void)
{
    return sizeof g_actions / sizeof g_actions[0];
}

const AprActionVTable *apr_action_at(size_t index)
{
    return index < apr_action_count() ? g_actions[index] : NULL;
}

/* ASCII lower-case, hand-written rather than tolower(): an id is ASCII by
 * definition and the CRT's locale must not get a vote in whether "WAV" and
 * "wav" are the same format. */
static int id_eq_nocase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == '\0' && *b == '\0';
}

/* Matched on `id`, which is what a session file stores. Never on the display
 * name, which is prose, is not even a string here any more (it is an AprStrId)
 * and changes with the interface language.
 *
 * CASE-INSENSITIVELY, and that was a fix rather than a nicety: `--out x.WAV`
 * resolved -- the extension pass has always been case-insensitive -- while
 * `--format WAV` was refused as "a format this build cannot write", which is
 * a sentence that is simply untrue. An id is a stable ASCII token; its CASE
 * was never part of the contract, only its spelling. Callers should still
 * store the vtable's own `id`, which is the canonical one. */
const AprActionVTable *apr_action_find(const char *id)
{
    size_t i, n = apr_action_count();

    if (!id) return NULL;
    for (i = 0; i < n; i++) {
        if (g_actions[i] && g_actions[i]->id &&
            id_eq_nocase(g_actions[i]->id, id)) {
            return g_actions[i];
        }
    }
    return NULL;
}

/* The registry's side of the optional hook (action.h). One place decides what
 * "no hook" means, so no caller has to test the pointer and none can forget
 * to. */
AprErr apr_action_check_config(const AprActionVTable *vt,
                               const AprActionConfig *cfg)
{
    if (!vt)  return APR_ERR(APR_E_INVALID_ARG, L"no action to ask");
    if (!cfg) return APR_ERR(APR_E_INVALID_ARG, L"no configuration to check");
    if (!vt->check_config) return apr_ok();
    return vt->check_config(cfg);
}
