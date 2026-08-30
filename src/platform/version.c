/*
 * version.c -- the three integers, parsed and ordered. See include/version.h.
 *
 * Deliberately free of every dependency: no windows.h, no catalog, no log. It
 * is called from the updater's worker thread, from the CLI, and from the About
 * box, and a comparison that could allocate or lock would be one more thing to
 * reason about on the path that decides whether to overwrite the running exe.
 */
#include "version.h"

#include <string.h>

AprVersion apr_version_current(void)
{
    AprVersion v;
    v.major = APR_VERSION_MAJOR;
    v.minor = APR_VERSION_MINOR;
    v.patch = APR_VERSION_PATCH;
    return v;
}

/* One dotted field. Advances *p past the digits. Returns 0 when there are no
 * digits at all, or when the field would overflow -- a version with a
 * ten-digit component is not a version, it is someone probing the parser. */
static int take_field(const wchar_t **p, int *out)
{
    const wchar_t *s = *p;
    long           v = 0;
    int            digits = 0;

    while (*s >= L'0' && *s <= L'9') {
        v = v * 10 + (*s - L'0');
        if (v > 1000000L) return 0;
        digits++;
        s++;
    }
    if (digits == 0) return 0;
    *out = (int)v;
    *p = s;
    return 1;
}

int apr_version_parse(const wchar_t *text, AprVersion *out)
{
    AprVersion     v;
    const wchar_t *p = text;

    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!text) return 0;

    /* A git tag is `v0.0.1` and a release name is often the same. Accepting
     * the prefix is not laxity -- refusing it would mean a manifest generated
     * straight from a tag never compares as newer, which fails silently. */
    if (*p == L'v' || *p == L'V') p++;

    if (!take_field(&p, &v.major)) return 0;
    if (*p != L'.') return 0;
    p++;
    if (!take_field(&p, &v.minor)) return 0;
    if (*p != L'.') return 0;
    p++;
    if (!take_field(&p, &v.patch)) return 0;

    /* NOTHING MAY FOLLOW. "0.0.1-rc2" is a version this product cannot order
     * against "0.0.1", so it is refused rather than silently truncated to the
     * release it is not. */
    if (*p != L'\0') return 0;

    *out = v;
    return 1;
}

int apr_version_compare(const AprVersion *a, const AprVersion *b)
{
    if (!a || !b) return 0;
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    return 0;
}

int apr_version_is_newer_than_current(const wchar_t *candidate)
{
    AprVersion them;
    AprVersion us = apr_version_current();

    /* UNPARSABLE IS NOT NEWER. The whole point of returning 0 here rather
     * than letting a caller compare a zeroed struct is that 0.0.0 would
     * compare as older than everything, which is the wrong wrong-answer: it
     * turns a corrupt manifest into "you are up to date" instead of into a
     * refusal the log records. */
    if (!apr_version_parse(candidate, &them)) return 0;
    return apr_version_compare(&them, &us) > 0;
}
