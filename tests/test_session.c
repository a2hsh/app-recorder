/*
 * test_session.c -- sessions: what survives a save, what a broken file does,
 * and every way a process or a device can fail to be the same thing tomorrow.
 *
 * NOTHING HERE RENDERS AUDIO (AGENTS.md rule 1). The only cases that actually
 * record use --fake, which synthesises into a ring and never touches an audio
 * endpoint in either direction. Every resolution case runs against a SUPPLIED
 * machine (AprSessionMachine) rather than the real one, which is what makes
 * "two copies of Chrome are running" and "the GoXLR is unplugged" ordinary
 * test cases instead of things that need a rig.
 *
 * The privacy-sensitive mode is exercised only through load and --dry-run. A
 * test that actually STARTED an EXCLUDE capture would record whatever the
 * author happened to be listening to, so none does.
 */
#include "test_runner.h"

#include <windows.h>
#include <string.h>
#include <wchar.h>

#include "cli/cli.h"
#include "session.h"
#include "strings.h"

/* ---------------------------------------------------------------------------
 * Fixtures
 * ------------------------------------------------------------------------- */

#define JSON_CAP 262144

static char  g_json[JSON_CAP];
static AprSession g_a, g_b;
static AprSessionLoadReport    g_lrep;
static AprSessionResolveReport g_rrep;

/* apr_* functions return AprErr BY VALUE, and a return value is not an
 * l-value in C -- failed(f(...)) does not compile. One helper rather than
 * a temporary at every call site. */
static int failed(AprErr e) { return apr_failed(&e); }

static void tmp_path(wchar_t *buf, size_t cch, const wchar_t *tag,
                     const wchar_t *ext)
{
    static LONG counter;
    wchar_t dir[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(buf, cch, _TRUNCATE, L"%lsapr_sess_%ls_%lu_%ld.%ls",
                 dir, tag, GetCurrentProcessId(),
                 InterlockedIncrement(&counter), ext);
}

static int file_exists(const wchar_t *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static AprErr write_text_file(const wchar_t *path, const char *text)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD  wrote = 0;
    if (h == INVALID_HANDLE_VALUE) return APR_ERR_LAST(L"create %ls", path);
    WriteFile(h, text, (DWORD)strlen(text), &wrote, NULL);
    CloseHandle(h);
    return apr_ok();
}

/* A session with two buses sharing one source -- the shape that makes this a
 * graph rather than a tree, and the whole reason the file has a top-level
 * source list. */
static void build_multibus(AprSession *s)
{
    apr_session_init(s);
    s->sample_rate = 44100;
    s->channels    = 2;
    s->duration_ms = 1500;

    s->source_count = 3;

    strcpy_s(s->sources[0].key, APR_SESSION_KEY_CCH, "teams");
    s->sources[0].kind = APR_SESSION_SRC_PROCESS;
    wcscpy_s(s->sources[0].name, APR_NAME_CCH, L"Teams");
    s->sources[0].pid = 8412;
    wcscpy_s(s->sources[0].exe, APR_DISC_NAME_CCH, L"Teams.exe");
    wcscpy_s(s->sources[0].path, APR_DISC_PATH_CCH,
             L"C:\\Program Files\\Teams\\Teams.exe");
    wcscpy_s(s->sources[0].window_class, APR_SESSION_CLASS_CCH,
             L"Chrome_WidgetWin_1");

    strcpy_s(s->sources[1].key, APR_SESSION_KEY_CCH, "mic");
    s->sources[1].kind = APR_SESSION_SRC_DEVICE;
    wcscpy_s(s->sources[1].name, APR_NAME_CCH, L"Chat Mic");
    wcscpy_s(s->sources[1].endpoint_id, APR_DISC_ENDPOINT_CCH,
             L"{0.0.1.00000000}.{7b1c0000-dead-beef-0000-000000000001}");
    wcscpy_s(s->sources[1].endpoint_name, APR_DISC_NAME_CCH,
             L"Chat Mic (TC-Helicon GoXLR)");

    strcpy_s(s->sources[2].key, APR_SESSION_KEY_CCH, "tone");
    s->sources[2].kind = APR_SESSION_SRC_FAKE;
    wcscpy_s(s->sources[2].name, APR_NAME_CCH, L"test tone");
    s->sources[2].fake_hz  = 440;
    s->sources[2].fake_ppm = 30;
    s->sources[2].fake_amp = 0.25f;

    s->bus_count = 2;

    wcscpy_s(s->buses[0].name, APR_NAME_CCH, L"Mix");
    s->buses[0].edge_count = 3;
    strcpy_s(s->buses[0].edges[0].key, APR_SESSION_KEY_CCH, "teams");
    s->buses[0].edges[0].gain_db_tenths = -60;
    strcpy_s(s->buses[0].edges[1].key, APR_SESSION_KEY_CCH, "mic");
    s->buses[0].edges[1].gain_db_tenths = 0;
    strcpy_s(s->buses[0].edges[2].key, APR_SESSION_KEY_CCH, "tone");
    s->buses[0].edges[2].gain_db_tenths = -125;
    s->buses[0].action_count = 2;
    strcpy_s(s->buses[0].actions[0].id, 16, "wav");
    wcscpy_s(s->buses[0].actions[0].path, APR_DISC_PATH_CCH, L"C:\\rec\\mix.wav");
    strcpy_s(s->buses[0].actions[1].id, 16, "m4a");
    wcscpy_s(s->buses[0].actions[1].path, APR_DISC_PATH_CCH, L"C:\\rec\\mix.m4a");
    s->buses[0].actions[1].bitrate_kbps = 192;
    s->buses[0].actions[1].quality      = 4;

    wcscpy_s(s->buses[1].name, APR_NAME_CCH, L"Voice only");
    s->buses[1].edge_count = 1;
    /* THE SAME SOURCE, at a different gain. One entry, two references. */
    strcpy_s(s->buses[1].edges[0].key, APR_SESSION_KEY_CCH, "mic");
    s->buses[1].edges[0].gain_db_tenths = 35;
    s->buses[1].action_count = 1;
    strcpy_s(s->buses[1].actions[0].id, 16, "wav");
    wcscpy_s(s->buses[1].actions[0].path, APR_DISC_PATH_CCH, L"C:\\rec\\voice.wav");
}

/* --- a machine we can describe completely --------------------------------- */

#define MACH_APPS 8
#define MACH_EPS  4

typedef struct Machine {
    AprAudioApp      apps[MACH_APPS];
    size_t           app_count;
    AprAudioEndpoint eps[MACH_EPS];
    size_t           ep_count;

    uint32_t class_pid[MACH_APPS];
    wchar_t  class_name[MACH_APPS][APR_SESSION_CLASS_CCH];
    size_t   class_count;

    AprSessionMachine view;
} Machine;

static const wchar_t *mach_class(void *user, uint32_t pid)
{
    Machine *m = (Machine *)user;
    size_t   i;
    for (i = 0; i < m->class_count; i++)
        if (m->class_pid[i] == pid) return m->class_name[i];
    return NULL;
}

static int mach_exists(void *user, uint32_t pid)
{
    Machine *m = (Machine *)user;
    size_t   i;
    for (i = 0; i < m->app_count; i++) if (m->apps[i].pid == pid) return 1;
    return 0;
}

static void mach_init(Machine *m)
{
    memset(m, 0, sizeof *m);
    m->view.apps           = m->apps;
    m->view.endpoints      = m->eps;
    m->view.window_class   = mach_class;
    m->view.process_exists = mach_exists;
    m->view.user           = m;
}

static void mach_app(Machine *m, uint32_t pid, const wchar_t *exe,
                     const wchar_t *path, int muted)
{
    AprAudioApp *a = &m->apps[m->app_count++];
    memset(a, 0, sizeof *a);
    a->pid = pid;
    wcscpy_s(a->exe, APR_DISC_NAME_CCH, exe);
    wcscpy_s(a->display, APR_DISC_NAME_CCH, exe);
    wcscpy_s(a->path, APR_DISC_PATH_CCH, path);
    a->active = 1;
    a->muted  = muted;
    a->volume = muted ? 0.0f : 1.0f;
    m->view.app_count = m->app_count;
}

static void mach_class_of(Machine *m, uint32_t pid, const wchar_t *cls)
{
    m->class_pid[m->class_count] = pid;
    wcscpy_s(m->class_name[m->class_count], APR_SESSION_CLASS_CCH, cls);
    m->class_count++;
}

static void mach_endpoint(Machine *m, const wchar_t *id, const wchar_t *name)
{
    AprAudioEndpoint *e = &m->eps[m->ep_count++];
    memset(e, 0, sizeof *e);
    wcscpy_s(e->id, APR_DISC_ENDPOINT_CCH, id);
    wcscpy_s(e->name, APR_DISC_NAME_CCH, name);
    m->view.endpoint_count = m->ep_count;
}

static const AprSessionResolution *res_for(const AprSessionResolveReport *r,
                                           size_t source_index)
{
    size_t i;
    for (i = 0; i < r->count; i++)
        if (r->items[i].source_index == source_index) return &r->items[i];
    return NULL;
}

/* A one-source session, so a resolution case is three lines instead of thirty. */
static void one_process_source(AprSession *s, const wchar_t *exe,
                               const wchar_t *path, const wchar_t *cls,
                               uint32_t pid_hint)
{
    apr_session_init(s);
    s->source_count = 1;
    strcpy_s(s->sources[0].key, APR_SESSION_KEY_CCH, "a");
    s->sources[0].kind = APR_SESSION_SRC_PROCESS;
    wcscpy_s(s->sources[0].name, APR_NAME_CCH, exe);
    wcscpy_s(s->sources[0].exe, APR_DISC_NAME_CCH, exe);
    wcscpy_s(s->sources[0].path, APR_DISC_PATH_CCH, path);
    if (cls) wcscpy_s(s->sources[0].window_class, APR_SESSION_CLASS_CCH, cls);
    s->sources[0].pid = pid_hint;

    s->bus_count = 1;
    wcscpy_s(s->buses[0].name, APR_NAME_CCH, L"Bus");
    s->buses[0].edge_count = 1;
    strcpy_s(s->buses[0].edges[0].key, APR_SESSION_KEY_CCH, "a");
    s->buses[0].action_count = 1;
    strcpy_s(s->buses[0].actions[0].id, 16, "wav");
    wcscpy_s(s->buses[0].actions[0].path, APR_DISC_PATH_CCH, L"out.wav");
}

static AprSessionResolveOptions no_opts(void)
{
    AprSessionResolveOptions o;
    memset(&o, 0, sizeof o);
    return o;
}

/* ===========================================================================
 * Round trip
 * ========================================================================= */

TEST(a_multi_bus_graph_survives_a_save_and_a_load)
{
    size_t len = 0, bi, ei, ai;
    AprErr e;

    build_multibus(&g_a);
    e = apr_session_write_utf8(&g_a, g_json, JSON_CAP, &len);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_GT_INT(0, (int)len);

    e = apr_session_load_utf8(g_json, len, &g_b, &g_lrep);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(APR_SESSION_FAULT_NONE, (int)g_lrep.fault);

    ASSERT_EQ_INT(44100, (int)g_b.sample_rate);
    ASSERT_EQ_INT(2, (int)g_b.channels);
    ASSERT_EQ_INT(1500, (int)g_b.duration_ms);
    ASSERT_EQ_INT(3, (int)g_b.source_count);
    ASSERT_EQ_INT(2, (int)g_b.bus_count);

    ASSERT_STR_EQ("teams", g_b.sources[0].key);
    ASSERT_EQ_INT(APR_SESSION_SRC_PROCESS, (int)g_b.sources[0].kind);
    ASSERT_WSTR_EQ(L"Teams", g_b.sources[0].name);
    ASSERT_EQ_INT(8412, (int)g_b.sources[0].pid);
    ASSERT_WSTR_EQ(L"Teams.exe", g_b.sources[0].exe);
    ASSERT_WSTR_EQ(L"C:\\Program Files\\Teams\\Teams.exe", g_b.sources[0].path);
    ASSERT_WSTR_EQ(L"Chrome_WidgetWin_1", g_b.sources[0].window_class);

    ASSERT_EQ_INT(APR_SESSION_SRC_DEVICE, (int)g_b.sources[1].kind);
    ASSERT_WSTR_EQ(L"{0.0.1.00000000}.{7b1c0000-dead-beef-0000-000000000001}",
                   g_b.sources[1].endpoint_id);
    ASSERT_WSTR_EQ(L"Chat Mic (TC-Helicon GoXLR)", g_b.sources[1].endpoint_name);

    ASSERT_EQ_INT(APR_SESSION_SRC_FAKE, (int)g_b.sources[2].kind);
    ASSERT_EQ_INT(440, (int)g_b.sources[2].fake_hz);
    ASSERT_EQ_INT(30,  (int)g_b.sources[2].fake_ppm);
    ASSERT_NEAR(0.25,  g_b.sources[2].fake_amp, 1e-6);

    ASSERT_WSTR_EQ(L"Mix", g_b.buses[0].name);
    ASSERT_EQ_INT(3, (int)g_b.buses[0].edge_count);
    ASSERT_EQ_INT(2, (int)g_b.buses[0].action_count);
    ASSERT_STR_EQ("m4a", g_b.buses[0].actions[1].id);
    ASSERT_EQ_INT(192, g_b.buses[0].actions[1].bitrate_kbps);
    ASSERT_EQ_INT(4,   g_b.buses[0].actions[1].quality);

    /* Every edge's gain, exactly, including the fractional one. */
    for (bi = 0; bi < g_a.bus_count; bi++) {
        for (ei = 0; ei < g_a.buses[bi].edge_count; ei++) {
            ASSERT_STR_EQ(g_a.buses[bi].edges[ei].key,
                          g_b.buses[bi].edges[ei].key);
            ASSERT_EQ_INT(g_a.buses[bi].edges[ei].gain_db_tenths,
                          g_b.buses[bi].edges[ei].gain_db_tenths);
        }
        for (ai = 0; ai < g_a.buses[bi].action_count; ai++) {
            ASSERT_STR_EQ(g_a.buses[bi].actions[ai].id,
                          g_b.buses[bi].actions[ai].id);
            ASSERT_WSTR_EQ(g_a.buses[bi].actions[ai].path,
                           g_b.buses[bi].actions[ai].path);
        }
    }
}

TEST(one_source_feeding_two_buses_is_one_entry_and_two_gains)
{
    size_t len = 0;
    AprErr e;

    build_multibus(&g_a);
    e = apr_session_write_utf8(&g_a, g_json, JSON_CAP, &len);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_session_load_utf8(g_json, len, &g_b, &g_lrep);
    ASSERT_FALSE(apr_failed(&e));

    /* "mic" is stored once ... */
    ASSERT_EQ_INT(3, (int)g_b.source_count);
    /* ... referenced from both buses ... */
    ASSERT_STR_EQ("mic", g_b.buses[0].edges[1].key);
    ASSERT_STR_EQ("mic", g_b.buses[1].edges[0].key);
    /* ... with per-edge gain, which is what makes this a graph (design 3.2). */
    ASSERT_EQ_INT(0,  g_b.buses[0].edges[1].gain_db_tenths);
    ASSERT_EQ_INT(35, g_b.buses[1].edges[0].gain_db_tenths);
    /* Both references point at the same entry. */
    ASSERT_EQ_INT(1, (int)g_b.buses[0].edges[1].source_index);
    ASSERT_EQ_INT(1, (int)g_b.buses[1].edges[0].source_index);
}

TEST(a_session_round_trips_through_a_real_file)
{
    wchar_t path[MAX_PATH];
    AprErr  e;

    tmp_path(path, MAX_PATH, L"rt", L"json");
    build_multibus(&g_a);
    e = apr_session_save(&g_a, path);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_TRUE(file_exists(path));

    e = apr_session_load(path, &g_b, &g_lrep);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(3, (int)g_b.source_count);
    ASSERT_EQ_INT(2, (int)g_b.bus_count);
    ASSERT_EQ_INT(APR_SESSION_FORMAT_VERSION, g_lrep.version);
    ASSERT_EQ_INT(0, g_lrep.from_newer_writer);
    DeleteFileW(path);
}

TEST(paths_with_backslashes_and_non_ascii_survive_the_json_escaping)
{
    size_t len = 0;
    AprErr e;

    apr_session_init(&g_a);
    g_a.source_count = 1;
    strcpy_s(g_a.sources[0].key, APR_SESSION_KEY_CCH, "a");
    g_a.sources[0].kind = APR_SESSION_SRC_FAKE;
    /* A quote, a backslash, a tab and something outside the BMP-ASCII range:
     * every escaping decision the writer has to make, in one string. */
    wcscpy_s(g_a.sources[0].name, APR_NAME_CCH, L"he said \"hi\"\tمرحبا");
    g_a.sources[0].fake_hz = 440;
    g_a.bus_count = 1;
    wcscpy_s(g_a.buses[0].name, APR_NAME_CCH, L"Bus");
    g_a.buses[0].edge_count = 1;
    strcpy_s(g_a.buses[0].edges[0].key, APR_SESSION_KEY_CCH, "a");
    g_a.buses[0].action_count = 1;
    strcpy_s(g_a.buses[0].actions[0].id, 16, "wav");
    wcscpy_s(g_a.buses[0].actions[0].path, APR_DISC_PATH_CCH,
             L"D:\\rec\\a\\b\\out.wav");

    e = apr_session_write_utf8(&g_a, g_json, JSON_CAP, &len);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_session_load_utf8(g_json, len, &g_b, &g_lrep);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_WSTR_EQ(L"he said \"hi\"\tمرحبا", g_b.sources[0].name);
    ASSERT_WSTR_EQ(L"D:\\rec\\a\\b\\out.wav", g_b.buses[0].actions[0].path);
}

/* ===========================================================================
 * Files that are not sessions
 * ========================================================================= */

static AprSessionFault fault_of(const char *json)
{
    AprErr e = apr_session_load_utf8(json, 0, &g_b, &g_lrep);
    (void)e;
    return g_lrep.fault;
}

TEST(malformed_json_is_refused_as_malformed)
{
    /* JSMN IS A TOKENIZER, NOT A VALIDATOR, and these are the cases it really
     * does catch: a character that cannot begin a value. Structural nonsense
     * that still tokenizes -- `{ , , }`, `{"a": }` -- is caught by the schema
     * walk instead and surfaces as NOT_A_SESSION or BAD_TYPE. Testing the two
     * mechanisms against the inputs each actually owns is the difference
     * between a suite that documents the parser and one that flatters it. */
    ASSERT_EQ_INT(APR_SESSION_FAULT_NOT_JSON, (int)fault_of("not json at all"));
    ASSERT_EQ_INT(APR_SESSION_FAULT_NOT_JSON, (int)fault_of("[1,2,3]}"));
    ASSERT_EQ_INT(APR_SESSION_FAULT_NOT_JSON, (int)fault_of("{\"a\": @}"));
    ASSERT_EQ_INT(APR_SESSION_FAULT_NOT_JSON, (int)fault_of(
        "{\"apprecorder\":{\"version\":1}} and then some rubbish"));
}

TEST(structural_nonsense_that_still_tokenizes_is_caught_by_the_schema)
{
    /* An object whose members are not key/value pairs. jsmn accepts it; we
     * must not, because "a source with no kind" would otherwise be read as a
     * source with kind 0. */
    ASSERT_EQ_INT(APR_SESSION_FAULT_NOT_A_SESSION,
                  (int)fault_of("{ \"apprecorder\": { } }"));
    ASSERT_EQ_INT(APR_SESSION_FAULT_BAD_VALUE, (int)fault_of(
        "{\"apprecorder\":{\"version\":1},\"sources\":[{\"key\":\"a\"}]}"));
    ASSERT_TRUE(wcsstr(g_lrep.where, L"kind") != NULL);
}

TEST(a_truncated_file_says_truncated_rather_than_malformed)
{
    /* The difference matters to a person: one file is corrupt, the other was
     * cut off -- by a full disk, a killed writer, a bad copy. */
    ASSERT_EQ_INT(APR_SESSION_FAULT_TRUNCATED,
                  (int)fault_of("{ \"apprecorder\": { \"version\": 1 }, \"sources\": ["));
    ASSERT_EQ_INT(APR_SESSION_FAULT_TRUNCATED,
                  (int)fault_of("{ \"apprecorder\": { \"version\": 1"));
}

TEST(an_empty_file_is_not_a_session)
{
    ASSERT_EQ_INT(APR_SESSION_FAULT_TRUNCATED, (int)fault_of(""));
    ASSERT_EQ_INT(APR_SESSION_FAULT_TRUNCATED, (int)fault_of("   \n  "));
}

TEST(valid_json_that_is_not_ours_says_so)
{
    ASSERT_EQ_INT(APR_SESSION_FAULT_NOT_A_SESSION,
                  (int)fault_of("{ \"name\": \"package\", \"version\": \"1.0\" }"));
    ASSERT_EQ_INT(APR_SESSION_FAULT_NOT_A_SESSION, (int)fault_of("{}"));
    /* A top-level array is JSON, and is not a session. */
    ASSERT_EQ_INT(APR_SESSION_FAULT_NOT_A_SESSION, (int)fault_of("[]"));
}

TEST(a_field_of_the_wrong_type_is_a_broken_file_not_a_future_one)
{
    /* This is the line between "ignore what you do not understand" and
     * "refuse": an unknown KEY is forward compatibility, a known key of the
     * wrong TYPE is corruption, and defaulting it would change the recording
     * without saying so. */
    ASSERT_EQ_INT(APR_SESSION_FAULT_BAD_TYPE, (int)fault_of(
        "{\"apprecorder\":{\"version\":1},"
        " \"session\":{\"sampleRate\":\"forty-eight thousand\"}}"));
    ASSERT_TRUE(wcsstr(g_lrep.where, L"sampleRate") != NULL);

    ASSERT_EQ_INT(APR_SESSION_FAULT_BAD_TYPE, (int)fault_of(
        "{\"apprecorder\":{\"version\":1},"
        " \"sources\":[{\"key\":\"a\",\"kind\":\"fake\",\"toneHz\":{}}]}"));
    ASSERT_TRUE(wcsstr(g_lrep.where, L"toneHz") != NULL);

    ASSERT_EQ_INT(APR_SESSION_FAULT_BAD_TYPE, (int)fault_of(
        "{\"apprecorder\":{\"version\":1}, \"sources\": \"lots\"}"));

    ASSERT_EQ_INT(APR_SESSION_FAULT_BAD_TYPE, (int)fault_of(
        "{\"apprecorder\":{\"version\":1},"
        " \"sources\":[{\"key\":\"a\",\"kind\":\"fake\"}],"
        " \"buses\":[{\"name\":\"B\",\"sources\":[{\"key\":\"a\","
        "   \"gainDb\":\"loud\"}]}]}"));
    ASSERT_TRUE(wcsstr(g_lrep.where, L"gainDb") != NULL);
}

TEST(a_value_that_is_impossible_is_refused_with_where_it_was)
{
    ASSERT_EQ_INT(APR_SESSION_FAULT_BAD_VALUE, (int)fault_of(
        "{\"apprecorder\":{\"version\":1},\"session\":{\"channels\":99}}"));
    ASSERT_TRUE(wcsstr(g_lrep.where, L"channels") != NULL);

    ASSERT_EQ_INT(APR_SESSION_FAULT_BAD_VALUE, (int)fault_of(
        "{\"apprecorder\":{\"version\":1},\"session\":{\"sampleRate\":3}}"));

    /* An unknown kind is not a future extension we can ignore: we would not
     * know what to capture. */
    ASSERT_EQ_INT(APR_SESSION_FAULT_BAD_VALUE, (int)fault_of(
        "{\"apprecorder\":{\"version\":1},"
        " \"sources\":[{\"key\":\"a\",\"kind\":\"telepathy\"}]}"));
    ASSERT_TRUE(wcsstr(g_lrep.where, L"kind") != NULL);
}

TEST(unknown_keys_are_ignored_counted_and_named)
{
    AprErr e = apr_session_load_utf8(
        "{\"apprecorder\":{\"version\":1,\"minReader\":1},"
        " \"session\":{\"sampleRate\":48000,\"loudnessTarget\":-16},"
        " \"colourScheme\":\"midnight\","
        " \"sources\":[{\"key\":\"a\",\"kind\":\"fake\",\"toneHz\":440,"
        "               \"reverbTailMs\":250}],"
        " \"buses\":[{\"name\":\"B\",\"sources\":[{\"key\":\"a\"}],"
        "             \"outputs\":[{\"format\":\"wav\",\"path\":\"o.wav\","
        "                           \"loudnessNormalise\":true}]}]}",
        0, &g_b, &g_lrep);

    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(APR_SESSION_FAULT_NONE, (int)g_lrep.fault);
    /* loudnessTarget, colourScheme, reverbTailMs, loudnessNormalise */
    ASSERT_EQ_INT(4, (int)g_lrep.unknown_keys);
    ASSERT_TRUE(g_lrep.first_unknown_key[0] != L'\0');
    /* And the parts it DID understand still landed. */
    ASSERT_EQ_INT(48000, (int)g_b.sample_rate);
    ASSERT_EQ_INT(440, (int)g_b.sources[0].fake_hz);
    ASSERT_EQ_INT(1, (int)g_b.bus_count);
}

TEST(a_bus_naming_a_source_that_is_not_in_the_file_is_refused)
{
    ASSERT_EQ_INT(APR_SESSION_FAULT_DANGLING_REF, (int)fault_of(
        "{\"apprecorder\":{\"version\":1},"
        " \"sources\":[{\"key\":\"a\",\"kind\":\"fake\"}],"
        " \"buses\":[{\"name\":\"B\",\"sources\":[{\"key\":\"ghost\"}]}]}"));
    ASSERT_TRUE(wcsstr(g_lrep.where, L"ghost") != NULL);
}

TEST(more_sources_than_the_graph_can_hold_is_refused_rather_than_truncated)
{
    static char big[262144];
    int  i, n = 0;

    n += sprintf_s(big + n, sizeof big - (size_t)n,
                   "{\"apprecorder\":{\"version\":1},\"sources\":[");
    for (i = 0; i < APR_MAX_SOURCES + 4; i++) {
        n += sprintf_s(big + n, sizeof big - (size_t)n,
                       "%s{\"key\":\"k%d\",\"kind\":\"fake\"}",
                       i ? "," : "", i);
    }
    n += sprintf_s(big + n, sizeof big - (size_t)n, "]}");

    ASSERT_EQ_INT(APR_SESSION_FAULT_TOO_MANY, (int)fault_of(big));
}

/* ===========================================================================
 * Versioning
 * ========================================================================= */

TEST(a_file_from_a_future_writer_that_says_we_can_read_it_loads)
{
    AprErr e = apr_session_load_utf8(
        "{\"apprecorder\":{\"version\":97,\"minReader\":1},"
        " \"session\":{\"sampleRate\":48000},"
        " \"sources\":[{\"key\":\"a\",\"kind\":\"fake\",\"toneHz\":440}],"
        " \"buses\":[{\"name\":\"B\",\"sources\":[{\"key\":\"a\"}],"
        "             \"outputs\":[{\"format\":\"wav\",\"path\":\"o.wav\"}]}]}",
        0, &g_b, &g_lrep);

    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(97, g_lrep.version);
    ASSERT_EQ_INT(1, g_lrep.min_reader);
    /* Readable, but the caller is told so it can warn -- something in that
     * file was written for a build we are not. */
    ASSERT_EQ_INT(1, g_lrep.from_newer_writer);
    ASSERT_EQ_INT(1, (int)g_b.source_count);
}

TEST(a_file_that_needs_a_newer_reader_is_refused_and_says_which)
{
    ASSERT_EQ_INT(APR_SESSION_FAULT_TOO_NEW, (int)fault_of(
        "{\"apprecorder\":{\"version\":97,\"minReader\":97},"
        " \"sources\":[{\"key\":\"a\",\"kind\":\"fake\"}]}"));
    ASSERT_EQ_INT(97, g_lrep.min_reader);
    ASSERT_EQ_INT(97, g_lrep.version);
}

TEST(a_file_older_than_this_build_understands_is_refused)
{
    ASSERT_EQ_INT(APR_SESSION_FAULT_TOO_OLD, (int)fault_of(
        "{\"apprecorder\":{\"version\":0},"
        " \"sources\":[{\"key\":\"a\",\"kind\":\"fake\"}]}"));
}

TEST(a_missing_version_is_not_a_session)
{
    ASSERT_EQ_INT(APR_SESSION_FAULT_NOT_A_SESSION, (int)fault_of(
        "{\"apprecorder\":{\"writer\":\"0.1.0\"},"
        " \"sources\":[{\"key\":\"a\",\"kind\":\"fake\"}]}"));
}

TEST(what_this_build_writes_is_what_this_build_will_read)
{
    size_t len = 0;
    build_multibus(&g_a);
    ASSERT_FALSE(failed(apr_session_write_utf8(&g_a, g_json, JSON_CAP, &len)));
    ASSERT_TRUE(strstr(g_json, "\"version\": 1") != NULL);
    ASSERT_TRUE(strstr(g_json, "\"minReader\": 1") != NULL);
    ASSERT_EQ_INT(APR_SESSION_MIN_READER, APR_SESSION_FORMAT_VERSION);
}

/* ===========================================================================
 * Resolution -- process identity
 * ========================================================================= */

TEST(a_process_at_the_path_the_file_recorded_resolves_exactly)
{
    Machine m;
    const AprSessionResolution *r;
    AprSessionResolveOptions o = no_opts();

    mach_init(&m);
    mach_app(&m, 4321, L"Teams.exe", L"C:\\Apps\\Teams\\Teams.exe", 0);
    one_process_source(&g_a, L"Teams.exe", L"C:\\Apps\\Teams\\Teams.exe", NULL, 8412);

    ASSERT_FALSE(failed(apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep)));
    ASSERT_EQ_INT(1, (int)g_rrep.ok);
    ASSERT_EQ_INT(0, (int)g_rrep.failed);
    r = res_for(&g_rrep, 0);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ_INT(APR_SESSION_EXACT, (int)r->status);
    /* The pid in the file was 8412 and is now 4321. The pid is a hint, and
     * the path is the key. */
    ASSERT_EQ_INT(4321, (int)r->chosen_pid);
    ASSERT_EQ_INT(4321, (int)g_a.sources[0].resolved_pid);
    ASSERT_EQ_INT(1, g_a.sources[0].resolved);
}

TEST(the_same_process_still_running_is_recognised_as_the_same_process)
{
    Machine m;
    const AprSessionResolution *r;
    AprSessionResolveOptions o = no_opts();

    mach_init(&m);
    mach_app(&m, 8412, L"Teams.exe", L"C:\\Apps\\Teams\\Teams.exe", 0);
    one_process_source(&g_a, L"Teams.exe", L"C:\\Apps\\Teams\\Teams.exe", NULL, 8412);

    ASSERT_FALSE(failed(apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep)));
    r = res_for(&g_rrep, 0);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ_INT(APR_SESSION_SAME_PROCESS, (int)r->status);
    ASSERT_EQ_INT(8412, (int)r->chosen_pid);
}

TEST(an_application_that_is_not_running_is_reported_not_dropped)
{
    Machine m;
    const AprSessionResolution *r;
    AprSessionResolveOptions o = no_opts();
    AprErr e;

    mach_init(&m);
    mach_app(&m, 100, L"chrome.exe", L"C:\\Chrome\\chrome.exe", 0);
    one_process_source(&g_a, L"Teams.exe", L"C:\\Apps\\Teams\\Teams.exe", NULL, 8412);

    e = apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(1, (int)g_rrep.failed);
    r = res_for(&g_rrep, 0);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ_INT(APR_SESSION_NOT_RUNNING, (int)r->status);
    ASSERT_WSTR_EQ(L"C:\\Apps\\Teams\\Teams.exe", r->wanted);
    /* THE SOURCE IS STILL IN THE MODEL. A bus that quietly lost a source
     * would record something nobody asked for and look fine doing it. */
    ASSERT_EQ_INT(1, (int)g_a.source_count);
    ASSERT_EQ_INT(0, g_a.sources[0].resolved);
}

TEST(allow_missing_lets_the_rest_of_the_session_run_and_still_reports_the_loss)
{
    Machine m;
    AprSessionResolveOptions o = no_opts();
    AprErr e;

    o.allow_missing = 1;
    mach_init(&m);
    mach_app(&m, 100, L"chrome.exe", L"C:\\Chrome\\chrome.exe", 0);
    one_process_source(&g_a, L"Teams.exe", L"C:\\Apps\\Teams\\Teams.exe", NULL, 0);

    e = apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep);
    ASSERT_FALSE(apr_failed(&e));          /* the caller asked to continue */
    ASSERT_EQ_INT(1, (int)g_rrep.failed);  /* and is still told what it lost */
    ASSERT_EQ_INT(APR_SESSION_NOT_RUNNING, (int)res_for(&g_rrep, 0)->status);
}

TEST(an_executable_that_moved_resolves_and_says_where_it_went)
{
    Machine m;
    const AprSessionResolution *r;
    AprSessionResolveOptions o = no_opts();

    mach_init(&m);
    /* Updated, reinstalled, or moved to another drive. */
    mach_app(&m, 777, L"Teams.exe", L"D:\\Program Files\\Teams\\current\\Teams.exe", 0);
    one_process_source(&g_a, L"Teams.exe", L"C:\\Apps\\Teams\\Teams.exe", NULL, 0);

    ASSERT_FALSE(failed(apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep)));
    ASSERT_EQ_INT(0, (int)g_rrep.ok);
    ASSERT_EQ_INT(1, (int)g_rrep.substituted);
    r = res_for(&g_rrep, 0);
    ASSERT_EQ_INT(APR_SESSION_MOVED, (int)r->status);
    ASSERT_WSTR_EQ(L"C:\\Apps\\Teams\\Teams.exe", r->wanted);
    ASSERT_WSTR_EQ(L"D:\\Program Files\\Teams\\current\\Teams.exe", r->substituted);
    ASSERT_EQ_INT(777, (int)r->chosen_pid);
}

TEST(several_instances_are_told_apart_by_window_class)
{
    Machine m;
    const AprSessionResolution *r;
    AprSessionResolveOptions o = no_opts();

    mach_init(&m);
    mach_app(&m, 11, L"chrome.exe", L"C:\\Chrome\\chrome.exe", 0);
    mach_app(&m, 22, L"chrome.exe", L"C:\\Chrome\\chrome.exe", 0);
    mach_app(&m, 33, L"chrome.exe", L"C:\\Chrome\\chrome.exe", 0);
    mach_class_of(&m, 11, L"Chrome_WidgetWin_0");
    mach_class_of(&m, 22, L"Chrome_WidgetWin_1");
    mach_class_of(&m, 33, L"Chrome_WidgetWin_0");

    one_process_source(&g_a, L"chrome.exe", L"C:\\Chrome\\chrome.exe",
                       L"Chrome_WidgetWin_1", 0);

    ASSERT_FALSE(failed(apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep)));
    r = res_for(&g_rrep, 0);
    ASSERT_EQ_INT(APR_SESSION_BY_WINDOW_CLASS, (int)r->status);
    ASSERT_EQ_INT(22, (int)r->chosen_pid);
    /* It still says how many it had to choose between. */
    ASSERT_EQ_INT(3, (int)r->candidate_count);
}

TEST(several_instances_with_nothing_to_choose_by_are_refused_and_all_named)
{
    Machine m;
    const AprSessionResolution *r;
    AprSessionResolveOptions o = no_opts();
    AprErr e;

    mach_init(&m);
    mach_app(&m, 11, L"chrome.exe", L"C:\\Chrome\\chrome.exe", 0);
    mach_app(&m, 22, L"chrome.exe", L"C:\\Chrome\\chrome.exe", 0);
    one_process_source(&g_a, L"chrome.exe", L"C:\\Chrome\\chrome.exe", NULL, 0);

    e = apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep);
    ASSERT_TRUE(apr_failed(&e));
    r = res_for(&g_rrep, 0);
    ASSERT_EQ_INT(APR_SESSION_AMBIGUOUS, (int)r->status);
    /* Refusing is only defensible if the answer comes with it: every rival,
     * by pid, so the user can pin one -- and so a UI can prompt (design 9). */
    ASSERT_EQ_INT(2, (int)r->candidate_count);
    ASSERT_EQ_INT(2, (int)r->candidates_listed);
    ASSERT_EQ_INT(11, (int)r->candidates[0].pid);
    ASSERT_EQ_INT(22, (int)r->candidates[1].pid);
}

TEST(a_caller_that_has_decided_any_instance_will_do_can_say_so)
{
    Machine m;
    const AprSessionResolution *r;
    AprSessionResolveOptions o = no_opts();

    o.pick_when_ambiguous = 1;
    mach_init(&m);
    mach_app(&m, 22, L"chrome.exe", L"C:\\Chrome\\chrome.exe", 0);
    mach_app(&m, 11, L"chrome.exe", L"C:\\Chrome\\chrome.exe", 0);
    one_process_source(&g_a, L"chrome.exe", L"C:\\Chrome\\chrome.exe", NULL, 0);

    ASSERT_FALSE(failed(apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep)));
    r = res_for(&g_rrep, 0);
    ASSERT_EQ_INT(APR_SESSION_FIRST_OF_MANY, (int)r->status);
    /* Lowest pid, which is at least deterministic -- and reported as a
     * substitution, never as an exact match. */
    ASSERT_EQ_INT(11, (int)r->chosen_pid);
    ASSERT_EQ_INT(1, (int)g_rrep.substituted);
    ASSERT_EQ_INT(0, (int)g_rrep.ok);
}

TEST(the_stored_pid_settles_a_tie_that_nothing_else_can)
{
    Machine m;
    const AprSessionResolution *r;
    AprSessionResolveOptions o = no_opts();

    mach_init(&m);
    mach_app(&m, 11, L"chrome.exe", L"C:\\Chrome\\chrome.exe", 0);
    mach_app(&m, 22, L"chrome.exe", L"C:\\Chrome\\chrome.exe", 0);
    /* A reload during the same boot: the process the user picked is still
     * there, so there is nothing to be ambiguous about. */
    one_process_source(&g_a, L"chrome.exe", L"C:\\Chrome\\chrome.exe", NULL, 22);

    ASSERT_FALSE(failed(apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep)));
    r = res_for(&g_rrep, 0);
    ASSERT_EQ_INT(APR_SESSION_SAME_PROCESS, (int)r->status);
    ASSERT_EQ_INT(22, (int)r->chosen_pid);
    /* The pid actually ADOPTED, not just the one reported. Asserting only on
     * the report let a mutation that adopted the wrong candidate pass, because
     * chosen_pid was being copied from what we searched for rather than from
     * what we found. */
    ASSERT_EQ_INT(22, (int)g_a.sources[0].resolved_pid);
}

TEST(a_pid_belonging_to_something_else_now_is_not_a_match)
{
    Machine m;
    const AprSessionResolution *r;
    AprSessionResolveOptions o = no_opts();

    mach_init(&m);
    /* 8412 was Teams when the session was saved. After a reboot it is Discord.
     * Trusting the number alone records the wrong application. */
    mach_app(&m, 8412, L"Discord.exe", L"C:\\Discord\\Discord.exe", 0);
    mach_app(&m, 99,   L"Teams.exe",   L"C:\\Apps\\Teams\\Teams.exe", 0);
    one_process_source(&g_a, L"Teams.exe", L"C:\\Apps\\Teams\\Teams.exe", NULL, 8412);

    ASSERT_FALSE(failed(apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep)));
    r = res_for(&g_rrep, 0);
    ASSERT_EQ_INT(APR_SESSION_EXACT, (int)r->status);
    ASSERT_EQ_INT(99, (int)r->chosen_pid);
}

TEST(a_muted_source_resolves_and_carries_the_flag_that_explains_the_silence)
{
    Machine m;
    AprSessionResolveOptions o = no_opts();

    mach_init(&m);
    mach_app(&m, 4321, L"Teams.exe", L"C:\\Apps\\Teams\\Teams.exe", 1);
    one_process_source(&g_a, L"Teams.exe", L"C:\\Apps\\Teams\\Teams.exe", NULL, 0);

    ASSERT_FALSE(failed(apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep)));
    /* Loopback is post-session-volume: this WILL record digital silence, and
     * that is a warning rather than a failure. */
    ASSERT_EQ_INT(1, g_a.sources[0].muted_now);
    ASSERT_EQ_INT(1, (int)g_rrep.ok);
}

/* ===========================================================================
 * Resolution -- device identity
 * ========================================================================= */

static void one_device_source(AprSession *s, const wchar_t *id,
                              const wchar_t *name)
{
    apr_session_init(s);
    s->source_count = 1;
    strcpy_s(s->sources[0].key, APR_SESSION_KEY_CCH, "d");
    s->sources[0].kind = APR_SESSION_SRC_DEVICE;
    wcscpy_s(s->sources[0].name, APR_NAME_CCH, name);
    wcscpy_s(s->sources[0].endpoint_id, APR_DISC_ENDPOINT_CCH, id);
    wcscpy_s(s->sources[0].endpoint_name, APR_DISC_NAME_CCH, name);
    s->bus_count = 1;
    wcscpy_s(s->buses[0].name, APR_NAME_CCH, L"Bus");
    s->buses[0].edge_count = 1;
    strcpy_s(s->buses[0].edges[0].key, APR_SESSION_KEY_CCH, "d");
    s->buses[0].action_count = 1;
    strcpy_s(s->buses[0].actions[0].id, 16, "wav");
    wcscpy_s(s->buses[0].actions[0].path, APR_DISC_PATH_CCH, L"out.wav");
}

TEST(a_capture_endpoint_that_is_present_resolves_by_its_id)
{
    Machine m;
    AprSessionResolveOptions o = no_opts();

    mach_init(&m);
    mach_endpoint(&m, L"{0.0.1.00000000}.{aaa}", L"Chat Mic (TC-Helicon GoXLR)");
    one_device_source(&g_a, L"{0.0.1.00000000}.{aaa}", L"Chat Mic (TC-Helicon GoXLR)");

    ASSERT_FALSE(failed(apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep)));
    ASSERT_EQ_INT(APR_SESSION_DEVICE_EXACT, (int)res_for(&g_rrep, 0)->status);
    ASSERT_WSTR_EQ(L"{0.0.1.00000000}.{aaa}", g_a.sources[0].resolved_endpoint_id);
}

TEST(a_device_that_is_not_plugged_in_is_named_the_way_a_person_knows_it)
{
    Machine m;
    const AprSessionResolution *r;
    AprSessionResolveOptions o = no_opts();
    AprErr e;

    mach_init(&m);
    mach_endpoint(&m, L"{0.0.1.00000000}.{zzz}", L"Microphone (Realtek)");
    one_device_source(&g_a, L"{0.0.1.00000000}.{aaa}", L"Chat Mic (TC-Helicon GoXLR)");

    e = apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep);
    ASSERT_TRUE(apr_failed(&e));
    r = res_for(&g_rrep, 0);
    ASSERT_EQ_INT(APR_SESSION_DEVICE_ABSENT, (int)r->status);
    /* THIS is why both halves are stored. "{0.0.1.00000000}.{aaa} was not
     * found" is not a sentence anyone can act on. */
    ASSERT_WSTR_EQ(L"Chat Mic (TC-Helicon GoXLR)", r->wanted);
}

TEST(a_device_whose_id_changed_is_found_by_its_friendly_name)
{
    Machine m;
    const AprSessionResolution *r;
    AprSessionResolveOptions o = no_opts();

    mach_init(&m);
    /* Same GoXLR, different USB port: Windows mints a new endpoint id. */
    mach_endpoint(&m, L"{0.0.1.00000000}.{bbb}", L"Chat Mic (TC-Helicon GoXLR)");
    one_device_source(&g_a, L"{0.0.1.00000000}.{aaa}", L"Chat Mic (TC-Helicon GoXLR)");

    ASSERT_FALSE(failed(apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep)));
    r = res_for(&g_rrep, 0);
    ASSERT_EQ_INT(APR_SESSION_DEVICE_BY_NAME, (int)r->status);
    ASSERT_EQ_INT(1, (int)g_rrep.substituted);
    ASSERT_WSTR_EQ(L"{0.0.1.00000000}.{bbb}", g_a.sources[0].resolved_endpoint_id);
}

TEST(two_devices_with_the_same_friendly_name_are_not_guessed_between)
{
    Machine m;
    AprSessionResolveOptions o = no_opts();

    mach_init(&m);
    mach_endpoint(&m, L"{0.0.1.00000000}.{bbb}", L"Chat Mic (TC-Helicon GoXLR)");
    mach_endpoint(&m, L"{0.0.1.00000000}.{ccc}", L"Chat Mic (TC-Helicon GoXLR)");
    one_device_source(&g_a, L"{0.0.1.00000000}.{aaa}", L"Chat Mic (TC-Helicon GoXLR)");

    ASSERT_TRUE(failed(apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep)));
    ASSERT_EQ_INT(APR_SESSION_AMBIGUOUS, (int)res_for(&g_rrep, 0)->status);
}

TEST(a_synthetic_source_needs_no_machine_at_all)
{
    Machine m;
    AprSessionResolveOptions o = no_opts();

    mach_init(&m);   /* nothing running, nothing plugged in */
    apr_session_init(&g_a);
    g_a.source_count = 1;
    strcpy_s(g_a.sources[0].key, APR_SESSION_KEY_CCH, "t");
    g_a.sources[0].kind = APR_SESSION_SRC_FAKE;
    g_a.sources[0].fake_hz = 440;
    wcscpy_s(g_a.sources[0].name, APR_NAME_CCH, L"tone");

    ASSERT_FALSE(failed(apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep)));
    ASSERT_EQ_INT(APR_SESSION_SYNTHETIC, (int)res_for(&g_rrep, 0)->status);
    ASSERT_EQ_INT(1, g_a.sources[0].resolved);
}

/* ===========================================================================
 * EXCLUDE mode -- design 4.1.1
 * ========================================================================= */

static void one_exclude_source(AprSession *s, const wchar_t *exe,
                               const wchar_t *path, uint32_t pid)
{
    one_process_source(s, exe, path, NULL, pid);
    s->sources[0].kind = APR_SESSION_SRC_SYSTEM_MINUS_TREE;
}

TEST(a_session_file_alone_never_turns_on_system_wide_capture)
{
    Machine m;
    const AprSessionResolution *r;
    AprSessionResolveOptions o = no_opts();
    AprErr e;

    mach_init(&m);
    mach_app(&m, 500, L"Discord.exe", L"C:\\Discord\\Discord.exe", 0);
    one_exclude_source(&g_a, L"Discord.exe", L"C:\\Discord\\Discord.exe", 0);

    /* The target resolves perfectly. That is not the question. */
    e = apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep);
    ASSERT_TRUE(apr_failed(&e));
    r = res_for(&g_rrep, 0);
    ASSERT_EQ_INT(APR_SESSION_NEEDS_CONSENT, (int)r->status);
    ASSERT_EQ_INT(1, g_rrep.system_capture_sources);
    ASSERT_EQ_INT(1, g_rrep.needs_system_capture_consent);
    ASSERT_EQ_INT(0, g_a.sources[0].resolved);
}

TEST(explicit_consent_is_what_enables_system_wide_capture)
{
    Machine m;
    AprSessionResolveOptions o = no_opts();

    o.allow_system_capture = 1;
    mach_init(&m);
    mach_app(&m, 500, L"Discord.exe", L"C:\\Discord\\Discord.exe", 0);
    one_exclude_source(&g_a, L"Discord.exe", L"C:\\Discord\\Discord.exe", 0);

    ASSERT_FALSE(failed(apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep)));
    ASSERT_EQ_INT(500, (int)g_a.sources[0].resolved_pid);
    ASSERT_EQ_INT(1, g_a.sources[0].resolved);
    /* The report still says this session captures the whole system, so a
     * front end can keep saying so every single time. */
    ASSERT_EQ_INT(1, g_rrep.system_capture_sources);
    ASSERT_EQ_INT(0, g_rrep.needs_system_capture_consent);
}

TEST(allow_missing_never_drops_the_target_of_an_exclusion)
{
    Machine m;
    AprSessionResolveOptions o = no_opts();
    AprErr e;

    o.allow_system_capture = 1;
    o.allow_missing        = 1;
    mach_init(&m);
    mach_app(&m, 1, L"chrome.exe", L"C:\\Chrome\\chrome.exe", 0);
    one_exclude_source(&g_a, L"Discord.exe", L"C:\\Discord\\Discord.exe", 0);

    /* Dropping an ordinary source records LESS than was asked for. Dropping
     * the target of an exclusion records MORE -- everything, with nothing
     * held back. So allow_missing must not reach this case. */
    e = apr_session_resolve_against(&g_a, &o, &m.view, &g_rrep);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_SESSION_NOT_RUNNING, (int)res_for(&g_rrep, 0)->status);
    ASSERT_EQ_INT(1, (int)g_rrep.failed);
}

TEST(exclude_mode_survives_a_round_trip_so_the_gate_sees_it)
{
    size_t len = 0;
    Machine m;
    AprSessionResolveOptions o = no_opts();

    one_exclude_source(&g_a, L"Discord.exe", L"C:\\Discord\\Discord.exe", 500);
    ASSERT_FALSE(failed(apr_session_write_utf8(&g_a, g_json, JSON_CAP, &len)));
    ASSERT_TRUE(strstr(g_json, "systemMinusTree") != NULL);

    ASSERT_FALSE(failed(apr_session_load_utf8(g_json, len, &g_b, &g_lrep)));
    ASSERT_EQ_INT(APR_SESSION_SRC_SYSTEM_MINUS_TREE, (int)g_b.sources[0].kind);

    mach_init(&m);
    mach_app(&m, 500, L"Discord.exe", L"C:\\Discord\\Discord.exe", 0);
    ASSERT_TRUE(failed(apr_session_resolve_against(&g_b, &o, &m.view, &g_rrep)));
    ASSERT_EQ_INT(1, g_rrep.needs_system_capture_consent);
}

/* ===========================================================================
 * The command line
 * ========================================================================= */

#define CAP_CCH 32768

typedef struct Cap {
    wchar_t out[CAP_CCH];
    wchar_t err[CAP_CCH];
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
}

static AprCliIo g_io;
static const AprCliIo *io_of(Cap *c)
{
    memset(c, 0, sizeof *c);
    g_io.write = cap_write;
    g_io.user  = c;
    return &g_io;
}

static int said(const Cap *c, const wchar_t *needle)
{
    return wcsstr(c->out, needle) != NULL || wcsstr(c->err, needle) != NULL;
}

#define ARGV(...) (const wchar_t *const[]){ L"apprecorder", __VA_ARGS__ }
#define ARGC(...) (int)(sizeof((const wchar_t *const[]){ L"apprecorder", __VA_ARGS__ }) \
                        / sizeof(const wchar_t *))
#define RUN(cap, ...) apr_cli_run(ARGC(__VA_ARGS__), ARGV(__VA_ARGS__), io_of(cap))

TEST(save_session_writes_a_file_that_loads_back_as_the_same_graph)
{
    Cap c;
    wchar_t sfile[MAX_PATH];

    tmp_path(sfile, MAX_PATH, L"save", L"json");
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"save-session", L"--session", sfile,
        L"--bus", L"Mix",   L"--fake", L"440,0,0.25", L"--gain", L"-6",
                            L"--out", L"mix.wav",
        L"--bus", L"Voice", L"--fake", L"220",  L"--out", L"voice.wav",
        L"--quiet"));
    ASSERT_TRUE(file_exists(sfile));

    ASSERT_FALSE(failed(apr_session_load(sfile, &g_b, &g_lrep)));
    ASSERT_EQ_INT(2, (int)g_b.bus_count);
    ASSERT_WSTR_EQ(L"Mix", g_b.buses[0].name);
    ASSERT_WSTR_EQ(L"Voice", g_b.buses[1].name);
    ASSERT_EQ_INT(2, (int)g_b.source_count);
    ASSERT_EQ_INT(-60, g_b.buses[0].edges[0].gain_db_tenths);
    ASSERT_EQ_INT(440, (int)g_b.sources[0].fake_hz);
    ASSERT_EQ_INT(220, (int)g_b.sources[1].fake_hz);
    ASSERT_STR_EQ("wav", g_b.buses[0].actions[0].id);

    /* Nothing was recorded: save-session describes a recording, it does not
     * make one. */
    ASSERT_FALSE(file_exists(L"mix.wav"));
    DeleteFileW(sfile);
}

TEST(a_saved_session_replayed_produces_the_same_plan)
{
    Cap c;
    wchar_t sfile[MAX_PATH];

    tmp_path(sfile, MAX_PATH, L"replay", L"json");
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"save-session", L"--session", sfile,
        L"--bus", L"Mix", L"--fake", L"440", L"--gain", L"-6",
        L"--out", L"mix.wav", L"--quiet"));

    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--session", sfile, L"--dry-run"));
    ASSERT_TRUE(said(&c, L"Mix"));
    ASSERT_TRUE(said(&c, L"mix.wav"));
    ASSERT_TRUE(said(&c, L"-6.0"));
    DeleteFileW(sfile);
}

TEST(a_session_file_that_is_not_there_is_the_absent_thing_exit_code)
{
    Cap c;
    wchar_t sfile[MAX_PATH];
    tmp_path(sfile, MAX_PATH, L"ghost", L"json");
    ASSERT_EQ_INT(APR_CLI_NOT_FOUND, RUN(&c, L"--session", sfile, L"--dry-run"));
}

TEST(a_session_file_that_cannot_be_used_is_the_configuration_exit_code)
{
    Cap c;
    wchar_t sfile[MAX_PATH];

    tmp_path(sfile, MAX_PATH, L"broken", L"json");
    ASSERT_FALSE(failed(write_text_file(sfile, "{ this is not json ")));
    /* Read, but not a recording that can be made -- which is exactly what
     * exit code 2 already means. */
    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--session", sfile, L"--dry-run"));
    DeleteFileW(sfile);

    tmp_path(sfile, MAX_PATH, L"newer", L"json");
    ASSERT_FALSE(failed(write_text_file(sfile,
        "{\"apprecorder\":{\"version\":99,\"minReader\":99}}")));
    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--session", sfile, L"--dry-run"));
    DeleteFileW(sfile);
}

TEST(a_session_that_wants_system_wide_capture_stops_without_the_flag)
{
    Cap c;
    wchar_t sfile[MAX_PATH];
    char    json[1024];

    tmp_path(sfile, MAX_PATH, L"excl", L"json");
    sprintf_s(json, sizeof json,
        "{\"apprecorder\":{\"version\":1,\"minReader\":1},"
        " \"sources\":[{\"key\":\"x\",\"kind\":\"systemMinusTree\","
        "               \"name\":\"Discord\",\"exe\":\"Discord.exe\","
        "               \"path\":\"C:\\\\Discord\\\\Discord.exe\","
        "               \"pid\":%lu}],"
        " \"buses\":[{\"name\":\"All\",\"sources\":[{\"key\":\"x\"}],"
        "             \"outputs\":[{\"format\":\"wav\",\"path\":\"all.wav\"}]}]}",
        (unsigned long)GetCurrentProcessId());
    ASSERT_FALSE(failed(write_text_file(sfile, json)));

    /* No flag: refused, and it says why rather than recording the machine. */
    ASSERT_EQ_INT(APR_CLI_CONFIG, RUN(&c, L"--session", sfile, L"--dry-run"));
    ASSERT_TRUE(said(&c, L"--allow-system-capture"));
    ASSERT_FALSE(file_exists(L"all.wav"));
    DeleteFileW(sfile);
}

TEST(the_source_options_and_a_session_file_are_not_combined)
{
    Cap c;
    wchar_t sfile[MAX_PATH];
    tmp_path(sfile, MAX_PATH, L"conflict", L"json");
    /* Ambiguity about where the graph came from would be a very good way to
     * record the wrong thing. */
    ASSERT_EQ_INT(APR_CLI_USAGE, RUN(&c, L"--session", sfile,
                                     L"--fake", L"440", L"--out", L"x.wav"));
    ASSERT_TRUE(said(&c, L"--session"));
}

TEST(a_saved_session_records_playable_files_when_it_is_replayed)
{
    Cap c;
    wchar_t sfile[MAX_PATH], wav[MAX_PATH];

    tmp_path(sfile, MAX_PATH, L"e2e", L"json");
    tmp_path(wav,   MAX_PATH, L"e2e", L"wav");

    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"save-session", L"--session", sfile,
        L"--bus", L"Mix", L"--fake", L"440,0,0.25", L"--out", wav, L"--quiet"));

    /* Only --fake sources, so nothing renders and nothing is captured from a
     * device: AGENTS.md rule 1 holds with no special pleading. */
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"--session", sfile,
                                  L"--duration", L"0.25", L"--quiet"));
    ASSERT_TRUE(file_exists(wav));
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        ASSERT_TRUE(GetFileAttributesExW(wav, GetFileExInfoStandard, &fa) != 0);
        ASSERT_GT_INT(1000, (int)fa.nFileSizeLow);
    }
    DeleteFileW(wav);
    DeleteFileW(sfile);
}

TEST(help_documents_the_session_options)
{
    Cap c;
    ASSERT_EQ_INT(APR_CLI_OK, RUN(&c, L"help"));
    ASSERT_TRUE(said(&c, L"--session"));
    ASSERT_TRUE(said(&c, L"save-session"));
    ASSERT_TRUE(said(&c, L"--allow-system-capture"));
    ASSERT_TRUE(said(&c, L"--allow-missing"));
}
