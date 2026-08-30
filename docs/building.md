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

A Release build is around 961 KB. The Debug build is much larger and is not a shipping number.

## Running the tests

```
build.cmd Release test
```

That is 38 suites, and they should all pass. They need no audio hardware: the core is testable against a synthetic source by design, so continuous integration and agent-driven development never depend on a microphone being plugged in.

**No test plays audio, and none touches an audio output device.** The suite sets `APPRECORDER_NO_TRAY` so that the user-interface suites cannot fire real shell notifications, which a screen reader would read aloud on every run.

One caveat: a handful of suites poll for asynchronous announcements and are timing-sensitive under heavy machine load. A single run occasionally loses one of them and it passes immediately on its own. If a run is not clean, re-run the failing suite before hunting for a bug.

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
