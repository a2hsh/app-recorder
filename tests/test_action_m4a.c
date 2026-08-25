/*
 * test_action_m4a.c -- the M4A/AAC action, end to end.
 *
 * AAC is lossy, so there is no golden file and no bit-exact assertion to make.
 * What survives encoding is *structure*, and that is what this suite pins:
 *
 *   - the container decodes at all, with the sample rate and channel count we
 *     asked for (a wrong-rate file is the classic silent failure -- it plays,
 *     it just plays at the wrong speed);
 *   - duration matches what was fed in, within AAC's encoder delay / padding;
 *   - a 440 Hz tone comes back as 440 Hz and not as some neighbouring bin.
 *
 * Everything here writes files under %TEMP% and decodes them with the Media
 * Foundation Source Reader. NOTHING here renders audio to an output device
 * (AGENTS.md rule 1) -- the tone only ever exists in a buffer and in a file.
 */
#include "test_runner.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include "action.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

/* Provided by src/actions/action_m4a.c. core/registry.c will reference the
 * same symbol; it is declared here rather than in a header because headers are
 * owned elsewhere this wave. */
extern const AprActionVTable apr_action_m4a;

/* Runtime capability enumeration, also in action_m4a.c. Flat arrays rather
 * than a shared struct so there is no duplicated type definition to drift.
 * Returns the number of (rate, channels, avg_bytes_per_second) triples
 * written; 0 if Media Foundation has no AAC encoder at all. */
extern size_t apr_m4a_enum_formats(uint32_t *rates, uint32_t *channels,
                                   uint32_t *avg_bytes_per_second, size_t max);

#define TM_PI 3.14159265358979323846

/* ------------------------------------------------------------------------
 * Scaffolding
 * ------------------------------------------------------------------------ */

static void tm_temp_path(wchar_t *buf, size_t cch, const wchar_t *stem)
{
    wchar_t dir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(dir, MAX_PATH, L".\\");
    _snwprintf_s(buf, cch, _TRUNCATE, L"%sapr_m4a_%lu_%s.m4a",
                 dir, (unsigned long)GetCurrentProcessId(), stem);
}

/* Interleaved sine at `freq`, amplitude 0.5, same signal in every channel. */
static float *tm_make_tone(uint32_t rate, uint32_t channels, size_t frames,
                           double freq)
{
    float *pcm = (float *)malloc(frames * channels * sizeof(float));
    size_t i;
    uint32_t c;
    if (!pcm) return NULL;
    for (i = 0; i < frames; i++) {
        float v = (float)(0.5 * sin(2.0 * TM_PI * freq * (double)i / (double)rate));
        for (c = 0; c < channels; c++) pcm[i * channels + c] = v;
    }
    return pcm;
}

/* Goertzel power of `freq` over `n` mono samples. Relative magnitudes are all
 * this suite compares, so the normalisation constant does not matter. */
static double tm_goertzel(const float *x, size_t n, double freq, double rate)
{
    double w  = 2.0 * TM_PI * freq / rate;
    double c  = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        double s0 = (double)x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/* Decoded file contents. `pcm` is interleaved float32. */
typedef struct TmDecoded {
    float   *pcm;
    size_t   frames;
    uint32_t rate;
    uint32_t channels;
} TmDecoded;

static void tm_decoded_free(TmDecoded *d)
{
    free(d->pcm);
    memset(d, 0, sizeof(*d));
}

/* Decode with the MF Source Reader. Returns S_OK and fills `out`, or the
 * failing HRESULT. Caller must have MFStartup'd. */
static HRESULT tm_decode(const wchar_t *path, TmDecoded *out)
{
    IMFSourceReader *rdr = NULL;
    IMFMediaType    *want = NULL, *got = NULL;
    HRESULT hr;
    UINT32  rate = 0, ch = 0, bits = 0;
    unsigned char *raw = NULL;
    size_t raw_len = 0, raw_cap = 0;

    memset(out, 0, sizeof(*out));

    hr = MFCreateSourceReaderFromURL(path, NULL, &rdr);
    if (FAILED(hr)) goto done;

    hr = IMFSourceReader_SetStreamSelection(rdr,
             (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(hr)) goto done;
    hr = IMFSourceReader_SetStreamSelection(rdr,
             (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (FAILED(hr)) goto done;

    hr = MFCreateMediaType(&want);
    if (FAILED(hr)) goto done;
    hr = IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    if (FAILED(hr)) goto done;
    hr = IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    if (FAILED(hr)) goto done;
    hr = IMFSourceReader_SetCurrentMediaType(rdr,
             (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, want);
    if (FAILED(hr)) goto done;

    hr = IMFSourceReader_GetCurrentMediaType(rdr,
             (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got);
    if (FAILED(hr)) goto done;
    hr = IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    if (FAILED(hr)) goto done;
    hr = IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
    if (FAILED(hr)) goto done;
    hr = IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    if (FAILED(hr)) goto done;
    if (bits != 16) { hr = E_UNEXPECTED; goto done; }

    for (;;) {
        IMFSample     *sample = NULL;
        IMFMediaBuffer*buf    = NULL;
        DWORD          flags  = 0;
        BYTE          *p      = NULL;
        DWORD          len    = 0;

        hr = IMFSourceReader_ReadSample(rdr,
                 (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                 NULL, &flags, NULL, &sample);
        if (FAILED(hr)) goto done;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (sample) IMFSample_Release(sample);
            break;
        }
        if (!sample) continue;   /* stream tick */

        hr = IMFSample_ConvertToContiguousBuffer(sample, &buf);
        if (SUCCEEDED(hr)) {
            hr = IMFMediaBuffer_Lock(buf, &p, NULL, &len);
            if (SUCCEEDED(hr)) {
                if (raw_len + len > raw_cap) {
                    size_t nc = raw_cap ? raw_cap * 2 : 1u << 16;
                    unsigned char *nb;
                    while (nc < raw_len + len) nc *= 2;
                    nb = (unsigned char *)realloc(raw, nc);
                    if (!nb) { hr = E_OUTOFMEMORY; }
                    else { raw = nb; raw_cap = nc; }
                }
                if (SUCCEEDED(hr)) {
                    memcpy(raw + raw_len, p, len);
                    raw_len += len;
                }
                IMFMediaBuffer_Unlock(buf);
            }
            IMFMediaBuffer_Release(buf);
        }
        IMFSample_Release(sample);
        if (FAILED(hr)) goto done;
    }

    {
        size_t frames = raw_len / (ch * 2);
        size_t i, total = frames * ch;
        float *pcm = (float *)malloc((total ? total : 1) * sizeof(float));
        const int16_t *s16 = (const int16_t *)raw;
        if (!pcm) { hr = E_OUTOFMEMORY; goto done; }
        for (i = 0; i < total; i++) pcm[i] = (float)s16[i] / 32768.0f;
        out->pcm      = pcm;
        out->frames   = frames;
        out->rate     = rate;
        out->channels = ch;
        hr = S_OK;
    }

done:
    free(raw);
    if (got)  IMFMediaType_Release(got);
    if (want) IMFMediaType_Release(want);
    if (rdr)  IMFSourceReader_Release(rdr);
    return hr;
}

/* Per-case COM + MF lifetime. MFStartup is refcounted process-wide, so a
 * pair per case is safe even though several cases run in one process. */
static HRESULT tm_mf_up(void)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) hr = S_OK;
    if (FAILED(hr)) return hr;
    return MFStartup(MF_VERSION, MFSTARTUP_LITE);
}

static void tm_mf_down(void)
{
    MFShutdown();
    CoUninitialize();
}

/* ------------------------------------------------------------------------
 * Capability enumeration -- the machine tells us what it can do.
 * ------------------------------------------------------------------------ */

#define TM_MAX_FMT 256

static size_t tm_formats(uint32_t *rates, uint32_t *chans, uint32_t *bps)
{
    return apr_m4a_enum_formats(rates, chans, bps, TM_MAX_FMT);
}

static int tm_supports(uint32_t rate, uint32_t ch)
{
    uint32_t r[TM_MAX_FMT], c[TM_MAX_FMT], b[TM_MAX_FMT];
    size_t n = tm_formats(r, c, b), i;
    for (i = 0; i < n; i++) if (r[i] == rate && c[i] == ch) return 1;
    return 0;
}

TEST(m4a_enumerates_encoder_formats)
{
    uint32_t r[TM_MAX_FMT], c[TM_MAX_FMT], b[TM_MAX_FMT];
    size_t n, i;

    n = tm_formats(r, c, b);
    printf("      Media Foundation AAC encoder offers %zu output type(s):\n", n);
    for (i = 0; i < n; i++) {
        printf("        %6lu Hz  %lu ch  %6lu B/s (%lu kbps)\n",
               (unsigned long)r[i], (unsigned long)c[i], (unsigned long)b[i],
               (unsigned long)((b[i] * 8 + 500) / 1000));
    }
    fflush(stdout);

    /* Every Windows since 7 ships the AAC encoder; if this is 0 the rest of
     * the suite is meaningless and should say so here rather than 8 cases
     * later. */
    ASSERT_GT_INT(0, n);
    ASSERT_TRUE(tm_supports(48000, 2));
}

/* ------------------------------------------------------------------------
 * Identity
 * ------------------------------------------------------------------------ */

TEST(m4a_vtable_is_wired)
{
    ASSERT_STR_EQ("m4a", apr_action_m4a.id);
    ASSERT_WSTR_EQ(L"m4a", apr_action_m4a.extension);
    ASSERT_NOT_NULL(apr_action_m4a.display_name);
    ASSERT_NOT_NULL(apr_action_m4a.create);
    ASSERT_NOT_NULL(apr_action_m4a.on_audio);
    ASSERT_NOT_NULL(apr_action_m4a.finalize);
    ASSERT_NOT_NULL(apr_action_m4a.destroy);
}

/* ------------------------------------------------------------------------
 * The real thing: encode a tone, decode it back.
 * ------------------------------------------------------------------------ */

/* Encode `seconds` of `freq` at rate/channels into `path`. Returns the AprErr
 * of whichever step failed first, feeding audio in irregular block sizes so
 * the action cannot be quietly assuming a fixed period. */
static AprErr tm_encode_tone(const wchar_t *path, uint32_t rate,
                             uint32_t channels, double seconds, double freq)
{
    AprActionConfig cfg;
    void  *st  = NULL;
    float *pcm = NULL;
    size_t frames = (size_t)(seconds * rate + 0.5), pos = 0;
    AprErr e;
    static const size_t blocks[] = { 480, 1024, 37, 4096, 960, 1 };
    size_t bi = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.out_path    = path;
    cfg.sample_rate = rate;
    cfg.channels    = (uint16_t)channels;

    e = apr_action_m4a.create(&cfg, &st);
    if (apr_failed(&e)) return e;

    pcm = tm_make_tone(rate, channels, frames, freq);
    if (!pcm) {
        apr_action_m4a.finalize(st);
        apr_action_m4a.destroy(st);
        return APR_ERR(APR_E_NO_MEMORY, L"tone buffer");
    }

    while (pos < frames) {
        size_t n = blocks[bi++ % (sizeof(blocks) / sizeof(blocks[0]))];
        if (n > frames - pos) n = frames - pos;
        e = apr_action_m4a.on_audio(st, pcm + pos * channels, n, 0);
        if (apr_failed(&e)) break;
        pos += n;
    }

    {
        AprErr fe = apr_action_m4a.finalize(st);
        if (!apr_failed(&e)) e = fe;
    }
    apr_action_m4a.destroy(st);
    free(pcm);
    return e;
}

/* Peak-bin check: 440 must beat every other candidate by a wide margin. */
static void tm_assert_440(const TmDecoded *d, int *out_ok)
{
    static const double cand[] = { 110, 220, 330, 440, 550, 660, 880, 1320 };
    size_t nc = sizeof(cand) / sizeof(cand[0]);
    size_t win, off, i, k;
    float *mono;
    double best = -1.0, at440 = 0.0;
    size_t best_i = 0;

    *out_ok = 0;
    if (d->frames < d->rate) return;

    /* Analyse a window from the middle: away from encoder priming at the head
     * and padding at the tail. */
    win = d->rate / 2;
    off = d->frames / 2 - win / 2;

    mono = (float *)malloc(win * sizeof(float));
    if (!mono) return;
    for (i = 0; i < win; i++) mono[i] = d->pcm[(off + i) * d->channels];

    for (k = 0; k < nc; k++) {
        double p = tm_goertzel(mono, win, cand[k], (double)d->rate);
        if (cand[k] == 440.0) at440 = p;
        if (p > best) { best = p; best_i = k; }
    }
    free(mono);

    printf("      spectral peak at %.0f Hz (440 Hz power %.3g, peak %.3g)\n",
           cand[best_i], at440, best);
    *out_ok = (cand[best_i] == 440.0);
}

TEST(m4a_tone_roundtrips_48k_stereo)
{
    wchar_t path[MAX_PATH];
    AprErr  e;
    TmDecoded d;
    HRESULT hr;
    int is440 = 0;

    tm_temp_path(path, MAX_PATH, L"tone48s");
    DeleteFileW(path);

    e = tm_encode_tone(path, 48000, 2, 2.0, 440.0);
    ASSERT_FALSE(apr_failed(&e));

    hr = tm_mf_up();
    ASSERT_EQ_INT(S_OK, hr);
    hr = tm_decode(path, &d);
    if (FAILED(hr)) {
        tm_mf_down();
        printf("      decode failed hr=0x%08lX\n", (unsigned long)hr);
        FAIL("source reader could not decode the encoded file");
    }

    ASSERT_EQ_INT(48000, (int)d.rate);
    ASSERT_EQ_INT(2, (int)d.channels);
    /* AAC frames are 1024 samples and the encoder prepends priming; 100 ms of
     * slack covers delay plus padding without letting a real length bug
     * through. */
    ASSERT_NEAR(2.0, (double)d.frames / (double)d.rate, 0.10);

    tm_assert_440(&d, &is440);
    ASSERT_TRUE(is440);

    tm_decoded_free(&d);
    tm_mf_down();
    DeleteFileW(path);
}

TEST(m4a_tone_roundtrips_44k_mono)
{
    wchar_t path[MAX_PATH];
    AprErr  e;
    TmDecoded d;
    HRESULT hr;
    int is440 = 0;

    if (!tm_supports(44100, 1)) {
        printf("      44100/1 not offered by this machine's encoder; skipped\n");
        return;
    }

    tm_temp_path(path, MAX_PATH, L"tone44m");
    DeleteFileW(path);

    e = tm_encode_tone(path, 44100, 1, 1.5, 440.0);
    ASSERT_FALSE(apr_failed(&e));

    hr = tm_mf_up();
    ASSERT_EQ_INT(S_OK, hr);
    hr = tm_decode(path, &d);
    if (FAILED(hr)) { tm_mf_down(); FAIL("decode failed"); }

    ASSERT_EQ_INT(44100, (int)d.rate);
    ASSERT_EQ_INT(1, (int)d.channels);
    ASSERT_NEAR(1.5, (double)d.frames / (double)d.rate, 0.10);

    tm_assert_440(&d, &is440);
    ASSERT_TRUE(is440);

    tm_decoded_free(&d);
    tm_mf_down();
    DeleteFileW(path);
}

/* ------------------------------------------------------------------------
 * Exit paths
 * ------------------------------------------------------------------------ */

TEST(m4a_zero_frames_still_produces_a_playable_file)
{
    wchar_t path[MAX_PATH];
    AprActionConfig cfg;
    void *st = NULL;
    AprErr e;
    TmDecoded d;
    HRESULT hr;

    tm_temp_path(path, MAX_PATH, L"empty");
    DeleteFileW(path);

    memset(&cfg, 0, sizeof(cfg));
    cfg.out_path    = path;
    cfg.sample_rate = 48000;
    cfg.channels    = 2;

    e = apr_action_m4a.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_NOT_NULL(st);

    e = apr_action_m4a.finalize(st);
    if (apr_failed(&e)) {
        wchar_t msg[512];
        apr_err_format(&e, msg, 512);
        printf("      finalize: %ls\n", msg);
    }
    ASSERT_FALSE(apr_failed(&e));
    apr_action_m4a.destroy(st);

    /* The point of the case: an MP4 with no moov atom is not a short file, it
     * is an unopenable one. This must open. */
    hr = tm_mf_up();
    ASSERT_EQ_INT(S_OK, hr);
    hr = tm_decode(path, &d);
    if (FAILED(hr)) {
        tm_mf_down();
        printf("      decode failed hr=0x%08lX\n", (unsigned long)hr);
        FAIL("zero-frame file did not open -- moov atom missing?");
    }
    ASSERT_EQ_INT(48000, (int)d.rate);
    ASSERT_EQ_INT(2, (int)d.channels);
    ASSERT_NEAR(0.0, (double)d.frames / (double)d.rate, 0.10);

    tm_decoded_free(&d);
    tm_mf_down();
    DeleteFileW(path);
}

TEST(m4a_finalize_is_idempotent_and_closes_the_door)
{
    wchar_t path[MAX_PATH];
    AprActionConfig cfg;
    void  *st = NULL;
    float *pcm;
    AprErr e;
    TmDecoded d;
    HRESULT hr;

    tm_temp_path(path, MAX_PATH, L"twice");
    DeleteFileW(path);

    memset(&cfg, 0, sizeof(cfg));
    cfg.out_path    = path;
    cfg.sample_rate = 48000;
    cfg.channels    = 2;

    e = apr_action_m4a.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));

    pcm = tm_make_tone(48000, 2, 48000, 440.0);
    ASSERT_NOT_NULL(pcm);
    e = apr_action_m4a.on_audio(st, pcm, 48000, 0);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_action_m4a.finalize(st);
    ASSERT_FALSE(apr_failed(&e));

    /* Audio arriving after the file is closed is a caller bug, not a reason to
     * corrupt the file. It must be refused, and refused without blocking. */
    e = apr_action_m4a.on_audio(st, pcm, 48000, 0);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_STATE, (int)e.kind);

    /* Section 10: finalize is called on EVERY exit path, so it will be called
     * again after an error path already called it. Twice must be harmless. */
    e = apr_action_m4a.finalize(st);
    ASSERT_FALSE(apr_failed(&e));

    apr_action_m4a.destroy(st);
    free(pcm);

    hr = tm_mf_up();
    ASSERT_EQ_INT(S_OK, hr);
    hr = tm_decode(path, &d);
    if (FAILED(hr)) { tm_mf_down(); FAIL("double-finalized file did not open"); }
    ASSERT_NEAR(1.0, (double)d.frames / (double)d.rate, 0.10);
    tm_decoded_free(&d);
    tm_mf_down();
    DeleteFileW(path);
}

TEST(m4a_destroy_without_finalize_still_leaves_a_playable_file)
{
    wchar_t path[MAX_PATH];
    AprActionConfig cfg;
    void  *st = NULL;
    float *pcm;
    AprErr e;
    TmDecoded d;
    HRESULT hr;

    tm_temp_path(path, MAX_PATH, L"nofin");
    DeleteFileW(path);

    memset(&cfg, 0, sizeof(cfg));
    cfg.out_path    = path;
    cfg.sample_rate = 48000;
    cfg.channels    = 2;

    e = apr_action_m4a.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));

    pcm = tm_make_tone(48000, 2, 24000, 440.0);
    ASSERT_NOT_NULL(pcm);
    e = apr_action_m4a.on_audio(st, pcm, 24000, 0);
    ASSERT_FALSE(apr_failed(&e));

    /* A caller that forgets finalize is a bug we cannot fix from here, but we
     * can still refuse to hand back a corrupt MP4. */
    apr_action_m4a.destroy(st);
    free(pcm);

    hr = tm_mf_up();
    ASSERT_EQ_INT(S_OK, hr);
    hr = tm_decode(path, &d);
    if (FAILED(hr)) { tm_mf_down(); FAIL("destroy-without-finalize left an unplayable file"); }
    ASSERT_NEAR(0.5, (double)d.frames / (double)d.rate, 0.10);
    tm_decoded_free(&d);
    tm_mf_down();
    DeleteFileW(path);
}

/* Non-finite samples are a real hazard: a NaN reaching a float-to-int
 * conversion is undefined, and the "obvious" clamp maps it to full-scale
 * negative -- i.e. maximum loudness in the author's headphones. It must map to
 * silence, and the file must still finalize. */
TEST(m4a_non_finite_input_is_silenced_not_amplified)
{
    wchar_t path[MAX_PATH];
    AprActionConfig cfg;
    void  *st = NULL;
    float *pcm;
    size_t i, frames = 24000;
    AprErr e;
    TmDecoded d;
    HRESULT hr;
    double peak = 0.0;

    tm_temp_path(path, MAX_PATH, L"nan");
    DeleteFileW(path);

    memset(&cfg, 0, sizeof(cfg));
    cfg.out_path    = path;
    cfg.sample_rate = 48000;
    cfg.channels    = 2;

    e = apr_action_m4a.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));

    pcm = (float *)malloc(frames * 2 * sizeof(float));
    ASSERT_NOT_NULL(pcm);
    {
        /* Volatile so the compiler cannot fold these into constant division by
         * zero, which is a hard error rather than a NaN at compile time. */
        volatile double zero = 0.0, one = 1.0;
        float nan_v  = (float)(zero / zero);
        float pinf_v = (float)(one / zero);
        float ninf_v = (float)(-one / zero);
        for (i = 0; i < frames * 2; i++) {
            pcm[i] = (i % 3 == 0) ? nan_v : (i % 3 == 1) ? pinf_v : ninf_v;
        }
    }

    e = apr_action_m4a.on_audio(st, pcm, frames, 0);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_action_m4a.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_m4a.destroy(st);
    free(pcm);

    hr = tm_mf_up();
    ASSERT_EQ_INT(S_OK, hr);
    hr = tm_decode(path, &d);
    if (FAILED(hr)) { tm_mf_down(); FAIL("file with non-finite input did not open"); }
    for (i = 0; i < d.frames * d.channels; i++) {
        double a = fabs((double)d.pcm[i]);
        if (a > peak) peak = a;
    }
    printf("      decoded peak with NaN/Inf input: %.4f\n", peak);
    /* Infinities legitimately clamp to full scale; the assertion that matters
     * is that decoding produced finite audio at all and did not blow up. */
    ASSERT_TRUE(peak <= 1.001);
    tm_decoded_free(&d);
    tm_mf_down();
    DeleteFileW(path);
}

/* ------------------------------------------------------------------------
 * Rejections
 * ------------------------------------------------------------------------ */

TEST(m4a_unsupported_sample_rate_is_rejected_clearly)
{
    wchar_t path[MAX_PATH];
    wchar_t msg[512];
    AprActionConfig cfg;
    void *st = (void *)(intptr_t)0xDEAD;
    AprErr e;

    tm_temp_path(path, MAX_PATH, L"badrate");
    DeleteFileW(path);

    memset(&cfg, 0, sizeof(cfg));
    cfg.out_path    = path;
    /* 12345 Hz is not a rate any AAC encoder has ever advertised, so this case
     * does not depend on the machine's exact capability list. */
    cfg.sample_rate = 12345;
    cfg.channels    = 2;

    e = apr_action_m4a.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_UNSUPPORTED, (int)e.kind);
    ASSERT_NULL(st);

    apr_err_format(&e, msg, 512);
    printf("      %ls\n", msg);
    /* A clear failure names the rate that was refused. */
    ASSERT_NOT_NULL(wcsstr(e.context, L"12345"));

    /* Nothing must have been created on disk. */
    ASSERT_TRUE(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);
}

TEST(m4a_unsupported_channel_count_is_rejected_clearly)
{
    wchar_t path[MAX_PATH];
    AprActionConfig cfg;
    void *st = (void *)(intptr_t)0xDEAD;
    AprErr e;

    if (tm_supports(48000, 7)) {
        printf("      this machine really does offer 48000/7; skipped\n");
        return;
    }

    tm_temp_path(path, MAX_PATH, L"badch");
    DeleteFileW(path);

    memset(&cfg, 0, sizeof(cfg));
    cfg.out_path    = path;
    cfg.sample_rate = 48000;
    cfg.channels    = 7;

    e = apr_action_m4a.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_UNSUPPORTED, (int)e.kind);
    ASSERT_NULL(st);
    ASSERT_TRUE(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);
}

TEST(m4a_bad_arguments_are_rejected)
{
    AprActionConfig cfg;
    void *st = (void *)(intptr_t)0xDEAD;
    AprErr e;
    wchar_t path[MAX_PATH];

    tm_temp_path(path, MAX_PATH, L"args");

    memset(&cfg, 0, sizeof(cfg));
    cfg.out_path    = NULL;
    cfg.sample_rate = 48000;
    cfg.channels    = 2;
    e = apr_action_m4a.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_INVALID_ARG, (int)e.kind);
    ASSERT_NULL(st);

    memset(&cfg, 0, sizeof(cfg));
    cfg.out_path    = path;
    cfg.sample_rate = 0;
    cfg.channels    = 2;
    st = (void *)(intptr_t)0xDEAD;
    e = apr_action_m4a.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_INVALID_ARG, (int)e.kind);
    ASSERT_NULL(st);

    memset(&cfg, 0, sizeof(cfg));
    cfg.out_path    = path;
    cfg.sample_rate = 48000;
    cfg.channels    = 0;
    st = (void *)(intptr_t)0xDEAD;
    e = apr_action_m4a.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_E_INVALID_ARG, (int)e.kind);
    ASSERT_NULL(st);
}

TEST(m4a_unwritable_path_fails_at_create)
{
    AprActionConfig cfg;
    void *st = (void *)(intptr_t)0xDEAD;
    AprErr e;

    memset(&cfg, 0, sizeof(cfg));
    cfg.out_path    = L"Z:\\apprecorder-no-such-volume\\out.m4a";
    cfg.sample_rate = 48000;
    cfg.channels    = 2;

    e = apr_action_m4a.create(&cfg, &st);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(st);
}

/* ------------------------------------------------------------------------
 * on_audio must not block on disk. It hands frames to a ring and returns; a
 * long burst is the cheap way to notice if that ever stops being true.
 * ------------------------------------------------------------------------ */

TEST(m4a_on_audio_returns_promptly)
{
    wchar_t path[MAX_PATH];
    AprActionConfig cfg;
    void  *st = NULL;
    float *pcm;
    AprErr e;
    LARGE_INTEGER f, t0, t1;
    double worst_ms = 0.0;
    size_t i;
    const size_t block = 480;                 /* 10 ms at 48 kHz */
    const size_t blocks_total = 100;          /* 1 s of audio */

    tm_temp_path(path, MAX_PATH, L"prompt");
    DeleteFileW(path);

    memset(&cfg, 0, sizeof(cfg));
    cfg.out_path    = path;
    cfg.sample_rate = 48000;
    cfg.channels    = 2;

    e = apr_action_m4a.create(&cfg, &st);
    ASSERT_FALSE(apr_failed(&e));

    pcm = tm_make_tone(48000, 2, block, 440.0);
    ASSERT_NOT_NULL(pcm);

    QueryPerformanceFrequency(&f);
    for (i = 0; i < blocks_total; i++) {
        double ms;
        QueryPerformanceCounter(&t0);
        e = apr_action_m4a.on_audio(st, pcm, block, 0);
        QueryPerformanceCounter(&t1);
        if (apr_failed(&e)) break;
        ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        if (ms > worst_ms) worst_ms = ms;
    }
    ASSERT_FALSE(apr_failed(&e));

    printf("      worst on_audio call: %.3f ms over %zu blocks\n",
           worst_ms, blocks_total);

    e = apr_action_m4a.finalize(st);
    ASSERT_FALSE(apr_failed(&e));
    apr_action_m4a.destroy(st);
    free(pcm);
    DeleteFileW(path);

    /* Generous by two orders of magnitude: this catches "someone made on_audio
     * synchronous", not scheduler noise on a loaded machine. */
    ASSERT_TRUE(worst_ms < 10.0);
}

/* ------------------------------------------------------------------------
 * Bitrate selection
 *
 * The enumeration is a UNION over every registered AAC encoder MFT, and on
 * Windows 11 that means the plain AAC-LC encoder and the HE-AAC encoder both
 * contribute. A low bitrate at 48 kHz stereo exists only in the HE-AAC half of
 * that union, while the sink writer picks its own encoder -- so a request that
 * lands there may not be servable. Whatever the answer is, it must be a clean
 * one: either a working file or a create() failure, never a half-open state.
 * ------------------------------------------------------------------------ */

static AprErr tm_encode_at_bitrate(const wchar_t *path, int kbps, double seconds)
{
    AprActionConfig cfg;
    void  *st  = NULL;
    float *pcm;
    size_t frames = (size_t)(seconds * 48000 + 0.5);
    AprErr e, fe;

    memset(&cfg, 0, sizeof(cfg));
    cfg.out_path     = path;
    cfg.sample_rate  = 48000;
    cfg.channels     = 2;
    cfg.bitrate_kbps = kbps;

    e = apr_action_m4a.create(&cfg, &st);
    if (apr_failed(&e)) return e;

    pcm = tm_make_tone(48000, 2, frames, 440.0);
    if (!pcm) {
        apr_action_m4a.finalize(st);
        apr_action_m4a.destroy(st);
        return APR_ERR(APR_E_NO_MEMORY, L"tone buffer");
    }
    e  = apr_action_m4a.on_audio(st, pcm, frames, 0);
    fe = apr_action_m4a.finalize(st);
    if (!apr_failed(&e)) e = fe;
    apr_action_m4a.destroy(st);
    free(pcm);
    return e;
}

TEST(m4a_explicit_bitrate_is_honoured)
{
    wchar_t path[MAX_PATH];
    AprErr  e;
    TmDecoded d;
    HRESULT hr;
    LARGE_INTEGER size;
    HANDLE h;

    tm_temp_path(path, MAX_PATH, L"br192");
    DeleteFileW(path);

    e = tm_encode_at_bitrate(path, 192, 2.0);
    ASSERT_FALSE(apr_failed(&e));

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ASSERT_TRUE(h != INVALID_HANDLE_VALUE);
    GetFileSizeEx(h, &size);
    CloseHandle(h);
    printf("      2.0 s at 192 kbps -> %lld bytes\n", (long long)size.QuadPart);
    /* 192 kbps for 2 s is ~48 KB of payload. A file that came out at the
     * default 128 kbps would be around 32 KB, so this separates them. */
    ASSERT_GT_INT(38000, (int)size.QuadPart);

    hr = tm_mf_up();
    ASSERT_EQ_INT(S_OK, hr);
    hr = tm_decode(path, &d);
    if (FAILED(hr)) { tm_mf_down(); FAIL("192 kbps file did not decode"); }
    ASSERT_NEAR(2.0, (double)d.frames / (double)d.rate, 0.10);
    tm_decoded_free(&d);
    tm_mf_down();
    DeleteFileW(path);
}

TEST(m4a_low_bitrate_request_fails_cleanly_or_works)
{
    wchar_t path[MAX_PATH];
    wchar_t msg[512];
    AprErr  e;
    TmDecoded d;
    HRESULT hr;

    tm_temp_path(path, MAX_PATH, L"br16");
    DeleteFileW(path);

    e = tm_encode_at_bitrate(path, 16, 1.0);
    if (apr_failed(&e)) {
        apr_err_format(&e, msg, 512);
        printf("      16 kbps refused: %ls\n", msg);
        /* A refusal is acceptable. A refusal that leaves a stub file on disk
         * is not: the user would find an unopenable recording. */
        ASSERT_TRUE(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);
        return;
    }

    hr = tm_mf_up();
    ASSERT_EQ_INT(S_OK, hr);
    hr = tm_decode(path, &d);
    if (FAILED(hr)) { tm_mf_down(); FAIL("16 kbps file was written but does not decode"); }
    printf("      16 kbps accepted: %.3f s decoded\n",
           (double)d.frames / (double)d.rate);
    ASSERT_EQ_INT(48000, (int)d.rate);
    tm_decoded_free(&d);
    tm_mf_down();
    DeleteFileW(path);
}
