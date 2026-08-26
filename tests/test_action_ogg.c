/*
 * test_action_ogg.c -- the Ogg Opus action, end to end.
 *
 * Opus is lossy, so there is no golden file here and not one bit-exact
 * assertion. What survives an encode is *structure*, and that is what this
 * suite pins:
 *
 *   - the stream demuxes and decodes at all, at 48 kHz and at the channel
 *     count the session asked for;
 *   - duration matches what was fed in -- and for Ogg that is stronger than
 *     it was for MP3, because the granule position makes it exact rather than
 *     approximate. The tolerance below is one 20 ms packet, not three frames
 *     of slop;
 *   - a 440 Hz tone comes back as 440 Hz and not as a neighbouring bin, at
 *     48 kHz and through the resampler from 44.1 and 96 kHz.
 *
 * Everything is demuxed with the vendored libogg and decoded with the
 * vendored libopus -- no header outside vendor/ is involved, and nothing
 * apprecorder.exe itself calls. (Unlike LAME, whose decoder is a separate
 * library half that provably stays out of the shipping image, Opus's decoder
 * shares the range coder, the MDCT and the mode tables with its encoder;
 * vendor/opus/PROVENANCE.md says what was and was not measured there.)
 *
 * NOTHING here renders audio to an output device (AGENTS.md rule 1): every
 * tone exists only in a buffer and in a file under %TEMP%.
 *
 * FFPROBE IS THE SECOND OPINION. A decoder written against the same
 * assumptions as the encoder can agree with it and both be wrong, so the
 * files this suite produces are also handed to ffprobe -- an external tool
 * with no stake in this code -- and its verdict on codec, rate, channels and
 * duration is asserted. That matters most for the killed-recording tests: the
 * claim "a truncated Ogg still plays" is worth nothing if the only thing that
 * plays it is us. When ffprobe is not installed those cases say so loudly and
 * skip, rather than silently passing.
 *
 * Four properties get more than a spot check, because they are the ones a
 * plausible-looking encoder gets wrong:
 *
 *   1. on_audio must never wait for the disk, and must not resample or encode
 *      either. PROVEN, not asserted, with the technique action_wav.c
 *      introduced: a test seam holds the writer thread's disk path shut, a
 *      second handle confirms nothing is reaching the file past the two
 *      header pages, and a feeder thread must still push a second of audio
 *      through on_audio inside a bounded wait.
 *
 *   2. A recording whose process is killed must still play. Ogg is a
 *      page-based container so this should degrade gracefully -- but "should"
 *      is not evidence. Two tests take the bytes a kill would leave (once by
 *      reading the file mid-recording, once by truncating a finished file
 *      inside a page) and put them through both decoders.
 *
 *   3. An overrun must cost a hole, not a desync. Frames the writer never
 *      reached are pushed through as silence, so the file stays exactly as
 *      long as the audio that was fed in.
 *
 *   4. Non-finite input must not come back as full-scale noise. This action
 *      deliberately does NOT scrub NaN and infinity -- core/mix.c owns that
 *      for the integer formats and Opus takes float directly -- so what
 *      libopus does with them is a question to measure, not to assume.
 */
#include "test_runner.h"
#include "action.h"

#include <windows.h>

#include <opus.h>
#include <ogg/ogg.h>

/* The vtable core/registry.c lists under APR_HAVE_ACTION_OGG. Declared here
 * rather than in a public header: registry.c is the only other caller and it
 * declares the same extern. */
extern const AprActionVTable apr_action_ogg;

/* Internal test seams. See the "test seams" section of action_ogg.c for what
 * each does and why it costs a real run nothing. Deliberately absent from
 * every header: nothing but this file may touch them. */
extern volatile HANDLE apr_ogg_test_write_gate;
extern volatile LONG64 apr_ogg_test_fail_after_bytes;

#define TO_PI 3.14159265358979323846

/* One Opus packet as action_ogg.c frames them: 20 ms at 48 kHz. Every
 * duration assertion below is expressed in these. */
#define TO_FRAME 960

/* Opus is 48 kHz whatever the session was. */
#define TO_RATE 48000

/* ------------------------------------------------------------------------
 * Scaffolding
 * ------------------------------------------------------------------------ */

static void to_path(wchar_t *buf, size_t cch, const wchar_t *stem)
{
    static LONG counter;
    wchar_t dir[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(buf, cch, _TRUNCATE, L"%lsapr_ogg_%ls_%lu_%ld.opus",
                 dir, stem, GetCurrentProcessId(), InterlockedIncrement(&counter));
}

static AprActionConfig to_cfg(const wchar_t *path, uint32_t rate, uint16_t ch)
{
    AprActionConfig c;
    memset(&c, 0, sizeof c);
    c.out_path    = path;
    c.sample_rate = rate;
    c.channels    = ch;
    return c;
}

/* Interleaved sine at `freq`, amplitude 0.5, identical in every channel. */
static float *to_tone(uint32_t rate, uint16_t channels, size_t frames, double freq)
{
    float *pcm = (float *)malloc(frames * channels * sizeof(float) + sizeof(float));
    size_t i, c;
    if (!pcm) return NULL;
    for (i = 0; i < frames; i++) {
        float v = (float)(0.5 * sin(2.0 * TO_PI * freq * (double)i / (double)rate));
        for (c = 0; c < channels; c++) pcm[i * channels + c] = v;
    }
    return pcm;
}

/* Feed `frames` in `block`-sized calls, as the mixer would. */
static AprErr to_feed(void *st, const float *pcm, size_t frames, uint16_t ch,
                      size_t block)
{
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done;
        AprErr e;
        if (n > block) n = block;
        e = apr_action_ogg.on_audio(st, pcm + done * ch, n, 0);
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
 */
static void to_gate_release(HANDLE gate)
{
    apr_ogg_test_write_gate = NULL;
    if (gate) SetEvent(gate);
}

static uint64_t to_file_size(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return 0;
    return ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

/* Read a file that may still be open for writing. Returns what was there at
 * the instant of the read -- which is exactly what a kill would leave behind,
 * since bytes handed to WriteFile belong to the file, not to the process. */
static unsigned char *to_slurp(const wchar_t *path, size_t *out_len)
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

static int to_spill(const wchar_t *path, const unsigned char *data, size_t len)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD  wrote = 0;
    BOOL   ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(h, data, (DWORD)len, &wrote, NULL);
    CloseHandle(h);
    return ok && wrote == (DWORD)len;
}

/* ------------------------------------------------------------------------
 * Demuxing and decoding, through the vendored libogg and libopus
 * ------------------------------------------------------------------------ */

typedef struct ToPcm {
    float   *pcm;             /* interleaved, `channels` wide, AFTER trimming */
    size_t   frames, cap;
    int      channels;
    int      rate;            /* always 48000 for a valid Ogg Opus stream    */
    int      input_rate;      /* OpusHead's informational original rate      */
    int      pre_skip;
    int      packets;         /* audio packets seen                          */
    int      pages;           /* Ogg pages seen                              */
    int      saw_head;
    int      saw_tags;
    int      saw_eos;
    int      decode_error;
    int64_t  last_granule;
} ToPcm;

static void to_pcm_free(ToPcm *p)
{
    free(p->pcm);
    memset(p, 0, sizeof *p);
}

static int to_pcm_push(ToPcm *p, const float *src, int frames)
{
    size_t need = p->frames + (size_t)frames;
    if (need > p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 65536;
        float *np;
        while (cap < need) cap *= 2;
        np = (float *)realloc(p->pcm, cap * (size_t)p->channels * sizeof(float));
        if (!np) return 0;
        p->pcm = np;
        p->cap = cap;
    }
    memcpy(p->pcm + p->frames * (size_t)p->channels, src,
           (size_t)frames * (size_t)p->channels * sizeof(float));
    p->frames += (size_t)frames;
    return 1;
}

static uint32_t to_u32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Demux an Ogg Opus file held in memory and decode every audio packet.
 *
 * Never fails the test itself: a stream that stops mid-page keeps everything
 * decoded up to that point, which is the interesting case for a killed
 * recording. The output is TRIMMED exactly as a conforming player would trim
 * it -- pre_skip off the front, and the tail cut back to what the last granule
 * position claims -- so `frames` is the duration the file actually has, not
 * the number of samples the codec happened to emit.
 */
static void to_decode(const unsigned char *data, size_t len, ToPcm *out)
{
    ogg_sync_state   oy;
    ogg_stream_state os;
    ogg_page         og;
    ogg_packet       op;
    OpusDecoder     *dec = NULL;
    int              stream_ready = 0, err = OPUS_OK;
    float            buf[5760 * 2];        /* 120 ms stereo, Opus's maximum */
    size_t           off = 0;

    memset(out, 0, sizeof *out);
    memset(&os, 0, sizeof os);          /* /W4 cannot see that stream_ready guards it */
    out->last_granule = -1;
    if (!data || len == 0) return;

    ogg_sync_init(&oy);

    while (off < len) {
        {
            size_t take = len - off;
            char  *dst;
            if (take > 4096) take = 4096;
            dst = ogg_sync_buffer(&oy, (long)take);
            if (!dst) break;
            memcpy(dst, data + off, take);
            ogg_sync_wrote(&oy, (long)take);
            off += take;
        }

        while (ogg_sync_pageout(&oy, &og) == 1) {
            out->pages++;
            if (!stream_ready) {
                if (ogg_page_bos(&og) == 0) continue;
                if (ogg_stream_init(&os, ogg_page_serialno(&og)) != 0) {
                    out->decode_error = 1;
                    goto done;
                }
                stream_ready = 1;
            }
            if (ogg_page_serialno(&og) != os.serialno) continue;
            if (ogg_page_eos(&og)) out->saw_eos = 1;
            if (ogg_stream_pagein(&os, &og) != 0) { out->decode_error = 1; continue; }

            for (;;) {
                int r = ogg_stream_packetout(&os, &op);
                if (r == 0) break;
                /* -1 is "there was a hole, keep going", not "stop": a
                 * truncated file is exactly the case that produces one. */
                if (r < 0) continue;

                if (!out->saw_head) {
                    if (op.bytes < 19 || memcmp(op.packet, "OpusHead", 8) != 0) {
                        out->decode_error = 1;
                        goto done;
                    }
                    out->channels   = op.packet[9];
                    out->pre_skip   = op.packet[10] | (op.packet[11] << 8);
                    out->input_rate = (int)to_u32le(op.packet + 12);
                    out->rate       = TO_RATE;
                    out->saw_head   = 1;
                    dec = opus_decoder_create(TO_RATE, out->channels, &err);
                    if (!dec || err != OPUS_OK) { out->decode_error = 1; goto done; }
                    continue;
                }
                if (!out->saw_tags) {
                    out->saw_tags = (op.bytes >= 8 &&
                                     memcmp(op.packet, "OpusTags", 8) == 0);
                    if (!out->saw_tags) out->decode_error = 1;
                    continue;
                }

                {
                    int n = opus_decode_float(dec, op.packet, (opus_int32)op.bytes,
                                              buf, 5760, 0);
                    if (n < 0) { out->decode_error = 1; goto done; }
                    out->packets++;
                    if (!to_pcm_push(out, buf, n)) { out->decode_error = 1; goto done; }
                }
                if (op.granulepos >= 0) out->last_granule = op.granulepos;
            }
        }
    }

done:
    if (dec) opus_decoder_destroy(dec);
    if (stream_ready) ogg_stream_clear(&os);
    ogg_sync_clear(&oy);

    /* Trim as a player would: drop the encoder's lookahead from the front,
     * and cut the tail back to what the last granule position claims. */
    if (out->frames > 0 && out->channels > 0) {
        size_t skip = (size_t)out->pre_skip;
        size_t keep;

        if (skip > out->frames) skip = out->frames;
        if (skip > 0) {
            memmove(out->pcm, out->pcm + skip * (size_t)out->channels,
                    (out->frames - skip) * (size_t)out->channels * sizeof(float));
            out->frames -= skip;
        }
        if (out->last_granule > (int64_t)out->pre_skip) {
            keep = (size_t)(out->last_granule - out->pre_skip);
            if (keep < out->frames) out->frames = keep;
        }
        else if (out->last_granule >= 0) {
            out->frames = 0;
        }
    }
}

static void to_decode_file(const wchar_t *path, ToPcm *out)
{
    size_t         len = 0;
    unsigned char *buf = to_slurp(path, &len);
    memset(out, 0, sizeof *out);
    out->last_granule = -1;
    if (!buf) return;
    to_decode(buf, len, out);
    free(buf);
}

/* Goertzel power at `freq` over `n` frames of the left channel. Only relative
 * magnitudes matter. */
static double to_goertzel(const float *x, int channels, size_t from, size_t n,
                          double freq, double rate)
{
    double w = 2.0 * TO_PI * freq / rate;
    double c = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        double s0 = (double)x[(from + i) * (size_t)channels] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/* The strongest 1 Hz bin in [lo, hi] over a window that starts well past any
 * settling. */
static double to_peak_hz(const ToPcm *p, double lo, double hi)
{
    size_t start = TO_RATE / 4;              /* skip 250 ms of settling */
    size_t win;
    double best_f = 0.0, best_p = -1.0, f;

    if (p->channels <= 0 || p->frames <= start + 4096) return 0.0;
    win = p->frames - start;
    if (win > (size_t)TO_RATE) win = (size_t)TO_RATE;

    for (f = lo; f <= hi; f += 1.0) {
        double pw = to_goertzel(p->pcm, p->channels, start, win, f, (double)TO_RATE);
        if (pw > best_p) { best_p = pw; best_f = f; }
    }
    return best_f;
}

/* Largest absolute sample over [from, to) of the left channel, 0..1. */
static double to_peak_level(const ToPcm *p, size_t from, size_t to)
{
    double m = 0.0;
    size_t i;
    if (p->channels <= 0) return 0.0;
    if (to > p->frames) to = p->frames;
    for (i = from; i < to; i++) {
        double v = fabs((double)p->pcm[i * (size_t)p->channels]);
        if (v > m) m = v;
    }
    return m;
}

/* Record `seconds` of `freq` and finalize. */
static AprErr to_record_tone(const wchar_t *path, uint32_t rate, uint16_t ch,
                             double seconds, double freq, int kbps, int quality)
{
    AprActionConfig cfg = to_cfg(path, rate, ch);
    size_t          frames = (size_t)(seconds * rate);
    void           *st = NULL;
    float          *pcm;
    AprErr          e;

    cfg.bitrate_kbps = kbps;
    cfg.quality      = quality;

    pcm = to_tone(rate, ch, frames, freq);
    if (!pcm) return APR_ERR(APR_E_NO_MEMORY, L"test tone");

    e = apr_action_ogg.create(&cfg, &st);
    if (apr_failed(&e)) { free(pcm); return e; }

    e = to_feed(st, pcm, frames, ch, 480);
    if (!apr_failed(&e)) e = apr_action_ogg.finalize(st);
    apr_action_ogg.destroy(st);
    free(pcm);
    return e;
}

/* ------------------------------------------------------------------------
 * The second opinion: ffprobe
 *
 * A decoder that shares this suite's assumptions can agree with the encoder
 * and both be wrong. ffprobe has no stake in any of it.
 * ------------------------------------------------------------------------ */

typedef struct ToProbe {
    int    ran;               /* ffprobe was found and produced output */
    char   codec[32];
    int    sample_rate;
    int    channels;
    double duration;          /* seconds; < 0 when ffprobe would not say */
} ToProbe;

static int to_probe_available(void)
{
    static int cached = -1;
    if (cached < 0) {
        FILE *f = _popen("ffprobe -version 2>&1", "r");
        char  line[256];
        cached = 0;
        if (f) {
            if (fgets(line, sizeof line, f) && strstr(line, "ffprobe")) cached = 1;
            _pclose(f);
        }
        if (!cached) {
            printf("      NOTE: ffprobe is not on PATH -- the external checks "
                   "in this case are SKIPPED, not passed.\n");
        }
    }
    return cached;
}

static void to_probe(const wchar_t *path, ToProbe *out)
{
    char  cmd[MAX_PATH * 2 + 256];
    char  narrow[MAX_PATH * 2];
    char  line[512];
    FILE *f;

    memset(out, 0, sizeof *out);
    out->duration = -1.0;
    if (!to_probe_available()) return;

    if (WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, (int)sizeof narrow,
                            NULL, NULL) == 0) {
        return;
    }
    _snprintf_s(cmd, sizeof cmd, _TRUNCATE,
                "ffprobe -v error -select_streams a:0 "
                "-show_entries stream=codec_name,sample_rate,channels "
                "-show_entries format=duration "
                "-of default=noprint_wrappers=1 \"%s\" 2>&1", narrow);

    f = _popen(cmd, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        char *nl;
        if (!eq) continue;
        *eq++ = '\0';
        nl = strpbrk(eq, "\r\n");
        if (nl) *nl = '\0';
        if      (strcmp(line, "codec_name")  == 0) { strncpy_s(out->codec, sizeof out->codec, eq, _TRUNCATE); out->ran = 1; }
        else if (strcmp(line, "sample_rate") == 0) { out->sample_rate = atoi(eq); out->ran = 1; }
        else if (strcmp(line, "channels")    == 0) { out->channels = atoi(eq);    out->ran = 1; }
        else if (strcmp(line, "duration")    == 0 && strcmp(eq, "N/A") != 0) {
            out->duration = atof(eq);
            out->ran = 1;
        }
    }
    _pclose(f);
}

/* ------------------------------------------------------------------------
 * Identity
 * ------------------------------------------------------------------------ */

TEST(the_vtable_identifies_itself_as_ogg)
{
    ASSERT_STR_EQ("ogg", apr_action_ogg.id);
    /* RFC 7845 section 9 recommends .opus for Opus in Ogg, and the registry
     * id and the file extension are separate fields so they may disagree. */
    ASSERT_WSTR_EQ(L"opus", apr_action_ogg.extension);
    ASSERT_NE_INT(0, apr_action_ogg.display_name_id);
    ASSERT_GT_INT(0, (long long)wcslen(apr_str(apr_action_ogg.display_name_id)));
    ASSERT_NOT_NULL((void *)(uintptr_t)apr_action_ogg.create);
    ASSERT_NOT_NULL((void *)(uintptr_t)apr_action_ogg.on_audio);
    ASSERT_NOT_NULL((void *)(uintptr_t)apr_action_ogg.finalize);
    ASSERT_NOT_NULL((void *)(uintptr_t)apr_action_ogg.destroy);
}

/* ------------------------------------------------------------------------
 * Round trips
 * ------------------------------------------------------------------------ */

TEST(a_440_hz_tone_comes_back_as_440_hz_at_48k_stereo)
{
    wchar_t path[MAX_PATH];
    ToPcm   pcm;
    ToProbe pr;
    AprErr  e;

    to_path(path, MAX_PATH, L"tone48");
    e = to_record_tone(path, 48000, 2, 1.0, 440.0, 0, 0);
    ASSERT_FALSE(apr_failed(&e));

    to_decode_file(path, &pcm);
    ASSERT_EQ_INT(0, pcm.decode_error);
    ASSERT_EQ_INT(1, pcm.saw_head);
    ASSERT_EQ_INT(1, pcm.saw_tags);
    ASSERT_EQ_INT(1, pcm.saw_eos);
    ASSERT_EQ_INT(TO_RATE, pcm.rate);
    ASSERT_EQ_INT(2, pcm.channels);
    ASSERT_EQ_INT(48000, pcm.input_rate);

    /* The pre-skip is real, was asked for rather than assumed, and is the
     * usual 6.5 ms. A zero here would mean the file starts early. */
    ASSERT_GT_INT(0, pcm.pre_skip);
    ASSERT_LT_INT(1000, pcm.pre_skip);

    /* Granule positions make duration EXACT, not approximate: one second in,
     * one second out, to within a single 20 ms packet. */
    ASSERT_GT_INT(48000 - TO_FRAME, (long long)pcm.frames);
    ASSERT_LT_INT(48000 + TO_FRAME, (long long)pcm.frames);

    ASSERT_NEAR(440.0, to_peak_hz(&pcm, 300.0, 700.0), 2.0);

    to_probe(path, &pr);
    if (pr.ran) {
        ASSERT_STR_EQ("opus", pr.codec);
        ASSERT_EQ_INT(48000, pr.sample_rate);
        ASSERT_EQ_INT(2, pr.channels);
        ASSERT_NEAR(1.0, pr.duration, 0.05);
    }

    to_pcm_free(&pcm);
    DeleteFileW(path);
}

TEST(a_440_hz_tone_survives_the_resampler_from_44k_mono)
{
    /* 44.1 kHz is the rate Opus cannot encode at all, and it is the most
     * common rate there is. It must not lose the bus: core/resample.c takes
     * it to 48 kHz on the writer thread and the file is a second long with
     * the tone still at 440 Hz. */
    wchar_t path[MAX_PATH];
    ToPcm   pcm;
    ToProbe pr;
    AprErr  e;

    to_path(path, MAX_PATH, L"tone44mono");
    e = to_record_tone(path, 44100, 1, 1.0, 440.0, 0, 0);
    ASSERT_FALSE(apr_failed(&e));

    to_decode_file(path, &pcm);
    ASSERT_EQ_INT(0, pcm.decode_error);
    ASSERT_EQ_INT(TO_RATE, pcm.rate);
    ASSERT_EQ_INT(1, pcm.channels);

    /* OpusHead remembers where the audio came from even though the samples
     * are now at 48 kHz. */
    ASSERT_EQ_INT(44100, pcm.input_rate);

    /* One second of 44.1 kHz audio is one second of 48 kHz audio. */
    ASSERT_GT_INT(48000 - 2 * TO_FRAME, (long long)pcm.frames);
    ASSERT_LT_INT(48000 + 2 * TO_FRAME, (long long)pcm.frames);

    ASSERT_NEAR(440.0, to_peak_hz(&pcm, 300.0, 700.0), 2.0);

    to_probe(path, &pr);
    if (pr.ran) {
        ASSERT_STR_EQ("opus", pr.codec);
        ASSERT_EQ_INT(1, pr.channels);
        ASSERT_NEAR(1.0, pr.duration, 0.06);
    }

    to_pcm_free(&pcm);
    DeleteFileW(path);
}

TEST(a_rate_above_48k_is_resampled_down_not_refused)
{
    wchar_t path[MAX_PATH];
    ToPcm   pcm;
    AprErr  e;

    to_path(path, MAX_PATH, L"tone96");
    e = to_record_tone(path, 96000, 2, 1.0, 440.0, 0, 0);
    ASSERT_FALSE(apr_failed(&e));

    to_decode_file(path, &pcm);
    ASSERT_EQ_INT(0, pcm.decode_error);
    ASSERT_EQ_INT(TO_RATE, pcm.rate);
    ASSERT_EQ_INT(96000, pcm.input_rate);
    ASSERT_GT_INT(48000 - 2 * TO_FRAME, (long long)pcm.frames);
    ASSERT_LT_INT(48000 + 2 * TO_FRAME, (long long)pcm.frames);
    ASSERT_NEAR(440.0, to_peak_hz(&pcm, 300.0, 700.0), 3.0);

    to_pcm_free(&pcm);
    DeleteFileW(path);
}

TEST(an_explicit_bitrate_is_honoured)
{
    /* VBR, so this is not arithmetic the way CBR MP3 was -- but a two-second
     * 64 kbps recording is still around 16 KB and cannot be 100 KB. */
    wchar_t  path[MAX_PATH];
    uint64_t bytes;
    AprErr   e;

    to_path(path, MAX_PATH, L"br64");
    e = to_record_tone(path, 48000, 2, 2.0, 440.0, 64, 0);
    ASSERT_FALSE(apr_failed(&e));

    bytes = to_file_size(path);
    ASSERT_GT_INT(6000, (long long)bytes);
    ASSERT_LT_INT(30000, (long long)bytes);

    DeleteFileW(path);
}

TEST(a_lower_complexity_still_produces_the_same_recording)
{
    /* quality 1..11 maps to Opus complexity 0..10, for a session that would
     * rather spend less CPU per bus. It must change nothing observable about
     * the file's shape. */
    wchar_t path[MAX_PATH];
    ToPcm   pcm;
    AprErr  e;

    to_path(path, MAX_PATH, L"cx0");
    e = to_record_tone(path, 48000, 2, 1.0, 440.0, 0, 1);   /* complexity 0 */
    ASSERT_FALSE(apr_failed(&e));

    to_decode_file(path, &pcm);
    ASSERT_EQ_INT(0, pcm.decode_error);
    ASSERT_EQ_INT(2, pcm.channels);
    ASSERT_GT_INT(48000 - TO_FRAME, (long long)pcm.frames);
    ASSERT_LT_INT(48000 + TO_FRAME, (long long)pcm.frames);
    ASSERT_NEAR(440.0, to_peak_hz(&pcm, 300.0, 700.0), 2.0);

    to_pcm_free(&pcm);
    DeleteFileW(path);
}

/* ------------------------------------------------------------------------
 * Lifecycle edges
 * ------------------------------------------------------------------------ */

TEST(a_recording_with_no_frames_still_finalizes_cleanly)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = NULL;
    ToPcm           pcm;
    ToProbe         pr;
    AprErr          e;

    to_path(path, MAX_PATH, L"empty");
    cfg = to_cfg(path, 48000, 2);

    e = apr_action_ogg.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_NOT_NULL(st);

    e = apr_action_ogg.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_ogg.destroy(st);

    /* Both headers, an end-of-stream page, and no audio. Not garbage, and not
     * a zero-byte file that no tool will identify. */
    to_decode_file(path, &pcm);
    ASSERT_EQ_INT(0, pcm.decode_error);
    ASSERT_EQ_INT(1, pcm.saw_head);
    ASSERT_EQ_INT(1, pcm.saw_tags);
    ASSERT_EQ_INT(1, pcm.saw_eos);
    ASSERT_EQ_INT(0, (long long)pcm.frames);

    to_probe(path, &pr);
    if (pr.ran) ASSERT_STR_EQ("opus", pr.codec);

    to_pcm_free(&pcm);
    DeleteFileW(path);
}

TEST(zero_frame_calls_are_a_no_op)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = NULL;
    float           dummy = 0.0f;
    AprErr          e;

    to_path(path, MAX_PATH, L"zeroframes");
    cfg = to_cfg(path, 48000, 2);
    e = apr_action_ogg.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_action_ogg.on_audio(st, &dummy, 0, 0);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_action_ogg.on_audio(st, NULL, 0, 0);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_action_ogg.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_ogg.destroy(st);
    DeleteFileW(path);
}

TEST(finalize_is_idempotent_and_closes_the_door)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = NULL;
    float          *pcm;
    uint64_t        after_first, after_second;
    AprErr          e;

    to_path(path, MAX_PATH, L"twice");
    cfg = to_cfg(path, 48000, 2);
    pcm = to_tone(48000, 2, 24000, 440.0);
    ASSERT_NOT_NULL(pcm);

    e = apr_action_ogg.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    e = to_feed(st, pcm, 24000, 2, 480);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_action_ogg.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    after_first = to_file_size(path);

    /* A second finalize must be a no-op, not a second end-of-stream page
     * appended to a closed file. */
    e = apr_action_ogg.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    after_second = to_file_size(path);
    ASSERT_EQ_U64(after_first, after_second);

    /* And audio after the door is shut is a caller error, not a crash and not
     * silently accepted into a file nobody will ever flush. */
    e = apr_action_ogg.on_audio(st, pcm, 480, 0);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_U64(after_first, to_file_size(path));

    apr_action_ogg.destroy(st);
    free(pcm);
    DeleteFileW(path);
}

TEST(destroy_without_finalize_still_leaves_a_playable_file)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = NULL;
    float          *pcm;
    ToPcm           dec;
    ToProbe         pr;
    AprErr          e;

    to_path(path, MAX_PATH, L"nofinal");
    cfg = to_cfg(path, 48000, 2);
    pcm = to_tone(48000, 2, 48000, 440.0);
    ASSERT_NOT_NULL(pcm);

    e = apr_action_ogg.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    e = to_feed(st, pcm, 48000, 2, 480);
    ASSERT_FALSE(apr_failed(&e));

    apr_action_ogg.destroy(st);          /* no finalize at all */

    to_decode_file(path, &dec);
    ASSERT_EQ_INT(0, dec.decode_error);
    ASSERT_EQ_INT(1, dec.saw_eos);
    ASSERT_EQ_INT(TO_RATE, dec.rate);
    ASSERT_GT_INT(48000 - TO_FRAME, (long long)dec.frames);
    ASSERT_NEAR(440.0, to_peak_hz(&dec, 300.0, 700.0), 2.0);

    to_probe(path, &pr);
    if (pr.ran) ASSERT_NEAR(1.0, pr.duration, 0.05);

    to_pcm_free(&dec);
    free(pcm);
    DeleteFileW(path);
}

/* ------------------------------------------------------------------------
 * Hostile input
 * ------------------------------------------------------------------------ */

TEST(non_finite_input_does_not_come_back_as_full_scale_noise)
{
    /*
     * This action deliberately scrubs nothing: AGENTS.md rule 4 gives NaN and
     * infinity to core/mix.c, which owns them for the integer formats where
     * the undefined cast is the hazard, and Opus takes float directly so this
     * path never goes near that code. What libopus does with a poisoned
     * sample is therefore a question to MEASURE.
     *
     * Half a second of tone, then half a second of NaNs and infinities. The
     * file must still be a second long, the tone half must survive, and the
     * poisoned half must not be a burst of full-scale hash in the ears of
     * someone wearing headphones.
     */
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = NULL;
    float          *pcm;
    ToPcm           dec;
    size_t          i;
    const size_t    frames = 48000, half = 24000;
    double          bad_peak;
    AprErr          e;

    to_path(path, MAX_PATH, L"nonfinite");
    cfg = to_cfg(path, 48000, 2);
    pcm = to_tone(48000, 2, frames, 440.0);
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

    e = apr_action_ogg.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    e = to_feed(st, pcm, frames, 2, 480);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_action_ogg.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_ogg.destroy(st);

    to_decode_file(path, &dec);
    ASSERT_EQ_INT(0, dec.decode_error);
    ASSERT_EQ_INT(TO_RATE, dec.rate);

    /* The timeline survived: garbage in is still a second of audio out. */
    ASSERT_GT_INT(frames - TO_FRAME, (long long)dec.frames);
    ASSERT_LT_INT(frames + TO_FRAME, (long long)dec.frames);

    /* The good half is still a real signal ... */
    ASSERT_TRUE(to_peak_level(&dec, 4800, half - 2400) > 0.25);

    /*
     * ... and the poisoned half is QUIET. That number is MEASURED, not
     * designed: libopus turns a block of NaNs and infinities into silence by
     * itself, so unlike action_mp3.c -- where a NaN through the psychoacoustic
     * model comes back as full-scale hash and had to be clamped before LAME
     * ever saw it -- this action needs no guard of its own, and AGENTS.md
     * rule 4 gets to stay unbroken.
     *
     * The threshold is the same 0.05 the MP3 suite uses, and it is a real
     * safety property rather than a formality: the author is blind, works in
     * headphones, and a burst of full-scale noise is the failure that matters.
     * If a future libopus ever stops doing this, THIS assertion is what says
     * so, and the answer then is a sanitiser here -- not a wider threshold.
     *
     * A NaN that survived all the way into the decoder's output would fail
     * this outright as well, since a NaN is not < anything.
     */
    bad_peak = to_peak_level(&dec, half + 4800, frames);
    printf("      libopus turned NaN/Inf into a decoded peak of %.8f\n", bad_peak);
    ASSERT_TRUE(bad_peak < 0.05);

    to_pcm_free(&dec);
    free(pcm);
    DeleteFileW(path);
}

TEST(impossible_configurations_are_refused)
{
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = (void *)(uintptr_t)1;
    AprErr          e;

    to_path(path, MAX_PATH, L"badcfg");

    e = apr_action_ogg.create(NULL, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(st);

    cfg = to_cfg(path, 48000, 2);
    e = apr_action_ogg.create(&cfg, NULL);
    ASSERT_TRUE(apr_failed(&e));

    cfg = to_cfg(NULL, 48000, 2);
    st  = (void *)(uintptr_t)1;
    e = apr_action_ogg.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(st);

    cfg = to_cfg(path, 0, 2);
    e = apr_action_ogg.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));

    cfg = to_cfg(path, 48000, 0);
    e = apr_action_ogg.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));

    /* Opus channel mapping family 0 is mono and stereo. A 5.1 bus needs a
     * channel-order decision that belongs to the session. */
    cfg = to_cfg(path, 48000, 6);
    e = apr_action_ogg.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));

    /* Past where the resampler can reach 48 kHz in either direction. */
    cfg = to_cfg(path, 4000, 2);
    e = apr_action_ogg.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));

    cfg = to_cfg(path, 768000, 2);
    e = apr_action_ogg.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));

    /* Outside Opus's bitrate range. */
    cfg = to_cfg(path, 48000, 2);
    cfg.bitrate_kbps = 900;
    e = apr_action_ogg.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));

    ASSERT_EQ_U64(0, to_file_size(path));   /* nothing created on the way out */

    /* NULL state is a caller bug, not a crash. */
    e = apr_action_ogg.on_audio(NULL, NULL, 0, 0);
    ASSERT_TRUE(apr_failed(&e));
    e = apr_action_ogg.finalize(NULL);
    ASSERT_TRUE(apr_failed(&e));
    apr_action_ogg.destroy(NULL);
}

TEST(an_unopenable_path_is_reported_not_crashed)
{
    AprActionConfig cfg = to_cfg(L"Z:\\apprecorder-no-such-directory\\out.opus",
                                 48000, 2);
    void           *st = (void *)(uintptr_t)1;
    AprErr          e = apr_action_ogg.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(st);
}

/* ------------------------------------------------------------------------
 * The constraint: on_audio must not wait for the disk
 * ------------------------------------------------------------------------ */

typedef struct ToFeedJob {
    void        *st;
    const float *pcm;
    size_t       frames;
    size_t       block;
    uint16_t     ch;
    LONGLONG     worst_ticks;
    LONG         failed;
} ToFeedJob;

static DWORD WINAPI to_feed_thread(LPVOID param)
{
    ToFeedJob *j    = (ToFeedJob *)param;
    size_t     done = 0;

    while (done < j->frames) {
        LARGE_INTEGER a, b;
        size_t        n = j->frames - done;
        AprErr        e;
        if (n > j->block) n = j->block;
        QueryPerformanceCounter(&a);
        e = apr_action_ogg.on_audio(j->st, j->pcm + done * j->ch, n, 0);
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
    /*
     * The gate is installed BEFORE create, so it holds even the two header
     * pages -- which means the file stays at zero bytes for the whole run and
     * the assertion below is unambiguous. An implementation that resampled,
     * encoded or wrote on the mixer thread cannot pass this.
     */
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st   = NULL;
    float          *pcm  = NULL;
    HANDLE          gate = NULL, thread = NULL;
    ToFeedJob       job;
    LARGE_INTEGER   freq;
    DWORD           waited;
    uint64_t        size_while_stalled;
    ToPcm           dec;
    double          worst_ms;
    const size_t    frames = 48000;          /* one second */
    AprErr          e;

    QueryPerformanceFrequency(&freq);
    to_path(path, MAX_PATH, L"noblock");
    cfg = to_cfg(path, 48000, 2);
    pcm = to_tone(48000, 2, frames, 440.0);
    ASSERT_NOT_NULL(pcm);

    /* Manual-reset, starts closed: every disk write the action attempts
     * blocks here until this test says otherwise. */
    gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    ASSERT_NOT_NULL(gate);

    /* create() writes the header pages, so let those through first -- the
     * point being proven is about on_audio, and create is allowed to touch
     * the disk because it runs on the caller's thread, not the mixer's. */
    SetEvent(gate);
    apr_ogg_test_write_gate = gate;

    e = apr_action_ogg.create(&cfg, &st);
    if (apr_failed(&e)) to_gate_release(gate);
    ASSERT_FALSE(apr_failed(&e));

    /* Now shut it. From here nothing may reach the disk. */
    ResetEvent(gate);
    size_while_stalled = to_file_size(path);

    memset(&job, 0, sizeof job);
    job.st = st; job.pcm = pcm; job.frames = frames; job.block = 480; job.ch = 2;

    thread = CreateThread(NULL, 0, to_feed_thread, &job, 0, NULL);
    if (!thread) to_gate_release(gate);
    ASSERT_NOT_NULL(thread);

    /* Five seconds is five times the audio being fed, and infinitely less
     * than the gate will ever open on its own. */
    waited = WaitForSingleObject(thread, 5000);

    {
        uint64_t size_after = to_file_size(path);
        SetEvent(gate);                  /* release the disk, whatever happened */
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);

        e = apr_action_ogg.finalize(st);
        apr_ogg_test_write_gate = NULL;
        CloseHandle(gate);
        ASSERT_FALSE(apr_failed(&e));
        apr_action_ogg.destroy(st);

        /* The whole point: a second of audio was accepted while not one byte
         * of it could reach the disk. */
        ASSERT_EQ_INT(WAIT_OBJECT_0, waited);
        ASSERT_EQ_INT(0, job.failed);
        ASSERT_EQ_U64(size_while_stalled, size_after);
    }

    /* And not merely finite: no single call came near a disk latency, or an
     * encoder's, or a resampler's. */
    worst_ms = 1000.0 * (double)job.worst_ticks / (double)freq.QuadPart;
    printf("      worst single on_audio call: %.3f ms\n", worst_ms);
    ASSERT_TRUE(worst_ms < 50.0);

    /* Nothing was lost while the disk was shut: the ring holds four seconds
     * and only one was fed. */
    to_decode_file(path, &dec);
    ASSERT_EQ_INT(0, dec.decode_error);
    ASSERT_EQ_INT(TO_RATE, dec.rate);
    ASSERT_GT_INT(frames - TO_FRAME, (long long)dec.frames);
    ASSERT_LT_INT(frames + TO_FRAME, (long long)dec.frames);
    ASSERT_NEAR(440.0, to_peak_hz(&dec, 300.0, 700.0), 2.0);

    to_pcm_free(&dec);
    free(pcm);
    DeleteFileW(path);
}

TEST(an_overrun_becomes_silence_so_the_timeline_survives)
{
    /* Twelve seconds through a four-second write-behind with the disk shut.
     * The ring rounds up to 262,144 frames -- 5.46 s -- so more than half the
     * recording is genuinely overwritten before the writer can reach it, and
     * an implementation that simply skipped the lost frames would produce a
     * file of 5.46 s rather than 12. Those frames ARE gone; what must not be
     * gone is the time they occupied, or this bus is permanently out of step
     * with every other bus in the session. */
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st   = NULL;
    float          *pcm  = NULL;
    HANDLE          gate = NULL;
    ToPcm           dec;
    ToProbe         pr;
    const size_t    frames = 12 * 48000;
    AprErr          e;

    to_path(path, MAX_PATH, L"overrun");
    cfg = to_cfg(path, 48000, 2);
    pcm = to_tone(48000, 2, frames, 440.0);
    ASSERT_NOT_NULL(pcm);

    gate = CreateEventW(NULL, TRUE, TRUE, NULL);   /* open: create needs it */
    ASSERT_NOT_NULL(gate);
    apr_ogg_test_write_gate = gate;

    e = apr_action_ogg.create(&cfg, &st);
    if (apr_failed(&e)) to_gate_release(gate);
    ASSERT_FALSE(apr_failed(&e));

    ResetEvent(gate);
    e = to_feed(st, pcm, frames, 2, 480);
    if (apr_failed(&e)) to_gate_release(gate);
    ASSERT_FALSE(apr_failed(&e));

    SetEvent(gate);
    e = apr_action_ogg.finalize(st);
    apr_ogg_test_write_gate = NULL;
    CloseHandle(gate);
    /* AND FINALIZE SAYS SO -- see the WAV suite for why it is reported here
     * and not out of on_audio. */
    ASSERT_TRUE(apr_failed(&e));
    apr_action_ogg.destroy(st);

    to_decode_file(path, &dec);
    ASSERT_EQ_INT(0, dec.decode_error);
    ASSERT_EQ_INT(TO_RATE, dec.rate);
    ASSERT_GT_INT(frames - 2 * TO_FRAME, (long long)dec.frames);
    ASSERT_LT_INT(frames + 2 * TO_FRAME, (long long)dec.frames);

    /* The tail is the audio that survived, and it is still the tone. */
    ASSERT_TRUE(to_peak_level(&dec, dec.frames - 48000, dec.frames - 4800) > 0.25);

    to_probe(path, &pr);
    if (pr.ran) ASSERT_NEAR(12.0, pr.duration, 0.1);

    to_pcm_free(&dec);
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
    wchar_t         snap_path[MAX_PATH];
    AprActionConfig cfg;
    void           *st  = NULL;
    float          *pcm = NULL;
    unsigned char  *snapshot = NULL;
    size_t          snap_len = 0;
    ToPcm           dec;
    ToProbe         pr;
    const size_t    frames = 4 * 48000;
    int             i;
    AprErr          e;

    to_path(path, MAX_PATH, L"killed");
    to_path(snap_path, MAX_PATH, L"killedsnap");
    cfg = to_cfg(path, 48000, 2);
    pcm = to_tone(48000, 2, frames, 440.0);
    ASSERT_NOT_NULL(pcm);

    e = apr_action_ogg.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    e = to_feed(st, pcm, frames, 2, 480);
    ASSERT_FALSE(apr_failed(&e));

    /* Poll rather than sleep: on a fast machine the encoder is already ahead. */
    for (i = 0; i < 600; i++) {
        free(snapshot);
        snapshot = to_slurp(path, &snap_len);
        if (snap_len > 12000) break;
        Sleep(10);
    }
    ASSERT_NOT_NULL(snapshot);
    ASSERT_GT_INT(12000, (long long)snap_len);

    /* No end-of-stream page has been written -- there has been no finalize --
     * and the file plays anyway. That is the graceful degradation a page-based
     * container buys, and it is the thing an MP4 cannot do -- which is why
     * this product has an Ogg action and no AAC one (design 8). */
    to_decode(snapshot, snap_len, &dec);
    ASSERT_EQ_INT(1, dec.saw_head);
    ASSERT_EQ_INT(1, dec.saw_tags);
    ASSERT_EQ_INT(0, dec.saw_eos);
    ASSERT_EQ_INT(TO_RATE, dec.rate);
    ASSERT_EQ_INT(2, dec.channels);
    ASSERT_GT_INT(24000, (long long)dec.frames);
    ASSERT_NEAR(440.0, to_peak_hz(&dec, 300.0, 700.0), 2.0);

    /* And ffprobe agrees, which is the part this suite cannot fake. */
    ASSERT_TRUE(to_spill(snap_path, snapshot, snap_len));
    to_probe(snap_path, &pr);
    if (pr.ran) {
        ASSERT_STR_EQ("opus", pr.codec);
        ASSERT_EQ_INT(48000, pr.sample_rate);
        ASSERT_EQ_INT(2, pr.channels);
    }

    to_pcm_free(&dec);
    free(snapshot);

    e = apr_action_ogg.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_ogg.destroy(st);
    free(pcm);
    DeleteFileW(path);
    DeleteFileW(snap_path);
}

TEST(a_file_truncated_mid_page_still_plays_up_to_the_cut)
{
    /*
     * The same guarantee from the other side, and the one the task asks to be
     * checked externally: chop a finished recording at an offset that lands
     * inside a page. Everything before the cut must still decode, and FFPROBE
     * must agree -- because "our decoder can read our file" is not evidence
     * that a killed recording plays.
     */
    wchar_t        path[MAX_PATH];
    wchar_t        cut_path[MAX_PATH];
    unsigned char *whole = NULL;
    size_t         len = 0, cut;
    ToPcm          full, part;
    ToProbe        pr;
    AprErr         e;

    to_path(path, MAX_PATH, L"trunc");
    to_path(cut_path, MAX_PATH, L"truncated");
    e = to_record_tone(path, 48000, 2, 4.0, 440.0, 96, 0);
    ASSERT_FALSE(apr_failed(&e));

    whole = to_slurp(path, &len);
    ASSERT_NOT_NULL(whole);
    ASSERT_GT_INT(20000, (long long)len);

    to_decode(whole, len, &full);
    ASSERT_EQ_INT(0, full.decode_error);
    ASSERT_GT_INT(4 * 48000 - TO_FRAME, (long long)full.frames);

    /* 37% of the way in, deliberately not on a page boundary. */
    cut = (len * 37u) / 100u + 7u;
    to_decode(whole, cut, &part);

    ASSERT_EQ_INT(TO_RATE, part.rate);
    ASSERT_EQ_INT(2, part.channels);
    ASSERT_EQ_INT(0, part.saw_eos);          /* the flag went with the tail */

    /* Roughly proportional, and never longer than the whole. */
    ASSERT_GT_INT((long long)(full.frames / 4), (long long)part.frames);
    ASSERT_LT_INT((long long)full.frames, (long long)part.frames);

    /* And what survived is the recording, not noise. */
    ASSERT_NEAR(440.0, to_peak_hz(&part, 300.0, 700.0), 2.0);

    /* THE EXTERNAL CHECK. ffprobe recovers codec, rate, channels and -- from
     * the last intact page's granule position, with no index to consult -- a
     * duration close to the fraction of the file that is left. */
    ASSERT_TRUE(to_spill(cut_path, whole, cut));
    to_probe(cut_path, &pr);
    if (pr.ran) {
        printf("      ffprobe on the truncated file: codec=%s rate=%d ch=%d "
               "duration=%.3f (whole file was 4.0 s, cut at %u%%)\n",
               pr.codec, pr.sample_rate, pr.channels, pr.duration, 37u);
        ASSERT_STR_EQ("opus", pr.codec);
        ASSERT_EQ_INT(48000, pr.sample_rate);
        ASSERT_EQ_INT(2, pr.channels);
        if (pr.duration >= 0.0) {
            ASSERT_TRUE(pr.duration > 0.5);
            ASSERT_TRUE(pr.duration < 4.0);
        }
    }

    to_pcm_free(&full);
    to_pcm_free(&part);
    free(whole);
    DeleteFileW(path);
    DeleteFileW(cut_path);
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
    ToPcm           dec;
    uint64_t        bytes;
    const size_t    frames = 6 * 48000;
    const LONG64    limit  = 12000;      /* ~1.5 s at the 64 kbps below */
    AprErr          e;

    to_path(path, MAX_PATH, L"ioerr");
    cfg = to_cfg(path, 48000, 2);
    cfg.bitrate_kbps = 64;
    pcm = to_tone(48000, 2, frames, 440.0);
    ASSERT_NOT_NULL(pcm);

    /* The volume takes ~12 KB and then refuses, as a full disk would. */
    apr_ogg_test_fail_after_bytes = limit;

    e = apr_action_ogg.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    (void)to_feed(st, pcm, frames, 2, 480);

    e = apr_action_ogg.finalize(st);
    apr_ogg_test_fail_after_bytes = 0;

    /* The failure is reported ... */
    ASSERT_TRUE(apr_failed(&e));
    apr_action_ogg.destroy(st);

    /* ... and what landed is a valid prefix, not a corrupt file: whole pages
     * only, because ogg_write_page only advances the byte count when the
     * header AND the body have both landed. */
    bytes = to_file_size(path);
    ASSERT_GT_INT(0, (long long)bytes);
    ASSERT_LE_INT(limit, (long long)bytes);

    to_decode_file(path, &dec);
    ASSERT_EQ_INT(TO_RATE, dec.rate);
    ASSERT_EQ_INT(2, dec.channels);
    ASSERT_GT_INT(TO_FRAME, (long long)dec.frames);
    ASSERT_NEAR(440.0, to_peak_hz(&dec, 300.0, 700.0), 3.0);

    to_pcm_free(&dec);
    free(pcm);
    DeleteFileW(path);
}

TEST(a_disk_that_refuses_from_the_first_byte_fails_at_create)
{
    /* Unlike MP3, this action writes its two header pages during create, so a
     * volume that refuses everything is discovered before a single frame of
     * audio has been accepted -- and the caller learns immediately rather
     * than at finalize. Nothing is left behind. */
    wchar_t         path[MAX_PATH];
    AprActionConfig cfg;
    void           *st = (void *)(uintptr_t)1;
    AprErr          e;

    to_path(path, MAX_PATH, L"ioerr0");
    cfg = to_cfg(path, 48000, 2);

    apr_ogg_test_fail_after_bytes = 1;   /* every write fails */
    e = apr_action_ogg.create(&cfg, &st);
    apr_ogg_test_fail_after_bytes = 0;

    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(st);

    /* An empty file is the honest outcome; a header claiming audio it does
     * not have would not be. */
    ASSERT_EQ_U64(0, to_file_size(path));
    DeleteFileW(path);
}

/* ========================================================================
 * Granule positions, and the 6.5 ms that used to stay inside the encoder
 * ====================================================================== */

/* THE SHAPE OF THE BUG (M5), in two halves that hid each other.
 *
 * The granule position was written as `pre_skip + samples fed in`. RFC 7845
 * defines it as the samples DECODABLE so far, and a packet decodes to exactly
 * 960 samples, so after k packets the true number is 960k -- 312 fewer than
 * what was written. EVERY PAGE overstated by 6.5 ms, which is a mid-stream
 * seek landing early. A duration check cannot see that: the last page's error
 * is often absorbed by the padding on the final frame, so the file measures
 * 1.000 s and is still wrong all the way through.
 *
 * And the encoder's lookahead was never flushed, so the last 312 samples fed
 * in were still inside libopus when the stream closed. That one shows up only
 * when the final partial frame leaves less than 312 samples of padding to
 * absorb it -- a remainder above 648 of 960, which is a third of all possible
 * stop positions, and is where "roughly one stop in three" comes from.
 *
 * So there are two checks below, and the file needs both to be honest:
 * granule_overstatement() for the per-page claim, and an exact frame count for
 * the tail.
 */

/* The largest amount by which any packet's granule position exceeds what a
 * decoder will have produced by then. Zero or less is correct; the old code
 * returned +312 on every page. */
static long long to_granule_overstatement(const wchar_t *path, int *out_packets)
{
    ogg_sync_state   oy;
    ogg_stream_state os;
    ogg_page         og;
    ogg_packet       op;
    unsigned char   *data;
    size_t           len = 0, off = 0;
    int              ready = 0, headers = 0, packets = 0;
    long long        worst = -1000000;

    if (out_packets) *out_packets = 0;
    data = to_slurp(path, &len);
    if (!data) return worst;

    memset(&os, 0, sizeof os);
    ogg_sync_init(&oy);
    while (off < len) {
        size_t take = len - off;
        char  *dst;
        if (take > 4096) take = 4096;
        dst = ogg_sync_buffer(&oy, (long)take);
        if (!dst) break;
        memcpy(dst, data + off, take);
        ogg_sync_wrote(&oy, (long)take);
        off += take;

        while (ogg_sync_pageout(&oy, &og) == 1) {
            if (!ready) {
                if (ogg_page_bos(&og) == 0) continue;
                if (ogg_stream_init(&os, ogg_page_serialno(&og)) != 0) goto done;
                ready = 1;
            }
            if (ogg_page_serialno(&og) != os.serialno) continue;
            if (ogg_stream_pagein(&os, &og) != 0) continue;
            for (;;) {
                int r = ogg_stream_packetout(&os, &op);
                if (r == 0) break;
                if (r < 0) continue;
                if (headers < 2) { headers++; continue; }
                packets++;
                if (op.granulepos >= 0) {
                    long long over = (long long)op.granulepos -
                                     (long long)packets * TO_FRAME;
                    if (over > worst) worst = over;
                }
            }
        }
    }
done:
    if (ready) ogg_stream_clear(&os);
    ogg_sync_clear(&oy);
    free(data);
    if (out_packets) *out_packets = packets;
    return worst;
}

static void to_granule_report(const wchar_t *label, const wchar_t *path,
                              const ToPcm *pcm)
{
    int       packets = 0;
    long long over = to_granule_overstatement(path, &packets);

    printf("      %ls: packets=%d decodable=%lld last_granule=%lld "
           "pre_skip=%d frames=%llu worst_page_overstatement=%+lld\n",
           label, packets, (long long)packets * TO_FRAME,
           (long long)pcm->last_granule, pcm->pre_skip,
           (unsigned long long)pcm->frames, over);
}

/* One second in, one second out, and no page claiming a sample the stream does
 * not hold. 48000 is a whole number of packets, so this is the case where the
 * duration comes out right EITHER WAY -- what it catches is the per-page
 * overstatement that a duration check walks straight past. */
TEST(no_page_ever_claims_a_sample_the_stream_does_not_hold)
{
    wchar_t path[MAX_PATH];
    ToPcm   pcm;
    ToProbe pr;
    AprErr  e;

    to_path(path, MAX_PATH, L"exact48");
    e = to_record_tone(path, 48000, 2, 1.0, 440.0, 0, 0);
    ASSERT_FALSE(apr_failed(&e));

    to_decode_file(path, &pcm);
    ASSERT_EQ_INT(0, pcm.decode_error);
    ASSERT_EQ_INT(1, pcm.saw_eos);
    to_granule_report(L"48 kHz, one second", path, &pcm);

    /* THE RFC RULE, on every page and not merely on the last: the old code
     * scored +312 here, which is 6.5 ms of seek error throughout the file. */
    ASSERT_LE_INT(0, to_granule_overstatement(path, NULL));

    ASSERT_EQ_INT(48000, (long long)pcm.frames);
    ASSERT_NEAR(440.0, to_peak_hz(&pcm, 300.0, 700.0), 2.0);

    to_probe(path, &pr);
    if (pr.ran) ASSERT_NEAR(1.0, pr.duration, 0.01);

    to_pcm_free(&pcm);
    DeleteFileW(path);
}

/* THE TAIL, and the length that exposes it.
 *
 * 48900 samples is 50 whole packets and 900 samples of a 51st, so the padding
 * on the final frame is 60 samples -- far less than the 312 the encoder is
 * still holding. Before the flush existed the file claimed 48900 and held
 * 48648: 252 samples, 5.25 ms of the take, gone. Any remainder above 648 of
 * 960 does this, which is a third of all stop positions. */
TEST(the_last_six_milliseconds_do_not_stay_inside_the_encoder)
{
    wchar_t path[MAX_PATH];
    ToPcm   pcm;
    AprErr  e;

    to_path(path, MAX_PATH, L"tail");
    e = to_record_tone(path, 48000, 2, 48900.0 / 48000.0, 440.0, 0, 0);
    ASSERT_FALSE(apr_failed(&e));

    to_decode_file(path, &pcm);
    ASSERT_EQ_INT(0, pcm.decode_error);
    to_granule_report(L"48 kHz, 48900 frames", path, &pcm);

    ASSERT_EQ_INT(48900, (long long)pcm.frames);
    ASSERT_LE_INT(0, to_granule_overstatement(path, NULL));

    to_pcm_free(&pcm);
    DeleteFileW(path);
}

/* m15: the resampler holds half a kernel of lookahead, and finalize primes it
 * with silence so the last few milliseconds of real audio come out. Counting
 * what came out as the recording's LENGTH then included the priming. The
 * length of a recording is the frames that came off the ring, converted --
 * arithmetic that cannot drift -- and never the resampler's output count. */
TEST(the_resamplers_priming_silence_is_not_part_of_the_duration)
{
    wchar_t path[MAX_PATH];
    ToPcm   pcm;
    ToProbe pr;
    AprErr  e;

    to_path(path, MAX_PATH, L"exact44");
    e = to_record_tone(path, 44100, 1, 1.0, 440.0, 0, 0);
    ASSERT_FALSE(apr_failed(&e));

    to_decode_file(path, &pcm);
    ASSERT_EQ_INT(0, pcm.decode_error);
    to_granule_report(L"44.1 kHz, one second", path, &pcm);

    /* 44100 frames at 44.1 kHz is 48000 frames at 48 kHz. To the sample. */
    ASSERT_EQ_INT(48000, (long long)pcm.frames);
    ASSERT_LE_INT(0, to_granule_overstatement(path, NULL));
    ASSERT_NEAR(440.0, to_peak_hz(&pcm, 300.0, 700.0), 2.0);

    to_probe(path, &pr);
    if (pr.ran) ASSERT_NEAR(1.0, pr.duration, 0.01);

    to_pcm_free(&pcm);
    DeleteFileW(path);
}
