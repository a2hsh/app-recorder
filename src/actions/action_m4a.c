/*
 * action_m4a.c -- M4A/AAC, encoded by the AAC encoder that ships in Windows.
 *
 * Design section 8 chose Media Foundation over fdk-aac deliberately: it adds
 * zero bytes to the binary, vendors nothing, and raises no patent-licensing
 * question. The cost is that we do not get to pick the format -- MF's AAC
 * encoder supports a small, fixed set of rate/channel/bitrate combinations,
 * and which ones is a property of the machine, not of this source file. So
 * nothing here is hardcoded: m4a_collect_formats() asks the encoder MFT what
 * it can actually do, and a session format that is not on that list is
 * refused with APR_E_UNSUPPORTED naming the rate and channel count.
 *
 * THREADING -- one rule, and it removes the whole problem
 *
 *   Every Media Foundation call in this file happens on one thread that this
 *   file owns and creates. The mixer thread that calls on_audio never touches
 *   COM, never touches MF, and never learns what apartment it is in. That
 *   matters because on_audio is called from whatever thread the bus mixes on
 *   -- possibly one that has initialised COM as an STA, possibly one that has
 *   not initialised it at all -- and a sink writer driven from a foreign
 *   apartment is a class of bug that shows up as a corrupt file hours later.
 *
 *   create() still reports MF failures synchronously: it blocks on a one-shot
 *   handshake until the encoder thread has either built the sink writer or
 *   failed trying, so an unsupported format or an unwritable path comes back
 *   from create() like any other error.
 *
 * on_audio MUST NOT BLOCK ON DISK (action.h, design 3.1). It converts to
 * 16-bit PCM -- which is what MF's AAC encoder takes; float32 in is not an
 * option -- pushes into a RingBuf and signals an event. That is the whole
 * write-behind buffer: 2 seconds of it, sized so a disk stall is absorbed here
 * rather than in the ring shared by every other bus. RingBuf is core's
 * (AGENTS.md rule 3); this file does not have its own FIFO.
 *
 * IF THE PROCESS DIES MID-RECORDING
 *
 *   The file is NOT playable. An MP4 keeps its index -- the moov atom, sample
 *   tables, durations -- in a box the sink writer only writes at Finalize(),
 *   so a killed process leaves ftyp + a large mdat and nothing that maps bytes
 *   to samples. Every ordinary exit path is covered (finalize() is idempotent,
 *   and destroy() finalizes for a caller that forgot), but nothing this file
 *   can do covers SIGKILL/TerminateProcess/power loss, because no code of ours
 *   runs. The remedies, none of them free, are recorded at the bottom of this
 *   file so the next person does not have to rediscover them.
 */
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <mferror.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "action.h"
#include "clock.h"
#include "log.h"
#include "mix.h"
#include "ringbuf.h"

/* Kept local rather than added to CMakeLists.txt: the build file is shared
 * with every other action landing this wave, and a link directive that travels
 * with the object that needs it cannot be lost in a merge. */
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

/* Frames moved per conversion / per MF sample. 2048 frames is ~43 ms at
 * 48 kHz: two AAC frames' worth, small enough that a conversion loop stays in
 * L1 and large enough that per-sample COM overhead is noise. */
#define M4A_CHUNK_FRAMES 2048

/* Write-behind depth. Design 3.1 puts disk-stall absorption *here*, not in the
 * source ring, precisely so one slow encoder cannot back-pressure every bus. */
#define M4A_BUFFER_SECONDS 2

/* How long finalize() waits for the encoder thread to drain and close. Long
 * enough for a heavily loaded machine to flush seconds of audio; finite
 * because hanging a shutdown forever is worse than reporting a timeout. */
#define M4A_FINALIZE_TIMEOUT_MS 30000

/* One AAC-LC frame. The floor on a recording's length -- see the empty-file
 * handling in m4a_thread; the sink cannot finalize a stream that got nothing. */
#define M4A_MIN_FRAMES 1024

/* The Windows encoder advertises well over a hundred combinations on Win11
 * (7 rates x 3 channel counts x several bitrates each), so this is sized to
 * hold the real list rather than the short one the docs describe. */
#define M4A_MAX_FORMATS 256

/* ==========================================================================
 * PCM conversion
 *
 * float32 -> 16-bit is core/mix.c's job (design 7, AGENTS.md rule 3) and this
 * file calls apr_pcm_from_float for it. What is here is a guard in front of
 * that call, not a second converter.
 *
 * TEMPORARY, AND IT BELONGS IN mix.c. apr_pcm_from_float reaches the integer
 * domain through a C cast, and a NaN or an infinity through cvttsd2si is
 * INT64_MIN -- which its clamp then pins to -32768, i.e. FULL-SCALE NEGATIVE.
 * A NaN in the mix therefore becomes maximum loudness, and a +Inf becomes
 * maximum loudness of the wrong sign. The author is blind and works in
 * headphones; "the bug shows up as a full-scale sample" is not an abstract
 * defect here. Scrubbing before the cast costs one compare per sample.
 *
 * Delete this the moment mix.c handles non-finite input itself; do not grow
 * it, and do not let any other clamping or scaling migrate into it.
 * ========================================================================== */

static void m4a_scrub_non_finite(const float *src, float *dst, size_t samples)
{
    size_t i;
    for (i = 0; i < samples; i++) {
        float v = src[i];
        if (v != v)         v =  0.0f;   /* NaN -> silence */
        else if (v >  1.0f) v =  1.0f;   /* +Inf and overs */
        else if (v < -1.0f) v = -1.0f;
        dst[i] = v;
    }
}

/* ==========================================================================
 * Runtime capability enumeration
 * ========================================================================== */

typedef struct M4aFormat {
    uint32_t rate;
    uint32_t channels;
    uint32_t avg_bytes_per_second;
} M4aFormat;

/* Requires MFStartup to have been called on this thread's process. Returns the
 * number of triples written. Best-effort: an MFT that refuses to enumerate is
 * skipped rather than failing the whole query. */
static size_t m4a_collect_formats(M4aFormat *out, size_t max)
{
    MFT_REGISTER_TYPE_INFO want;
    IMFActivate **acts = NULL;
    UINT32 n_acts = 0, ai;
    size_t count = 0;
    HRESULT hr;

    want.guidMajorType = MFMediaType_Audio;
    want.guidSubtype   = MFAudioFormat_AAC;

    hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
                   MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                   MFT_ENUM_FLAG_SORTANDFILTER,
                   NULL, &want, &acts, &n_acts);
    if (FAILED(hr) || !acts) return 0;

    for (ai = 0; ai < n_acts; ai++) {
        IMFTransform *mft = NULL;
        DWORD ti;

        if (count >= max) { IMFActivate_Release(acts[ai]); continue; }

        hr = IMFActivate_ActivateObject(acts[ai], &IID_IMFTransform, (void **)&mft);
        if (SUCCEEDED(hr) && mft) {
            for (ti = 0; ; ti++) {
                IMFMediaType *mt = NULL;
                UINT32 rate = 0, ch = 0, bps = 0;
                GUID sub;

                hr = IMFTransform_GetOutputAvailableType(mft, 0, ti, &mt);
                if (FAILED(hr) || !mt) break;

                /* Several MFTs advertise the same triples (the AAC and HE-AAC
                 * encoders overlap heavily, and MFTEnumEx lists both a local
                 * and a registered activation for each). Deduplicating here is
                 * not tidiness: without it the identical entries fill `max`
                 * and genuinely distinct formats fall off the end. */
                if (SUCCEEDED(IMFMediaType_GetGUID(mt, &MF_MT_SUBTYPE, &sub)) &&
                    IsEqualGUID(&sub, &MFAudioFormat_AAC) &&
                    SUCCEEDED(IMFMediaType_GetUINT32(
                        mt, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) &&
                    SUCCEEDED(IMFMediaType_GetUINT32(
                        mt, &MF_MT_AUDIO_NUM_CHANNELS, &ch)) &&
                    SUCCEEDED(IMFMediaType_GetUINT32(
                        mt, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps)) &&
                    count < max)
                {
                    size_t k;
                    int dup = 0;
                    for (k = 0; k < count; k++) {
                        if (out[k].rate == rate && out[k].channels == ch &&
                            out[k].avg_bytes_per_second == bps) { dup = 1; break; }
                    }
                    if (!dup) {
                        out[count].rate                 = rate;
                        out[count].channels             = ch;
                        out[count].avg_bytes_per_second = bps;
                        count++;
                    }
                }
                IMFMediaType_Release(mt);
                if (count >= max) break;
            }
            IMFTransform_Release(mft);
            IMFActivate_ShutdownObject(acts[ai]);
        }
        IMFActivate_Release(acts[ai]);
    }

    CoTaskMemFree(acts);
    return count;
}

/* Public for tests and diagnostics: what can this machine actually encode?
 * Self-contained -- initialises and tears down COM/MF around the query, and is
 * safe to call on a thread that has already done either. */
size_t apr_m4a_enum_formats(uint32_t *rates, uint32_t *channels,
                            uint32_t *avg_bytes_per_second, size_t max)
{
    M4aFormat fmts[M4A_MAX_FORMATS];
    size_t n = 0, i;
    HRESULT co, mf;

    if (!rates || !channels || !avg_bytes_per_second || max == 0) return 0;
    if (max > M4A_MAX_FORMATS) max = M4A_MAX_FORMATS;

    co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (co != S_OK && co != S_FALSE && co != RPC_E_CHANGED_MODE) return 0;

    mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (SUCCEEDED(mf)) {
        n = m4a_collect_formats(fmts, max);
        MFShutdown();
    }
    if (co == S_OK || co == S_FALSE) CoUninitialize();

    for (i = 0; i < n; i++) {
        rates[i]                = fmts[i].rate;
        channels[i]             = fmts[i].channels;
        avg_bytes_per_second[i] = fmts[i].avg_bytes_per_second;
    }
    return n;
}

/* Insert `v` into a sorted, duplicate-free list. Returns the new length. */
static size_t m4a_insert_sorted(uint32_t *list, size_t n, size_t cap, uint32_t v)
{
    size_t i, j;
    for (i = 0; i < n; i++) {
        if (list[i] == v) return n;
        if (list[i] > v)  break;
    }
    if (n >= cap) return n;
    for (j = n; j > i; j--) list[j] = list[j - 1];
    list[i] = v;
    return n + 1;
}

/* Compact "11025,16000,...,48000 Hz; 1,2,6,8 ch" for the rejection message.
 * Never fails; an empty list renders as "(none)". */
static void m4a_describe_support(const M4aFormat *f, size_t n,
                                 wchar_t *buf, size_t cch)
{
    uint32_t rates[16], chans[16];
    size_t nr = 0, nc = 0, i;

    buf[0] = L'\0';
    for (i = 0; i < n; i++) {
        nr = m4a_insert_sorted(rates, nr, 16, f[i].rate);
        nc = m4a_insert_sorted(chans, nc, 16, f[i].channels);
    }
    if (nr == 0) { wcscpy_s(buf, cch, L"(none)"); return; }

    for (i = 0; i < nr; i++) {
        wchar_t part[24];
        _snwprintf_s(part, 24, _TRUNCATE, i ? L",%lu" : L"%lu",
                     (unsigned long)rates[i]);
        wcscat_s(buf, cch, part);
    }
    wcscat_s(buf, cch, L" Hz; ");
    for (i = 0; i < nc; i++) {
        wchar_t part[24];
        _snwprintf_s(part, 24, _TRUNCATE, i ? L",%lu" : L"%lu",
                     (unsigned long)chans[i]);
        wcscat_s(buf, cch, part);
    }
    wcscat_s(buf, cch, L" ch");
}

/* Pick the advertised bitrate closest to `want_kbps` (0 => 128 kbps, the
 * usual default for stereo AAC-LC) among the entries matching rate/channels.
 * Ties go to the higher bitrate. Returns APR_E_UNSUPPORTED, naming the
 * refused format and listing what is on offer, when nothing matches. */
static AprErr m4a_pick_bitrate(uint32_t rate, uint32_t channels, int want_kbps,
                               uint32_t *out_avg_bytes)
{
    M4aFormat fmts[M4A_MAX_FORMATS];
    size_t n, i;
    long target, best_delta = 0;
    uint32_t best = 0;
    int found = 0;

    n = m4a_collect_formats(fmts, M4A_MAX_FORMATS);
    if (n == 0) {
        return APR_ERR(APR_E_UNSUPPORTED,
                       L"no Media Foundation AAC encoder is registered on this "
                       L"system; M4A output is unavailable");
    }

    target = (want_kbps > 0) ? want_kbps : 128;

    for (i = 0; i < n; i++) {
        long kbps, delta;
        if (fmts[i].rate != rate || fmts[i].channels != channels) continue;
        kbps  = (long)((fmts[i].avg_bytes_per_second * 8 + 500) / 1000);
        delta = kbps > target ? kbps - target : target - kbps;
        if (!found || delta < best_delta ||
            (delta == best_delta && fmts[i].avg_bytes_per_second > best)) {
            best       = fmts[i].avg_bytes_per_second;
            best_delta = delta;
            found      = 1;
        }
    }

    if (!found) {
        wchar_t avail[96];
        m4a_describe_support(fmts, n, avail, 96);
        return APR_ERR(APR_E_UNSUPPORTED,
                       L"the Windows AAC encoder has no output type for %lu Hz "
                       L"/ %lu ch (available: %s)",
                       (unsigned long)rate, (unsigned long)channels, avail);
    }

    *out_avg_bytes = best;
    return apr_ok();
}

/* ==========================================================================
 * State
 * ========================================================================== */

typedef struct M4aState {
    uint32_t rate;
    uint32_t channels;
    int      want_kbps;
    uint32_t avg_bytes_per_second;   /* filled in by the encoder thread */
    wchar_t  path[MAX_PATH * 2];

    RingBuf *ring;                   /* int16 interleaved, encoder-thread reader */
    RingReader reader;               /* encoder thread only */

    float   *scrub;                  /* on_audio scratch, mixer thread only */
    int16_t *cvt;                    /* on_audio scratch, mixer thread only */
    int16_t *enc;                    /* encoder-thread scratch */
    int16_t *silence;                /* zeroes, for filling ring overruns */

    HANDLE  thread;
    HANDLE  ev_wake;                 /* auto-reset: data queued, or stop */
    HANDLE  ev_ready;                /* manual-reset: init handshake complete */

    volatile LONG stop;              /* finalize asked the thread to wind up */
    volatile LONG closed;            /* file is closed; on_audio must refuse */
    volatile LONG failed;            /* run_err is valid */

    AprErr init_err;                 /* encoder thread -> create(), via ev_ready */
    AprErr run_err;                  /* first failure once running */

    int finalized;                   /* finalize() has completed once */

    uint64_t frames_in;              /* accepted by on_audio */
    uint64_t frames_encoded;         /* handed to the sink writer */
    uint64_t frames_lost;            /* dropped by ring overrun, refilled as silence */
} M4aState;

/* ==========================================================================
 * Encoder thread
 * ========================================================================== */

static AprErr m4a_make_input_type(const M4aState *s, IMFMediaType **out)
{
    IMFMediaType *t = NULL;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return APR_ERR_HR(hr, L"MFCreateMediaType (PCM input)");

    hr = IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, s->rate);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, s->channels);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BLOCK_ALIGNMENT,
                                    s->channels * 2);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                    s->rate * s->channels * 2);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(t, &MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

    if (FAILED(hr)) {
        IMFMediaType_Release(t);
        return APR_ERR_HR(hr, L"describing %lu Hz / %lu ch 16-bit PCM input",
                          (unsigned long)s->rate, (unsigned long)s->channels);
    }
    *out = t;
    return apr_ok();
}

static AprErr m4a_make_output_type(const M4aState *s, IMFMediaType **out)
{
    IMFMediaType *t = NULL;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return APR_ERR_HR(hr, L"MFCreateMediaType (AAC output)");

    hr = IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, s->rate);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, s->channels);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                    s->avg_bytes_per_second);
    /* Payload type 0 = raw AAC, which is what goes inside an MP4 track.
     * ADTS (type 1) here produces a file that decodes but carries a redundant
     * header on every frame. */
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(t, &MF_MT_AAC_PAYLOAD_TYPE, 0);

    if (FAILED(hr)) {
        IMFMediaType_Release(t);
        return APR_ERR_HR(hr, L"describing AAC output at %lu B/s",
                          (unsigned long)s->avg_bytes_per_second);
    }
    *out = t;
    return apr_ok();
}

static AprErr m4a_open_writer(M4aState *s, IMFSinkWriter **out_writer,
                              DWORD *out_stream)
{
    IMFAttributes *attrs = NULL;
    IMFMediaType  *in_t = NULL, *out_t = NULL;
    IMFSinkWriter *w = NULL;
    DWORD stream = 0;
    AprErr e;
    HRESULT hr;

    e = m4a_pick_bitrate(s->rate, s->channels, s->want_kbps,
                         &s->avg_bytes_per_second);
    if (apr_failed(&e)) return e;

    hr = MFCreateAttributes(&attrs, 2);
    if (FAILED(hr)) return APR_ERR_HR(hr, L"MFCreateAttributes for the sink writer");

    /* The container comes from this attribute, not from the file extension.
     * MFCreateSinkWriterFromURL will happily guess from ".m4a", but the guess
     * is a registry lookup that a codec pack can change underneath us; naming
     * the container makes the output deterministic. */
    hr = IMFAttributes_SetGUID(attrs, &MF_TRANSCODE_CONTAINERTYPE,
                               &MFTranscodeContainerType_MPEG4);
    if (SUCCEEDED(hr)) {
        /* We are not a real-time pipeline. Throttling would make WriteSample
         * block to pace the clock, which is pointless here and would only make
         * the drain slower. */
        hr = IMFAttributes_SetUINT32(attrs, &MF_SINK_WRITER_DISABLE_THROTTLING,
                                     TRUE);
    }
    if (FAILED(hr)) { e = APR_ERR_HR(hr, L"configuring sink writer attributes"); goto fail; }

    hr = MFCreateSinkWriterFromURL(s->path, NULL, attrs, &w);
    if (FAILED(hr)) { e = APR_ERR_HR(hr, L"creating the MPEG-4 sink for \"%s\"", s->path); goto fail; }

    e = m4a_make_output_type(s, &out_t);
    if (apr_failed(&e)) goto fail;
    hr = IMFSinkWriter_AddStream(w, out_t, &stream);
    if (FAILED(hr)) { e = APR_ERR_HR(hr, L"adding the AAC stream"); goto fail; }

    e = m4a_make_input_type(s, &in_t);
    if (apr_failed(&e)) goto fail;
    hr = IMFSinkWriter_SetInputMediaType(w, stream, in_t, NULL);
    if (FAILED(hr)) { e = APR_ERR_HR(hr, L"setting the 16-bit PCM input type"); goto fail; }

    hr = IMFSinkWriter_BeginWriting(w);
    if (FAILED(hr)) { e = APR_ERR_HR(hr, L"IMFSinkWriter::BeginWriting"); goto fail; }

    IMFMediaType_Release(in_t);
    IMFMediaType_Release(out_t);
    IMFAttributes_Release(attrs);
    *out_writer = w;
    *out_stream = stream;
    return apr_ok();

fail:
    if (in_t)  IMFMediaType_Release(in_t);
    if (out_t) IMFMediaType_Release(out_t);
    if (w)     IMFSinkWriter_Release(w);
    if (attrs) IMFAttributes_Release(attrs);
    /* Nothing partly written should survive a failed open: a zero-byte .m4a
     * left behind reads to the user as "it recorded and produced nothing". */
    DeleteFileW(s->path);
    return e;
}

/* Hand `frames` frames of 16-bit PCM to the sink writer, timestamped from the
 * running frame count so the timeline stays exact for the whole session. */
static AprErr m4a_write_frames(M4aState *s, IMFSinkWriter *w, DWORD stream,
                               const int16_t *pcm, size_t frames)
{
    IMFMediaBuffer *buf = NULL;
    IMFSample      *smp = NULL;
    DWORD bytes = (DWORD)(frames * s->channels * sizeof(int16_t));
    BYTE *dst = NULL;
    LONGLONG t0, t1;
    AprErr e = apr_ok();
    HRESULT hr;

    if (frames == 0) return e;

    hr = MFCreateMemoryBuffer(bytes, &buf);
    if (FAILED(hr)) return APR_ERR_HR(hr, L"MFCreateMemoryBuffer (%lu bytes)",
                                      (unsigned long)bytes);
    hr = IMFMediaBuffer_Lock(buf, &dst, NULL, NULL);
    if (FAILED(hr)) { e = APR_ERR_HR(hr, L"locking the encoder input buffer"); goto done; }
    memcpy(dst, pcm, bytes);
    IMFMediaBuffer_Unlock(buf);
    hr = IMFMediaBuffer_SetCurrentLength(buf, bytes);
    if (FAILED(hr)) { e = APR_ERR_HR(hr, L"SetCurrentLength"); goto done; }

    hr = MFCreateSample(&smp);
    if (FAILED(hr)) { e = APR_ERR_HR(hr, L"MFCreateSample"); goto done; }
    hr = IMFSample_AddBuffer(smp, buf);
    if (FAILED(hr)) { e = APR_ERR_HR(hr, L"IMFSample::AddBuffer"); goto done; }

    /* Absolute, from the cumulative frame count, via clock.c's exact 128-bit
     * arithmetic (design 7 owns this; do not open-code frames*1e7/rate here).
     * Timestamping each buffer from a running "+= duration" accumulator is how
     * a multi-hour recording ends up minutes out of sync. */
    t0 = (LONGLONG)apr_frames_to_hns(s->frames_encoded, s->rate);
    t1 = (LONGLONG)apr_frames_to_hns(s->frames_encoded + frames, s->rate);

    hr = IMFSample_SetSampleTime(smp, t0);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleDuration(smp, t1 - t0);
    if (FAILED(hr)) { e = APR_ERR_HR(hr, L"timestamping the encoder input sample"); goto done; }

    hr = IMFSinkWriter_WriteSample(w, stream, smp);
    if (FAILED(hr)) { e = APR_ERR_HR(hr, L"IMFSinkWriter::WriteSample at frame %llu",
                                     (unsigned long long)s->frames_encoded); goto done; }

    s->frames_encoded += frames;

done:
    if (smp) IMFSample_Release(smp);
    if (buf) IMFMediaBuffer_Release(buf);
    return e;
}

static void m4a_note_failure(M4aState *s, AprErr e)
{
    if (InterlockedCompareExchange(&s->failed, 1, 0) == 0) {
        s->run_err = e;
        APR_LOG_ERR(APR_LOG_ERROR, &s->run_err);
    }
}

/* Fill `frames` of silence for data the ring overwrote before we read it.
 * The gap is real and already happened; leaving it out would shorten the file
 * and desync it against every other bus (ringbuf.h, overrun policy). */
static AprErr m4a_write_silence(M4aState *s, IMFSinkWriter *w, DWORD stream,
                                uint64_t frames)
{
    AprErr e = apr_ok();
    while (frames > 0 && !apr_failed(&e)) {
        size_t n = (frames > M4A_CHUNK_FRAMES) ? M4A_CHUNK_FRAMES : (size_t)frames;
        e = m4a_write_frames(s, w, stream, s->silence, n);
        frames -= n;
    }
    return e;
}

static DWORD WINAPI m4a_thread(LPVOID param)
{
    M4aState      *s = (M4aState *)param;
    IMFSinkWriter *w = NULL;
    DWORD stream = 0;
    HRESULT co, hr;
    AprErr e;
    int mf_up = 0, dead = 0;

    co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (co != S_OK && co != S_FALSE && co != RPC_E_CHANGED_MODE) {
        s->init_err = APR_ERR_HR(co, L"CoInitializeEx on the M4A encoder thread");
        goto ready;
    }

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        s->init_err = APR_ERR_HR(hr, L"MFStartup");
        goto ready;
    }
    mf_up = 1;

    e = m4a_open_writer(s, &w, &stream);
    if (apr_failed(&e)) { s->init_err = e; goto ready; }

    s->init_err = apr_ok();

ready:
    SetEvent(s->ev_ready);
    if (apr_failed(&s->init_err)) {
        if (mf_up) MFShutdown();
        if (co == S_OK || co == S_FALSE) CoUninitialize();
        return 0;
    }

    APR_INFO(L"m4a: encoding %lu Hz / %lu ch at %lu kbps -> %s",
             (unsigned long)s->rate, (unsigned long)s->channels,
             (unsigned long)((s->avg_bytes_per_second * 8 + 500) / 1000),
             s->path);

    for (;;) {
        size_t n;
        uint64_t lost = 0;
        /* Read the stop flag BEFORE draining. finalize() sets `closed`, then
         * `stop`, then signals -- so anything a caller managed to write before
         * it was told to stop is already in the ring by the time we see the
         * flag, and this pass will drain it. Reading the flag afterwards would
         * make that a race that loses the tail of every recording. */
        int stopping = InterlockedCompareExchange(&s->stop, 0, 0) != 0;

        /* The 100 ms timeout is not polling for data -- on_audio signals every
         * time -- it is a floor on how long a missed signal could cost, and it
         * is skipped entirely once we are closing so finalize() is not made
         * 100 ms slower for nothing. */
        if (!stopping) WaitForSingleObject(s->ev_wake, 100);

        for (;;) {
            n = rb_read(&s->reader, s->enc, M4A_CHUNK_FRAMES, &lost);
            if (lost > 0) {
                s->frames_lost += lost;
                APR_RT1(APR_LOG_WARN,
                        L"m4a: write-behind overrun, %lld frames replaced with silence",
                        (int64_t)lost);
                if (!dead) {
                    AprErr se = m4a_write_silence(s, w, stream, lost);
                    if (apr_failed(&se)) { m4a_note_failure(s, se); dead = 1; }
                }
            }
            if (n == 0) break;
            if (!dead) {
                AprErr we = m4a_write_frames(s, w, stream, s->enc, n);
                if (apr_failed(&we)) { m4a_note_failure(s, we); dead = 1; }
            }
            /* When `dead`, keep draining: the ring must never fill, because a
             * full ring is how a failed encoder starts costing other buses. */
        }

        if (stopping) break;
    }

    /* A stream that received nothing cannot be finalized: the sink returns
     * MF_E_SINK_NO_SAMPLES_PROCESSED (0xC00D4A44) and leaves a file with no
     * moov atom, i.e. one that will not open. A bus that was armed and never
     * fed -- a muted app, a source that failed to start -- is a real and
     * fairly common outcome, and the user is owed a file that opens and is
     * audibly empty rather than one that errors in their player. So: one AAC
     * frame of silence, ~21 ms at 48 kHz, and the file is valid. */
    if (!dead && s->frames_encoded == 0) {
        AprErr se = m4a_write_silence(s, w, stream, M4A_MIN_FRAMES);
        if (apr_failed(&se)) { m4a_note_failure(s, se); dead = 1; }
        else APR_INFO(L"m4a: no audio was ever delivered; wrote %d frames of "
                      L"silence so \"%s\" is a valid file",
                      (int)M4A_MIN_FRAMES, s->path);
    }

    /* Finalize even after a write failure. Whatever samples the writer did
     * accept are only reachable if the moov atom gets written; skipping this
     * on the error path turns a truncated recording into no recording. */
    hr = IMFSinkWriter_Finalize(w);
    if (FAILED(hr))
        m4a_note_failure(s, APR_ERR_HR(hr, L"IMFSinkWriter::Finalize on \"%s\"", s->path));

    IMFSinkWriter_Release(w);

    APR_INFO(L"m4a: closed after %llu frames (%llu lost to overrun)",
             (unsigned long long)s->frames_encoded,
             (unsigned long long)s->frames_lost);

    MFShutdown();
    if (co == S_OK || co == S_FALSE) CoUninitialize();
    return 0;
}

/* ==========================================================================
 * VTable
 * ========================================================================== */

static void m4a_free(M4aState *s)
{
    if (!s) return;
    if (s->thread)   CloseHandle(s->thread);
    if (s->ev_wake)  CloseHandle(s->ev_wake);
    if (s->ev_ready) CloseHandle(s->ev_ready);
    rb_destroy(s->ring);
    free(s->scrub);
    free(s->cvt);
    free(s->enc);
    free(s->silence);
    free(s);
}

static AprErr m4a_create(const AprActionConfig *cfg, void **out_state)
{
    M4aState *s;
    AprErr e;
    size_t samples = (size_t)M4A_CHUNK_FRAMES;

    if (out_state) *out_state = NULL;
    if (!cfg || !out_state)
        return APR_ERR(APR_E_INVALID_ARG, L"m4a: NULL config or out_state");
    if (!cfg->out_path || cfg->out_path[0] == L'\0')
        return APR_ERR(APR_E_INVALID_ARG, L"m4a: no output path");
    if (cfg->sample_rate == 0)
        return APR_ERR(APR_E_INVALID_ARG, L"m4a: sample_rate is 0");
    if (cfg->channels == 0)
        return APR_ERR(APR_E_INVALID_ARG, L"m4a: channels is 0");
    if (wcslen(cfg->out_path) >= MAX_PATH * 2)
        return APR_ERR(APR_E_INVALID_ARG, L"m4a: output path is too long");

    s = (M4aState *)calloc(1, sizeof(M4aState));
    if (!s) return APR_ERR(APR_E_NO_MEMORY, L"m4a: state allocation failed");

    s->rate      = cfg->sample_rate;
    s->channels  = cfg->channels;
    s->want_kbps = cfg->bitrate_kbps;
    wcscpy_s(s->path, MAX_PATH * 2, cfg->out_path);

    samples *= s->channels;
    s->scrub   = (float   *)malloc(samples * sizeof(float));
    s->cvt     = (int16_t *)malloc(samples * sizeof(int16_t));
    s->enc     = (int16_t *)malloc(samples * sizeof(int16_t));
    s->silence = (int16_t *)calloc(samples, sizeof(int16_t));
    if (!s->scrub || !s->cvt || !s->enc || !s->silence) {
        m4a_free(s);
        return APR_ERR(APR_E_NO_MEMORY, L"m4a: scratch buffer allocation failed");
    }

    /* One RingBuf, core's (AGENTS.md rule 3). Frames are int16 interleaved, so
     * 2 s at 48 kHz stereo is 384 KB -- the write-behind depth design 3.1 puts
     * inside the action rather than in the shared source ring. */
    e = rb_create((size_t)s->rate * M4A_BUFFER_SECONDS,
                  s->channels * sizeof(int16_t), &s->ring);
    if (apr_failed(&e)) { m4a_free(s); return e; }
    rb_reader_init(&s->reader, s->ring, RB_START_OLDEST);

    s->ev_wake  = CreateEventW(NULL, FALSE, FALSE, NULL);
    s->ev_ready = CreateEventW(NULL, TRUE,  FALSE, NULL);
    if (!s->ev_wake || !s->ev_ready) {
        e = APR_ERR_LAST(L"m4a: CreateEvent failed");
        m4a_free(s);
        return e;
    }

    s->thread = CreateThread(NULL, 0, m4a_thread, s, 0, NULL);
    if (!s->thread) {
        e = APR_ERR_LAST(L"m4a: could not start the encoder thread");
        m4a_free(s);
        return e;
    }

    /* Synchronous handshake: an unsupported format or an unwritable path is
     * reported from create(), not discovered an hour into a session. */
    WaitForSingleObject(s->ev_ready, INFINITE);
    if (apr_failed(&s->init_err)) {
        e = s->init_err;
        /* The thread is on its way out (MFShutdown, CoUninitialize) but still
         * holds `s`. Join before freeing. If the join somehow times out, LEAK
         * the state rather than free memory a live thread is reading: a few
         * hundred KB is recoverable, a use-after-free is not. */
        if (WaitForSingleObject(s->thread, M4A_FINALIZE_TIMEOUT_MS) == WAIT_OBJECT_0) {
            m4a_free(s);
        } else {
            APR_WARN(L"m4a: encoder thread did not exit after a failed open; "
                     L"leaking its state rather than freeing it underneath it");
        }
        return e;
    }

    *out_state = s;
    return apr_ok();
}

static AprErr m4a_on_audio(void *state, const float *pcm, size_t frames,
                           uint64_t qpc)
{
    M4aState *s = (M4aState *)state;
    size_t done = 0;

    (void)qpc;   /* encoders place audio by frame count, not wall clock */

    if (!s) return APR_ERR(APR_E_INVALID_ARG, L"m4a: NULL state");
    if (InterlockedCompareExchange(&s->closed, 0, 0) != 0)
        return APR_ERR(APR_E_STATE, L"m4a: audio arrived after the file was closed");
    if (frames == 0) return apr_ok();
    if (!pcm) return APR_ERR(APR_E_INVALID_ARG, L"m4a: NULL pcm with %zu frames", frames);

    while (done < frames) {
        size_t n = frames - done;
        if (n > M4A_CHUNK_FRAMES) n = M4A_CHUNK_FRAMES;
        m4a_scrub_non_finite(pcm + done * s->channels, s->scrub, n * s->channels);
        apr_pcm_from_float(s->scrub, APR_PCM_S16, s->cvt, n * s->channels);
        /* Never blocks, never allocates, never takes a lock: the whole reason
         * the encoder lives on its own thread. */
        rb_write(s->ring, s->cvt, n);
        done += n;
    }
    s->frames_in += frames;

    SetEvent(s->ev_wake);

    /* Report a failure the encoder thread already hit, so the bus can mark the
     * action failed. The audio above was still buffered: this call is not the
     * place to decide the recording is over. */
    if (InterlockedCompareExchange(&s->failed, 0, 0) != 0) return s->run_err;
    return apr_ok();
}

static AprErr m4a_finalize(void *state)
{
    M4aState *s = (M4aState *)state;
    DWORD wr;

    if (!s) return APR_ERR(APR_E_INVALID_ARG, L"m4a: NULL state");
    if (s->finalized) return apr_ok();   /* called on every exit path; be idempotent */

    /* `closed` before `stop`: it shuts the door on calls that have not started
     * yet, and the encoder thread reads `stop` before its final drain, so
     * everything already in the ring still gets written. A call to on_audio
     * that is *in flight* when finalize begins is not covered and cannot be --
     * the bus must stop feeding an action before it finalizes it. */
    InterlockedExchange(&s->closed, 1);
    InterlockedExchange(&s->stop, 1);
    SetEvent(s->ev_wake);

    wr = WaitForSingleObject(s->thread, M4A_FINALIZE_TIMEOUT_MS);
    if (wr != WAIT_OBJECT_0) {
        /* Deliberately no TerminateThread: killing a thread inside the MPEG-4
         * sink leaves the file half-indexed AND leaks the writer. Report and
         * let the caller decide. */
        return APR_ERR(APR_E_TIMEOUT,
                       L"m4a: encoder thread did not finish closing \"%s\" within %d s",
                       s->path, M4A_FINALIZE_TIMEOUT_MS / 1000);
    }
    s->finalized = 1;

    if (InterlockedCompareExchange(&s->failed, 0, 0) != 0) return s->run_err;
    return apr_ok();
}

static void m4a_destroy(void *state)
{
    M4aState *s = (M4aState *)state;
    if (!s) return;
    /* A caller that skipped finalize has a bug, but the user should still get
     * a file that opens. Close it properly before letting the state go. */
    if (!s->finalized) {
        AprErr e = m4a_finalize(s);
        if (apr_failed(&e)) {
            APR_LOG_ERR(APR_LOG_WARN, &e);
            /* finalize() timed out. It deliberately does NOT TerminateThread,
             * so the encoder thread is still alive and still reading `s` --
             * its ring, its scratch buffers, its path. Freeing here would be a
             * use-after-free under a live thread, during shutdown of a real
             * recording. Leak instead, exactly as m4a_create does on the same
             * join timeout: a few hundred KB is recoverable, this is not. */
            if (!s->finalized) {
                APR_WARN(L"m4a: encoder thread for \"%s\" did not exit; "
                         L"leaking its state rather than freeing it underneath it",
                         s->path);
                return;
            }
        }
    }
    m4a_free(s);
}

const AprActionVTable apr_action_m4a = {
    "m4a",
    L"M4A (AAC, Media Foundation)",
    L"m4a",
    m4a_create,
    m4a_on_audio,
    m4a_finalize,
    m4a_destroy
};

/*
 * ---------------------------------------------------------------------------
 * If the process dies mid-recording (unresolved, and honestly so)
 *
 * Standard MP4 puts the sample index in `moov`, written at Finalize(). Kill
 * the process and the file is ftyp + mdat: the AAC frames are all there, but
 * nothing says where they start or how long they run, and no ordinary player
 * will open it. `ffmpeg`/`untrunc` can rebuild the index from a known-good
 * reference file; we cannot, from inside a process that is no longer running.
 *
 * Three ways out, all with a real cost, none taken yet:
 *
 *   1. Fragmented MP4 (MFCreateFMPEG4MediaSink). Writes self-describing moof
 *      fragments as it goes, so a killed process leaves everything up to the
 *      last complete fragment playable. Cost: fMP4 in a .m4a is fine for
 *      VLC/ffmpeg/browsers and dubious for older hardware players, which is a
 *      real regression for a file people hand to other people.
 *   2. Encode to ADTS-framed .aac (payload type 1, no container). Self-syncing
 *      by construction -- truncate it anywhere and it still plays. Cost: it is
 *      not an .m4a, so it belongs behind a separate action id, not here.
 *   3. Periodic close-and-reopen into numbered segments. Bounds the loss to
 *      one segment, at the price of splitting one recording into many files.
 *
 * Until one is chosen: every ordinary exit path is covered. finalize() runs on
 * error paths as well as clean ones (design 10), is idempotent, and destroy()
 * calls it for a caller that forgot -- both are pinned by tests.
 * ---------------------------------------------------------------------------
 */
