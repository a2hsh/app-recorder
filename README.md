# apprecorder

Record each application on your Windows PC separately, into as many mixes as you want.

## The problem it solves

A hardware mixer gives you a fixed number of buses. A GoXLR gives three: Stream Mix 1, Stream Mix 2, and Chat Mic. If you want a fourth mix — say, "the game and my voice, but not Discord" — you cannot have one. You are out of channels.

Software has no such limit. apprecorder captures the audio of individual applications, mixes them into as many buses as you care to define, and writes each bus to its own file. Ten buses is the same amount of work as one.

It does this without a virtual audio cable. Windows 10 version 2004 added a way to tap one process's audio directly, and apprecorder uses it. Nothing is inserted into the signal path, nothing is rerouted, and the application you are recording plays through its normal output exactly as before.

## Who it is for

People who record more than one thing at once and need the results separated: streamers, podcasters, people who keep a copy of their own meetings, anyone whose mixer ran out of channels.

It is also built for people who work by keyboard and screen reader. Accessibility here is a correctness property, tested rather than assumed. See [docs/accessibility.md](docs/accessibility.md).

## What it does that the alternatives do not

- **Per-application capture, per-PID.** An app is captured whichever hardware channel it is assigned to. No mixer reconfiguration, ever.
- **As many buses as you want.** One source can feed several buses at once, at a different gain on each. "Everything" and "just Teams" are two buses over the same tap, not two recordings.
- **Sample-accurate alignment.** Sources come from different clock domains and drift apart over hours. apprecorder corrects for that, so two files from one session line up with each other and not merely each with itself.
- **A real command line.** Every listing has a JSON form, every failure has an exit code a script can branch on, and `--dry-run` validates a whole configuration without opening an audio device or writing a byte.
- **One file, no installer, no runtime.** A single 961 KB executable. Nothing to install, nothing to uninstall, nothing in the registry.

## Requirements

Windows 10 version 2004 (build 19041) or newer, 64-bit. Per-process audio capture does not exist on earlier versions, and apprecorder refuses to start rather than pretending otherwise — it tells you which build you have and which build it needs.

## Install

Download `apprecorder.exe` and put it somewhere on your PATH. That is the install.

`apprecorder-wait.cmd` ships beside it. You only need it for scripting; see [docs/command-line.md](docs/command-line.md) for why.

## The first five minutes

### Find out what is playing

```
apprecorder list-apps
apprecorder list-devices
```

`list-apps` shows the applications that are playing audio right now, with their process ids. Add `--all` to include ones that hold an audio session but happen to be silent. `list-devices` shows the capture endpoints this machine has — your microphone, and each of your mixer's capture channels.

### Record one application

```
apprecorder --exe teams.exe --out meeting.mp3
```

That is a whole recording. Press Ctrl+C to stop. Every file is closed and made playable before apprecorder exits.

### Record two mixes at once

```
apprecorder --bus Mix   --exe teams.exe --device "Chat Mic" --out mix.wav ^
            --bus Voice                 --device "Chat Mic" --out voice.wav
```

Two buses, one shared microphone, two files, one session. `mix.wav` has the meeting and your voice; `voice.wav` has your voice alone. Because they share one clock anchor, they line up with each other.

### Check before you commit

```
apprecorder --bus Mix --exe teams.exe --out mix.mp3 --dry-run
```

`--dry-run` reads the whole command line, resolves every process and device, checks every output path, prints what would happen, and stops. It opens no audio device and writes no file.

### Save it for tomorrow

```
apprecorder save-session --session studio.json --bus Mix --exe teams.exe --out mix.mp3
apprecorder record --session studio.json
```

Process ids are not stable across a reboot, so a session file stores what identifies an application rather than its pid, and tells you what it found when it reopens.

### Or use the window

Double-click `apprecorder.exe` and you get the window instead of the command line. Same program, same graph, same session files. Ctrl+1 adds a source, Ctrl+2 adds a bus, Ctrl+E connects them, Ctrl+3 adds an output, Ctrl+R starts recording.

## Sources, buses, actions

This is the one concept everything else rests on, so it is worth thirty seconds.

A **source** is something that produces audio: an application, or a hardware input. A **bus** is a mix. An **action** is what happens to a mix — right now, writing it to a file.

Sources feed buses. Buses feed actions. **An action attaches to a bus, never to a source.** You do not record an application; you put an application on a bus and record the bus. That is why "record Teams and my mic together" and "record them separately" are the same feature: one bus with two sources, or two buses with one each.

A source may feed several buses at once, at its own gain on each. That is what makes this a graph rather than a tree, and it is why the second file above costs nothing extra.

[docs/using-apprecorder.md](docs/using-apprecorder.md) goes through this properly.

## Formats

| Format | Written by | Notes |
|---|---|---|
| WAV | apprecorder itself | float32, uncompressed. Up to 8 channels. |
| MP3 | LAME 3.100 | Mono or stereo. CBR 192 kbps by default. |
| Ogg Opus | libopus 1.5.2 | Mono or stereo. VBR, 96 kbps stereo by default. Always encoded at 48 kHz. |

The format comes from the file extension, so `--out mix.mp3` needs no `--format`. Both `mix.opus` and `mix.ogg` reach the Opus encoder.

There is no AAC or M4A, and that is deliberate. MP4 keeps its index at the front of the file and writes it last, so a recording that ends in a crash or a power cut is a file no player will open and nothing can repair. WAV, MP3 and Ogg all degrade gracefully instead: whatever reached the disk still plays. An AAC action was written, tested and deleted for exactly this reason.

## What it does not do

- **It is not a virtual audio device.** It records; it does not present buses to other applications as inputs. That needs a signed driver, which is a different project.
- **No live monitoring.** Hardware mixers do this deterministically. Windows is not a real-time OS and cannot bound worst-case latency, so apprecorder does not compete here. Nobody listens to a bus in real time, which is precisely what lets it spend latency on getting the sync right.
- **No effects beyond gain.** It is not a DAW.

## Honest status

This is **version 0.0.1**. Some things you should know before you rely on it:

- **The hardware capture path has never been exercised by an automated test against real hardware.** Enumerating capture devices is tested and works. Actually starting one and pumping audio from it is not covered by the test suite, because starting a microphone without consent is not something a test may do. That is the path carrying all of the real clock drift. Application capture is the well-tested half.
- **Arabic is planned but not shipped.** The whole product is built for it — every user-facing string is in a catalog with positional inserts and six plural forms, and the layout mirrors — but roughly 500 strings are still awaiting translation. Today it runs in English.
- **Transcription is designed for but not built.** The action interface exists so it can drop in later. It has not.
- There is no installer and no file-type association. `apprecorder.exe` is a file you put somewhere and run.

## Building from source

Everything you need ships with Visual Studio 2022 Build Tools — MSVC, CMake and Ninja all come in the box. From the repository root:

```
build.cmd Release
```

Details in [docs/building.md](docs/building.md).

## Licence

**apprecorder's own licence has not been chosen yet.** See [docs/licensing.md](docs/licensing.md) — the author needs to make this call before the first public release, and there is a placeholder waiting for it.

**One obligation binds a binary release right now, regardless of that choice.** apprecorder statically links **libmp3lame, which is LGPL-2.0-or-later**. Static linking is permitted, but LGPL section 6 requires that a recipient be able to relink the application against a modified libmp3lame. So **any published build must ship either the object files or the `vendor/lame/` source tree alongside it**, together with the LGPL text and a notice that libmp3lame is used. This is a packaging obligation, not a build one, and it applies the moment a compiled `apprecorder.exe` is uploaded anywhere.

libogg and libopus are BSD-3-Clause. They require only that their copyright notice and licence text be reproduced in the documentation. No relinking, no source, no special packaging.

## Documentation

- [Using apprecorder](docs/using-apprecorder.md) — sources, buses, actions, sessions, pause and resume, the tray.
- [The command line](docs/command-line.md) — commands, flags, JSON output, and the exit-code contract.
- [Accessibility](docs/accessibility.md) — the keyboard map, how the canvas is navigated, and how announcements work.
- [Building from source](docs/building.md)
- [Licensing](docs/licensing.md)
