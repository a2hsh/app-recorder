/*
 * action_wav.c -- the WAV action: interleaved float32 in, float32 WAV out.
 *
 * This is the archival / DAW path. What arrives at on_audio is exactly what
 * lands in the file: no sample-format conversion, no clamping, no dither, no
 * gain. A file written here is bit-identical to what the mixer produced, which
 * is the property that makes WAV the golden-file target the other three
 * encoders are tested against (design section 8).
 *
 * -------------------------------------------------------------------------
 * ON_AUDIO DOES NOT TOUCH THE DISK
 * -------------------------------------------------------------------------
 *
 * on_audio runs on the mixer thread, which is feeding every other bus in the
 * session. A WriteFile there would put an unbounded stall -- antivirus, a
 * spinning disk waking up, a network volume, an SMR write-cache flush, all of
 * which are seconds, not milliseconds -- in front of every other recording.
 * So this action owns its write-behind buffering (action.h, design 3.1):
 *
 *   mixer thread            writer thread (one per action)
 *   -----------             ------------------------------
 *   rb_write()  ---ring--->  rb_read() -> WriteFile()
 *   SetEvent()               waits on the event, 100 ms backstop
 *
 * The ring is core/ringbuf.c -- the single owner of that shape (design 7).
 * Everything on_audio does is lock-free, allocation-free and bounded:
 * rb_write is a memcpy and two stores, and SetEvent on an already-signalled
 * auto-reset event is a no-op. Nothing it calls can wait on a file system.
 *
 * The ring is sized for FOUR SECONDS of stall. Beyond that the producer wins,
 * as ringbuf.h explains, and the frames the writer never reached are gone --
 * but rb_read reports exactly how many, and this file writes that many frames
 * of SILENCE in their place. A recording with a four-second hole still lines
 * up sample-for-sample with every other bus in the session; one that is merely
 * short is permanently desynced against all of them. The count is logged, and
 * the hole is audible, so nothing is silently swallowed.
 *
 * -------------------------------------------------------------------------
 * THE FILE IS PLAYABLE AT EVERY INSTANT, NOT JUST AFTER FINALIZE
 * -------------------------------------------------------------------------
 *
 * RIFF puts its sizes at the front, so the naive encoder can only write them
 * on the way out -- and a recording whose process is killed (or whose machine
 * loses power) is then a file whose header says "zero bytes of audio". Two
 * things prevent that here:
 *
 *   1. A complete, valid header is written up front, before any audio.
 *   2. The writer thread rewrites it, in place, roughly once per second of
 *      audio. Two seeks and 116 bytes -- on the writer thread, so it cannot be
 *      felt by the mixer.
 *
 * So a kill -9 at any moment leaves a file that opens, and that contains
 * everything up to at most the last second. finalize then writes the exact
 * final sizes and truncates to them, and is called on every exit path
 * including error paths; destroy performs the same shutdown if finalize was
 * never called at all.
 *
 * -------------------------------------------------------------------------
 * PAST 4 GB: RF64, DECIDED IN PLACE, NEVER REFUSED AND NEVER SPLIT
 * -------------------------------------------------------------------------
 *
 * RIFF sizes are 32-bit: a WAV tops out just under 4 GiB. That is 3.1 hours of
 * 48 kHz stereo float32, or 47 minutes of eight channels -- well inside what a
 * long multi-track session does, so "it will not happen" is not available.
 *
 * The three options were refuse, roll to a new file, or RF64:
 *
 *   - Refusing loses the tail of a session that may have been running for
 *     hours and cannot be re-recorded. Unacceptable for the archival format.
 *   - Rolling to part-2 keeps every byte but hands the user a manual splice
 *     across every bus, and each roll is a seam the drift work exists to
 *     prevent. Rejected.
 *   - RF64 (EBU Tech 3306) is the standard 64-bit extension of exactly this
 *     file: identical fmt and data chunks, with the sizes moved into a `ds64`
 *     chunk. Reaper, Pro Tools, Nuendo, Audacity, ffmpeg and foobar2000 all
 *     read it. Chosen.
 *
 * The mechanism is the trick RF64 was designed around, and it costs nothing
 * while unused: a 36-byte `JUNK` chunk is reserved immediately after "WAVE"
 * from the first byte of the file. Under 4 GiB the file is an ordinary
 * RIFF/WAVE with a JUNK chunk every reader already ignores. The moment the
 * data would overflow, the periodic header rewrite emits "RF64" in place of
 * "RIFF", "ds64" in place of "JUNK", and 0xFFFFFFFF in the 32-bit size
 * fields. Nothing is moved, no data is rewritten, and the promotion happens
 * on the writer thread like any other header patch. There is no size at which
 * this action refuses to record.
 */
#include "action.h"
#include "log.h"
#include "ringbuf.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

/* --- format constants ----------------------------------------------------- */

#define WAV_HEADER_BYTES   116u   /* RIFF/WAVE + JUNK|ds64 + fmt + fact + data */
#define WAV_DS64_PAYLOAD    28u   /* riffSize + dataSize + sampleCount + table */
#define WAV_FMT_PAYLOAD     40u   /* WAVEFORMATEXTENSIBLE                      */
#define WAV_BYTES_PER_SAMPLE 4u   /* float32, and only float32                 */

/* Offsets into the header, spelled out so the builder reads as the layout. */
enum {
    OFF_RIFF_ID     = 0,
    OFF_RIFF_SIZE   = 4,
    OFF_WAVE_ID     = 8,
    OFF_DS64_ID     = 12,
    OFF_DS64_SIZE   = 16,
    OFF_DS64_RIFF   = 20,
    OFF_DS64_DATA   = 28,
    OFF_DS64_FRAMES = 36,
    OFF_DS64_TABLE  = 44,
    OFF_FMT_ID      = 48,
    OFF_FMT_SIZE    = 52,
    OFF_FMT_TAG     = 56,
    OFF_FMT_CH      = 58,
    OFF_FMT_RATE    = 60,
    OFF_FMT_AVG     = 64,
    OFF_FMT_ALIGN   = 68,
    OFF_FMT_BITS    = 70,
    OFF_FMT_CBSIZE  = 72,
    OFF_FMT_VALID   = 74,
    OFF_FMT_MASK    = 76,
    OFF_FMT_SUBFMT  = 80,
    OFF_FACT_ID     = 96,
    OFF_FACT_SIZE   = 100,
    OFF_FACT_FRAMES = 104,
    OFF_DATA_ID     = 108,
    OFF_DATA_SIZE   = 112
};

/* The largest value a 32-bit size field may hold: 0xFFFFFFFF is RF64's "look
 * in ds64" sentinel and must never be a real size. */
#define WAV_MAX32 0xFFFFFFFEu

/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, serialised. Little-endian Data1/2/3 then
 * Data4 in order: {00000003-0000-0010-8000-00AA00389B71}. Written as bytes so
 * the on-disk order is stated once, here, rather than inferred from a struct
 * layout at three call sites. */
static const unsigned char k_subtype_ieee_float[16] = {
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
};

/* --- tuning --------------------------------------------------------------- */

/* Write-behind depth. Rings between source and bus absorb mixer jitter only
 * (250 ms, design 3.1); disk stalls are absorbed HERE, which is why this is
 * an order of magnitude larger. Four seconds is 1.5 MB at 48 kHz stereo --
 * affordable per bus, and longer than the stalls a busy consumer disk
 * actually produces. */
#define WAV_WRITE_BEHIND_MS 4000u

/* Never let an exotic session format turn the write-behind into a huge
 * allocation, and never let a tiny one leave no headroom at all. */
#define WAV_RING_MIN_FRAMES  4096u
#define WAV_RING_MAX_BYTES   (16u * 1024u * 1024u)

/* Staging buffer for one disk write. 64 KiB is large enough that per-call
 * overhead disappears and small enough to stay off the large-allocation
 * paths; it is also 8192 frames of stereo float32, the boundary the tests
 * probe either side of. */
#define WAV_CHUNK_TARGET_BYTES (64u * 1024u)
#define WAV_CHUNK_MIN_FRAMES   256u
#define WAV_CHUNK_MAX_FRAMES   16384u

/* The writer sleeps on its event; this backstop only matters if a wakeup is
 * ever missed, which would otherwise cost latency rather than data. */
#define WAV_WAKE_TIMEOUT_MS 100u

/* --- test seams ----------------------------------------------------------- */
/*
 * Both are zero in every real run and are read ONLY on the writer thread, so
 * neither costs the mixer a single instruction. tests/test_action_wav.c
 * declares them extern; they are deliberately absent from every public header
 * because nothing outside that file may set them.
 *
 *   write_gate       -- a handle the writer waits on before each payload
 *                       write. Holding it shut simulates a disk that has
 *                       stopped answering, which is how the "on_audio never
 *                       waits for the disk" claim is proven rather than
 *                       asserted.
 *   fail_after_bytes -- when nonzero, the first payload write that would push
 *                       the data chunk past this many bytes fails, as a full
 *                       volume does. Header rewrites are deliberately NOT
 *                       failed: rewriting blocks already allocated is exactly
 *                       what still succeeds on a full disk, and is what makes
 *                       the truncated file playable.
 */
volatile HANDLE apr_wav_test_write_gate;
volatile LONG64 apr_wav_test_fail_after_bytes;

/* --- state ---------------------------------------------------------------- */

typedef struct WavAction {
    /* Immutable after create. */
    wchar_t  *path;
    uint32_t  sample_rate;
    uint16_t  channels;
    uint32_t  frame_bytes;
    uint64_t  patch_interval;   /* data bytes between header rewrites */
    size_t    chunk_frames;

    /* The write-behind. Producer: the mixer thread. Consumer: our writer. */
    RingBuf   *ring;
    RingReader rd;              /* writer thread only */
    HANDLE     wake;
    HANDLE     thread;

    /* Cross-thread flags. Aligned 32-bit values on x64: loads and stores are
     * atomic, and every transition here is one-way. */
    volatile LONG stop;         /* finalize asked the writer to wind up */
    volatile LONG failed;       /* the writer has recorded an I/O failure */

    /* Writer thread only until the thread has been joined. */
    unsigned char *stage;       /* chunk_frames * frame_bytes                 */
    unsigned char *silence;     /* same size, permanently zero                */
    uint64_t       data_bytes;  /* payload bytes actually on disk             */
    uint64_t       patched;     /* data_bytes as of the last header rewrite   */
    uint64_t       lost_frames; /* frames replaced by silence                 */
    AprErr         io_err;      /* first failure, verbatim                    */
    HANDLE         file;

    int finalized;
} WavAction;

/* --- little-endian writers ------------------------------------------------ */

static void put16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static void put64(unsigned char *p, uint64_t v)
{
    put32(p, (uint32_t)(v & 0xFFFFFFFFu));
    put32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static void puttag(unsigned char *p, const char *tag)
{
    memcpy(p, tag, 4);
}

/* Standard speaker mask for a channel count. Zero above 8: an unknown mask is
 * better than a wrong one, and every reader treats 0 as "unassigned". */
static uint32_t channel_mask(uint16_t channels)
{
    switch (channels) {
    case 1:  return 0x00000004u;                 /* FC                        */
    case 2:  return 0x00000003u;                 /* FL FR                     */
    case 3:  return 0x00000007u;                 /* FL FR FC                  */
    case 4:  return 0x00000033u;                 /* FL FR BL BR   (quad)      */
    case 5:  return 0x00000037u;                 /* FL FR FC BL BR            */
    case 6:  return 0x0000003Fu;                 /* 5.1                       */
    case 7:  return 0x0000013Fu;                 /* 5.1 + BC                  */
    case 8:  return 0x0000063Fu;                 /* 7.1                       */
    default: return 0u;
    }
}

/* ---------------------------------------------------------------------------
 * apr_wav_header_build -- the whole 116-byte header for a file that currently
 * holds `data_bytes` of payload. Pure: same inputs, same bytes, no state.
 * That is what lets it serve create, every periodic rewrite and finalize, and
 * be tested directly at sizes no test could ever write (see the RF64 case in
 * tests/test_action_wav.c). Returns the number of bytes written, always
 * WAV_HEADER_BYTES; `dst` must have room for that many.
 *
 * Not static, and not in a public header: registry.c has no use for it and
 * only the test declares it.
 * ------------------------------------------------------------------------- */
size_t apr_wav_header_build(unsigned char *dst, uint32_t sample_rate,
                            uint16_t channels, uint64_t data_bytes)
{
    uint32_t block_align = (uint32_t)channels * WAV_BYTES_PER_SAMPLE;
    uint64_t frames      = block_align ? data_bytes / block_align : 0;
    uint64_t riff_size   = (uint64_t)(WAV_HEADER_BYTES - 8u) + data_bytes;
    int      rf64        = (riff_size > WAV_MAX32);

    memset(dst, 0, WAV_HEADER_BYTES);

    /* RIFF, or RF64 with the 32-bit fields deferring to ds64. */
    puttag(dst + OFF_RIFF_ID, rf64 ? "RF64" : "RIFF");
    put32(dst + OFF_RIFF_SIZE, rf64 ? 0xFFFFFFFFu : (uint32_t)riff_size);
    puttag(dst + OFF_WAVE_ID, "WAVE");

    /* The reserved chunk. Identical size either way -- promotion is a rename
     * plus three numbers, never a move. */
    puttag(dst + OFF_DS64_ID, rf64 ? "ds64" : "JUNK");
    put32(dst + OFF_DS64_SIZE, WAV_DS64_PAYLOAD);
    if (rf64) {
        put64(dst + OFF_DS64_RIFF, riff_size);
        put64(dst + OFF_DS64_DATA, data_bytes);
        put64(dst + OFF_DS64_FRAMES, frames);
        put32(dst + OFF_DS64_TABLE, 0);          /* no chunk-size table */
    }

    /* fmt: WAVE_FORMAT_EXTENSIBLE + IEEE_FLOAT. The plain 0x0003 tag would
     * also describe float32, but EXTENSIBLE is what carries a channel mask and
     * what WASAPI hands us in the first place -- keeping the two the same
     * removes a translation nobody would benefit from. */
    puttag(dst + OFF_FMT_ID, "fmt ");
    put32(dst + OFF_FMT_SIZE, WAV_FMT_PAYLOAD);
    put16(dst + OFF_FMT_TAG, 0xFFFEu);           /* WAVE_FORMAT_EXTENSIBLE */
    put16(dst + OFF_FMT_CH, channels);
    put32(dst + OFF_FMT_RATE, sample_rate);
    put32(dst + OFF_FMT_AVG, sample_rate * block_align);
    put16(dst + OFF_FMT_ALIGN, (uint16_t)block_align);
    put16(dst + OFF_FMT_BITS, (uint16_t)(WAV_BYTES_PER_SAMPLE * 8u));
    put16(dst + OFF_FMT_CBSIZE, 22u);
    put16(dst + OFF_FMT_VALID, (uint16_t)(WAV_BYTES_PER_SAMPLE * 8u));
    put32(dst + OFF_FMT_MASK, channel_mask(channels));
    memcpy(dst + OFF_FMT_SUBFMT, k_subtype_ieee_float, sizeof k_subtype_ieee_float);

    /* fact: required for non-PCM WAV, and the field a player uses for
     * duration when it trusts the header over the data chunk. */
    puttag(dst + OFF_FACT_ID, "fact");
    put32(dst + OFF_FACT_SIZE, 4u);
    put32(dst + OFF_FACT_FRAMES,
          (rf64 || frames > WAV_MAX32) ? 0xFFFFFFFFu : (uint32_t)frames);

    puttag(dst + OFF_DATA_ID, "data");
    put32(dst + OFF_DATA_SIZE,
          rf64 ? 0xFFFFFFFFu : (uint32_t)data_bytes);

    return WAV_HEADER_BYTES;
}

/* --- writer thread: disk I/O lives here and nowhere else ------------------ */

/* Record the first failure verbatim and stop writing payload. Everything
 * after this point is about leaving the file playable, not about salvaging
 * the recording. Writer thread only (or the finalizing thread once joined). */
static void wav_fail(WavAction *st, AprErr e)
{
    if (!st->failed) {
        st->io_err = e;
        APR_LOG_ERR(APR_LOG_ERROR, &e);
        st->failed = 1;
    }
}

/* WriteFile until every byte is gone or it refuses. Partial writes are real
 * (a full volume takes what it can) and are retried, not assumed away. */
static AprErr wav_write_all(WavAction *st, const void *buf, size_t bytes)
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

/* One payload write. `data_bytes` only advances on complete success, so the
 * header can never claim more than the file holds -- if a write dies halfway,
 * the surplus bytes are trailing junk that finalize truncates away. */
static void wav_write_payload(WavAction *st, const unsigned char *buf,
                              size_t bytes)
{
    HANDLE gate;
    LONG64 limit;
    AprErr e;

    if (st->failed || bytes == 0) return;

    gate = apr_wav_test_write_gate;
    if (gate) (void)WaitForSingleObject(gate, INFINITE);

    limit = apr_wav_test_fail_after_bytes;
    if (limit > 0 && st->data_bytes + bytes > (uint64_t)limit) {
        wav_fail(st, APR_ERR(APR_E_IO,
                             L"simulated disk failure past %lld bytes", limit));
        return;
    }

    e = wav_write_all(st, buf, bytes);
    if (apr_failed(&e)) { wav_fail(st, e); return; }
    st->data_bytes += bytes;
}

/* Frames the ring overwrote before the writer reached them. They are written
 * as silence rather than skipped: see the header comment. */
static void wav_write_silence(WavAction *st, uint64_t frames)
{
    st->lost_frames += frames;
    APR_WARN(L"wav: disk fell %llu frames behind on \"%ls\"; "
             L"filling with silence to keep the timeline aligned",
             (unsigned long long)frames, st->path);

    while (frames > 0 && !st->failed) {
        size_t n = (frames > (uint64_t)st->chunk_frames)
                 ? st->chunk_frames : (size_t)frames;
        wav_write_payload(st, st->silence, n * st->frame_bytes);
        frames -= n;
    }
}

/* Rewrite the header in place for the current data_bytes. `seek_back` returns
 * the file pointer to the append position; finalize does not need it.
 *
 * A failed seek-back is fatal to the recording -- appending at the wrong
 * offset would corrupt what is already good -- so it stops the writer. */
static void wav_write_header(WavAction *st, int seek_back)
{
    unsigned char hdr[WAV_HEADER_BYTES];
    LARGE_INTEGER to;
    AprErr        e;

    apr_wav_header_build(hdr, st->sample_rate, st->channels, st->data_bytes);

    to.QuadPart = 0;
    if (!SetFilePointerEx(st->file, to, NULL, FILE_BEGIN)) {
        wav_fail(st, APR_ERR_LAST(L"seeking to the header of \"%ls\"", st->path));
        return;
    }

    e = wav_write_all(st, hdr, sizeof hdr);
    if (apr_failed(&e)) wav_fail(st, e);

    if (seek_back) {
        to.QuadPart = (LONGLONG)((uint64_t)WAV_HEADER_BYTES + st->data_bytes);
        if (!SetFilePointerEx(st->file, to, NULL, FILE_BEGIN)) {
            wav_fail(st, APR_ERR_LAST(L"returning to the append point of \"%ls\"",
                                      st->path));
        }
    }
    st->patched = st->data_bytes;
}

/* Move everything the ring holds onto the disk, then keep the header roughly
 * a second behind the truth. Writer thread only. */
static void wav_drain(WavAction *st)
{
    for (;;) {
        uint64_t lost = 0;
        size_t   got  = rb_read(&st->rd, st->stage, st->chunk_frames, &lost);

        if (lost > 0) wav_write_silence(st, lost);
        if (got == 0) break;
        wav_write_payload(st, st->stage, got * st->frame_bytes);
    }

    if (st->data_bytes - st->patched >= st->patch_interval) {
        wav_write_header(st, 1);
    }
}

static DWORD WINAPI wav_writer_thread(LPVOID param)
{
    WavAction *st = (WavAction *)param;

    for (;;) {
        /* Read the flag BEFORE draining. Anything the mixer wrote before
         * finalize set it is therefore visible to the drain that follows, so
         * one more pass after seeing it is enough -- there is no window in
         * which a frame is left in the ring. */
        LONG stopping = st->stop;

        wav_drain(st);
        if (stopping) break;

        (void)WaitForSingleObject(st->wake, WAV_WAKE_TIMEOUT_MS);
    }
    return 0;
}

/* --- shutdown ------------------------------------------------------------- */

/* Stop the writer, write the exact final header, trim any partial tail, close
 * the file. Runs for finalize AND for a destroy that never saw one, and does
 * as much as it can even when the recording already failed: leaving a playable
 * file is the last thing this action owes the user. */
static void wav_shutdown(WavAction *st)
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

        wav_write_header(st, 0);

        /* The file is now exactly as long as the header says. Anything past
         * this is the tail of a write that died mid-flight. */
        end.QuadPart = (LONGLONG)((uint64_t)WAV_HEADER_BYTES + st->data_bytes);
        if (SetFilePointerEx(st->file, end, NULL, FILE_BEGIN)) {
            (void)SetEndOfFile(st->file);
        }

        /* No FlushFileBuffers: the cache survives process death, which is the
         * failure this action defends against, and forcing a physical flush of
         * a multi-gigabyte file would stall the exit for nothing. */
        CloseHandle(st->file);
        st->file = INVALID_HANDLE_VALUE;
    }

    APR_INFO(L"wav: closed \"%ls\" -- %llu frames, %llu lost to silence",
             st->path,
             (unsigned long long)(st->frame_bytes
                                  ? st->data_bytes / st->frame_bytes : 0),
             (unsigned long long)st->lost_frames);
}

static void wav_free(WavAction *st)
{
    if (!st) return;
    if (st->thread) { CloseHandle(st->thread); st->thread = NULL; }
    if (st->file != INVALID_HANDLE_VALUE && st->file != NULL) CloseHandle(st->file);
    if (st->wake) CloseHandle(st->wake);
    rb_destroy(st->ring);
    free(st->stage);
    free(st->silence);
    free(st->path);
    free(st);
}

/* --- vtable --------------------------------------------------------------- */

static AprErr wav_create(const AprActionConfig *cfg, void **out_state)
{
    WavAction    *st;
    unsigned char hdr[WAV_HEADER_BYTES];
    uint64_t      ring_frames, ring_bytes;
    uint32_t      block_align;
    size_t        path_cch, chunk;
    AprErr        e;

    if (!out_state) {
        return APR_ERR(APR_E_INVALID_ARG, L"wav: no out_state");
    }
    *out_state = NULL;

    if (!cfg || !cfg->out_path || !cfg->out_path[0]) {
        return APR_ERR(APR_E_INVALID_ARG, L"wav: no output path");
    }
    if (cfg->sample_rate == 0) {
        return APR_ERR(APR_E_INVALID_ARG, L"wav: sample rate is zero");
    }
    if (cfg->channels == 0) {
        return APR_ERR(APR_E_INVALID_ARG, L"wav: channel count is zero");
    }

    /* nBlockAlign and nAvgBytesPerSec are 16- and 32-bit: a format that
     * cannot be described in the header must be refused now, not discovered
     * as a corrupt file later. */
    block_align = (uint32_t)cfg->channels * WAV_BYTES_PER_SAMPLE;
    if (block_align > 0xFFFFu) {
        return APR_ERR(APR_E_UNSUPPORTED,
                       L"wav: %u channels of float32 exceed the 16-bit block align",
                       (unsigned)cfg->channels);
    }
    if ((uint64_t)cfg->sample_rate * block_align > 0xFFFFFFFFu) {
        return APR_ERR(APR_E_UNSUPPORTED,
                       L"wav: %u Hz x %u channels exceeds the 32-bit byte rate",
                       (unsigned)cfg->sample_rate, (unsigned)cfg->channels);
    }

    /* bitrate_kbps and quality are lossy-codec knobs; float32 WAV has neither
     * a bitrate to choose nor a quality to trade. Ignored, not rejected: a
     * session file that carries them for every action must still load. */

    st = (WavAction *)calloc(1, sizeof *st);
    if (!st) return APR_ERR(APR_E_NO_MEMORY, L"wav: state");
    st->file = INVALID_HANDLE_VALUE;

    st->sample_rate = cfg->sample_rate;
    st->channels    = cfg->channels;
    st->frame_bytes = block_align;
    st->patch_interval = (uint64_t)cfg->sample_rate * block_align;   /* ~1 s */

    path_cch = wcslen(cfg->out_path) + 1;
    st->path = (wchar_t *)malloc(path_cch * sizeof(wchar_t));
    if (!st->path) { wav_free(st); return APR_ERR(APR_E_NO_MEMORY, L"wav: path"); }
    memcpy(st->path, cfg->out_path, path_cch * sizeof(wchar_t));

    /* Staging and silence buffers: one disk write each. */
    chunk = WAV_CHUNK_TARGET_BYTES / block_align;
    if (chunk < WAV_CHUNK_MIN_FRAMES) chunk = WAV_CHUNK_MIN_FRAMES;
    if (chunk > WAV_CHUNK_MAX_FRAMES) chunk = WAV_CHUNK_MAX_FRAMES;
    st->chunk_frames = chunk;

    st->stage   = (unsigned char *)malloc(chunk * block_align);
    st->silence = (unsigned char *)calloc(chunk, block_align);
    if (!st->stage || !st->silence) {
        wav_free(st);
        return APR_ERR(APR_E_NO_MEMORY, L"wav: %zu byte staging buffers",
                       (size_t)chunk * block_align);
    }

    /* The write-behind itself. */
    ring_frames = (uint64_t)cfg->sample_rate * WAV_WRITE_BEHIND_MS / 1000u;
    ring_bytes  = ring_frames * block_align;
    if (ring_bytes > WAV_RING_MAX_BYTES) ring_frames = WAV_RING_MAX_BYTES / block_align;
    if (ring_frames < WAV_RING_MIN_FRAMES) ring_frames = WAV_RING_MIN_FRAMES;

    e = rb_create((size_t)ring_frames, block_align, &st->ring);
    if (apr_failed(&e)) { wav_free(st); return e; }
    rb_reader_init(&st->rd, st->ring, RB_START_OLDEST);

    /* FILE_SHARE_READ so the recording can be inspected (or streamed) while
     * it is being written -- the periodic header rewrite is what makes that
     * worth allowing. */
    st->file = CreateFileW(st->path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (st->file == INVALID_HANDLE_VALUE) {
        AprErr open_err = APR_ERR_LAST(L"creating \"%ls\"", st->path);
        wav_free(st);
        return open_err;
    }

    /* A complete header before a single frame of audio: from this moment the
     * file on disk is a valid (empty) WAV, whatever happens next. */
    apr_wav_header_build(hdr, st->sample_rate, st->channels, 0);
    e = wav_write_all(st, hdr, sizeof hdr);
    if (apr_failed(&e)) { wav_free(st); return e; }

    st->wake = CreateEventW(NULL, FALSE, FALSE, NULL);   /* auto-reset */
    if (!st->wake) {
        AprErr ev_err = APR_ERR_LAST(L"wav: creating the writer event");
        wav_free(st);
        return ev_err;
    }

    st->thread = CreateThread(NULL, 0, wav_writer_thread, st, 0, NULL);
    if (!st->thread) {
        AprErr th_err = APR_ERR_LAST(L"wav: starting the writer thread");
        wav_free(st);
        return th_err;
    }

    APR_INFO(L"wav: recording \"%ls\" at %u Hz x %u ch float32 "
             L"(%llu frame write-behind)",
             st->path, (unsigned)st->sample_rate, (unsigned)st->channels,
             (unsigned long long)rb_capacity_frames(st->ring));

    *out_state = st;
    return apr_ok();
}

/*
 * The mixer thread's entire contact with this action. Bounded, lock-free and
 * allocation-free; see the header comment for why nothing here may touch the
 * file system.
 */
static AprErr wav_on_audio(void *state, const float *pcm, size_t frames,
                           uint64_t qpc)
{
    WavAction *st = (WavAction *)state;

    /* WAV has nowhere to record a wall-clock position, and does not need one:
     * the mixer has already resolved every gap into frames before this call
     * (design 5), so sample position IS timeline position. */
    (void)qpc;

    if (!st) return APR_ERR(APR_E_INVALID_ARG, L"wav: on_audio with no state");
    if (frames == 0) return apr_ok();
    if (!pcm) return APR_ERR(APR_E_INVALID_ARG, L"wav: %zu frames from NULL", frames);
    if (st->stop) {
        return APR_ERR(APR_E_STATE, L"wav: audio after finalize on \"%ls\"", st->path);
    }

    rb_write(st->ring, pcm, frames);
    SetEvent(st->wake);

    /* Reported every call rather than once: the bus decides what a dead disk
     * means for the session (design 10), and constructing an AprErr allocates
     * nothing and takes no lock (err.h). */
    if (st->failed) {
        return APR_ERR(APR_E_IO, L"wav: the writer for \"%ls\" has failed", st->path);
    }
    return apr_ok();
}

static AprErr wav_finalize(void *state)
{
    WavAction *st = (WavAction *)state;

    if (!st) return APR_ERR(APR_E_INVALID_ARG, L"wav: finalize with no state");

    if (!st->finalized) {
        wav_shutdown(st);
        st->finalized = 1;
    }
    /* Idempotent, and it keeps returning the same verdict: finalize is called
     * from several exit paths and none of them should have to remember
     * whether another already ran. */
    return st->failed ? st->io_err : apr_ok();
}

static void wav_destroy(void *state)
{
    WavAction *st = (WavAction *)state;

    if (!st) return;
    /* A destroy that never saw a finalize still owes the user a playable
     * file. */
    if (!st->finalized) {
        wav_shutdown(st);
        st->finalized = 1;
    }
    wav_free(st);
}

/* Registered by core/registry.c, which declares
 * `extern const AprActionVTable apr_action_wav;` -- there is no
 * registration call here, and no plugin machinery anywhere (design 3.3). */
const AprActionVTable apr_action_wav = {
    "wav",
    APR_S_ACTION_NAME_WAV,
    L"wav",
    wav_create,
    wav_on_audio,
    wav_finalize,
    wav_destroy
};
