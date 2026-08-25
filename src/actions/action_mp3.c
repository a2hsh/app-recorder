/*
 * action_mp3.c -- the MP3 action: interleaved float32 in, MPEG-1 Layer III out,
 * encoded by the libmp3lame vendored under vendor/lame (design section 8).
 *
 * This is the "send it to someone" path. WAV is the archive; MP3 is the file
 * that opens in every player, phone and browser written since 1995, at a tenth
 * of the size. It is lossy, so unlike action_wav.c nothing here promises the
 * bytes back -- what it promises is that the file is the right length, at the
 * right rate, and playable.
 *
 * -------------------------------------------------------------------------
 * ON_AUDIO NEITHER ENCODES NOR TOUCHES THE DISK
 * -------------------------------------------------------------------------
 *
 * on_audio runs on the mixer thread, which is feeding every other bus in the
 * session. Two things must therefore stay out of it, and MP3 has both:
 *
 *   - The disk. A WriteFile here would put an unbounded stall (antivirus, a
 *     spinning disk waking up, a network volume) in front of every other
 *     recording, so this action owns its write-behind (action.h, design 3.1).
 *
 *   - The encoder. LAME is a psychoacoustic model, an MDCT and an iterative
 *     rate loop; at algorithm quality 3 that is single-digit percent of a core
 *     per stream, and its cost varies per frame with the signal. Running it on
 *     the mixer thread would make every bus in the session pay the jitter of
 *     this one's bitrate loop. action_wav.c has nothing to do on that thread
 *     and so does not have to make this choice; this file does.
 *
 * So the ring carries RAW FLOAT FRAMES, not encoded bytes, and the writer
 * thread does the encoding:
 *
 *   mixer thread            writer thread (one per action)
 *   -----------             ------------------------------
 *   rb_write()  ---ring--->  rb_read() -> sanitise -> LAME -> WriteFile()
 *   SetEvent()               waits on the event, 100 ms backstop
 *
 * The ring is core/ringbuf.c -- the single owner of that shape (design 7).
 * Everything on_audio does is lock-free, allocation-free and bounded: rb_write
 * is a memcpy and two stores, and SetEvent on an already-signalled auto-reset
 * event is a no-op. Nothing it calls can wait on a file system or on a
 * quantisation loop.
 *
 * The ring is sized for FOUR SECONDS. Beyond that the producer wins, as
 * ringbuf.h explains, and the frames the writer never reached are gone -- but
 * rb_read reports exactly how many, and this file encodes that many frames of
 * SILENCE in their place. A recording with a four-second hole still lines up
 * sample-for-sample with every other bus in the session; one that is merely
 * short is permanently desynced against all of them.
 *
 * -------------------------------------------------------------------------
 * WHAT A KILL -9 MID-RECORDING LEAVES BEHIND
 * -------------------------------------------------------------------------
 *
 * A playable file, missing only the seek table. This is the one place MP3 is
 * structurally kinder than the container formats: an MP3 is a bare sequence of
 * self-describing frames, so every byte already handed to WriteFile is already
 * decodable, with no index, no size field and no moov atom to repair. An MP4
 * truncated the same way does not open at all.
 *
 * Concretely, at the instant of a kill the file holds:
 *
 *   - a reserved frame at offset 0, written by LAME at lame_init_params time,
 *     which is a valid frame header followed by zeroes. No "Xing"/"Info"
 *     magic has been stamped into it yet, so a decoder treats it as one
 *     ordinary frame of silence -- 24 ms at the head, not a parse failure;
 *   - every complete frame the encoder had produced up to the kill.
 *
 * What is missing is the LAME tag: duration, the seek table, and the gapless
 * delay/padding fields. A player recovers duration by scanning instead, which
 * for the CBR default is exact. finalize is what stamps that tag, by writing
 * lame_get_lametag_frame back over the reserved frame at offset 0 -- and it
 * runs on every exit path, including error paths and a destroy that never saw
 * a finalize. This is claimed nowhere and tested twice, once by decoding the
 * bytes on disk mid-recording and once by decoding a truncated file
 * (tests/test_action_mp3.c).
 *
 * Note what is deliberately NOT here: action_wav.c rewrites its header once a
 * second because RIFF puts sizes at the front and a stale one makes the file
 * unopenable. MP3 has no such field, so there is no periodic patch, no seeking
 * during a recording, and nothing to get wrong.
 *
 * -------------------------------------------------------------------------
 * CBR BY DEFAULT, AT 192 kbps
 * -------------------------------------------------------------------------
 *
 * VBR is the better format on quality-per-byte and this action supports it.
 * The default is still CBR, for one reason that outranks the trade: a VBR file
 * whose Xing tag never got written -- which is exactly what the kill above
 * leaves -- reports a duration derived from its first frame's bitrate, so a
 * three-hour recording that happened to open quietly can present as forty
 * minutes and seek nowhere near where the user clicked. The same file in CBR
 * is exact by arithmetic. A recorder is judged on the recordings it saves from
 * a bad exit, so the default is the one that degrades honestly.
 *
 * 192 kbps is the rate: transparent enough for the speech-plus-music mix an
 * app recording usually is, ~1.4 MB per minute, and a valid MPEG-1 rate at 32,
 * 44.1 and 48 kHz so no session format silently gets a different one.
 *
 * AprActionConfig maps on:
 *
 *   quality == 0  ->  CBR at bitrate_kbps (0 = 192).
 *   quality 1..10 ->  VBR at V(quality-1); bitrate_kbps is not used, because
 *                     in VBR the quality target IS the rate control.
 *
 * -------------------------------------------------------------------------
 * FORMATS MP3 CANNOT CARRY
 * -------------------------------------------------------------------------
 *
 * Sample rate: MPEG audio stops at 48 kHz. A 96 kHz session is NOT refused --
 * LAME resamples internally and the file comes out at 48 kHz with the same
 * duration and the same content. Refusing would mean a high-rate session
 * silently loses its MP3 bus, which is worse than a resample nobody can hear.
 *
 * Channels: MP3 has mono and stereo and nothing else. More than two IS
 * refused, at create, with a message that says so -- a 5.1 bus needs a downmix
 * decision that belongs to the session, not to an encoder quietly dropping
 * four channels. (core/mix.c already owns apr_mix_map_channels; wiring it in
 * here is the natural upgrade if the session ever wants that.)
 */
#include "action.h"
#include "log.h"
#include "ringbuf.h"

#include "lame.h"

#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- tuning --------------------------------------------------------------- */

/* Write-behind depth. Rings between source and bus absorb mixer jitter only
 * (250 ms, design 3.1); disk stalls AND encoder stalls are absorbed HERE,
 * which is why this is an order of magnitude larger. */
#define MP3_WRITE_BEHIND_MS 4000u

/* Never let an exotic session format turn the write-behind into a huge
 * allocation, and never let a tiny one leave no headroom at all. */
#define MP3_RING_MIN_FRAMES 4096u
#define MP3_RING_MAX_BYTES  (16u * 1024u * 1024u)

/* Frames handed to LAME per call. 64 KiB of float32 is 8192 frames of stereo:
 * seven MPEG granule pairs, so the encoder's internal buffer is kept busy
 * without this action holding a large staging allocation per bus. */
#define MP3_CHUNK_TARGET_BYTES (64u * 1024u)
#define MP3_CHUNK_MIN_FRAMES   1152u
#define MP3_CHUNK_MAX_FRAMES   16384u

/* lame.h's rule for the output buffer is 1.25 * nsamples + 7200. The extra
 * 16 KiB covers the reserved tag frame that the first encode call flushes out
 * ahead of any audio, and keeps the same buffer usable for the final
 * lame_encode_flush and for the tag write-back. */
#define MP3_OUT_SLACK_BYTES 16384u

/* The writer sleeps on its event; this backstop only matters if a wakeup is
 * ever missed, which would cost latency rather than data. */
#define MP3_WAKE_TIMEOUT_MS 100u

#define MP3_DEFAULT_KBPS 192
#define MP3_MAX_KBPS     320

/* --- test seams ----------------------------------------------------------- */
/*
 * Both are zero in every real run and are read ONLY on the writer thread, so
 * neither costs the mixer a single instruction. tests/test_action_mp3.c
 * declares them extern; they are deliberately absent from every public header
 * because nothing outside that file may set them. The technique is
 * action_wav.c's, and it transfers unchanged to any action that owns a writer
 * thread.
 *
 *   write_gate       -- a handle the writer waits on before each payload
 *                       write. Holding it shut simulates a disk that has
 *                       stopped answering, which is how the "on_audio never
 *                       waits for the disk" claim is proven rather than
 *                       asserted.
 *   fail_after_bytes -- when nonzero, the first payload write that would push
 *                       the file past this many bytes fails, as a full volume
 *                       does. The tag write-back is deliberately NOT failed:
 *                       overwriting blocks already allocated is exactly what
 *                       still succeeds on a full disk.
 */
volatile HANDLE apr_mp3_test_write_gate;
volatile LONG64 apr_mp3_test_fail_after_bytes;

/* --- state ---------------------------------------------------------------- */

typedef struct Mp3Action {
    /* Immutable after create. */
    wchar_t *path;
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t frame_bytes;
    size_t   chunk_frames;
    int      vbr;             /* 0 = CBR at bitrate_kbps, 1 = VBR at vbr_q */
    int      bitrate_kbps;
    int      vbr_q;

    /* The write-behind. Producer: the mixer thread. Consumer: our writer. */
    RingBuf   *ring;
    RingReader rd;            /* writer thread only */
    HANDLE     wake;
    HANDLE     thread;

    /* Cross-thread flags. Aligned 32-bit values on x64: loads and stores are
     * atomic, and every transition here is one-way. */
    volatile LONG stop;       /* finalize asked the writer to wind up */
    volatile LONG failed;     /* the writer has recorded a failure     */

    /* Owned by the writer thread from CreateThread until it has been joined.
     * create() builds `lame` before the thread exists and finalize touches it
     * after the join, so it never has two users at once. */
    lame_global_flags *lame;
    float             *stage;      /* chunk_frames * channels floats  */
    float             *silence;    /* same size, permanently zero     */
    unsigned char     *out;        /* one encode's worth of mp3 bytes */
    size_t             out_bytes;
    uint64_t           pcm_frames; /* frames handed to the encoder    */
    uint64_t           mp3_bytes;  /* bytes actually on disk          */
    uint64_t           lost_frames;/* frames replaced by silence      */
    AprErr             io_err;     /* first failure, verbatim         */
    HANDLE             file;

    int finalized;
} Mp3Action;

/* --- LAME's diagnostics go to our log, never to stderr -------------------- */
/*
 * A GUI process has no stderr worth writing to, and libmp3lame is chatty at
 * init. These run on whichever thread is inside LAME; apr_log_write takes no
 * lock and allocates nothing, so that is safe from the writer thread.
 *
 * The text is libmp3lame's own English diagnostics, not user-facing strings --
 * AGENTS.md rule 5 exempts internal log messages, and there is nothing to
 * translate in "bitrate 128 not allowed" anyway.
 */
static void mp3_report(const char *fmt, va_list ap)
{
    char   narrow[240];
    size_t i;

    if (_vsnprintf_s(narrow, sizeof narrow, _TRUNCATE, fmt, ap) < 0) {
        narrow[sizeof narrow - 1] = '\0';
    }
    for (i = 0; narrow[i]; i++) {
        if (narrow[i] == '\n' || narrow[i] == '\r') { narrow[i] = '\0'; break; }
    }
    if (narrow[0]) APR_WARN(L"mp3: libmp3lame: %hs", narrow);
}

static void mp3_report_quiet(const char *fmt, va_list ap)
{
    (void)fmt; (void)ap;
}

/* --- writer thread: encoding and disk I/O live here and nowhere else ------ */

/* Record the first failure verbatim and stop producing. Everything after this
 * point is about leaving a playable file, not about salvaging the recording. */
static void mp3_fail(Mp3Action *st, AprErr e)
{
    if (!st->failed) {
        st->io_err = e;
        APR_LOG_ERR(APR_LOG_ERROR, &e);
        st->failed = 1;
    }
}

/* WriteFile until every byte is gone or it refuses. Partial writes are real (a
 * full volume takes what it can) and are retried, not assumed away. */
static AprErr mp3_write_all(Mp3Action *st, const void *buf, size_t bytes)
{
    const unsigned char *p = (const unsigned char *)buf;

    while (bytes > 0) {
        DWORD want = (bytes > 0x01000000u) ? 0x01000000u : (DWORD)bytes;
        DWORD got  = 0;
        if (!WriteFile(st->file, p, want, &got, NULL)) {
            return APR_ERR_LAST(L"writing %lu bytes to \"%ls\"",
                                (unsigned long)want, st->path);
        }
        if (got == 0) {
            return APR_ERR(APR_E_IO, L"disk accepted 0 of %lu bytes for \"%ls\"",
                           (unsigned long)want, st->path);
        }
        p         += got;
        bytes     -= got;
    }
    return apr_ok();
}

/* One append of encoder output. mp3_bytes only advances on complete success,
 * so it can never claim more than the file holds -- if a write dies halfway,
 * the surplus is a trailing partial frame that shutdown truncates away. */
static void mp3_write_payload(Mp3Action *st, const unsigned char *buf,
                              size_t bytes)
{
    HANDLE gate;
    LONG64 limit;
    AprErr e;

    if (st->failed || bytes == 0) return;

    gate = apr_mp3_test_write_gate;
    if (gate) (void)WaitForSingleObject(gate, INFINITE);

    limit = apr_mp3_test_fail_after_bytes;
    if (limit > 0 && st->mp3_bytes + bytes > (uint64_t)limit) {
        mp3_fail(st, APR_ERR(APR_E_IO,
                             L"simulated disk failure past %lld bytes", limit));
        return;
    }

    e = mp3_write_all(st, buf, bytes);
    if (apr_failed(&e)) { mp3_fail(st, e); return; }
    st->mp3_bytes += bytes;
}

/*
 * Make a block safe to hand to a psychoacoustic model.
 *
 * A NaN reaching LAME does not stay local: it goes through an FFT, so one
 * poisoned sample turns a whole frame's spectrum into NaN, the rate loop then
 * quantises garbage, and what the user hears is a burst of full-scale hash.
 * An infinity is worse -- LAME scales by 32767 into a float, so +INF stays
 * +INF. Neither can be allowed in, and neither can be usefully "encoded", so
 * both become silence.
 *
 * Finite samples outside [-1, +1] are clamped rather than zeroed: those are a
 * genuinely too-loud mix, and hard clipping is what the format would do to
 * them anyway. Clamping first also keeps FLT_MAX from becoming an infinity
 * inside LAME's own scaling.
 *
 * This is not PCM format conversion, so it is not core/mix.c's job (design 7):
 * nothing changes representation, it is float in and float out. It is an
 * encoder-input precondition, and it lives next to the encoder that needs it.
 * If a second lossy encoder wants the same guard, hoist it then.
 */
static void mp3_sanitize(float *p, size_t samples)
{
    size_t i;
    for (i = 0; i < samples; i++) {
        float v = p[i];
        if (!(v == v) || v > 3.4e38f || v < -3.4e38f) {
            /* NaN fails its own equality test; the bounds catch both
             * infinities without an isinf that would drag in a mode switch. */
            p[i] = 0.0f;
        }
        else if (v > 1.0f)  p[i] = 1.0f;
        else if (v < -1.0f) p[i] = -1.0f;
    }
}

/* Encode one block and append whatever frames come out. */
static void mp3_encode(Mp3Action *st, const float *pcm, size_t frames)
{
    int n;

    if (st->failed || frames == 0) return;

    if (st->channels == 2) {
        n = lame_encode_buffer_interleaved_ieee_float(
                st->lame, pcm, (int)frames, st->out, (int)st->out_bytes);
    }
    else {
        /* Mono: LAME ignores the right pointer once num_channels is 1, but it
         * still checks it for NULL. */
        n = lame_encode_buffer_ieee_float(
                st->lame, pcm, pcm, (int)frames, st->out, (int)st->out_bytes);
    }

    if (n < 0) {
        mp3_fail(st, APR_ERR(APR_E_IO,
                             L"mp3: libmp3lame refused %llu frames for \"%ls\" (%d)",
                             (unsigned long long)frames, st->path, n));
        return;
    }
    st->pcm_frames += frames;
    if (n > 0) mp3_write_payload(st, st->out, (size_t)n);
}

/* Frames the ring overwrote before the writer reached them. They are encoded
 * as silence rather than skipped: see the header comment. */
static void mp3_encode_silence(Mp3Action *st, uint64_t frames)
{
    st->lost_frames += frames;
    APR_WARN(L"mp3: the encoder fell %llu frames behind on \"%ls\"; "
             L"filling with silence to keep the timeline aligned",
             (unsigned long long)frames, st->path);

    while (frames > 0 && !st->failed) {
        size_t n = (frames > (uint64_t)st->chunk_frames)
                 ? st->chunk_frames : (size_t)frames;
        mp3_encode(st, st->silence, n);
        frames -= n;
    }
}

/* Move everything the ring holds through the encoder and onto the disk.
 * Writer thread only. */
static void mp3_drain(Mp3Action *st)
{
    for (;;) {
        uint64_t lost = 0;
        size_t   got  = rb_read(&st->rd, st->stage, st->chunk_frames, &lost);

        if (lost > 0) mp3_encode_silence(st, lost);
        if (got == 0) break;

        mp3_sanitize(st->stage, got * st->channels);
        mp3_encode(st, st->stage, got);
    }
}

static DWORD WINAPI mp3_writer_thread(LPVOID param)
{
    Mp3Action *st = (Mp3Action *)param;

    for (;;) {
        /* Read the flag BEFORE draining. Anything the mixer wrote before
         * finalize set it is therefore visible to the drain that follows, so
         * one more pass after seeing it is enough -- there is no window in
         * which a frame is left in the ring. */
        LONG stopping = st->stop;

        mp3_drain(st);
        if (stopping) break;

        (void)WaitForSingleObject(st->wake, MP3_WAKE_TIMEOUT_MS);
    }
    return 0;
}

/* --- shutdown ------------------------------------------------------------- */

/*
 * Stamp the Xing/LAME tag over the reserved frame at offset 0: duration, the
 * 100-entry seek table, and the gapless delay/padding fields. Until this
 * happens the reserved frame is a valid frame of silence and the file plays
 * without it, which is the whole point of the kill story above.
 *
 * Not routed through mp3_write_payload: this overwrites bytes already
 * allocated rather than appending, so it must not advance mp3_bytes and must
 * not be subject to the full-disk seam -- rewriting an allocated block is
 * exactly what still succeeds when a volume is full.
 */
static void mp3_write_lametag(Mp3Action *st)
{
    unsigned char *buf = st->out;
    size_t         need, wrote;
    LARGE_INTEGER  to;
    AprErr         e;

    /* After a failure the reserved frame may never have reached the disk, and
     * writing a tag over the first bytes of whatever did would corrupt it. */
    if (st->failed) return;

    /* size 0 asks for the size rather than the bytes. 0 back means this stream
     * has no tag (nothing was ever encoded, or LAME turned the tag off because
     * it would not fit the frame). */
    need = lame_get_lametag_frame(st->lame, NULL, 0);
    if (need == 0) return;

    if (need > st->out_bytes || need > st->mp3_bytes) {
        APR_WARN(L"mp3: the %llu byte LAME tag does not fit the %llu bytes "
                 L"written to \"%ls\"; leaving the file without a seek table",
                 (unsigned long long)need, (unsigned long long)st->mp3_bytes,
                 st->path);
        return;
    }

    wrote = lame_get_lametag_frame(st->lame, buf, st->out_bytes);
    if (wrote == 0 || wrote > need) return;

    to.QuadPart = 0;
    if (!SetFilePointerEx(st->file, to, NULL, FILE_BEGIN)) {
        mp3_fail(st, APR_ERR_LAST(L"seeking to the tag frame of \"%ls\"", st->path));
        return;
    }

    e = mp3_write_all(st, buf, wrote);
    if (apr_failed(&e)) mp3_fail(st, e);
}

/*
 * Stop the writer, flush the encoder's last frame, stamp the tag, trim any
 * partial tail, close the file. Runs for finalize AND for a destroy that never
 * saw one, and does as much as it can even when the recording already failed:
 * leaving a playable file is the last thing this action owes the user.
 */
static void mp3_shutdown(Mp3Action *st)
{
    st->stop = 1;
    if (st->wake) SetEvent(st->wake);

    if (st->thread) {
        /* INFINITE deliberately. If the disk really is hung, the alternative
         * is abandoning a thread that is inside WriteFile on this handle and
         * then seeking and closing it underneath -- that trades a slow exit
         * for a corrupt file. */
        WaitForSingleObject(st->thread, INFINITE);
        CloseHandle(st->thread);
        st->thread = NULL;
    }
    /* The writer is gone: its fields are ours now, with no synchronisation. */

    if (st->file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER end;

        if (st->lame && !st->failed) {
            /* The last partial frame, padded out and emitted. Without this the
             * final up-to-26 ms of the recording is still inside LAME. */
            int n = lame_encode_flush(st->lame, st->out, (int)st->out_bytes);
            if (n < 0) {
                mp3_fail(st, APR_ERR(APR_E_IO,
                                     L"mp3: flushing the final frame of \"%ls\" (%d)",
                                     st->path, n));
            }
            else if (n > 0) {
                mp3_write_payload(st, st->out, (size_t)n);
            }
        }

        if (st->lame) mp3_write_lametag(st);

        /* The file is now exactly as long as the frames that landed. Anything
         * past this is the tail of a write that died mid-flight. */
        end.QuadPart = (LONGLONG)st->mp3_bytes;
        if (SetFilePointerEx(st->file, end, NULL, FILE_BEGIN)) {
            (void)SetEndOfFile(st->file);
        }

        /* No FlushFileBuffers: the cache survives process death, which is the
         * failure this action defends against, and forcing a physical flush
         * would stall the exit for nothing. */
        CloseHandle(st->file);
        st->file = INVALID_HANDLE_VALUE;
    }

    APR_INFO(L"mp3: closed \"%ls\" -- %llu frames in, %llu bytes out, "
             L"%llu frames lost to silence",
             st->path, (unsigned long long)st->pcm_frames,
             (unsigned long long)st->mp3_bytes,
             (unsigned long long)st->lost_frames);
}

static void mp3_free(Mp3Action *st)
{
    if (!st) return;
    if (st->thread) { CloseHandle(st->thread); st->thread = NULL; }
    if (st->file != INVALID_HANDLE_VALUE && st->file != NULL) CloseHandle(st->file);
    if (st->wake) CloseHandle(st->wake);
    if (st->lame) lame_close(st->lame);
    rb_destroy(st->ring);
    free(st->stage);
    free(st->silence);
    free(st->out);
    free(st->path);
    free(st);
}

/* --- vtable --------------------------------------------------------------- */

static AprErr mp3_create(const AprActionConfig *cfg, void **out_state)
{
    Mp3Action *st;
    uint64_t   ring_frames, ring_bytes;
    uint32_t   frame_bytes;
    size_t     path_cch, chunk;
    int        kbps, quality;
    AprErr     e;

    if (!out_state) {
        return APR_ERR(APR_E_INVALID_ARG, L"mp3: no out_state");
    }
    *out_state = NULL;

    if (!cfg || !cfg->out_path || !cfg->out_path[0]) {
        return APR_ERR(APR_E_INVALID_ARG, L"mp3: no output path");
    }
    if (cfg->sample_rate == 0) {
        return APR_ERR(APR_E_INVALID_ARG, L"mp3: sample rate is zero");
    }
    if (cfg->channels == 0) {
        return APR_ERR(APR_E_INVALID_ARG, L"mp3: channel count is zero");
    }
    /* See the header comment: rate is resampled, channels are not invented. */
    if (cfg->channels > 2) {
        return APR_ERR(APR_E_UNSUPPORTED,
                       L"mp3: MPEG audio carries mono or stereo, not %u channels",
                       (unsigned)cfg->channels);
    }

    quality = cfg->quality;
    if (quality < 0 || quality > 10) {
        return APR_ERR(APR_E_INVALID_ARG,
                       L"mp3: quality %d is outside 0 (CBR) to 10 (VBR V9)",
                       quality);
    }

    kbps = cfg->bitrate_kbps ? cfg->bitrate_kbps : MP3_DEFAULT_KBPS;
    if (kbps < 8 || kbps > MP3_MAX_KBPS) {
        return APR_ERR(APR_E_UNSUPPORTED,
                       L"mp3: %d kbps is outside the 8 to %d the format allows",
                       cfg->bitrate_kbps, MP3_MAX_KBPS);
    }

    frame_bytes = (uint32_t)cfg->channels * (uint32_t)sizeof(float);

    st = (Mp3Action *)calloc(1, sizeof *st);
    if (!st) return APR_ERR(APR_E_NO_MEMORY, L"mp3: state");
    st->file = INVALID_HANDLE_VALUE;

    st->sample_rate  = cfg->sample_rate;
    st->channels     = cfg->channels;
    st->frame_bytes  = frame_bytes;
    st->vbr          = (quality > 0);
    st->vbr_q        = quality > 0 ? quality - 1 : 0;
    st->bitrate_kbps = kbps;

    path_cch = wcslen(cfg->out_path) + 1;
    st->path = (wchar_t *)malloc(path_cch * sizeof(wchar_t));
    if (!st->path) { mp3_free(st); return APR_ERR(APR_E_NO_MEMORY, L"mp3: path"); }
    memcpy(st->path, cfg->out_path, path_cch * sizeof(wchar_t));

    /* Staging, silence and encoder-output buffers: one encode call each. */
    chunk = MP3_CHUNK_TARGET_BYTES / frame_bytes;
    if (chunk < MP3_CHUNK_MIN_FRAMES) chunk = MP3_CHUNK_MIN_FRAMES;
    if (chunk > MP3_CHUNK_MAX_FRAMES) chunk = MP3_CHUNK_MAX_FRAMES;
    st->chunk_frames = chunk;
    st->out_bytes    = chunk + chunk / 4u + MP3_OUT_SLACK_BYTES;

    st->stage   = (float *)malloc(chunk * cfg->channels * sizeof(float));
    st->silence = (float *)calloc(chunk * cfg->channels, sizeof(float));
    st->out     = (unsigned char *)malloc(st->out_bytes);
    if (!st->stage || !st->silence || !st->out) {
        mp3_free(st);
        return APR_ERR(APR_E_NO_MEMORY, L"mp3: %llu byte encode buffers",
                       (unsigned long long)(chunk * frame_bytes + st->out_bytes));
    }

    /* --- the encoder ------------------------------------------------------ */

    st->lame = lame_init();
    if (!st->lame) {
        mp3_free(st);
        return APR_ERR(APR_E_NO_MEMORY, L"mp3: libmp3lame would not initialise");
    }

    lame_set_errorf(st->lame, mp3_report);
    lame_set_msgf(st->lame, mp3_report_quiet);
    lame_set_debugf(st->lame, mp3_report_quiet);

    (void)lame_set_in_samplerate(st->lame, (int)cfg->sample_rate);
    (void)lame_set_num_channels(st->lame, (int)cfg->channels);
    (void)lame_set_mode(st->lame, cfg->channels == 1 ? MONO : JOINT_STEREO);

    /* We stamp the tag ourselves in finalize (mp3_write_lametag), so the
     * reserved frame must be the very first bytes of the file -- no automatic
     * ID3v2 block in front of it to account for. */
    lame_set_write_id3tag_automatic(st->lame, 0);
    (void)lame_set_bWriteVbrTag(st->lame, 1);

    if (st->vbr) {
        (void)lame_set_VBR(st->lame, vbr_default);
        (void)lame_set_VBR_q(st->lame, st->vbr_q);
    }
    else {
        (void)lame_set_VBR(st->lame, vbr_off);
        (void)lame_set_brate(st->lame, st->bitrate_kbps);
    }

    if (lame_init_params(st->lame) < 0) {
        AprErr init_err = APR_ERR(APR_E_UNSUPPORTED,
                                  L"mp3: libmp3lame refused %u Hz x %u channels "
                                  L"at %d kbps",
                                  (unsigned)cfg->sample_rate,
                                  (unsigned)cfg->channels, st->bitrate_kbps);
        mp3_free(st);
        return init_err;
    }

    /* --- the write-behind ------------------------------------------------- */

    ring_frames = (uint64_t)cfg->sample_rate * MP3_WRITE_BEHIND_MS / 1000u;
    ring_bytes  = ring_frames * frame_bytes;
    if (ring_bytes > MP3_RING_MAX_BYTES) ring_frames = MP3_RING_MAX_BYTES / frame_bytes;
    if (ring_frames < MP3_RING_MIN_FRAMES) ring_frames = MP3_RING_MIN_FRAMES;

    e = rb_create((size_t)ring_frames, frame_bytes, &st->ring);
    if (apr_failed(&e)) { mp3_free(st); return e; }
    rb_reader_init(&st->rd, st->ring, RB_START_OLDEST);

    /* FILE_SHARE_READ so the recording can be inspected (or streamed) while it
     * is being written -- an MP3 is decodable from its first frame, so that is
     * genuinely useful here and not just a courtesy. */
    st->file = CreateFileW(st->path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (st->file == INVALID_HANDLE_VALUE) {
        AprErr open_err = APR_ERR_LAST(L"creating \"%ls\"", st->path);
        mp3_free(st);
        return open_err;
    }

    st->wake = CreateEventW(NULL, FALSE, FALSE, NULL);   /* auto-reset */
    if (!st->wake) {
        AprErr ev_err = APR_ERR_LAST(L"mp3: creating the writer event");
        mp3_free(st);
        return ev_err;
    }

    st->thread = CreateThread(NULL, 0, mp3_writer_thread, st, 0, NULL);
    if (!st->thread) {
        AprErr th_err = APR_ERR_LAST(L"mp3: starting the writer thread");
        mp3_free(st);
        return th_err;
    }

    APR_INFO(L"mp3: recording \"%ls\" at %u Hz x %u ch -> %u Hz %ls "
             L"(%llu frame write-behind)",
             st->path, (unsigned)st->sample_rate, (unsigned)st->channels,
             (unsigned)lame_get_out_samplerate(st->lame),
             st->vbr ? L"VBR" : L"CBR",
             (unsigned long long)rb_capacity_frames(st->ring));

    *out_state = st;
    return apr_ok();
}

/*
 * The mixer thread's entire contact with this action. Bounded, lock-free and
 * allocation-free; see the header comment for why neither the encoder nor the
 * file system may appear here.
 */
static AprErr mp3_on_audio(void *state, const float *pcm, size_t frames,
                           uint64_t qpc)
{
    Mp3Action *st = (Mp3Action *)state;

    /* MP3 has nowhere to record a wall-clock position, and does not need one:
     * the mixer has already resolved every gap into frames before this call
     * (design 5), so sample position IS timeline position. */
    (void)qpc;

    if (!st) return APR_ERR(APR_E_INVALID_ARG, L"mp3: on_audio with no state");
    if (frames == 0) return apr_ok();
    if (!pcm) return APR_ERR(APR_E_INVALID_ARG, L"mp3: %llu frames from NULL",
                             (unsigned long long)frames);
    if (st->stop) {
        return APR_ERR(APR_E_STATE, L"mp3: audio after finalize on \"%ls\"", st->path);
    }

    rb_write(st->ring, pcm, frames);
    SetEvent(st->wake);

    /* Reported every call rather than once: the bus decides what a dead disk
     * means for the session (design 10), and constructing an AprErr allocates
     * nothing and takes no lock (err.h). */
    if (st->failed) {
        return APR_ERR(APR_E_IO, L"mp3: the writer for \"%ls\" has failed", st->path);
    }
    return apr_ok();
}

static AprErr mp3_finalize(void *state)
{
    Mp3Action *st = (Mp3Action *)state;

    if (!st) return APR_ERR(APR_E_INVALID_ARG, L"mp3: finalize with no state");

    if (!st->finalized) {
        mp3_shutdown(st);
        st->finalized = 1;
    }
    /* Idempotent, and it keeps returning the same verdict: finalize is called
     * from several exit paths and none of them should have to remember whether
     * another already ran. */
    return st->failed ? st->io_err : apr_ok();
}

static void mp3_destroy(void *state)
{
    Mp3Action *st = (Mp3Action *)state;

    if (!st) return;
    /* A destroy that never saw a finalize still owes the user a playable
     * file. */
    if (!st->finalized) {
        mp3_shutdown(st);
        st->finalized = 1;
    }
    mp3_free(st);
}

/* Registered by core/registry.c under APR_HAVE_ACTION_MP3, which CMake defines
 * from the presence of this file. There is no registration call here and no
 * plugin machinery anywhere (design 3.3). */
const AprActionVTable apr_action_mp3 = {
    "mp3",
    L"MP3 (MPEG-1 Layer III)",
    L"mp3",
    mp3_create,
    mp3_on_audio,
    mp3_finalize,
    mp3_destroy
};
