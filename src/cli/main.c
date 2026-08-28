/*
 * main.c -- the console entry point, and nothing else.
 *
 * Everything the program does lives in cli.c behind apr_cli_main, so the whole
 * command line is reachable from a test without a process boundary. This file
 * exists only to be the thing the linker starts.
 *
 * wmain rather than main: paths and application names are not ASCII, and going
 * through the ANSI command line would mangle them before apprecorder ever saw
 * them. MSVC selects wmainCRTStartup on its own when wmain is present.
 *
 * ---------------------------------------------------------------------------
 * THE ONE THING THAT HAPPENS BEFORE apr_cli_main
 *
 *   The Windows floor (platform/winver.c). Below build 19041 there is no
 *   process-loopback API, so there is no recording to be made on this machine
 *   whatever the command line says -- and every path inside cli.c would fail
 *   later, one confusing source at a time, instead of once and clearly.
 *
 *   It is here rather than inside apr_cli_main because the decision it guards
 *   is about the machine, not the arguments. The part worth testing --
 *   reading the real build and comparing it to the floor -- lives in
 *   winver.c and is tested there; what remains here is a print and a return.
 */
#include <windows.h>
#include <stdio.h>

#include "cli/cli.h"
#include "strings.h"
#include "winver.h"

int wmain(int argc, wchar_t **argv)
{
    (void)apr_str_init();

    if (!apr_win_meets_floor()) {
        wchar_t text[512], need[32], have[32];
        const wchar_t *args[2];

        apr_str_number((int64_t)APR_WIN_MIN_BUILD, need, 32);
        apr_str_number((int64_t)apr_win_build(),   have, 32);
        args[0] = need;
        args[1] = have;
        apr_str_format(APR_S_ERR_WINDOWS_TOO_OLD, text, 512, args, 2);

        fwprintf(stderr, L"%ls\n", text);

        /* APR_CLI_CONFIG: "read, but not a recording that can be made." The
         * codes are contract and are documented in the help text, so this
         * reuses the one that already means exactly this rather than
         * appending a ninth. It is also the answer --dry-run owes: on this
         * machine the real run could not have happened either. */
        return APR_CLI_CONFIG;
    }

    return apr_cli_main(argc, argv);
}
