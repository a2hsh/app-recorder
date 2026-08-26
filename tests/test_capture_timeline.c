/*
 * test_capture_timeline.c -- what one arriving WASAPI packet does to the clock.
 *
 * NO HARDWARE, NO COM, NO AUDIO. apr_wasapi_packet_start and
 * apr_wasapi_packet_gap are pure arithmetic precisely so that the two
 * decisions the whole of design 5.2 rests on -- where a source's frame 0 sits
 * on the master timeline, and how many frames the engine failed to hand over
 * -- can be checked without a driver.
 *
 * QPF is deliberately 3,579,545 Hz here, matching tests/test_clock.c: the spike
 * machine's 10 MHz makes a tick and a 100 ns unit coincide and hides a whole
 * class of unit bug (clock.h says so in as many words). The price is that
 * frames and ticks do not divide evenly, so every conversion floors and a
 * frame count can land one low -- clock.h bounds that at strictly less than
 * one frame however long the session runs, and ASSERT_FRAMES below allows
 * exactly that much and no more.
 *
 * THE PACKET MODEL, stated because a wrong one makes this whole file lie
 *
 *   A shared-mode capture client delivers one PERIOD per event, so packet k
 *   covers the interval [arrival(k-1), arrival(k)] and its length in frames is
 *   that interval. The first packet is the one with no predecessor: it covers
 *   the period that ended the instant it arrived. So an arrival advances by
 *   the size of the packet that is ARRIVING, not the one that just left --
 *   modelling it the other way round invents a hole and then congratulates the
 *   code for filling it.
 *
 * WHAT THE SWEEP GOT WRONG, AND WHAT IT GOT RIGHT
 *
 *   The claim (BUGS.md M6) was that anchoring at packet ARRIVAL makes device
 *   gap-fill "systematically short by ~one packet": a dropout of G fills only
 *   G-480, and a dropout shorter than one packet never fills at all.
 *
 *   THAT IS WRONG. steady_state_with_uniform_packets_never_fills and
 *   a_dropout_shorter_than_one_packet_is_still_filled both pass against the
 *   OLD arithmetic too, and the reason is that the gap was measured against
 *   the frames delivered BEFORE the current packet -- an omission that
 *   cancelled the arrival-time anchor exactly. With uniform packets the old
 *   delta was 0, and a real dropout of G gave exactly G.
 *
 *   The anchor was still one packet late, and that is a real error with two
 *   real consequences, one pinned by each of:
 *
 *     anchor_is_the_first_frames_capture_time_not_its_arrival -- a source's
 *       whole timeline sat one packet late, which is invisible while every
 *       source has the same packet size and is a straight sync error the
 *       moment one does not (a 1024-frame endpoint mixed with a 480-frame
 *       process tap lands 11 ms out and stays there).
 *
 *     a_first_packet_of_a_different_size_does_not_bias_every_later_gap -- the
 *       cancellation held only while the first packet matched the rest. An
 *       engine that hands over a burst at Start and single periods afterwards
 *       left a PERMANENT negative bias, suppressing that much of every
 *       subsequent fill for the rest of the session.
 */
#include "test_runner.h"

#include "capture/wasapi_common.h"
#include "clock.h"

#include <audioclient.h>

#define QPF   3579545ull      /* not 10 MHz, on purpose */
#define RATE  48000u
#define PKT   480u            /* 10 ms, what the engine hands over in practice */
#define T0    900000000ull    /* arrival of the first packet */

static uint64_t ticks_for(uint64_t frames)
{
    return apr_frames_to_ticks(frames, QPF, RATE);
}

static void clock_for(AprClock *c) { apr_clock_init(c, QPF, RATE); }

/* Frame counts crossing a tick conversion are exact to within one frame, and
 * a floor is the only reason they are not exact. Anything looser than this
 * would stop the file saying anything. */
#define ASSERT_FRAMES(expected, actual)                                       \
    do {                                                                      \
        uint64_t tr_e_ = (uint64_t)(expected), tr_a_ = (uint64_t)(actual);    \
        uint64_t tr_d_ = tr_e_ > tr_a_ ? tr_e_ - tr_a_ : tr_a_ - tr_e_;       \
        ASSERT_LE_INT(1, (long long)tr_d_);                                   \
    } while (0)

/* ==========================================================================
 * Where frame 0 actually happened
 * ======================================================================= */

TEST(packet_start_is_one_packet_before_arrival)
{
    uint64_t start = apr_wasapi_packet_start(T0, PKT, QPF, RATE);
    ASSERT_EQ_U64(ticks_for(PKT), T0 - start);
}

TEST(packet_start_saturates_instead_of_wrapping)
{
    /* A synthetic timeline that starts near zero must not produce an anchor in
     * the far future. */
    ASSERT_EQ_U64(0u, apr_wasapi_packet_start(10, PKT, QPF, RATE));
}

TEST(anchor_is_the_first_frames_capture_time_not_its_arrival)
{
    AprClock c;

    clock_for(&c);
    ASSERT_EQ_U64(0u, apr_wasapi_packet_gap(&c, 0, T0, PKT, 0));

    ASSERT_TRUE(apr_clock_anchored(&c));
    /* RED before the fix: the anchor was T0 itself, so this difference was 0
     * and every source sat one packet -- 10 ms -- late on the bus timeline. */
    ASSERT_EQ_U64(ticks_for(PKT), T0 - c.anchor_ticks);
}

TEST(a_late_anchor_walks_back_over_frames_already_delivered)
{
    /* A first buffer flagged TIMESTAMP_ERROR must not set the anchor (design
     * 5.2 step 4), so by the time one may, frames are already in the ring.
     * Frame 0 is still frame 0: the anchor has to walk back over them. */
    AprClock c;
    uint64_t arrival2 = T0 + ticks_for(PKT);

    clock_for(&c);
    ASSERT_EQ_U64(0u, apr_wasapi_packet_gap(&c, 0, T0, PKT,
                                            AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR));
    ASSERT_FALSE(apr_clock_anchored(&c));

    ASSERT_EQ_U64(0u, apr_wasapi_packet_gap(&c, PKT, arrival2, PKT, 0));
    ASSERT_TRUE(apr_clock_anchored(&c));
    ASSERT_EQ_U64(2 * ticks_for(PKT), arrival2 - c.anchor_ticks);
}

/* ==========================================================================
 * The gap. A steady stream must never fill; a real dropout must fill exactly.
 * ======================================================================= */

/* Walk a stream of packets through the same function drain() calls. Every
 * packet carries DATA_DISCONTINUITY, so any nonzero answer is the arithmetic
 * inventing a gap rather than the model containing one. Returns total silence
 * asked for. */
static uint64_t run_stream(AprClock *c, uint32_t first, uint32_t rest,
                           int packets)
{
    uint64_t t = T0, delivered = 0, fill = 0;
    int      i;

    for (i = 0; i < packets; i++) {
        uint32_t n = (i == 0) ? first : rest;
        uint64_t g;

        if (i) t += ticks_for(n);     /* the ARRIVING packet's own span */
        g          = apr_wasapi_packet_gap(c, delivered, t, n,
                                           AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY);
        fill      += g;
        delivered += g + n;
    }
    return fill;
}

TEST(steady_state_with_uniform_packets_never_fills)
{
    /* Two hundred packets, every one of them flagged, and nothing to fill --
     * because nothing is missing.
     *
     * THIS TEST IS GREEN BEFORE THE FIX AS WELL, and that is the point of it:
     * it is the disproof of BUGS.md M6's stated mechanism. If the old code had
     * really run "480 frames short at steady state" this would have shown a
     * fill on every packet. It did not, because the gap was measured against
     * the frames delivered BEFORE the arriving packet, and that cancelled the
     * arrival-time anchor exactly. */
    AprClock c;
    clock_for(&c);
    ASSERT_EQ_U64(0u, run_stream(&c, PKT, PKT, 200));
}

/* Anchor on a first packet of `first` frames, run `steady` ordinary periods,
 * then hand over a packet that arrives `slip` frames late: the engine dropped
 * `slip` frames, a long way into the session. Returns what the code asked to
 * fill. */
static uint64_t burst_then_dropout(AprClock *c, uint32_t first, int steady,
                                   uint64_t slip)
{
    uint64_t t = T0, delivered = 0;
    int      i;

    clock_for(c);
    apr_wasapi_packet_gap(c, delivered, t, first, 0);   /* anchor */
    delivered += first;

    for (i = 0; i < steady; i++) {
        t += ticks_for(PKT);
        delivered += apr_wasapi_packet_gap(c, delivered, t, PKT, 0) + PKT;
    }

    t += ticks_for(PKT) + ticks_for(slip);
    return apr_wasapi_packet_gap(c, delivered, t, PKT,
                                 AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY);
}

TEST(a_first_packet_of_a_different_size_does_not_bias_every_later_gap)
{
    /* THE REAL BUG, and the one the sweep was reaching for. The cancellation
     * above held only while the first packet matched the rest. Anchored at
     * arrival, the steady-state delta was (first_packet - current_packet)
     * rather than 0 -- so an engine that hands over a 1440-frame burst at Start
     * and 480-frame periods afterwards ran a PERMANENT -960 bias, and a real
     * dropout fifty packets later was under-filled by exactly that much.
     *
     * RED before the fix: this returns 0, and 960 frames of real timeline
     * shear vanish with no gap in the file to show for them. Anchored at frame
     * 0's capture time the two counts sit on one timeline and the answer is
     * the true loss whatever the packet sizes were. */
    AprClock c;
    ASSERT_FRAMES(2 * PKT, burst_then_dropout(&c, 3 * PKT, 50, 2 * PKT));
}

TEST(a_dropout_late_in_a_uniform_session_is_still_filled_exactly)
{
    /* The control for the case above: same shape, ordinary first packet. */
    AprClock c;
    ASSERT_FRAMES(2 * PKT, burst_then_dropout(&c, PKT, 50, 2 * PKT));
}

/* Anchor on one packet at T0, then hand over a packet that arrives `slip`
 * frames later than the next period would have: the engine dropped `slip`
 * frames. Returns what the code asked to fill. */
static uint64_t dropout(AprClock *c, uint64_t slip, DWORD flags)
{
    uint64_t arrival;

    clock_for(c);
    apr_wasapi_packet_gap(c, 0, T0, PKT, 0);            /* anchor, no gap */
    arrival = T0 + ticks_for(PKT) + ticks_for(slip);
    return apr_wasapi_packet_gap(c, PKT, arrival, PKT, flags);
}

TEST(a_real_dropout_fills_exactly_what_went_missing)
{
    AprClock c;
    ASSERT_FRAMES(2 * PKT,
                  dropout(&c, 2 * PKT, AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY));
}

TEST(a_dropout_shorter_than_one_packet_is_still_filled)
{
    /* The sweep believed sub-packet dropouts could never fill. They could,
     * and they still must: the mixer's claim is sample accuracy, not packet
     * accuracy. */
    AprClock c;
    uint64_t fill = dropout(&c, 37, AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY);
    ASSERT_GT_INT(0, (long long)fill);
    ASSERT_FRAMES(37u, fill);
}

TEST(no_discontinuity_means_no_frame_is_ever_invented)
{
    /* Nothing is synthesized speculatively. The source is fifty packets behind
     * the clock and still not one sample goes in, because the engine has not
     * said it dropped anything. */
    AprClock c;
    ASSERT_EQ_U64(0u, dropout(&c, 50 * PKT, 0));
}

TEST(a_discontinuity_with_no_usable_timestamp_does_not_fill)
{
    AprClock c;
    ASSERT_EQ_U64(0u, dropout(&c, 4 * PKT,
                              AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY |
                                  AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR));
}

/* ==========================================================================
 * M3: a process tap fills an engine-reported gap too
 * ======================================================================= */

TEST(the_gap_decision_does_not_depend_on_the_source_kind)
{
    /* Design 5.1 says a process tap arrives perfect and IS the reference
     * timeline, and nothing here contradicts that:
     * no_discontinuity_means_no_frame_is_ever_invented shows that with no flag
     * set, neither kind has a single sample invented for it.
     *
     * But "arrives perfect" is a measurement of an engine keeping up, not a
     * promise about one that has just set DATA_DISCONTINUITY -- and until M3
     * the pump made exactly that stall possible by running an unbounded COM
     * enumeration between packets. On the REFERENCE timeline a silent shear is
     * the worst failure in the system: every bus reading this tap moves
     * against every bus that does not, permanently, with nothing in the file
     * to show it. Design 3.1's policy for a loss is alignment over content, so
     * the frames the engine admits it dropped go in as silence.
     *
     * The decision is kind-independent BY CONSTRUCTION: apr_wasapi_packet_gap
     * takes no device_mode argument at all, which is what stops the two kinds
     * drifting apart again. This test asserts that shape, not a behaviour, and
     * would not compile if the argument came back. */
    AprClock c;
    ASSERT_FRAMES(2 * PKT,
                  dropout(&c, 2 * PKT, AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY));
}

/* ==========================================================================
 * Degenerate input
 * ======================================================================= */

TEST(an_empty_packet_neither_anchors_nor_fills)
{
    AprClock c;
    clock_for(&c);
    ASSERT_EQ_U64(0u, apr_wasapi_packet_gap(&c, 0, T0, 0, 0));
    ASSERT_FALSE(apr_clock_anchored(&c));
}

TEST(a_null_clock_is_refused_rather_than_faulting)
{
    ASSERT_EQ_U64(0u, apr_wasapi_packet_gap(NULL, 0, T0, PKT, 0));
}
