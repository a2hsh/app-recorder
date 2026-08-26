/*
 * test_graph.c -- the model: nodes, edges, and the projections both UI views
 * read.
 *
 * The property under test throughout is that this is a GRAPH. A tree would
 * pass most of these; the ones it would fail are the ones about a source
 * feeding several buses at once, and about removing a node without leaving a
 * bus holding a reader into freed memory.
 *
 * No timeline here -- test_sync.c owns that. These are shape and lifetime.
 */
#include "test_runner.h"
#include "graph.h"

#include <string.h>

#define RATE 48000u

static AprCaptureConfig fake(void)
{
    AprCaptureConfig c;
    memset(&c, 0, sizeof c);
    c.kind         = APR_SRC_FAKE;
    c.sample_rate  = 1;      /* deliberately wrong: the graph must overwrite it */
    c.channels     = 1;
    return c;
}

static AprGraph *make(uint16_t channels)
{
    AprGraph *g = NULL;
    apr_graph_create(RATE, channels, &g);
    return g;
}

static AprSourceId src(AprGraph *g, const wchar_t *name)
{
    AprCaptureConfig c = fake();
    AprSourceId      id = 0;
    apr_graph_add_source(g, name, &c, &id);
    return id;
}

static AprBusId bus(AprGraph *g, const wchar_t *name)
{
    AprBusId id = 0;
    apr_graph_add_bus(g, name, &id);
    return id;
}

/* ---- creation ------------------------------------------------------------ */

TEST(create_rejects_a_format_nothing_could_honour)
{
    AprGraph *g = (AprGraph *)1;
    AprErr    e;

    e = apr_graph_create(0, 2, &g);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(g);

    e = apr_graph_create(RATE, 0, &g);
    ASSERT_TRUE(apr_failed(&e));

    e = apr_graph_create(RATE, 99, &g);
    ASSERT_TRUE(apr_failed(&e));

    e = apr_graph_create(RATE, 2, NULL);
    ASSERT_TRUE(apr_failed(&e));
}

TEST(the_session_format_is_imposed_on_every_source)
{
    /* A process-loopback client returns E_NOTIMPL from GetMixFormat, so format
     * is decided once, here, and handed down (design 4.1). A caller asking for
     * something else must not get it. */
    AprGraph   *g = make(2);
    AprSourceId a = src(g, L"Teams");
    AprSource  *s = apr_graph_source(g, a);

    ASSERT_NOT_NULL(s);
    ASSERT_EQ_INT((int)RATE, (int)apr_source_rate(s));
    ASSERT_EQ_INT(2, apr_source_channels(s));
    apr_graph_destroy(g);
}

TEST(ids_start_at_one_and_are_never_reused)
{
    /* 0 means "no node", and a UI element or session file may hold an id long
     * after the node is gone; handing that id to a different node later is how
     * a screen reader ends up announcing the wrong thing. */
    AprGraph   *g = make(1);
    AprSourceId a = src(g, L"A");
    AprSourceId b = src(g, L"B");
    AprSourceId c;

    ASSERT_EQ_INT(1, (int)a);
    ASSERT_EQ_INT(2, (int)b);

    apr_graph_remove_source(g, a);
    c = src(g, L"C");
    ASSERT_NE_INT((int)a, (int)c);
    ASSERT_EQ_INT(3, (int)c);
    apr_graph_destroy(g);
}

TEST(unknown_ids_resolve_to_null_rather_than_a_neighbour)
{
    AprGraph *g = make(1);

    src(g, L"A");
    ASSERT_NULL(apr_graph_source(g, 0));
    ASSERT_NULL(apr_graph_source(g, 999));
    ASSERT_NULL(apr_graph_bus(g, 1));
    ASSERT_NULL(apr_graph_source_at(g, 5));
    ASSERT_NULL(apr_graph_bus_at(g, 0));
    apr_graph_destroy(g);
}

/* ---- edges --------------------------------------------------------------- */

TEST(one_source_feeds_several_buses_which_is_the_whole_point)
{
    AprGraph   *g = make(1);
    AprSourceId teams = src(g, L"Teams");
    AprBusId    full  = bus(g, L"Full mix");
    AprBusId    only  = bus(g, L"Teams only");
    AprBusId    third = bus(g, L"Backup");
    AprErr      e;

    e = apr_graph_connect(g, teams, full, 1.0f);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_graph_connect(g, teams, only, 0.5f);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_graph_connect(g, teams, third, 0.25f);
    ASSERT_FALSE(apr_failed(&e));

    ASSERT_EQ_INT(3, apr_source_refcount(apr_graph_source(g, teams)));
    ASSERT_EQ_INT(3, (int)apr_graph_edge_count(g));

    /* Each edge has its own gain: the same source at different levels on
     * different buses is a normal thing to want. */
    ASSERT_NEAR(1.0,  apr_bus_gain(apr_graph_bus(g, full),  teams), 0.0);
    ASSERT_NEAR(0.5,  apr_bus_gain(apr_graph_bus(g, only),  teams), 0.0);
    ASSERT_NEAR(0.25, apr_bus_gain(apr_graph_bus(g, third), teams), 0.0);
    apr_graph_destroy(g);
}

TEST(connecting_the_same_pair_twice_is_refused)
{
    AprGraph   *g = make(1);
    AprSourceId a = src(g, L"A");
    AprBusId    b = bus(g, L"B");
    AprErr      e;

    apr_graph_connect(g, a, b, 1.0f);
    e = apr_graph_connect(g, a, b, 1.0f);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(1, apr_source_refcount(apr_graph_source(g, a)));
    ASSERT_EQ_INT(1, (int)apr_graph_edge_count(g));
    apr_graph_destroy(g);
}

TEST(connecting_something_that_does_not_exist_is_not_found)
{
    AprGraph   *g = make(1);
    AprSourceId a = src(g, L"A");
    AprBusId    b = bus(g, L"B");
    AprErr      e;

    e = apr_graph_connect(g, 999, b, 1.0f);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_NOT_FOUND, (int)e.kind);

    e = apr_graph_connect(g, a, 999, 1.0f);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_NOT_FOUND, (int)e.kind);

    e = apr_graph_disconnect(g, a, b);
    ASSERT_TRUE(apr_failed(&e));      /* never connected */
    apr_graph_destroy(g);
}

TEST(disconnecting_one_edge_leaves_the_others_alone)
{
    AprGraph   *g = make(1);
    AprSourceId teams = src(g, L"Teams");
    AprBusId    full  = bus(g, L"Full");
    AprBusId    only  = bus(g, L"Only");

    apr_graph_connect(g, teams, full, 1.0f);
    apr_graph_connect(g, teams, only, 1.0f);
    ASSERT_EQ_INT(2, apr_source_refcount(apr_graph_source(g, teams)));

    apr_graph_disconnect(g, teams, full);
    ASSERT_FALSE(apr_graph_connected(g, teams, full));
    ASSERT_TRUE(apr_graph_connected(g, teams, only));
    ASSERT_EQ_INT(1, apr_source_refcount(apr_graph_source(g, teams)));
    apr_graph_destroy(g);
}

TEST(a_gain_that_is_not_a_number_is_refused_at_the_door)
{
    /* A NaN gain turns the whole bus into NaN, and every integer encoder
     * downstream renders that as silence. Refuse it where there is still a
     * caller to tell. */
    AprGraph   *g = make(1);
    AprSourceId a = src(g, L"A");
    AprBusId    b = bus(g, L"B");
    volatile float z = 0.0f;
    float  bad = z / z;
    AprErr e;

    e = apr_graph_connect(g, a, b, bad);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(0, (int)apr_graph_edge_count(g));

    apr_graph_connect(g, a, b, 1.0f);
    e = apr_bus_set_gain(apr_graph_bus(g, b), a, bad);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NEAR(1.0, apr_bus_gain(apr_graph_bus(g, b), a), 0.0);
    apr_graph_destroy(g);
}

/* ---- removal and lifetime ------------------------------------------------ */

TEST(removing_a_source_takes_every_edge_with_it)
{
    /* The lifetime bug this shape exists to make impossible: a bus left
     * holding a reader into a freed source. */
    AprGraph   *g = make(1);
    AprSourceId teams = src(g, L"Teams");
    AprSourceId mic   = src(g, L"Mic");
    AprBusId    full  = bus(g, L"Full");
    AprBusId    only  = bus(g, L"Only");

    apr_graph_connect(g, teams, full, 1.0f);
    apr_graph_connect(g, teams, only, 1.0f);
    apr_graph_connect(g, mic,   full, 1.0f);
    ASSERT_EQ_INT(3, (int)apr_graph_edge_count(g));

    apr_graph_remove_source(g, teams);

    ASSERT_NULL(apr_graph_source(g, teams));
    ASSERT_EQ_INT(1, (int)apr_graph_source_count(g));
    ASSERT_EQ_INT(1, (int)apr_graph_edge_count(g));
    ASSERT_FALSE(apr_graph_connected(g, teams, full));
    ASSERT_TRUE(apr_graph_connected(g, mic, full));
    ASSERT_EQ_INT(0, (int)apr_bus_source_count(apr_graph_bus(g, only)));
    apr_graph_destroy(g);
}

TEST(removing_a_bus_gives_its_readers_back)
{
    AprGraph   *g = make(1);
    AprSourceId teams = src(g, L"Teams");
    AprBusId    full  = bus(g, L"Full");
    AprBusId    only  = bus(g, L"Only");

    apr_graph_connect(g, teams, full, 1.0f);
    apr_graph_connect(g, teams, only, 1.0f);
    ASSERT_EQ_INT(2, apr_source_refcount(apr_graph_source(g, teams)));

    apr_graph_remove_bus(g, only);
    ASSERT_EQ_INT(1, apr_source_refcount(apr_graph_source(g, teams)));
    ASSERT_EQ_INT(1, (int)apr_graph_bus_count(g));
    ASSERT_NULL(apr_graph_bus(g, only));
    apr_graph_destroy(g);
}

TEST(removing_something_that_is_not_there_is_not_found)
{
    AprGraph *g = make(1);
    AprErr    e;

    e = apr_graph_remove_source(g, 7);
    ASSERT_TRUE(apr_failed(&e));
    e = apr_graph_remove_bus(g, 7);
    ASSERT_TRUE(apr_failed(&e));
    apr_graph_destroy(g);
}

/* ---- projections --------------------------------------------------------- */

TEST(both_projections_answer_the_question_the_view_actually_asks)
{
    /* A node window announces its own edges out; a tree panel nests sources
     * under a bus. Neither view walks the model itself, which is how the two
     * stop agreeing. */
    AprGraph   *g = make(1);
    AprSourceId teams = src(g, L"Teams");
    AprSourceId mic   = src(g, L"Mic");
    AprBusId    full  = bus(g, L"Full");
    AprBusId    only  = bus(g, L"Only");
    AprBusId    ids[4];
    AprSourceId sids[4];

    apr_graph_connect(g, teams, full, 1.0f);
    apr_graph_connect(g, teams, only, 1.0f);
    apr_graph_connect(g, mic,   full, 1.0f);

    ASSERT_EQ_INT(2, (int)apr_graph_buses_for_source(g, teams, ids, 4));
    ASSERT_EQ_INT((int)full, (int)ids[0]);
    ASSERT_EQ_INT((int)only, (int)ids[1]);

    ASSERT_EQ_INT(1, (int)apr_graph_buses_for_source(g, mic, ids, 4));
    ASSERT_EQ_INT((int)full, (int)ids[0]);

    /* Order is connection order, so a list read aloud is stable between
     * refreshes. */
    ASSERT_EQ_INT(2, (int)apr_graph_sources_for_bus(g, full, sids, 4));
    ASSERT_EQ_INT((int)teams, (int)sids[0]);
    ASSERT_EQ_INT((int)mic,   (int)sids[1]);

    apr_graph_destroy(g);
}

TEST(a_projection_reports_the_true_count_even_when_it_cannot_write_it)
{
    AprGraph   *g = make(1);
    AprSourceId teams = src(g, L"Teams");
    AprBusId    a = bus(g, L"A"), b = bus(g, L"B"), c = bus(g, L"C");
    AprBusId    one[1];

    apr_graph_connect(g, teams, a, 1.0f);
    apr_graph_connect(g, teams, b, 1.0f);
    apr_graph_connect(g, teams, c, 1.0f);

    /* A caller that sized its buffer from a stale count must learn it was too
     * small, not silently see a truncated list. */
    ASSERT_EQ_INT(3, (int)apr_graph_buses_for_source(g, teams, one, 1));
    ASSERT_EQ_INT((int)a, (int)one[0]);
    ASSERT_EQ_INT(3, (int)apr_graph_buses_for_source(g, teams, NULL, 0));

    ASSERT_EQ_INT(0, (int)apr_graph_buses_for_source(g, 999, NULL, 0));
    ASSERT_EQ_INT(0, (int)apr_graph_sources_for_bus(g, 999, NULL, 0));
    apr_graph_destroy(g);
}

/* ---- actions ------------------------------------------------------------- */

TEST(actions_are_attached_by_the_id_a_session_file_stores)
{
    AprGraph *g = make(2);
    AprBusId  b = bus(g, L"Main");
    AprActionConfig cfg;
    AprErr    e;

    memset(&cfg, 0, sizeof cfg);
    e = apr_graph_add_action(g, b, "none", &cfg);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(1, (int)apr_bus_action_count(apr_graph_bus(g, b)));
    ASSERT_STR_EQ("none", apr_bus_action_at(apr_graph_bus(g, b), 0)->id);

    e = apr_graph_add_action(g, b, "definitely-not-an-action", &cfg);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_NOT_FOUND, (int)e.kind);

    e = apr_graph_add_action(g, 999, "none", &cfg);
    ASSERT_TRUE(apr_failed(&e));
    apr_graph_destroy(g);
}

TEST(an_action_gets_the_session_format_not_whatever_it_was_handed)
{
    /* cfg arrives with zeroes; if the graph did not fill them in, the action
     * would be created for a 0 Hz stream and would have to refuse. */
    AprGraph *g = make(2);
    AprBusId  b = bus(g, L"Main");
    AprActionConfig cfg;
    AprErr    e;

    memset(&cfg, 0, sizeof cfg);
    e = apr_graph_add_action(g, b, "none", &cfg);
    ASSERT_FALSE(apr_failed(&e));
    apr_graph_destroy(g);
}

/* ---- capacity and running ------------------------------------------------ */

TEST(capacity_limits_are_refused_not_overrun)
{
    AprGraph *g = make(1);
    int       i;
    AprErr    e;

    for (i = 0; i < APR_MAX_BUSES; i++) {
        e = apr_graph_add_bus(g, L"b", NULL);
        ASSERT_FALSE(apr_failed(&e));
    }
    e = apr_graph_add_bus(g, L"one too many", NULL);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_MAX_BUSES, (int)apr_graph_bus_count(g));
    apr_graph_destroy(g);
}

TEST(a_bus_refuses_more_sources_than_it_can_hold)
{
    AprGraph *g = make(1);
    AprBusId  b = bus(g, L"Main");
    int       i;
    AprErr    e = apr_ok();

    for (i = 0; i < APR_MAX_SOURCES_PER_BUS; i++) {
        e = apr_graph_connect(g, src(g, L"s"), b, 1.0f);
        ASSERT_FALSE(apr_failed(&e));
    }
    e = apr_graph_connect(g, src(g, L"one more"), b, 1.0f);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_MAX_SOURCES_PER_BUS,
                  (int)apr_bus_source_count(apr_graph_bus(g, b)));
    apr_graph_destroy(g);
}

TEST(ticking_a_graph_that_is_not_running_is_a_state_error)
{
    AprGraph *g = make(1);
    AprErr    e;

    ASSERT_FALSE(apr_graph_running(g));
    e = apr_graph_tick(g, 1000);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_STATE, (int)e.kind);

    apr_graph_run(g, 1000);
    ASSERT_TRUE(apr_graph_running(g));
    e = apr_graph_tick(g, 2000);
    ASSERT_FALSE(apr_failed(&e));

    apr_graph_stop(g);
    ASSERT_FALSE(apr_graph_running(g));
    apr_graph_stop(g);                     /* idempotent */
    apr_graph_destroy(g);
}

TEST(stopping_finalizes_every_action_including_ones_already_finalized)
{
    AprGraph *g = make(1);
    AprBusId  b = bus(g, L"Main");
    AprActionConfig cfg;

    memset(&cfg, 0, sizeof cfg);
    apr_graph_add_action(g, b, "none", &cfg);
    apr_graph_run(g, 1000);
    apr_graph_stop(g);
    apr_graph_stop(g);
    ASSERT_FALSE(apr_bus_action_failed(apr_graph_bus(g, b), 0));
    apr_graph_destroy(g);              /* must not finalize twice or fault */
}

TEST(null_graph_arguments_are_survived)
{
    AprBusId ids[2];

    ASSERT_EQ_INT(0, (int)apr_graph_rate(NULL));
    ASSERT_EQ_INT(0, (int)apr_graph_channels(NULL));
    ASSERT_EQ_INT(0, (int)apr_graph_source_count(NULL));
    ASSERT_EQ_INT(0, (int)apr_graph_bus_count(NULL));
    ASSERT_EQ_INT(0, (int)apr_graph_edge_count(NULL));
    ASSERT_EQ_INT(0, (int)apr_graph_buses_for_source(NULL, 1, ids, 2));
    ASSERT_EQ_INT(0, (int)apr_graph_sources_for_bus(NULL, 1, NULL, 0));
    ASSERT_FALSE(apr_graph_connected(NULL, 1, 1));
    ASSERT_NULL(apr_graph_source(NULL, 1));
    ASSERT_NULL(apr_graph_bus(NULL, 1));
    apr_graph_destroy(NULL);
}
