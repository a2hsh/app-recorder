# Using apprecorder

This page explains the model the whole product rests on, then everything built on top of it: sessions, pause and resume, and the notification-area icon.

## Sources, buses and actions

There are three kinds of thing, and they sit in a row.

**A source produces audio.** It is either an application, captured per process, or a hardware capture endpoint — a microphone, a line input, one of a mixer's capture channels. Once opened, both look identical to everything downstream.

**A bus is a mix.** It takes any number of sources, sums them, and hands the result on. It is the software equivalent of a channel on a hardware mixer, except that you can have as many as you like.

**An action is what happens to a mix.** Today there is one kind that matters: write it to a file. The interface was built so that other kinds — transcription, for instance — can be added later without touching the audio engine. None have been yet.

The flow is always the same direction: **source → bus → action**.

### The part a newcomer will not guess

**Actions attach to buses, not to sources.**

You never record an application. You put an application on a bus, and you record the bus. Every file that apprecorder writes is the output of exactly one bus.

This is why two things that feel very different are the same feature:

- "Record Teams and my microphone together" is **one bus with two sources and one output**.
- "Record Teams and my microphone separately" is **two buses with one source each, and one output on each**.

Same code path, same command grammar, one extra `--bus`.

### One source, several buses

A source can feed more than one bus at the same time, and it carries its own gain on each connection. This is the thing hardware cannot do without physically splitting a signal.

So "the full mix" and "just the guest's microphone" are two buses reading the same microphone. There is one capture of that microphone; both buses read it independently, and each keeps its own position, its own resampler and its own drift correction. Adding the second bus costs almost nothing.

Because a source can appear on several buses, "the selected source" is ambiguous on its own. What is unambiguous is *this source, on this bus* — which is exactly the connection between them, and the thing that carries the gain.

### One bus, several outputs

A bus can have several actions. Recording the same mix to WAV for editing and to MP3 for sending is one bus with two outputs, not two recordings.

## Kinds of source

### An application

Named by process id (`--pid`) or by program file (`--exe teams.exe`). If two things called `teams.exe` are playing at once, apprecorder says so and asks you to name the one you mean by pid rather than guessing.

Capture walks the process tree, which is what makes browsers and Electron applications work — a browser renders audio in a child process, and capturing the parent picks it up.

Two things about application capture are worth knowing before they surprise you:

- **Capture sits after the Windows volume mixer.** An application muted in the volume mixer records as pure silence, even though it is running normally. apprecorder warns you about this before a recording starts, and again if it happens during one, because it is otherwise indistinguishable from a successful recording.
- **Windows never reports that a captured application has exited.** It just keeps handing over silence, for ever, with no error. apprecorder detects the exit itself and tells you. It then goes looking for the application, and if it comes back the recording resumes at the correct place in the file, with the gap filled by exactly the silence that was missed. You are told about both halves: the loss and the recovery.

### A hardware input

Named by friendly name or endpoint id (`--device "Chat Mic"`). Run `apprecorder list-devices` to see what this machine has.

If you use a mixer with onboard processing, take your microphone from the mixer's own capture endpoint rather than from an application tap. That way the recording carries the hardware's gate, compressor and EQ. An application tap captures audio *before* hardware processing, which is right for applications and wrong for a voice.

A hardware input that is unplugged mid-recording is detected, and apprecorder looks for it to come back in the same way it does for an application.

### Everything the machine plays, minus one application

There is a mode that records all system audio and holds back a single process tree. It is genuinely useful — "record everything except the game" — and it is genuinely dangerous, so it is treated carefully.

**It holds back a whole process tree, not one application.** Holding back a launcher also holds back everything that launcher ever started. Holding back a terminal silently drops every application launched from it, including ones you never associated with that terminal. apprecorder shows you what is currently in the tree before you commit.

**And everything else on the machine goes into the file.** Every application, including ones started after the recording began. It is never the default, and a session file cannot switch it on by itself — you have to say yes each time, either by typing `--allow-system-capture` or by confirming a dialog.

### A synthetic source

`--fake` generates a tone that apprecorder makes up itself. It reads no audio hardware and plays nothing. It exists so that a configuration, a pipeline or a script can be exercised end to end without touching a microphone, and so that failures which look exactly like a healthy recording — a source going silent, a source dying — can be rehearsed on purpose.

## Outputs and file names

The format is chosen from the file extension. `--out mix.mp3` is enough; `--format` is only for the case where the name does not say.

An output name is a template. These tokens are replaced:

- `{date}` — the local date, as `2026-08-30`
- `{time}` — the local time, as `14-03-52` (colons are not legal in a filename)
- `{bus}` — the bus's name
- `{ext}` — the extension the chosen format writes
- `{n}` — the lowest number for which the whole path does not exist yet

Anything outside a token is copied literally, and an unrecognised token is left alone, so an ordinary Windows path is its own template and keeps working.

**A name that is already a recording is never overwritten.** `mix.wav` becomes `mix-2.wav`, then `mix-3.wav`. There is no prompt, deliberately: the thing being recorded does not wait for a dialog, and the seconds spent answering one are seconds missing from the take. Nothing is silent about it — apprecorder tells you where the take actually went.

## Stopping

A recording killed part-way that leaves an unplayable file is the worst thing this program could do, and nothing outside the process can repair one once the process is gone.

So Ctrl+C is a request, not a kill. It sets a flag, the recording loop notices, and every file is finalized before the process exits. A second Ctrl+C says so and still refuses to abandon the finalize. Closing the console window gets the same treatment. Closing the application window while a recording is running offers you three answers: stop and close, keep recording, or leave it recording in the notification area.

## Pause and resume

Pausing takes its time out of the file. The take carries on as one file, at one clock, with the paused span simply absent — it is not filled with silence, and it is not a separate file.

It is not a mute: nothing at all is written while it lasts. And it is not a stop: nothing is finalized, so a recording still produces exactly one file per output no matter how many times you paused it.

At the command line, press **P** while recording. It both pauses and resumes. This works only when apprecorder is attached to a real console — a scripted or piped run grows no keyboard reader at all, so it can never swallow a byte something else was going to read.

In the window, **Ctrl+P** pauses and **Ctrl+Shift+P** resumes. Both transitions are announced out loud, and the status line keeps saying "paused" underneath with the *recorded* length rather than the elapsed one.

There is no pause in a session file. A pause is a property of a take in flight, not of a saved configuration, and a session that loaded already paused would be a recording that starts by not recording.

## Sessions

A session file is a whole configuration written down as JSON: every bus, every source, every gain, every output and its settings. It is not a preset of a few fields; it is the graph.

Save one from the command line with `save-session`, or from the window with Ctrl+S. Reopen one with `record --session`, with Ctrl+O in the window, or by passing the file as apprecorder's only argument — which is also what happens if you drag it onto the executable. apprecorder does not register itself for `.json` files, so double-clicking one will not open it unless you associate it yourself.

**A process id is not a name.** It is a number the kernel hands out and takes back, and after a reboot it means nothing at all. So a session stores what identifies an application — its image path, its program name, its window class — and resolves that when the file is reopened. Matching is by image path first, program name second, window class as the only tiebreaker between two instances, and never by a bare pid.

**Reopening a session gives you a report, not a yes or no.** Every source comes back with a status: found as asked, found somewhere else, not found, or several candidates that you need to choose between. Both the window and the command line show you the same report, because every one of those outcomes is otherwise silent — a bus that quietly loses its source still records, still finishes, and still produces a file missing the thing it was made for.

A hardware device stores both its endpoint id and its friendly name, so that a missing device can be described as "Chat Mic" rather than as a pair of GUIDs.

If a run loses something along the way, it finishes with exit code 6 rather than 0: every file plays, but what was recorded is not what was asked for.

## The notification area

A recording runs for hours, and the window is minimised for almost all of them. For that whole time, the tray icon is the application.

**The tooltip is a live status readout.** It updates every second and says what state the recording is in and how long it has been going — "apprecorder — recording, 01:12:30". Windows has a keyboard path to the notification area (Windows+B, then the arrow keys), and a screen reader reads each icon's tooltip as focus lands on it. So you can check on a session from inside any other application, without raising a window and without anything being spoken at you unprompted.

**The icon's menu** shows the window, starts and stops recording, pauses and resumes, opens a session, and quits. Shift+F10 or the Applications key opens it from the keyboard — it is the same menu and the same code path as a right-click, not a mouse feature a keyboard happens to reach.

**Balloons are the background-announcement channel.** Anything that happens while the window is not in front goes out as a notification-area balloon: a source that died, a source that came back, a source that turned out to be muted, an output that had to be renamed, a recording that started or stopped. A live region on an unfocused background window is not reliably announced by any screen reader, and that is exactly the state a recording spends its life in.

## What happens when something goes wrong

The rule is that **failure of one source never takes down a session**. A source that cannot be opened, or that dies mid-recording, is reported and the recording carries on without it. Every file is finalized to a playable state on every exit path.

Where audio is genuinely lost — a source that was away for twenty seconds, a stall longer than the internal buffer — apprecorder chooses alignment over content. It emits exactly as much silence as was lost and resumes at the correct absolute position. A hole in one track is recoverable; a permanent timeline shift silently ruins every track on the bus.
