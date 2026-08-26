/*
 * capture.c -- the capture factory and the shared status cell.
 *
 * apr_capture_create is, per include/capture.h, the ONLY place in the codebase
 * permitted to switch on AprSourceKind. Nothing above this file may ask what
 * kind a source is; that is what makes design section 4.3 true -- the whole
 * core runs on capture_fake with no audio hardware present.
 *
 * Design section 7 lists five files under src/capture/. This is a sixth: the
 * factory and the status cell belong to no one kind, and putting them in
 * wasapi_common.c would drag the fake source through WASAPI headers for no
 * reason. Reported as a deliberate deviation from the module layout.
 */
#include "apr_winver.h"

#include <windows.h>
#include <stdlib.h>

#include "capture_internal.h"
#include "clock.h"
#include "log.h"
#include "ringbuf.h"

/* ---------------------------------------------------------------------------
 * Status cell. See capture_internal.h for why it is shaped this way.
 * ------------------------------------------------------------------------- */

void apr_capstat_init(AprCapStatus *s)
{
    ZeroMemory(s, sizeof(*s));
    s->alive = 1;
    s->err   = apr_ok();
}

void apr_capstat_set_error(AprCapStatus *s, const AprErr *e)
{
    InterlockedIncrement(&s->err_seq);   /* -> odd: readers retry */
    MemoryBarrier();
    s->err = *e;
    MemoryBarrier();
    InterlockedIncrement(&s->err_seq);   /* -> even: readers proceed */
}

void apr_capstat_clear_error(AprCapStatus *s)
{
    AprErr ok = apr_ok();
    apr_capstat_set_error(s, &ok);
}

void apr_capstat_set_alive(AprCapStatus *s, int alive) { s->alive = alive ? 1 : 0; }
void apr_capstat_set_muted(AprCapStatus *s, int muted) { s->muted = muted ? 1 : 0; }
int  apr_capstat_alive(const AprCapStatus *s) { return s->alive != 0; }

void apr_capstat_read(const AprCapStatus *s, AprCaptureStatus *out)
{
    int tries;

    out->anchor_ticks    = (uint64_t)s->anchor_ticks;
    out->frames_written  = (uint64_t)s->frames_written;
    out->discontinuities = (uint64_t)s->discontinuities;
    out->alive           = (int)s->alive;
    out->muted           = (int)s->muted;

    /* Seqlock read. An error is raised a handful of times in a whole session,
     * so this loop effectively always runs once; the bound is there so a
     * pathological writer cannot spin a mixer tick forever. */
    for (tries = 0; tries < 8; tries++) {
        LONG a = s->err_seq;
        MemoryBarrier();
        out->last_error = s->err;
        MemoryBarrier();
        if (!(a & 1) && a == s->err_seq) return;
    }
}

/* ---------------------------------------------------------------------------
 * Rejoining a running timeline. See capture_internal.h.
 * ------------------------------------------------------------------------- */

void apr_capresume_init(AprCapResume *r, const AprCaptureConfig *cfg)
{
    ZeroMemory(r, sizeof(*r));
    if (!cfg) return;
    r->anchor_ticks = cfg->resume_anchor_ticks;
    r->sample_rate  = cfg->sample_rate;
    r->done         = (cfg->resume_anchor_ticks == 0);
}

uint64_t apr_capresume_fill(AprCapResume *r, RingBuf *rb,
                            uint64_t first_frame_ticks)
{
    uint64_t want, have, pad;

    if (!r || r->done || !rb) return 0;
    r->done = 1;                       /* once, whatever the answer is */

    if (first_frame_ticks <= r->anchor_ticks) return 0;

    /* Where this frame belongs, measured from the timeline's OWN anchor and
     * not from anything this capture has done. Floor, exactly like every other
     * tick-to-frame conversion in the system (clock.h). */
    want = apr_mul_div_u64(first_frame_ticks - r->anchor_ticks,
                           r->sample_rate, apr_qpc_freq(), NULL);
    have = rb_write_pos(rb);
    if (want <= have) return 0;        /* somebody already padded this far */

    pad = want - have;

    /* rb_write_silence is O(ring), not O(pad): a write longer than the ring
     * keeps the newest capacity frames and advances the cursor by the full
     * count (ringbuf.h), which is precisely what a twenty-minute hole needs.
     * Nothing here loops over the gap. */
    rb_write_silence(rb, (size_t)pad);
    r->padded = pad;

    APR_DEBUG(L"rejoined a running timeline: %llu frames of silence before "
              L"the first recovered frame, which lands at %llu",
              (unsigned long long)pad, (unsigned long long)want);
    return pad;
}

/* ---------------------------------------------------------------------------
 * Shared validation
 * ------------------------------------------------------------------------- */

AprErr apr_capture_check_common(const AprCaptureConfig *cfg, const RingBuf *rb)
{
    size_t want;

    if (!cfg) return APR_ERR(APR_E_INVALID_ARG, L"config is NULL");
    if (!rb)  return APR_ERR(APR_E_INVALID_ARG, L"ring buffer is NULL");
    if (cfg->sample_rate == 0)
        return APR_ERR(APR_E_INVALID_ARG, L"sample_rate is 0");
    if (cfg->channels == 0 || cfg->channels > 8)
        return APR_ERR(APR_E_INVALID_ARG, L"channels %u outside 1..8",
                       (unsigned)cfg->channels);

    /* Every source in apprecorder produces interleaved float32; the ring has
     * to have been created for that or frames silently shear. */
    want = (size_t)cfg->channels * sizeof(float);
    if (rb_frame_bytes(rb) != want)
        return APR_ERR(APR_E_INVALID_ARG,
                       L"ring frame is %zu bytes, %u float32 channels need %zu",
                       rb_frame_bytes(rb), (unsigned)cfg->channels, want);

    return apr_ok();
}

/* ---------------------------------------------------------------------------
 * The factory
 * ------------------------------------------------------------------------- */

AprErr apr_capture_create(const AprCaptureConfig *cfg, RingBuf *rb,
                          AprCapture **out)
{
    const AprCaptureVTable *vt;
    AprCapture *c;
    AprErr e;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"out is NULL");
    *out = NULL;

    e = apr_capture_check_common(cfg, rb);
    if (apr_failed(&e)) return e;

    switch (cfg->kind) {
    case APR_SRC_PROCESS: vt = apr_capture_process_vtable(); break;
    case APR_SRC_DEVICE:  vt = apr_capture_device_vtable();  break;
    case APR_SRC_FAKE:    vt = apr_capture_fake_vtable();    break;
    default:
        return APR_ERR(APR_E_INVALID_ARG, L"unknown source kind %d",
                       (int)cfg->kind);
    }

    c = (AprCapture *)calloc(1, sizeof(*c));
    if (!c) return APR_ERR(APR_E_NO_MEMORY, L"AprCapture");
    c->vt   = vt;
    c->impl = NULL;

    e = vt->open(c, cfg, rb);
    if (apr_failed(&e)) {
        /* A half-open capture can already own its thread -- the WASAPI kinds
         * create it before they touch COM -- so this close can be abandoned
         * exactly like any other. When it is, `c` is handed back through
         * `*out` DESPITE the failure: that non-NULL pointer is how the caller
         * learns its ring is still being written into and must not be freed
         * (see capture.h). The open error is what is returned, because it is
         * the one that explains why there is no usable capture. */
        AprErr ce = c->impl ? vt->close(c) : apr_ok();
        if (apr_failed(&ce)) {
            APR_LOG_ERR(APR_LOG_ERROR, &ce);
            *out = c;
        } else {
            free(c);
        }
        return e;
    }

    *out = c;
    return apr_ok();
}

/* The join/free boundary for the whole capture layer. See the long note in
 * capture.h: the value this returns is what stops a caller freeing a ring a
 * wedged pump is still writing into, and the free below is deliberately in
 * the same function as the test, so that a caller who ignores the value
 * leaks rather than corrupts. */
AprErr apr_capture_destroy(AprCapture *c)
{
    AprErr e;

    if (!c) return apr_ok();
    e = (c->vt && c->vt->close) ? c->vt->close(c) : apr_ok();
    if (apr_failed(&e)) return e;   /* abandoned: free NOTHING */
    free(c);
    return apr_ok();
}
