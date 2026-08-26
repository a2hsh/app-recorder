/*
 * session_save.c -- the hand-rolled half of design section 9.
 *
 * The writer is ours and the reader is jsmn's, and that split is not laziness
 * in either direction. Emitting JSON is a bounded append with four characters
 * to escape; a dependency there would buy nothing and cost a vendored library
 * in the shipped binary. PARSING is where the sharp edges are -- nesting,
 * partial input, hostile input -- which is exactly what jsmn is for.
 *
 * THREE THINGS IN HERE ARE LOAD-BEARING.
 *
 * 1. THE BUFFER CANNOT OVERFLOW AND CANNOT LIE ABOUT IT.
 *
 *    Every append goes through buf_put, which either writes or sets `over`
 *    and writes nothing. Nothing checks a return value at the call site --
 *    dozens of unchecked appends is how a writer ends up emitting half a
 *    document -- and the single check happens once at the end. A truncated
 *    session file that still parses would be the worst possible outcome here:
 *    it would load, and it would be missing a bus.
 *
 * 2. NO FLOAT IS EVER FORMATTED.
 *
 *    Gain is stored in tenths of a dB and printed as "%ld.%ld"; amplitude is
 *    scaled to an integer millionth and printed the same way. So the text is
 *    exact, the round trip is exact, and neither the CRT locale nor a decimal
 *    comma can get into a file that a script parses. This is the same rule
 *    apr_str_number_fixed follows for display, for the same reason.
 *
 * 3. THE SAVE IS ATOMIC.
 *
 *    Write a sibling temporary, then MoveFileExW over the target. A session
 *    file is a configuration the author will have spent real time on, and an
 *    interrupted save that leaves a half-written file where a whole one used
 *    to be destroys it. Replacing in one operation means the old file survives
 *    every failure up to the rename.
 *
 * No user-facing string is built here (AGENTS.md rule 6). This file produces a
 * wire format -- ASCII field names a script matches on -- and reports failure
 * as an AprErr for the front end to localize.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "session.h"

/* ---------------------------------------------------------------------------
 * Bounded append
 * ------------------------------------------------------------------------- */

typedef struct Buf {
    char  *p;
    size_t cap;
    size_t len;
    int    over;
} Buf;

static void buf_put(Buf *b, const char *s, size_t n)
{
    if (b->over) return;
    if (b->len + n + 1 > b->cap) { b->over = 1; return; }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void buf_puts(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }

static void buf_i64(Buf *b, int64_t v)
{
    char t[24];
    int  n = sprintf_s(t, sizeof t, "%lld", (long long)v);
    if (n > 0) buf_put(b, t, (size_t)n);
}

/* `scaled` / 10^decimals, exactly, with no float anywhere in the path. */
static void buf_fixed(Buf *b, int64_t scaled, int decimals)
{
    char     t[48];
    int64_t  div = 1, whole, frac;
    int      i, n;

    for (i = 0; i < decimals; i++) div *= 10;
    whole = scaled / div;
    frac  = scaled % div;
    if (frac < 0) frac = -frac;

    /* A negative value between -1 and 0 has a whole part of 0, and "0.5"
     * would silently drop the sign. */
    n = sprintf_s(t, sizeof t, "%s%lld.%0*lld",
                  (scaled < 0 && whole == 0) ? "-" : "",
                  (long long)whole, decimals, (long long)frac);
    if (n > 0) buf_put(b, t, (size_t)n);
}

static void buf_indent(Buf *b, int level)
{
    int i;
    for (i = 0; i < level; i++) buf_puts(b, "  ");
}

/* ---------------------------------------------------------------------------
 * Strings
 *
 * Wide text becomes UTF-8 first and is escaped byte-wise afterwards. That
 * order matters: every byte an escape can apply to is below 0x80, and every
 * byte of a multi-byte UTF-8 sequence is at or above it, so the escaper cannot
 * cut a character in half no matter what is in the string.
 * ------------------------------------------------------------------------- */

static void buf_json_string(Buf *b, const wchar_t *w)
{
    char   u8[1024];
    int    n, i;

    buf_puts(b, "\"");
    if (!w || !*w) { buf_puts(b, "\""); return; }

    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, (int)sizeof u8, NULL, NULL);
    if (n <= 0) {
        /* Cannot happen for anything this product stores -- every field is a
         * bounded array shorter than u8 -- but a writer that silently emitted
         * nothing here would produce a file claiming an empty path. */
        b->over = 1;
        buf_puts(b, "\"");
        return;
    }
    n--;   /* drop the terminator */

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)u8[i];
        switch (c) {
        case '"':  buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\b': buf_puts(b, "\\b");  break;
        case '\f': buf_puts(b, "\\f");  break;
        case '\n': buf_puts(b, "\\n");  break;
        case '\r': buf_puts(b, "\\r");  break;
        case '\t': buf_puts(b, "\\t");  break;
        default:
            if (c < 0x20) {
                char esc[8];
                int  k = sprintf_s(esc, sizeof esc, "\\u%04x", (unsigned)c);
                if (k > 0) buf_put(b, esc, (size_t)k);
            } else {
                buf_put(b, (const char *)&c, 1);
            }
            break;
        }
    }
    buf_puts(b, "\"");
}

/* An ASCII key straight from the model (a source key, an action id). Escaped
 * through the same path so that a hand-edited file with a quote in a key
 * cannot produce a document we would then refuse to read back. */
static void buf_json_ascii(Buf *b, const char *s)
{
    wchar_t w[APR_SESSION_KEY_CCH * 2];
    size_t  i;
    for (i = 0; i + 1 < sizeof w / sizeof w[0] && s[i]; i++)
        w[i] = (wchar_t)(unsigned char)s[i];
    w[i] = L'\0';
    buf_json_string(b, w);
}

static void kv_str(Buf *b, int level, const char *key, const wchar_t *val,
                   int comma)
{
    buf_indent(b, level);
    buf_puts(b, "\""); buf_puts(b, key); buf_puts(b, "\": ");
    buf_json_string(b, val);
    buf_puts(b, comma ? ",\n" : "\n");
}

static void kv_ascii(Buf *b, int level, const char *key, const char *val,
                     int comma)
{
    buf_indent(b, level);
    buf_puts(b, "\""); buf_puts(b, key); buf_puts(b, "\": ");
    buf_json_ascii(b, val);
    buf_puts(b, comma ? ",\n" : "\n");
}

static void kv_i64(Buf *b, int level, const char *key, int64_t val, int comma)
{
    buf_indent(b, level);
    buf_puts(b, "\""); buf_puts(b, key); buf_puts(b, "\": ");
    buf_i64(b, val);
    buf_puts(b, comma ? ",\n" : "\n");
}

static void kv_fixed(Buf *b, int level, const char *key, int64_t scaled,
                     int decimals, int comma)
{
    buf_indent(b, level);
    buf_puts(b, "\""); buf_puts(b, key); buf_puts(b, "\": ");
    buf_fixed(b, scaled, decimals);
    buf_puts(b, comma ? ",\n" : "\n");
}

/* ---------------------------------------------------------------------------
 * The model
 * ------------------------------------------------------------------------- */

const char *apr_session_kind_wire(AprSessionSrcKind k)
{
    switch (k) {
    case APR_SESSION_SRC_SYSTEM_MINUS_TREE: return "systemMinusTree";
    case APR_SESSION_SRC_DEVICE:            return "device";
    case APR_SESSION_SRC_FAKE:              return "fake";
    case APR_SESSION_SRC_PROCESS:
    default:                                return "process";
    }
}

void apr_session_init(AprSession *s)
{
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->sample_rate = 48000;
    s->channels    = 2;
}

/* ---------------------------------------------------------------------------
 * The writer
 * ------------------------------------------------------------------------- */

static void write_source(Buf *b, const AprSessionSource *s, int last)
{
    buf_indent(b, 2); buf_puts(b, "{\n");
    kv_ascii(b, 3, "key",  s->key, 1);
    kv_ascii(b, 3, "kind", apr_session_kind_wire(s->kind), 1);

    switch (s->kind) {
    case APR_SESSION_SRC_PROCESS:
    case APR_SESSION_SRC_SYSTEM_MINUS_TREE:
        kv_str(b, 3, "name", s->name, 1);
        /* Written for the same-boot case and never sufficient on its own.
         * session_load.c uses it only to break a tie among candidates that
         * already matched on image path. */
        kv_i64(b, 3, "pid", (int64_t)s->pid, 1);
        kv_str(b, 3, "exe", s->exe, 1);
        kv_str(b, 3, "path", s->path, 1);
        kv_str(b, 3, "windowClass", s->window_class, 0);
        break;

    case APR_SESSION_SRC_DEVICE:
        kv_str(b, 3, "name", s->name, 1);
        /* BOTH halves, always. The id is what the capture layer opens; the
         * name is the only half a person can be told about when the device
         * turns out not to be plugged in. */
        kv_str(b, 3, "endpointId", s->endpoint_id, 1);
        kv_str(b, 3, "endpointName", s->endpoint_name, 0);
        break;

    case APR_SESSION_SRC_FAKE:
    default: {
        /* The five health fields are written only when the source actually
         * has any, so a healthy synthetic source produces exactly the file it
         * produced before they existed. All-zero IS healthy (capture.h), so
         * one test covers all five. */
        int health = s->fake_mute_at || s->fake_unmute_at || s->fake_die_at ||
                     s->fake_start_muted || s->fake_start_dead;

        kv_str(b, 3, "name", s->name, 1);
        kv_i64(b, 3, "toneHz", (int64_t)s->fake_hz, 1);
        kv_i64(b, 3, "ratePpm", (int64_t)s->fake_ppm, 1);
        kv_fixed(b, 3, "amplitude",
                 (int64_t)((double)s->fake_amp * 1000000.0 +
                           (s->fake_amp < 0 ? -0.5 : 0.5)), 6, health);
        if (health) {
            kv_i64(b, 3, "muteAtFrame",   (int64_t)s->fake_mute_at, 1);
            kv_i64(b, 3, "unmuteAtFrame", (int64_t)s->fake_unmute_at, 1);
            kv_i64(b, 3, "dieAtFrame",    (int64_t)s->fake_die_at, 1);
            kv_i64(b, 3, "startMuted",    s->fake_start_muted ? 1 : 0, 1);
            kv_i64(b, 3, "startDead",     s->fake_start_dead ? 1 : 0, 0);
        }
        break;
    }
    }

    buf_indent(b, 2); buf_puts(b, last ? "}\n" : "},\n");
}

static void write_bus(Buf *b, const AprSessionBus *bus, int last)
{
    size_t i;

    buf_indent(b, 2); buf_puts(b, "{\n");
    kv_str(b, 3, "name", bus->name, 1);

    buf_indent(b, 3); buf_puts(b, "\"sources\": [\n");
    for (i = 0; i < bus->edge_count; i++) {
        buf_indent(b, 4); buf_puts(b, "{ ");
        buf_puts(b, "\"key\": ");    buf_json_ascii(b, bus->edges[i].key);
        buf_puts(b, ", \"gainDb\": ");
        buf_fixed(b, bus->edges[i].gain_db_tenths, 1);
        buf_puts(b, i + 1 < bus->edge_count ? " },\n" : " }\n");
    }
    buf_indent(b, 3); buf_puts(b, "],\n");

    buf_indent(b, 3); buf_puts(b, "\"outputs\": [\n");
    for (i = 0; i < bus->action_count; i++) {
        buf_indent(b, 4); buf_puts(b, "{\n");
        /* The REGISTRY ID, not the file extension. They coincide today and
         * are allowed to diverge. */
        kv_ascii(b, 5, "format", bus->actions[i].id, 1);
        kv_str(b, 5, "path", bus->actions[i].path, 1);
        kv_i64(b, 5, "bitrateKbps", bus->actions[i].bitrate_kbps, 1);
        kv_i64(b, 5, "quality", bus->actions[i].quality, 0);
        buf_indent(b, 4);
        buf_puts(b, i + 1 < bus->action_count ? "},\n" : "}\n");
    }
    buf_indent(b, 3); buf_puts(b, "]\n");

    buf_indent(b, 2); buf_puts(b, last ? "}\n" : "},\n");
}

AprErr apr_session_write_utf8(const AprSession *s, char *buf, size_t cap,
                              size_t *out_len)
{
    Buf    b;
    size_t i;

    if (out_len) *out_len = 0;
    if (!s || !buf || cap == 0)
        return APR_ERR(APR_E_INVALID_ARG, L"apr_session_write_utf8");

    b.p = buf; b.cap = cap; b.len = 0; b.over = 0;
    buf[0] = '\0';

    buf_puts(&b, "{\n");

    /* The two version numbers. See session.h: one says what wrote the file,
     * the other says what is needed to read it honestly. */
    buf_indent(&b, 1); buf_puts(&b, "\"apprecorder\": {\n");
    kv_i64(&b, 2, "version", APR_SESSION_FORMAT_VERSION, 1);
    kv_i64(&b, 2, "minReader", APR_SESSION_MIN_READER, 1);
    kv_ascii(&b, 2, "writer", "apprecorder", 0);
    buf_indent(&b, 1); buf_puts(&b, "},\n");

    buf_indent(&b, 1); buf_puts(&b, "\"session\": {\n");
    kv_i64(&b, 2, "sampleRate", (int64_t)s->sample_rate, 1);
    kv_i64(&b, 2, "channels", (int64_t)s->channels, 1);
    kv_i64(&b, 2, "durationMs", s->duration_ms, 0);
    buf_indent(&b, 1); buf_puts(&b, "},\n");

    /* SOURCES ARE TOP LEVEL AND BUSES REFERENCE THEM BY KEY. A format that
     * nested sources inside buses could not express one source feeding two
     * buses at two different gains, which is the shape this whole product
     * exists for (design 3.2). */
    buf_indent(&b, 1); buf_puts(&b, "\"sources\": [\n");
    for (i = 0; i < s->source_count; i++)
        write_source(&b, &s->sources[i], i + 1 == s->source_count);
    buf_indent(&b, 1); buf_puts(&b, "],\n");

    buf_indent(&b, 1); buf_puts(&b, "\"buses\": [\n");
    for (i = 0; i < s->bus_count; i++)
        write_bus(&b, &s->buses[i], i + 1 == s->bus_count);
    buf_indent(&b, 1); buf_puts(&b, "]\n");

    buf_puts(&b, "}\n");

    /* One check, at the end, for every append above. */
    if (b.over) {
        buf[0] = '\0';
        return APR_ERR(APR_E_NO_MEMORY, L"session needs more than %llu bytes",
                       (unsigned long long)cap);
    }
    if (out_len) *out_len = b.len;
    return apr_ok();
}

AprErr apr_session_save(const AprSession *s, const wchar_t *path)
{
    static char buf[APR_SESSION_MAX_BYTES];
    wchar_t     tmp[APR_DISC_PATH_CCH + 8];
    size_t      len = 0;
    AprErr      e;
    HANDLE      h;
    DWORD       wrote = 0;

    if (!s || !path || !path[0])
        return APR_ERR(APR_E_INVALID_ARG, L"apr_session_save");

    e = apr_session_write_utf8(s, buf, sizeof buf, &len);
    if (apr_failed(&e)) return e;

    /* Sibling temporary, then replace in one operation. Everything up to the
     * rename leaves the previous file exactly as it was. */
    _snwprintf_s(tmp, APR_DISC_PATH_CCH + 8, _TRUNCATE, L"%ls.tmp", path);

    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return APR_ERR_LAST(L"create %ls", tmp);

    if (!WriteFile(h, buf, (DWORD)len, &wrote, NULL) || wrote != (DWORD)len) {
        AprErr w = APR_ERR_LAST(L"write %ls", tmp);
        CloseHandle(h);
        DeleteFileW(tmp);
        return w;
    }
    /* Flush before the rename, so a power loss cannot leave the directory
     * entry pointing at a file whose contents never reached the disk. */
    FlushFileBuffers(h);
    CloseHandle(h);

    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        AprErr w = APR_ERR_LAST(L"replace %ls", path);
        DeleteFileW(tmp);
        return w;
    }
    return apr_ok();
}
