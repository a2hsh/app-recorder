# Changelog

Versions are `MAJOR.MINOR.PATCH`. The three numbers are defined once, in
[`include/version.h`](include/version.h), and everything else — the window
title, `apprecorder version`, the updater's comparison — derives from them.

While the major number is `0`, the command grammar and the JSON field names may
still change between releases. The exit codes may not: they are contract from
0.0.1 onward, scripts branch on them, and they will not be renumbered.

## 0.0.1 — 2026-09-10

First public release.

### Recording

- **Per-application capture**, per process id, using the WASAPI process
  loopback that arrived in Windows 10 version 2004. Nothing is inserted into
  the signal path and no virtual audio device is installed; the application
  being recorded plays through its normal output exactly as before.
- Capture follows the **process tree**, which is what makes browsers and
  Electron applications work — audio rendered in a child process is picked up
  by capturing the parent.
- **Hardware inputs** — microphones, line inputs, a mixer's capture channels —
  are sources on the same footing as applications. Once opened, the two look
  identical to everything downstream.
- **A system-minus-one-tree mode** records everything the machine plays while
  holding back a single process tree. It is never the default, and a session
  file cannot switch it on by itself.
- **Synthetic sources** (`--fake`) generate a tone apprecorder makes up. They
  read no audio hardware and play nothing, and they can be told to go silent or
  to die at a chosen frame, so the two failures that look exactly like a
  healthy recording can be rehearsed on purpose.

### The graph

- **Sources feed buses; buses feed actions.** An action attaches to a bus and
  never to a source, which is why "record Teams and my microphone together" and
  "record them separately" are the same feature with one extra `--bus`.
- One source may feed **several buses at once**, at its own gain on each. There
  is one capture of that source; each bus keeps its own position, resampler and
  drift correction, so the second bus costs almost nothing.
- One bus may carry **several outputs**, so the same mix can go to WAV for
  editing and MP3 for sending without recording it twice.

### Alignment

- Sources arrive from different clock domains and drift apart over hours. A
  backlog-driven controller corrects for it continuously, so files from one
  session line up **with each other** and not merely each with itself. Measured
  drift over three hours: **0.43 frames**.
- All clock arithmetic is exact 128-bit integer maths against the performance
  counter frequency. Nothing accumulates, so error cannot compound with
  recording length.

### Formats

- **WAV**, float32, up to 8 channels, written by apprecorder itself. A file
  that passes 4 GB is promoted to RF64 in place rather than being truncated or
  refused.
- **MP3** via LAME 3.100. Mono or stereo, CBR 192 kbps by default, V0–V9
  available.
- **Ogg Opus** via libopus 1.5.2. Mono or stereo, VBR, 96 kbps stereo by
  default, always encoded at 48 kHz with the session's own rate recorded in the
  header.
- There is **no AAC or M4A**, deliberately. MP4 writes its index last, so a
  recording interrupted by a crash or a power cut is a file no player will open
  and nothing can repair. An AAC action was written, tested and deleted for
  that reason.

### Durability

- **Every file is finalized and playable on every exit path.** Ctrl+C is a
  request rather than a kill: it sets a flag, the loop notices, the files are
  closed, and a second Ctrl+C still refuses to abandon the finalize. Closing
  the console window is treated the same way.
- **Failure of one source never takes down a session.** A source that cannot be
  opened, or that dies mid-recording, is reported and the rest carries on.
- **A dead source is detected and reconnected.** Windows never reports that a
  captured application has exited — it hands over silence for ever, with no
  error. apprecorder notices, says so, watches for the application to come
  back, and resumes at the correct absolute position with the gap filled by
  exactly the silence that was missed.
- Where audio is genuinely lost, **alignment wins over content**. A hole in one
  track is recoverable; a permanent timeline shift silently ruins every track
  on the bus.
- **A name that is already a recording is never overwritten.** `mix.wav`
  becomes `mix-2.wav`, without a prompt, because the thing being recorded does
  not wait for a dialog.

### Pause and resume

- Pausing **takes its time out of the file**. The take stays one file at one
  clock with the paused span absent — not filled with silence, and not split.
- **P** at the command line, **Ctrl+P** and **Ctrl+Shift+P** in the window. The
  console keyboard reader starts only when standard input really is a console,
  so a piped or scripted run can never swallow a byte meant for something else.

### The window

- A **signal-flow canvas** showing the graph as nodes and edges, with sources,
  buses and outputs in columns.
- A **structure panel** giving the same graph as a tree, for when a list is
  faster to scan than a canvas.
- **Light and dark appearance**, following the system setting and overridable
  with Ctrl+D.
- **A notification-area icon** whose tooltip is a live status readout, updated
  every second, and whose menu covers showing the window, starting and stopping
  a recording, pausing, opening a session and quitting.

### Accessibility

- **Every operation is reachable by keyboard.** There are no mouse-only paths,
  and the automated UI Automation test has already caught a real violation of
  that rule.
- The canvas nodes are **real windows in the accessibility tree**, not painted
  regions with an accessibility story bolted on afterwards.
- **The keyboard map is generated from the binding tables themselves**, so F1
  cannot document a shortcut the program does not implement.
- Announcements choose their channel: a live region while the window is in
  front, a notification-area balloon while it is not — because a live region on
  an unfocused background window is not reliably announced by any screen
  reader, and that is the state a long recording spends its life in.

### The command line

- `record`, `list-apps`, `list-devices`, `save-session`, `update`, `help`,
  `version`.
- `--json` on every listing, for scripts.
- `--dry-run` resolves every process, device and output path and prints what
  would happen, without opening an audio device or writing a byte.
- **Exit codes are contract**, including `6` — every file plays, but what was
  recorded is not what was asked for.
- `apprecorder-wait.cmd` ships beside the executable so that `&&` and
  `%ERRORLEVEL%` work in a batch file. See
  [docs/command-line.md](docs/command-line.md) for why a shim is needed at all.

### Sessions

- A session file is the **whole graph** as JSON, not a preset of a few fields.
- **A process id is not a name.** Sessions store image path, program name and
  window class, and resolve those on load. Matching is never by a bare pid.
- **Reopening a session gives you a report, not a yes or no.** Every source
  comes back as found, moved, missing, or ambiguous — because a bus that
  quietly loses its source still records, still finishes, and still produces a
  file missing the thing it was made for.

### Updates

- Checks a **public GitHub release** at startup, every five minutes, and after
  a recording stops if five minutes have passed since the last check.
- **The host is not trusted.** The manifest is signed with ECDSA P-256 against
  a public key compiled into the binary, and it carries the SHA-256 of the
  executable, so one signature protects both. A missing signature is as fatal
  as a wrong one.
- **Nothing installs itself.** apprecorder asks, downloads, verifies, and puts
  the new version in place at the next start. The previous version is kept
  until the new one has started successfully once.
- **Never during a recording**, not even paused.
- The check is **opt-out and the setting persists**: `apprecorder update
  --disable`. See [docs/updating.md](docs/updating.md).

### Packaging

- **One executable**, just over 1 MB, static CRT, no runtime redistributable,
  no installer. The only thing written outside the executable's own folder is the
  updater's state under `HKCU\Software\apprecorder\Update`.
- The same binary is the command line and the window. Give it a command and it
  is a CLI; double-click it and it is an application.
- Refuses to start on Windows earlier than 10 version 2004, naming the build
  you have and the build it needs, rather than failing later and vaguely.

### Localization

- Every user-facing string lives in a catalog with positional inserts and the
  six CLDR Arabic plural forms, and the layout mirrors for right-to-left.
- **Arabic is not shipped in 0.0.1.** 565 strings are still awaiting
  translation; today it runs in English.

### Known limitations in this release

- **The hardware capture path has no automated test against real hardware.**
  Enumeration is tested; opening a microphone and pumping audio from it is not,
  because starting a microphone without consent is not something a test may do.
  That is the path carrying all of the real clock drift.
- **No live monitoring**, and there will not be. Windows cannot bound
  worst-case latency, and not competing there is what lets apprecorder spend
  latency on getting the sync right instead.
- **No effects beyond gain.** It is not a DAW.
- **No installer and no file-type association.** A `.json` session opens if you
  drag it onto the executable or pass it as the only argument, but
  double-clicking one will not open it unless you associate it yourself.
- **Transcription is designed for but not built.** The action interface exists
  so it can drop in later.
