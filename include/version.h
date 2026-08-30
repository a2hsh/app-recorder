/*
 * version.h -- what build this is. ONE OWNER, and the reason is the updater.
 *
 * ===========================================================================
 * WHY THIS IS A MODULE AND NOT A #define IN cli.c
 *
 *   The number used to live in exactly one place -- `APR_CLI_VERSION` in
 *   src/cli/cli.c -- which was fine while the only thing that read it was
 *   `apprecorder version`. It stopped being fine the moment an updater
 *   existed: the updater COMPARES this number against a number a server
 *   published, and decides whether to replace the running image on the
 *   strength of that comparison. A second copy that drifts is then not a
 *   cosmetic defect, it is a build that offers to overwrite itself with
 *   itself, or refuses an update it needs. AGENTS.md rule 3, applied to a
 *   literal.
 *
 *   So: the three numbers below are the definition, APR_VERSION_STRING is
 *   built from them by the preprocessor so the text cannot disagree with the
 *   numbers, and everything else -- the CLI, the About box, the session
 *   writer, the updater -- reads one of those two.
 *
 * ===========================================================================
 * COMPARISON IS NUMERIC, NEVER LEXICOGRAPHIC
 *
 *   `wcscmp(L"0.10.0", L"0.9.0") < 0`, so a string compare says 0.10.0 is
 *   OLDER than 0.9.0 and the tenth release of a line never installs. That is
 *   not a hypothetical: it is the single most common way a home-grown updater
 *   quietly stops updating, and it does it silently, months after anyone
 *   would think to look. apr_version_compare() compares major, then minor,
 *   then patch, as integers. tests/test_version.c pins exactly that case.
 *
 * ===========================================================================
 * WHAT IS ACCEPTED
 *
 *   "0.0.1" and "v0.0.1" -- a leading `v` because that is how a git tag and a
 *   GitHub release are named, and a manifest generated from a tag will carry
 *   whichever spelling the author typed. Everything else is REFUSED rather
 *   than guessed at: an unparsable version in a manifest means the manifest is
 *   not one we understand, and "not understood" must never resolve to "newer".
 */
#ifndef APPRECORDER_VERSION_H
#define APPRECORDER_VERSION_H

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THE DEFINITION. Everything else in this file and in the product is derived
 * from these three integers. */
#define APR_VERSION_MAJOR 0
#define APR_VERSION_MINOR 0
#define APR_VERSION_PATCH 1

/* Stringize-then-widen, in that order: `#x` must run before `L##x` sees it,
 * which is the same two-step src/platform/err.c uses to turn a symbol into a
 * wide literal of its own spelling. Doing it this way is what makes it
 * impossible for APR_VERSION_STRING to say something the integers do not. */
#define APR_VER_STR_(x)  #x
#define APR_VER_STR(x)   APR_VER_STR_(x)
#define APR_VER_WIDE_(x) L##x
#define APR_VER_WIDE(x)  APR_VER_WIDE_(x)
#define APR_VER_W(x)     APR_VER_WIDE(APR_VER_STR(x))

#define APR_VERSION_STRING                                                     \
    APR_VER_W(APR_VERSION_MAJOR) L"."                                          \
    APR_VER_W(APR_VERSION_MINOR) L"."                                          \
    APR_VER_W(APR_VERSION_PATCH)

typedef struct AprVersion {
    int major;
    int minor;
    int patch;
} AprVersion;

/* This build, as three integers. */
AprVersion apr_version_current(void);

/* Parse "MAJOR.MINOR.PATCH", with an optional leading 'v' or 'V'.
 *
 * Returns nonzero on success. On failure `out` is zeroed and the caller must
 * treat the input as unusable -- NOT as 0.0.0, which would compare as older
 * than everything and turn a corrupt manifest into a downgrade offer.
 *
 * Trailing text is refused too: "0.0.1-beta" is not a version this product
 * knows how to order against "0.0.1", so it is not accepted at all. */
int apr_version_parse(const wchar_t *text, AprVersion *out);

/* <0 if a is older than b, 0 if equal, >0 if a is newer. Field by field, as
 * integers. See the note above about why this is not a string compare. */
int apr_version_compare(const AprVersion *a, const AprVersion *b);

/* Nonzero when `candidate` parses AND is strictly newer than this build.
 * The one question the updater actually asks, so that no caller re-derives it
 * and gets the unparsable case wrong. */
int apr_version_is_newer_than_current(const wchar_t *candidate);

#ifdef __cplusplus
}
#endif
#endif /* APPRECORDER_VERSION_H */
