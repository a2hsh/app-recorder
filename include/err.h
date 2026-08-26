/*
 * err.h -- apprecorder's single error type and the only HRESULT-to-text path.
 *
 * Design section 7 names platform/err.c the sole owner of "HRESULT to message".
 * Nothing else in the tree may call FormatMessage, keep its own error struct,
 * or hand-roll a WASAPI error table. If something is missing here, extend this
 * file.
 *
 * SHAPE
 *
 *   AprErr is a plain value: no pointers into heap, no ownership, no cleanup.
 *   Copy it, return it, store it in a Source. Everything it points at
 *   (`file`, `func`) is a string literal with static lifetime supplied by the
 *   compiler, and the human context is an inline fixed array.
 *
 *   Consequence that matters: constructing and formatting an AprErr allocates
 *   nothing and takes no lock, so a capture callback can report a failure
 *   without leaving the real-time path. (Emitting it to a log still goes
 *   through log.h, which has its own rules.)
 *
 * ENGLISH IN HERE, TRANSLATED AT THE POINT OF DISPLAY -- READ THIS BEFORE
 * WRITING A MESSAGE
 *
 *   `context` is DIAGNOSTIC. It is written at the raise site, in English, it
 *   names functions, thresholds and internal ids, and it exists for the log.
 *   It MUST NOT reach a user: AGENTS.md rule 6 puts text a screen reader
 *   reads in the catalog, and a raise site on a capture pump cannot put it
 *   there -- apr_str() locks and may allocate.
 *
 *   So an error travels as an IDENTITY and becomes words only where it is
 *   displayed. The identity is (`kind`, `code`) plus, where those are too
 *   coarse to answer the question the user is actually asking, `reason`: the
 *   catalog id of the sentence a person should hear. Setting it is one
 *   integer store -- still allocation-free, still lock-free, still safe on an
 *   audio thread.
 *
 *   include/errmsg.h turns that identity into the user's language, and
 *   nothing else may. apr_err_format() below stays exactly what it is:
 *   English, allocation-free, callable anywhere, and FOR THE LOG ONLY.
 *
 * CONVENTION
 *
 *   Functions that can fail return AprErr by value and take out-parameters for
 *   their results. `apr_failed(&e)` is the test. Functions that cannot fail
 *   return their result directly -- do not launder success through AprErr for
 *   symmetry.
 */
#ifndef APPRECORDER_ERR_H
#define APPRECORDER_ERR_H

#include <windows.h>
#include <stddef.h>

/* Characters (including the terminator) available for the human context of one
 * error. Fixed so AprErr stays copyable and allocation-free. */
#define APR_ERR_CONTEXT_CCH 160

typedef enum AprErrKind {
    APR_OK = 0,          /* not a failure */
    APR_E_HRESULT,       /* `code` is an HRESULT from COM/WASAPI/MF */
    APR_E_WIN32,         /* `code` is a GetLastError() value */
    APR_E_ERRNO,         /* `code` is a C runtime errno */
    APR_E_INVALID_ARG,   /* caller passed something impossible */
    APR_E_NO_MEMORY,     /* allocation failed */
    APR_E_STATE,         /* right call, wrong moment */
    APR_E_NOT_FOUND,     /* named device / process / key does not exist */
    APR_E_UNSUPPORTED,   /* well-formed request this build cannot serve */
    APR_E_TIMEOUT,       /* waited long enough */
    APR_E_OVERRUN,       /* a consumer fell behind and data was dropped */
    APR_E_IO,            /* file or stream failure with no better code */

    /* Right call, wrong moment, AND THE MOMENT WILL PASS. Distinct from
     * APR_E_STATE because a caller answers it differently: APR_E_STATE is a
     * programming error to report, APR_E_BUSY is "not while a recording is
     * running", which has its own sentence in the catalog and is the honest
     * answer to a keystroke rather than a failure. Appended at the end of the
     * enum on purpose -- every value above it is unchanged. */
    APR_E_BUSY
} AprErrKind;

typedef struct AprErr {
    AprErrKind  kind;
    long        code;                        /* HRESULT / DWORD / errno, else 0 */

    /* The catalog id of the sentence a USER should hear for this failure, or
     * 0 when (kind, code) already carries everything a user can act on.
     *
     * Typed `int` and not `AprStrId` deliberately: strings.h includes THIS
     * header, so the dependency cannot run both ways. errmsg.h has the typed
     * accessor and is the only thing that should read this field. */
    int         reason;

    const char *func;                        /* static; __func__ at the raise site */
    const char *file;                        /* static; __FILE__ at the raise site */
    int         line;
    wchar_t     context[APR_ERR_CONTEXT_CCH];/* human "what were we doing";
                                              * DIAGNOSTIC, English, log only */
} AprErr;

/* ---------------------------------------------------------------------------
 * Construction
 *
 * Prefer the macros: they capture the raise site. `apr_err_make` is public only
 * so wrappers can forward a va_list-free call; it never fails and never
 * allocates. A context longer than APR_ERR_CONTEXT_CCH-1 is truncated, always
 * NUL-terminated. Thread-safe (touches nothing shared).
 * ------------------------------------------------------------------------- */

AprErr apr_err_make(AprErrKind kind, long code,
                    const char *func, const char *file, int line,
                    _In_z_ _Printf_format_string_ const wchar_t *fmt, ...);

/* As apr_err_make, and additionally names the catalog sentence a user should
 * hear (`reason`, an AprStrId value; 0 for none). Identical guarantees: never
 * fails, never allocates, takes no lock, safe on a capture thread. */
AprErr apr_err_make_r(AprErrKind kind, long code, int reason,
                      const char *func, const char *file, int line,
                      _In_z_ _Printf_format_string_ const wchar_t *fmt, ...);

/* A success value. Thread-safe, allocation-free. */
AprErr apr_ok(void);

/* Nonzero when `e` describes a failure. `e` must not be NULL. */
int apr_failed(const AprErr *e);

/* Raise with an explicit kind that carries no numeric code. */
#define APR_ERR(kind, ...) \
    apr_err_make((kind), 0, __func__, __FILE__, __LINE__, __VA_ARGS__)

/* Raise from an HRESULT (COM, WASAPI, Media Foundation). */
#define APR_ERR_HR(hr, ...) \
    apr_err_make(APR_E_HRESULT, (long)(hr), __func__, __FILE__, __LINE__, __VA_ARGS__)

/* Raise from an explicit Win32 status. */
#define APR_ERR_WIN32(dw, ...) \
    apr_err_make(APR_E_WIN32, (long)(dw), __func__, __FILE__, __LINE__, __VA_ARGS__)

/* Raise from GetLastError(). Snapshot it first: the format arguments below are
 * evaluated after the read, so nothing can clobber it in between. */
#define APR_ERR_LAST(...) \
    apr_err_make(APR_E_WIN32, (long)GetLastError(), __func__, __FILE__, __LINE__, __VA_ARGS__)

/* Raise from a C runtime errno. */
#define APR_ERR_ERRNO(en, ...) \
    apr_err_make(APR_E_ERRNO, (long)(en), __func__, __FILE__, __LINE__, __VA_ARGS__)

/* ---------------------------------------------------------------------------
 * ...and the same raises, naming the sentence a USER hears.
 *
 * `reason` is an APR_S_* id from strings.h. Reach for these ONLY where a user
 * is the audience AND (kind, code) is too coarse to answer the question they
 * are asking. "That bus already holds as many sources as it can mix" is worth
 * saying; "graph.c was handed a null pointer" is not, and its kind already
 * covers it. The format string stays English diagnostic text for the log
 * either way -- these macros add a sentence, they do not translate one.
 * ------------------------------------------------------------------------- */

#define APR_ERR_SAY(kind, reason, ...) \
    apr_err_make_r((kind), 0, (int)(reason), __func__, __FILE__, __LINE__, \
                   __VA_ARGS__)

#define APR_ERR_HR_SAY(hr, reason, ...) \
    apr_err_make_r(APR_E_HRESULT, (long)(hr), (int)(reason), \
                   __func__, __FILE__, __LINE__, __VA_ARGS__)

#define APR_ERR_WIN32_SAY(dw, reason, ...) \
    apr_err_make_r(APR_E_WIN32, (long)(dw), (int)(reason), \
                   __func__, __FILE__, __LINE__, __VA_ARGS__)

/* Snapshot GetLastError() first, exactly as APR_ERR_LAST does. */
#define APR_ERR_LAST_SAY(reason, ...) \
    apr_err_make_r(APR_E_WIN32, (long)GetLastError(), (int)(reason), \
                   __func__, __FILE__, __LINE__, __VA_ARGS__)

/* ---------------------------------------------------------------------------
 * Rendering
 * ------------------------------------------------------------------------- */

/* Human-readable text for `hr` into caller-owned `buf` (cch characters,
 * must be >= 1). Returns `buf`, always NUL-terminated, truncated if short.
 *
 * Order of resolution:
 *   1. apprecorder's hand-written table -- the AUDCLNT_* codes, which
 *      FormatMessage does not know (facility 0x889 has no message resource).
 *   2. FormatMessageW from the system message table, trailing CR/LF stripped.
 *   3. "Unrecognised error 0x........" so a code is never swallowed.
 *
 * Allocation-free (no FORMAT_MESSAGE_ALLOCATE_BUFFER) and thread-safe.
 * `buf` must not be NULL. */
const wchar_t *apr_hresult_message(HRESULT hr, wchar_t *buf, size_t cch);

/* The symbolic name for `hr` ("AUDCLNT_E_DEVICE_INVALIDATED") or NULL when the
 * code is not one apprecorder tables by hand. The returned pointer is static
 * and outlives everything. Thread-safe, allocation-free. */
const wchar_t *apr_hresult_name(HRESULT hr);

/* Static name of a kind ("APR_E_OVERRUN"). Never NULL, even for a value
 * outside the enum. Thread-safe, allocation-free. */
const wchar_t *apr_err_kind_name(AprErrKind kind);

/* Render a whole error into caller-owned `buf` (cch >= 1): context, the
 * decoded system message, the symbolic name and numeric code, and the raise
 * site. Returns `buf`, always NUL-terminated, truncated if short. A success
 * value renders as "no error". `e` and `buf` must not be NULL.
 * Allocation-free and thread-safe.
 *
 * THIS IS THE LOG'S RENDERING AND IT IS ENGLISH. apr_log_err() calls it on
 * whatever thread raised the error, capture pumps included, which is exactly
 * why it may not touch the string catalog. Never show its output to a user;
 * apr_err_reason() in errmsg.h is that path. */
const wchar_t *apr_err_format(const AprErr *e, wchar_t *buf, size_t cch);

#endif /* APPRECORDER_ERR_H */
