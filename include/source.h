/*
 * source.h -- one capture, one ring, and one reader per consuming bus.
 *
 * A Source is the join between the capture layer (which owns a thread and
 * fills a ring) and the graph (which may point SEVERAL buses at that same
 * ring). Everything that is per-consumer -- a ring cursor, a resampler, a
 * drift controller -- lives in an AprSourceReader, and everything that is
 * per-source -- the capture, the ring, the clock anchor, health -- lives in
 * the AprSource. That split is what makes this a graph and not a tree.
 *
 * REFCOUNT IS BUS BOOKKEEPING, NOT BUFFER LIFETIME (design 3.1)
 *
 *   apr_source_refcount() counts open readers so the UI can say "Teams feeds
 *   two buses" and so removing the last edge is distinguishable from removing
 *   one of several. The RING is not told. Consumers hold their own cursors,
 *   the producer never looks at them, and teaching ringbuf about readers would
 *   put a shared counter on an audio callback's path for no gain.
 *
 * RING SIZING: 250 ms, and it is deliberately not more.
 *
 *   The ring absorbs MIXER SCHEDULING JITTER only -- 96 KB per source at 48 kHz
 *   stereo float32. Disk stalls are absorbed by write-behind buffering inside
 *   each action, so a slow encoder cannot back-pressure a buffer that every
 *   other bus is also reading from. Sizing rings for disk instead would cost
 *   ~384 KB per source per second of tolerance for nothing.
 *
 * THE TWO KINDS ARE NOT SYMMETRIC (design 5.1, corrected by measurement)
 *
 *   A PROCESS TAP is the reference timeline. Measured: gapless, 100% fill,
 *   every buffer exactly 480 frames, silence delivered as real 0.0f samples,
 *   AUDCLNT_BUFFERFLAGS_SILENT never set in 190 s. It is consumed directly --
 *   no resampler is even allocated for one. Anything else would be filtering
 *   audio that is already correct.
 *
 *   A DEVICE CAPTURE rides a hardware crystal and is the only thing in the
 *   system that drifts. It gets a resampler and a PI controller, and its
 *   backlog is held at the mixer's lookbehind so its audio lands at its true
 *   position on the bus timeline rather than a fixed latency later.
 *
 * ALIGNMENT OVER CONTENT
 *
 *   When the ring laps a reader (the mixer stalled for longer than 250 ms),
 *   the frames are gone. The reader emits exactly that many frames of silence
 *   and carries on at the right absolute position, rather than sliding
 *   everything after the gap earlier. A dropout you can hear is recoverable; a
 *   file that is 40 ms out from every other file for the next three hours is
 *   not.
 *
 * THREAD SAFETY: one AprSourceReader belongs to one thread (its bus). The
 * AprSource is created and destroyed with no bus running. apr_source_pull
 * allocates nothing and takes no lock.
 */
#ifndef APPRECORDER_SOURCE_H
#define APPRECORDER_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "capture.h"
#include "clock.h"
#include "err.h"
#include "ringbuf.h"

/* Mixer jitter the ring absorbs. See the header comment. */
#define APR_SOURCE_RING_MS 250

/* Buses one source may feed at once. */
#define APR_MAX_READERS_PER_SOURCE 32

/* Characters in a source's display name, terminator included. */
#define APR_NAME_CCH 64

typedef uint32_t AprSourceId;

typedef struct AprSource       AprSource;
typedef struct AprSourceReader AprSourceReader;

/* ---------------------------------------------------------------------------
 * Lifetime. Create and destroy with no bus ticking.
 * ------------------------------------------------------------------------- */

/* Builds the ring, then the capture behind it (apr_capture_create is the one
 * place allowed to switch on the source kind, and this is its only caller in
 * core). No thread runs until apr_source_start. */
AprErr apr_source_create(AprSourceId id, const wchar_t *name,
                         const AprCaptureConfig *cfg, AprSource **out);

/* Stops the capture, releases the ring. Every reader must be closed first. */
void apr_source_destroy(AprSource *s);

AprErr apr_source_start(AprSource *s);
void   apr_source_stop(AprSource *s);

/* ---------------------------------------------------------------------------
 * Identity and state -- the queries the canvas and the accessibility tree both
 * read. Neither view is derived from the other; both project this.
 * ------------------------------------------------------------------------- */

AprSourceId    apr_source_id(const AprSource *s);
AprSourceKind  apr_source_kind(const AprSource *s);
const wchar_t *apr_source_name(const AprSource *s);
uint32_t       apr_source_rate(const AprSource *s);
uint16_t       apr_source_channels(const AprSource *s);
int            apr_source_refcount(const AprSource *s);

/* Nonzero for a source that IS the reference timeline and is consumed
 * untouched, rather than one that must be corrected onto it.
 *
 * Defaults from the kind: a process tap is the reference (measured gapless,
 * 100% fill, locked to the audio engine), a device capture is not (it rides a
 * crystal). A fake defaults to NOT the reference, because its whole purpose is
 * to stand in for a drifting crystal. */
int apr_source_is_reference(const AprSource *s);

/* Override the above. This exists so the reference path -- the one that must
 * allocate no resampler and touch no sample -- is testable without a real
 * application running and rendering audio, which design 4.3 requires and
 * AGENTS.md rule 1 makes awkward to arrange. Setting it once a reader is open
 * has no effect: the reader has already decided whether it needs a
 * resampler. */
void apr_source_set_reference(AprSource *s, int is_reference);

RingBuf    *apr_source_ring(AprSource *s);
AprCapture *apr_source_capture(AprSource *s);

/* Refresh the cached health snapshot from the capture and anchor the source's
 * clock the first time a frame's arrival time is known. Cheap; the bus calls
 * it once per tick before pulling. `out` may be NULL. */
void apr_source_poll(AprSource *s, AprCaptureStatus *out);

int apr_source_alive(const AprSource *s);   /* 0 once known dead */
int apr_source_muted(const AprSource *s);   /* session volume 0: records silence */
int apr_source_anchored(const AprSource *s);

const AprClock *apr_source_clock(const AprSource *s);

/* Frames the capture has delivered so far. */
uint64_t apr_source_frames(const AprSource *s);

/* ---------------------------------------------------------------------------
 * Readers -- one per consuming bus
 * ------------------------------------------------------------------------- */

/* `target_backlog` is the frames of audio a device source keeps buffered ahead
 * of the reader, and it should be the mixer's lookbehind: a bus running that
 * far behind wall clock has exactly that much waiting in every ring, so
 * holding it there puts the source's audio at its TRUE bus position instead of
 * a fixed latency later. Ignored for process taps, which need no jitter
 * buffer. Raises the source's refcount. */
AprErr apr_source_reader_open(AprSource *s, double target_backlog,
                              AprSourceReader **out);

/* Lowers the refcount. NULL is allowed. */
void apr_source_reader_close(AprSourceReader *rd);

/* What one pull produced. The caller mixes `frames` frames from
 * `out + lead * channels` at destination offset `lead`. */
typedef struct AprSourcePull {
    size_t   lead;       /* silence frames before this source's audio starts */
    size_t   frames;     /* frames written, at out + lead * channels */
    uint64_t lost;       /* frames replaced by silence after a ring overrun */
    int      underrun;   /* the source had less than the tick asked for */
    int      running;    /* the source has begun contributing */
} AprSourcePull;

/* Produce the audio this source contributes to bus frames
 * [bus_frame, bus_frame + frames).
 *
 * `bus_clock` is the BUS's clock, anchored when the bus started; it is what
 * places this source's first frame on the bus timeline, and it is why two
 * sources that started 40 ms apart stay 40 ms apart. `now_ticks` is QPC at the
 * tick, and drives the drift controller.
 *
 * `out` must have room for frames * apr_source_channels() floats. Only the
 * region [lead, lead + frames) is written -- the caller's accumulator supplies
 * the rest, which is silence for this source and whatever other sources have
 * already contributed. */
AprSourcePull apr_source_pull(AprSourceReader *rd, const AprClock *bus_clock,
                              uint64_t bus_frame, uint64_t now_ticks,
                              float *out, size_t frames);

/* ---------------------------------------------------------------------------
 * Reader diagnostics. These are what the sync tests assert on -- backlog is
 * the alignment error, not a health metric.
 * ------------------------------------------------------------------------- */

/* Frames produced minus frames consumed, right now, exact to 2^-32. */
double apr_source_reader_backlog(const AprSourceReader *rd);

/* Backlog minus the target: the number the multi-hour test requires to stay
 * below one sample. */
double apr_source_reader_error(const AprSourceReader *rd);

/* The correction currently applied, in ppm. Display only. */
double apr_source_reader_trim_ppm(const AprSourceReader *rd);

/* Output frames this reader has placed on the bus timeline. */
uint64_t apr_source_reader_out_frames(const AprSourceReader *rd);

/* Bus frame this reader's first output frame landed on. */
uint64_t apr_source_reader_first_bus_frame(const AprSourceReader *rd);

#endif /* APPRECORDER_SOURCE_H */
