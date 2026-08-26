/*
 * test_action_wav.c -- golden-file tests for src/actions/action_wav.c.
 *
 * WAV is the reference encoder: the file format is fully specified, so nothing
 * here is asserted approximately. Headers are compared byte-for-byte against
 * literals spelled out in this file, and payloads are compared bit-for-bit
 * against the exact float32 patterns that were fed in -- including -0.0, a
 * denormal, a NaN and magnitudes outside [-1,+1], any of which a stray
 * conversion, clamp or dither would disturb.
 *
 * Three properties get more than a spot check, because they are the ones a
 * plausible-looking implementation gets wrong:
 *
 *   1. on_audio must never wait for the disk. This is PROVEN, not asserted:
 *      a test seam holds the writer thread's disk path shut, the test confirms
 *      from a second handle that nothing beyond the header has reached the
 *      file, and a feeder thread must still complete a second of audio within
 *      a bounded wait. An implementation that writes from on_audio cannot pass
 *      -- it would still be inside its first call.
 *
 *   2. A recording that is never finalized must still be playable. The header
 *      is read back WHILE recording and must already describe the data on disk.
 *
 *   3. finalize after an I/O error must still leave a playable file, with a
 *      header that describes exactly the bytes that made it.
 */
#include "test_runner.h"
#include "action.h"

#include <windows.h>

/* The vtable core/registry.c will list. Declared here rather than in a public
 * header: registry.c is the only other caller, and it declares the same
 * `extern const AprActionVTable apr_action_wav;`. */
extern const AprActionVTable apr_action_wav;

/* Internal seams. See the "test seams" section of action_wav.c for what each
 * does and why it costs nothing on a real run. Deliberately not in a header:
 * nothing but this file may touch them. */
extern size_t apr_wav_header_build(unsigned char *dst, uint32_t sample_rate,
                                   uint16_t channels, uint64_t data_bytes);
extern volatile HANDLE apr_wav_test_write_gate;
extern volatile LONG64 apr_wav_test_fail_after_bytes;

#define WAV_HDR_BYTES 116

/* ------------------------------------------------------------------------
 * The golden header: 116 bytes of a 48 kHz stereo float32 recording holding
 * no audio at all. Written out by hand so a change to the header builder has
 * to be justified against the format rather than against itself.
 * ------------------------------------------------------------------------ */
static const unsigned char k_golden_48k_stereo_empty[WAV_HDR_BYTES] = {
    /* 0   */ 'R','I','F','F',
    /* 4   */ 0x6C,0x00,0x00,0x00,          /* 108 = header - 8 + 0 data     */
    /* 8   */ 'W','A','V','E',
    /* 12  */ 'J','U','N','K',              /* reserved for a ds64 upgrade   */
    /* 16  */ 0x1C,0x00,0x00,0x00,          /* 28                            */
    /* 20  */ 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    /* 36  */ 0,0,0,0, 0,0,0,0, 0,0,0,0,
    /* 48  */ 'f','m','t',' ',
    /* 52  */ 0x28,0x00,0x00,0x00,          /* 40 = WAVEFORMATEXTENSIBLE     */
    /* 56  */ 0xFE,0xFF,                    /* WAVE_FORMAT_EXTENSIBLE        */
    /* 58  */ 0x02,0x00,                    /* channels                      */
    /* 60  */ 0x80,0xBB,0x00,0x00,          /* 48000                         */
    /* 64  */ 0x00,0xDC,0x05,0x00,          /* 384000 avg bytes/sec          */
    /* 68  */ 0x08,0x00,                    /* block align                   */
    /* 70  */ 0x20,0x00,                    /* 32 bits                       */
    /* 72  */ 0x16,0x00,                    /* cbSize 22                     */
    /* 74  */ 0x20,0x00,                    /* 32 valid bits                 */
    /* 76  */ 0x03,0x00,0x00,0x00,          /* FRONT_LEFT | FRONT_RIGHT      */
    /* 80  */ 0x03,0x00,0x00,0x00,0x00,0x00,0x10,0x00,
              0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71, /* IEEE_FLOAT         */
    /* 96  */ 'f','a','c','t',
    /* 100 */ 0x04,0x00,0x00,0x00,
    /* 104 */ 0x00,0x00,0x00,0x00,          /* 0 sample frames               */
    /* 108 */ 'd','a','t','a',
    /* 112 */ 0x00,0x00,0x00,0x00           /* 0 data bytes                  */
};

/* ---- little helpers ------------------------------------------------------ */

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void tmp_path(wchar_t *buf, size_t cch, const wchar_t *tag)
{
    static LONG counter;
    wchar_t dir[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(buf, cch, _TRUNCATE, L"%lsapr_wav_%ls_%lu_%ld.wav",
                 dir, tag, GetCurrentProcessId(),
                 InterlockedIncrement(&counter));
}

static unsigned char *slurp(const wchar_t *path, size_t *out_len)
{
    HANDLE         h;
    LARGE_INTEGER  size;
    unsigned char *buf;
    size_t         total = 0, want_total;

    *out_len = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return NULL; }

    want_total = (size_t)size.QuadPart;
    buf = (unsigned char *)malloc(want_total + 1);
    if (!buf) { CloseHandle(h); return NULL; }

    while (total < want_total) {
        size_t left = want_total - total;
        DWORD  want = (left > (1u << 20)) ? (1u << 20) : (DWORD)left;
        DWORD  got  = 0;
        if (!ReadFile(h, buf + total, want, &got, NULL) || got == 0) break;
        total += got;
    }
    CloseHandle(h);
    *out_len = total;
    return buf;
}

/* Read just the header of a file that may still be open for writing. */
static int peek_header(const wchar_t *path, unsigned char *hdr)
{
    HANDLE h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD  got = 0;
    int    ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = ReadFile(h, hdr, WAV_HDR_BYTES, &got, NULL) && got == WAV_HDR_BYTES;
    CloseHandle(h);
    return ok;
}

static uint64_t file_size(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return 0;
    return ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

/* Deterministic signal. The first samples are bit patterns chosen to break any
 * implementation that converts, clamps or dithers; the rest is a xorshift
 * stream forced non-zero, so substituted silence is visible. */
static void fill_signal(float *dst, size_t count, uint32_t seed)
{
    static const uint32_t specials[] = {
        0x80000000u, /* -0.0                  */
        0x3F800000u, /* +1.0                  */
        0xBF800000u, /* -1.0                  */
        0x40490FDBu, /* pi -- outside [-1,+1] */
        0x7F7FFFFFu, /* FLT_MAX               */
        0xFF7FFFFFu, /* -FLT_MAX              */
        0x00000001u, /* smallest denormal     */
        0x7FC00000u  /* quiet NaN             */
    };
    uint32_t s = seed ? seed : 1u;
    size_t   i;

    for (i = 0; i < count; i++) {
        uint32_t bits;
        if (i < sizeof specials / sizeof specials[0]) {
            bits = specials[i];
        } else {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            bits = s | 0x00000001u;   /* never a clean zero */
        }
        memcpy(&dst[i], &bits, sizeof bits);
    }
}

static AprActionConfig cfg_of(const wchar_t *path, uint32_t rate, uint16_t ch)
{
    AprActionConfig c;
    memset(&c, 0, sizeof c);
    c.out_path    = path;
    c.sample_rate = rate;
    c.channels    = ch;
    return c;
}

/* Feed `frames` of `pcm` in `block` sized pieces, as the mixer would. */
static AprErr feed(void *st, const float *pcm, size_t frames, uint16_t ch,
                   size_t block)
{
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done;
        AprErr e;
        if (n > block) n = block;
        e = apr_action_wav.on_audio(st, pcm + done * ch, n, 0);
        if (apr_failed(&e)) return e;
        done += n;
    }
    return apr_ok();
}

/* ---- identity ------------------------------------------------------------ */

TEST(the_vtable_identifies_itself_as_wav)
{
    ASSERT_STR_EQ("wav", apr_action_wav.id);
    ASSERT_WSTR_EQ(L"wav", apr_action_wav.extension);
    ASSERT_NE_INT(0, apr_action_wav.display_name_id);
    ASSERT_GT_INT(0, (long long)wcslen(apr_str(apr_action_wav.display_name_id)));
    ASSERT_NOT_NULL((void *)(uintptr_t)apr_action_wav.create);
    ASSERT_NOT_NULL((void *)(uintptr_t)apr_action_wav.on_audio);
    ASSERT_NOT_NULL((void *)(uintptr_t)apr_action_wav.finalize);
    ASSERT_NOT_NULL((void *)(uintptr_t)apr_action_wav.destroy);
}

/* ---- headers ------------------------------------------------------------- */

TEST(a_recording_with_no_frames_is_a_valid_empty_wav)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = NULL;
    AprErr          e;
    unsigned char  *file;
    size_t          len = 0;

    tmp_path(path, MAX_PATH, L"empty");
    cfg = cfg_of(path, 48000, 2);

    e = apr_action_wav.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_NOT_NULL(st);

    e = apr_action_wav.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_wav.destroy(st);

    file = slurp(path, &len);
    ASSERT_NOT_NULL(file);
    ASSERT_EQ_U64(WAV_HDR_BYTES, len);
    ASSERT_MEM_EQ(k_golden_48k_stereo_empty, file, WAV_HDR_BYTES);

    free(file);
    DeleteFileW(path);
}

TEST(zero_frame_calls_are_a_no_op)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = NULL;
    AprErr          e;
    unsigned char  *file;
    size_t          len = 0;
    float           dummy = 0.0f;

    tmp_path(path, MAX_PATH, L"zeroframes");
    cfg = cfg_of(path, 48000, 2);
    e = apr_action_wav.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));

    /* Zero frames is legal with a buffer and with no buffer at all. */
    e = apr_action_wav.on_audio(st, &dummy, 0, 0);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_action_wav.on_audio(st, NULL, 0, 0);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_action_wav.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_wav.destroy(st);

    file = slurp(path, &len);
    ASSERT_NOT_NULL(file);
    ASSERT_EQ_U64(WAV_HDR_BYTES, len);
    ASSERT_MEM_EQ(k_golden_48k_stereo_empty, file, WAV_HDR_BYTES);
    free(file);
    DeleteFileW(path);
}

TEST(the_header_carries_the_session_format)
{
    unsigned char hdr[WAV_HDR_BYTES];
    size_t        n;

    /* 6 channels at 96 kHz: block align 24, 5.1 channel mask. */
    n = apr_wav_header_build(hdr, 96000, 6, 240);
    ASSERT_EQ_U64(WAV_HDR_BYTES, n);

    ASSERT_MEM_EQ("RIFF", hdr + 0, 4);
    ASSERT_EQ_U64(108u + 240u, rd32(hdr + 4));
    ASSERT_MEM_EQ("WAVE", hdr + 8, 4);
    ASSERT_MEM_EQ("JUNK", hdr + 12, 4);
    ASSERT_MEM_EQ("fmt ", hdr + 48, 4);
    ASSERT_EQ_U64(40, rd32(hdr + 52));
    ASSERT_EQ_U64(0xFFFE, rd16(hdr + 56));
    ASSERT_EQ_U64(6, rd16(hdr + 58));
    ASSERT_EQ_U64(96000, rd32(hdr + 60));
    ASSERT_EQ_U64(96000u * 24u, rd32(hdr + 64));
    ASSERT_EQ_U64(24, rd16(hdr + 68));
    ASSERT_EQ_U64(32, rd16(hdr + 70));
    ASSERT_EQ_U64(22, rd16(hdr + 72));
    ASSERT_EQ_U64(32, rd16(hdr + 74));
    ASSERT_EQ_U64(0x3F, rd32(hdr + 76));            /* 5.1 */
    ASSERT_MEM_EQ(k_golden_48k_stereo_empty + 80, hdr + 80, 16);
    ASSERT_MEM_EQ("fact", hdr + 96, 4);
    ASSERT_EQ_U64(10, rd32(hdr + 104));             /* 240 / 24 frames */
    ASSERT_MEM_EQ("data", hdr + 108, 4);
    ASSERT_EQ_U64(240, rd32(hdr + 112));

    /* Mono lands on FRONT_CENTER, not on half a stereo mask. */
    n = apr_wav_header_build(hdr, 44100, 1, 0);
    ASSERT_EQ_U64(WAV_HDR_BYTES, n);
    ASSERT_EQ_U64(1, rd16(hdr + 58));
    ASSERT_EQ_U64(44100, rd32(hdr + 60));
    ASSERT_EQ_U64(44100u * 4u, rd32(hdr + 64));
    ASSERT_EQ_U64(4, rd16(hdr + 68));
    ASSERT_EQ_U64(0x04, rd32(hdr + 76));
}

TEST(past_four_gigabytes_the_header_becomes_rf64)
{
    unsigned char  hdr[WAV_HDR_BYTES];
    const uint64_t big = 5ull * 1024ull * 1024ull * 1024ull;  /* 5 GiB */

    (void)apr_wav_header_build(hdr, 48000, 2, big);

    ASSERT_MEM_EQ("RF64", hdr + 0, 4);
    ASSERT_EQ_U64(0xFFFFFFFFu, rd32(hdr + 4));      /* real size is in ds64 */
    ASSERT_MEM_EQ("WAVE", hdr + 8, 4);
    ASSERT_MEM_EQ("ds64", hdr + 12, 4);             /* JUNK promoted in place */
    ASSERT_EQ_U64(28, rd32(hdr + 16));
    ASSERT_EQ_U64(108ull + big, rd64(hdr + 20));    /* riffSize            */
    ASSERT_EQ_U64(big, rd64(hdr + 28));             /* dataSize            */
    ASSERT_EQ_U64(big / 8ull, rd64(hdr + 36));      /* sampleCount, frames */
    ASSERT_EQ_U64(0, rd32(hdr + 44));               /* no chunk table      */
    ASSERT_EQ_U64(0xFFFFFFFFu, rd32(hdr + 104));    /* fact defers to ds64 */
    ASSERT_MEM_EQ("data", hdr + 108, 4);
    ASSERT_EQ_U64(0xFFFFFFFFu, rd32(hdr + 112));

    /* fmt is untouched by the promotion: the payload does not change. */
    ASSERT_MEM_EQ(k_golden_48k_stereo_empty + 48, hdr + 48, 48);

    /* One byte under the limit is still plain RIFF. */
    (void)apr_wav_header_build(hdr, 48000, 2, 0xFFFFFFFEull - 108ull);
    ASSERT_MEM_EQ("RIFF", hdr + 0, 4);
    ASSERT_MEM_EQ("JUNK", hdr + 12, 4);
    ASSERT_EQ_U64(0xFFFFFFFEu, rd32(hdr + 4));
}

/* ---- payloads ------------------------------------------------------------ */

/* Record `frames` of a known signal in `block` sized calls, then assert the
 * file is byte-exact end to end. */
static void roundtrip(const wchar_t *tag, uint32_t rate, uint16_t ch,
                      size_t frames, size_t block)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st      = NULL;
    float          *pcm     = NULL;
    unsigned char  *file    = NULL;
    size_t          len     = 0;
    size_t          payload = frames * ch * sizeof(float);
    unsigned char   want_hdr[WAV_HDR_BYTES];
    AprErr          e;

    tmp_path(path, MAX_PATH, tag);
    cfg = cfg_of(path, rate, ch);

    pcm = (float *)malloc(payload ? payload : 1);
    ASSERT_NOT_NULL(pcm);
    fill_signal(pcm, frames * ch, (uint32_t)(frames * 2654435761u + ch));

    e = apr_action_wav.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));

    e = feed(st, pcm, frames, ch, block);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_action_wav.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_wav.destroy(st);

    (void)apr_wav_header_build(want_hdr, rate, ch, payload);

    file = slurp(path, &len);
    ASSERT_NOT_NULL(file);
    ASSERT_EQ_U64(WAV_HDR_BYTES + payload, len);
    ASSERT_MEM_EQ(want_hdr, file, WAV_HDR_BYTES);
    ASSERT_MEM_EQ(pcm, file + WAV_HDR_BYTES, payload);

    free(file);
    free(pcm);
    DeleteFileW(path);
}

TEST(one_frame_roundtrips_bit_exactly)
{
    roundtrip(L"one", 48000, 2, 1, 1);
}

TEST(sizes_around_the_writers_staging_chunk_roundtrip)
{
    /* The writer stages 64 KiB at a time, which is 8192 frames of stereo
     * float32. Sizes either side of that boundary, and of twice it, are where
     * an off-by-one in the drain loop shows up. */
    roundtrip(L"b8191",  48000, 2, 8191,  4096);
    roundtrip(L"b8192",  48000, 2, 8192,  8192);
    roundtrip(L"b8193",  48000, 2, 8193,  8193);
    roundtrip(L"b16384", 48000, 2, 16384, 8192);
    roundtrip(L"b16385", 48000, 2, 16385, 1024);
}

TEST(a_large_recording_roundtrips_bit_exactly)
{
    /* 200,000 frames is 1.6 MB of payload and stays inside the write-behind
     * buffer, so nothing may be lost even if the disk never runs. */
    roundtrip(L"large", 48000, 2, 200000, 480);
}

TEST(mono_and_multichannel_roundtrip)
{
    roundtrip(L"mono", 44100, 1, 4321, 512);
    roundtrip(L"six",  96000, 6, 3000, 333);
}

/* ---- the constraint: on_audio must not wait for the disk ----------------- */

typedef struct FeedJob {
    void        *st;
    const float *pcm;
    size_t       frames;
    size_t       block;
    uint16_t     ch;
    LONGLONG     worst_ticks;
    LONG         failed;
} FeedJob;

static DWORD WINAPI feed_thread(LPVOID param)
{
    FeedJob *j    = (FeedJob *)param;
    size_t   done = 0;

    while (done < j->frames) {
        LARGE_INTEGER a, b;
        size_t        n = j->frames - done;
        AprErr        e;
        if (n > j->block) n = j->block;
        QueryPerformanceCounter(&a);
        e = apr_action_wav.on_audio(j->st, j->pcm + done * j->ch, n, 0);
        QueryPerformanceCounter(&b);
        if (b.QuadPart - a.QuadPart > j->worst_ticks)
            j->worst_ticks = b.QuadPart - a.QuadPart;
        if (apr_failed(&e)) j->failed = 1;
        done += n;
    }
    return 0;
}

TEST(on_audio_never_waits_for_the_disk)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st   = NULL;
    float          *pcm  = NULL;
    HANDLE          gate = NULL, thread = NULL;
    FeedJob         job;
    LARGE_INTEGER   freq;
    DWORD           waited;
    uint64_t        size_while_stalled = 0;
    unsigned char  *file = NULL;
    unsigned char   want_hdr[WAV_HDR_BYTES];
    size_t          len     = 0;
    const size_t    frames  = 48000;             /* one second */
    const size_t    payload = 48000 * 2 * sizeof(float);
    double          worst_ms;
    AprErr          e;

    QueryPerformanceFrequency(&freq);
    tmp_path(path, MAX_PATH, L"noblock");
    cfg = cfg_of(path, 48000, 2);

    pcm = (float *)malloc(payload);
    ASSERT_NOT_NULL(pcm);
    fill_signal(pcm, frames * 2, 0xC0FFEEu);

    /* Manual-reset, starts closed: every disk write the writer thread attempts
     * blocks here until this test says otherwise. */
    gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    ASSERT_NOT_NULL(gate);
    apr_wav_test_write_gate = gate;

    e = apr_action_wav.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));

    memset(&job, 0, sizeof job);
    job.st = st; job.pcm = pcm; job.frames = frames; job.block = 480; job.ch = 2;

    thread = CreateThread(NULL, 0, feed_thread, &job, 0, NULL);
    ASSERT_NOT_NULL(thread);

    /* Five seconds is 5x the audio being fed, and infinitely less than the
     * gate will ever open on its own. */
    waited = WaitForSingleObject(thread, 5000);
    size_while_stalled = file_size(path);

    SetEvent(gate);                     /* release the disk, whatever happened */
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);

    e = apr_action_wav.finalize(st);
    apr_wav_test_write_gate = NULL;
    CloseHandle(gate);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_wav.destroy(st);

    /* The whole point: a second of audio was accepted while not one payload
     * byte could reach the disk. */
    ASSERT_EQ_INT(WAIT_OBJECT_0, waited);
    ASSERT_EQ_INT(0, job.failed);
    ASSERT_EQ_U64(WAV_HDR_BYTES, size_while_stalled);

    /* And it was not merely finite: no single call came near a disk latency. */
    worst_ms = 1000.0 * (double)job.worst_ticks / (double)freq.QuadPart;
    ASSERT_TRUE(worst_ms < 50.0);

    /* Nothing was lost while the disk was shut. */
    (void)apr_wav_header_build(want_hdr, 48000, 2, payload);
    file = slurp(path, &len);
    ASSERT_NOT_NULL(file);
    ASSERT_EQ_U64(WAV_HDR_BYTES + payload, len);
    ASSERT_MEM_EQ(want_hdr, file, WAV_HDR_BYTES);
    ASSERT_MEM_EQ(pcm, file + WAV_HDR_BYTES, payload);

    free(file);
    free(pcm);
    DeleteFileW(path);
}

TEST(an_overrun_becomes_silence_so_the_timeline_survives)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st   = NULL;
    float          *pcm  = NULL;
    HANDLE          gate = NULL;
    unsigned char  *file = NULL;
    size_t          len     = 0;
    const size_t    frames  = 400000;   /* comfortably past the write-behind */
    const size_t    payload = 400000 * 2 * sizeof(float);
    const size_t    tail    = 100000;   /* frames certain to still be buffered */
    size_t          i, zeros = 0;
    AprErr          e;

    tmp_path(path, MAX_PATH, L"overrun");
    cfg = cfg_of(path, 48000, 2);
    pcm = (float *)malloc(payload);
    ASSERT_NOT_NULL(pcm);
    fill_signal(pcm, frames * 2, 0xBEEF01u);

    gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    ASSERT_NOT_NULL(gate);
    apr_wav_test_write_gate = gate;

    e = apr_action_wav.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));

    /* The disk is shut for all of this: far more audio than the buffer holds. */
    e = feed(st, pcm, frames, 2, 480);
    ASSERT_FALSE(apr_failed(&e));

    SetEvent(gate);
    e = apr_action_wav.finalize(st);
    apr_wav_test_write_gate = NULL;
    CloseHandle(gate);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_wav.destroy(st);

    file = slurp(path, &len);
    ASSERT_NOT_NULL(file);

    /* Every frame is accounted for: what was dropped became silence, so the
     * file still lines up sample for sample with every other bus. */
    ASSERT_EQ_U64(WAV_HDR_BYTES + payload, len);
    ASSERT_EQ_U64(frames, rd32(file + 104));
    ASSERT_EQ_U64(payload, rd32(file + 112));

    /* The most recent audio is the audio that survived, bit for bit. */
    ASSERT_MEM_EQ(pcm + (frames - tail) * 2,
                  file + WAV_HDR_BYTES + (frames - tail) * 2 * sizeof(float),
                  tail * 2 * sizeof(float));

    /* And the hole really is silence -- the input never contains a zero. */
    for (i = 0; i < frames * 2; i++) {
        if (rd32(file + WAV_HDR_BYTES + i * 4) == 0) zeros++;
    }
    ASSERT_GT_INT(0, (long long)zeros);

    free(file);
    free(pcm);
    DeleteFileW(path);
}

/* ---- surviving a finalize that never happens ----------------------------- */

TEST(the_header_is_patched_while_recording)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st  = NULL;
    float          *pcm = NULL;
    unsigned char   hdr[WAV_HDR_BYTES];
    const size_t    frames   = 72000;         /* 1.5 seconds */
    const size_t    payload  = 72000 * 2 * sizeof(float);
    uint32_t        declared = 0;
    int             i;
    AprErr          e;

    tmp_path(path, MAX_PATH, L"patched");
    cfg = cfg_of(path, 48000, 2);
    pcm = (float *)malloc(payload);
    ASSERT_NOT_NULL(pcm);
    fill_signal(pcm, frames * 2, 0x5EED01u);

    e = apr_action_wav.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    e = feed(st, pcm, frames, 2, 480);
    ASSERT_FALSE(apr_failed(&e));

    /* Without finalizing -- as if the process were about to be killed -- the
     * header on disk must already describe real audio. Polled, not slept on,
     * so a fast machine finishes immediately. */
    for (i = 0; i < 300; i++) {
        if (peek_header(path, hdr)) {
            declared = rd32(hdr + 112);
            if (declared > 0) break;
        }
        Sleep(10);
    }

    ASSERT_GT_INT(0, (long long)declared);
    ASSERT_EQ_U64(108u + declared, rd32(hdr + 4));
    ASSERT_EQ_U64(declared / 8u, rd32(hdr + 104));
    ASSERT_LE_INT(file_size(path), (long long)(WAV_HDR_BYTES + declared));

    e = apr_action_wav.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_wav.destroy(st);

    ASSERT_EQ_U64(WAV_HDR_BYTES + payload, file_size(path));
    free(pcm);
    DeleteFileW(path);
}

TEST(destroy_without_finalize_still_leaves_a_playable_file)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st   = NULL;
    float          *pcm  = NULL;
    unsigned char  *file = NULL;
    size_t          len     = 0;
    const size_t    frames  = 5000;
    const size_t    payload = 5000 * 2 * sizeof(float);
    AprErr          e;

    tmp_path(path, MAX_PATH, L"nofinal");
    cfg = cfg_of(path, 48000, 2);
    pcm = (float *)malloc(payload);
    ASSERT_NOT_NULL(pcm);
    fill_signal(pcm, frames * 2, 0x1234u);

    e = apr_action_wav.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    e = feed(st, pcm, frames, 2, 1000);
    ASSERT_FALSE(apr_failed(&e));

    apr_action_wav.destroy(st);        /* no finalize at all */

    file = slurp(path, &len);
    ASSERT_NOT_NULL(file);
    ASSERT_EQ_U64(WAV_HDR_BYTES + payload, len);
    ASSERT_EQ_U64(payload, rd32(file + 112));
    ASSERT_MEM_EQ(pcm, file + WAV_HDR_BYTES, payload);

    free(file);
    free(pcm);
    DeleteFileW(path);
}

TEST(finalize_is_idempotent)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = NULL;
    AprErr          e;

    tmp_path(path, MAX_PATH, L"twice");
    cfg = cfg_of(path, 48000, 2);
    e = apr_action_wav.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_action_wav.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_action_wav.finalize(st);
    ASSERT_FALSE(apr_failed(&e));

    apr_action_wav.destroy(st);
    ASSERT_EQ_U64(WAV_HDR_BYTES, file_size(path));
    DeleteFileW(path);
}

/* ---- error paths --------------------------------------------------------- */

TEST(finalize_after_a_write_error_leaves_a_playable_file)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st   = NULL;
    float          *pcm  = NULL;
    unsigned char  *file = NULL;
    size_t          len     = 0;
    const size_t    frames  = 10000;
    const size_t    payload = 10000 * 2 * sizeof(float);
    AprErr          e;

    tmp_path(path, MAX_PATH, L"ioerr");
    cfg = cfg_of(path, 48000, 2);
    pcm = (float *)malloc(payload);
    ASSERT_NOT_NULL(pcm);
    fill_signal(pcm, frames * 2, 0xDEADu);

    /* Every payload write fails, from the very first one. */
    apr_wav_test_fail_after_bytes = 1;

    e = apr_action_wav.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    (void)feed(st, pcm, frames, 2, 512);

    e = apr_action_wav.finalize(st);
    apr_wav_test_fail_after_bytes = 0;

    /* The failure is reported ... */
    ASSERT_TRUE(apr_failed(&e));
    apr_action_wav.destroy(st);

    /* ... and what is on disk is still a valid, if empty, WAV. */
    file = slurp(path, &len);
    ASSERT_NOT_NULL(file);
    ASSERT_EQ_U64(WAV_HDR_BYTES, len);
    ASSERT_MEM_EQ(k_golden_48k_stereo_empty, file, WAV_HDR_BYTES);

    free(file);
    free(pcm);
    DeleteFileW(path);
}

TEST(a_partial_recording_keeps_its_prefix_exactly)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st   = NULL;
    float          *pcm  = NULL;
    unsigned char  *file = NULL;
    unsigned char   want_hdr[WAV_HDR_BYTES];
    size_t          len = 0;
    uint32_t        declared;
    const size_t    frames  = 40000;              /* 320 KB of payload */
    const size_t    payload = 40000 * 2 * sizeof(float);
    /* Comfortably more than one 64 KiB staging write, comfortably less than
     * the whole recording: some audio has to land, and some has to be lost. */
    const LONG64    limit   = 200000;
    AprErr          e;

    tmp_path(path, MAX_PATH, L"partial");
    cfg = cfg_of(path, 48000, 2);
    pcm = (float *)malloc(payload);
    ASSERT_NOT_NULL(pcm);
    fill_signal(pcm, frames * 2, 0xFACEu);

    /* The disk takes ~200 KB and then refuses, as a full volume would. */
    apr_wav_test_fail_after_bytes = limit;

    e = apr_action_wav.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    (void)feed(st, pcm, frames, 2, 512);
    e = apr_action_wav.finalize(st);
    apr_wav_test_fail_after_bytes = 0;
    ASSERT_TRUE(apr_failed(&e));
    apr_action_wav.destroy(st);

    file = slurp(path, &len);
    ASSERT_NOT_NULL(file);
    declared = rd32(file + 112);

    /* Some audio landed, no more than the disk accepted, and the header says
     * exactly how much -- no trailing partial frame, no over-claim. */
    ASSERT_GT_INT(0, (long long)declared);
    ASSERT_LE_INT(limit, (long long)declared);
    ASSERT_EQ_U64(0, declared % 8);
    ASSERT_EQ_U64(WAV_HDR_BYTES + declared, len);

    (void)apr_wav_header_build(want_hdr, 48000, 2, declared);
    ASSERT_MEM_EQ(want_hdr, file, WAV_HDR_BYTES);
    ASSERT_MEM_EQ(pcm, file + WAV_HDR_BYTES, declared);

    free(file);
    free(pcm);
    DeleteFileW(path);
}

TEST(impossible_configurations_are_refused)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = (void *)(uintptr_t)1;
    AprErr          e;

    tmp_path(path, MAX_PATH, L"badcfg");

    e = apr_action_wav.create(NULL, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(st);

    cfg = cfg_of(path, 48000, 2);
    e = apr_action_wav.create(&cfg, NULL);
    ASSERT_TRUE(apr_failed(&e));

    cfg = cfg_of(NULL, 48000, 2);
    st  = (void *)(uintptr_t)1;
    e = apr_action_wav.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(st);

    cfg = cfg_of(path, 0, 2);
    e = apr_action_wav.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));

    cfg = cfg_of(path, 48000, 0);
    e = apr_action_wav.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));

    /* nBlockAlign is 16 bits: 20000 channels of float32 cannot be described. */
    cfg = cfg_of(path, 48000, 20000);
    e = apr_action_wav.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));

    ASSERT_EQ_U64(0, file_size(path));   /* nothing was created on the way out */

    /* NULL state is a caller bug, not a crash. */
    e = apr_action_wav.on_audio(NULL, NULL, 0, 0);
    ASSERT_TRUE(apr_failed(&e));
    e = apr_action_wav.finalize(NULL);
    ASSERT_TRUE(apr_failed(&e));
    apr_action_wav.destroy(NULL);
}

TEST(an_unopenable_path_is_reported_not_crashed)
{
    AprActionConfig cfg;
    void           *st = (void *)(uintptr_t)1;
    AprErr          e;

    cfg = cfg_of(L"Z:\\apprecorder-no-such-directory\\out.wav", 48000, 2);
    e = apr_action_wav.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(st);
}
