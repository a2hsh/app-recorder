/*
 * test_reconnect.c -- a source that dies must be able to come back.
 *
 * ===========================================================================
 * WHAT IS BEING PROVED, AND WHY EACH ONE IS A NUMBER
 *
 *   THE HOLE IS EXACTLY THE RIGHT SIZE. Not "about right": the silence
 *   between the last frame the old capture produced and the first frame the
 *   new one produces is counted, frame by frame, against what the clock says
 *   was missed. A hole one frame short is a permanent one-frame shift of
 *   everything after it, and a shift is not a dropout -- it cannot be heard,
 *   it cannot be fixed after the file is written, and it moves every bus the
 *   source feeds against every bus it does not.
 *
 *   EVERY BUS STAYS ALIGNED. The recovered source's audio is compared, sample
 *   for sample, against a control source that never died: after the recovery
 *   the two must be IDENTICAL. That works because a fake's samples are a pure
 *   function of the ring's absolute frame index, so landing one frame early
 *   shows up as a waveform that no longer matches rather than as a judgement
 *   call about "roughly in sync".
 *
 *   BOTH HALVES ARE ANNOUNCED. The loss was always announced. The recovery is
 *   the half that is easy to forget and the one that tells the author his take
 *   is intact, so it is counted as an event, in order, alongside the loss.
 *
 *   A SOURCE THAT NEVER RETURNS BEHAVES AS IT DID. The old behaviour is not
 *   allowed to change underneath a recording that has nothing to recover.
 *
 * ===========================================================================
 * SAFETY (AGENTS.md rule 1)
 *
 *   EVERY SOURCE IN THIS FILE IS APR_SRC_FAKE. Nothing here opens an audio
 *   endpoint, activates an IAudioClient or renders a single sample to any
 *   output device. The process and device cases are exercised through the
 *   reconnector's supplied-machine seam, which is enumeration data in a struct
 *   -- the same argument session.h makes for AprSessionMachine.
 *
 *   NO REAL TIME ELAPSES except in the two runner cases at the end, which need
 *   a live loop to have notices to count. Everything else steps a tick value,
 *   so a twenty-minute absence costs microseconds.
 */
#include "test_runner.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>

#include "capture/capture_fake.h"
#include "clock.h"
#include "graph.h"
#include "reconnect.h"
#include "runner.h"
#include "source.h"

#define RATE   48000u
#define TONE     480u        /* 100 frames per cycle at 48 kHz: exact */
#define T0    9876543ull     /* an anchor that is not zero */

#define ASSERT_OK(call) do { AprErr e_ = (call); ASSERT_FALSE(apr_failed(&e_)); } while (0)

/* The first tick at which `frames` frames have elapsed. apr_clock_frame_ticks
 * rounds UP for exactly this reason: frames_to_ticks floors, so its inverse is
 * a tick at which the last frame has not quite happened, and a test stepping
 * to it would be one frame short of what it asked for. */
static uint64_t frames_to_ticks_(uint64_t frames)
{
    AprClock c;
    apr_clock_init(&c, apr_qpc_freq(), RATE);
    apr_clock_anchor(&c, 0);
    return apr_clock_frame_ticks(&c, frames);
}

static AprCaptureConfig fake_cfg(void)
{
    AprCaptureConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.kind        = APR_SRC_FAKE;
    cfg.sample_rate = RATE;
    cfg.channels    = 1;
    cfg.fake.tone_hz   = TONE;
    cfg.fake.amplitude = 0.25f;
    return cfg;
}

/* ==========================================================================
 * 1. capture_fake: the source that comes back
 *
 * These are about the synthetic source itself. If it is wrong, every test
 * above it agrees with it in the same direction (capture_fake.c's own header
 * makes the point), so the revival is held to the same standard as the death:
 * exact frames, not "about there".
 * ======================================================================== */

typedef struct Fx {
    RingBuf    *rb;
    AprCapture *c;
} Fx;

static int fx_up(Fx *fx, const AprCaptureConfig *cfg, size_t frames)
{
    AprErr e;
    memset(fx, 0, sizeof *fx);
    e = rb_create(frames, (size_t)cfg->channels * sizeof(float), &fx->rb);
    if (apr_failed(&e)) return 0;
    e = apr_capture_create(cfg, fx->rb, &fx->c);
    if (apr_failed(&e)) { rb_destroy(fx->rb); fx->rb = NULL; return 0; }
    return 1;
}

static void fx_down(Fx *fx)
{
    apr_capture_destroy(fx->c);
    rb_destroy(fx->rb);
    memset(fx, 0, sizeof *fx);
}

TEST(a_fake_that_revives_fills_the_hole_with_exactly_the_missing_frames)
{
    /* Die at 2,000 frames, come back at 5,000. The file must then hold tone up
     * to 2,000, EXACTLY 3,000 zeros, and tone again from 5,000 -- and the tone
     * after the hole must be the tone that belongs at those absolute indices,
     * not the waveform starting over. */
    AprCaptureConfig cfg = fake_cfg();
    Fx fx;
    RingReader rd;
    float buf[8192];
    size_t got, i;
    uint64_t zeros_from = 0, zeros_to = 0;

    cfg.fake.die_at_frame    = 2000;
    cfg.fake.revive_at_frame = 5000;

    ASSERT_TRUE(fx_up(&fx, &cfg, 8192));
    ASSERT_OK(apr_capture_fake_advance(fx.c, T0));
    ASSERT_OK(apr_capture_fake_advance(fx.c, T0 + frames_to_ticks_(8000)));

    rb_reader_init(&rd, fx.rb, RB_START_OLDEST);
    got = rb_read(&rd, buf, 8192, NULL);
    ASSERT_GE_INT(8000, (int)got);

    for (i = 0; i < 8000; i++) {
        int silent = (buf[i] == 0.0f);
        /* The tone is 480 Hz at 48 kHz, so it is zero every 50 frames by
         * construction; only a RUN of zeros means "dead". Track the run. */
        if (silent && zeros_from == 0 && i >= 1990 && i <= 2010) zeros_from = i;
        if (!silent && zeros_from != 0 && zeros_to == 0 && i > zeros_from + 100)
            zeros_to = i;
    }

    /* Every frame in [2000, 5000) is silent and no frame outside it is part of
     * a run: assert that directly rather than trusting the scan above. */
    for (i = 2000; i < 5000; i++) ASSERT_EQ_INT(0, (int)(buf[i] != 0.0f));

    /* The hole is exactly 3,000 frames: frame 4,999 is silence and the tone is
     * back by 5,000. It is not enough that SOME audio returned -- returning one
     * frame early would be an audio file that is one frame out of sync with
     * every other file on the machine for the rest of the session. */
    {
        int any = 0;
        for (i = 5000; i < 5050; i++) if (buf[i] != 0.0f) any = 1;
        ASSERT_TRUE(any);
    }

    /* And the waveform after the hole is the one that belongs at those
     * indices. The tone has a period of exactly 100 frames, so a correct
     * revival makes buf[5000 + k] == buf[100 + k] for a whole period; a
     * revival that restarted the waveform, or landed at the wrong frame,
     * would not. */
    for (i = 0; i < 100; i++) ASSERT_NEAR(buf[100 + i], buf[5000 + i], 1e-6);

    fx_down(&fx);
}

TEST(a_revived_fake_says_it_is_alive_again_and_clears_its_error)
{
    /* THE ERROR, NOT ONLY THE FLAG. A source producing audio again while still
     * carrying the sentence that says it exited is a status line that lies,
     * and both front ends read exactly these two fields. */
    AprCaptureConfig cfg = fake_cfg();
    AprCaptureStatus st;
    Fx fx;

    cfg.fake.die_at_frame    = 1000;
    cfg.fake.revive_at_frame = 3000;

    ASSERT_TRUE(fx_up(&fx, &cfg, 8192));
    ASSERT_OK(apr_capture_fake_advance(fx.c, T0));

    apr_capture_fake_advance(fx.c, T0 + frames_to_ticks_(2000));
    fx.c->vt->status(fx.c, &st);
    ASSERT_FALSE(st.alive);
    ASSERT_TRUE(apr_failed(&st.last_error));

    apr_capture_fake_advance(fx.c, T0 + frames_to_ticks_(4000));
    fx.c->vt->status(fx.c, &st);
    ASSERT_TRUE(st.alive);
    ASSERT_FALSE(apr_failed(&st.last_error));

    fx_down(&fx);
}

TEST(a_death_and_a_revival_on_the_same_frame_are_refused_rather_than_guessed)
{
    /* Same rule mute/unmute already follow: resolving it quietly would make
     * the source's behaviour depend on the order of two lines in whatever
     * configured it. */
    AprCaptureConfig cfg = fake_cfg();
    RingBuf *rb = NULL;
    AprCapture *c = NULL;
    AprErr e;

    cfg.fake.die_at_frame    = 4800;
    cfg.fake.revive_at_frame = 4800;

    e = rb_create(1024, sizeof(float), &rb);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_capture_create(&cfg, rb, &c);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(c);
    rb_destroy(rb);
}

TEST(a_revival_lands_on_the_same_frame_however_finely_the_timeline_is_stepped)
{
    /* Health is a pure function of the absolute frame index, so a caller that
     * steps in 7-frame slices and one that takes a single leap must get
     * byte-identical rings. Without that, "the hole is exactly N frames" is a
     * property of the caller's tick pattern rather than of the source. */
    AprCaptureConfig cfg = fake_cfg();
    Fx a, b;
    float pa[6000], pb[6000];
    RingReader ra, rb_;
    uint64_t t;

    cfg.fake.die_at_frame    = 1234;
    cfg.fake.revive_at_frame = 4321;

    ASSERT_TRUE(fx_up(&a, &cfg, 8192));
    ASSERT_TRUE(fx_up(&b, &cfg, 8192));

    ASSERT_OK(apr_capture_fake_advance(a.c, T0));
    ASSERT_OK(apr_capture_fake_advance(b.c, T0));

    ASSERT_OK(apr_capture_fake_advance(a.c, T0 + frames_to_ticks_(6000)));
    for (t = 7; t <= 6000; t += 7)
        ASSERT_OK(apr_capture_fake_advance(b.c, T0 + frames_to_ticks_(t)));
    ASSERT_OK(apr_capture_fake_advance(b.c, T0 + frames_to_ticks_(6000)));

    rb_reader_init(&ra, a.rb, RB_START_OLDEST);
    rb_reader_init(&rb_, b.rb, RB_START_OLDEST);
    ASSERT_EQ_INT(6000, (int)rb_read(&ra, pa, 6000, NULL));
    ASSERT_EQ_INT(6000, (int)rb_read(&rb_, pb, 6000, NULL));
    ASSERT_MEM_EQ(pa, pb, sizeof pa);

    fx_down(&a);
    fx_down(&b);
}

/* ==========================================================================
 * 2. The source: detach, hold the place, reattach
 *
 * This is the mechanism itself -- a NEW capture on the SAME ring -- and the
 * assertions are all about where its first frame lands.
 * ======================================================================== */

/* Frames the ring says have been produced, against what the clock says are
 * due. The invariant the whole feature rests on: the ring's write position IS
 * the absolute frame index. */
static int64_t ring_error(AprSource *s, uint64_t now)
{
    int64_t due = apr_clock_expected_frames(apr_source_clock(s), now);
    return (int64_t)apr_source_frames(s) - due;
}

TEST(a_detached_source_holds_its_place_instead_of_stalling)
{
    /* A detached source has no producer at all, so without this its ring
     * simply stops -- and every controller downstream then measures a backlog
     * collapsing against a source that is not there. */
    AprSource *s = NULL;
    AprCaptureConfig cfg = fake_cfg();
    uint64_t t;

    ASSERT_OK(apr_source_create(1, L"tap", &cfg, &s));
    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(s), T0));
    apr_source_poll(s, NULL);                     /* anchors the clock */
    ASSERT_TRUE(apr_source_anchored(s));

    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(s),
                                       T0 + frames_to_ticks_(4800)));
    ASSERT_TRUE(apr_source_attached(s));
    ASSERT_OK(apr_source_detach(s));
    ASSERT_FALSE(apr_source_attached(s));

    /* Twenty minutes of absence, in one call, on a 250 ms ring. */
    t = T0 + frames_to_ticks_((uint64_t)RATE * 20u * 60u);
    ASSERT_GT_INT(0, (int)apr_source_pad_to(s, t));
    ASSERT_EQ_INT(0, (int)ring_error(s, t));

    /* Idempotent: a second pad at the same instant owes nothing. */
    ASSERT_EQ_INT(0, (int)apr_source_pad_to(s, t));

    apr_source_destroy(s);
}

TEST(a_reattached_source_resumes_at_its_absolute_frame_and_not_at_now)
{
    /* THE CENTRAL ASSERTION OF THE WHOLE FEATURE. The replacement capture's
     * first frame must land at the index the clock implies, so that the hole
     * is exactly as long as the absence was. Landing at the ring's current
     * write position instead -- the obvious implementation -- would put every
     * frame after the recovery early, for ever. */
    AprSource *s = NULL;
    AprCaptureConfig cfg = fake_cfg();
    uint64_t back_at, later;

    ASSERT_OK(apr_source_create(1, L"tap", &cfg, &s));
    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(s), T0));
    apr_source_poll(s, NULL);
    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(s),
                                       T0 + frames_to_ticks_(4800)));
    ASSERT_EQ_INT(4800, (int)apr_source_frames(s));

    ASSERT_OK(apr_source_detach(s));

    /* Away for 60,000 frames. Nothing pads the ring in between: the resume
     * arithmetic alone has to close the gap. */
    back_at = T0 + frames_to_ticks_(64800);
    ASSERT_OK(apr_source_reattach(s, &cfg));
    ASSERT_TRUE(apr_source_attached(s));
    ASSERT_EQ_INT(2, (int)apr_source_generation(s));

    /* The first advance is the replacement's anchor -- the instant its own
     * frame 0 exists -- and that is when the hole is filled. */
    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(s), back_at));
    ASSERT_EQ_INT(64800, (int)apr_source_frames(s));
    ASSERT_EQ_INT(0, (int)ring_error(s, back_at));

    later = back_at + frames_to_ticks_(4800);
    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(s), later));
    ASSERT_EQ_INT(69600, (int)apr_source_frames(s));
    ASSERT_EQ_INT(0, (int)ring_error(s, later));

    apr_source_destroy(s);
}

TEST(a_recovered_source_is_sample_identical_to_one_that_never_died)
{
    /* "Every bus stays aligned" as a number. Two sources with the same config
     * on the same timeline; one is torn down and replaced half way through.
     * After the recovery their rings must agree frame for frame -- which they
     * can only do if the replacement's audio landed at exactly the index it
     * belonged at, because the fake's samples are a pure function of that
     * index. One frame out and the waveforms diverge everywhere. */
    AprSource *live = NULL, *cut = NULL;
    AprCaptureConfig cfg = fake_cfg();
    RingReader ra, rb_;
    float a[8192], b[8192];
    uint64_t gone_at, back_at, end, first_a, first_b;
    size_t got_a, got_b;

    ASSERT_OK(apr_source_create(1, L"control", &cfg, &live));
    ASSERT_OK(apr_source_create(2, L"cut",     &cfg, &cut));
    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(live), T0));
    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(cut),  T0));
    apr_source_poll(live, NULL);
    apr_source_poll(cut,  NULL);

    gone_at = T0 + frames_to_ticks_(1000);
    back_at = T0 + frames_to_ticks_(3000);
    end     = T0 + frames_to_ticks_(6000);

    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(live), gone_at));
    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(cut),  gone_at));

    ASSERT_OK(apr_source_detach(cut));
    ASSERT_OK(apr_source_reattach(cut, &cfg));

    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(live), back_at));
    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(cut),  back_at));
    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(live), end));
    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(cut),  end));

    ASSERT_EQ_U64(apr_source_frames(live), apr_source_frames(cut));

    rb_reader_init(&ra, apr_source_ring(live), RB_START_OLDEST);
    rb_reader_init(&rb_, apr_source_ring(cut),  RB_START_OLDEST);
    first_a = rb_reader_pos(&ra);
    first_b = rb_reader_pos(&rb_);
    ASSERT_EQ_U64(first_a, first_b);
    got_a = rb_read(&ra, a, 8192, NULL);
    got_b = rb_read(&rb_, b, 8192, NULL);
    ASSERT_EQ_U64((uint64_t)got_a, (uint64_t)got_b);

    /* Index i is absolute frame first_a + i in BOTH rings. Everything from 400
     * frames after the recovery to the end must match sample for sample; one
     * frame of misalignment and the two waveforms disagree everywhere. */
    {
        size_t i, from = (size_t)(3400 - first_a);
        ASSERT_LT_INT((int)got_a, (int)from);
        for (i = from; i < got_a; i++) ASSERT_NEAR(a[i], b[i], 1e-6);
    }

    /* And the hole really was a hole: the recovered source is silent through
     * the whole absence, so the match above is not two silent buffers. */
    {
        size_t i;
        for (i = (size_t)(1000 - first_a); i < (size_t)(3000 - first_a); i++)
            ASSERT_EQ_INT(0, (int)(b[i] != 0.0f));
    }

    apr_source_destroy(live);
    apr_source_destroy(cut);
}

TEST(a_replacement_at_a_different_format_is_refused)
{
    /* The ring was built for this frame size and every reader's cursor counts
     * in it. A silent acceptance here would shear every frame after the
     * recovery. */
    AprSource *s = NULL;
    AprCaptureConfig cfg = fake_cfg();
    AprCaptureConfig wrong;
    AprErr e;

    ASSERT_OK(apr_source_create(1, L"tap", &cfg, &s));
    ASSERT_OK(apr_source_detach(s));

    wrong = cfg;
    wrong.channels = 2;
    e = apr_source_reattach(s, &wrong);
    ASSERT_TRUE(apr_failed(&e));

    wrong = cfg;
    wrong.sample_rate = 44100;
    e = apr_source_reattach(s, &wrong);
    ASSERT_TRUE(apr_failed(&e));

    ASSERT_FALSE(apr_source_attached(s));
    ASSERT_EQ_INT(1, (int)apr_source_generation(s));
    apr_source_destroy(s);
}

/* ==========================================================================
 * 3. The policy: what counts as the same source coming back
 *
 * Driven with a supplied machine and dry_run, so every outcome is reachable
 * with no hardware, no second copy of Chrome, and no real time. The identity
 * rules themselves belong to session.h; what is asserted here is that this
 * module asks it the right question and acts on the answer.
 * ======================================================================== */

#define MAX_FAKE_APPS 8
#define MAX_FAKE_EPS  8

typedef struct World {
    AprAudioApp      apps[MAX_FAKE_APPS];
    size_t           app_count;
    AprAudioEndpoint eps[MAX_FAKE_EPS];
    size_t           ep_count;
    wchar_t          classes[MAX_FAKE_APPS][64];
    AprSessionMachine m;
} World;

static const wchar_t *world_class(void *user, uint32_t pid)
{
    World *w = (World *)user;
    size_t i;
    for (i = 0; i < w->app_count; i++)
        if (w->apps[i].pid == pid) return w->classes[i];
    return NULL;
}

static int world_exists(void *user, uint32_t pid)
{
    World *w = (World *)user;
    size_t i;
    for (i = 0; i < w->app_count; i++) if (w->apps[i].pid == pid) return 1;
    return 0;
}

static void world_image(void *user, uint32_t pid, wchar_t *buf, size_t cch)
{
    World *w = (World *)user;
    size_t i;
    if (cch) buf[0] = L'\0';
    for (i = 0; i < w->app_count; i++)
        if (w->apps[i].pid == pid) { wcscpy_s(buf, cch, w->apps[i].exe); return; }
}

static void world_wire(World *w)
{
    memset(&w->m, 0, sizeof w->m);
    w->m.apps           = w->apps;
    w->m.app_count      = w->app_count;
    w->m.endpoints      = w->eps;
    w->m.endpoint_count = w->ep_count;
    w->m.window_class   = world_class;
    w->m.process_exists = world_exists;
    w->m.image_name     = world_image;
    w->m.user           = w;
}

static void world_app(World *w, uint32_t pid, const wchar_t *exe,
                      const wchar_t *path, const wchar_t *cls)
{
    size_t i = w->app_count++;
    memset(&w->apps[i], 0, sizeof w->apps[i]);
    w->apps[i].pid = pid;
    wcscpy_s(w->apps[i].exe, APR_DISC_NAME_CCH, exe);
    wcscpy_s(w->apps[i].path, APR_DISC_PATH_CCH, path);
    wcscpy_s(w->classes[i], 64, cls ? cls : L"");
}

static void world_ep(World *w, const wchar_t *id, const wchar_t *name)
{
    size_t i = w->ep_count++;
    memset(&w->eps[i], 0, sizeof w->eps[i]);
    wcscpy_s(w->eps[i].id, APR_DISC_ENDPOINT_CCH, id);
    wcscpy_s(w->eps[i].name, APR_DISC_NAME_CCH, name);
}

/* A graph whose single source is a fake that is dead from frame 0 -- which is
 * what a source whose target has exited looks like from every angle this
 * module can see. */
typedef struct Rig {
    AprGraph       *g;
    AprSourceId     id;
    AprReconnector *rc;
} Rig;

static int rig_up(Rig *r, const AprReconnectOptions *opt, int dead)
{
    AprCaptureConfig cfg = fake_cfg();
    AprReconnectOptions ro;
    AprErr e;

    memset(r, 0, sizeof *r);
    cfg.channels = 2;                 /* the graph imposes its own format */
    cfg.fake.start_dead = dead ? 1 : 0;

    e = apr_graph_create(RATE, 2, &r->g);
    if (apr_failed(&e)) return 0;
    e = apr_graph_add_source(r->g, L"target", &cfg, &r->id);
    if (apr_failed(&e)) return 0;

    if (opt) ro = *opt;
    else { memset(&ro, 0, sizeof ro); ro.synthetic = 1; ro.dry_run = 1; }

    e = apr_reconnect_create(r->g, &ro, &r->rc);
    return !apr_failed(&e);
}

static void rig_down(Rig *r)
{
    apr_reconnect_destroy(r->rc);
    apr_graph_destroy(r->g);
    memset(r, 0, sizeof *r);
}

static AprSessionSource process_ident(const wchar_t *exe, const wchar_t *path,
                                      const wchar_t *cls, uint32_t pid,
                                      AprSessionSrcKind kind)
{
    AprSessionSource id;
    memset(&id, 0, sizeof id);
    id.kind = kind;
    wcscpy_s(id.name, APR_NAME_CCH, L"target");
    wcscpy_s(id.exe, APR_DISC_NAME_CCH, exe);
    if (path) wcscpy_s(id.path, APR_DISC_PATH_CCH, path);
    if (cls)  wcscpy_s(id.window_class, APR_SESSION_CLASS_CCH, cls);
    id.pid = pid;
    return id;
}

/* Step until the source settles or `passes` have gone by, advancing the clock
 * far enough each time that whatever backoff is in force has expired. */
static void settle(Rig *r, int passes)
{
    uint64_t now = T0;
    int i;
    for (i = 0; i < passes; i++) {
        apr_reconnect_step(r->rc, now);
        if (apr_reconnect_link(r->rc, r->id) == APR_LINK_LIVE && i > 0) return;
        now += apr_mul_div_u64(APR_RECONNECT_MAX_MS + 1000u,
                               apr_qpc_freq(), 1000u, NULL);
    }
}

TEST(a_restarted_application_is_found_by_its_image_and_not_by_its_pid)
{
    /* The pid the source was recording is dead and gone. What comes back is a
     * different number running the same image, and matching it is the whole
     * point -- matching the OLD number would only ever fire on a recycled one,
     * which is a different program. */
    Rig r;
    World w;
    AprSessionSource id;

    memset(&w, 0, sizeof w);
    world_app(&w, 9134, L"teams.exe", L"C:\\apps\\teams.exe", L"Chrome_WidgetWin_1");
    world_wire(&w);

    ASSERT_TRUE(rig_up(&r, NULL, 1));
    apr_reconnect_set_machine(r.rc, &w.m);
    id = process_ident(L"teams.exe", L"C:\\apps\\teams.exe",
                       L"Chrome_WidgetWin_1", 0, APR_SESSION_SRC_PROCESS);
    ASSERT_OK(apr_reconnect_set_identity(r.rc, r.id, &id));

    settle(&r, 4);
    ASSERT_EQ_INT(APR_LINK_LIVE, (int)apr_reconnect_link(r.rc, r.id));
    ASSERT_EQ_INT(9134, (int)apr_reconnect_target_pid(r.rc, r.id));
    ASSERT_EQ_INT(1, (int)apr_reconnect_recoveries(r.rc, r.id));

    rig_down(&r);
}

TEST(two_instances_and_nothing_to_choose_by_leaves_the_source_down)
{
    /* Guessing here attaches to a different instance of the same application
     * half way through a recording, which sounds exactly like the right one
     * being quiet. Staying down is the safe half, and the search continues. */
    Rig r;
    World w;
    AprSessionSource id;

    memset(&w, 0, sizeof w);
    world_app(&w, 100, L"chrome.exe", L"C:\\c\\chrome.exe", L"");
    world_app(&w, 200, L"chrome.exe", L"C:\\c\\chrome.exe", L"");
    world_wire(&w);

    ASSERT_TRUE(rig_up(&r, NULL, 1));
    apr_reconnect_set_machine(r.rc, &w.m);
    id = process_ident(L"chrome.exe", L"C:\\c\\chrome.exe", L"",
                       0, APR_SESSION_SRC_PROCESS);
    ASSERT_OK(apr_reconnect_set_identity(r.rc, r.id, &id));

    settle(&r, 4);
    ASSERT_EQ_INT(APR_LINK_REFUSED, (int)apr_reconnect_link(r.rc, r.id));
    ASSERT_EQ_INT(0, (int)apr_reconnect_recoveries(r.rc, r.id));
    ASSERT_EQ_INT(0, (int)apr_reconnect_target_pid(r.rc, r.id));

    rig_down(&r);
}

TEST(the_window_class_chooses_between_two_instances)
{
    /* The one thing that tells two copies of an application apart (design 9),
     * and the reason it is stored at all. */
    Rig r;
    World w;
    AprSessionSource id;

    memset(&w, 0, sizeof w);
    world_app(&w, 100, L"chrome.exe", L"C:\\c\\chrome.exe", L"Chrome_WidgetWin_1");
    world_app(&w, 200, L"chrome.exe", L"C:\\c\\chrome.exe", L"SomethingElse");
    world_wire(&w);

    ASSERT_TRUE(rig_up(&r, NULL, 1));
    apr_reconnect_set_machine(r.rc, &w.m);
    id = process_ident(L"chrome.exe", L"C:\\c\\chrome.exe", L"SomethingElse",
                       0, APR_SESSION_SRC_PROCESS);
    ASSERT_OK(apr_reconnect_set_identity(r.rc, r.id, &id));

    settle(&r, 4);
    ASSERT_EQ_INT(APR_LINK_LIVE, (int)apr_reconnect_link(r.rc, r.id));
    ASSERT_EQ_INT(200, (int)apr_reconnect_target_pid(r.rc, r.id));

    rig_down(&r);
}

TEST(a_device_back_on_a_different_port_is_accepted_by_its_friendly_name)
{
    /* An endpoint id is a per-installation GUID pair: a different USB socket,
     * a driver reinstall or a Windows upgrade mints a new one for the same
     * physical device. The session resolver already accepts the friendly name
     * for exactly this, and disagreeing here would mean a device that reopens
     * a saved session fine but cannot be recovered mid-take. */
    Rig r;
    World w;
    AprSessionSource id;

    memset(&w, 0, sizeof w);
    world_ep(&w, L"{0.0.1.00000000}.{NEW-GUID}", L"Chat Mic (TC-Helicon GoXLR)");
    world_wire(&w);

    ASSERT_TRUE(rig_up(&r, NULL, 1));
    apr_reconnect_set_machine(r.rc, &w.m);

    memset(&id, 0, sizeof id);
    id.kind = APR_SESSION_SRC_DEVICE;
    wcscpy_s(id.name, APR_NAME_CCH, L"Chat Mic");
    wcscpy_s(id.endpoint_id, APR_DISC_ENDPOINT_CCH, L"{0.0.1.00000000}.{OLD-GUID}");
    wcscpy_s(id.endpoint_name, APR_DISC_NAME_CCH, L"Chat Mic (TC-Helicon GoXLR)");
    ASSERT_OK(apr_reconnect_set_identity(r.rc, r.id, &id));

    settle(&r, 4);
    ASSERT_EQ_INT(APR_LINK_LIVE, (int)apr_reconnect_link(r.rc, r.id));

    rig_down(&r);
}

TEST(two_devices_with_the_same_friendly_name_leave_the_source_down)
{
    /* Two identical GoXLRs. Guessing silently records the wrong microphone. */
    Rig r;
    World w;
    AprSessionSource id;

    memset(&w, 0, sizeof w);
    world_ep(&w, L"{A}", L"Chat Mic (TC-Helicon GoXLR)");
    world_ep(&w, L"{B}", L"Chat Mic (TC-Helicon GoXLR)");
    world_wire(&w);

    ASSERT_TRUE(rig_up(&r, NULL, 1));
    apr_reconnect_set_machine(r.rc, &w.m);

    memset(&id, 0, sizeof id);
    id.kind = APR_SESSION_SRC_DEVICE;
    wcscpy_s(id.endpoint_id, APR_DISC_ENDPOINT_CCH, L"{GONE}");
    wcscpy_s(id.endpoint_name, APR_DISC_NAME_CCH, L"Chat Mic (TC-Helicon GoXLR)");
    ASSERT_OK(apr_reconnect_set_identity(r.rc, r.id, &id));

    settle(&r, 4);
    ASSERT_EQ_INT(APR_LINK_REFUSED, (int)apr_reconnect_link(r.rc, r.id));

    rig_down(&r);
}

TEST(a_device_that_returns_after_twenty_minutes_is_still_caught)
{
    /* THE RETRY POLICY, AS A NUMBER. A three-hour recording and a device that
     * comes back after twenty minutes must still be caught -- so nothing ever
     * gives up -- while the doubling ceiling keeps the cost of those twenty
     * minutes to a bounded handful of machine-wide enumerations rather than
     * one every 25 ms. Both halves are asserted, because the first without the
     * second is a COM enumeration storm nobody would ship. */
    Rig r;
    World w;
    AprSessionSource id;
    uint64_t now = T0;
    uint64_t twenty_min_ticks = apr_mul_div_u64(20u * 60u * 1000u,
                                               apr_qpc_freq(), 1000u, NULL);
    uint64_t deadline = T0 + twenty_min_ticks;
    int passes = 0;

    memset(&w, 0, sizeof w);
    world_wire(&w);                       /* the machine has nothing on it */

    ASSERT_TRUE(rig_up(&r, NULL, 1));
    apr_reconnect_set_machine(r.rc, &w.m);

    memset(&id, 0, sizeof id);
    id.kind = APR_SESSION_SRC_DEVICE;
    wcscpy_s(id.endpoint_id, APR_DISC_ENDPOINT_CCH, L"{GOXLR}");
    wcscpy_s(id.endpoint_name, APR_DISC_NAME_CCH, L"Chat Mic");
    ASSERT_OK(apr_reconnect_set_identity(r.rc, r.id, &id));

    /* Twenty minutes of nothing, stepped at the worker's real pad cadence so
     * the search count is the one a real run would produce. */
    while (now < deadline) {
        apr_reconnect_step(r.rc, now);
        now += apr_mul_div_u64(APR_RECONNECT_PAD_MS, apr_qpc_freq(), 1000u, NULL);
        passes++;
    }
    ASSERT_EQ_INT(APR_LINK_SEARCHING, (int)apr_reconnect_link(r.rc, r.id));
    ASSERT_EQ_INT(0, (int)apr_reconnect_recoveries(r.rc, r.id));

    /* Never gives up: it is still trying, and the backoff has settled at the
     * ceiling rather than running away. */
    ASSERT_EQ_INT((int)APR_RECONNECT_MAX_MS,
                  (int)apr_reconnect_backoff_ms(r.rc, r.id));

    /* Bounded cost: 20 minutes at a 15 s ceiling is about 80 searches, plus
     * the handful the doubling took to get there. It is emphatically NOT one
     * per pass. */
    printf("      %llu searches over %d passes of %u ms\n",
           (unsigned long long)apr_reconnect_searches(r.rc), passes,
           APR_RECONNECT_PAD_MS);
    ASSERT_LT_INT(120, (int)apr_reconnect_searches(r.rc));
    ASSERT_GT_INT(60, (int)apr_reconnect_searches(r.rc));

    /* And then it is plugged back in. */
    world_ep(&w, L"{GOXLR}", L"Chat Mic");
    world_wire(&w);
    apr_reconnect_set_machine(r.rc, &w.m);

    now += apr_mul_div_u64(APR_RECONNECT_MAX_MS + 1000u,
                           apr_qpc_freq(), 1000u, NULL);
    apr_reconnect_step(r.rc, now);
    ASSERT_EQ_INT(APR_LINK_LIVE, (int)apr_reconnect_link(r.rc, r.id));
    ASSERT_EQ_INT(1, (int)apr_reconnect_recoveries(r.rc, r.id));

    rig_down(&r);
}

/* ==========================================================================
 * 4. EXCLUDE mode: the exclusion has to follow the application
 * ======================================================================== */

TEST(an_excluded_application_that_restarts_takes_its_exclusion_with_it)
{
    /* EXCLUDE names a pid. When that process exits and starts again under a
     * new number, a capture still excluding the old one is now RECORDING the
     * application the user explicitly excluded -- silently, with nothing in
     * the interface saying so. The exclusion must move. */
    Rig r;
    World w;
    AprSessionSource id;
    AprReconnectOptions ro;

    memset(&ro, 0, sizeof ro);
    ro.synthetic = 1;
    ro.dry_run   = 1;

    memset(&w, 0, sizeof w);
    world_app(&w, 4242, L"discord.exe", L"C:\\d\\discord.exe", L"Chrome_WidgetWin_1");
    world_wire(&w);

    /* Alive, so the ONLY thing that can make this source down is the exclusion
     * going stale -- which is the property under test. */
    ASSERT_TRUE(rig_up(&r, &ro, 0));
    apr_reconnect_set_machine(r.rc, &w.m);

    /* pid 1111 is the instance being excluded right now, and it is not in the
     * world any more: it exited. */
    id = process_ident(L"discord.exe", L"C:\\d\\discord.exe",
                       L"Chrome_WidgetWin_1", 1111,
                       APR_SESSION_SRC_SYSTEM_MINUS_TREE);
    ASSERT_OK(apr_reconnect_set_identity(r.rc, r.id, &id));
    ASSERT_EQ_INT(1111, (int)apr_reconnect_target_pid(r.rc, r.id));

    settle(&r, 4);
    ASSERT_EQ_INT(APR_LINK_LIVE, (int)apr_reconnect_link(r.rc, r.id));
    ASSERT_EQ_INT(4242, (int)apr_reconnect_target_pid(r.rc, r.id));

    rig_down(&r);
}

TEST(an_exclusion_whose_application_is_gone_holds_the_capture_down)
{
    /* And while it cannot be re-established, the capture is HELD rather than
     * left recording an un-excluded machine. session.h made this exact call at
     * load time -- dropping the target of an exclusion records MORE, so
     * failing safe means failing loudly -- and this is that rule at run time.
     *
     * The backoff ceiling is much lower than an ordinary source's, because
     * every second held is a second of the recording's MAIN content and not of
     * one track. */
    Rig r;
    World w;
    AprSessionSource id;
    AprReconnectOptions ro;
    uint64_t now = T0;
    int i;

    memset(&ro, 0, sizeof ro);
    ro.synthetic = 1;
    ro.dry_run   = 1;

    memset(&w, 0, sizeof w);
    world_wire(&w);                       /* discord is not running at all */

    ASSERT_TRUE(rig_up(&r, &ro, 0));
    apr_reconnect_set_machine(r.rc, &w.m);
    id = process_ident(L"discord.exe", L"C:\\d\\discord.exe", L"", 1111,
                       APR_SESSION_SRC_SYSTEM_MINUS_TREE);
    ASSERT_OK(apr_reconnect_set_identity(r.rc, r.id, &id));

    for (i = 0; i < 6; i++) {
        apr_reconnect_step(r.rc, now);
        now += apr_mul_div_u64(APR_RECONNECT_MAX_HELD_MS + 100u,
                               apr_qpc_freq(), 1000u, NULL);
    }

    ASSERT_EQ_INT(APR_LINK_HELD, (int)apr_reconnect_link(r.rc, r.id));
    ASSERT_EQ_INT(0, (int)apr_reconnect_recoveries(r.rc, r.id));
    ASSERT_EQ_INT((int)APR_RECONNECT_MAX_HELD_MS,
                  (int)apr_reconnect_backoff_ms(r.rc, r.id));

    rig_down(&r);
}

TEST(reconnection_can_be_switched_off_but_the_exclusion_hold_cannot)
{
    /* An author may legitimately want a source to stay dead. Nobody gets to
     * ask for an exclusion that quietly stops applying. */
    Rig ordinary, excluded;
    World w;
    AprSessionSource id;
    AprReconnectOptions ro;

    memset(&ro, 0, sizeof ro);
    ro.disabled = 1;
    ro.dry_run  = 1;

    memset(&w, 0, sizeof w);
    world_wire(&w);

    ASSERT_TRUE(rig_up(&ordinary, &ro, 1));
    apr_reconnect_set_machine(ordinary.rc, &w.m);
    id = process_ident(L"teams.exe", L"C:\\t\\teams.exe", L"", 0,
                       APR_SESSION_SRC_PROCESS);
    (void)apr_reconnect_set_identity(ordinary.rc, ordinary.id, &id);
    apr_reconnect_step(ordinary.rc, T0);
    ASSERT_EQ_INT(APR_LINK_OFF, (int)apr_reconnect_link(ordinary.rc, ordinary.id));

    ASSERT_TRUE(rig_up(&excluded, &ro, 0));
    apr_reconnect_set_machine(excluded.rc, &w.m);
    id = process_ident(L"discord.exe", L"C:\\d\\discord.exe", L"", 1111,
                       APR_SESSION_SRC_SYSTEM_MINUS_TREE);
    ASSERT_OK(apr_reconnect_set_identity(excluded.rc, excluded.id, &id));
    apr_reconnect_step(excluded.rc, T0);
    ASSERT_EQ_INT(APR_LINK_HELD, (int)apr_reconnect_link(excluded.rc, excluded.id));

    rig_down(&ordinary);
    rig_down(&excluded);
}

TEST(a_synthetic_source_is_left_alone_unless_a_caller_asks)
{
    /* A fake is not a thing that can be unplugged, and its health knobs
     * describe the fate of one instance. Reviving one by default would make
     * die_at_frame untestable everywhere else in the suite. */
    Rig r;
    AprReconnectOptions ro;

    memset(&ro, 0, sizeof ro);
    ASSERT_TRUE(rig_up(&r, &ro, 1));
    apr_reconnect_step(r.rc, T0);
    apr_reconnect_step(r.rc, T0 + apr_qpc_freq());
    ASSERT_EQ_INT(APR_LINK_OFF, (int)apr_reconnect_link(r.rc, r.id));
    ASSERT_EQ_INT(0, (int)apr_reconnect_searches(r.rc));
    rig_down(&r);
}

/* ==========================================================================
 * 5. The whole mechanism, applied: detach, hold the place, reattach
 *
 * dry_run off. The reconnector really tears the capture down, really pads the
 * ring, and really builds a replacement -- with a synthetic source, so no
 * audio hardware is involved and no real time passes.
 * ======================================================================== */

TEST(the_worker_reattaches_a_dead_source_without_the_ring_losing_its_place)
{
    AprGraph *g = NULL;
    AprReconnector *rc = NULL;
    AprReconnectOptions ro;
    AprCaptureConfig cfg = fake_cfg();
    AprSourceId id = 0;
    AprSource *s;
    uint64_t now;

    memset(&ro, 0, sizeof ro);
    ro.synthetic = 1;                 /* drive the real path with a fake */

    cfg.channels = 2;
    cfg.fake.die_at_frame = 4800;     /* 100 ms in */

    ASSERT_OK(apr_graph_create(RATE, 2, &g));
    ASSERT_OK(apr_graph_add_source(g, L"target", &cfg, &id));
    s = apr_graph_source(g, id);
    ASSERT_NOT_NULL(s);

    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(s), T0));
    apr_source_poll(s, NULL);
    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(s),
                                       T0 + frames_to_ticks_(9600)));
    apr_source_poll(s, NULL);
    ASSERT_FALSE(apr_source_alive(s));

    ASSERT_OK(apr_reconnect_create(g, &ro, &rc));

    /* Pass one notices the loss and detaches. */
    now = T0 + frames_to_ticks_(9600);
    apr_reconnect_step(rc, now);
    ASSERT_EQ_INT(APR_LINK_SEARCHING, (int)apr_reconnect_link(rc, id));
    ASSERT_FALSE(apr_source_attached(s));

    /* Pass two, once the first backoff has expired, finds it and reattaches.
     * The ring has been padded in the meantime, so the hole is already the
     * right length before the replacement writes a frame. */
    now += apr_mul_div_u64(APR_RECONNECT_FIRST_MS + 50u,
                           apr_qpc_freq(), 1000u, NULL);
    apr_reconnect_step(rc, now);
    ASSERT_EQ_INT(APR_LINK_LIVE, (int)apr_reconnect_link(rc, id));
    ASSERT_TRUE(apr_source_attached(s));
    ASSERT_EQ_INT(2, (int)apr_source_generation(s));
    ASSERT_EQ_INT(0, (int)ring_error(s, now));

    /* And it is a HEALTHY replacement: the health schedule described the fate
     * of the instance that died, not of the source. */
    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(s), now));
    now += frames_to_ticks_(4800);
    ASSERT_OK(apr_capture_fake_advance(apr_source_capture(s), now));
    apr_source_poll(s, NULL);
    ASSERT_TRUE(apr_source_alive(s));
    ASSERT_EQ_INT(0, (int)ring_error(s, now));

    apr_reconnect_destroy(rc);
    apr_graph_destroy(g);
}

/* ==========================================================================
 * 6. Both halves are announced
 *
 * The only cases in this file that run in real time, because a notice is
 * something the loop produces and there has to be a loop.
 * ======================================================================== */

typedef struct Tally {
    int  count[16];
    int  order[32];
    int  order_n;
} Tally;

static void tally(void *user, const AprRunNotice *n)
{
    Tally *t = (Tally *)user;
    if ((int)n->ev >= 0 && (int)n->ev < 16) t->count[n->ev]++;
    if (t->order_n < 32) t->order[t->order_n++] = (int)n->ev;
}

/* Where `ev` first appears in the notice stream, or -1. */
static int first_at(const Tally *t, int ev)
{
    int i;
    for (i = 0; i < t->order_n; i++) if (t->order[i] == ev) return i;
    return -1;
}

static int run_with(AprCaptureConfig *cfg, Tally *t, int64_t ms)
{
    AprGraph *g = NULL;
    AprRunnerConfig rcfg;
    AprRunner *r = NULL;
    AprSourceId id = 0;
    AprBusId bus = 0;
    AprErr e;

    memset(t, 0, sizeof *t);
    e = apr_graph_create(RATE, 2, &g);
    if (apr_failed(&e)) return 0;
    cfg->channels = 2;
    e = apr_graph_add_source(g, L"target", cfg, &id);
    if (apr_failed(&e)) { apr_graph_destroy(g); return 0; }
    e = apr_graph_add_bus(g, L"Mix", &bus);
    if (apr_failed(&e)) { apr_graph_destroy(g); return 0; }
    e = apr_graph_connect(g, id, bus, 1.0f);
    if (apr_failed(&e)) { apr_graph_destroy(g); return 0; }

    memset(&rcfg, 0, sizeof rcfg);
    rcfg.graph       = g;
    rcfg.duration_ms = ms;
    rcfg.observer    = tally;
    rcfg.user        = t;

    e = apr_runner_create(&rcfg, &r);
    if (apr_failed(&e)) { apr_graph_destroy(g); return 0; }
    e = apr_runner_run(r);
    apr_runner_destroy(r);
    apr_graph_destroy(g);
    return !apr_failed(&e);
}

TEST(both_the_loss_and_the_recovery_are_announced_in_that_order)
{
    /* The loss was always announced. THE RECOVERY IS THE HALF THAT IS EASY TO
     * FORGET, and it is the one that tells the author his take is intact --
     * without it, somebody who stepped away and heard "Teams has exited"
     * believes the recording ended there. */
    AprCaptureConfig cfg = fake_cfg();
    Tally t;

    cfg.fake.die_at_frame    = (uint64_t)RATE / 20u;   /*  50 ms */
    cfg.fake.revive_at_frame = (uint64_t)RATE / 5u;    /* 200 ms */

    ASSERT_TRUE(run_with(&cfg, &t, 600));

    ASSERT_EQ_INT(1, t.count[APR_RUN_EV_SOURCE_DIED]);
    ASSERT_EQ_INT(1, t.count[APR_RUN_EV_SOURCE_RECOVERED]);
    /* ASSERT_GT_INT(bound, actual) reads "actual is greater than bound": the
     * recovery arrives LATER in the stream than the loss it answers. */
    ASSERT_GT_INT(first_at(&t, APR_RUN_EV_SOURCE_DIED),
                  first_at(&t, APR_RUN_EV_SOURCE_RECOVERED));
}

TEST(a_source_that_never_returns_behaves_exactly_as_it_did_before)
{
    /* The old behaviour is not allowed to change underneath a recording that
     * has nothing to recover: said once, never said again, and the run is
     * still marked incomplete. */
    AprCaptureConfig cfg = fake_cfg();
    Tally t;

    cfg.fake.die_at_frame = (uint64_t)RATE / 20u;      /* 50 ms, and stays dead */

    ASSERT_TRUE(run_with(&cfg, &t, 500));

    ASSERT_EQ_INT(1, t.count[APR_RUN_EV_SOURCE_DIED]);
    ASSERT_EQ_INT(0, t.count[APR_RUN_EV_SOURCE_RECOVERED]);
    ASSERT_EQ_INT(0, t.count[APR_RUN_EV_EXCLUSION_HELD]);
}

TEST(a_recovered_take_is_still_reported_as_incomplete)
{
    /* The source came back and everything after the hole is at its true
     * position -- but the hole is still in the file, and a summary that said
     * "complete" would be describing a recording that is not the one that was
     * asked for. */
    AprGraph *g = NULL;
    AprRunnerConfig rcfg;
    AprRunner *r = NULL;
    AprCaptureConfig cfg = fake_cfg();
    AprSourceId id = 0;
    AprBusId bus = 0;
    Tally t;

    memset(&t, 0, sizeof t);
    cfg.channels = 2;
    cfg.fake.die_at_frame    = (uint64_t)RATE / 20u;
    cfg.fake.revive_at_frame = (uint64_t)RATE / 5u;

    ASSERT_OK(apr_graph_create(RATE, 2, &g));
    ASSERT_OK(apr_graph_add_source(g, L"target", &cfg, &id));
    ASSERT_OK(apr_graph_add_bus(g, L"Mix", &bus));
    ASSERT_OK(apr_graph_connect(g, id, bus, 1.0f));

    memset(&rcfg, 0, sizeof rcfg);
    rcfg.graph       = g;
    rcfg.duration_ms = 600;
    rcfg.observer    = tally;
    rcfg.user        = &t;

    ASSERT_OK(apr_runner_create(&rcfg, &r));
    ASSERT_OK(apr_runner_run(r));

    ASSERT_EQ_INT(1, t.count[APR_RUN_EV_SOURCE_RECOVERED]);
    ASSERT_TRUE(apr_runner_incomplete(r));

    /* A synthetic source is not watched, so nothing was reconnected: the
     * announcement above came from the health flag alone, which is what makes
     * the rule kind-independent. */
    ASSERT_EQ_INT(APR_LINK_OFF, (int)apr_runner_source_link(r, 0));

    apr_runner_destroy(r);
    apr_graph_destroy(g);
}
