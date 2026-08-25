/*
 * test_ringbuf.c -- core/ringbuf.c, the single-producer / multi-consumer ring.
 *
 * The properties that matter downstream:
 *   - the producer (an audio callback) never blocks and never fails;
 *   - a consumer that falls behind loses data but is TOLD, exactly, how many
 *     frames it lost, so the drift corrector can fill the hole;
 *   - consumers are independent: one slow bus must not disturb another;
 *   - nothing is ever handed back torn, even while the producer is running.
 */
#include "test_runner.h"
#include "ringbuf.h"

#include <windows.h>

/* ---- construction -------------------------------------------------------- */

TEST(capacity_rounds_up_to_a_power_of_two)
{
    RingBuf *rb = NULL;
    AprErr   e  = rb_create(1000, 4, &rb);

    ASSERT_FALSE(apr_failed(&e));
    ASSERT_NOT_NULL(rb);
    ASSERT_EQ_U64(1024, rb_capacity_frames(rb));
    ASSERT_EQ_U64(4, rb_frame_bytes(rb));
    ASSERT_EQ_U64(0, rb_write_pos(rb));
    rb_destroy(rb);
}

TEST(a_power_of_two_capacity_is_kept_as_is)
{
    RingBuf *rb = NULL;
    (void)rb_create(512, 8, &rb);
    ASSERT_EQ_U64(512, rb_capacity_frames(rb));
    rb_destroy(rb);
}

TEST(degenerate_dimensions_are_refused)
{
    RingBuf *rb = (RingBuf *)(void *)1;
    AprErr   e;

    e = rb_create(0, 4, &rb);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(rb);

    rb = (RingBuf *)(void *)1;
    e = rb_create(64, 0, &rb);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(rb);
}

TEST(destroying_null_is_allowed)
{
    rb_destroy(NULL);
    ASSERT_EQ_INT(0, 0);
}

/* ---- the simple path ----------------------------------------------------- */

TEST(what_is_written_is_what_is_read)
{
    RingBuf   *rb = NULL;
    RingReader rd;
    uint32_t   src[8], dst[8];
    uint64_t   lost = 999;
    int        i;

    (void)rb_create(64, sizeof(uint32_t), &rb);
    for (i = 0; i < 8; i++) src[i] = 0xA0000000u + (uint32_t)i;

    rb_reader_init(&rd, rb, RB_START_OLDEST);
    rb_write(rb, src, 8);

    ASSERT_EQ_U64(8, rb_reader_available(&rd));
    ASSERT_EQ_U64(8, rb_read(&rd, dst, 8, &lost));
    ASSERT_EQ_U64(0, lost);
    ASSERT_MEM_EQ(src, dst, sizeof src);
    ASSERT_EQ_U64(8, rb_reader_pos(&rd));
    ASSERT_EQ_U64(0, rb_read(&rd, dst, 8, &lost));   /* drained */
    rb_destroy(rb);
}

TEST(a_short_read_takes_only_what_is_asked_for)
{
    RingBuf   *rb = NULL;
    RingReader rd;
    uint32_t   src[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    uint32_t   dst[8] = { 0 };

    (void)rb_create(64, sizeof(uint32_t), &rb);
    rb_reader_init(&rd, rb, RB_START_OLDEST);
    rb_write(rb, src, 8);

    ASSERT_EQ_U64(3, rb_read(&rd, dst, 3, NULL));
    ASSERT_EQ_U64(3, rb_reader_pos(&rd));
    ASSERT_EQ_U64(5, rb_reader_available(&rd));
    ASSERT_EQ_INT(2, (int)dst[2]);
    rb_destroy(rb);
}

TEST(silence_is_written_as_zero_frames)
{
    RingBuf   *rb = NULL;
    RingReader rd;
    float      dst[4] = { 1.f, 1.f, 1.f, 1.f };
    int        i;

    (void)rb_create(64, sizeof(float), &rb);
    rb_reader_init(&rd, rb, RB_START_OLDEST);
    rb_write_silence(rb, 4);

    ASSERT_EQ_U64(4, rb_read(&rd, dst, 4, NULL));
    for (i = 0; i < 4; i++) ASSERT_NEAR(0.0, dst[i], 0.0);
    rb_destroy(rb);
}

TEST(reads_and_writes_wrap_around_the_end_of_the_storage)
{
    RingBuf   *rb  = NULL;
    RingReader rd;
    uint32_t   frame, got;
    uint32_t   n = 0;

    /* 16-frame ring, 500 frames pushed through it in chunks of 5: every
     * possible alignment of a chunk against the wrap point is exercised. */
    (void)rb_create(16, sizeof(uint32_t), &rb);
    rb_reader_init(&rd, rb, RB_START_OLDEST);

    while (n < 500) {
        uint32_t chunk[5];
        int      i;
        for (i = 0; i < 5; i++) chunk[i] = n + (uint32_t)i;
        rb_write(rb, chunk, 5);
        n += 5;

        for (i = 0; i < 5; i++) {
            frame = (uint32_t)rb_reader_pos(&rd);
            if (rb_read(&rd, &got, 1, NULL) != 1) FAIL("ring went empty early");
            if (got != frame) FAIL("frame content does not match its index");
        }
    }
    ASSERT_EQ_U64(500, rb_reader_pos(&rd));
    ASSERT_EQ_U64(0, rb_reader_lost(&rd));
    rb_destroy(rb);
}

/* ---- overrun ------------------------------------------------------------- */

TEST(a_slow_consumer_loses_the_oldest_frames_and_is_told_how_many)
{
    RingBuf   *rb = NULL;
    RingReader rd;
    uint32_t   src[100], dst[64];
    uint64_t   lost = 0;
    int        i;

    (void)rb_create(64, sizeof(uint32_t), &rb);
    for (i = 0; i < 100; i++) src[i] = (uint32_t)i;

    rb_reader_init(&rd, rb, RB_START_OLDEST);
    rb_write(rb, src, 100);           /* 36 frames past capacity */

    /* Overrun is visible before reading, so a consumer can react without
     * copying anything. */
    ASSERT_EQ_U64(64, rb_reader_available(&rd));

    ASSERT_EQ_U64(64, rb_read(&rd, dst, 64, &lost));
    ASSERT_EQ_U64(36, lost);
    ASSERT_EQ_U64(36, rb_reader_lost(&rd));
    ASSERT_EQ_INT(36, (int)dst[0]);    /* the oldest surviving frame */
    ASSERT_EQ_INT(99, (int)dst[63]);
    ASSERT_EQ_U64(100, rb_reader_pos(&rd));
    rb_destroy(rb);
}

TEST(loss_accumulates_across_reads)
{
    RingBuf   *rb = NULL;
    RingReader rd;
    uint32_t   dst[8];
    uint64_t   lost = 0;

    (void)rb_create(8, sizeof(uint32_t), &rb);
    rb_reader_init(&rd, rb, RB_START_OLDEST);

    rb_write_silence(rb, 20);
    (void)rb_read(&rd, dst, 8, &lost);
    ASSERT_EQ_U64(12, lost);

    rb_write_silence(rb, 20);
    (void)rb_read(&rd, dst, 8, &lost);
    ASSERT_EQ_U64(12, lost);
    ASSERT_EQ_U64(24, rb_reader_lost(&rd));
    rb_destroy(rb);
}

TEST(a_single_write_larger_than_the_ring_keeps_the_newest_frames)
{
    RingBuf   *rb = NULL;
    RingReader rd;
    uint32_t   src[200], dst[64];
    uint64_t   lost = 0;
    int        i;

    (void)rb_create(64, sizeof(uint32_t), &rb);
    for (i = 0; i < 200; i++) src[i] = (uint32_t)i;

    rb_reader_init(&rd, rb, RB_START_OLDEST);
    rb_write(rb, src, 200);

    ASSERT_EQ_U64(200, rb_write_pos(rb));
    ASSERT_EQ_U64(64, rb_read(&rd, dst, 64, &lost));
    ASSERT_EQ_U64(136, lost);
    ASSERT_EQ_INT(136, (int)dst[0]);
    ASSERT_EQ_INT(199, (int)dst[63]);
    rb_destroy(rb);
}

TEST(skipping_forward_reports_loss_the_same_way)
{
    RingBuf   *rb = NULL;
    RingReader rd;
    uint64_t   lost = 0;

    (void)rb_create(64, sizeof(uint32_t), &rb);
    rb_reader_init(&rd, rb, RB_START_OLDEST);
    rb_write_silence(rb, 100);

    ASSERT_EQ_U64(10, rb_skip(&rd, 10, &lost));
    ASSERT_EQ_U64(36, lost);
    ASSERT_EQ_U64(46, rb_reader_pos(&rd));
    rb_destroy(rb);
}

/* ---- several consumers --------------------------------------------------- */

TEST(consumers_hold_independent_cursors)
{
    RingBuf   *rb = NULL;
    RingReader fast, slow;
    uint32_t   src[32], dst[32];
    int        i;

    (void)rb_create(64, sizeof(uint32_t), &rb);
    for (i = 0; i < 32; i++) src[i] = (uint32_t)i;

    rb_reader_init(&fast, rb, RB_START_OLDEST);
    rb_reader_init(&slow, rb, RB_START_OLDEST);
    rb_write(rb, src, 32);

    ASSERT_EQ_U64(32, rb_read(&fast, dst, 32, NULL));
    ASSERT_EQ_U64(0, rb_reader_available(&fast));

    /* The slow reader still sees everything: reading does not consume. */
    ASSERT_EQ_U64(32, rb_reader_available(&slow));
    memset(dst, 0, sizeof dst);
    ASSERT_EQ_U64(32, rb_read(&slow, dst, 32, NULL));
    ASSERT_MEM_EQ(src, dst, sizeof src);
    rb_destroy(rb);
}

TEST(only_the_consumer_that_fell_behind_is_overrun)
{
    RingBuf   *rb = NULL;
    RingReader fast, slow;
    uint32_t   dst[64];
    uint64_t   lost_fast = 0, lost_slow = 0;
    int        i;

    (void)rb_create(64, sizeof(uint32_t), &rb);
    rb_reader_init(&fast, rb, RB_START_OLDEST);
    rb_reader_init(&slow, rb, RB_START_OLDEST);

    for (i = 0; i < 10; i++) {
        rb_write_silence(rb, 32);
        (void)rb_read(&fast, dst, 64, &lost_fast);
        ASSERT_EQ_U64(0, lost_fast);       /* keeping up: never loses a frame */
    }

    (void)rb_read(&slow, dst, 64, &lost_slow);
    ASSERT_EQ_U64(320 - 64, lost_slow);
    ASSERT_EQ_U64(0, rb_reader_lost(&fast));
    rb_destroy(rb);
}

TEST(a_reader_can_attach_at_the_write_cursor)
{
    RingBuf   *rb = NULL;
    RingReader late;
    uint32_t   dst[8];

    (void)rb_create(64, sizeof(uint32_t), &rb);
    rb_write_silence(rb, 40);

    rb_reader_init(&late, rb, RB_START_LATEST);
    ASSERT_EQ_U64(40, rb_reader_pos(&late));
    ASSERT_EQ_U64(0, rb_reader_available(&late));
    ASSERT_EQ_U64(0, rb_read(&late, dst, 8, NULL));

    rb_write_silence(rb, 8);
    ASSERT_EQ_U64(8, rb_read(&late, dst, 8, NULL));
    rb_destroy(rb);
}

TEST(a_reader_attaching_at_the_oldest_frame_skips_what_is_already_gone)
{
    RingBuf   *rb = NULL;
    RingReader rd;

    (void)rb_create(64, sizeof(uint32_t), &rb);
    rb_write_silence(rb, 300);

    rb_reader_init(&rd, rb, RB_START_OLDEST);
    ASSERT_EQ_U64(300 - 64, rb_reader_pos(&rd));
    ASSERT_EQ_U64(64, rb_reader_available(&rd));
    ASSERT_EQ_U64(0, rb_reader_lost(&rd));   /* it was never there to lose */
    rb_destroy(rb);
}

/* ---- concurrency --------------------------------------------------------- */

#define CONC_FRAMES 400000u

typedef struct {
    RingBuf *rb;
    volatile LONG *go;
} ProducerArgs;

/* Every frame carries its own absolute index, so a consumer can prove both
 * that the bytes are intact and that its loss accounting is exact. */
static DWORD WINAPI conc_producer(LPVOID param)
{
    ProducerArgs *args = (ProducerArgs *)param;
    uint64_t      chunk[64];
    uint64_t      n = 0;

    while (!*args->go) YieldProcessor();
    while (n < CONC_FRAMES) {
        uint64_t k = (CONC_FRAMES - n) < 64 ? (CONC_FRAMES - n) : 64;
        uint64_t i;
        for (i = 0; i < k; i++) chunk[i] = n + i;
        rb_write(args->rb, chunk, (size_t)k);
        n += k;
    }
    return 0;
}

TEST(a_concurrent_consumer_sees_intact_frames_and_exact_loss)
{
    RingBuf     *rb = NULL;
    RingReader   rd;
    ProducerArgs args;
    volatile LONG go = 0;
    HANDLE       th;
    uint64_t    *dst;
    uint64_t     read_total = 0, lost = 0, corrupt = 0;
    int          spins = 0;

    (void)rb_create(4096, sizeof(uint64_t), &rb);
    dst = (uint64_t *)malloc(2048 * sizeof(uint64_t));
    ASSERT_NOT_NULL(dst);

    rb_reader_init(&rd, rb, RB_START_OLDEST);
    args.rb = rb;
    args.go = &go;
    th = CreateThread(NULL, 0, conc_producer, &args, 0, NULL);
    ASSERT_NOT_NULL(th);
    InterlockedExchange(&go, 1);

    while (rb_reader_pos(&rd) < CONC_FRAMES) {
        uint64_t base = rb_reader_pos(&rd);
        size_t   got  = rb_read(&rd, dst, 2048, &lost);
        size_t   i;

        base += lost;                        /* rb_read reports the jump first */
        for (i = 0; i < got; i++) {
            if (dst[i] != base + i) corrupt++;
        }
        read_total += got;
        if (got == 0 && ++spins > 200000000) FAIL("producer appears stuck");
    }

    (void)WaitForSingleObject(th, 10000);
    CloseHandle(th);

    ASSERT_EQ_U64(0, corrupt);
    ASSERT_EQ_U64(CONC_FRAMES, read_total + rb_reader_lost(&rd));
    ASSERT_EQ_U64(CONC_FRAMES, rb_write_pos(rb));
    ASSERT_EQ_U64(CONC_FRAMES, rb_reader_pos(&rd));

    free(dst);
    rb_destroy(rb);
}

typedef struct {
    RingBuf      *rb;
    volatile LONG *go;
    int           slow;
    uint64_t      start;       /* where this reader attached */
    uint64_t      read_total;
    uint64_t      lost_total;
    uint64_t      corrupt;
} ConsumerArgs;

static DWORD WINAPI conc_consumer(LPVOID param)
{
    ConsumerArgs *args = (ConsumerArgs *)param;
    RingReader    rd;
    uint64_t      dst[512];
    uint64_t      lost = 0;

    while (!*args->go) YieldProcessor();

    /* This thread may not get scheduled until the producer is well underway,
     * so RB_START_OLDEST can legitimately land above zero. Frames written
     * before a reader attached are not that reader's loss -- record where we
     * came in so the accounting identity below can be checked exactly. */
    rb_reader_init(&rd, args->rb, RB_START_OLDEST);
    args->start = rb_reader_pos(&rd);

    while (rb_reader_pos(&rd) < CONC_FRAMES) {
        uint64_t base = rb_reader_pos(&rd);
        size_t   got  = rb_read(&rd, dst, 512, &lost);
        size_t   i;

        base += lost;
        for (i = 0; i < got; i++) {
            if (dst[i] != base + i) args->corrupt++;
        }
        args->read_total += got;
        if (args->slow && got) Sleep(0);
    }
    args->lost_total = rb_reader_lost(&rd);
    return 0;
}

TEST(two_concurrent_consumers_at_different_speeds_stay_correct)
{
    RingBuf     *rb = NULL;
    ProducerArgs pargs;
    ConsumerArgs cargs[2];
    volatile LONG go = 0;
    HANDLE       th[3];
    int          i;

    (void)rb_create(2048, sizeof(uint64_t), &rb);
    memset(cargs, 0, sizeof cargs);
    for (i = 0; i < 2; i++) { cargs[i].rb = rb; cargs[i].go = &go; }
    cargs[1].slow = 1;

    pargs.rb = rb;
    pargs.go = &go;

    th[0] = CreateThread(NULL, 0, conc_consumer, &cargs[0], 0, NULL);
    th[1] = CreateThread(NULL, 0, conc_consumer, &cargs[1], 0, NULL);
    th[2] = CreateThread(NULL, 0, conc_producer, &pargs, 0, NULL);
    ASSERT_NOT_NULL(th[0]);
    ASSERT_NOT_NULL(th[1]);
    ASSERT_NOT_NULL(th[2]);

    InterlockedExchange(&go, 1);
    ASSERT_NE_INT(WAIT_TIMEOUT, WaitForMultipleObjects(3, th, TRUE, 60000));
    for (i = 0; i < 3; i++) CloseHandle(th[i]);

    ASSERT_EQ_U64(0, cargs[0].corrupt);
    ASSERT_EQ_U64(0, cargs[1].corrupt);

    /* Every frame of the stream is accounted for, per reader, with no
     * double-counting and nothing unexplained: where it came in, what it read,
     * and what it was told it lost. The deliberately slow one is expected to
     * have lost frames -- that is the policy working, not a failure. */
    for (i = 0; i < 2; i++) {
        ASSERT_EQ_U64(CONC_FRAMES,
                      cargs[i].start + cargs[i].read_total + cargs[i].lost_total);
    }
    rb_destroy(rb);
}
