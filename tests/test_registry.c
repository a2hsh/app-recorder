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
