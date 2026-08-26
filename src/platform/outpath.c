/*
 * outpath.c -- template expansion, the collision policy, and the writability
 * probe. See include/outpath.h for why all three live in one place and why the
 * policy is auto-increment rather than a prompt.
 */
#include "outpath.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

/* How far the collision policy will count before giving up. Four digits is
 * more takes of one name than a person will ever make in a session, and a
 * bound is what stops a wrong answer becoming an infinite loop on a folder
 * that reports every name as existing. */
#define APR_OUT_MAX_TAKES 9999

/* ---------------------------------------------------------------------------
 * Small string plumbing
 * ------------------------------------------------------------------------- */

typedef struct Sink {
    wchar_t *buf;
    size_t   cap;    /* including the terminator */
    size_t   len;
    int      overflow;
} Sink;

static void sink_init(Sink *s, wchar_t *buf, size_t cch)
{
    s->buf = buf;
    s->cap = cch;
    s->len = 0;
    s->overflow = 0;
    if (cch) buf[0] = L'\0';
}

static void sink_char(Sink *s, wchar_t c)
{
    if (s->cap == 0 || s->len + 1 >= s->cap) { s->overflow = 1; return; }
    s->buf[s->len++] = c;
    s->buf[s->len] = L'\0';
}

static void sink_str(Sink *s, const wchar_t *t)
{
    if (!t) return;
    for (; *t; t++) sink_char(s, *t);
}

/* Anything a Windows filename cannot hold, replaced rather than dropped: a bus
 * called "Teams / Zoom" must not silently become a request to create a folder
 * called "Teams ". The path separators are in the list precisely because {bus}
 * expands INSIDE a path the user already chose. */
static void sink_str_sanitized(Sink *s, const wchar_t *t)
{
    static const wchar_t *bad = L"<>:\"/\\|?*";

    if (!t) return;
    for (; *t; t++) {
        wchar_t c = *t;
        if (c < 32 || wcschr(bad, c) != NULL) c = L'-';
        sink_char(s, c);
    }
}

static void sink_u(Sink *s, unsigned v, int width)
{
    wchar_t tmp[16];
    _snwprintf_s(tmp, 16, _TRUNCATE, L"%0*u", width, v);
    sink_str(s, tmp);
}

/* ---------------------------------------------------------------------------
 * Tokens
 * ------------------------------------------------------------------------- */

/* Matches "{name}" at `p`. Returns the length of the whole token, or 0. */
static size_t token_at(const wchar_t *p, const wchar_t *name)
{
    size_t i = 0;

    if (p[0] != L'{') return 0;
    for (i = 0; name[i]; i++) {
        if (towlower(p[1 + i]) != name[i]) return 0;
    }
    if (p[1 + i] != L'}') return 0;
    return i + 2;
}

/* Nonzero when `name` appears as a token anywhere in `tmpl`. Case-insensitive,
 * because token_at is, and a caller asking "does this use {n}" must get the
 * same answer the expander will act on. */
static int has_token(const wchar_t *tmpl, const wchar_t *name)
{
    const wchar_t *p;

    if (!tmpl) return 0;
    for (p = tmpl; *p; p++) {
        if (p[0] == L'{' && p[1] == L'{') { p++; continue; }
        if (token_at(p, name)) return 1;
    }
    return 0;
}

int apr_out_has_tokens(const wchar_t *tmpl)
{
    return has_token(tmpl, L"date") || has_token(tmpl, L"time") ||
           has_token(tmpl, L"bus")  || has_token(tmpl, L"ext")  ||
           has_token(tmpl, L"n");
}

/* One expansion pass. `take` is what {n} becomes. */
static AprErr expand_with(const wchar_t *tmpl, const AprOutContext *ctx,
                          const SYSTEMTIME *lt, unsigned take,
                          wchar_t *out, size_t cch)
{
    Sink s;
    const wchar_t *p;

    if (!tmpl || !out || cch == 0) {
        return APR_ERR(APR_E_INVALID_ARG, L"apr_out_expand: no template or no room");
    }
    sink_init(&s, out, cch);

    for (p = tmpl; *p; ) {
        size_t n;

        /* "{{" is a literal brace, so a template can still name a folder that
         * genuinely has one. */
        if (p[0] == L'{' && p[1] == L'{') { sink_char(&s, L'{'); p += 2; continue; }

        if ((n = token_at(p, L"date")) != 0) {
            sink_u(&s, lt->wYear, 4);  sink_char(&s, L'-');
            sink_u(&s, lt->wMonth, 2); sink_char(&s, L'-');
            sink_u(&s, lt->wDay, 2);
            p += n; continue;
        }
        if ((n = token_at(p, L"time")) != 0) {
            /* Hyphens, not colons: a colon in a Windows filename opens an
             * alternate data stream, which is not a recording. */
            sink_u(&s, lt->wHour, 2);   sink_char(&s, L'-');
            sink_u(&s, lt->wMinute, 2); sink_char(&s, L'-');
            sink_u(&s, lt->wSecond, 2);
            p += n; continue;
        }
        if ((n = token_at(p, L"bus")) != 0) {
            sink_str_sanitized(&s, ctx ? ctx->bus_name : NULL);
            p += n; continue;
        }
        if ((n = token_at(p, L"ext")) != 0) {
            sink_str_sanitized(&s, ctx ? ctx->extension : NULL);
            p += n; continue;
        }
        if ((n = token_at(p, L"n")) != 0) {
            sink_u(&s, take, 1);
            p += n; continue;
        }

        /* Not a token we know. Copy it, brace and all -- see the header. */
        sink_char(&s, *p);
        p++;
    }

    if (s.overflow) {
        return APR_ERR(APR_E_NO_MEMORY, L"the name \"%ls\" expands past %zu characters",
                       tmpl, cch);
    }
    if (s.len == 0) {
        return APR_ERR(APR_E_INVALID_ARG, L"\"%ls\" expands to an empty name", tmpl);
    }
    return apr_ok();
}

AprErr apr_out_expand(const wchar_t *tmpl, const AprOutContext *ctx,
                      wchar_t *out, size_t cch)
{
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    return expand_with(tmpl, ctx, &lt, 1u, out, cch);
}

/* ---------------------------------------------------------------------------
 * The disk
 * ------------------------------------------------------------------------- */

static int exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

AprErr apr_out_probe_writable(const wchar_t *path)
{
    HANDLE h;
    BOOL   created;

    if (!path || !path[0]) {
        return APR_ERR(APR_E_INVALID_ARG, L"apr_out_probe_writable: no path");
    }

    SetLastError(0);
    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return APR_ERR_LAST(L"cannot write %ls", path);
    }

    /* OPEN_ALWAYS, never CREATE_ALWAYS: a file that is already there is opened
     * and left exactly as it was. Only one we had to make ourselves is
     * removed again, which is what makes this safe to run against a name the
     * user may have recorded to yesterday. */
    created = (GetLastError() != ERROR_ALREADY_EXISTS);
    CloseHandle(h);
    if (created) DeleteFileW(path);
    return apr_ok();
}

/* Split "dir\stem.ext" so a number can be inserted before the extension:
 * "mix.wav" -> "mix" + ".wav", "mix" -> "mix" + "". The dot must be in the
 * LAST component, or "C:\v1.2\mix" would be cut in the folder name. */
static void split_extension(const wchar_t *path, size_t *stem_len)
{
    const wchar_t *dot = wcsrchr(path, L'.');
    const wchar_t *sep = wcsrchr(path, L'\\');
    const wchar_t *alt = wcsrchr(path, L'/');

    if (sep && alt && alt > sep) sep = alt;
    if (!sep) sep = alt;

    if (!dot || (sep && dot < sep)) { *stem_len = wcslen(path); return; }
    *stem_len = (size_t)(dot - path);
}

AprErr apr_out_resolve(const wchar_t *tmpl, const AprOutContext *ctx,
                       wchar_t *out, size_t cch, int *out_collided)
{
    SYSTEMTIME lt;
    AprErr     e;
    unsigned   take;

    if (out_collided) *out_collided = 0;
    if (!out || cch == 0) {
        return APR_ERR(APR_E_INVALID_ARG, L"apr_out_resolve: no room");
    }
    GetLocalTime(&lt);

    /* {n} MEANS "THE LOWEST FREE NUMBER", so it is resolved by expanding the
     * whole template again for each candidate rather than by patching a
     * number into a finished string. That is what lets {n} sit anywhere --
     * "take {n} of {bus}.wav" is a name this understands. */
    if (has_token(tmpl, L"n")) {
        for (take = 1; take <= APR_OUT_MAX_TAKES; take++) {
            e = expand_with(tmpl, ctx, &lt, take, out, cch);
            if (apr_failed(&e)) return e;
            if (!exists(out)) return apr_ok();
        }
        return APR_ERR(APR_E_STATE,
                       L"every name from \"%ls\" up to %d is already taken",
                       tmpl, APR_OUT_MAX_TAKES);
    }

    e = expand_with(tmpl, ctx, &lt, 1u, out, cch);
    if (apr_failed(&e)) return e;
    if (!exists(out)) return apr_ok();

    /* THE COLLISION POLICY. The name is taken, so the next free one is used
     * and the previous take survives untouched. See the header for why this
     * is not a prompt -- and note that the caller is TOLD, which is the half
     * that keeps it from being a silent rename. */
    if (out_collided) *out_collided = 1;
    {
        wchar_t base[APR_OUT_PATH_CCH];
        size_t  stem = 0;

        if (wcslen(out) + 1 >= APR_OUT_PATH_CCH) {
            return APR_ERR(APR_E_NO_MEMORY, L"\"%ls\" leaves no room for a take number", out);
        }
        wcscpy_s(base, APR_OUT_PATH_CCH, out);
        split_extension(base, &stem);

        for (take = 2; take <= APR_OUT_MAX_TAKES; take++) {
            int n = _snwprintf_s(out, cch, _TRUNCATE, L"%.*ls-%u%ls",
                                 (int)stem, base, take, base + stem);
            if (n < 0) {
                return APR_ERR(APR_E_NO_MEMORY,
                               L"\"%ls\" leaves no room for a take number", base);
            }
            if (!exists(out)) return apr_ok();
        }
    }
    return APR_ERR(APR_E_STATE, L"every take of \"%ls\" up to %d already exists",
                   tmpl, APR_OUT_MAX_TAKES);
}

AprErr apr_out_validate(const wchar_t *tmpl, const AprOutContext *ctx,
                        wchar_t *out_expanded, size_t cch)
{
    wchar_t local[APR_OUT_PATH_CCH];
    AprErr  e;

    if (out_expanded && cch) out_expanded[0] = L'\0';

    e = apr_out_expand(tmpl, ctx, local, APR_OUT_PATH_CCH);
    if (apr_failed(&e)) return e;
    if (out_expanded && cch) {
        if (wcslen(local) + 1 > cch) {
            return APR_ERR(APR_E_NO_MEMORY, L"\"%ls\" does not fit", local);
        }
        wcscpy_s(out_expanded, cch, local);
    }

    /* THE FILE IS NOT OPENED FOR THE RECORDING HERE. This asks the folder one
     * question -- may something be created in you -- and leaves nothing
     * behind. The recording's own handle is taken at apr_bus_start(), which is
     * the whole point of the lifetime this validation exists to make safe. */
    return apr_out_probe_writable(local);
}

/* ---------------------------------------------------------------------------
 * The default name
 * ------------------------------------------------------------------------- */

void apr_out_default_template(wchar_t *out, size_t cch)
{
    wchar_t profile[APR_OUT_PATH_CCH];
    wchar_t dir[APR_OUT_PATH_CCH];
    DWORD   n;

    if (!out || cch == 0) return;
    out[0] = L'\0';

    /* The environment rather than SHGetKnownFolderPath: the Music folder is
     * where a recorder's output belongs, and reading USERPROFILE costs neither
     * a shell32 dependency nor a COM apartment in a module that is otherwise
     * pure file system. If the folder is not there -- a redirected profile, a
     * service account -- the profile root is used, which always is. */
    n = GetEnvironmentVariableW(L"USERPROFILE", profile, APR_OUT_PATH_CCH);
    if (n == 0 || n >= APR_OUT_PATH_CCH) {
        dir[0] = L'.';
        dir[1] = L'\0';
    } else {
        _snwprintf_s(dir, APR_OUT_PATH_CCH, _TRUNCATE, L"%ls\\Music", profile);
        if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) {
            wcscpy_s(dir, APR_OUT_PATH_CCH, profile);
        }
    }

    /* {bus} {date} {time}.{ext} -- a name that names the recording and, being
     * stamped to the second, cannot collide with the take before it. */
    _snwprintf_s(out, cch, _TRUNCATE, L"%ls\\{bus} {date} {time}.{ext}", dir);
}
