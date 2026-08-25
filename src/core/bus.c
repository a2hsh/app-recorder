/*
 * bus.c -- the mixer tick. See include/bus.h for why the bus deliberately runs
 * behind wall clock and what that buys.
 */
#include "bus.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "mix.h"

typedef struct BusEdge {
    AprSource       *src;
    AprSourceReader *rd;
    float            gain;
} BusEdge;

typedef struct BusAction {
    const AprActionVTable *vt;
    void                  *state;
    int                    failed;
    int                    finalized;
    AprErr                 err;
} BusAction;

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
    apr_bus_stop(b);
    for (i = 0; i < b->action_count; i++) {
        if (b->actions[i].vt && b->actions[i].state) {
            b->actions[i].vt->destroy(b->actions[i].state);
        }
    }
    for (i = 0; i < b->edge_count; i++) apr_source_reader_close(b->edges[i].rd);
    free(b->acc);
    free(b->srcbuf);
    free(b);
}

AprBusId       apr_bus_id(const AprBus *b)       { return b ? b->id : 0; }
const wchar_t *apr_bus_name(const AprBus *b)     { return b ? b->name : L""; }
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
    if (!(gain == gain)) return APR_ERR(APR_E_INVALID_ARG, L"gain is not a number");
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
    /* A NaN gain would turn the whole bus into NaN, and every integer encoder
     * downstream would render that as silence rather than audio. Refuse it
     * here, where there is still a caller to tell. */
    if (!(gain == gain)) return APR_ERR(APR_E_INVALID_ARG, L"gain is not a number");
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

AprErr apr_bus_add_action(AprBus *b, const AprActionVTable *vt,
                          const AprActionConfig *cfg)
{
    void  *state = NULL;
    AprErr e;

    if (!b || !vt || !cfg) return APR_ERR(APR_E_INVALID_ARG, L"bus, action or config is null");
    if (b->action_count >= APR_MAX_ACTIONS_PER_BUS) {
        return APR_ERR(APR_E_STATE, L"bus %u already has %d actions",
                       b->id, APR_MAX_ACTIONS_PER_BUS);
    }

    e = vt->create(cfg, &state);
    if (apr_failed(&e)) return e;

    b->actions[b->action_count].vt        = vt;
    b->actions[b->action_count].state     = state;
    b->actions[b->action_count].failed    = 0;
    b->actions[b->action_count].finalized = 0;
    b->actions[b->action_count].err       = apr_ok();
    b->action_count++;
    return apr_ok();
}

size_t apr_bus_action_count(const AprBus *b) { return b ? b->action_count : 0; }

const AprActionVTable *apr_bus_action_at(const AprBus *b, size_t index)
{
    return (b && index < b->action_count) ? b->actions[index].vt : NULL;
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

AprErr apr_bus_start(AprBus *b, uint64_t start_ticks)
{
    if (!b) return APR_ERR(APR_E_INVALID_ARG, L"null bus");
    if (b->running) return apr_ok();
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
     * half-written recording must still open in a player (design 10). */
    for (i = 0; i < b->action_count; i++) {
        BusAction *a = &b->actions[i];
        AprErr     e;

        if (a->finalized || !a->vt || !a->state) continue;
        e = a->vt->finalize(a->state);
        a->finalized = 1;
        if (apr_failed(&e)) {
            APR_LOG_ERR(APR_LOG_ERROR, &e);
            if (!apr_failed(&first)) first = e;
            if (!a->failed) { a->failed = 1; a->err = e; }
        }
    }
    b->running = 0;
    return first;
}
