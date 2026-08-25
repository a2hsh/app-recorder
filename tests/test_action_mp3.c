/*
 * test_action_mp3.c -- the MP3 action, end to end.
 *
 * MP3 is lossy, so there is no golden file here and not one bit-exact
 * assertion. What survives an encode is *structure*, and that is what this
 * suite pins:
 *
 *   - the stream decodes at all, at the sample rate and channel count the
 *     session asked for (a wrong-rate file is the classic silent failure: it
 *     plays, it just plays at the wrong speed);
 *   - duration matches what was fed in, within LAME's encoder delay and the
 *     padding of the final frame;
 *   - a 440 Hz tone comes back as 440 Hz and not as a neighbouring bin.
 *
 * Everything is decoded with hip_decode, the mpglib decoder that ships inside
 * libmp3lame. vendor/lame/config.h defines HAVE_MPGLIB without
 * DECODE_ON_THE_FLY precisely so this file can call it while apprecorder.exe
 * never links a byte of it. NOTHING here renders audio to an output device
 * (AGENTS.md rule 1) -- every tone exists only in a buffer and in a file under
 * %TEMP%.
 *
 * Three properties get more than a spot check, because they are the ones a
 * plausible-looking encoder gets wrong:
 *
 *   1. on_audio must never wait for the disk. PROVEN, not asserted, with the
 *      same technique action_wav.c introduced: a test seam holds the writer
 *      thread's disk path shut, a second handle confirms nothing is reaching
 *      the file, and a feeder thread must still push a second of audio through
 *      on_audio inside a bounded wait. An implementation that encodes or
 *      writes on the mixer thread cannot pass.
 *
 *   2. A recording whose process is killed must still play. MP3 is a frame
 *      stream, so this should degrade gracefully -- but "should" is not
 *      evidence. Two tests take the bytes that a kill would leave (once by
 *      reading the file mid-recording, once by truncating a finished file at
 *      an arbitrary offset) and decode them.
 *
 *   3. An overrun must cost a hole, not a desync. Frames the writer never
 *      reached are encoded as silence, so the file stays exactly as long as
 *      the audio that was fed in.
 */
#include "test_runner.h"
#include "action.h"

#include <windows.h>
#include <stdarg.h>

#include "lame.h"

/* The vtable core/registry.c lists under APR_HAVE_ACTION_MP3. Declared here
 * rather than in a public header: registry.c is the only other caller and it
 * declares the same extern. */
extern const AprActionVTable apr_action_mp3;

/* Internal test seams. See the "test seams" section of action_mp3.c for what
 * each does and why it costs a real run nothing. Deliberately absent from
 * every header: nothing but this file may touch them. */
extern volatile HANDLE apr_mp3_test_write_gate;
extern volatile LONG64 apr_mp3_test_fail_after_bytes;

#define TP_PI 3.14159265358979323846

/* One MPEG-1 Layer III granule pair. Encoder delay plus final-frame padding
 * live inside a couple of these, which is the tolerance every duration
 * assertion below is expressed in. */
#define TP_FRAME_SAMPLES 1152

/* ------------------------------------------------------------------------
 * Scaffolding
 * ------------------------------------------------------------------------ */

static void tp_path(wchar_t *buf, size_t cch, const wchar_t *stem)
{
    static LONG counter;
    wchar_t dir[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(buf, cch, _TRUNCATE, L"%lsapr_mp3_%ls_%lu_%ld.mp3",
                 dir, stem, GetCurrentProcessId(), InterlockedIncrement(&counter));
}

static AprActionConfig tp_cfg(const wchar_t *path, uint32_t rate, uint16_t ch)
{
    AprActionConfig c;
    memset(&c, 0, sizeof c);
    c.out_path    = path;
    c.sample_rate = rate;
    c.channels    = ch;
    return c;
}

/* Interleaved sine at `freq`, amplitude 0.5, identical in every channel. */
static float *tp_tone(uint32_t rate, uint16_t channels, size_t frames, double freq)
{
    float *pcm = (float *)malloc(frames * channels * sizeof(float) + sizeof(float));
    size_t i, c;
    if (!pcm) return NULL;
    for (i = 0; i < frames; i++) {
        float v = (float)(0.5 * sin(2.0 * TP_PI * freq * (double)i / (double)rate));
        for (c = 0; c < channels; c++) pcm[i * channels + c] = v;
    }
    return pcm;
}

/* Feed `frames` in `block`-sized calls, as the mixer would. */
static AprErr tp_feed(void *st, const float *pcm, size_t frames, uint16_t ch,
                      size_t block)
{
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done;
        AprErr e;
        if (n > block) n = block;
        e = apr_action_mp3.on_audio(st, pcm + done * ch, n, 0);
        if (apr_failed(&e)) return e;
        done += n;
    }
    return apr_ok();
}

/*
 * Open the write gate and detach it, on the way OUT of a gated test -- the
 * failing way out included.
 *
 * An ASSERT_* returns from the test case immediately, so a gated test that
 * trips one would otherwise leave the gate shut and still installed. The very
 * next test's writer thread then parks in it, and shutdown's INFINITE join
 * parks on that: one failed assertion becomes a hung suite, and a hang hides
 * every result after it. The handle is deliberately NOT closed here -- on the
 * failure path a writer thread may still be inside WaitForSingleObject on it.
 * Leaking a handle in a test that has already failed is the cheap half of that
 * trade.
 */
static void tp_gate_release(HANDLE gate)
{
    apr_mp3_test_write_gate = NULL;
    if (gate) SetEvent(gate);
}

static uint64_t tp_file_size(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return 0;
    return ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

/* Read a file that may still be open for writing. Returns what was there at
 * the instant of the read -- which is exactly what a kill would leave behind,
 * since bytes handed to WriteFile belong to the file, not to the process. */
static unsigned char *tp_slurp(const wchar_t *path, size_t *out_len)
{
    HANDLE         h;
    LARGE_INTEGER  size;
    unsigned char *buf;
    size_t         total = 0, want;

    *out_len = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return NULL; }

    want = (size_t)size.QuadPart;
    buf  = (unsigned char *)malloc(want + 1);
    if (!buf) { CloseHandle(h); return NULL; }

    while (total < want) {
        size_t left = want - total;
        DWORD  ask  = (left > (1u << 20)) ? (1u << 20) : (DWORD)left;
        DWORD  got  = 0;
        if (!ReadFile(h, buf + total, ask, &got, NULL) || got == 0) break;
        total += got;
    }
    CloseHandle(h);
    *out_len = total;
    return buf;
}

/* ------------------------------------------------------------------------
 * Decoding, through libmp3lame's own mpglib
 * ------------------------------------------------------------------------ */

typedef struct TpPcm {
    short *l, *r;
    size_t frames, cap;
    int    rate;
    int    channels;
    int    tag_frames;   /* frames the Xing/LAME tag claims; 0 = no tag found */
    int    decode_error; /* mpglib refused somewhere; earlier output is kept  */
} TpPcm;

static void tp_pcm_free(TpPcm *p)
{
    free(p->l);
    free(p->r);
    memset(p, 0, sizeof *p);
}

static int tp_pcm_push(TpPcm *p, const short *l, const short *r, int n)
{
    if (p->frames + (size_t)n > p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 65536;
        short *nl, *nr;
        while (cap < p->frames + (size_t)n) cap *= 2;
        nl = (short *)realloc(p->l, cap * sizeof(short));
        nr = (short *)realloc(p->r, cap * sizeof(short));
        if (nl) p->l = nl;
        if (nr) p->r = nr;
        if (!nl || !nr) return 0;
        p->cap = cap;
    }
    memcpy(p->l + p->frames, l, (size_t)n * sizeof(short));
    memcpy(p->r + p->frames, r, (size_t)n * sizeof(short));
    p->frames += (size_t)n;
    return 1;
}

/* mpglib narrates truncated streams on stderr by default. A killed recording
 * is a truncated stream on purpose here, so silence it -- a passing test that
 * prints "bitstream problem" reads as a failure. */
static void tp_quiet(const char *fmt, va_list ap) { (void)fmt; (void)ap; }

/* Decode a whole MP3 held in memory. Never fails the test itself: a stream
 * that stops mid-frame sets decode_error and keeps everything decoded up to
 * that point, which is the interesting case for a killed recording. */
static void tp_decode(const unsigned char *data, size_t len, TpPcm *out)
{
    hip_t          hip;
    mp3data_struct md;
    short          l[2 * TP_FRAME_SAMPLES], r[2 * TP_FRAME_SAMPLES];
    size_t         off = 0;

    memset(out, 0, sizeof *out);
    memset(&md, 0, sizeof md);
    if (!data || len == 0) return;

    hip = hip_decode_init();
    if (!hip) return;
    hip_set_errorf(hip, tp_quiet);
    hip_set_msgf(hip, tp_quiet);
    hip_set_debugf(hip, tp_quiet);

    while (off < len) {
        size_t take = len - off;
        int    n;
        if (take > 1024) take = 1024;
        n = hip_decode1_headers(hip, (unsigned char *)(uintptr_t)(data + off),
                                take, l, r, &md);
        off += take;
        while (n > 0) {
            if (!tp_pcm_push(out, l, r, n)) { n = -1; break; }
            n = hip_decode1_headers(hip, (unsigned char *)(uintptr_t)(data + off),
                                    0, l, r, &md);
        }
        if (n < 0) { out->decode_error = 1; break; }
    }

    hip_decode_exit(hip);

    out->rate       = md.samplerate;
    out->channels   = md.stereo;
    out->tag_frames = md.totalframes;
}

static void tp_decode_file(const wchar_t *path, TpPcm *out)
{
    size_t         len = 0;
    unsigned char *buf = tp_slurp(path, &len);
    memset(out, 0, sizeof *out);
    if (!buf) return;
    tp_decode(buf, len, out);
    free(buf);
}

/* Goertzel power at `freq` over `n` samples. Only relative magnitudes matter. */
static double tp_goertzel(const short *x, size_t n, double freq, double rate)
{
    double w = 2.0 * TP_PI * freq / rate;
    double c = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        double s0 = (double)x[i] / 32768.0 + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/* The strongest 1 Hz bin in [lo, hi] over a window that starts well past the
 * encoder delay. */
static double tp_peak_hz(const TpPcm *p, double lo, double hi)
{
    size_t start = (size_t)p->rate / 4;      /* skip 250 ms of settling */
    size_t win;
    double best_f = 0.0, best_p = -1.0, f;

    if (p->frames <= start + 4096) return 0.0;
    win = p->frames - start;
    if (win > (size_t)p->rate) win = (size_t)p->rate;

    for (f = lo; f <= hi; f += 1.0) {
        double pw = tp_goertzel(p->l + start, win, f, (double)p->rate);
        if (pw > best_p) { best_p = pw; best_f = f; }
    }
    return best_f;
}

/* Largest absolute sample over [from, to) of the left channel, 0..1. */
static double tp_peak_level(const TpPcm *p, size_t from, size_t to)
{
    double m = 0.0;
    size_t i;
    if (to > p->frames) to = p->frames;
    for (i = from; i < to; i++) {
        double v = fabs((double)p->l[i]) / 32768.0;
        if (v > m) m = v;
    }
    return m;
}

/* Record `seconds` of `freq` and finalize. `quality` picks CBR (0) or VBR. */
static AprErr tp_record_tone(const wchar_t *path, uint32_t rate, uint16_t ch,
                             double seconds, double freq, int kbps, int quality)
{
    AprActionConfig cfg = tp_cfg(path, rate, ch);
    size_t          frames = (size_t)(seconds * rate);
    void           *st = NULL;
    float          *pcm;
    AprErr          e;

    cfg.bitrate_kbps = kbps;
    cfg.quality      = quality;

    pcm = tp_tone(rate, ch, frames, freq);
    if (!pcm) return APR_ERR(APR_E_NO_MEMORY, L"test tone");

    e = apr_action_mp3.create(&cfg, &st);
    if (apr_failed(&e)) { free(pcm); return e; }

    e = tp_feed(st, pcm, frames, ch, 480);
    if (!apr_failed(&e)) e = apr_action_mp3.finalize(st);
    apr_action_mp3.destroy(st);
    free(pcm);
    return e;
}

/* ------------------------------------------------------------------------
 * Identity
 * ------------------------------------------------------------------------ */

TEST(the_vtable_identifies_itself_as_mp3)
{
    ASSERT_STR_EQ("mp3", apr_action_mp3.id);
    ASSERT_WSTR_EQ(L"mp3", apr_action_mp3.extension);
    ASSERT_NOT_NULL(apr_action_mp3.display_name);
    ASSERT_NOT_NULL((void *)(uintptr_t)apr_action_mp3.create);
    ASSERT_NOT_NULL((void *)(uintptr_t)apr_action_mp3.on_audio);
    ASSERT_NOT_NULL((void *)(uintptr_t)apr_action_mp3.finalize);
    ASSERT_NOT_NULL((void *)(uintptr_t)apr_action_mp3.destroy);
}

/* ------------------------------------------------------------------------
 * Round trips
 * ------------------------------------------------------------------------ */

TEST(a_440_hz_tone_comes_back_as_440_hz_at_48k_stereo)
{
    wchar_t path[MAX_PATH];
    TpPcm   pcm;
    AprErr  e;

    tp_path(path, MAX_PATH, L"tone48");
    e = tp_record_tone(path, 48000, 2, 1.0, 440.0, 0, 0);
    ASSERT_FALSE(apr_failed(&e));

    tp_decode_file(path, &pcm);
    ASSERT_GT_INT(0, (long long)pcm.frames);
    ASSERT_EQ_INT(48000, pcm.rate);
    ASSERT_EQ_INT(2, pcm.channels);

    /* Duration: one second, plus at most a couple of frames of encoder delay
     * and final-frame padding. */
    ASSERT_GT_INT(48000 - TP_FRAME_SAMPLES, (long long)pcm.frames);
    ASSERT_LT_INT(48000 + 3 * TP_FRAME_SAMPLES, (long long)pcm.frames);

    /* The LAME tag was written back over the reserved frame at offset 0. */
    ASSERT_GT_INT(0, pcm.tag_frames);

    ASSERT_NEAR(440.0, tp_peak_hz(&pcm, 300.0, 700.0), 2.0);

    tp_pcm_free(&pcm);
    DeleteFileW(path);
}

TEST(a_440_hz_tone_comes_back_as_440_hz_at_44k_mono)
{
    wchar_t path[MAX_PATH];
    TpPcm   pcm;
    AprErr  e;

    tp_path(path, MAX_PATH, L"tone44mono");
    e = tp_record_tone(path, 44100, 1, 1.0, 440.0, 0, 0);
    ASSERT_FALSE(apr_failed(&e));

    tp_decode_file(path, &pcm);
    ASSERT_GT_INT(0, (long long)pcm.frames);
    ASSERT_EQ_INT(44100, pcm.rate);
    ASSERT_EQ_INT(1, pcm.channels);
    ASSERT_GT_INT(44100 - TP_FRAME_SAMPLES, (long long)pcm.frames);
    ASSERT_LT_INT(44100 + 3 * TP_FRAME_SAMPLES, (long long)pcm.frames);
    ASSERT_NEAR(440.0, tp_peak_hz(&pcm, 300.0, 700.0), 2.0);

    tp_pcm_free(&pcm);
    DeleteFileW(path);
}

TEST(a_rate_mp3_cannot_carry_is_resampled_not_refused)
{
    /* MPEG audio tops out at 48 kHz. A 96 kHz session must still record: LAME
     * resamples internally, so the file comes back at 48 kHz with the SAME
     * duration and the same tone. Refusing here would mean a high-rate session
     * silently loses its MP3 bus. */
    wchar_t path[MAX_PATH];
    TpPcm   pcm;
    AprErr  e;

    tp_path(path, MAX_PATH, L"tone96");
    e = tp_record_tone(path, 96000, 2, 1.0, 440.0, 0, 0);
    ASSERT_FALSE(apr_failed(&e));

    tp_decode_file(path, &pcm);
    ASSERT_GT_INT(0, (long long)pcm.frames);
    ASSERT_EQ_INT(48000, pcm.rate);
    ASSERT_GT_INT(48000 - 2 * TP_FRAME_SAMPLES, (long long)pcm.frames);
    ASSERT_LT_INT(48000 + 4 * TP_FRAME_SAMPLES, (long long)pcm.frames);
    ASSERT_NEAR(440.0, tp_peak_hz(&pcm, 300.0, 700.0), 3.0);

    tp_pcm_free(&pcm);
    DeleteFileW(path);
}

TEST(an_explicit_cbr_bitrate_is_honoured)
{
    /* CBR means the file size is arithmetic, not an estimate: two seconds at
     * 128 kbps is 32,000 bytes of frames plus one tag frame. */
    wchar_t  path[MAX_PATH];
    uint64_t bytes;
    AprErr   e;

    tp_path(path, MAX_PATH, L"cbr128");
    e = tp_record_tone(path, 48000, 2, 2.0, 440.0, 128, 0);
    ASSERT_FALSE(apr_failed(&e));

    bytes = tp_file_size(path);
    ASSERT_GT_INT(30000, (long long)bytes);
    ASSERT_LT_INT(35000, (long long)bytes);

    DeleteFileW(path);
}

TEST(vbr_produces_a_playable_file_with_a_seek_table)
{
    wchar_t path[MAX_PATH];
    TpPcm   pcm;
    AprErr  e;

    /* quality 3 == VBR V2. */
    tp_path(path, MAX_PATH, L"vbr");
    e = tp_record_tone(path, 48000, 2, 1.0, 440.0, 0, 3);
    ASSERT_FALSE(apr_failed(&e));

    tp_decode_file(path, &pcm);
    ASSERT_EQ_INT(48000, pcm.rate);
    ASSERT_EQ_INT(2, pcm.channels);
    ASSERT_GT_INT(48000 - TP_FRAME_SAMPLES, (long long)pcm.frames);
    ASSERT_LT_INT(48000 + 3 * TP_FRAME_SAMPLES, (long long)pcm.frames);

    /* Without the Xing tag a VBR file has no seek table and every player
     * reports the wrong duration -- so this assertion is the point of the
     * write-back in finalize, not a detail. */
    ASSERT_GT_INT(0, pcm.tag_frames);

    ASSERT_NEAR(440.0, tp_peak_hz(&pcm, 300.0, 700.0), 2.0);

    tp_pcm_free(&pcm);
    DeleteFileW(path);
}

/* ------------------------------------------------------------------------
 * Lifecycle edges
 * ------------------------------------------------------------------------ */

TEST(a_recording_with_no_frames_still_finalizes_cleanly)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg = tp_cfg(path, 48000, 2);
    void           *st  = NULL;
    TpPcm           pcm;
    AprErr          e;

    tp_path(path, MAX_PATH, L"empty");
    cfg = tp_cfg(path, 48000, 2);

    e = apr_action_mp3.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_NOT_NULL(st);

    e = apr_action_mp3.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_mp3.destroy(st);

    /* Whatever LAME emits for an empty stream, it must not be garbage: it
     * decodes, and it decodes to (almost) nothing. */
    tp_decode_file(path, &pcm);
    ASSERT_LT_INT(2 * TP_FRAME_SAMPLES, (long long)pcm.frames);
    tp_pcm_free(&pcm);
    DeleteFileW(path);
}

TEST(zero_frame_calls_are_a_no_op)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = NULL;
    float           dummy = 0.0f;
    AprErr          e;

    tp_path(path, MAX_PATH, L"zeroframes");
    cfg = tp_cfg(path, 48000, 2);
    e = apr_action_mp3.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_action_mp3.on_audio(st, &dummy, 0, 0);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_action_mp3.on_audio(st, NULL, 0, 0);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_action_mp3.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_mp3.destroy(st);
    DeleteFileW(path);
}

TEST(finalize_is_idempotent_and_closes_the_door)
{
    wchar_t  path[MAX_PATH];
    AprActionConfig cfg;
    void    *st = NULL;
    float   *pcm;
    uint64_t after_first, after_second;
    AprErr   e;

    tp_path(path, MAX_PATH, L"twice");
    cfg = tp_cfg(path, 48000, 2);
    pcm = tp_tone(48000, 2, 24000, 440.0);
    ASSERT_NOT_NULL(pcm);

    e = apr_action_mp3.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    e = tp_feed(st, pcm, 24000, 2, 480);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_action_mp3.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    after_first = tp_file_size(path);

    /* A second finalize must be a no-op, not a second flush appending another
     * tail to a closed file. */
    e = apr_action_mp3.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    after_second = tp_file_size(path);
    ASSERT_EQ_U64(after_first, after_second);

    /* And audio after the door is shut is a caller error, not a crash and not
     * silently accepted into a file nobody will ever flush. */
    e = apr_action_mp3.on_audio(st, pcm, 480, 0);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_U64(after_first, tp_file_size(path));

    apr_action_mp3.destroy(st);
    free(pcm);
    DeleteFileW(path);
}

TEST(destroy_without_finalize_still_leaves_a_playable_file)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = NULL;
    float          *pcm;
    TpPcm           dec;
    AprErr          e;

    tp_path(path, MAX_PATH, L"nofinal");
    cfg = tp_cfg(path, 48000, 2);
    pcm = tp_tone(48000, 2, 48000, 440.0);
    ASSERT_NOT_NULL(pcm);

    e = apr_action_mp3.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    e = tp_feed(st, pcm, 48000, 2, 480);
    ASSERT_FALSE(apr_failed(&e));

    apr_action_mp3.destroy(st);          /* no finalize at all */

    tp_decode_file(path, &dec);
    ASSERT_EQ_INT(48000, dec.rate);
    ASSERT_GT_INT(48000 - TP_FRAME_SAMPLES, (long long)dec.frames);
    ASSERT_NEAR(440.0, tp_peak_hz(&dec, 300.0, 700.0), 2.0);

    tp_pcm_free(&dec);
    free(pcm);
    DeleteFileW(path);
}

/* ------------------------------------------------------------------------
 * Hostile input
 * ------------------------------------------------------------------------ */

TEST(non_finite_input_is_silenced_not_amplified)
{
    /* A NaN reaching LAME's psychoacoustic model poisons an FFT and comes out
     * as noise across the whole spectrum -- in a recorder that is a burst of
     * full-scale hash in the user's ears. Half a second of tone, then half a
     * second of NaN and infinities: the file must still be a second long, the
     * tone half must survive, and the poisoned half must be silent. */
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = NULL;
    float          *pcm;
    TpPcm           dec;
    size_t          i;
    const size_t    frames = 48000, half = 24000;
    AprErr          e;

    tp_path(path, MAX_PATH, L"nonfinite");
    cfg = tp_cfg(path, 48000, 2);
    pcm = tp_tone(48000, 2, frames, 440.0);
    ASSERT_NOT_NULL(pcm);

    for (i = half; i < frames; i++) {
        static const uint32_t bad[4] = {
            0x7FC00000u,  /* quiet NaN     */
            0xFFC00000u,  /* negative NaN  */
            0x7F800000u,  /* +infinity     */
            0xFF800000u   /* -infinity     */
        };
        uint32_t bits = bad[i & 3u];
        memcpy(&pcm[i * 2 + 0], &bits, sizeof bits);
        memcpy(&pcm[i * 2 + 1], &bits, sizeof bits);
    }

    e = apr_action_mp3.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    e = tp_feed(st, pcm, frames, 2, 480);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_action_mp3.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_mp3.destroy(st);

    tp_decode_file(path, &dec);
    ASSERT_EQ_INT(48000, dec.rate);
    ASSERT_GT_INT(frames - TP_FRAME_SAMPLES, (long long)dec.frames);
    ASSERT_LT_INT(frames + 3 * TP_FRAME_SAMPLES, (long long)dec.frames);

    /* The good half is still a real signal ... */
    ASSERT_TRUE(tp_peak_level(&dec, 4800, half - 2400) > 0.25);
    /* ... and the poisoned half is quiet, not full-scale hash. */
    ASSERT_TRUE(tp_peak_level(&dec, half + 4800, frames) < 0.05);

    tp_pcm_free(&dec);
    free(pcm);
    DeleteFileW(path);
}

TEST(impossible_configurations_are_refused)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = (void *)(uintptr_t)1;
    AprErr          e;

    tp_path(path, MAX_PATH, L"badcfg");

    e = apr_action_mp3.create(NULL, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(st);

    cfg = tp_cfg(path, 48000, 2);
    e = apr_action_mp3.create(&cfg, NULL);
    ASSERT_TRUE(apr_failed(&e));

    cfg = tp_cfg(NULL, 48000, 2);
    st  = (void *)(uintptr_t)1;
    e = apr_action_mp3.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(st);

    cfg = tp_cfg(path, 0, 2);
    e = apr_action_mp3.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));

    cfg = tp_cfg(path, 48000, 0);
    e = apr_action_mp3.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));

    /* MP3 has exactly two channel layouts. A 5.1 bus needs a different action
     * or a downmix; it must not get a file that lies about its layout. */
    cfg = tp_cfg(path, 48000, 6);
    e = apr_action_mp3.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));

    ASSERT_EQ_U64(0, tp_file_size(path));   /* nothing created on the way out */

    /* NULL state is a caller bug, not a crash. */
    e = apr_action_mp3.on_audio(NULL, NULL, 0, 0);
    ASSERT_TRUE(apr_failed(&e));
    e = apr_action_mp3.finalize(NULL);
    ASSERT_TRUE(apr_failed(&e));
    apr_action_mp3.destroy(NULL);
}

TEST(an_unopenable_path_is_reported_not_crashed)
{
    AprActionConfig cfg = tp_cfg(L"Z:\\apprecorder-no-such-directory\\out.mp3",
                                 48000, 2);
    void           *st = (void *)(uintptr_t)1;
    AprErr          e = apr_action_mp3.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(st);
}

/* ------------------------------------------------------------------------
 * The constraint: on_audio must not wait for the disk
 * ------------------------------------------------------------------------ */

typedef struct TpFeedJob {
    void        *st;
    const float *pcm;
    size_t       frames;
    size_t       block;
    uint16_t     ch;
    LONGLONG     worst_ticks;
    LONG         failed;
} TpFeedJob;

static DWORD WINAPI tp_feed_thread(LPVOID param)
{
    TpFeedJob *j    = (TpFeedJob *)param;
    size_t     done = 0;

    while (done < j->frames) {
        LARGE_INTEGER a, b;
        size_t        n = j->frames - done;
        AprErr        e;
        if (n > j->block) n = j->block;
        QueryPerformanceCounter(&a);
        e = apr_action_mp3.on_audio(j->st, j->pcm + done * j->ch, n, 0);
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
    TpFeedJob       job;
    LARGE_INTEGER   freq;
    DWORD           waited;
    uint64_t        size_while_stalled;
    TpPcm           dec;
    double          worst_ms;
    const size_t    frames = 48000;          /* one second */
    AprErr          e;

    QueryPerformanceFrequency(&freq);
    tp_path(path, MAX_PATH, L"noblock");
    cfg = tp_cfg(path, 48000, 2);
    pcm = tp_tone(48000, 2, frames, 440.0);
    ASSERT_NOT_NULL(pcm);

    /* Manual-reset, starts closed: every disk write the writer thread attempts
     * blocks here until this test says otherwise. */
    gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    ASSERT_NOT_NULL(gate);
    apr_mp3_test_write_gate = gate;

    e = apr_action_mp3.create(&cfg, &st);
    if (apr_failed(&e)) tp_gate_release(gate);
    ASSERT_FALSE(apr_failed(&e));

    memset(&job, 0, sizeof job);
    job.st = st; job.pcm = pcm; job.frames = frames; job.block = 480; job.ch = 2;

    thread = CreateThread(NULL, 0, tp_feed_thread, &job, 0, NULL);
    if (!thread) tp_gate_release(gate);
    ASSERT_NOT_NULL(thread);

    /* Five seconds is five times the audio being fed, and infinitely less than
     * the gate will ever open on its own. */
    waited = WaitForSingleObject(thread, 5000);
    size_while_stalled = tp_file_size(path);

    SetEvent(gate);                      /* release the disk, whatever happened */
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);

    e = apr_action_mp3.finalize(st);
    apr_mp3_test_write_gate = NULL;
    CloseHandle(gate);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_mp3.destroy(st);

    /* The whole point: a second of audio was accepted while not one byte could
     * reach the disk. */
    ASSERT_EQ_INT(WAIT_OBJECT_0, waited);
    ASSERT_EQ_INT(0, job.failed);
    ASSERT_EQ_U64(0, size_while_stalled);

    /* And not merely finite: no single call came near a disk latency. */
    worst_ms = 1000.0 * (double)job.worst_ticks / (double)freq.QuadPart;
    ASSERT_TRUE(worst_ms < 50.0);

    /* Nothing was lost while the disk was shut: the ring holds four seconds
     * and only one was fed. */
    tp_decode_file(path, &dec);
    ASSERT_EQ_INT(48000, dec.rate);
    ASSERT_GT_INT(frames - TP_FRAME_SAMPLES, (long long)dec.frames);
    ASSERT_LT_INT(frames + 3 * TP_FRAME_SAMPLES, (long long)dec.frames);
    ASSERT_NEAR(440.0, tp_peak_hz(&dec, 300.0, 700.0), 2.0);

    tp_pcm_free(&dec);
    free(pcm);
    DeleteFileW(path);
}

TEST(an_overrun_becomes_silence_so_the_timeline_survives)
{
    /* Twelve seconds through a four-second write-behind with the disk shut.
     * The ring rounds up to 262,144 frames -- 5.46 s -- so more than half the
     * recording is genuinely overwritten before the writer can reach it, and
     * an implementation that simply skips the lost frames would produce a file
     * of 5.46 s rather than 12. Those frames ARE gone; what must not be gone
     * is the time they occupied, or this bus is permanently out of step with
     * every other bus in the session. */
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st   = NULL;
    float          *pcm  = NULL;
    HANDLE          gate = NULL;
    TpPcm           dec;
    const size_t    frames = 12 * 48000;
    AprErr          e;

    tp_path(path, MAX_PATH, L"overrun");
    cfg = tp_cfg(path, 48000, 2);
    pcm = tp_tone(48000, 2, frames, 440.0);
    ASSERT_NOT_NULL(pcm);

    gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    ASSERT_NOT_NULL(gate);
    apr_mp3_test_write_gate = gate;

    e = apr_action_mp3.create(&cfg, &st);
    if (apr_failed(&e)) tp_gate_release(gate);
    ASSERT_FALSE(apr_failed(&e));

    e = tp_feed(st, pcm, frames, 2, 480);
    if (apr_failed(&e)) tp_gate_release(gate);
    ASSERT_FALSE(apr_failed(&e));

    SetEvent(gate);
    e = apr_action_mp3.finalize(st);
    apr_mp3_test_write_gate = NULL;
    CloseHandle(gate);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_mp3.destroy(st);

    tp_decode_file(path, &dec);
    ASSERT_EQ_INT(48000, dec.rate);
    ASSERT_GT_INT(frames - 2 * TP_FRAME_SAMPLES, (long long)dec.frames);
    ASSERT_LT_INT(frames + 4 * TP_FRAME_SAMPLES, (long long)dec.frames);

    /* The tail is the audio that survived, and it is still the tone. */
    ASSERT_TRUE(tp_peak_level(&dec, dec.frames - 48000, dec.frames - 4800) > 0.25);

    tp_pcm_free(&dec);
    free(pcm);
    DeleteFileW(path);
}

/* ------------------------------------------------------------------------
 * Surviving a kill
 * ------------------------------------------------------------------------ */

TEST(the_bytes_on_disk_mid_recording_already_play)
{
    /* Read the file WHILE recording, with no finalize in sight. Bytes handed
     * to WriteFile belong to the file and survive the process, so what this
     * read returns is exactly what a kill -9 would leave behind. */
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st  = NULL;
    float          *pcm = NULL;
    unsigned char  *snapshot = NULL;
    size_t          snap_len = 0;
    TpPcm           dec;
    const size_t    frames = 3 * 48000;
    int             i;
    AprErr          e;

    tp_path(path, MAX_PATH, L"killed");
    cfg = tp_cfg(path, 48000, 2);
    pcm = tp_tone(48000, 2, frames, 440.0);
    ASSERT_NOT_NULL(pcm);

    e = apr_action_mp3.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    e = tp_feed(st, pcm, frames, 2, 480);
    ASSERT_FALSE(apr_failed(&e));

    /* Poll rather than sleep: on a fast machine the encoder is already ahead. */
    for (i = 0; i < 400; i++) {
        free(snapshot);
        snapshot = tp_slurp(path, &snap_len);
        if (snap_len > 40000) break;
        Sleep(10);
    }
    ASSERT_NOT_NULL(snapshot);
    ASSERT_GT_INT(40000, (long long)snap_len);

    /* A frame stream with no Xing tag yet -- which is the graceful degradation
     * MP3 buys us over a container format. It decodes, it is the right rate,
     * and it is the tone. */
    tp_decode(snapshot, snap_len, &dec);
    ASSERT_EQ_INT(48000, dec.rate);
    ASSERT_EQ_INT(2, dec.channels);
    ASSERT_GT_INT(24000, (long long)dec.frames);
    ASSERT_NEAR(440.0, tp_peak_hz(&dec, 300.0, 700.0), 2.0);

    /* Nothing has written the seek table yet, and that is correct: it does not
     * exist until finalize. The file is still playable without it. */
    ASSERT_EQ_INT(0, dec.tag_frames);

    tp_pcm_free(&dec);
    free(snapshot);

    e = apr_action_mp3.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_mp3.destroy(st);
    free(pcm);
    DeleteFileW(path);
}

TEST(a_file_truncated_mid_frame_still_plays_up_to_the_cut)
{
    /* The same guarantee from the other side: chop a finished recording at an
     * offset that lands inside a frame. Everything before the cut must still
     * decode. This is what makes MP3 the format that degrades gracefully --
     * an MP4 truncated the same way is an unopenable file. */
    wchar_t        path[MAX_PATH];
    unsigned char *whole = NULL;
    size_t         len = 0, cut;
    TpPcm          full, part;
    AprErr         e;

    tp_path(path, MAX_PATH, L"trunc");
    e = tp_record_tone(path, 48000, 2, 3.0, 440.0, 128, 0);
    ASSERT_FALSE(apr_failed(&e));

    whole = tp_slurp(path, &len);
    ASSERT_NOT_NULL(whole);
    ASSERT_GT_INT(20000, (long long)len);

    tp_decode(whole, len, &full);
    ASSERT_GT_INT(3 * 48000 - TP_FRAME_SAMPLES, (long long)full.frames);

    /* 37% of the way in, deliberately not on a frame boundary. */
    cut = (len * 37u) / 100u + 7u;
    tp_decode(whole, cut, &part);

    ASSERT_EQ_INT(48000, part.rate);
    ASSERT_EQ_INT(2, part.channels);

    /* Roughly proportional, and never longer than the whole. */
    ASSERT_GT_INT((long long)(full.frames / 4), (long long)part.frames);
    ASSERT_LT_INT((long long)full.frames, (long long)part.frames);

    /* And what survived is the recording, not noise. */
    ASSERT_NEAR(440.0, tp_peak_hz(&part, 300.0, 700.0), 2.0);

    tp_pcm_free(&full);
    tp_pcm_free(&part);
    free(whole);
    DeleteFileW(path);
}

/* ------------------------------------------------------------------------
 * A disk that stops accepting bytes
 * ------------------------------------------------------------------------ */

TEST(a_mid_stream_write_failure_is_reported_and_the_prefix_survives)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st  = NULL;
    float          *pcm = NULL;
    TpPcm           dec;
    uint64_t        bytes;
    const size_t    frames = 4 * 48000;
    const LONG64    limit  = 24000;      /* ~1.5 s at the 128 kbps below */
    AprErr          e;

    tp_path(path, MAX_PATH, L"ioerr");
    cfg = tp_cfg(path, 48000, 2);
    cfg.bitrate_kbps = 128;
    pcm = tp_tone(48000, 2, frames, 440.0);
    ASSERT_NOT_NULL(pcm);

    /* The volume takes ~24 KB and then refuses, as a full disk would. */
    apr_mp3_test_fail_after_bytes = limit;

    e = apr_action_mp3.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    (void)tp_feed(st, pcm, frames, 2, 480);

    e = apr_action_mp3.finalize(st);
    apr_mp3_test_fail_after_bytes = 0;

    /* The failure is reported ... */
    ASSERT_TRUE(apr_failed(&e));
    apr_action_mp3.destroy(st);

    /* ... and what landed is a valid prefix, not a corrupt file. */
    bytes = tp_file_size(path);
    ASSERT_GT_INT(0, (long long)bytes);
    ASSERT_LE_INT(limit, (long long)bytes);

    tp_decode_file(path, &dec);
    ASSERT_EQ_INT(48000, dec.rate);
    ASSERT_GT_INT(TP_FRAME_SAMPLES, (long long)dec.frames);
    ASSERT_NEAR(440.0, tp_peak_hz(&dec, 300.0, 700.0), 3.0);

    tp_pcm_free(&dec);
    free(pcm);
    DeleteFileW(path);
}

TEST(a_disk_that_refuses_from_the_first_byte_fails_without_a_corrupt_file)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st  = NULL;
    float          *pcm = NULL;
    AprErr          e;

    tp_path(path, MAX_PATH, L"ioerr0");
    cfg = tp_cfg(path, 48000, 2);
    pcm = tp_tone(48000, 2, 48000, 440.0);
    ASSERT_NOT_NULL(pcm);

    apr_mp3_test_fail_after_bytes = 1;   /* every write fails */

    e = apr_action_mp3.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    (void)tp_feed(st, pcm, 48000, 2, 480);
    e = apr_action_mp3.finalize(st);
    apr_mp3_test_fail_after_bytes = 0;

    ASSERT_TRUE(apr_failed(&e));
    apr_action_mp3.destroy(st);

    /* An empty file is the honest outcome; a header claiming audio it does not
     * have would not be. */
    ASSERT_EQ_U64(0, tp_file_size(path));

    free(pcm);
    DeleteFileW(path);
}
