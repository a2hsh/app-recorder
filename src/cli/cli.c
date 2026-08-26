/*
 * cli.c -- apprecorder's command line.
 *
 * Read cli.h first; it holds the grammar and the reasoning. What follows are
 * the four things about this file that are load-bearing rather than obvious.
 *
 * 1. FINALIZE RUNS ON EVERY PATH, AND THAT IS ARRANGED STRUCTURALLY.
 *
 *    A recording that never reached finalize is at best short and at worst
 *    unplayable, and no surviving process can repair it once this one is gone.
 *    That asymmetry is what killed AAC/M4A outright (design 8), and the three
 *    formats left still owe the user a clean close. So there is exactly ONE
 *    place a graph is destroyed, it is a
 *    __finally block, and apr_graph_destroy stops every bus -- which finalizes
 *    every action -- before it frees anything. Nothing in do_record returns
 *    past that block, an access violation inside it unwinds through it (main
 *    supplies the __except that makes unwinding happen), and Ctrl+C never
 *    reaches it at all because Ctrl+C is a request rather than a kill.
 *
 * 2. CTRL+C IS A REQUEST.
 *
 *    The handler sets a flag, signals an event and returns TRUE, so Windows
 *    does not terminate us; the record loop notices and stops properly. A
 *    second Ctrl+C says the files are being closed and STILL refuses to
 *    abandon them. CTRL_CLOSE/LOGOFF/SHUTDOWN are different: Windows gives the
 *    handler a few seconds and then terminates the process regardless, so
 *    those block in the handler until the files are closed.
 *
 * 3. --dry-run OPENS NO AUDIO DEVICE.
 *
 *    Everything apr_cli_resolve does is a property query -- the process table,
 *    the audio engine's session list, the endpoint list, and a
 *    create-then-delete probe of each output path. None of it activates an
 *    IAudioClient. That is what makes the flag worth having in a script: it
 *    answers "would this work" without any of the side effects of finding out.
 *
 * 4. TEXT IS LOCALIZED; JSON IS NOT.
 *
 *    Every line a person reads comes from the catalog (AGENTS.md rule 6). The
 *    field names and the numbers inside --json output are deliberately fixed
 *    ASCII: they are a wire format a script matches on, and localizing them
 *    would break every script the moment the interface language changed. That
 *    is why JSON numbers go through swprintf and display numbers go through
 *    apr_str_number, which is also the only place digit shaping is decided.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
#include <wctype.h>

#include "cli/cli.h"

#include "action.h"
#include "clock.h"
#include "discover.h"
#include "graph.h"
#include "log.h"
#include "outpath.h"
#include "runner.h"
#include "session.h"
#include "strings.h"

#define APR_CLI_VERSION L"0.1.0"

/* Longest single line the CLI composes. Catalog entries are whole sentences
 * and paths are up to MAX_PATH, so this has room for two of each. */
#define LINE_CCH 1200

/* How many rows the listings will hold. Far past any real machine; a fuller
 * list than this is reported as its true count rather than silently cut. */
#define MAX_APPS      128
#define MAX_ENDPOINTS 64
#define MAX_TREE      256

/* ---------------------------------------------------------------------------
 * Output
 * ------------------------------------------------------------------------- */

typedef struct Ctx {
    const AprCliIo *io;
    int json;
    int quiet;
} Ctx;

static void emit(const Ctx *cx, int stream, const wchar_t *line)
{
    if (cx && cx->io && cx->io->write) cx->io->write(cx->io->user, stream,
                                                     line ? line : L"");
}

static void say(const Ctx *cx, int stream, AprStrId id,
                const wchar_t *const *args, size_t nargs)
{
    wchar_t buf[LINE_CCH];
    apr_str_format(id, buf, LINE_CCH, args, nargs);
    emit(cx, stream, buf);
}

#define SAY0(cx, stream, id) say((cx), (stream), (id), NULL, 0)

/* An indented line -- a source under its bus, a device id under its device.
 *
 * The indent lives HERE and never in the catalog, and that is not tidiness:
 * indentation is layout, and in a right-to-left language a nested row is
 * inset from the other side. A catalog entry that began with two spaces would
 * be inset from the wrong side the moment the interface language changed, and
 * a translator has no way to fix that from inside the string. apr_str_is_rtl()
 * is the one place direction comes from (strings.h). */
static void say_indented(const Ctx *cx, int stream, int level, AprStrId id,
                         const wchar_t *const *args, size_t nargs)
{
    wchar_t body[LINE_CCH];
    wchar_t out[LINE_CCH];
    wchar_t pad[17];
    int     n = level * 2;

    if (n > 16) n = 16;
    wmemset(pad, L' ', (size_t)n);
    pad[n] = L'\0';

    apr_str_format(id, body, LINE_CCH, args, nargs);
    if (apr_str_is_rtl())
        _snwprintf_s(out, LINE_CCH, _TRUNCATE, L"%ls%ls", body, pad);
    else
        _snwprintf_s(out, LINE_CCH, _TRUNCATE, L"%ls%ls", pad, body);
    emit(cx, stream, out);
}

/* Progress and status: suppressed by --quiet, and never emitted at all in
 * --json mode, where the document is the whole output. */
static void note(const Ctx *cx, AprStrId id,
                 const wchar_t *const *args, size_t nargs)
{
    if (cx->quiet || cx->json) return;
    say(cx, APR_CLI_STDOUT, id, args, nargs);
}

/* Warnings are NOT suppressed by --quiet: --quiet means "warnings and errors
 * only", and a muted source or a dead one is exactly what a person needs to
 * be told. */
static void warn(const Ctx *cx, AprStrId id,
                 const wchar_t *const *args, size_t nargs)
{
    if (cx->json) return;
    say(cx, APR_CLI_STDERR, id, args, nargs);
}

/* ---------------------------------------------------------------------------
 * Numbers
 *
 * num()/fixed() are for people and go through the localization layer, which is
 * the only place the Western-versus-Arabic-Indic digit question is decided.
 * The JSON emitter never uses them.
 * ------------------------------------------------------------------------- */

typedef struct NumBuf { wchar_t s[40]; } NumBuf;

static const wchar_t *num(NumBuf *b, int64_t v)
{
    apr_str_number(v, b->s, sizeof b->s / sizeof b->s[0]);
    return b->s;
}

static const wchar_t *fixed(NumBuf *b, int64_t scaled, int decimals)
{
    apr_str_number_fixed(scaled, decimals, b->s, sizeof b->s / sizeof b->s[0]);
    return b->s;
}

static const wchar_t *errtext(const AprErr *e, wchar_t *buf, size_t cch)
{
    return apr_err_format(e, buf, cch);
}

/* ---------------------------------------------------------------------------
 * JSON. Two spaces of indent, one value per line, so a document is a sequence
 * of short lines rather than one enormous one -- easier to read in a terminal
 * and identical to a parser.
 * ------------------------------------------------------------------------- */

static void json_escape(const wchar_t *s, wchar_t *out, size_t cch)
{
    size_t o = 0;

    if (!out || cch == 0) return;
    out[0] = L'\0';
    for (; s && *s && o + 8 < cch; s++) {
        wchar_t ch = *s;
        switch (ch) {
        case L'"':  out[o++] = L'\\'; out[o++] = L'"';  break;
        case L'\\': out[o++] = L'\\'; out[o++] = L'\\'; break;
        case L'\n': out[o++] = L'\\'; out[o++] = L'n';  break;
        case L'\r': out[o++] = L'\\'; out[o++] = L'r';  break;
        case L'\t': out[o++] = L'\\'; out[o++] = L't';  break;
        default:
            if (ch < 0x20) {
                o += (size_t)_snwprintf_s(out + o, cch - o, _TRUNCATE,
                                          L"\\u%04x", (unsigned)ch);
            } else {
                out[o++] = ch;
            }
        }
    }
    out[o] = L'\0';
}

static void jline(const Ctx *cx, int indent, const wchar_t *fmt, ...)
{
    wchar_t buf[LINE_CCH];
    wchar_t pad[17];
    int     n = indent * 2;
    va_list ap;

    if (n > 16) n = 16;
    wmemset(pad, L' ', (size_t)n);
    pad[n] = L'\0';

    va_start(ap, fmt);
    _vsnwprintf_s(buf, LINE_CCH, _TRUNCATE, fmt, ap);
    va_end(ap);

    {
        wchar_t out[LINE_CCH];
        _snwprintf_s(out, LINE_CCH, _TRUNCATE, L"%ls%ls", pad, buf);
        emit(cx, APR_CLI_STDOUT, out);
    }
}

static void jstr(const Ctx *cx, int indent, const wchar_t *key,
                 const wchar_t *value, int comma)
{
    wchar_t esc[LINE_CCH];
    json_escape(value, esc, LINE_CCH);
    jline(cx, indent, L"\"%ls\": \"%ls\"%ls", key, esc, comma ? L"," : L"");
}

static void jnum(const Ctx *cx, int indent, const wchar_t *key,
                 int64_t value, int comma)
{
    jline(cx, indent, L"\"%ls\": %lld%ls", key, (long long)value,
          comma ? L"," : L"");
}

static void jbool(const Ctx *cx, int indent, const wchar_t *key, int value,
                  int comma)
{
    jline(cx, indent, L"\"%ls\": %ls%ls", key, value ? L"true" : L"false",
          comma ? L"," : L"");
}

static void jreal(const Ctx *cx, int indent, const wchar_t *key, double value,
                  int comma)
{
    jline(cx, indent, L"\"%ls\": %.3f%ls", key, value, comma ? L"," : L"");
}

/* ---------------------------------------------------------------------------
 * Failure
 * ------------------------------------------------------------------------- */

static AprCliExit fail(const Ctx *cx, AprCliExit code, AprStrId id,
                       const wchar_t *const *args, size_t nargs)
{
    wchar_t msg[LINE_CCH];

    apr_str_format(id, msg, LINE_CCH, args, nargs);
    if (cx->json) {
        jline(cx, 0, L"{");
        jbool(cx, 1, L"ok", 0, 1);
        jnum(cx, 1, L"exitCode", (int64_t)code, 1);
        jstr(cx, 1, L"error", msg, 0);
        jline(cx, 0, L"}");
    } else {
        emit(cx, APR_CLI_STDERR, msg);
    }
    return code;
}

/* ---------------------------------------------------------------------------
 * Parsing primitives. Hand-written rather than wcstol/wcstod, so that neither
 * the CRT locale nor a stray suffix can turn "48k" into 48.
 * ------------------------------------------------------------------------- */

static int parse_i64(const wchar_t *s, int64_t *out)
{
    int64_t v = 0;
    int     neg = 0, digits = 0;

    if (!s || !*s) return 0;
    if (*s == L'-') { neg = 1; s++; }
    else if (*s == L'+') s++;
    for (; *s; s++) {
        if (*s < L'0' || *s > L'9') return 0;
        if (v > (INT64_MAX - 9) / 10) return 0;
        v = v * 10 + (*s - L'0');
        digits++;
    }
    if (!digits) return 0;
    *out = neg ? -v : v;
    return 1;
}

/* "-6.5" with decimals=1 -> -65: a decimal read into a scaled integer, so no
 * float is ever parsed and the CRT's locale cannot decide what a decimal point
 * is. Extra precision is truncated rather than rounded -- a tenth of a decibel
 * is already below anything audible, and a predictable truncation beats a
 * surprising round.
 *
 * One accumulator on purpose. An earlier version kept the whole and fractional
 * parts apart and scaled them separately; that shape made the Release compiler
 * (cl 14.42, /O1 /GL) fail with an internal compiler error, and it was harder
 * to read besides. */
static int parse_fixed(const wchar_t *s, int decimals, int64_t *out)
{
    int64_t value = 0;
    int     neg = 0, digits = 0, taken = 0, seen_dot = 0, d;

    if (!s || !*s || decimals < 0 || decimals > 9) return 0;
    if (*s == L'-') { neg = 1; s++; }
    else if (*s == L'+') s++;

    for (; *s; s++) {
        if (*s == L'.') {
            if (seen_dot) return 0;
            seen_dot = 1;
            continue;
        }
        if (*s < L'0' || *s > L'9') return 0;
        digits++;
        if (seen_dot) {
            if (taken >= decimals) continue;   /* past our precision: drop it */
            taken++;
        }
        if (value > (INT64_MAX - 9) / 10) return 0;
        value = value * 10 + (*s - L'0');
    }
    if (!digits) return 0;

    for (d = taken; d < decimals; d++) {
        if (value > INT64_MAX / 10) return 0;
        value *= 10;
    }
    *out = neg ? -value : value;
    return 1;
}

static int eq(const wchar_t *a, const wchar_t *b)
{
    return a && b && wcscmp(a, b) == 0;
}

static int ieq(const wchar_t *a, const wchar_t *b)
{
    return a && b && _wcsicmp(a, b) == 0;
}

static int icontains(const wchar_t *hay, const wchar_t *needle)
{
    size_t nl;
    if (!hay || !needle || !*needle) return 0;
    nl = wcslen(needle);
    for (; *hay; hay++) {
        if (_wcsnicmp(hay, needle, nl) == 0) return 1;
    }
    return 0;
}

static void copy_cch(wchar_t *dst, size_t cch, const wchar_t *src)
{
    if (!dst || cch == 0) return;
    dst[0] = L'\0';
    if (src) wcsncpy_s(dst, cch, src, _TRUNCATE);
}

/* ---------------------------------------------------------------------------
 * The stop signal, shared by the console control handler and the record loop
 * ------------------------------------------------------------------------- */

/* How long CTRL_CLOSE/LOGOFF/SHUTDOWN waits for the files. INFINITE: see the
 * handler. Named so the suite can pin it, because "it waits" is a promise in
 * cli.h and the last value here was a number that broke it. */
#define APR_CLI_CLOSE_WAIT_MS INFINITE

static volatile LONG g_stop_requested;
static volatile LONG g_handler_installed;
static HANDLE        g_stop_event;       /* manual reset */
static HANDLE        g_finished_event;   /* manual reset; files are closed */

void apr_cli_request_stop(void)
{
    InterlockedExchange(&g_stop_requested, 1);
    if (g_stop_event) SetEvent(g_stop_event);
}

int apr_cli_test_ctrl_handler_installed(void)
{
    return (int)InterlockedCompareExchange(&g_handler_installed, 0, 0);
}

unsigned long apr_cli_test_close_wait_ms(void)
{
    return (unsigned long)APR_CLI_CLOSE_WAIT_MS;
}

/* Written straight to the console rather than through AprCliIo: this runs on a
 * thread Windows injects, and the io a caller supplied may not expect that. */
static void console_line(int stream, const wchar_t *text);

static BOOL WINAPI ctrl_handler(DWORD type)
{
    LONG was;

    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        was = InterlockedExchange(&g_stop_requested, 1);
        if (g_stop_event) SetEvent(g_stop_event);
        if (was) {
            /* Pressed again. Say what is happening and keep finalizing: an
             * abandoned encoder is an unplayable file, which is worse than
             * waiting. */
            console_line(APR_CLI_STDERR, apr_str(APR_S_WARN_STILL_FINISHING));
        }
        return TRUE;

    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        /* Windows terminates the process shortly after this returns, so the
         * work has to finish HERE. Block until the files are closed. */
        InterlockedExchange(&g_stop_requested, 1);
        if (g_stop_event) SetEvent(g_stop_event);
        /* NO TIMEOUT, and that is the whole point. cli.h promises this handler
         * blocks until the files are closed; a four-second cap turned that
         * promise into "four seconds, then an unplayable file", and it did so
         * by GUARANTEEING the kill at four seconds rather than merely risking
         * one. Windows decides when we die either way -- and while we are
         * still here it offers the user an End Task dialog they may decline,
         * which is more time, not less. There is nothing to gain by returning
         * early and a recording to lose. */
        if (g_finished_event)
            WaitForSingleObject(g_finished_event, APR_CLI_CLOSE_WAIT_MS);
        return TRUE;

    default:
        return FALSE;
    }
}

static void stop_signal_open(void)
{
    InterlockedExchange(&g_stop_requested, 0);
    if (!g_stop_event)     g_stop_event     = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_finished_event) g_finished_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_stop_event)     ResetEvent(g_stop_event);
    if (g_finished_event) ResetEvent(g_finished_event);

    if (SetConsoleCtrlHandler(ctrl_handler, TRUE))
        InterlockedExchange(&g_handler_installed, 1);
}

static void stop_signal_close(void)
{
    if (g_finished_event) SetEvent(g_finished_event);
    if (apr_cli_test_ctrl_handler_installed()) {
        SetConsoleCtrlHandler(ctrl_handler, FALSE);
        InterlockedExchange(&g_handler_installed, 0);
    }
    InterlockedExchange(&g_stop_requested, 0);
}

static int stop_requested(void)
{
    return (int)InterlockedCompareExchange(&g_stop_requested, 0, 0);
}

/* ---------------------------------------------------------------------------
 * Defaults
 * ------------------------------------------------------------------------- */

static void plan_defaults(AprCliPlan *p)
{
    memset(p, 0, sizeof *p);
    p->cmd       = APR_CLI_CMD_RECORD;
    p->rate      = 48000;
    p->channels  = 2;
    p->log_level = APR_LOG_WARN;
}

static AprCliBus *current_bus(AprCliPlan *p)
{
    if (p->bus_count == 0) {
        /* The implicit bus. Its name comes from the catalog rather than a
         * literal, because it is shown to a person. */
        copy_cch(p->buses[0].name, APR_NAME_CCH,
                 apr_str(APR_S_CLI_DEFAULT_BUS_NAME));
        p->bus_count = 1;
    }
    return &p->buses[p->bus_count - 1];
}

/* ---------------------------------------------------------------------------
 * Parse
 * ------------------------------------------------------------------------- */

/* Which options consume the argument after them. ONE list, read twice: the
 * parse loop below uses it to know what to swallow, and prescan uses it to
 * know what NOT to look at. Two copies would part company the first time an
 * option was added, and the way they would part company is silent. */
static int option_takes_value(const wchar_t *a)
{
    return eq(a, L"--bus") || eq(a, L"--pid") || eq(a, L"--exe") ||
           eq(a, L"--device") || eq(a, L"--fake") ||
           eq(a, L"--system-minus-tree") || eq(a, L"--gain") ||
           eq(a, L"--out") || eq(a, L"--format") ||
           eq(a, L"--bitrate") || eq(a, L"--quality") ||
           eq(a, L"--rate") || eq(a, L"--channels") ||
           eq(a, L"--duration") || eq(a, L"--lang") ||
           eq(a, L"--log-level") || eq(a, L"--log-file") ||
           eq(a, L"--session");
}

/* --json and --quiet have to be known BEFORE the option that fails, because
 * they may be written after it. That is why this is a separate pass -- but a
 * pass that walked every word would find them inside somebody else's value:
 * `--out --json` names a file called "--json", and reading it as the flag
 * turned a plain-text refusal into a JSON one for a command line that never
 * asked for either. So this walks the same grammar the parser does and steps
 * over each option's value. */
static int prescan(int argc, const wchar_t *const *argv, const wchar_t *flag)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (eq(argv[i], flag)) return 1;
        if (option_takes_value(argv[i])) i++;
    }
    return 0;
}

static AprCliExit range_error(const Ctx *cx, const wchar_t *opt,
                              int64_t lo, int64_t hi, int decimals,
                              const wchar_t *given)
{
    NumBuf a, b;
    const wchar_t *args[4];

    args[0] = opt;
    args[1] = fixed(&a, lo, decimals);
    args[2] = fixed(&b, hi, decimals);
    args[3] = given;
    return fail(cx, APR_CLI_USAGE, APR_S_ERR_OUT_OF_RANGE, args, 4);
}

static AprCliExit number_error(const Ctx *cx, const wchar_t *opt,
                               const wchar_t *given)
{
    const wchar_t *args[2];
    args[0] = opt;
    args[1] = given;
    return fail(cx, APR_CLI_USAGE, APR_S_ERR_BAD_NUMBER, args, 2);
}

/* --- the health half of a --fake spec ---------------------------------------
 *
 * A synthetic source can be asked to go silent or to die part way through, so
 * that the two failures which look exactly like a successful recording can be
 * exercised from a command line. capture.h explains why they look like
 * success; what matters here is that both are described on the source itself,
 * in the same comma-separated spec as its tone and its drift, rather than on
 * a flag of their own -- health is part of describing a synthetic source.
 *
 * A frame is never 0: capture.h reserves 0 for "never", and frame 0 is what
 * the bare words `muted` and `dead` are for. Refusing a 0 frame here keeps
 * those two spellings from meaning two different things.
 * -------------------------------------------------------------------------- */

/* The ceiling is session.h's, not a second one of ours: what can be typed has
 * to be what can be written down and read back again. */
static int fake_frame(const wchar_t *v, uint64_t *out)
{
    int64_t n;
    if (!parse_i64(v, &n) || n < 1 || n > APR_SESSION_MAX_FRAME) return 0;
    *out = (uint64_t)n;
    return 1;
}

static int is_fake_health(const wchar_t *tok)
{
    return eq(tok, L"muted") || eq(tok, L"dead") ||
           wcsncmp(tok, L"mute=",   5) == 0 ||
           wcsncmp(tok, L"unmute=", 7) == 0 ||
           wcsncmp(tok, L"die=",    4) == 0;
}

static int parse_fake_health(const wchar_t *tok, AprCliSource *s)
{
    if (eq(tok, L"muted")) { s->fake_start_muted = 1; return 1; }
    if (eq(tok, L"dead"))  { s->fake_start_dead  = 1; return 1; }
    if (wcsncmp(tok, L"mute=",   5) == 0) return fake_frame(tok + 5, &s->fake_mute_at);
    if (wcsncmp(tok, L"unmute=", 7) == 0) return fake_frame(tok + 7, &s->fake_unmute_at);
    if (wcsncmp(tok, L"die=",    4) == 0) return fake_frame(tok + 4, &s->fake_die_at);
    return 0;
}

/* Reads "<hz>[,<ppm>[,<amplitude>]]" followed by any number of health fields:
 *
 *     muted | dead | mute=<frame> | unmute=<frame> | die=<frame>
 *
 * The tone is still required and still first. A health field may appear as
 * soon as the fields before it have been given -- "440,dead" is as readable as
 * "440,0,0.25,dead" and means the same thing -- and once one has been seen,
 * everything after it is health too, so no health word can be mistaken for a
 * number that was meant to be a drift.
 *
 * `mute=` and `unmute=` landing on the SAME frame is not resolved here: the
 * fake source refuses it at open (capture.h), which keeps one rule in one
 * place and reports it with the reason attached. */
static int parse_fake(const wchar_t *spec, AprCliSource *s)
{
    wchar_t  copy[APR_CLI_SPEC_CCH];
    wchar_t *ctxp = NULL;
    wchar_t *tok;
    int64_t  v;
    int      field = 0;
    int      health = 0;

    s->fake_hz  = 440;
    s->fake_ppm = 0;
    s->fake_amp = 0.25f;

    /* A silently truncated spec would parse as a different, valid one. */
    if (!spec || wcslen(spec) >= APR_CLI_SPEC_CCH) return 0;
    copy_cch(copy, APR_CLI_SPEC_CCH, spec);

    tok = wcstok_s(copy, L",", &ctxp);
    while (tok) {
        if (health || field >= 3 || is_fake_health(tok)) {
            health = 1;
            if (!parse_fake_health(tok, s)) return 0;
        } else {
            switch (field) {
            case 0:
                if (!parse_i64(tok, &v) || v < 0 || v > 192000) return 0;
                s->fake_hz = (uint32_t)v;
                break;
            case 1:
                if (!parse_i64(tok, &v) || v < -100000 || v > 100000) return 0;
                s->fake_ppm = (int32_t)v;
                break;
            default:
                if (!parse_fixed(tok, 6, &v) || v < 0 || v > 1000000) return 0;
                s->fake_amp = (float)((double)v / 1000000.0);
                break;
            }
            field++;
        }
        tok = wcstok_s(NULL, L",", &ctxp);
    }
    return field > 0;
}

static AprCliSource *add_source(AprCliPlan *p, AprCliBus *b,
                                AprCliSourceKind kind, const wchar_t *spec)
{
    AprCliSource *s;

    (void)p;
    if (b->source_count >= APR_MAX_SOURCES_PER_BUS) return NULL;
    s = &b->sources[b->source_count++];
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->gain = 1.0f;
    s->gain_db_tenths = 0;
    copy_cch(s->spec, APR_CLI_SPEC_CCH, spec);
    copy_cch(s->label, APR_NAME_CCH, spec);
    return s;
}

AprCliExit apr_cli_parse(int argc, const wchar_t *const *argv,
                         AprCliPlan *plan, const AprCliIo *io)
{
    Ctx cx;
    AprCliBus    *bus = NULL;
    AprCliSource *last = NULL;
    char  pending_format[16];
    int   pending_bitrate = 0, pending_quality = 0;
    int   i = 1;
    /* The first option that describes the graph, remembered so that
     * "--session and --pid together" can name which one it means rather than
     * making the user work it out. */
    const wchar_t *graph_opt = NULL;

    if (!plan) return APR_CLI_INTERNAL;
    plan_defaults(plan);
    pending_format[0] = '\0';

    cx.io    = io;
    /* --json and --quiet may appear anywhere, including after the option that
     * fails, so they are read before anything can fail. */
    cx.json  = prescan(argc, argv, L"--json");
    cx.quiet = prescan(argc, argv, L"--quiet");

    if (argc > 1 && argv[1] && argv[1][0] != L'-') {
        const wchar_t *c = argv[1];
        if      (eq(c, L"record"))       plan->cmd = APR_CLI_CMD_RECORD;
        else if (eq(c, L"list-apps"))    plan->cmd = APR_CLI_CMD_LIST_APPS;
        else if (eq(c, L"list-devices")) plan->cmd = APR_CLI_CMD_LIST_DEVICES;
        else if (eq(c, L"help"))         plan->cmd = APR_CLI_CMD_HELP;
        else if (eq(c, L"version"))      plan->cmd = APR_CLI_CMD_VERSION;
        else if (eq(c, L"save-session")) plan->cmd = APR_CLI_CMD_SAVE_SESSION;
        else {
            const wchar_t *args[1];
            args[0] = c;
            return fail(&cx, APR_CLI_USAGE, APR_S_ERR_UNKNOWN_COMMAND, args, 1);
        }
        i = 2;
    }

    for (; i < argc; i++) {
        const wchar_t *a = argv[i];
        const wchar_t *v = NULL;
        int64_t        n = 0;
        int            wants_value;

        if (!a) continue;

        wants_value = option_takes_value(a);

        if (eq(a, L"--bus") || eq(a, L"--pid") || eq(a, L"--exe") ||
            eq(a, L"--device") || eq(a, L"--fake") ||
            eq(a, L"--system-minus-tree") || eq(a, L"--gain") ||
            eq(a, L"--out") || eq(a, L"--format") ||
            eq(a, L"--bitrate") || eq(a, L"--quality"))
        {
            if (!graph_opt) graph_opt = a;
        }

        if (wants_value) {
            if (i + 1 >= argc) {
                const wchar_t *args[1];
                args[0] = a;
                return fail(&cx, APR_CLI_USAGE, APR_S_ERR_OPTION_NEEDS_VALUE,
                            args, 1);
            }
            v = argv[++i];
        }

        /* --- session-wide ------------------------------------------------- */
        if (eq(a, L"--json") || eq(a, L"--quiet")) {
            plan->json  = cx.json;
            plan->quiet = cx.quiet;
            continue;
        }
        if (eq(a, L"--dry-run")) { plan->dry_run = 1; continue; }
        if (eq(a, L"--all"))     { plan->all = 1;     continue; }
        if (eq(a, L"--session")) {
            copy_cch(plan->session_file, APR_CLI_SPEC_CCH, v);
            continue;
        }
        /* Consent, and it does nothing on its own. Without a session file
         * asking for an EXCLUDE source this flag enables no capture of any
         * kind -- design 4.1.1 is about a FILE not being able to widen the
         * scope, and a flag that widened it by itself would be the same bug
         * wearing a different hat. */
        if (eq(a, L"--allow-system-capture")) {
            plan->allow_system_capture = 1;
            continue;
        }
        if (eq(a, L"--allow-missing")) { plan->allow_missing = 1; continue; }
        if (eq(a, L"--help") || eq(a, L"-h") || eq(a, L"-?")) {
            plan->cmd = APR_CLI_CMD_HELP;
            continue;
        }
        if (eq(a, L"--rate")) {
            if (!parse_i64(v, &n)) return number_error(&cx, a, v);
            if (n < 8000 || n > 384000) return range_error(&cx, a, 8000, 384000, 0, v);
            plan->rate = (uint32_t)n;
            plan->explicit_rate = 1;
            continue;
        }
        if (eq(a, L"--channels")) {
            if (!parse_i64(v, &n)) return number_error(&cx, a, v);
            if (n < 1 || n > 8) return range_error(&cx, a, 1, 8, 0, v);
            plan->channels = (uint16_t)n;
            plan->explicit_channels = 1;
            continue;
        }
        if (eq(a, L"--duration")) {
            if (!parse_fixed(v, 3, &n)) return number_error(&cx, a, v);
            if (n < 1 || n > 86400000) return range_error(&cx, a, 1, 86400000, 3, v);
            plan->duration_ms = n;
            plan->explicit_duration = 1;
            continue;
        }
        if (eq(a, L"--lang")) { copy_cch(plan->lang, 32, v); continue; }
        if (eq(a, L"--log-file")) {
            copy_cch(plan->log_file, APR_CLI_SPEC_CCH, v);
            continue;
        }
        if (eq(a, L"--log-level")) {
            if      (ieq(v, L"trace")) plan->log_level = APR_LOG_TRACE;
            else if (ieq(v, L"debug")) plan->log_level = APR_LOG_DEBUG;
            else if (ieq(v, L"info"))  plan->log_level = APR_LOG_INFO;
            else if (ieq(v, L"warn"))  plan->log_level = APR_LOG_WARN;
            else if (ieq(v, L"error")) plan->log_level = APR_LOG_ERROR;
            else if (ieq(v, L"off"))   plan->log_level = APR_LOG_OFF;
            else {
                const wchar_t *args[2];
                args[0] = a; args[1] = v;
                return fail(&cx, APR_CLI_USAGE, APR_S_ERR_BAD_NUMBER, args, 2);
            }
            continue;
        }

        /* Everything below describes a recording -- which save-session also
         * does, because writing one down and making one take exactly the
         * same grammar. */
        if (plan->cmd != APR_CLI_CMD_RECORD &&
            plan->cmd != APR_CLI_CMD_SAVE_SESSION) {
            const wchar_t *args[2];
            args[0] = a;
            args[1] = argv[1];
            return fail(&cx, APR_CLI_USAGE, APR_S_ERR_OPTION_NOT_FOR_COMMAND,
                        args, 2);
        }

        /* --- buses -------------------------------------------------------- */
        if (eq(a, L"--bus")) {
            if (plan->bus_count >= APR_MAX_BUSES) {
                NumBuf nb;
                const wchar_t *args[1];
                args[0] = num(&nb, APR_MAX_BUSES);
                return fail(&cx, APR_CLI_CONFIG, APR_S_ERR_TOO_MANY_BUSES, args, 1);
            }
            bus = &plan->buses[plan->bus_count++];
            memset(bus, 0, sizeof *bus);
            copy_cch(bus->name, APR_NAME_CCH, v);
            last = NULL;
            continue;
        }

        /* --- sources ------------------------------------------------------ */
        if (eq(a, L"--pid") || eq(a, L"--exe") || eq(a, L"--device") ||
            eq(a, L"--fake") || eq(a, L"--system-minus-tree"))
        {
            AprCliSourceKind kind =
                eq(a, L"--pid")    ? APR_CLI_SRC_PID :
                eq(a, L"--exe")    ? APR_CLI_SRC_EXE :
                eq(a, L"--device") ? APR_CLI_SRC_DEVICE :
                eq(a, L"--fake")   ? APR_CLI_SRC_FAKE :
                                     APR_CLI_SRC_SYSTEM_MINUS_TREE;

            bus = current_bus(plan);
            last = add_source(plan, bus, kind, v);
            if (!last) {
                NumBuf nb;
                const wchar_t *args[2];
                args[0] = bus->name;
                args[1] = num(&nb, APR_MAX_SOURCES_PER_BUS);
                return fail(&cx, APR_CLI_CONFIG, APR_S_ERR_TOO_MANY_SOURCES,
                            args, 2);
            }
            if (kind == APR_CLI_SRC_FAKE && !parse_fake(v, last))
                return number_error(&cx, a, v);
            if ((kind == APR_CLI_SRC_PID || kind == APR_CLI_SRC_SYSTEM_MINUS_TREE)) {
                if (!parse_i64(v, &n) || n <= 0 || n > 0xffffffffLL)
                    return number_error(&cx, a, v);
                last->pid = (uint32_t)n;
            }
            continue;
        }

        if (eq(a, L"--gain")) {
            if (!last) {
                return fail(&cx, APR_CLI_USAGE, APR_S_ERR_GAIN_WITHOUT_SOURCE,
                            NULL, 0);
            }
            if (!parse_fixed(v, 1, &n)) return number_error(&cx, a, v);
            if (n < -1200 || n > 400) return range_error(&cx, a, -1200, 400, 1, v);
            last->gain_db_tenths = (int32_t)n;
            last->gain = (float)pow(10.0, (double)n / 200.0);
            continue;
        }

        /* --- outputs ------------------------------------------------------ */
        if (eq(a, L"--format")) {
            size_t k;
            pending_format[0] = '\0';
            for (k = 0; k + 1 < sizeof pending_format && v[k]; k++)
                pending_format[k] = (char)(v[k] < 128 ? v[k] : '?');
            pending_format[k] = '\0';
            continue;
        }
        if (eq(a, L"--bitrate")) {
            if (!parse_i64(v, &n)) return number_error(&cx, a, v);
            if (n < 0 || n > 1152) return range_error(&cx, a, 0, 1152, 0, v);
            pending_bitrate = (int)n;
            continue;
        }
        if (eq(a, L"--quality")) {
            if (!parse_i64(v, &n)) return number_error(&cx, a, v);
            if (n < 0 || n > 10) return range_error(&cx, a, 0, 10, 0, v);
            pending_quality = (int)n;
            continue;
        }
        if (eq(a, L"--out")) {
            AprCliOutput *o;
            bus = current_bus(plan);
            if (bus->output_count >= APR_CLI_MAX_OUTPUTS_PER_BUS) {
                NumBuf nb;
                const wchar_t *args[2];
                args[0] = bus->name;
                args[1] = num(&nb, APR_CLI_MAX_OUTPUTS_PER_BUS);
                return fail(&cx, APR_CLI_CONFIG, APR_S_ERR_TOO_MANY_OUTPUTS,
                            args, 2);
            }
            o = &bus->outputs[bus->output_count++];
            memset(o, 0, sizeof *o);
            copy_cch(o->path, APR_CLI_SPEC_CCH, v);
            strcpy_s(o->action_id, sizeof o->action_id, pending_format);
            o->bitrate_kbps = pending_bitrate;
            o->quality      = pending_quality;
            /* Deliberately one-shot: --format applies to the output it
             * precedes, so two outputs of different formats do not need the
             * flag cleared by hand. */
            pending_format[0] = '\0';
            pending_bitrate = 0;
            pending_quality = 0;
            continue;
        }

        {
            const wchar_t *args[1];
            args[0] = a;
            return fail(&cx, APR_CLI_USAGE, APR_S_ERR_UNKNOWN_OPTION, args, 1);
        }
    }

    plan->json  = cx.json;
    plan->quiet = cx.quiet;

    /* A session file describes the WHOLE recording. Merging it with options
     * typed alongside would leave "which one wins" as something a person has
     * to remember at 2 a.m., and getting it wrong records the wrong thing. */
    if (plan->cmd == APR_CLI_CMD_RECORD && plan->session_file[0] && graph_opt) {
        const wchar_t *args[1];
        args[0] = graph_opt;
        return fail(&cx, APR_CLI_USAGE, APR_S_ERR_SESSION_WITH_SOURCES, args, 1);
    }
    if (plan->cmd == APR_CLI_CMD_SAVE_SESSION && !plan->session_file[0])
        return fail(&cx, APR_CLI_USAGE, APR_S_ERR_SESSION_NEEDED, NULL, 0);

    return APR_CLI_OK;
}

/* ---------------------------------------------------------------------------
 * Resolve
 * ------------------------------------------------------------------------- */

static const wchar_t *extension_of(const wchar_t *path)
{
    const wchar_t *dot = wcsrchr(path, L'.');
    const wchar_t *sep = wcsrchr(path, L'\\');
    const wchar_t *alt = wcsrchr(path, L'/');

    if (sep && alt && alt > sep) sep = alt;
    if (!sep) sep = alt;
    if (!dot) return NULL;
    if (sep && dot < sep) return NULL;
    return dot[1] ? dot + 1 : NULL;
}

/* Case-insensitive compare of a registry id (ASCII, by definition -- session
 * files hold it) against a file extension. Not _wcsicmp on a widened copy,
 * because there is nothing to widen: an id that is not plain ASCII is not an
 * id this product would accept in a session file either. */
static int ieq_ascii_w(const char *a, const wchar_t *b)
{
    if (!a || !b) return 0;
    for (; *a && *b; a++, b++) {
        if (towlower((wint_t)(unsigned char)*a) != towlower((wint_t)*b)) return 0;
    }
    return *a == '\0' && *b == L'\0';
}

/* TWO PASSES, AND THE ORDER IS THE POINT.
 *
 * First the extension each action actually WRITES. That is the authoritative
 * mapping and it must win: the ogg action writes ".opus" because RFC 7845 says
 * so, and a path spelled that way has to reach it.
 *
 * Then the registry id. This is what makes `--out mix.ogg` work without an
 * explicit --format, which it did not before: ".opus" resolved and ".ogg" did
 * not, so the same action was reachable by the spelling almost nobody types
 * and not by the one everybody does. An id is a stable, lowercase, ASCII token
 * that already names the format, so treating it as an accepted spelling costs
 * nothing and removes a surprise.
 *
 * An action whose extension is EMPTY is reachable by neither pass. That is the
 * built-in "none" sink: it writes no file, so "x.none" is a format this build
 * cannot write and must be refused, not silently turned into a discard. */
static const AprActionVTable *action_for_extension(const wchar_t *ext)
{
    size_t i, n = apr_action_count();

    for (i = 0; i < n; i++) {
        const AprActionVTable *vt = apr_action_at(i);
        if (vt && vt->extension && vt->extension[0] && ieq(vt->extension, ext))
            return vt;
    }
    for (i = 0; i < n; i++) {
        const AprActionVTable *vt = apr_action_at(i);
        if (vt && vt->extension && vt->extension[0] && ieq_ascii_w(vt->id, ext))
            return vt;
    }
    return NULL;
}

/* "Can this path be created and written" lives in src/platform/outpath.c now:
 * the bus needs the same answer at the moment an output is added, and two
 * copies of a CreateFileW probe would be exactly the DRY failure AGENTS.md
 * rule 3 names (outpath.h). What was here is what is there. */

static void full_path(const wchar_t *in, wchar_t *out, size_t cch)
{
    if (GetFullPathNameW(in, (DWORD)cch, out, NULL) == 0)
        copy_cch(out, cch, in);
}

/* An output path is a TEMPLATE (outpath.h), so every question below -- is this
 * writable, is it the same file as that one -- has to be asked of what it
 * expands to and never of the braces themselves. Probing "mix.{ext}" literally
 * would create a file called exactly that. */
static void expanded_output_path(const AprCliBus *b, const AprCliOutput *o,
                                 wchar_t *out, size_t cch)
{
    AprOutContext ctx;
    const AprActionVTable *vt = apr_action_find(o->action_id);
    AprErr e;

    ctx.bus_name  = b->name;
    ctx.extension = vt ? vt->extension : NULL;

    e = apr_out_expand(o->path, &ctx, out, cch);
    if (apr_failed(&e)) copy_cch(out, cch, o->path);
}

/* One predicate, used both to count matches and to list them. */
static int exe_hit(const AprAudioApp *a, const wchar_t *spec, int exact)
{
    if (exact) return ieq(a->exe, spec);
    return icontains(a->exe, spec) || icontains(a->display, spec);
}

static AprCliExit resolve_process(const Ctx *cx, AprCliSource *s,
                                  const AprAudioApp *apps, size_t app_count)
{
    const wchar_t *args[2];
    size_t i;

    if (s->kind == APR_CLI_SRC_PID || s->kind == APR_CLI_SRC_SYSTEM_MINUS_TREE) {
        if (!apr_process_exists(s->pid)) {
            args[0] = s->spec;
            return fail(cx, APR_CLI_NOT_FOUND, APR_S_ERR_PID_NOT_RUNNING, args, 1);
        }
        if (apr_process_image_name(s->pid, s->label, APR_NAME_CCH) == 0)
            copy_cch(s->label, APR_NAME_CCH, s->spec);
        for (i = 0; i < app_count; i++) {
            if (apps[i].pid == s->pid) s->muted_now = apps[i].muted;
        }
        return APR_CLI_OK;
    }

    /* --exe: matched against what the audio engine currently knows about, not
     * against the process table. An application with no audio session has
     * nothing to record. */
    {
        size_t  hits = 0, first = 0;
        wchar_t list[LINE_CCH];
        int     exact = 1;

        /* An exact executable name wins outright; a partial one is tried only
         * when nothing matched exactly. `exact` then records WHICH pass found
         * the matches, so the "which one did you mean" list below uses the very
         * same predicate and cannot name something that was not counted. */
        for (i = 0; i < app_count; i++) {
            if (!exe_hit(&apps[i], s->spec, 1)) continue;
            if (!hits) first = i;
            hits++;
        }
        if (hits == 0) {
            exact = 0;
            for (i = 0; i < app_count; i++) {
                if (!exe_hit(&apps[i], s->spec, 0)) continue;
                if (!hits) first = i;
                hits++;
            }
        }
        if (hits == 0) {
            args[0] = s->spec;
            return fail(cx, APR_CLI_NOT_FOUND, APR_S_ERR_EXE_NOT_PLAYING, args, 1);
        }
        if (hits > 1) {
            NumBuf nb;
            list[0] = L'\0';
            for (i = 0; i < app_count; i++) {
                wchar_t one[64];
                if (!exe_hit(&apps[i], s->spec, exact)) continue;
                _snwprintf_s(one, 64, _TRUNCATE, L"%ls%ls",
                             list[0] ? L", " : L"", num(&nb, apps[i].pid));
                wcsncat_s(list, LINE_CCH, one, _TRUNCATE);
            }
            args[0] = s->spec;
            args[1] = list;
            return fail(cx, APR_CLI_NOT_FOUND, APR_S_ERR_EXE_AMBIGUOUS, args, 2);
        }
        s->pid       = apps[first].pid;
        s->muted_now = apps[first].muted;
        copy_cch(s->label, APR_NAME_CCH,
                 apps[first].exe[0] ? apps[first].exe : apps[first].display);
        return APR_CLI_OK;
    }
}

static AprCliExit resolve_device(const Ctx *cx, AprCliSource *s,
                                 const AprAudioEndpoint *eps, size_t ep_count)
{
    const wchar_t *args[2];
    size_t i, hits = 0, first = 0;
    wchar_t list[LINE_CCH];

    for (i = 0; i < ep_count; i++) {
        if (ieq(eps[i].id, s->spec)) { first = i; hits = 1; break; }
    }
    if (hits == 0) {
        for (i = 0; i < ep_count; i++) {
            if (icontains(eps[i].name, s->spec)) {
                if (!hits) first = i;
                hits++;
            }
        }
    }
    if (hits == 0) {
        args[0] = s->spec;
        return fail(cx, APR_CLI_NOT_FOUND, APR_S_ERR_DEVICE_NOT_FOUND, args, 1);
    }
    if (hits > 1) {
        list[0] = L'\0';
        for (i = 0; i < ep_count; i++) {
            if (icontains(eps[i].name, s->spec)) {
                wchar_t one[APR_DISC_NAME_CCH + 4];
                _snwprintf_s(one, APR_DISC_NAME_CCH + 4, _TRUNCATE, L"%ls%ls",
                             list[0] ? L", " : L"", eps[i].name);
                wcsncat_s(list, LINE_CCH, one, _TRUNCATE);
            }
        }
        args[0] = s->spec;
        args[1] = list;
        return fail(cx, APR_CLI_NOT_FOUND, APR_S_ERR_DEVICE_AMBIGUOUS, args, 2);
    }
    copy_cch(s->endpoint_id, APR_DISC_ENDPOINT_CCH, eps[first].id);
    copy_cch(s->label, APR_NAME_CCH,
             eps[first].name[0] ? eps[first].name : eps[first].id);
    return APR_CLI_OK;
}

/* The privacy warning. Design 4.1.1 is emphatic that this must never be worded
 * as "everything except X": what is held back is a whole process TREE, it
 * grows after the recording starts, and naming a terminal holds back every
 * program launched from it. So the warning names the tree, lists who is in it
 * right now, and says the launcher case out loud. */
static void warn_system_capture(const Ctx *cx, const AprCliSource *s)
{
    uint32_t tree[MAX_TREE];
    size_t   count = 0, i;
    wchar_t  list[LINE_CCH];
    const wchar_t *args[1];
    AprErr   e;

    args[0] = s->label;
    SAY0(cx, APR_CLI_STDERR, APR_S_WARN_SYSTEM_CAPTURE_SCOPE);
    say(cx, APR_CLI_STDERR, APR_S_WARN_SYSTEM_CAPTURE_TREE, args, 1);

    e = apr_enum_process_tree(s->pid, tree, MAX_TREE, &count);
    if (!apr_failed(&e) && count > 0) {
        list[0] = L'\0';
        for (i = 0; i < count && i < MAX_TREE; i++) {
            wchar_t name[APR_NAME_CCH];
            wchar_t one[APR_NAME_CCH + 32];
            NumBuf  nb;
            if (apr_process_image_name(tree[i], name, APR_NAME_CCH) == 0)
                copy_cch(name, APR_NAME_CCH, num(&nb, tree[i]));
            _snwprintf_s(one, APR_NAME_CCH + 32, _TRUNCATE, L"%ls%ls (%ls)",
                         list[0] ? L", " : L"", name, num(&nb, tree[i]));
            if (wcslen(list) + wcslen(one) + 1 >= LINE_CCH) break;
            wcsncat_s(list, LINE_CCH, one, _TRUNCATE);
        }
        args[0] = list;
        say(cx, APR_CLI_STDERR, APR_S_WARN_SYSTEM_CAPTURE_MEMBERS, args, 1);
    }

    args[0] = s->label;
    say(cx, APR_CLI_STDERR, APR_S_WARN_SYSTEM_CAPTURE_LAUNCHER, args, 1);
}

AprCliExit apr_cli_resolve(AprCliPlan *plan, const AprCliIo *io)
{
    static AprAudioApp      apps[MAX_APPS];
    static AprAudioEndpoint eps[MAX_ENDPOINTS];
    Ctx    cx;
    size_t app_count = 0, ep_count = 0;
    size_t bi, si, oi;
    int    want_apps = 0, want_devices = 0;

    if (!plan) return APR_CLI_INTERNAL;
    cx.io = io; cx.json = plan->json; cx.quiet = plan->quiet;

    if (plan->cmd != APR_CLI_CMD_RECORD &&
        plan->cmd != APR_CLI_CMD_SAVE_SESSION) return APR_CLI_OK;

    /* ----------------------------------------------------------------------
     * WHAT --allow-missing MEANS WHEN IT EMPTIES A BUS
     *
     * THE RULE: under --allow-missing, a bus whose every source was dropped is
     * itself dropped, with a sentence, and the rest of the run proceeds. The
     * run is refused only when NO bus is left -- there is then nothing to
     * record at all, which is the same exit 2 an empty command line gets.
     *
     * The alternative -- refusing the whole run because one bus emptied -- is
     * what this code did, and it is the exact outcome the flag is typed to
     * prevent. Two buses, "Mix" (Teams + mic) and "Voice" (mic); Teams is not
     * playing; --allow-missing drops it, "Mix" is now source-less, and the
     * whole session aborts, so VOICE NEVER RECORDS EITHER. The meeting is lost
     * by the flag that existed to save it.
     *
     * Dropping the bus takes its outputs with it, so no empty file appears
     * where a recording was expected -- an empty file is a worse lie than a
     * missing one. And the run ends INCOMPLETE rather than OK, because what
     * was recorded is not what was asked for: that is what exit 6 means, and
     * it is the same verdict a dropped SOURCE already produces.
     *
     * RECORD ONLY. `save-session` describes a recording rather than making
     * one, and a session file quietly written with a bus missing is the silent
     * configuration loss this product refuses everywhere else.
     * -------------------------------------------------------------------- */
    if (plan->allow_missing && plan->cmd == APR_CLI_CMD_RECORD) {
        size_t keep = 0;
        for (bi = 0; bi < plan->bus_count; bi++) {
            const wchar_t *args[1];
            if (plan->buses[bi].source_count == 0) {
                args[0] = plan->buses[bi].name;
                warn(&cx, APR_S_WARN_BUS_DROPPED, args, 1);
                plan->session_incomplete = 1;
                continue;
            }
            if (keep != bi) plan->buses[keep] = plan->buses[bi];
            keep++;
        }
        plan->bus_count = keep;
    }

    if (plan->bus_count == 0)
        return fail(&cx, APR_CLI_CONFIG, APR_S_ERR_NO_SOURCES, NULL, 0);

    for (bi = 0; bi < plan->bus_count; bi++) {
        AprCliBus *b = &plan->buses[bi];
        const wchar_t *args[1];
        args[0] = b->name;
        if (b->source_count == 0)
            return fail(&cx, APR_CLI_CONFIG, APR_S_ERR_BUS_HAS_NO_SOURCE, args, 1);
        if (b->output_count == 0)
            return fail(&cx, APR_CLI_CONFIG, APR_S_ERR_BUS_HAS_NO_OUTPUT, args, 1);
        for (si = 0; si < b->source_count; si++) {
            switch (b->sources[si].kind) {
            case APR_CLI_SRC_PID:
            case APR_CLI_SRC_EXE:
            case APR_CLI_SRC_SYSTEM_MINUS_TREE: want_apps = 1;    break;
            case APR_CLI_SRC_DEVICE:            want_devices = 1; break;
            default: break;
            }
        }
    }

    /* Enumeration, not activation: nothing here opens an audio client, which
     * is what lets --dry-run be trusted. */
    if (want_apps)    (void)apr_enum_audio_apps(apps, MAX_APPS, &app_count);
    if (want_devices) (void)apr_enum_capture_endpoints(eps, MAX_ENDPOINTS, &ep_count);
    if (app_count > MAX_APPS)      app_count = MAX_APPS;
    if (ep_count  > MAX_ENDPOINTS) ep_count  = MAX_ENDPOINTS;

    for (bi = 0; bi < plan->bus_count; bi++) {
        AprCliBus *b = &plan->buses[bi];

        for (si = 0; si < b->source_count; si++) {
            AprCliSource *s = &b->sources[si];
            AprCliExit    rc = APR_CLI_OK;

            switch (s->kind) {
            case APR_CLI_SRC_PID:
            case APR_CLI_SRC_EXE:
                rc = resolve_process(&cx, s, apps, app_count);
                break;
            case APR_CLI_SRC_SYSTEM_MINUS_TREE:
                rc = resolve_process(&cx, s, apps, app_count);
                if (rc == APR_CLI_OK) warn_system_capture(&cx, s);
                break;
            case APR_CLI_SRC_DEVICE:
                rc = resolve_device(&cx, s, eps, ep_count);
                break;
            case APR_CLI_SRC_FAKE:
                /* Its label stays the spec it was given ("440,30"), because the
                 * kind is already printed beside it and repeating "generated by
                 * apprecorder (generated by apprecorder)" tells nobody which
                 * synthetic source this is. */
                break;
            }
            if (rc != APR_CLI_OK) return rc;

            /* Loopback sits after the session volume, so a muted application
             * records as pure silence while looking perfectly healthy. Say so
             * BEFORE the recording, which is the only moment it helps. */
            if (s->muted_now) {
                const wchar_t *args[1];
                args[0] = s->label;
                warn(&cx, APR_S_WARN_SOURCE_MUTED, args, 1);
            }
        }

        for (oi = 0; oi < b->output_count; oi++) {
            AprCliOutput          *o = &b->outputs[oi];
            const AprActionVTable *vt;
            const wchar_t         *args[2];
            AprErr                 e;

            if (o->action_id[0] == '\0') {
                const wchar_t *ext = extension_of(o->path);
                if (!ext) {
                    args[0] = o->path;
                    return fail(&cx, APR_CLI_CONFIG, APR_S_ERR_NO_EXTENSION, args, 1);
                }
                vt = action_for_extension(ext);
                if (!vt) {
                    args[0] = ext;
                    return fail(&cx, APR_CLI_CONFIG, APR_S_ERR_UNKNOWN_FORMAT, args, 1);
                }
            } else {
                vt = apr_action_find(o->action_id);
                if (!vt) {
                    wchar_t wide[16];
                    size_t  k;
                    for (k = 0; k + 1 < 16 && o->action_id[k]; k++)
                        wide[k] = (wchar_t)o->action_id[k];
                    wide[k] = L'\0';
                    args[0] = wide;
                    return fail(&cx, APR_CLI_CONFIG, APR_S_ERR_UNKNOWN_FORMAT,
                                args, 1);
                }
            }

            /* THE CANONICAL SPELLING, always, whichever way the format was
             * chosen. `--format WAV` now resolves -- apr_action_find matches
             * case-insensitively, exactly as the extension pass always did,
             * so `--out x.WAV` working while `--format WAV` was refused as "a
             * format this build cannot write" is over -- and writing the
             * vtable's own id back is what keeps a session file holding the
             * wire value rather than whatever case somebody typed. */
            strcpy_s(o->action_id, sizeof o->action_id, vt->id);

            /* ASK THE ENCODER BEFORE ANYTHING IS RECORDED.
             *
             * This is the check whose absence could cost a whole take.
             * `--bitrate 400 --out meeting.mp3 --duration 3600` parsed, passed
             * --dry-run, and then ran for an hour writing nothing, because the
             * only thing that knew LAME refuses 400 kbps was create() -- which
             * runs at apr_bus_start, where a refusal is downgraded to a
             * skipped output and the session carries on regardless. The
             * registry knows every action; now it can be asked (action.h).
             *
             * Exit 2: the command line was read and does not describe a
             * recording that can be made. */
            {
                AprActionConfig acfg;

                memset(&acfg, 0, sizeof acfg);
                acfg.sample_rate  = plan->rate;
                acfg.channels     = plan->channels;
                acfg.bitrate_kbps = o->bitrate_kbps;
                acfg.quality      = o->quality;

                e = apr_action_check_config(vt, &acfg);
                if (apr_failed(&e)) {
                    wchar_t why[512];
                    args[0] = o->path;
                    args[1] = errtext(&e, why, 512);
                    return fail(&cx, APR_CLI_CONFIG,
                                APR_S_ERR_OUTPUT_UNSUPPORTED, args, 2);
                }
            }

            /* Two recordings cannot share a file, and the comparison has to
             * be of the EXPANDED, resolved paths: "a.wav" and ".\a.wav" are
             * one file, while two buses both saving to "{bus}.wav" are not --
             * comparing the templates would refuse that second, correct, case
             * out of hand. */
            {
                wchar_t mine[APR_CLI_SPEC_CCH], full_mine[APR_CLI_SPEC_CCH];
                size_t  bj, oj;
                /* {n} MEANS "THE LOWEST FREE NUMBER", resolved against the disk
                 * at the instant each recording starts, so a template carrying
                 * one names a DIFFERENT file every time it is used -- and the
                 * comparison below cannot see that, because expanding without
                 * the disk turns every {n} into 1. Two buses both saving to
                 * "take{n}.wav" were therefore refused as a duplicate when
                 * they are precisely the case the token exists for. */
                int mine_numbered = apr_out_has_token(o->path, L"n");

                expanded_output_path(b, o, mine, APR_CLI_SPEC_CCH);
                full_path(mine, full_mine, APR_CLI_SPEC_CCH);

                for (bj = 0; bj <= bi && !mine_numbered; bj++) {
                    size_t limit = (bj == bi) ? oi : plan->buses[bj].output_count;
                    for (oj = 0; oj < limit; oj++) {
                        const AprCliOutput *other_o = &plan->buses[bj].outputs[oj];
                        wchar_t other[APR_CLI_SPEC_CCH], full_other[APR_CLI_SPEC_CCH];
                        if (apr_out_has_token(other_o->path, L"n")) continue;
                        expanded_output_path(&plan->buses[bj], other_o,
                                             other, APR_CLI_SPEC_CCH);
                        full_path(other, full_other, APR_CLI_SPEC_CCH);
                        if (ieq(full_mine, full_other)) {
                            args[0] = o->path;
                            return fail(&cx, APR_CLI_CONFIG,
                                        APR_S_ERR_DUPLICATE_OUTPUT, args, 1);
                        }
                    }
                }
            }

            /* THE EARLY CHECK, and it is the same one the bus makes when a UI
             * adds an output: expand the name and ask the folder whether
             * something may be created in it. Nothing is opened for the
             * recording here -- that happens at apr_bus_start() (bus.h). */
            {
                AprOutContext ctx;
                wchar_t shown[APR_CLI_SPEC_CCH];

                ctx.bus_name  = b->name;
                ctx.extension = vt->extension;
                e = apr_out_validate(o->path, &ctx, shown, APR_CLI_SPEC_CCH);
                if (apr_failed(&e)) {
                    wchar_t why[512];
                    args[0] = shown[0] ? shown : o->path;
                    args[1] = errtext(&e, why, 512);
                    return fail(&cx, APR_CLI_OUTPUT,
                                APR_S_ERR_OUTPUT_NOT_WRITABLE, args, 2);
                }
            }
        }
    }

    return APR_CLI_OK;
}

/* ---------------------------------------------------------------------------
 * help / version
 * ------------------------------------------------------------------------- */

static void print_usage(const Ctx *cx)
{
    static const AprStrId lines[] = {
        APR_S_APP_NAME, APR_S_APP_TAGLINE,
        (AprStrId)0,
        APR_S_CLI_USAGE_HEADER,
        (AprStrId)0,
        APR_S_CLI_COMMANDS_HEADER,
        APR_S_CLI_CMD_RECORD, APR_S_CLI_CMD_LIST_APPS, APR_S_CLI_CMD_LIST_DEVICES,
        APR_S_CLI_CMD_SAVE_SESSION, APR_S_CLI_CMD_HELP, APR_S_CLI_CMD_VERSION,
        (AprStrId)0,
        APR_S_CLI_SOURCES_HEADER,
        APR_S_CLI_OPT_BUS, APR_S_CLI_OPT_PID, APR_S_CLI_OPT_EXE,
        APR_S_CLI_OPT_DEVICE, APR_S_CLI_OPT_FAKE, APR_S_CLI_OPT_FAKE_HEALTH,
        APR_S_CLI_OPT_GAIN,
        APR_S_CLI_OPT_SYSTEM_MINUS_TREE,
        (AprStrId)0,
        APR_S_CLI_OUTPUTS_HEADER,
        APR_S_CLI_OPT_OUT, APR_S_CLI_OPT_OUT_TOKENS,
        APR_S_CLI_OPT_FORMAT, APR_S_CLI_OPT_BITRATE,
        APR_S_CLI_OPT_QUALITY,
        (AprStrId)0,
        APR_S_CLI_SESSION_HEADER,
        APR_S_CLI_OPT_RATE, APR_S_CLI_OPT_CHANNELS, APR_S_CLI_OPT_DURATION,
        APR_S_CLI_OPT_DRY_RUN, APR_S_CLI_OPT_JSON, APR_S_CLI_OPT_QUIET,
        APR_S_CLI_OPT_ALL, APR_S_CLI_OPT_LANG, APR_S_CLI_OPT_LOG_LEVEL,
        APR_S_CLI_OPT_LOG_FILE,
        APR_S_CLI_OPT_SESSION, APR_S_CLI_OPT_ALLOW_SYSTEM_CAPTURE,
        APR_S_CLI_OPT_ALLOW_MISSING,
        (AprStrId)0,
        APR_S_CLI_EXIT_HEADER,
        APR_S_CLI_EXIT_OK, APR_S_CLI_EXIT_USAGE, APR_S_CLI_EXIT_CONFIG,
        APR_S_CLI_EXIT_NOT_FOUND, APR_S_CLI_EXIT_OUTPUT, APR_S_CLI_EXIT_CAPTURE,
        APR_S_CLI_EXIT_INCOMPLETE, APR_S_CLI_EXIT_INTERNAL,
        (AprStrId)0,
        APR_S_CLI_EXAMPLES_HEADER,
        APR_S_CLI_EXAMPLE_ONE, APR_S_CLI_EXAMPLE_TWO,
        (AprStrId)0,
        APR_S_CLI_STOP_HINT
    };
    size_t i;

    for (i = 0; i < sizeof lines / sizeof lines[0]; i++) {
        if (lines[i] == 0) emit(cx, APR_CLI_STDOUT, L"");
        else               emit(cx, APR_CLI_STDOUT, apr_str(lines[i]));
    }
}

static AprCliExit do_version(const Ctx *cx)
{
    const wchar_t *args[1];
    args[0] = APR_CLI_VERSION;

    if (cx->json) {
        jline(cx, 0, L"{");
        jstr(cx, 1, L"name", apr_str(APR_S_APP_NAME), 1);
        jstr(cx, 1, L"version", APR_CLI_VERSION, 0);
        jline(cx, 0, L"}");
    } else {
        say(cx, APR_CLI_STDOUT, APR_S_CLI_VERSION_LINE, args, 1);
    }
    return APR_CLI_OK;
}

/* ---------------------------------------------------------------------------
 * Listings
 * ------------------------------------------------------------------------- */

static AprCliExit do_list_apps(const Ctx *cx, const AprCliPlan *plan)
{
    static AprAudioApp apps[MAX_APPS];
    size_t count = 0, i, shown = 0;
    AprErr e;

    e = apr_enum_audio_apps(apps, MAX_APPS, &count);
    if (apr_failed(&e)) {
        wchar_t why[512];
        const wchar_t *args[2];
        args[0] = apr_str(APR_S_APP_NAME);
        args[1] = errtext(&e, why, 512);
        return fail(cx, APR_CLI_INTERNAL, APR_S_ERR_CAPTURE_START, args, 2);
    }
    if (count > MAX_APPS) count = MAX_APPS;

    for (i = 0; i < count; i++) if (plan->all || apps[i].active) shown++;

    if (cx->json) {
        size_t emitted = 0;
        jline(cx, 0, L"{");
        jline(cx, 1, L"\"apps\": [");
        for (i = 0; i < count; i++) {
            if (!plan->all && !apps[i].active) continue;
            emitted++;
            jline(cx, 2, L"{");
            jnum(cx, 3, L"pid", (int64_t)apps[i].pid, 1);
            jstr(cx, 3, L"exe", apps[i].exe, 1);
            jstr(cx, 3, L"name", apps[i].display, 1);
            jstr(cx, 3, L"path", apps[i].path, 1);
            jbool(cx, 3, L"active", apps[i].active, 1);
            jbool(cx, 3, L"muted", apps[i].muted, 1);
            jreal(cx, 3, L"volume", (double)apps[i].volume, 0);
            /* The comma has to count what was EMITTED, not what was scanned:
             * filtering out the last row of the scan would otherwise leave a
             * trailing comma and a document no parser accepts. */
            jline(cx, 2, L"}%ls", (emitted < shown) ? L"," : L"");
        }
        jline(cx, 1, L"]");
        jline(cx, 0, L"}");
        return APR_CLI_OK;
    }

    if (shown == 0) {
        SAY0(cx, APR_CLI_STDOUT, APR_S_LIST_APPS_EMPTY);
        return APR_CLI_OK;
    }

    SAY0(cx, APR_CLI_STDOUT, APR_S_LIST_APPS_HEADER);
    for (i = 0; i < count; i++) {
        NumBuf nb;
        const wchar_t *args[2];
        AprStrId id;

        if (!plan->all && !apps[i].active) continue;
        args[0] = num(&nb, (int64_t)apps[i].pid);
        args[1] = apps[i].display[0] ? apps[i].display : apps[i].exe;
        id = apps[i].muted   ? APR_S_LIST_APPS_ROW_MUTED
           : !apps[i].active ? APR_S_LIST_APPS_ROW_IDLE
                             : APR_S_LIST_APPS_ROW;
        say(cx, APR_CLI_STDOUT, id, args, 2);
    }
    return APR_CLI_OK;
}

static AprCliExit do_list_devices(const Ctx *cx)
{
    static AprAudioEndpoint eps[MAX_ENDPOINTS];
    size_t count = 0, i;
    AprErr e;

    e = apr_enum_capture_endpoints(eps, MAX_ENDPOINTS, &count);
    if (apr_failed(&e)) {
        wchar_t why[512];
        const wchar_t *args[2];
        args[0] = apr_str(APR_S_APP_NAME);
        args[1] = errtext(&e, why, 512);
        return fail(cx, APR_CLI_INTERNAL, APR_S_ERR_CAPTURE_START, args, 2);
    }
    if (count > MAX_ENDPOINTS) count = MAX_ENDPOINTS;

    if (cx->json) {
        jline(cx, 0, L"{");
        jline(cx, 1, L"\"devices\": [");
        for (i = 0; i < count; i++) {
            jline(cx, 2, L"{");
            jstr(cx, 3, L"id", eps[i].id, 1);
            jstr(cx, 3, L"name", eps[i].name, 1);
            jbool(cx, 3, L"default", eps[i].is_default, 0);
            jline(cx, 2, L"}%ls", (i + 1 < count) ? L"," : L"");
        }
        jline(cx, 1, L"]");
        jline(cx, 0, L"}");
        return APR_CLI_OK;
    }

    if (count == 0) {
        SAY0(cx, APR_CLI_STDOUT, APR_S_LIST_DEVICES_EMPTY);
        return APR_CLI_OK;
    }
    SAY0(cx, APR_CLI_STDOUT, APR_S_LIST_DEVICES_HEADER);
    for (i = 0; i < count; i++) {
        const wchar_t *args[1];
        args[0] = eps[i].name[0] ? eps[i].name : eps[i].id;
        say(cx, APR_CLI_STDOUT,
            eps[i].is_default ? APR_S_LIST_DEVICES_ROW_DEFAULT
                              : APR_S_LIST_DEVICES_ROW, args, 1);
        args[0] = eps[i].id;
        say_indented(cx, APR_CLI_STDOUT, 2, APR_S_LIST_DEVICES_ID_LINE, args, 1);
    }
    return APR_CLI_OK;
}

/* ---------------------------------------------------------------------------
 * --dry-run
 * ------------------------------------------------------------------------- */

static AprStrId kind_string(AprCliSourceKind k)
{
    switch (k) {
    case APR_CLI_SRC_DEVICE:              return APR_S_SOURCE_KIND_DEVICE;
    case APR_CLI_SRC_FAKE:                return APR_S_SOURCE_KIND_FAKE;
    case APR_CLI_SRC_SYSTEM_MINUS_TREE:   return APR_S_SOURCE_KIND_SYSTEM_MINUS_TREE;
    default:                              return APR_S_SOURCE_KIND_PROCESS;
    }
}

static const char *kind_wire(AprCliSourceKind k)
{
    switch (k) {
    case APR_CLI_SRC_DEVICE:            return "device";
    case APR_CLI_SRC_FAKE:              return "fake";
    case APR_CLI_SRC_SYSTEM_MINUS_TREE: return "systemMinusTree";
    default:                            return "process";
    }
}

static void wide_of(const char *s, wchar_t *out, size_t cch)
{
    size_t i;
    for (i = 0; i + 1 < cch && s[i]; i++) out[i] = (wchar_t)s[i];
    out[i] = L'\0';
}

static AprCliExit do_dry_run(const Ctx *cx, const AprCliPlan *p)
{
    size_t bi, si, oi;

    if (cx->json) {
        jline(cx, 0, L"{");
        jbool(cx, 1, L"ok", 1, 1);
        jbool(cx, 1, L"dryRun", 1, 1);
        jnum(cx, 1, L"exitCode", 0, 1);
        jnum(cx, 1, L"sampleRate", (int64_t)p->rate, 1);
        jnum(cx, 1, L"channels", (int64_t)p->channels, 1);
        jnum(cx, 1, L"durationMs", p->duration_ms, 1);
        jline(cx, 1, L"\"buses\": [");
        for (bi = 0; bi < p->bus_count; bi++) {
            const AprCliBus *b = &p->buses[bi];
            jline(cx, 2, L"{");
            jstr(cx, 3, L"name", b->name, 1);
            jline(cx, 3, L"\"sources\": [");
            for (si = 0; si < b->source_count; si++) {
                const AprCliSource *s = &b->sources[si];
                wchar_t kw[32];
                wide_of(kind_wire(s->kind), kw, 32);
                jline(cx, 4, L"{");
                jstr(cx, 5, L"kind", kw, 1);
                jstr(cx, 5, L"spec", s->spec, 1);
                jstr(cx, 5, L"label", s->label, 1);
                jnum(cx, 5, L"pid", (int64_t)s->pid, 1);
                jstr(cx, 5, L"endpointId", s->endpoint_id, 1);
                jbool(cx, 5, L"muted", s->muted_now, 1);
                jreal(cx, 5, L"gainDb", (double)s->gain_db_tenths / 10.0, 0);
                jline(cx, 4, L"}%ls", (si + 1 < b->source_count) ? L"," : L"");
            }
            jline(cx, 3, L"],");
            jline(cx, 3, L"\"outputs\": [");
            for (oi = 0; oi < b->output_count; oi++) {
                const AprCliOutput *o = &b->outputs[oi];
                wchar_t fw[16];
                wide_of(o->action_id, fw, 16);
                jline(cx, 4, L"{");
                jstr(cx, 5, L"path", o->path, 1);
                jstr(cx, 5, L"format", fw, 1);
                jnum(cx, 5, L"bitrateKbps", o->bitrate_kbps, 1);
                jnum(cx, 5, L"quality", o->quality, 0);
                jline(cx, 4, L"}%ls", (oi + 1 < b->output_count) ? L"," : L"");
            }
            jline(cx, 3, L"]");
            jline(cx, 2, L"}%ls", (bi + 1 < p->bus_count) ? L"," : L"");
        }
        jline(cx, 1, L"]");
        jline(cx, 0, L"}");
        return APR_CLI_OK;
    }

    SAY0(cx, APR_CLI_STDOUT, APR_S_STATUS_DRY_RUN_HEADER);
    {
        NumBuf a, b;
        const wchar_t *args[2];
        args[0] = num(&a, (int64_t)p->rate);
        args[1] = num(&b, (int64_t)p->channels);
        say(cx, APR_CLI_STDOUT, APR_S_STATUS_PLAN_SESSION, args, 2);
    }
    for (bi = 0; bi < p->bus_count; bi++) {
        const AprCliBus *b = &p->buses[bi];
        const wchar_t *args[3];

        args[0] = b->name;
        say(cx, APR_CLI_STDOUT, APR_S_STATUS_PLAN_BUS, args, 1);

        for (si = 0; si < b->source_count; si++) {
            const AprCliSource *s = &b->sources[si];
            NumBuf g;
            args[0] = s->label;
            args[1] = apr_str(kind_string(s->kind));
            args[2] = fixed(&g, s->gain_db_tenths, 1);
            say_indented(cx, APR_CLI_STDOUT, 1, APR_S_STATUS_PLAN_SOURCE, args, 3);
        }
        for (oi = 0; oi < b->output_count; oi++) {
            const AprCliOutput *o = &b->outputs[oi];
            wchar_t fw[16];
            wide_of(o->action_id, fw, 16);
            args[0] = o->path;
            args[1] = fw;
            say_indented(cx, APR_CLI_STDOUT, 1, APR_S_STATUS_PLAN_OUTPUT, args, 2);
        }
    }
    SAY0(cx, APR_CLI_STDOUT, APR_S_STATUS_DRY_RUN_OK);
    return APR_CLI_OK;
}

/* ---------------------------------------------------------------------------
 * Recording
 * ------------------------------------------------------------------------- */

/* Per-source health bookkeeping used to live here. It belongs to the loop, so
 * it now lives in core/runner.c and is reachable through the runner's snapshot
 * -- which is also what lets the UI draw it without touching the graph from
 * another thread. */
typedef struct RunState {
    AprGraph *g;
    AprBusId  bus_ids[APR_MAX_BUSES];
    int       incomplete;
    uint64_t  frames_out[APR_MAX_BUSES];

    /* WHICH OUTPUTS NEVER OPENED A FILE, which is not the same question as
     * "which outputs are marked failed" and the difference decides both the
     * exit code and what the summary is allowed to claim.
     *
     * The runner announces a failed action twice over the life of a run: once
     * from the poll it makes BEFORE APR_RUN_EV_STARTED, which can only mean
     * the file would not open, and again later for anything that went wrong
     * while recording or while closing. Only the first kind means there is no
     * file. Watching where the notice falls relative to STARTED is how this
     * tells them apart -- the bus exposes one `failed` flag for both. */
    unsigned char never_opened[APR_MAX_BUSES][APR_CLI_MAX_OUTPUTS_PER_BUS];
    int           never_opened_count;
} RunState;

static AprCliExit build_graph(const Ctx *cx, const AprCliPlan *p, RunState *st)
{
    size_t bi, si, oi;
    AprErr e;

    e = apr_graph_create(p->rate, p->channels, &st->g);
    if (apr_failed(&e)) {
        wchar_t why[512];
        const wchar_t *args[2];
        args[0] = apr_str(APR_S_APP_NAME);
        args[1] = errtext(&e, why, 512);
        return fail(cx, APR_CLI_INTERNAL, APR_S_ERR_CAPTURE_START, args, 2);
    }

    for (bi = 0; bi < p->bus_count; bi++) {
        const AprCliBus *b = &p->buses[bi];

        e = apr_graph_add_bus(st->g, b->name, &st->bus_ids[bi]);
        if (apr_failed(&e)) {
            wchar_t why[512];
            const wchar_t *args[2];
            args[0] = b->name;
            args[1] = errtext(&e, why, 512);
            return fail(cx, APR_CLI_CONFIG, APR_S_ERR_CAPTURE_START, args, 2);
        }

        for (si = 0; si < b->source_count; si++) {
            const AprCliSource *s = &b->sources[si];
            AprCaptureConfig    cfg;
            AprSourceId         sid = 0;

            memset(&cfg, 0, sizeof cfg);
            switch (s->kind) {
            case APR_CLI_SRC_DEVICE:
                cfg.kind = APR_SRC_DEVICE;
                cfg.device.endpoint_id = s->endpoint_id;
                break;
            case APR_CLI_SRC_FAKE:
                cfg.kind = APR_SRC_FAKE;
                cfg.fake.tone_hz        = s->fake_hz;
                cfg.fake.rate_error_ppm = s->fake_ppm;
                cfg.fake.amplitude      = s->fake_amp;
                cfg.fake.mute_at_frame   = s->fake_mute_at;
                cfg.fake.unmute_at_frame = s->fake_unmute_at;
                cfg.fake.die_at_frame    = s->fake_die_at;
                cfg.fake.start_muted     = s->fake_start_muted;
                cfg.fake.start_dead      = s->fake_start_dead;
                break;
            case APR_CLI_SRC_SYSTEM_MINUS_TREE:
                cfg.kind = APR_SRC_PROCESS;
                cfg.process.pid     = s->pid;
                cfg.process.exclude = 1;
                break;
            default:
                cfg.kind = APR_SRC_PROCESS;
                cfg.process.pid     = s->pid;
                cfg.process.exclude = 0;
                break;
            }

            e = apr_graph_add_source(st->g, s->label, &cfg, &sid);
            if (apr_failed(&e)) {
                wchar_t why[512];
                const wchar_t *args[2];
                args[0] = s->label;
                args[1] = errtext(&e, why, 512);
                return fail(cx, APR_CLI_CAPTURE, APR_S_ERR_CAPTURE_START, args, 2);
            }
            e = apr_graph_connect(st->g, sid, st->bus_ids[bi], s->gain);
            if (apr_failed(&e)) {
                const wchar_t *args[2];
                args[0] = s->label;
                args[1] = b->name;
                return fail(cx, APR_CLI_CONFIG, APR_S_ERR_SOURCE_ALREADY_ON_BUS,
                            args, 2);
            }
        }

        for (oi = 0; oi < b->output_count; oi++) {
            const AprCliOutput *o = &b->outputs[oi];
            AprActionConfig     acfg;

            memset(&acfg, 0, sizeof acfg);
            acfg.out_path     = o->path;
            acfg.sample_rate  = p->rate;
            acfg.channels     = p->channels;
            acfg.bitrate_kbps = o->bitrate_kbps;
            acfg.quality      = o->quality;

            e = apr_graph_add_action(st->g, st->bus_ids[bi], o->action_id, &acfg);
            if (apr_failed(&e)) {
                wchar_t why[512];
                const wchar_t *args[2];
                args[0] = o->path;
                args[1] = errtext(&e, why, 512);
                return fail(cx, APR_CLI_OUTPUT, APR_S_ERR_FILE_OPEN, args, 2);
            }
        }
    }
    return APR_CLI_OK;
}

/* ---------------------------------------------------------------------------
 * THE LOOP ITSELF LIVES IN core/runner.c, NOT HERE.
 *
 * It used to be twenty lines in this file, and they are the twenty lines the
 * UI needs most: arm, anchor at one QPC instant, tick, WAIT OUT THE MIXER'S
 * LOOKBEHIND so the final block is not thrown away, then finalize on every
 * exit path. A second front end copying them is the DRY failure AGENTS.md
 * rule 3 names, and the copy would go wrong SILENTLY -- a recording missing
 * its last block still opens and still sounds like a recording.
 *
 * What stays here is what is genuinely the command line's: turning the
 * runner's notices into the sentences and the exit codes a script branches on.
 * The order those sentences come out in is the runner's event order, and it is
 * exactly the order this file produced before the extraction. tests/test_cli.c
 * is what proves that.
 * ------------------------------------------------------------------------- */

typedef struct RunObs {
    const Ctx        *cx;
    const AprCliPlan *p;
    const AprGraph   *g;
    RunState         *st;
    int               started;    /* APR_RUN_EV_STARTED has been seen   */
    int               finishing;  /* APR_RUN_EV_FINISHING has been seen */
} RunObs;

/* The file an output is writing, or -- before it opens one, and for the
 * built-in sink that writes none -- the name that was asked for. One helper,
 * because "which file do we name to the user" must have one answer whether it
 * is being asked while recording or in the closing summary. */
static const wchar_t *written_path(const AprBus *b, size_t index)
{
    const wchar_t *real = apr_bus_action_current_path(b, index);
    return real[0] ? real : apr_bus_action_path(b, index);
}

/* THE FILE, NOT THE TEMPLATE. This used to print the plan's `--out` argument,
 * which was the same string as the filename right up until an output path
 * became a template and the collision policy became able to move a take aside
 * (outpath.h). What a script parses and what a person goes looking for is the
 * name on disk, so that is what is printed; the graph is the only thing that
 * knows it. */
static void say_output_lines(const RunObs *o, AprStrId id)
{
    size_t bi, oi;

    for (bi = 0; o->g && bi < apr_graph_bus_count(o->g); bi++) {
        AprBus *b = apr_graph_bus_at(o->g, bi);
        if (!b) continue;
        for (oi = 0; oi < apr_bus_action_count(b); oi++) {
            const wchar_t *args[1];
            args[0] = written_path(b, oi);
            if (!args[0][0]) continue;
            note(o->cx, id, args, 1);
        }
    }
}

/* Called on the thread running the loop, which for the CLI is this one. */
static void cli_observer(void *user, const AprRunNotice *n)
{
    RunObs *o = (RunObs *)user;
    wchar_t why[512];
    const wchar_t *args[2];

    if (!o || !n) return;

    switch (n->ev) {
    case APR_RUN_EV_ARM_FAILED:
        args[0] = apr_str(APR_S_APP_NAME);
        args[1] = errtext(&n->err, why, 512);
        /* One source failing to arm does not stop a session (design 10), but
         * it has to be said out loud. */
        warn(o->cx, APR_S_ERR_CAPTURE_START, args, 2);
        break;

    case APR_RUN_EV_STARTED:
        o->started = 1;
        say_output_lines(o, APR_S_STATUS_RECORDING_TO);
        if (!o->p->duration_ms && !o->cx->quiet && !o->cx->json)
            SAY0(o->cx, APR_CLI_STDOUT, APR_S_CLI_STOP_HINT);
        break;

    case APR_RUN_EV_SOURCE_DIED:
        args[0] = n->name;
        warn(o->cx, APR_S_WARN_SOURCE_EXITED, args, 1);
        break;

    case APR_RUN_EV_SOURCE_MUTED:
        args[0] = n->name;
        warn(o->cx, APR_S_WARN_SOURCE_MUTED, args, 1);
        break;

    case APR_RUN_EV_ACTION_FAILED:
        /* BEFORE "started" means the file never opened. apr_graph_start has
         * already run by then, so this is the create() that refused, and there
         * is nothing on disk under that name -- which is what stops the
         * summary below reporting an hour of audio into a file that does not
         * exist. */
        if (!o->started && o->st &&
            n->bus_index < APR_MAX_BUSES &&
            n->action_index < APR_CLI_MAX_OUTPUTS_PER_BUS &&
            !o->st->never_opened[n->bus_index][n->action_index])
        {
            o->st->never_opened[n->bus_index][n->action_index] = 1;
            o->st->never_opened_count++;
        }
        args[0] = n->name;
        args[1] = errtext(&n->err, why, 512);
        if (o->finishing) {
            /* After "finishing" the file is written and closed: what failed
             * was the close, or the encoder is reporting that the disk fell
             * behind and part of the take is silence. "Stopped taking audio"
             * would be untrue, and being told nothing at all is how a
             * four-second disk stall used to pass for a clean recording. */
            args[0] = n->path[0] ? n->path : n->name;
            warn(o->cx, APR_S_WARN_OUTPUT_DEGRADED, args, 2);
        } else {
            warn(o->cx, APR_S_WARN_ACTION_FAILED, args, 2);
        }
        break;

    case APR_RUN_EV_OUTPUT_RENAMED:
        /* The take already on disk was kept and this one moved aside. Said
         * out loud, at the moment it happens, because a rename nobody hears
         * about is only a politer kind of surprise (outpath.h). */
        args[0] = n->path;
        warn(o->cx, APR_S_WARN_OUTPUT_RENAMED, args, 1);
        break;

    case APR_RUN_EV_FINISHING:
        o->finishing = 1;
        say_output_lines(o, APR_S_STATUS_FINISHING);
        break;

    default:
        break;
    }
}

static AprCliExit record_loop(const Ctx *cx, const AprCliPlan *p, RunState *st)
{
    AprRunnerConfig cfg;
    AprRunner      *r = NULL;
    RunObs          obs;
    AprErr          e;
    size_t          bi;

    memset(&obs, 0, sizeof obs);
    obs.cx = cx;
    obs.p  = p;
    obs.g  = st->g;
    obs.st = st;

    memset(&cfg, 0, sizeof cfg);
    cfg.graph       = st->g;
    cfg.duration_ms = p->duration_ms;
    /* The console control handler is installed before any runner exists and
     * has to be able to stop one that does not yet, so it signals a shared
     * event rather than calling in. Handing that event to the runner is what
     * keeps Ctrl+C one mechanism instead of two. */
    cfg.stop_event  = g_stop_event;
    cfg.observer    = cli_observer;
    cfg.user        = &obs;

    e = apr_runner_create(&cfg, &r);
    if (apr_failed(&e)) {
        wchar_t why[512];
        const wchar_t *args[2];
        args[0] = apr_str(APR_S_APP_NAME);
        args[1] = errtext(&e, why, 512);
        return fail(cx, APR_CLI_INTERNAL, APR_S_ERR_CAPTURE_START, args, 2);
    }

    e = apr_runner_run(r);
    if (apr_failed(&e)) {
        wchar_t why[512];
        const wchar_t *args[2];
        args[0] = apr_str(APR_S_APP_NAME);
        args[1] = errtext(&e, why, 512);
        apr_runner_destroy(r);
        return fail(cx, APR_CLI_INTERNAL, APR_S_ERR_CAPTURE_START, args, 2);
    }

    for (bi = 0; bi < p->bus_count && bi < APR_MAX_BUSES; bi++) {
        st->frames_out[bi] = apr_runner_bus_frames(r, st->bus_ids[bi]);
    }
    if (apr_runner_incomplete(r)) st->incomplete = 1;

    apr_runner_destroy(r);
    return APR_CLI_OK;
}


/* WHAT WAS ACTUALLY WRITTEN, read out of the graph rather than out of the
 * plan. The graph is still alive here -- do_record destroys it in its
 * __finally, after this -- and it is the only thing that knows what each
 * output's name expanded to and whether the collision policy moved it. A
 * summary that named the template would send the user looking for a file that
 * is not there. */
/* Did this output ever open a file? Reads the map the observer filled while
 * the run was still going, because after apr_bus_stop the bus's own `failed`
 * flag no longer distinguishes "never opened" from "closed badly". */
static int output_never_opened(const RunState *st, size_t bi, size_t oi)
{
    if (bi >= APR_MAX_BUSES || oi >= APR_CLI_MAX_OUTPUTS_PER_BUS) return 0;
    return st->never_opened[bi][oi] != 0;
}

static size_t output_count_total(const RunState *st)
{
    size_t bi, n = 0;
    for (bi = 0; bi < apr_graph_bus_count(st->g); bi++) {
        AprBus *b = apr_graph_bus_at(st->g, bi);
        if (b) n += apr_bus_action_count(b);
    }
    return n;
}

/* NOTHING WAS RECORDED AT ALL, which is a different outcome from "something
 * went wrong" and must not wear its clothes.
 *
 * A run in which every single output failed to open used to end INCOMPLETE --
 * documented as "recorded and playable" -- with a --json document listing each
 * output's path beside the full duration of the run. An hour of `"seconds":
 * 3600` for a file that was never created. A script branching on exit 6 keeps
 * going; a person reading the report goes looking for a file that is not
 * there; and the audio is gone either way.
 *
 * Exit 4 is what this is: "a file could not be created, or could not be closed
 * properly." It is the code the same failure already gets when it is caught
 * before recording starts (apr_cli_resolve), so a script branches on one thing
 * whichever side of the start it happened. Note the asymmetry with a run where
 * SOME outputs opened: that is exit 6 and correct, because there is a playable
 * recording -- just not all of the one that was asked for. */
static int nothing_was_written(const RunState *st)
{
    size_t total = output_count_total(st);
    return total > 0 && (size_t)st->never_opened_count >= total;
}

static void report_result(const Ctx *cx, const AprCliPlan *p, RunState *st,
                          AprCliExit code)
{
    size_t bi, oi;
    int    empty = nothing_was_written(st);

    if (cx->json) {
        jline(cx, 0, L"{");
        jbool(cx, 1, L"ok", code == APR_CLI_OK, 1);
        jnum(cx, 1, L"exitCode", (int64_t)code, 1);
        jbool(cx, 1, L"dryRun", 0, 1);
        if (empty) {
            wchar_t msg[LINE_CCH];
            apr_str_format(APR_S_ERR_NOTHING_WAS_WRITTEN, msg, LINE_CCH, NULL, 0);
            jstr(cx, 1, L"error", msg, 1);
        }
        jline(cx, 1, L"\"outputs\": [");
        for (bi = 0; bi < apr_graph_bus_count(st->g); bi++) {
            AprBus *b = apr_graph_bus_at(st->g, bi);
            if (!b) continue;
            for (oi = 0; oi < apr_bus_action_count(b); oi++) {
                const AprActionVTable *vt = apr_bus_action_at(b, oi);
                wchar_t fw[16];
                int gone = output_never_opened(st, bi, oi);
                int last = (bi + 1 == apr_graph_bus_count(st->g)) &&
                           (oi + 1 == apr_bus_action_count(b));
                wide_of(vt && vt->id ? vt->id : "", fw, 16);
                jline(cx, 2, L"{");
                jstr(cx, 3, L"path", written_path(b, oi), 1);
                jstr(cx, 3, L"format", fw, 1);
                jstr(cx, 3, L"bus", apr_bus_name(b), 1);
                /* An output with no file is said so in its own entry, and its
                 * duration is zero rather than the length of the run. A script
                 * that reads `seconds` and goes looking for the file has to be
                 * able to trust both. */
                jbool(cx, 3, L"failed", gone, 1);
                jreal(cx, 3, L"seconds",
                      (gone || !p->rate)
                          ? 0.0
                          : (double)st->frames_out[bi] / (double)p->rate, 0);
                jline(cx, 2, L"}%ls", last ? L"" : L",");
            }
        }
        jline(cx, 1, L"]");
        jline(cx, 0, L"}");
        return;
    }

    if (empty) SAY0(cx, APR_CLI_STDERR, APR_S_ERR_NOTHING_WAS_WRITTEN);

    for (bi = 0; bi < apr_graph_bus_count(st->g); bi++) {
        AprBus *b = apr_graph_bus_at(st->g, bi);
        if (!b) continue;
        for (oi = 0; oi < apr_bus_action_count(b); oi++) {
            NumBuf sb;
            const wchar_t *args[2];
            int64_t ms = p->rate
                ? (int64_t)apr_mul_div_u64(st->frames_out[bi], 1000u, p->rate, NULL)
                : 0;
            /* "Wrote x, 3600.000 seconds" about a file that was never created
             * is the sentence this whole guard exists to delete. It was
             * already announced as a failure when the run started. */
            if (output_never_opened(st, bi, oi)) continue;
            args[0] = written_path(b, oi);
            args[1] = fixed(&sb, ms, 3);
            if (!args[0][0]) continue;
            note(cx, APR_S_STATUS_WROTE, args, 2);
        }
    }
    if (!cx->quiet && !empty) SAY0(cx, APR_CLI_STDOUT, APR_S_STATUS_STOPPED);
}

static AprCliExit do_record(const Ctx *cx, const AprCliPlan *p)
{
    static RunState st;      /* static: APR_MAX_SOURCES of bookkeeping */
    AprCliExit rc;

    memset(&st, 0, sizeof st);
    stop_signal_open();

    /* ONE place a graph is destroyed, and it is a __finally. apr_graph_destroy
     * stops every bus, which finalizes every action, before it frees anything
     * -- so the "a killed recording must not leave an unplayable file" rule is
     * a property of the control flow rather than of remembering to call
     * something. main() supplies the __except that makes an access violation
     * unwind through here rather than skip it. */
    __try {
        rc = build_graph(cx, p, &st);
        if (rc == APR_CLI_OK) rc = record_loop(cx, p, &st);
        if (rc == APR_CLI_OK && st.incomplete) rc = APR_CLI_INCOMPLETE;
        /* A run that opened no file at all did not record anything, whatever
         * else went right -- see nothing_was_written(). */
        if ((rc == APR_CLI_OK || rc == APR_CLI_INCOMPLETE) &&
            nothing_was_written(&st))
        {
            rc = APR_CLI_OUTPUT;
        }
        if (rc == APR_CLI_OK || rc == APR_CLI_INCOMPLETE || rc == APR_CLI_OUTPUT)
            report_result(cx, p, &st, rc);
    }
    __finally {
        apr_graph_destroy(st.g);
        st.g = NULL;
        stop_signal_close();
    }

    return rc;
}

/* ---------------------------------------------------------------------------
 * Session files
 *
 * Two directions over one flag: `record --session f` reads it, `save-session
 * --session f` writes it. Which one is the command's job to say.
 *
 * THE INTERESTING PART IS NEITHER OF THOSE. It is that opening a session is a
 * SEARCH -- process ids do not survive a reboot, so every source has to be
 * found again, and it can be found in ways that are not what the file asked
 * for. session.h owns that search and hands back a report; everything below
 * is the front end turning that report into sentences and one exit code.
 *
 * The exit codes are the existing ones, deliberately, rather than a parallel
 * scheme for sessions:
 *
 *   2 CONFIG      the file was read and cannot be turned into a recording:
 *                 malformed, truncated, not ours, from a reader we are not,
 *                 or asking for system-wide capture without consent.
 *   3 NOT_FOUND   the file is not there, or something it names is not there.
 *   4 OUTPUT      save-session could not write it.
 *   6 INCOMPLETE  --allow-missing was given and something was in fact missing:
 *                 every file is playable, and it is not what was asked for.
 * ------------------------------------------------------------------------- */

/* Static: an AprSession is a few hundred kilobytes of fixed arrays, for the
 * same reason AprCliPlan is (session.h), and a local one overflows the
 * default 1 MB stack. */
static AprSession              g_session;
static AprSessionLoadReport    g_load_report;
static AprSessionResolveReport g_resolve_report;

static AprStrId session_fault_string(AprSessionFault f)
{
    switch (f) {
    case APR_SESSION_FAULT_FILE_MISSING:    return APR_S_ERR_SESSION_NOT_FOUND;
    case APR_SESSION_FAULT_FILE_UNREADABLE: return APR_S_ERR_SESSION_UNREADABLE;
    case APR_SESSION_FAULT_TOO_LARGE:       return APR_S_ERR_SESSION_TOO_MANY;
    case APR_SESSION_FAULT_NOT_JSON:        return APR_S_ERR_SESSION_NOT_JSON;
    case APR_SESSION_FAULT_TRUNCATED:       return APR_S_ERR_SESSION_TRUNCATED;
    case APR_SESSION_FAULT_NOT_A_SESSION:   return APR_S_ERR_SESSION_NOT_A_SESSION;
    case APR_SESSION_FAULT_TOO_NEW:         return APR_S_ERR_SESSION_TOO_NEW;
    case APR_SESSION_FAULT_TOO_OLD:         return APR_S_ERR_SESSION_TOO_OLD;
    case APR_SESSION_FAULT_BAD_TYPE:        return APR_S_ERR_SESSION_BAD_FIELD;
    case APR_SESSION_FAULT_BAD_VALUE:       return APR_S_ERR_SESSION_BAD_VALUE;
    case APR_SESSION_FAULT_TOO_MANY:        return APR_S_ERR_SESSION_TOO_MANY;
    case APR_SESSION_FAULT_DANGLING_REF:    return APR_S_ERR_SESSION_DANGLING_REF;
    default:                                return APR_S_ERR_SESSION_NOT_A_SESSION;
    }
}

/* Every load fault is exit 2 except "the file is not there", which is the
 * absent-named-thing code the rest of the CLI already uses. */
static AprCliExit session_fault_code(AprSessionFault f)
{
    return (f == APR_SESSION_FAULT_FILE_MISSING) ? APR_CLI_NOT_FOUND
                                                 : APR_CLI_CONFIG;
}

static AprCliExit report_load_fault(const Ctx *cx, const AprCliPlan *p,
                                    const AprSessionLoadReport *rep,
                                    const AprErr *e)
{
    const wchar_t *args[3];
    NumBuf a, b;
    wchar_t why[512];

    args[0] = p->session_file;
    switch (rep->fault) {
    case APR_SESSION_FAULT_TOO_NEW:
        args[1] = num(&a, rep->min_reader);
        args[2] = num(&b, APR_SESSION_FORMAT_VERSION);
        return fail(cx, APR_CLI_CONFIG, APR_S_ERR_SESSION_TOO_NEW, args, 3);
    case APR_SESSION_FAULT_TOO_OLD:
        args[1] = num(&a, rep->version);
        return fail(cx, APR_CLI_CONFIG, APR_S_ERR_SESSION_TOO_OLD, args, 2);
    case APR_SESSION_FAULT_BAD_TYPE:
    case APR_SESSION_FAULT_BAD_VALUE:
    case APR_SESSION_FAULT_TOO_MANY:
    case APR_SESSION_FAULT_DANGLING_REF:
        /* `where` is a JSON path -- "buses[1].sources[0].gainDb" -- which is
         * the difference between a person finding the line and not. */
        args[1] = rep->where;
        return fail(cx, session_fault_code(rep->fault),
                    session_fault_string(rep->fault), args, 2);
    case APR_SESSION_FAULT_FILE_UNREADABLE:
        args[1] = errtext(e, why, 512);
        return fail(cx, APR_CLI_CONFIG, APR_S_ERR_SESSION_UNREADABLE, args, 2);
    default:
        return fail(cx, session_fault_code(rep->fault),
                    session_fault_string(rep->fault), args, 1);
    }
}

/* One resolution, said out loud. Substitutions are warnings -- the recording
 * proceeds and the user is told what was swapped -- and failures are either a
 * warning (under --allow-missing) or the reason the run stops. */
static void say_resolution(const Ctx *cx, const AprCliPlan *p,
                           const AprSession *s,
                           const AprSessionResolution *r)
{
    const AprSessionSource *src = &s->sources[r->source_index];
    const wchar_t *args[3];
    NumBuf nb;

    switch (r->status) {
    case APR_SESSION_MOVED:
        args[0] = r->name;
        args[1] = r->substituted;
        args[2] = r->wanted;
        warn(cx, APR_S_WARN_SESSION_MOVED, args, 3);
        break;
    case APR_SESSION_BY_WINDOW_CLASS:
        args[0] = r->name;
        args[1] = num(&nb, (int64_t)r->chosen_pid);
        warn(cx, APR_S_WARN_SESSION_BY_WINDOW_CLASS, args, 2);
        break;
    case APR_SESSION_FIRST_OF_MANY:
        args[0] = r->name;
        args[1] = num(&nb, (int64_t)r->chosen_pid);
        warn(cx, APR_S_WARN_SESSION_FIRST_OF_MANY, args, 2);
        break;
    case APR_SESSION_DEVICE_BY_NAME:
        args[0] = r->name;
        warn(cx, APR_S_WARN_SESSION_DEVICE_BY_NAME, args, 1);
        break;

    case APR_SESSION_NOT_RUNNING:
        args[0] = r->name;
        args[1] = r->wanted;
        warn(cx, APR_S_ERR_SESSION_SOURCE_MISSING, args, 2);
        break;
    case APR_SESSION_DEVICE_ABSENT:
        /* r->wanted is the FRIENDLY name, never the endpoint GUID. That is
         * the whole reason both halves are stored. */
        args[0] = r->wanted;
        warn(cx, APR_S_ERR_SESSION_DEVICE_MISSING, args, 1);
        break;
    case APR_SESSION_AMBIGUOUS: {
        wchar_t list[LINE_CCH];
        size_t  i;
        args[0] = r->name;
        warn(cx, APR_S_ERR_SESSION_AMBIGUOUS, args, 1);
        /* Refusing is only defensible with the answer attached: name every
         * rival so the user can pin one with --pid. */
        list[0] = L'\0';
        for (i = 0; i < r->candidates_listed; i++) {
            wchar_t one[48];
            _snwprintf_s(one, 48, _TRUNCATE, L"%ls%ls",
                         list[0] ? L", " : L"",
                         num(&nb, (int64_t)r->candidates[i].pid));
            wcsncat_s(list, LINE_CCH, one, _TRUNCATE);
        }
        if (list[0]) {
            args[0] = list;
            warn(cx, APR_S_ERR_SESSION_AMBIGUOUS_PIDS, args, 1);
        }
        break;
    }
    case APR_SESSION_NEEDS_CONSENT:
        args[0] = p->session_file;
        args[1] = r->name;
        warn(cx, APR_S_ERR_SESSION_NEEDS_CONSENT, args, 2);
        break;

    default:
        break;
    }

    if (!apr_session_status_usable(r->status) && p->allow_missing &&
        src->kind != APR_SESSION_SRC_SYSTEM_MINUS_TREE)
    {
        args[0] = r->name;
        warn(cx, APR_S_WARN_SESSION_DROPPED, args, 1);
    }
}

/* The loaded, resolved session becomes an ordinary plan, so that everything
 * downstream -- apr_cli_resolve, --dry-run, do_record -- is the code path a
 * typed command line already takes. A second builder would be a second set of
 * bugs. */
static void plan_from_session(AprCliPlan *p, const AprSession *s)
{
    size_t bi, ei, oi;

    if (!p->explicit_rate)     p->rate        = s->sample_rate;
    if (!p->explicit_channels) p->channels    = s->channels;
    if (!p->explicit_duration) p->duration_ms = s->duration_ms;

    p->bus_count = 0;
    for (bi = 0; bi < s->bus_count && bi < APR_MAX_BUSES; bi++) {
        const AprSessionBus *sb = &s->buses[bi];
        AprCliBus *b = &p->buses[p->bus_count++];

        memset(b, 0, sizeof *b);
        copy_cch(b->name, APR_NAME_CCH, sb->name);

        for (ei = 0; ei < sb->edge_count; ei++) {
            const AprSessionSource *ss;
            AprCliSource *cs;

            if (sb->edges[ei].source_index >= s->source_count) continue;
            ss = &s->sources[sb->edges[ei].source_index];

            /* A source that could not be found is left out HERE and nowhere
             * else, so that "dropped" is one decision in one place. Reaching
             * this line at all means the caller already agreed to it. */
            if (!ss->resolved) continue;
            if (b->source_count >= APR_MAX_SOURCES_PER_BUS) continue;

            cs = &b->sources[b->source_count++];
            memset(cs, 0, sizeof *cs);
            cs->gain_db_tenths = sb->edges[ei].gain_db_tenths;
            cs->gain = (float)pow(10.0, (double)cs->gain_db_tenths / 200.0);
            copy_cch(cs->label, APR_NAME_CCH,
                     ss->resolved_label[0] ? ss->resolved_label : ss->name);

            switch (ss->kind) {
            case APR_SESSION_SRC_DEVICE:
                cs->kind = APR_CLI_SRC_DEVICE;
                copy_cch(cs->endpoint_id, APR_DISC_ENDPOINT_CCH,
                         ss->resolved_endpoint_id);
                copy_cch(cs->spec, APR_CLI_SPEC_CCH, ss->resolved_endpoint_id);
                break;
            case APR_SESSION_SRC_FAKE:
                cs->kind = APR_CLI_SRC_FAKE;
                cs->fake_hz  = ss->fake_hz;
                cs->fake_ppm = ss->fake_ppm;
                cs->fake_amp = ss->fake_amp;
                cs->fake_mute_at     = ss->fake_mute_at;
                cs->fake_unmute_at   = ss->fake_unmute_at;
                cs->fake_die_at      = ss->fake_die_at;
                cs->fake_start_muted = ss->fake_start_muted;
                cs->fake_start_dead  = ss->fake_start_dead;
                _snwprintf_s(cs->spec, APR_CLI_SPEC_CCH, _TRUNCATE, L"%lu,%ld",
                             (unsigned long)ss->fake_hz, (long)ss->fake_ppm);
                break;
            case APR_SESSION_SRC_SYSTEM_MINUS_TREE:
                /* Kept as its own kind rather than collapsed into a pid, so
                 * that apr_cli_resolve prints the whole tree warning again on
                 * every single run (design 4.1.1). A session must not make
                 * system-wide capture quieter than typing it does. */
                cs->kind = APR_CLI_SRC_SYSTEM_MINUS_TREE;
                cs->pid  = ss->resolved_pid;
                _snwprintf_s(cs->spec, APR_CLI_SPEC_CCH, _TRUNCATE, L"%lu",
                             (unsigned long)ss->resolved_pid);
                break;
            case APR_SESSION_SRC_PROCESS:
            default:
                cs->kind = APR_CLI_SRC_PID;
                cs->pid  = ss->resolved_pid;
                cs->muted_now = ss->muted_now;
                _snwprintf_s(cs->spec, APR_CLI_SPEC_CCH, _TRUNCATE, L"%lu",
                             (unsigned long)ss->resolved_pid);
                break;
            }
        }

        for (oi = 0; oi < sb->action_count &&
                     b->output_count < APR_CLI_MAX_OUTPUTS_PER_BUS; oi++) {
            AprCliOutput *o = &b->outputs[b->output_count++];
            memset(o, 0, sizeof *o);
            copy_cch(o->path, APR_CLI_SPEC_CCH, sb->actions[oi].path);
            strcpy_s(o->action_id, sizeof o->action_id, sb->actions[oi].id);
            o->bitrate_kbps = sb->actions[oi].bitrate_kbps;
            o->quality      = sb->actions[oi].quality;
        }
    }
}

/* THE SESSION COULD NOT BE TURNED INTO A RECORDING, said once, at the end.
 *
 * Every reason was already said in detail by say_resolution -- but through
 * warn(), which is silent under --json, so a `record --session x --json` that
 * could not resolve printed NOTHING AT ALL and exited 3. A script got an exit
 * code and an empty stream where the contract says there is always a document.
 * fail() is the one place that writes the JSON failure shape, so the refusal
 * goes through it, and in text mode it adds the one line that was missing
 * anyway: a verdict after the list of reasons. */
static AprCliExit session_refused(const Ctx *cx, const AprCliPlan *p,
                                  AprCliExit code, const wchar_t *first_bad)
{
    const wchar_t *args[2];

    args[0] = p->session_file;
    args[1] = (first_bad && first_bad[0]) ? first_bad : p->session_file;
    return fail(cx, code, APR_S_ERR_SESSION_NOT_USABLE, args, 2);
}

static AprCliExit load_session(const Ctx *cx, AprCliPlan *p)
{
    AprSessionResolveOptions opt;
    AprErr e;
    size_t i;
    AprCliExit worst = APR_CLI_OK;
    wchar_t    first_bad[APR_NAME_CCH];

    first_bad[0] = L'\0';

    e = apr_session_load(p->session_file, &g_session, &g_load_report);
    if (apr_failed(&e)) return report_load_fault(cx, p, &g_load_report, &e);

    /* Readable, and not written by this build. Say so once: a setting we
     * ignored may be the reason the recording is not what they expected. */
    if (g_load_report.from_newer_writer) {
        const wchar_t *args[1];
        args[0] = p->session_file;
        warn(cx, APR_S_WARN_SESSION_FROM_NEWER, args, 1);
    }
    if (g_load_report.unknown_keys > 0) {
        const wchar_t *args[2];
        args[0] = p->session_file;
        args[1] = g_load_report.first_unknown_key;
        warn(cx, APR_S_WARN_SESSION_UNKNOWN_KEYS, args, 2);
    }

    memset(&opt, 0, sizeof opt);
    opt.allow_system_capture = p->allow_system_capture;
    opt.allow_missing        = p->allow_missing;
    /* pick_when_ambiguous stays off. There is nobody to prompt in a script,
     * so the CLI refuses and names the rivals -- exactly what --exe already
     * does for the same situation. */

    e = apr_session_resolve(&g_session, &opt, &g_resolve_report);

    for (i = 0; i < g_resolve_report.count; i++) {
        const AprSessionResolution *r = &g_resolve_report.items[i];
        const AprSessionSource     *s = &g_session.sources[r->source_index];

        say_resolution(cx, p, &g_session, r);
        if (apr_session_status_usable(r->status)) continue;

        if (!first_bad[0]) {
            copy_cch(first_bad, APR_NAME_CCH,
                     r->name[0] ? r->name : r->wanted);
        }

        /* Consent is a configuration answer, not a missing thing: the file
         * was perfectly readable and describes a recording we will not make. */
        if (r->status == APR_SESSION_NEEDS_CONSENT) {
            worst = APR_CLI_CONFIG;
        } else if (s->kind == APR_SESSION_SRC_SYSTEM_MINUS_TREE ||
                   !p->allow_missing) {
            if (worst != APR_CLI_CONFIG) worst = APR_CLI_NOT_FOUND;
        } else {
            /* Dropped on purpose. Playable files, not the asked-for
             * recording -- which is precisely what exit 6 means. */
            p->session_incomplete = 1;
        }
    }
    if (worst != APR_CLI_OK) return session_refused(cx, p, worst, first_bad);
    if (apr_failed(&e) && !p->allow_missing)
        return session_refused(cx, p, APR_CLI_NOT_FOUND, first_bad);

    plan_from_session(p, &g_session);

    {
        const wchar_t *args[1];
        args[0] = p->session_file;
        note(cx, APR_S_STATUS_SESSION_LOADED, args, 1);
    }
    return APR_CLI_OK;
}

/* ---------------------------------------------------------------------------
 * save-session
 *
 * The plan is already resolved when this runs, so the pids are real and the
 * endpoint ids are real -- which is what lets the identity fields be captured
 * from the live machine rather than guessed from what was typed.
 *
 * Sources are DEDUPLICATED here. The command line nests a source inside each
 * bus it feeds, because that is how it was typed; the file lists each source
 * once and references it by key, because that is what makes it a graph and
 * what lets one source carry a different gain on each bus (design 3.2).
 * ------------------------------------------------------------------------- */

static int same_source(const AprSessionSource *a, const AprCliSource *b)
{
    switch (a->kind) {
    case APR_SESSION_SRC_DEVICE:
        return b->kind == APR_CLI_SRC_DEVICE &&
               _wcsicmp(a->endpoint_id, b->endpoint_id) == 0;
    case APR_SESSION_SRC_FAKE:
        /* Health is part of a synthetic source's identity: two fakes that
         * differ only in when they go silent are two different sources, and
         * interning them together would quietly drop one. */
        return b->kind == APR_CLI_SRC_FAKE &&
               a->fake_hz == b->fake_hz && a->fake_ppm == b->fake_ppm &&
               a->fake_amp == b->fake_amp &&
               a->fake_mute_at == b->fake_mute_at &&
               a->fake_unmute_at == b->fake_unmute_at &&
               a->fake_die_at == b->fake_die_at &&
               a->fake_start_muted == b->fake_start_muted &&
               a->fake_start_dead == b->fake_start_dead;
    case APR_SESSION_SRC_SYSTEM_MINUS_TREE:
        return b->kind == APR_CLI_SRC_SYSTEM_MINUS_TREE && a->pid == b->pid;
    case APR_SESSION_SRC_PROCESS:
    default:
        return (b->kind == APR_CLI_SRC_PID || b->kind == APR_CLI_SRC_EXE) &&
               a->pid == b->pid;
    }
}

static size_t intern_source(AprSession *s, const AprCliSource *cs)
{
    AprSessionSource *d;
    size_t i;

    for (i = 0; i < s->source_count; i++)
        if (same_source(&s->sources[i], cs)) return i;

    if (s->source_count >= APR_MAX_SOURCES) return (size_t)-1;
    d = &s->sources[s->source_count];
    memset(d, 0, sizeof *d);
    sprintf_s(d->key, APR_SESSION_KEY_CCH, "s%u", (unsigned)s->source_count);
    wcscpy_s(d->name, APR_NAME_CCH, cs->label[0] ? cs->label : cs->spec);

    switch (cs->kind) {
    case APR_CLI_SRC_DEVICE:
        d->kind = APR_SESSION_SRC_DEVICE;
        (void)apr_session_describe_device(d, cs->endpoint_id);
        break;
    case APR_CLI_SRC_FAKE:
        d->kind     = APR_SESSION_SRC_FAKE;
        d->fake_hz  = cs->fake_hz;
        d->fake_ppm = cs->fake_ppm;
        d->fake_amp = cs->fake_amp;
        d->fake_mute_at     = cs->fake_mute_at;
        d->fake_unmute_at   = cs->fake_unmute_at;
        d->fake_die_at      = cs->fake_die_at;
        d->fake_start_muted = cs->fake_start_muted;
        d->fake_start_dead  = cs->fake_start_dead;
        break;
    case APR_CLI_SRC_SYSTEM_MINUS_TREE:
        d->kind = APR_SESSION_SRC_SYSTEM_MINUS_TREE;
        (void)apr_session_describe_process(d, cs->pid);
        break;
    default:
        d->kind = APR_SESSION_SRC_PROCESS;
        (void)apr_session_describe_process(d, cs->pid);
        break;
    }
    s->source_count++;
    return s->source_count - 1;
}

static AprCliExit do_save_session(const Ctx *cx, const AprCliPlan *p)
{
    size_t bi, si, oi;
    AprErr e;

    apr_session_init(&g_session);
    g_session.sample_rate = p->rate;
    g_session.channels    = p->channels;
    g_session.duration_ms = p->duration_ms;

    for (bi = 0; bi < p->bus_count && bi < APR_MAX_BUSES; bi++) {
        const AprCliBus *cb = &p->buses[bi];
        AprSessionBus   *sb = &g_session.buses[g_session.bus_count++];

        memset(sb, 0, sizeof *sb);
        wcscpy_s(sb->name, APR_NAME_CCH, cb->name);

        for (si = 0; si < cb->source_count; si++) {
            size_t idx = intern_source(&g_session, &cb->sources[si]);
            AprSessionEdge *ed;
            /* THE POOL IS FULL, AND THAT IS A REFUSAL, NOT A SHRUG.
             *
             * This used to `continue`: the source was dropped, the file was
             * written, the run exited 0 and --json cheerfully reported
             * "sources": 64. Silent configuration loss, in the one artefact
             * whose entire job is to reproduce a recording exactly -- and it
             * would not be noticed until the session was replayed weeks later
             * and a bus was quietly missing an input. A session that cannot
             * hold what was described is not a session worth writing. */
            if (idx == (size_t)-1) {
                const wchar_t *args[2];
                args[0] = p->session_file;
                args[1] = cb->sources[si].label[0] ? cb->sources[si].label
                                                   : cb->sources[si].spec;
                return fail(cx, APR_CLI_CONFIG, APR_S_ERR_SESSION_TOO_MANY,
                            args, 2);
            }
            ed = &sb->edges[sb->edge_count++];
            memset(ed, 0, sizeof *ed);
            strcpy_s(ed->key, APR_SESSION_KEY_CCH, g_session.sources[idx].key);
            ed->gain_db_tenths = cb->sources[si].gain_db_tenths;
            ed->source_index   = idx;
        }
        for (oi = 0; oi < cb->output_count; oi++) {
            AprSessionAction *a = &sb->actions[sb->action_count++];
            memset(a, 0, sizeof *a);
            strcpy_s(a->id, sizeof a->id, cb->outputs[oi].action_id);
            wcscpy_s(a->path, APR_DISC_PATH_CCH, cb->outputs[oi].path);
            a->bitrate_kbps = cb->outputs[oi].bitrate_kbps;
            a->quality      = cb->outputs[oi].quality;
        }
    }

    e = apr_session_save(&g_session, p->session_file);
    if (apr_failed(&e)) {
        wchar_t why[512];
        const wchar_t *args[2];
        args[0] = p->session_file;
        args[1] = errtext(&e, why, 512);
        return fail(cx, APR_CLI_OUTPUT, APR_S_ERR_SESSION_NOT_WRITTEN, args, 2);
    }

    if (cx->json) {
        jline(cx, 0, L"{");
        jbool(cx, 1, L"ok", 1, 1);
        jnum(cx, 1, L"exitCode", 0, 1);
        jstr(cx, 1, L"session", p->session_file, 1);
        jnum(cx, 1, L"sources", (int64_t)g_session.source_count, 1);
        jnum(cx, 1, L"buses", (int64_t)g_session.bus_count, 0);
        jline(cx, 0, L"}");
    } else {
        const wchar_t *args[1];
        args[0] = p->session_file;
        note(cx, APR_S_STATUS_SESSION_SAVED, args, 1);
    }
    return APR_CLI_OK;
}

/* ---------------------------------------------------------------------------
 * Driver
 * ------------------------------------------------------------------------- */

static LANGID langid_of(const wchar_t *tag)
{
    LCID lcid;

    if (!tag || !tag[0]) return MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    lcid = LocaleNameToLCID(tag, LOCALE_ALLOW_NEUTRAL_NAMES);
    if (lcid == 0) return 0;
    return LANGIDFROMLCID(lcid);
}

AprCliExit apr_cli_run(int argc, const wchar_t *const *argv, const AprCliIo *io)
{
    static AprCliPlan plan;   /* static: several KB of buses */
    Ctx        cx;
    AprCliExit rc;
    int        log_owned = 0;

    /* English unless asked otherwise. Design 6.2: the CLI is an automation
     * surface, so it does not follow the thread UI language the way the app
     * does -- a script's output must not change because the machine's
     * language did. */
    apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));

    cx.io = io; cx.json = 0; cx.quiet = 0;

    if (argc <= 1) {
        print_usage(&cx);
        return APR_CLI_USAGE;
    }

    rc = apr_cli_parse(argc, argv, &plan, io);
    cx.json  = plan.json;
    cx.quiet = plan.quiet;
    if (rc != APR_CLI_OK) return rc;

    if (plan.lang[0]) {
        LANGID id = langid_of(plan.lang);
        /* An unreadable tag is not fatal: the program stays usable in the
         * primary language, which is what strings.h promises. */
        if (id) apr_str_set_language(id);
    }

    apr_log_set_level(plan.log_level);
    if (plan.log_file[0]) {
        AprLogConfig lc;
        memset(&lc, 0, sizeof lc);
        lc.level            = plan.log_level;
        lc.path             = plan.log_file;
        lc.background_drain = 1;
        (void)apr_log_init(&lc);
        log_owned = 1;
    }

    switch (plan.cmd) {
    case APR_CLI_CMD_HELP:
        print_usage(&cx);
        rc = APR_CLI_OK;
        break;
    case APR_CLI_CMD_VERSION:
        rc = do_version(&cx);
        break;
    case APR_CLI_CMD_LIST_APPS:
        rc = do_list_apps(&cx, &plan);
        break;
    case APR_CLI_CMD_LIST_DEVICES:
        rc = do_list_devices(&cx);
        break;
    case APR_CLI_CMD_SAVE_SESSION:
        rc = apr_cli_resolve(&plan, io);
        if (rc == APR_CLI_OK) rc = do_save_session(&cx, &plan);
        break;
    case APR_CLI_CMD_RECORD:
    default:
        /* A session file is read BEFORE apr_cli_resolve, and then resolved
         * again by it. That is not duplicated work: session resolution turns
         * stored identity into a pid, and apr_cli_resolve turns a pid into a
         * checked, writable, non-colliding recording. Running the second over
         * the first means a session-loaded run and a typed one reach
         * do_record through exactly the same checks. */
        if (plan.session_file[0]) {
            rc = load_session(&cx, &plan);
            if (rc != APR_CLI_OK) break;
        }
        rc = apr_cli_resolve(&plan, io);
        if (rc == APR_CLI_OK) {
            rc = plan.dry_run ? do_dry_run(&cx, &plan) : do_record(&cx, &plan);
            /* Recorded, playable, and not what the session asked for. */
            if (rc == APR_CLI_OK && plan.session_incomplete)
                rc = APR_CLI_INCOMPLETE;
        }
        break;
    }

    if (log_owned) apr_log_shutdown();
    return rc;
}

/* ---------------------------------------------------------------------------
 * The console
 *
 * Wide text goes to a console with WriteConsoleW and to a pipe or a file as
 * UTF-8. Doing it by hand rather than through the CRT's _O_U8TEXT modes keeps
 * both paths correct at once: a redirected run produces UTF-8 a script can
 * read, and an interactive run produces text the console renders directly with
 * no code page in the way.
 * ------------------------------------------------------------------------- */

static void console_line(int stream, const wchar_t *text)
{
    HANDLE h = GetStdHandle(stream == APR_CLI_STDERR ? STD_ERROR_HANDLE
                                                     : STD_OUTPUT_HANDLE);
    DWORD  mode = 0, written = 0;
    size_t len  = text ? wcslen(text) : 0;

    if (h == NULL || h == INVALID_HANDLE_VALUE) return;

    if (GetConsoleMode(h, &mode)) {
        if (len) WriteConsoleW(h, text, (DWORD)len, &written, NULL);
        WriteConsoleW(h, L"\r\n", 2, &written, NULL);
        return;
    }
    {
        char  *utf8;
        int    need = WideCharToMultiByte(CP_UTF8, 0, text ? text : L"",
                                          (int)len, NULL, 0, NULL, NULL);
        utf8 = (char *)malloc((size_t)need + 2);
        if (!utf8) return;
        if (need) WideCharToMultiByte(CP_UTF8, 0, text, (int)len, utf8, need,
                                      NULL, NULL);
        utf8[need]     = '\r';
        utf8[need + 1] = '\n';
        WriteFile(h, utf8, (DWORD)need + 2, &written, NULL);
        free(utf8);
    }
}

static void console_write(void *user, int stream, const wchar_t *line)
{
    (void)user;
    console_line(stream, line);
}

int apr_cli_main(int argc, wchar_t **argv)
{
    AprCliIo   io;
    AprCliExit rc;

    SetConsoleOutputCP(CP_UTF8);
    io.write = console_write;
    io.user  = NULL;

    /* The outer handler exists so that the __finally in do_record actually
     * runs during unwinding: without a handler somewhere above it, an access
     * violation would take the process down with the encoders unfinalized. */
    __try {
        rc = apr_cli_run(argc, (const wchar_t *const *)argv, &io);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        rc = APR_CLI_INTERNAL;
    }
    return (int)rc;
}
