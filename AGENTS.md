# Rules for agents working in this repo

Read this before doing anything. These are not style preferences.

---

## 1. NEVER play audio through the user's default output device

**This is the hard rule and it has already been broken once.**

The author is blind. He works by listening, wears headphones fed by a GoXLR, and
a stray tone is not a minor annoyance — it is painful, it masks the screen reader
he navigates with, and at volume it is a hearing risk. A previous agent wrote a
looping tone player to have something to capture, failed to stop it, and put a
sustained tone in his ears until a human said "I'm hearing a long tone?"

Therefore:

- **Do not call `SoundPlayer`, `MediaPlayer`, `[console]::beep`, `ffplay`, `vlc`,
  `start file.wav`, or anything else that renders to the default endpoint.**
- **Never loop audio.** Not "briefly", not "just to test".
- **Never write a player whose stop path is untested.** If a stop can fail, it
  will fail while making noise.

### How to get something to capture instead

In this order of preference:

1. **Session volume `1e-4` — NOT zero.** This question has been measured and
   settled: **process loopback is post-session-volume.** At volume 0 you capture
   pure digital silence (0 of 1,534,080 samples non-zero) even though the engine
   confirms the app is rendering, so volume 0 is useless as a test harness.
   At `1e-4` the endpoint sits at **−92 dBFS — inaudible** — and loopback
   captures a clean signal scaling linearly (peak `0.000025` = exactly
   `0.25 × 1e-4`). That is the permanent safe harness, and it is what every
   capture test already uses.

   **Use `spike/spike_silentplayer.c` unmodified.** It already enforces all of
   this structurally: a fail-closed volume gate before `Start()`, a hard
   `MAX_SAFE_VOLUME 0.001f` ceiling that refuses before creating any COM object,
   a doubly-bounded finite frame budget, and a watchdog armed before any audio
   object that `TerminateProcess`es itself. Do not write your own player.
2. **Ask the author to play something he controls** and can stop himself.
3. **Capture with a fake source.** Most of the codebase never needs real audio at
   all — see design section 4.3. If you are reaching for a speaker, first check
   whether `capture_fake` answers your question.

### Mandatory belt-and-braces if you ever do render audio

- Hard duration cap, enforced by the OS, not by your loop logic.
- No loop constructs. One shot, finite buffer, then exit.
- The process must die on its own without anything calling stop.
- Announce in your report exactly what you played, at what volume, for how long.

---

## 2. Build

`build.cmd [Debug|Release] [spikes] [test]` from the repo root. It wraps
`vcvars64.bat`. **Never invoke `cl.exe`, `cmake`, or `ninja` directly** — they are
not on PATH and the bundled copies live inside the VS Build Tools tree.

`/W4 /WX` is on. Warnings are errors. This codebase hand-authors COM vtables;
sloppiness here is not survivable.

---

## 3. DRY is enforced, not encouraged

Design section 7 names a **single owner** for each cross-cutting concern:

| Concern | Sole owner |
|---|---|
| Ring buffers | `core/ringbuf.c` |
| Sample-rate conversion | `core/resample.c` |
| QPC / drift arithmetic | `core/clock.c` |
| PCM format conversion | `core/mix.c` |
| COM vtable boilerplate | `capture/com_shim.c` |
| HRESULT to message | `platform/err.c` |

Writing your own ring buffer, your own resampler, or your own HRESULT formatter
is a review failure even if it works. If the existing one does not fit your case,
**say so and extend it** — do not fork it.

---

## 4. Action encoders: fixed conventions

Adding an encoder is one file plus one test file. Nothing else.

- **Export exactly `const AprActionVTable apr_action_<id>;`** — not
  `apr_action_<id>_vtable`. `core/registry.c` picks it up by that name.
- **Do not edit `CMakeLists.txt` to register it.** `APR_HAVE_ACTION_<ID>` is
  defined automatically from the presence of `src/actions/action_<id>.c`, so a
  half-written encoder cannot break the link for everyone else.
- **Do not self-register.** The registry owns the table.
- **`on_audio` must never block on disk.** Own a writer thread and write-behind
  through `core/ringbuf.c` — never a private FIFO. Copy the proof seam from
  `action_wav.c` (`apr_wav_test_write_gate`): hold the disk path shut, push
  audio through `on_audio`, assert it still returns inside a bounded wait. Prove
  it; do not assert it.
- **`finalize` must leave a playable file on every exit path**, including error
  paths and half-written recordings.
- **Do not scrub NaN/Inf yourself.** `core/mix.c` handles it for every integer
  format. Duplicating that is a rule 3 failure.

## 5. Accessibility is a correctness property here

- **`NM_CUSTOMDRAW`, never `LVS_OWNERDRAWFIXED`.** Custom-draw changes painting
  and keeps the accessibility tree; full owner-draw replaces the control's
  semantics and leaves a screen reader nothing to read.
- Every operation reachable by keyboard. No mouse-only paths, ever.
- Canvas nodes are real child HWNDs precisely so the platform supplies MSAA/UIA.
  Do not "optimise" them into owner-drawn rectangles.

---

## 6. No user-facing string is ever a literal in code

This app ships in Arabic. Retrofitting i18n is expensive, so it is designed in
from the start — see design section 6.2.

- **Every user-facing string goes in the `.rc` STRINGTABLE with a named ID.**
  Not in a `wchar_t*` in your source. This includes error messages, CLI output,
  UIA names and descriptions, and anything a screen reader will read.
- **Never concatenate sentences from fragments.** Arabic word order differs and a
  translator cannot reorder pieces joined in code. Use positional inserts so
  arguments can move: **`%1!s!`, `%2!s!`** — the Win32 `FormatMessageW` spelling.
  **Not `%1$s`**, which is POSIX and works on neither Win32 nor the MSVC CRT.
- **Every insert is `!s!`.** Run numbers through `apr_str_number()` first.
- **`apr_str()` locks and may allocate — it is NOT safe on an audio callback.**
  Resolve strings on the UI or a worker thread, never in `on_audio` or a capture
  pump.
- **Never use `printf("%d items")` shaped plurals.** Arabic has six plural forms
  to English's two. Go through the plural-aware API.
- **Never hardcode a layout direction.** Signal flow is left-to-right in English
  and right-to-left in Arabic. Direction is a parameter.
- **Never size a control to fit its English string.** Arabic needs more vertical
  room at the same point size.
- Logical order (source → bus → action) is language-independent. Only painting
  mirrors; the accessibility tree does not.

Internal log messages and code comments are exempt — those are English.

## 7. Scope discipline

Do the task you were given. Do not build ahead into other waves — the core is
deliberately sequenced because parallel edits to a shared audio pipeline produce
divergent implementations, which is the DRY failure above.

Do not commit. Leave changes in the working tree and report what you did,
including what failed and what you are unsure of.
