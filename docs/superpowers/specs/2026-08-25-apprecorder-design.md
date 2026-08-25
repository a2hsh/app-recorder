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

### 3.2 Bus

Owns N sources, mixes to float32 at the session rate, fans out to M actions.

```c
typedef struct Bus {
    BusId    id;
    wchar_t  name[64];
    SourceId sources[MAX_SOURCES_PER_BUS];
    float    gain[MAX_SOURCES_PER_BUS];   /* per-source, linear */
    size_t   source_count;
    Action  *actions[MAX_ACTIONS_PER_BUS];
    size_t   action_count;
} Bus;
```

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

**Three gotchas that must be handled, not discovered:**

1. **`GetMixFormat` returns `E_NOTIMPL` on a process-loopback client.** There is
   no device to ask. We *supply* the format to `Initialize` — session rate, 2ch,
   float32 — rather than negotiating it. This is why `WaveFormat` is a
   session-level decision, not a per-source one.
2. **A silent app produces no buffers at all.** If we simply append what arrives,
   every track desyncs the moment an app goes quiet. Silence must be
   *synthesized from timestamp gaps* (section 5). This is the single most likely
   source of sync bugs in the project.
3. Requires `AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK`,
   MTA, and `hnsBufferDuration = 0`. The completion handler signals an event the
   opening thread waits on.

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

**Mechanism:**

1. `IAudioCaptureClient::GetBuffer` yields `pu64QPCPosition` (100 ns units). QPC
   is the single master timeline for the whole session.
2. Each source records a `ClockAnchor` at its first frame. Every buffer
   thereafter carries an absolute QPC timestamp.
3. The mixer pulls at a fixed cadence. For each source it compares *expected*
   frame count (derived from elapsed QPC) against *actual* frames available:
   - **Gap** (silent app, or dropout) then synthesize silence to fill it.
   - **Steady divergence** is clock drift. A slow PI controller nudges a
     fractional resample ratio; correction is applied gradually so it is never
     audible.
   - `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` is treated as a gap and logged.
   - `AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR` falls back to frame counting for that
     buffer and must not poison the anchor.
4. Resampling is windowed-sinc, in `core/resample.c`, shared by every path. One
   implementation, used everywhere — no per-source variants.

**Test strategy:** the fake source can be given a deliberately wrong clock rate
(for example +30 ppm). A simulated multi-hour session must end with alignment
error below one sample. This runs in milliseconds and needs no hardware.

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

### 6.2 The beauty/accessibility line

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
  platform/  err.c  log.c  str.c  fs.c
tests/       test_runner.h  test_*.c
```

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
open, or dies mid-recording (app closed), transitions to a `SRC_FAILED` state,
synthesizes silence to keep alignment intact, surfaces in the UI, and recording
continues. Files are always finalized to a playable state — every action's
`finalize` is called on any exit path.

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
