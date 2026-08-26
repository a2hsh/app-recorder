/*
 * test_registry.c -- the static action table.
 *
 * The table is a build-time artifact, so most of what can go wrong here is
 * structural: a vtable with a hole in it, two actions claiming the same id, a
 * lookup that matches on the wrong field. All of those are silent at runtime
 * and fatal in a session file.
 */
#include "test_runner.h"
#include "action.h"

#include <string.h>

TEST(the_table_is_never_empty)
{
    /* An empty table would make apr_action_at meaningless and, in C, make the
     * array declaration itself ill-formed. The built-in discard sink is what
     * guarantees it. */
    ASSERT_GT_INT(0, (long long)apr_action_count());
    ASSERT_NOT_NULL(apr_action_at(0));
}

TEST(every_entry_is_complete)
{
    /* A vtable with a NULL on_audio does not fail until a recording is
     * running, which is the worst possible moment to find out. */
    size_t i, n = apr_action_count();

    for (i = 0; i < n; i++) {
        const AprActionVTable *vt = apr_action_at(i);
        ASSERT_NOT_NULL(vt);
        ASSERT_NOT_NULL(vt->id);
        /* The display name is a catalog id, not a literal (AGENTS.md rule 6).
         * An unset one is 0, and a set one that has no .rc entry resolves to a
         * visible placeholder rather than an empty string -- so a non-empty
         * resolution is the real check, not merely a non-zero id. */
        ASSERT_NE_INT(0, vt->display_name_id);
        ASSERT_GT_INT(0, (long long)wcslen(apr_str(vt->display_name_id)));
        ASSERT_NOT_NULL(vt->extension);
        ASSERT_NOT_NULL(vt->create);
        ASSERT_NOT_NULL(vt->on_audio);
        ASSERT_NOT_NULL(vt->finalize);
        ASSERT_NOT_NULL(vt->destroy);
        ASSERT_GT_INT(0, (long long)strlen(vt->id));
    }
}

TEST(ids_are_unique_because_session_files_hold_them)
{
    size_t i, k, n = apr_action_count();

    for (i = 0; i < n; i++) {
        for (k = i + 1; k < n; k++) {
            if (strcmp(apr_action_at(i)->id, apr_action_at(k)->id) == 0) {
                printf("      duplicate action id \"%s\" at %zu and %zu\n",
                       apr_action_at(i)->id, i, k);
                FAIL("two actions share an id");
            }
        }
    }
    ASSERT_GT_INT(0, (long long)n);
}

TEST(find_matches_on_id_and_nothing_else)
{
    const AprActionVTable *vt = apr_action_find("none");

    ASSERT_NOT_NULL(vt);
    ASSERT_STR_EQ("none", vt->id);

    ASSERT_NULL(apr_action_find("nope"));
    ASSERT_NULL(apr_action_find(""));
    ASSERT_NULL(apr_action_find(NULL));
    /* Not matched on prose: the display name is a catalog id and its text
     * changes with the interface language. */
    ASSERT_NULL(apr_action_find("No output (discard)"));
}

TEST(index_out_of_range_is_null_not_a_fault)
{
    ASSERT_NULL(apr_action_at(apr_action_count()));
    ASSERT_NULL(apr_action_at((size_t)-1));
}

TEST(everything_registered_is_reachable_by_its_own_id)
{
    size_t i, n = apr_action_count();

    for (i = 0; i < n; i++) {
        const AprActionVTable *vt = apr_action_at(i);
        ASSERT_TRUE(apr_action_find(vt->id) == vt);
    }
}

TEST(the_discard_sink_accepts_audio_and_finalizes)
{
    const AprActionVTable *vt = apr_action_find("none");
    AprActionConfig cfg;
    void  *st = NULL;
    float  pcm[64];
    AprErr e;

    memset(&cfg, 0, sizeof cfg);
    memset(pcm, 0, sizeof pcm);
    cfg.sample_rate = 48000;
    cfg.channels    = 2;

    e = vt->create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_NOT_NULL(st);

    e = vt->on_audio(st, pcm, 32, 0);
    ASSERT_FALSE(apr_failed(&e));

    e = vt->finalize(st);
    ASSERT_FALSE(apr_failed(&e));

    /* Audio after finalize is a state error, not silently accepted. */
    e = vt->on_audio(st, pcm, 32, 0);
    ASSERT_TRUE(apr_failed(&e));

    vt->destroy(st);
    vt->destroy(NULL);
}

TEST(the_discard_sink_rejects_a_format_it_was_not_given)
{
    const AprActionVTable *vt = apr_action_find("none");
    AprActionConfig cfg;
    void  *st = (void *)1;
    AprErr e;

    memset(&cfg, 0, sizeof cfg);
    e = vt->create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(st);

    e = vt->create(NULL, &st);
    ASSERT_TRUE(apr_failed(&e));

    e = vt->create(&cfg, NULL);
    ASSERT_TRUE(apr_failed(&e));
}

/* ===========================================================================
 * Asking an action what it will accept, before anything is recorded
 * ========================================================================= */

/* m16: an id is a stable ASCII token and its CASE was never part of the
 * contract. `--out x.WAV` always resolved -- the extension pass is
 * case-insensitive -- while `--format WAV` was refused as a format this build
 * cannot write. */
TEST(an_id_is_matched_whatever_case_it_is_written_in)
{
    ASSERT_NOT_NULL(apr_action_find("wav"));
    ASSERT_TRUE(apr_action_find("wav") == apr_action_find("WAV"));
    ASSERT_TRUE(apr_action_find("wav") == apr_action_find("Wav"));
    ASSERT_TRUE(apr_action_find("none") == apr_action_find("NONE"));
    /* And it is still a MATCH, not a prefix or a fuzzy one. */
    ASSERT_NULL(apr_action_find("wa"));
    ASSERT_NULL(apr_action_find("wavv"));
    /* The canonical spelling is the vtable's own, which is what a caller
     * stores in a session file. */
    ASSERT_STR_EQ("wav", apr_action_find("WAV")->id);
}

/* M1: the registry can be ASKED. Without this the only thing that knew
 * libmp3lame refuses 400 kbps was create(), which runs after the recording
 * has already started. */
TEST(an_action_can_be_asked_whether_it_will_take_a_configuration)
{
    AprActionConfig cfg;
    AprErr e;

    memset(&cfg, 0, sizeof cfg);
    cfg.sample_rate = 48000;
    cfg.channels    = 2;

    /* Nothing unusual asked for: every format takes it. */
    e = apr_action_check_config(apr_action_find("wav"), &cfg);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_action_check_config(apr_action_find("mp3"), &cfg);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_action_check_config(apr_action_find("ogg"), &cfg);
    ASSERT_FALSE(apr_failed(&e));

    /* THE ONE THAT COST A TAKE: 400 kbps is inside the command line's own
     * 0..1152 range and outside MP3's 8..320. */
    cfg.bitrate_kbps = 400;
    e = apr_action_check_config(apr_action_find("mp3"), &cfg);
    ASSERT_TRUE(apr_failed(&e));
    /* Opus goes to 510, so the same number is fine there -- the limits belong
     * to the format and not to a rule someone wrote once. */
    e = apr_action_check_config(apr_action_find("ogg"), &cfg);
    ASSERT_FALSE(apr_failed(&e));
    cfg.bitrate_kbps = 0;

    /* Neither lossy format carries more than two channels; WAV carries eight. */
    cfg.channels = 8;
    e = apr_action_check_config(apr_action_find("mp3"), &cfg);
    ASSERT_TRUE(apr_failed(&e));
    e = apr_action_check_config(apr_action_find("ogg"), &cfg);
    ASSERT_TRUE(apr_failed(&e));
    e = apr_action_check_config(apr_action_find("wav"), &cfg);
    ASSERT_FALSE(apr_failed(&e));
    cfg.channels = 2;

    /* Opus resamples, but not from anywhere: a rate it cannot reach is a
     * refusal at plan time rather than at apr_bus_start. */
    cfg.sample_rate = 4000;
    e = apr_action_check_config(apr_action_find("ogg"), &cfg);
    ASSERT_TRUE(apr_failed(&e));
}

/* An action with no hook accepts whatever create() would, and the registry is
 * the single place that decides what a missing hook means -- so no caller has
 * to test the pointer, and none can forget to. */
TEST(an_action_with_no_check_hook_accepts_what_create_would)
{
    AprActionConfig cfg;
    AprErr e;

    memset(&cfg, 0, sizeof cfg);
    cfg.sample_rate = 48000;
    cfg.channels    = 2;

    ASSERT_NULL(apr_action_find("none")->check_config);
    e = apr_action_check_config(apr_action_find("none"), &cfg);
    ASSERT_FALSE(apr_failed(&e));

    /* And being asked about nothing is an error rather than a crash. */
    e = apr_action_check_config(NULL, &cfg);
    ASSERT_TRUE(apr_failed(&e));
    e = apr_action_check_config(apr_action_find("wav"), NULL);
    ASSERT_TRUE(apr_failed(&e));
}

/* The check and create() must never be able to disagree: they are the same
 * function, and this is what says so if somebody ever copies the numbers. */
TEST(what_the_check_refuses_create_refuses_too)
{
    AprActionConfig cfg;
    void   *st = (void *)1;
    AprErr  e;

    memset(&cfg, 0, sizeof cfg);
    cfg.out_path     = L"apr_registry_should_never_exist.mp3";
    cfg.sample_rate  = 48000;
    cfg.channels     = 2;
    cfg.bitrate_kbps = 400;

    e = apr_action_check_config(apr_action_find("mp3"), &cfg);
    ASSERT_TRUE(apr_failed(&e));

    e = apr_action_find("mp3")->create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(st);
}
