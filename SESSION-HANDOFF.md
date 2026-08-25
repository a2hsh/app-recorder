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
**22/22 suites green in Debug and Release**, `/W4 /WX` clean, tree suite run 3x
in each configuration with no flakes. Nothing committed.

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
