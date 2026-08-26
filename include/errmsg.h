/*
 * errmsg.h -- an AprErr, in the user's language. The ONLY path from a failure
 * to a sentence a person hears.
 *
 * ===========================================================================
 * WHY THIS IS A SEPARATE HEADER FROM err.h
 *
 *   strings.h includes err.h (apr_str_set_language returns an AprErr), so
 *   err.h cannot name AprStrId and the dependency cannot run both ways. This
 *   header sits above both and names both. The IMPLEMENTATION is still in
 *   src/platform/err.c, which AGENTS.md rule 3 names the sole owner of "code
 *   to message" -- this is a declaration's home, not a second owner.
 *
 * ===========================================================================
 * THE SPLIT THIS HEADER EXISTS TO ENFORCE
 *
 *   apr_err_format()  (err.h)   DIAGNOSTIC. English. Context, symbol, numeric
 *                               code, file, line, function. Allocation-free
 *                               and lock-free, so apr_log_err() may call it on
 *                               a capture pump. FOR THE LOG.
 *
 *   apr_err_reason()  (here)    WHAT A USER HEARS. Resolved from the catalog
 *                               in the current display language. No context,
 *                               no file, no line, no function -- those name
 *                               apprecorder's own internals and mean nothing
 *                               to the person whose recording just stopped.
 *                               Locks and may allocate: UI or worker threads
 *                               only, exactly like apr_str().
 *
 *   The frame is the operation, the reason is why. "Could not open %1!s! for
 *   writing" already names what was being attempted, in the catalog, so the
 *   reason does not have to -- which is what makes dropping the raise site's
 *   English context a simplification rather than a loss.
 *
 * ===========================================================================
 * WHERE THE WORDS COME FROM, IN ORDER
 *
 *   1. e->reason, when the raise site named a sentence (APR_ERR_SAY).
 *      Fully translated. This is how a refusal that only the raising code
 *      understands still becomes the right Arabic.
 *   2. A hand-tabled WASAPI code -> its APR_S_ERR_HR_* entry. Fully
 *      translated. Windows ships no message resource for facility 0x889, so
 *      these sentences are apprecorder's own and belong in the catalog.
 *   3. FormatMessageW, ASKED FOR THE DISPLAY LANGUAGE. Kept, on purpose:
 *      Windows genuinely localizes thousands of ordinary HRESULT and Win32
 *      codes -- access denied, disk full, sharing violation, the ones a user
 *      actually hits -- and that space is open-ended, so tabling it is not a
 *      thing anyone can finish. The one change from err.c's diagnostic call
 *      is the language: err.c asks for LANG_NEUTRAL (the THREAD's language),
 *      which would hand English text to a user running --lang ar-SA.
 *   4. The kind's own APR_S_ERR_REASON_* sentence. Fully translated, and the
 *      floor: nothing falls through to a literal in C. When step 3 could not
 *      produce text in the display language, the numeric code survives as an
 *      insert instead of being swallowed.
 *
 *   Steps 1, 2 and 4 are catalog. Step 3 is Windows' own catalog. No step is
 *   a literal in apprecorder's source, and that is the property M11 was about.
 */
#ifndef APPRECORDER_ERRMSG_H
#define APPRECORDER_ERRMSG_H

#include "err.h"
#include "strings.h"

/* The catalog id for the sentence describing `e`, following steps 1, 2 and 4
 * above. NEVER returns APR_S__NONE for a failure: an error with nothing
 * better resolves to its kind's sentence, and an unknown kind to
 * APR_S_ERR_REASON_UNKNOWN. Returns APR_S__NONE only for NULL or APR_OK.
 *
 * Step 3 is deliberately NOT visible here: a system message has no catalog id.
 * Use apr_err_reason() to get text; this entry point exists so that tests can
 * assert every reachable failure has a declared id, and so that a caller with
 * its own sentence-building can name one.
 *
 * Pure, allocation-free and thread-safe: it reads `e` and a static table. */
AprStrId apr_err_reason_id(const AprErr *e);

/* The reason clause for `e`, in the CURRENT display language, into
 * caller-owned `buf` (cch >= 1). Returns the number of characters written,
 * excluding the terminator; always NUL-terminates when cch >= 1.
 *
 * A success value writes an empty string and returns 0 -- a caller must not
 * be able to accidentally report "no error" as a reason.
 *
 * TAKES A LOCK AND MAY ALLOCATE (apr_str). Never call it from on_audio or a
 * capture pump. Build the AprErr there; resolve it here. */
size_t apr_err_reason(const AprErr *e, wchar_t *buf, size_t cch);

/* ---------------------------------------------------------------------------
 * Introspection -- how the completeness check sees the WASAPI table.
 *
 * The table is src/platform/err.c's, and it carries English text for the log
 * AS WELL AS a catalog id for the user. Two spellings of the same sentence is
 * a drift hazard, so it is pinned rather than trusted: tests/test_err.c walks
 * this and asserts the English catalog entry equals the table's text, and
 * tests/test_strings.c asserts every entry names a declared id.
 * ------------------------------------------------------------------------- */

typedef struct AprErrHrEntry {
    HRESULT        hr;
    const wchar_t *name;   /* "AUDCLNT_E_DEVICE_INVALIDATED" */
    const wchar_t *text;   /* English, for the log */
    AprStrId       reason; /* the catalog entry a user hears */
} AprErrHrEntry;

int                  apr_err_hr_table_count(void);
const AprErrHrEntry *apr_err_hr_table_at(int index);  /* NULL when out of range */

#endif /* APPRECORDER_ERRMSG_H */
