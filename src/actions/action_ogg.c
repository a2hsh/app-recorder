/*
 * action_ogg.c -- the OGG action: interleaved float32 in, Ogg Opus out,
 * encoded by the libopus vendored under vendor/opus and paged by the libogg
 * vendored under vendor/ogg (design section 8).
 *
 * WAV is the archive, MP3 is the file that opens everywhere, M4A is the one
 * Apple software wants. Opus is the one that is simply better: at 96 kbps it
 * beats MP3 at 192, it is the only codec here with no patent question left
 * open, and both halves of it are BSD -- which, after the LGPL relink
 * obligation vendor/lame drags into every release (design 8.1), is worth
 * something on its own.
 *
 * -------------------------------------------------------------------------
 * ON_AUDIO NEITHER ENCODES, RESAMPLES NOR TOUCHES THE DISK
 * -------------------------------------------------------------------------
 *
 * This is action_mp3.c's structure, unchanged, because the argument is
 * unchanged: on_audio runs on the mixer thread that feeds every other bus in
 * the session, so a WriteFile there would put an unbounded stall (antivirus, a
 * disk spinning up, a network volume) in front of every other recording, and
 * an encoder there would put its per-frame cost in front of them too.
 *
 * Opus has a THIRD thing to keep off that thread, which MP3 did not: the
 * resampler. Opus is 48 kHz native (see below), so a 44.1 kHz session is
 * windowed-sinc converted on the way in, and that is a per-sample cost with
 * its own buffering. It belongs where the encoder is.
 *
 * So the ring carries RAW FLOAT FRAMES at the session format, and the writer
 * thread does everything else:
 *
 *   mixer thread          writer thread (one per action)
 *   ------------          ------------------------------
 *   rb_write()  --ring-->  rb_read -> resample -> 20 ms frame -> opus_encode
 *   SetEvent()                     -> ogg page -> WriteFile()
 *
 * The ring is core/ringbuf.c and the resampler is core/resample.c -- the two
 * single owners of those shapes (design 7). Everything on_audio does is
 * lock-free, allocation-free and bounded: rb_write is a memcpy and two stores,
 * and SetEvent on an already-signalled auto-reset event is a no-op.
 *
 * The ring is sized for FOUR SECONDS. Beyond that the producer wins, as
 * ringbuf.h explains, and the frames the writer never reached are gone -- but
 * rb_read reports exactly how many, and this file pushes that many frames of
 * SILENCE through the same path in their place. A recording with a four-second
 * hole still lines up sample-for-sample with every other bus in the session;
 * one that is merely short is permanently desynced against all of them.
 *
 * -------------------------------------------------------------------------
 * OPUS IS 48 kHz NATIVE, SO EVERYTHING IS ENCODED AT 48 kHz
 * -------------------------------------------------------------------------
 *
 * opus_encoder_create accepts 8, 12, 16, 24 and 48 kHz and nothing else, and
 * Ogg Opus counts granule positions in 48 kHz units regardless of which was
 * chosen (RFC 7845 section 4). A session at 44.1 kHz -- the most common rate
 * there is -- cannot be encoded at its own rate at all.
 *
 * The choice is therefore not "resample or not", it is "resample, or lose the
 * bus". This action resamples, through core/resample.c, on the writer thread,
 * ALWAYS to 48 kHz:
 *
 *   - 48 kHz in: the resampler is not created at all and the float frames go
 *     straight to the encoder. The common case pays nothing.
 *   - anything else: ratio = in_rate / 48000 in Q32.32, which is exactly the
 *     quantity resample.h documents (input frames per output frame).
 *
 * Even the rates Opus does accept natively (8/12/16/24 kHz) are resampled to
 * 48 kHz rather than passed through. Opus would upsample them internally
 * anyway for its CELT layer; doing it here means ONE granule-position
 * arithmetic instead of two, and one code path to get right instead of five.
 *
 * The original rate is not lost: it goes into OpusHead's `input_sample_rate`
 * field, which RFC 7845 defines as informational exactly so a tool can tell
 * where a file came from.
 *
 * Rates outside [6000, 384000] are refused at create, because that is where
 * APR_RESAMPLE_MAX_RATIO (8) runs out. Nothing in this project produces one.
 *
 * -------------------------------------------------------------------------
 * 20 ms FRAMES
 * -------------------------------------------------------------------------
 *
 * Opus encodes 2.5, 5, 10, 20, 40 or 60 ms per packet and the caller owns the
 * choice. This action uses 20 ms -- 960 frames at 48 kHz:
 *
 *   - It is Opus's own default and what every RFC 7845 file in the wild
 *     carries, so it is the size every decoder is best tested against.
 *   - Below it, per-packet overhead starts to matter: at 2.5 ms the TOC byte
 *     and the Ogg lacing byte alone are ~6 kbps.
 *   - Above it, quality at a given bitrate does not improve for music, and
 *     the tail a killed recording loses grows with no compensation.
 *
 * The frame size is fixed for the whole stream. A partial frame can only
 * happen at the very end, and finalize pads it with silence and then trims it
 * back off with the granule position (below), so the file's duration is the
 * duration of the audio and not of the padding.
 *
 * -------------------------------------------------------------------------
 * PRE-SKIP AND GRANULE POSITIONS
 * -------------------------------------------------------------------------
 *
 * Opus has encoder lookahead: the first samples out of the decoder are not the
 * first samples that went in, they are the filter's warm-up. RFC 7845 handles
 * this with `pre_skip` in OpusHead -- the number of 48 kHz samples a player
 * must discard from the front -- and this action asks the encoder for the real
 * number (OPUS_GET_LOOKAHEAD) rather than assuming the usual 312. Getting it
 * wrong makes the file start 6.5 ms early, which is inaudible on its own and
 * a permanent 6.5 ms offset against every other bus in the session.
 *
 * The granule position of every audio page is then
 *
 *     pre_skip + (48 kHz frames of real audio encoded so far)
 *
 * which is exactly what RFC 7845 asks for: "the total number of samples
 * decodable from the beginning of the stream up to the end of the page,
 * INCLUDING the pre-skip". Two consequences fall out of it for free:
 *
 *   - Duration is exact even though the stream is VBR. Subtracting pre_skip
 *     from the last granule position gives the sample count, with no seek
 *     table to write and nothing to patch in finalize. This is the structural
 *     difference from MP3, which had to default to CBR precisely because a
 *     VBR file whose Xing tag never got written lies about its length.
 *   - The final partial frame's silence padding is trimmed by arithmetic: the
 *     last packet decodes to 960 samples but the granule position only claims
 *     the real ones, and a decoder is required to drop the difference.
 *
 * -------------------------------------------------------------------------
 * WHAT A KILL -9 MID-RECORDING LEAVES BEHIND
 * -------------------------------------------------------------------------
 *
 * A playable file whose duration is right up to the last complete page.
 *
 * Ogg is a page-based container and that is the whole reason to expect this,
 * but "should" is not evidence -- tests/test_action_ogg.c truncates a finished
 * recording at an offset chosen to land inside a page and checks the result
 * with **ffprobe**, an external tool with no stake in this code, as well as
 * with libopus. What the file holds at the instant of a kill is:
 *
 *   - the OpusHead page and the OpusTags page, both written and flushed
 *     during create, before a single audio frame exists;
 *   - every complete audio page libogg had handed to WriteFile.
 *
 * What is missing is the end-of-stream flag on the last page. A demuxer that
 * cares recovers the duration by seeking to the last page with a valid
 * capture pattern and reading its granule position -- which is right there in
 * the page header, not in an index at the front of the file. There is nothing
 * to repair, no size field to patch, and no moov atom to rebuild: an M4A
 * truncated the same way does not open at all, and that asymmetry is why the
 * M4A action has to work so much harder than this one.
 *
 * The cost is bounded and worth stating: libogg accumulates roughly 4 KB
 * before it emits a page, so a kill loses up to ~350 ms of encoded audio on
 * top of whatever was still in the four-second ring. There is deliberately NO
 * periodic ogg_stream_flush to shrink that -- flushing a page early wastes a
 * 27+ byte header on a fraction of a page, forever, to buy back a third of a
 * second in an event that also loses four seconds to the ring.
 *
 * -------------------------------------------------------------------------
 * NaN AND INFINITY ARE NOT SCRUBBED HERE
 * -------------------------------------------------------------------------
 *
 * AGENTS.md rule 4 says core/mix.c owns that, and it does -- for every INTEGER
 * format, where the undefined behaviour of casting a NaN is the real hazard.
 * Opus takes float directly, so nothing on this path goes near that code, and
 * duplicating it would be the rule 3 failure the rule is there to prevent.
 *
 * What happens instead is a property of libopus, and tests/test_action_ogg.c
 * measures it rather than assuming it: opus_encode_float's SILK path clamps
 * through celt/float_cast.h's FLOAT2INT16 and its CELT path carries the value
 * into an MDCT. The test feeds half a second of quiet NaNs and infinities and
 * asserts the decoded result is not full-scale hash in the user's ears.
 *
 * -------------------------------------------------------------------------
 * BITRATE AND COMPLEXITY
 * -------------------------------------------------------------------------
 *
 * VBR, which is Opus's default and which costs nothing here because granule
 * positions make duration exact either way (above).
 *
 * The default rate is 96 kbps for stereo and 64 kbps for mono. Opus at 96 kbps
 * stereo is transparent for the speech-plus-music mix an app recording usually
 * is -- the same bar MP3 needs 192 kbps to clear -- and it is ~700 KB per
 * minute against MP3's 1.4 MB.
 *
 * AprActionConfig maps on:
 *
 *   bitrate_kbps 0   ->  96 stereo / 64 mono. Otherwise 6..510, Opus's range.
 *   quality      0   ->  complexity 10, libopus's own default.
 *   quality 1..11    ->  complexity quality-1, for a caller that would rather
 *                        spend less CPU per bus.
 *
 * -------------------------------------------------------------------------
 * FORMATS THIS ACTION CANNOT CARRY
 * -------------------------------------------------------------------------
 *
 * Channels: mono and stereo, Ogg Opus channel mapping family 0. More than two
 * IS refused, at create, with a message that says so. Family 1 would carry it,
 * but a 5.1 bus needs a channel-order decision that belongs to the session and
 * not to an encoder quietly guessing at one. This is the same line
 * action_mp3.c draws, for the same reason.
 */
#include "action.h"
#include "log.h"
#include "resample.h"
#include "ringbuf.h"

#include <opus.h>
#include <ogg/ogg.h>

#include <windows.h>
#include <stdlib.h>
#include <string.h>

/* --- tuning --------------------------------------------------------------- */

/* Write-behind depth. Rings between source and bus absorb mixer jitter only
 * (250 ms, design 3.1); disk stalls, resampler cost AND encoder cost are
 * absorbed HERE, which is why this is an order of magnitude larger. Matches
 * action_mp3.c so the two lossy actions fail the same way under the same
 * pressure. */
#define OGG_WRITE_BEHIND_MS 4000u

/* Never let an exotic session format turn the write-behind into a huge
 * allocation, and never let a tiny one leave no headroom at all. */
#define OGG_RING_MIN_FRAMES 4096u
#define OGG_RING_MAX_BYTES  (16u * 1024u * 1024u)

/* Opus's native rate, and the only rate this action encodes at. */
#define OGG_OPUS_RATE 48000u

/* 20 ms at 48 kHz. See the header comment for why this one. */
#define OGG_FRAME_SAMPLES 960u

/* Frames pulled out of the ring per rb_read. 8192 frames of stereo is 64 KiB,
 * which is eight 20 ms packets: enough to keep the encoder busy without this
 * action holding a large staging allocation per bus. */
#define OGG_CHUNK_TARGET_BYTES (64u * 1024u)
#define OGG_CHUNK_MIN_FRAMES   OGG_FRAME_SAMPLES
#define OGG_CHUNK_MAX_FRAMES   16384u

/* Input frames handed to apr_resample in one go, and the output buffer that
 * has to be able to hold the result. The worst upsample this action allows is
 * 6 kHz -> 48 kHz, so the output buffer is eight times the input block plus
 * slack for the fractional position. */
#define OGG_RS_IN_BLOCK  1024u
#define OGG_RS_OUT_SLACK 64u

/* opus_encode's own recommendation for a maximum output buffer. A 20 ms
 * stereo packet at 510 kbps is under 1300 bytes; this is generous on purpose,
 * because a short buffer is reported as an error rather than a truncation and
 * that would be a recording lost to arithmetic. */
#define OGG_PACKET_MAX_BYTES 4000u

/* The writer sleeps on its event; this backstop only matters if a wakeup is
 * ever missed, which would cost latency rather than data. */
#define OGG_WAKE_TIMEOUT_MS 100u

/* Opus's own bitrate limits, and the defaults argued in the header comment. */
#define OGG_MIN_KBPS          6
#define OGG_MAX_KBPS        510
#define OGG_DEFAULT_KBPS_MONO   64
#define OGG_DEFAULT_KBPS_STEREO 96

/* Where APR_RESAMPLE_MAX_RATIO (8) runs out in each direction. */
#define OGG_MIN_SESSION_RATE 6000u
#define OGG_MAX_SESSION_RATE 384000u

/* --- test seams ----------------------------------------------------------- */
/*
 * Both are zero in every real run and are read ONLY on the writer thread, so
 * neither costs the mixer a single instruction. tests/test_action_ogg.c
 * declares them extern; they are deliberately absent from every public header
 * because nothing outside that file may set them. The technique is
 * action_wav.c's, unchanged.
 *
 *   write_gate       -- a handle the writer waits on before each page write.
 *                       Holding it shut simulates a disk that has stopped
 *                       answering, which is how the "on_audio never waits for
 *                       the disk" claim is proven rather than asserted.
 *   fail_after_bytes -- when nonzero, the first page write that would push the
 *                       file past this many bytes fails, as a full volume
 *                       does.
 */
volatile HANDLE apr_ogg_test_write_gate;
volatile LONG64 apr_ogg_test_fail_after_bytes;

/* --- state ---------------------------------------------------------------- */

typedef struct OggAction {
    /* Immutable after create. */
    wchar_t *path;
    uint32_t sample_rate;      /* the SESSION rate, not the encoder's */
    uint16_t channels;
    uint32_t frame_bytes;
    size_t   chunk_frames;
    int      bitrate_bps;
    int      complexity;

    /* The write-behind. Producer: the mixer thread. Consumer: our writer. */
    RingBuf   *ring;
    RingReader rd;             /* writer thread only */
    HANDLE     wake;
    HANDLE     thread;

    /* Cross-thread flags. Aligned 32-bit values on x64: loads and stores are
     * atomic, and every transition here is one-way. */
    volatile LONG stop;        /* finalize asked the writer to wind up */
    volatile LONG failed;      /* the writer has recorded a failure     */

    /* Owned by the writer thread from CreateThread until it has been joined.
     * create() builds all of this before the thread exists and shutdown
     * touches it after the join, so it never has two users at once. */
    OpusEncoder   *enc;
    AprResampler  *rs;         /* NULL when the session is already 48 kHz */
    ogg_stream_state os;
    int            os_ready;

    float         *stage;      /* chunk_frames * ch, session rate       */
    float         *silence;    /* same size, permanently zero           */
    float         *rs_out;     /* rs_out_cap * ch, 48 kHz               */
    size_t         rs_out_cap;
    float         *acc;        /* OGG_FRAME_SAMPLES * ch, 48 kHz        */
    size_t         acc_fill;
    unsigned char *packet;     /* one encoded Opus packet               */

    uint32_t       pre_skip;   /* 48 kHz samples a player must discard  */
    uint64_t       enc_frames; /* real 48 kHz frames handed to Opus     */
    uint64_t       in_frames;  /* session-rate frames taken from the ring */
    uint64_t       lost_frames;/* frames replaced by silence            */
    ogg_int64_t    packetno;
    uint64_t       file_bytes; /* bytes actually on disk                */
    AprErr         io_err;     /* first failure, verbatim               */
    HANDLE         file;

    int finalized;
} OggAction;

/* --- writer thread: encoding and disk I/O live here and nowhere else ------ */

/* Record the first failure verbatim and stop producing. Everything after this
 * point is about leaving a playable file, not about salvaging the recording. */
static void ogg_fail(OggAction *st, AprErr e)
{
    if (!st->failed) {
        st->io_err = e;
        APR_LOG_ERR(APR_LOG_ERROR, &e);
        st->failed = 1;
    }
}

/* WriteFile until every byte is gone or it refuses. Partial writes are real (a
 * full volume takes what it can) and are retried, not assumed away. */
static AprErr ogg_write_all(OggAction *st, const void *buf, size_t bytes)
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
        p     += got;
        bytes -= got;
    }
    return apr_ok();
}

/*
 * One Ogg page onto the disk. Header and body go in a single logical unit --
 * a page split across a failure is the one thing a demuxer cannot resync past
 * cleanly, so file_bytes only advances when BOTH landed and shutdown truncates
 * anything past it.
 */
static void ogg_write_page(OggAction *st, const ogg_page *og)
{
    HANDLE gate;
    LONG64 limit;
    size_t bytes;
    AprErr e;

    if (st->failed) return;
    if (og->header_len <= 0 && og->body_len <= 0) return;

    bytes = (size_t)og->header_len + (size_t)og->body_len;

    gate = apr_ogg_test_write_gate;
    if (gate) (void)WaitForSingleObject(gate, INFINITE);

    limit = apr_ogg_test_fail_after_bytes;
    if (limit > 0 && st->file_bytes + bytes > (uint64_t)limit) {
        ogg_fail(st, APR_ERR(APR_E_IO,
                             L"simulated disk failure past %lld bytes", limit));
        return;
    }

    e = ogg_write_all(st, og->header, (size_t)og->header_len);
    if (apr_failed(&e)) { ogg_fail(st, e); return; }
    e = ogg_write_all(st, og->body, (size_t)og->body_len);
    if (apr_failed(&e)) { ogg_fail(st, e); return; }

    st->file_bytes += bytes;
}

/* Emit whatever complete pages libogg is now willing to give up. It buffers
 * to roughly 4 KB on its own, which is the granularity a killed recording
 * loses -- see the header comment for why that is left alone. */
static void ogg_pump_pages(OggAction *st)
{
    ogg_page og;
    while (!st->failed && ogg_stream_pageout(&st->os, &og) != 0) {
        ogg_write_page(st, &og);
    }
}

/* Force out everything buffered, complete page or not. Used for the two
 * header packets, which RFC 7845 requires to start their own pages, and once
 * at end of stream. */
static void ogg_flush_pages(OggAction *st)
{
    ogg_page og;
    while (!st->failed && ogg_stream_flush(&st->os, &og) != 0) {
        ogg_write_page(st, &og);
    }
}

/* Encode one 20 ms frame sitting in `acc` and hand the packet to libogg.
 * `real_frames` is how many of the 960 are audio rather than end padding; the
 * granule position counts only those, which is what trims the padding back off
 * at the decoder (header comment). */
static void ogg_emit_frame(OggAction *st, size_t real_frames, int eos)
{
    ogg_packet op;
    int        n;

    if (st->failed) return;

    n = opus_encode_float(st->enc, st->acc, (int)OGG_FRAME_SAMPLES,
                          st->packet, (opus_int32)OGG_PACKET_MAX_BYTES);
    if (n < 0) {
        ogg_fail(st, APR_ERR(APR_E_IO,
                             L"ogg: libopus refused a frame for \"%ls\" (%d: %hs)",
                             st->path, n, opus_strerror(n)));
        return;
    }

    st->enc_frames += real_frames;

    memset(&op, 0, sizeof op);
    op.packet     = st->packet;
    op.bytes      = n;
    op.b_o_s      = 0;
    op.e_o_s      = eos;
    op.granulepos = (ogg_int64_t)(st->pre_skip + st->enc_frames);
    op.packetno   = st->packetno++;

    if (ogg_stream_packetin(&st->os, &op) != 0) {
        ogg_fail(st, APR_ERR(APR_E_IO,
                             L"ogg: libogg refused a packet for \"%ls\"", st->path));
        return;
    }

    if (eos) ogg_flush_pages(st);
    else     ogg_pump_pages(st);
}

/* Accumulate 48 kHz frames into the 20 ms frame buffer, encoding each time it
 * fills. Everything upstream of this is about getting frames to 48 kHz; this
 * is the only place that knows what a packet is. */
static void ogg_accumulate(OggAction *st, const float *pcm, size_t frames)
{
    uint16_t ch = st->channels;

    while (frames > 0 && !st->failed) {
        size_t room = OGG_FRAME_SAMPLES - st->acc_fill;
        size_t take = frames < room ? frames : room;

        memcpy(st->acc + st->acc_fill * ch, pcm, take * ch * sizeof(float));
        st->acc_fill += take;
        pcm          += take * ch;
        frames       -= take;

        if (st->acc_fill == OGG_FRAME_SAMPLES) {
            ogg_emit_frame(st, OGG_FRAME_SAMPLES, 0);
            st->acc_fill = 0;
        }
    }
}

/*
 * Session rate in, 48 kHz into the accumulator. The 48 kHz case skips the
 * resampler entirely -- it is not even created -- so the common session format
 * pays nothing for this branch.
 *
 * The loop respects resample.h's contract: an apr_resample call is given no
 * more input than apr_resampler_input_space() reports, which is what makes
 * "every input frame is absorbed" true and means this file never has to carry
 * a remainder. When the internal buffer is full, take is 0 and the call runs
 * purely to drain output, which frees the space for the next pass.
 */
static void ogg_push_pcm(OggAction *st, const float *pcm, size_t frames)
{
    uint16_t ch = st->channels;

    if (!st->rs) { ogg_accumulate(st, pcm, frames); return; }

    while (frames > 0 && !st->failed) {
        size_t space = apr_resampler_input_space(st->rs);
        size_t take  = frames;
        size_t used  = 0, got;

        if (take > space)            take = space;
        if (take > OGG_RS_IN_BLOCK)  take = OGG_RS_IN_BLOCK;

        got = apr_resample(st->rs, take ? pcm : NULL, take, &used,
                           st->rs_out, st->rs_out_cap);
        if (got > 0) ogg_accumulate(st, st->rs_out, got);

        pcm    += used * ch;
        frames -= used;

        /* Neither input absorbed nor output produced means the resampler
         * cannot make progress, and looping again would spin. It cannot
         * happen with the contract respected above; bailing is cheaper than
         * trusting that. */
        if (used == 0 && got == 0) break;
    }
}

/* Frames the ring overwrote before the writer reached them. They are pushed as
 * silence rather than skipped: see the header comment. */
static void ogg_push_silence(OggAction *st, uint64_t frames)
{
    st->lost_frames += frames;
    APR_WARN(L"ogg: the encoder fell %llu frames behind on \"%ls\"; "
             L"filling with silence to keep the timeline aligned",
             (unsigned long long)frames, st->path);

    while (frames > 0 && !st->failed) {
        size_t n = (frames > (uint64_t)st->chunk_frames)
                 ? st->chunk_frames : (size_t)frames;
        st->in_frames += n;
        ogg_push_pcm(st, st->silence, n);
        frames -= n;
    }
}

/* Move everything the ring holds through resampler, encoder and onto the disk.
 * Writer thread only. */
static void ogg_drain(OggAction *st)
{
    for (;;) {
        uint64_t lost = 0;
        size_t   got  = rb_read(&st->rd, st->stage, st->chunk_frames, &lost);

        if (lost > 0) ogg_push_silence(st, lost);
        if (got == 0) break;

        st->in_frames += got;
        ogg_push_pcm(st, st->stage, got);
    }
}

static DWORD WINAPI ogg_writer_thread(LPVOID param)
{
    OggAction *st = (OggAction *)param;

    for (;;) {
        /* Read the flag BEFORE draining. Anything the mixer wrote before
         * finalize set it is therefore visible to the drain that follows, so
         * one more pass after seeing it is enough -- there is no window in
         * which a frame is left in the ring. */
        LONG stopping = st->stop;

        ogg_drain(st);
        if (stopping) break;

        (void)WaitForSingleObject(st->wake, OGG_WAKE_TIMEOUT_MS);
    }
    return 0;
}

/* --- the two header packets ----------------------------------------------- */

static void ogg_put_u16le(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void ogg_put_u32le(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/*
 * OpusHead, RFC 7845 section 5.1. 19 bytes for channel mapping family 0.
 *
 * These are FORMAT CONSTANTS, not user-facing text: "OpusHead" is a magic
 * number that happens to be spelled in ASCII, in the same category as RIFF's
 * "fmt " chunk id. AGENTS.md rule 6 governs strings a human reads; nothing
 * here is ever shown to anyone.
 */
static void ogg_write_head(OggAction *st)
{
    unsigned char head[19];
    ogg_packet    op;

    memcpy(head, "OpusHead", 8);
    head[8] = 1;                                   /* version               */
    head[9] = (unsigned char)st->channels;
    ogg_put_u16le(head + 10, st->pre_skip);
    ogg_put_u32le(head + 12, st->sample_rate);     /* original, informational */
    ogg_put_u16le(head + 16, 0);                   /* output gain, Q7.8     */
    head[18] = 0;                                  /* mapping family 0      */

    memset(&op, 0, sizeof op);
    op.packet     = head;
    op.bytes      = (long)sizeof head;
    op.b_o_s      = 1;
    op.granulepos = 0;
    op.packetno   = st->packetno++;

    if (ogg_stream_packetin(&st->os, &op) != 0) {
        ogg_fail(st, APR_ERR(APR_E_IO, L"ogg: libogg refused OpusHead for \"%ls\"",
                             st->path));
        return;
    }
    /* RFC 7845 4.1: the ID header is alone on the first page. */
    ogg_flush_pages(st);
}

/*
 * OpusTags, RFC 7845 section 5.2: libopus's vendor string and one ENCODER
 * comment. Same argument as OpusHead about rule 6 -- a Vorbis comment key is
 * a format field with a fixed ASCII spelling, not prose. It is here because a
 * file that says what wrote it is worth a great deal three years later when
 * someone is holding a recording and a bug report.
 */
static void ogg_write_tags(OggAction *st)
{
    static const char k_encoder[] = "ENCODER=apprecorder";
    const char    *vendor = opus_get_version_string();
    size_t         vlen   = strlen(vendor);
    size_t         clen   = sizeof k_encoder - 1;
    size_t         total  = 8 + 4 + vlen + 4 + 4 + clen;
    unsigned char *buf;
    ogg_packet     op;

    buf = (unsigned char *)malloc(total);
    if (!buf) {
        ogg_fail(st, APR_ERR(APR_E_NO_MEMORY, L"ogg: OpusTags for \"%ls\"", st->path));
        return;
    }

    memcpy(buf, "OpusTags", 8);
    ogg_put_u32le(buf + 8, (uint32_t)vlen);
    memcpy(buf + 12, vendor, vlen);
    ogg_put_u32le(buf + 12 + vlen, 1);                  /* one comment */
    ogg_put_u32le(buf + 16 + vlen, (uint32_t)clen);
    memcpy(buf + 20 + vlen, k_encoder, clen);

    memset(&op, 0, sizeof op);
    op.packet     = buf;
    op.bytes      = (long)total;
    op.granulepos = 0;
    op.packetno   = st->packetno++;

    if (ogg_stream_packetin(&st->os, &op) != 0) {
        ogg_fail(st, APR_ERR(APR_E_IO, L"ogg: libogg refused OpusTags for \"%ls\"",
                             st->path));
        free(buf);
        return;
    }
    /* RFC 7845 4.1: audio data does not share a page with the comment header. */
    ogg_flush_pages(st);
    free(buf);
}

/* --- shutdown ------------------------------------------------------------- */

/*
 * Stop the writer, flush the resampler's tail, pad and encode the final frame,
 * close the stream, trim any partial page, close the file. Runs for finalize
 * AND for a destroy that never saw one, and does as much as it can even when
 * the recording already failed: leaving a playable file is the last thing this
 * action owes the user.
 */
static void ogg_shutdown(OggAction *st)
{
    st->stop = 1;
    if (st->wake) SetEvent(st->wake);

    if (st->thread) {
        /* INFINITE deliberately. If the disk really is hung, the alternative
         * is abandoning a thread that is inside WriteFile on this handle and
         * then closing it underneath -- that trades a slow exit for a corrupt
         * file. */
        WaitForSingleObject(st->thread, INFINITE);
        CloseHandle(st->thread);
        st->thread = NULL;
    }
    /* The writer is gone: its fields are ours now, with no synchronisation. */

    if (st->file != INVALID_HANDLE_VALUE && st->os_ready && st->enc) {
        size_t real;

        /* The resampler holds half a kernel of lookahead. Without priming it
         * with silence the last few milliseconds of the recording never come
         * out of it -- and unlike the encoder's own padding there is nothing
         * downstream to notice they are missing. */
        if (st->rs && !st->failed) {
            size_t tail = apr_resampler_lookahead(st->rs) + 1;
            while (tail > 0 && !st->failed) {
                size_t n = tail > st->chunk_frames ? st->chunk_frames : tail;
                /* Not counted in in_frames: this is filter priming, not audio. */
                ogg_push_pcm(st, st->silence, n);
                tail -= n;
            }
        }

        /* Pad the final partial frame with silence. The granule position
         * claims only the real samples, so a decoder trims the difference --
         * this is what makes the file's duration the audio's duration and not
         * the padding's. */
        real = st->acc_fill;
        if (real > 0) {
            memset(st->acc + real * st->channels, 0,
                   (OGG_FRAME_SAMPLES - real) * st->channels * sizeof(float));
        }
        else {
            memset(st->acc, 0, OGG_FRAME_SAMPLES * st->channels * sizeof(float));
        }
        /* Always emit one last packet, so the stream always carries an
         * end-of-stream page. When there was nothing pending it claims zero
         * new samples and is trimmed away entirely, which is the same
         * mechanism as the padding above. */
        ogg_emit_frame(st, real, 1);
        st->acc_fill = 0;
    }

    if (st->os_ready) { ogg_stream_clear(&st->os); st->os_ready = 0; }

    if (st->file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER end;

        /* The file is now exactly as long as the pages that landed whole.
         * Anything past this is the tail of a write that died mid-flight, and
         * a half page is the one thing a demuxer cannot resync past. */
        end.QuadPart = (LONGLONG)st->file_bytes;
        if (SetFilePointerEx(st->file, end, NULL, FILE_BEGIN)) {
            (void)SetEndOfFile(st->file);
        }

        /* No FlushFileBuffers: the cache survives process death, which is the
         * failure this action defends against, and forcing a physical flush
         * would stall the exit for nothing. */
        CloseHandle(st->file);
        st->file = INVALID_HANDLE_VALUE;
    }

    APR_INFO(L"ogg: closed \"%ls\" -- %llu frames in at %u Hz, %llu frames "
             L"encoded at 48 kHz, %llu bytes out, %llu frames lost to silence",
             st->path, (unsigned long long)st->in_frames,
             (unsigned)st->sample_rate, (unsigned long long)st->enc_frames,
             (unsigned long long)st->file_bytes,
             (unsigned long long)st->lost_frames);
}

static void ogg_free(OggAction *st)
{
    if (!st) return;
    if (st->thread) { CloseHandle(st->thread); st->thread = NULL; }
    if (st->os_ready) { ogg_stream_clear(&st->os); st->os_ready = 0; }
    if (st->file != INVALID_HANDLE_VALUE && st->file != NULL) CloseHandle(st->file);
    if (st->wake) CloseHandle(st->wake);
    if (st->enc) opus_encoder_destroy(st->enc);
    apr_resampler_destroy(st->rs);
    rb_destroy(st->ring);
    free(st->stage);
    free(st->silence);
    free(st->rs_out);
    free(st->acc);
    free(st->packet);
    free(st->path);
    free(st);
}

/* --- vtable --------------------------------------------------------------- */

/* An Ogg logical stream is identified by a serial number, and two streams
 * multiplexed into one file must not share one. Nothing here multiplexes, but
 * a session with several Ogg buses produces several files that a user may well
 * concatenate, so this is mixed from the clock, the process and a per-action
 * counter rather than left constant. */
static uint32_t ogg_serialno(void)
{
    static LONG    counter;
    LARGE_INTEGER  qpc;
    uint64_t       mix;

    QueryPerformanceCounter(&qpc);
    mix = (uint64_t)qpc.QuadPart;
    mix ^= (uint64_t)GetCurrentProcessId() << 32;
    mix ^= (uint64_t)(uint32_t)InterlockedIncrement(&counter) * 2654435761u;
    mix ^= mix >> 29;
    mix *= 0xBF58476D1CE4E5B9ull;
    mix ^= mix >> 32;
    return (uint32_t)mix & 0x7FFFFFFFu;
}

static AprErr ogg_create(const AprActionConfig *cfg, void **out_state)
{
    OggAction *st;
    uint64_t   ring_frames, ring_bytes;
    uint32_t   frame_bytes;
    size_t     path_cch, chunk;
    int        kbps, quality, err = OPUS_OK;
    opus_int32 lookahead = 0;
    AprErr     e;

    if (!out_state) {
        return APR_ERR(APR_E_INVALID_ARG, L"ogg: no out_state");
    }
    *out_state = NULL;

    if (!cfg || !cfg->out_path || !cfg->out_path[0]) {
        return APR_ERR(APR_E_INVALID_ARG, L"ogg: no output path");
    }
    if (cfg->sample_rate == 0) {
        return APR_ERR(APR_E_INVALID_ARG, L"ogg: sample rate is zero");
    }
    if (cfg->channels == 0) {
        return APR_ERR(APR_E_INVALID_ARG, L"ogg: channel count is zero");
    }
    /* See the header comment: mapping family 0 is mono and stereo. */
    if (cfg->channels > 2) {
        return APR_ERR(APR_E_UNSUPPORTED,
                       L"ogg: Opus channel mapping family 0 carries mono or "
                       L"stereo, not %u channels", (unsigned)cfg->channels);
    }
    if (cfg->sample_rate < OGG_MIN_SESSION_RATE ||
        cfg->sample_rate > OGG_MAX_SESSION_RATE) {
        return APR_ERR(APR_E_UNSUPPORTED,
                       L"ogg: %u Hz is outside the %u to %u this action can "
                       L"resample to Opus's native 48 kHz",
                       (unsigned)cfg->sample_rate,
                       (unsigned)OGG_MIN_SESSION_RATE,
                       (unsigned)OGG_MAX_SESSION_RATE);
    }

    quality = cfg->quality;
    if (quality < 0 || quality > 11) {
        return APR_ERR(APR_E_INVALID_ARG,
                       L"ogg: quality %d is outside 0 (default) to 11", quality);
    }

    kbps = cfg->bitrate_kbps;
    if (kbps == 0) {
        kbps = (cfg->channels == 1) ? OGG_DEFAULT_KBPS_MONO
                                    : OGG_DEFAULT_KBPS_STEREO;
    }
    if (kbps < OGG_MIN_KBPS || kbps > OGG_MAX_KBPS) {
        return APR_ERR(APR_E_UNSUPPORTED,
                       L"ogg: %d kbps is outside the %d to %d Opus allows",
                       cfg->bitrate_kbps, OGG_MIN_KBPS, OGG_MAX_KBPS);
    }

    frame_bytes = (uint32_t)cfg->channels * (uint32_t)sizeof(float);

    st = (OggAction *)calloc(1, sizeof *st);
    if (!st) return APR_ERR(APR_E_NO_MEMORY, L"ogg: state");
    st->file = INVALID_HANDLE_VALUE;

    st->sample_rate = cfg->sample_rate;
    st->channels    = cfg->channels;
    st->frame_bytes = frame_bytes;
    st->bitrate_bps = kbps * 1000;
    st->complexity  = quality > 0 ? quality - 1 : 10;

    path_cch = wcslen(cfg->out_path) + 1;
    st->path = (wchar_t *)malloc(path_cch * sizeof(wchar_t));
    if (!st->path) { ogg_free(st); return APR_ERR(APR_E_NO_MEMORY, L"ogg: path"); }
    memcpy(st->path, cfg->out_path, path_cch * sizeof(wchar_t));

    chunk = OGG_CHUNK_TARGET_BYTES / frame_bytes;
    if (chunk < OGG_CHUNK_MIN_FRAMES) chunk = OGG_CHUNK_MIN_FRAMES;
    if (chunk > OGG_CHUNK_MAX_FRAMES) chunk = OGG_CHUNK_MAX_FRAMES;
    st->chunk_frames = chunk;
    st->rs_out_cap   = OGG_RS_IN_BLOCK * 8u + OGG_RS_OUT_SLACK;

    st->stage   = (float *)malloc(chunk * cfg->channels * sizeof(float));
    st->silence = (float *)calloc(chunk * cfg->channels, sizeof(float));
    st->rs_out  = (float *)malloc(st->rs_out_cap * cfg->channels * sizeof(float));
    st->acc     = (float *)malloc(OGG_FRAME_SAMPLES * cfg->channels * sizeof(float));
    st->packet  = (unsigned char *)malloc(OGG_PACKET_MAX_BYTES);
    if (!st->stage || !st->silence || !st->rs_out || !st->acc || !st->packet) {
        ogg_free(st);
        return APR_ERR(APR_E_NO_MEMORY, L"ogg: encode buffers for \"%ls\"",
                       cfg->out_path);
    }

    /* --- the resampler ---------------------------------------------------- */
    /*
     * Only when the session is not already at Opus's rate. The ratio is input
     * frames per output frame (resample.h), which for in -> 48000 is
     * in / 48000 exactly; max_ratio has to cover whichever direction is the
     * larger stretch, because apr_resampler_set_ratio_q32 clamps to
     * [1/max_ratio, max_ratio].
     */
    if (cfg->sample_rate != OGG_OPUS_RATE) {
        double ratio = (double)cfg->sample_rate / (double)OGG_OPUS_RATE;
        double max   = ratio > 1.0 ? ratio : 1.0 / ratio;

        if (max > APR_RESAMPLE_MAX_RATIO) max = APR_RESAMPLE_MAX_RATIO;
        e = apr_resampler_create(cfg->channels, max, &st->rs);
        if (apr_failed(&e)) { ogg_free(st); return e; }
        apr_resampler_set_ratio_q32(st->rs,
                                    (uint64_t)(ratio * 4294967296.0 + 0.5));
    }

    /* --- the encoder ------------------------------------------------------ */

    st->enc = opus_encoder_create((opus_int32)OGG_OPUS_RATE, (int)cfg->channels,
                                  OPUS_APPLICATION_AUDIO, &err);
    if (!st->enc || err != OPUS_OK) {
        AprErr enc_err = APR_ERR(APR_E_UNSUPPORTED,
                                 L"ogg: libopus refused 48 kHz x %u channels "
                                 L"(%d: %hs)", (unsigned)cfg->channels, err,
                                 opus_strerror(err));
        ogg_free(st);
        return enc_err;
    }

    (void)opus_encoder_ctl(st->enc, OPUS_SET_BITRATE(st->bitrate_bps));
    (void)opus_encoder_ctl(st->enc, OPUS_SET_VBR(1));
    (void)opus_encoder_ctl(st->enc, OPUS_SET_COMPLEXITY(st->complexity));
    /* The signal is whatever the user routed into the bus, so let libopus's
     * own classifier decide per frame rather than pinning it to speech or
     * music. That classifier is src/analysis.c, which is why the vendored
     * build compiles OPUS_SOURCES_FLOAT. */
    (void)opus_encoder_ctl(st->enc, OPUS_SET_SIGNAL(OPUS_AUTO));

    /* Ask, do not assume. It is 312 samples in every build anyone has shipped,
     * and a wrong pre-skip is a permanent offset against every other bus. */
    if (opus_encoder_ctl(st->enc, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK ||
        lookahead < 0) {
        lookahead = 0;
    }
    st->pre_skip = (uint32_t)lookahead;

    /* --- the write-behind ------------------------------------------------- */

    ring_frames = (uint64_t)cfg->sample_rate * OGG_WRITE_BEHIND_MS / 1000u;
    ring_bytes  = ring_frames * frame_bytes;
    if (ring_bytes > OGG_RING_MAX_BYTES) ring_frames = OGG_RING_MAX_BYTES / frame_bytes;
    if (ring_frames < OGG_RING_MIN_FRAMES) ring_frames = OGG_RING_MIN_FRAMES;

    e = rb_create((size_t)ring_frames, frame_bytes, &st->ring);
    if (apr_failed(&e)) { ogg_free(st); return e; }
    rb_reader_init(&st->rd, st->ring, RB_START_OLDEST);

    /* FILE_SHARE_READ so the recording can be inspected while it is being
     * written -- an Ogg stream is decodable from its first pages, so that is
     * genuinely useful here and not just a courtesy. */
    st->file = CreateFileW(st->path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (st->file == INVALID_HANDLE_VALUE) {
        AprErr open_err = APR_ERR_LAST(L"creating \"%ls\"", st->path);
        ogg_free(st);
        return open_err;
    }

    if (ogg_stream_init(&st->os, (int)ogg_serialno()) != 0) {
        AprErr os_err = APR_ERR(APR_E_NO_MEMORY,
                                L"ogg: libogg would not start a stream for \"%ls\"",
                                st->path);
        ogg_free(st);
        return os_err;
    }
    st->os_ready = 1;

    /* Both header pages are on the disk before create returns. That is what
     * makes a kill one millisecond later leave a file a demuxer recognises
     * rather than an empty one. */
    ogg_write_head(st);
    ogg_write_tags(st);
    if (st->failed) {
        AprErr hdr_err = st->io_err;
        ogg_free(st);
        return hdr_err;
    }

    st->wake = CreateEventW(NULL, FALSE, FALSE, NULL);   /* auto-reset */
    if (!st->wake) {
        AprErr ev_err = APR_ERR_LAST(L"ogg: creating the writer event");
        ogg_free(st);
        return ev_err;
    }

    st->thread = CreateThread(NULL, 0, ogg_writer_thread, st, 0, NULL);
    if (!st->thread) {
        AprErr th_err = APR_ERR_LAST(L"ogg: starting the writer thread");
        ogg_free(st);
        return th_err;
    }

    APR_INFO(L"ogg: recording \"%ls\" at %u Hz x %u ch -> 48 kHz Opus VBR "
             L"%d kbps, complexity %d, pre-skip %u (%llu frame write-behind)",
             st->path, (unsigned)st->sample_rate, (unsigned)st->channels,
             st->bitrate_bps / 1000, st->complexity, (unsigned)st->pre_skip,
             (unsigned long long)rb_capacity_frames(st->ring));

    *out_state = st;
    return apr_ok();
}

/*
 * The mixer thread's entire contact with this action. Bounded, lock-free and
 * allocation-free; see the header comment for why neither the resampler, the
 * encoder nor the file system may appear here.
 */
static AprErr ogg_on_audio(void *state, const float *pcm, size_t frames,
                           uint64_t qpc)
{
    OggAction *st = (OggAction *)state;

    /* Ogg records position as a granule count, and does not need a wall clock
     * to do it: the mixer has already resolved every gap into frames before
     * this call (design 5), so sample position IS timeline position. */
    (void)qpc;

    if (!st) return APR_ERR(APR_E_INVALID_ARG, L"ogg: on_audio with no state");
    if (frames == 0) return apr_ok();
    if (!pcm) return APR_ERR(APR_E_INVALID_ARG, L"ogg: %llu frames from NULL",
                             (unsigned long long)frames);
    if (st->stop) {
        return APR_ERR(APR_E_STATE, L"ogg: audio after finalize on \"%ls\"", st->path);
    }

    rb_write(st->ring, pcm, frames);
    SetEvent(st->wake);

    /* Reported every call rather than once: the bus decides what a dead disk
     * means for the session (design 10), and constructing an AprErr allocates
     * nothing and takes no lock (err.h). */
    if (st->failed) {
        return APR_ERR(APR_E_IO, L"ogg: the writer for \"%ls\" has failed", st->path);
    }
    return apr_ok();
}

static AprErr ogg_finalize(void *state)
{
    OggAction *st = (OggAction *)state;

    if (!st) return APR_ERR(APR_E_INVALID_ARG, L"ogg: finalize with no state");

    if (!st->finalized) {
        ogg_shutdown(st);
        st->finalized = 1;
    }
    /* Idempotent, and it keeps returning the same verdict: finalize is called
     * from several exit paths and none of them should have to remember whether
     * another already ran. */
    return st->failed ? st->io_err : apr_ok();
}

static void ogg_destroy(void *state)
{
    OggAction *st = (OggAction *)state;

    if (!st) return;
    /* A destroy that never saw a finalize still owes the user a playable
     * file. */
    if (!st->finalized) {
        ogg_shutdown(st);
        st->finalized = 1;
    }
    ogg_free(st);
}

/* Registered by core/registry.c under APR_HAVE_ACTION_OGG, which CMake defines
 * from the presence of this file. There is no registration call here and no
 * plugin machinery anywhere (design 3.3). */
/*
 * The extension is "opus", not "ogg", and that is deliberate. RFC 7845
 * section 9 recommends it, and it is the difference between a file every
 * player handles and a file an older Vorbis-only player opens and then cannot
 * decode. The registry id stays "ogg" because that is what design section 8
 * and every session file already written call this action; the id and the
 * extension are separate fields precisely so they can disagree.
 */
const AprActionVTable apr_action_ogg = {
    "ogg",
    L"Ogg Opus",
    L"opus",
    ogg_create,
    ogg_on_audio,
    ogg_finalize,
    ogg_destroy
};
