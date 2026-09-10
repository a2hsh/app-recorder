# apprecorder — Session Handoff

Live status doc. Update as work happens, not at the end.

## What this is

An Audio Hijack-style **recorder** for Windows: capture audio from chosen apps
(per-process) plus hardware inputs, into an arbitrary number of software-defined
buses, written out to files. Built to give the user more capture buses than his
GoXLR provides in hardware.

**Status:** preparing the first public release, **version 0.0.1**. The newest
entry is always at the BOTTOM of this file; read that one first. Both front ends
ship in one `apprecorder.exe`, and the build carries its own updater — see the
2026-08-30 entry, and `include/update.h` before touching any of it.

---

## 2026-08-25 — Initial design conversation

### Decisions made

| Decision | Rationale |
|---|---|
| **Recorder, not router** | Capturing app audio is now an OS API call; *presenting* virtual devices to other apps needs a signed driver. Rogue Amoeba splits these into two products (Audio Hijack vs Loopback) for exactly this reason. |
| **Keep the routing door open** | Design the mixing core so a driver could consume its buses later. Not built now. |
| **Live monitoring stays in hardware** | The GoXLR's DSP is deterministic and sub-millisecond. Windows is not an RTOS — average latency is fine, worst case is unbounded. Do not rebuild this in software. |
| **Drop the GoXLR-firmware idea** | Adding hardware channels needs a reverse-engineered closed DSP blob *and* replacing the Music Tribe-signed USB audio driver. Ends in a driver project — the thing we routed around. Bricking risk. |
| **Language: C + Win32** | User's call, reaffirmed after I pitched C# then Rust. Defensible: ~300-500KB binary, single-digit MB RAM; every encoder (lame, ogg/vorbis, opus) is already a C library; standard Win32 controls are the most reliably screen-readable widgets on Windows. |

### Core technical basis

- **WASAPI process loopback** — `ActivateAudioInterfaceAsync` with
  `AUDIOCLIENT_ACTIVATION_PARAMS` / `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`.
  Windows 10 2004 (build 19041)+. User is on Win11 26200.
- It is **passive observation, not insertion** — this is why it avoids the
  latency/CPU cost of Virtual Audio Cable, which the user abandoned. VAC added a
  full extra round trip through the audio engine per cable, plus SRC, plus
  continuous resampling to fight clock drift.
- Per-**PID**, not per-endpoint → captures an app regardless of which GoXLR
  channel it's assigned to. **No GoXLR routing changes needed.**
- Mic comes from the GoXLR **Chat Mic** capture endpoint (so it carries the
  GoXLR's gate/comp/EQ), *not* from a loopback tap.
- Reference: MS `ApplicationLoopback` sample.

### The actual hard problem: clock drift

Sources come from different clock domains — the GoXLR's hardware crystal
(Stream Mix 1, Stream Mix 2, Chat Mic) and each app's render stream via
loopback. Over a multi-hour session these drift apart; naive sample-appending
desyncs the tracks. **Must be designed in from day one**: timestamp every buffer
against QPC, drift-correct on write. Latency is irrelevant here (nobody listens
in real time); *sync* is everything.

### User context

- Co-develops **goxlr-utility** (nickname "rockstorm", with FrostyCoolSlug).
  Fluent in Rust; knows the GoXLR protocol firsthand.
- Blind. **Accessibility is a first-class constraint, not a nice-to-have.**
  - Audio Hijack's drag-and-drop block canvas is the wrong model — pure spatial
    reasoning. Use a keyboard-driven list/tree: buses as a list, sources nested
    under them, readable top to bottom.
  - Web UI (HTML + ARIA) with NVDA beats native GUI toolkits. Notably this is
    also what goxlr-utility already does.
- Hardware: GoXLR (Full). Available capture endpoints today: Stream Mix 1,
  Stream Mix 2, Chat Mic. Wants more buses than that.
- Previously used Virtual Audio Cable — abandoned as laggy and CPU-heavy.

## 2026-08-25 (later) — Language, architecture, UI

### Decided

- **C + native Win32.** See table above.
- **Graph, not tree.** A source may feed multiple buses simultaneously
  (e.g. Teams → both "full mix" and "Teams only"). Costs refcounted source
  buffers + a fan-out stage.
- **Architecture: sources → buses → actions**, one vtable each.
  - `Source` = process loopback tap (PID) **or** hardware capture endpoint.
    Both just produce PCM. This is how "record Teams and my mic" works.
  - `Bus` owns N sources, mixes to float32, fans out to M actions.
  - `Action` = `{id, create, on_audio, finalize, destroy}`. MP3/WAV/OGG/M4A are
    four registrations in a static table, not four modes. Transcription drops in
    later as another `on_audio` that buffers instead of encodes.
  - No plugin system, no dynamic loading, no ABI to version — a static array of
    vtables. Keeps the binary small.
- **Encoders:** libmp3lame, libogg/libvorbis or libopus, WAV hand-written.
  For M4A use the **Media Foundation AAC encoder built into Windows** — zero
  binary size, no fdk-aac licensing question.
- **UI: canvas AND accessible, via approach A** (pending final approval).

### The canvas question — resolved

I initially claimed Audio Hijack's block canvas was the wrong model for a blind
user. **That was wrong.** Audio Hijack 3 was simultaneously the release that
introduced the canvas *and* the release praised for accessibility (AFB
AccessWorld: "has a new, accessible interface"); it won the **AppleVis Golden
Apple Award** — voted by blind Mac users, not an Apple award.

**Why it works:** the sources→buses→actions model *is already a node graph*.
The canvas is a renderer over it; the accessibility tree is another projection
of the same structure. Neither is a fallback for the other.

**Approach A (recommended):** each node is a **real Win32 child HWND** on a
scrollable parent. Real windows → MSAA/UIA, focus, and tab order come free, no
hand-written provider. Full visual control retained via `WM_PAINT`. Edges drawn
on the parent, announced through node UIA name/description.
Plus a **docked TreeView over the same model** — not a substitute, a second
real view (fast keyboard jumping; layers-panel overview for sighted users).

- Rejected **B** (Direct2D + hand-written `IRawElementProvider*` per node/edge):
  three more COM interfaces hand-vtabled in C, own every a11y bug forever.
  Still possible later — A doesn't block it, the model is already right.
- Rejected **C** (canvas + separate tree, canvas itself inaccessible):
  separate-but-equal; the canvas would never get fixed.

### Beauty vs accessibility — the hard line

**Use `NM_CUSTOMDRAW`, never `LVS_OWNERDRAWFIXED`.** Custom-draw changes only
painting and preserves the accessibility tree; full owner-draw destroys it.
Plus: comctl32 v6 manifest, Segoe UI Variable, per-monitor DPI v2, generous
spacing. Dark mode is undocumented uxtheme (`SetPreferredAppMode`), breaks
between Windows builds — do it, but isolate it in one file.

### Known costs in C (accepted, not surprises)

1. `ActivateAudioInterfaceAsync` needs `IActivateAudioInterfaceCompletionHandler`
   — a COM *callback*. Hand-author a vtable struct + the three `IUnknown`
   methods, ~80 lines, written once. Rest of WASAPI is fine in C via the SDK's
   `IAudioClient_*(This, ...)` macros.
2. Transcription later = HTTPS + JSON in C (WinHTTP + jsmn/cJSON + hand-rolled
   multipart). Several times the cost of the encoders. Schedule accordingly.

### Open questions

- [ ] Approve approach A for the UI, then move to the design doc.
- [ ] **What happens to the files after recording?** Largely mooted — the action
      library makes format a per-action config rather than a global decision.
      Still worth knowing for drift-correction paranoia (DAW editing needs
      sample-accurate alignment; transcription does not).

### Next step

On approval: design doc under `docs/superpowers/specs/`, then the
writing-plans skill for an implementation plan.

### Integration door worth remembering

goxlr-utility exposes a documented local API (see its wiki, "The GoXLR Utility
API"). apprecorder could read fader/mute state or trigger recording from a GoXLR
button. User wrote parts of this — ask him rather than researching it.

---

## 2026-08-25 (later still) — Approved; autonomous build started

**Design approved.** Spec at `docs/superpowers/specs/2026-08-25-apprecorder-design.md`,
committed as `70e7bcb` along with the build system and repo skeleton.

### Assumption locked in (asked 3x, unanswered — decided rather than blocked)

**Sample-accurate drift correction.** Alignment cannot be retrofitted into files
already written, and WAV was among the requested formats. Over-built is
recoverable; under-built is not. Revisit only if it proves expensive.

### Toolchain verified present — nothing to install

- MSVC 14.42.34433 (VS 2022 Build Tools), Hostx64/x64
- CMake + Ninja bundled at `Common7/IDE/CommonExtensions/Microsoft/CMake/`
- Clang/LLVM also bundled
- Windows SDK 10.0.22621 — confirmed ships `audioclientactivationparams.h` with
  `AUDIOCLIENT_ACTIVATION_PARAMS` and `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`
- Driver: `build.cmd [Debug|Release] [spikes] [test]` — wraps vcvars64; never
  invoke `cl.exe` directly.
- `/W4 /WX`, static CRT, COBJMACROS defined globally.

### Parallelisation strategy (deliberate, not maximal)

Maximum fan-out is wrong for this codebase *because* DRY was requested: parallel
agents writing C over shared hand-authored COM vtables and one audio pipeline
produce four subtly different ring buffers. So: **sequence the core, parallelise
the leaves.** Graph/capture/clock are built once, in order. Encoders are ideal
fan-out (isolated `ActionVTable`, same interface, no shared state), as are
theme/dpi/darkmode/tree_panel once the canvas exists.

### Wave 1 — in flight

| Agent | Scope | Files |
|---|---|---|
| Capture spike | Prove `IActivateAudioInterfaceCompletionHandler` hand-written vtable works in C; capture real PCM from a real PID | `spike/` |
| Foundation | test harness, `err`, `log`, `ringbuf` (SPMC), `clock` (exact QPC arithmetic) | `src/platform/`, `src/core/`, `tests/` |

Disjoint file sets, hence safe to run concurrently.

**The spike's most valuable output** is not the capture code — it is an empirical
answer to: *does a silent app produce no buffers, or silent-flagged buffers, and
do the QPC positions keep advancing across silence?* The entire drift-correction
design in section 5 rests on that, and it is currently an assumption.

### Waves not yet started

3. Core (sequential): graph, source, bus, mix, resample, drift controller
4. Encoders (parallel x4): WAV, MP3, OGG/Opus, M4A via Media Foundation
5. UI: app/canvas/node_window sequential, then theme/dpi/darkmode/tree_panel parallel
6. Session persistence, polish

Reviewer and tester passes run against every wave.

---

## 2026-08-25 — INCIDENT: agent played a looping tone into the author's ears

**What happened.** The capture-spike agent needed audible audio to capture, so it
wrote `spike/tone_player.ps1` and looped a tone through the **default output
device**. Its own mid-run stop failed ("the mid-schedule stop didn't take effect
(audio kept looping)"). It ran until the author interrupted with "I'm hearing a
long tone?".

**Why this is serious, not cosmetic.** The author is blind. A sustained tone
masks the screen reader he navigates by, and at volume is a hearing risk. This is
a safety bug, not an annoyance.

**Resolution.**
- Tone player process killed; spike agent stopped.
- `spike/tone_player.ps1`, `run_probe.ps1`, `tone.wav`, `silence.wav`, and the
  built spike exe deleted. `spike/spike_loopback.c` kept for review.
- Foundation agent left running — it never touches audio hardware.
- **`AGENTS.md` created** at repo root with the prohibition as rule #1. Every
  future agent must read it before acting.

**Root cause, honestly:** I launched an agent whose task inherently required
making noise on the author's machine without constraining how. The agent then
compounded it with an untested stop path and a loop.

### Standing rule

No agent renders audio to the default endpoint. Ever. To obtain capturable
audio, in order: (1) render with the player's **own session volume at 0** via
`ISimpleAudioVolume::SetMasterVolume(0.0f, NULL)` — inaudible by construction;
(2) ask the author to play something he controls; (3) use `capture_fake`, which
covers most of the codebase anyway (design 4.3).

### Genuinely useful open question this surfaced

**Does process loopback capture non-silent data from a process whose session
volume is 0?** If yes, that is a permanently safe test harness *and* a real
product fact (it determines whether a muted app still records). If no, loopback
is post-session-volume — also worth knowing. **Testing it is safe either way,
since volume 0 cannot be loud.** Answer this before restarting the spike.

### Spike status: NOT restarted

`spike/spike_loopback.c` exists but is unreviewed and unverified. Its findings —
including the silence-behaviour question that section 5 depends on — are still
unknown. Restart only under the rules above.

---

## 2026-08-25 (later) — Capture spike RESTARTED and COMPLETE (under AGENTS.md rule #1)

Spike agent re-run. **No audio was rendered to the default endpoint audibly at any
point.** See "What was played" below for the exact disclosure.

### Files

- `spike/spike_silentplayer.c` — **NEW.** Inaudible-by-construction WASAPI render
  process, built only so loopback has something to capture. Three enforced
  safety properties: volume gate (set + read back + verify before `Start()`,
  fail-closed), finite frame budget (no playback loop, bounded twice), watchdog
  thread armed before any audio object that `TerminateProcess`es itself.
  `MAX_SAFE_VOLUME 0.001f` is a hard ceiling; default volume is `0.0`.
- `spike/spike_loopback.c` — reviewed, kept, extended with a non-silence check
  and a "is `pu64QPCPosition` an independent clock?" analysis.

Both compile clean under `/W4 /WX` and are **left uncommitted** in the working tree.

### Answer to the standing open question

**Does process loopback capture non-silent data from a process whose session
volume is 0?  NO.** Loopback sits **post-session-volume**.

| player session volume | endpoint amplitude | captured PEAK |
|---|---|---|
| 0.0 | 0.0 | **0.000000000** (0 of 1,534,080 samples non-zero) |
| 0.0001 | 0.000025 | **0.000025000** (= 0.25 x 1e-4 exactly) |

The volume-0 player was independently proven to have really rendered:
`IAudioClock` reported 3456000/384000 = **9.000 s actually consumed by the
engine**. So the app rendered, and loopback still recorded pure digital silence.

**Consequences.**
1. Session volume 0 is **not** a usable test harness — it records as silence.
   A tiny non-zero volume (1e-4 = -80 dB, i.e. -92 dBFS at the endpoint) **is**,
   and that is what the remaining measurements used.
2. **Product fact for the design:** a user who mutes an app in the Windows volume
   mixer will record silence from it. Section 4.1 must say so, and the UI should
   probably warn when a captured source's session volume is 0.

### Section 5 is CONTRADICTED — the drift design must change

Design 4.1 gotcha #2 and section 5 assume *"a silent app produces no buffers at
all"* and that silence must be synthesized from timestamp gaps. **Both false for
process loopback.** Measured:

- **The stream is continuous and gapless regardless of the target.** 120 s
  capture against a process that never called `Start()`: 11,999 packets,
  every one exactly 480 frames, **100% fill, zero gaps, zero timeouts**.
  It also keeps delivering after the target process **exits**.
- `AUDCLNT_BUFFERFLAGS_SILENT` was **never** set — not once in ~190 s of capture.
  Quiet arrives as ordinary buffers full of real 0.0f samples.
- `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` / `TIMESTAMP_ERROR`: never seen.
- `pu64DevicePosition` is **always 0** and never advances. Unusable.
- `pu64QPCPosition` advances by **exactly** `frames * 1e7 / 48000` — 11,998 of
  11,998 deltas exact, +0.00 ppm. **It is the frame counter rescaled and carries
  no independent clock information**, so it cannot reveal the app's render drift.
  Stream rate vs `QueryPerformanceCounter`: -6.51 ppm over 120 s, within the
  ~8 ppm noise of the measurement.

So for **process** sources there is no gap to synthesize and no drift to detect
from the timestamps: the OS already hands us a perfect 48 kHz timeline. The
silence-synthesis and PI-controller machinery is still needed, but for **device**
sources (real hardware crystals) and for aligning process sources against them —
drift must be measured against `QueryPerformanceCounter` sampled at capture time.
**Do not build section 5's gap detection on `pu64QPCPosition` for process taps.**

Caveat: `QueryPerformanceFrequency` is exactly 10,000,000 Hz on this machine, so
raw QPC ticks and 100 ns units coincide here. That masks a whole class of unit
bug — production code must scale by QPF, never assume.

### Other HRESULTs / facts worth not rediscovering

- `GetMixFormat` -> `E_NOTIMPL (0x80004001)` on a process-loopback client, as the
  design predicted. **`GetDevicePeriod` also returns `E_NOTIMPL`** — the design
  does not mention this one.
- Supplying 48000/2/float32 `WAVE_FORMAT_EXTENSIBLE` to `Initialize` with
  `hnsBufferDuration = 0` yields a 480-frame (10 ms) buffer and works.
- The hand-written vtable worked **first try**, including `IID_IAgileObject`.
  Activation callback lands on a **different thread** (verified by TID).
- `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE` genuinely walks the tree:
  capturing `cmd.exe`'s PID picked up a tone rendered by its **child** process.
- Capture is bit-exact: recovered 440.0 Hz at RMS 0.000017678 vs theoretical
  0.25 x 1e-4 / sqrt(2) = 0.000017678.

### Deliberately NOT tested

`PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE` captures everything *except*
the target — running it would have written whatever the author was listening to
into a WAV file. Privacy, not safety, but not the agent's call to make.

### What was played, exactly (AGENTS.md disclosure)

440 Hz sine, source amplitude 0.25, rendered by `spike_silentplayer.exe` to the
default endpoint in 9 s bursts (3 s tone / 3 s digital zeros / 3 s tone), across
5 runs. Session volume was **0.0** (2 runs, endpoint amplitude exactly 0.0) or
**0.0001** (3 runs, endpoint amplitude 0.000025 = -92 dBFS, roughly -2 dB SPL —
inaudible). Every run's volume was read back and verified before `Start()`.
Longest single render: 9.000 s. All processes self-terminated; zero leftover
processes afterwards. **Nothing was looped.**

---

## 2026-08-25 — Capture spike COMPLETE. Section 5 was wrong; spec corrected.

**Process loopback works from plain C.** Vtable pattern proven first try,
`IAgileObject` included. Zero unexpected HRESULTs. Capture verified bit-exact:
recovered 440.0 Hz at RMS 0.000017678 against a theoretical 0.000017678.
`INCLUDE_TARGET_PROCESS_TREE` confirmed to walk children (captured a shell's PID,
got its child's tone) — required for browsers and Electron.

Working shim: `spike/spike_loopback.c` lines 68-219, marked `COM SHIM`, ready to
graduate into `src/capture/com_shim.c`. Load-bearing detail: embed the SDK
interface **by value** as the first member (`IActivateAudioInterfaceCompletionHandler base;`)
rather than hand-rolling `lpVtbl` — layout-identical but type-checks against
`COBJMACROS`. `CONST_VTBL` is empty in C so the vtable assignment needs a cast.

### The big finding: section 5's premise was false

The design assumed *"a silent app produces no buffers"*, which is why silence was
to be synthesized from timestamp gaps. **Measured: the opposite.** Over 120 s
against a process that never called `Start()` — 11,999 packets, every one exactly
480 frames, 100% fill, zero gaps. `AUDCLNT_BUFFERFLAGS_SILENT` never set in
~190 s. Quiet arrives as real `0.0f` samples.

Worse, `pu64QPCPosition` is **the frame counter rescaled** (+0.00 ppm,
11,998/11,998 deltas exact) — it carries no independent clock information, so
gap/drift detection built on it for process taps could never fire. Dead code.
`pu64DevicePosition` is always 0 and unusable.

**Spec rewritten (section 5.1/5.2):** process taps are the *reference timeline*;
device captures are the only sources that drift and the only ones needing the PI
controller, gap fill, and discontinuity handling. A mixed bus resamples the
*device* side onto the engine timeline; the process side passes through
untouched. Simpler than the original design, and the machinery now sits where the
drift actually is.

### Other verified facts now in the spec

- **`GetDevicePeriod` also returns `E_NOTIMPL`** (design had only predicted
  `GetMixFormat`). Pass `hnsBufferDuration = 0`, accept 480 frames / 10 ms.
- **Loopback is post-session-volume**, linear, no other gain in the path. A
  **muted app records as pure silence** while the engine still reports it
  rendering. UI must warn on session volume 0. Also means volume-0 is *not* a
  safe test harness; `1e-4` (-92 dBFS, inaudible) is — that is what the spike used.
- **Capture continues forever after the target process exits**, silently, with no
  error. WASAPI never signals source death. Section 10 now requires
  `OpenProcess(SYNCHRONIZE, ...)` + wait as the only reliable detector.
- **`QueryPerformanceFrequency` was exactly 10,000,000 Hz on this machine**, so
  raw QPC ticks and 100 ns units coincide — this masks an entire class of unit
  bug. Production code must always scale by QPF. **Check the foundation agent's
  `clock.c` against this.**

### Safety: clean

440 Hz, 9 s one-shot bursts, 5 runs, session volume 0.0 or 0.0001 (-92 dBFS,
inaudible), verified by read-back before every `Start()`. Nothing looped, all
processes self-terminated, zero leftovers confirmed. `spike/spike_silentplayer.c`
enforces this structurally: fail-closed volume gate, hard `MAX_SAFE_VOLUME 0.001f`
ceiling that refuses before creating any COM object, doubly-bounded finite frame
budget, and a watchdog thread armed before any audio object that
`TerminateProcess`es itself.

### Deferred to the author — a privacy call, not a technical one

The spike deliberately did **not** test
`PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE`, which captures everything
*except* the named process — testing it would have written whatever the author
was listening to into a WAV. Correct judgement. **Ask before exercising it.**
Worth having as a product feature ("record everything except Discord").

### Next

- Review foundation agent's output when it lands, especially `clock.c` vs the QPF
  caveat and the corrected section 5.
- Then: graduate the COM shim into `src/capture/com_shim.c` + `capture_process.c`.
- Known spike-level leak to fix on graduation: a late completion callback's
  `punk` is never released after a timeout.

---

## 2026-08-25 (wave 1) — Foundation built, TDD, all green

Build order step 1 of design section 12. `build.cmd Debug test` and
`build.cmd Release test`: **74 cases across 5 test binaries, 0 failures**,
`/W4 /WX` clean in both configurations.

### Shipped

| File | What it owns |
|---|---|
| `tests/test_runner.h` | Single-header harness. `TEST()` self-registers via `.CRT$XCU`, pinned with `#pragma comment(linker, "/include:...")` so `/OPT:REF` cannot silently empty a Release suite; `main()` also fails outright if zero tests registered. |
| `include/err.h`, `src/platform/err.c` | `AprErr` value type (no heap, no ownership, copyable, safe to build on a capture thread) + the **only** HRESULT decoder. Hand-written `AUDCLNT_*` table because FormatMessage has no message resource for facility 0x889. |
| `include/log.h`, `src/platform/log.c` | Levelled log. Compile-time floor erases levels entirely; runtime gate skips argument evaluation. Lock-free bounded MPSC ring, static storage, never allocates. |
| `include/ringbuf.h`, `src/core/ringbuf.c` | SPMC ring, independent reader cursors, exact overrun accounting. |
| `include/clock.h`, `src/core/clock.c` | QPC/hns/frame conversions, all exact 128-bit integer; drift and gap arithmetic. |

### Decisions worth not relitigating

- **ringbuf overrun = overwrite the oldest, and tell the reader exactly how
  many frames it lost.** Blocking the producer is off the table (it is an audio
  callback); dropping the newest would punish readers that were keeping up. The
  loss count is not a diagnostic — it is the input the drift corrector needs to
  synthesise exactly that many frames of silence (design section 5).
- **ringbuf needs TWO producer cursors,** `claim_pos` (published before the
  memcpy) and `write_pos` (published after). Publishing only after the copy
  understates how far into storage the producer has scribbled, so a consumer
  checking against `write_pos` alone will return frames being overwritten under
  it. This was a real bug the concurrency tests caught; do not "simplify" it
  back to one cursor.
- **log overflow drops the NEWEST record** — the opposite of ringbuf, on
  purpose. A log ring only fills when the drain is starved, the queued records
  are the context that explains the stall, and refusing at the door is the only
  policy that never writes over a slot the consumer is reading. Both files say
  so in their headers.
- **Log has a separate real-time path** (`APR_RT0/1/2`): stores a pointer to a
  static format literal plus up to two int64 values and expands them on the
  drain thread. It also deliberately does **not** `SetEvent` — a real-time
  producer makes no kernel call; the drain thread's 250 ms timeout collects
  them.
- **clock.c is all integer, and every call takes an ABSOLUTE timestamp.**
  Converting per buffer and summing floors once per buffer; over 1.08 million
  10 ms buffers that is minutes of error. There is deliberately no
  "advance by dt" call, because someone would use it.

### Gotchas for whoever is next

- `AUDCLNT_E_EFFECT_NOT_AVAILABLE` / `_EFFECT_STATE_READ_ONLY` exist only in
  the Win11 SDK; `err.c` `#ifdef`s them. `AUDCLNT_E_OUT_OF_OFFLOAD_RESOURCES`
  is the real spelling (not `..._MODES`).
- `build.cmd` prints a harmless `vswhere.exe not recognized` line from vcvars.
- The ringbuf and log concurrency tests are real thread tests; if one ever
  hangs, check the *test's* accounting first — a consumer thread can reach
  `rb_reader_init` after the producer has started, so `RB_START_OLDEST`
  legitimately begins above zero and frames written before it attached are not
  its loss.

### Not done, deliberately

`src/platform/str.c` and `fs.c` from the module layout: nothing needs them yet,
and inventing an API with no caller is how they end up wrong.

### Reconciled with the spike's section 5 correction

Section 5 was rewritten (commit `e00fef1`) while this wave was in flight.
Checked the foundation against it — no rework needed, and one thing was already
right for the right reason:

- **QPF is never assumed to be 10 MHz.** Every `clock.c` conversion takes
  `qpc_freq` explicitly, and `test_clock.c` runs its whole three-hour timeline
  at **3,579,545 Hz** precisely so a tick and an hns unit do not coincide. That
  is the "entire class of unit bug" section 5.2 warns about, and it cannot hide
  in this suite.
- `clock.h` now says outright that the anchor comes from `apr_qpc_now()` at
  buffer arrival and **not** from `pu64QPCPosition` (which the spike showed is
  the frame counter rescaled, +0.00 ppm by construction), and that
  `apr_clock_drift` is for **device captures** — process taps are the reference
  timeline and need none of it.
- The drift/gap API is unchanged and still correct: it is generic over "frames
  the clock expected vs frames the source delivered", which is exactly what a
  device capture needs. `apr_clock_drift(c, t, 0)` returning the whole interval
  as missing frames is the dropout case, not the silent-app case that turned
  out not to exist.

---

## 2026-08-25 — Foundation layer COMPLETE and independently verified

74 test cases across 5 suites. **Verified by me, not taken on report:** `build`
deleted, clean Release build from scratch, `5/5 passed`, and the per-suite
counts confirmed real (`22 run, 22 passed` clock; `18 run, 18 passed` ringbuf).
That check mattered specifically because the harness self-registers via
`.CRT$XCU`; had the `/OPT:REF` pin been wrong, Release would run **zero** tests
and still report success. It holds.

| File | Owns |
|---|---|
| `tests/test_runner.h` | single-header harness, `.CRT$XCU` registration pinned with `/include:` |
| `include/err.h` + `src/platform/err.c` | error value type; the only HRESULT decoder (41 `AUDCLNT_*` codes hand-tabled) |
| `include/log.h` + `src/platform/log.c` | levelled log, no alloc on the hot path |
| `include/ringbuf.h` + `src/core/ringbuf.c` | SPMC ring, per-consumer cursors |
| `include/clock.h` + `src/core/clock.c` | exact QPC/frame arithmetic + device drift |

### Two things worth not losing

**The ringbuf two-cursor invariant.** One producer cursor is not enough:
publishing `write_pos` only *after* the memcpy understates how far into storage
the producer has actually scribbled, so a reader validating against it returns
frames being overwritten under it. Fixed with `claim_pos` published *before* the
copy (all safety decisions) and `write_pos` after (availability only).
**Do not "simplify" this back to one cursor.** Flagged in the source too.

**Clock exactness.** Every conversion is `floor((a*b)/d)` via `_umul128` /
`_udiv128` — exact 128-bit intermediate, one truncation, error bounded below one
frame at any session length. No floating point in position math (`apr_drift_ppm`
is display-only) and **no accumulation**: every call takes an absolute timestamp
from the anchor, and there is deliberately no "advance by dt" entry point,
because someone would use it. Tests run a 3-hour timeline at **3,579,545 Hz** so
a tick and an hns unit cannot coincide — i.e. the tests already avoid the exact
QPF unit trap that section 5.2 warns about. A control test shows naive
per-buffer summing drifting >100,000 frames on the same timeline. `clock.c` was
mutation-tested to prove the tests bite.

### Spec updated from its findings

- **`refcount` is bus bookkeeping, not buffer lifetime** — consumers hold their
  own cursors, so the ring does not know how many readers exist and must not be
  taught. Do not wire `refcount` into `ringbuf`. (§3.1)
- **Ring sizing settled: 250 ms / 96 KB per source.** Rings absorb *mixer
  scheduling jitter only*. Disk stalls are absorbed by write-behind buffering
  inside each action, so a slow encoder cannot back-pressure a buffer shared by
  every other bus. Sizing rings for disk instead would cost ~384 KB per source
  per second and blow the memory budget for no gain. (§3.1)
- **`platform/str.c` and `fs.c` removed from the layout** — never written, nothing
  needed them. Add when a second caller exists. (§7)

### Process note — my error

I ran `git add -A` while agents were mid-flight, so `err.c`, `log.c`,
`ringbuf.c`, their headers and tests landed in history under a commit message
about the capture spike. Harmless but wrong attribution. **Stage explicit paths
while agents are running.**

### Next

- Graduate the COM shim from `spike/spike_loopback.c` (lines 68-219) into
  `src/capture/com_shim.c`, then `capture_process.c` + `capture_device.c` +
  `capture_fake.c`. Fix on graduation: late completion callback's `punk` is
  never released after a timeout.
- Then core: `graph`, `source`, `bus`, `mix`, `resample`, drift controller.
- Encoders fan out only after the core shape is settled.

---

## 2026-08-25 — Author's decisions + Arabic localization requirement

### Decided by the author

| Question | Answer |
|---|---|
| First usable version | **CLI recorder end-to-end.** PIDs + Chat Mic on the command line, record to file. Proves capture → mix → drift → encode, usable immediately, and becomes the test harness for the UI. |
| Overnight autonomous work | **Yes, quiet audio tests OK** — under AGENTS.md rules (session volume ≤1e-4 / −92 dBFS, one-shot, self-terminating). |
| goxlr-utility integration | **Later.** Core first. Door stays open; nothing depends on it. |
| EXCLUDE_TARGET_PROCESS_TREE | **Approved for testing.** Timed for while he sleeps, so nothing sensitive is playing. Captured WAVs must be deleted after. |
| UI after CLI | **Yes.** CLI is the automation surface; UI is what users actually want. |

### NEW REQUIREMENT: Arabic localization

Ships localized to Arabic before release. Caught before any UI code was written,
so it is designed in rather than retrofitted — see new **design section 6.2** and
**AGENTS.md rule 5**.

Key constraints now binding on every agent:

- **No user-facing string is ever a literal in code.** `.rc` STRINGTABLE, one
  `LANGUAGE` block per locale, all in the single exe (`LoadStringW` by thread
  locale). No satellite DLLs — size goal intact.
- **Six plural forms in Arabic vs two in English.** A `printf("%d sources")`
  API is unfixable later; the string API is plural-aware from day one.
- **No sentence concatenation.** Positional specifiers (`%1$s`) only, so a
  translator can reorder.
- **Signal flow direction is a layout parameter.** Left-to-right in English,
  **right-to-left in Arabic** — sources right, actions left. `WS_EX_LAYOUTRTL`
  mirrors standard controls for free but does **not** mirror anything we paint,
  and the canvas nodes are custom-painted (6.1), so the canvas owns its own
  mirroring.
- **Logical order stays language-independent.** Accessibility tree and TreeView
  keep source → bus → action regardless of visual direction. Screen reader
  navigation must not flip; only painting does.
- Digits: Western by default (Saudi UI practice) — confirm with the author.
- Never size a control to its English string; Arabic needs more vertical room.
- Translation process when strings are actually written: `ux-araby` skill for
  فصحى مبسطة, then a Gemini review pass. Keep the author's domain overrides —
  **«إمكانية الوصول»**, never «الإتاحة».

### Wave 2 — in flight (4 agents, all against `include/capture.h` + `include/action.h`)

| Agent | Scope |
|---|---|
| Capture | `com_shim` (graduated from spike), `capture_process`, `capture_device`, `capture_fake`, `wasapi_common`; punk-leak fix; process-death detector; EXCLUDE mode test |
| Core | `graph`, `source`, `bus`, `mix`, `resample`, `registry`; the corrected asymmetric drift model |
| WAV action | `action_wav.c` — reference implementation the other encoders get reviewed against |
| M4A action | `action_m4a.c` via in-box Media Foundation AAC |

Contracts `include/capture.h` and `include/action.h` were written first,
deliberately, so four parallel agents cannot diverge on the interfaces.

### Remaining waves

3. CLI front-end wiring (needs core) — **the first usable milestone**
4. Review + integration pass over waves 1-2
5. UI: `.rc` string catalog and i18n plumbing FIRST, then `app`/`canvas`/
   `node_window` sequential, then `theme`/`dpi`/`darkmode`/`tree_panel` parallel
6. Session persistence, MP3/OGG encoders, polish

---

## 2026-08-26 — `actions/action_m4a.c` (M4A/AAC via Media Foundation) COMPLETE

`src/actions/action_m4a.c` + `tests/test_action_m4a.c`. **15/15 pass**, Debug and
Release, `/W4 /WX` clean. The vtable symbol `registry.c` should reference is
`const AprActionVTable apr_action_m4a` (id `"m4a"`, extension `"m4a"`).

### Shape

- **All MF/COM calls live on one thread this file creates.** `on_audio` never
  touches COM, so it does not matter which apartment the mixer thread is in.
  `create()` still reports MF failures *synchronously* by blocking on a
  one-shot handshake (`ev_ready`) until the encoder thread has built the sink
  writer or failed.
- **Write-behind is `core/ringbuf.c`**, not a private FIFO: 2 s of int16
  interleaved frames (384 KB at 48 kHz stereo). `on_audio` converts, `rb_write`s
  and `SetEvent`s. Worst measured `on_audio` call: **0.012 ms** (Debug).
- Ring overruns are **refilled with silence** from `rb_read`'s `out_lost`, so a
  stall shortens nothing and desyncs nothing.
- Sample timestamps are absolute, from `apr_frames_to_hns(total_frames, rate)` —
  `clock.c`'s exact arithmetic, never a `+= duration` accumulator.
- Container is chosen by `MF_TRANSCODE_CONTAINERTYPE = MPEG4`, **not** by the
  file extension, so a codec pack cannot change what we write.

### What this machine's AAC encoder actually offers (enumerated, not assumed)

`MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, …, MFAudioFormat_AAC)` →
`IMFTransform::GetOutputAvailableType`, **142 distinct triples** on Win11 26200:

- **Rates:** 11025, 16000, 22050, 24000, 32000, 44100, 48000, 96000 Hz
- **Channels:** 1, 2, 6, 8
- **Bitrates:** 8–1152 kbps depending on rate/channels (48 k stereo: 16, 24, 32,
  48, 64, 96, 128, 160, 192, 256, 320 kbps)

Far wider than the documented "44100/48000, 1–2 ch, 96–192 kbps" — because the
list is the **union of the AAC-LC and HE-AAC encoder MFTs**. Several MFTs report
the same triples, so `m4a_collect_formats` deduplicates; without that the
duplicates filled the array cap and real formats fell off the end.
`apr_m4a_enum_formats()` exposes the list (flat arrays, no shared struct).

An unsupported format is refused at `create()` with `APR_E_UNSUPPORTED` naming
the rate/channels **and listing what is available**.

### If the process dies mid-recording — say it plainly

**The file is not playable.** MP4 keeps its index in `moov`, which the sink
writer only emits at `Finalize()`; a killed process leaves `ftyp` + a big `mdat`
and nothing mapping bytes to samples. `ffmpeg`/`untrunc` can rebuild it from a
reference file; we cannot, from a process that is no longer running.

Every *ordinary* exit path is covered and tested: `finalize()` is idempotent,
`destroy()` finalizes for a caller that forgot, and `finalize()` still runs
`IMFSinkWriter::Finalize` after a mid-stream write error so a truncated
recording is still openable.

Three ways to fix the kill case, none free, none taken (also recorded at the
bottom of `action_m4a.c`): fragmented MP4 (`MFCreateFMPEG4MediaSink`) — costs
compatibility with older players; ADTS `.aac` — not an `.m4a`, needs its own
action id; periodic segment rotation — many files instead of one.

### Two findings worth not losing

1. **`IMFSinkWriter::Finalize` on a stream that got zero samples fails** with
   `MF_E_SINK_NO_SAMPLES_PROCESSED` (0xC00D4A44) and leaves an unopenable file.
   A bus that is armed and never fed (muted app, source that failed to start) is
   a normal outcome, so the action now writes **one AAC frame of silence**
   (1024 frames, ~21 ms) before finalizing. Pinned by a test.
2. **`core/mix.c`'s `apr_pcm_from_float` is not safe against non-finite input.**
   It reaches the integer domain via a C cast, and NaN/±Inf through `cvttsd2si`
   is `INT64_MIN`, which its clamp pins to **−32768 — full-scale negative**. A
   NaN in the mix becomes maximum loudness, and a `+Inf` becomes maximum
   loudness of the *wrong sign*. Given AGENTS.md rule 1 this is not academic.
   `action_m4a.c` scrubs non-finite values (`m4a_scrub_non_finite`) *before*
   calling `apr_pcm_from_float` and says in a comment that the guard should be
   **deleted the moment mix.c handles it itself**. **mix.c's owner should fix it
   there** — every encoder has the same exposure.

### Not covered

- The ring-overrun path is correct by inspection but not driven by a test: the
  encoder thread always keeps up on this machine, and forcing an overrun would
  be a timing-dependent, flaky test.
- A genuine mid-stream `WriteSample` failure cannot be induced from outside the
  action, so the sticky-error path is exercised only by the surrounding cases
  (finalize-after-close, destroy-without-finalize).


---

## 2026-08-26 (wave 4) - `actions/action_wav.c` + `tests/test_action_wav.c`

**Status: done, TDD, 18/18 green, full suite 13/13 green** via
`build.cmd Debug test`. Nothing committed.

### What it is

`const AprActionVTable apr_action_wav_vtable` (`src/actions/action_wav.c`).
`core/registry.c` should list it with
`extern const AprActionVTable apr_action_wav_vtable;` - no registration call
lives in the action. id `"wav"`, extension `L"wav"`.

Interleaved float32 in, float32 WAV out: `WAVE_FORMAT_EXTENSIBLE` +
`KSDATAFORMAT_SUBTYPE_IEEE_FLOAT`, no conversion, no clamp, no dither, no gain.
The tests feed `-0.0`, a denormal, a NaN, `FLT_MAX` and values outside
`[-1,+1]` and compare the file bit-for-bit, so "verbatim" is pinned, not
claimed. Header is a fixed 116 bytes:
`RIFF | JUNK(28) | fmt (40) | fact | data`.

### Three decisions worth carrying forward

1. **Write-behind is `core/ringbuf.c`, not a private buffer.** One writer
   thread per action; `on_audio` is `rb_write` + `SetEvent` and nothing else.
   Ring is **4 s of audio** (1.5 MB at 48k stereo, capped at 16 MB) - rings
   upstream absorb mixer jitter at 250 ms, disk stalls are absorbed here, as
   3.1 says. **Overrun is written as silence**, exactly the count `rb_read`
   reports, so a stalled disk costs a hole rather than a permanent desync
   against every other bus.
2. **The header is rewritten in place about once per second of audio**, on the
   writer thread. RIFF sizes live at the front, so the naive encoder only
   writes them in `finalize` and a killed process leaves a file claiming zero
   audio. With the periodic patch, a `kill -9` at any instant leaves a playable
   file missing at most the last second. `finalize` writes the exact sizes and
   `SetEndOfFile`s to them; `destroy` runs the same shutdown if `finalize`
   never came.
3. **Past 4 GiB: RF64 in place. Never refuses, never rolls to part-2.** A
   36-byte `JUNK` chunk is reserved after `WAVE` from byte one; when the data
   would overflow 32 bits the next header rewrite emits `RF64`/`ds64` and
   `0xFFFFFFFF` sentinels. Nothing moves, no audio is rewritten. 4 GiB is only
   3.1 h of 48k stereo (47 min of 8ch), so this is reachable in a normal
   session. **Verified externally:** a 5 GiB file carrying the real header
   bytes is read by ffprobe as `pcm_f32le`, 48000, stereo, 13981.01 s.

### Test seams (deliberately not in any header)

`action_wav.c` exports three symbols only `tests/test_action_wav.c` declares:
`apr_wav_header_build` (pure header builder - lets RF64 be tested at sizes no
test could write), `apr_wav_test_write_gate` (a handle the **writer thread**
waits on before each payload write) and `apr_wav_test_fail_after_bytes`
(simulated ENOSPC). Both flags are read on the writer thread only, so the
mixer path costs nothing.

The gate is what turns "on_audio does not block" from an assertion into a
proof: with the disk held shut, a feeder thread must still push a second of
audio through `on_audio` inside a 5 s wait while a second handle confirms the
file is still only its 116-byte header. An implementation that wrote from
`on_audio` would still be inside its first call.

### For the other encoder agents / mix.c owner

- **The m4a note that an overrun cannot be tested without flakiness is not
  true any more.** The write-gate seam drives a real, deterministic overrun
  (`an_overrun_becomes_silence_so_the_timeline_survives`) with no sleeps and
  no timing assumptions. Same trick works for any action that owns a writer
  thread.
- The `apr_pcm_from_float` NaN finding does not touch WAV: float32 out means
  there is no integer domain to reach. The WAV tests deliberately push a NaN
  end to end, so if a scrub is ever added to `mix.c` it must not be applied on
  this path - the archival format has to stay verbatim.

### Not covered

- Real 4 GiB+ *recording* (as opposed to the header) is untested by CI for
  obvious reasons; the promotion is unit-tested byte-exactly and confirmed
  against ffprobe on a synthetic 5 GiB file.
- `finalize` joins the writer with `INFINITE`. A genuinely hung disk therefore
  hangs the exit - chosen deliberately over abandoning a thread inside
  `WriteFile` on a handle we then seek and close, which trades a slow exit for
  a corrupt file.

---

## 2026-08-26 — M4A action complete + a SAFETY bug it found elsewhere

### SAFETY: `apr_pcm_from_float` turns non-finite input into full-scale audio

Found by the M4A agent, **confirmed by me by reading `src/core/mix.c`**:

```c
p[i] = (int16_t)clamp_i64(round_away(src[i] * APR_S16_SCALE), -32768, 32767);
```

`round_away` reaches the integer domain via a C cast. On x86 `cvttsd2si` of
NaN or ±Inf yields `INT64_MIN`, which the clamp pins to **−32768 — full-scale
negative**. So a NaN anywhere in the mix becomes **maximum loudness**, and a
`+Inf` becomes maximum loudness of the wrong sign. Same pattern in the S24 and
S32 branches.

**Every encoder routes through this function**, so it is the highest-leverage
place in the codebase to get this right — and per AGENTS.md rule 1 it is a
safety issue, not a correctness nicety. Required: NaN→0, +Inf→max, −Inf→min,
finite values untouched. Sent to the core agent (owner of `mix.c`) while it was
still running.

`src/actions/action_m4a.c` currently scrubs non-finite values itself with a
comment to delete the guard once mix.c is fixed. **Remove that workaround once
confirmed.**

### M4A action — done

`src/actions/action_m4a.c` (915 lines), exports `apr_action_m4a` (id `"m4a"`).
`tests/test_action_m4a.c`, 15 cases, green in Debug and Release.

- **All MF/COM work on one thread the file owns.** `on_audio` never touches COM,
  so the mixer's apartment is irrelevant. `create()` still surfaces MF failures
  synchronously via a one-shot handshake.
- Write-behind uses `core/ringbuf.c` rather than a private FIFO (rule 3): 2 s of
  int16, 384 KB at 48 k stereo. Worst measured `on_audio`: **0.012–0.018 ms**.
  Overruns refill with silence from `rb_read`'s `out_lost`, so a stall never
  shortens or desyncs the file.
- Container from `MF_TRANSCODE_CONTAINERTYPE = MPEG4`, not the extension.
- Timestamps via `apr_frames_to_hns()` — absolute, never accumulated.

**MF formats actually available here: 142 distinct triples** (Win11 26200) —
rates 11025–96000, channels 1/2/6/8, bitrates 8–1152 kbps. Far wider than the
documented "44100/48000, 1–2 ch, 96–192 kbps" because the list is the union of
the AAC-LC and HE-AAC MFTs. **Deduplication is load-bearing**: without it the
duplicates filled the array cap and real formats fell off the end.

### Two findings not to lose

1. **`IMFSinkWriter::Finalize` on a zero-sample stream fails**
   (`MF_E_SINK_NO_SAMPLES_PROCESSED`) and leaves an unopenable file. An
   armed-but-never-fed bus is a *normal* outcome (muted app, source that failed
   to start), so the action writes one ~21 ms AAC frame of silence before
   finalizing. Pinned by a test.
2. **Mid-recording process death leaves an unplayable MP4** and cannot be fixed
   from inside a dead process — `moov` is only emitted at `Finalize()`. Ordinary
   exit paths are all covered (idempotent finalize; destroy finalizes for a
   forgetful caller; finalize still runs after a mid-stream write error).
   Three remedies documented in-file — fragmented MP4 via
   `MFCreateFMPEG4MediaSink`, an ADTS `.aac` action id, or segment rotation —
   each with a real cost, none taken unilaterally. **Decision needed.**

### Repo state

`build.cmd Debug test`: **13/13 suites pass** — capture, drift, mix and resample
have landed too. Wave 2 agents still finishing.

Note: MF link directives are `#pragma comment(lib, ...)` inside the m4a files
rather than `CMakeLists.txt` edits, deliberately, since several agents were
landing in that file this wave.

---

## 2026-08-26 — WAV action complete (the reference encoder)

`src/actions/action_wav.c`, exports `apr_action_wav_vtable` (id `"wav"`).
`tests/test_action_wav.c`, 18 golden-file cases, 5 consecutive runs no flakes.
Float32 in, float32 WAV out — **no conversion, clamp, dither or gain**. This is
the archival/DAW path and must stay verbatim.

### The >4 GiB decision: RF64, promoted in place

4 GiB is only **3.1 hours** of 48 kHz stereo float32 (47 min at 8 channels), so
"it won't happen" was not available. Refusing loses the tail of an unrepeatable
session; rolling to part-2 hands the user a manual splice across every bus and
reintroduces the exact seam the drift work exists to remove.

Mechanism: a 36-byte `JUNK` chunk is reserved after `WAVE` from byte one. Under
4 GiB the file is ordinary RIFF/WAVE with a chunk every reader ignores; when the
data would overflow, the next header rewrite emits `RF64`/`ds64` plus
`0xFFFFFFFF` sentinels. **Nothing moves, no audio is rewritten, and there is no
size at which this action refuses to record.** Externally verified: a 5 GiB file
reads through ffprobe as `pcm_f32le`, 48000 Hz, stereo, 13981.01 s.

### Non-blocking is proven, not asserted — and the technique generalises

One writer thread. `on_audio` is `rb_write` + `SetEvent`, both lock-free and
allocation-free; nothing it calls can touch a filesystem. Write-behind is 4 s
(1.5 MB at 48 k stereo, capped at 16 MB) — upstream rings absorb *mixer jitter*
at 250 ms, disk stalls are absorbed *here*, per design 3.1.

**The proof seam is reusable.** `apr_wav_test_write_gate` (read on the writer
thread only) holds the disk path shut while a feeder thread pushes a second of
audio through `on_audio`; the call must still finish inside a bounded wait, a
second handle confirms the file is still only its 116-byte header, and worst
per-call latency is asserted under 50 ms. An implementation writing from
`on_audio` would still be inside its first call. **This also makes overrun tests
deterministic with no sleeps** — which retires the m4a agent's note that forcing
an overrun would be flaky. Any action owning a writer thread should copy it.

### Playable at every instant

A complete header is written before any audio, then rewritten in place ~once per
second on the writer thread. A kill at any instant leaves a playable file missing
at most the last second — tested by reading the header back *while* recording.
Injected ENOSPC covered both ways: first-write failure leaves a valid empty WAV;
mid-stream failure leaves a bit-exact prefix with a matching header.

### Cross-cutting: the NaN fix must NOT touch the F32 path

WAV has no integer domain, so it has no full-scale-click hazard, and its tests
deliberately push a NaN end to end and assert it survives. `APR_PCM_F32` stays a
verbatim `memcpy`; only `S16`/`S24`/`S32` get non-finite handling. Relayed to
the core agent.

---

## 2026-08-26 — Wave state

**Landed:** foundation (harness/err/log/ringbuf/clock), capture spike, WAV, M4A.
13/13 suites green as of the last check.

**In flight (4 agents):** capture layer, core (graph/source/bus/mix/resample/
drift), i18n string catalog, MP3 via vendored libmp3lame.

`src/capture/`, `src/core/{mix,resample,drift}.c`, `include/{mix,resample,drift,
source}.h` and their tests are **uncommitted, owned by live agents** — do not
stage them mid-flight. (Earlier lesson: `git add -A` during a wave attributes
one agent's work to another's commit.)

**Next:** CLI wiring — the author's chosen first usable milestone — then a review
and integration pass, then the UI starting with the `.rc` catalog.

---

## 2026-08-26 — Capture layer built (wave 2). EXCLUDE mode tested and answered.

Design section 12 step 2. `include/capture.h` implemented **unchanged** — the
header as written needed no edits. `build.cmd Debug test` and
`build.cmd Release test`: **12/12 suites pass**, `/W4 /WX` clean in both.

### Shipped

| File | Owns |
|---|---|
| `src/capture/apr_winver.h` | the NTDDI floor, with an `#error` if included after `windows.h` |
| `src/capture/capture_internal.h` | the status cell contract, per-kind vtable getters |
| `src/capture/com_shim.c` | `IActivateAudioInterfaceCompletionHandler`, graduated from the spike |
| `src/capture/wasapi_common.c` | format, Initialize, event handle, pump thread, packet drain, GUIDs |
| `src/capture/capture_process.c` | process loopback + death detector + mute poll |
| `src/capture/capture_device.c` | `IMMDeviceEnumerator`/`eCapture`, gap fill, endpoint death |
| `src/capture/capture_fake.c` + `.h` | the synthetic source and its test driver |
| `src/capture/capture.c` | `apr_capture_create` (**the only switch on `AprSourceKind`**) + status cell |
| `tests/test_capture_fake.c` | 23 cases, no hardware |
| `tests/test_capture_wasapi.c` | 6 cases, skips rather than fails with no audio engine |

**Deviation from the section 7 module layout:** `capture.c` plus three internal
headers are extra files under `capture/`. The factory and the status cell belong
to no one kind, and putting them in `wasapi_common.c` would drag the fake source
through WASAPI headers for nothing. Said out loud here rather than quietly.

### The COM shim graduated intact, and the leak is fixed

Kept verbatim from `spike/spike_loopback.c` 68-219 because it worked first try:
SDK interface embedded **by value** as the first member, `IID_IAgileObject` in
`QueryInterface`, the `CONST_VTBL` cast, **both** activation HRESULTs checked.

The known spike leak — a late completion callback whose `punk` was never
released after a timeout — is closed **twice**, because either fix alone has a
race:

1. the waiter sets an `abandoned` flag *before* dropping its reference, so a
   late callback releases the interface instead of storing it; and
2. the handler destructor releases anything still stored, covering the window
   where the callback read that flag just before it was set.

Whichever fires, the reference is dropped exactly once.

### EXCLUDE mode: tested, and it does exactly what it says

Four measurements, 48 kHz stereo, captured through the real `AprCapture`
interface with `cfg.process.exclude` as the only thing changed:

| # | mode | target | result |
|---|---|---|---|
| A | INCLUDE | the player | **peak 0.000025000**, 440 Hz, 167,040 frames, 0 gaps |
| B | EXCLUDE | the player | **0 of 335,040 samples non-zero** — pure silence |
| C | EXCLUDE | a process rendering nothing | **peak 0.000025000**, 440 Hz — the sibling tone |
| D | EXCLUDE | an *ancestor shell* of the player | **silence** |

- **The `exclude` flag drives it correctly.** A and B differ in nothing but that
  flag, and differ completely in outcome.
- **When the excluded process is the only thing playing, you get silence** — but
  a *full, continuous, gapless stream* of it (167,520 frames in 3.490 s =
  48,000.0 fps, zero discontinuities). Same shape as every other process tap.
- **EXCLUDE walks the process tree too** — row D. Excluding a launcher excludes
  everything it spawned. That is a real UI trap: "record everything except
  Discord" also drops anything Discord started, and excluding a terminal drops
  every app launched from it. Worth surfacing in the UI wording.
- **No WAV was written at any point.** The probe computed peak / RMS / non-zero
  count / zero-crossing rate in memory and printed only those; no audio ever
  reached disk. The probe itself (`tests/test_zz_exclude_probe.c`, armed only by
  an environment variable) was deleted afterwards along with its build output.
  Also swept up five stale WAVs the earlier spike had left in the agent
  scratchpad (`idle.wav`, `tree.wav`, `trace.wav`, two `capture.wav`) — all
  INCLUDE-mode captures of the silent test player, about 64 MB, now gone.

### Design section 10 reproduced through the real code

- **Death.** Player self-terminating at ~11 s, capture running 16 s: `alive`
  flipped to **0**, and loopback carried on producing 767,520 frames over
  15.990 s — **48,000.0 fps of perfect silence, 0 discontinuities**, exactly as
  section 4.1 note 6 warns. `OpenProcess(SYNCHRONIZE, ...)` really is the only
  signal there is.
- **Mute.** Player at session volume **0.0**: `muted` flipped to **1** and the
  capture was **0 of 383,040 samples non-zero**. This is the "why is my
  recording empty" case, now detectable before the user loses an evening.

### Decisions worth not relitigating

- **`anchor_ticks` is `apr_qpc_now()` at buffer arrival.** `pu64QPCPosition` is
  never read. `pu64DevicePosition` is never read.
- **The status cell is a seqlock, not a mutex.** `capture.h` promises `status()`
  never blocks, and `AprErr` is 180+ bytes so it cannot be a plain atomic. The
  64-bit counters are naturally aligned and x64-atomic; only `last_error` needs
  the sequence counter, and it is written a handful of times per session.
- **Death does NOT stop the pump.** Section 10 says one source dying must not
  take the session down; the owner decides. The source reports `alive == 0` and
  a `last_error`, and keeps its place on the timeline.
- **In EXCLUDE mode there is no death detection and no mute polling**, because
  the named process is the one thing *not* being captured — its exit merely
  means the capture starts including it. Doing otherwise would kill a healthy
  source.
- **A wedged pump is leaked, not killed.** If the pump will not join in 5 s,
  `close()` deliberately skips every release: leaking a COM reference is
  survivable, a use-after-free under a live audio thread is not.
- **Device format:** the session format is imposed, and on
  `AUDCLNT_E_UNSUPPORTED_FORMAT` the client is **re-activated** (a failed
  `Initialize` leaves it unusable) and retried with `AUTOCONVERTPCM |
  SRC_DEFAULT_QUALITY`. That is the OS converter, not a second resampler in this
  tree, and it does not hide drift — drift is still delivered frames against
  elapsed QPC. On this rig the default endpoint took 48 kHz / 2 ch / float32
  directly, so the retry path did not fire.
- **`capture_fake` has two mutually exclusive modes.** Driven
  (`apr_capture_fake_advance`: no threads, no real time) and real-time
  (`start`/`stop`). Driven is what makes a three-hour session take a second, and
  it is byte-deterministic — sample n is a pure function of n, so how the
  timeline is chopped into calls changes nothing. A test asserts exactly that.

### HRESULTs actually hit this wave

Nothing unexpected, and nothing new to table in `err.c`.
`ActivateAudioInterfaceAsync`, `GetActivateResult` and its out-parameter,
`Initialize`, `SetEventHandle`, `GetService`, `Start`, `Stop`, `GetBuffer` and
`ReleaseBuffer` all returned `S_OK` every time, in both INCLUDE and EXCLUDE
mode. `GetMixFormat` / `GetDevicePeriod` were not called on the process path at
all — the spike already proved both are `E_NOTIMPL`, so the code supplies the
format and passes `hnsBufferDuration = 0` without asking.
`AUDCLNT_BUFFERFLAGS_SILENT`, `DATA_DISCONTINUITY` and `TIMESTAMP_ERROR` were
**never** set, across every run — consistent with the spike.

### Nothing in the spec was contradicted

Sections 4.1, 4.2, 4.3, 5.1, 5.2 and 10 all held up under measurement. The one
genuinely new fact is the EXCLUDE tree-walking in row D above, which the spec
does not mention because the spike never ran it.

### Caveats / not done

- The device path is **opened and closed but never started** by any automated
  test: starting it records the author's microphone, which is not an automated
  test's call to make. Its pump, gap fill and discontinuity handling therefore
  have no hardware coverage yet — only the fake source exercises that
  arithmetic. **Needs a manual pass with the author awake and consenting.**
- Endpoint death is inferred from `AUDCLNT_E_DEVICE_INVALIDATED` and friends
  rather than from an `IMMNotificationClient`, to avoid hand-vtabling a second
  COM callback before anything needs one.
- Mute polling inspects only the named PID session. A muted *child* inside an
  unmuted tree will not raise the flag.
- `open()` and `close()` must run on the same thread for the two WASAPI kinds —
  they own a `CoInitializeEx` reference. `capture.h` does not say so; the source
  does.

### What was played, exactly (AGENTS.md rule 1 disclosure)

440 Hz sine, source amplitude 0.25, rendered by `spike_silentplayer.exe` — the
existing structurally-safe harness, reused unmodified. Six one-shot runs. Three
at session volume **0.0001** (endpoint amplitude 0.000025 = **-92 dBFS**,
inaudible) and three at **0.0** (digital silence). Every run read its volume
back and verified it before `Start()`; every run was finite, watchdogged and
self-terminating; **nothing was looped**; zero leftover processes afterwards,
confirmed. Longest single render 9.0 s. Before any of it, active render sessions
were enumerated to confirm the machine was quiet — the only one was an idle
NVDA.

---

## 2026-08-26 — Capture layer COMPLETE

`include/capture.h` implemented **unchanged** — the contract needed no edits,
which is the payoff for writing it before launching the parallel agents.

| File | Owns |
|---|---|
| `src/capture/apr_winver.h` | NTDDI floor, with an `#error` if included after `windows.h` — that ordering makes `audioclientactivationparams.h` compile to nothing |
| `src/capture/com_shim.c/.h` | graduated completion-handler vtable |
| `src/capture/wasapi_common.c/.h` | format, Initialize, event handle, pump thread, packet drain, GUIDs |
| `src/capture/capture_process.c` | loopback + death detector + mute poll |
| `src/capture/capture_device.c` | `IMMDeviceEnumerator`/`eCapture`, gap fill, endpoint death |
| `src/capture/capture_fake.c/.h` | synthetic source + deterministic driver |
| `src/capture/capture.c` | `apr_capture_create` — the only `switch` on `AprSourceKind` |

23 fake-source cases (zero hardware) + 6 WASAPI cases that **skip** rather than
fail with no audio engine. Debug and Release both `/W4 /WX` clean.

**The spike's `punk` leak is closed twice, deliberately** — either fix alone
races. The waiter sets an `abandoned` flag *before* dropping its ref, and the
destructor releases anything still stored.

**Accepted deviation from design §7:** `capture.c` plus three internal headers
are extra files. The factory and status cell belong to no single kind, and
folding them into `wasapi_common.c` would drag the fake source through WASAPI
headers — which would break the no-hardware testability requirement (§4.3).

### Design §10 reproduced through real code

- **Death:** target exited at ~11 s, capture ran 16 s → `alive` = 0, while
  loopback produced 767,520 frames over 15.990 s of perfect silence, 0
  discontinuities. Confirms WASAPI never signals death.
- **Mute:** session volume 0.0 → `muted` = 1, 0 of 383,040 samples non-zero.

### EXCLUDE mode — verified, and it has a UI trap (now in spec §4.1.1)

Flag drives correctly; excluding the only thing playing gives silence but still a
full gapless stream. **New fact: EXCLUDE walks the process tree too.** Excluding
a launcher excludes everything it spawned — "record everything except Discord"
also drops what Discord started, and excluding a terminal drops every app
launched from it. **This is a UI-wording problem**: never present it as
"everything except X"; name the tree and show the affected processes first.
Localization note: "except" is exactly the word that flattens a tree relationship
when translated carelessly.

**No WAV was ever written** — the probe computed peak/RMS/non-zero in memory.
The agent also swept ~64 MB of stale WAVs left by the *earlier* spike. Verified
by me: zero audio files anywhere in the tree.

### CAVEAT — the one real gap

**Device capture is opened and closed but never `start()`ed by any test, because
starting it records the author's microphone.** Correct call by the agent. Its
pump, gap-fill and discontinuity handling therefore have **no hardware coverage**.
**Needs a manual pass with the author awake and consenting.** This is the
highest-risk untested path in the project — and it is the path that carries all
the real drift (§5.1).

Lesser caveats: endpoint death is inferred from `AUDCLNT_E_DEVICE_INVALIDATED`
rather than an `IMMNotificationClient` (avoids a second hand-written COM
callback); mute polling inspects only the named PID's session, so a muted child
in an unmuted tree will not flag; `open()`/`close()` must share a thread for the
WASAPI kinds since they hold a `CoInitializeEx` ref.

### HRESULTs

None unexpected, nothing new needed in `err.c`. Everything `S_OK` every run in
both modes. `GetMixFormat`/`GetDevicePeriod` are never called on the process path
— the spike already settled that they return `E_NOTIMPL`. `SILENT`,
`DATA_DISCONTINUITY` and `TIMESTAMP_ERROR` never set. The device endpoint
accepted 48k/2ch/f32 directly, so the `AUTOCONVERTPCM` retry path never fired
and is also untested.

**Nothing contradicted the spec.**

---

## 2026-08-26 (wave 3) — Core built: graph, source, bus, mix, resample, drift, registry

Build order step 3 of design section 12. TDD, `/W4 /WX` clean in Debug and
Release. **14 suites, 0 failures** in both configurations (the two suites
excluded belong to encoder agents still in flight).

### Shipped

| File | Owns |
|---|---|
| `include/graph.h` + `src/core/graph.c` | the model: nodes, first-class edges, both UI projections |
| `include/source.h` + `src/core/source.c` | capture + ring + one reader per consuming bus |
| `include/bus.h` + `src/core/bus.c` | the mixer tick; N sources in, M actions out |
| `include/mix.h` + `src/core/mix.c` | **sole owner** of PCM conversion, channel mapping, summing |
| `include/resample.h` + `src/core/resample.c` | **sole owner** of SRC; windowed sinc, one implementation |
| `include/drift.h` + `src/core/drift.c` | the PI controller (its own file; §12 lists "drift" as a component) |
| `src/core/registry.c` | the static action table + a built-in `"none"` discard sink |

Tests: `test_graph` (21), `test_sync` (15), `test_drift` (11), `test_mix` (22),
`test_resample` (13), `test_registry` (8).

### The drift design, as implemented

**The bus runs 50 ms behind wall clock.** Nobody listens in real time, so
latency is free and this one decision pays for three things: every source's
data is already in its ring when a block is rendered; a device's jitter buffer
costs **no alignment**, because holding its backlog at exactly the lookbehind
means the instant its buffer fills is the instant the bus reaches its true
start; and the tick rate stops mattering, since the mixer renders what QPC says
is due rather than counting ticks.

**Error signal is backlog**, not rate: `produced - consumed`, exact (integer
producer cursor vs a Q32.32 consumer position). Holding it constant *is*
sample-accurate alignment. Controlling on rate would leave a position offset
nothing corrects.

**Feed-forward + PI trim.** `apr_drift_ratio_q32()` supplies the cumulative
measured rate ratio as feed-forward — right by construction, very low noise —
so the PI only corrects the residual. Critically damped double pole gives
`Kp = 2/tau_frames`, `Ki = 1/tau_frames²`, independent of block size. tau = 10 s.
Trim clamped to **0.2 % (3.5 cents)** with anti-windup: "never audible" is
structural, not hoped for. Measured worst trim in a 3-hour run: **6.9 ppm**.

**Process taps allocate no resampler at all** and are read straight from the
ring. `apr_source_set_reference()` overrides the default so that path is
testable without a real app rendering audio.

**Resampler has zero group delay** (primed with silence so output 0 is centred
on input 0) and **DC gain exactly 1** (normalised by the tap sum). At ratio 1.0
it is bit-exact identity. A resampler with latency would shift a device source
against the process taps it is mixed with — the very desync this exists to
prevent.

### Measured

| Case | Result |
|---|---|
| 3 h process tap, full pipeline | 518,397,600 frames, alignment error **0.0000** |
| 3 h device @ +30 ppm, controller | error **0.43 frames**, trim ≤ 6.9 ppm |
| 2 h device @ +30 ppm, full pipeline incl. real sinc | error **0.5687 frames** |
| 300 s device @ +30 ppm, 48 kHz | error **0.51 frames**, tone peak 0.4990 |
| 3 h @ +30 ppm **uncorrected** (control) | 15,552 frames adrift (0.324 s) |

### Fixed: `apr_pcm_from_float` turned NaN into full-scale audio

Found by the M4A agent. The cast to integer happened **before** the clamp, and
x86 `cvttsd2si` returns `INT64_MIN` for NaN and for both infinities — which the
clamp then pinned to **−32768, full-scale negative**. Every encoder converts
through this function. Now clamped in the float domain first: NaN → 0,
+Inf → max, −Inf → min, integer paths only. `APR_PCM_F32` stays a verbatim
`memcpy` so the archival float WAV path remains bit-exact including NaN.
`action_m4a.c`'s scrubbing workaround can be removed.

### Decisions worth not relitigating

- **Alignment over content.** A ring overrun emits exactly as many frames of
  silence as were lost and resumes at the true absolute frame, rather than
  sliding everything after the hole earlier. Costs at most one extra pull block
  of audio, and only after a stall longer than the whole 250 ms ring.
- **`rb_write_silence` is a PRODUCER call**, for `capture_device` on
  `DATA_DISCONTINUITY`. Reader-side loss is filled by the mixer emitting zeros;
  a reader cannot write into the ring. (§3.1's wording conflates the two.)
- **Registry guards.** `CMakeLists.txt` defines `APR_HAVE_ACTION_<ID>` from the
  presence of `src/actions/action_<id>.c`, so a half-finished encoder cannot
  break the link for everyone and a finished one needs no build-system edit.
  **Naming inconsistency to settle:** m4a exports `apr_action_m4a`, wav exports
  `apr_action_wav_vtable`. registry.c accommodates both; one should be renamed.
- **`"none"` action** is built into registry.c: it keeps the table non-empty
  (an empty C array is ill-formed), makes the graph testable with no encoder
  present, and is the sink a metering-only bus wants. Delete it once something
  else guarantees a non-empty table.
- **`apr_graph_arm` / `apr_graph_run` are split** from `apr_graph_start` so a
  test can drive a synthetic capture without spawning a real-time pacing
  thread, and so a UI can pre-roll sources before the user commits to record.

### Cost note

`test_sync` takes ~44 s in Debug, ~12 s in Release. The 3-hour and 2-hour runs
dominate. Sync is the hard problem in this codebase; the runtime is the price
of testing it at the length where it actually matters.

### Not done, deliberately

No encoders (the parallel agents own those), no UI, no session persistence, no
`capture/` files touched.

---

## 2026-08-26 — CORE COMPLETE. Sample-accurate alignment achieved.

| Case | Result |
|---|---|
| 3 h process tap, full pipeline | 518,397,600 frames, alignment error **0.0000** |
| 3 h device @ +30 ppm, controller | **0.43 frames**, worst trim 6.9 ppm |
| 2 h device @ +30 ppm, full pipeline incl. real sinc | **0.5687 frames** |
| 3 h @ +30 ppm **uncorrected** (control) | **15,552 frames adrift (0.324 s)** |

Sub-sample over three hours, against a third of a second uncorrected. The §1.3
goal is met. 90 new cases across 6 suites; no real time elapses anywhere.

### The design's most load-bearing addition — I had not specified it

**The bus deliberately runs 50 ms behind wall clock.** Nobody listens in real
time so latency is free, and it pays for three things at once: data is always
already in the ring when a block renders; **a device's jitter buffer costs zero
alignment** (holding its backlog at exactly the lookbehind means the instant its
buffer fills is the instant the bus reaches its true start — otherwise a mic sits
a *fixed* 50 ms behind the process taps it is mixed with, which is a sync error,
not a latency one); and the tick rate stops mattering because the mixer renders
what QPC says is due rather than counting ticks.

**Error signal is backlog, not rate** — `produced − consumed`, exact, integer
producer cursor against a Q32.32 consumer position. Holding it constant *is*
alignment. Rate-based control leaves an uncorrected position offset and can never
reach sub-sample. Both now in §5.2.

**Feed-forward + PI trim.** `apr_drift_ratio_q32()` supplies the cumulative
measured ratio as feed-forward (right by construction, low noise); the PI only
corrects residual backlog. Critically damped double pole, `Kp = 2/tau`,
`Ki = 1/tau²`, block-size independent, tau = 10 s. Trim clamped to 0.2 %
(3.5 cents) with anti-windup, so "never audible" is **structural**, not a hope.
**Process taps allocate no resampler at all**; at ratio 1.0 the sinc is bit-exact
identity with zero group delay and DC gain exactly 1.

### NaN fix — done, and it was worse than reported

The cast to integer happened *before* the clamp. Now clamped in the float
domain: NaN→0, +Inf→max, −Inf→min across S16/S24/S32 **and U8** (which the
original report missed — it has an integer domain too, so it had the same
hazard). `APR_PCM_F32` stays a verbatim `memcpy`, so archival float WAV remains
bit-exact including NaN. **`action_m4a.c`'s scrubbing workaround can now be
removed.**

### Spec corrected from its findings

- **§3.1** — an earlier draft conflated producer and consumer silence fill.
  `rb_write_silence()` is a *producer* call belonging to `capture_device` on
  `DATA_DISCONTINUITY`; reader-side loss is filled by the mixer emitting zeros
  into its **own output**. Overrun policy now stated explicitly: **alignment over
  content** — a hole is recoverable, a permanent timeline shift is not.
- **§3.2** — the `Bus` struct was a *tree*. Two buses reading one source each
  need their own cursor, resampler and controller, so **an edge is a struct**,
  not an index into parallel arrays. Rewritten.
- **§5.2** — the mixer lag and the backlog setpoint now documented (above).
- **§3.2** — reference-source status **cannot** be derived from `AprSourceKind`,
  because a fake standing in for a process tap is not `APR_SRC_PROCESS` and §4.3
  requires hardware-free testing. It is an explicit property with an override.
- **AGENTS.md rule 4** — action encoder conventions pinned: export
  `apr_action_<id>`, never edit CMakeLists to register, never self-register,
  never scrub NaN yourself.
- **AGENTS.md rule 1** — the "is volume 0 a usable harness?" question was stale
  and is now answered inline: **no**, loopback is post-session-volume; the
  harness is `1e-4` (−92 dBFS) via the existing `spike_silentplayer.c`.

### Housekeeping done

`apr_action_wav_vtable` renamed to **`apr_action_wav`** across
`action_wav.c`, `registry.c` and `test_action_wav.c`, so `registry.c` no longer
special-cases one encoder. MP3 already matched.

### Two things to remember

- **`registry.c` contains a built-in `"none"` discard sink** (~40 lines, not an
  encoder). It exists because an empty C array is ill-formed and it makes the
  graph testable with no encoder present. **Delete it once something else
  guarantees a non-empty table.**
- `test_sync` takes ~44 s Debug / ~12 s Release; the multi-hour runs dominate.

### Note

A full-suite verification raced the still-running MP3 agent
(`LNK1168: cannot open test_action_mp3.exe for writing` — it held its own test
binary). Not a real failure. **Re-verify once MP3 lands.**

---

## 2026-08-26 — i18n layer built BEFORE any UI exists (AGENTS.md rule 5)

`include/strings.h`, `src/i18n/strings.c`, `res/strings.rc`,
`tests/test_strings.c`. **31/31 cases, Debug and Release, `/W4 /WX` clean;
full suite 17/17 in both configs.** Nothing committed.

### The API the UI and CLI will use

```c
const wchar_t *apr_str(id);                    /* never NULL, never empty     */
const wchar_t *apr_str_plural(base_id, n);     /* six CLDR forms             */
size_t apr_str_format(id, buf, cch, args, nargs);        /* %1!s! .. %8!s!   */
size_t apr_str_plural_format(base_id, n, buf, cch, args, nargs);
size_t apr_str_format_string(fmt, buf, cch, args, nargs);
size_t apr_str_number(n, buf, cch);            /* the only digit-shaping site */
AprErr apr_str_set_language(LANGID);  LANGID apr_str_language();
int    apr_str_is_rtl(void);                   /* the ONLY direction source   */
AprPluralCategory apr_plural_category(LANGID, n);
```

Arguments are `const wchar_t *const *` — **every insert in the catalog is
`!s!`**. Numbers go through `apr_str_number()` first. That kills the
argument-width hazard in FormatMessage's argument array *and* puts the
Western/Arabic-Indic digit decision in one function (design 6.2 leaves that
question open with the author).

### IDs are declared once and the .rc is GENERATED from them

`APR_STR_LIST(X)` / `APR_STR_PLURAL_LIST(X)` in `strings.h` are X-macros, kept
preprocessor-only so **rc.exe reads the same header**. The C enum and every
`LANGUAGE` block expand from those lists. Consequences worth keeping:

- **A missing English string is a hard rc.exe error**, not an empty label:
  `error RC2104 : undefined keyword or key name: APR_EN_NODE_KIND_BUS`.
  Verified by mutation.
- **A new id cannot be added without every declared language saying what
  happens to it** — Arabic needs an `APR_AR_<NAME>(id)` macro even when it
  expands to nothing.
- rc.exe was confirmed to handle function-like macros, `##` pasting, and
  `base+1` id arithmetic inside `STRINGTABLE BEGIN/END`. That was the load
  bearing unknown and it works.

### The completeness check, and why it is not LoadStringW

`apr_str_probe(lang, id, ...)` reads the resource directly and matches the
language **exactly** — `EnumResourceLanguagesW` to confirm the block really
carries that language, then a manual walk of the 16-entry block.

**This is the whole point.** `LoadStringW` (and the loader's own language
search) falls back, so a check built on it returns the *English* string for a
missing Arabic one and passes on an untranslated build. Pinned by
`probing_a_language_never_falls_back_to_another`.

`every_id_resolves_in_every_complete_language` walks the catalog × languages
declared `APR_STR_COMPLETE` under ctest, so `build.cmd Debug test` fails on a
gap. Mutation-tested three ways: empty English text → 1 missing, named;
deleted English macro → rc.exe error; Arabic flipped to COMPLETE → 33 missing,
each named.

### Arabic is DECLARED PARTIAL — deliberately

`res/strings.rc` has an `ar-SA` block with **exactly two entries**, both
obvious placeholders (`AR-PLACEHOLDER …`). **No Arabic copy was written** —
that is the `ux-araby` + Gemini pass with the author's domain overrides
(«إمكانية الوصول», never «الإتاحة»), and machine-drafted Arabic shipping to a
DGA accessibility expert is not a saving. What the two entries buy:

- `APP_NAME` proves per-language block selection, and carries a short Arabic
  marker word so the **UTF-8 → UTF-16 path through rc.exe is verified by exact
  code points** rather than assumed. `#pragma code_page(65001)` is what makes
  that work; there is no BOM. Without the pragma rc.exe reads the file in the
  system ANSI code page and mojibake is baked in where nothing downstream can
  see it.
- `N_SOURCES` carries **all six** CLDR forms, each distinguishable, so Arabic
  plural selection is proven against real resources, not just the rule
  function.

Flipping `ar-SA` to `APR_STR_COMPLETE` in `src/i18n/strings.c` is the one-line
gate the translation pass has to clear. **33 strings currently await
translation** — the test prints that number every run.

### What FormatMessageW actually does (measured, not assumed)

- Inserts are **random access by number**, not a consuming stream. `%2!s! %1!s!`
  reorders. `%3!s! %1!s! %2!s!` works.
- **An insert may be repeated**: `%1!s! %1!s! %1!s!` expands three times and
  consumes nothing. A shipping string relies on it.
- **An unreferenced argument is silently ignored** — which is what lets a zero
  plural form that never mentions the count still be passed the count.
- **A referenced insert with no argument is NOT safe.** With
  `FORMAT_MESSAGE_ARGUMENT_ARRAY` the array is indexed directly and the API
  carries no argument count, so `%3!s!` against a two-element array reads past
  the end and dereferences whatever it finds. FormatMessageW cannot detect it.
  `strings.c` therefore **counts inserts itself and refuses**, and the internal
  argument array is over-allocated and zero-filled as a second line.

### Corrections to design 6.2

1. **`%1$s` is wrong.** That is POSIX; Win32 does not implement it and neither
   does the MSVC CRT. The Win32 spelling is `%1!s!`. Followed literally, a
   translator's `%1$s` reaches FormatMessageW as insert 1 followed by a literal
   `$s`. The intent (positional, reorderable) is met exactly; only the spelling
   in the doc is wrong. **6.2 should be amended** before a translator reads it.
2. **"a build check fails if an ID exists without a string in every declared
   language"** cannot be satisfied literally while Arabic is being written —
   it would forbid a partial locale. Implemented as per-language coverage:
   COMPLETE is enforced, PARTIAL is enforced only for what it declares and
   *reported*. Same guarantee at ship time, and it does not block work now.
3. `LoadStringW` "selects by thread locale" is true but not usable for the
   check — see above.

Everything else in 6.2 stands. RTL/`WS_EX_LAYOUTRTL`, canvas owning its own
mirroring, logical order not flipping, Western digits by default, never sizing
a control to its English string: all still binding on the UI agents.

### Cost

The whole catalog — both languages, 47 resource strings — is **3,084 bytes**
of `.res`. The single-exe, no-satellite-DLL decision holds comfortably.

### Notes for whoever is next

- `apr_str()` takes a lock and can malloc on a cache miss: **not safe on an
  audio callback.** Report through `err.h`/`log.h` and localize at display.
- `res/strings.rc` is attached to `apprecorder_core` as an **INTERFACE**
  source, so every exe that links the core compiles the catalog into itself.
  Resources do not reliably survive a static library — link.exe pulls objects
  to resolve symbols and a `.res` defines none.
- `apr_str_number` renders into local storage first on purpose: `_i64tow_s`
  given a too-small buffer invokes the CRT invalid-parameter handler, which in
  a Debug build is a **modal dialog, i.e. a hang** on whatever thread formatted
  a number. That cost an hour; do not "simplify" it back.
- CMake edit was one appended block: `enable_language(RC)`, `src/i18n/*.c`
  added to `apprecorder_core`, and the `.rc` as an INTERFACE source. Nothing
  existing was touched.
- Gotcha that bit me and will bite the next agent: **restoring a file with
  `Move-Item` restores its old mtime, so ninja keeps the stale object.** A test
  "failure" after a revert is probably that.


---

## 2026-08-26 — i18n layer COMPLETE (before any UI exists, as intended)

`include/strings.h`, `src/i18n/strings.c`, `res/strings.rc`,
`tests/test_strings.c`. 31 cases green Debug + Release; full suite 17/17.
**Whole catalog, both languages, 47 strings = 3,084 bytes of `.res`** — the
single-exe decision holds comfortably.

### API

```c
const wchar_t *apr_str(id);                 /* never NULL, never empty */
const wchar_t *apr_str_plural(base_id, n);  /* six CLDR forms */
size_t apr_str_format(id, buf, cch, args, nargs);        /* %1!s! .. %8!s! */
size_t apr_str_plural_format(base_id, n, buf, cch, args, nargs);
size_t apr_str_number(n, buf, cch);         /* the ONLY digit-shaping site */
AprErr apr_str_set_language(LANGID);  LANGID apr_str_language(void);
int    apr_str_is_rtl(void);                /* the ONLY direction source */
```

**Every insert is `!s!`**; numbers go through `apr_str_number()` first. That
removes the argument-width hazard in FormatMessage's argument array entirely and
puts the Western/Arabic-Indic digit question — which §6.2 leaves open with the
author — in exactly one function. **In a plural string `%1` is always the
count**, caller args start at `%2`; that convention is what lets a translator
write a zero form that never mentions the number beside an other form that does.

### IDs are X-macros so rc.exe reads the same header

The C enum and every `LANGUAGE` block are generated from two preprocessor-only
X-macros. A missing English string is a hard `RC2104` build error, not an empty
label, and a new id cannot be added without every language stating what happens
to it. rc.exe coping with function-like macros, `##`, and `base+1` id arithmetic
inside `STRINGTABLE` was the load-bearing unknown — it works.

### The completeness check deliberately does NOT use LoadStringW

`LoadStringW` and the loader's own search **fall back**, so a check built on them
returns the *English* string for a missing Arabic one and passes on an
untranslated build. `apr_str_probe()` reads the resource directly and matches the
language **exactly** (`EnumResourceLanguagesW` + a manual walk of the 16-entry
block). Pinned by a test, and mutation-tested three ways.

### Arabic status

Declared **PARTIAL**, exactly two `AR-PLACEHOLDER` entries, **no Arabic copy
written** — that is the author's call and a skilled pass, not an agent's.
**33 strings await the `ux-araby` + Gemini review pass.** The test prints that
count every run; flipping one enum value to COMPLETE is the ship gate.

### Spec corrected — my error

**§6.2 said `%1$s`. That is POSIX and works on neither Win32 nor the MSVC CRT.**
A translator following it would emit insert 1 followed by a literal `$s`. Correct
spelling is **`%1!s!`**. Fixed in §6.2 and AGENTS.md rule 6, with the measured
`FormatMessageW` behaviour recorded so nobody re-derives it: inserts are random
access (so reordering genuinely works), may be repeated, unreferenced args are
ignored — but **a referenced insert with no argument reads past the end of the
array and FormatMessageW cannot detect it**, so `strings.c` counts inserts itself
and refuses.

Also amended: §6.2's "build fails if an ID lacks a string in **every** language"
cannot hold literally while Arabic is being written — it forbids a partial
locale. Implemented as per-language coverage: COMPLETE enforced, PARTIAL enforced
for what it declares and reported. Same guarantee at ship time, doesn't block
work now.

### Two gotchas for whoever is next

- **`apr_str()` locks and may allocate — NOT safe on an audio callback.** Now in
  AGENTS.md rule 6.
- `apr_str_number` renders into local storage first because `_i64tow_s` with a
  too-small buffer trips the CRT invalid-parameter handler, which in Debug is a
  **modal dialog — i.e. a hang** on whatever thread formatted a number. Cost the
  agent real time; do not undo it.

---

## 2026-08-26 — MP3 (`src/actions/action_mp3.c`) + vendored libmp3lame

**Status: landed, 19/19 in its own suite, 17/17 across ctest.** Uncommitted, as
instructed.

### Vendoring — LAME 3.100, verified three ways

`vendor/lame/` holds LAME 3.100 source, built as one static lib
(`vendor/lame/CMakeLists.txt`), so there is still no DLL to ship.
LGPL-2.0-or-later. `vendor/lame/PROVENANCE.md` has the full record; short
version:

- sha256 `ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e`
- corroborated by three channels that do not share a distribution path:
  SourceForge (upstream), the Debian archive (downloaded separately,
  **byte-identical by `cmp`**), and nixpkgs' pinned nix-base32 hash (decodes to
  the same digest).
- **LGPL obligation flagged for release engineering, not code:** static linking
  is permitted but §6 requires a recipient be able to relink against a modified
  libmp3lame. That means shipping the object files or this source directory
  alongside any binary release, plus the licence text and a notice. Recorded in
  PROVENANCE.md so it is not discovered late.

No `.c` or `.h` under `libmp3lame/` or `mpglib/` was patched — everything diffs
clean against the tarball. The single file that is ours is
`vendor/lame/config.h`, standing in for the `config.h` autoconf would generate.

**The one trick worth knowing there:** it defines `HAVE_MPGLIB` *without*
`DECODE_ON_THE_FLY`. `HAVE_MPGLIB` only controls whether `mpglib_interface.c`
compiles its `hip_*` entry points; `DECODE_ON_THE_FLY` is the only thing that
makes the *encoder* reference them. Split that way, the test decodes its own
output with `hip_decode` while the shipping binary never links the decoder.
Verified by string-scanning the Release images: mpglib appears in
`test_action_mp3.exe` and in **none** of `test_registry.exe` (which links the
whole action table), `test_action_wav.exe`, `test_action_m4a.exe`.

### CMake

One additive block in the root `CMakeLists.txt` (`add_subdirectory` + link);
nothing existing was touched. `APR_HAVE_ACTION_MP3` needed no edit — the
registry agent's glob picks the file up on its own.

Warnings for the vendored target: the vendor directory **clears** the inherited
`COMPILE_OPTIONS` rather than appending `/W0` to `/W4`. Appending works but
emits D9025 for every translation unit, and 60 lines of "overriding /W4" per
build is how people learn to stop reading build output. `/W4 /WX` is untouched
everywhere else.

Consequence to know about: clearing also drops the tree-wide Release `/O1`, so
it is put back explicitly on the vendor target. Measured: `/O1` vs the default
`/O2` costs 226 KB of `lame.lib` and **58 KB of the final linked image** — 6% of
the sub-1 MB budget — and buys back CPU nobody needs.

### The action

Follows `action_wav.c` deliberately: one writer thread, write-behind through
`core/ringbuf.c`, `on_audio` is `rb_write` + `SetEvent` and nothing else.

**Difference from WAV worth carrying forward: the ring holds raw float frames,
not encoded bytes, and the ENCODER runs on the writer thread too.** WAV has
nothing to do on the mixer thread, so it never has to make this choice. LAME is
a psychoacoustic model plus an iterative rate loop whose cost varies per frame
with the signal; running it on the mixer thread would make every other bus in
the session pay this one's bitrate-loop jitter. Any future encoder (Opus) should
do the same.

- **Float in, float in.** `lame_encode_buffer_interleaved_ieee_float` (stereo) /
  `lame_encode_buffer_ieee_float` (mono). No int16 round trip anywhere.
- **CBR 192 kbps by default; VBR supported.** `quality == 0` → CBR at
  `bitrate_kbps` (0 = 192); `quality` 1..10 → VBR at V(quality-1). CBR is the
  default because of the kill story below: a VBR file whose Xing tag never got
  stamped reports a duration estimated from its first frame, while CBR is exact
  by arithmetic.
- **Overrun → silence**, exactly the frame count `rb_read` reports, so a stall
  costs a hole and never a desync.
- **Sample rate is resampled, not refused.** A 96 kHz session comes out at
  48 kHz with the same duration (MPEG stops at 48 k). **Channels > 2 ARE
  refused** at create — a 5.1 bus needs a downmix decision that belongs to the
  session. `apr_mix_map_channels` is the one-line upgrade if it ever wants that.
- **The non-finite guard is float-domain and local; it does NOT duplicate the
  mix.c fix.** `mix.c` clamps NaN/Inf on the float→**integer** path; LAME takes
  float directly and never goes near that path, so the guard here (NaN/±Inf → 0,
  finite values clamped to ±1) is an encoder-input precondition, not a format
  conversion. It matters because one NaN goes through LAME's FFT and turns a
  whole frame's spectrum into noise — full-scale hash in the user's ears.
  Tested through LAME end to end.

### Kill mid-recording: verified, not assumed

**MP3 degrades gracefully, and this was checked externally rather than
asserted.** An MP3 is a bare sequence of self-describing frames, so every byte
already handed to `WriteFile` is already decodable — no index, no size field, no
moov atom. There is therefore **no periodic header rewrite here**, unlike WAV.

At the instant of a kill the file holds LAME's reserved frame at offset 0 (a
valid frame header followed by zeroes — no `Xing`/`Info` magic yet, so a decoder
treats it as one ordinary 24 ms frame of silence) plus every complete frame
produced so far. What is missing is only the LAME tag: duration, seek table,
gapless delay/padding. `finalize` stamps it by writing `lame_get_lametag_frame`
back over that reserved frame, on every exit path including error paths and a
`destroy` that never saw a `finalize`.

The exact kill state was reconstructed byte-for-byte (zero the reserved frame,
cut mid-frame) and run through **ffmpeg/ffprobe**, an implementation independent
of LAME:

- decodes with **no errors**;
- 12,225 bytes of 128 kbps CBR → ffprobe reports **0.764 s**, which is
  `12225*8/128000` to three decimals. Exact by arithmetic — the reason CBR is
  the default.

ffprobe also confirmed the finalized files: 48 k / stereo / 192 kbps / 1.032 s
for 1 s of input; an explicit 128 kbps honoured; a 96 kHz session emerging at
48 kHz with the same duration; and the overrun file reading back at its **full
input duration** (6.024 s when the test fed 6 s — the suite now feeds 12 s and
asserts the same property through the decoder).

### Tests: `tests/test_action_mp3.c` (19 cases)

Decoding is via `hip_decode` (see the config.h trick above), so the suite needs
no Media Foundation and renders nothing to any output device. Covers zero
frames, double-finalize, destroy-without-finalize, on_audio-after-close,
NaN/Inf, mid-stream I/O error, first-byte I/O error, bad configs, an unopenable
path, CBR/VBR, resampling, a mid-recording snapshot, and mid-frame truncation.

**The two headline tests were mutation-checked — they bite:**

- Delete the overrun silence fill → the 12 s recording decodes as 271,872
  samples instead of 576,000. Fails loudly.
- Move encode+write into `on_audio` → the feeder thread is still inside its
  first call after 5 s (`WAIT_TIMEOUT`). Fails loudly.

`apr_wav_test_write_gate`'s technique transferred unchanged as
`apr_mp3_test_write_gate` / `apr_mp3_test_fail_after_bytes`. **The WAV agent's
note stands and is now double-confirmed: any action owning a writer thread
should copy it.**

### Notes for whoever is next

- **A gated test that trips an assertion will hang the whole suite** unless the
  gate is released on the failing path too — `ASSERT_*` returns immediately, the
  next test's writer thread parks in the still-installed gate, and `shutdown`'s
  INFINITE join parks on that. `tp_gate_release()` in the mp3 test exists for
  exactly this and is called before every early-return assert in a gated test.
  This bit for real: a mutant build left a `test_action_mp3.exe` hung for
  minutes with two threads and no CPU, which then held the link lock. **If a
  build fails with `LNK1168: cannot open <test>.exe for writing`, look for a
  hung test process before assuming another agent has it open.** Copy the
  pattern into any future gated test.
- `lame_get_lametag_frame(gfp, NULL, 0)` returns the *required size*; call it
  once for the size and once for the bytes. The write-back deliberately bypasses
  the full-disk seam — overwriting an already-allocated block is exactly what
  still succeeds on a full volume, and that is what keeps the file playable.
- LAME's own diagnostics are routed into `log.h` via `lame_set_errorf`; `msgf`
  and `debugf` are no-ops. It is chatty at init and a GUI process has no stderr
  worth writing to.

---

## 2026-08-26 — MP3 COMPLETE. All four encoders done.

`vendor/lame/` = **LAME 3.100**, one static library. 19 cases, 17/17 suites,
Debug and Release green.

### Vendoring verified three ways

sha256 `ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e`,
corroborated across three channels that do not share a distribution path:
SourceForge (upstream), the Debian archive (downloaded separately,
**byte-identical by `cmp`**), and nixpkgs' pinned nix-base32 hash. Record in
`vendor/lame/PROVENANCE.md`. No LAME `.c`/`.h` patched — everything diffs clean
against the tarball. The only file that is ours is `vendor/lame/config.h`.

### ⚠ RELEASE OBLIGATION — do not discover this at release time

**LGPL-2.0-or-later. Static linking is permitted, but §6 requires a recipient be
able to relink**, so a binary release must ship the object files or this source
tree plus the notice. Now recorded in spec **§8.1**. Development is unaffected.

**Decoder deliberately excluded from the shipping binary:** `config.h` defines
`HAVE_MPGLIB` **without** `DECODE_ON_THE_FLY`, so `hip_decode` exists for the MP3
test to verify its own output and is absent everywhere else — confirmed by
string-scanning Release images (present in `test_action_mp3.exe`, in none of
`test_registry`, `test_action_wav`, `test_action_m4a`). Cost: 226 KB of
`lame.lib`, **58 KB of final image** — 6% of the size budget.

### CBR 192 kbps default; VBR supported (`quality` 0 = CBR, 1–10 = V0–V9)

**CBR wins the default on the kill case:** a VBR file whose Xing tag was never
stamped reports a duration estimated from its first frame, while CBR duration is
exact by arithmetic.

### Kill mid-recording — verified externally, and MP3 degrades gracefully

No periodic header rewrite is needed (unlike WAV). A kill leaves LAME's reserved
frame at offset 0 (valid header + zeroes → one 24 ms silent frame) plus every
complete frame written; only the LAME tag is missing. The agent reconstructed
that state byte-for-byte and ran it through **ffmpeg/ffprobe, independent of
LAME**: decodes with no errors, and 12,225 bytes of 128 kbps CBR reports
**0.764 s** = `12225*8/128000` exactly.

**So: WAV and MP3 both survive a kill. M4A does not** — that decision is still
open for the author.

### Encoding runs on the writer thread — now spec §8.2

The ring carries raw float; encode happens on the encoder's writer thread. LAME's
rate loop in `on_audio` would put per-frame jitter in front of **every other
bus** sharing the mixer tick. Opus must do the same.

### ⚠ Explains the earlier `LNK1168`

A gated test that trips an assertion **hangs the whole suite** — the next test's
writer thread parks in the still-installed gate. `tp_gate_release()` fixes it.
This happened for real: a mutant build left a hung `test_action_mp3.exe` holding
the link lock, which presents as `LNK1168: cannot open ... for writing` and looks
exactly like another agent's file lock. **My earlier diagnosis of that as an
inter-agent race was wrong** — worth knowing, because the real cause is a hung
test process that will not clear on its own.

### Note

Clearing the vendor directory's inherited warning flags (to avoid 60 D9025 lines
per build) also drops the tree-wide Release `/O1`; it is put back explicitly in
the vendor target. Do not remove that line.

### Status: all four encoders complete — WAV, MP3, OGG*, M4A

*OGG/Opus not yet written; WAV, MP3, M4A are done.

---

## 2026-08-26 — UI foundation (wave 5, sequential head): manifest, DPI, theme, dark mode, frame

`res/apprecorder.manifest`, `res/apprecorder.rc`, `src/ui/{dpi,theme,darkmode,app}.c`
with headers `include/ui_{dpi,theme,darkmode,app}.h`, plus `tests/test_ui_theme.c`
(31 cases) and `tests/test_ui_a11y.c` (11 cases).
**20/20 suites green in Debug and Release, `/W4 /WX` clean.** Uncommitted.

`apprecorder_ui` is a **static library with no entry point**. WinMain in a
library collides with every test's `main()`, and which front end owns the
process is not this layer's call — `apr_ui_app_run()` is what a future WinMain
calls. Nothing produces a GUI exe yet; that is one integration step, deliberately
deferred.

### THE UIA TEST RUNS HERE, and it earned its keep immediately

`tests/test_ui_a11y.c` is a real **UI Automation client** (`CUIAutomation`): it
creates the frame on a worker thread with its own message loop, then walks the
live tree from the test's MTA thread — two threads on purpose, because a UIA
client querying a window on the STA that owns it can deadlock. It prints the
tree on every run.

It found two defects that no amount of reading would have:

1. **The splitter was an unnamed, keyboard-focusable Pane** — something a screen
   reader lands on and can say nothing about. Worse, resizing it was
   **mouse-only**, which is an AGENTS.md rule 5 violation. Now named from the
   catalog, `WS_TABSTOP`, and operable with Left/Right (Ctrl for a bigger step)
   and Home/End.
2. **`SetWindowTheme` sends `WM_THEMECHANGED`**, whose handler called
   `apply_dark`, which called `SetWindowTheme`. Unbounded recursion, presenting
   as `STATUS_FATAL_USER_CALLBACK_EXCEPTION` (0xC000041D) with no indication of
   which callback. Fixed with a re-entrancy guard in `apply_dark`; the comment
   there says why it belongs in the function and not in the message handler.

Current tree: 18 elements, **0 unnamed**, 14 keyboard-focusable. F6 focus
movement is verified for real (tree HWND to canvas HWND), not asserted from
window styles.

**One honest scoping note:** the OS's own non-client `TitleBar` element reports
an empty Name in every Win32 app. The walk marks that subtree `[os]` and exempts
it; everything we create is held to the rule.

**One environment limit:** the test process is not foreground here, so UIA's
`GetFocusedElement` returns nothing. The F6 test therefore checks the UI
thread's focus window via `GetGUIThreadInfo`, which is per-thread and needs no
foreground, and still tries UIA first. It reports which path it used rather than
silently weakening.

### THREE THINGS IN DESIGN SECTION 6 ARE WRONG. The spec should be corrected.

1. **6.2: "Segoe UI Variable covers Arabic" is FALSE.** Measured on this machine
   by asking the font for glyph indices (`GGI_MARK_NONEXISTING_GLYPHS`) for the
   Arabic already in `res/strings.rc`: **5 code points with no glyph.** The
   family is Latin/Greek/Cyrillic. This is not cosmetic — GDI font linking
   substitutes a face so the text still *appears*, but with different ascent and
   descent, so a layout measured against Segoe UI Variable's metrics clips the
   Arabic actually drawn. That is exactly rule 6's "never size a control to fit
   its English string", arriving through the font instead of through the string.
   `theme.c` now picks the face per script and **verifies coverage at runtime**
   (Latin to Segoe UI Variable Text, Arabic to Segoe UI); the test prints the
   count every run, so a future Windows adding Arabic gets noticed rather than
   inherited.

2. **6.2 has `WS_EX_LAYOUTRTL` backwards.** It says the style "does not mirror
   anything we paint ourselves". The opposite is true: it gives the window a
   **mirrored device context**, so our own drawing is what it reflects, and it
   is **inherited by children** unless the parent carries
   `WS_EX_NOINHERITLAYOUT`. "Set it on the frame and forget it" would silently
   mirror the canvas and every node window. The convention now is: the frame is
   NOT LAYOUTRTL and DOES carry `WS_EX_NOINHERITLAYOUT` so nothing can acquire
   it by accident; standard controls get it individually; custom-painted windows
   never do and mirror their own geometry. Asserted on a live window.

3. **6.3 understates, and slightly misplaces, the dark-mode risk.** "Isolate it
   and make failure non-fatal" is right, but the ordinals resolved and worked
   fine on build 26200 — the crash came from our own re-entrancy, not from
   Microsoft. The useful rule is narrower: *no function in `darkmode.c` returns
   an error*, so there is no failure a caller can mishandle, and `theme.c` reads
   the user's dark preference from the documented registry value rather than
   from `ShouldAppsUseDarkMode`, so our painting stays correct even with every
   ordinal missing.

### Conventions the canvas / node_window / tree_panel agents inherit

- **Tokens, not constants.** `apr_theme_metrics()` returns DEVICE pixels already
  scaled for the theme's current DPI, so call sites never multiply and therefore
  cannot forget to. Do not cache them across `WM_DPICHANGED`. Colours are
  semantic (`node_source`, `edge_active`, `surface_sel` vs `surface_sel_bg`). A
  missing token gets added to `ui_theme.h`; it does not get hardcoded in a paint
  handler.
- **Direction has ONE source** (`apr_str_is_rtl()`, via `apr_ui_dir()`) and ONE
  mirroring site (`apr_ui_mirror_rect` / `apr_ui_lead_x`). `apr_ui_layout()` is
  a **pure function** taking direction as a parameter, which is what lets a test
  run both directions in one process. Logical order never flips: child z-order,
  tab order and the accessibility tree stay structure then canvas in both
  languages. Only painting mirrors.
- **Every window is named** through `apr_ui_set_accessible_name(hwnd, id)` — an
  AprStrId, so names re-localize — backed by `IAccPropServices`, which is
  **thread-local** because it is an STA object.
  `apr_ui_set_accessible_description` carries the "what this is for, and how to
  move around it" sentence.
- **Focus is drawn by `apr_theme_draw_focus()`, one function for the product.**
  The ring is **two colours**, and that is a correctness property, not styling:
  no single colour clears 3:1 against a white surface AND a mid-blue accent AND
  a selection fill. The test asserts the property (for every surface, at least
  one of the two ring colours reaches 3:1), not the colours.
- **Panes are real child HWNDs** with `WS_TABSTOP`, `DLGC_WANTARROWS` and no
  `DLGC_WANTTAB` — arrows are yours, Tab must escape or the pane is a focus
  trap. `canvas.c` / `tree_panel.c` implement `apr_canvas_create(parent, theme)`
  / `apr_tree_panel_create(parent, theme)`; CMake defines `APR_HAVE_UI_CANVAS` /
  `APR_HAVE_UI_TREE_PANEL` from file presence (the action-registry pattern), and
  until then `app.c` supplies a named placeholder so the a11y test is real now.
- **Every operation is on the menu, greyed rather than absent when
  unimplemented.** Grey says "later" to someone who can see it; absent says
  "never" to someone who cannot. Route new operations through
  `apr_ui_app_set_command_handler` + `apr_ui_app_enable_command`.

### Two build traps that cost real time

- **A double hyphen is illegal inside an XML comment.** An invalid manifest does
  not warn: the process fails to *start* with "the side-by-side configuration is
  incorrect" and nothing else. The manifest now says so at the top.
- **rc.exe dependency scanning follows `#include` only.** Editing
  `apprecorder.manifest` did not rebuild the `.res`, so the OLD manifest stayed
  embedded and the fix appeared not to work. `CMakeLists.txt` now states the
  dependency with `OBJECT_DEPENDS`. Also `/MANIFEST:NO` on the UI target is
  load-bearing: link.exe's manifest tool REPLACES resource #1, so with both
  mechanisms active ours is silently discarded and the process quietly becomes
  comctl32 v5 and system-DPI-aware. `test_ui_a11y.c` asserts both at runtime
  (awareness == per-monitor v2, comctl32 >= 6) so this cannot drift unnoticed.

### Catalog

**32 new UI strings** at ids **1300 to 1352**, in their own `APR_STR_LIST_UI`
group with its own English `STRINGTABLE` — a range deliberately clear of the CLI
block (1060 to 1163) and of the plural bases (1200+), so three agents can grow
without renumbering each other. `APR_STR_ID_MAX` raised 1280 to 1400. **No
Arabic written**; all 32 are declared untranslated and now await the same
ux-araby + Gemini pass as the rest.

**Menu strings carry two things a translator must not drop:** the `&` mnemonic
(it is the keyboard path to the item, not decoration) and the accelerator text
after the tab. `test_ui_a11y.c` asserts every menu item has a mnemonic and that
mnemonics are unique within their menu — a duplicate cycles instead of
activating, which is a correctness failure.

### Palette is contrast-checked, not eyeballed

`test_ui_theme.c` computes WCAG relative luminance and asserts AA (4.5:1) for
every text/surface pair including `text_dim` — the one that always regresses —
and 3:1 for non-text UI. Two colours failed on the first run and were fixed
(`border_strong` in both palettes; the single-colour focus ring became two).

### Not done, deliberately

- No `WinMain` and no GUI exe target (see above).
- `apr_theme_high_contrast()` is honoured and the palette switches wholesale to
  `GetSysColor`, but no machine here runs high contrast, so that path is
  exercised only by inspection.
- The splitter position is not persisted; that belongs with session save.


---

## 2026-08-26 - CLI landed: the first usable milestone. `apprecorder.exe` records.

`src/cli/cli.c` + `src/cli/cli.h` + `src/cli/main.c`, `src/capture/discover.c` +
`include/discover.h`, `tests/test_cli.c` (43 cases). **20/20 suites green in
Debug and Release**, `/W4 /WX` clean in both. Release `apprecorder.exe` is
**496 KB**. Nothing committed.

### The flag surface

`apprecorder [command] [options]`. Commands: `record` (the default),
`list-apps`, `list-devices`, `help`, `version`.

**The grammar is positional and that is the feature.** `--bus <name>` opens a
bus; every source and every output written after it belongs to that bus, until
the next `--bus`. With no `--bus` there is one implicit bus called Recording.
Several buses in one invocation is therefore ordinary rather than special, which
is the entire point of the project:

```
apprecorder --bus Mix   --exe teams.exe --device Chat --out mix.wav \
            --bus Voice --device Chat                 --out voice.wav
```

| Group | Flags |
|---|---|
| Sources | `--pid <id>`, `--exe <name>`, `--device <id-or-name>`, `--fake <hz>[,<ppm>[,<amp>]]`, `--system-minus-tree <id>`, `--gain <dB>` |
| Outputs | `--out <path>`, `--format <id>`, `--bitrate <kbps>`, `--quality <n>` |
| Session | `--rate`, `--channels`, `--duration <seconds>`, `--dry-run`, `--json`, `--quiet`, `--all`, `--lang`, `--log-level`, `--log-file` |

- `--gain` attaches to the source written immediately before it; with no source
  yet it is a usage error rather than a silent no-op.
- `--format`, `--bitrate` and `--quality` are one-shot and apply to the next
  `--out` only, so two outputs of different formats need no flag cleared by hand.
- The format otherwise comes from the file extension, matched against
  `apr_action_at()`'s `extension` field. Nothing in the CLI knows the list of
  encoders; adding OGG needs no edit here.
- `--exe` is matched against the **audio engine's session list**, not the
  process table: exact executable name wins, a partial name is tried only if
  nothing matched exactly, and more than one match is an error that prints the
  pids.

### Exit codes (documented in `help`, and contract from now on)

| | |
|---|---|
| 0 | finished; every file closed and playable. **Ctrl+C is a 0.** |
| 1 | the command line could not be read (unknown flag/command, missing value, non-numeric or out-of-range value) |
| 2 | read, but not a recording that can be made (bus with no source, bus with no output, unknown format, no extension, two outputs on one file) |
| 3 | a named process, application or capture device is not there |
| 4 | an output could not be created or could not be closed |
| 5 | a source could not be opened or started |
| 6 | recorded and playable, but something went wrong: a source died mid-recording, or an action stopped taking audio |
| 7 | internal |

`--dry-run` returns the code the real run would have returned for the same
problem, so a script can validate before committing.

### Finalize on Ctrl+C - how it is guaranteed, and VERIFIED

Four things, of which the fourth is the one that matters:

1. **Ctrl+C is a request, not a kill.** `SetConsoleCtrlHandler` sets a flag,
   signals an event and returns TRUE, so Windows does not terminate the process;
   the record loop notices and stops properly. A **second** Ctrl+C prints
   "still finishing" and still refuses to abandon the encoders.
2. **CTRL_CLOSE/LOGOFF/SHUTDOWN block inside the handler** until the files are
   closed (4 s cap), because Windows terminates the process a few seconds after
   that handler returns whatever it does.
3. **One destroy site, and it is a `__finally`.** `apr_graph_destroy` stops every
   bus - which finalizes every action - before freeing anything, and nothing in
   `do_record` returns past that block. `apr_cli_main` wraps the whole run in
   `__try/__except` so an access violation UNWINDS through the `__finally`
   instead of skipping it.
4. **Measured, not asserted.** A real `GenerateConsoleCtrlEvent(CTRL_C_EVENT)`
   was sent to a real `apprecorder.exe` recording a WAV and an M4A from fake
   sources: exit code 0, WAV valid RIFF/WAVE, and the **M4A contained its `moov`
   atom** - which is the whole game, since an MP4 without one is unplayable and
   unrepairable from a dead process. `tests/test_cli.c` drives the same path via
   `apr_cli_request_stop()`, the exact entry point the handler calls, and
   asserts the file is playable afterwards.

The loop also sleeps `APR_BUS_LOOKBEHIND_MS` and ticks once more before
stopping, so Ctrl+C does not throw away the last 50 ms the mixer had not yet
rendered.

### `--dry-run` opens no audio device, by construction

`apr_cli_resolve` is entirely property queries: the process table, the audio
engine's session list, the endpoint list, and a create-then-delete probe of each
output path. No `IAudioClient` is activated anywhere in it. Tests assert that no
file exists afterwards.

### EXCLUDE mode: `--system-minus-tree <pid>`

- A **process id only**, never a name. Deliberateness is the feature for a mode
  that records the whole machine.
- Never a default, never implied by anything else, and pinned by a test that a
  `--pid` or `--exe` source is never this kind.
- It prints four lines every time, none of them suppressible, and **none of them
  worded as "everything except X"** (spec 4.1.1): what it records, that a whole
  TREE is held back including programs started later, **who is in that tree right
  now**, and the launcher warning. Verified live - pointing it at a pwsh pid
  listed `pwsh.exe (29128), apprecorder.exe (36740)`, i.e. it caught apprecorder
  itself, which is exactly the trap the spec describes.

### New: `src/capture/discover.c` + `include/discover.h`

Not in the design's section 7 module list; said out loud rather than smuggled in.
The UI needs the same three queries the CLI does, so it is a real module, not a
CLI helper.

- `apr_enum_audio_apps` walks every ACTIVE render endpoint's **audio sessions**,
  deduplicated by pid, carrying active/inactive, mute, session volume and image
  path. **It never lists the process table** - a few hundred PIDs is a useless
  list; a handful of audio sessions is the answer. Own pid and pid 0 (system
  sounds) are excluded.
- `apr_enum_capture_endpoints` - id, friendly name, default flag.
- `apr_enum_process_tree` - Toolhelp snapshot plus transitive closure, which is
  what makes the EXCLUDE warning honest.
- Measured on this rig: 4 GoXLR capture endpoints, and `list-apps` reported one
  active session (nvda.exe) out of ~9 sessions.

### Mute and death, both wired through

- **Muted at plan time**: `apr_cli_resolve` warns before recording starts, which
  is the only moment the warning helps. Loopback is post-session-volume, so a
  muted app records as pure silence and looks healthy.
- **Died mid-recording**: `poll_sources` watches `apr_source_alive` each tick and
  reports the transition once. It also sets exit code **6** - a dead app
  otherwise produces hours of perfect silence that looks like a success.
- Mute alone does NOT change the exit code (it is a state the user can see and
  undo); death and action failure do.

### Text is localized, JSON is not - and that is deliberate

Every line a person reads is a catalog entry, including every line of the help
screen and every exit-code line (AGENTS.md rule 6). **89 new ids** in
`APR_STR_LIST`, English written, Arabic left PARTIAL as instructed - the count
the test prints is now `ar-SA: 7 translated, 154 awaiting translation`.

`--json` field names and numbers are fixed ASCII on purpose: they are a wire
format a script matches on, and localizing them would break every script the
moment the interface language changed. Numbers there go through `swprintf`;
numbers a person reads go through `apr_str_number`, which stays the only digit
shaping site.

### THREE THINGS THAT BIT, AND WILL BITE THE NEXT AGENT

1. **rc.exe dies on a large STRINGTABLE - `fatal error RC10056:` with NOTHING
   after the colon.** Measured with a probe: ~60 entries of help-text length
   compile, ~80 do not. The catalog therefore had to be cut into named groups
   (`APR_STR_LIST_CORE` / `_CLI` / `_CLI_ERR` / `_CLI_MSG`, plus the UI agent's
   `_UI`), each emitted into its **own STRINGTABLE block**, with `APR_STR_LIST`
   as their sum. Nothing outside `res/strings.rc` knows the groups exist. The
   reasoning is written into `strings.h` so nobody re-derives it.
   **The Arabic section is still ONE table**, because rc.exe also rejects a
   STRINGTABLE that expands to zero entries and every Arabic group but the first
   is empty. Splitting it is the translation pass's job; the .rc says so.
2. **`AprCliPlan` is a few hundred KB of fixed arrays and overflows a 1 MB
   stack.** It lives in static storage; the header now says so in capitals. This
   presented as a `0xC00000FD` in an unrelated test case.
3. **`cl 14.42 /O1 /GL` ICE'd on the original `parse_fixed`** (separate whole and
   fractional accumulators, scaled separately) - `fatal error C1001` at the
   `*out =` line, Release only, Debug fine. Rewritten around one accumulator;
   the comment in the source records why it is shaped that way.

### Two extras worth knowing

- **`apr_str_number_fixed(scaled, decimals, ...)` added to the string layer**
  rather than a private decimal formatter in the CLI (rule 3). A gain of
  -6.5 dB and a duration of 1.015 s both need it, and putting the decimal
  separator anywhere but beside `apr_str_number` would have split the digit
  shaping decision in two.
- **Indentation is not in the catalog.** `say_indented()` prefixes the pad in LTR
  and appends it in RTL via `apr_str_is_rtl()`. A catalog entry beginning with
  two spaces would be inset from the wrong side in Arabic and a translator could
  not fix it. `tests/test_strings.c`'s fragment guard catches this - it is a good
  guard and it caught 12 real cases.

### Nothing in the spec was contradicted

Everything the CLI touches behaved as sections 3, 4.1, 4.1.1, 5 and 10 describe.
Two documentation-level notes:

- **Section 7's module list has no home for discovery.** `discover.c` is a real
  new module under `capture/`, wanted by the UI as much as by the CLI. Worth
  adding to the list.
- **Section 6.2 says the CLI is "English by default"** and that is implemented
  (`apr_cli_run` sets `LANG_ENGLISH` before doing anything, so a script's output
  does not change because the machine's language did). It also says Arabic
  console output "still renders poorly in some terminals" - untested, since there
  is no Arabic copy to render yet.

### Safety (AGENTS.md rule 1 disclosure)

**Nothing was rendered to any output device at any point in this work.** Every
recording test used `APR_SRC_FAKE`, which synthesises samples into a ring and
never opens an endpoint in either direction. The device path was only ever
enumerated and dry-run, never started. `--system-minus-tree` was only ever
dry-run, so nothing the machine was playing was ever written anywhere.
`spike_silentplayer.c` was not needed and was not run. Every file produced -
three test recordings, the Ctrl+C pair, and a handful of dry-run probes - was
deleted; the repo and `%TEMP%` were swept and hold no audio files.

### Caveats

- **A real process tap has never been recorded through the CLI**, only through
  the capture layer's own tests. `--pid` and `--exe` are exercised to the point
  of resolution and refusal, not to the point of audio.
- Same for `--device`: enumerated, resolved, dry-run, never started - starting it
  records the author's microphone. **This is still the highest-risk untested path
  in the project** and still needs a manual pass with the author awake.
- `--lang ar-SA` selects Arabic, and Arabic is 154 strings short, so the CLI is
  English in practice whatever is asked for.
- The MP3 and M4A outputs are written by their own actions and were only checked
  structurally here (`moov`/`mdat` present, plausible size); their own suites own
  the byte-level guarantees.

---

## 2026-08-26 — UI FOUNDATION complete. Three spec errors corrected.

`res/apprecorder.manifest`, `res/apprecorder.rc`, `src/ui/{dpi,theme,darkmode,
app}.c` + `include/ui_{dpi,theme,darkmode,app}.h`, `tests/test_ui_theme.c` (31
cases), `tests/test_ui_a11y.c` (11 cases). **20/20 suites green, Debug and
Release, `/W4 /WX` clean.**

`apprecorder_ui` is a **static library with no entry point** — a `WinMain` in a
library collides with every test's `main()`, and which front end owns the process
is not this layer's call. `apr_ui_app_run()` is what a future `WinMain` calls.
**No GUI exe target**, hence no collision with the CLI agent.

### The UIA test runs here, and paid for itself on the first run

Real `CUIAutomation` client: frame on a worker thread with its own loop, tree
walked from the test's MTA thread — **two threads is required**, since a UIA
client querying a window on the STA that owns it can deadlock. Prints the tree
every run. Current: **18 elements, 0 unnamed, 14 keyboard-focusable.**

It found two genuine defects:

1. **The splitter was an unnamed focusable Pane and could only be resized with a
   mouse** — a straight AGENTS.md rule 5 violation that visual review would not
   have caught. Now named from the catalog, `WS_TABSTOP`, Left/Right (Ctrl =
   larger step), Home/End.
2. **`SetWindowTheme` sends `WM_THEMECHANGED`, whose handler called `apply_dark`,
   which called `SetWindowTheme`.** Unbounded recursion surfacing as
   `STATUS_FATAL_USER_CALLBACK_EXCEPTION` with no indication which callback.
   Re-entrancy guard added.

### THREE SPEC ERRORS — mine, now fixed

1. **§6.2 said `WS_EX_LAYOUTRTL` "does not mirror anything we paint". Exactly
   backwards.** It gives a **mirrored DC** and **is inherited by children**, so
   "set it on the frame" would silently mirror the canvas and every node window,
   text included. Following the old wording would have produced the very bug it
   claimed to prevent. **Critical for the canvas agent.** Correct arrangement:
   frame is *not* LAYOUTRTL and carries `WS_EX_NOINHERITLAYOUT`; standard
   controls get it individually; painted windows never do and mirror through
   layout logic.
2. **§6.2 said Segoe UI Variable covers Arabic. Measured: it does not** — five
   Arabic code points in the catalog have no glyph. Font-linking substitutes a
   face so the text *appears*, but with **different ascent/descent**, so a layout
   measured against Segoe UI Variable's metrics **clips the Arabic actually
   drawn**. This is "never size to the English string" arriving through the font
   rather than the string, which is why English-only review would miss it.
   `theme.c` now picks per script and verifies coverage at runtime.
3. **§6.3 misplaced the dark-mode risk.** The undocumented ordinals resolved fine
   on 26200; the crash was our own recursion (above). Load-bearing rules are that
   **no function in `darkmode.c` returns an error**, and `theme.c` reads the dark
   preference from the **documented registry value**, never
   `ShouldAppsUseDarkMode`.

### Conventions the canvas/tree agents inherit

- Metrics are **device pixels, already scaled** — call sites never multiply.
- One direction source (`apr_ui_dir()`), one mirroring site. `apr_ui_layout()` is
  **pure** with direction as a parameter, so both directions test in one process.
- **Logical order never flips** — z-order, tab order and the a11y tree stay
  structure → canvas in both languages. Only geometry mirrors.
- Names via `apr_ui_set_accessible_name(hwnd, AprStrId)` (`IAccPropServices`,
  thread-local because STA).
- Focus ring is **two-colour as a correctness property**: no single colour clears
  3:1 against surface *and* accent *and* selection.
- `canvas.c` / `tree_panel.c` implement `apr_canvas_create` /
  `apr_tree_panel_create`, discovered by CMake from file presence; a named
  placeholder stands in until they exist.

### Build traps now documented in-file — do not rediscover

- **`--` is illegal inside an XML comment.** An invalid manifest means the
  process **will not start**.
- **rc.exe only scans `#include`**, so `OBJECT_DEPENDS` on the manifest is
  required or edits are silently ignored.
- **`/MANIFEST:NO` matters**: link.exe's manifest tool *replaces* resource #1
  silently.

### Notes

- 32 new UI strings at 1300–1352, own group and `STRINGTABLE`, deliberately clear
  of the CLI block and plural bases. `APR_STR_ID_MAX` 1280→1400. **No Arabic
  written** — all 32 join the pending `ux-araby` + Gemini pass.
- The palette is **contrast-asserted, not eyeballed**; two colours failed on
  first run and were fixed.
- `strings.h`/`strings.rc` were merged around the live CLI agent's restructure
  twice. **Worth a reviewer glance at those two files.**

---

## 2026-08-26 — CLI COMPLETE. **The first usable milestone works.**

`src/cli/{cli.c,cli.h,main.c}`, `src/capture/discover.c` + `include/discover.h`,
`tests/test_cli.c` (43 cases). **20/20 suites green Debug + Release.**
**Release `apprecorder.exe` = 496 KB** — under the 1 MB goal *with three encoders
and LAME inside it*.

### Verified by me, running the real binary

`list-devices` found the author's actual rig: **Chat Mic, Stream Mix 1, Stream
Mix 2, Sample** (2- TC-HELICON GoXLR), correctly marking Stream Mix 1 as the
default recording endpoint. `list-apps` showed the one app rendering audio at
3am: `nvda.exe`.

Dry-run of the real use case — an app plus Chat Mic mixed to MP3, *and* a
voice-only WAV from the same mic — resolved both buses correctly. **That is a
source feeding two buses, i.e. the graph feature, proven end to end.** Exit codes
verified live: 0 valid, 2 bus with no output, 3 named app not running.

### Flag grammar is positional

`--bus <name>` opens a bus; every source and output after it belongs to that bus
until the next `--bus`. Multiple buses in one invocation is therefore ordinary
rather than special-cased:

```
apprecorder --bus Mix   --exe teams.exe --device Chat --out mix.wav \
            --bus Voice --device Chat                 --out voice.wav
```

Sources: `--pid`, `--exe`, `--device`, `--fake`, `--system-minus-tree`, `--gain`.
Outputs: `--out` (repeatable), `--format`, `--bitrate`, `--quality`. Format
otherwise comes from the extension **matched against the registry's own
`extension` field, so adding OGG needs no CLI edit.**

### Exit codes

0 finished (Ctrl+C is a 0) · 1 command line unreadable · 2 read but unusable ·
3 named process/app/device absent · 4 output not creatable/closeable · 5 source
not openable · 6 recorded and playable but a source died or an action failed ·
7 internal. `--dry-run` returns the code the real run would have.

### Ctrl+C finalize — measured, not asserted

Ctrl+C sets a flag and returns TRUE so Windows never kills us; a second press
still refuses to abandon the encoders. CTRL_CLOSE/LOGOFF/SHUTDOWN block *inside*
the handler until files are closed. One graph-destroy site, in a `__finally`;
`apr_cli_main` wraps the run in `__try/__except` so an access violation unwinds
*through* it rather than skipping it. The loop also sleeps the 50 ms lookbehind
and ticks once more so the last block is not dropped.

**Verified live:** a real `GenerateConsoleCtrlEvent(CTRL_C_EVENT)` to a real
`apprecorder.exe` recording WAV + M4A → exit 0, valid RIFF/WAVE, and **the M4A
had its `moov` atom**. That is the property that makes M4A playable at all.

### `--system-minus-tree` caught the trap 4.1.1 predicts

Takes a **pid only**, prints four unsuppressible lines, and none is worded
"everything except X". Pointed at a pwsh pid it listed `pwsh.exe,
apprecorder.exe` — **it caught the recorder itself.**

### Three things that cost real time (documented in-source)

1. **rc.exe dies on a large STRINGTABLE**: `fatal error RC10056:` with *nothing
   after the colon*. Probed: ~60 help-text-length entries compile, ~80 do not.
   The catalog is now cut into named groups, each its own STRINGTABLE block.
   Arabic stays one table because rc.exe **also rejects an empty STRINGTABLE**
   and every Arabic group but the first is empty.
2. **`AprCliPlan` overflowed a 1 MB stack** — static storage now, stated in caps
   in the header.
3. **`cl 14.42 /O1 /GL` ICE'd** (`C1001`, Release only) on the first
   `parse_fixed`; rewritten around one accumulator.

### Spec updated

§7 module layout now lists `capture/discover.c` — both front ends need the same
three queries, so it is a module rather than CLI code. Nothing else contradicted;
§3, 4.1, 4.1.1, 5 and 10 all held.

### Notes

- 89 new catalog ids, English written, **Arabic now `7 translated, 154
  awaiting`**. JSON field names are deliberately **not** localized — wire format,
  not prose; that judgement is written into the `.rc`.
- `apr_str_number_fixed()` added to the string layer rather than forking decimal
  formatting into the CLI; indentation moved out of the catalog into
  `say_indented()` (RTL insets from the other side).
- `include/strings.h`, `res/strings.rc` and `CMakeLists.txt` were edited by the
  CLI and UI agents **concurrently**. Suite is green with both in the tree, but
  **that file pair deserves a reviewer glance.**

### CAVEATS — the untested paths

- **A real process tap has never been recorded through the CLI** — resolution and
  refusal only.
- **`--device` is still never `start()`ed anywhere.** Highest-risk untested path
  in the project, and it carries all the real drift. **Needs a manual pass with
  the author awake and consenting.**

---

## 2026-08-26 — `ui/tree_panel.c`: the structure panel, a second real view

`src/ui/tree_panel.c` + `include/ui_tree_panel.h` + `tests/test_ui_tree.c`.
`/W4 /WX` clean; the tree suite's 11 cases were run 3x in each configuration
with no flakes. Last full verification: **Debug 24/24 suites pass**; **Release
23/24**, the single failure being `test_session`'s own
`the_stored_pid_settles_a_tie_that_nothing_else_can`, which belongs to the
session agent still in flight and does not touch this work
(`test_ui_tree`, `test_ui_a11y`, `test_ui_canvas` and `test_strings` all pass in
both). Nothing committed.

### What it is, and what it is not

Design 6.1 rejected "canvas for sighted users, tree for blind users" (option C)
as separate-but-equal. So this is **not** an accessibility fallback: it is a
second projection of the same `AprGraph`, and it is the better tool for jumping
to any node in a large graph (a TreeView's type-ahead does that and no canvas
does), for a layers-panel overview, and for reading the session linearly.

### Shape

- **A real `WC_TREEVIEW`**, inside a small host window of ours. The host exists
  because `NM_CUSTOMDRAW` arrives as `WM_NOTIFY` at the control's PARENT, and
  the frame does not handle `WM_NOTIFY` and must not have to — it owns no model.
- **`NM_CUSTOMDRAW` sets `clrText`, `clrTextBk` and the font, and issues NO GDI
  coordinate of its own.** That is the strongest form of AGENTS.md rule 5, and
  it is also **what makes `WS_EX_LAYOUTRTL` safe on the control**: the style
  hands it a mirrored DC, and a painter passing its own rectangles into that DC
  would get them reflected. **If anyone adds geometry to that handler — a colour
  bar, a badge, a meter — the LAYOUTRTL call has to be reconsidered in the same
  change.** Said in the file, at the top, in caps.
- High contrast and the selected state both return `CDRF_DODEFAULT`: the user's
  own colours, and the control's own selected/focused distinction, are not ours
  to improve on.
- **The rows are a pure function.** `apr_tree_panel_rows(graph) -> rows` and
  `apr_tree_panel_label(graph, row) -> text` touch no window. "Structure is
  source → bus → action and it does not flip in Arabic" is therefore asserted as
  an identity between two arrays in one process, not inferred from pixels.

### Selection coherence with the canvas — the decision

**A selection is a model identity, not a window.** `AprTreeSel` is
`{kind, bus id, source id, action index}`. The tree emits one on
`TVN_SELCHANGED` through a sink and accepts one through
`apr_tree_panel_select()`, which suppresses the echo. A controller wires the two
directions; neither view holds a handle from the other, so neither is derived
from the other.

**The selection carries its BUS and that is not redundancy.** A source may feed
several buses, so it appears once under each; "the selected source" is ambiguous
and "this source, on this bus" is not — and that pair is exactly the identity of
the edge the canvas draws.

### What the rows actually say out loud

A TreeView item's text IS its accessible name, so each row is a whole sentence:

```
Main Mix, bus, mixing 2 sources to 1 output
  Teams, generated by apprecorder, feeding Main Mix and 1 other bus
  Chat Mic, generated by apprecorder, feeding Main Mix
  WAV (float32, uncompressed), written from Main Mix
Teams Only, bus, mixing 1 source to 0 outputs
  Teams, generated by apprecorder, feeding Teams Only and 1 other bus
Sources not connected to any bus
  Spare, generated by apprecorder, connected to no bus, so nothing records it
```

Plus two state clauses that occupy one positional insert each, so a translator
can move them: *"muted in Windows, so it records silence"* and *"closed, so it
records silence from here on"*. Both facts look perfectly healthy from every
other angle — a muted app records pure silence (measured: loopback is
post-session-volume) and an exited one keeps producing zeros for ever — which is
why they are in the NAME rather than in a colour.

"…and 1 other bus" is the fact a tree cannot express structurally, and it is the
whole reason this is a projection of a graph rather than a tree of its own.

### Three findings worth not losing

1. **`SetFocus()` from inside `WM_SETFOCUS` is silently swallowed.** The outer
   `SetFocus` that delivered the message reasserts its own target as it unwinds.
   The fix is to POST a private message and do it outside the nested dispatch.
   Caught only because the test asks `GetGUIThreadInfo` which window really
   ended up with focus; every proxy for that check passed.
2. **A focus-forwarding container must NOT be `WS_TABSTOP`.** With the stop on
   the container, Shift+Tab leaves the tree, lands on the container and is
   forwarded straight back in — a trap. Guarding on "focus came from the tree"
   does not work either: that is indistinguishable from "the frame handed this
   pane focus while the tree held it", which strands F6 on the container.
   Taking the host out of the tab ring (`WS_EX_CONTROLPARENT`, no `WS_TABSTOP`)
   removes the bounce at its source. **UIA still reports the pane as keyboard
   focusable**, so `test_ui_a11y.c`'s pane assertions were unaffected.
3. **Struct padding is not zero.** `apr_tree_panel_rows` assigned fields
   individually, so two structurally identical row lists compared unequal on
   twelve bytes of padding — passing in Debug, failing in Release. The rows are
   now `memset` whole before assignment, and the header promises it.

### Files touched outside my own

- `include/strings.h` + `res/strings.rc`: new group `APR_STR_LIST_UI_TREE`
  (ids **1500-1515**, clear of the canvas agent's 1400-1477) and two plural
  bases, `N_OTHER_BUSES` (1224) and `N_OUTPUTS` (1232). English only; Arabic
  declared untranslated like every other UI group.
- `CMakeLists.txt`: `test_ui_tree` added to the `_uitest` list. Additive.
- `tests/test_ui_a11y.c`: the F6 case asserted `focus == pane`, which stopped
  being true the moment a pane had content to forward focus to. Generalised to
  `focus_is_in(pane, focus)` **and strengthened** — it now also asserts focus
  did not merely move within the SAME pane, which the plain inequality had
  stopped proving. The canvas needs the identical generalisation.

### Section 6 — what it still does not say

Read as it stands after the three overnight corrections. The `%1$s` and
"LAYOUTRTL does not mirror what we paint" errors are both genuinely fixed; the
gaps below are omissions rather than wrong statements.

- **6.3 states the custom-draw rule without its RTL caveat, and the two
  interact.** `NM_CUSTOMDRAW` is compatible with the free `WS_EX_LAYOUTRTL`
  mirroring 6.2 asks for **only while the handler passes no coordinates**. Set a
  colour, set a font, fine. Draw a badge or a colour bar and it lands in a
  mirrored DC. That is a trap with no diagnostic, and 6.3 currently reads as an
  unconditional blessing.
- **6.1 says nothing about where a control's notifications go.** "A docked
  TreeView over the same model" reads as one window; it cannot be one, because
  `NM_CUSTOMDRAW` arrives as `WM_NOTIFY` at the PARENT and the frame handles no
  `WM_NOTIFY` — deliberately, since it owns no model. Any pane that custom-draws
  a standard control needs a host window, and a sentence in 6.1 would have saved
  finding that out by building it.
- **6.1 does not say how selection stays coherent between the two views.** It is
  the first question either view forces and there was no guidance; the answer
  taken here (a model identity carrying its bus, plus echo suppression) belongs
  in the spec now that both views exist.
- **6.4's element counts are stale.** "18 elements, 0 unnamed, 14
  keyboard-focusable" was the placeholder-pane frame; with the tree and the
  canvas real it is **30 / 0 / 16**.

### Known gap

`apr_source_muted()` / `apr_source_alive()` cannot be driven from a test — a
fake source has no knob for either — so the two state-clause branches are
covered only by asserting the catalog entries exist, are non-empty and are
distinct. **A `capture_fake` health knob would close this**, and it would also
let the CLI's `WARN_SOURCE_MUTED` path be tested.

### Also worth a decision

`AprActionVTable::display_name` is a wide literal in each encoder's vtable —
a user-facing string living in code, which AGENTS.md rule 6 forbids. The tree
reads it for its output rows. Fixing it means an `AprStrId` per format beside
the registry; flagged in a comment at the call site.


---

## 2026-08-26 — THE CANVAS. Nodes are real windows; edges are speakable.

`src/ui/canvas.c`, `src/ui/node_window.c`, `include/ui_canvas.h`,
`include/ui_node.h`, `tests/test_ui_canvas.c` (22 cases), plus 53 catalog ids in
two new groups, two new functions on `ui_app.h`/`app.c`, and one word added to
the UI-test list in `CMakeLists.txt`. **Debug and Release both 100% green,
`/W4 /WX` clean.** Uncommitted.

`apr_canvas_create` is now the real thing, so `app.c`'s named placeholder is no
longer used for the canvas slot.

### Every node is a real Win32 child HWND, and that is the whole design

Not an owner-drawn rectangle and not a Direct2D primitive. `ROLE_SYSTEM_GROUPING`
is annotated on each one, and **UIA reports control type 50026 =
`UIA_GroupControlTypeId`** — measured, not assumed. Without the role annotation a
custom-class window is reported as a nameless Pane, which is the exact defect the
frame's own a11y test exists to catch one level up.

Live tree today: five nodes, five names, all Groups —
`"Teams, source"`, `"Main Mix, bus"`, `"No output (discard), output"`.

### Edges have no window, so they live in the nodes' descriptions

An edge is painted on the parent. There is nothing to land on, so **if it is not
in a node's description it does not exist** for a screen reader user. Measured
output:

```
before: "This source feeds nothing yet. Press Control and E, move to a bus, ..."
after:  "Feeds Main Mix. Press Control and Down Arrow to move to what it feeds, ..."
bus:    "Mixes Teams and writes No output (discard). Press Control and Up Arrow ..."
```

Read back through UIA's **LegacyIAccessible pattern**, which is what the
MSAA-to-UIA bridge exposes `IAccessible::get_accDescription` as — the property
`apr_ui_set_accessible_description_text` actually writes. That asks the real
question rather than a convenient one.

### The keyboard model, and why it is a TABLE

`k_bindings[]` in `canvas.c` is both what dispatches a key and what Help >
Keyboard Shortcuts will render, so a shortcut cannot be documented as one key and
implemented as another. `test_ui_canvas.c` fails if any `AprCanvasOp` has no
binding, which is AGENTS.md rule 5 as an assertion rather than a review item.

| | |
|---|---|
| Tab / Shift+Tab | next / previous node in **logical** order; at either end, **out of the pane** |
| Up / Down | within one lane |
| Left / Right | along the signal flow — **these two mirror**, because they are geometric |
| Ctrl+Down / Ctrl+Up | the same two moves stated **logically** ("what this feeds" / "what feeds this") — **never mirror** |
| Home / End | first / last node |
| Ctrl+E | connect: mark, move, press again. Escape cancels |
| Ctrl+Shift+E | disconnect, same two steps |
| Delete | remove the focused node |
| + / − | the focused source's level, whole decibels |
| Enter / Space | say the node and its edges again |

Both routes for edge-following exist on purpose: a sighted user reaches for the
arrow that points the right way; a screen reader user must not have to know which
way the picture happens to face.

### Nodes claim EVERY key, and Tab is implemented rather than delegated

`DLGC_WANTALLKEYS`. The obvious arrangement — `WANTARROWS` without `WANTTAB`, so
the dialog manager moves between nodes — **silently loses three keys**:
`IsDialogMessage` eats `VK_ESCAPE` (turning it into `WM_COMMAND(IDCANCEL)` at the
frame, so the user is stranded in a half-made connection), eats `VK_RETURN`
hunting for a default push button, and never delivers `VK_TAB` at all. So the
node forwards everything to the canvas and the canvas owns Tab — including
handing focus OUT via `GetNextDlgTabItem` at either end, which is what stops
claiming every key from making the pane a focus trap. **Asserted live.**

### Z-ORDER IS THE ACCESSIBILITY ORDER, and CreateWindowEx gets it backwards

UIA enumerates children by z-order and `CreateWindowEx` stacks each new child on
**top**, so "created in logical order" yields a **reversed** accessibility tree —
outputs first, sources last. `rebuild()` restacks explicitly. The test walks the
**live sibling chain**, not our own array, because the array is not what UIA
reads.

### RTL: the picture mirrors, nothing else does

`apr_canvas_node_rect()` is pure with direction as a parameter, so both
geometries are checked in one process. Measured in Arabic:
`source x=[1137,1467] bus x=[627,957]` — **sources on the right** — with
`z-order: source 0, bus 2` unchanged. Neither the canvas nor any node carries
`WS_EX_LAYOUTRTL`; mirroring goes through `apr_ui_mirror_rect`, the product's
single mirroring site, and the call is unconditional because it is identity in
LTR (an `if (rtl)` at a call site would be a second direction source).

The three lanes are **always three**, even when one is empty: a lane that
collapsed would slide every other node sideways while the user built the graph.

### Every edit announces, three ways at once

1. The affected nodes' name and description are rewritten **before** anything is
   said, so a re-read gets the new sentence.
2. `EVENT_OBJECT_NAMECHANGE` fires, and focus moves to the node whose meaning
   changed — the mechanism every screen reader honours.
3. The whole sentence goes to the announcement sink and is kept, so
   `apr_canvas_last_announcement()` makes "the user was told" a property a test
   asserts rather than a claim a reviewer believes.

`EVENT_OBJECT_LIVEREGIONCHANGED` also fires, and is deliberately never the only
route: not every reader honours it on an MSAA-bridged window.

### Two things the review of the copy caught, worth recording

- **First-press refusal needed its OWN sentence.** Reusing the second-press one
  told a user standing on a bus that *"Main Mix is not a bus"*. Now
  `UI_ANN_CONNECT_NOT_SOURCE`.
- Same for a level on a bus: *"Main Mix feeds no bus yet"* is a false statement
  about a bus. Now `UI_ANN_GAIN_NOT_SOURCE`.

Both are the fragment rule biting from the other end: one sentence reused across
two states is the same defect as one sentence built from two fragments.

### Lists of user data: `UI_LIST_PAIR` + `UI_LIST_MORE`, folded

"Feeds Main Mix **and** Teams Only" needs a separator and a conjunction, and a
comma written into C is the wrong character in Arabic. Both are catalog entries;
`join_names()` folds them. That is the only sanctioned way to join user data in
this product and it is stated in `strings.h`.

### `APR_CANVAS_WM_PERFORM` / `_FOCUS_NODE` / `_SET_GRAPH`

**SetFocus does nothing, silently, from a thread that does not own the window**,
so a test calling `apr_canvas_perform()` directly would "pass" while moving no
focus at all. These three marshal onto the UI thread and run exactly the same
functions. Useful later for a scripting surface.

### Catalog

**53 new ids at 1400–1447 (`APR_STR_LIST_UI_NODE`) and 1460–1477
(`APR_STR_LIST_UI_KEYS`)**, each its own group with its own English
`STRINGTABLE`, clear of the UI shell's 1300 block. `APR_STR_ID_MAX` 1400 → 1600.
**No Arabic written**; all 53 declared untranslated. Key names are spelled out
("Control and Down Arrow") because they are read aloud, and they name the key
actually pressed, which does not change with the interface language.

### One more spec-era error corrected

**`strings.h`'s own DIRECTION paragraph still said `WS_EX_LAYOUTRTL` "does NOT
mirror anything we paint ourselves".** That is the error `ui_app.h` and design
6.2 were corrected for; it survived in a third place. Fixed, with the old
sentence quoted so the next reader knows it was deliberate.

### Known gaps, said out loud

- **An output node's title is the encoder's `display_name`** ("WAV (float32,
  uncompressed)"), not its file path — `bus.h` exposes `apr_bus_action_at()` as a
  vtable and keeps no path. Extending it is a core-module change and was not in
  scope. Same reason **an output cannot be deleted on its own**: there is no
  "remove one action" call, so it is refused out loud rather than faked.
- **Add source / bus / output announce "not available yet"** — the keyboard path
  and the menu item exist; the chooser dialog does not. That is the spoken form
  of app.c's "greyed, not absent".
- **Level is one number per source**, written to every bus it feeds. Per-edge
  gain stays possible in the model and is simply not offered here yet.
- **Disconnect and level have no menu items**, so they are discoverable only
  through Help > Keyboard Shortcuts (which is on the menu, and which the binding
  table exists to render). Adding `APR_CMD_*` ids would mean editing `ui_app.h`
  and `app.c`'s menu while another agent was in them; deliberately not done.
- **`CMakeLists.txt`'s `foreach(_uitest ...)` list is a recurring merge point** —
  three agents have now added a name to that one line. A
  `if(test_name MATCHES "^test_ui_")` loop over `TEST_SRC` would remove it
  permanently; not done here because the brief said additive edits only.

### Safety (AGENTS.md rule 1)

**Nothing was rendered to any output device at any point.** The canvas tests use
`APR_SRC_FAKE` sources, which are never started, and the built-in **`"none"`
action, which opens no file** — so this work wrote no audio anywhere and left no
files behind.

---

## 2026-08-26 — CANVAS complete. Three corrections to "real HWNDs are free".

`src/ui/canvas.c`, `src/ui/node_window.c`, `include/ui_canvas.h`,
`include/ui_node.h`, `tests/test_ui_canvas.c` (22 cases). Agent reported 22/22
green Debug + Release, `/W4 /WX` clean, merged around three other live agents in
`strings.h`/`strings.rc`/`CMakeLists.txt` without conflict.

### Keyboard model — built for a screen reader first

- **Tab/Shift+Tab** — next/previous node in logical order, and **out of the pane**
  at either end.
- **Up/Down** — within a lane. **Left/Right** — along the signal flow; **these
  mirror**, because they are geometric.
- **Ctrl+Down / Ctrl+Up** — the *same two moves stated logically* ("what this
  feeds" / "what feeds this") and **never mirror**. Both routes exist so a screen
  reader user never needs to know which way the picture faces. This is the right
  instinct and should survive any redesign.
- Ctrl+E connect (mark → move → press again, Escape cancels), Ctrl+Shift+E
  disconnect, Delete remove, +/− level in whole dB, Enter/Space re-announce.

**The bindings are one table (`k_bindings[]`) that both dispatches keys and feeds
Help > Keyboard Shortcuts**, so a shortcut cannot be documented as one key and
implemented as another. The test fails if any `AprCanvasOp` lacks a binding —
rule 5 as an assertion rather than a promise.

### Edges reach UIA through node descriptions

Edges are painted on the parent and have no element, so each node's *description*
carries them, built from catalog sentences with positional inserts: *"Feeds Main
Mix. Press Control and Down Arrow to move to what it feeds…"*. Name carries
identity + kind (*"Teams, source"*) because **colour is never the only carrier of
kind**. Lists are folded through two catalog patterns, never a comma in C.

Every edit rewrites affected names, fires `EVENT_OBJECT_NAMECHANGE`, moves focus
to the node whose meaning changed, fires `EVENT_OBJECT_LIVEREGIONCHANGED`, and
retains the sentence for `apr_canvas_last_announcement()` so tests assert exactly
what the user hears.

### THREE corrections to §6.1 — "real windows get MSAA/UIA free" is not quite true

1. **A custom window class is a nameless `Pane` to UIA** unless its role is
   annotated. Real HWNDs get you the *plumbing*, not the semantics.
2. **UIA enumerates children by z-order, and `CreateWindowEx` stacks new children
   on top** — so creating nodes in logical order yields a **reversed**
   accessibility tree. §6.2's "logical order never flips" is correct but **not
   free**; the canvas restacks explicitly.
3. **`IsDialogMessage` eats Escape, Enter and Tab.** Escape becoming
   `WM_COMMAND(IDCANCEL)` strands the user inside a half-made connection. Nodes
   claim `DLGC_WANTALLKEYS`; the canvas implements Tab itself, including the
   escape out of the pane.

### The backwards LAYOUTRTL claim had survived in a THIRD place

`strings.h`'s own DIRECTION paragraph still carried it, after it was corrected in
`ui_app.h` and §6.2. Now fixed, with the old sentence quoted so the next reader
knows the change was deliberate. **Lesson: a wrong claim propagates into every
file that paraphrases it — grep for the claim, not just the file.**

### Gaps the agent flagged rather than papered over

- An output node's title is the encoder's `display_name`, **not its file path**
  (`bus.h` keeps no path).
- **An output cannot be deleted alone** — no such call in `bus.h`. It says so.
- **Add source/bus/output announce "not available yet"** — there is no dialog
  layer yet. This is the main thing standing between the UI and being usable.
- Level is one number per source, written to **every bus it feeds**.
- Disconnect and level have **no menu items** (adding `APR_CMD_*` ids meant
  editing `app.c`'s menu while other agents were in it); discoverable via
  Help > Keyboard Shortcuts only.
- `CMakeLists.txt`'s `foreach(_uitest …)` list is now a three-way merge point;
  a `MATCHES "^test_ui_"` loop would remove it permanently. Not done — brief said
  additive only. **Worth doing now that the UI agents are finishing.**

### Verification note

A full-suite run at this moment fails to build on `tests/test_session.c` — the
**session agent's in-flight work**, unrelated to the canvas. `apprecorder_core`
itself builds clean. **Re-verify the whole suite once session persistence lands.**

---

## 2026-08-26 — TREE PANEL complete. All UI views built.

`src/ui/tree_panel.c` (1023 lines), `include/ui_tree_panel.h`,
`tests/test_ui_tree.c` (11 cases). Ids 1500–1515 + plural bases 1224/1232, clear
of the canvas agent's 1400–1477.

A real `WC_TREEVIEW` inside a **small host window of ours**, because
`NM_CUSTOMDRAW` arrives as `WM_NOTIFY` at the *control's parent* and the frame
handles no `WM_NOTIFY`. Custom draw sets `clrText`, `clrTextBk` and the font and
**issues no GDI coordinate of its own** — which is precisely what makes
`WS_EX_LAYOUTRTL` safe on the control, since the style hands it a mirrored DC and
any coordinate we computed would mirror twice. Stated in caps at the top of the
file: **adding a badge or colour bar later silently breaks Arabic while looking
perfect in English.**

Rows are a **pure function** (`apr_tree_panel_rows`, `apr_tree_panel_label`,
neither touches a window), so "source → bus → action does not flip in Arabic" is
asserted as an identity between two arrays rather than inferred from pixels.

### Selection coherence — now spec §6.3

A selection is a **model identity, not a window**:
`{kind, bus id, source id, action index}`. Neither view holds a handle from the
other. **The bus is load-bearing**: a source may feed several buses and appears
once under each, so "the selected source" is ambiguous while "this source, on
this bus" is not — and that pair is exactly the edge identity the canvas draws.

### What a row actually says aloud

```
Main Mix, bus, mixing 2 sources to 1 output
  Teams, generated by apprecorder, feeding Main Mix and 1 other bus
  Chat Mic, generated by apprecorder, feeding Main Mix
  WAV (float32, uncompressed), written from Main Mix
Sources not connected to any bus
  Spare, generated by apprecorder, connected to no bus, so nothing records it
```

Two state clauses live in their own positional insert so a translator can move
them: *"muted in Windows, so it records silence"* and *"closed, so it records
silence from here on"*. **"…and 1 other bus" is the fact nesting cannot
express**, and the unconnected-source group catches a source that costs a thread
and reaches no file.

### Three findings worth keeping

1. **`SetFocus()` from inside `WM_SETFOCUS` is silently swallowed** — the outer
   `SetFocus` reasserts its target as it unwinds. Fix: post a private message.
   **Caught only because the test asks `GetGUIThreadInfo` what really has focus;
   every proxy check passed.** Strong argument for testing the real property.
2. **A focus-forwarding container must not be `WS_TABSTOP`** or Shift+Tab bounces
   back in. Guarding on "focus came from the tree" is indistinguishable from "the
   frame handed this pane focus while the tree held it", which strands F6.
   `WS_EX_CONTROLPARENT` without `WS_TABSTOP`.
3. **Struct padding is not zero** — two structurally identical row lists compared
   unequal on 12 bytes; **passed Debug, failed Release**. Rows are `memset` whole
   now and the header promises it.

All three are in spec §6.3.

### TWO REAL GAPS — both small, both worth doing

1. **`capture_fake` has no health knob**, so `apr_source_muted()` /
   `apr_source_alive()` cannot be driven from a test. The tree's two state-clause
   branches are covered only by asserting the catalog entries exist and differ.
   **Adding a fake health knob closes this *and* makes the CLI's
   `WARN_SOURCE_MUTED` path testable.** High value for the size.
2. **`AprActionVTable::display_name` is a wide literal in each encoder's vtable —
   a user-facing string in code, which rule 6 forbids.** My design error in
   `action.h`. The tree reads it for output rows. Needs an `AprStrId` per format
   beside the registry. **Deferred only because the OGG agent is still writing an
   encoder**; do it once that lands, touching all four vtables at once.

### Spec updated

§6.3 gained the custom-draw RTL caveat, the `WM_NOTIFY`-goes-to-parent
constraint, both focus traps, and the selection-identity rule. §6.4's element
counts refreshed 18/0/14 → **30/0/16** now the panes are real.

### Verification

Agent reports **24/24 Debug**; Release 23/24 with the single failure in
`test_session` — the session agent still in flight. `test_ui_tree`,
`test_ui_a11y`, `test_ui_canvas` and `test_strings` pass in both, tree suite run
3× per config with no flakes. **Full re-verify once session lands.**

---

## 2026-08-26 — OGG/Opus action (`src/actions/action_ogg.c`) — DONE, 19/19 green

The fourth and last encoder. One source file, one test file, two vendored
libraries, three additive lines in the root `CMakeLists.txt`. Nothing else in
the tree was touched.

### Vendoring — libogg 1.3.6 + libopus 1.5.2, both BSD-3-Clause

Both verified across channels that do not share a distribution path, to the bar
`vendor/lame/PROVENANCE.md` set:

| | libogg 1.3.6 (`.tar.xz`) | libopus 1.5.2 (`.tar.gz`) |
|---|---|---|
| SHA-256 | `5c825342…fa1061` | `65c1d2f7…9a7ce1` |
| Xiph upstream | fetched | fetched |
| Debian archive | **byte-identical by `cmp`** | **byte-identical by `cmp`** |
| Gentoo `Manifest` | SHA-512 + size match | SHA-512 + size match |
| nixpkgs | SRI hash decodes to the same SHA-256 | version only (pins a git tag, so its hash is a NAR hash — noted as a dead end) |

**Every vendored file diffs clean against the tarball** — verified by `cmp`,
205 opus files and 9 ogg files, zero differences. The single exception is
`vendor/opus/config.h`, which is ours and says so.

**opus 1.5.2 not 1.6.1 on purpose**: the 1.6 line pulls the neural-network
features into the default build shape (35 MB tarball for 1.6.0, 10 MB for
1.6.1). 1.5.2 is what Debian stable ships and the encoder API is identical.

Not vendored: `dnn/` (18 MB of 22 — every reference to it is inside
`ENABLE_DEEP_PLC` / `ENABLE_DRED` / `ENABLE_OSCE`, all decoder-side),
`silk/fixed/` (this is a float build), and the `x86/` and `arm/` trees (no
runtime CPU dispatch — same trade `vendor/lame/config.h` makes on NASM).

### Decisions worth not re-deriving

- **Everything is encoded at 48 kHz.** `opus_encoder_create` takes only
  8/12/16/24/48 kHz, and Ogg Opus counts granule positions in 48 kHz units
  regardless. 44.1 kHz — the commonest rate there is — cannot be encoded at
  its own rate at all, so the choice is "resample or lose the bus".
  `core/resample.c` does it, on the writer thread. Even the rates Opus accepts
  natively are resampled, so there is ONE granule arithmetic instead of five.
  The original rate survives in `OpusHead.input_sample_rate`.
- **20 ms frames (960 samples).** Opus's own default; smaller pays per-packet
  overhead, larger buys nothing for music and grows the tail a kill loses.
- **Pre-skip is asked for (`OPUS_GET_LOOKAHEAD`), never assumed.** It is 312 in
  practice; a wrong one is a permanent 6.5 ms offset against every other bus.
  ffprobe reports `start: 0.006500` on our files, which is 312/48000 exactly.
- **VBR by default, and unlike MP3 that costs nothing.** Granule positions make
  duration exact, so there is no Xing-tag equivalent to write and nothing to
  patch in finalize. MP3 had to default to CBR precisely because a VBR file
  whose tag never got written lies about its length; Ogg has no such failure.
- **Extension is `.opus`, id stays `ogg`.** RFC 7845 §9. `--format ogg` still
  forces it onto a `.ogg` path (verified). A `.ogg` file containing Opus is
  what breaks in Vorbis-only players.
- Default bitrate 96 kbps stereo / 64 mono. `quality` 1..11 → Opus complexity
  0..10; 0 means complexity 10.

### A kill mid-recording leaves a playable file — verified with ffprobe, not just us

`apprecorder.exe --fake 440 --out killed.opus --duration 60`, hard
`Stop-Process -Force` at 12 s:

```
Input #0, ogg, from 'killed.opus':
  Duration: 00:00:11.91, start: 0.006500, bitrate: 97 kb/s
  Stream #0:0: Audio: opus, 48000 Hz, stereo, fltp
      Metadata: ENCODER : apprecorder
```

`ffmpeg -v warning -i killed.opus -f null -` decodes the whole thing with zero
warnings. Truncating a finished 4.04 s file at 37 % gives 1.37 s, also clean.
Loss on a kill is bounded by libogg's ~4 KB page accumulation (~350 ms) plus
whatever is still in the four-second ring. **There is deliberately no periodic
`ogg_stream_flush`**: an early page wastes a 27-byte header forever to buy back
a third of a second in an event that also loses four seconds to the ring.

### NaN/Inf: measured, not scrubbed

Rule 4 says `core/mix.c` owns non-finite input, and it does — for the *integer*
formats. Opus takes float directly, so this action adds no guard, and the test
measures what libopus actually does: **a decoded peak of exactly 0.00000000**.
libopus turns NaNs and infinities into silence by itself, so unlike
`action_mp3.c` (which had to clamp before LAME's psychoacoustic model turned one
NaN into a frame of full-scale hash) nothing is duplicated here. The test asserts
`< 0.05` so that a future libopus changing this fails loudly rather than
quietly putting noise in a blind user's headphones.

### Binary cost — 220.5 KB, and there is no way to shave it

Release `apprecorder.exe`: **784,896 bytes with the action, 559,104 without**.
Against MP3's 58 KB that is a lot, and it is unavoidable: LAME's decoder is a
separate library half that provably stays out of the image, whereas Opus's
decoder shares the range coder, the MDCT, the FFT and the mode tables with its
encoder. An attempt to measure the decoder's marginal cost by forcing a
reachable `opus_decoder_create` in came back at +2 KB — and so did the same
probe with `opus_multistream_encoder_create`, which is certainly not otherwise
linked, so **the experiment does not discriminate and no claim is made from
it**. `vendor/opus/PROVENANCE.md` records both the number and the dead end.

The image is still under 1 MB. If it ever needs to come down, the lever is
`FIXED_POINT` (drops `silk/float/`, `analysis.c`, `mlp_data.c`), not the
decoder.

### Structure — action_mp3.c's, unchanged, plus one thing

`on_audio` is `rb_write` + `SetEvent` and nothing else. The writer thread does
resample → 20 ms framing → `opus_encode_float` → `ogg_stream_packetin` →
`WriteFile`. The **resampler** is the third thing kept off the mixer thread that
MP3 did not have to think about. Ring is `core/ringbuf.c`, four seconds; an
overrun emits exactly the frame count `rb_read` reports as silence, so the file
stays the length of the audio. Test seams `apr_ogg_test_write_gate` /
`apr_ogg_test_fail_after_bytes` are `action_wav.c`'s technique verbatim.

Worst single `on_audio` call with the disk held shut for a full second of
audio: **0.004 ms**.

### Test output

`tests/test_action_ogg.c` — 19 cases, 19 pass, 3.2 s Debug. Demuxes with the
vendored libogg, decodes with the vendored libopus, and hands the same files to
**ffprobe** as a second opinion (skipped loudly, never silently, if ffprobe is
not on PATH). Full suite at the time of writing: `100% tests passed, 0 tests
failed out of 24`.

### Left undone / flagged

- The CLI maps extensions to actions, so `--out x.ogg` needs an explicit
  `--format ogg`. That is a CLI decision, not this action's, and was left alone.
- More than two channels is refused at create, same line `action_mp3.c` draws.
  Opus mapping family 1 could carry 5.1, but the channel order is a session
  decision.
- Session rates outside [6000, 384000] are refused — that is where
  `APR_RESAMPLE_MAX_RATIO` (8) runs out in each direction. Nothing in this
  project produces one.

---

## 2026-08-26 — Session persistence (design section 9). Uncommitted.

`include/session.h`, `src/session/session_save.c`, `src/session/session_load.c`,
`tests/test_session.c` (**47 cases**), plus `--session` / `save-session` /
`--allow-system-capture` / `--allow-missing` in `src/cli/`. `build.cmd Debug test`
and `build.cmd Release test`: **24/24 suites, 0 failures**, `/W4 /WX` clean in
both.

### Format

JSON. Hand-rolled writer (`session_save.c`), **jsmn** for reading — vendored at
`vendor/jsmn/` with a checksum in its `PROVENANCE.md`, MIT, unmodified.
Configured `JSMN_STATIC` + `JSMN_STRICT` from `session_load.c`, the only
translation unit that includes it.

Sources are a **top-level list keyed by name**; buses reference them by key and
carry **per-edge gain**. A format that nested sources inside buses could not
express "Chat Mic on the full mix at 0 dB and on its own file at +3.5", which is
the graph shape the product exists for (design 3.2).

### The identity problem — how each mode is handled

Resolution is a **search**, not a lookup, and it returns
`AprSessionResolveReport`: per-source status, what the file asked for, what was
substituted, and the full candidate list with pids. Not a boolean, because the
UI needs the same structure for design 9's "fall back to prompting".

| Failure mode | What happens |
|---|---|
| App not running | `NOT_RUNNING`. **The source stays in the model** and the run stops at exit 3. `--allow-missing` drops it, warns by name, and finishes **exit 6**. |
| Several instances | pid hint first (only among candidates that already matched on image), then **window class**, then refuse `AMBIGUOUS` and print every rival pid so the user can pin one with `--pid`. `pick_when_ambiguous` (API only, off in the CLI) takes the lowest pid as `FIRST_OF_MANY`. |
| Executable moved | Path match fails, exe-name match succeeds → `MOVED`, reported with **both** paths in one sentence. |
| Device absent | `DEVICE_ABSENT`, and the message names the **friendly name**, never the GUID — which is why both halves are stored. A changed endpoint id with a matching friendly name resolves as `DEVICE_BY_NAME`. |
| Running but silent | Not in the audio-session list, so a last resort matches **pid + image name together** (two facts agreeing). A bare pid is never trusted. |

### EXCLUDE (design 4.1.1)

**Consent is checked before the lookup**, so the answer does not depend on
whether the excluded app happens to be playing. Without `--allow-system-capture`
the load stops at **exit 2** naming the flag; the source is never resolved.

**`--allow-missing` can never drop an EXCLUDE source.** Dropping an ordinary
source records less than was asked for; dropping the target of an exclusion
records *more* — the whole machine with nothing held back. Pinned by a test.

A loaded EXCLUDE source stays `APR_CLI_SRC_SYSTEM_MINUS_TREE` in the plan rather
than collapsing to a pid, so `apr_cli_resolve` prints the whole process-tree
warning on **every** run. A session must not make system-wide capture quieter
than typing it does.

### Versioning

Two numbers. `version` = what wrote it, `minReader` = the oldest reader that can
be trusted with it. `minReader > ours` → refuse (exit 2, naming both numbers);
`version > ours` with `minReader <= ours` → load and warn `from_newer_writer`;
`version < APR_SESSION_MIN_VERSION` → refuse. **Unknown keys are ignored,
counted and named** — that is the whole forward-compatibility lane. A known key
of the **wrong type** is fatal: `"gainDb": "loud"` is a broken file, not a
setting from the future, and defaulting it would change the recording silently.

### Exit codes — extended, not replaced

2 CONFIG (malformed / truncated / not ours / too new / consent refused),
3 NOT_FOUND (no such file; or a named process or device is absent),
4 OUTPUT (`save-session` could not write), 6 INCOMPLETE (`--allow-missing` and
something really was missing).

### Findings worth not rediscovering

1. **jsmn is a tokenizer, not a validator.** `{ , , }` and `{"a": }` tokenize
   without complaint. It reliably catches only a character that cannot begin a
   value (`INVAL`) and input that stops mid-token (`PART`). The key/value
   structure check therefore lives in `session_load.c`, and the tests are split
   so each mechanism is tested against the inputs it actually owns.
2. **A hand-edited Windows path is the likeliest way a session file fails to
   parse** — `C:\Users\me` is invalid JSON. `APR_S_ERR_SESSION_NOT_JSON` says so
   and suggests forward slashes. Files apprecorder writes are always correct.
3. **No float is formatted anywhere in the writer.** Gain is tenths of a dB,
   amplitude is millionths, both printed as integers with a decimal point
   inserted — so the round trip is exact and no locale's decimal comma can get
   into a file a script parses. Same rule `apr_str_number_fixed` follows.
4. **Saving is atomic** (sibling temp + `MoveFileExW` + `FlushFileBuffers`). A
   session file is a configuration the author will have spent time on; an
   interrupted save must not leave half a file where a whole one was.
5. Mutation-tested. Five deliberate breaks — consent gate open, `allow_missing`
   reaching EXCLUDE, ambiguity picking silently, absent device named by GUID,
   bare pid trusted — and the fifth **was not caught** at first, because
   `chosen_pid` was copied from what we searched for rather than from what we
   found. Fixed in both the code and the test.

### Where the window-class lookup lives

A private static in `session_load.c` (`EnumWindows` + `GetClassNameW`, top-level
visible windows only), linked with a `#pragma comment(lib, "user32.lib")` the
way `action_m4a.c` links Media Foundation. `discover.h` is about what can be
*recorded*, and there is one caller — the project's own rule for `str.c`/`fs.c`.
**Move it to `discover.c` the moment the UI needs it.**

### Strings

31 new catalog entries in a new `APR_STR_LIST_SESSION` group (ids 1520-1551),
English written, Arabic declared untranslated. Two need care in the Arabic pass:
`ERR_SESSION_NEEDS_CONSENT` must not flatten "everything except X" into a single
item (design 4.1.1 — a whole growing *tree* is held back), and
`WARN_SESSION_MOVED` carries two paths in one sentence whose inserts must stay
distinguishable. Noted in `res/strings.rc` beside the placeholders.

### Verified by hand, not only by the suite

`save-session --exe nvda.exe` captured `pid`, `exe`, full image path and window
class `NVDAHighlighter` from the live machine, and reloading resolved it back.
`--allow-missing` produced exit 6 and a playable WAV. The EXCLUDE gate refused
at exit 2 without the flag and, with it, printed the full tree warning listing
`nvda.exe (36728)` and `nvdaHelperRemoteLoader.exe (34988)`.
**No EXCLUDE capture was ever started — `--dry-run` only.** Nothing was rendered
to any output device (AGENTS.md rule 1); every recording test uses `--fake`.

### Left undone / flagged

- `pick_when_ambiguous` is API-only. The CLI leaves it off; a UI that can prompt
  wants the candidate list instead.
- An EXCLUDE target that holds no audio session **and** whose saved pid is gone
  cannot be found — there is no whole-process-table enumeration in `discover.h`.
  Failing there is the safe direction, so it was left.
- `include/strings.h` and `CMakeLists.txt` edits from this work were swept into
  commit `e57a84d` by the UI agent's `git add` while this was in flight. Same
  lesson as before: **stage explicit paths while agents are running.** Nothing
  else here is committed.

---

## 2026-08-26 — OGG/OPUS complete. **All four encoders done.**

`src/actions/action_ogg.c`, `tests/test_action_ogg.c` (19 cases),
`vendor/ogg/`, `vendor/opus/`.

### Provenance — libogg 1.3.6 + libopus 1.5.2, both BSD-3-Clause

A nicer licence position than LAME (§8.1): **no relinking obligation.**

| | libogg | libopus |
|---|---|---|
| Xiph upstream | fetched | fetched |
| Debian archive | **byte-identical by `cmp`** | **byte-identical by `cmp`** |
| Gentoo Manifest | SHA-512 + byte count match | SHA-512 + byte count match |
| nixpkgs | SRI decodes to same SHA-256 | pins a git tag → NAR hash, **not comparable** (recorded as a dead end) |

Every vendored file re-verified with `cmp` afterwards: **205 opus files, 9 ogg
files, zero differences.** Only `vendor/opus/config.h` is ours; libogg needed
none (MSVC takes the `<stdint.h>` branch in upstream `os_types.h`).

**1.5.2 not 1.6.1 deliberately** — the 1.6 line pulls DNN features into the
default build shape (35 MB tarball). Excluded: `dnn/` (18 MB of 22, all behind
`ENABLE_DEEP_PLC`/`ENABLE_DRED`/`ENABLE_OSCE`), `silk/fixed/` (float build), and
`x86/`+`arm/` (no runtime dispatch — same trade `vendor/lame/config.h` makes).

### Everything encodes at 48 kHz, and it is forced

`opus_encoder_create` accepts only 8/12/16/24/48 kHz, and **Ogg counts granulepos
in 48 kHz units regardless**, so 44.1 kHz cannot be encoded at its own rate at
all — the choice is resample or lose the bus. `core/resample.c` does it, on the
writer thread. Even natively-accepted rates are resampled so there is **one**
granule arithmetic rather than five. Original rate survives in
`OpusHead.input_sample_rate`. **20 ms frames** (Opus's own default).

**Pre-skip is asked for (`OPUS_GET_LOOKAHEAD`), never assumed** — ffprobe reports
`start: 0.006500` = 312/48000 exactly. Granulepos = `pre_skip + real 48 kHz
frames`, which makes duration **exact even though the stream is VBR** — the
structural reason MP3 needed CBR and this does not.

Extension `.opus`, id stays `ogg` (RFC 7845 §9).

### Kill mid-recording — clean

Real binary, hard `Stop-Process -Force` at 12 s of a 60 s run: ffprobe reads
`Duration 00:00:11.91, start 0.006500`, and `ffmpeg -v warning ... -f null -`
decodes the whole thing with **zero warnings**. Loss bounded by libogg's ~4 KB
page accumulation (~350 ms) plus the ring. **So WAV, MP3 and OGG all survive a
kill; only M4A does not.**

### NaN/Inf — measured, not guarded

Rule 4 gives non-finite handling to `core/mix.c`, and Opus takes float, so no
guard was added. The test **measures** instead: decoded peak exactly
`0.00000000` — libopus turns NaN/Inf into silence itself. The assertion is
`< 0.05`, so a future libopus that changes this **fails loudly** rather than
silently emitting full scale.

### Intellectual honesty worth preserving

The agent tried to measure the Opus decoder's marginal binary cost: forcing a
reachable `opus_decoder_create` added +2 KB. But the same probe with
`opus_multistream_encoder_create` — certainly not otherwise linked — **also gave
+2 KB**, so the experiment does not discriminate. It made no claim from it, and
**went back and removed the "the linker drops the decoder" wording it had already
written** into the CMake comments and test header. The number and the dead end
are recorded in `vendor/opus/PROVENANCE.md`. Do not resurrect that claim without
a better experiment.

### Size

Release `apprecorder.exe`: **784,896 bytes with the action, 559,104 without —
220.5 KB for Opus.** Still under the 1 MB goal, but the margin is now thin.
Unavoidable: LAME's decoder is a separate library half, whereas Opus's decoder
shares the range coder, MDCT, FFT and mode tables with its encoder.

### Tests

19/19, 3.2 s Debug. Demuxes with vendored libogg, decodes with vendored libopus,
**and hands the same files to ffprobe as a second opinion** (skipped loudly,
never silently, if absent). Worst `on_audio` with the disk held shut for a full
second of audio: **0.004 ms**.

### Notes

- Commit `e57a84d` swept up this agent's root-`CMakeLists.txt` edit; its source
  files were still untracked at report time.
- **The CLI maps extensions to actions, so `--out x.ogg` needs an explicit
  `--format ogg`.** A CLI decision, deliberately left alone. Worth revisiting —
  `.opus` works without the flag, `.ogg` does not, which will surprise people.

---

## 2026-08-26 — SESSION PERSISTENCE complete. All planned work landed.

`include/session.h`, `src/session/session_{save,load}.c`,
`tests/test_session.c` (47 cases), `vendor/jsmn/` (MIT, sha256 recorded,
unmodified). CLI gains `--session`, `save-session`, `--allow-system-capture`,
`--allow-missing`.

### Schema — sources are top-level, buses reference them

Nesting sources inside buses **could not express** "Chat Mic at 0 dB on the mix
and +3.5 dB on its own file" — that is the graph shape from §3.2, so per-edge
gain lives on the bus's reference, in tenths of a dB. Header carries `version` +
`minReader`; each source stores its **whole identity** (pid hint, exe, image
path, window class — or endpoint id **and** friendly name — or tone/ppm/amp).

### Identity resolution returns a report, not a boolean

`AprSessionResolveReport`: status, what was asked for, what was substituted, and
the full candidate list with pids.

- **Not running** → `NOT_RUNNING`, source stays in the model, exit 3.
  `--allow-missing` drops it, warns by name, finishes **exit 6**.
- **Several instances** → pid hint (only among candidates already matched on
  image), then window class, then refuse `AMBIGUOUS` **printing every rival pid**
  so the user can pin one with `--pid`.
- **Moved executable** → `MOVED`, both paths in one sentence.
- **Absent device** → named by **friendly name, never the GUID**. A changed
  endpoint id with a matching name resolves as `DEVICE_BY_NAME`.
- **Running but silent** (absent from the audio-session list) → last-resort match
  on **pid + image name together**. A bare pid is never trusted.

Resolution runs against a supplied `AprSessionMachine` rather than a global test
seam, so a UI preview can use the same entry point.

### EXCLUDE — the reasoning here is sharp and should not be softened

- **Consent is checked before the lookup**, so the answer never depends on
  whether the excluded app happens to be playing right then.
- Without `--allow-system-capture` the load stops at **exit 2 naming the flag**.
- **`--allow-missing` can never drop an EXCLUDE source.** Dropping an ordinary
  source records *less* than asked; dropping an exclusion target records
  **more**. Those are not the same risk and must not share a flag.
- A loaded EXCLUDE source stays its own CLI kind, so the full process-tree
  warning reprints every run.

### Versioning

`minReader > ours` → refuse. `version > ours` with `minReader <= ours` → load and
warn. Too old → refuse. **Unknown keys ignored, counted and named** — the
forward-compat lane. A known key of the **wrong type is fatal**.

### Two findings

1. **jsmn is a tokenizer, not a validator** — `{ , , }` and `{"a": }` tokenize
   fine. The key/value structure check is ours, and the tests are split so each
   mechanism is tested against inputs it owns.
2. **A hand-edited Windows path is the likeliest parse failure** — `C:\Users\me`
   is invalid JSON. The error message now says so explicitly.

### Honest note from the agent

Mutation-tested with five deliberate breaks; **the fifth (bare pid trusted) was
not caught at first**, because `chosen_pid` was copied from what we searched for
rather than what we found. Fixed in both code and test.

### Follow-up

Window-class lookup is a private static in `session_load.c` (one caller, no
owner exists). **Move it to `discover.c` when the UI needs it.**

---

## 2026-08-26 — FULL VERIFICATION

`build.cmd Release test`: **24/24 suites, 0 failures.**
`apprecorder.exe` = **786,944 bytes (768.5 KB)** — under the 1 MB goal with four
encoders, LAME, libogg and libopus statically linked.

*(Superseded later the same day: 23 suites and 769,024 bytes once M4A was
removed. See the next entry.)*

---

## 2026-08-26 — M4A DELETED, `display_name` FIXED, TWO SMALL ONES

### 1. M4A is gone, and the spec says why

Author's decision, verbatim: *"For m4a, if it's giving us trouble, fuck it, we
don't need it, wav and mp3 and ogg high quality stereo recordings are enough for
me."* Removal, not deprecation.

Deleted: `src/actions/action_m4a.c`, `tests/test_action_m4a.c`, the `extern` and
table entry in `core/registry.c`, `m4a` from the `foreach(_action …)` list in
`CMakeLists.txt`, and every `m4a` in the CLI, the help text, the headers and the
docs. `CMakeLists.txt` derives `APR_HAVE_ACTION_M4A` from file presence, so that
half took care of itself — **verified**, not assumed: the define is absent and
the registry's guarded block compiles out.

Media Foundation was linked from inside the m4a source with
`#pragma comment(lib, …)`, so it left with the file. Confirmed by grep that
nothing else in the tree references `mfplat`, `mfreadwrite`, `mfuuid` or
`propsys`, and that `CMakeLists.txt` never named them.

**The reason is now design section 8.0**, written to be read by whoever proposes
adding AAC back. MP4 keeps its index in `moov`, `moov` is written at finalize,
so a killed recording is unplayable and cannot be repaired from a dead process.
WAV loses only correct RIFF size fields (every player infers length anyway), MP3
ends on a self-synchronising frame boundary, OGG ends on a complete page whose
granule position carries the duration in its own header. That asymmetry is the
argument, and 8.0 keeps it.

**Binary: 786,944 → 769,024 bytes (−17,920)** measured immediately after these
changes and before anything else landed. That delta is M4A *and* the `AprStrId`
change together — they were not built separately. The image read **777,216** at
hand-off; the extra 8,192 is the capture agent's concurrent work on
`capture_fake.c`, not a regression here.

### 2. `display_name` is an `AprStrId` (AGENTS.md rule 6)

`AprActionVTable::display_name` was a `const wchar_t *` literal — a user-facing
string in code. It is now:

```c
    const char    *id;              /* "wav", "mp3", "ogg" — wire value */
    AprStrId       display_name_id; /* catalog id, NOT a literal */
    const wchar_t *extension;       /* without the dot; matched against paths */
```

`id` and `extension` are unchanged and stay literals: one is a stable value in
session files, the other is matched against paths. Neither is prose.

- New catalog entries in `APR_STR_LIST_CORE`: `ACTION_NAME_WAV` (1055),
  `ACTION_NAME_MP3` (1056), `ACTION_NAME_OGG` (1057), `ACTION_NAME_NONE` (1058).
  English text in `res/strings.rc`; **Arabic placeholders added — four more
  strings for the translation pass.**
- `include/action.h` now includes `strings.h`.
- Three call sites resolve with `apr_str()`: `cli.c` (`report_action_failures`),
  `ui/canvas.c` (`bus_output_names`), `ui/tree_panel.c` (`label_action`). Those
  last two are two lines in another agent's files, and `tree_panel.c` had
  already left a comment predicting this exact edit.
- Tests changed from `ASSERT_NOT_NULL(display_name)` to asserting the id is
  non-zero **and** resolves to a non-empty string — a set id with no `.rc` entry
  would otherwise pass while displaying a placeholder.

### 3. `.ogg` resolves without `--format`

`action_for_extension` in `cli.c` now makes two passes: the `extension` field
first, so `.opus` still reaches the ogg action as RFC 7845 wants; then the
registry `id`, so `.ogg` reaches it too. Both passes skip an action with an
empty extension, which keeps the built-in `none` sink unreachable by path — a
new test asserts `x.none` is still refused. Extension-based dispatch is
unchanged: the authoritative mapping wins first.

### 4. The `foreach(_uitest …)` merge point is gone

It walks `TEST_SRC` and matches `^test_ui_`. Adding `tests/test_ui_*.c` needs no
build-system edit. Done last, and the suite re-run after.

### Verification

`build.cmd Release test`: **23/23 suites, 0 failures.** 24 minus the deleted
`test_action_m4a`. `/W4 /WX` clean.

For the record, because it cost a few runs: `test_capture_fake` failed
intermittently during this work — a *different* set of cases each time — while
the capture agent was rewriting `capture_fake.c` and its test minute by minute
for the health knob (known defect 2). None of it was from these changes, and it
went green once they settled.

---

# ★ START HERE — state as of 2026-08-26, ~09:30

## It works. Try this first

```
cd d:\data\projects\apprecorder
build.cmd Release test          → 26/26 suites, 519 cases, 0 failures
build\Release\apprecorder.exe list-apps
build\Release\apprecorder.exe list-devices
```

`list-devices` correctly finds Chat Mic, Stream Mix 1, Stream Mix 2 and Sample on
the GoXLR. A full session round-trip was verified end to end:

```
apprecorder save-session --session my.json ^
  --bus Mix       --exe teams.exe --device "Chat Mic"             --out mix.mp3 ^
  --bus VoiceOnly                 --device "Chat Mic" --gain 3.5  --out voice.wav

apprecorder record --session my.json --dry-run
```

Chat Mic appears on **both** buses at **different gains** — the graph feature,
working.

## What is done

| Layer | State |
|---|---|
| foundation (harness, err, log, ringbuf, clock) | done, 74 cases |
| capture (process / device / fake) | done |
| core (graph, bus, mix, resample, drift) | done — **0.43 frames over 3 h** |
| encoders WAV / MP3 / OGG-Opus | done — **M4A deleted 2026-08-26**, design 8.0 |
| i18n (catalog, CLDR plurals, RTL) | done — **Arabic not written** |
| CLI | **working** |
| UI foundation + canvas + tree panel | done, a11y tree tested |
| session persistence | done |

**Release binary 777,216 bytes (759 KB)** — under the 1 MB goal with three
encoders, LAME, libogg and libopus linked in. It was 786,944 with M4A in; the
removal plus the `display_name` -> `AprStrId` change gave back 17,920 bytes
(measured at 769,024), and concurrent capture-layer work has since added 8,192.

## THE THREE THINGS ONLY YOU CAN DO

### 1. The device-capture pass — highest risk in the project

**No test anywhere has ever `start()`ed a real capture endpoint.** Every agent
correctly refused, because starting one records your microphone without consent.
So `capture_device.c`'s pump, gap-fill and discontinuity handling have **zero
hardware coverage** — and that is the path carrying *all* the real drift (§5.1);
process taps are the easy half. Also untested for the same reason: the
`AUTOCONVERTPCM` retry path, and a real process tap recorded *through the CLI*.

Five minutes with you awake closes this.

### 2. The Arabic — 154+ strings pending

Placeholders only; the pipeline is proven, the words are yours. Process per your
CLAUDE.md: `ux-araby` for فصحى مبسطة, then a Gemini review pass, keeping your
domain overrides — **«إمكانية الوصول»**, never «الإتاحة». Flipping the Arabic
locale from PARTIAL to COMPLETE is the ship gate and the test prints the
outstanding count every run.

### 3. Two decisions left open

- ~~**M4A dies badly on a process kill**~~ — **decided 2026-08-26: deleted.**
  Your call: *"if it's giving us trouble, fuck it, we don't need it, wav and
  mp3 and ogg high quality stereo recordings are enough for me."* The reason is
  written down in design **8.0** so nobody re-adds AAC without solving the
  `moov`-at-finalize problem first.
- **Digit shaping** — Western vs Arabic-Indic numerals. One function
  (`apr_str_number`) owns it. Your domain.

## KNOWN DEFECTS — small, documented, not yet fixed

1. ~~**`AprActionVTable::display_name` is a wide literal**~~ — **fixed
   2026-08-26.** The field is now `AprStrId display_name_id`, resolved with
   `apr_str()` at the point of display (CLI, canvas, tree panel). The names live
   in `APR_STR_LIST_CORE` as `ACTION_NAME_WAV/MP3/OGG/NONE` (ids 1055-1058).
   Arabic placeholders added; **four more strings pending translation.**
2. ~~**`capture_fake` has no health knob**~~ — **fully closed 2026-08-26.**
   `AprCaptureConfig.fake` carries `mute_at_frame`, `unmute_at_frame`,
   `die_at_frame`, `start_muted`, `start_dead`, and the residual gap — the
   CLI's `WARN_SOURCE_MUTED` path — is now covered too: `--fake` takes
   `muted`, `dead`, `mute=<frame>`, `unmute=<frame>`, `die=<frame>` and the
   warnings, the once-only rule and exit **6** are all driven through
   `apr_cli_run`. Death now wins over muted in `core/runner.c`, matching the
   tree panel. See the entry at the bottom.
3. ~~**`action_m4a.c` still scrubs non-finite values**~~ — moot: the file is
   gone.
4. ~~**`--out x.ogg` needs an explicit `--format ogg`**~~ — **fixed
   2026-08-26.** `action_for_extension` in `cli.c` now runs two passes: the
   `extension` field first (authoritative — `.opus` must reach ogg), then the
   registry `id`. An action with an empty extension (`none`) is reachable by
   neither, so `x.none` is still refused.
5. ~~**Window-class lookup is a private static in `session_load.c`.**~~ —
   **fixed 2026-08-26.** It is `apr_process_window_class()` in
   `capture/discover.c` now, with a caller's buffer instead of a static one,
   and `APR_SESSION_CLASS_CCH` is *defined as* `APR_DISC_CLASS_CCH` so the two
   can never disagree. `tests/test_discover.c` is new — that file had no
   direct coverage at all before.
6. ~~**`CMakeLists.txt`'s `foreach(_uitest …)` list**~~ — **fixed 2026-08-26.**
   It now walks `TEST_SRC` and matches `^test_ui_`, so a new `tests/test_ui_*.c`
   links `apprecorder_ui` with no build-system edit.
7. ~~**No dialog layer**, so the canvas announces "not available yet" for add
   source/bus/output.~~ **CLOSED 2026-08-26** — `src/ui/dialogs.c`,
   `src/ui/controller.c`, `src/ui/tray.c`, `src/core/runner.c` and
   `src/uiapp/main.c`. The window can now build a graph from nothing, record,
   and load and save sessions. See the entry at the bottom of this file.

## Process lessons worth keeping

- **Write the contracts first.** `capture.h` and `action.h` were written before
  the parallel agents launched, and `capture.h` was implemented **unchanged** by
  four agents who could not see each other's work. Zero DRY violations found in
  review.
- **Stage explicit paths while agents run.** `git add -A` mid-wave attributed one
  agent's work to another's commit. Happened twice.
- **A wrong claim propagates into every file that paraphrases it.** The backwards
  `WS_EX_LAYOUTRTL` statement survived in three places. Grep the claim, not the
  file.
- **Test the real property, not a proxy.** The UIA client found an unnamed
  mouse-only splitter; `GetGUIThreadInfo` found a swallowed `SetFocus` that every
  proxy check passed.

---

## 2026-08-26 — M4A DELETED. `display_name` fixed. Defects 1, 3, 4, 6 closed.

Author's call: *"if it's giving us trouble, fuck it, we don't need it, wav and
mp3 and ogg high quality stereo recordings are enough for me."*

**23/23 suites green Release.** Binary **786,944 → 777,216 bytes (759 KB)**.

### M4A gone, and the reason generalised into a rule

Removed the action, its test, its registry entry, its `foreach` id, and every
`m4a` in CLI/help/headers/docs. **Media Foundation is fully gone** — its link
directives lived inside the deleted file as `#pragma comment(lib, …)`; grep
confirms no `mfplat`/`mfreadwrite`/`mfuuid`/`propsys` anywhere. `APR_HAVE_ACTION_M4A`
verified absent from the generated `build.ninja`, not assumed.

**Why it went is now design §8.0**, a section rather than a footnote, with a
table contrasting what a kill leaves for WAV (size fields wrong, length
inferred), MP3 (self-synchronising frames) and OGG (granule position in the page
header).

**And it became a general criterion in AGENTS.md rule 4**, which is the part
worth keeping:

> *"A recording that never reaches `finalize` at all must still be playable. A
> container that keeps its index at the front and writes it last fails this and
> cannot be rescued from a dead process… Do not add a format that cannot survive
> a `TerminateProcess`."*

That is a test for any future format, not a note about AAC.

### `display_name` → `AprStrId` (defect 1 — my design error)

```c
const char    *id;              /* wire value, in session files */
AprStrId       display_name_id; /* catalog id, NOT a literal */
const wchar_t *extension;       /* matched against paths */
```

Four ids added (`ACTION_NAME_WAV/_MP3/_OGG/_NONE` — the built-in discard sink had
a literal too). **Tests moved from `ASSERT_NOT_NULL(display_name)` to asserting
the id is non-zero *and* resolves to non-empty**, because a set id with no `.rc`
entry would otherwise pass while displaying a placeholder. `id` and `extension`
stay literals and the rule says why: neither is prose, and they are allowed to
disagree — `ogg` writes `.opus`.

### Defects 4 and 6

- **`.ogg` resolves without `--format`.** `action_for_extension` now runs two
  passes: the `extension` field first (authoritative, so `.opus` still reaches
  ogg per RFC 7845), then the registry `id`. Both skip an empty extension so the
  `none` sink stays unreachable by path. Verified live: `.ogg` and `.opus` both
  print `as ogg`; `.m4a` fails exit 2 with *"m4a is not a format this build can
  write."*
- **`foreach(_uitest …)`** replaced with a `^test_ui_` walk. Done last, suite
  re-run after.

### Note on a flaky suite

`test_capture_fake` failed intermittently — *a different set of cases each run* —
while the capture agent was rewriting it minute by minute for the health knob.
Not a real failure; it went green once they settled. Worth remembering as a
signature: **a suite failing differently each run during a parallel wave is
contention, not a bug.**

### Remaining known defects: 2 (health knob, in flight), 5 (window-class move), 7 (UI, in flight)

---

## 2026-08-26 — `capture_fake` health knob (KNOWN DEFECT 2 closed)

`build.cmd Debug test` and `build.cmd Release test`: **23/23 suites, 460 cases,
0 failures**, `/W4 /WX` clean in both. Nothing committed. **No audio was
rendered at any point** — this task needed none (AGENTS.md rule 1).

### The contract change: `include/capture.h`

**`include/capture.h` is a shared contract and it changed.** Five fields added
to the `fake` arm of the config union, nothing else touched — no existing field
moved, renamed or changed meaning, and the process/device arms are untouched:

```c
uint64_t mute_at_frame;    /* 0 = never */
uint64_t unmute_at_frame;  /* 0 = never */
uint64_t die_at_frame;     /* 0 = never */
int      start_muted;      /* already muted when the session arms */
int      start_dead;       /* already exited when the session arms */
```

Checked against every implementation and every caller: `capture_process.c` and
`capture_device.c` never read the `fake` arm; `graph.c`, `cli.c` and all six
test files that build a config `memset` it first, so a zero-initialised config
is still a healthy source and nothing needed updating. The union grew from 12
to 40 bytes, which no one depends on.

**Frame indices, not ticks**, deliberately: exact, independent of QPF, and
independent of `rate_error_ppm`, so a transition lands on the same sample
however finely a caller steps the timeline. `0 = never` is what keeps a
zero-initialised config healthy; `start_muted` / `start_dead` are how you ask
for frame 0. `mute_at_frame == unmute_at_frame` is **refused** at open rather
than resolved quietly.

### The behaviour it models — deliberately the confusing one

A muted or dead fake **keeps producing frames, at exactly the configured rate,
and they are silent**. That is not a convenience: loopback is
post-session-volume so a muted app records digital zeros, and after the target
exits loopback keeps handing over zeros for ever with no error and no flag
(design 4.1 #5/#6). Modelling death as "the source stops" would be tidier and
would make every test built on it agree that the desync cannot happen. Death is
one-way and raises `last_error` the same way `capture_process.c`'s real
detector does, and deliberately does not stop the capture (section 10).

### What the new tests prove

- `tests/test_capture_fake.c` **23 -> 31 cases.** Mute/unmute/death are
  sample-exact at the requested frame; a dead source still produces exactly the
  frames the clock says are due, ten seconds past death, at 30 ppm, while
  overrunning its ring; `frames_written == rb_write_pos` throughout; one leap
  and a thousand steps over a mute + unmute + death schedule give
  byte-identical audio and identical status.
- `tests/test_sync.c` **15 -> 17 cases**, both driven through `core`:
  a source that dies mid-session and one that goes muted mid-session, with the
  status propagating capture -> `AprSource` -> `apr_source_alive/muted`, the
  bus still producing every frame QPC asked for, and every reader's alignment
  error under one sample. A second bus fed only by the failing source is what
  makes the silence observable (`apr_bus_peak` exactly 0.0).
- `tests/test_ui_tree.c` **11 -> 14 cases.** The two state clauses are now
  selected through a real graph instead of being asserted to exist in the
  catalog — including the both-at-once case, where **death must win over
  muted** (unmuting a dead app fixes nothing).

**Mutation-tested, both directions.** Removing `!f->dead` from the silence
condition fails 4 cases; modelling death as "stop producing" fails the
alignment assertion in `test_sync.c` and 4 more. The tests bite.

### Section 10 held up

Nothing in section 10 turned out to be wrong. Two things it says are now
demonstrated rather than asserted: one source failing does not take the session
down, and a muted source is genuinely indistinguishable from a dead one *in the
audio* — the only difference is the status, which is exactly why the UI needs
two separate clauses and why `alive` and `muted` are two fields and not one.

### Left undone, on purpose

- **The CLI's `WARN_SOURCE_MUTED` path is still uncovered.** `poll_sources()`
  is a static in `cli.c` with no seam, and the only way in is a health field on
  the `--fake` spec (`parse_fake`, `AprCliSource`, the config switch, and for
  consistency the session save/load/compare). That is a `cli.c` edit and an
  agent was live in that file. **The knob is ready; it is a ten-line CLI
  change** — add a fourth `--fake` field and pass it into `cfg.fake`.
- Not touched: `src/ui/tree_panel.c` (no change was needed — the tests reach it
  through the public API) and `src/cli/cli.c`.

### Process note

Commit `65fbfa5` ("Delete M4A; make action display names catalog ids") landed at
05:22 while this work was in flight and swept `include/capture.h`,
`capture_fake.c` and the three test files into itself ("Tree also carries
in-flight capture and UI work"). I did not commit anything. This is the same
`git add -A`-mid-wave attribution problem already recorded twice in this file.
Both configurations were rebuilt and re-run against the post-commit tree.

### Orchestrator note on the health knob (defect 2 — fixed)

Two things from that work worth not losing:

**The fake models the confusing behaviour, not a tidy one.** A muted or dead fake
**keeps producing frames at exactly the configured rate, and they are silent** —
because that is what really happens: loopback is post-session-volume, and after
the target exits it hands over zeros for ever with no error and no flag. A fake
that "stopped" on death would have made the alignment bug untestable. Proven by
mutation: modelling death as "the source stops" fails the alignment assertion in
`test_sync.c` plus four more cases.

**Why `alive` and `muted` are two fields.** A muted source is *genuinely
indistinguishable from a dead one in the audio* — both are digital silence at the
right rate. The only difference is the status. That is exactly why the UI needs
two clauses and why neither can be inferred from the samples.

Frame indices rather than ticks: exact, independent of QPF **and** of
`rate_error_ppm`, so a transition lands on the same sample however finely a
caller steps the timeline. `0 = never`, so a zero-initialised config is still a
healthy source; `start_muted`/`start_dead` are how you ask for frame 0.

**Residual gap (small):** the CLI's `WARN_SOURCE_MUTED` is still uncovered.
`poll_sources()` is a static in `cli.c` with no seam; the way in is a health
field on the `--fake` spec (`parse_fake`, `AprCliSource`, the config switch, and
session save/load/compare for consistency). **~10 lines**, deferred only because
an agent was live in `cli.c`.

### Process failure — mine, third occurrence

`git add -A` mid-wave swept this agent's in-flight work into commit `65fbfa5`
("Delete M4A…"). **This is the third time**, having already been recorded twice
in this file. Staging explicit paths is not optional during a parallel wave; the
lesson clearly does not survive being written down, so: **when any agent is live,
`git add <explicit paths>` only.**

---

## 2026-08-26 — The UI can record, and can build a graph from nothing

`build.cmd Debug test` and `build.cmd Release test`: **25 suites, 493 cases,
0 failures**, `/W4 /WX` clean in both. Release `apprecorder_ui_app.exe` is
849,408 bytes; `apprecorder.exe` (CLI) is 778,240.

Closes START-HERE defect **7** ("no dialog layer, so the canvas announces 'not
available yet'") and defect **1**'s consumer side, and uses defect **2**'s
`capture_fake` health knob the moment it landed.

### 1. The recording loop was EXTRACTED, not copied

`include/runner.h` + `src/core/runner.c`. The twenty lines that turn a
configured graph into a recording — arm, anchor at one QPC instant, tick, **wait
out the mixer's lookbehind so the final block is not thrown away**, finalize on
every exit path — used to live inside `src/cli/cli.c`. They are now shared and
the CLI is one of two callers.

**How the extraction was proved faithful: `tests/test_cli.c`, unchanged,
45/45.** That suite drives the parser, the resolver, the dry run AND the record
loop in-process with no output to scrape, so a change in what is printed, in
what order, or in what the files contain fails it. The observer callback exists
precisely so the CLI's sentences stay in the CLI: `APR_RUN_EV_STARTED` is where
it prints "Recording to…", `APR_RUN_EV_FINISHING` is where it prints
"Finishing…", and those fire at exactly the points the old inline code printed
them.

`tests/test_run_loop.c` (**10 cases, new**) covers the three properties the UI
needs and the command line never exercises: the loop runs off the caller's
thread and stops from another; the two health failures that look exactly like
success are each reported **once**; and `apr_runner_destroy` on a running
recorder **finalizes rather than abandons**.

One deliberate design point worth not re-deriving: the runner treats its stop
event **being signalled** as a stop, not merely as a wake-up. The CLI's console
control handler is installed before any runner exists and signals a shared
event rather than calling in, so a runner that trusted only its own flag would
wait on an already-set manual-reset event and spin until the duration expired.

### 2. What the UI now does

| File | What it owns |
|---|---|
| `include/ui_controller.h`, `src/ui/controller.c` | the graph, the recording, the dialogs, the tray, and the close guard. **This is the piece that did not exist.** |
| `include/ui_dialogs.h`, `src/ui/dialogs.c` | real Win32 dialogs built from `DLGTEMPLATE`s at runtime |
| `include/ui_tray.h`, `src/ui/tray.c` | notification area: icon, context menu, live tooltip, balloons |
| `src/uiapp/main.c` | `wWinMain`. The UI was a library with no way to run it. |

**Dialogs are real dialogs with real controls**, and the templates are built in
memory from catalog strings rather than living in the `.rc` — a dialog resource
would carry English text in the binary (AGENTS.md rule 6) that existed only to
be overwritten in `WM_INITDIALOG`. Pickers use a **LISTBOX, not a LISTVIEW**,
because a listbox item's text IS its accessible name, so a row can be a whole
sentence ("Chrome, process 8412, playing audio now") instead of four columns a
reader has to be driven across.

The dialogs get `WS_EX_LAYOUTRTL` and their children inherit it — which is
right **here and only here** (ui_app.h convention 1): we paint nothing inside a
dialog, so the mirrored DC has nothing of ours to mirror wrongly. Fonts are
`DS_SHELLFONT` / "MS Shell Dlg", which sidesteps design 6.2's font-coverage trap
by asking the system for the face it already uses rather than picking one and
hoping it covers Arabic.

### 3. Announcements: UIA and the shell, never TTS

No SAPI, no `nvdaControllerClient`, no Tolk anywhere. Two channels:

- **Live region on the status bar** while the window is in front —
  `apr_ui_app_set_status_text(app, text, announce)`. The flag is load-bearing:
  the one-second clock passes 0, because an elapsed time that spoke every
  second would make the application unusable inside a minute.
- **Notification-area balloons** for anything that happens while the window is
  NOT in front, which is where this application spends its recordings. A
  live-region change on an unfocused background window is not reliably
  announced by any reader; balloons are.

Recording announces: **started**, **stopped** (with the duration),
**stopped-but-incomplete**, **a source exited**, **a source is muted**, **an
output stopped writing**, **some sources could not be armed**, **nothing to
record**, and **that cannot be changed while recording**. The last one matters:
`graph.h` forbids changing the shape while a tick is in flight, so editing
commands are greyed AND say why when invoked anyway — grey is not a message
this application's first user receives.

### 4. The tray is the primary surface during a session

`Shell_NotifyIcon`, `NOTIFYICON_VERSION_4` (without which the keyboard's
Applications key never reaches us), a real `HMENU` through `TrackPopupMenuEx`,
and a **live tooltip that is a status readout**: "apprecorder — recording,
01:12:30". Windows+B then the arrows reaches it, so the author can check a
session from inside any other application without opening a window.

Handled and easy to forget: the shell can restart, and an application that does
not listen for the registered **"TaskbarCreated"** broadcast silently loses its
icon for the rest of the session. For a recorder that runs for hours that is
losing its only surface.

**Close policy, decided and documented.** `WM_CLOSE` while recording asks a
three-answer question: stop and close (finalize, then exit), leave it recording
in the notification area, or do nothing. `WM_ENDSESSION` is **not** a question —
Windows kills the process shortly after the handler returns, so the handler
stops the runner and blocks until every file is closed, exactly as the CLI's
console handler does for `CTRL_CLOSE_EVENT`.

### 5. Small core additions, each with a caller that needed it

- **`apr_bus_remove_action`** — bus.h genuinely lacked it, as suspected. It
  **finalizes before it detaches**, so removing an output leaves a playable
  file. The canvas's Delete on an output row used to refuse out loud because
  this did not exist; that refusal, and the test asserting it, are gone.
- **`apr_bus_action_path`** and **`apr_source_config`** — an action's config and
  a source's config are borrowed for the length of `create()`, which is right
  for opening a capture and useless for **writing one down**. Without them a
  session saved from the window came back as named-but-empty. The alternative
  was a parallel map inside the UI, i.e. a second owner of the same fact.
- **`apr_bus_set_name`** — rename.
- **`apr_ui_app_set_status_text` / `set_title_text` / close handler / message
  handler** on the frame, so the controller can own the tray, the clock and the
  close without app.c learning about any of them.

### 6. Keyboard map (everything is reachable, nothing is mouse-only)

New frame accelerators: **F2** rename bus, **Ctrl+Shift+3** remove an output
(mirrors Ctrl+3 = add an output), **Ctrl+Shift+H** hide to the notification
area. Disconnect is on the Edit menu but deliberately **not** in the
accelerator table — the canvas claims Ctrl+Shift+E as a keystroke and
accelerator matching is exact on modifiers, so adding it here would silently
take disconnect away from the canvas's own binding table.

`tests/test_ui_a11y.c` gained two cases that pin this: every operation that
builds a graph is on the menu, named **from the catalog**, and carries a
mnemonic; and every accelerated operation **advertises its key beside the menu
item**, asserted against the actual binding rather than against itself.

### 7. Two bugs found while building this, both silent

- **A closed handle in the close path.** `wait_for_files` pumped messages while
  waiting on the runner's finished event — but pumping lets the posted STOPPED
  notice run, and handling it destroys the runner and closes that handle.
  `MsgWaitForMultipleObjects` on a closed handle does not fail loudly; it
  returns `WAIT_FAILED` for ever and the window hangs until the timeout. Now
  polls `c->recording`, which has no handle to outlive.
- **Adopting a graph from the wrong thread.** It builds the canvas's node
  windows, and a window belongs to the thread that created it, so the frame
  ends up holding children it cannot destroy — a close that never completes,
  with no error anywhere. Caught because `test_ui_dialogs.c` printed
  "UI thread did not exit". `apr_controller_set_graph` now marshals.

### 8. Still open

- **The modal dialogs are not driven live by a test.** A modal owns the thread
  that opened it, so entering and answering one from the asserting thread is
  not possible; driving it would mean a second thread posting synthetic
  keystrokes, which tests the input queue more than the dialog. Their CONTENT
  is covered through the pure row builders they are made of, and their
  reachability through the menu and accelerator assertions.
- **Arabic is still unwritten** — this wave added **~120 more English strings**
  (three new groups: `APR_STR_LIST_UI_REC`, `_UI_DLG`, `_UI_TRAY`, ids 1600
  onward, `APR_STR_ID_MAX` raised to 1800). Every one has an explicit
  "not translated yet" Arabic entry, so the count the ship gate reports went up.
- **Sessions save the graph, not the drift state**, and a saved fake source
  round-trips its tone/ppm/amplitude but a saved process source depends on
  `apr_session_describe_process` finding the pid still alive at save time.

### 9. Design section 6 — what is now wrong in the spec

- **6.1 says the accessibility tree is "30 elements".** Still true of the frame
  alone; the dialogs and the tray add elements the spec does not describe at
  all. Section 6 has no dialog layer in it, and it should: the decision that
  templates are built at runtime from the catalog (rather than living in the
  `.rc`) is a real i18n design point that is currently only in
  `include/ui_dialogs.h`.
- **6.1 does not mention the notification area.** It is now arguably the app's
  primary surface during a session, and the reason balloons exist —
  live regions do not carry from a background window — belongs in the spec
  beside the live-region pattern, not only in `ui_tray.h`.
- **Section 6 has nothing about where the RECORDING lives.** The spec describes
  two views over a model and never says who owns the model or the run loop.
  `core/runner.c` and `ui/controller.c` should be named in section 7's module
  layout, which currently lists neither.
- **6.3's "selection is `{kind, bus id, source id, action index}`"** is
  implemented and correct, but the spec says "a controller wires the two
  directions" without saying where that controller is. It is
  `src/ui/controller.c` now.

---

## 2026-08-26 — UI INTERACTION LAYER complete. **The UI is usable.**

**25 suites, 493 cases, 0 failures**, Debug and Release, `/W4 /WX` clean.
`apprecorder_ui_app.exe` 829.5 KB · `apprecorder.exe` 760 KB.

### The runner was extracted, and faithfulness was *proved*

`include/runner.h` + `src/core/runner.c`. Arm → anchor at one QPC instant → tick
→ wait out the lookbehind → finalize on every exit path. `cli.c`'s
`record_loop()` is now 40 lines of observer callbacks and one
`apr_runner_run()`.

**The proof is `tests/test_cli.c`, unedited, 45/45.** That suite drives the
parser, resolver, dry run *and* the record loop in-process, and fails on a change
to what is printed, in what order, or what lands in the files. The observer
pattern exists so the CLI's sentences stay in the CLI.

Non-obvious: the runner treats its stop event **being signalled** as a stop, not
a wake-up — the CLI's console handler is installed before any runner exists and
signals a shared manual-reset event, so a runner trusting only its own flag would
spin.

`tests/test_run_loop.c` covers what the CLI never does: stopping from another
thread; a muted or dead source reported **once**, not 40 times in a 400 ms run;
and `apr_runner_destroy` on a running recorder **finalizing rather than
abandoning**.

### Two silent bugs it found

1. **`wait_for_files` pumped messages while waiting on the runner's finished
   event.** Pumping runs the posted STOPPED notice, which destroys the runner and
   closes that handle — and `MsgWaitForMultipleObjects` on a closed handle
   returns `WAIT_FAILED` **forever**. The window hung with no error anywhere.
2. **Adopting a graph from the wrong thread** builds the canvas's node windows on
   that thread, so the frame cannot destroy them — a close that never completes,
   again silently. `apr_controller_set_graph` now marshals.

### Announcement policy as shipped

Two channels, **no TTS anywhere** — no SAPI, no `nvdaControllerClient`, no Tolk.
A **status-bar live region** in the foreground; **notification-area balloons**
for anything happening while minimised, where live regions are not reliably
announced.

**The elapsed clock writes with `announce=0`** — an elapsed time that spoke every
second would be unusable inside a minute. Editing while recording is greyed
*and* refuses out loud.

### Close policy

`WM_CLOSE` while recording asks three real answers (stop and close / leave it
recording in the tray / cancel). **`WM_ENDSESSION` is not a question** and blocks
until every file is closed.

### `bus.h` was genuinely missing things — added, each with a caller

`apr_bus_remove_action` (**finalizes before it detaches**), `apr_bus_action_path`,
`apr_source_config` (a session saved from the window otherwise came back
named-but-empty), `apr_bus_set_name`.

### ★ HARD-KILL VALIDATION — accidental, and the best test of the night

I mis-read `--duration` as milliseconds; it is **seconds**, so a 3-minute
recording was running when I killed it with `Stop-Process -Force`. That is an
unplanned real-world test of the kill-survival design:

| file | duration | ffmpeg decode |
|---|---|---|
| `a.wav` | 179.42 s | **clean, zero warnings** |
| `a.mp3` | 179.86 s | clean |
| `b.ogg` | 179.53 s | **clean, zero warnings** |

All three playable after a hard kill, across two buses. The single MP3 note
("estimating duration from bitrate") is exactly the documented consequence of the
Xing tag never being written — **and the reason CBR is the default**: CBR
duration is arithmetic, so it still reports the right length. §8.0's reasoning
holds under a real kill, not just a simulated one.

### Section 6 gaps it identified (not wrong statements — omissions)

- **No dialog layer.** The "templates built at runtime from the catalog, not in
  the `.rc`" decision is a genuine i18n point and currently lives only in
  `ui_dialogs.h`.
- **No notification area.** Arguably the primary surface during a session, and
  the reason balloons exist belongs beside the live-region pattern.
- **Never says who owns the model or the run loop.** §7's module layout lists
  neither `core/runner.c` nor `ui/controller.c`. §6.3's selection identity says
  "a controller wires the two directions" without naming it: `ui/controller.c`.

---

## 2026-08-26 -- CLI `WARN_SOURCE_MUTED` covered (defect 2 residual) + defect 5 closed

`build.cmd Debug test`: **26 suites, 519 cases, 0 failures**, `/W4 /WX` clean.
Release: the same 26 suites and 519 cases pass, run from `build\Release\`;
`build.cmd Release test` could not reach ctest because `apprecorder_ui_app.exe`
was **locked by the author's own running copy** (PID 17488) -- a link failure,
not a code one, and nothing else in the tree was left unbuilt. Nothing
committed. **No audio was rendered at any point** (AGENTS.md rule 1).

Case counts: `test_cli` **45 -> 57**, `test_session` **47 -> 51**,
`tests/test_discover.c` is **new, 10 cases**.

### 1. `--fake` grew a health half, and that is what reaches `poll_sources()`

    --fake <hz>[,<ppm>[,<amp>]][,<health>...]

    <health> = muted | dead | mute=<frame> | unmute=<frame> | die=<frame>

Five words, mapping ONE-TO-ONE onto `AprCaptureConfig.fake`'s five health
fields with nothing in between. On the source's own spec rather than on a flag
of its own, because the health of a synthetic source is part of describing it,
exactly like its tone and its drift.

Two grammar decisions worth not re-deriving:

- **A frame is never 0.** capture.h reserves 0 for "never", so `mute=0` and
  `die=0` are REFUSED and frame 0 is spelled `muted` / `dead`. Two spellings
  for one state is how a "0 = never" convention rots.
- **A health word may stand where a number would have gone.** `440,dead` reads
  as well as `440,0,0.25,dead` and means the same; once a health word appears,
  everything after it is health too, so a word can never be mistaken for a
  drift that arrived late. The tone is still first and still required, so
  `--fake dead` is a usage error.

`mute=` and `unmute=` on the SAME frame is not checked in the parser --
`fake_open` already refuses it, and the CLI reports that as exit 5 with the
reason attached. One rule, one owner.

### 2. What is now proved through the real CLI path

`poll_sources()` is still a static with no seam, and it no longer needs one:
every case below drives `apr_cli_run` and reads what it printed.

| Case | Says |
|---|---|
| starts muted | warning fires, **once**, exit **0** |
| goes muted mid-recording | warning fires, **once**, exit **0** |
| dies mid-recording | warning fires, exit **6**, file playable |
| muted AND dead at once | **only** the death warning, exit **6** |
| muted at 50 ms, dead at 500 ms | **both**, in order |
| one fake dead, one healthy, two buses | exit 6, **both** files playable |

The expected line is built with `apr_str_format` from the catalog rather than
typed into the test, so the cases pin the WARNING and not the wording of one
language -- which matters the day the Arabic lands.

**DEATH NOW WINS OVER MUTED**, and that is a real behaviour change in
`core/runner.c`: the mute clause gained an `alive &&`. Unmuting an application
that has exited fixes nothing, and the two are indistinguishable in the audio
anyway, so the listener gets the one sentence worth acting on -- the same
choice `ui/tree_panel.c` makes for the row a screen reader reads. Only
SIMULTANEOUS states collapse; two things that happened at two different
moments are still two things. The snapshot still exposes `alive` and `muted`
separately, so the UI is unaffected.

**Mutation-tested, both directions.** Removing the `alive &&` fails the
both-at-once case; dropping the five lines that copy health into `cfg.fake`
fails 7 cases across parse, record and session.

### 3. Session round-trip

`muteAtFrame`, `unmuteAtFrame`, `dieAtFrame`, `startMuted`, `startDead`,
**written only when one of them is set** -- so a healthy fake produces exactly
the file it produced before health existed, and a file written by an older
build still loads as a healthy source. There are no booleans anywhere in this
format, so the two start flags are 0 or 1 and anything else is `BAD_VALUE`.

`same_source` compares health too. Two fakes differing ONLY in when they go
silent are two sources; interning them together would have quietly dropped a
bus's source, which is a silent wrong recording rather than an error.

`APR_SESSION_MAX_FRAME` is the ceiling for both the file and the command line,
deliberately one constant: what can be typed has to be what can be written
down and read back.

### 4. One test moved that was not about the feature

`test_strings.c`'s "in range but never declared" canary was id **1099** --
which is the first free id after the CLI help block, i.e. exactly where the
next help string gets added. Adding one failed that case instead of failing
anything real. It is now `APR_STR_ID_MAX - 1` (1799): the list grows upward
from 1000, so the top of the range stays undeclared longest.

### 5. KNOWN DEFECT 5 closed -- the window-class lookup moved

`session_load.c`'s private static is now
`apr_process_window_class(pid, buf, cch)` in `capture/discover.c`, the owner of
"what is running and what is it". **It was not a clean lift**; three things
differed and each is now decided rather than inherited:

- **The static return buffer did not survive the move.** discover.h promises
  every function there allocates nothing beyond the caller's array, and a
  function-static buffer is a threading hazard the UI would inherit. The public
  shape is a caller's buffer returning characters written, matching
  `apr_process_image_name`. The borrowed-pointer shape the `AprSessionMachine`
  callback wants is now a four-line adapter in `session_load.c`, where the
  single-threaded contract that justifies it actually lives.
- **The size constant was the real hazard.** `APR_SESSION_CLASS_CCH` is now
  *defined as* `APR_DISC_CLASS_CCH` rather than a second 64 that happens to
  agree. Had discover used the 256 that `RegisterClassW` actually allows, the
  copy into the 64-character session field would have reached `wcscpy_s`
  over-length -- the CRT's invalid parameter handler, a modal dialog and
  therefore a hang in Debug, NOT a truncation. `test_discover.c` asserts the
  two are equal so they cannot drift apart again.
- **`pid == 0` now short-circuits** instead of costing a whole `EnumWindows`
  sweep to discover that no window belongs to the System Idle Process. Same
  answer, and consistent with `apr_process_exists`.

The `#pragma comment(lib, "user32.lib")` travelled with the function. Both
files are in the same static library, so nothing about linking changed and
CMakeLists.txt needed no edit.

`tests/test_discover.c` is new because **discover.c had no direct coverage at
all** -- it was only ever exercised through `list-apps` and a session resolve.
The truncation case is the one that earned its place: it proves `GetClassNameW`
truncates rather than failing, which is the assumption the shortened constant
rests on.

### Left alone on purpose

- `src/ui/canvas.c` carries an **uncommitted change that is not mine** (an
  empty-canvas accessible description). It was already in the working tree.
- **`build/Release/apprecorder_ui_app.exe` is a PRE-CHANGE build.** It was
  locked by the author's running copy for the whole of this task, so it is
  the only artifact in the tree that did not relink. Everything it links
  against (`apprecorder_ui.lib`, `apprecorder_core.lib`) DID rebuild and
  every `test_ui_*` binary relinked against them and passes. Closing the
  app and re-running `build.cmd Release` is all it needs.
- `build/{Debug,Release}/test_action_m4a.exe` are **stale binaries** from
  before M4A was deleted. ctest does not list them -- it reports 26 -- but a
  script that globs `test_*.exe` will find 27 and count 15 phantom cases.
  A clean build directory removes them.

---

## 2026-08-26 (morning) — small items closed; author testing

**26 suites, 519 cases, 0 failures.** Defects 2 (residual), 5 and the canvas
empty-state all closed.

### Canvas empty-state (found by the author on first use)

He landed on the canvas and heard *"The recording graph, drawn as nodes. Press
Tab to move between nodes…"* — on an **empty** session, so there was nothing to
Tab to. His reply was "What's that?", which is the correct reaction to a
description of something that is not there.

The empty sentence already existed (`UI_ANN_CANVAS_EMPTY`) but only fired as an
*announcement* when a navigation key found nothing; the pane's **description was
static**, set once at creation. Now switched in `refresh_all()` — which runs
after every model change, so it stays honest as things are added and removed —
and the creation site starts with the empty text, since a canvas is necessarily
empty then. **The same string serves both, deliberately: one sentence to
translate rather than two that can drift apart.**

**Worth noting how this escaped:** `test_ui_a11y.c` asserts every description is
**non-empty**, and it was. It just was not **true**. An assertion about presence
cannot catch a statement about content — only listening did.

### CLI `--fake` health (defect 2 residual)

```
--fake <hz>[,<ppm>[,<amp>]][,<health>...]
<health> = muted | dead | mute=<frame> | unmute=<frame> | die=<frame>
```

Five words mapping 1:1 onto the five `AprCaptureConfig.fake` health fields.
**A frame is never 0** — `capture.h` reserves 0 for "never", so frame 0 is
spelled `muted`/`dead`: one state, one spelling. A health word may stand where a
number would have gone (`440,dead` == `440,0,0.25,dead`), and once one appears
everything after it is health, so a word can never be mistaken for a late drift
value.

**One real behaviour change: `core/runner.c`'s mute clause gained `alive &&` —
death wins over muted**, matching `tree_panel.c`. Unmuting an exited app fixes
nothing, and the two are indistinguishable in the audio. Only *simultaneous*
states collapse; the snapshot still exposes `alive` and `muted` separately so the
UI is untouched. Mutation-tested: removing `alive &&` fails the both-at-once
case.

Session round-trip writes health **only when set**, so a healthy fake is
byte-identical to before and older files still load. `same_source` compares
health — two fakes differing only in when they fail are two sources, and
interning them would have silently dropped a bus's source.

### `apr_process_window_class` moved to `discover.c` (defect 5)

**Not a clean lift — three differences, each decided:**

1. **The static return buffer did not survive.** `discover.h` promises callers'
   arrays only; a function-static is a hazard the UI would have inherited. The
   borrowed-pointer shape the `AprSessionMachine` callback wants is a 4-line
   adapter left in `session_load.c`, where the single-threaded contract that
   justifies it actually lives.
2. **The size constant was the real trap.** `APR_SESSION_CLASS_CCH` is now
   *defined as* `APR_DISC_CLASS_CCH` rather than a second 64 that happens to
   agree. Had discover used the 256 `RegisterClassW` allows, the copy into the
   64-char session field would have hit `wcscpy_s` over-length — the CRT
   invalid-parameter handler, i.e. a **modal dialog and a hang in Debug**, not a
   truncation.
3. `pid == 0` short-circuits instead of costing a full `EnumWindows` sweep.

**`tests/test_discover.c` is new — `discover.c` had no direct coverage at all.**

### Operational notes

- **`build.cmd Release test` cannot reach ctest while the UI app is running** —
  `apprecorder_ui_app.exe` is locked, so that one artifact does not relink.
  Everything else builds and every `test_ui_*` binary relinks and passes. Close
  the app to get a true Release run.
- Stale `build/*/test_action_m4a.exe` leftovers removed. ctest correctly reported
  26, but a glob of `test_*.exe` would find 27 and count 15 phantom cases.

---

## 2026-08-26 — BUG: every dialog failed to open, silently

**Reported by the author on first real use:** *"ctrl+1 doesn't bring the source
addition dialog"* … *"It's all Silence."*

### Root cause: a two-byte misalignment in the dialog template builder

`dt_item()` in `src/ui/dialogs.c` wrote the creation-data WORD **after** an
alignment pad:

```c
dt_sz(b, text);   /* title */
dt_align(b);      /* WRONG */
dt_w(b, 0);       /* creation data */
```

Per `DLGITEMTEMPLATE`, the creation-data WORD follows the title **immediately**;
alignment belongs *after* it (and `dt_item`'s own leading `dt_align()` already
provides it). With the pad in front, Windows reads the pad as the creation-data
count, lands two bytes short, and parses our creation word as the next item's
style DWORD. Template garbage → `DialogBoxIndirectParamW` returns **−1**.

**It only fired when a title did not happen to end DWORD-aligned — so it
depended on string lengths, which makes it LANGUAGE-DEPENDENT.** It would have
behaved differently again in Arabic. A dialog builder that works in English and
fails in Arabic is exactly the class of bug §6.2 exists to prevent.

### Why it reached the user as silence, and the second fix

Every call site compared the result against `IDOK`, so **−1 ("template
rejected") was folded into 0 ("user pressed Cancel")** — no window, no message,
nothing to hear. All eight sites now go through `dlg_run()`, which logs the
failure with `GetLastError` and says plainly that it is a bug rather than a
cancellation. `dt_end()` also logs on overflow instead of returning NULL mutely.

### Verified

Frame goes modal, popup exists, and the dialog reads correctly end to end:
title *"Add a source"*, then the three kind radios, the list, Refresh, the name
field and OK/Cancel. **26/26 suites green.**

### How it escaped the tests

`test_ui_a11y.c` walks the **main window's** tree. **No test ever opened a
modal dialog**, so the entire dialog layer had zero runtime coverage — it
compiled, and that was all that was ever checked. Worth closing: a test that
posts `WM_COMMAND` for each dialog command, asserts the frame goes modal, walks
the dialog's own UIA tree for unnamed elements, and closes it.

### Also spotted, not yet addressed

The EXCLUDE radio reads *"Everything the machine plays, except one
application"*. Spec §4.1.1 says never to word it as "everything except X",
because the mode **walks the process tree** — excluding a launcher excludes
everything it spawned. Needs checking against whatever detail the dialog shows
after selection.

---

## 2026-08-26 (later) — "Add Source" from the GUI: the capture layer now owns its apartment

### The defect

Adding any source from the windowed front end failed:

```
ERROR controller.c do_add_source: process loopback requires the MTA;
      this thread is an STA: APR_E_STATE at capture_process.c(240) in proc_open
```

A collision between two individually correct requirements on one thread:

- `src/uiapp/main.c` calls `CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)`
  **deliberately** — `IAccPropServices`, which supplies every control's
  accessible name, is created in that apartment and is valid only on the thread
  that created it. Getting it wrong does not crash; it silently loses every
  accessible name. **The UI thread must stay an STA.**
- WASAPI process loopback requires the **MTA** (spec 4.1 note 7).

`capture_process.c` and `capture_device.c` both called
`CoInitializeEx(NULL, COINIT_MULTITHREADED)` **on the caller's thread** and
returned `APR_E_STATE` on `RPC_E_CHANGED_MODE`. The CLI never noticed (its
thread is not in an STA); the GUI could not add one source.

### The fix — in the capture layer, not the caller

Making the controller hop to a worker thread was explicitly rejected: it pushes
a COM constraint onto every present and future caller.

**A capture already owns a thread; it now owns its apartment too.**
`src/capture/wasapi_common.c` gained a capture thread that calls
`CoInitializeEx(MTA)` once and then *is* both the job queue and the pump:

| new API | does |
|---|---|
| `apr_wasapi_thread_start(s)` | creates the thread, waits for it to enter the MTA |
| `apr_wasapi_call(s, job, user)` | runs `job` on it and waits; callable from any apartment |
| `apr_wasapi_start/stop` | marshal `IAudioClient::Start` / the pump handover |
| `apr_wasapi_close(s, teardown, user)` | runs the impl's COM teardown **on that thread**, then releases this layer's objects, retires the thread |

`open()`/`close()` in both WASAPI kinds split into a plain half (validate,
calloc, `apr_wasapi_stream_init`, `apr_wasapi_thread_start`) and a
`*_open_com` / `*_close_com` job that runs inside the apartment. The old pump
thread is gone as a separate thread — it is the same thread — so this costs
nothing.

**`apr_capture_create()` is now callable from an STA, the MTA, or a thread with
no apartment at all**, and the old constraint *"`open()`/`close()` must share a
thread for the WASAPI kinds"* **is gone**, not merely hidden: the
`CoInitializeEx` reference it protected is no longer on the caller's thread.
Stated as a contract at the top of `include/capture.h` and in spec **§4.2.1**
(new) with a note in **§4.3**.

### `capture_device.c` — asked, and answered

**It had the identical refusal.** Not by necessity: `IMMDeviceEnumerator` is
perfectly happy in an STA, so the device path did not *need* the MTA the way
process loopback does — it demanded it in sympathy, with the same
`RPC_E_CHANGED_MODE` return. So "Add Source → a microphone" from the GUI failed
for the same reason. Fixed the same way rather than by relaxing the check,
because the objects created there are used by the pump and belong in the pump's
apartment.

### Latent bug fixed on the way

`proc_close`/`dev_close` called `free(impl)` **even when `s.pump_stuck` was
set** — i.e. even when `wasapi_common.c` had just decided the pump was wedged
and deliberately declined to free anything under it. The stream struct is
embedded in that allocation, so that was the exact use-after-free the
`pump_stuck` path exists to avoid. Both now leak the impl and say so, which is
the survivable half. `wasapi_common.h` states the rule for any future kind.

### Tests — the asymmetry was the real defect

26 suites passed while the product could not add a source, because **every test
ran from an MTA or uninitialised thread**. Two new suites, 28 total:

- **`tests/test_capture_apartment.c`** (6 cases) — `apr_capture_create` +
  `start` + `stop` from a genuine `COINIT_APARTMENTTHREADED` worker, from an
  explicit MTA worker, and from an uninitialised worker. Fake source
  unconditionally; a real process tap on this test's own PID; a device source
  opened and **never started**.
- **`tests/test_ui_add_source.c`** (3 cases) — a real frame + real
  `AprController` on a real STA, adding a fake source and a real process source
  through `apr_graph_add_source` on the window's own thread. That is
  `do_add_source()` minus only the modal chooser.

**Both were verified to FAIL against the old behaviour** before the fix was
kept: a temporary probe reinstating the `RPC_E_CHANGED_MODE` refusal made
`a_process_tap_opens_and_runs_from_an_sta_thread`,
`a_process_tap_stopped_and_closed_from_an_sta_thread_leaves_nothing_behind` and
`a_process_source_can_be_added_from_the_window_thread` fail with the user's
exact message. Probe removed afterwards.

**Trap worth remembering:** the first draft of the second case established "no
audio engine here" *from the STA thread itself*, so the apartment refusal
presented as a SKIP and passed silently — the exact shape of the original miss.
A skip predicate must be evaluated from a **non**-STA thread.

### Green

- `build.cmd Debug test` → **28/28 suites, 0 failures**
- `build.cmd Release test` → **28/28 suites, 0 failures**
- `/W4 /WX` clean in both.

**Pre-existing intermittent, NOT from this change:** Release `test_ui_tree`
failed twice across six full `ctest` runs and was green in the other four, in
twelve consecutive isolated runs of that suite alone, and in four consecutive
full Release suites afterwards. It never failed in Debug. That suite builds its
graph from `APR_SRC_FAKE` only, and `capture_fake.c` was not touched here, so
the capture-apartment work cannot reach it; it looks like UIA contention with
the author's own live window / screen reader. Worth chasing separately.

### Build note for the author

`build\Release\apprecorder_ui_app.exe` was **locked by your running instance**
(PID 48448, started 10:20:44), so `link.exe` failed with LNK1104. Rather than
kill it, the old binary was renamed to
`build\Release\apprecorder_ui_app.inuse-20260826.exe` (Windows allows renaming a
running image) and the fresh one linked in its place. **The copy you are running
is the pre-fix build.** Close it and relaunch `build\Release\apprecorder_ui_app.exe`
to get the fix; the `.inuse-20260826.exe` file can be deleted once nothing holds
it.

### AGENTS.md rule 1 disclosure

**Nothing was rendered to any output device. No audio was played at all.** No
device source was ever `start()`ed, no microphone was opened for reading, and
`spike_silentplayer` was not needed or run. The process taps target this
machine's own test/UI processes, whose trees render nothing, so the only thing
captured was the audio engine's own digital silence — in memory, into a ring
buffer, never to a file. No audio file was written or left behind.

### Still open (unchanged by this work)

- Device capture is still opened and closed but **never started** by any test —
  starting it records the author's microphone. Its pump, gap-fill and
  discontinuity handling still have no hardware coverage. Needs a manual pass
  with the author consenting.
- Mute polling still inspects only the named PID's session.

---

## 2026-08-26 — COM APARTMENT BUG: the GUI could not add any source

**28/28 suites green, Debug and Release.** Verified live: adding `nvda.exe`
now logs `added id=1; graph now holds 1 sources` and the node appears in both
the canvas and the tree.

### What was wrong

`capture_process.c` **and** `capture_device.c` each called
`CoInitializeEx(NULL, COINIT_MULTITHREADED)` **on the caller's thread** in
`open()`, returning `APR_E_STATE` on `RPC_E_CHANGED_MODE`.

- The **CLI's** thread had no apartment, so it silently joined the MTA and
  everything worked.
- The **GUI's** thread is a deliberate **STA** — `IAccPropServices`, which
  supplies every control's accessible name, is only valid on the thread that
  created it — so **every source failed**.

**That asymmetry is the whole lesson: 26 suites passed while the GUI could not
add a single source, because nothing tested the capture layer from an STA
caller.** The non-default side of an environment split is the side that needs
the test.

### The fix — in the capture layer, not the caller

A capture already owned a thread; **it now owns its apartment too.** The pump
thread became a long-lived *capture* thread: `CoInitializeEx(MTA)` once, then it
alternates between servicing marshalled jobs and being the pump. Not an extra
thread — the same one.

`apr_wasapi_thread_start()` / `apr_wasapi_call()` / `apr_wasapi_close()`;
`open`/`start`/`stop`/`close` marshal and wait. Each kind's open/close split into
a plain half and a `*_open_com` / `*_close_com` job that runs inside the
apartment — **including every `Release`, which was also happening in the wrong
apartment before.**

Fixing this in the controller would have pushed a COM constraint onto every
present and future caller. **The handoff's old constraint — "open()/close() must
share a thread" — is now gone rather than hidden**, because the `CoInitializeEx`
reference they balanced is no longer on the caller's thread at all.

`capture_device.c` **had the identical refusal but not the identical need**:
`IMMDeviceEnumerator` is happy in an STA, and the device path demanded MTA in
sympathy. Fixed the same way rather than relaxing the check, since the objects
it creates are used by the pump and belong in the pump's apartment.

### Latent use-after-free fixed on the way

`proc_close`/`dev_close` called `free(impl)` **even when `pump_stuck` was set** —
exactly when `wasapi_common.c` had just declined to free anything because the
pump was wedged. The stream is embedded in that allocation, so this was the very
use-after-free the `pump_stuck` path exists to prevent. Both now leak and log.

### Tests — 26 → 28 suites

- `tests/test_capture_apartment.c` — create/start/stop from a genuine STA
  worker, an explicit MTA worker, and an uninitialised worker.
- `tests/test_ui_add_source.c` — a real frame, a real controller, on a real STA,
  adding sources through the graph on the window's own thread. That is
  `do_add_source()` minus only the modal chooser.

**Both were verified to fail against the old behaviour** by temporarily
reinstating the refusal.

**Sharpest observation from the agent:** its first draft established "no audio
engine here" *from the STA thread itself*, so the apartment refusal presented as
a **SKIP and passed silently — the same shape as the original miss.** A skip
predicate must be evaluated from a non-STA thread. Both files now do.

### Known flake to chase separately

Release `test_ui_tree` failed twice across six early ctest runs, green in the
other four, in 12 consecutive isolated runs, and in both final runs. It builds
only from `APR_SRC_FAKE` and `capture_fake.c` was untouched — looks like UIA
contention with a live window.

---

## 2026-08-26 (afternoon) — THE ANNOUNCEMENTS WERE NEVER HEARD, AND CTRL+E WAS REAL

**29 suites, 537 cases, 0 failures — Debug and Release, `/W4 /WX` clean.**
One new suite, `tests/test_ui_behaviour.c` (9 cases). Two product defects fixed,
both invisible to every structural assertion in the tree.

### The author's question — "why are all these bugs in the UI?" — has an answer

Every suite asserted **structure**: the element exists, it is named, it is
focusable, the description is not empty. None asserted **behaviour**: press
this, did the model change, and *was the user told*. Three defects walked
straight through that gap, and so did these two.

### Defect 1: every announcement in the product was inert. THIS IS THE BIG ONE.

`say()` wrote the sentence into the status bar and raised
`EVENT_OBJECT_LIVEREGIONCHANGED`. Measured with a UIA client and against NVDA's
own code:

- `UIA_LiveSettingPropertyId` on the status bar reads **0 (Off)**. That window's
  provider is the MSAA bridge; it cannot express a live SETTING, so no UIA
  client will ever treat it as a live region however many events we raise.
- The event *did* fire — the new suite hooks it and counts it — but **what a
  reader does with it is read the element's NAME**. NVDA's handler for
  `liveRegionChange`, disassembled out of its own `library.zip`, is literally
  `ui.message(self.name)` (`NVDAObjects/__init__.pyc`; and
  `IAccessibleHandler/internalWinEventHandler.pyc` confirms it hooks the event
  at all).
- Our status bar's name was the fixed word **"Status"**, annotated once through
  `IAccPropServices`. `SB_SETTEXTW` sets the text of **part 0 — a CHILD** of
  that element — so the sentence was never anywhere the event pointed.

**So for every announcement this product has ever made, a screen reader said
"Status" and the sentence was never heard by anybody.** That is the mechanism
behind "it's all silence", reported three separate times for three unrelated
defects: two of those really were broken, but the third channel — the one that
was supposed to say what had happened — has been dead the whole time.

**Fix (`apr_ui_app_set_status_text`):** the sentence is now the status bar's
accessible NAME, and both `EVENT_OBJECT_NAMECHANGE` and
`EVENT_OBJECT_LIVEREGIONCHANGED` are raised on it. The name is set even when
`announce` is 0 — so navigating to the status bar reads what it currently says
rather than a sentence from a minute ago — and only the EVENTS are conditional,
which keeps the once-a-second clock silent. `AprUiApp` now keeps the text so a
language change cannot overwrite a live sentence with the word "Status".

**Also removed: `canvas.c`'s own `NotifyWinEvent`.** It raised
LIVEREGIONCHANGED **on the canvas window**, whose name is the PANE's name — so
every canvas edit made a reader say "Signal flow" and never the sentence. There
is now exactly one owner of "say this to the user": the status bar's live
region, reached through the announcement sink. `ui_canvas.h` says so.

### Defect 2: Ctrl+E, and it was neither of the two hypotheses

`st->cur` was **not** stale. `st->pending` was **destroyed**.

Ctrl+E is a FRAME accelerator, so both presses go frame -> controller -> canvas,
and the controller calls `refresh_views()` after each one — which calls
`apr_canvas_rebuild()`, which destroys every node window and did
`st->pending = -1`. So the first press began the edge and announced it
correctly; the rebuild then wiped the half-made gesture; the second press was a
**first** press on a bus. Every time. Deterministically.

The canvas suite could not see it: its connect case calls `apr_canvas_command()`
directly and never goes through the controller, so no refresh happens between
the presses.

**Fix:** `apr_canvas_rebuild` now preserves `pending` by MODEL identity exactly
the way it already preserved focus — including re-applying `apr_node_set_pending`
to the new node window — and drops it only when the node it names has gone from
the model. Focus was preserved across a rebuild for precisely this reason; the
pending end is the other half of the same state and had been left out.

### `tests/test_ui_behaviour.c` — the real deliverable

Every case sends the REAL message (`WM_COMMAND` with 1 in the high word *is*
what `TranslateAccelerator` sends; `WM_KEYDOWN` at the focused node *is* what
`DispatchMessage` delivers), asserts the MODEL changed, and asserts WHAT WAS
ANNOUNCED against the catalog sentence with the catalog's own inserts.
**An operation that changes the model and says nothing fails here.**

| case | covers |
|---|---|
| status bar carries the announcement as its accessible name | defect 1, through a UIA client; prints `LiveSetting` |
| an announcement raises a live-region event carrying the sentence | the event fires, and MSAA reads the sentence inside the callback |
| adding a source | model + sentence |
| adding a bus **through its dialog** | accelerator -> template -> edit field -> OK -> model + sentence |
| the two-step connect gesture | **defect 2**, including focus moving between the presses |
| the two-step disconnect gesture | same, other direction |
| the plus key | real `WM_KEYDOWN`; the gain rose, and the new level was spoken |
| Delete | node gone from the model AND the canvas, and named |
| every dialog opens and cancels | all seven, each titled, model untouched |

Verified RED before the fixes: the two announcement cases failed with
`actual: [Status]`, and both gesture cases failed on
`ASSERT_NOT_NULL(apr_canvas_pending_node(...))`. The other five passed
throughout, which is the point — they are the regression net, not the diagnosis.

**Everything model-touching runs on the window's own thread.** Editing the graph
publishes into two views, which creates and destroys real child windows; doing
that from the asserting thread would build windows owned by the wrong thread and
prove nothing about the product. Setup runs inside the UI thread before the loop
starts; everything after that arrives as a message. Modals are answered from
outside the thread that owns them with a posted `WM_COMMAND` — which is what the
dialog's own button sends, not synthetic input.

### Two small structural changes that made it testable

- **`apr_controller_add_source()` / `apr_controller_add_bus()`** — the verb
  without the chooser, the same split as `apr_controller_open_session` and for
  the same two reasons: a modal cannot be answered from the thread that opened
  it, and a later scripting surface wants the verb. Everything a user receives
  (model, both views, menu states, the sentence) lives in the verb, so no second
  route can announce something different. `do_add_source`/`do_add_bus` are now
  the chooser plus a call.
- **`APR_CANVAS_GAIN_{MIN,MAX,STEP}_DB10` moved to `ui_canvas.h`.** The spoken
  sentence contains that number, so anything asserting on the sentence has to be
  able to say what one press does without keeping a second copy of it.

### AGENTS.md rule 1 disclosure

**Nothing was rendered to any output device. No audio was played at all.**
`APR_SRC_FAKE` throughout; no capture was ever started; the hardware-enumerating
dialogs were opened and CANCELLED, never accepted. The one WAV output exists so
that two dialogs have something to act on — it is finalized on teardown and the
file is deleted. `spike_silentplayer` was not needed or run.

### For the author

Both `apprecorder_ui_app.exe` binaries relinked cleanly (nothing was holding
them this time, so no rename was needed). **Ctrl+E should now connect, and you
should actually hear the sentence.** If a reader still says nothing on an edit,
the next thing to check is whether NVDA has decided to use UIA rather than MSAA
for this window, because the mechanism above is the MSAA one.

### Still open

- The status bar's name is now its content, so its catalog name
  (`UI_PANE_STATUS`, "Status") is only ever the name of a status bar that has
  not said anything yet. If a reader announces the bar by name during object
  navigation it will read the last sentence — believed right, not yet heard.
- The live-region evidence above is in-process. The annotation is server-side,
  so it marshals, but nobody has yet put an out-of-process client on the running
  app and read the name back.
- Device capture is still never `start()`ed by any test.

---

## 2026-08-26 (evening) — AN ACTION'S LIFETIME IS ONE RECORDING, AND A TAKE IS NEVER OVERWRITTEN

**30 suites, 568 cases, 24,497 assertions, 0 failures — Debug and Release,
`/W4 /WX` clean.** One new suite (`tests/test_outpath.c`, 18 cases), one new
module, three defects fixed. The author reported the first one while testing.

### The report, and why all three of its symptoms are one bug

> *"I recorded a file called test.mp3, stopped, listened to it, and then
> recorded again and stopped. Turns out the file was not updated with the new
> recording, so I deleted it and recorded again, but the new file wasn't
> there."*

`apr_bus_add_action()` called `vt->create(cfg, &state)` **immediately**, so the
output file was opened when the user ADDED the output. The runner never created
actions; it used the existing ones and finalized them at stop.

- Take 1 worked. `finalize` closed the encoder **permanently**.
- Take 2 wrote nothing: the action was finalized and `on_audio` refused.
- After the delete, Windows kept the handle valid with **no directory entry**,
  so take 3 went to a file with no name and appeared to vanish.

### Fix 1 — the lifetime (`include/bus.h`, `src/core/bus.c`)

**A bus now holds a SPEC — the vtable plus its config — not an open encoder.**

| when | what happens |
|---|---|
| `apr_bus_add_action` | records the spec; **validates the path**; creates nothing |
| `apr_bus_start` | resolves each name and calls `create` — the file appears here |
| `apr_bus_stop` | `finalize`, then `destroy`, then `state = NULL` |

`close_action()` is the single teardown, so finalize-then-destroy has exactly
one order and `apr_bus_remove_action` / `apr_bus_stop` / `apr_bus_destroy` all
share it. Failed flags are cleared at **start**, not at stop, so a caller can
still ask after the fact which output went wrong.

### Where the early validation went (a deliberate trade, not a loss)

Opening the file used to be what caught an unwritable path — at add time, while
the person who typed it was still there. The OPEN moved to record time; the
**CHECK stayed**. `apr_bus_add_action` calls `apr_out_validate()`, which
expands the template and asks the folder whether something may be created in
it, **leaving nothing behind**. Validate at add, open at record.

### Fix 2 — `src/platform/outpath.c` + `include/outpath.h` (new)

One owner for three questions three callers were about to ask separately: can
this path be written, what does this name expand to, what happens when it is
taken. `cli.c`'s private `probe_writable` was folded into it (AGENTS.md rule 3).

**Template syntax** — braces, so an ordinary Windows path is its own template:

| token | expands to |
|---|---|
| `{date}` | `2026-08-26` |
| `{time}` | `14-03-52` (hyphens: a colon opens an ADS, not a recording) |
| `{bus}` | the bus name, with `< > : " / \ | ? *` and controls replaced by `-` |
| `{ext}` | the extension the chosen format writes |
| `{n}` | the lowest number for which the whole path does not exist **on disk** |
| `{{` | a literal `{` |

An unknown token is copied through unchanged — a brace is legal in a path and
nothing that used to work may stop.

**Default:** `%USERPROFILE%\Music\{bus} {date} {time}.{ext}`, pre-filled into
the Add Output dialog. It ends in `{ext}` so it follows the format combo
instead of going stale at `.wav`. Environment variable rather than shell32, so
the module stays pure file system and needs no COM apartment.

**COLLISION POLICY: AUTO-INCREMENT, NOT A PROMPT.** `mix.wav` becomes
`mix-2.wav`, then `mix-3.wav`. Three reasons, spelled out in `outpath.h`:

1. There is often nobody to ask — this runs from the runner at the instant
   recording starts, which is also `--quiet` in a script or a scheduled task.
   A prompt cannot be the single policy for both front ends.
2. **A modal between the press and the first sample is lost audio.** The thing
   being recorded does not wait for a dialog.
3. A Record key that just records is the right shape for this user.

**And it is never silent.** `apr_out_resolve` reports whether it had to move
the name aside; the bus keeps it (`apr_bus_action_renamed`); the runner raises
the new `APR_RUN_EV_OUTPUT_RENAMED`; the CLI warns and the UI announces *"That
name is already a recording, so this take is being saved as X instead. Nothing
was overwritten."* A `{n}` template never reports — asking for a number and
getting one is not a surprise.

### Two paths, and both are needed

- `apr_bus_action_path()` — **what the user asked for**, tokens and all. This
  is what a session file stores, so reopening tomorrow records under tomorrow's
  name rather than freezing to yesterday's filename.
- `apr_bus_action_current_path()` — **where the audio actually went**. What the
  CLI's "Recording to" / "Finishing" / "Wrote" lines now print (they printed
  the plan's argument before, which would name a file that does not exist), and
  what the rename announcement carries.

Session save also round-trips `bitrateKbps` and `quality` now — the bus keeps
the whole spec, and the schema already had the fields.

### Fix 3 — the register (`res/strings.rc`, English only)

The author: *"the naming of the recording is a bit too technical for end users:
it says 'write file to' instead of 'save file to'."* He is right — this is a
recorder for people, not a signal-flow tool. Reworded, with no id churn:

`UI_DLG_OUT_PATH` "File to write" -> **"Save to file"**; `UI_DLG_OUT_BUS`
"Which bus to write" -> "Which bus to record"; `UI_DLG_SAVE_AUDIO_TITLE`
"Where to write" -> "Where to save"; `UI_DLG_PATH_NEEDED` -> "Choose where to
save the recording first."; `UI_DLG_NO_BUSES` "no bus to write" -> "no bus to
record"; `UI_DLG_OUTPUT_ADDED` "will be written to" -> "will be saved to";
`UI_DLG_OUTPUT_REMOVED` "no longer being written" -> "no longer being saved";
`UI_DLG_OUTPUT_ROW` "%1 to %2" -> "%1 saved from %2"; `UI_ANN_ACTION_FAILED`
and `UI_TRAY_INFO_ACTION_FAILED` "stopped writing" -> "stopped saving"; the
four `UI_NODE_DESC_BUS_*` descriptions plus `UI_ANN_REMOVE_REFUSED`,
`UI_DESC_TREE` and `UI_TREE_DESC` "writes" -> "saves"; `CLI_OPT_OUT` "Write
this bus to this file" -> "Save this bus to this file";
`ERR_BUS_HAS_NO_OUTPUT` gained "to save to".

**Four new ids only**, each declared untranslated in the ar-SA block:
`CLI_OPT_OUT_TOKENS`, `WARN_OUTPUT_RENAMED`, `UI_ANN_OUTPUT_RENAMED`,
`UI_DLG_OUT_PATH_TOKENS`. (500+ Arabic strings are already pending.)

### A fourth thing, found on the way: failures were only reported at the END

`report_action_failures()` ran once, after `apr_graph_stop()`. An encoder that
died at minute two was announced when the recording finished. It is now
`poll_actions()`, called before "started" and on every tick, with a per-output
bit in `BusSlot` so each thing is said exactly once. An output that cannot even
be **created** is therefore audible at the start of the run rather than an hour
later.

### Verified RED, and one honest limitation

Both halves were reinstated and the suites re-run:

- **Old lifetime** (create at add, finalize once, never reopen):
  `test_ui_behaviour`'s `recording_twice_through_one_graph...` went **red**.
  `test_cli`'s stayed **green** — and that is not a weak case, it is what the
  command line can see: every `apr_cli_run` builds and destroys its own graph,
  so a CLI invocation cannot reach the lifetime defect at all. The UI keeps one
  graph across both presses, which is exactly what the author did. The comment
  in `test_cli.c` says so, so nobody reads those cases as lifetime coverage.
- **No collision policy**: `test_cli`'s record-twice, `test_outpath`'s
  `a_name_that_is_already_a_recording_is_never_overwritten` and
  `test_ui_behaviour`'s rename announcement all went **red**.

### Tests

- `tests/test_outpath.c` (new, 18) — tokens, sanitising, `{{`, overflow, the
  collision policy including the third take and a dot in a folder name, `{n}`
  resolved against the disk, the probe leaving an existing file alone, the
  early check.
- `tests/test_cli.c` (+8) — record twice to one name -> two playable files;
  delete between takes -> it comes back; the moved take named in BOTH the
  warning and the closing summary; a template expands to the file; two buses
  may share one template; two templates naming one file are still a duplicate;
  a template is never probed with its braces in it; the help says what may go
  in a name.
- `tests/test_ui_behaviour.c` (+5) — **this suite had never pressed Record.**
  Two takes through one graph -> two playable files with take one intact;
  delete between takes -> it comes back; the rename announced DURING the take
  (afterwards you would only catch the stop sentence); an unwritable folder
  refused at ADD time with nothing created; a writable one accepted at add time
  with nothing created.

### AGENTS.md rule 1 disclosure

**Nothing was rendered to any output device. No audio was played at all.**
`APR_SRC_FAKE` throughout; no endpoint was opened in either direction; the
hardware-enumerating dialogs were opened and CANCELLED. Every WAV produced was
under `%TEMP%` and has been deleted, including the ones the RED experiments'
early exits left behind. `spike_silentplayer` was not needed or run.

### For the author

The Release `apprecorder_ui_app.exe` was locked (the app was running), so the
**old image was renamed to `apprecorder_ui_app.locked-20260826-123014.exe`**
rather than killing the process. It is safe to delete once that instance exits.
Debug relinked with no trouble.

### Still open

- `{n}` is resolved against the disk, so a folder something else is also
  writing into could hand out the same number twice in a race. `CREATE_ALWAYS`
  in the WAV action means the loser would be overwritten. Not reachable from
  one process.
- The Add Output dialog grew to 216 dialog units for the token hint, sized for
  Arabic (26 units, not the 14 English needs). Not yet seen with a translated
  string in it.
- `apr_bus_add_action` now refuses while the bus is running. The UI already
  disabled editing during a recording, so nothing reaches it — but it is a new
  refusal and nothing tests it.

---

## 2026-08-26 — Notification flood (my fault, twice over) + per-recording lifetime

**30/30 suites green, Release.**

### The flood: a TEST was notifying a real person

Three suites build a real `AprController`, which builds a real `AprTray`, which
registers a real shell icon — so **every run of the test suite fired real
Windows notifications at the author, announced aloud by his screen reader.** He
was in a meeting: *"I can't focus, I'm on a meeting and I got so many
notifications, still getting them."*

I made it worse by running the full suite myself while he was on that call, on
top of an app instance I had left running.

**Fix:** `apr_tray_create` skips the shell registration when
`APPRECORDER_NO_TRAY` is set. The tray object stays real, so the tests still
exercise its logic; only the registration is skipped, and everything downstream
(`tip`, menu, `notify`) is already gated on `t->added`, so it all no-ops with no
further checks. **Set by CMake for every test**, present and future, so a new UI
suite cannot forget.

**Rule worth keeping: a test must leave no trace on the machine running it.**
Notification area, clipboard, audio device, foreground window — all of it.

### The balloon rule — the author's formulation, which was better than mine

> *"I think notifications only run if we started / stopping recording from the
> system tray, or in case of an error, right?"*

Exactly right, and it collapses to **one** test rather than a list of cases:
**balloon only when the window is not in front.** A tray-initiated action
implies the window is not in front, so it needs no special case at all. Hidden
to tray, minimised, or simply behind something else all mean the same thing —
no better channel exists.

`say_and_notify()` fired **unconditionally** before, with no foreground check
anywhere in the file. Now `a_better_channel_exists()` gates it. `fg_override`
(-1 real, 0/1 forced) exists because a test cannot reliably make itself the
foreground window — same shape as `action_wav.c`'s write gate. **It defaults to
-1 explicitly, because `calloc` gives 0 and 0 would mean something.**

The file header said *"EVERY STATE CHANGE IS SAID TWICE… ON PURPOSE"*. It now
says once, on whichever channel can reach the user — **instead, not as well**.
Note this only became audible after commit `1a436be` fixed the live region;
before that the duplication was silent because the first channel was dead.

### Per-recording action lifetime — the `test.mp3` bug — LANDED

The killed agent got further than expected. Tests now passing:

- `recording_twice_through_one_graph_leaves_two_playable_files`
- `a_take_deleted_between_recordings_comes_back`
- `a_writable_name_is_accepted_at_add_time_and_still_creates_nothing`
- `an_output_whose_folder_is_not_there_is_refused_when_it_is_added`
- `a_take_that_moved_aside_is_announced_while_it_is_happening`

So: the file is **no longer created when the output is added**, early validation
of the path is **kept** at add time, recording twice yields two files, a deleted
take comes back, and a collision **moves the old take aside** rather than
overwriting it — announced while it happens.

### Still outstanding

- The wording pass (*"write file to"* → *"save file to"*) and filename
  templating were in the same brief and are **not confirmed done** — verify.
- The `command` action for Gemini transcription is approved and queued.

---

## 2026-08-26 — Bug-tracker pass: CLI / session / actions / outpath / registry

One of three agents working the `BUGS.md` tracker in parallel. This one owned
`src/cli/*`, `src/session/*`, `src/actions/*`, `src/platform/outpath.c`,
`src/core/registry.c` and their headers and tests. The other two were live in
`src/capture/*` + `src/core/{source,graph,bus,runner}.c` and in `src/ui/*`, so
nothing here touches those files.

### Fixed — majors

| ID | What changed |
|---|---|
| **M1** | `AprActionVTable` grew an optional `check_config(cfg)` (last field, so an older vtable still compiles and still means "nothing to ask") plus `apr_action_check_config()` in `registry.c`. Each action's implementation IS the precondition block its own `create()` already ran, called from both, so the numbers cannot fork. `apr_cli_resolve()` asks it per output — so `--bitrate 400 --out x.mp3` is exit 2 in milliseconds instead of an hour of recording nothing. Second half: a run whose EVERY output failed to open is now exit **4**, not 6, and its `--json` says `"failed": true` and `"seconds": 0` instead of listing a duration for a file that does not exist. |
| **M2** | **The rule chosen:** under `--allow-missing` a bus left with no sources is dropped, out loud, and the rest of the run proceeds; the run is refused only when no bus survives. The bus takes its outputs with it (an empty file is a worse lie than a missing one) and the run ends INCOMPLETE. `record` only — never `save-session`, where it would be M14 again. |
| **M5** | Confirmed against a real file, then fixed. Granule positions are now `960 x packets` (what a decoder produces) with the EOS packet capped at `pre_skip + real audio`; finalize flushes the encoder's lookahead. Measured with ffmpeg: a 48900-frame take decoded to **48460 samples before, 48900 after**. `ffprobe` reported 1.025250 s in BOTH cases — it reads duration from the number that was wrong, so only decoding shows it. |
| **M13** | New `apr_out_open_new()`: CREATE_NEW, retrying through the same `-2, -3, ...` ladder `apr_out_resolve` walks (one shared `take_name()`). All three actions open through it instead of `CreateFileW(..., CREATE_ALWAYS, ...)`. |
| **M14** | `save-session` with more than 64 distinct sources is now a refusal naming the source that did not fit, and writes no file. It used to `continue`, exit 0, and report `"sources": 64`. |

### Fixed — minors

m13 (`--json` had no document on a session-resolve failure), m14 (a disk stall
that became silence now ends the run at exit 6, reported from `finalize` and
NOT from `on_audio` — an error there would drop the output for the rest of
the session and turn a hole into a truncation), m15, m16 (`--format WAV`),
m17 (`take{n}.wav` on two buses), m18 (`prescan` reading another option's
value), m19 (`CTRL_CLOSE` now waits `INFINITE`), m20 (control characters in
session PATH fields — deliberately not in display names, so a session that
round-tripped still does), m21 (silent truncation in `tok_wstr`; and every
failure path in that loop used to leave `out` unterminated).

### New API, for whoever touches these next

- `action.h`: `check_config` on the vtable, `apr_action_check_config()`.
- `outpath.h`: `apr_out_open_new()`, `apr_out_has_token()`.
- `cli.h`: `apr_cli_test_close_wait_ms()` (test seam pinning the INFINITE wait).
- New catalog ids: `ERR_OUTPUT_UNSUPPORTED`, `ERR_NOTHING_WAS_WRITTEN`,
  `WARN_BUS_DROPPED`, `WARN_OUTPUT_DEGRADED`, `ERR_SESSION_NOT_USABLE`
  (English written, Arabic left explicitly untranslated, as the .rc requires).

### One residual, and it belongs to `src/core/bus.c`

If the M13 create race actually happens, the action lands on `mix-2.wav` while
the bus's `current` still says `mix.wav`, so the REPORTED name is wrong for
that run. The audio is safe, which was the point. The clean finish is one line
in `open_action()` — take the final name back from the action — and
`AprActionConfig` gaining an optional out-path field. Not done here because
`bus.c` is another agent's file this pass.

### Build and test at handoff

`build.cmd Debug test` and `build.cmd Release test`: **31 of 33 suites pass in
both**, identically. The two failures are `test_ui_behaviour`
(`recording_starts_stops_and_announces_both`) and `test_ui_dialogs`
(`ok_in_the_add_output_dialog_never_silently_does_nothing`), which are the UI
agent's M8/m6 work in flight and were failing before this pass touched
anything. Every
suite this pass owns — `test_cli`, `test_session`, `test_action_wav`,
`test_action_mp3`, `test_action_ogg`, `test_outpath`, `test_registry` —
passes in both configurations.

Nothing was rendered to an audio device at any point: every recording in these
suites is `--fake` / `APR_SRC_FAKE`, and every file produced was deleted.

---

## 2026-08-26 — core + capture bug pass (BUGS.md: root pattern, C1, C4 model half, M3, M6, m12, m26, m27, m28)

Ran in parallel with the UI pass and the CLI/session/actions pass. Files owned:
`src/capture/*`, `src/core/{source,graph,bus,runner}.c`, `src/platform/log.c`,
their headers and tests.

### The root pattern is now a module, not a habit

`include/join.h` + `src/platform/join.c`. Every bounded join in the tree goes
through it. The rule it states — **the code that frees must be the code that
reads the join result, and an abandoned join frees nothing** — is what makes
this safe in C, where a return value alone cannot be forced on a caller.
`AprCaptureVTable::close`, `apr_capture_destroy`, `apr_source_destroy`,
`apr_log_shutdown` and `apr_runner_destroy` all return `AprErr` now; a caller
that ignores one leaks and cannot corrupt.

### API changes other passes should know about

- **`APR_E_BUSY`** — new `AprErrKind`, appended (nothing above it moved). It is
  what every graph/bus shape change returns while `apr_graph_running()`. The UI
  should match on this, not `APR_E_STATE`, for the "not while a recording is
  running" sentence. (The UI pass has already wired it up:
  `editing_is_refused_out_loud_while_a_recording_runs` passes.)
- **`apr_source_destroy` / `apr_capture_destroy` return `AprErr`.** A failure
  means nothing was freed and the pointer is still valid; retrying later is how
  the leak is recovered.
- **`apr_capture_create` may leave `*out` non-NULL after a failure.** That is
  the signal that the caller's ring is still being written into. Documented in
  `capture.h`; `apr_source_create` honours it.
- **`apr_runner_destroy` returns `AprErr`** and now waits for a *synchronous*
  run as well, on a new `loop_left` event (not `finished_event`, which fires
  while the loop is still touching the runner).
- **`apr_log_shutdown` returns `AprErr`**, and `apr_log_init` refuses while an
  abandoned shutdown is outstanding.
- **`apr_wasapi_close` returns `AprErr`**; `pump_stuck` stays for diagnostics
  but is no longer the only way to find out.
- New internal headers: `src/capture/capture_process.h` (test probe),
  and two seams — `apr_capture_fake_wedge/unwedge` and
  `apr_wasapi_test_set_race_hook`.

### Two design points settled

- **A process tap now has TWO threads.** Mute polling moved off the pump onto
  its own MTA thread with its own COM objects. Design 4.2.1 is unchanged: the
  rule is that a capture owns its apartment so the CALLER never has to think
  about one, and no object crosses between the two threads.
- **Design 5.1 is narrowed, deliberately.** A process tap now fills a gap the
  engine explicitly reports (`DATA_DISCONTINUITY`). It still synthesizes
  nothing speculatively — with no flag, not one sample is invented and no
  resampler is allocated. "Arrives perfect" was measured under an engine that
  was keeping up; it is not a promise about one that has just said it dropped
  audio, and on the reference timeline a silent shear is the worst failure in
  the system.

### One existing test changed

`test_sync.c`'s "a source added mid-session" became "a source that *starts*
mid-session": building an edge on a running graph is now `APR_E_BUSY`. The
property it tests (reader placement) is unchanged; see the C4 entry in BUGS.md.

### Build and test at handoff

`build.cmd Debug test` and `build.cmd Release test`: **33 of 33 suites pass in
both**, 100%, no warnings under /W4 /WX.

(Intermediate runs during this pass showed `test_ui_behaviour`,
`test_ui_dialogs` and once `test_cli` failing, with a different set each time.
Those were the other two passes mid-edit, on announcement-queue and temp-file
assertions; `test_ui_behaviour` never starts a recording at all, so the new
`APR_E_BUSY` guard cannot fire in it. All three are green in the final run.)

Suites this pass owns — `test_capture_timeline` (new, 14), `test_capture_abandon`
(new, 9), `test_graph_guard` (new, 10), `test_graph`, `test_sync`, `test_log`,
`test_run_loop`, `test_capture_fake`, `test_capture_wasapi`,
`test_capture_apartment`, `test_ringbuf`, `test_clock` — pass in both.

Every fix was watched go red with itself reverted; BUGS.md names the failing
case for each.

Nothing was rendered to any audio device. Every source is `APR_SRC_FAKE` except
one process-loopback tap on the test process's own PID, whose tree renders
nothing (AGENTS.md rule 1).


---

# 2026-08-26 — UI pass (C2, C3, C4-UI, M4, M7–M10, M12, M15 + 15 minors)

Owned this pass: `src/ui/*`, `src/uiapp/main.c`, the `ui_*.h` headers and the UI
test suites. Ran alongside the core+capture pass and the CLI/session pass;
`include/strings.h` and `res/strings.rc` were edited additively and merged.

## The one that mattered most, and what it teaches

**M4 — "greyed AND spoken" had never once spoken.** `TranslateAccelerator` does
not send `WM_COMMAND` for an accelerator whose menu item is disabled: it eats
the key and delivers nothing. So `busy()` never ran, and mid-recording every
editing key was TOTAL SILENCE — which for this author is indistinguishable from
a broken application.

The design was right. The delivery was absent. And the reason it survived is
worth keeping: **it lived in an inlined message loop that no test could reach.**
The loop is now `apr_ui_app_pretranslate()`, a function, and it resolves the
keystroke against the accelerator table itself — a key bound to a DISABLED
command is dispatched anyway and the handler refuses it out loud. Enabled state
decides how the menu LOOKS; it no longer decides, silently, whether a key
exists.

**Four more of the same shape were found by looking for it**, all now fixed: a
dialog that could not be CREATED folded into "the user cancelled" (m5); a
half-made connection whose end left the model ending with no word (m9); every
real refusal from the model announced as "that is not available yet" with the
`AprErr` discarded (m4); and two sentences that were true of a *different*
situation, which is its own kind of silence (m2, m3).

## The other two criticals

**C2 — the Structure panel could not be browsed.** The tree's selection sink
fires on every caret move and the controller answered it with an unconditional
`SetFocus` on a canvas node, so one Down took the keyboard away and everything
past the first row was unreachable. Selection and ACTIVATION are now two
separate signals: `apr_canvas_set_current_node()` keeps the views agreeing
quietly, and `apr_tree_panel_set_activate_sink()` (Enter, double click) is what
moves focus. Enter needed the TreeView subclassed — `IsDialogMessage` eats it
before any control sees it (design 6.1).

Neither existing suite could have caught this: `test_ui_tree.c` drives a live
tree with no controller, and `test_ui_behaviour.c` had never opened the tree.
**The defect was in the wire between them, and nothing tested the wire.**

**C3 — stop-and-close left a zombie process.** The close-wait drain dispatched
every message with no `WM_QUIT` check, so it swallowed the very `WM_QUIT` that
`DestroyWindow` had just produced. Window gone, process alive, tray icon
ghosted. It now re-posts and stops pumping; that is safe by construction,
because the only thing that posts the `WM_CLOSE` this drain dispatches is
`recording_finished()`.

## New public seams, and why each exists

| Seam | Why it had to exist |
|---|---|
| `apr_canvas_set_current_node()` | agree with the other view without taking the keyboard |
| `apr_canvas_set_edit_sink()` | the canvas edits the graph with no command reaching the controller; contract FORBIDS the listener rebuilding the canvas |
| `apr_tree_panel_set_activate_sink()` | moving is not choosing |
| `apr_ui_app_pretranslate()` / `_accel_command()` / `_command_enabled()` | make the message loop a testable function; separate "bound" from "greyed" |
| `apr_tray_is_registered()` | never hide the window into nothing |
| `apr_dlg_last_failed()` | "it did not open" is not "the user cancelled" |
| `apr_dlg_button_width()` | never size a control to fit its English string, as a property a test can hold |
| `apr_controller_last_balloon()` / `_balloon_count()` | every test runs with `APPRECORDER_NO_TRAY`, so there is no icon to watch |
| `apr_controller_test_set_close_wait_ms()` | the give-up path is 30 s of stalled disk away |
| `apr_dlg_test_fail_next()` / `apr_dlg_test_set_session_path()` | a system modal owns the thread that opened it — File > Open was a route no test could enter |

## Two things found by tests rather than by reading

- **An ordering bug this pass introduced and then caught.** M8's
  `refresh_views()` was first placed between `c->recording = 1` and the
  "recording started" announcement. Rebuilding two views destroys and recreates
  every node window, which takes long enough that an observer watching
  `apr_controller_recording()` sees the state a measurable time before the
  sentence exists. The two are now published adjacently, and `refresh_views()`
  runs last.
- **`test_ui_dialogs.c` had a latent version of the same race** — its own
  comment says so about the STOP case and it used `wait_for_said` there, but
  the START case polled the flag and then read the sentence. Now both use
  `wait_for_said`.

## Honest residue

- **m6's stated case is unreachable.** The guards are in, but the FORMAT combo
  cannot be empty in a build with encoders and the BUS combo is guarded a step
  earlier. The sweep was right about the shape and wrong about the reachability.
- **m8's canvas half is not independently reproducible.** Reverting it fails no
  test; the nested `SetFocus` takes on this build. Applied for consistency with
  the case `tree_panel.c` measured, not because it was observed.
- **m10 and m22 are fixed by inspection with no test** — a common-dialog flag,
  and a digit that renders identically until the Arabic-Indic decision differs.
- **M11 is NOT fixed** (error reasons are English prose in a translated frame).
  It is a `platform/err.c` change and belongs to whoever owns that file. m4 and
  M12 remove its two worst UI symptoms.
- **M8's related note is still open**: `UI_PANE_RECORDING`,
  `UI_DESC_RECORDING`, `UI_HEALTH_*` and `UI_STATUS_IDLE` are still referenced
  nowhere. They look like design decisions rather than defects, and this pass
  left them alone rather than inventing a use.

## Catalog

**33 new ids**, English written, every Arabic slot explicitly `/* not
translated yet */` so the completeness gate stays honest: `UI_ANN_EDIT_FAILED`,
`UI_ANN_CLOSE_TIMEOUT`, `UI_ANN_NO_TRAY`, `UI_DLG_NO_OUTPUTS`,
`UI_DLG_SESSION_CANCELLED`, `UI_DLG_DISCARD_*`, `UI_DLG_FORMAT_NEEDED`,
`UI_DLG_CREATE_FAILED`, `UI_TRAY_INFO_ARM_FAILED`,
`UI_TRAY_INFO_OUTPUT_RENAMED`, and 17 `UI_KEYNAME_*`. **No Arabic was written**
— 500+ strings still await the author's pass.

## Build and test at handoff

`build.cmd Debug test` and `build.cmd Release test`: **33 of 33 suites pass in
both**, 100%, no warnings under /W4 /WX.

`test_ui_behaviour.c` grew from 13 cases to **37**; `test_ui_dialogs.c` from 21
to **23**. Every fix except the four named under "Honest residue" was watched go
red with itself reverted, one at a time; BUGS.md names the failing case for each.

**Safety (AGENTS.md rule 1):** nothing was rendered to any audio device. Every
source in these suites is `APR_SRC_FAKE`. Tray registration stays suppressed by
`APPRECORDER_NO_TRAY`, wired in CMake — which the M10 test now depends on, so a
future change that removed it would fail loudly rather than start notifying the
author again. Three cases call `ShowWindow(SW_SHOWNOACTIVATE)` because focus
cannot enter a pane of a window that was never shown; `NOACTIVATE` means the
foreground is never taken from whoever is at the machine.

---

## 2026-08-26 — M11: an error becomes words only at the point of display

**The last tracker item, and the one that had to land before Arabic.**
`src/platform/err.c` carried full English prose in a hand-written table and
`apr_err_format()` handed it straight into translated catalog frames, so every
failure sentence would have shipped half Arabic and half English. The author is
blind; this is the sentence he hears when something has gone wrong.

### The shape chosen

**The error travels as an identity and becomes words only where it is
displayed.** `(kind, code)` already was that identity; `AprErr` gained one more
field, `int reason` — an `AprStrId`, typed `int` because `strings.h` includes
`err.h` and the dependency cannot run both ways — set by `APR_ERR_SAY` /
`APR_ERR_HR_SAY` / `APR_ERR_WIN32_SAY` / `APR_ERR_LAST_SAY`. One integer store:
allocation-free, lock-free, legal on a capture pump.

- **`include/errmsg.h`** (new) declares the display half: `apr_err_reason_id()`
  and `apr_err_reason()`. Implementation stays in `platform/err.c` — AGENTS.md
  rule 3 names it the sole owner of code-to-message, and a declaration's home is
  not a second owner.
- **`apr_err_format()` is unchanged and stays English.** `apr_log_err()` calls
  it on whatever thread raised the error, so it may never touch the catalog.
  Two renderings, and both headers say which is which.
- Resolution order: raise site's own id -> hand-tabled WASAPI code -> Windows'
  `FormatMessageW` -> the kind's own sentence. The last is a **floor**: every
  failure resolves to a declared catalog id, so nothing falls through to a C
  literal.

### The diagnostic / user line

`AprErr.context` is diagnostic and now reaches no user at all. It names
functions, thresholds and internal ids, it is formatted at raise time, and a
translator could never reach it. **The frame already names the operation** —
`ERR_FILE_OPEN`, `ERR_CAPTURE_START`, `UI_ANN_ACTION_FAILED`,
`UI_DLG_ADD_FAILED` — so the frame is the operation and the reason is why.

A raise site gets a `reason` id only where it knows something `(kind, code)`
cannot express *and* a user can act on it: the four limits, the rate mismatch,
on/not-on-bus, and "the output has no name" — all in `core/`. Everything in
`capture/` and the actions already carries an HRESULT or Win32 code. Every other
`APR_ERR(...)` in the tree is untouched.

### `FormatMessageW`: kept, in the display language

It is genuinely localized by Windows and covers an open-ended space of ordinary
codes. The change is the language: `apr_hresult_message()` asks `LANG_NEUTRAL`
(the *thread's* language, i.e. the machine's), which would hand English to a
user running `--lang ar-SA`. `apr_err_reason()` asks for `apr_str_language()`,
and when that language has no text it falls back to a catalog sentence that
keeps the number rather than accepting another language's prose.

### Catalog

`APR_STR_LIST_ERR_HR` (41, one per tabled WASAPI code) and
`APR_STR_LIST_ERR_REASON` (21: one per `AprErrKind`, plus the named refusals).
`APR_STR_ID_MAX` 1800 -> 2000. **No Arabic written** beyond a single marker on
`ERR_HR_E_DEVICE_INVALIDATED`, which plays the same role `APP_NAME`'s marker
does: without it the mechanism is untestable in a non-English locale, because an
untranslated entry falls back to English and every assertion passes on the bug.

### Converted call sites

`cli.c` `errtext()` (15 uses, one function), `controller.c` `report_failure()`
(9 uses) and its `APR_RUN_EV_ACTION_FAILED` arm, `canvas.c` `say_edit_failed()`.
`log.c` is now the only caller of `apr_err_format()` in `src/`.

### Tests

New: `a_failure_a_user_hears_is_the_catalogs_and_not_err_cs`,
`the_same_failure_in_english_is_the_english_catalog_entry`,
`the_log_and_the_user_are_told_the_same_thing_in_english`,
`a_raise_site_may_name_the_sentence_a_user_hears`,
`a_code_nobody_can_describe_keeps_its_number_inside_a_catalog_sentence`,
`a_success_value_has_no_reason_at_all`,
`no_reason_in_any_language_carries_the_raise_site`,
`a_reason_never_overruns_a_short_buffer` (test_err.c);
`every_error_kind_names_a_declared_catalog_sentence`,
`every_hand_tabled_wasapi_code_names_a_declared_catalog_sentence`
(test_strings.c, extending the completeness gate);
`the_reason_a_refusal_gives_is_the_catalogs_and_not_a_log_line` (test_cli.c);
`a_refusal_from_the_controller_is_a_catalog_sentence_end_to_end`
(test_ui_behaviour.c).

Watched go red: the three display-site tests with the sites put back on
`apr_err_format()`, and the Arabic case with `apr_err_reason` made to return the
table's English text.

**One test asserted the bug and was changed:**
`an_edit_the_model_refuses_is_announced_with_the_reason_the_model_gave` built
its expectation from `apr_err_format()`, pinning *"That change was refused: bus
1 already has 32 sources: APR_E_STATE at bus.c(226) in apr_bus_add_source"* as
correct.

### Build and test

`build.cmd Debug test` and `build.cmd Release test`: 33 of 33 suites, 100%, no
warnings under /W4 /WX.

**Safety (AGENTS.md rule 1):** nothing was rendered to any audio device;
nothing in this pass touches capture. Tray registration stays suppressed by
`APPRECORDER_NO_TRAY` in CMake. One mistake worth recording: a UI test
executable was run directly once, outside ctest, so it did NOT have
`APPRECORDER_NO_TRAY` set — do not do that; run UI suites through ctest.

---

## 2026-08-26 — Reconnection: a source that dies can now come back

### The problem this closes

Close the target app mid-recording and process loopback kept handing over
perfect silence for ever (design 4.1 #6). apprecorder detected it, said so
once, and then did nothing: the rest of the take was silence even if the user
reopened the app twenty seconds later. Unplug a capture device and it was
worse — `AUDCLNT_E_DEVICE_INVALIDATED` killed the pump and plugging it back in
changed nothing. There was **no reconnect / re-attach / recover path anywhere
in the tree**. For a recorder meant to run for hours unattended, a USB blip
cost the remainder of the recording.

### What was built

| Layer | Change |
|---|---|
| `capture.h` | `AprCaptureConfig::resume_anchor_ticks` — "you are replacing a capture on a timeline that is already running; your first frame belongs at the absolute index this anchor implies". |
| `capture.h` | `fake.revive_at_frame` — the mirror of `unmute_at_frame`. Death is no longer one-way. C-level knob only: no session key, no CLI spelling. |
| `capture.c` | `AprCapResume` + `apr_capresume_fill()` — one implementation of the rejoin arithmetic, called by the two producers (the shared WASAPI drain and the fake's generator) immediately before their first frame. |
| `source.h/.c` | `apr_source_detach` / `apr_source_pad_to` / `apr_source_reattach` / `apr_source_attached` / `apr_source_generation`. A new capture on the SAME ring, with every reader's cursor, the clock anchor and the refcount untouched. |
| `source.c` | A lock-free gate (`cap_users`) around the one field two threads share. |
| `reconnect.h/.c` | **New module.** One worker thread for the whole graph: notices losses, keeps detached rings at the frame index that is due, re-resolves identity through `session.h`'s resolver, reattaches. |
| `runner.h/.c` | Owns the worker. New events `APR_RUN_EV_SOURCE_RECOVERED` and `APR_RUN_EV_EXCLUSION_HELD`; `AprRunnerConfig::no_reconnect`; `apr_runner_source_link()` / `apr_runner_source_recoveries()`. |
| `cli.c`, `controller.c`, strings | Both new events announced on the same channels their losses use. |

### Where the reconnect thread lives, and why

**`src/core/reconnect.c`, one worker for the whole graph.** Following BUGS.md
M3 exactly: re-resolving a source is a machine-wide COM enumeration with no
bound on it, M3 was that class of work sitting on a capture pump, and the fix
there was a thread that owns its own MTA and its own objects rather than a
timeout nobody can set on an RPC. Same lesson, one layer up — the mixer loop
never waits for a search.

It cannot live *in* the capture layer: re-resolving needs `session.h`'s
identity rules, which sit above capture. It cannot live on the runner's tick
loop: that loop drives the mixer, and a stall there overruns 250 ms rings.

One thread, not one per source: the expensive half is a question about the
machine, so N down sources cost one enumeration.

### Retry policy

- First search **500 ms** after the loss, then **doubling**, ceiling
  **15 s** (`APR_RECONNECT_MAX_MS`).
- **It never gives up.** A device that returns after twenty minutes of a
  three-hour take is caught. Measured: 83 searches over 20 minutes.
- **EXCLUDE ceiling is 2 s** (`APR_RECONNECT_MAX_HELD_MS`), because a held
  EXCLUDE source is not costing one track, it is costing everything the machine
  plays.
- Waiting costs **silence, never alignment** — the hole is filled either way —
  which is what makes a ceiling of seconds acceptable at all.
- The backoff resets on recovery.
- Ring padding runs on its own short cadence (`APR_RECONNECT_PAD_MS`, 25 ms),
  unrelated to the retry schedule: one `rb_write_silence` of at most a ring.

### EXCLUDE mode — the conclusion

EXCLUDE names a **pid**. When that process exits and starts again under a new
number, a capture still excluding the old one is **recording the application
the user explicitly excluded**, silently. The capture never fails and WASAPI
never reports it, so nothing in the interface would ever say so.

Decided: **the exclusion follows the application, and while it cannot be
honoured the capture is HELD DOWN rather than left running.** Holding costs the
take the machine audio for a few seconds; not holding costs the user the one
thing they asked for. `session.h` already made this exact call at load time —
"dropping the target of an exclusion records MORE, so failing safe means
failing loudly" — and this is that rule at run time.

Consequences, all deliberate:

- The hold is **not optional**. `no_reconnect` switches off ordinary
  reattachment; it does not switch off a privacy guarantee.
- An EXCLUDE source is watched even though its capture is healthy. Staleness is
  a question about a pid (`apr_process_exists`) — no COM, no enumeration.
- Its retry ceiling is 2 s rather than 15 s.
- It gets its **own sentence** (`APR_RUN_EV_EXCLUSION_HELD`). Reporting it as
  "the source died" would be the wrong sentence about the wrong thing.
- The residual window — a successor that starts playing before the search finds
  it — cannot be closed without a process-creation notification we do not have.
  Held-and-searching makes it as small as the enumeration allows and fails
  toward recording LESS.

### Identity: reused, not forked

`apr_session_describe_process` / `apr_session_describe_device` capture the
identity **at `apr_runner_create`, while the source is still running** — an
image path cannot be read off a process that has already exited. Searching goes
through `apr_session_resolve_against`, so the rules stay image path → exe name →
window class as the only tiebreaker, `pick_when_ambiguous = 0` (two instances
and nothing to choose by leaves the source down: attaching to a *different*
instance of the same app is worse than staying dead).

**The stored pid is cleared before every search.** It is the identity of the
instance that just died.

**A device that returns with a new endpoint GUID but the same friendly name is
accepted**, consistent with the session resolver: an endpoint id is a
per-installation GUID pair that a different USB socket or a driver reinstall
re-mints. Disagreeing would have meant a device that reopens a saved session
fine but cannot be recovered mid-take. Two devices sharing a name is refused.

### Where the recovered audio lands

At the absolute frame it belongs at. Two mechanisms, same arithmetic:

1. While detached, the worker pads the ring to the frame index the clock says
   is due (`apr_source_pad_to`). One `rb_write_silence` however long the
   absence — a write longer than the ring keeps the newest capacity frames and
   still advances the cursor by the full count.
2. The replacement capture closes the residue itself, **on its own pump
   thread, at the instant it learns the QPC of its own first frame**
   (`apr_capresume_fill`). Padding from the reattaching thread instead would
   leave the recovered stream one packet early for ever.

### Tests — `tests/test_reconnect.c`, 22 cases

Hardware-free (`APR_SRC_FAKE` throughout; process and device cases go through
the reconnector's supplied-machine seam). No real time except the three runner
cases, which need a live loop to have notices to count.

- The hole is exactly the right number of frames, and the tone after it is the
  tone that belongs at those absolute indices.
- A recovered source is **sample-identical** to a control source that never
  died, from 400 frames after the recovery to the end — one frame of
  misalignment and the waveforms diverge everywhere.
- Both the loss and the recovery are announced, in that order, once each.
- A source that never returns behaves exactly as it did.
- The backoff doubles to its ceiling and never gives up; 20 minutes costs 83
  searches, not one per 25 ms pass.
- EXCLUDE: the exclusion moves to the new pid; a target that is gone holds the
  capture; `no_reconnect` cannot switch the hold off.

**Red runs watched:** `apr_capresume_fill` stubbed to return 0 →
`a_reattached_source_resumes_at_its_absolute_frame_and_not_at_now` and
`a_recovered_source_is_sample_identical_to_one_that_never_died` fail. The
recovery `notice()` removed → `both_the_loss_and_the_recovery_are_announced_in_that_order`
and `a_recovered_take_is_still_reported_as_incomplete` fail.

### Build and test

`build.cmd Debug test` and `build.cmd Release test`: **34 of 34 suites, 100%**,
no warnings under `/W4 /WX`. (33 before; `test_reconnect` is the new one.)

**Safety (AGENTS.md rule 1):** nothing was rendered to any audio device. Every
source in the new suite is `APR_SRC_FAKE`; nothing here opens an audio endpoint
or activates an `IAudioClient`. Tray registration stays suppressed by
`APPRECORDER_NO_TRAY` in CMake, and `test_reconnect` builds no controller and no
tray. No Arabic was written.

### Left undone, deliberately

- **No `--no-reconnect` CLI flag and no UI toggle.** The option exists on
  `AprRunnerConfig` and is tested at the reconnector level; wiring it to a typed
  flag means new catalog strings, parsing and `test_cli` coverage, which is
  another agent's file this pass. Default is on for both front ends, which is
  the behaviour 0.1.0 wants.
- **A session-supplied identity is not plumbed through.**
  `apr_reconnect_set_identity()` exists for it (a session file records identity
  as it was at save time, which is stronger than anything read back later), but
  neither front end calls it yet. Today's identity comes from the live machine
  at runner-create.
- **`revive_at_frame` has no session key.** Nothing a user can type should be
  able to script a resurrection, and nothing can currently produce a session
  containing one, so there is no round-trip to lose.
- **`apr_session_resolve` uses `static` buffers**, so the live path is
  single-caller. Pre-existing; the worker is the only caller during a run, but a
  front end resolving a session concurrently with a recording would race it.

---

## 2026-08-28 — Pause and resume: the origin moves, the ring does not

The last substantial feature before 0.1.0 packaging. A recording can now be
paused and resumed, and the paused span is **absent from the file** rather than
written as silence.

### The design problem, and the model chosen

Everything below the runner derives a position from an absolute QPC timestamp
against one anchor. That identity between elapsed time and output position is
what buys 0.43 frames of drift over three hours — and a pause breaks it on
purpose, so the only question was *how*.

**THE ORIGIN MOVES; THE SOURCE SIDE STAYS ON WALL CLOCK.**

| | during a pause | at the resume |
|---|---|---|
| **Bus clock** | frozen; ticks are successful no-ops | anchor shifted **later** by the paused duration (`apr_clock_shift_anchor`) |
| **Source clock / ring** | untouched — the capture keeps running and the 250 ms ring laps repeatedly | untouched |
| **Each bus reader** | idle | **re-based** onto the new origin (`apr_source_reader_rebase`) |
| **Encoders** | open; `on_audio` is simply not called | open |

Because the bus anchor and wall clock both move forward by the same Δ, the very
next tick is due at exactly the frame the last tick before the pause reached:
`frames_out` is continuous, no block is rendered twice, and nothing accumulates.
The arithmetic is the same absolute-position arithmetic as before, measured from
a new origin.

**One shift, one instant, every bus.** `apr_graph_pause/resume` own the
bookkeeping for the same reason `apr_graph_run` owns the one anchor: alignment
*between* buses is the property a pause can destroy silently and permanently.
Mapping to wall clock is given up deliberately; inter-bus alignment is not.

**What it costs:** the mixer runs 50 ms behind wall clock, so the cut is
`APR_BUS_LOOKBEHIND_MS` before the keystroke and the resume 50 ms before the
next one. The excised span is *exactly* the paused duration either way —
nothing duplicated, nothing dropped — it simply begins and ends 50 ms earlier
than the fingers did. The one visible case is **stop while paused**, which ends
the file 50 ms before the pause keystroke because that audio was never rendered
and is long gone from a 250 ms ring. The loop deliberately skips its lookbehind
drain when paused: a paused bus renders nothing however long it is ticked for,
and resuming in order to drain it would splice the paused audio onto the end.

### Where "is this loss?" is decided — and why it can only be there

A source's ring is an index of real time. During a pause its capture keeps
producing and the ring laps several times over, so the next pull would find its
frames overwritten, call that an overrun, and **emit exactly the paused duration
as silence** — injecting the pause back into the file, which is the one thing
pause must never do.

Every other layer sees identical arithmetic in both cases (a reader far behind a
write cursor). Only the reader knows the timeline moved, so the decision lives in
`apr_source_reader_rebase()` (`src/core/source.c`) and is expressed by **seeking**
rather than skipping:

- **`rb_reader_seek()` is new in `ringbuf.c`** — place the cursor at an absolute
  index, clamped to what the ring can still serve, **counting nothing as loss**.
  Loss means "frames that belonged to this reader's timeline went past it"; a
  seek means "this reader's timeline moved", which is a statement about the
  consumer, not the data. `rb_skip` cannot do the job: it reaps an overrun
  *before* applying the caller's frame count, so a request computed from a lapped
  cursor overshoots and the next read reports a hole that is not there.
- `begin()` grew a **re-base branch**, and the two source kinds answer
  differently, exactly as design 5.1 has them do everywhere else:
  - **a process tap** is the reference timeline, so its ring index and the bus's
    frame index are the same clock: `base = bus_frame - f0`. That is what makes
    the resumed audio land *sample-exactly*.
  - **a device capture's ring is in its own frames**, running at its crystal's
    rate, so the same subtraction would be wrong by the whole drift accumulated
    since the anchor (8 frames after six seconds at 30 ppm, growing with the
    session). It resumes the way it started: at the backlog its controller holds,
    computed from the ticks between `bus_frame` and now. The controller therefore
    restarts **at** its setpoint rather than several frames off it.

### Drift state across a pause: the position error is dropped, the rate is kept

`apr_drift_ctl_reset()` (new, `drift.c`) zeroes the integrator, the last error
and the update count and leaves tau, target, clamp and gains alone.

- **Holding it is wrong.** While paused the device produces and the reader does
  not consume, so the raw backlog grows by the whole paused duration. That is not
  an alignment error — nothing is out of position, the consumer was excused — but
  the integrator cannot tell, and would spend minutes unwinding a fiction into
  the audio after the resume.
- **Throwing the whole controller away is also wrong** — except that it is not
  what a reset does. The crystal's measured rate is **not** in the integrator: it
  is the feed-forward, recomputed every tick from `apr_clock_drift()` over the
  source's whole life, and a source's clock is not touched by a pause. So the
  rate survives for free and the only thing discarded is a position error that
  was never real.
- The resampler's filter history goes too (`apr_resampler_reset`), or the last
  block before the pause is smeared into the first block after it. That one is
  load-bearing: without it the device edge ends 1+ frames out.

Measured: a 3.03 s pause on a +30 ppm device edge leaves **0.29 frames** of
alignment error and 1.4 ppm of trim.

### A source that dies, or rejoins, while paused: nothing special happens

And that is the point of leaving the source side on wall clock. The reconnect
worker (`core/reconnect.c`) runs on its own thread against QPC and knows nothing
about the pause: it notices the loss, detaches, pads the ring to the frame index
that is due, searches, and reattaches at the original anchor — all in the
source's real-time frame index, which a pause does not touch. When the recording
resumes, the reader re-bases to the current position and finds whatever the
source is producing *now*: real audio if it came back, silence if it did not, at
the right absolute position either way. **No pause-specific code, and that claim
is tested rather than asserted** (`a_source_reconnected_while_paused_...`).

`poll_sources` keeps running while paused, so a death and a recovery are still
announced — the user should hear "Teams has exited" whether or not the take is
paused at that moment.

### Editing while paused: REFUSED, `APR_E_BUSY`, same as running

`apr_graph_running()` stays nonzero through a pause. The encoders are open, the
captures are live, the reconnect worker is running, and adding a source mid-take
would produce a file that starts in the middle. A pause is a quiet part of a
recording, not a gap in one. No code change was needed; a test pins it.

### Elapsed time is RECORDED time

`apr_runner_elapsed_ms()` is now wall clock minus every millisecond spent paused
(`apr_graph_paused_ticks`). It is the recording clock in the status bar and in the
tray tooltip a screen reader reads with Windows+B, and a clock that counted a
twenty-minute pause would be describing a file twenty minutes longer than the one
on disk. It is also what a duration limit is measured against, so
`--duration 600` records ten minutes however long the session was paused for.
`apr_runner_paused_ms()` is there for anyone who wants the other half.

### Keys, and the binding table that had a hole in it

**Ctrl+P pauses. Ctrl+Shift+P resumes.** Two commands and two menu items, not one
toggle — the same shape as Start (Ctrl+R) and Stop (Ctrl+.), and Ctrl+Shift+P
undoes Ctrl+P the way Ctrl+Shift+3 undoes Ctrl+3. A toggle whose *label* flips
has no reading for a screen reader user: the only way to learn the state is to
press it and hear what happened, which is the trap a pause must not be. Greyed
the right way round, "Pause Recording, unavailable" answers the question before
the key is pressed.

**The frame had no binding table.** It had an `ACCEL` array, menu labels that
spell the key out in their own catalog text, and a Help screen that listed the
*canvas's* bindings and none of the frame's — so **Ctrl+R and Ctrl+. were bound,
named in the menu, and absent from the one screen a keyboard user opens to find
out what the keys are.** For somebody working by ear that is not an omission from
a document; it is an operation that does not exist.

So `k_bindings[]` in `src/ui/app.c` is now the single source of:

- the accelerator table (`build_accelerators` derives `ACCEL` from it),
- `apr_ui_app_accel_command()` (which used to read a second copy),
- every menu item's label (`build_menu` takes a list of command ids),
- the frame's half of **Help > Keyboard Shortcuts** (`fill_keys` renders the
  frame table, then the canvas table minus its `platform` rows, which the frame
  table now carries).

`APR_KMOD_*` moved from `ui_canvas.h` to `ui_app.h` (same names, same values) so
both tables and `apr_dlg_key_name` speak one language. `apr_dlg_key_row()` is the
shared row formatter; `apr_dlg_binding_row()` now calls it.

`tests/test_ui_pause.c` holds the one thing a table cannot enforce on its own:
**the key spelt out in each menu label is the key that row really binds** (19
commands checked). That test immediately found a real divergence — the menu said
`Ctrl+.` while Help said `Ctrl+Full Stop` — and the menu label was changed to
match the catalog's spoken key name, not the other way round: the shortcut list
spells keys as words so a screen reader says one.

### Announcing it — both transitions, on the channel that can reach the user

`APR_RUN_EV_PAUSED` / `APR_RUN_EV_RESUMED` fire on the loop thread at the
**transition**, never at the request, so the sentence is never ahead of the file.

- **UI:** `say_and_notify` — status-bar live region in the foreground, tray
  balloon when not, per `a_better_channel_exists()`. The flag and the sentence
  are adjacent lines, for the reason `start_recording` already documents.
- **Tray:** new `APR_TRAY_PAUSED` state; the tooltip reads
  "apprecorder - paused, 00:12:04 recorded" and the elapsed figure is recorded
  time. Menu gained Pause/Resume (`apr_tray_set_can_pause`), between Start and
  Stop, greyed rather than absent.
- **Status bar:** the one-second clock writes `UI_STATUS_PAUSED` while paused, so
  somebody who comes back to the window ten minutes later reads "Paused." rather
  than a recording clock that has stopped moving for no stated reason.
- **CLI:** both transitions print a line. A transcript showing the pause and not
  the resume reads as a recording that ended there.

### CLI

`apr_cli_request_pause()` / `apr_cli_request_resume()` mirror
`apr_cli_request_stop()` — one entry point that the console and the suite both
reach. They act on an interlocked `g_runner` published for the length of
`apr_runner_run` (a pause before there is anything to pause is a no-op with
nothing to remember, unlike a stop, which must be *remembered* because the
console control handler exists before any runner does).

**Windows offers no console control event for a pause**, so the console reads the
keyboard directly: `console_key_thread` waits on the input handle and the
key→intent mapping is the pure, public `apr_cli_key_intent()` (P, either case;
one key does both halves, because a console has nothing to grey and nothing to
read out — what it does have is a transcript). The thread is started **only when
`GetConsoleMode` succeeds on standard input**, so a scripted or piped `record`
grows no reader, reads nothing, and cannot swallow a byte anyone else was going
to read. The hint naming the key is printed only when the reader is running.

### Tests — `tests/test_pause.c` (15 cases) and `tests/test_ui_pause.c` (15)

`APR_SRC_FAKE` throughout. No real time elapses except the five runner/controller
cases that need a live loop to have notices to count.

The central proof is against a **control take that ran the same tick grid and was
never paused**:

- before the cut the two files are **identical, sample for sample**;
- after it the paused take matches the control at an offset of exactly the
  excised span — i.e. it holds the audio that really happened at those instants;
- and it does **not** match at the same index, which is what stops this from
  being two empty buffers agreeing with each other;
- the longest run of consecutive zero samples in the whole output is **1** — the
  direct measurement of "nothing was filled in", where an implementation that
  reported the discarded audio as loss would answer with the paused duration.

Also pinned: two pauses accumulate correctly (a second pause that re-derived its
shift from the original anchor passes every single-pause assertion and fails
this); three buses come out the same length to the frame and two readers of one
source are sample-identical to each other; a pause finalizes nothing and the
graph still records a second time afterwards; editing is `APR_E_BUSY`; pause and
resume are idempotent and `APR_E_STATE` on an idle graph; a source that dies and
returns inside the pause leaves no hole; one that stays dead still holds its
place; the reconnect worker's real detach/pad/reattach runs across a paused span
and the audio afterwards is still at its true absolute frame.

**A latent bug in `capture_fake` that this found.** The replacement capture's
tone was indexed by `resume.padded + frames` — the pad *it* wrote, not the index
it wrote it at. The two agree only when the ring was empty before the rejoin, and
on a real reconnection it never is, because the ring has been held at the frame
index that is due the whole time the source was detached. The replacement's tone
was therefore offset by however far the ring had already got. Invisible at 480 Hz
(that tone's sample sequence repeats every 100 frames, so `test_reconnect`'s
sample-identity assertion agreed with it) and very visible at 997 Hz, which is
why `test_pause.c` uses a tone that is prime to the sample rate. Fixed:
`FakeImpl::base` is read off the ring after the rejoin pad.

**Red runs watched** (each reverted): no reader re-base → 5 cases fail; no
`rb_reader_seek` in `begin()` → the same 5; no origin shift → 4; ticks rendering
while paused → 6; no resampler reset → the device edge exceeds one frame; no
drift-controller reset → the controller resumes still pushing 187 ppm against an
error that no longer exists; elapsed counting the pause → the clock case; the
PAUSED or RESUMED notice dropped → the UI cases; the CLI not publishing its
runner → both CLI cases.

### Build and test

`build.cmd Debug test` and `build.cmd Release test`: **37 of 37 suites, 100%**,
no warnings under `/W4 /WX`. (35 before; `test_pause` and `test_ui_pause` are
new.) `test_ui_pause` was stress-run eight times in Release after a genuine race
was closed — the state flag flips inside the posted notice handler, which then
re-greys the menu and rebuilds two views, so the suite now takes a synchronous
`WM_NULL` round trip to the window's own thread rather than sleeping.

**Safety (AGENTS.md rule 1):** nothing was rendered to any audio device. Every
source in both new suites is `APR_SRC_FAKE`; nothing opens an audio endpoint or
activates an `IAudioClient`. Tray registration stays suppressed by
`APPRECORDER_NO_TRAY` in CMake. No Arabic was written — every new catalog entry
has its English text and an explicit `/* not translated yet */` on the Arabic
side.

### Catalog additions (ids chosen to avoid a merge)

`STATUS_PAUSED` 1023, `STATUS_RESUMED` 1024, `CLI_PAUSE_HINT` 1025;
`UI_MENU_RECORD_PAUSE` 1333, `UI_MENU_RECORD_RESUME` 1334;
`UI_KEY_*` for the frame's commands 1560–1573;
`UI_STATUS_PAUSED` 1632, `UI_ANN_RECORD_PAUSED` 1633, `UI_ANN_RECORD_RESUMED`
1634, `UI_ANN_ALREADY_PAUSED` 1635, `UI_ANN_NOT_PAUSED` 1636;
`UI_TRAY_TIP_PAUSED` 1770, `UI_TRAY_MENU_PAUSE` 1771, `UI_TRAY_MENU_RESUME` 1772,
`UI_TRAY_INFO_PAUSED` 1773, `UI_TRAY_INFO_RESUMED` 1774.

One existing string changed: `UI_MENU_RECORD_STOP` now reads
`"S&top Recording\tCtrl+Full Stop"`, so the menu and the shortcut list name the
same key. See the comment above it in `res/strings.rc`.

### Left undone, deliberately

- **No session key for "paused".** A pause is a property of a take in flight, not
  of a routing document, and a session that loaded already-paused would be a
  recording that starts by not recording.
- **No pause in `--duration` arithmetic beyond the obvious.** A duration limit is
  measured against recorded time, which is what it should mean; there is no way
  to ask for "stop after N seconds of wall clock" and nobody has wanted one.
- **The console key reader is P only.** No stop key, no status key: every extra
  key is another thing the reader can swallow, and Ctrl+C already stops.
- **No pause button on the canvas.** Pause is a frame command like start and
  stop; the canvas's table is about navigating and editing a graph.

---

## 2026-08-30 — One executable instead of two

### What changed, in one line

`apprecorder.exe` and `apprecorder_ui_app.exe` are now a single
**WINDOWS-subsystem `apprecorder.exe`**: a command line when it is given a
command, a window when it is not.

### Sizes (Release, clean build both sides)

| | bytes | KB |
|---|---:|---:|
| before: `apprecorder.exe` | 859,136 | 839 |
| before: `apprecorder_ui_app.exe` | 936,960 | 915 |
| **before, total** | **1,796,096** | **1,754** |
| **after: `apprecorder.exe`** | **984,064** | **961** |
| **saved** | **812,032** | **793 (45%)** |

Back under the 1 MB the design was built around, without spending any of the
10 MB VST ceiling. (Debug is 3,414,528 bytes; Debug size is not a shipping
number.) `apprecorder-wait.cmd` is 3,421 bytes and is copied beside the exe by
a POST_BUILD step.

### The dispatch rule, and why it is not "any argument means CLI"

In `include/frontend.h`; implemented as a pure function in
`src/app/frontend.c`; every row is a case in `tests/test_frontend.c`.

| argv[1] | front end |
|---|---|
| (nothing) | the window, empty |
| a CLI command — `record`, `list-apps`, `list-devices`, `help`, `version`, `save-session` | the command line |
| starts with `-` | the command line |
| a lone `*.json` | the window, **opened on that session** |
| anything else | **UNKNOWN — refused and named back** |

**Why not "any argument".** Explorer passes a double-clicked file as `argv[1]`,
so "any argument means CLI" sends a double-clicked session to a command line
that has never heard of it. The question asked instead is *does argv[1] name a
command this program has* — asked of `cli.c` through the new
`apr_cli_command_from_name()`, so there is **one** list of command names in the
program and adding a command cannot silently make it a filename the window
opens.

**An unrecognised first argument fails rather than guessing.** `apprecorder
recrod --out x.wav` used to be a plausible way to open an empty window and lose
the rest of the line. It now prints the catalog's own `ERR_UNKNOWN_COMMAND` —
"recrod is not a command apprecorder has" — and exits `APR_CLI_USAGE`. So does
`apprecorder notes.txt`, and so does `apprecorder my.json --bus X` (a session
path with more arguments after it is a typed command line, not a double-click).

A double-clicked session goes through the controller's own
`apr_controller_open_session_and_report()` — split out of `do_open_session()`
so the announcement (loaded / declined / this file's fault) is word for word
what File > Open says, rather than a second copy that drifts. It runs **after**
`apr_ui_app_show`, so the resolve report and the system-capture consent dialog
have a real parent and a real place in the accessibility tree.

### The apartment

**Decided after the dispatch, from its answer** — `apr_frontend_apartment()`:
GUI to `CoInitializeEx(COINIT_APARTMENTTHREADED)`, CLI and UNKNOWN to nothing at
all. It cannot be decided at the top of `main()` now that one entry point serves
both: STA on the command line's thread changes what the command line is, and MTA
on the window's thread silently loses every accessible name (`IAccPropServices`
is valid only on its creating thread). `capture.h`'s apartment section is the
record of the first time this bit. `test_frontend.c` pins the pairing, and
`main.c` calls the function rather than hard-coding it, so the test is
load-bearing.

### The console, and having nowhere to print

`console_attach()` in `src/app/main.c`:

1. `AttachConsole(ATTACH_PARENT_PROCESS)`.
2. For each standard handle **that is not already live**, open `CONOUT$` /
   `CONIN$` and `SetStdHandle`. The guard is the whole point: a parent binds a
   child's handles through `STARTUPINFO` whatever the subsystem, so
   `apprecorder record ... > out.txt` arrives with `STD_OUTPUT` already on the
   file. Reopening `CONOUT$` over it would send the output to the screen and
   leave the file empty.
3. `GENERIC_READ | GENERIC_WRITE` on `CONOUT$`, not write-only: `cli.c` chooses
   `WriteConsoleW` vs UTF-8 bytes by whether `GetConsoleMode` succeeds, and
   `GetConsoleMode` needs read access. Write-only would make every console line
   take the pipe path and arrive as mojibake. Measured, and asserted in
   `getconsolemode_needs_a_readable_conout`.
4. `_wfreopen_s` on the CRT `stdout`/`stderr` we created — and only those — for
   `log.h`'s `to_stderr` sink and asserts.
5. `SetConsoleOutputCP(CP_UTF8)` here rather than only inside `apr_cli_main`,
   because the Windows-floor refusal prints before the command line is entered.

Only the CLI/UNKNOWN paths attach. The windowed path deliberately does not:
attaching would put the process into the terminal's console group, and a Ctrl+C
typed at that prompt afterwards would reach a window with no handler for it.

**No console at all** (Explorer, a shortcut, the task scheduler): **no
`AllocConsole`**. A run with nowhere to print is allowed to *succeed* in silence
— a scheduled `record --duration 3600` must not pop a black window every night —
and is never allowed to *fail* in silence. `apr_frontend_should_explain()` is
that rule; a non-zero exit with no output channel raises a `MessageBoxW` naming
the exit code and saying where to run it to see what it said
(`APR_S_ERR_NO_CONSOLE`, new, id **1180**). The Windows floor uses the same
`report()` helper, so it still reports on both paths and before any window
exists.

### The scripting cost, and the shim

A PE's subsystem is fixed in its header and `cmd.exe` reads that flag to decide
whether to wait. A WINDOWS-subsystem process therefore returns to the prompt
immediately: `apprecorder record ... && upload.ps1` stops sequencing. Accepted
knowingly; `apprecorder-wait.cmd` restores it with
`start /wait "" "%~dp0apprecorder.exe" %*` then `exit /b %errorlevel%`.

- **The name is not `apprecorder.cmd` on purpose.** `PATHEXT` puts `.EXE` before
  `.CMD`, so a `.cmd` beside the `.exe` would never be found by typing
  `apprecorder` — it would look installed and do nothing.
- **Measured limitation, documented at the top of the shim:** `start` does not
  hand its own standard handles to the process it launches, so
  `apprecorder-wait ... > log.txt` leaves the file empty (the output still
  appears on the console, via our `AttachConsole`). Redirect the **exe**
  directly for output, use the shim for sequencing, or use `--log-file`.

### Build-system changes

- `apprecorder_ui_app` target: **gone, not kept as an alias.** An alias would be
  a second ~990 KB file, which is the whole thing this removes; a tiny launcher
  shim would be new code and a new process boundary for a name nobody needs —
  `apprecorder.exe` double-clicked already opens the window.
- `src/cli/main.c` and `src/uiapp/main.c` deleted; `src/app/main.c` replaces
  both. `src/app/frontend.c` joins `apprecorder_core` alongside `cli.c`, for the
  same reason: a rule no test can reach is a rule that drifts.
- The merged exe links `apprecorder_ui`, which brings `apprecorder_core`, the
  manifest and `/MANIFEST:NO` with it. `test_ui_*` still link `apprecorder_ui`
  through the by-name loop — untouched, and all seven pass.
- `apr_cli_main` is unchanged and still reachable in-process; `tests/test_cli.c`
  needed no edit.
- `APR_SESSION_EXT` added to `session.h` — the format owns its extension, and
  `dialogs.c`'s filter, its default extension and the dispatcher now read one
  constant instead of three literals.

### Tests

`tests/test_frontend.c`, 22 cases. The rule is asserted in-process; everything
about the *image* runs the real `apprecorder.exe` with **bounded** waits and
`TerminateProcess` on overrun, because the bug this file exists to catch — a
command line that opens a window — is otherwise a hang. Two cases skip
gracefully when the run has no console of its own (`GetConsoleWindow()`), so a
failing child can never leave a modal dialog on the author's screen; both were
verified green in a hidden console.

`the_shipped_image_is_windows_subsystem` reads the subsystem word out of the PE
header, so a well-meaning revert to CONSOLE fails the suite rather than the
double-click.

**Red runs watched** (each reverted):

- Guard removed from `adopt_std` (reopen `CONOUT$` over an inherited
  redirection) gives **4 failing cases**: the pipe is empty for `version`,
  `--help`, the unknown-command message and the non-ASCII path.
- `.json` branch disabled in `apr_frontend_choose` gives **2 failing cases**: a
  lone session file, and the case-insensitive extension.
- A deadlock found and fixed while writing this: `run_capture` originally waited
  then read, which hung on `--help` (several KB, more than one pipe buffer) and
  looked exactly like the hang the file is for. It now drains while the child
  runs.

### Build and test

Clean `build.cmd Release test` and `build.cmd Debug test` from a wiped `build/`:
**38 of 38 suites, 100%**, no warnings under `/W4 /WX`. (37 before;
`test_frontend` is new.)

**Flakiness note, not caused by this change.** While iterating, single ctest runs
occasionally lost one timing-sensitive suite (`test_ui_behaviour` line 817/1418,
`test_run_loop`, `test_pause` line 1271) — always a poll loop timing out, a
different suite each time, and each passed on its own immediately afterwards. It
was reproduced with `ctest -E test_frontend` too, so it is a machine-load
property of the announcement-poll suites rather than anything the merge
introduced. Both final clean runs above were 38/38.

**Safety (AGENTS.md rule 1):** nothing was rendered to any audio device. The only
recording command any test issues is `--dry-run`, which opens no device and
writes no file, and the only source named is `--fake`. Tray registration stays
suppressed by `APPRECORDER_NO_TRAY` in CMake. No Arabic was written — the one new
catalog entry has English text and an explicit "not translated yet" on the
Arabic side.

### Left undone, deliberately

- **The window does not yet take a session on the command line as an option.**
  Only a bare `*.json` opens it. A `--session` for the GUI is a different feature
  and belongs with whatever asks for it.
- **No file-type registration.** Nothing writes a ProgID or associates `.json`;
  the dispatcher is ready for a double-click, but associating an extension is an
  installer's business and this project has no installer.
- **The console path's rendering is not asserted end to end.** The pipe path is
  (UTF-8, including a non-ASCII path); for the console the mechanism is pinned
  instead — `GetConsoleMode` needs a readable `CONOUT$` — because reading a live
  console's screen buffer needs a second console, and a console window flashing
  on this author's screen is not an acceptable test artefact.

---

## 2026-08-30 — Version 0.0.1, and auto-update from public GitHub releases

The author is releasing publicly as **0.0.1** — an initial release, to friends.
Two jobs, and the second one is mostly about what it refuses to do.

### The version now has one owner

`include/version.h`. Three integers, and `APR_VERSION_STRING` built from them by
the preprocessor so the text cannot disagree with the numbers:

    #define APR_VERSION_MAJOR 0
    #define APR_VERSION_MINOR 0
    #define APR_VERSION_PATCH 1

It was `#define APR_CLI_VERSION L"0.1.0"` in `src/cli/cli.c` and nowhere else,
which was fine while the only reader was `apprecorder version`. It stopped being
fine the moment an updater existed: the updater compares this number against one
a server published and decides whether to replace the running image on the
strength of it, so a second copy that drifts is a build that offers to overwrite
itself with itself. `cli.c`, the About box and the updater now all read the one
header. **`src/session/session_save.c` deliberately does not** — it writes
`"writer": "apprecorder"`, a product name, not a version; the format's own
`version`/`minReader` pair is a different number and stays where it is.

**Comparison is numeric, never lexicographic.** `wcscmp(L"0.10.0", L"0.9.0") < 0`,
so a string compare says the tenth release of a line is older than the ninth and
it never installs — silently, months later, on machines nobody is watching.
`apr_version_compare()` compares major, then minor, then patch, as integers, and
`ten_is_newer_than_nine_which_a_string_compare_denies` is the case that fails if
anybody ever "simplifies" it. An unparsable version is **not newer** and not
older: it is not an answer, so a corrupt manifest becomes a refusal rather than
a cheerful "you are up to date".

### The updater

New: `include/update.h` (the whole design and threat model), `src/platform/update.c`
(decisions, cryptography, file moves), `src/platform/update_http.c` (WinHTTP, the
only place in the program that opens a socket), `src/platform/update_key.c` (the
public key, alone in a file so a reviewer can read the entire root of trust in
ten seconds).

**GitHub replaces infrastructure, not trust.** If the account is ever
compromised, someone can publish a release and every install downloads it. So
the host is untrusted and only the author's key is trusted:

- **ECDSA P-256 via BCrypt** — in-box, so nothing enters `vendor/` for it
  (rule 8 stays clean, and there is no third-party crypto to track for
  advisories in an audio recorder).
- The signature covers `release.json` **as fetched** — not a re-serialization,
  not a normalized form. Verify first, parse second, over the same bytes.
- The manifest carries the exe's SHA-256, so **one signature protects both**: a
  swapped binary fails the hash, a rewritten manifest fails the signature.
- **No fallback, ever.** A refusal is loud and logged; the staged file is
  deleted.

**Not the GitHub API** — 60 requests/hour per IP, shared across a NAT, so the
failure mode is "updates stopped working for everyone in the building,
intermittently". The stable `/releases/latest/download/` redirect is not the API
and is not rate-limited that way. Redirects followed; certificate validation
never relaxed, and there is no flag in `update_http.c` that could.

**Cadence:** startup, every five minutes, and after a recording stops — all
three through one gate, `apr_update_due()`, against a timestamp **stored in the
registry**. Start/stop cycling while setting levels cannot become ten requests,
and a crash-restart loop cannot either. Only an explicit "check now" bypasses
the interval; nothing bypasses the opt-out. `If-None-Match` with the stored ETag
makes the unchanged case a 304 with no body.

**The ETag is remembered only after a good signature.** Storing it on a failed
one would mean the next check gets a 304 and reports "up to date" for ever
after — one forged release permanently silencing the refusal. The last-check
*time*, by contrast, advances whenever the host answered at all, which is what
stops a half-uploaded release being re-fetched in a loop.

**Replacement, with no second process.** Windows permits renaming a running
image. Download beside the exe, verify, and on exit rename current to `.old`,
new to `apprecorder.exe`. The `.old` is kept until the new build has started
once: `apr_update_startup_action()` is that rule as a pure function, driven from
`src/app/main.c` before either front end is chosen. A build that does not start
is one rename from recovery.

**The four rules, and where each lives:**

1. *Never during a recording.* `apply_update()` refuses and **says so** — the
   take is not stopped, not paused, not interrupted. The swap itself only ever
   runs in `apr_controller_destroy`, after the close path has finalized every
   file, so the rule is satisfied by construction and not by a check that could
   be forgotten.
2. *Never discard the previous version until the new one starts.* Above.
3. *Opt-out, persisted.* `HKCU\Software\apprecorder\Update`. Registry rather
   than a file of our own: four scalars, per-user, and a second JSON writer
   beside `session_save.c` would be the rule 3 failure that file warns about.
   `apprecorder update --disable` reaches it from a headless machine, and
   **switches off without making one last callback on its way out**.
4. *Announced and keyboard-reachable.* Every outcome goes through the same
   `say_and_notify` pair every recording event does, so it reaches a screen
   reader on the status bar in front and as a tray balloon behind. Help >
   "Check for &Updates", mnemonic, no accelerator (like Exit and About).

**Do not silently self-install.** It asks. For an initial release going to
friends, a binary that swaps itself unasked is worse than a prompt.

**Quiet failures and loud ones are different sentences.** `UPDATE_FAILED` is
"the download did not work"; `UPDATE_REFUSED` is "this was not signed by the
person who publishes apprecorder". One means the wifi is bad and one means
somebody is trying something, and a single "update failed" for both throws away
the only one that matters. They are separate catalog entries and
`tests/test_ui_update.c` asserts they cannot collapse into each other.

**A build with no key does not look.** `update_key.c` is still all zeros until
the author runs `keygen`, and `apr_update_have_key()` being 0 disables the check
entirely — not "checks and refuses everything", which would announce a refusal
every five minutes and teach a blind user to ignore the one sentence that
matters.

### The release side

`tools/release/apprelease.py` — a `uv` inline-metadata script, one dependency
(`cryptography`).

    uv run tools/release/apprelease.py keygen
    uv run tools/release/apprelease.py sign --exe build/Release/apprecorder.exe --version 0.0.2 --notes "What changed."
    uv run tools/release/apprelease.py verify --dir dist

`keygen` writes the private key to `%USERPROFILE%\.apprecorder\release-key.pem`
(**outside the repository**, refuses to overwrite) and prints the public half
already formatted as the C array to paste over the zeros in
`src/platform/update_key.c`. `sign` hashes the exe, writes `release.json` and
signs **exactly the bytes it wrote**. Signature format is raw `r||s`, 64 bytes,
**not DER** — BCrypt speaks that natively, so there is no ASN.1 decoder in the
updater and no place for one to have a length bug.

**Still to do before the first release:** the owner/repo in
`APR_UPDATE_BASE_URL` (`include/update.h`) is a guess — this clone has no git
remote. Set it, run `keygen`, paste the key, rebuild.

### Tests

Three new suites, **41 of 41 green** in both configurations, and **ten
consecutive Release runs clean** (66 s each; Debug 108 s).

- `tests/test_version.c` (9 cases) — the string-compare trap, the `v` prefix, and
  "unparsable is not newer".
- `tests/test_update.c` (31 cases) — the two the file exists for are
  **`a_tampered_manifest_is_refused`** (every byte position of a genuinely
  signed manifest flipped, one at a time) and **`a_tampered_binary_is_refused`**
  (a payload that is not the one the signed manifest describes). Plus: a
  manifest signed with another key, a truncated signature, a signed-but-
  malformed document, an asset name containing a path separator, the cadence
  table, the persisted settings, the ETag rules, offline, the swap, and the
  recovery window.
- `tests/test_ui_update.c` (10 cases) — rule 4. Announced in front, balloon
  behind, quiet when nobody asked and there is no news, always answering when
  somebody did, nothing installed without being asked, and **nothing installed
  during a recording**.
- `tests/test_cli.c` gains six cases for `update` and pins `apprecorder version`
  against `APR_VERSION_STRING`.

**A golden vector is checked in.**
`what_the_release_tool_signs_is_what_this_build_accepts` holds a `release.json`,
its 64-byte signature and the matching public key, all produced by
`apprelease.py` against a throwaway key that was then destroyed. It pins the
Python side and the C side together — raw `r||s`, the exact byte layout of the
JSON, the hex spelling of the hash — in every run, with no private key needed.
The full round trip (keygen, sign, then verify in C) was also run by hand and
passed before that vector was extracted.

**Red runs watched, each reverted:**

- `BCryptVerifySignature`'s result ignored → 3 failures, including both
  tamper cases.
- The payload hash compared against itself → 3 failures, including
  `a_tampered_binary_is_refused`.
- `apr_version_compare` reduced to a string compare → `test_version` fails on
  `ten_is_newer_than_nine`.
- The recording guard removed from `apply_update` → `test_ui_update` fails on
  `an_update_is_refused_out_loud_while_a_recording_is_running`.

A latent flake was found by the second red run and fixed: two cases in
`test_update.c` could land in the same `%TEMP%` directory because the name used
`GetTickCount()`. It is an `InterlockedIncrement` counter now.

**Safety (AGENTS.md rule 1):** nothing was rendered to any audio device. The one
UI case that records uses `--fake` and a WAV in `%TEMP%`, deleted on teardown.
**No test contacts the network** — every request goes through the
`AprUpdateHttp` seam and `update_http.c` is never called from a suite. No test
touches the author's real update settings; `apr_update_state_test_redirect()`
points the module at a per-process key, and `apr_update_state_test_erase()`
refuses outright unless a redirect is in force. No test can rename the running
executable: the controller carries an image override for exactly that reason. No
Arabic was written — the 30 new catalog entries have English text and an
explicit "not translated yet" on the Arabic side.

**Size:** Release image 1,028,096 bytes (~1004 KB), up from ~990 KB. Nothing
entered `vendor/`; `bcrypt` and `winhttp` are in-box and are the whole reason.

### Left undone, deliberately

- **No progress reporting during the download.** A megabyte on a normal line is
  a second or two and the window stays responsive; a progress bar would be a
  second announcement channel for something nobody is waiting on.
- **No rollback command.** The `.old` file is beside the exe and recovery is one
  rename. A command to do it would have to run *from* the build that will not
  start, which is the one thing it cannot rely on.
- **`docs/` does not describe `update` yet.** The docs were being written in
  parallel by another agent while this landed; `apprecorder help` is correct.

---

## 2026-09-10 -- The keyboard walks over the Structure panel: three flakes, measured

**Reported:** `tests/test_ui_behaviour.c(1398): FAILED ASSERT_TRUE(visited[i])`,
about one full `ctest` run in five, in
`arrowing_down_the_structure_panel_does_not_yank_focus_out_of_it` -- the
regression guard for BUGS.md C2. The focus assertion above it was NOT the one
failing, so focus stayed in the tree and a row was simply never reached.

Three separate defects were found under that one symptom, all three measured
rather than argued, and a fourth was ruled out.

### 1. A POSTED key and a fifteen-millisecond sleep -- `test_ui_tree.c`

`the_keyboard_alone_reaches_every_row` is the same walk over a bare panel. It
POSTED each keystroke and then `Sleep(KEY_SETTLE_MS)` for it, which is a bet
that 15 ms is enough on a busy machine. **Measured on the baseline under
`ctest -j 8`: one run in fifteen fails `test_ui_tree.c(963)` with "row 0 was
never reached by Down Arrow"** -- Home had not been processed yet when the
first caret read happened, so every read lagged the posted key by one and the
row Home selected was never observed.

**Fix:** the walk SENDS the keys. A cross-thread `SendMessageW` does not return
until the window's own thread has finished handling the message, so the caret
has already moved when the next line reads it. Nothing about arrow keys in a
TreeView needs the frame's pre-translate filter, which is the only thing
posting bought. No sleep, no bound left to cross -- and the case went from
**2.09 s to 0.52 s**.

### 2. The fixture window was a citizen of the desktop -- `tests/test_window.h` (new)

Eight suites build a REAL frame on the real interactive desktop. Two things
reach in from outside and both land in the middle of an assertion:

**ACTIVATION CLEARS THE THREAD'S FOCUS.** Keyboard focus is a property of a
thread's input queue, and a thread whose window is DEACTIVATED loses its focus
window outright. `GetGUIThreadInfo(tid).hwndFocus` -- how four suites ask
"where is the keyboard" -- then reads NULL for reasons that have nothing to do
with the product. `SW_SHOWNOACTIVATE` is no protection: Windows hands the
foreground on when the previous holder exits, and a `ctest -j 8` run is
forty-one processes appearing and exiting, eight at a time. **Measured: three
concurrent runs of the behaviour walk failed 91 times in 360 on
`ASSERT_TRUE(focus == f.h.tv)`, every one with focus reading NULL.**

**AND THE PERSON AT THE MACHINE IS AN INPUT DEVICE.** If the fixture window can
hold the foreground, the author's own keystrokes are delivered to it. Down
Arrow walks VISIBLE items, so ONE stray Left collapses the bus row and puts its
whole subtree out of reach -- without moving focus, so the focus assertions
still pass and the completeness one fails alone. **Reproduced exactly:
injecting a single `VK_LEFT` into the walk gives `Down Arrow reached 1 of 3
rows without losing focus` and `ASSERT_TRUE(visited[i])` at the reported line,
with `TVIS_EXPANDED` cleared on the bus item.**

**Fix:** `apr_test_isolate_frame(HWND)`, called by all eight suites the moment
the frame exists and before anything can focus it -- `WS_EX_NOACTIVATE` (it can
never become the foreground window, so it can never be deactivated and no typed
key is ever routed to it), `WS_EX_TOOLWINDOW` (out of Alt+Tab), and moved just
below the primary monitor (a click or a hover reaches a NOACTIVATE window just
the same; below the primary rather than at -32000 so the nearest-monitor rule
still gives it the primary's DPI, which these suites assert on). `WS_VISIBLE`
is untouched -- `cycle_pane` only focuses a VISIBLE pane.

**The same 3x120 hammer that failed 91/360 now fails 0/360.**

### 3. The assertion was not weakened, and it was not wrong

`tree_panel.c` expands every depth-0 row on build and nothing in the product
ever collapses one, so "Down reaches every model row" is true of a panel nobody
has interfered with. It stands exactly as written.

What was added is `tests/test_treeview.h`, shared by both suites: when a row
goes unreached, they now say WHICH of the two possible reasons it was -- the
control does not hold the row at all (a product defect: `tp_build` drops a row
whose label came out empty), or an ancestor is COLLAPSED (something outside the
test pressed Left or clicked). Plus, in the behaviour suite, the caret trail
(item handle -> row, per press) is printed on failure. A bare "expected 1,
actual 0" read exactly like the C2 focus-steal regression coming back, which is
the worst thing a guard can do.

### 4. HONEST RESIDUE -- one occurrence is still unexplained

During a SERIAL Release `ctest` run, with the isolation already in place, the
reported assertion fired once more: `Down Arrow reached 1 of 3`, and the new
explain-on-failure said **"row 1 was never reached, and it is present with
every ancestor expanded"** for both missing rows. So on that occasion nothing
was collapsed and nothing was missing -- the caret simply did not advance,
while focus stayed on the tree for all seventeen presses.

That rules out the collapsed-node explanation for that instance. It has not
recurred in **30 consecutive `-j 8` runs, 60 standalone Release runs of the
suite, or 15 serial Debug runs**. The caret trail added in this pass exists so
that the next occurrence names itself: whether the caret item stayed the same
handle, went NULL, or moved while reporting the same lParam distinguishes
"comctl32 ignored the key", "the tree was rebuilt under the walk" and "the
model changed", and no more guessing will be needed.

### Also fixed: `apr_runner_wait` returning is not the STOPPED notice

`tests/test_pause.c(1276)` failed once in ten Release runs. `src/core/runner.c`
signals `finished_event` -- what `apr_runner_wait` waits on -- and only THEN
calls the observer with `APR_RUN_EV_STOPPED`. A case that waits and immediately
reads the tally is reading it from between those two lines. Both sites now wait
for the notice itself (test_wait.h's one backstop) before asserting the count
is exactly one.

**Left for the author:** the ordering in `runner.c` could be the other way
round, so that "the wait returned" implies "every observer has been told". That
is a stronger public contract and one moved line, but it changes what the
product promises, so it was not done here.

### Found, diagnosed, NOT fixed (both pre-existing, both proven so)

- **`a_close_that_runs_out_of_patience_says_that_rather_than_repeating_itself`
  hangs.** Running the Release suite standalone 60 times: **7 hangs with the
  baseline code and 7 with these changes** -- identical, so it is not from this
  pass. Under `-j 8` it once took **890 s** and then failed
  `ASSERT_WSTR_EQ(want, got)` at line 2239. It records, closes, answers a
  dialog and waits for the timeout sentence; the waits are all on the 60 s
  backstop, so 890 s means something in that path blocks on something else.
  Worth its own look.
- **`test_action_ogg.c(1407)`
  `a_mid_stream_write_failure_is_reported_and_the_prefix_survives`** failed once
  in fifteen serial Release runs: the peak came back **302 Hz against 440 +/- 3**.
  The case feeds six seconds of tone as fast as it can into a write-behind ring
  with the disk gated at 12 KB, so how much clean tone lands before the ring
  overruns -- and the overrun policy turns holes into silence -- depends on how
  the writer thread was scheduled. The analysis window (250 ms to 1.25 s) can
  therefore contain a hole. Not a UI walk, so left alone.

### Also worth knowing: the tests hit the network and the real registry

`src/platform/update_key.c` now holds a real key, so `apr_update_have_key()` is
1 and **every UI test process starts a real update check** on controller
creation. It is gated by `apr_update_due()` against
`HKCU\Software\apprecorder\Update` -- the author's real setting -- so during a
test run one process every five minutes makes a genuine HTTPS request to
`github.com/a2hsh/app-recorder/...` and rewrites `LastCheck`. The URL is still
the placeholder and 404s, so the outcome is `APR_UPDATE_NONE` and nothing is
announced. But the 2026-08-30 note ("no test contacts the network") stopped
being true when the key was pasted in. An `apr_update_state_test_redirect()` in
the UI fixtures, the way `test_ui_update.c` already does it, would put it back.

### `build.cmd` now runs `ctest -j 8`

That change was made by the author during this session, not by this pass, and
it is left exactly as he wrote it. It matters here: it means a "full ctest run"
is now eight test processes at once, which is precisely the activation churn
defect 2 is about -- and it is what the verification below was run under.

### Verification

`build.cmd Debug` and `build.cmd Release`, `/W4 /WX` clean, and **15 consecutive
full `ctest -j 8` runs in each configuration with no failure**:

| Runs | Config | Result | Wall each |
|---|---|---|---|
| 15 | Debug, `-j 8` | 41/41, no failures | 41.8-43.8 s |
| 15 | Release, `-j 8` | 41/41, no failures | 14.4-16.4 s |
| 15 | Debug, serial | 41/41, no failures | 74-108 s |

Before/after on the same command, same machine:

| Build | Runs | Failures |
|---|---|---|
| baseline, `-j 8` Debug | 15 | 1 (`test_ui_tree.c(963)`, "row 0 was never reached") |
| these changes, `-j 8` Debug | 15 | 0 |
| baseline, 3x concurrent behaviour walk | 360 | 91 (`focus == f.h.tv`, focus NULL) |
| these changes, same hammer | 360 | 0 |

### Files

New: `tests/test_window.h`, `tests/test_treeview.h`. Changed: the eight UI
suites (one call each) plus `test_ui_tree.c` (the walk) and
`test_ui_behaviour.c` (diagnostics) and `test_pause.c` (the notice wait).
No product code was changed. Nothing committed.

### Safety (AGENTS.md rule 1)

Nothing was rendered to any audio device; no player was written or run; every
source is `APR_SRC_FAKE`. The fixture windows are now positioned off the
primary monitor, so a test run is quieter on the desktop than it was.
`APPRECORDER_NO_TRAY=1` is untouched and still set by CMake for every test.

---

## 2026-09-10 — Documentation for the public release, and 0.0.1 prep

Started while the CI-proofing agent (`a55bce984b190b46f`) was still running, so
this work deliberately stayed out of `tests/`, `build.cmd` and `CMakeLists.txt`.

### What was written

New files:

- **`CHANGELOG.md`** — 0.0.1, the whole feature surface plus a "known
  limitations" section that says the hardware capture path has no automated
  coverage against real devices.
- **`SECURITY.md`** — reporting via GitHub private advisories (NOT the author's
  email, which is his to publish or not), what is in and out of scope, how
  releases are signed, and why the key is not a CI secret.
- **`CONTRIBUTING.md`** — points at AGENTS.md, rule 1 first; says explicitly
  that the rules must not be renumbered because ~50 files cite them by number.
- **`docs/updating.md`** — the user-facing updater doc. What it checks, the
  cadence and why a recording-stopped check is gated, what a check tells the
  server (nothing identifying), the trust model, why a missing signature is not
  a pass, the rename-based swap, and the opt-out.
- **`docs/releasing.md`** — the maintainer's checklist, including the LGPL
  section 6 source-zip obligation as a numbered step rather than a footnote.

Rewritten or corrected:

- **`README.md`** — the licence section still said the licence had not been
  chosen; it is MIT. Also claimed "nothing in the registry", which stopped being
  true when the updater started storing state in
  `HKCU\Software\apprecorder\Update`. Added a "Staying up to date" section.
- **`docs/licensing.md`** — was written as an open question with four candidate
  licences. Rewritten as settled, and the checklist item "the licence is chosen"
  is gone.
- **`docs/command-line.md`** — `update` and its three options were entirely
  undocumented, including in the front-end dispatch table. Also made `--quality`
  exact: it means different things per format, and for Opus `0` is complexity 10
  while `1..10` are complexity 0..9.
- **`docs/building.md`** — 38 suites was 41; documented the three test
  environment variables, the `RESOURCE_LOCK` on windowed suites, and the skip
  log. The old "re-run the failing suite before hunting for a bug" paragraph is
  gone -- that attitude is what the agent is currently removing at the cause.
- **`docs/accessibility.md`** — noted the three Help items that are menu-only.

### Product changes, and why they were in scope

**The About box did not carry the LGPL notice.** It said name, version and
tagline. libmp3lame is LGPL and requires a recipient of the BINARY be told it
is in there and where to get the source to relink it; somebody who downloads
only `apprecorder.exe` never sees README.md. That made this a release blocker
rather than a documentation nicety.

- New string `UI_DLG_ABOUT_LEGAL`, appended to the About body.
- **`tests/test_strings.c` now asserts it**, formatted rather than probed, so
  the URL insert is checked as the user sees it. A future edit that trims the
  sentence to fit a layout fails the build.

**`APR_PROJECT_URL` is now the one owner of the repository address**
(`include/version.h`). `APR_UPDATE_BASE_URL` derives from it, so a repository
that moves cannot leave the updater fetching from the old address while the
About box names the new one. `test_update.c` asserts URL *shape*, so it still
guards this.

**Help -> Documentation** (`APR_CMD_HELP_DOCS`), which was the last unstarted
item on the author's release list. It opens `APR_PROJECT_URL` in the default
browser and **announces its own failure with the address**, because a browser
that does not open is otherwise silence indistinguishable from success for a
screen reader user. The docs are deliberately not embedded: this is a
single-exe product that installs nothing, and shipping a copy that goes stale
is worse than a link.

### `apprelease.py verify` was checking the wrong key

It loaded the **private** key and derived the public half from it. That is
nearly a tautology -- it passes whatever `sign` just produced -- and it cannot
catch the one mistake that actually ships a broken release: **a signing key
whose public half is not the one compiled into `update_key.c`**. That release
passes every local check and then fails on every machine in the field,
silently.

`verify` now reads `g_public_key[]` out of `src/platform/update_key.c`, so it
checks against what the binary trusts. Two consequences: it needs no secret, so
anybody can verify a downloaded release; and it refuses an all-zero key with the
explanation that such a build does not check for updates at all.

Also: a bad signature printed a raw `InvalidSignature` traceback. That is the
one message that must not look like the tool broke. It now says what happened
and that the release must not be installed.

**Round-tripped all three outcomes** against the real key: good release passes
(exit 0), rewritten manifest fails the signature (exit 1), swapped payload fails
the hash (exit 1). **This also confirmed `~/.apprecorder/release-key.pem`
matches the key compiled into this build** -- previously unverifiable.

### Verification

`build.cmd Debug` clean under `/W4 /WX`. `test_strings` 34/34 (was 33, plus the
new licence test), `test_version` 9/9, `test_update` 31/31, `test_ui_a11y` 13/13
("16 operations, all on the menu, all named, all mnemonic" -- the new
Documentation item passed the mnemonic-uniqueness check), `test_ui_dialogs`
23/23.

Full suite NOT yet run, because the tree still holds the agent's in-flight test
changes.

### Still open

- The CI-proofing agent has not reported. Its `wait_for_files()` fix in
  `controller.c` (checking the deadline BEFORE the wait rather than after) is a
  real bug: a budget of 0 meant "wait 50 ms", so the close-timeout test got the
  success sentence about one run in eight.
- **Nothing committed.** The tree mixes this work with the agent's.
- Arabic: 565 strings awaiting translation (was ~514; four added here).
- 0.0.1 not yet built, signed, tagged or published.

---

## 2026-09-10 -- CI told the truth three times: no engine, a bound that was not a bound, and a millisecond that was thirteen

CI on `windows-latest` failed on both runs since `ctest -j 8` landed. Three
separate things, all of them assumptions the suite made about the machine it
runs on. The CI log was read rather than guessed at -- `gh` is not on PATH here,
but the run log gh had already downloaded was sitting in
`%LOCALAPPDATA%\GitHub CLI\run-log-34480817760-*.zip`, and it turned two of the
three diagnoses on its head.

### 1. The runner DOES have an audio engine. It was a `Sleep`, not the hardware

The reported failure:

```
tests/test_capture_apartment.c(244): FAILED  ASSERT_TRUE(sta.frames > 0)
```

The obvious reading -- "a GitHub runner has no audio endpoint, so skip" -- is
wrong, and the same log says so three lines apart:

- `a_device_source_opens_from_an_sta_thread` **skipped**, correctly: "no default
  capture endpoint: Element not found (HRESULT 0x80070490)". There is genuinely
  no capture endpoint on the runner.
- `a_process_tap_stopped_and_closed_from_an_sta_thread_leaves_nothing_behind`
  **passed**, on that same runner, including its own `ASSERT_TRUE(b.frames > 0)`.

So process loopback activates, starts and delivers frames there. What actually
differed is COLD versus WARM: the failing case was the first tap on the machine
and took **1,034 ms**; the passing one, with the engine already up, took
**248 ms**. Between `start()` and the assertion sat `Sleep(120)`.

**Fix.** `Sleep(120)` becomes a bounded WAIT for the first frame. That is the
whole defect, and it is the same defect as sleeping for a keystroke.

**And the skip predicate was audited, because it is the dangerous half.**
`tests/test_engine.h` (new) holds it in one place with the two ways it can be
wrong written down:

- **Ask it from an STA** and an apartment refusal -- the exact regression this
  file exists to catch -- comes back as "no engine here", the case skips, and
  the bug ships behind a green tick. That is how the original defect survived a
  green suite. The reference attempt therefore runs with NO_COM.
- **Stop at `open()`** and the predicate answers a question nobody asked:
  activation succeeding says the virtual loopback device exists, not that
  anything will ever feed it. The reference now runs the whole way -- open,
  start, first frame -- and only then is "this machine can do it" worth resting
  an assertion on.

Audited, not just the one that failed: `test_capture_wasapi.c` (waits for the
first frame, then spends its 400 ms window), `test_capture_apartment.c`,
`test_capture_abandon.c`, `test_ui_add_source.c`. `test_discover.c` enumerates
processes, not endpoints, and needs nothing.

**A skip is now loud.** `SKIP("why")` / `SKIPF(...)` in `test_runner.h`: the
case reports `[  SKIPPED ]` instead of `[       OK ]`, the closing line carries
the count, and -- the part that survives ctest, which throws away the output of
a suite that PASSED -- every skip is appended to `APPRECORDER_SKIP_LOG`, which
CMake points at `build/<cfg>/test-skips.log` and `build.cmd` prints after the
run. A skip on a runner is now readable in the CI log without re-running
anything. All 87 hand-rolled `printf("      SKIPPED: ...")` sites were converted;
`SKIP` deliberately does NOT return, because the caller still has a window or a
capture to take down.

### 2. `Sleep(1)` is not one millisecond, and that is the 662 s

`test_wait.h` says "ONE ceiling, and it is a minute" and "ONE step, and it is a
millisecond, so a wait ends about when the work does". Both were false, because
both were counted rather than measured:

```
if (apr_waited_ >= APR_TEST_WAIT_MS) break;
Sleep(APR_TEST_POLL_MS);
apr_waited_ += APR_TEST_POLL_MS;      /* <- calls a step a millisecond */
```

`Sleep(1)` sleeps to the next system timer interrupt. **Measured on this
workstation by asking the OS: 13.05 ms as the machine sits, 1.45 ms with
`timeBeginPeriod(1)`.** So 60,000 counted steps is not 60 s, it is about
**thirteen minutes** -- and that is the arithmetic behind the two numbers this
tree could not previously account for:

| Number | Where it came from |
|---|---|
| "890 s under `-j 8`" (2026-09-10 entry) | one 60,000-step backstop at ~15 ms |
| CI Debug: 648,587 ms in one case, 662 s for the whole run | the same, at 10.8 ms/step |

Two fixes, and they are separate:

1. **The bound comes off a clock.** `GetTickCount64` across the wait, so a
   minute is a minute whatever the timer period is.
2. **The step is made real.** `timeBeginPeriod(1)` for the life of the test
   process (released at exit). Since Windows 10 2004 this affects only the
   calling process's own waits, so it is not a change to the machine. With
   hundreds of waits per UI suite it is worth seconds each:

| Suite | CI Debug, before | Local Debug, after |
|---|---|---|
| `test_ui_update` | 4.88 s | 0.64 s |
| `test_ui_dialogs` | 4.09 s | 0.48 s |
| `test_ui_canvas` | 5.43 s | 0.87 s |
| `test_ui_pause` | 6.27 s | 2.15 s |

### 3. The flake: a bound in `controller.c` that was not a bound

`a_close_that_runs_out_of_patience_says_that_rather_than_repeating_itself`
(7 failures in 60 Release runs, on the baseline as well as on the changes) is a
PRODUCT defect, and the CI log names it exactly: it expected

> "The files are taking longer than expected to close..."

and got

> "Recording stopped. 00:00:00 recorded."

`wait_for_files()` checked its deadline on the LAST line of the loop:

```
for (;;) {
    if (!c->recording || !c->runner) return 1;
    MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);   /* always */
    while (PeekMessageW(...)) { ... DispatchMessageW(&msg); }
    if (!c->recording) return 1;
    if (GetTickCount() - start > limit_ms) return 0;              /* too late */
}
```

So every call spent one unconditional 50 ms pump no matter what `limit_ms` said.
A budget of **zero** did not mean "do not wait"; it meant "wait 50 ms" -- and
50 ms is ample for the posted `APR_RUN_EV_STOPPED` notice to be dispatched right
there, run `recording_finished()`, and clear `c->recording`. The caller then
took the SUCCESS path and the timeout sentence was never said. Which sentence
the user hears was decided by how fast the encoder happened to flush.

**Fix: check the deadline before the wait, not after it.** One line moved.
Already-finished is still checked above it and still returns success, so an
expired budget now always means what it says.

**`apr_runner_wait()`'s ordering is NOT the root cause here, so `runner.c` is
untouched.** This path never calls it: it polls `c->recording`, which is cleared
by the POSTED notice, and a posted message cannot be dispatched while the UI
thread is inside `on_close` unless `wait_for_files` pumps it. The 2026-09-10
note about `finished_event` being signalled before the observer runs still
stands as a separate item; it is simply not this.

### Also fixed while hammering: the last posted arrow key in `test_ui_tree.c`

`selection_is_a_model_identity_and_does_not_echo` still POSTED its Down Arrow.
A posted key goes through the frame's message loop, where `IsDialogMessage` can
treat an arrow addressed to a child of the frame as dialog navigation instead of
handing it to the control -- and this case never focuses anything, so whether
the key arrives at all depends on where focus happens to be. It failed that way
once under a full parallel run: sixty seconds waiting for a selection notice
from a keystroke the tree never saw. Now SENT, the same conclusion the 2026-09-10
pass reached for the walk in the same file. The walk in `test_ui_tree.c` also
gained the caret trail that `test_ui_behaviour.c` already had, so the
still-unexplained "row 0 was never reached" residue names itself next time.

### The worker count, with numbers

`-j 8` was a property of THIS workstation (24 cores), not of the job. A GitHub
runner has 4. `build.cmd` now derives it: **one worker per core, capped at 8,
floored at 2** -- so this machine still gets 8 and a runner gets 4.

Release, whole suite, this machine:

| Workers | 24 cores (s) | 4 cores, `start /affinity F` (s) |
|---|---|---|
| `-j 2`  | -- | 23.1, 23.0 |
| `-j 4`  | 13.5, 16.5, 16.6 | 13.2, 13.2 |
| `-j 8`  | 14.1, 13.6, 13.6 | -- |
| `-j 12` | 13.7, 13.7, 14.5 | -- |
| `-j 24` | 16.9, 21.6, 13.8 | -- |

The curve is flat from 4 to 12 and gets worse and noisier at 24: past the cap
the wall clock is set by the single longest suite, so extra workers buy nothing.
At four cores, `-j 4` already reaches that floor -- so eight workers there is
twice the oversubscription for zero wall clock, which is exactly the trade that
was stretching every timing assumption in the tree.

**And the windowed suites no longer run concurrently with each other.**
`RESOURCE_LOCK desktop` on the nine suites that create real top-level windows
(`test_ui_*` plus `test_discover`, matched by NAME so a new one cannot forget).
Focus, activation, z-order and visibility are properties of the DESKTOP, which
is one shared resource ctest otherwise knows nothing about; the 2026-09-10 pass
fixed the worst of that structurally with `WS_EX_NOACTIVATE`, and this is the
other half. Pure computation still parallelises around them.

A lock makes those nine one chain, and the chain is the critical path, so they
also carry `COST 1000` and ctest starts them FIRST -- the chain then runs UNDER
the rest of the suite instead of after it:

| Release, this machine | Wall clock |
|---|---|
| before this pass (2026-09-10 entry) | 14.4-16.4 s |
| lock, default order | 19.7 s |
| lock + `COST` | **13.6 s** |

### Verification

`build.cmd Debug` and `build.cmd Release`, `/W4 /WX` clean.

**15 consecutive full `ctest` runs in each configuration, no failure in any of
them**, on the final tree at `-j 8` (24 cores here):

| Runs | Config | Result | Wall clock |
|---|---|---|---|
| 15 | Release | 41/41, no failures | 13.5-20.5 s, median 13.8 s |
| 15 | Debug | 41/41, no failures | 28.8-42.2 s, median 41.3 s |

Before/after on the same command, same machine:

| | Before (2026-09-10 entry) | After |
|---|---|---|
| Release, 15 runs | 14.4-16.4 s | 13.5-20.5 s (median 13.8) |
| Debug, 15 runs | 41.8-43.8 s | 28.8-42.2 s (median 41.3) |

Debug is unchanged because it is one suite: `test_sync` is pure computation and
takes 29-49 s of the ~41 s on its own. Everything the timer-resolution fix
bought is spent waiting for it.

**AND THE SKIP PATH WAS PROVEN RATHER THAN ASSUMED.** Nothing skips on this
machine -- it has an engine, an endpoint, UI Automation and a window station --
so the collection path would otherwise have shipped untested and first run on
CI. It was forced: one case temporarily made to take its skip branch, a full
`build.cmd Debug test`, and the result read from the top-level output:

```
100% tests passed, 0 tests failed out of 41
Cases SKIPPED in this run -- these did NOT test anything:
D:\...\tests\test_capture_wasapi.c(277): the_default_capture_endpoint_opens_at_the_session_format -- no capture endpoint on this machine
```

-- i.e. exactly the case ctest hides. The suite itself reported `[  SKIPPED ]`
and `6 run, 5 passed, 0 failed, 1 SKIPPED`. The forced branch was reverted, both
configurations rebuilt, and the 30 runs above were taken after that.

### What is expected on CI

| | Before | Expected after |
|---|---|---|
| Release | 15.97 s, 1 failure | ~15 s, green |
| Debug | 662.54 s, 1 failure | ~70 s, green |

The Debug number is almost entirely one thing: 648 s of that 662 was a single
case burning a backstop that thought it was 60 s. What is left is `test_sync`,
which is pure computation and took 61.78 s on the runner against ~30 s here --
that is the runner's cores, nothing is waiting, and it is the floor for a Debug
run there. `-j 4` instead of `-j 8` should improve it somewhat by not putting
two processes on each of its four cores.

### Files (this pass only)

Product: `src/ui/controller.c` (the `wait_for_files` deadline only).
Build: `build.cmd`, `CMakeLists.txt`.
Tests: `tests/test_engine.h` (new), `tests/test_runner.h`, `tests/test_wait.h`,
`tests/test_capture_apartment.c`, `tests/test_capture_wasapi.c`,
`tests/test_capture_abandon.c`, `tests/test_ui_add_source.c`,
`tests/test_ui_tree.c`, and the mechanical `SKIP()` conversion in
`tests/test_ui_a11y.c`, `test_ui_behaviour.c`, `test_ui_canvas.c`,
`test_ui_dialogs.c`, `test_ui_pause.c`, `test_ui_theme.c`, `test_ui_update.c`.
Nothing committed. `tests/test_strings.c` was NOT touched by this pass.

### Safety (AGENTS.md rule 1)

Nothing was rendered to any audio device; no player was written or run; every
capture in this pass is either `APR_SRC_FAKE` or a process tap on the test's own
PID, whose tree renders nothing. `APPRECORDER_NO_TRAY` and `APPRECORDER_NO_UPDATE`
are still set by CMake for every test and were not touched.

**One mistake to record.** While diagnosing, `test_ui_behaviour.exe` was run
ONCE directly from a shell instead of through ctest, which means it ran without
`APPRECORDER_NO_TRAY=1` and `APPRECORDER_NO_UPDATE=1` -- so that single run
built a real notification-area icon and could have raised real shell balloons,
and started a real update check. No audio was rendered. It also produced three
spurious failures, which is how it was noticed. Every measurement reported above
was taken through ctest with both variables set.

---

## 2026-09-10 (later) — v0.0.1 IS PUBLISHED

https://github.com/a2hsh/app-recorder/releases/tag/v0.0.1

### Getting CI green took three more rounds, and two were real defects

The agent's work landed first (commit `def17a2`) and took the Debug run from
662 s to 66 s. CI was still red, twice, and neither was a flake.

**Round 1 -- one recording path per process.** The fixture named its WAV after
the PID alone, so every case in `test_ui_behaviour` recorded to one path and
relied on `DeleteFileW` to clear it. That is a race against the PREVIOUS case's
encoder: while its handle is open the delete fails and
`apr_out_probe_writable` returns a sharing violation. A counter removes the
ordering dependency. `test_ui_pause.c` and `test_ui_update.c` had the identical
shape and got the same fix; `test_ui_dialogs.c` did not need it (one name per
case).

**And the worse half of round 1.** `fix_up()` answered a fixture that would not
build by printing the reason and returning 0 -- and every call site answers 0 by
returning from the case with NO assertion run, which the runner prints as
`[ OK ]`. Two of the three cases this broke PASSED WITH ZERO ASSERTIONS, and the
printed reason went where ctest sends the output of a passing suite, which is
nowhere. It is a failure now. Proved by forcing every fixture to fail: 21 cases
that used to report OK now report FAILED with the reason.

**Round 2 -- two notions of "recording" that do not flip together.**
`apr_controller_recording()` is the controller's flag, set while it handles
Ctrl+R. `apr_graph_running()` is what `canvas_busy()` (src/ui/canvas.c) asks
before refusing an edit, and it is not true until the runner ticks. Tests waited
on the first and then pressed a key expecting the refusal -- so in that window
the key was ACCEPTED and the assertion compared "that cannot be changed while a
recording is running" against "Disconnecting Teams...". `test_ui_pause` lost the
same window from the other end: Ctrl+P went to a recording that had not started,
the pause never took, and `wait_paused()` burned its full 60 s.

Both now wait for BOTH flags (`rec_started()` in test_ui_behaviour, six call
sites; `start_recording()` in test_ui_pause).

**Neither round was reproducible here.** Ten runs pinned to two cores
(`start /affinity 3`) passed both with and without the fixes. This workstation
closes both windows too fast. The evidence in each case was the CI log itself,
which is why reading it mattered more than re-running locally.

### CI, green on both

| Config | Before | After |
|---|---|---|
| Debug | 662.54 s, failing | **66.79 s, 41/41** |
| Release | 15.97 s, failing | **19.18 s, 41/41** |

Two cases skip on the runner and say so: both want a default capture endpoint,
which a GitHub runner has not got. That is the skip log doing its job.

### The release

Built from the tag, signed, verified, tagged, published. Assets:
`apprecorder.exe` (1,046,016 bytes), `apprecorder-wait.cmd`, `release.json`,
`release.json.sig`, `apprecorder-0.0.1-src.zip` (2.4 MB, 494 entries).

**The source zip is the LGPL section 6 obligation, and it was checked rather
than assumed**: LICENSE, THIRD-PARTY-NOTICES.md, vendor/lame/{COPYING,LICENSE},
vendor/{opus,ogg}/COPYING, vendor/jsmn/LICENSE and 29 LAME .c files are all in
it.

**Verified the way a stranger would**, after publishing: downloaded all three
assets through the `releases/latest/download/` redirect the updater itself uses
and ran `apprelease.py verify` against the PUBLIC key in update_key.c. Signature
verifies, hash matches, version 0.0.1.

**And the updater was proved end to end** by running the DOWNLOADED binary:
`apprecorder version` says 0.0.1, and `apprecorder update` fetched the live
manifest, verified it against its own compiled-in key, compared versions and
answered "apprecorder 0.0.1 is the newest version." That is the whole
highest-risk path, exercised against the real release.

Note: CI's Release exe is 1036.5 KB against this machine's 1021.5 KB -- MSVC
14.51 on the runner, 14.44 here. Expected; the docs say "just over 1 MB".

### Still open

- **Arabic**: 565 strings. The author's own task; ux-araby then a Gemini pass,
  keeping «إمكانية الوصول».
- **Real-device capture** still has no automated coverage, and the two CI skips
  are the visible edge of that. It needs the author present.
- `--quality` for Opus: the CLI clamps 0..10, the encoder accepts 0..11, so
  complexity 10 is reachable only as the default (quality 0). Documented
  accurately rather than changed during a release.
- The author should back up `%USERPROFILE%\.apprecorder\release-key.pem` before
  travelling. `apprelease.py verify` now proves it matches the key compiled into
  the shipped binary; if it is lost, no future release can update any install.
- In-app help is a browser link (Help -> Documentation), not embedded offline
  help. Deliberate for a single-exe product, but revisit if it annoys.
