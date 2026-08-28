/*
 * ringbuf.c -- single-producer, multi-consumer ring. See include/ringbuf.h for
 * the contract and the reasoning behind the overrun policy.
 *
 * Synchronisation is TWO 64-bit cursors, both written only by the producer and
 * read by every consumer:
 *
 *   claim_pos  published BEFORE the producer copies. Storage for every frame
 *              index below it may already have been overwritten, including
 *              right now.
 *   write_pos  published AFTER the copy. Everything below it is complete and
 *              readable.
 *
 * One cursor is not enough, and the bug it hides is subtle: publishing only
 * after the copy means write_pos UNDERSTATES how far into storage the producer
 * has scribbled, so a consumer checking against it would happily return frames
 * the producer was overwriting at that very moment. Consumers therefore take
 * availability from write_pos and every safety decision from claim_pos.
 *
 * apprecorder is x64-only, where an aligned 64-bit load or store is atomic and
 * the hardware orders loads with loads and stores with stores; the only
 * reordering left to prevent is the compiler's, which _ReadWriteBarrier does.
 * No consumer ever writes a cursor the producer reads, so the producer's path
 * holds no atomic read-modify-write at all -- two stores and a memcpy.
 */
#include "ringbuf.h"

#include <intrin.h>
#include <malloc.h>
#include <string.h>

/* Attempts to complete a copy before the reader is repositioned outright. Each
 * failure means the producer wrote a whole buffer's worth during our memcpy. */
#define RB_READ_MAX_RETRIES 8

/* C4324: the struct is padded by the alignment specifier below. That padding
 * is the point -- see the comment on write_pos. */
#pragma warning(push)
#pragma warning(disable : 4324)
struct RingBuf {
    unsigned char *data;
    size_t         capacity;     /* frames; power of two */
    size_t         mask;         /* capacity - 1 */
    size_t         frame_bytes;

    /* Own cache line: consumers poll these and must not be made to fight the
     * producer for the line holding the fields above. */
    __declspec(align(64)) volatile uint64_t claim_pos;
    volatile uint64_t write_pos;
};
#pragma warning(pop)

static uint64_t load_acquire(const volatile uint64_t *p)
{
    uint64_t v = *p;
    _ReadWriteBarrier();
    return v;
}

static void store_release(volatile uint64_t *p, uint64_t v)
{
    _ReadWriteBarrier();
    *p = v;
}

static size_t round_up_pow2(size_t v)
{
    size_t n = 1;
    while (n < v) {
        size_t next = n << 1;
        if (next < n) return n;      /* saturate rather than wrap */
        n = next;
    }
    return n;
}

/* ------------------------------------------------------------------------- */

AprErr rb_create(size_t capacity_frames, size_t frame_bytes, RingBuf **out)
{
    RingBuf *rb;
    size_t   cap;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"rb_create with no out slot");
    *out = NULL;

    if (capacity_frames == 0 || frame_bytes == 0) {
        return APR_ERR(APR_E_INVALID_ARG,
                       L"ring of %zu frames x %zu bytes is degenerate",
                       capacity_frames, frame_bytes);
    }
    cap = round_up_pow2(capacity_frames);
    if (cap > (size_t)-1 / frame_bytes) {
        return APR_ERR(APR_E_INVALID_ARG,
                       L"ring of %zu frames x %zu bytes overflows",
                       cap, frame_bytes);
    }

    rb = (RingBuf *)_aligned_malloc(sizeof *rb, 64);
    if (!rb) return APR_ERR(APR_E_NO_MEMORY, L"ring buffer header");

    memset(rb, 0, sizeof *rb);
    rb->capacity    = cap;
    rb->mask        = cap - 1;
    rb->frame_bytes = frame_bytes;
    rb->claim_pos   = 0;
    rb->write_pos   = 0;

    rb->data = (unsigned char *)_aligned_malloc(cap * frame_bytes, 64);
    if (!rb->data) {
        _aligned_free(rb);
        return APR_ERR(APR_E_NO_MEMORY, L"ring buffer of %zu frames x %zu bytes",
                       cap, frame_bytes);
    }
    memset(rb->data, 0, cap * frame_bytes);
    *out = rb;
    return apr_ok();
}

void rb_destroy(RingBuf *rb)
{
    if (!rb) return;
    _aligned_free(rb->data);
    _aligned_free(rb);
}

size_t rb_capacity_frames(const RingBuf *rb) { return rb->capacity; }
size_t rb_frame_bytes(const RingBuf *rb)     { return rb->frame_bytes; }

uint64_t rb_write_pos(const RingBuf *rb)
{
    return load_acquire(&rb->write_pos);
}

/* ---- producer ----------------------------------------------------------- */

/* Copy `frames` frames into the storage starting at absolute index `at`,
 * splitting at the wrap point. `src` NULL means write zeroes. */
static void put(RingBuf *rb, uint64_t at, const unsigned char *src, size_t frames)
{
    size_t off   = (size_t)(at & rb->mask);
    size_t first = rb->capacity - off;
    size_t fb    = rb->frame_bytes;

    if (first > frames) first = frames;

    if (src) memcpy(rb->data + off * fb, src, first * fb);
    else     memset(rb->data + off * fb, 0, first * fb);

    if (first < frames) {
        size_t rest = frames - first;
        if (src) memcpy(rb->data, src + first * fb, rest * fb);
        else     memset(rb->data, 0, rest * fb);
    }
}

static void write_impl(RingBuf *rb, const unsigned char *src, size_t frames)
{
    uint64_t w;

    if (!rb || frames == 0) return;

    /* Only the producer writes write_pos, so a plain read is enough here. */
    w = rb->write_pos;

    if (frames > rb->capacity) {
        /* Keep the newest capacity frames. The cursor still moves by the full
         * count: the frames we skip really did exist on the timeline, and a
         * consumer must be told about them as loss, not have them vanish. */
        size_t skip = frames - rb->capacity;
        if (src) src += skip * rb->frame_bytes;
        w      += skip;
        frames  = rb->capacity;
    }

    /* Claim before scribbling, publish after: see the header comment. */
    store_release(&rb->claim_pos, w + frames);
    put(rb, w, src, frames);
    store_release(&rb->write_pos, w + frames);
}

void rb_write(RingBuf *rb, const void *src, size_t frames)
{
    if (frames && !src) return;
    write_impl(rb, (const unsigned char *)src, frames);
}

void rb_write_silence(RingBuf *rb, size_t frames)
{
    write_impl(rb, NULL, frames);
}

/* ---- consumers ---------------------------------------------------------- */

void rb_reader_init(RingReader *rd, RingBuf *rb, RbStart start)
{
    uint64_t w;

    if (!rd) return;
    rd->rb   = rb;
    rd->lost = 0;
    rd->pos  = 0;
    if (!rb) return;

    if (start == RB_START_OLDEST) {
        /* claim_pos, not write_pos: the frame the producer is overwriting
         * right now is already gone as far as a new reader is concerned. */
        uint64_t c = load_acquire(&rb->claim_pos);
        rd->pos = (c > (uint64_t)rb->capacity) ? c - rb->capacity : 0;
    } else {
        w = load_acquire(&rb->write_pos);
        rd->pos = w;
    }
}

uint64_t rb_reader_pos(const RingReader *rd)  { return rd->pos; }
uint64_t rb_reader_lost(const RingReader *rd) { return rd->lost; }

uint64_t rb_reader_available(const RingReader *rd)
{
    uint64_t w, c, cap, start;

    if (!rd || !rd->rb) return 0;
    cap = (uint64_t)rd->rb->capacity;
    w   = load_acquire(&rd->rb->write_pos);
    c   = load_acquire(&rd->rb->claim_pos);

    start = rd->pos;
    if (c - start > cap) start = c - cap;      /* already overrun by this much */
    return w > start ? w - start : 0;
}

/* Force the cursor up to the oldest frame still safe to read, if the producer
 * has lapped it. `c` is the claim cursor. Returns the frames written off. */
static uint64_t reap_overrun(RingReader *rd, uint64_t c)
{
    uint64_t cap = (uint64_t)rd->rb->capacity;
    uint64_t lost;

    if (c - rd->pos <= cap) return 0;

    lost      = (c - rd->pos) - cap;
    rd->pos  += lost;
    rd->lost += lost;
    return lost;
}

size_t rb_read(RingReader *rd, void *dst, size_t max_frames, uint64_t *out_lost)
{
    RingBuf       *rb;
    unsigned char *out = (unsigned char *)dst;
    uint64_t       lost_total = 0;
    int            attempt;

    if (out_lost) *out_lost = 0;
    if (!rd || !rd->rb || !out || max_frames == 0) return 0;
    rb = rd->rb;

    for (attempt = 0; ; attempt++) {
        uint64_t w = load_acquire(&rb->write_pos);
        uint64_t c = load_acquire(&rb->claim_pos);
        uint64_t avail, n, c2;
        size_t   off, first, fb = rb->frame_bytes;

        lost_total += reap_overrun(rd, c);

        avail = w > rd->pos ? w - rd->pos : 0;
        n     = avail < (uint64_t)max_frames ? avail : (uint64_t)max_frames;
        if (n == 0) {
            if (out_lost) *out_lost = lost_total;
            return 0;
        }

        off   = (size_t)(rd->pos & rb->mask);
        first = rb->capacity - off;
        if ((uint64_t)first > n) first = (size_t)n;

        memcpy(out, rb->data + off * fb, first * fb);
        if ((uint64_t)first < n) {
            memcpy(out + first * fb, rb->data, ((size_t)n - first) * fb);
        }

        /* Was the oldest frame we just copied still ours for the whole copy?
         * If the producer CLAIMED past pos + capacity while we were in memcpy,
         * some of those bytes are from the next epoch. Checking write_pos here
         * instead would miss a copy still in flight. */
        c2 = load_acquire(&rb->claim_pos);
        if (c2 - rd->pos <= (uint64_t)rb->capacity) {
            rd->pos += n;
            if (out_lost) *out_lost = lost_total;
            return (size_t)n;
        }

        if (attempt >= RB_READ_MAX_RETRIES) {
            /* The producer is outrunning us badly enough that retrying is
             * futile. Land half a buffer behind it -- far enough to have a
             * chance, recent enough to be useful -- and report the whole jump
             * so the caller can account for it. */
            uint64_t target = c2 - (uint64_t)rb->capacity / 2;
            if (target > rd->pos) {
                uint64_t jump = target - rd->pos;
                rd->pos    = target;
                rd->lost  += jump;
                lost_total += jump;
            }
            if (out_lost) *out_lost = lost_total;
            return 0;
        }
    }
}

size_t rb_skip(RingReader *rd, size_t max_frames, uint64_t *out_lost)
{
    uint64_t w, c, avail, n, lost;

    if (out_lost) *out_lost = 0;
    if (!rd || !rd->rb || max_frames == 0) return 0;

    w = load_acquire(&rd->rb->write_pos);
    c = load_acquire(&rd->rb->claim_pos);

    lost = reap_overrun(rd, c);
    if (out_lost) *out_lost = lost;

    avail = w > rd->pos ? w - rd->pos : 0;
    n     = avail < (uint64_t)max_frames ? avail : (uint64_t)max_frames;
    rd->pos += n;
    return (size_t)n;
}

/* See ringbuf.h. The clamp is against the CLAIM cursor's oldest safe frame and
 * the WRITE cursor, because those are the same two bounds every read is held
 * to; landing outside them would hand back frames the producer is in the middle
 * of, or frames it has not written. `lost` is deliberately untouched: this is
 * the reader moving its own timeline, not data going past it. */
uint64_t rb_reader_seek(RingReader *rd, uint64_t pos)
{
    uint64_t w, c, cap, oldest;

    if (!rd || !rd->rb) return 0;

    w   = load_acquire(&rd->rb->write_pos);
    c   = load_acquire(&rd->rb->claim_pos);
    cap = (uint64_t)rd->rb->capacity;

    oldest = c > cap ? c - cap : 0;
    if (pos < oldest) pos = oldest;
    if (pos > w)      pos = w;

    rd->pos = pos;
    return rd->pos;
}
