/*
 * source.c -- a capture, its ring, and one reader per consuming bus.
 * See include/source.h for the contract and for why the two kinds are not
 * symmetric.
 */
#include "source.h"

#include <stdlib.h>
#include <string.h>

#include "drift.h"
#include "log.h"
#include "mix.h"
#include "resample.h"

/* Input frames moved from the ring into the resampler per inner iteration.
 * Bounds the per-reader scratch; nothing about the result depends on it. */
#define APR_SOURCE_BLOCK 512u

/* A device crystal is never off by more than a few hundred ppm. Sizing the
 * kernel for 1% leaves three orders of magnitude of headroom and keeps the
 * filter support (and so the buffer) small. */
#define APR_SOURCE_MAX_RATIO 1.01

#define Q32_ONE 4294967296.0

/* Long enough for an endpoint id, which is a pair of GUIDs in braces. Stated
 * here rather than taken from discover.h so that source.h -- which every core
 * file includes -- does not acquire a dependency on the discovery layer for
 * one array bound. */
#define APR_DISC_ENDPOINT_CCH_LOCAL 256

struct AprSource {
    AprSourceId      id;
    wchar_t          name[APR_NAME_CCH];
    AprSourceKind    kind;
    uint32_t         rate;
    uint16_t         channels;

    RingBuf         *rb;
    AprCapture      *cap;
    AprClock         clock;
    AprCaptureStatus st;

    int              started;
    int              refcount;
    int              is_reference;

    /* What this source was asked to capture, kept so a session can be written
     * down later. The endpoint id is copied into `endpoint` and cfg.device
     * points at that copy, because the caller's string was borrowed for the
     * length of apr_source_create() only. */
    AprCaptureConfig cfg;
    wchar_t          endpoint[APR_DISC_ENDPOINT_CCH_LOCAL];
};

struct AprSourceReader {
    AprSource     *src;
    RingReader     rr;
    AprResampler  *rs;          /* NULL for a process tap: nothing to correct */
    AprDriftCtl    ctl;
    double         target;

    uint64_t       in_need;     /* absolute ring index of the next input frame */
    uint64_t       in_base;     /* ring index the resampler counts from */
    uint64_t       first_bus;   /* bus frame carrying this reader's output 0 */
    uint64_t       out_pos;     /* output frames placed so far */
    int            begun;

    float         *scratch;     /* APR_SOURCE_BLOCK frames x channels */
};

/* ---------------------------------------------------------------------------
 * Lifetime
 * ------------------------------------------------------------------------- */

static void copy_name(wchar_t *dst, const wchar_t *src)
{
    size_t i = 0;
    if (src) for (; i + 1 < APR_NAME_CCH && src[i]; i++) dst[i] = src[i];
    dst[i] = L'\0';
}

AprErr apr_source_create(AprSourceId id, const wchar_t *name,
                         const AprCaptureConfig *cfg, AprSource **out)
{
    AprSource *s;
    AprErr     e;
    size_t     cap_frames;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"source with no out slot");
    *out = NULL;
    if (!cfg) return APR_ERR(APR_E_INVALID_ARG, L"source with no capture config");
    if (cfg->sample_rate == 0 || cfg->channels == 0 || cfg->channels > APR_MAX_CHANNELS) {
        return APR_ERR(APR_E_INVALID_ARG, L"source at %u Hz / %u channels",
                       cfg->sample_rate, cfg->channels);
    }

    s = (AprSource *)calloc(1, sizeof *s);
    if (!s) return APR_ERR(APR_E_NO_MEMORY, L"source state");

    s->id       = id;
    s->kind     = cfg->kind;
    s->rate     = cfg->sample_rate;
    s->channels = cfg->channels;
    copy_name(s->name, name);
    apr_clock_init(&s->clock, apr_qpc_freq(), s->rate);
    s->st.alive      = 1;
    s->is_reference  = (cfg->kind == APR_SRC_PROCESS);

    /* Keep what we were asked to capture. The endpoint id in `cfg` is borrowed
     * for the length of this call, so it is COPIED and the retained config
     * points at the copy -- otherwise a session saved an hour later would
     * write down a dangling pointer's worth of nothing. */
    s->cfg = *cfg;
    if (cfg->kind == APR_SRC_DEVICE && cfg->device.endpoint_id) {
        size_t i = 0;
        for (; i + 1 < APR_DISC_ENDPOINT_CCH_LOCAL && cfg->device.endpoint_id[i]; i++) {
            s->endpoint[i] = cfg->device.endpoint_id[i];
        }
        s->endpoint[i] = L'\0';
        s->cfg.device.endpoint_id = s->endpoint;
    }

    /* 250 ms: mixer jitter only. Disk stalls are absorbed inside each action,
     * because a slow encoder must never back-pressure a ring every other bus
     * is reading (design 3.1). */
    cap_frames = (size_t)s->rate * APR_SOURCE_RING_MS / 1000u;
    e = rb_create(cap_frames, (size_t)s->channels * sizeof(float), &s->rb);
    if (apr_failed(&e)) { free(s); return e; }

    e = apr_capture_create(cfg, s->rb, &s->cap);
    if (apr_failed(&e)) {
        /* capture.h: a non-NULL capture after a FAILED create means the
         * half-open capture could not be retired and is still writing into
         * this ring. Leak both rather than free either. */
        if (s->cap) {
            APR_WARN(L"source %u leaked (ring included): its capture could "
                     L"not be retired after a failed open", id);
            return e;
        }
        rb_destroy(s->rb);
        free(s);
        return e;
    }

    *out = s;
    return apr_ok();
}

AprErr apr_source_destroy(AprSource *s)
{
    AprErr e;

    if (!s) return apr_ok();
    if (s->refcount != 0) {
        APR_WARN(L"source %u destroyed with %d reader(s) still open",
                 s->id, s->refcount);
    }

    /* THE ONE LINE THIS WHOLE MECHANISM EXISTS FOR. apr_capture_destroy makes
     * the bounded join and reports whether the capture thread actually left.
     * Until it says yes, that thread is still writing into s->rb, so the ring
     * is not ours to free and neither is anything holding it. See source.h. */
    e = apr_capture_destroy(s->cap);   /* implies stop */
    if (apr_failed(&e)) {
        APR_LOG_ERR(APR_LOG_ERROR, &e);
        APR_WARN(L"source %u leaked (ring included) rather than freed under a "
                 L"live capture thread", s->id);
        return e;
    }

    rb_destroy(s->rb);
    free(s);
    return apr_ok();
}

AprErr apr_source_start(AprSource *s)
{
    AprErr e;

    if (!s) return APR_ERR(APR_E_INVALID_ARG, L"start of a null source");
    if (s->started) return apr_ok();
    e = s->cap->vt->start(s->cap);
    if (apr_failed(&e)) return e;
    s->started = 1;
    return apr_ok();
}

void apr_source_stop(AprSource *s)
{
    if (!s || !s->started) return;
    s->cap->vt->stop(s->cap);
    s->started = 0;
}

/* ---------------------------------------------------------------------------
 * Identity and state
 * ------------------------------------------------------------------------- */

AprSourceId    apr_source_id(const AprSource *s)       { return s ? s->id : 0; }
AprSourceKind  apr_source_kind(const AprSource *s)     { return s ? s->kind : APR_SRC_FAKE; }
const wchar_t *apr_source_name(const AprSource *s)     { return s ? s->name : L""; }
uint32_t       apr_source_rate(const AprSource *s)     { return s ? s->rate : 0; }
uint16_t       apr_source_channels(const AprSource *s) { return s ? s->channels : 0; }
int            apr_source_refcount(const AprSource *s) { return s ? s->refcount : 0; }

const AprCaptureConfig *apr_source_config(const AprSource *s)
{
    return s ? &s->cfg : NULL;
}
RingBuf       *apr_source_ring(AprSource *s)           { return s ? s->rb : NULL; }
AprCapture    *apr_source_capture(AprSource *s)        { return s ? s->cap : NULL; }
int            apr_source_alive(const AprSource *s)    { return s ? s->st.alive : 0; }
int            apr_source_muted(const AprSource *s)    { return s ? s->st.muted : 0; }
int            apr_source_anchored(const AprSource *s) { return s ? s->clock.anchored : 0; }
const AprClock *apr_source_clock(const AprSource *s)   { return s ? &s->clock : NULL; }
uint64_t       apr_source_frames(const AprSource *s)   { return s ? rb_write_pos(s->rb) : 0; }

/* A process tap IS the reference timeline: gapless, 100% fill, locked to the
 * audio engine. Only a hardware crystal drifts (design 5.1). */
int apr_source_is_reference(const AprSource *s)
{
    return s && s->is_reference;
}

void apr_source_set_reference(AprSource *s, int is_reference)
{
    if (s) s->is_reference = is_reference ? 1 : 0;
}

void apr_source_poll(AprSource *s, AprCaptureStatus *out)
{
    if (!s) { if (out) memset(out, 0, sizeof *out); return; }

    s->cap->vt->status(s->cap, &s->st);
    if (!s->clock.anchored && s->st.anchor_ticks != 0) {
        apr_clock_anchor(&s->clock, s->st.anchor_ticks);
    }
    if (out) *out = s->st;
}

/* ---------------------------------------------------------------------------
 * Readers
 * ------------------------------------------------------------------------- */

AprErr apr_source_reader_open(AprSource *s, double target_backlog,
                              AprSourceReader **out)
{
    AprSourceReader *rd;
    AprErr           e;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"reader with no out slot");
    *out = NULL;
    if (!s) return APR_ERR(APR_E_INVALID_ARG, L"reader on a null source");
    if (s->refcount >= APR_MAX_READERS_PER_SOURCE) {
        return APR_ERR(APR_E_STATE, L"source %u already feeds %d buses",
                       s->id, s->refcount);
    }
    if (!(target_backlog >= 0.0)) target_backlog = 0.0;

    rd = (AprSourceReader *)calloc(1, sizeof *rd);
    if (!rd) return APR_ERR(APR_E_NO_MEMORY, L"source reader");

    rd->scratch = (float *)calloc(APR_SOURCE_BLOCK * s->channels, sizeof(float));
    if (!rd->scratch) { free(rd); return APR_ERR(APR_E_NO_MEMORY, L"reader scratch"); }

    rd->src    = s;
    rd->target = target_backlog;
    /* OLDEST, not LATEST: a bus joining a source that has been running for a
     * few ticks should get the audio the ring still holds, not a hole. */
    rb_reader_init(&rd->rr, s->rb, RB_START_OLDEST);

    if (!apr_source_is_reference(s)) {
        e = apr_resampler_create(s->channels, APR_SOURCE_MAX_RATIO, &rd->rs);
        if (apr_failed(&e)) { free(rd->scratch); free(rd); return e; }
        apr_drift_ctl_init(&rd->ctl, s->rate, target_backlog, 0.0);
    }

    s->refcount++;                 /* bus bookkeeping; the ring is not told */
    *out = rd;
    return apr_ok();
}

void apr_source_reader_close(AprSourceReader *rd)
{
    if (!rd) return;
    if (rd->src && rd->src->refcount > 0) rd->src->refcount--;
    apr_resampler_destroy(rd->rs);
    free(rd->scratch);
    free(rd);
}

/* Fill exactly `count` frames starting at absolute ring index rd->in_need,
 * substituting silence for anything the producer has already overwritten or
 * has not written yet, and advance in_need by exactly `count`.
 *
 * ALIGNMENT OVER CONTENT. When the producer has lapped us, the frames it
 * overwrote are gone; we emit that many zeros and carry on at the right
 * absolute index rather than sliding everything after the hole earlier. A
 * dropout is recoverable; a file three hours out of sync is not.
 *
 * Audio that rb_read had already copied when it discovered the lap is
 * discarded for the same reason: it belongs at an index beyond the end of the
 * block being filled, so there is nowhere correct to put it. That costs at
 * most one pull block of content on top of what was actually overwritten, and
 * only on a stall longer than the whole ring -- which is already a dropout.
 * test_sync.c pins that bound.
 *
 * Returns the frames replaced by silence because of overrun. */
static uint64_t pull_aligned(AprSourceReader *rd, float *dst, size_t count,
                             int *underrun)
{
    uint16_t ch    = rd->src->channels;
    size_t   place = 0;
    uint64_t lost  = 0;

    while (place < count) {
        uint64_t need = rd->in_need + place;
        uint64_t cur  = rb_reader_pos(&rd->rr);
        uint64_t skipped_lost = 0;
        size_t   n;

        if (cur < need) {                       /* deliberately skipping ahead */
            if (rb_skip(&rd->rr, (size_t)(need - cur), &skipped_lost) == 0 &&
                skipped_lost == 0) {
                break;                          /* nothing there yet */
            }
            continue;
        }
        if (cur > need) {                       /* those frames are gone */
            size_t gap = (size_t)(cur - need);
            if (gap > count - place) gap = count - place;
            memset(dst + place * ch, 0, gap * ch * sizeof(float));
            lost  += gap;
            place += gap;
            continue;
        }

        n = rb_read(&rd->rr, dst + place * ch, count - place, &skipped_lost);
        if (skipped_lost) continue;             /* lapped: next pass zero-fills */
        if (n == 0) break;                      /* underrun */
        place += n;
    }

    if (place < count) {
        memset(dst + place * ch, 0, (count - place) * ch * sizeof(float));
        if (underrun) *underrun = 1;
    }
    rd->in_need += count;
    return lost;
}

/* Decide where this reader's output frame 0 lands on the bus timeline, and
 * which ring frame it comes from. Returns 0 while the reader is not ready. */
static int begin(AprSourceReader *rd, const AprClock *bus_clock, uint64_t bus_frame)
{
    AprSource *s        = rd->src;
    uint64_t   base     = rb_reader_pos(&rd->rr);
    uint64_t   produced = rb_write_pos(s->rb);
    int64_t    f0, first;

    /* A device source starts reading only once its jitter buffer holds the
     * target, so the backlog begins AT the target instead of climbing to it.
     * Because the mixer runs exactly that far behind wall clock, this instant
     * is also the instant the bus reaches the source's true start -- so the
     * jitter buffer costs no alignment. A process tap needs none of this. */
    if (rd->rs && produced < base + (uint64_t)rd->target) return 0;

    f0    = apr_clock_expected_frames(bus_clock, s->clock.anchor_ticks);
    first = f0 + (int64_t)base;

    if (first < (int64_t)bus_frame) {
        /* The source has been running since before this bus reached it (a bus
         * added mid-session, or a slow first tick). Drop the stale head so
         * what remains still lands at its true absolute position. */
        base += (uint64_t)((int64_t)bus_frame - first);
        first = (int64_t)bus_frame;
    }

    rd->in_need   = base;
    rd->in_base   = base;
    rd->first_bus = (uint64_t)first;
    rd->out_pos   = 0;
    rd->begun     = 1;
    return 1;
}

static void update_ratio(AprSourceReader *rd, uint64_t now_ticks, uint64_t block)
{
    AprSource *s = rd->src;
    AprDrift   d;
    double     backlog;
    uint64_t   produced = rb_write_pos(s->rb);

    d = apr_clock_drift(&s->clock, now_ticks, produced);

    /* Exact: an integer producer cursor against a Q32.32 consumer position. */
    backlog = (double)produced - ((double)rd->in_base +
              (double)apr_resampler_in_pos_q32(rd->rs) / Q32_ONE);

    apr_resampler_set_ratio_q32(rd->rs,
        apr_drift_ctl_update(&rd->ctl, &d, backlog, block));
}

AprSourcePull apr_source_pull(AprSourceReader *rd, const AprClock *bus_clock,
                              uint64_t bus_frame, uint64_t now_ticks,
                              float *out, size_t frames)
{
    AprSourcePull p;
    AprSource    *s;
    uint16_t      ch;
    size_t        lead, want;
    int           under = 0;

    memset(&p, 0, sizeof p);
    if (!rd || !rd->src || !bus_clock || !out || frames == 0) return p;
    s  = rd->src;
    ch = s->channels;

    apr_source_poll(s, NULL);
    if (!s->clock.anchored)             { p.lead = frames; return p; }
    if (!rd->begun && !begin(rd, bus_clock, bus_frame)) { p.lead = frames; return p; }

    p.running = 1;
    if (rd->first_bus >= bus_frame + frames) { p.lead = frames; return p; }

    lead = rd->first_bus > bus_frame ? (size_t)(rd->first_bus - bus_frame) : 0;
    want = frames - lead;

    if (!rd->rs) {
        /* Process tap: the reference timeline. Straight out of the ring, no
         * resampler, no gap synthesis -- it arrives perfect, so it is treated
         * as perfect (design 5.1 step 3). */
        p.lost = pull_aligned(rd, out + lead * ch, want, &under);
    } else {
        size_t done = 0;

        while (done < want) {
            size_t need  = apr_resampler_input_needed(rd->rs, want - done);
            size_t space = apr_resampler_input_space(rd->rs);
            size_t k     = need < space ? need : space;
            size_t used  = 0;

            if (k > APR_SOURCE_BLOCK) k = APR_SOURCE_BLOCK;
            if (k) p.lost += pull_aligned(rd, rd->scratch, k, &under);

            done += apr_resample(rd->rs, rd->scratch, k, &used,
                                 out + (lead + done) * ch, want - done);
            if (k == 0 && used == 0) break;
        }
        if (done < want) {
            memset(out + (lead + done) * ch, 0, (want - done) * ch * sizeof(float));
            under = 1;
        }

        /* Consume first, THEN measure and retune. Measuring before consuming
         * leaves a standing one-block offset that reads as drift and is not. */
        update_ratio(rd, now_ticks, want);
    }

    rd->out_pos += want;
    p.lead       = lead;
    p.frames     = want;
    p.underrun   = under;
    return p;
}

/* ---------------------------------------------------------------------------
 * Reader diagnostics
 * ------------------------------------------------------------------------- */

double apr_source_reader_backlog(const AprSourceReader *rd)
{
    uint64_t produced;

    if (!rd || !rd->src) return 0.0;
    produced = rb_write_pos(rd->src->rb);

    if (rd->rs) {
        return (double)produced - ((double)rd->in_base +
               (double)apr_resampler_in_pos_q32(rd->rs) / Q32_ONE);
    }
    return (double)produced - (double)rd->in_need;
}

double apr_source_reader_error(const AprSourceReader *rd)
{
    if (!rd) return 0.0;
    return apr_source_reader_backlog(rd) - rd->target;
}

double apr_source_reader_trim_ppm(const AprSourceReader *rd)
{
    return (rd && rd->rs) ? apr_drift_ctl_trim_ppm(&rd->ctl) : 0.0;
}

uint64_t apr_source_reader_out_frames(const AprSourceReader *rd)
{
    return rd ? rd->out_pos : 0;
}

uint64_t apr_source_reader_first_bus_frame(const AprSourceReader *rd)
{
    return rd ? rd->first_bus : 0;
}
