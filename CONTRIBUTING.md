# Contributing

Bug reports, patches and questions are welcome. This page is what you need to
know before writing code — the conventions, and the small number of rules that
are not conventions.

For a security problem, do **not** open an issue. See [SECURITY.md](SECURITY.md).

## Read AGENTS.md first

[`AGENTS.md`](AGENTS.md) holds the rules this codebase is actually held to. It
is addressed to AI agents because most of the code was written by them, but it
governs everybody, and it is short.

**Rule 1 is the one to read before you write a line.** The author is blind,
works by listening, and wears headphones fed by a hardware mixer. **Nothing in
this repository may render audio to the default output device** — not briefly,
not to have something to capture, not in a test. This rule exists because it
was broken once and put a sustained tone in a blind man's ears until he asked
what was happening. If you need a signal to capture, `spike/spike_silentplayer.c`
already does it safely at −92 dBFS, and most of the tree never needs real audio
at all because the capture layer sits behind an interface a fake implements.

**Do not renumber the rules.** Around fifty source files cite them by number,
in comments that explain why a piece of code is shaped the way it is. Adding a
rule at the end is fine; inserting one in the middle silently rewrites fifty
explanations.

## Building and testing

```
build.cmd            configure and build Debug
build.cmd Release    configure and build Release
build.cmd test       build Debug, then run the suite
```

Visual Studio 2022 Build Tools with the C++ workload is the entire toolchain.
MSVC, CMake and Ninja all ship inside it and `build.cmd` finds them itself.
Nothing is downloaded during a build; every dependency is vendored as source.

Details in [docs/building.md](docs/building.md).

**Warnings are errors** (`/W4 /WX`). This codebase hand-writes COM vtables, and
sloppiness there is not survivable.

### What a test may not do

**A test must leave no trace on the machine running it.** No tray icon, no
notification, no network request, no registry write, and no sound. The build
sets `APPRECORDER_NO_TRAY` and `APPRECORDER_NO_UPDATE` for every suite so that
a new one cannot forget, but a test that finds a way around those is a bug.

A suite that needs hardware the machine does not have must **skip loudly**, not
pass quietly. `build.cmd` prints the skipped cases after a run, because a green
run containing a silent skip is the shape of "passed" that means nothing.

## Conventions that are load-bearing

**No user-facing string literals in code.** Every sentence a person reads lives
in the catalog: an X-macro list in [`include/strings.h`](include/strings.h) and
the text in [`res/strings.rc`](res/strings.rc), so that `rc.exe` and the C
compiler read the same list and neither can drift. Adding a string means adding
it to both, plus an Arabic placeholder. Field names in `--json` output are the
deliberate exception — those are a wire format that scripts match on, and they
are never translated.

**Code is English.** Comments, identifiers, commit messages and log lines, all
of them, always. Arabic belongs in user-facing strings and nowhere else.

**Accessibility is a correctness property, not a feature.** Every operation must
be reachable by keyboard; there are no mouse-only paths, and the automated UI
Automation suite has already caught a real violation of that. Canvas nodes are
real windows in the accessibility tree rather than painted regions. The keyboard
map shown by F1 is generated from the same table that dispatches the keys, so a
shortcut cannot be documented as one key and implemented as another — keep it
that way rather than adding a second list.

**One owner per fact.** The version is three integers in
[`include/version.h`](include/version.h) and everything derives from them,
because a second copy that drifts is an updater that offers to replace a build
with itself. The same reasoning applies elsewhere; look for the existing owner
before adding a constant.

**Nothing enters `vendor/` casually.** State why, measure what it adds to the
shipping image in bytes, verify the source across channels that do not share a
distribution path, record the licence and any obligation it creates, and take
the smallest thing that works. Rule 8 has the full form, and each vendored tree
carries a `PROVENANCE.md` showing it was followed.

**Stay in scope.** A change that fixes one thing and tidies four others is a
change nobody can review.

## Adding an encoder

One source file and one test file, and no build-system edit.

Write `src/actions/action_<id>.c` exporting a single
`const AprActionVTable apr_action_<id>`, and add the extern and array entry to
`src/core/registry.c` inside the matching guard. `APR_HAVE_ACTION_<ID>` is
defined automatically from the presence of the source file, so a half-written
encoder cannot break the link for everyone else.

Two conventions there are not negotiable, both in rule 4: **`on_audio` must
never block on disk**, and **`finalize` must leave a playable file on every exit
path** — including a recording that never reaches `finalize` because the process
was killed. A format that keeps its index at the front of the file and writes it
last cannot meet the second one, which is why there is no AAC action and why
adding one will be declined.

## Commits and pull requests

- **One change per commit**, with a message saying what changed and why. The why
  is the valuable half; the diff already shows the what.
- **Run `build.cmd test` before you push.** Both configurations if you touched
  anything the Release optimiser might see differently.
- **Say what you did not test.** Especially anything on the hardware capture
  path, which has no automated coverage against real devices.
- If a change affects what a user sees or does, update the docs in the same
  commit, and add a line to [`CHANGELOG.md`](CHANGELOG.md).

## Translation

Arabic is the next language and 565 strings are waiting. The infrastructure is
finished — positional inserts, the six CLDR plural forms, mirrored layout — so
this is translation work rather than engineering work.

Two constraints if you take it on. The register is **technical, not literary**:
short and functional, like DevTools or GitHub, not explanatory prose. And the
term for accessibility is **إمكانية الوصول**, not الإتاحة.
