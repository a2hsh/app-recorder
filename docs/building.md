# Building from source

There is nothing to install beyond a compiler, and nothing to fetch. Every dependency is vendored in the repository as source.

## What you need

**Visual Studio 2022 Build Tools**, with the "Desktop development with C++" workload. That is the whole list.

MSVC, CMake and Ninja all ship inside that install. `build.cmd` finds the bundled copies itself, so none of them needs to be on your PATH and you do not need a separate CMake or Ninja. The build was developed against MSVC 14.42 and the Windows SDK 10.0.22621.

Do not invoke `cl.exe`, `cmake` or `ninja` directly. They live inside the Build Tools tree and are not on PATH; `build.cmd` wraps `vcvars64.bat` for exactly this reason.

## Building

From the repository root:

```
build.cmd            configure and build Debug
build.cmd Release    configure and build Release
build.cmd test       build Debug, then run the test suite
build.cmd Release test
```

Output lands in `build\Debug\` or `build\Release\`. The product is a single `apprecorder.exe`, with `apprecorder-wait.cmd` copied beside it by the build. There is nothing else to ship.

A Release build is 1,046,016 bytes — just over 1 MB. The ceiling is 10 MB and CI enforces it; the headroom is there for bus effects and VST hosting. The Debug build is much larger and is not a shipping number.

## Running the tests

```
build.cmd Release test
```

That is 41 suites, and they should all pass. They need no audio hardware: the core is testable against a synthetic source by design, so continuous integration and agent-driven development never depend on a microphone being plugged in.

They run in parallel, one worker per core, capped at 8. Most suites are pure computation and finish in hundredths of a second; the wall clock is set by the few that are not.

### A test leaves no trace on the machine running it

**No test plays audio, and none touches an audio output device.** Beyond that, the build sets three environment variables for every suite so that a new one cannot forget:

- `APPRECORDER_NO_TRAY` — no tray icon and no shell notifications, which a screen reader would otherwise read aloud on every run.
- `APPRECORDER_NO_UPDATE` — no HTTPS request and no registry write. A suite that reaches github.com is also a suite that fails on an aeroplane.
- `APPRECORDER_SKIP_LOG` — where a skipped case records itself.

Nine of the suites create real top-level windows on the real desktop and assert about focus, activation and what UI Automation reports. Those hold a `RESOURCE_LOCK`, so ctest never runs two of them at once no matter how many workers there are — focus is a property of the desktop, which is a single shared resource ctest otherwise knows nothing about.

### Read the skipped list

`build.cmd` prints the cases that skipped, after the run. ctest keeps the output of a suite that *failed* and discards the rest, so a case that skipped for want of an audio engine — the one line explaining why a green run proved less than it looks — was otherwise invisible.

A skip is not a pass. If something skipped that should not have, that is the result, not a footnote to it.

## Where the dependencies come from

Nothing is downloaded during a build. `vendor/` holds the source of everything that is linked in:

- `vendor/lame/` — LAME 3.100, the MP3 encoder. LGPL-2.0-or-later.
- `vendor/opus/` — libopus 1.5.2, the Opus encoder. BSD-3-Clause.
- `vendor/ogg/` — libogg 1.3.6, the Ogg container. BSD-3-Clause.
- `vendor/jsmn/` — a small JSON parser, used to read session files. MIT.

Each has a `PROVENANCE.md` recording the exact release, its checksums, the independent channels those checksums were verified against, what was and was not copied out of the upstream tarball, and what it costs the shipping image in bytes. No upstream `.c` or `.h` file is patched; the only file that is ours in each tree is a hand-written `config.h` standing in for the one autoconf would generate.

If you are adding a dependency, read rule 8 of `AGENTS.md` first. The short version: state why, measure the cost in bytes added to the shipping image, verify the source across channels that do not share a distribution path, record the licence and any obligation it creates, and take the smallest thing that works.

## Build settings worth knowing about

- **C11, MSVC only.** The build refuses any other compiler rather than half-working on it.
- **`/W4 /WX`.** Warnings are errors. This codebase hand-writes COM vtables, and sloppiness there is not survivable.
- **Static CRT.** No runtime redistributable is required to run the result.
- **Release uses `/O1 /GL` with `/LTCG /OPT:REF /OPT:ICF`** — optimised for size rather than speed, which is the right trade for a program whose hot loop is an audio callback that has microseconds of work to do.
- **The application manifest is a resource, not a linker-generated one.** The linker's manifest tool would silently replace it, leaving a process that is quietly comctl32 v5 and system-DPI-aware with no warning at all. The build passes `/MANIFEST:NO`, and a test asserts at runtime that the intended settings actually took effect.

## Adding an encoder

One source file and one test file. Nothing else.

Write `src/actions/action_<id>.c` exporting a single `const AprActionVTable apr_action_<id>`, and add the extern and array entry to `src/core/registry.c` inside the matching guard. **You do not edit `CMakeLists.txt`** — `APR_HAVE_ACTION_<ID>` is defined automatically from the presence of the source file, so a half-written encoder cannot break the link for everyone else and a finished one needs no build-system change.

The conventions an encoder has to meet, and the reasons behind them, are in rule 4 of `AGENTS.md`. Two of them are load-bearing: `on_audio` must never block on disk, and `finalize` must leave a playable file on every exit path — including a recording that never reaches `finalize` at all, because the process was killed. A format that keeps its index at the front of the file and writes it last cannot meet that second one, which is why there is no AAC action.

## Spikes

`build.cmd Debug spikes` also builds the throwaway probes in `spike/`. They are feasibility experiments, kept because their measurements are cited in the design document, not part of the product.

**If you need something to capture, use `spike/spike_silentplayer.c` unmodified.** It renders at an inaudible level with a hard volume ceiling, a finite frame budget and a self-terminating watchdog, all enforced structurally rather than by loop logic. Read rule 1 of `AGENTS.md` before writing anything that renders audio. The author is blind and works by listening; a stray tone is not a minor annoyance.
