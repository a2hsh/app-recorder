/*
 * frontend.c -- the dispatch rule, as a pure function.
 *
 * It lives in apprecorder_core rather than beside main() for one reason: a
 * rule that decides which half of the program runs must be testable without
 * starting the program. tests/test_frontend.c drives every row of the table in
 * frontend.h through apr_frontend_choose() directly.
 */
#include "frontend.h"

#include <wchar.h>

#include "cli/cli.h"
#include "session.h"

/* Case-insensitive suffix test. The extension comes from the file system, and
 * Windows file systems do not care about case -- a session saved as .json is
 * routinely handed back by Explorer as .JSON. The COMMAND names above are
 * case-SENSITIVE by contrast, and deliberately: those are grammar, not paths. */
static int ends_with_i(const wchar_t *s, const wchar_t *suffix)
{
    size_t ls, lf;

    if (!s || !suffix) return 0;
    ls = wcslen(s);
    lf = wcslen(suffix);
    if (lf == 0 || ls < lf) return 0;
    return _wcsicmp(s + (ls - lf), suffix) == 0;
}

void apr_frontend_choose(int argc, const wchar_t *const *argv,
                         AprFrontEndChoice *out)
{
    const wchar_t *a1;

    if (!out) return;
    out->front        = APR_FRONT_GUI;
    out->session_path = NULL;
    out->bad_arg      = NULL;

    /* argv[0] is the program name, exactly as a shell passes it. A run with
     * nothing after it is a double-click, or someone typing the name to see
     * what happens: the window, empty. */
    if (argc < 2 || !argv || !argv[1] || argv[1][0] == L'\0') return;

    a1 = argv[1];

    /* Asked of cli.c, so this program has ONE list of command names. */
    if (apr_cli_command_from_name(a1, NULL)) {
        out->front = APR_FRONT_CLI;
        return;
    }

    /* Options are the command line's grammar and the window has none. This is
     * also what carries `--help`, `-h` and `-?` -- which are options rather
     * than commands -- to the front end that can answer them. An option the
     * command line does not know is reported by the command line, in its own
     * words, with its own exit code; there is nothing better this rule could
     * say about it. */
    if (a1[0] == L'-') {
        out->front = APR_FRONT_CLI;
        return;
    }

    /* A lone session file: Explorer double-click, or a shortcut, or a drop on
     * the executable. Open the window on it.
     *
     * Existence is deliberately NOT checked. This function is pure so that the
     * rule is a table in a test rather than a fixture on disk, and "the file
     * is not there" is a better sentence from the window -- which can name it
     * and offer to open another -- than from a dispatcher that would have to
     * guess whether a missing .json was a typo'd command instead. */
    if (argc == 2 && ends_with_i(a1, L"." APR_SESSION_EXT)) {
        out->front        = APR_FRONT_GUI;
        out->session_path = a1;
        return;
    }

    /* Everything else is a mistake, and it is named rather than guessed at.
     * `apprecorder recrod --out x.wav` used to be a plausible way to open an
     * empty window and lose the rest of the command line in silence. */
    out->front   = APR_FRONT_UNKNOWN;
    out->bad_arg = a1;
}

AprApartment apr_frontend_apartment(AprFrontEnd front)
{
    /* The window's thread, and only the window's thread. See frontend.h.
     *
     * APR_FRONT_UNKNOWN gets NONE with the rest: it builds no window and opens
     * no device, it prints one sentence and leaves. */
    return front == APR_FRONT_GUI ? APR_APARTMENT_STA : APR_APARTMENT_NONE;
}

int apr_frontend_should_explain(int have_output, int exit_code)
{
    return !have_output && exit_code != 0;
}
