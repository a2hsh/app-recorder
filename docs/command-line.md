# The command line

apprecorder's command line is an automation surface. Every listing has a machine-readable form, every failure has an exit code a script can branch on, and `--dry-run` validates a whole configuration without opening an audio device or writing a byte.

```
apprecorder [command] [options]
```

`apprecorder help` prints the whole surface. This page is the same material with the reasoning attached.

## One executable, two front ends

`apprecorder.exe` is a command line when you give it a command, and a window when you do not.

| First argument | What you get |
|---|---|
| nothing | the window, empty |
| `record`, `list-apps`, `list-devices`, `save-session`, `update`, `help`, `version` | the command line |
| anything starting with `-` | the command line |
| a lone `*.json` file | the window, opened on that session |
| anything else | refused, and named back to you |

An unrecognised first argument fails rather than guessing. `apprecorder recrod --out x.wav` prints "recrod is not a command apprecorder has" and exits 1, instead of opening an empty window and losing the rest of the line.

### The two files, and why there are two

A release contains `apprecorder.exe` and `apprecorder.com`. **Keep them together, and type `apprecorder`** — you get the right one automatically, because `PATHEXT` is searched in order and its default begins `.COM;.EXE`.

The reason is one bit in the executable's header. `apprecorder.exe` is a WINDOWS-subsystem image, because a console image flashes a black window on every double-click. Two shells treat such an image differently from an ordinary console program:

- **cmd.exe** waits for it and reports its exit code correctly, but hands it **no standard handles**, so `apprecorder version > out.txt` writes an **empty file**. The output is not lost — it goes to the console — it just does not follow a redirect, a pipe, or a captured variable.
- **PowerShell does not wait for it at all.** It returns in hundredths of a second with no exit code, so a scripted `record --duration 3600` reports success while the recording is still running.

`apprecorder.com` is a 4 KB console-subsystem launcher that runs the executable and passes it the real handles. With it, everything behaves the way a command-line program should, in both shells:

```
apprecorder list-devices --json > devices.json
apprecorder version | findstr 0.
$v = apprecorder version
apprecorder record --exe teams.exe --out mix.wav && upload.ps1
```

Exit codes pass through unchanged, and Ctrl+C reaches apprecorder itself — the launcher deliberately ignores it and keeps waiting, so a take is still finalized rather than cut off.

If you run `apprecorder.exe` explicitly, by full path or by typing the extension, you get the behaviour above and a redirect will produce an empty file. That is the only reason to prefer one name over the other, and typing `apprecorder` avoids it.

**`--log-file` is unaffected by any of this.** It opens the file itself, so it works whichever way you start the program:

```
apprecorder record ... --log-file run.log --log-level info
```

> Versions before 0.1.0 shipped a different answer, `apprecorder-wait.cmd`. It used `start /wait` to restore a wait that cmd was already doing, and `start` is precisely what severs the standard handles — so it made redirection worse while requiring a second name to remember. It has been removed; delete it if you have a copy.

## The grammar is positional, and that is the point

`--bus` opens a bus. **Every source and every output written after it belongs to that bus, until the next `--bus`.** Without any `--bus` there is one implicit bus called "Recording".

That is what makes several buses in one invocation ordinary rather than special — and several buses in one invocation is the entire reason this project exists.

```
apprecorder --bus Mix   --exe teams.exe --device "Chat Mic" --out mix.wav ^
            --bus Voice                 --device "Chat Mic" --out voice.wav
```

Two buses, one shared device source, two files, one session, one clock anchor — so the two files line up with each other and not merely each with itself.

Similarly, `--gain` applies to the source written just before it, and `--format`, `--bitrate` and `--quality` apply to the `--out` written just after them. Each is one-shot, so two outputs in different formats do not need anything cleared by hand.

## Commands

| Command | What it does |
|---|---|
| `record` | Record. This is what apprecorder does when no command is given. |
| `list-apps` | List the applications playing audio right now. |
| `list-devices` | List this computer's audio capture devices. |
| `save-session` | Describe a recording and write it down instead of making it. Takes the same grammar as `record`. |
| `update` | Check whether a newer apprecorder has been published. |
| `help` | Print the whole surface. |
| `version` | Print which build this is. |

### `update`

```
apprecorder update [--install] [--enable | --disable]
```

Prints whether this is the newest version. Typing the command counts as asking,
so it goes to the network immediately rather than waiting for the five-minute
interval — but it still honours the opt-out, and says updates are off rather
than quietly making the request you refused.

| Option | Meaning |
|---|---|
| `--install` | Download the newer version and put it in place at the next start. |
| `--enable` | Check for newer versions from now on. Changes the setting and checks nothing. |
| `--disable` | Stop checking for newer versions. Changes the setting and checks nothing. |

`--enable` and `--disable` cannot both be given: apprecorder remembers this
setting, so it would be remembering the wrong one.

**Neither toggle touches the network**, and the symmetry is what makes that
believable. `--disable` obviously must not make one last callback on its way
out. If `--enable` made one, the two commands would differ in whether they
reach the network, which is not something anybody would remember correctly.

A download that fails verification exits **2**, not 4. It is not a failed
download; it is a file claiming to be apprecorder that the signing key did not
sign, and it gets an exit code that a script can tell apart from a flaky
network. See [updating.md](updating.md).

## Sources

Each one joins the bus opened most recently.

| Option | Meaning |
|---|---|
| `--pid <id>` | Capture the application with this process id. |
| `--exe <name>` | Capture the application whose program file is `<name>`, for example `chrome.exe`. |
| `--device <name>` | Capture this hardware input. Matches the friendly name or the endpoint id. |
| `--system-minus-tree <id>` | Record all system audio and hold back one process tree, named by its process id. |
| `--fake <spec>` | Add a source apprecorder generates itself. Reads no audio hardware and plays nothing. |
| `--gain <dB>` | Gain for the source named just before it. `0` leaves it alone. Range −120.0 to +40.0, one decimal. |

All of the source options may be repeated.

`--exe` refuses rather than guessing when several processes match. It names the candidates and asks you to pick one with `--pid`.

`--system-minus-tree` holds back a whole **process tree**, and records everything else the machine is playing — see [Using apprecorder](using-apprecorder.md). Read what it prints before you use it.

### `--fake`

```
--fake <hz>[,<ppm>[,<amp>]][,<health>...]
```

`<hz>` is the tone. `<ppm>` is a deliberate clock error, which is how a synthetic source stands in for a hardware capture that drifts. `<amp>` is amplitude.

`<health>` is one or more of `muted`, `dead`, `mute=<frame>`, `unmute=<frame>`, `die=<frame>`. It makes a synthetic source go silent, or stop, at a frame you choose — so the two failures that look exactly like a good recording can be rehearsed without any audio hardware and without playing a sound.

## Outputs

Each one joins the bus opened most recently.

| Option | Meaning |
|---|---|
| `--out <name>` | Save this bus to this file. May be repeated. |
| `--format <id>` | Write the next `--out` in this format instead of the one its extension names. |
| `--bitrate <kbps>` | Bitrate for the next `--out`, where the format has one. `0` leaves it to the encoder. Range 0 to 1152. |
| `--quality <n>` | Quality for the next `--out`, where the format has one. `0` leaves it to the encoder. Range 0 to 10. |

Formats are `wav`, `mp3` and `ogg`. The extension usually decides: `mix.wav`, `mix.mp3`, `mix.opus`. `mix.ogg` also reaches the Opus encoder, so you do not have to remember which of the two spellings this build wanted.

**`--quality` means a different thing in each format**, which is why `0` is spelled "leave it to the encoder" rather than "lowest".

For **MP3**, `--quality 0` means constant bitrate at `--bitrate` (192 kbps by default). `--quality 1` to `10` mean variable bitrate at V0 to V9, and `--bitrate` is then unused.

For **Opus**, encoding is always variable bitrate and `--bitrate` sets it — 96 kbps stereo or 64 mono by default. `--quality` sets encoder *complexity*, which trades CPU for quality at the same bitrate rather than changing the bitrate: `0` means libopus's own default of complexity 10, and `1` to `10` mean complexity 0 to 9. So `--quality 1` is the cheapest and `--quality 0` is the most thorough, which reads backwards until you remember that `0` means "do not tune this".

`--out` names may contain `{date}`, `{time}`, `{bus}`, `{n}` and `{ext}`. A name that is already a recording is never overwritten; the new take is saved beside it.

## The session as a whole

| Option | Meaning |
|---|---|
| `--rate <hz>` | Sample rate every source and every file uses. 48000 by default. Range 8000 to 384000. |
| `--channels <n>` | Channel count every source and every file uses. 2 by default. Range 1 to 8. |
| `--duration <seconds>` | Stop on its own after this long. Without it, recording runs until Ctrl+C. Accepts three decimals, up to 86400. |
| `--session <path>` | The session file to read (`record`) or write (`save-session`). |
| `--allow-system-capture` | Consent for a system-minus-tree source that a session file asks for. |
| `--allow-missing` | Record without the sources that could not be found, rather than stopping. |
| `--dry-run` | Check the whole command line and print what would happen. Opens no audio device and writes no file. |
| `--json` | Print JSON for a script to read instead of text for a person. |
| `--quiet` | Print warnings and errors only. |
| `--all` | In `list-apps`, include applications that hold an audio session but are silent right now. |
| `--lang <tag>` | Interface language, for example `en-US`. |
| `--log-level <level>` | `trace`, `debug`, `info`, `warn`, `error` or `off`. `warn` by default. |
| `--log-file <path>` | Write the log to this file as well. |
| `--help`, `-h`, `-?` | Print the help. |

MP3 and Ogg Opus carry mono or stereo only; `--channels` above 2 needs WAV. Opus always encodes at 48 kHz and resamples the session rate onto it, recording the original rate in the file's header.

**`--session` with `record` cannot be combined with sources or outputs on the same line.** A session file describes the whole recording, and merging it with options typed alongside would leave "which one wins" as something you have to remember at two in the morning. Getting it wrong records the wrong thing. Flags that describe the session as a whole — rate, channels, duration — *are* allowed, and one you actually typed wins over the file's own value.

**`--allow-system-capture` does nothing on its own.** Without a session file asking for a system-minus-tree source, it enables no capture of any kind. A flag that widened the privacy scope by itself would be the same problem as a file that did.

**A system-minus-tree source is never droppable under `--allow-missing`.** Dropping an ordinary source records less than was asked for. Dropping the target of an exclusion would record *more* — everything, with nothing held back.

## Exit codes

These are contract. They are documented in the help text, scripts branch on them, and they will not be renumbered.

| Code | Meaning |
|---|---|
| 0 | Finished. Every file was closed and every file plays. |
| 1 | The command line could not be read. |
| 2 | The command line was read, but it does not describe a recording that can be made. |
| 3 | A process, application or capture device named on the command line is not there. |
| 4 | A file could not be created, or could not be closed properly. |
| 5 | A source could not be opened or could not be started. |
| 6 | Recording finished and every file plays, but something went wrong while it ran. |
| 7 | apprecorder failed and has no better description for it than that. |

`update` reuses two of them rather than inventing its own. A download that could not be fetched is **4**, the ordinary "a file did not work" code. A download that *arrived* and failed verification is **2** — the command was fine and the machine is fine; what turned up is not a release this key signed. Those two must not be folded together: one means the network is unreliable, the other means somebody is trying something.

**6 is the one worth branching on.** It means the files are fine but the session is not what you asked for: a source died mid-recording, a session file lost a source, an output stopped saving. A run that quietly returns 0 after recording three hours of silence is the failure this code exists to prevent.

## JSON output

`--json` changes the shape of what is printed, not what is done. The field names are fixed ASCII and are never translated — they are a wire format that scripts match on. Everything a *person* reads goes through the string catalog; everything a *program* parses does not.

`version` emits `name` and `version`. `list-apps` emits an `apps` array of `pid`, `exe`, `name`, `path`, `active`, `muted`, `volume`. `list-devices` emits a `devices` array of `id`, `name`, `default`. `--dry-run` emits `exitCode`, `sampleRate`, `channels`, `durationMs` and the resolved bus structure. A failure emits `exitCode` and `error`.

## Stopping

Ctrl+C is a request, not a kill. It sets a flag, the loop notices, and every action is finalized before the process exits. A second Ctrl+C says so and still refuses to abandon the finalize. The console close button gets the same treatment, and its handler blocks until the files are closed — Windows gives such a handler a few seconds before terminating the process regardless, and a bounded wait there would not risk a kill part-way through finalize, it would guarantee one.

Press **P** during a recording to pause or carry on. The paused time is left out of the file rather than filled with silence. The keyboard reader starts only when standard input really is a console, so a scripted or piped `record` never grows one and can never swallow a byte meant for something else.

## Running with nowhere to print

Started from Explorer, a shortcut or the task scheduler, apprecorder has no console and does not allocate one.

A run that **succeeds** that way stays silent. A scheduled `record --duration 3600` must not pop a black window every night.

A run that **fails** that way never stays silent. A non-zero exit with no output channel raises a message box naming the exit code and saying where to run it to see what it said.
