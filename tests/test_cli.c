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

/* The ogg action's own disk-failure seam. Reached from here because "every
 * output failed to open" is a property of the RUN, and the run is what this
 * suite drives -- there is no other way to make a create() fail that
 * apr_cli_resolve has not already caught. */
extern volatile LONG64 apr_ogg_test_fail_after_bytes;

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

TEST(a_fake_source_can_be_asked_to_go_silent_or_to_die)
{
    Cap c; static AprCliPlan p;
    const AprCliSource *s;

    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p,
        L"--fake", L"440,30,0.25,mute=4800,unmute=9600,die=14400",
        L"--out", L"x.wav"));
    s = &p.buses[0].sources[0];
    /* The tone, the drift and the amplitude still read the same way. */
    ASSERT_EQ_INT(440, (int)s->fake_hz);
    ASSERT_EQ_INT(30,  (int)s->fake_ppm);
    ASSERT_NEAR(0.25,  s->fake_amp, 1e-6);
    ASSERT_EQ_INT(4800,  (int)s->fake_mute_at);
    ASSERT_EQ_INT(9600,  (int)s->fake_unmute_at);
    ASSERT_EQ_INT(14400, (int)s->fake_die_at);
    ASSERT_FALSE(s->fake_start_muted);
    ASSERT_FALSE(s->fake_start_dead);
}

TEST(the_two_start_states_are_words_because_frame_zero_is_not_a_frame)
{
    /* capture.h reserves a frame of 0 for "never", so frame 0 is asked for by
     * name. The two spellings must not both be reachable. */
    Cap c; static AprCliPlan p;

    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p, L"--fake", L"440,muted,dead",
                                    L"--out", L"x.wav"));
    ASSERT_TRUE(p.buses[0].sources[0].fake_start_muted);
    ASSERT_TRUE(p.buses[0].sources[0].fake_start_dead);
    ASSERT_EQ_INT(0, (int)p.buses[0].sources[0].fake_mute_at);
    ASSERT_EQ_INT(0, (int)p.buses[0].sources[0].fake_die_at);

    ASSERT_EQ_INT(APR_CLI_USAGE, PARSE(&c, &p, L"--fake", L"440,mute=0",
                                       L"--out", L"x.wav"));
    ASSERT_EQ_INT(APR_CLI_USAGE, PARSE(&c, &p, L"--fake", L"440,die=0",
                                       L"--out", L"x.wav"));
}

TEST(a_health_word_may_stand_where_a_number_would_have_gone)
{
    /* "440,dead" is as readable as "440,0,0.25,dead" and means the same. The
     * tone stays first and stays required. */
    Cap c; static AprCliPlan p;

    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p, L"--fake", L"440,dead",
                                    L"--out", L"x.wav"));
    ASSERT_EQ_INT(440, (int)p.buses[0].sources[0].fake_hz);
    ASSERT_TRUE(p.buses[0].sources[0].fake_start_dead);

    /* A number after a health word is not a drift that arrived late. */
    ASSERT_EQ_INT(APR_CLI_USAGE, PARSE(&c, &p, L"--fake", L"440,dead,30",
                                       L"--out", L"x.wav"));
    /* And a spec with no tone at all is still refused. */
    ASSERT_EQ_INT(APR_CLI_USAGE, PARSE(&c, &p, L"--fake", L"dead",
                                       L"--out", L"x.wav"));
}

TEST(an_unreadable_health_field_is_a_usage_error_and_quotes_it)
{
    Cap c; static AprCliPlan p;

    ASSERT_EQ_INT(APR_CLI_USAGE, PARSE(&c, &p, L"--fake", L"440,sleepy",
                                       L"--out", L"x.wav"));
    ASSERT_TRUE(said(&c, L"440,sleepy"));
    ASSERT_EQ_INT(APR_CLI_USAGE, PARSE(&c, &p, L"--fake", L"440,die=later",
                                       L"--out", L"x.wav"));
    ASSERT_EQ_INT(APR_CLI_USAGE, PARSE(&c, &p, L"--fake", L"440,die=-1",
                                       L"--out", L"x.wav"));
}

TEST(one_frame_cannot_be_both_the_mute_and_the_unmute)
{
    /* The rule has ONE owner -- capture.h's fake_open refuses it -- so this
     * arrives as a capture failure carrying the reason, not as a second copy
     * of the rule in the parser. */
    Cap     c;
    wchar_t path[MAX_PATH];

    tmp_path(path, MAX_PATH, L"bothat", L"wav");
    ASSERT_EQ_INT(APR_CLI_CAPTURE, RUN(&c, L"--fake", L"440,mute=100,unmute=100",
                                       L"--out", path, L"--duration", L"0.2"));
    DeleteFileW(path);
}

TEST(the_help_says_how_to_ask_a_fake_source_for_a_health_failure)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"help"));
    ASSERT_TRUE(has(c.out, L"<health>"));
    ASSERT_TRUE(has(c.out, L"mute=<frame>"));
    ASSERT_TRUE(has(c.out, L"die=<frame>"));
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
    wchar_t wav[MAX_PATH], mp3[MAX_PATH];
    tmp_path(wav, MAX_PATH, L"ext", L"wav");
    tmp_path(mp3, MAX_PATH, L"ext", L"mp3");

    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p, L"--fake", L"440",
                                    L"--out", wav, L"--out", mp3));
    ASSERT_EQ_INT(APR_CLI_OK, apr_cli_resolve(&p, io_of(&c)));
    ASSERT_STR_EQ("wav", p.buses[0].outputs[0].action_id);
    ASSERT_STR_EQ("mp3", p.buses[0].outputs[1].action_id);
    ASSERT_FALSE(file_exists(wav));   /* resolving must not create anything */
    ASSERT_FALSE(file_exists(mp3));
}

/* The ogg action WRITES ".opus" -- RFC 7845 recommends it and the vtable says
 * so -- but ".ogg" is what most people type, and it is also the registry id.
 * Both must land on the same action with no --format, or the two spellings
 * disagree for a reason nobody can see from the command line. */
TEST(both_opus_and_ogg_extensions_reach_the_ogg_action)
{
    Cap c; static AprCliPlan p;
    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p, L"--fake", L"440",
                                    L"--out", L"a.opus", L"--out", L"b.ogg",
                                    L"--out", L"c.OGG"));
    ASSERT_EQ_INT(APR_CLI_OK, apr_cli_resolve(&p, io_of(&c)));
    ASSERT_STR_EQ("ogg", p.buses[0].outputs[0].action_id);
    ASSERT_STR_EQ("ogg", p.buses[0].outputs[1].action_id);
    ASSERT_STR_EQ("ogg", p.buses[0].outputs[2].action_id);
}

/* The id fallback must not turn every id into an extension: "none" is a
 * registry entry with no extension at all, and a file called x.none is a file
 * this build cannot write, not a request to discard the audio. */
TEST(an_action_with_no_extension_is_not_reachable_by_one)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--fake", L"440", L"--out", L"x.none",
                                      L"--dry-run"));
    ASSERT_TRUE(said(&c, L"none"));
}

TEST(format_overrides_the_extension_for_the_output_that_follows_it)
{
    Cap c; static AprCliPlan p;
    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p, L"--fake", L"440",
                                    L"--format", L"wav", L"--out", L"a.mp3",
                                    L"--out", L"b.mp3"));
    ASSERT_EQ_INT(APR_CLI_OK, apr_cli_resolve(&p, io_of(&c)));
    ASSERT_STR_EQ("wav", p.buses[0].outputs[0].action_id);
    ASSERT_STR_EQ("mp3", p.buses[0].outputs[1].action_id);
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

/* ===========================================================================
 * RECORDING TWICE
 *
 * The author's report, in full:
 *
 *   "I recorded a file called test.mp3, stopped, listened to it, and then
 *   recorded again and stopped. Turns out the file was not updated with the
 *   new recording, so I deleted it and recorded again, but the new file
 *   wasn't there."
 *
 * Two defects underneath it. The LIFETIME one -- apr_bus_add_action() used to
 * create the encoder, so a finalized action refused every later recording and
 * a deleted file left a live handle with no directory entry. And the OVERWRITE
 * one -- with the lifetime fixed, the second take would still have landed on
 * top of the first.
 *
 * WHAT THESE CASES CAN AND CANNOT SEE, because it matters: every apr_cli_run
 * builds a graph, records, and destroys it, so a command line CANNOT reach the
 * lifetime defect at all -- take two gets a brand new action either way.
 * Verified by reinstating the old lifetime: these cases stayed green and
 * tests/test_ui_behaviour.c's went red, because the UI keeps one graph across
 * both presses, which is exactly what the author did.
 *
 * So what is asserted here is THE COLLISION POLICY and what the command line
 * says about it. The lifetime is test_ui_behaviour.c's, and it is red without
 * the fix.
 *
 * Driven by a synthetic source; no audio endpoint is opened in either
 * direction.
 * ========================================================================= */

/* One 0.4-second take of a synthetic tone into `path`. */
static AprCliExit record_once(Cap *c, const wchar_t *path)
{
    return RUN(c, L"--fake", L"440,0,0.25", L"--out", path,
               L"--duration", L"0.4", L"--quiet");
}

/* "mix.wav" -> "mix-2.wav": what the collision policy does with a name that
 * is already a recording (include/outpath.h). */
static void sibling_take(const wchar_t *path, int take, wchar_t *out, size_t cch)
{
    const wchar_t *dot = wcsrchr(path, L'.');
    size_t stem = dot ? (size_t)(dot - path) : wcslen(path);
    _snwprintf_s(out, cch, _TRUNCATE, L"%.*ls-%d%ls", (int)stem, path, take,
                 dot ? dot : L"");
}

TEST(recording_twice_to_one_name_leaves_two_playable_files)
{
    Cap      c;
    wchar_t  path[MAX_PATH], second[MAX_PATH];
    uint32_t first_bytes = 0, second_bytes = 0;

    tmp_path(path, MAX_PATH, L"twice", L"wav");
    sibling_take(path, 2, second, MAX_PATH);
    DeleteFileW(path);
    DeleteFileW(second);

    ASSERT_EQ_INT(APR_CLI_OK, record_once(&c, path));
    ASSERT_TRUE(wav_is_playable(path, &first_bytes));

    /* THE SECOND TAKE. Before the lifetime fix this wrote nothing at all: the
     * action had been finalized by the first stop and refused audio for ever
     * afterwards. */
    ASSERT_EQ_INT(APR_CLI_OK, record_once(&c, path));

    /* Two files, both playable, and the FIRST ONE IS STILL THE FIRST TAKE --
     * a recorder that loses a previous take is the thing this is here to
     * prevent. */
    ASSERT_TRUE(wav_is_playable(path, &first_bytes));
    printf("      take 1: [%ls] %u bytes\n", path, first_bytes);
    ASSERT_TRUE(file_exists(second));
    ASSERT_TRUE(wav_is_playable(second, &second_bytes));
    printf("      take 2: [%ls] %u bytes\n", second, second_bytes);
    ASSERT_GT_INT(40000, (int)second_bytes);

    DeleteFileW(path);
    DeleteFileW(second);
}

TEST(a_take_deleted_between_recordings_comes_back)
{
    /* The third act of the report. Deleting the file did not help, because
     * Windows keeps an open handle valid with no directory entry -- so the
     * next recording went to a file with no name and appeared to vanish.
     * Nothing is open between recordings now. */
    Cap      c;
    wchar_t  path[MAX_PATH], second[MAX_PATH];
    uint32_t bytes = 0;

    tmp_path(path, MAX_PATH, L"deleted", L"wav");
    sibling_take(path, 2, second, MAX_PATH);
    DeleteFileW(path);
    DeleteFileW(second);

    ASSERT_EQ_INT(APR_CLI_OK, record_once(&c, path));
    ASSERT_TRUE(file_exists(path));

    ASSERT_TRUE(DeleteFileW(path) != 0);
    ASSERT_FALSE(file_exists(path));

    /* The name is free again, so it is used again -- no take number, because
     * there is no take to protect. */
    ASSERT_EQ_INT(APR_CLI_OK, record_once(&c, path));
    ASSERT_TRUE(file_exists(path));
    ASSERT_TRUE(wav_is_playable(path, &bytes));
    ASSERT_GT_INT(40000, (int)bytes);
    ASSERT_FALSE(file_exists(second));

    DeleteFileW(path);
}

TEST(the_take_that_moved_aside_is_named_out_loud)
{
    /* Auto-increment without a sentence is only a politer surprise. The run
     * says which name was taken and which one the audio went to, and the
     * "recording to" line names the file that really exists. */
    Cap     c;
    wchar_t path[MAX_PATH], second[MAX_PATH];

    tmp_path(path, MAX_PATH, L"named", L"wav");
    sibling_take(path, 2, second, MAX_PATH);
    DeleteFileW(path);
    DeleteFileW(second);

    ASSERT_EQ_INT(APR_CLI_OK, record_once(&c, path));

    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--fake", L"440,0,0.25", L"--out", path,
                                  L"--duration", L"0.4"));
    printf("      said: %ls", c.out);
    printf("      err:  %ls", c.err);
    /* The warning names the file the audio really went to, and so does the
     * closing summary -- which used to print the name that was ASKED for and
     * would now send the user looking for a file that is not there. */
    ASSERT_TRUE(has(c.err, second));
    ASSERT_TRUE(has(c.out, second));

    DeleteFileW(path);
    DeleteFileW(second);
}

/* ===========================================================================
 * Output names are templates
 * ========================================================================= */

TEST(an_output_name_may_be_a_template_and_the_file_is_the_expansion)
{
    Cap     c;
    wchar_t dir[MAX_PATH], tmpl[MAX_PATH], want[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, dir);

    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(tmpl, MAX_PATH, _TRUNCATE, L"%lsapr_cli_t%lu_{bus}_{n}.wav",
                 dir, GetCurrentProcessId());
    _snwprintf_s(want, MAX_PATH, _TRUNCATE, L"%lsapr_cli_t%lu_Voice_1.wav",
                 dir, GetCurrentProcessId());
    DeleteFileW(want);

    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--bus", L"Voice", L"--fake", L"440,0,0.25",
                                  L"--out", tmpl, L"--duration", L"0.4", L"--quiet"));
    printf("      template: [%ls]\n", tmpl);
    printf("      file:     [%ls]\n", want);
    ASSERT_TRUE(wav_is_playable(want, NULL));
    /* And nothing was created under the literal name, braces and all. */
    ASSERT_FALSE(file_exists(tmpl));

    DeleteFileW(want);
}

TEST(two_buses_may_share_one_template_because_it_names_two_files)
{
    /* Comparing the TEMPLATES rather than what they expand to would refuse
     * this out of hand -- and it is the ordinary way to record two buses. */
    Cap     c;
    wchar_t dir[MAX_PATH], tmpl[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, dir);

    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(tmpl, MAX_PATH, _TRUNCATE, L"%lsapr_cli_s%lu_{bus}.wav",
                 dir, GetCurrentProcessId());
    _snwprintf_s(a, MAX_PATH, _TRUNCATE, L"%lsapr_cli_s%lu_One.wav",
                 dir, GetCurrentProcessId());
    _snwprintf_s(b, MAX_PATH, _TRUNCATE, L"%lsapr_cli_s%lu_Two.wav",
                 dir, GetCurrentProcessId());
    DeleteFileW(a);
    DeleteFileW(b);

    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c,
        L"--bus", L"One", L"--fake", L"440,0,0.25", L"--out", tmpl,
        L"--bus", L"Two", L"--fake", L"220,0,0.25", L"--out", tmpl,
        L"--duration", L"0.4", L"--quiet"));

    ASSERT_TRUE(wav_is_playable(a, NULL));
    ASSERT_TRUE(wav_is_playable(b, NULL));
    DeleteFileW(a);
    DeleteFileW(b);
}

TEST(one_template_that_names_one_file_on_two_buses_is_still_a_duplicate)
{
    /* The check did not get weaker, only more accurate: two outputs that
     * expand to the same path are still refused, because two recordings
     * cannot share one file. */
    Cap     c;
    wchar_t dir[MAX_PATH], tmpl[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, dir);

    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(tmpl, MAX_PATH, _TRUNCATE, L"%lsapr_cli_d%lu_{date}.wav",
                 dir, GetCurrentProcessId());

    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c,
        L"--bus", L"One", L"--fake", L"440", L"--out", tmpl,
        L"--bus", L"Two", L"--fake", L"220", L"--out", tmpl,
        L"--dry-run"));
    ASSERT_TRUE(said(&c, L"{date}"));
}

TEST(a_template_is_never_probed_with_its_braces_in_it)
{
    /* The early check has to expand first: probing "x.{ext}" literally would
     * create a file called exactly that and leave it behind. */
    Cap     c;
    wchar_t dir[MAX_PATH], tmpl[MAX_PATH], want[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, dir);

    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(tmpl, MAX_PATH, _TRUNCATE, L"%lsapr_cli_p%lu_{bus}.wav",
                 dir, GetCurrentProcessId());
    _snwprintf_s(want, MAX_PATH, _TRUNCATE, L"%lsapr_cli_p%lu_Main.wav",
                 dir, GetCurrentProcessId());

    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--bus", L"Main", L"--fake", L"440",
                                  L"--out", tmpl, L"--dry-run"));
    ASSERT_FALSE(file_exists(tmpl));
    /* --dry-run creates nothing, including the file the probe had to make to
     * answer the question. */
    ASSERT_FALSE(file_exists(want));
}

TEST(the_help_says_what_may_go_in_an_output_name)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"help"));
    ASSERT_TRUE(said(&c, L"{date}"));
    ASSERT_TRUE(said(&c, L"{n}"));
}

/* ---------------------------------------------------------------------------
 * The two failures that look exactly like a successful recording
 *
 * A muted source and a dead one both produce digital silence at exactly the
 * right rate, for ever, with no error anywhere (capture.h, design 4.1 #5/#6).
 * Every other indicator says the run went fine, so what the command line says
 * about them IS the whole warning -- there is nothing else to notice.
 *
 * These drive the shipped record path, warning text included, through a
 * synthetic source asked to fail on cue. No audio hardware is involved in
 * either direction (AGENTS.md rule 1).
 * ------------------------------------------------------------------------- */

/* The expected line, built from the catalog rather than typed here, so the
 * case pins the WARNING and not the wording of one language. */
static void warn_line(AprStrId id, const wchar_t *name, wchar_t *out, size_t cch)
{
    const wchar_t *args[1];
    args[0] = name;
    apr_str_format(id, out, cch, args, 1);
}

static int count_of(const wchar_t *hay, const wchar_t *needle)
{
    size_t n = wcslen(needle);
    int    seen = 0;
    if (!n) return 0;
    while ((hay = wcsstr(hay, needle)) != NULL) { seen++; hay += n; }
    return seen;
}

/* Both streams: a warning must be counted wherever it was written. */
static int said_times(const Cap *c, const wchar_t *needle)
{
    return count_of(c->out, needle) + count_of(c->err, needle);
}

TEST(a_source_that_starts_muted_is_warned_about_exactly_once)
{
    Cap     c;
    wchar_t path[MAX_PATH], expect[512];

    tmp_path(path, MAX_PATH, L"mute0", L"wav");
    warn_line(APR_S_WARN_SOURCE_MUTED, L"440,0,0.25,muted", expect, 512);

    /* Muted is NOT incomplete: what was asked for is what was recorded, and
     * silence was the honest answer. Only death changes the exit code. */
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--fake", L"440,0,0.25,muted",
                                  L"--out", path, L"--duration", L"0.5"));
    /* Once. Health is polled every tick and a warning per tick is a warning
     * nobody reads. */
    ASSERT_EQ_INT(1, said_times(&c, expect));
    ASSERT_TRUE(wav_is_playable(path, NULL));
    DeleteFileW(path);
}

TEST(a_source_that_goes_muted_part_way_through_is_warned_about_exactly_once)
{
    Cap     c;
    wchar_t path[MAX_PATH], expect[512];

    /* 4800 frames is 100 ms in at 48 kHz -- well inside the recording, and a
     * frame index rather than a time so it lands on the same sample however
     * the timeline is stepped. */
    tmp_path(path, MAX_PATH, L"mutemid", L"wav");
    warn_line(APR_S_WARN_SOURCE_MUTED, L"440,0,0.25,mute=4800", expect, 512);

    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--fake", L"440,0,0.25,mute=4800",
                                  L"--out", path, L"--duration", L"0.5"));
    ASSERT_EQ_INT(1, said_times(&c, expect));
    ASSERT_TRUE(wav_is_playable(path, NULL));
    DeleteFileW(path);
}

TEST(a_source_that_dies_part_way_through_ends_the_run_incomplete)
{
    Cap     c;
    wchar_t path[MAX_PATH], expect[512];

    tmp_path(path, MAX_PATH, L"diemid", L"wav");
    warn_line(APR_S_WARN_SOURCE_EXITED, L"440,0,0.25,die=4800", expect, 512);

    /* 6, not 0. The file is complete and playable and it is NOT what was
     * asked for, which is exactly what exit 6 is for -- a script that only
     * checked for a file would call this a success. */
    ASSERT_EQ_INT(APR_CLI_INCOMPLETE, RUN(&c, L"--fake", L"440,0,0.25,die=4800",
                                          L"--out", path, L"--duration", L"0.5"));
    ASSERT_EQ_INT(1, said_times(&c, expect));
    ASSERT_TRUE(wav_is_playable(path, NULL));
    DeleteFileW(path);
}

TEST(a_dead_source_is_not_also_described_as_muted)
{
    /* Death wins, the same way src/ui/tree_panel.c picks one clause for the
     * row a screen reader reads: unmuting an application that has exited
     * fixes nothing, so the one sentence worth acting on is the only one
     * said. */
    Cap     c;
    wchar_t path[MAX_PATH], muted[512], exited[512];

    tmp_path(path, MAX_PATH, L"bothstates", L"wav");
    warn_line(APR_S_WARN_SOURCE_MUTED,  L"440,0,0.25,muted,dead", muted,  512);
    warn_line(APR_S_WARN_SOURCE_EXITED, L"440,0,0.25,muted,dead", exited, 512);

    ASSERT_EQ_INT(APR_CLI_INCOMPLETE, RUN(&c, L"--fake", L"440,0,0.25,muted,dead",
                                          L"--out", path, L"--duration", L"0.4"));
    ASSERT_EQ_INT(1, said_times(&c, exited));
    ASSERT_EQ_INT(0, said_times(&c, muted));
    ASSERT_TRUE(wav_is_playable(path, NULL));
    DeleteFileW(path);
}

TEST(a_source_that_goes_silent_and_later_dies_says_both_things)
{
    /* Only SIMULTANEOUS states collapse. Two things that happened at two
     * different moments are two things that happened. */
    Cap     c;
    wchar_t path[MAX_PATH], muted[512], exited[512];

    /* 50 ms apart would be enough for a 10 ms health poll; they are 450 ms
     * apart so that a scheduler hiccup on a loaded machine cannot collapse
     * them into one moment and make this case flap. */
    tmp_path(path, MAX_PATH, L"thenboth", L"wav");
    warn_line(APR_S_WARN_SOURCE_MUTED,  L"440,0,0.25,mute=2400,die=24000", muted,  512);
    warn_line(APR_S_WARN_SOURCE_EXITED, L"440,0,0.25,mute=2400,die=24000", exited, 512);

    ASSERT_EQ_INT(APR_CLI_INCOMPLETE,
                  RUN(&c, L"--fake", L"440,0,0.25,mute=2400,die=24000",
                      L"--out", path, L"--duration", L"0.8"));
    ASSERT_EQ_INT(1, said_times(&c, muted));
    ASSERT_EQ_INT(1, said_times(&c, exited));
    DeleteFileW(path);
}

TEST(one_source_failing_does_not_take_the_other_down)
{
    /* Design section 10. The healthy bus must still get its whole file. */
    Cap     c;
    wchar_t good[MAX_PATH], bad[MAX_PATH];

    tmp_path(good, MAX_PATH, L"survivor", L"wav");
    tmp_path(bad,  MAX_PATH, L"casualty", L"wav");

    ASSERT_EQ_INT(APR_CLI_INCOMPLETE, RUN(&c,
        L"--bus", L"Good", L"--fake", L"440,0,0.25",      L"--out", good,
        L"--bus", L"Bad",  L"--fake", L"220,0,0.25,dead", L"--out", bad,
        L"--duration", L"0.4"));
    ASSERT_TRUE(wav_is_playable(good, NULL));
    ASSERT_TRUE(wav_is_playable(bad, NULL));
    DeleteFileW(good);
    DeleteFileW(bad);
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
 * What an encoder will and will not accept -- asked BEFORE recording (M1)
 * ========================================================================= */

/* THE ONE THAT COST A TAKE.
 *
 * `--bitrate 400 --out meeting.mp3 --duration 3600 --json` parsed, passed
 * --dry-run, and then recorded for a full hour into nothing: LAME refuses 400
 * kbps, but the refusal could only happen inside create(), which runs at
 * apr_bus_start -- after the recording has begun, where a failed output is
 * downgraded to a per-output skip. --json swallowed the warning and the final
 * document listed meeting.mp3 with "seconds": 3600 beside exit code 6, whose
 * documented meaning is "recorded and playable".
 *
 * This test runs the same command line. It must come back in milliseconds, as
 * a configuration error, having opened nothing -- so the 3600 here is not a
 * slow test, it is the point. */
TEST(a_bitrate_the_format_refuses_is_refused_before_any_recording_starts)
{
    Cap c;
    wchar_t mp3[MAX_PATH];

    tmp_path(mp3, MAX_PATH, L"kbps", L"mp3");
    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--fake", L"440",
                                      L"--bitrate", L"400", L"--out", mp3,
                                      L"--duration", L"3600", L"--json"));
    ASSERT_FALSE(file_exists(mp3));
    /* --json still gets a document, and it says config rather than 6. */
    ASSERT_TRUE(said(&c, L"\"exitCode\": 2"));
}

TEST(a_dry_run_refuses_a_bitrate_the_format_cannot_write)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--fake", L"440",
                                      L"--bitrate", L"400", L"--out", L"x.mp3",
                                      L"--dry-run"));
    ASSERT_TRUE(said(&c, L"x.mp3"));
    ASSERT_FALSE(file_exists(L"x.mp3"));
}

/* Channels are the same shape of refusal and neither lossy format can do more
 * than two, so a session at 8 channels has to be told at plan time and not an
 * hour in. WAV takes all eight, which is why it is here too: the check is the
 * encoder's, not a blanket rule. */
TEST(more_channels_than_the_format_carries_is_refused_at_plan_time)
{
    Cap c;

    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--fake", L"440", L"--channels", L"8",
                                      L"--out", L"x.opus", L"--dry-run"));
    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--fake", L"440", L"--channels", L"8",
                                      L"--out", L"x.mp3", L"--dry-run"));
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--fake", L"440", L"--channels", L"8",
                                  L"--out", L"x.wav", L"--dry-run"));
    ASSERT_FALSE(file_exists(L"x.wav"));
}

/* A RUN THAT OPENED NO FILE AT ALL IS NOT A RECORDING.
 *
 * Some create() failures cannot be predicted from the numbers -- a volume that
 * refuses every write is the honest example -- so the second half of M1 is
 * what the run REPORTS when they happen. It used to report exit 6, documented
 * as "recorded and playable", with each output listed at the full duration of
 * the run: a path, a length, and no file. */
TEST(a_run_whose_every_output_failed_to_open_is_not_a_success)
{
    Cap     c;
    wchar_t opus[MAX_PATH];

    tmp_path(opus, MAX_PATH, L"noopen", L"opus");

    /* Every write refused, so the ogg action's create() cannot lay down its
     * header pages and the output never opens. */
    apr_ogg_test_fail_after_bytes = 1;
    ASSERT_EQ_INT(APR_CLI_OUTPUT, RUN(&c, L"--fake", L"440", L"--out", opus,
                                      L"--duration", L"0.3", L"--json"));
    apr_ogg_test_fail_after_bytes = 0;

    /* Exit 4 -- "a file could not be created" -- and the document says so
     * rather than quietly listing a duration for a file nobody can open. */
    ASSERT_TRUE(said(&c, L"\"exitCode\": 4"));
    ASSERT_TRUE(said(&c, L"\"ok\": false"));
    ASSERT_TRUE(said(&c, L"\"failed\": true"));
    ASSERT_TRUE(said(&c, L"\"seconds\": 0.000"));

    DeleteFileW(opus);
}

/* And the asymmetry: one output of two failing is still a recording, so it
 * stays exit 6 and the file that DID open is reported with its real length. */
TEST(one_output_failing_while_another_records_is_still_incomplete_not_a_failure)
{
    Cap     c;
    wchar_t wav[MAX_PATH], opus[MAX_PATH];

    tmp_path(wav,  MAX_PATH, L"half", L"wav");
    tmp_path(opus, MAX_PATH, L"half", L"opus");

    apr_ogg_test_fail_after_bytes = 1;
    ASSERT_EQ_INT(APR_CLI_INCOMPLETE, RUN(&c, L"--fake", L"440",
                                          L"--out", wav, L"--out", opus,
                                          L"--duration", L"0.3", L"--json"));
    apr_ogg_test_fail_after_bytes = 0;

    ASSERT_TRUE(said(&c, L"\"exitCode\": 6"));
    ASSERT_TRUE(file_exists(wav));
    DeleteFileW(wav);
    DeleteFileW(opus);
}

/* ===========================================================================
 * The small ones
 * ========================================================================= */

/* m16: the extension pass has always matched case-insensitively, so
 * `--out x.WAV` worked while `--format WAV` was refused as "a format this
 * build cannot write" -- a sentence that was simply untrue. Both spellings
 * now reach the action, and what is stored is the canonical id, because that
 * is the wire value a session file holds. */
TEST(a_format_named_in_capitals_is_the_same_format)
{
    Cap c; static AprCliPlan p;
    wchar_t out[MAX_PATH];

    tmp_path(out, MAX_PATH, L"caps", L"dat");
    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p, L"--fake", L"440",
                                    L"--format", L"WAV", L"--out", out));
    ASSERT_EQ_INT(APR_CLI_OK, apr_cli_resolve(&p, io_of(&c)));
    ASSERT_STR_EQ("wav", p.buses[0].outputs[0].action_id);
    ASSERT_FALSE(file_exists(out));
}

/* m17: {n} means "the lowest free number", resolved against the disk when each
 * recording starts, so two buses using one numbered template name two files.
 * Comparing their expansions cannot see that -- without the disk every {n} is
 * 1 -- and the pair was refused as a duplicate. */
TEST(two_buses_may_share_a_numbered_template_because_it_names_two_files)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--bus", L"A", L"--fake", L"440",
                                  L"--out", L"take{n}.wav",
                                  L"--bus", L"B", L"--fake", L"220",
                                  L"--out", L"take{n}.wav", L"--dry-run"));
    ASSERT_FALSE(file_exists(L"take1.wav"));
}

/* m18: --json and --quiet may be written after the option that fails, so they
 * are read in a pass of their own -- but a pass that looked at every word
 * found them inside somebody else's VALUE. `--log-file --json` names a log
 * file called "--json"; it does not ask for JSON. */
TEST(a_flag_that_is_another_options_value_is_not_that_flag)
{
    Cap c; static AprCliPlan p;

    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p, L"--fake", L"440", L"--out", L"a.wav",
                                    L"--log-file", L"--json"));
    ASSERT_EQ_INT(0, p.json);
    ASSERT_WSTR_EQ(L"--json", p.log_file);

    ASSERT_EQ_INT(APR_CLI_OK, PARSE(&c, &p, L"--fake", L"440", L"--out", L"a.wav",
                                    L"--log-file", L"--quiet"));
    ASSERT_EQ_INT(0, p.quiet);
}

/* m19: cli.h promises the console close handler blocks until the files are
 * closed. A four-second cap did not risk a kill part-way through finalize, it
 * GUARANTEED one at four seconds. */
TEST(the_console_close_handler_waits_for_the_files_without_a_deadline)
{
    ASSERT_EQ_U64((uint64_t)INFINITE, (uint64_t)apr_cli_test_close_wait_ms());
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
