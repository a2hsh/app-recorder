/*
 * session_load.c -- reading a session back, and the harder half: working out
 * what its sources MEAN on a machine that has been rebooted since.
 *
 * Two jobs, deliberately separated:
 *
 *   apr_session_load*        bytes  -> model. A pure function. It opens no
 *                            device, enumerates no process, touches no path.
 *   apr_session_resolve*     model  -> something openable, plus a REPORT of
 *                            everything that was not what the file asked for.
 *
 * Keeping them apart is what lets a UI validate a file it was handed without
 * side effects, and lets every identity failure mode below be a plain unit
 * test against a supplied AprSessionMachine rather than something needing two
 * copies of Chrome and an unplugged GoXLR.
 *
 * ===========================================================================
 * JSMN IS A TOKENIZER, NOT A VALIDATOR -- AND THAT IS THE SCHEMA CHECK'S JOB
 *
 *   This surprised me and it is worth writing down rather than rediscovering.
 *   jsmn splits the text into spans; it does NOT enforce that an object's
 *   members are key/value pairs. `{ , , , }` and `{"a": }` both tokenize
 *   without complaint. What it reliably catches is a character that cannot
 *   begin a value (JSMN_ERROR_INVAL) and input that stops mid-token
 *   (JSMN_ERROR_PART).
 *
 *   So the structural check lives here: an object's members are walked as
 *   key-then-value, a key that is not a string is a malformed document, and
 *   every value's TYPE is checked before it is read. Nothing is inferred from
 *   "jsmn accepted it".
 *
 * ===========================================================================
 * WHAT IS FATAL AND WHAT IS NOT
 *
 *   UNKNOWN KEY   -> ignored, counted, named in the report. This is the whole
 *                    forward-compatibility mechanism (see session.h on the two
 *                    version numbers): a later build that only ADDS optional
 *                    settings leaves minReader at 1 and this reader keeps
 *                    working.
 *   WRONG TYPE    -> fatal. "gainDb": "loud" is not a setting from the future,
 *                    it is a broken file, and defaulting it would change the
 *                    recording without saying so.
 *   IMPOSSIBLE
 *   VALUE         -> fatal, same argument. 99 channels is not a channel count
 *                    we can clamp on the user's behalf.
 *   UNKNOWN KIND  -> fatal. Every other unknown thing can be ignored because
 *                    ignoring it records the same audio; not knowing what a
 *                    source IS means not knowing what to capture.
 *
 * ===========================================================================
 * WHERE THE WINDOW-CLASS LOOKUP LIVES
 *
 *   In discover.c, as apr_process_window_class(). It used to be a private
 *   static here on the project's standing rule of adding the shared module
 *   when a second caller exists and not before; the second caller arrived, so
 *   it moved to the owner of "what is running and what is it" (design 7).
 *   What is left in this file is the adapter that gives the
 *   AprSessionMachine callback the borrowed-pointer shape it wants.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "discover.h"
#include "session.h"

/* One translation unit, so nothing of jsmn's reaches the link, and strict
 * mode so that a character which cannot begin a value is an error rather than
 * a shrug. See vendor/jsmn/PROVENANCE.md. */
#define JSMN_STATIC
#define JSMN_STRICT
#pragma warning(push)
#pragma warning(disable : 4127 4244 4245 4267 4389 4505 4701)
#include "jsmn.h"
#pragma warning(pop)

/* A session is a few kilobytes; this is the structural ceiling, not a budget.
 * Past it jsmn returns NOMEM and the load fails with APR_SESSION_FAULT_TOO_MANY
 * rather than in the allocator. */
#define MAX_TOKENS 20000

/* How deep a document may nest before we stop descending. Ours is four deep.
 * This exists so that a file made of ten thousand open brackets costs a
 * refusal rather than the stack. */
#define MAX_DEPTH 24

/* ---------------------------------------------------------------------------
 * Parser state
 * ------------------------------------------------------------------------- */

typedef struct P {
    const char      *js;
    const jsmntok_t *t;
    int              ntok;
    AprSession           *s;
    AprSessionLoadReport *rep;
} P;

static int pfail(P *p, AprSessionFault f, const wchar_t *where)
{
    if (p->rep->fault == APR_SESSION_FAULT_NONE) {
        p->rep->fault = f;
        if (where) wcscpy_s(p->rep->where, APR_SESSION_WHERE_CCH, where);
    }
    return -1;
}

static void punknown(P *p, const wchar_t *key)
{
    p->rep->unknown_keys++;
    if (p->rep->first_unknown_key[0] == L'\0' && key && key[0])
        wcscpy_s(p->rep->first_unknown_key, 64, key);
}

/* ---------------------------------------------------------------------------
 * Token walking
 * ------------------------------------------------------------------------- */

/* Index of the token after the one at `i`, whatever it contains. Returns -1
 * past the depth cap or off the end, and every caller treats that as fatal. */
static int tok_skip(const P *p, int i, int depth)
{
    int n, k;

    if (i < 0 || i >= p->ntok || depth > MAX_DEPTH) return -1;
    switch (p->t[i].type) {
    case JSMN_OBJECT:
        n = p->t[i].size;
        i++;
        for (k = 0; k < n; k++) {
            i = tok_skip(p, i, depth + 1);   /* key   */
            i = tok_skip(p, i, depth + 1);   /* value */
            if (i < 0) return -1;
        }
        return i;
    case JSMN_ARRAY:
        n = p->t[i].size;
        i++;
        for (k = 0; k < n; k++) {
            i = tok_skip(p, i, depth + 1);
            if (i < 0) return -1;
        }
        return i;
    default:
        return i + 1;
    }
}

static size_t tok_len(const jsmntok_t *t)
{
    return (size_t)(t->end - t->start);
}

/* A key token compared against an ASCII literal. Keys are never escaped by
 * our writer and a hand-edited file with an escape in a key simply will not
 * match, which is the right outcome: it is then an unknown key. */
static int key_is(const P *p, int i, const char *name)
{
    size_t n = tok_len(&p->t[i]);
    if (p->t[i].type != JSMN_STRING) return 0;
    if (strlen(name) != n) return 0;
    return memcmp(p->js + p->t[i].start, name, n) == 0;
}

/* A JSON string token as wide text, with the escapes undone.
 *
 * UTF-8 -> UTF-16 first, then unescape, and that order is the point: every
 * escape sequence is ASCII, so working in the wide domain afterwards cannot
 * cut a multi-byte character in half, and \uXXXX lands as one UTF-16 unit
 * with surrogate pairs falling out for free. */
static int tok_wstr(const P *p, int i, wchar_t *out, size_t cch)
{
    wchar_t raw[APR_DISC_PATH_CCH * 2];
    int     n, k;
    size_t  o = 0;

    out[0] = L'\0';
    if (p->t[i].type != JSMN_STRING) return -1;
    if (tok_len(&p->t[i]) >= sizeof raw / sizeof raw[0]) return -1;

    n = MultiByteToWideChar(CP_UTF8, 0, p->js + p->t[i].start,
                            (int)tok_len(&p->t[i]), raw,
                            (int)(sizeof raw / sizeof raw[0]) - 1);
    if (n < 0) return -1;
    raw[n] = L'\0';

    for (k = 0; k < n && o + 1 < cch; k++) {
        if (raw[k] != L'\\') { out[o++] = raw[k]; continue; }
        if (k + 1 >= n) return -1;
        k++;
        switch (raw[k]) {
        case L'"':  out[o++] = L'"';  break;
        case L'\\': out[o++] = L'\\'; break;
        case L'/':  out[o++] = L'/';  break;
        case L'b':  out[o++] = L'\b'; break;
        case L'f':  out[o++] = L'\f'; break;
        case L'n':  out[o++] = L'\n'; break;
        case L'r':  out[o++] = L'\r'; break;
        case L't':  out[o++] = L'\t'; break;
        case L'u': {
            unsigned v = 0;
            int      d;
            if (k + 4 >= n) return -1;
            for (d = 0; d < 4; d++) {
                wchar_t c = raw[++k];
                v <<= 4;
                if      (c >= L'0' && c <= L'9') v |= (unsigned)(c - L'0');
                else if (c >= L'a' && c <= L'f') v |= (unsigned)(c - L'a' + 10);
                else if (c >= L'A' && c <= L'F') v |= (unsigned)(c - L'A' + 10);
                else return -1;
            }
            out[o++] = (wchar_t)v;
            break;
        }
        default:
            return -1;
        }
    }
    out[o] = L'\0';
    return 0;
}

static int tok_astr(const P *p, int i, char *out, size_t cch)
{
    wchar_t w[APR_SESSION_KEY_CCH * 2];
    size_t  k;

    out[0] = '\0';
    if (tok_wstr(p, i, w, sizeof w / sizeof w[0]) != 0) return -1;
    for (k = 0; k + 1 < cch && w[k]; k++) {
        if (w[k] > 127) return -1;   /* keys and action ids are ASCII */
        out[k] = (char)w[k];
    }
    if (w[k]) return -1;             /* would have been truncated */
    out[k] = '\0';
    return 0;
}

/* A JSON number as `scaled` * 10^-decimals, exactly, with no float in the
 * path -- the same rule the writer follows, so a value round-trips through
 * the text unchanged and a decimal comma in someone's locale cannot get in.
 * Rounds half away from zero if the file carries more decimals than we keep. */
static int tok_fixed(const P *p, int i, int decimals, int64_t *out)
{
    const char *s;
    size_t      n, k = 0;
    int         neg = 0, seen = 0, d;
    int64_t     whole = 0, frac = 0, div = 1, extra = 0;

    if (p->t[i].type != JSMN_PRIMITIVE) return -1;
    s = p->js + p->t[i].start;
    n = tok_len(&p->t[i]);
    if (n == 0) return -1;

    if (s[0] == '-') { neg = 1; k = 1; }
    else if (s[0] == '+') { k = 1; }

    for (; k < n && s[k] >= '0' && s[k] <= '9'; k++) {
        if (whole > (INT64_MAX - 9) / 10) return -1;
        whole = whole * 10 + (s[k] - '0');
        seen = 1;
    }
    if (k < n && s[k] == '.') {
        k++;
        for (d = 0; k < n && s[k] >= '0' && s[k] <= '9'; k++, d++) {
            seen = 1;
            if (d < decimals) { frac = frac * 10 + (s[k] - '0'); div *= 10; }
            else if (d == decimals) extra = s[k] - '0';
        }
        for (; d < decimals; d++) { frac *= 10; div *= 10; }
    } else {
        for (d = 0; d < decimals; d++) div *= 10;
        frac = 0;
    }
    /* Exponents are legal JSON and nothing this product writes uses one.
     * Refusing is better than silently reading 1e3 as 1. */
    if (!seen || k != n) return -1;

    if (whole > (INT64_MAX - frac) / div) return -1;
    *out = whole * div + frac;
    if (extra >= 5) (*out)++;
    if (neg) *out = -*out;
    return 0;
}

static int tok_i64(const P *p, int i, int64_t *out)
{
    return tok_fixed(p, i, 0, out);
}

/* ---------------------------------------------------------------------------
 * Where-paths, for a report a person can act on
 * ------------------------------------------------------------------------- */

static const wchar_t *wat(wchar_t *buf, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, APR_SESSION_WHERE_CCH, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

/* ---------------------------------------------------------------------------
 * The schema
 * ------------------------------------------------------------------------- */

/* Every value read goes through one of these, so "wrong type" is reported
 * once, uniformly, with the key it happened at. */
#define WANT(cond, fault, where) do { if (!(cond)) return pfail(p, (fault), (where)); } while (0)

static int read_header(P *p, int obj)
{
    int    n = p->t[obj].size, k, i = obj + 1;
    int    have_version = 0;
    wchar_t w[APR_SESSION_WHERE_CCH];

    for (k = 0; k < n; k++) {
        int key = i, val;
        WANT(p->t[key].type == JSMN_STRING, APR_SESSION_FAULT_NOT_JSON,
             L"apprecorder");
        val = key + 1;
        if (val >= p->ntok) return pfail(p, APR_SESSION_FAULT_TRUNCATED, L"apprecorder");

        if (key_is(p, key, "version") || key_is(p, key, "minReader")) {
            int64_t v = 0;
            int is_min = key_is(p, key, "minReader");
            WANT(tok_i64(p, val, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"apprecorder.%ls", is_min ? L"minReader" : L"version"));
            WANT(v >= 0 && v < 100000, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"apprecorder.%ls", is_min ? L"minReader" : L"version"));
            if (is_min) p->rep->min_reader = (int)v;
            else      { p->rep->version = (int)v; have_version = 1; }
        } else if (key_is(p, key, "writer")) {
            /* Informational. Never acted on -- a file must be judged by its
             * version numbers, not by who claims to have written it. */
        } else {
            wchar_t kw[64];
            (void)tok_wstr(p, key, kw, 64);
            punknown(p, kw);
        }
        i = tok_skip(p, val, 0);
        if (i < 0) return pfail(p, APR_SESSION_FAULT_NOT_JSON, L"apprecorder");
    }

    /* No version at all is not a session file. Anything may contain an object
     * called "apprecorder"; only ours says which dialect it is. */
    if (!have_version)
        return pfail(p, APR_SESSION_FAULT_NOT_A_SESSION, L"apprecorder.version");

    /* minReader defaults to the file's own version when it is not stated:
     * a writer that did not think about forward compatibility gets the
     * conservative reading. */
    if (p->rep->min_reader == 0) p->rep->min_reader = p->rep->version;

    if (p->rep->min_reader > APR_SESSION_FORMAT_VERSION)
        return pfail(p, APR_SESSION_FAULT_TOO_NEW, L"apprecorder.minReader");
    if (p->rep->version < APR_SESSION_MIN_VERSION)
        return pfail(p, APR_SESSION_FAULT_TOO_OLD, L"apprecorder.version");
    if (p->rep->version > APR_SESSION_FORMAT_VERSION)
        p->rep->from_newer_writer = 1;
    return 0;
}

static int read_session_block(P *p, int obj)
{
    int     n = p->t[obj].size, k, i = obj + 1;
    wchar_t w[APR_SESSION_WHERE_CCH];

    for (k = 0; k < n; k++) {
        int     key = i, val;
        int64_t v = 0;
        WANT(p->t[key].type == JSMN_STRING, APR_SESSION_FAULT_NOT_JSON, L"session");
        val = key + 1;
        if (val >= p->ntok) return pfail(p, APR_SESSION_FAULT_TRUNCATED, L"session");

        if (key_is(p, key, "sampleRate")) {
            WANT(tok_i64(p, val, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"session.sampleRate"));
            WANT(v >= 8000 && v <= 384000, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"session.sampleRate"));
            p->s->sample_rate = (uint32_t)v;
        } else if (key_is(p, key, "channels")) {
            WANT(tok_i64(p, val, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"session.channels"));
            WANT(v >= 1 && v <= 8, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"session.channels"));
            p->s->channels = (uint16_t)v;
        } else if (key_is(p, key, "durationMs")) {
            WANT(tok_i64(p, val, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"session.durationMs"));
            WANT(v >= 0 && v <= 86400000, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"session.durationMs"));
            p->s->duration_ms = v;
        } else {
            wchar_t kw[64];
            (void)tok_wstr(p, key, kw, 64);
            punknown(p, kw);
        }
        i = tok_skip(p, val, 0);
        if (i < 0) return pfail(p, APR_SESSION_FAULT_NOT_JSON, L"session");
    }
    return 0;
}

static int kind_of_wire(const P *p, int val, AprSessionSrcKind *out)
{
    char w[32];
    if (tok_astr(p, val, w, sizeof w) != 0) return -1;
    if (strcmp(w, "process") == 0)          { *out = APR_SESSION_SRC_PROCESS;  return 0; }
    if (strcmp(w, "systemMinusTree") == 0)  { *out = APR_SESSION_SRC_SYSTEM_MINUS_TREE; return 0; }
    if (strcmp(w, "device") == 0)           { *out = APR_SESSION_SRC_DEVICE;   return 0; }
    if (strcmp(w, "fake") == 0)             { *out = APR_SESSION_SRC_FAKE;     return 0; }
    return 1;   /* well-formed string, unknown kind */
}

static int read_source(P *p, int obj, size_t index)
{
    AprSessionSource *s = &p->s->sources[index];
    int      n, k, i;
    wchar_t  w[APR_SESSION_WHERE_CCH];
    int      have_kind = 0;

    WANT(p->t[obj].type == JSMN_OBJECT, APR_SESSION_FAULT_BAD_TYPE,
         wat(w, L"sources[%d]", (int)index));

    memset(s, 0, sizeof *s);
    s->fake_amp = 0.25f;

    n = p->t[obj].size;
    i = obj + 1;
    for (k = 0; k < n; k++) {
        int     key = i, val;
        int64_t v = 0;

        WANT(p->t[key].type == JSMN_STRING, APR_SESSION_FAULT_NOT_JSON,
             wat(w, L"sources[%d]", (int)index));
        val = key + 1;
        if (val >= p->ntok)
            return pfail(p, APR_SESSION_FAULT_TRUNCATED,
                         wat(w, L"sources[%d]", (int)index));

        if (key_is(p, key, "key")) {
            WANT(tok_astr(p, val, s->key, APR_SESSION_KEY_CCH) == 0,
                 APR_SESSION_FAULT_BAD_TYPE, wat(w, L"sources[%d].key", (int)index));
            WANT(s->key[0] != '\0', APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"sources[%d].key", (int)index));
        } else if (key_is(p, key, "kind")) {
            int r = kind_of_wire(p, val, &s->kind);
            WANT(r <= 0, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"sources[%d].kind", (int)index));
            WANT(r == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"sources[%d].kind", (int)index));
            have_kind = 1;
        } else if (key_is(p, key, "name")) {
            WANT(tok_wstr(p, val, s->name, APR_NAME_CCH) == 0,
                 APR_SESSION_FAULT_BAD_TYPE, wat(w, L"sources[%d].name", (int)index));
        } else if (key_is(p, key, "pid")) {
            WANT(tok_i64(p, val, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"sources[%d].pid", (int)index));
            WANT(v >= 0 && v <= 0xffffffffLL, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"sources[%d].pid", (int)index));
            s->pid = (uint32_t)v;
        } else if (key_is(p, key, "exe")) {
            WANT(tok_wstr(p, val, s->exe, APR_DISC_NAME_CCH) == 0,
                 APR_SESSION_FAULT_BAD_TYPE, wat(w, L"sources[%d].exe", (int)index));
        } else if (key_is(p, key, "path")) {
            WANT(tok_wstr(p, val, s->path, APR_DISC_PATH_CCH) == 0,
                 APR_SESSION_FAULT_BAD_TYPE, wat(w, L"sources[%d].path", (int)index));
        } else if (key_is(p, key, "windowClass")) {
            WANT(tok_wstr(p, val, s->window_class, APR_SESSION_CLASS_CCH) == 0,
                 APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"sources[%d].windowClass", (int)index));
        } else if (key_is(p, key, "endpointId")) {
            WANT(tok_wstr(p, val, s->endpoint_id, APR_DISC_ENDPOINT_CCH) == 0,
                 APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"sources[%d].endpointId", (int)index));
        } else if (key_is(p, key, "endpointName")) {
            WANT(tok_wstr(p, val, s->endpoint_name, APR_DISC_NAME_CCH) == 0,
                 APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"sources[%d].endpointName", (int)index));
        } else if (key_is(p, key, "toneHz")) {
            WANT(tok_i64(p, val, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"sources[%d].toneHz", (int)index));
            WANT(v >= 0 && v <= 192000, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"sources[%d].toneHz", (int)index));
            s->fake_hz = (uint32_t)v;
        } else if (key_is(p, key, "ratePpm")) {
            WANT(tok_i64(p, val, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"sources[%d].ratePpm", (int)index));
            WANT(v >= -100000 && v <= 100000, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"sources[%d].ratePpm", (int)index));
            s->fake_ppm = (int32_t)v;
        } else if (key_is(p, key, "amplitude")) {
            WANT(tok_fixed(p, val, 6, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"sources[%d].amplitude", (int)index));
            WANT(v >= 0 && v <= 1000000, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"sources[%d].amplitude", (int)index));
            s->fake_amp = (float)((double)v / 1000000.0);
        } else if (key_is(p, key, "muteAtFrame")) {
            WANT(tok_i64(p, val, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"sources[%d].muteAtFrame", (int)index));
            WANT(v >= 0 && v <= APR_SESSION_MAX_FRAME, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"sources[%d].muteAtFrame", (int)index));
            s->fake_mute_at = (uint64_t)v;
        } else if (key_is(p, key, "unmuteAtFrame")) {
            WANT(tok_i64(p, val, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"sources[%d].unmuteAtFrame", (int)index));
            WANT(v >= 0 && v <= APR_SESSION_MAX_FRAME, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"sources[%d].unmuteAtFrame", (int)index));
            s->fake_unmute_at = (uint64_t)v;
        } else if (key_is(p, key, "dieAtFrame")) {
            WANT(tok_i64(p, val, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"sources[%d].dieAtFrame", (int)index));
            WANT(v >= 0 && v <= APR_SESSION_MAX_FRAME, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"sources[%d].dieAtFrame", (int)index));
            s->fake_die_at = (uint64_t)v;
        } else if (key_is(p, key, "startMuted")) {
            WANT(tok_i64(p, val, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"sources[%d].startMuted", (int)index));
            WANT(v == 0 || v == 1, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"sources[%d].startMuted", (int)index));
            s->fake_start_muted = (int)v;
        } else if (key_is(p, key, "startDead")) {
            WANT(tok_i64(p, val, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"sources[%d].startDead", (int)index));
            WANT(v == 0 || v == 1, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"sources[%d].startDead", (int)index));
            s->fake_start_dead = (int)v;
        } else {
            wchar_t kw[64];
            (void)tok_wstr(p, key, kw, 64);
            punknown(p, kw);
        }
        i = tok_skip(p, val, 0);
        if (i < 0)
            return pfail(p, APR_SESSION_FAULT_NOT_JSON,
                         wat(w, L"sources[%d]", (int)index));
    }

    WANT(s->key[0] != '\0', APR_SESSION_FAULT_BAD_VALUE,
         wat(w, L"sources[%d].key", (int)index));
    WANT(have_kind, APR_SESSION_FAULT_BAD_VALUE,
         wat(w, L"sources[%d].kind", (int)index));
    if (s->name[0] == L'\0') {
        /* Never leave a source nameless: every message about it names it. */
        if (s->exe[0])                wcscpy_s(s->name, APR_NAME_CCH, s->exe);
        else if (s->endpoint_name[0]) wcscpy_s(s->name, APR_NAME_CCH, s->endpoint_name);
    }
    return 0;
}

static int read_edge(P *p, int obj, size_t bi, size_t ei, AprSessionEdge *e)
{
    int     n, k, i;
    wchar_t w[APR_SESSION_WHERE_CCH];

    WANT(p->t[obj].type == JSMN_OBJECT, APR_SESSION_FAULT_BAD_TYPE,
         wat(w, L"buses[%d].sources[%d]", (int)bi, (int)ei));

    memset(e, 0, sizeof *e);
    e->source_index = (size_t)-1;

    n = p->t[obj].size;
    i = obj + 1;
    for (k = 0; k < n; k++) {
        int     key = i, val;
        int64_t v = 0;

        WANT(p->t[key].type == JSMN_STRING, APR_SESSION_FAULT_NOT_JSON,
             wat(w, L"buses[%d].sources[%d]", (int)bi, (int)ei));
        val = key + 1;
        if (val >= p->ntok)
            return pfail(p, APR_SESSION_FAULT_TRUNCATED,
                         wat(w, L"buses[%d].sources[%d]", (int)bi, (int)ei));

        if (key_is(p, key, "key")) {
            WANT(tok_astr(p, val, e->key, APR_SESSION_KEY_CCH) == 0,
                 APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"buses[%d].sources[%d].key", (int)bi, (int)ei));
        } else if (key_is(p, key, "gainDb")) {
            WANT(tok_fixed(p, val, 1, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"buses[%d].sources[%d].gainDb", (int)bi, (int)ei));
            WANT(v >= -1200 && v <= 400, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"buses[%d].sources[%d].gainDb", (int)bi, (int)ei));
            e->gain_db_tenths = (int32_t)v;
        } else {
            wchar_t kw[64];
            (void)tok_wstr(p, key, kw, 64);
            punknown(p, kw);
        }
        i = tok_skip(p, val, 0);
        if (i < 0)
            return pfail(p, APR_SESSION_FAULT_NOT_JSON,
                         wat(w, L"buses[%d].sources[%d]", (int)bi, (int)ei));
    }
    WANT(e->key[0] != '\0', APR_SESSION_FAULT_BAD_VALUE,
         wat(w, L"buses[%d].sources[%d].key", (int)bi, (int)ei));
    return 0;
}

static int read_action(P *p, int obj, size_t bi, size_t ai,
                       AprSessionAction *a)
{
    int     n, k, i;
    wchar_t w[APR_SESSION_WHERE_CCH];

    WANT(p->t[obj].type == JSMN_OBJECT, APR_SESSION_FAULT_BAD_TYPE,
         wat(w, L"buses[%d].outputs[%d]", (int)bi, (int)ai));

    memset(a, 0, sizeof *a);
    n = p->t[obj].size;
    i = obj + 1;
    for (k = 0; k < n; k++) {
        int     key = i, val;
        int64_t v = 0;

        WANT(p->t[key].type == JSMN_STRING, APR_SESSION_FAULT_NOT_JSON,
             wat(w, L"buses[%d].outputs[%d]", (int)bi, (int)ai));
        val = key + 1;
        if (val >= p->ntok)
            return pfail(p, APR_SESSION_FAULT_TRUNCATED,
                         wat(w, L"buses[%d].outputs[%d]", (int)bi, (int)ai));

        if (key_is(p, key, "format")) {
            /* NOT validated against the registry here. apr_cli_resolve
             * already owns "is this a format this build can write", and two
             * places answering that question is how they come to disagree. */
            WANT(tok_astr(p, val, a->id, sizeof a->id) == 0,
                 APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"buses[%d].outputs[%d].format", (int)bi, (int)ai));
        } else if (key_is(p, key, "path")) {
            WANT(tok_wstr(p, val, a->path, APR_DISC_PATH_CCH) == 0,
                 APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"buses[%d].outputs[%d].path", (int)bi, (int)ai));
        } else if (key_is(p, key, "bitrateKbps")) {
            WANT(tok_i64(p, val, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"buses[%d].outputs[%d].bitrateKbps", (int)bi, (int)ai));
            WANT(v >= 0 && v <= 1152, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"buses[%d].outputs[%d].bitrateKbps", (int)bi, (int)ai));
            a->bitrate_kbps = (int)v;
        } else if (key_is(p, key, "quality")) {
            WANT(tok_i64(p, val, &v) == 0, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"buses[%d].outputs[%d].quality", (int)bi, (int)ai));
            WANT(v >= 0 && v <= 10, APR_SESSION_FAULT_BAD_VALUE,
                 wat(w, L"buses[%d].outputs[%d].quality", (int)bi, (int)ai));
            a->quality = (int)v;
        } else {
            wchar_t kw[64];
            (void)tok_wstr(p, key, kw, 64);
            punknown(p, kw);
        }
        i = tok_skip(p, val, 0);
        if (i < 0)
            return pfail(p, APR_SESSION_FAULT_NOT_JSON,
                         wat(w, L"buses[%d].outputs[%d]", (int)bi, (int)ai));
    }
    WANT(a->path[0] != L'\0', APR_SESSION_FAULT_BAD_VALUE,
         wat(w, L"buses[%d].outputs[%d].path", (int)bi, (int)ai));
    return 0;
}

static int read_bus(P *p, int obj, size_t index)
{
    AprSessionBus *b = &p->s->buses[index];
    int      n, k, i;
    wchar_t  w[APR_SESSION_WHERE_CCH];

    WANT(p->t[obj].type == JSMN_OBJECT, APR_SESSION_FAULT_BAD_TYPE,
         wat(w, L"buses[%d]", (int)index));

    memset(b, 0, sizeof *b);
    n = p->t[obj].size;
    i = obj + 1;
    for (k = 0; k < n; k++) {
        int key = i, val;

        WANT(p->t[key].type == JSMN_STRING, APR_SESSION_FAULT_NOT_JSON,
             wat(w, L"buses[%d]", (int)index));
        val = key + 1;
        if (val >= p->ntok)
            return pfail(p, APR_SESSION_FAULT_TRUNCATED,
                         wat(w, L"buses[%d]", (int)index));

        if (key_is(p, key, "name")) {
            WANT(tok_wstr(p, val, b->name, APR_NAME_CCH) == 0,
                 APR_SESSION_FAULT_BAD_TYPE, wat(w, L"buses[%d].name", (int)index));
        } else if (key_is(p, key, "sources")) {
            int m, j, q;
            WANT(p->t[val].type == JSMN_ARRAY, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"buses[%d].sources", (int)index));
            m = p->t[val].size;
            if (m > (int)APR_MAX_SOURCES_PER_BUS)
                return pfail(p, APR_SESSION_FAULT_TOO_MANY,
                             wat(w, L"buses[%d].sources", (int)index));
            j = val + 1;
            for (q = 0; q < m; q++) {
                if (read_edge(p, j, index, (size_t)q, &b->edges[q]) != 0) return -1;
                j = tok_skip(p, j, 0);
                if (j < 0) return pfail(p, APR_SESSION_FAULT_NOT_JSON,
                                        wat(w, L"buses[%d].sources", (int)index));
            }
            b->edge_count = (size_t)m;
        } else if (key_is(p, key, "outputs")) {
            int m, j, q;
            WANT(p->t[val].type == JSMN_ARRAY, APR_SESSION_FAULT_BAD_TYPE,
                 wat(w, L"buses[%d].outputs", (int)index));
            m = p->t[val].size;
            if (m > (int)APR_MAX_ACTIONS_PER_BUS)
                return pfail(p, APR_SESSION_FAULT_TOO_MANY,
                             wat(w, L"buses[%d].outputs", (int)index));
            j = val + 1;
            for (q = 0; q < m; q++) {
                if (read_action(p, j, index, (size_t)q, &b->actions[q]) != 0)
                    return -1;
                j = tok_skip(p, j, 0);
                if (j < 0) return pfail(p, APR_SESSION_FAULT_NOT_JSON,
                                        wat(w, L"buses[%d].outputs", (int)index));
            }
            b->action_count = (size_t)m;
        } else {
            wchar_t kw[64];
            (void)tok_wstr(p, key, kw, 64);
            punknown(p, kw);
        }
        i = tok_skip(p, val, 0);
        if (i < 0) return pfail(p, APR_SESSION_FAULT_NOT_JSON,
                                wat(w, L"buses[%d]", (int)index));
    }
    if (b->name[0] == 0)
        _snwprintf_s(b->name, APR_NAME_CCH, _TRUNCATE, L"%d", (int)index + 1);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Public: load
 * ------------------------------------------------------------------------- */

static int link_edges(P *p)
{
    size_t  bi, ei, si;
    wchar_t w[APR_SESSION_WHERE_CCH];

    for (bi = 0; bi < p->s->bus_count; bi++) {
        AprSessionBus *b = &p->s->buses[bi];
        for (ei = 0; ei < b->edge_count; ei++) {
            b->edges[ei].source_index = (size_t)-1;
            for (si = 0; si < p->s->source_count; si++) {
                if (strcmp(b->edges[ei].key, p->s->sources[si].key) == 0) {
                    b->edges[ei].source_index = si;
                    break;
                }
            }
            if (b->edges[ei].source_index == (size_t)-1) {
                wchar_t kw[APR_SESSION_KEY_CCH];
                size_t  c;
                for (c = 0; c + 1 < APR_SESSION_KEY_CCH && b->edges[ei].key[c]; c++)
                    kw[c] = (wchar_t)b->edges[ei].key[c];
                kw[c] = L'\0';
                return pfail(p, APR_SESSION_FAULT_DANGLING_REF,
                             wat(w, L"buses[%d].sources[%d].key \"%ls\"",
                                 (int)bi, (int)ei, kw));
            }
        }
    }
    return 0;
}

AprErr apr_session_load_utf8(const char *json, size_t len, AprSession *out,
                             AprSessionLoadReport *rep)
{
    static jsmntok_t     toks[MAX_TOKENS];
    static AprSessionLoadReport scratch;
    jsmn_parser          jp;
    P                    p;
    int                  r, i, n, k;
    wchar_t              w[APR_SESSION_WHERE_CCH];

    if (!rep) rep = &scratch;
    memset(rep, 0, sizeof *rep);
    if (!json || !out) {
        rep->fault = APR_SESSION_FAULT_FILE_UNREADABLE;
        return APR_ERR(APR_E_INVALID_ARG, L"apr_session_load_utf8");
    }
    apr_session_init(out);
    if (len == 0) len = strlen(json);
    if (len > APR_SESSION_MAX_BYTES) {
        rep->fault = APR_SESSION_FAULT_TOO_LARGE;
        return APR_ERR(APR_E_UNSUPPORTED, L"session file is %llu bytes",
                       (unsigned long long)len);
    }

    jsmn_init(&jp);
    r = jsmn_parse(&jp, json, len, toks, MAX_TOKENS);
    if (r == JSMN_ERROR_INVAL) {
        rep->fault = APR_SESSION_FAULT_NOT_JSON;
        return APR_ERR(APR_E_INVALID_ARG, L"not JSON");
    }
    if (r == JSMN_ERROR_PART || r == 0) {
        /* An empty file lands here too, and "cut off" is the honest reading:
         * it is not corrupt, there is simply nothing in it yet. */
        rep->fault = APR_SESSION_FAULT_TRUNCATED;
        return APR_ERR(APR_E_INVALID_ARG, L"JSON ends early");
    }
    if (r == JSMN_ERROR_NOMEM) {
        rep->fault = APR_SESSION_FAULT_TOO_MANY;
        return APR_ERR(APR_E_NO_MEMORY, L"session file has too many elements");
    }
    if (r < 0 || toks[0].type != JSMN_OBJECT) {
        rep->fault = APR_SESSION_FAULT_NOT_A_SESSION;
        return APR_ERR(APR_E_INVALID_ARG, L"not an apprecorder session");
    }

    p.js = json; p.t = toks; p.ntok = r; p.s = out; p.rep = rep;

    /* PASS 1: the version numbers, before anything else is believed. A file
     * that needs a newer reader must be refused whether or not its "sources"
     * happen to parse, and the header is not required to come first. */
    {
        int found = 0;
        n = toks[0].size;
        i = 1;
        for (k = 0; k < n; k++) {
            int key = i, val;
            if (toks[key].type != JSMN_STRING) {
                rep->fault = APR_SESSION_FAULT_NOT_JSON;
                return APR_ERR(APR_E_INVALID_ARG, L"object member is not a key");
            }
            val = key + 1;
            if (val >= r) {
                rep->fault = APR_SESSION_FAULT_TRUNCATED;
                return APR_ERR(APR_E_INVALID_ARG, L"JSON ends early");
            }
            if (key_is(&p, key, "apprecorder")) {
                if (toks[val].type != JSMN_OBJECT) {
                    rep->fault = APR_SESSION_FAULT_NOT_A_SESSION;
                    return APR_ERR(APR_E_INVALID_ARG, L"apprecorder is not an object");
                }
                if (read_header(&p, val) != 0)
                    return APR_ERR(APR_E_UNSUPPORTED, L"session header");
                found = 1;
            }
            i = tok_skip(&p, val, 0);
            if (i < 0) {
                rep->fault = APR_SESSION_FAULT_NOT_JSON;
                return APR_ERR(APR_E_INVALID_ARG, L"malformed JSON");
            }
        }
        if (!found) {
            rep->fault = APR_SESSION_FAULT_NOT_A_SESSION;
            return APR_ERR(APR_E_INVALID_ARG, L"not an apprecorder session");
        }
        /* The unknown-key count belongs to the document, not to two passes
         * over it; the header pass has just counted its own members once. */
    }

    /* PASS 2: everything else. */
    n = toks[0].size;
    i = 1;
    for (k = 0; k < n; k++) {
        int key = i, val = i + 1;

        if (key_is(&p, key, "apprecorder")) {
            /* Already read. Not counted as unknown. */
        } else if (key_is(&p, key, "session")) {
            if (toks[val].type != JSMN_OBJECT) {
                rep->fault = APR_SESSION_FAULT_BAD_TYPE;
                wcscpy_s(rep->where, APR_SESSION_WHERE_CCH, L"session");
                return APR_ERR(APR_E_INVALID_ARG, L"session is not an object");
            }
            if (read_session_block(&p, val) != 0)
                return APR_ERR(APR_E_INVALID_ARG, L"session block");
        } else if (key_is(&p, key, "sources")) {
            int m, j, q;
            if (toks[val].type != JSMN_ARRAY) {
                rep->fault = APR_SESSION_FAULT_BAD_TYPE;
                wcscpy_s(rep->where, APR_SESSION_WHERE_CCH, L"sources");
                return APR_ERR(APR_E_INVALID_ARG, L"sources is not an array");
            }
            m = toks[val].size;
            if (m > (int)APR_MAX_SOURCES) {
                rep->fault = APR_SESSION_FAULT_TOO_MANY;
                wcscpy_s(rep->where, APR_SESSION_WHERE_CCH, L"sources");
                return APR_ERR(APR_E_UNSUPPORTED, L"%d sources; the maximum is %d",
                               m, (int)APR_MAX_SOURCES);
            }
            j = val + 1;
            for (q = 0; q < m; q++) {
                if (read_source(&p, j, (size_t)q) != 0)
                    return APR_ERR(APR_E_INVALID_ARG, L"source");
                j = tok_skip(&p, j, 0);
                if (j < 0) {
                    rep->fault = APR_SESSION_FAULT_NOT_JSON;
                    return APR_ERR(APR_E_INVALID_ARG, L"malformed JSON");
                }
            }
            out->source_count = (size_t)m;
        } else if (key_is(&p, key, "buses")) {
            int m, j, q;
            if (toks[val].type != JSMN_ARRAY) {
                rep->fault = APR_SESSION_FAULT_BAD_TYPE;
                wcscpy_s(rep->where, APR_SESSION_WHERE_CCH, L"buses");
                return APR_ERR(APR_E_INVALID_ARG, L"buses is not an array");
            }
            m = toks[val].size;
            if (m > (int)APR_MAX_BUSES) {
                rep->fault = APR_SESSION_FAULT_TOO_MANY;
                wcscpy_s(rep->where, APR_SESSION_WHERE_CCH, L"buses");
                return APR_ERR(APR_E_UNSUPPORTED, L"%d buses; the maximum is %d",
                               m, (int)APR_MAX_BUSES);
            }
            j = val + 1;
            for (q = 0; q < m; q++) {
                if (read_bus(&p, j, (size_t)q) != 0)
                    return APR_ERR(APR_E_INVALID_ARG, L"bus");
                j = tok_skip(&p, j, 0);
                if (j < 0) {
                    rep->fault = APR_SESSION_FAULT_NOT_JSON;
                    return APR_ERR(APR_E_INVALID_ARG, L"malformed JSON");
                }
            }
            out->bus_count = (size_t)m;
        } else {
            wchar_t kw[64];
            (void)tok_wstr(&p, key, kw, 64);
            punknown(&p, kw);
        }
        i = tok_skip(&p, val, 0);
        if (i < 0) {
            rep->fault = APR_SESSION_FAULT_NOT_JSON;
            return APR_ERR(APR_E_INVALID_ARG, L"malformed JSON");
        }
    }

    if (link_edges(&p) != 0)
        return APR_ERR(APR_E_NOT_FOUND, L"a bus names a source that is not in the file");

    rep->source_count = out->source_count;
    rep->bus_count    = out->bus_count;
    (void)w;
    return apr_ok();
}

AprErr apr_session_load(const wchar_t *path, AprSession *out,
                        AprSessionLoadReport *rep)
{
    static char  buf[APR_SESSION_MAX_BYTES + 1];
    static AprSessionLoadReport scratch;
    HANDLE       h;
    LARGE_INTEGER size;
    DWORD        got = 0;
    AprErr       e;

    if (!rep) rep = &scratch;
    memset(rep, 0, sizeof *rep);
    if (!path || !path[0] || !out)
        return APR_ERR(APR_E_INVALID_ARG, L"apr_session_load");

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD gle = GetLastError();
        rep->fault = (gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND)
                     ? APR_SESSION_FAULT_FILE_MISSING
                     : APR_SESSION_FAULT_FILE_UNREADABLE;
        return APR_ERR_WIN32(gle, L"open %ls", path);
    }
    if (!GetFileSizeEx(h, &size)) {
        AprErr r = APR_ERR_LAST(L"size of %ls", path);
        CloseHandle(h);
        rep->fault = APR_SESSION_FAULT_FILE_UNREADABLE;
        return r;
    }
    if (size.QuadPart > (LONGLONG)APR_SESSION_MAX_BYTES) {
        CloseHandle(h);
        rep->fault = APR_SESSION_FAULT_TOO_LARGE;
        return APR_ERR(APR_E_UNSUPPORTED, L"%ls is %lld bytes", path,
                       (long long)size.QuadPart);
    }
    if (!ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL)) {
        AprErr r = APR_ERR_LAST(L"read %ls", path);
        CloseHandle(h);
        rep->fault = APR_SESSION_FAULT_FILE_UNREADABLE;
        return r;
    }
    CloseHandle(h);
    buf[got] = '\0';

    /* A UTF-8 BOM is what Notepad and PowerShell's Set-Content leave behind,
     * and it is not JSON. Skipping it is the difference between "your session
     * file is corrupt" and it simply working. */
    {
        const char *start = buf;
        size_t      n = got;
        if (n >= 3 && (unsigned char)buf[0] == 0xEF &&
            (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
            start += 3; n -= 3;
        }
        e = apr_session_load_utf8(start, n, out, rep);
    }
    return e;
}

/* ===========================================================================
 * Resolution
 * ========================================================================= */

int apr_session_status_usable(AprSessionResolveStatus st)
{
    switch (st) {
    case APR_SESSION_EXACT:
    case APR_SESSION_SAME_PROCESS:
    case APR_SESSION_DEVICE_EXACT:
    case APR_SESSION_SYNTHETIC:
    case APR_SESSION_MOVED:
    case APR_SESSION_BY_WINDOW_CLASS:
    case APR_SESSION_FIRST_OF_MANY:
    case APR_SESSION_DEVICE_BY_NAME:
        return 1;
    default:
        return 0;
    }
}

int apr_session_status_substituted(AprSessionResolveStatus st)
{
    switch (st) {
    case APR_SESSION_MOVED:
    case APR_SESSION_BY_WINDOW_CLASS:
    case APR_SESSION_FIRST_OF_MANY:
    case APR_SESSION_DEVICE_BY_NAME:
        return 1;
    default:
        return 0;
    }
}

static int wieq(const wchar_t *a, const wchar_t *b)
{
    if (!a || !b) return 0;
    return _wcsicmp(a, b) == 0;
}

/* ---- process ------------------------------------------------------------ */

#define MAX_CAND 64

typedef struct Cands {
    size_t idx[MAX_CAND];
    size_t n;
    size_t total;      /* true count, even when idx[] filled up */
} Cands;

static void cand_add(Cands *c, size_t i)
{
    c->total++;
    if (c->n < MAX_CAND) c->idx[c->n++] = i;
}

static void fill_candidates(AprSessionResolution *r, const AprSessionMachine *m,
                            const Cands *c)
{
    size_t i;
    r->candidate_count   = c->total;
    r->candidates_listed = 0;
    for (i = 0; i < c->n && i < APR_SESSION_MAX_CANDIDATES; i++) {
        const AprAudioApp *a = &m->apps[c->idx[i]];
        AprSessionCandidate *k = &r->candidates[r->candidates_listed++];
        memset(k, 0, sizeof *k);
        k->pid = a->pid;
        wcscpy_s(k->path, APR_DISC_PATH_CCH, a->path);
        if (m->window_class) {
            const wchar_t *cls = m->window_class(m->user, a->pid);
            if (cls) wcscpy_s(k->window_class, APR_SESSION_CLASS_CCH, cls);
        }
    }
}

static void adopt_app(AprSessionSource *s, const AprAudioApp *a)
{
    s->resolved     = 1;
    s->resolved_pid = a->pid;
    s->muted_now    = a->muted;
    wcscpy_s(s->resolved_label, APR_NAME_CCH,
             a->exe[0] ? a->exe : (a->display[0] ? a->display : s->name));
}

static void resolve_process(AprSessionSource *s,
                            const AprSessionResolveOptions *opt,
                            const AprSessionMachine *m,
                            AprSessionResolution *r)
{
    Cands  c;
    size_t i;
    int    by_name_only = 0;

    wcscpy_s(r->wanted, APR_DISC_PATH_CCH, s->path[0] ? s->path : s->exe);

    memset(&c, 0, sizeof c);

    /* 1. The strong key: the same image, at the same place. */
    if (s->path[0]) {
        for (i = 0; i < m->app_count; i++)
            if (m->apps[i].path[0] && wieq(m->apps[i].path, s->path))
                cand_add(&c, i);
    }

    /* 2. The executable NAME. This is the update / reinstall / moved-drive
     *    case, and it is a substitution rather than a match: we found
     *    something plausibly the same program somewhere else. */
    if (c.total == 0 && s->exe[0]) {
        by_name_only = 1;
        for (i = 0; i < m->app_count; i++)
            if (m->apps[i].exe[0] && wieq(m->apps[i].exe, s->exe))
                cand_add(&c, i);
    }

    if (c.total == 0) {
        /* 3. Last: the stored pid, and ONLY when the process at that number
         *    is still the same executable. A bare pid is worthless -- it is
         *    the thing this whole file exists to work around -- but pid plus
         *    a matching image name is two facts agreeing, and it covers the
         *    case the audio-session list cannot see: an application that is
         *    running but has not made a sound yet, which for a session saved
         *    "for the next meeting" is the normal state of affairs. */
        if (s->pid && s->exe[0] && m->process_exists &&
            m->process_exists(m->user, s->pid) && m->image_name)
        {
            wchar_t leaf[APR_DISC_NAME_CCH];
            leaf[0] = L'\0';
            m->image_name(m->user, s->pid, leaf, APR_DISC_NAME_CCH);
            if (leaf[0] && wieq(leaf, s->exe)) {
                r->status     = APR_SESSION_SAME_PROCESS;
                r->chosen_pid = s->pid;
                s->resolved     = 1;
                s->resolved_pid = s->pid;
                wcscpy_s(s->resolved_label, APR_NAME_CCH, leaf);
                return;
            }
        }
        r->status = APR_SESSION_NOT_RUNNING;
        return;
    }

    fill_candidates(r, m, &c);

    if (c.total > 1) {
        /* 4a. The stored pid as a TIEBREAKER only -- among candidates that
         *     have already matched on image. Reloading a session in the same
         *     boot has nothing to be ambiguous about. */
        if (s->pid) {
            for (i = 0; i < c.n; i++) {
                if (m->apps[c.idx[i]].pid == s->pid) {
                    r->status = APR_SESSION_SAME_PROCESS;
                    /* From the CANDIDATE, never from s->pid, even though they
                     * are equal on this line. Reporting the number we were
                     * looking for rather than the one we found means the
                     * report agrees with itself whatever the search did, which
                     * is exactly the kind of agreement that hides a bug. A
                     * mutation test caught this. */
                    r->chosen_pid = m->apps[c.idx[i]].pid;
                    adopt_app(s, &m->apps[c.idx[i]]);
                    return;
                }
            }
        }

        /* 4b. Window class. This is the only thing that tells two instances
         *     of one executable apart (design 9), and it is why it is stored. */
        if (s->window_class[0] && m->window_class) {
            size_t hit = (size_t)-1, hits = 0;
            for (i = 0; i < c.n; i++) {
                const wchar_t *cls = m->window_class(m->user, m->apps[c.idx[i]].pid);
                if (cls && wieq(cls, s->window_class)) {
                    if (!hits) hit = c.idx[i];
                    hits++;
                }
            }
            if (hits == 1) {
                r->status     = APR_SESSION_BY_WINDOW_CLASS;
                r->chosen_pid = m->apps[hit].pid;
                wcscpy_s(r->substituted, APR_DISC_PATH_CCH, m->apps[hit].path);
                adopt_app(s, &m->apps[hit]);
                return;
            }
        }

        /* 4c. Nothing left to choose by. Refuse, unless the caller has said
         *     that any instance will do -- see session.h on why refusing is
         *     the default. */
        if (!opt->pick_when_ambiguous) {
            r->status = APR_SESSION_AMBIGUOUS;
            return;
        }
        {
            size_t best = c.idx[0];
            for (i = 1; i < c.n; i++)
                if (m->apps[c.idx[i]].pid < m->apps[best].pid) best = c.idx[i];
            r->status     = APR_SESSION_FIRST_OF_MANY;
            r->chosen_pid = m->apps[best].pid;
            wcscpy_s(r->substituted, APR_DISC_PATH_CCH, m->apps[best].path);
            adopt_app(s, &m->apps[best]);
            return;
        }
    }

    /* Exactly one. */
    {
        const AprAudioApp *a = &m->apps[c.idx[0]];
        if (s->pid && a->pid == s->pid)  r->status = APR_SESSION_SAME_PROCESS;
        else if (by_name_only)           r->status = APR_SESSION_MOVED;
        else                             r->status = APR_SESSION_EXACT;

        if (r->status == APR_SESSION_MOVED)
            wcscpy_s(r->substituted, APR_DISC_PATH_CCH,
                     a->path[0] ? a->path : a->exe);
        r->chosen_pid = a->pid;
        adopt_app(s, a);
    }
}

/* ---- device ------------------------------------------------------------- */

static void resolve_device(AprSessionSource *s, const AprSessionMachine *m,
                           AprSessionResolution *r)
{
    size_t i, hit = (size_t)-1, hits = 0;

    /* THE FRIENDLY NAME IS WHAT A PERSON IS TOLD, whichever way this goes.
     * "{0.0.1.00000000}.{7b1c...} was not found" is not actionable; "Chat Mic
     * (TC-Helicon GoXLR) is not connected" is. */
    wcscpy_s(r->wanted, APR_DISC_PATH_CCH,
             s->endpoint_name[0] ? s->endpoint_name : s->endpoint_id);

    for (i = 0; i < m->endpoint_count; i++) {
        if (s->endpoint_id[0] && wieq(m->endpoints[i].id, s->endpoint_id)) {
            r->status = APR_SESSION_DEVICE_EXACT;
            wcscpy_s(s->resolved_endpoint_id, APR_DISC_ENDPOINT_CCH,
                     m->endpoints[i].id);
            wcscpy_s(s->resolved_label, APR_NAME_CCH, m->endpoints[i].name);
            s->resolved = 1;
            return;
        }
    }

    /* The id is a per-installation GUID pair: a different USB port, a driver
     * reinstall or a Windows upgrade mints a new one for the same physical
     * device. The friendly name survives all three. */
    if (s->endpoint_name[0]) {
        for (i = 0; i < m->endpoint_count; i++) {
            if (wieq(m->endpoints[i].name, s->endpoint_name)) {
                if (!hits) hit = i;
                hits++;
            }
        }
    }
    if (hits == 1) {
        r->status = APR_SESSION_DEVICE_BY_NAME;
        wcscpy_s(r->substituted, APR_DISC_PATH_CCH, m->endpoints[hit].id);
        wcscpy_s(s->resolved_endpoint_id, APR_DISC_ENDPOINT_CCH,
                 m->endpoints[hit].id);
        wcscpy_s(s->resolved_label, APR_NAME_CCH, m->endpoints[hit].name);
        s->resolved = 1;
        return;
    }
    if (hits > 1) {
        /* Two identical GoXLRs, or two of anything. Guessing here silently
         * records the wrong microphone. */
        r->status          = APR_SESSION_AMBIGUOUS;
        r->candidate_count = hits;
        return;
    }
    r->status = APR_SESSION_DEVICE_ABSENT;
}

/* ---- the whole session -------------------------------------------------- */

AprErr apr_session_resolve_against(AprSession *s,
                                   const AprSessionResolveOptions *opt,
                                   const AprSessionMachine *m,
                                   AprSessionResolveReport *rep)
{
    AprSessionResolveOptions defaults;
    size_t i;
    size_t hard = 0;

    /* A report is not optional. Resolution without one is exactly the silent
     * failure this module exists to prevent. */
    if (!s || !m || !rep) return APR_ERR(APR_E_INVALID_ARG, L"apr_session_resolve");
    if (!opt) { memset(&defaults, 0, sizeof defaults); opt = &defaults; }

    memset(rep, 0, sizeof *rep);

    for (i = 0; i < s->source_count && i < APR_MAX_SOURCES; i++) {
        AprSessionSource     *src = &s->sources[i];
        AprSessionResolution *r   = &rep->items[rep->count++];

        memset(r, 0, sizeof *r);
        r->source_index = i;
        wcscpy_s(r->name, APR_NAME_CCH, src->name);

        src->resolved     = 0;
        src->resolved_pid = 0;
        src->resolved_endpoint_id[0] = L'\0';
        src->muted_now = 0;
        wcscpy_s(src->resolved_label, APR_NAME_CCH, src->name);

        switch (src->kind) {
        case APR_SESSION_SRC_SYSTEM_MINUS_TREE:
            rep->system_capture_sources++;
            /* CONSENT IS CHECKED BEFORE THE LOOKUP, not after it (design
             * 4.1.1). Whether the excluded application happens to be running
             * has no bearing on whether the user agreed to record everything
             * else on the machine, and asking in that order means the answer
             * is the same every time. */
            if (!opt->allow_system_capture) {
                r->status = APR_SESSION_NEEDS_CONSENT;
                wcscpy_s(r->wanted, APR_DISC_PATH_CCH,
                         src->path[0] ? src->path : src->exe);
                rep->needs_system_capture_consent = 1;
                break;
            }
            resolve_process(src, opt, m, r);
            break;

        case APR_SESSION_SRC_PROCESS:
            resolve_process(src, opt, m, r);
            break;

        case APR_SESSION_SRC_DEVICE:
            resolve_device(src, m, r);
            break;

        case APR_SESSION_SRC_FAKE:
        default:
            r->status   = APR_SESSION_SYNTHETIC;
            src->resolved = 1;
            break;
        }

        if (apr_session_status_usable(r->status)) {
            if (apr_session_status_substituted(r->status)) rep->substituted++;
            else                                           rep->ok++;
        } else {
            rep->failed++;
            /* allow_missing lets an ORDINARY source be dropped: the recording
             * then contains less than was asked for, which is bad but bounded.
             * It must never reach an EXCLUDE source, because dropping the
             * target of an exclusion records MORE -- the whole machine, with
             * nothing held back. Failing safe here means failing loudly. */
            if (src->kind == APR_SESSION_SRC_SYSTEM_MINUS_TREE ||
                !opt->allow_missing)
                hard++;
        }
    }

    if (hard)
        return APR_ERR(APR_E_NOT_FOUND,
                       L"%llu of %llu sources could not be resolved",
                       (unsigned long long)hard,
                       (unsigned long long)s->source_count);
    return apr_ok();
}

/* ---------------------------------------------------------------------------
 * The live machine
 *
 * Enumeration only. Nothing here activates an IAudioClient, which is what
 * lets --session --dry-run mean the same thing --dry-run already means.
 * ------------------------------------------------------------------------- */

#define LIVE_APPS 128
#define LIVE_EPS  64

/* The lookup itself is discover.c's (it has a second caller now, and design 7
 * gives "what is running and what is it" one owner). What stays here is the
 * SHAPE the AprSessionMachine callback wants: a borrowed pointer, or NULL for
 * "no window", rather than a caller's buffer.
 *
 * One static buffer is enough because session.h makes a resolution
 * single-threaded by contract, and the returned pointer is documented as
 * valid only until the next call -- which is exactly how the callback is
 * used: read, compared, and copied before the next candidate is asked. */
static const wchar_t *live_window_class(void *user, uint32_t pid)
{
    static wchar_t out[APR_SESSION_CLASS_CCH];

    (void)user;
    if (apr_process_window_class(pid, out, APR_SESSION_CLASS_CCH) == 0)
        return NULL;
    return out;
}

static int live_process_exists(void *user, uint32_t pid)
{
    (void)user;
    return apr_process_exists(pid);
}

static void live_image_name(void *user, uint32_t pid, wchar_t *buf, size_t cch)
{
    (void)user;
    if (cch) buf[0] = L'\0';
    (void)apr_process_image_name(pid, buf, cch);
}

AprErr apr_session_describe_process(AprSessionSource *s, uint32_t pid)
{
    static AprAudioApp apps[LIVE_APPS];
    size_t n = 0, i;
    const wchar_t *cls;

    if (!s) return APR_ERR(APR_E_INVALID_ARG, L"apr_session_describe_process");

    s->pid = pid;
    (void)apr_enum_audio_apps(apps, LIVE_APPS, &n);
    if (n > LIVE_APPS) n = LIVE_APPS;
    for (i = 0; i < n; i++) {
        if (apps[i].pid != pid) continue;
        if (apps[i].exe[0])  wcscpy_s(s->exe,  APR_DISC_NAME_CCH, apps[i].exe);
        if (apps[i].path[0]) wcscpy_s(s->path, APR_DISC_PATH_CCH, apps[i].path);
        break;
    }
    /* An application at a higher integrity level is not in the audio session
     * list with a readable path, but its image NAME is usually still
     * available -- and the name alone is enough to match on later. */
    if (s->exe[0] == L'\0')
        (void)apr_process_image_name(pid, s->exe, APR_DISC_NAME_CCH);

    /* The one thing that tells two instances of an application apart, and the
     * reason design 9 names it. Absent for a console application or a window
     * that has not been created yet, which is fine: it is a tiebreaker, and a
     * source with one instance never needs it. */
    cls = live_window_class(NULL, pid);
    if (cls && cls[0]) wcscpy_s(s->window_class, APR_SESSION_CLASS_CCH, cls);

    if (s->exe[0] == L'\0' && s->path[0] == L'\0')
        return APR_ERR(APR_E_NOT_FOUND, L"no image name for process %lu",
                       (unsigned long)pid);
    return apr_ok();
}

AprErr apr_session_describe_device(AprSessionSource *s, const wchar_t *endpoint_id)
{
    static AprAudioEndpoint eps[LIVE_EPS];
    size_t n = 0, i;

    if (!s || !endpoint_id)
        return APR_ERR(APR_E_INVALID_ARG, L"apr_session_describe_device");

    wcscpy_s(s->endpoint_id, APR_DISC_ENDPOINT_CCH, endpoint_id);
    (void)apr_enum_capture_endpoints(eps, LIVE_EPS, &n);
    if (n > LIVE_EPS) n = LIVE_EPS;
    for (i = 0; i < n; i++) {
        if (!wieq(eps[i].id, endpoint_id)) continue;
        wcscpy_s(s->endpoint_name, APR_DISC_NAME_CCH, eps[i].name);
        return apr_ok();
    }
    return APR_ERR(APR_E_NOT_FOUND, L"endpoint %ls", endpoint_id);
}

AprErr apr_session_resolve(AprSession *s, const AprSessionResolveOptions *opt,
                           AprSessionResolveReport *rep)
{
    static AprAudioApp      apps[LIVE_APPS];
    static AprAudioEndpoint eps[LIVE_EPS];
    AprSessionMachine m;
    size_t napps = 0, neps = 0, i;
    int    want_apps = 0, want_devices = 0;

    if (!s || !rep) return APR_ERR(APR_E_INVALID_ARG, L"apr_session_resolve");

    for (i = 0; i < s->source_count; i++) {
        switch (s->sources[i].kind) {
        case APR_SESSION_SRC_PROCESS:
        case APR_SESSION_SRC_SYSTEM_MINUS_TREE: want_apps = 1;    break;
        case APR_SESSION_SRC_DEVICE:            want_devices = 1; break;
        default: break;
        }
    }
    if (want_apps)    (void)apr_enum_audio_apps(apps, LIVE_APPS, &napps);
    if (want_devices) (void)apr_enum_capture_endpoints(eps, LIVE_EPS, &neps);
    if (napps > LIVE_APPS) napps = LIVE_APPS;
    if (neps  > LIVE_EPS)  neps  = LIVE_EPS;

    memset(&m, 0, sizeof m);
    m.apps           = apps;
    m.app_count      = napps;
    m.endpoints      = eps;
    m.endpoint_count = neps;
    m.window_class   = live_window_class;
    m.process_exists = live_process_exists;
    m.image_name     = live_image_name;
    m.user           = NULL;

    return apr_session_resolve_against(s, opt, &m, rep);
}
