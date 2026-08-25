/*
 * test_cli.c -- the command line: what it reads, what it refuses, what it
 * would do, and that stopping it leaves playable files.
 *
 * NOTHING HERE RENDERS AUDIO (AGENTS.md rule 1). Every case that actually
 * records uses --fake, which synthesises its samples into a ring and never
 * touches an audio endpoint in either direction. Every case that involves a
 * real process or a real device stops at --dry-run, which by construction
 * opens no audio client.
 *
 * The privacy-sensitive mode gets the same treatment for a second reason:
 * --system-minus-tree records whatever the machine is playing, so it is
 * exercised only through --dry-run, never run.
 */
#include "test_runner.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "cli/cli.h"
#include "strings.h"

/* ---------------------------------------------------------------------------
 * A capturing AprCliIo
 * ------------------------------------------------------------------------- */

#define CAP_CCH 16384

typedef struct Cap {
    wchar_t out[CAP_CCH];
    wchar_t err[CAP_CCH];
    int     lines;
} Cap;

static void cap_write(void *user, int stream, const wchar_t *line)
{
    Cap     *c = (Cap *)user;
    wchar_t *dst = (stream == APR_CLI_STDERR) ? c->err : c->out;
    size_t   have = wcslen(dst);
    size_t   want = line ? wcslen(line) : 0;

    if (have + want + 2 >= CAP_CCH) return;
    if (want) memcpy(dst + have, line, want * sizeof(wchar_t));
    dst[have + want]     = L'\n';
    dst[have + want + 1] = L'\0';
    c->lines++;
}

static AprCliIo cap_io(Cap *c)
{
    AprCliIo io;
    memset(c, 0, sizeof *c);
    io.write = cap_write;
    io.user  = c;
    return io;
}

static int has(const wchar_t *hay, const wchar_t *needle)
{
    return wcsstr(hay, needle) != NULL;
}

/* Both streams, because "did it say this at all" is usually the question. */
static int said(const Cap *c, const wchar_t *needle)
{
    return has(c->out, needle) || has(c->err, needle);
}

/* ---------------------------------------------------------------------------
 * Files
 * ------------------------------------------------------------------------- */

static void tmp_path(wchar_t *buf, size_t cch, const wchar_t *tag,
                     const wchar_t *ext)
{
    static LONG counter;
    wchar_t dir[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(buf, cch, _TRUNCATE, L"%lsapr_cli_%ls_%lu_%ld.%ls",
                 dir, tag, GetCurrentProcessId(),
                 InterlockedIncrement(&counter), ext);
}

static int file_exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static uint64_t file_size(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return 0;
    return ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

/* A WAV that a player will open: RIFF/WAVE, a data chunk, and a data size that
 * is both non-zero and consistent with the file length. This is the property
 * that a killed recording is meant to preserve. */
static int wav_is_playable(const wchar_t *path, uint32_t *out_data_bytes)
{
    unsigned char buf[4096];
    HANDLE  h;
    DWORD   got = 0;
    uint64_t size = file_size(path);
    size_t  i;

    if (out_data_bytes) *out_data_bytes = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, buf, (DWORD)sizeof buf, &got, NULL)) got = 0;
    CloseHandle(h);
    if (got < 64) return 0;
    if (memcmp(buf, "RIFF", 4) != 0 && memcmp(buf, "RF64", 4) != 0) return 0;
    if (memcmp(buf + 8, "WAVE", 4) != 0) return 0;

    for (i = 12; i + 8 <= (size_t)got; i += 4) {
        if (memcmp(buf + i, "data", 4) == 0) {
            uint32_t n = (uint32_t)buf[i + 4] | ((uint32_t)buf[i + 5] << 8) |
                         ((uint32_t)buf[i + 6] << 16) | ((uint32_t)buf[i + 7] << 24);
            if (out_data_bytes) *out_data_bytes = n;
            return n > 0 && (uint64_t)n + i + 8 <= size;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Running the CLI
 * ------------------------------------------------------------------------- */

#define ARGV(...) \
    (const wchar_t *const[]){ L"apprecorder", __VA_ARGS__ }

#define ARGC(...) \
    (int)(sizeof((const wchar_t *const[]){ L"apprecorder", __VA_ARGS__ }) / \
          sizeof(const wchar_t *))

#define RUN(cap, ...) apr_cli_run(ARGC(__VA_ARGS__), ARGV(__VA_ARGS__), io_of(cap))
#define PARSE(cap, plan, ...) \
    apr_cli_parse(ARGC(__VA_ARGS__), ARGV(__VA_ARGS__), (plan), io_of(cap))

static AprCliIo g_io_storage;
static const AprCliIo *io_of(Cap *c)
{
    g_io_storage = cap_io(c);
    return &g_io_storage;
}

/* ===========================================================================
 * Parsing
 * ========================================================================= */

TEST(a_bare_source_and_output_make_one_implicit_bus)
{
    Cap c; static AprCliPlan p;
    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p, L"--pid", L"1234", L"--out", L"x.wav"));
    ASSERT_EQ_INT(APR_CLI_CMD_RECORD, p.cmd);
    ASSERT_EQ_INT(1, (int)p.bus_count);
    ASSERT_EQ_INT(1, (int)p.buses[0].source_count);
    ASSERT_EQ_INT(APR_CLI_SRC_PID, p.buses[0].sources[0].kind);
    ASSERT_WSTR_EQ(L"1234", p.buses[0].sources[0].spec);
    ASSERT_EQ_INT(1, (int)p.buses[0].output_count);
    ASSERT_WSTR_EQ(L"x.wav", p.buses[0].outputs[0].path);
    ASSERT_EQ_INT(48000, (int)p.rate);
    ASSERT_EQ_INT(2, (int)p.channels);
}

TEST(bus_flags_partition_everything_written_after_them)
{
    /* The whole reason the project exists: more buses than a hardware mixer
     * has, in one invocation, sharing sources. */
    Cap c; static AprCliPlan p;
    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p,
        L"--bus", L"Mix",   L"--fake", L"440", L"--device", L"Chat", L"--out", L"mix.wav",
        L"--bus", L"Voice", L"--device", L"Chat",                   L"--out", L"voice.wav"));

    ASSERT_EQ_INT(2, (int)p.bus_count);
    ASSERT_WSTR_EQ(L"Mix", p.buses[0].name);
    ASSERT_EQ_INT(2, (int)p.buses[0].source_count);
    ASSERT_EQ_INT(1, (int)p.buses[0].output_count);
    ASSERT_WSTR_EQ(L"Voice", p.buses[1].name);
    ASSERT_EQ_INT(1, (int)p.buses[1].source_count);
    ASSERT_EQ_INT(APR_CLI_SRC_DEVICE, p.buses[1].sources[0].kind);
    ASSERT_WSTR_EQ(L"voice.wav", p.buses[1].outputs[0].path);
}

TEST(gain_belongs_to_the_source_written_before_it)
{
    Cap c; static AprCliPlan p;
    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p,
        L"--fake", L"440", L"--gain", L"-6", L"--fake", L"220", L"--out", L"x.wav"));
    ASSERT_EQ_INT(2, (int)p.buses[0].source_count);
    ASSERT_EQ_INT(-60, (int)p.buses[0].sources[0].gain_db_tenths);
    ASSERT_EQ_INT(0,   (int)p.buses[0].sources[1].gain_db_tenths);
    /* -6 dB is half the amplitude, near enough. */
    ASSERT_NEAR(0.5011872, p.buses[0].sources[0].gain, 1e-5);
    ASSERT_NEAR(1.0,       p.buses[0].sources[1].gain, 1e-6);
}

TEST(gain_with_no_source_to_apply_it_to_is_a_usage_error)
{
    Cap c; static AprCliPlan p;
    ASSERT_EQ_INT(APR_CLI_USAGE, PARSE(&c, &p, L"--gain", L"-6", L"--out", L"x.wav"));
    ASSERT_TRUE(said(&c, L"--gain"));
}

TEST(fractional_gain_survives_the_command_line)
{
    Cap c; static AprCliPlan p;
    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p,
        L"--fake", L"440", L"--gain", L"-6.5", L"--out", L"x.wav"));
    ASSERT_EQ_INT(-65, (int)p.buses[0].sources[0].gain_db_tenths);
}

TEST(fake_sources_take_a_tone_a_drift_and_an_amplitude)
{
    Cap c; static AprCliPlan p;
    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p,
        L"--fake", L"440,30,0.25", L"--fake", L"1000", L"--out", L"x.wav"));
    ASSERT_EQ_INT(440, (int)p.buses[0].sources[0].fake_hz);
    ASSERT_EQ_INT(30,  (int)p.buses[0].sources[0].fake_ppm);
    ASSERT_NEAR(0.25,  p.buses[0].sources[0].fake_amp, 1e-6);
    ASSERT_EQ_INT(1000, (int)p.buses[0].sources[1].fake_hz);
    ASSERT_EQ_INT(0,    (int)p.buses[0].sources[1].fake_ppm);
}

TEST(an_unknown_option_is_a_usage_error_and_says_which_one)
{
    Cap c; static AprCliPlan p;
    ASSERT_EQ_INT(APR_CLI_USAGE, PARSE(&c, &p, L"--recrod", L"x.wav"));
    ASSERT_TRUE(said(&c, L"--recrod"));
}

TEST(an_unknown_command_is_a_usage_error_and_says_which_one)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_USAGE, RUN(&c, L"lst-apps"));
    ASSERT_TRUE(said(&c, L"lst-apps"));
}

TEST(an_option_with_no_value_is_a_usage_error)
{
    Cap c; static AprCliPlan p;
    ASSERT_EQ_INT(APR_CLI_USAGE, PARSE(&c, &p, L"--pid"));
    ASSERT_TRUE(said(&c, L"--pid"));
}

TEST(a_value_that_is_not_a_number_is_a_usage_error)
{
    Cap c; static AprCliPlan p;
    ASSERT_EQ_INT(APR_CLI_USAGE, PARSE(&c, &p, L"--rate", L"fortyeight"));
    ASSERT_TRUE(said(&c, L"fortyeight"));
}

TEST(a_number_outside_the_range_is_a_usage_error)
{
    Cap c; static AprCliPlan p;
    ASSERT_EQ_INT(APR_CLI_USAGE, PARSE(&c, &p, L"--channels", L"99",
                                       L"--fake", L"440", L"--out", L"x.wav"));
    ASSERT_EQ_INT(APR_CLI_USAGE, PARSE(&c, &p, L"--rate", L"3",
                                       L"--fake", L"440", L"--out", L"x.wav"));
}

TEST(options_that_belong_to_another_command_are_refused)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_USAGE, RUN(&c, L"list-devices", L"--pid", L"1234"));
    ASSERT_TRUE(said(&c, L"--pid"));
}

/* ===========================================================================
 * Configuration errors -- read correctly, but not a recording
 * ========================================================================= */

TEST(a_bus_with_no_source_is_refused_by_name)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--bus", L"Empty", L"--out", L"x.wav",
                                      L"--dry-run"));
    ASSERT_TRUE(said(&c, L"Empty"));
}

TEST(a_bus_with_no_output_is_refused_by_name)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--bus", L"Silent", L"--fake", L"440",
                                      L"--dry-run"));
    ASSERT_TRUE(said(&c, L"Silent"));
}

TEST(recording_nothing_at_all_is_refused)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--dry-run", L"--rate", L"48000"));
}

TEST(a_format_this_build_cannot_write_is_a_configuration_error)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--fake", L"440",
                                      L"--format", L"flac", L"--out", L"x.flac",
                                      L"--dry-run"));
    ASSERT_TRUE(said(&c, L"flac"));
}

TEST(a_path_with_no_extension_says_so_rather_than_guessing)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--fake", L"440", L"--out", L"recording",
                                      L"--dry-run"));
    ASSERT_TRUE(said(&c, L"recording"));
}

TEST(the_extension_chooses_the_format_when_format_is_not_given)
{
    Cap c; static AprCliPlan p;
    wchar_t wav[MAX_PATH], m4a[MAX_PATH];
    tmp_path(wav, MAX_PATH, L"ext", L"wav");
    tmp_path(m4a, MAX_PATH, L"ext", L"m4a");

    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p, L"--fake", L"440",
                                    L"--out", wav, L"--out", m4a));
    ASSERT_EQ_INT(APR_CLI_OK, apr_cli_resolve(&p, io_of(&c)));
    ASSERT_STR_EQ("wav", p.buses[0].outputs[0].action_id);
    ASSERT_STR_EQ("m4a", p.buses[0].outputs[1].action_id);
    ASSERT_FALSE(file_exists(wav));   /* resolving must not create anything */
    ASSERT_FALSE(file_exists(m4a));
}

TEST(format_overrides_the_extension_for_the_output_that_follows_it)
{
    Cap c; static AprCliPlan p;
    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p, L"--fake", L"440",
                                    L"--format", L"wav", L"--out", L"a.m4a",
                                    L"--out", L"b.m4a"));
    ASSERT_EQ_INT(APR_CLI_OK, apr_cli_resolve(&p, io_of(&c)));
    ASSERT_STR_EQ("wav", p.buses[0].outputs[0].action_id);
    ASSERT_STR_EQ("m4a", p.buses[0].outputs[1].action_id);
}

TEST(two_outputs_cannot_share_one_file)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--bus", L"A", L"--fake", L"440",
                                      L"--out", L"same.wav",
                                      L"--bus", L"B", L"--fake", L"220",
                                      L"--out", L"same.wav", L"--dry-run"));
    ASSERT_TRUE(said(&c, L"same.wav"));
}

/* ===========================================================================
 * Resolution against the machine
 * ========================================================================= */

TEST(a_process_id_that_is_not_running_is_reported_as_not_found)
{
    Cap c;
    /* Not a pid Windows will hand out: they are multiples of four and this is
     * far past any plausible value on a running system. */
    ASSERT_EQ_INT(APR_CLI_NOT_FOUND, RUN(&c, L"--pid", L"2147483644",
                                         L"--out", L"x.wav", L"--dry-run"));

    /* A pid that is not a decimal number never reaches the machine at all:
     * that is the command line being wrong, not the process being absent. */
    ASSERT_EQ_INT(APR_CLI_USAGE, RUN(&c, L"--pid", L"0x7ffffff0",
                                     L"--out", L"x.wav", L"--dry-run"));
}

TEST(an_application_that_is_not_playing_anything_is_reported_as_not_found)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_NOT_FOUND,
                  RUN(&c, L"--exe", L"no-such-program-at-all.exe",
                      L"--out", L"x.wav", L"--dry-run"));
    ASSERT_TRUE(said(&c, L"no-such-program-at-all.exe"));
}

TEST(a_capture_device_that_is_not_there_is_reported_as_not_found)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_NOT_FOUND,
                  RUN(&c, L"--device", L"no-such-capture-device", L"--out", L"x.wav",
                      L"--dry-run"));
    ASSERT_TRUE(said(&c, L"no-such-capture-device"));
}

TEST(an_output_path_that_cannot_be_written_is_its_own_exit_code)
{
    Cap c;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(path, MAX_PATH, _TRUNCATE,
                 L"%lsapr_cli_no_such_directory_%lu\\x.wav", dir,
                 GetCurrentProcessId());

    ASSERT_EQ_INT(APR_CLI_OUTPUT, RUN(&c, L"--fake", L"440", L"--out", path,
                                      L"--dry-run"));
    ASSERT_TRUE(said(&c, L"x.wav"));
    ASSERT_FALSE(file_exists(path));
}

/* ===========================================================================
 * --dry-run
 * ========================================================================= */

TEST(dry_run_reports_the_plan_and_creates_nothing)
{
    Cap c;
    wchar_t path[MAX_PATH];
    tmp_path(path, MAX_PATH, L"dry", L"wav");

    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--bus", L"Main", L"--fake", L"440",
                                  L"--gain", L"-3", L"--out", path, L"--dry-run"));
    ASSERT_TRUE(said(&c, L"Main"));
    ASSERT_TRUE(said(&c, path));
    ASSERT_TRUE(said(&c, L"-3.0"));
    ASSERT_TRUE(said(&c, L"48000"));
    ASSERT_FALSE(file_exists(path));
}

TEST(dry_run_in_json_is_a_document_a_script_can_read)
{
    Cap c;
    wchar_t path[MAX_PATH];
    tmp_path(path, MAX_PATH, L"dryjson", L"wav");

    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--fake", L"440", L"--out", path,
                                  L"--dry-run", L"--json"));
    ASSERT_EQ_INT(L'{', c.out[0]);
    ASSERT_TRUE(has(c.out, L"\"dryRun\": true"));
    ASSERT_TRUE(has(c.out, L"\"format\": \"wav\""));
    ASSERT_TRUE(has(c.out, L"\"sampleRate\": 48000"));
    ASSERT_FALSE(file_exists(path));
}

TEST(a_json_failure_is_still_json)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--json", L"--dry-run"));
    ASSERT_EQ_INT(L'{', c.out[0]);
    ASSERT_TRUE(has(c.out, L"\"exitCode\": 2"));
}

/* ===========================================================================
 * The privacy-sensitive mode
 * ========================================================================= */

TEST(nothing_but_the_explicit_flag_ever_selects_system_capture)
{
    Cap c; static AprCliPlan p;
    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p, L"--pid", L"1234", L"--out", L"x.wav"));
    ASSERT_EQ_INT(APR_CLI_SRC_PID, p.buses[0].sources[0].kind);

    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p, L"--exe", L"chrome.exe", L"--out", L"x.wav"));
    ASSERT_EQ_INT(APR_CLI_SRC_EXE, p.buses[0].sources[0].kind);
}

TEST(system_capture_says_what_it_records_and_that_it_takes_a_whole_tree)
{
    /* Dry run only. Actually running this would record whatever the machine
     * is playing, which is not an automated test's call to make. */
    Cap     c;
    wchar_t path[MAX_PATH], pid[32];
    tmp_path(path, MAX_PATH, L"tree", L"wav");
    _snwprintf_s(pid, 32, _TRUNCATE, L"%lu", GetCurrentProcessId());

    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--system-minus-tree", pid,
                                  L"--out", path, L"--dry-run"));

    /* It must describe the scope, and it must not describe it as "everything
     * except one program" -- the thing held back is a whole tree that grows. */
    ASSERT_TRUE(said(&c, L"every sound this computer plays"));
    ASSERT_TRUE(said(&c, L"every program it has started"));
    ASSERT_TRUE(said(&c, L"launcher"));
    ASSERT_FALSE(file_exists(path));
}

TEST(system_capture_needs_a_process_id_that_exists)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_NOT_FOUND, RUN(&c, L"--system-minus-tree", L"2147483644",
                                         L"--out", L"x.wav", L"--dry-run"));
}

/* ===========================================================================
 * Listings
 * ========================================================================= */

TEST(list_devices_answers_and_its_json_parses_as_an_object)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"list-devices", L"--json"));
    ASSERT_EQ_INT(L'{', c.out[0]);
    ASSERT_TRUE(has(c.out, L"\"devices\""));
}

TEST(list_apps_answers_and_never_lists_the_whole_process_table)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"list-apps", L"--json"));
    ASSERT_EQ_INT(L'{', c.out[0]);
    ASSERT_TRUE(has(c.out, L"\"apps\""));
    /* No trailing comma before the closing bracket: list-apps filters rows out
     * of the scan, and counting the scan rather than what was emitted is how
     * that turns into a document no parser accepts. */
    ASSERT_FALSE(has(c.out, L"},\n  ]"));
    /* A machine has hundreds of processes and at most a handful of audio
     * sessions. If this ever fails, the listing has stopped asking the audio
     * engine and started asking the process table. */
    ASSERT_LT_INT(120, c.lines);
}

TEST(help_prints_the_usage_and_the_exit_codes_a_script_branches_on)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"help"));
    ASSERT_TRUE(has(c.out, L"Usage:"));
    ASSERT_TRUE(has(c.out, L"--bus"));
    ASSERT_TRUE(has(c.out, L"--dry-run"));
    ASSERT_TRUE(has(c.out, L"Exit codes"));
}

TEST(no_arguments_at_all_prints_the_usage_and_fails)
{
    Cap c;
    const wchar_t *const argv[] = { L"apprecorder" };
    ASSERT_EQ_INT(APR_CLI_USAGE, apr_cli_run(1, argv, io_of(&c)));
    ASSERT_TRUE(has(c.out, L"Usage:"));
}

TEST(version_answers)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"version"));
    ASSERT_TRUE(has(c.out, L"apprecorder"));
}

/* ===========================================================================
 * Recording, driven entirely by synthetic sources
 * ========================================================================= */

TEST(a_short_recording_writes_a_playable_file)
{
    Cap      c;
    wchar_t  path[MAX_PATH];
    uint32_t data = 0;

    tmp_path(path, MAX_PATH, L"rec", L"wav");
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--fake", L"440,0,0.25", L"--out", path,
                                  L"--duration", L"0.4", L"--quiet"));
    ASSERT_TRUE(file_exists(path));
    ASSERT_TRUE(wav_is_playable(path, &data));
    /* 0.4 s at 48 kHz stereo float32 is 153,600 bytes; allow the lookbehind. */
    ASSERT_GT_INT(40000, (int)data);
    DeleteFileW(path);
}

TEST(several_buses_in_one_invocation_each_get_their_own_file)
{
    /* This is the feature the whole project exists for. */
    Cap     c;
    wchar_t a[MAX_PATH], b[MAX_PATH];

    tmp_path(a, MAX_PATH, L"busa", L"wav");
    tmp_path(b, MAX_PATH, L"busb", L"wav");

    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c,
        L"--bus", L"One", L"--fake", L"440,0,0.25", L"--out", a,
        L"--bus", L"Two", L"--fake", L"220,0,0.25", L"--out", b,
        L"--duration", L"0.4", L"--quiet"));

    ASSERT_TRUE(wav_is_playable(a, NULL));
    ASSERT_TRUE(wav_is_playable(b, NULL));
    DeleteFileW(a);
    DeleteFileW(b);
}

TEST(one_bus_can_be_written_to_two_formats_at_once)
{
    Cap     c;
    wchar_t wav[MAX_PATH], mp3[MAX_PATH];

    tmp_path(wav, MAX_PATH, L"twofmt", L"wav");
    tmp_path(mp3, MAX_PATH, L"twofmt", L"mp3");

    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--fake", L"440,0,0.25",
                                  L"--out", wav, L"--out", mp3,
                                  L"--duration", L"0.4", L"--quiet"));
    ASSERT_TRUE(wav_is_playable(wav, NULL));
    ASSERT_TRUE(file_exists(mp3));
    ASSERT_GT_INT(500, (int)file_size(mp3));
    DeleteFileW(wav);
    DeleteFileW(mp3);
}

/* ---------------------------------------------------------------------------
 * Stopping. The property under test is the one that matters most in this
 * program: an interrupted recording is finalized, not abandoned.
 * ------------------------------------------------------------------------- */

typedef struct RecThread {
    wchar_t     path[MAX_PATH];
    Cap         cap;
    AprCliIo    io;
    AprCliExit  rc;
    HANDLE      started;
} RecThread;

static DWORD WINAPI record_until_stopped(void *user)
{
    RecThread *r = (RecThread *)user;
    const wchar_t *const argv[] = {
        L"apprecorder", L"--fake", L"440,0,0.25", L"--out", r->path, L"--quiet"
    };
    SetEvent(r->started);
    r->rc = apr_cli_run((int)(sizeof argv / sizeof argv[0]), argv, &r->io);
    return 0;
}

TEST(a_stop_request_finalizes_every_file_rather_than_abandoning_it)
{
    RecThread r;
    HANDLE    th;
    int       saw_handler = 0;
    int       i;

    memset(&r, 0, sizeof r);
    tmp_path(r.path, MAX_PATH, L"stop", L"wav");
    r.io.write = cap_write;
    r.io.user  = &r.cap;
    r.rc = (AprCliExit)-1;
    r.started = CreateEventW(NULL, TRUE, FALSE, NULL);
    ASSERT_NOT_NULL(r.started);

    th = CreateThread(NULL, 0, record_until_stopped, &r, 0, NULL);
    ASSERT_NOT_NULL(th);

    /* Wait for the recording to be genuinely under way: the console control
     * handler is installed for exactly as long as the record loop runs, so it
     * is also the signal that we are inside it. */
    WaitForSingleObject(r.started, 5000);
    for (i = 0; i < 500 && !saw_handler; i++) {
        saw_handler = apr_cli_test_ctrl_handler_installed();
        if (!saw_handler) Sleep(10);
    }
    ASSERT_TRUE(saw_handler);
    Sleep(200);

    /* Exactly what the Ctrl+C handler does. */
    apr_cli_request_stop();

    ASSERT_EQ_INT(WAIT_OBJECT_0, (int)WaitForSingleObject(th, 15000));
    CloseHandle(th);
    CloseHandle(r.started);

    ASSERT_EQ_INT(APR_CLI_OK, r.rc);
    ASSERT_TRUE(wav_is_playable(r.path, NULL));
    /* And the handler is gone again, so a later Ctrl+C is the console's. */
    ASSERT_FALSE(apr_cli_test_ctrl_handler_installed());
    DeleteFileW(r.path);
}

TEST(a_stop_request_before_anything_starts_is_harmless)
{
    Cap     c;
    wchar_t path[MAX_PATH];

    apr_cli_request_stop();     /* nothing is running */
    tmp_path(path, MAX_PATH, L"prestop", L"wav");

    /* and it must not leak into the next recording */
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--fake", L"440,0,0.25", L"--out", path,
                                  L"--duration", L"0.3", L"--quiet"));
    ASSERT_TRUE(wav_is_playable(path, NULL));
    DeleteFileW(path);
}

TEST(an_output_that_cannot_be_created_stops_the_run_before_any_device_opens)
{
    /* The first output is fine, the second cannot be created. Every output
     * is checked before any of them is opened and before any capture starts,
     * so the run fails with the output code and leaves nothing behind. */
    Cap     c;
    wchar_t good[MAX_PATH], bad[MAX_PATH], dir[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, dir);

    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    tmp_path(good, MAX_PATH, L"halfgood", L"wav");
    _snwprintf_s(bad, MAX_PATH, _TRUNCATE, L"%lsapr_cli_nodir_%lu\\bad.wav",
                 dir, GetCurrentProcessId());

    ASSERT_EQ_INT(APR_CLI_OUTPUT, RUN(&c, L"--fake", L"440", L"--out", good,
                                      L"--out", bad, L"--quiet"));
    ASSERT_FALSE(file_exists(bad));
    if (file_exists(good)) DeleteFileW(good);
}

/* ===========================================================================
 * Language
 * ========================================================================= */

TEST(the_cli_speaks_english_by_default_whatever_the_thread_locale_is)
{
    /* Design 6.2: the CLI is an automation surface, so it does not follow the
     * user's UI language unless it is asked to. */
    Cap c;
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"version"));
    ASSERT_EQ_INT(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                  (int)apr_str_language());
}

TEST(an_unreadable_language_tag_does_not_stop_the_program)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"version", L"--lang", L"not-a-language"));
    apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
}
