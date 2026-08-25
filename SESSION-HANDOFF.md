# apprecorder — Session Handoff

Live status doc. Update as work happens, not at the end.

## What this is

An Audio Hijack-style **recorder** for Windows: capture audio from chosen apps
(per-process) plus hardware inputs, into an arbitrary number of software-defined
buses, written out to files. Built to give the user more capture buses than his
GoXLR provides in hardware.

**Status:** design approved. **Wave 1 (Foundation) complete** — see the
2026-08-25 (wave 1) entry at the bottom. Next: wave 2, the capture spike.

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
