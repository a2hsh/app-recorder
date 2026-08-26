/*
 * test_graph_guard.c -- the model refuses to be rewritten mid-recording, and
 * a gain has to be a finite number.
 *
 * BUGS.md C4. graph.h said "do not mutate the shape while a tick is in
 * flight" and that was the whole of the enforcement. The runner's loop walks
 * g->buses and each bus walks its edge array; a UI thread rewriting either one
 * underneath it is a corrupted mix or a crash, mid-recording, with the files
 * already open. The front end greys its editing commands, but Ctrl+Shift+E is
 * deliberately not a frame accelerator and reaches the canvas as a raw
 * keystroke that never passes the controller's busy() check -- and the CLI,
 * session loading and this test are callers too.
 *
 * REFUSED, NOT IGNORED. A silent no-op is the worst available outcome for
 * someone working by ear: the keystroke does nothing, says nothing, and is
 * indistinguishable from a broken app. APR_E_BUSY is what lets a front end say
 * "that cannot be changed while a recording is running" rather than emit a
 * generic failure or, worse, nothing.
 *
 * NOTHING HERE RENDERS AUDIO: one silent fake source, no actions that write.
 */
#include "test_runner.h"

#include "bus.h"
#include "capture.h"
#include "clock.h"
#include "graph.h"
#include "source.h"

#include <string.h>

/* MSVC in C mode: an assignment is not an lvalue, so the &(e = f(...)) idiom
 * does not compile. This is the same assertion without the trick. */
#define ASSERT_OK(call) do { AprErr e_ = (call); ASSERT_FALSE(apr_failed(&e_)); } while (0)

typedef struct Fixture {
    AprGraph   *g;
    AprSourceId src;
    AprSourceId spare;
    AprBusId    bus;
} Fixture;

static void fake_cfg(AprCaptureConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->kind         = APR_SRC_FAKE;
    cfg->sample_rate  = 48000;
    cfg->channels     = 2;
    cfg->fake.tone_hz = 0;      /* silence */
}

static void build(Fixture *f)
{
    AprCaptureConfig cfg;
    AprErr e;

    memset(f, 0, sizeof(*f));
    e = apr_graph_create(48000, 2, &f->g);
    ASSERT_FALSE(apr_failed(&e));

    fake_cfg(&cfg);
    ASSERT_OK(apr_graph_add_source(f->g, L"Teams", &cfg, &f->src));
    ASSERT_OK(apr_graph_add_source(f->g, L"Mic", &cfg, &f->spare));
    ASSERT_OK(apr_graph_add_bus(f->g, L"Main Mix", &f->bus));
    ASSERT_OK(apr_graph_connect(f->g, f->src, f->bus, 1.0f));
}

/* Anchor the buses WITHOUT arming any capture: no thread, no frames, and the
 * graph is running as far as every guard below is concerned. */
static void start(Fixture *f)
{
    AprErr e = apr_graph_run(f->g, apr_qpc_now());
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_TRUE(apr_graph_running(f->g) != 0);
}

static void teardown(Fixture *f)
{
    AprErr e = apr_graph_stop(f->g);
    (void)e;
    apr_graph_destroy(f->g);
}

/* ==========================================================================
 * C4 -- the shape is frozen while the graph runs
 * ======================================================================= */

TEST(disconnect_is_refused_while_recording)
{
    Fixture f;
    AprErr  e;

    build(&f);
    start(&f);

    e = apr_graph_disconnect(f.g, f.src, f.bus);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_BUSY, e.kind);

    /* And it really did not happen -- a refusal that half-applies would be
     * worse than either outcome. */
    ASSERT_TRUE(apr_graph_connected(f.g, f.src, f.bus) != 0);
    ASSERT_EQ_INT(1, (long long)apr_graph_edge_count(f.g));

    teardown(&f);
}

TEST(connect_is_refused_while_recording)
{
    Fixture f;
    AprErr  e;

    build(&f);
    start(&f);

    e = apr_graph_connect(f.g, f.spare, f.bus, 1.0f);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_BUSY, e.kind);
    ASSERT_FALSE(apr_graph_connected(f.g, f.spare, f.bus) != 0);

    teardown(&f);
}

TEST(a_gain_write_is_refused_while_recording)
{
    /* The canvas writes gain straight to the bus, not through the graph
     * (canvas.c calls apr_bus_set_gain), so the guard has to be on the bus as
     * well or this path is still open. */
    Fixture f;
    AprBus *b;
    AprErr  e;

    build(&f);
    start(&f);

    b = apr_graph_bus(f.g, f.bus);
    ASSERT_NOT_NULL(b);

    e = apr_bus_set_gain(b, f.src, 0.25f);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_BUSY, e.kind);
    ASSERT_NEAR(1.0, apr_bus_gain(b, f.src), 1e-9);

    teardown(&f);
}

TEST(adding_and_removing_nodes_is_refused_while_recording)
{
    Fixture          f;
    AprCaptureConfig cfg;
    AprBusId         nb = 0;
    AprSourceId      ns = 0;
    AprErr           e;

    build(&f);
    start(&f);
    fake_cfg(&cfg);

    e = apr_graph_add_source(f.g, L"late", &cfg, &ns);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_BUSY, e.kind);

    e = apr_graph_add_bus(f.g, L"late", &nb);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_BUSY, e.kind);

    e = apr_graph_remove_source(f.g, f.src);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_BUSY, e.kind);

    e = apr_graph_remove_bus(f.g, f.bus);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_BUSY, e.kind);

    /* Nothing moved. */
    ASSERT_EQ_INT(2, (long long)apr_graph_source_count(f.g));
    ASSERT_EQ_INT(1, (long long)apr_graph_bus_count(f.g));

    teardown(&f);
}

TEST(adding_an_output_is_refused_while_recording)
{
    /* An output added mid-take would never be opened -- apr_bus_start has
     * already run -- so it would sit in the model claiming to record and write
     * nothing at all. */
    Fixture         f;
    AprActionConfig cfg;
    AprErr          e;

    build(&f);
    start(&f);

    memset(&cfg, 0, sizeof(cfg));
    e = apr_graph_add_action(f.g, f.bus, "none", &cfg);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_BUSY, e.kind);

    teardown(&f);
}

TEST(every_refusal_lifts_again_once_the_graph_stops)
{
    /* A guard that never lets go is a different bug. */
    Fixture f;
    AprBus *b;

    build(&f);
    start(&f);
    ASSERT_OK(apr_graph_stop(f.g));
    ASSERT_FALSE(apr_graph_running(f.g) != 0);

    ASSERT_OK(apr_graph_connect(f.g, f.spare, f.bus, 1.0f));
    ASSERT_OK(apr_graph_disconnect(f.g, f.src, f.bus));

    b = apr_graph_bus(f.g, f.bus);
    ASSERT_OK(apr_bus_set_gain(b, f.spare, 0.5f));
    ASSERT_NEAR(0.5, apr_bus_gain(b, f.spare), 1e-9);

    apr_graph_destroy(f.g);
}

/* ==========================================================================
 * m12 -- an infinite gain is refused, like a NaN one
 * ======================================================================= */

/* Built at runtime: a literal 1.0f/0.0f is a compile-time division by zero
 * under /W4 /WX, and INFINITY is a macro this codebase does not otherwise
 * lean on. */
static float make_inf(int negative)
{
    volatile float one = 1.0f, zero = 0.0f;
    float v = one / zero;
    return negative ? -v : v;
}

static float make_nan(void)
{
    volatile float zero = 0.0f;
    return zero / zero;
}

TEST(an_infinite_gain_is_refused_by_connect)
{
    /* NaN was already caught, and NaN is the MILD case: core/mix.c scrubs it
     * on the way into every integer format, so a NaN gain renders as silence.
     * Infinity does not scrub to silence -- it clamps, and the file comes out
     * as sustained digital full scale for as long as the recording runs, in
     * the headphones of someone who cannot see a meter. AGENTS.md rule 1 makes
     * that the worse half, and it was the half that got through. */
    Fixture f;
    AprErr  e;

    build(&f);

    e = apr_graph_connect(f.g, f.spare, f.bus, make_inf(0));
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_INVALID_ARG, e.kind);
    ASSERT_FALSE(apr_graph_connected(f.g, f.spare, f.bus) != 0);

    e = apr_graph_connect(f.g, f.spare, f.bus, make_inf(1));
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_INVALID_ARG, e.kind);
    ASSERT_FALSE(apr_graph_connected(f.g, f.spare, f.bus) != 0);

    apr_graph_destroy(f.g);
}

TEST(an_infinite_gain_is_refused_by_set_gain)
{
    Fixture f;
    AprBus *b;
    AprErr  e;

    build(&f);
    b = apr_graph_bus(f.g, f.bus);

    e = apr_bus_set_gain(b, f.src, make_inf(0));
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_INVALID_ARG, e.kind);

    e = apr_bus_set_gain(b, f.src, make_inf(1));
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_INVALID_ARG, e.kind);

    /* The gain that was there is untouched. */
    ASSERT_NEAR(1.0, apr_bus_gain(b, f.src), 1e-9);

    apr_graph_destroy(f.g);
}

TEST(a_nan_gain_is_still_refused)
{
    Fixture f;
    AprBus *b;
    AprErr  e;

    build(&f);
    b = apr_graph_bus(f.g, f.bus);

    e = apr_bus_set_gain(b, f.src, make_nan());
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_INVALID_ARG, e.kind);

    e = apr_graph_connect(f.g, f.spare, f.bus, make_nan());
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_INVALID_ARG, e.kind);

    apr_graph_destroy(f.g);
}

TEST(an_ordinary_finite_gain_still_goes_through)
{
    Fixture f;
    AprBus *b;

    build(&f);
    b = apr_graph_bus(f.g, f.bus);

    ASSERT_OK(apr_bus_set_gain(b, f.src, 0.0f));
    ASSERT_NEAR(0.0, apr_bus_gain(b, f.src), 1e-9);
    ASSERT_OK(apr_bus_set_gain(b, f.src, -2.5f));
    ASSERT_NEAR(-2.5, apr_bus_gain(b, f.src), 1e-9);

    apr_graph_destroy(f.g);
}
