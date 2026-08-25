# apprecorder — Design

**Status:** approved 2026-08-25
**Target:** Windows 10 2004 (build 19041) and later, x64

## 1. What it is

An Audio Hijack-style recorder for Windows. The user selects sources — running
applications and hardware capture devices — routes them into an arbitrary number
of software-defined buses, and attaches actions to each bus. Recording is the
first action; the action interface is designed so transcription and others drop
in later without touching the engine.

It exists because hardware mixers expose a fixed number of buses (the author's
GoXLR gives three: Stream Mix 1, Stream Mix 2, Chat Mic) and software has no
such limit.

### Goals

- Capture any running app's audio **per-process**, plus any hardware capture endpoint.
- Arbitrary bus count. A source may feed several buses at once.
- Encode to WAV, MP3, OGG/Opus, M4A.
- Small and cheap: target under 1 MB binary, single-digit MB resident, negligible idle CPU.
- **Accessible and good-looking at the same time**, neither one a fallback.

### Non-goals

- **Virtual audio devices / live routing to other apps.** That needs a signed
  driver — a different project. The data model must not preclude it later.
- **Live monitoring.** Hardware mixers do this deterministically; Windows is not
  an RTOS and cannot bound worst-case latency. Not competing here.
- Audio effects/DSP beyond gain. Not a DAW.

### The design constraint that follows from the non-goals

Because nobody listens in real time, **latency is irrelevant**. What matters
instead is **sync**: sources arrive from different clock domains and drift apart
over long sessions. Sync is the hard problem in this codebase and everything
below is shaped by it.

---

## 2. Language and toolchain

C11, Win32, no runtime dependency. MSVC 14.42 (`cl`), CMake + Ninja (both bundled
with VS 2022 Build Tools), Windows SDK 10.0.22621.

Rationale: every encoder needed is already a C library (libmp3lame, libogg,
libopus); standard Win32 controls are the most reliably screen-readable widgets
on Windows; static linking yields a few hundred KB rather than the MBs a managed
or Rust runtime would add.

Accepted costs, so they are not surprises:

1. `ActivateAudioInterfaceAsync` requires implementing
   `IActivateAudioInterfaceCompletionHandler`, a COM *callback*. In C this means
   hand-authoring a vtable struct plus the three `IUnknown` methods. Roughly 80
   lines, written once, in `capture/com_shim.c`. The rest of WASAPI is fine in C
   via the SDK's `IAudioClient_*(This, ...)` macros (define `COBJMACROS`).
2. Transcription (later) needs HTTPS + JSON: WinHTTP plus a small JSON parser,
   with hand-rolled multipart. Several times the cost of the encoders.

---

## 3. Architecture

Three layers. One vtable each. The whole system is a directed graph: sources fan
out to buses, buses fan out to actions, and a single source may feed more than
one bus.

**This graph is the model, and the canvas UI is a renderer over it.** The
accessibility tree is another projection of the same structure. Neither view is
derived from the other; both read the same `Graph`.

### 3.1 Source

A source produces PCM. It is either a process tap or a hardware endpoint; both
present identically to everything downstream.

```c
typedef enum { SRC_PROCESS, SRC_DEVICE } SourceKind;

typedef struct Source {
    SourceId     id;
    SourceKind   kind;
    union {
        struct { DWORD pid; ProcessLoopbackMode mode; } process;
        struct { WCHAR *endpoint_id; }                 device;
    };
    WaveFormat   fmt;          /* negotiated at open */
    RingBuffer  *rb;           /* producer: capture thread; consumer: each bus */
    ClockAnchor  anchor;       /* QPC of first frame; see section 5 */
    int          refcount;     /* number of buses consuming this source */
} Source;
```

`refcount` is what makes the graph (rather than a tree) work: several buses may
read the same source. The ring buffer is single-producer / multi-consumer, each
consumer holding its own read cursor.

**`refcount` is bus bookkeeping, not buffer lifetime.** Because consumers hold
their own cursors, the ring has no idea how many readers exist and must not be
taught. Do not wire `refcount` into `ringbuf`.

**Ring sizing.** Rings absorb *mixer scheduling jitter only* — **250 ms**, which
is 96 KB per source at 48 kHz stereo float32. They are deliberately not sized for
disk stalls: a slow encoder must not back pressure into a buffer shared by every
other bus. Each action owns its own write-behind buffering, so I/O hiccups are
absorbed where they happen. Sizing rings for disk instead would cost ~384 KB per
source per second of tolerance and blow the single-digit-MB budget for no gain.

**Overrun policy** (implemented, do not revisit without cause): the producer is
an audio callback and can never block, so the oldest frames are overwritten and
loss is confined to the reader that fell behind.

**Silence fill happens on two different sides — an earlier draft of this section
conflated them.** They are not the same mechanism:

- **Producer side.** `rb_write_silence()` is a *producer* call. It belongs to
  `capture_device` filling a real hardware gap on
  `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY`. A reader can never call it.
- **Consumer side.** `rb_read`'s skipped-frame count tells a *reader* it was
  lapped. The mixer fills that by emitting zeros into **its own output**, then
  resuming at the true absolute frame position.

**The overrun contract, stated as policy rather than left to each reader:** a
stall longer than the ring loses that audio permanently. When that happens we
choose **alignment over content** — emit exactly as many zeros as were lost and
resume at the correct absolute frame. A hole is recoverable; a permanent
timeline shift silently ruins every track on the bus. This bound is pinned by a
test.

### 3.2 Bus

Owns N sources, mixes to float32 at the session rate, fans out to M actions.

**An edge is a struct, not an index.** An earlier draft stored `SourceId
sources[]` with a parallel `gain[]`, which is a *tree* — it has nowhere to put
per-edge state. Two buses reading one source each need their **own** ring
cursor, their own resampler, and their own drift controller, because they
consume at independent positions. Per-edge state is what makes this a graph:

```c
typedef struct BusEdge {
    SourceId     source;
    float        gain;          /* linear */
    RingReader   reader;        /* this bus's own cursor into the source */
    AprResampler *rs;           /* NULL for reference sources — see 5.2 */
    AprDrift     drift;         /* this edge's own controller */
} BusEdge;

typedef struct Bus {
    BusId    id;
    wchar_t  name[64];
    BusEdge  edges[MAX_SOURCES_PER_BUS];
    size_t   edge_count;
    Action  *actions[MAX_ACTIONS_PER_BUS];
    size_t   action_count;
} Bus;
```

**Which sources are "reference" cannot be derived from `AprSourceKind` alone.**
Design 4.3 requires the whole core to be testable with no hardware, and a fake
source standing in for a process tap is not `APR_SRC_PROCESS`. Reference status
is therefore an explicit property with an override, not a `switch` on kind.

"Record Teams and my mic" is one bus, two sources, one action. Recording them
separately is two buses. Same code path.

### 3.3 Action

```c
typedef struct ActionVTable {
    const char *id;                        /* "wav", "mp3", "ogg", "m4a" */
    const char *display_name;
    void* (*create)(const ActionConfig *cfg, const WaveFormat *fmt);
    int   (*on_audio)(void *st, const float *pcm, size_t frames, uint64_t qpc);
    int   (*finalize)(void *st);
    void  (*destroy)(void *st);
} ActionVTable;
```

Registration is a **static array** in `core/registry.c`. No plugin system, no
dynamic loading, no ABI to version — this is what keeps the binary small and the
build simple. Adding Opus is adding one file and one array entry.

Transcription later is just an `on_audio` that accumulates instead of encoding,
and does its network work in `finalize`.

---

## 4. Capture layer

### 4.1 Process loopback

```c
AUDIOCLIENT_ACTIVATION_PARAMS p = {0};
p.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
p.ProcessLoopbackParams.TargetProcessId     = pid;
p.ProcessLoopbackParams.ProcessLoopbackMode =
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

PROPVARIANT pv = {0};
pv.vt = VT_BLOB;
pv.blob.cbSize    = sizeof(p);
pv.blob.pBlobData = (BYTE*)&p;

ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                            &IID_IAudioClient, &pv, handler, &op);
```

**Verified behaviour** (spike, 2026-08-25 — measured, not assumed):

1. **`GetMixFormat` returns `E_NOTIMPL`** on a process-loopback client. There is
   no device to ask. We *supply* the format to `Initialize` — session rate, 2ch,
   float32 `WAVE_FORMAT_EXTENSIBLE` — rather than negotiating. This is why
   `WaveFormat` is a session-level decision, not a per-source one.
   **`GetDevicePeriod` returns `E_NOTIMPL` too.** Pass `hnsBufferDuration = 0`
   and accept what you get (480 frames / 10 ms in practice).
2. **The stream is continuous and gapless regardless of what the target does.**
   Measured over 120 s against a process that never called `Start()`: 11,999
   packets, *every one exactly 480 frames, 100% fill, zero gaps, zero timeouts*.
   Quiet arrives as ordinary buffers full of real `0.0f` samples.
   `AUDCLNT_BUFFERFLAGS_SILENT` was **never set** in ~190 s of capture, and
   neither was `DATA_DISCONTINUITY` or `TIMESTAMP_ERROR`.
   **There is nothing to synthesize for a process tap.** An earlier draft of this
   document asserted the opposite; see section 5.
3. **`pu64DevicePosition` is always 0.** Unusable. Do not read it.
4. **`pu64QPCPosition` is the frame counter rescaled**, advancing by exactly
   `frames × 1e7 / 48000` (11,998 of 11,998 deltas exact, +0.00 ppm). It carries
   **no independent clock information** and cannot reveal an app's render drift.
5. **Loopback is post-session-volume.** Captured amplitude scales linearly with
   the target's volume in the Windows mixer, with no other gain in the path
   (volume `1e-4` yielded captured peak `0.000025` = exactly `0.25 × 1e-4`).
   **A muted app records as pure silence** even though the engine confirms it is
   rendering. The UI must warn when a captured source's session volume is 0.
6. **Capture continues after the target process exits** — silence, forever, with
   no error. **WASAPI will never tell you a source died.** See section 10.
7. Requires `AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK`,
   MTA, and `hnsBufferDuration = 0`. The completion handler fires on a *different*
   thread; signal an event the opening thread waits on.
8. `QueryInterface` on the completion handler must succeed for `IID_IUnknown`,
   `IID_IActivateAudioInterfaceCompletionHandler`, **and `IID_IAgileObject`**.
   Two HRESULTs come back from activation — `GetActivateResult`'s own return
   *and* its out-parameter. Check both.
9. `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE` genuinely walks the tree:
   capturing a shell's PID picks up a tone rendered by its child. Required for
   browsers and Electron apps.

### 4.1.1 EXCLUDE mode — verified, and it has a UI trap

`PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE` captures everything *except*
the named process. Verified across four runs varying nothing but the flag:

| mode | target | result |
|---|---|---|
| INCLUDE | the player | peak 0.000025, 440 Hz, 0 gaps |
| EXCLUDE | the player | **0 of 335,040 samples non-zero** |
| EXCLUDE | a process rendering nothing | peak 0.000025 — the *sibling's* tone |
| EXCLUDE | an ancestor shell of the player | **silence** |

The flag works, and excluding the only thing playing yields silence — but still a
**full gapless stream** of it (48,000.0 fps, 0 discontinuities), consistent with
4.1 #2.

**The trap: EXCLUDE walks the process tree as well.** Excluding a launcher
excludes everything it ever spawned. "Record everything except Discord" will also
drop whatever Discord started, and excluding a terminal silently drops every app
launched from it — including ones the user never associated with that terminal.

**This is a UI-wording problem, not a bug.** The interface must never present
this as "everything except X". It has to say the tree is excluded, name what is
currently in that tree, and ideally show the user the affected processes before
they commit. Note for localization (6.2): the Arabic phrasing needs the same
precision, and "except" is exactly the kind of word that flattens a tree
relationship into a single item when translated carelessly.

**Privacy.** EXCLUDE records whatever the machine is playing. It must never be
the default, never be silently enabled by a session file without confirmation,
and the UI must make clear that everything else on the system is being captured.

Capture is **passive** — the app keeps rendering to its real endpoint untouched.
This is why it avoids the cost of a virtual-cable approach: no insertion into the
signal path, no extra engine round trip, no reroute. It is also per-**PID**, not
per-endpoint, so an app is captured regardless of which hardware channel it is
assigned to; no mixer reconfiguration is ever required.

### 4.2 Device capture

Standard `IMMDeviceEnumerator` then `EnumAudioEndpoints(eCapture)` then
`IAudioClient::Initialize` with `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`.

Note for the author's own rig: the microphone should be taken from the mixer's
**Chat Mic** capture endpoint, so it carries the hardware's gate/comp/EQ. A
process-loopback tap captures app audio *pre*-hardware-processing, which is
correct for apps and wrong for a voice.

### 4.3 Testability boundary

`capture_*.c` implement one internal interface. A **fake source** driven by a
synthetic clock implements the same interface, so the entire core — mixing, drift
correction, actions — is testable **without audio hardware**. This is a hard
architectural requirement, not a convenience: CI and agent-driven development
cannot rely on a GoXLR being plugged in.

---

## 5. The clock and drift correction

The core problem. Sources come from independent clock domains — a hardware
crystal for device captures, the render clock of each app for loopback taps — and
they run at genuinely different rates. Over a three-hour session, naive
sample-appending leaves tracks visibly out of sync.

**Decision: sample-accurate alignment.** Alignment cannot be retrofitted into
files already written; over-building here is recoverable, under-building is not.

### 5.1 Correction — the two source kinds are not symmetric

An earlier draft treated all sources alike and planned to detect gaps and drift
from `pu64QPCPosition`. **The spike disproved that.** For process taps that field
is the frame counter rescaled: it reports +0.00 ppm by construction and can never
fire a gap or drift check. Code built on it would be dead code.

The real picture:

| | **Process tap** | **Device capture** |
|---|---|---|
| Timeline | audio engine — gapless, 100% fill, always | hardware crystal |
| Silence | real `0.0f` samples, never a gap | genuine dropouts possible |
| Drift vs QPC | none meaningful (measured −6.51 ppm over 120 s, inside the ~8 ppm noise floor) | **real, and the whole problem** |
| Death signal | none — keeps emitting silence forever | endpoint removal is reported |

**So process taps are the reference timeline, and device captures are what must
be corrected onto it.** This is simpler than the original design, and it puts the
machinery where the drift actually is.

### 5.2 Mechanism

**The mixer deliberately lags wall clock by 50 ms.** This was missing from an
earlier draft and is arguably the most load-bearing decision in the whole sync
design. Nobody listens in real time (see 1.3), so latency is free — and buying
it pays for three things at once:

- Data is always already in the ring when a block renders.
- **A device's jitter buffer costs no alignment.** Holding its backlog at exactly
  the lookbehind means the instant its buffer fills is the instant the bus
  reaches its true start. Without this, a mic sits a *fixed* 50 ms behind the
  process taps it is mixed with — and that is a sync error, not a latency one,
  which defeats the mixed-bus rule below.
- The tick rate stops mattering: the mixer renders what QPC says is due rather
  than counting ticks.

**The control error signal is backlog, not rate.** `produced − consumed`, exact:
an integer producer cursor against a Q32.32 consumer position. Holding that
constant *is* sample-accurate alignment. Rate-based control leaves an
uncorrected position offset and cannot reach sub-sample accuracy — naming "a PI
controller" without naming its setpoint, as an earlier draft did, is not enough
to build from.

1. **Master timeline is `QueryPerformanceCounter` sampled at capture time** — not
   `pu64QPCPosition`, which is derived and therefore useless for this.
   **Always scale by `QueryPerformanceFrequency`.** QPF happened to be exactly
   10,000,000 Hz on the spike machine, which makes raw ticks and 100 ns units
   coincide and silently hides an entire class of unit bug. Never assume it.
2. Each source records a `ClockAnchor` at its first frame: QPC at arrival, plus
   its own frame counter.
3. **Process taps** are consumed directly. No gap synthesis, no silence fill, no
   discontinuity handling. They arrive perfect; treat them as perfect.
4. **Device captures** get the full treatment, because they are the only sources
   that need it:
   - Compare frames delivered against QPC elapsed since the anchor.
   - **Steady divergence is crystal drift.** A slow PI controller nudges a
     fractional resample ratio, applied gradually so it is never audible.
   - `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` is a real gap here — fill with
     silence and log it.
   - `AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR` falls back to frame counting for that
     buffer and must not poison the anchor.
5. Resampling is windowed-sinc, in `core/resample.c`, shared by every path. One
   implementation — no per-source variants.

**Consequence for mixed buses.** A bus combining a process tap and a device
capture (the common case: "Teams plus my mic") resamples the *device* side onto
the engine timeline. The process side passes through untouched.

**Test strategy:** the fake source can be given a deliberately wrong clock rate
(for example +30 ppm) to stand in for a device capture. A simulated multi-hour
session must end with alignment error below one sample. Runs in milliseconds,
needs no hardware.

---

## 6. UI

### 6.1 Approach: nodes are real windows

Each graph node is an actual **Win32 child HWND** on a scrollable parent canvas.
Because they are real windows, MSAA/UIA exposure, focus, and tab order come
essentially free — no hand-written accessibility provider. Full visual control is
retained by painting in `WM_PAINT`. Edges are drawn on the parent and announced
through each node's UIA name and description ("Teams, source, feeding Main Mix").

Rejected alternatives, recorded so they are not relitigated:

- **Direct2D canvas plus hand-written UIA provider** (`IRawElementProviderSimple`,
  `IRawElementProviderFragment`, `IRawElementProviderFragmentRoot`, one fragment
  per node and edge). Maximum visual freedom and edge-level navigation, but three
  more COM interfaces hand-vtabled in C and permanent ownership of every
  accessibility bug. Remains possible later — the model is already correct for it.
- **Canvas plus a separate tree, canvas itself inaccessible.** Separate-but-equal;
  the canvas would never get fixed.

A **docked TreeView over the same model** ships alongside the canvas — not a
substitute but a second real view: fast keyboard jumping, and a layers-panel
overview for sighted users on a large graph.

### 6.2 Localization — Arabic ships after v1, but is designed in now

The app will be localized to Arabic before release. Retrofitting that into a
Win32 codebase is expensive; designing for it before the UI exists is nearly
free. **No user-facing string is ever a literal in code**, starting from the
first line of UI.

**Catalog.** Win32 `STRINGTABLE` resources in `.rc`, one `LANGUAGE` block per
locale, all embedded in the single exe — `LoadStringW` selects by thread locale.
No satellite DLLs, no extra files, and the cost is only the string bytes, which
keeps the size goal intact. Every string gets a named ID in one enum; a build
check fails if an ID exists without a string in every declared language.

**Plurals are the trap.** Arabic has **six** plural forms (zero, one, two, few,
many, other) against English's two. A `printf("%d sources")` shaped API is
unfixable later, so the string API is plural-aware from day one:
`apr_str_plural(id, n)`. English simply uses two of the six slots.

**Never concatenate sentences.** Arabic word order differs, so fragments joined
in code cannot be reordered by a translator. Use positional format specifiers
(`%1$s`, `%2$d`) exclusively — the translator must be able to move the arguments.

**RTL layout.** `WS_EX_LAYOUTRTL` mirrors standard child-control positioning for
free. **It does not mirror anything we paint ourselves** — and our canvas nodes
are custom-painted HWNDs (6.1), so the canvas owns its mirroring explicitly.

**Signal-flow direction is a layout parameter, not a constant.** The graph reads
left-to-right in English and must read **right-to-left in Arabic**: sources on
the right, actions on the left. Hardcoding flow direction anywhere is a review
failure.

**Logical order stays language-independent.** The accessibility tree and the
TreeView follow source → bus → action regardless of visual direction. Screen
reader navigation order must not flip with the layout; only the painting does.

**Digits.** Default to Western (Hindu-Arabic) numerals, which is standard Saudi
UI practice. Confirm with the author before shipping — he is an accessibility
expert at DGA and this is his domain.

**Fonts.** Segoe UI Variable covers Arabic. Verify rendering at the target sizes
rather than assuming; Arabic needs more vertical room than Latin at the same
point size, so **never** size a control to fit its English string.

**Translation process** (when the strings are actually written, not now): draft
through the `ux-araby` skill for فصحى مبسطة, then a Gemini review pass. Keep the
author's domain-term overrides — notably **«إمكانية الوصول»** for accessibility,
never «الإتاحة».

**CLI.** Same catalog, but English by default. Arabic console output needs
`SetConsoleOutputCP(CP_UTF8)` and still renders poorly in some terminals; the CLI
is an automation surface, so this is low priority.

### 6.3 The beauty/accessibility line

**`NM_CUSTOMDRAW`, never `LVS_OWNERDRAWFIXED`.** Custom-draw alters painting only
and preserves the accessibility tree; full owner-draw replaces the control's
semantics and leaves a screen reader nothing to read. This is a hard rule.

Supporting: comctl32 v6 manifest, Segoe UI Variable, per-monitor DPI v2, generous
spacing. Dark mode is undocumented uxtheme (`SetPreferredAppMode`) and breaks
between Windows builds — implement it, isolate it entirely in `ui/darkmode.c`,
and make its failure non-fatal.

Every operation must be reachable by keyboard. No mouse-only paths.

---

## 7. Module layout

Shared primitives exist exactly once. This is the DRY contract; anything
duplicating them is a review failure.

```
src/
  core/      graph.c  bus.c  source.c  registry.c
             ringbuf.c  clock.c  resample.c  mix.c
  capture/   com_shim.c  wasapi_common.c
             capture_process.c  capture_device.c  capture_fake.c
  actions/   action_wav.c  action_mp3.c  action_ogg.c  action_m4a.c
  ui/        app.c  canvas.c  node_window.c  tree_panel.c
             theme.c  darkmode.c  dpi.c
  session/   session_load.c  session_save.c
  platform/  err.c  log.c
tests/       test_runner.h  test_*.c
```

`platform/str.c` and `fs.c` appeared in an earlier draft and were never written,
because nothing needed them. Add them when a second caller exists, not before.

Single owners of cross-cutting concerns:

| Concern | Sole owner |
|---|---|
| Ring buffers | `core/ringbuf.c` |
| Sample-rate conversion | `core/resample.c` |
| QPC / drift | `core/clock.c` |
| PCM format conversion | `core/mix.c` |
| COM vtable boilerplate | `capture/com_shim.c` |
| HRESULT to message | `platform/err.c` |

---

## 8. Encoders

| Format | Implementation | Notes |
|---|---|---|
| WAV | hand-written | trivial; also the golden-file test target |
| MP3 | libmp3lame | LGPL, C |
| OGG | libogg + libopus | BSD, C |
| M4A | **Media Foundation AAC encoder** | ships in Windows: zero binary cost, no fdk-aac licensing question |

Each is one `ActionVTable` in one file. They share no state and touch no core
internals, which makes them the natural unit of parallel work.

---

## 9. Session persistence

JSON (hand-rolled writer; jsmn for reading). A session records the graph: sources
with stable identity, buses, gains, actions and their config. Processes are
matched on reopen by executable path and window class, falling back to prompting
— PIDs are not stable across reboots.

---

## 10. Error handling

Failure of one source must never take down a session. A source that fails to
open, or dies mid-recording, transitions to `SRC_FAILED`, surfaces in the UI, and
recording continues. Files are always finalized to a playable state — every
action's `finalize` is called on any exit path.

**Process death needs its own detector.** The spike confirmed that process
loopback keeps delivering silence indefinitely after the target exits, with no
error and no flag. WASAPI will never tell us the app is gone. Each process source
therefore holds a handle from `OpenProcess(SYNCHRONIZE, ...)` and waits on it; a
signalled handle is the *only* reliable death signal. Without this, closing Teams
mid-session yields hours of silence that looks like a successful recording.

**Muted sources look identical to dead ones.** Because loopback is
post-session-volume, an app muted in the Windows mixer records as pure silence.
Poll the source's `ISimpleAudioVolume` and warn in the UI — this will otherwise
be the single most common "why is my recording empty" support question.

---

## 11. Testing

A single-header assert harness in `tests/test_runner.h` — no external framework,
consistent with the size goal.

- **Unit:** ringbuf under concurrent access; resampler impulse and frequency
  response; clock gap and drift arithmetic; each encoder against golden files.
- **Sync (the important one):** fake sources with deliberately mismatched clock
  rates over a simulated multi-hour session; assert sub-sample alignment.
- **Integration:** full graph, fake sources, all encoders, verify output.
- Hardware-dependent paths are exercised manually against a real rig; nothing in
  CI depends on hardware being present.

---

## 12. Build order

Sequenced where things are load-bearing, parallel where they are leaves.

1. **Foundation** (sequential) — `platform/`, `ringbuf`, `clock`, test harness.
2. **Capture spike** (sequential) — `com_shim` plus `capture_process`. Riskiest
   unknown: the completion-handler vtable in C. Prove it captures real PCM from a
   real app before anything is built on top.
3. **Core** (sequential) — `graph`, `source`, `bus`, `mix`, `resample`, drift.
   Shape must be settled before any leaf work begins.
4. **Encoders** (parallel) — WAV, MP3, OGG, M4A: four agents, one interface, no
   shared state.
5. **UI** (partly parallel) — `app`, `canvas`, `node_window` sequential;
   `theme`, `dpi`, `darkmode`, `tree_panel` parallel after the canvas exists.
6. **Session and polish.**

Review and test passes run against every wave.
