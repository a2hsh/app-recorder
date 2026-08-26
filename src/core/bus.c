/*
 * bus.c -- the mixer tick. See include/bus.h for why the bus deliberately runs
 * behind wall clock and what that buys.
 */
#include "bus.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "mix.h"
#include "outpath.h"

typedef struct BusEdge {
    AprSource       *src;
    AprSourceReader *rd;
    float            gain;
} BusEdge;

/* ONE OUTPUT, AS A SPEC. There is no open encoder here between recordings --
 * see the lifetime note in bus.h for the data loss that taught us that.
 *
 * `state` is non-NULL only between apr_bus_start() and apr_bus_stop(). */
typedef struct BusAction {
    const AprActionVTable *vt;

    /* The spec. Copied, not borrowed: AprActionConfig::out_path is valid only
     * for the length of the add_action() call, and this has to answer "what
     * does this output write" for as long as the graph lives -- to a session
     * save an hour later, and to every recording after this one. */
    wchar_t path[APR_OUT_PATH_CCH];      /* the template, as the user gave it */
    int     bitrate_kbps;
    int     quality;

    /* The recording in progress, or the last one. */
    void   *state;
    int     failed;
    int     finalized;
    AprErr  err;
    wchar_t current[APR_OUT_PATH_CCH];   /* what the template resolved to */
    int     renamed;                     /* the asked-for name was taken */
} BusAction;

/* An action with no extension writes no file -- the built-in "none" sink is
 * the case, and it is what lets a bus be metered or tested with nothing on
 * disk. Everything path-shaped below is skipped for one. */
static int action_writes_a_file(const AprActionVTable *vt)
{
    return vt && vt->extension && vt->extension[0];
}

struct AprBus {
    AprBusId  id;
    wchar_t   name[APR_NAME_CCH];
    uint32_t  rate;
    uint16_t  channels;

    BusEdge   edges[APR_MAX_SOURCES_PER_BUS];
    size_t    edge_count;

    BusAction actions[APR_MAX_ACTIONS_PER_BUS];
    size_t    action_count;

    AprClock  clock;
    uint64_t  lookbehind_ticks;
    double    lookbehind_frames;
    int       running;
    uint64_t  frames_out;
    float     peak;

    float    *acc;      /* APR_BUS_BLOCK_FRAMES x channels */
    float    *srcbuf;   /* APR_BUS_BLOCK_FRAMES x APR_MAX_CHANNELS */
};

static void copy_name(wchar_t *dst, const wchar_t *src)
{
    size_t i = 0;
    if (src) for (; i + 1 < APR_NAME_CCH && src[i]; i++) dst[i] = src[i];
    dst[i] = L'\0';
}

/* ---------------------------------------------------------------------------
 * Lifetime
 * ------------------------------------------------------------------------- */

AprErr apr_bus_create(AprBusId id, const wchar_t *name,
                      uint32_t sample_rate, uint16_t channels, AprBus **out)
{
    AprBus *b;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"bus with no out slot");
    *out = NULL;
    if (sample_rate == 0 || channels == 0 || channels > APR_MAX_CHANNELS) {
        return APR_ERR(APR_E_INVALID_ARG, L"bus at %u Hz / %u channels",
                       sample_rate, channels);
    }

    b = (AprBus *)calloc(1, sizeof *b);
    if (!b) return APR_ERR(APR_E_NO_MEMORY, L"bus state");

    b->id       = id;
    b->rate     = sample_rate;
    b->channels = channels;
    copy_name(b->name, name);
    apr_clock_init(&b->clock, apr_qpc_freq(), sample_rate);

    b->lookbehind_frames = (double)sample_rate * APR_BUS_LOOKBEHIND_MS / 1000.0;
    b->lookbehind_ticks  = apr_frames_to_ticks((uint64_t)b->lookbehind_frames,
                                               b->clock.qpc_freq, sample_rate);

    b->acc    = (float *)calloc(APR_BUS_BLOCK_FRAMES * channels, sizeof(float));
    b->srcbuf = (float *)calloc(APR_BUS_BLOCK_FRAMES * APR_MAX_CHANNELS, sizeof(float));
    if (!b->acc || !b->srcbuf) {
        free(b->acc); free(b->srcbuf); free(b);
        return APR_ERR(APR_E_NO_MEMORY, L"bus mixing scratch");
    }

    *out = b;
    return apr_ok();
}

void apr_bus_destroy(AprBus *b)
{
    size_t i;

    if (!b) return;
    /* Finalizes and destroys any recording still open. Nothing should be left
     * for the loop below now that an action's life ends with its recording;
     * it stays as a belt, not because a state is expected to be found. */
    apr_bus_stop(b);
    for (i = 0; i < b->action_count; i++) {
        if (b->actions[i].vt && b->actions[i].state) {
            b->actions[i].vt->destroy(b->actions[i].state);
            b->actions[i].state = NULL;
        }
    }
    for (i = 0; i < b->edge_count; i++) apr_source_reader_close(b->edges[i].rd);
    free(b->acc);
    free(b->srcbuf);
    free(b);
}

AprBusId       apr_bus_id(const AprBus *b)       { return b ? b->id : 0; }
const wchar_t *apr_bus_name(const AprBus *b)     { return b ? b->name : L""; }


int apr_bus_set_name(AprBus *b, const wchar_t *name)
{
    if (!b || !name || !name[0]) return 0;
    copy_name(b->name, name);
    return 1;
}

uint32_t       apr_bus_rate(const AprBus *b)     { return b ? b->rate : 0; }
uint16_t       apr_bus_channels(const AprBus *b) { return b ? b->channels : 0; }
int            apr_bus_running(const AprBus *b)  { return b ? b->running : 0; }
uint64_t apr_bus_frames_out(const AprBus *b)     { return b ? b->frames_out : 0; }
float    apr_bus_peak(const AprBus *b)           { return b ? b->peak : 0.0f; }
const AprClock *apr_bus_clock(const AprBus *b)   { return b ? &b->clock : NULL; }
double apr_bus_lookbehind_frames(const AprBus *b) { return b ? b->lookbehind_frames : 0.0; }

/* ---------------------------------------------------------------------------
 * Edges
 * ------------------------------------------------------------------------- */

/* A GAIN THAT IS NOT A FINITE NUMBER IS REFUSED, and infinity is the half
 * that used to get through.
 *
 * NaN was already caught here, and NaN is the mild case: core/mix.c scrubs it
 * on the way into every integer format, so a NaN gain renders as silence --
 * wrong, but quiet. +-Inf does not scrub to silence. Inf * anything nonzero is
 * Inf, mix.c clamps it, and the file comes out as SUSTAINED DIGITAL FULL
 * SCALE, for as long as the recording runs, on a machine whose owner is
 * wearing headphones and cannot see a meter. AGENTS.md rule 1 makes that the
 * worse failure by a distance, so the check that caught the tidier one now
 * catches both -- and in one function, so the two call sites cannot drift
 * apart again.
 *
 * isfinite() is C99 and covers both halves in one test; hand-rolling it out of
 * comparisons against FLT_MAX invites a constant that folds to infinity and a
 * /W4 overflow warning. */
static int gain_is_finite(float g)
{
    return isfinite((double)g) != 0;
}

static size_t find_edge(const AprBus *b, AprSourceId id)
{
    size_t i;
    for (i = 0; i < b->edge_count; i++) {
        if (apr_source_id(b->edges[i].src) == id) return i;
    }
    return (size_t)-1;
}

AprErr apr_bus_add_source(AprBus *b, AprSource *s, float gain)
{
    AprSourceReader *rd = NULL;
    AprErr           e;

    if (!b || !s) return APR_ERR(APR_E_INVALID_ARG, L"bus or source is null");
    if (b->running) {
        return APR_ERR(APR_E_BUSY,
                       L"a source cannot be added to bus %u while it is "
                       L"recording", b->id);
    }
    if (!gain_is_finite(gain))
        return APR_ERR(APR_E_INVALID_ARG, L"gain is not a finite number");
    if (apr_source_rate(s) != b->rate) {
        /* Sources are opened at the session rate; a mismatch means the graph
         * was built wrong, and silently resampling it would hide that. */
        return APR_ERR(APR_E_UNSUPPORTED, L"source %u is %u Hz, bus is %u Hz",
                       apr_source_id(s), apr_source_rate(s), b->rate);
    }
    if (find_edge(b, apr_source_id(s)) != (size_t)-1) {
        return APR_ERR(APR_E_STATE, L"source %u already feeds bus %u",
                       apr_source_id(s), b->id);
    }
    if (b->edge_count >= APR_MAX_SOURCES_PER_BUS) {
        return APR_ERR(APR_E_STATE, L"bus %u already has %d sources",
                       b->id, APR_MAX_SOURCES_PER_BUS);
    }

    /* Target backlog IS the lookbehind: see the header. */
    e = apr_source_reader_open(s, b->lookbehind_frames, &rd);
    if (apr_failed(&e)) return e;

    b->edges[b->edge_count].src  = s;
    b->edges[b->edge_count].rd   = rd;
    b->edges[b->edge_count].gain = gain;
    b->edge_count++;
    return apr_ok();
}

AprErr apr_bus_remove_source(AprBus *b, AprSourceId id)
{
    size_t i;

    if (!b) return APR_ERR(APR_E_INVALID_ARG, L"null bus");
    if (b->running) {
        return APR_ERR(APR_E_BUSY,
                       L"a source cannot be removed from bus %u while it is "
                       L"recording", b->id);
    }
    i = find_edge(b, id);
    if (i == (size_t)-1) {
        return APR_ERR(APR_E_NOT_FOUND, L"source %u does not feed bus %u", id, b->id);
    }
    apr_source_reader_close(b->edges[i].rd);   /* lowers the source's refcount */
    for (; i + 1 < b->edge_count; i++) b->edges[i] = b->edges[i + 1];
    b->edge_count--;
    return apr_ok();
}

int apr_bus_has_source(const AprBus *b, AprSourceId id)
{
    return b && find_edge(b, id) != (size_t)-1;
}

size_t apr_bus_source_count(const AprBus *b) { return b ? b->edge_count : 0; }

AprSource *apr_bus_source_at(const AprBus *b, size_t index)
{
    return (b && index < b->edge_count) ? b->edges[index].src : NULL;
}

AprSourceReader *apr_bus_reader_at(const AprBus *b, size_t index)
{
    return (b && index < b->edge_count) ? b->edges[index].rd : NULL;
}

AprErr apr_bus_set_gain(AprBus *b, AprSourceId id, float gain)
{
    size_t i;

    if (!b) return APR_ERR(APR_E_INVALID_ARG, L"null bus");
    /* See gain_is_finite: NaN renders as silence, infinity renders as
     * sustained full scale, and only one of the two used to be refused. */
    if (!gain_is_finite(gain))
        return APR_ERR(APR_E_INVALID_ARG, L"gain is not a finite number");
    /* The mixer reads this array on the runner's thread; the canvas writes it
     * on the UI thread. A float store is not the hazard -- rewriting a gain
     * the user cannot then hear applied, mid-take, is. Refuse and say so
     * (graph.h). */
    if (b->running) {
        return APR_ERR(APR_E_BUSY,
                       L"the gain on bus %u cannot be changed while it is "
                       L"recording", b->id);
    }
    i = find_edge(b, id);
    if (i == (size_t)-1) {
        return APR_ERR(APR_E_NOT_FOUND, L"source %u does not feed bus %u", id, b->id);
    }
    b->edges[i].gain = gain;
    return apr_ok();
}

float apr_bus_gain(const AprBus *b, AprSourceId id)
{
    size_t i;
    if (!b) return 0.0f;
    i = find_edge(b, id);
    return i == (size_t)-1 ? 0.0f : b->edges[i].gain;
}

/* ---------------------------------------------------------------------------
 * Actions
 * ------------------------------------------------------------------------- */

/* The context every template on this bus expands against. */
static AprOutContext out_ctx(const AprBus *b, const AprActionVTable *vt)
{
    AprOutContext c;
    c.bus_name  = b->name;
    c.extension = vt ? vt->extension : NULL;
    return c;
}

/* NOTHING IS CREATED HERE. See the lifetime note in bus.h: the file is opened
 * at apr_bus_start(). What DOES happen here is the check that used to be a
 * side effect of creating it -- an unwritable folder is refused now, while the
 * person who typed the name is still standing there. */
AprErr apr_bus_add_action(AprBus *b, const AprActionVTable *vt,
                          const AprActionConfig *cfg)
{
    BusAction *a;

    if (!b || !vt || !cfg) return APR_ERR(APR_E_INVALID_ARG, L"bus, action or config is null");
    if (b->action_count >= APR_MAX_ACTIONS_PER_BUS) {
        return APR_ERR(APR_E_STATE, L"bus %u already has %d actions",
                       b->id, APR_MAX_ACTIONS_PER_BUS);
    }
    if (b->running) {
        /* Adding an output halfway through would produce a file that starts in
         * the middle of the session and lines up with nothing. */
        return APR_ERR(APR_E_STATE, L"bus %u is recording", b->id);
    }

    if (action_writes_a_file(vt)) {
        AprOutContext ctx = out_ctx(b, vt);
        AprErr        e;

        if (!cfg->out_path || !cfg->out_path[0]) {
            return APR_ERR(APR_E_INVALID_ARG, L"the \"%hs\" output needs a name",
                           vt->id ? vt->id : "(null)");
        }
        if (wcslen(cfg->out_path) + 1 > APR_OUT_PATH_CCH) {
            return APR_ERR(APR_E_INVALID_ARG,
                           L"that name is longer than %d characters",
                           APR_OUT_PATH_CCH - 1);
        }
        /* THE EARLY CHECK. Expands the template and asks the folder whether
         * something may be created in it; creates nothing that was not there
         * and leaves nothing behind (outpath.h). */
        e = apr_out_validate(cfg->out_path, &ctx, NULL, 0);
        if (apr_failed(&e)) return e;
    }

    a = &b->actions[b->action_count];
    memset(a, 0, sizeof *a);
    a->vt           = vt;
    a->bitrate_kbps = cfg->bitrate_kbps;
    a->quality      = cfg->quality;
    a->err          = apr_ok();
    if (cfg->out_path) {
        size_t k = 0;
        for (; k + 1 < APR_OUT_PATH_CCH && cfg->out_path[k]; k++) {
            a->path[k] = cfg->out_path[k];
        }
        a->path[k] = L'\0';
    }
    b->action_count++;
    return apr_ok();
}

/* Finalize and destroy whatever recording this action has open. Idempotent,
 * and it is the ONLY place an action's state is torn down, so there is exactly
 * one order for it: finalize (a half-written file must still open), then
 * destroy, then forget. Returns the finalize's result. */
static AprErr close_action(BusAction *a)
{
    AprErr e = apr_ok();

    if (!a->vt || !a->state) return e;

    if (!a->finalized) {
        e = a->vt->finalize(a->state);
        a->finalized = 1;
    }
    a->vt->destroy(a->state);
    a->state = NULL;
    return e;
}

/* IF SOMETHING IS OPEN, FINALIZE IT FIRST -- with no way to skip that step. An
 * encoder detached without finalizing leaves a file with no index, no trailing
 * sizes and, for some formats, nothing that opens.
 *
 * Between recordings there is nothing open at all now, so removing an output
 * from an idle graph touches no file: it forgets a plan, which is what it
 * always looked like from the outside. See the header. */
AprErr apr_bus_remove_action(AprBus *b, size_t index)
{
    AprErr e;
    size_t i;

    if (!b) return APR_ERR(APR_E_INVALID_ARG, L"apr_bus_remove_action: bus is null");
    if (index >= b->action_count) {
        return APR_ERR(APR_E_NOT_FOUND, L"bus %u has no action %zu", b->id, index);
    }

    e = close_action(&b->actions[index]);

    for (i = index; i + 1 < b->action_count; i++) {
        b->actions[i] = b->actions[i + 1];
    }
    b->action_count--;
    memset(&b->actions[b->action_count], 0, sizeof b->actions[0]);

    /* The finalize's result is returned rather than swallowed: the output IS
     * gone either way, and a caller that wants to say "the file was closed but
     * the last write failed" needs to be told. */
    return e;
}

size_t apr_bus_action_count(const AprBus *b) { return b ? b->action_count : 0; }

const AprActionVTable *apr_bus_action_at(const AprBus *b, size_t index)
{
    return (b && index < b->action_count) ? b->actions[index].vt : NULL;
}

const wchar_t *apr_bus_action_path(const AprBus *b, size_t index)
{
    return (b && index < b->action_count) ? b->actions[index].path : L"";
}

const wchar_t *apr_bus_action_current_path(const AprBus *b, size_t index)
{
    return (b && index < b->action_count) ? b->actions[index].current : L"";
}

int apr_bus_action_renamed(const AprBus *b, size_t index)
{
    return (b && index < b->action_count) ? b->actions[index].renamed : 0;
}

int apr_bus_action_bitrate(const AprBus *b, size_t index)
{
    return (b && index < b->action_count) ? b->actions[index].bitrate_kbps : 0;
}

int apr_bus_action_quality(const AprBus *b, size_t index)
{
    return (b && index < b->action_count) ? b->actions[index].quality : 0;
}

int apr_bus_action_failed(const AprBus *b, size_t index)
{
    return (b && index < b->action_count) ? b->actions[index].failed : 0;
}

AprErr apr_bus_action_error(const AprBus *b, size_t index)
{
    return (b && index < b->action_count) ? b->actions[index].err : apr_ok();
}

/* ---------------------------------------------------------------------------
 * The tick
 * ------------------------------------------------------------------------- */

static void render(AprBus *b, uint64_t bus_frame, size_t n, uint64_t now_ticks)
{
    size_t i;

    apr_mix_silence(b->acc, n, b->channels);

    for (i = 0; i < b->edge_count; i++) {
        AprSourcePull p;
        AprMixSpan    span;
        uint16_t      sch = apr_source_channels(b->edges[i].src);

        p = apr_source_pull(b->edges[i].rd, &b->clock, bus_frame, now_ticks,
                            b->srcbuf, n);
        if (p.frames == 0) continue;

        span.pcm      = b->srcbuf + p.lead * sch;
        span.frames   = p.frames;
        span.channels = sch;
        span.offset   = p.lead;
        span.gain     = b->edges[i].gain;
        apr_mix_add(b->acc, n, b->channels, &span);
    }

    b->peak = apr_mix_peak(b->acc, n, b->channels);

    for (i = 0; i < b->action_count; i++) {
        BusAction *a = &b->actions[i];
        AprErr     e;

        if (a->failed || a->finalized) continue;
        e = a->vt->on_audio(a->state, b->acc, n,
                            apr_clock_frame_ticks(&b->clock, bus_frame));
        if (apr_failed(&e)) {
            /* One broken encoder must not cost the session (design 10). Drop
             * it from the fan-out; its finalize still runs on stop. */
            a->failed = 1;
            a->err    = e;
            APR_LOG_ERR(APR_LOG_ERROR, &e);
        }
    }
}

/* Open one output for the recording that is about to start: resolve the name,
 * then create the encoder. The action's whole life is between here and
 * apr_bus_stop(). */
static void open_action(AprBus *b, BusAction *a)
{
    AprActionConfig cfg;
    AprErr          e;

    a->failed     = 0;
    a->finalized  = 0;
    a->err        = apr_ok();
    a->state      = NULL;
    a->current[0] = L'\0';
    a->renamed    = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.sample_rate  = b->rate;
    cfg.channels     = b->channels;
    cfg.bitrate_kbps = a->bitrate_kbps;
    cfg.quality      = a->quality;

    if (action_writes_a_file(a->vt)) {
        AprOutContext ctx = out_ctx(b, a->vt);

        /* THE COLLISION POLICY LIVES IN outpath.c AND IS APPLIED HERE, at the
         * only instant that can know whether the name is taken: now. The take
         * already on disk is never overwritten. */
        e = apr_out_resolve(a->path, &ctx, a->current, APR_OUT_PATH_CCH,
                            &a->renamed);
        if (apr_failed(&e)) {
            a->failed = 1;
            a->err    = e;
            APR_LOG_ERR(APR_LOG_ERROR, &e);
            return;
        }
        cfg.out_path = a->current;
    }

    e = a->vt->create(&cfg, &a->state);
    if (apr_failed(&e)) {
        /* One output that will not open must not cost the session (design 10),
         * so it is marked and skipped exactly as one that fails mid-recording
         * is. apr_bus_action_error() carries the reason, and the runner says it
         * out loud at the start rather than at the end. */
        a->state  = NULL;
        a->failed = 1;
        a->err    = e;
        APR_LOG_ERR(APR_LOG_ERROR, &e);
    }
}

AprErr apr_bus_start(AprBus *b, uint64_t start_ticks)
{
    size_t i;

    if (!b) return APR_ERR(APR_E_INVALID_ARG, L"null bus");
    if (b->running) return apr_ok();

    /* EVERY ACTION IS CREATED HERE, NOT WHEN IT WAS ADDED. That is the whole
     * of the lifetime fix -- see bus.h. */
    for (i = 0; i < b->action_count; i++) open_action(b, &b->actions[i]);

    apr_clock_anchor(&b->clock, start_ticks);
    b->frames_out = 0;
    b->running    = 1;
    return apr_ok();
}

AprErr apr_bus_tick(AprBus *b, uint64_t now_ticks)
{
    int64_t  due;
    uint64_t target;

    if (!b) return APR_ERR(APR_E_INVALID_ARG, L"null bus");
    if (!b->running) return APR_ERR(APR_E_STATE, L"bus %u is not running", b->id);

    /* Frames due at (now - lookbehind), from an ABSOLUTE timestamp. Ticks are
     * never counted: a late or early tick changes the block size and nothing
     * else. */
    if (now_ticks < b->lookbehind_ticks) return apr_ok();
    due = apr_clock_expected_frames(&b->clock, now_ticks - b->lookbehind_ticks);
    if (due <= 0) return apr_ok();

    target = (uint64_t)due;
    while (b->frames_out < target) {
        uint64_t remain = target - b->frames_out;
        size_t   n = remain > APR_BUS_BLOCK_FRAMES ? APR_BUS_BLOCK_FRAMES : (size_t)remain;

        render(b, b->frames_out, n, now_ticks);
        b->frames_out += n;
    }
    return apr_ok();
}

AprErr apr_bus_stop(AprBus *b)
{
    AprErr first = apr_ok();
    size_t i;

    if (!b) return APR_ERR(APR_E_INVALID_ARG, L"null bus");

    /* EVERY action is finalized on EVERY exit path, failed ones included: a
     * half-written recording must still open in a player (design 10). Then it
     * is DESTROYED, because the encoder's life is this recording and the next
     * apr_bus_start() makes a fresh one.
     *
     * The failed flags are NOT cleared here -- apr_bus_start() clears them --
     * so a caller can still ask, after the fact, which output went wrong. */
    for (i = 0; i < b->action_count; i++) {
        BusAction *a = &b->actions[i];
        AprErr     e = close_action(a);

        if (apr_failed(&e)) {
            APR_LOG_ERR(APR_LOG_ERROR, &e);
            if (!apr_failed(&first)) first = e;
            if (!a->failed) { a->failed = 1; a->err = e; }
        }
    }
    b->running = 0;
    return first;
}
