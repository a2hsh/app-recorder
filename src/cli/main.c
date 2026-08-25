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
 */
#include "cli/cli.h"

int wmain(int argc, wchar_t **argv)
{
    return apr_cli_main(argc, argv);
}
