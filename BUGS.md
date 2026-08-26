# apprecorder — bug tracker

Consolidated from four independent read-only sweeps (Fable 5), 2026-08-26:
UI · core+capture · CLI/session/encoders · accessibility+i18n.

Nothing here is fixed yet. Ranked by severity, then by confidence.
**[2×]** marks a finding two sweeps reached independently — treat as certain.

Severity means: **critical** = data loss, corruption, crash, or a shipped
feature that cannot be used at all. **major** = silent wrong behaviour, a
safety net that never fires, or something the user is never told. **minor** =
real but bounded.

---

## The pattern worth fixing at the root

Three separate bugs this week, and one more below, are the same shape:
**something freed after a bounded wait that may have timed out.**
`m4a_destroy` → both capture close paths → now `apr_source_destroy` (C1).

The mechanism that lets it recur: `proc_close`, `dev_close`,
`apr_capture_destroy` and `apr_source_destroy` all return **`void`**, so a
`pump_stuck` that one layer correctly detected cannot reach the layer that
frees. **A timed-out join must return a value the caller is forced to handle.**
Fixing the three instances without fixing that will produce a fourth.

---

## Critical

### C1 — The ring buffer is freed under a live capture thread
`src/core/source.c:139-148` · `capture/wasapi_common.c:468-500` ·
`capture_process.c:329` · `capture_device.c:199`

`pump_stuck` protects the capture's *own* allocation but never propagates to
the source that owns the ring, so `apr_source_destroy` calls `rb_destroy()`
regardless.

**Failure:** WASAPI wedges inside `GetBuffer` (exactly what `pump_stuck` is
for). `apr_wasapi_stop` times out at 5 s, the close paths correctly leak and
return — but they return `void`, so `apr_source_destroy` frees the 96 KB ring
anyway. The pump later unwedges and `memcpy`s into freed memory. **Heap
corruption from an audio thread.**

### C2 — The Structure panel cannot be browsed by keyboard **[2×]**
`src/ui/controller.c:1377-1408` (`on_tree_select`) → `src/ui/canvas.c:984-991`

The tree's selection sink fires on **every caret move** and the controller
answers it with an unconditional `SetFocus` on the canvas node. Suppression
exists only for the *programmatic* direction.

**Failure:** F6 into the tree, press Down. Focus is yanked to the canvas, the
reader announces the canvas node instead of the tree row, and the next Down
operates the canvas. **Everything past the first row is unreachable** — the
rich tree sentences are the panel's entire purpose.

**Fix direction:** update the canvas's `cur` without focusing unless the user
actually activated (Enter / double-click).

### C3 — "Stop recording and close" leaves a zombie process
`src/ui/controller.c:1134-1145` (`wait_for_files`) · `1045-1050` ·
`src/ui/app.c:1427`

The close-wait pump dispatches every message with **no `WM_QUIT` check**.
`recording_finished` posts `WM_CLOSE`, the same drain dispatches it,
`DestroyWindow` runs, `PostQuitMessage` fires — and the drain then retrieves
and discards `WM_QUIT`.

**Failure:** every stop-and-close that finishes inside 30 s — *the normal
case*. Window gone, process alive, `apr_controller_destroy` never runs, tray
icon ghosted, second launch coexists with the first. Files are safe.

### C4 — Ctrl+Shift+E rewrites the graph mid-recording, unguarded
`src/ui/canvas.c:1651-1708` · `src/core/graph.c:208-231`

Disconnect is deliberately **not** a frame accelerator, so it bypasses the
controller's `busy()` check and reaches the canvas as a raw keystroke — and
`apr_graph_connect`/`disconnect` never check `g->running` either.

**Failure:** disconnect during a recording and the UI thread rewrites the bus
edge arrays while the runner iterates them in `apr_graph_tick`. Corrupted mix
or crash mid-recording. Same hole covers mouse-click edge completion and,
more mildly, gain writes.

---

## Major

### M1 — A whole take can be recorded into nothing, then reported as success
`src/cli/cli.c:861-866` · `session_load.c:642` · `action_mp3.c:604` ·
`action_ogg.c:870` · `bus.c:511-521` · `cli.c:147,1951`

`--bitrate` accepts 0–1152 but MP3 allows 8–320 and Opus 6–510. The refusal
happens at `apr_bus_start`, after recording has begun; a failed action is
downgraded to a per-action skip.

**Failure:** `--bitrate 400 --out meeting.mp3 --duration 3600 --json`. Dry-run
passes. The session runs a full hour **recording nothing**, `--json` suppresses
the warning entirely, and the final document lists the output path with
`"seconds": 3600` — a file that does not exist — beside `exitCode: 6`, whose
documented meaning is "recorded and playable". Audio unrecoverable.

**Fix direction:** validate bitrate/rate/channels against the chosen encoder at
resolve/dry-run time.

### M2 — `--allow-missing` aborts the entire run it exists to save
`src/cli/cli.c:2218` · `cli.c:1195-1196`

Session resolution correctly drops an unresolvable source, but a bus left with
zero sources then hard-fails `apr_cli_resolve` with exit 2.

**Failure:** two-bus session (`Mix`: Teams + mic, `Voice`: mic). Teams isn't
playing. `--allow-missing` drops it, `Mix` is now source-less, **the whole run
aborts and `Voice` never records either.** The meeting is lost by the flag
typed to prevent exactly that.

### M3 — Process-tap pump blocks on COM enumeration; a stall shears the reference timeline
`src/capture/capture_process.c:80-199` · `wasapi_common.c:221-290`

Mute detection runs **inline on the drain thread**: an `ISimpleAudioVolume` RPC
every 500 ms, plus a full every-endpoint × every-session enumeration every 2 s
whenever the target isn't rendering (the common idle case).

**Failure:** enumeration stalls past the buffer → WASAPI drops packets and sets
`DATA_DISCONTINUITY`. Process taps deliberately **do not fill** (design 5.1:
"arrives perfect"), so the dropped frames vanish from the index space and every
bus containing that source **desyncs permanently and silently** — the one
failure the entire clock design exists to prevent. The spike that measured
"process taps never gap" predates this polling code.

**Fix direction:** move mute polling off the pump thread, or fill measured gaps
on process taps when the engine itself caused them.

### M4 — The "greyed **and** spoken" refusal never speaks
`src/ui/app.c:1435-1437` · `controller.c:270-303`

`TranslateAccelerator` does not deliver `WM_COMMAND` for an accelerator whose
menu item is disabled — the keystroke is swallowed, so `busy()` never runs.

**Failure:** mid-recording, Ctrl+1 gives **total silence**, indistinguishable
from a broken app. *"That cannot be changed while a recording is running"*
(`strings.rc:514`) has never once played. The only live route into `busy()` is
the tray's "Open Session", which by definition fires while the window is
hidden — where `say0` reaches nobody.

### M5 — Ogg: granulepos overstates by `pre_skip`, and lookahead is never flushed
`src/actions/action_ogg.c:458,719-746`

Granulepos is written as `pre_skip + frames_encoded`; RFC 7845 defines it as
samples *decodable* so far. Shutdown pads the final partial frame but never
feeds the extra ~312 samples of flush silence that opusenc does.

**Failure:** roughly one stop in three, up to **6.5 ms of real audio stays
inside the encoder** and the final page's granulepos exceeds the file's
decodable length — strict tools flag it, duration disagrees with content, and
mid-stream seeks land 6.5 ms early. Confirm with `ffprobe`/`opusdec`.

### M6 — Device gap-fill is systematically short by ~one packet
`src/capture/wasapi_common.c:185-216` · `src/core/source.c:329-360`

The clock is anchored at the *arrival* of the first packet, but that packet's
frames were captured *before* it, so steady state runs ≈480 frames short.

**Failure:** a real dropout of G frames fills only G−480, and **any dropout
shorter than one packet is never filled at all**. The PI controller then
recovers by stretching ~10 ms over its 10 s tau instead of preserving
alignment. The 0.43-frame/3-hour result used fake sources, so the real-device
path is unproven. **Fix direction:** anchor at `now − frames·qpc_freq/rate`.

### M7 — `apr_controller_model_changed` has no production caller
`src/ui/controller.c:1511-1519` (callers: tests only) · edits at
`canvas.c:1159-1207,1698-1707`

Its own header says that without it "the tree panel would still be showing the
old shape". That is now what happens.

**Failure:** disconnect with Ctrl+Shift+E, or complete a connect by click. The
canvas updates; the tree still says *"Mic — feeds Voice Mix"*. The view used
to audit a session reports a connection that no longer exists.

### M8 — The tree's "recording" and "failed" clauses can never be heard when true **[2×]**
`src/ui/controller.c:969-1008,1077-1088` · `tree_panel.c:284-286,364-367`

Nothing rebuilds the tree when recording starts, or when an action fails.

**Failure:** start recording, F6 to the tree — **every bus reads as idle for the
whole session**. If the encoder fails an hour in, the row keeps saying it is
saving. The sentences exist, are queued for translation, and are unreachable.
(Related: `UI_PANE_RECORDING`, `UI_DESC_RECORDING`, `UI_HEALTH_*`,
`UI_STATUS_IDLE` are in the catalog and referenced nowhere in `src/`.)

### M9 — Events that only happen while backgrounded are announced only to the background window **[2×]**
`src/ui/controller.c:1059-1060` (`ARM_FAILED`) · `1090-1095` (`OUTPUT_RENAMED`)

`SOURCE_DIED`, `SOURCE_MUTED`, `ACTION_FAILED` and `STOPPED` honour the tray
contract. These two do not, and `OUTPUT_RENAMED` has no `TRAY_INFO_*` sibling
in the catalog at all.

**Failure:** start from the tray with the window hidden; a source fails to arm,
or the take is renamed aside — the sentence lands on a status bar nobody can
see. The rename notice is the one fact the honest-collision policy insists the
user be told.

### M10 — Hide-to-tray never checks the icon actually registered
`src/ui/controller.c:1256-1259,1174-1183` · `tray.c:94-108`

`Shell_NotifyIcon(NIM_ADD)` failure is a warning only; both hide paths call
`ShowWindow(SW_HIDE)` unconditionally.

**Failure:** shell refuses the icon (or Explorer crashed before
`TaskbarCreated`), user presses Ctrl+Shift+H — **the window vanishes with no
surface at all.** Win+B finds nothing, Alt+Tab finds nothing, and it is still
recording.

### M11 — Every error "reason" is untranslatable English prose
`src/platform/err.c` (table ~:33, `apr_err_format` :264) → `controller.c:309`,
`1077` and the CLI's `%2` inserts

Rule 6 says text a screen reader reads lives in the catalog. The reason text is
English prose wrapped in a translated frame.

**Failure:** when Arabic ships, **every failure sentence is half Arabic, half
English.** This is a mechanism gap, and the design says retrofitting i18n is
exactly the expensive kind. Cheaper now than after forty more accrete.

### M12 — Cancelling a session load is announced as a failure, in English
`src/ui/controller.c:692-704,740-750`

Returns `APR_ERR(..., L"the user declined this session")`, then formats that
internal literal into `UI_DLG_SESSION_FAILED`.

**Failure:** you press Cancel deliberately and hear *"That session could not be
loaded: the user declined this session."* A cancel is not a failure; the reason
is an untranslatable literal; and it refers to you in the third person.

### M13 — `CREATE_ALWAYS` after a separate `exists()` check
`src/platform/outpath.c:508-541` vs `action_wav.c:637`, `action_mp3.c:699`,
`action_ogg.c:973`

**Failure:** two apprecorder processes (two scheduled tasks in the same second,
same template) both pass `exists()` before either creates, then both
`CREATE_ALWAYS` the same path and interleave writes. The take is corrupt, and
if a previous take was auto-renamed aside, the file at that name is garbage.
The "never overwritten" promise is advisory, not atomic. **Fix:** `CREATE_NEW`
plus retry through the collision loop.

### M14 — `save-session` silently drops sources past 64
`src/cli/cli.c:2395,2450`

`intern_source` returns `-1` when full and the caller `continue`s — no warning,
no error, **exit 0**, and JSON reports `"sources": 64`.

**Failure:** silent config loss in the exact artifact you rely on to reproduce
a recording.

### M15 — Ctrl+T with focus on the splitter strands focus in a hidden window **[2×]**
`src/ui/app.c:744-757`

The focus rescue checks the tree pane but not the splitter, which the same
relayout hides.

**Failure:** Tab to "Panel divider", press Ctrl+T. Focus stays on an invisible
window; the reader goes quiet; arrows silently resize a hidden panel.

---

## Minor

| # | Finding | Where |
|---|---|---|
| m1 | `ACTION_FAILED` balloons unconditionally — heard twice in the foreground | `controller.c:1082` |
| m2 | Close-timeout replays "will close when written" and then never closes | `controller.c:1198` |
| m3 | "Remove output" with no outputs says *"There is no bus"* | `controller.c:529` |
| m4 | Real edit failures announced as *"not available yet"*; the `AprErr` is discarded | `canvas.c:1179,1256` |
| m5 | Dialog create-failure is loud in the log, still silent to the user | `dialogs.c:200` + `controller.c:365` |
| m6 | Add Output: OK can silently do nothing when the format combo is empty | `dialogs.c:1299-1308` |
| m7 | File > New / Open discard an unsaved session with no prompt | `controller.c:1214` |
| m8 | `SetFocus` from inside `WM_SETFOCUS` in frame and canvas — the pattern `tree_panel.c` measured as swallowed | `app.c:992`, `canvas.c:1636` |
| m9 | A pending connect whose node vanishes ends the mode silently | `canvas.c:924` |
| m10 | `OFN_OVERWRITEPROMPT` asks "replace?" but outpath renames aside instead | `dialogs.c:1197` |
| m11 | Session title bypasses `UI_TITLE_SESSION`; frame name becomes a bare path | `controller.c:725,896` |
| m12 | `±Inf` gain passes validation (NaN is caught) → sustained full scale | `bus.c:181,248` |
| m13 | `--json` emits **no document at all** on session-resolve failure | `cli.c:2314-2335` |
| m14 | A >4 s disk stall becomes silence; run still exits 0, loss only in the log | `action_*.c` overrun paths |
| m15 | Ogg: resampler priming silence can inflate duration (non-48 k only) | `action_ogg.c:719` |
| m16 | `--format WAV` refused while `--out x.WAV` works (case-sensitive) | `registry.c:133` |
| m17 | Two `take{n}.wav` outputs both expand to `take1` and are refused as duplicates | `cli.c:1292` |
| m18 | `prescan` honours `--json`/`--quiet` even as another option's value | `cli.c:490` |
| m19 | `CTRL_CLOSE` waits 4 s; the header promises it blocks until files are closed | `cli.c:426` |
| m20 | `\t`/`\r`/`\n` are legal JSON escapes and silently mangle hand-edited paths | `session_load.c:194` |
| m21 | `tok_wstr` truncates over-long session strings without a fault | `session_load.c:190` |
| m22 | Literal `L"0"` reaches the user instead of `apr_str_number(0,…)` | `dialogs.c:1270` |
| m23 | Help dialog key names are hardcoded English; catalog sentences translate theirs | `dialogs.c:378-410` |
| m24 | Fixed 96-unit buttons clip their own English, let alone Arabic | `dialogs.c:520-551` |
| m25 | Register: "write/wrote/written" where a person says "save" | `strings.rc:75,183,206,343,418,437` |
| m26 | `apr_log_shutdown` races its drain thread after a 2 s timeout | `log.c:334-357` |
| m27 | `apr_wasapi_call` running-check is a TOCTOU against `start()` (latent) | `wasapi_common.c:414` |
| m28 | `apr_runner_destroy` doesn't wait for a *synchronous* run (latent) | `runner.c:409` |
| m29 | `interactive=0` session open returns ok despite unresolved sources | `controller.c:709` vs `ui_controller.h:132` |

---

## Verified sound

Worth recording, because these were specifically hunted and hold up:

- **ringbuf two-cursor invariant** — correct at every use; readers take
  availability from `write_pos` and every safety decision from `claim_pos`.
- **clock arithmetic** — 128-bit throughout, one truncation per conversion,
  nothing accumulates, nothing assumes 10 MHz QPF.
- **NaN/±Inf** — no unguarded float→integer path anywhere; WAV stays verbatim.
- **finalize on every exit path**, including the **new one-recording action
  lifetime**, which the CLI sweep checked specifically and found correct.
- **EXCLUDE consent** — cannot be bypassed, including via `--allow-missing`,
  and re-warns on every run.
- **RTL discipline** — no painted window carries `WS_EX_LAYOUTRTL`, one
  mirroring site, custom draw issues no coordinates.
- **The status-bar live-region fix** and the Ctrl+E `pending` fix are both
  genuinely correct.
- **WAV RF64 promotion**, **MP3 CBR kill-safety**, session versioning, atomic
  save, plural machinery, and the `rc.exe` completeness gate.
- No `MessageBeep`, no owner-draw, no `%1$s`, no printf-plurals in `src/`.
