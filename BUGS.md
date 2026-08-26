# apprecorder — bug tracker

Consolidated from four independent read-only sweeps (Fable 5), 2026-08-26:
UI · core+capture · CLI/session/encoders · accessibility+i18n.

Ranked by severity, then by confidence. Fixed entries say so in their
heading; the dated notes below record which pass did what.
**[2×]** marks a finding two sweeps reached independently — treat as certain.

Severity means: **critical** = data loss, corruption, crash, or a shipped
feature that cannot be used at all. **major** = silent wrong behaviour, a
safety net that never fires, or something the user is never told. **minor** =
real but bounded.

**2026-08-26, CLI / session / actions pass:** M1, M2, M5, M13, M14 and m13–m21
are fixed, each with a test that fails without the fix. Every entry below says
what was done and, where a decision had to be made, which one and why. Nothing
was deleted; one finding (M5) is marked confirmed against a real file rather
than taken on the sweep's word, and one fix (M13) names a residual that belongs
to a file this pass does not own.

**2026-08-26, core / capture pass:** the root pattern below is fixed as a
mechanism, and C1, C4 (model half), M3, M6, m12, m26, m27 and m28 are fixed on
top of it — each with a test that was watched go red with the fix reverted, and
each red run is named in its entry. **M6's stated mechanism is wrong**; the
entry says so, keeps the finding, and records what the real error was.

---

## The pattern worth fixing at the root — **FIXED (mechanism)**

Three separate bugs this week, and one more below, are the same shape:
**something freed after a bounded wait that may have timed out.**
`m4a_destroy` → both capture close paths → now `apr_source_destroy` (C1).

The mechanism that lets it recur: `proc_close`, `dev_close`,
`apr_capture_destroy` and `apr_source_destroy` all return **`void`**, so a
`pump_stuck` that one layer correctly detected cannot reach the layer that
frees. **A timed-out join must return a value the caller is forced to handle.**
Fixing the three instances without fixing that will produce a fourth.

**What was done.** `include/join.h` + `src/platform/join.c` now own the pattern,
the way `core/ringbuf.c` owns ring buffers (AGENTS.md rule 3). It is small on
purpose: `AprJoin { APR_JOIN_EXITED, APR_JOIN_ABANDONED }`, two bounded waits,
and `APR_ERR_ABANDONED(what)`. Every bounded join in the tree now goes through
it — both WASAPI ones, the new mute poller, the fake source's pacing thread,
`apr_log_shutdown` and `apr_runner_destroy`. `close` in `AprCaptureVTable`,
`apr_capture_destroy`, `apr_source_destroy`, `apr_log_shutdown` and
`apr_runner_destroy` all return `AprErr` now.

**On "forced to handle" — a correction to the framing, and the reason the fix
is not only a return value.** C has no way to force a caller to read one:
`[[nodiscard]]` needs C23, and `_Check_return_` only fires under `/analyze`. A
return value alone would therefore have been a *report*, not a guarantee, and
the fourth instance would have been someone ignoring it.

So the rule that is actually enforced is stronger and is stated once, in
join.h: **the code that frees must be the code that reads the join result, and
an abandoned join frees nothing at all.** `apr_capture_destroy` makes the join
and does the free; `apr_source_destroy` reads that and owns the ring. Memory
safety lives in those two functions and nowhere else. Every layer above them
gets an `AprErr` so it can *say* what happened — `apr_graph_remove_source`
propagates it, `apr_graph_destroy` logs it — but no layer above them is trusted
with the decision. **A caller that drops the value leaks; it cannot corrupt.**

One place could not be expressed as a return value, and it is worth naming: a
capture that fails to *open* has already created its thread, so retiring it is a
join like any other and can be abandoned — but `apr_capture_create` has to
return the *open* error, which is the one that explains why there is no capture.
There, `*out` is left non-NULL despite the failure, and that pointer is the
signal to `apr_source_create` that its ring is still being written into.
Documented at both ends.

Pinned by `tests/test_capture_abandon.c`. The seam is
`apr_capture_fake_wedge()`: the fake source's pacing thread ignores its stop
event and keeps writing into the ring, which is the real failure minus the part
that needs a broken audio driver. **Red run:** with the check in
`apr_source_destroy` removed, `a_wedged_capture_makes_destroy_fail_instead_of_
freeing_the_ring` fails and the suite then *aborts* — the heap corruption is
the demonstration.

---

## Critical

### C1 — The ring buffer is freed under a live capture thread — **FIXED**
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

**Fixed as a consequence of the root pattern above, which is where the detail
lives.** In short: `apr_source_destroy` now returns `AprErr`, and it frees the
ring only when `apr_capture_destroy` says the capture thread actually left. On
abandonment the source, its ring and its capture are all leaked deliberately and
`s` stays a valid pointer, so the leak is recoverable — destroy it again once
the pump unwedges and it completes.

`apr_graph_remove_source` propagates the failure (and still releases the slot,
because the source is out of the graph either way and leaving the pointer there
would only mean freeing it twice at `apr_graph_destroy`). `apr_graph_destroy`
stays `void` — it is the top of the tree and has nobody to report to — but it
logs, and it still does not free what it could not retire.

**Tests:** `tests/test_capture_abandon.c`, five cases. The load-bearing one
reads `rb_write_pos()` on the ring *after* destroy returned a failure and
asserts it is still advancing: that read is only legal because nothing was
freed, and under the old behaviour it is a use-after-free against a live
producer. **Red run:** suite aborts (see above).

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

**FIXED (UI agent, 2026-08-26).** Exactly that. `apr_canvas_set_current_node()` moves the
canvas's `cur`, scrolls the node into view and repaints, and touches focus only
if the canvas already held it; the tree grew a SECOND, separate signal --
`apr_tree_panel_set_activate_sink()`, fired by Enter and by a double click --
and the controller answers the caret with the quiet one and activation with
`apr_canvas_focus_node()`. Enter needed the TreeView subclassed, because
`IsDialogMessage` eats it before any control sees it (design 6.1).

Because focus arriving at the canvas lands on `cur`, this also means F6 out of
the tree now lands on the row the user was standing on.

Pinned by `arrowing_down_the_structure_panel_does_not_yank_focus_out_of_it` and
`the_caret_moves_the_canvas_quietly_and_only_enter_takes_the_keyboard_there` in
`tests/test_ui_behaviour.c` -- the first suite to wire a real controller to a
real tree, which is why neither existing suite could have caught this: the
defect is in the wire between them.

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

**FIXED (UI agent, 2026-08-26).** The drain re-posts `WM_QUIT` and stops pumping. It is safe to
stop there by construction: the only thing that posts the `WM_CLOSE` this drain
dispatches is `recording_finished()`, so the files are already closed.

Pinned by `stop_recording_and_close_actually_ends_the_process` -- it asserts the
UI thread ENDS, which is the property, and it hangs to the timeout without the
fix.

### C4 — Ctrl+Shift+E rewrites the graph mid-recording, unguarded
`src/ui/canvas.c:1651-1708` · `src/core/graph.c:208-231`

Disconnect is deliberately **not** a frame accelerator, so it bypasses the
controller's `busy()` check and reaches the canvas as a raw keystroke — and
`apr_graph_connect`/`disconnect` never check `g->running` either.

**Failure:** disconnect during a recording and the UI thread rewrites the bus
edge arrays while the runner iterates them in `apr_graph_tick`. Corrupted mix
or crash mid-recording. Same hole covers mouse-click edge completion and,
more mildly, gain writes.

**FIXED (UI agent, 2026-08-26).** **UI half only** -- the engine half (`apr_graph_connect` /
`disconnect` refusing while running) belongs to the core agent.

`canvas_busy()` refuses the edge gesture, the mouse click that completes it and
the level keys while `apr_graph_running()`, and says
`UI_ANN_BUSY_RECORDING` -- the sentence the design always intended and which,
until M4 below was also fixed, had never once been heard. The check is BEFORE
the gesture starts, so a user is never left holding one end of an edge they
will not be allowed to finish.

Pinned by `a_canvas_key_that_would_rewire_a_running_graph_is_refused_in_words`.

**Model half FIXED** (the canvas half is the UI pass's). `graph.h` said "do not
mutate the shape while a tick is in flight" and that was the entire
enforcement. Now every shape change refuses while `apr_graph_running()`:
`connect`, `disconnect`, `add_source`, `remove_source`, `add_bus`,
`remove_bus`, `add_action`. `apr_bus_set_gain` refuses too, and it has to be on
the bus rather than the graph because `canvas.c:1309` writes gain straight to
`apr_bus_set_gain` and never passes through the graph at all.

**The refusal is `APR_E_BUSY`, a new kind appended to `AprErrKind`** (nothing
above it moves; `err.c`'s name table gained one line). It is deliberately
distinguishable from `APR_E_STATE`: `APR_E_STATE` is a programming error to
report, `APR_E_BUSY` is "not while a recording is running", which has its own
sentence and is the honest answer to a keystroke. **UI pass: this is the value
to match on** for the canvas's spoken refusal — not `APR_E_STATE`, and not a
silent no-op, which for someone working by ear is indistinguishable from a
broken app.

Guarding it here rather than only in the controller is the point: the CLI,
session loading and the tests are callers too, and `Ctrl+Shift+E` is
deliberately not a frame accelerator, so it reaches the canvas as a raw
keystroke that never passes `busy()`.

**Tests:** `tests/test_graph_guard.c` — every refusal, that each one leaves the
model untouched, and that all of them lift again once the graph stops.

**One existing test changed, and it is worth reading.**
`test_sync.c`'s `a_source_added_mid_session_lands_where_it_starts_not_at_the_
beginning` built its second edge *while the graph was running*, which this fix
now forbids. The property it actually tests is reader placement — a source whose
first frame arrives five seconds in must land five seconds in — and that is
untouched: the edge is now built before the bus starts, and what arrives late is
the source's first frame (the fake anchors on its first advance, so registering
it for pumping at the five-second mark *is* a capture that produced nothing
until then). Renamed to `a_source_that_starts_mid_session_...`, and it now also
asserts the `APR_E_BUSY` refusal it used to depend on not existing.

---

## Major

### M1 — A whole take can be recorded into nothing, then reported as success — **FIXED 2026-08-26**
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

**Fixed, in two halves, because it was two bugs wearing one hat.**

*An encoder can now be asked.* `AprActionVTable` grew an optional
`check_config(cfg)` — last field, so a vtable written before it still compiles
and still means "nothing to ask" — plus `apr_action_check_config()` in
`core/registry.c`, which is the single place that decides what a NULL hook
means. Each action's implementation IS the block its own `create()` already
ran, moved up and called from both, so the numbers cannot drift into two
answers. `apr_cli_resolve()` asks it for every output, which puts the refusal
in `--dry-run` and in the record path alike, at exit 2. `out_path` is
deliberately not consulted: writability is `outpath.c`'s question and has its
own answer at its own moment.

*A run that opened no file is no longer shaped like a success.* The CLI now
distinguishes an output that NEVER OPENED from one that failed later, by
watching where the runner's `ACTION_FAILED` notice falls relative to
`APR_RUN_EV_STARTED` — the bus exposes one `failed` flag for both, and the
runner's pre-STARTED poll can only mean create() refused. If every output in
the run is in that set, the exit code is **4** ("a file could not be created"),
the same code the same failure gets when `apr_cli_resolve` catches it first, so
a script branches on one thing either side of the start. `--json` gains
`"failed"` per output and reports `"seconds": 0` for one with no file, and the
text summary no longer says "Wrote x, 3600.000 seconds" about a file that does
not exist. SOME outputs failing stays exit 6, which is correct: there is a
playable recording, just not all of the one that was asked for.

**Not fixed, and named rather than hidden:** `lame_init_params` can still
refuse a rate/bitrate pairing that the declared limits allow, and only create()
can know. That is exactly why the second half exists.

**Tests:** `test_registry.c` — `an_action_can_be_asked_whether_it_will_take_a_configuration`,
`an_action_with_no_check_hook_accepts_what_create_would`,
`what_the_check_refuses_create_refuses_too`. `test_cli.c` —
`a_bitrate_the_format_refuses_is_refused_before_any_recording_starts` (the
reported command line verbatim, `--duration 3600` included: it must come back
in milliseconds), `a_dry_run_refuses_a_bitrate_the_format_cannot_write`,
`more_channels_than_the_format_carries_is_refused_at_plan_time`,
`a_run_whose_every_output_failed_to_open_is_not_a_success`,
`one_output_failing_while_another_records_is_still_incomplete_not_a_failure`.

### M2 — `--allow-missing` aborts the entire run it exists to save — **FIXED 2026-08-26**
`src/cli/cli.c:2218` · `cli.c:1195-1196`

Session resolution correctly drops an unresolvable source, but a bus left with
zero sources then hard-fails `apr_cli_resolve` with exit 2.

**Failure:** two-bus session (`Mix`: Teams + mic, `Voice`: mic). Teams isn't
playing. `--allow-missing` drops it, `Mix` is now source-less, **the whole run
aborts and `Voice` never records either.** The meeting is lost by the flag
typed to prevent exactly that.

**THE RULE CHOSEN:** under `--allow-missing`, a bus whose every source was
dropped is **itself dropped, out loud, and the rest of the run proceeds**. The
run is refused only when **no** bus survives — there is then nothing to record
at all, which is the same exit 2 an empty command line already gets.

Three things follow from that and each was a decision:

- **The bus takes its outputs with it.** Recording a source-less bus would
  produce a file of pure silence at exactly the length of the session, which is
  a worse lie than a missing file: it looks like a successful take.
- **The run ends INCOMPLETE (6), never OK.** What was recorded is not what was
  asked for. This is the same verdict a dropped *source* already produces, so
  the flag has one meaning rather than two.
- **`record` only, never `save-session`.** Writing down a session with a bus
  quietly missing is precisely the silent configuration loss M14 is about.

Refusing the whole run remains the behaviour **without** the flag: nobody said
recording less was acceptable, and a two-source bus that lost one source still
records (that was never the bug).

**Tests:** `test_session.c` —
`allow_missing_records_the_buses_that_still_have_sources` (the reported
scenario: Voice records, Mix is named as dropped, and no empty `Mix` file
appears), `without_allow_missing_a_source_less_bus_is_still_a_refusal`,
`a_session_with_nothing_left_to_record_is_refused_even_with_allow_missing`.

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

**FIXED — the first, with the second as a net, and here is why not "bound it".**

*Primary: the poll moved off the pump entirely.* `capture_process.c` now owns a
second thread that enters its own MTA, creates, uses and releases its own COM
objects, and polls every 500 ms for the life of the capture. The pump's tick
callback is now death detection only — a zero-timeout `WaitForSingleObject` on a
handle we already hold: no RPC, no enumeration, nothing unbounded.

*Why not bound the call.* There is no timeout knob on a COM RPC. "Bounding" it
means issuing it from another thread and giving up on it, which is the same
extra thread with a worse contract. Two threads per process source is the honest
price, and both are asleep almost always.

*This does not weaken design 4.2.1.* The rule there is that a capture owns its
apartment so its **caller** never has to think about one. The mute thread owns
its own, and no object crosses between it and the pump — which is also why
`proc_close_com` is gone: the `ISimpleAudioVolume` is released as the poller's
own thread unwinds, in the apartment it was created in.

*Secondary: a process tap now fills a gap the engine reports.* And this is the
part that needs justifying against design 5.1, because 5.1 says process taps
arrive perfect. **What 5.1 actually established was measured under an engine
that was keeping up** — 190 s, zero `DATA_DISCONTINUITY`, zero `SILENT`. It is
not a claim about an engine that has just told us it dropped audio. When
`AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` is set, the engine is *reporting a
loss*, and design 3.1's stated policy for a loss is alignment over content:
emit exactly the frames that went missing and resume at the true absolute
position.

**What that means for the reference-timeline claim, stated plainly:** the claim
survives, because *nothing is ever synthesized speculatively*. With no
discontinuity flag — the case 5.1 measured, and the case that holds all the
time in practice — not one sample is invented for a process tap, no resampler is
allocated for one, and `apr_source_pull` still consumes it straight out of the
ring. What changed is only the behaviour in a case 5.1 never observed and the
old code answered with a log line and a permanent silent shear. On the source
that IS the reference, a shear is the worst failure in the system: every bus
reading that tap moves against every bus that does not, for ever, with nothing
in the file to show it. Choosing a hole over a shear there is the same trade
design 3.1 already made for ring overruns.

The fill decision is now **kind-independent by construction**:
`apr_wasapi_packet_gap()` takes no `device_mode` argument at all, which is what
stops the two kinds drifting apart again.

**Tests.** `tests/test_capture_abandon.c` →
`mute_polling_does_not_run_on_the_pump_thread` compares the pump's thread id
against the poller's through a new probe (`src/capture/capture_process.h`); it
skips where there is no audio engine, per design 11, and ran for real here.
**Red run:** before the fix the two ids are equal. The fill half is pinned
hardware-free in `tests/test_capture_timeline.c`.

### M4 — The "greyed **and** spoken" refusal never speaks
`src/ui/app.c:1435-1437` · `controller.c:270-303`

`TranslateAccelerator` does not deliver `WM_COMMAND` for an accelerator whose
menu item is disabled — the keystroke is swallowed, so `busy()` never runs.

**Failure:** mid-recording, Ctrl+1 gives **total silence**, indistinguishable
from a broken app. *"That cannot be changed while a recording is running"*
(`strings.rc:514`) has never once played. The only live route into `busy()` is
the tray's "Open Session", which by definition fires while the window is
hidden — where `say0` reaches nobody.

**FIXED (UI agent, 2026-08-26).** The message loop was inlined in `apr_ui_app_run`, which is a
path no test could reach -- and that is how this survived. It is now
`apr_ui_app_pretranslate()`, a function, and it resolves the keystroke against
the accelerator table ITSELF: a key bound to a DISABLED command is dispatched
anyway, and the handler refuses it out loud. Enabled state now decides how the
menu looks and no longer decides, silently, whether a key exists.
`apr_ui_app_accel_command()` (pure) and `apr_ui_app_command_enabled()` make
"bound" and "greyed" separately assertable.

Pinned by `an_editing_key_pressed_while_recording_says_why_it_was_refused`,
which POSTS F2 and Delete into the UI thread's own queue so they travel the
real filter. Both are unmodified keys, and a synthetic message does not move
the keyboard state, so the modifier read is 0 and deterministic.

**Others of the same shape found while in there,** each now fixed and pinned:
m5 (a dialog that could not be CREATED was folded into "the user cancelled" --
loud in a log, silent to the user), m9 (a half-made connection whose end left
the model ended the mode with no word), m4 (every real refusal from the model
was announced as "that is not available yet", with the reason discarded), m3
and m2 (a sentence that was true of a different situation, which is its own
kind of silence).

### M5 — Ogg: granulepos overstates by `pre_skip`, and lookahead is never flushed — **CONFIRMED AND FIXED 2026-08-26**
`src/actions/action_ogg.c:458,719-746`

Granulepos is written as `pre_skip + frames_encoded`; RFC 7845 defines it as
samples *decodable* so far. Shutdown pads the final partial frame but never
feeds the extra ~312 samples of flush silence that opusenc does.

**Failure:** roughly one stop in three, up to **6.5 ms of real audio stays
inside the encoder** and the final page's granulepos exceeds the file's
decodable length — strict tools flag it, duration disagrees with content, and
mid-stream seeks land 6.5 ms early. Confirm with `ffprobe`/`opusdec`.

**CONFIRMED against a real file with `ffprobe` and `ffmpeg`, and the sweep was
right about both halves.** Measured by recording 48900 frames of 440 Hz at
48 kHz — 50 whole packets plus 900 samples of a 51st, so the final frame has
only 60 samples of padding and cannot absorb the encoder's 312 — then
decoded with a tool that has no stake in this code:

| | before | after |
|---|---|---|
| last granule position | 49212 | 49212 |
| samples a decoder can produce (960 × packets) | 48960 | 49920 |
| `ffprobe` duration | 1.025250 s | 1.025250 s |
| **samples `ffmpeg` actually decodes** | **48460** | **48900** |
| worst per-page overstatement | **+440** | **0** |

Two readings, both bad, and note that **`ffprobe` cannot see either of them**:
it reports duration from the granule position, which is exactly the number that
was wrong, so the file measured 1.025250 s before and after while its content
changed by 440 samples. Only decoding shows it.

- The file claimed 49212 while the stream held 48960 decodable samples. That is
  a granule position past the end of its own stream, which strict tools flag.
- **440 samples — 9.2 ms of the take — were not in the file at all.** They were
  still inside libopus when it was closed.
- Every page overstated by up to 440 samples, so a mid-stream seek landed that
  far early throughout. No duration check can see that, which is why the
  regression test walks every packet rather than only the last.

The remainder decides whether the TAIL loss shows: a recording whose length mod
960 is above 648 leaves less padding than the lookahead, which is a third of
all stop positions — "roughly one stop in three", as the sweep said. The
per-page overstatement was every recording, always.

**Fixed, in two halves:**

- **The granule position is now what a decoder will have produced** —
  `960 × packets`, which is the only thing RFC 7845's "decodable ... including
  the pre-skip" can mean when every packet decodes to 960 samples. The
  end-of-stream packet is CAPPED at `pre_skip + real audio` so the final
  frame's padding is still trimmed by arithmetic; a cap can only shorten.
- **finalize flushes the encoder's lookahead**, by topping the accumulator up
  to `pre_skip + the real audio` with silence before that last packet, so the
  real tail actually comes out of libopus. Expressed as "top up to what the
  file must contain" rather than "push 312 more", so it is right whether or not
  the resampler flush already produced some of them.

The recording's length is now derived from the frames that came off the ring
(`in_frames × 48000 ÷ rate`) rather than from the resampler's output count —
arithmetic that cannot drift, and the fix for **m15** falls out of it.

**Tests:** `test_action_ogg.c` —
`no_page_ever_claims_a_sample_the_stream_does_not_hold` (walks every packet: no
granule position may exceed 960 × packets-so-far; it scored +440 before),
`the_last_six_milliseconds_do_not_stay_inside_the_encoder` (the 48900-frame
case above: 48900 in, 48900 out, exactly; 48648 before),
`the_resamplers_priming_silence_is_not_part_of_the_duration`. All three print
their numbers, so the measurement above is reproducible by running the
suite.

### M6 — Device gap-fill is systematically short by ~one packet — **THE MECHANISM AS STATED IS WRONG; a different, real bug is in the same lines, and is FIXED**
`src/capture/wasapi_common.c:185-216` · `src/core/source.c:329-360`

The clock is anchored at the *arrival* of the first packet, but that packet's
frames were captured *before* it, so steady state runs ≈480 frames short.

**Failure:** a real dropout of G frames fills only G−480, and **any dropout
shorter than one packet is never filled at all**. The PI controller then
recovers by stretching ~10 ms over its 10 s tau instead of preserving
alignment. The 0.43-frame/3-hour result used fake sources, so the real-device
path is unproven. **Fix direction:** anchor at `now − frames·qpc_freq/rate`.

---

**Verified first, as asked, and the finding is kept rather than deleted because
its fix direction turned out to be right for a reason it did not give.**

**What is wrong with it.** The drift was measured against `s->frames`, which
counts everything delivered **before** the arriving packet — and that omission
cancelled the arrival-time anchor **exactly**. The arithmetic, on the old code:

> anchor = arrival of packet 1, at which point delivered = 0. Packet *n*
> arrives one period later than packet *n−1*, so at packet *n*:
> expected = (n−1)·480 and delivered = (n−1)·480. **delta = 0.**

So steady state did **not** run 480 short, a dropout of G filled exactly G, and
a sub-packet dropout filled exactly. `steady_state_with_uniform_packets_never_
fills` and `a_dropout_shorter_than_one_packet_is_still_filled` in
`tests/test_capture_timeline.c` are **green against the old code as well**, and
they are in the file specifically as that disproof.

**What was actually wrong, in the same two lines.**

1. **The anchor really was one packet late** — not for gap-fill, but for
   *placement*. `source.c:begin()` puts a source's ring frame 0 at the bus frame
   its anchor names, so every source landed ~10 ms late on the bus timeline.
   Invisible while every source has the same packet size, because they all move
   together; **a straight sync error the moment one does not.** A 1024-frame
   endpoint mixed with a 480-frame process tap sits 11 ms out and stays there,
   which is exactly the "Teams plus my mic" case design 5.2 exists for.

2. **The cancellation held only while packet sizes were uniform.** With a first
   packet of N frames and 480 thereafter, the steady-state delta was `480 − N`
   *for the rest of the session* — an engine that hands over a burst at `Start`
   and single periods afterwards left a permanent negative bias, and a real
   dropout an hour later was under-filled by exactly that much. That is the
   sweep's "systematically short", with the right sign and a different cause
   and magnitude.

**Fixed** by doing what the sweep's fix direction said, for the reason above:
anchor and measure at the packet's **first frame's capture time**,
`now − frames·qpc_freq/rate`, which puts the anchor and `delivered` on one
timeline whatever the packet sizes are. A late anchor (a first buffer flagged
`TIMESTAMP_ERROR` may not anchor) also walks back over frames already
delivered, so frame 0 is frame 0.

Both decisions moved out of `drain()` into two pure functions,
`apr_wasapi_packet_start()` and `apr_wasapi_packet_gap()`, so the whole of
design 5.2 step 4 is testable with no driver — which is why this was invisible
for so long.

**Tests:** `tests/test_capture_timeline.c`, 14 cases at QPF 3,579,545 (not
10 MHz — clock.h). **Red run** with the anchor put back to arrival:
`anchor_is_the_first_frames_capture_time_not_its_arrival`,
`a_late_anchor_walks_back_over_frames_already_delivered` and
`a_first_packet_of_a_different_size_does_not_bias_every_later_gap` all fail;
the last one is the interesting one, since it is a *dropout fifty packets in*
that the old bias silently eats.

**Still not closed by this pass:** the sweep is right that the real-device path
is unproven on hardware. There is also a residual the finding did not reach — a
drain sweep that hands over several packets at one arrival timestamp (a burst
after a stall) leaves a `(burst−1)·480` negative bias, because the pump cannot
see how many packets are pending. It is unchanged by this fix, it errs toward
under-filling rather than inventing silence, and closing it needs
`GetCurrentPadding` on the capture client. Worth its own entry.

### M7 — `apr_controller_model_changed` has no production caller
`src/ui/controller.c:1511-1519` (callers: tests only) · edits at
`canvas.c:1159-1207,1698-1707`

Its own header says that without it "the tree panel would still be showing the
old shape". That is now what happens.

**Failure:** disconnect with Ctrl+Shift+E, or complete a connect by click. The
canvas updates; the tree still says *"Mic — feeds Voice Mix"*. The view used
to audit a session reports a connection that no longer exists.

**FIXED (UI agent, 2026-08-26).** Through a new `apr_canvas_set_edit_sink()` rather than through
`apr_controller_model_changed()`, and the difference is deliberate: the sink
fires at the END of an edit, and its contract forbids the listener from
rebuilding the canvas -- doing that from inside an operation still in flight
would destroy the node windows that operation is about to focus. The controller
answers it by refreshing the TREE, which is the view that had no other way to
hear, plus the menu states. `apr_controller_model_changed()` remains the route
for a caller OUTSIDE the canvas (a test, a later scripting surface), which is
what its header describes.

Pinned by `an_edit_the_canvas_makes_by_itself_reaches_the_tree_panel`, which
compares every live TreeView row's text against the model's own projection.

### M8 — The tree's "recording" and "failed" clauses can never be heard when true **[2×]**
`src/ui/controller.c:969-1008,1077-1088` · `tree_panel.c:284-286,364-367`

Nothing rebuilds the tree when recording starts, or when an action fails.

**Failure:** start recording, F6 to the tree — **every bus reads as idle for the
whole session**. If the encoder fails an hour in, the row keeps saying it is
saving. The sentences exist, are queued for translation, and are unreachable.
(Related: `UI_PANE_RECORDING`, `UI_DESC_RECORDING`, `UI_HEALTH_*`,
`UI_STATUS_IDLE` are in the catalog and referenced nowhere in `src/`.)

**FIXED (UI agent, 2026-08-26).** `refresh_views()` now runs when a recording starts and when an
action fails. It runs AFTER the start announcement, not before: rebuilding two
views destroys and recreates every node window, which takes long enough that an
observer watching `apr_controller_recording()` can see "it is recording" a
measurable time before it can hear "recording started" -- a real ordering bug,
found because it broke an existing test.

Pinned by `the_tree_says_a_bus_is_recording_while_it_is_recording`, which waits
for the model's OWN `UI_TREE_BUS_RECORDING` sentence to appear in the live
control.

**The related note is still open:** `UI_PANE_RECORDING`, `UI_DESC_RECORDING`,
`UI_HEALTH_*` and `UI_STATUS_IDLE` remain unreferenced. `UI_HEALTH_*` looks
like it wants a node-window badge and `UI_PANE_RECORDING`/`UI_DESC_RECORDING`
like a pane that does not exist; both are design decisions, not defects, and
they were left alone.

### M9 — Events that only happen while backgrounded are announced only to the background window **[2×]**
`src/ui/controller.c:1059-1060` (`ARM_FAILED`) · `1090-1095` (`OUTPUT_RENAMED`)

`SOURCE_DIED`, `SOURCE_MUTED`, `ACTION_FAILED` and `STOPPED` honour the tray
contract. These two do not, and `OUTPUT_RENAMED` has no `TRAY_INFO_*` sibling
in the catalog at all.

**Failure:** start from the tray with the window hidden; a source fails to arm,
or the take is renamed aside — the sentence lands on a status bar nobody can
see. The rename notice is the one fact the honest-collision policy insists the
user be told.

**FIXED (UI agent, 2026-08-26).** Both go through `say_and_notify()` now, with two new catalog
entries: `UI_TRAY_INFO_ARM_FAILED` and `UI_TRAY_INFO_OUTPUT_RENAMED`.

Pinned by `a_take_that_moved_aside_is_told_to_a_window_that_is_not_in_front`.
Asserting it needed a new test seam, `apr_controller_last_balloon()` /
`apr_controller_balloon_count()`: every test runs with `APPRECORDER_NO_TRAY`
set (AGENTS.md rule 1), so there is deliberately no shell icon to observe, and
without the seam "it went out on the channel that can reach a hidden window"
was unassertable. The count half also pins m1 --
`nothing_balloons_while_the_window_is_in_front`.

### M10 — Hide-to-tray never checks the icon actually registered
`src/ui/controller.c:1256-1259,1174-1183` · `tray.c:94-108`

`Shell_NotifyIcon(NIM_ADD)` failure is a warning only; both hide paths call
`ShowWindow(SW_HIDE)` unconditionally.

**Failure:** shell refuses the icon (or Explorer crashed before
`TaskbarCreated`), user presses Ctrl+Shift+H — **the window vanishes with no
surface at all.** Win+B finds nothing, Alt+Tab finds nothing, and it is still
recording.

**FIXED (UI agent, 2026-08-26).** `apr_tray_is_registered()` answers the question, and both hide
paths -- Ctrl+Shift+H and the close dialog's "leave it recording in the
notification area" -- refuse and say `UI_ANN_NO_TRAY` instead. The recording
keeps running either way, which is what the user asked for; only the vanishing
is refused.

Pinned by `hiding_the_window_is_refused_when_there_is_no_icon_to_hide_into`,
which is free: `APPRECORDER_NO_TRAY` puts every test permanently in exactly
that state.

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

**FIXED (UI agent, 2026-08-26).** A decline sets a flag on the controller rather than being
encoded in the error, because the caller has to be able to tell "you said no"
from "this file is broken" and the return value alone cannot. `do_open_session`
then says `UI_DLG_SESSION_CANCELLED` -- *"That session was not opened."*

Pinned by `cancelling_a_session_load_is_not_reported_as_a_failure`, which drives
the whole File > Open route, picker included. That needed one more seam,
`apr_dlg_test_set_session_path()`: the common file picker is a system modal
owned by the thread that opened it, so File > Open and File > Save As were
routes no test could enter at all -- and the sentences live BELOW the picker.

M11 is the general form of the untranslatable-reason half and is NOT fixed
here; it is a `platform/err.c` change and belongs to whoever owns that file.

### M13 — `CREATE_ALWAYS` after a separate `exists()` check — **FIXED 2026-08-26**
`src/platform/outpath.c:508-541` vs `action_wav.c:637`, `action_mp3.c:699`,
`action_ogg.c:973`

**Failure:** two apprecorder processes (two scheduled tasks in the same second,
same template) both pass `exists()` before either creates, then both
`CREATE_ALWAYS` the same path and interleave writes. The take is corrupt, and
if a previous take was auto-renamed aside, the file at that name is garbage.
The "never overwritten" promise is advisory, not atomic. **Fix:** `CREATE_NEW`
plus retry through the collision loop.

**Fixed as suggested.** `outpath.c` gained `apr_out_open_new()`: CREATE_NEW,
and on `ERROR_FILE_EXISTS` it walks the same `-2, -3, ...` ladder
`apr_out_resolve` walks — the ladder itself is now one function, `take_name()`,
used by both, so the two cannot produce different names. All three actions open
through it instead of calling `CreateFileW(..., CREATE_ALWAYS, ...)`
themselves, so the create IS the claim and "a take is never overwritten" is a
property of the file system rather than of how little time passes between two
calls.

`apr_out_resolve` deliberately keeps its no-side-effects contract: the bus
still resolves the template first, because that is what it reports as the
recording's name. In the ordinary case the resolved name is still free and the
open succeeds on the first try.

**One residual, and it is in `bus.c`, which this pass does not own.** If the
race actually happens, the action lands on `mix-2.wav` while the bus's
`current` still says `mix.wav`, so the reported name is wrong for that one run
(the audio is safe, which is the point). The action logs the discrepancy. The
clean finish is one line in `open_action()` — take the final name back from the
action — and needs whoever owns `src/core/bus.c`; the shape to use is
`AprActionConfig` gaining an optional out-path field.

**Tests:** `test_outpath.c` —
`opening_a_new_take_never_touches_a_file_that_is_already_there`,
`a_free_name_is_opened_as_itself_and_reports_no_collision`,
`the_handle_that_comes_back_is_the_one_the_recording_writes_through`,
`a_path_that_cannot_be_created_at_all_fails_rather_than_counting`.
`test_action_wav.c` — `a_take_already_on_disk_is_never_truncated_by_the_next_create`,
which is the race itself: the file is already there when `create()` runs, and
the eight bytes in it are still there afterwards.

### M14 — `save-session` silently drops sources past 64 — **FIXED 2026-08-26**
`src/cli/cli.c:2395,2450`

`intern_source` returns `-1` when full and the caller `continue`s — no warning,
no error, **exit 0**, and JSON reports `"sources": 64`.

**Failure:** silent config loss in the exact artifact you rely on to reproduce
a recording.

**Fixed:** the full pool is now a refusal — exit 2, naming the source that did
not fit — and no file is written at all. A session that cannot hold what was
described is not a session worth writing, and the alternative is a loss nobody
notices until the session is replayed weeks later and a bus is missing an
input.

**Test:** `test_session.c` —
`a_session_that_cannot_hold_every_source_is_refused_rather_than_trimmed`
(70 distinct synthetic sources over seven buses; refused, and no file left
behind).

### M15 — Ctrl+T with focus on the splitter strands focus in a hidden window **[2×]**
`src/ui/app.c:744-757`

The focus rescue checks the tree pane but not the splitter, which the same
relayout hides.

**Failure:** Tab to "Panel divider", press Ctrl+T. Focus stays on an invisible
window; the reader goes quiet; arrows silently resize a hidden panel.

**FIXED (UI agent, 2026-08-26).** The rescue checks the splitter as well as the tree pane.

Pinned by `ctrl_t_with_focus_on_the_divider_does_not_strand_it_in_a_hidden_window`,
which Tabs to the divider for real and then asserts that whatever holds focus
afterwards is a VISIBLE window.

---

## Minor

| # | Finding | Where |
|---|---|---|
| m1 | `ACTION_FAILED` balloons unconditionally — heard twice in the foreground — **FIXED. Routed through `say_and_notify()` like every other event. Pinned: `nothing_balloons_while_the_window_is_in_front`** | `controller.c:1082` |
| m2 | Close-timeout replays "will close when written" and then never closes — **FIXED. Its own sentence, `UI_ANN_CLOSE_TIMEOUT`. Pinned: `a_close_that_runs_out_of_patience_...` (needed `apr_controller_test_set_close_wait_ms`)** | `controller.c:1198` |
| m3 | "Remove output" with no outputs says *"There is no bus"* — **FIXED. `UI_DLG_NO_OUTPUTS`. Pinned: `removing_an_output_from_a_bus_that_has_none_talks_about_outputs`** | `controller.c:529` |
| m4 | Real edit failures announced as *"not available yet"*; the `AprErr` is discarded — **FIXED. `UI_ANN_EDIT_FAILED` carries the reason `apr_err_format()` gives. Pinned: `an_edit_the_model_refuses_is_announced_with_the_reason_the_model_gave`** | `canvas.c:1179,1256` |
| m5 | Dialog create-failure is loud in the log, still silent to the user — **FIXED. `apr_dlg_last_failed()` separates "rejected" from "cancelled"; the controller announces `UI_DLG_CREATE_FAILED`. Pinned: `a_window_that_could_not_be_created_is_announced_rather_than_logged` (via `apr_dlg_test_fail_next`)** | `dialogs.c:200` + `controller.c:365` |
| m6 | Add Output: OK can silently do nothing when the format combo is empty — **FIXED, PARTLY UNREACHABLE — read the note below** | `dialogs.c:1299-1308` |
| m7 | File > New / Open discard an unsaved session with no prompt — **FIXED. A `dirty` flag plus a confirm on File > New / Open. Pinned: `a_session_with_unsaved_changes_is_not_discarded_without_asking`** | `controller.c:1214` |
| m8 | `SetFocus` from inside `WM_SETFOCUS` in frame and canvas — the pattern `tree_panel.c` measured as swallowed — **FIXED in both, but see the note below — the canvas half is not independently reproducible** | `app.c:992`, `canvas.c:1636` |
| m9 | A pending connect whose node vanishes ends the mode silently — **FIXED. The rebuild says `UI_ANN_CANCELLED` when the held end has gone. Pinned: `a_half_made_connection_whose_end_disappears_says_it_has_been_cancelled`** | `canvas.c:924` |
| m10 | `OFN_OVERWRITEPROMPT` asks "replace?" but outpath renames aside instead — **FIXED (flag removed). NOT PINNED — see the note below** | `dialogs.c:1197` |
| m11 | Session title bypasses `UI_TITLE_SESSION`; frame name becomes a bare path — **FIXED. `set_session_title()` formats `UI_TITLE_SESSION`. Pinned: `the_frame_title_is_the_catalogs_sentence_and_not_a_bare_path`** | `controller.c:725,896` |
| m12 | **FIXED** — `±Inf` gain passes validation (NaN is caught) → sustained full scale | `bus.c:181,248` |
| m13 | ~~`--json` emits **no document at all** on session-resolve failure~~ **FIXED** — the reasons were all said through `warn()`, which is silent in JSON. The refusal now goes through `fail()`, which is the one place that writes the failure shape; text mode gains the verdict line it was missing anyway. | `cli.c:2314-2335` |
| m14 | ~~A >4 s disk stall becomes silence; run still exits 0, loss only in the log~~ **FIXED** — all three `finalize`s now return an error when frames were replaced with silence, so the run ends **6** and the CLI says so. Reported from `finalize`, **not** `on_audio`: an error out of `on_audio` drops the output from the bus for the rest of the session, which would turn a hole into a truncation. The sentence is its own (`WARN_OUTPUT_DEGRADED`), because "stopped taking audio" is untrue of a file that is closed and plays. | `action_*.c` overrun paths |
| m15 | ~~Ogg: resampler priming silence can inflate duration (non-48 k only)~~ **FIXED with M5** — length now comes from `in_frames × 48000 ÷ rate`, the frames that came off the ring, and never from the resampler's output count. | `action_ogg.c:719` |
| m16 | ~~`--format WAV` refused while `--out x.WAV` works (case-sensitive)~~ **FIXED** — `apr_action_find` matches an id case-insensitively (ASCII, hand-written, so the CRT locale gets no vote), and `apr_cli_resolve` writes the vtable's canonical id back, which is what keeps a session file holding the wire value rather than the case someone typed. | `registry.c:133` |
| m17 | ~~Two `take{n}.wav` outputs both expand to `take1` and are refused as duplicates~~ **FIXED** — a template containing `{n}` is excluded from the duplicate comparison, because `{n}` is resolved against the disk when each recording starts and therefore names a different file every time. New `apr_out_has_token()` so the CLI can ask outpath.c rather than parsing braces itself. | `cli.c:1292` |
| m18 | ~~`prescan` honours `--json`/`--quiet` even as another option's value~~ **FIXED** — `prescan` now walks the same grammar the parser does and steps over each option's value. The list of value-taking options was extracted into `option_takes_value()` and is read by both, so they cannot part company. | `cli.c:490` |
| m19 | ~~`CTRL_CLOSE` waits 4 s; the header promises it blocks until files are closed~~ **FIXED** — the wait is now `INFINITE`. A four-second cap did not *risk* a kill part-way through finalize, it **guaranteed** one at four seconds; Windows decides when we die either way, and while we are still alive it offers the user an End Task dialog they may decline. Pinned by `apr_cli_test_close_wait_ms()`. | `cli.c:426` |
| m20 | ~~`\t`/`\r`/`\n` are legal JSON escapes and silently mangle hand-edited paths~~ **FIXED** — path-typed fields go through `tok_wpath()`, which refuses any character below 0x20 after unescaping. A Windows filename cannot hold one, so this costs nothing that was going to work; **display names are deliberately left alone**, so a session that round-tripped correctly still does. | `session_load.c:194` |
| m21 | ~~`tok_wstr` truncates over-long session strings without a fault~~ **FIXED** — running out of room is now `APR_SESSION_FAULT_BAD_TYPE` with the JSON path of the field. Found while fixing it: **every** failure path in that loop returned with `out` unterminated, and the unknown-key call site ignores the return by design — all of them now exit through one `bad:` label that terminates. | `session_load.c:190` |
| m22 | Literal `L"0"` reaches the user instead of `apr_str_number(0,…)` — **FIXED. NOT PINNED — see the note below** | `dialogs.c:1270` |
| m23 | Help dialog key names are hardcoded English; catalog sentences translate theirs — **FIXED. 17 new `UI_KEYNAME_*` entries. Pinned: `every_named_key_takes_its_name_from_the_catalog`** | `dialogs.c:378-410` |
| m24 | Fixed 96-unit buttons clip their own English, let alone Arabic — **FIXED. `apr_dlg_button_width()`, and the dialog grows to hold the buttons. Pinned: `a_button_is_sized_to_its_own_caption_...` and `the_close_dialogs_buttons_are_wide_enough_for_their_own_captions`** | `dialogs.c:520-551` |
| m25 | Register: "write/wrote/written" where a person says "save" | `strings.rc:75,183,206,343,418,437` |
| m26 | **FIXED** — `apr_log_shutdown` races its drain thread after a 2 s timeout | `log.c:334-357` |
| m27 | **FIXED** — `apr_wasapi_call` running-check is a TOCTOU against `start()` (latent) | `wasapi_common.c:414` |
| m28 | **FIXED** — `apr_runner_destroy` doesn't wait for a *synchronous* run (latent) | `runner.c:409` |
| m29 | `interactive=0` session open returns ok despite unresolved sources — **FIXED. Returns `APR_E_NOT_FOUND` when resolve dropped something. Pinned: `a_session_opened_with_nobody_to_ask_reports_the_sources_it_dropped`** | `controller.c:709` vs `ui_controller.h:132` |

### Minors this pass fixed, with what each one turned out to be

**m12 — `±Inf` gain.** Both call sites (`apr_bus_add_source`,
`apr_bus_set_gain`) tested `gain == gain`, which catches NaN only. Now one
shared `gain_is_finite()` using `isfinite()`, so the two cannot drift apart
again. Worth saying why this was the wrong half to catch: **NaN is the mild
case.** `core/mix.c` scrubs NaN on the way into every integer format, so a NaN
gain renders as *silence* — wrong, but quiet. Infinity does not scrub to
silence; it clamps, and the take comes out as sustained digital full scale for
as long as the recording runs, in the headphones of someone who cannot see a
meter. AGENTS.md rule 1 makes that the worse outcome by a distance, and it was
the one getting through. Tests: `test_graph_guard.c`, four cases covering both
infinities, NaN and an ordinary finite gain still going through.

**m26 — `apr_log_shutdown` vs its drain thread.** It waited 2 s, *ignored the
result*, and then closed the file handle, cleared `g_sink` and drained the ring
itself — while a thread that had not left was inside `emit_line()` doing all
three: two consumers on a single-consumer ring, a `WriteFile` to a handle about
to close, and a sink pointer swapped under a live call. Now returns `AprErr`;
on abandonment nothing is torn down, `g_thread` and `g_wake` are **kept** so a
later call can finish the job, and `apr_log_init` refuses while a shutdown is
outstanding rather than `memset`-ing the ring under a live reader. Test:
`test_log.c`, using a sink that blocks — a blocking sink *is* a drain thread
that will not leave. **Red run:** returns success, and the second record never
reaches the sink because `g_sink` was yanked.

**m27 — `apr_wasapi_call` TOCTOU.** The `running` check moved inside the
critical section. `apr_wasapi_start` raises `running` *before* it takes the
lock, so whoever gets the lock second now sees the truth. Left outside, both
could find it clear, `start` would win the lock and turn the thread into the
pump, and this call would post a job into a queue nobody was serving and wait
on it **for ever** — `post()` waits `INFINITE` by design, because an accepted
job always completes. So the symptom was a hang, not a refusal. Test:
`test_capture_abandon.c`, via a race hook (`apr_wasapi_test_set_race_hook`) that
fires at exactly the instant the answer could go stale. **Red run:** the job is
posted and runs, and the call returns success.

**m28 — `apr_runner_destroy` and a synchronous run.** It joined the thread
`apr_runner_run_async` creates and nothing else; `apr_runner_run` executes on
the *caller's* thread, for which there is no handle, so destroy fell straight
through to `free()` while a live loop was still writing into that allocation and
had not finalized a file. Now waits — and on a **new `loop_left` event, not
`finished_event`**, which matters: `finished_event` means "the files are
closed", and the loop still reads `r` after setting it (the `STOPPED` notice).
Waiting on that one narrows the window instead of closing it. Bounded at 30 s
(a normal tail is one lookbehind block plus every `finalize`, which may touch
disk); on timeout nothing is freed and an `AprErr` comes back. Test:
`test_run_loop.c` runs the recording on a worker thread and asserts destroy
returned *after* `apr_runner_run` did. **Red run:** the flag is still 0.

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

---

## 2026-08-26, UI pass — what was done, and what was NOT

**Fixed and pinned:** C2, C3, C4 (UI half), M4, M7, M8, M9, M10, M12, M15, and
m1, m2, m3, m4, m5, m7, m9, m11, m23, m24, m29. Each fix was checked by
reverting it and watching the named test fail; the reversions are not in the
tree.

**Honest residue, because a tracker that only records wins is a tracker nobody
can trust:**

- **m6 is fixed but its stated case is unreachable.** Every `return TRUE` on an
  empty combo now names what is missing and puts focus on it. But the FORMAT
  combo cannot be empty in a build that has encoders, and the BUS combo turns
  out to be guarded a step earlier -- `apr_dlg_add_output()` refuses with
  `UI_DLG_NO_BUSES` before it builds the dialog at all. So the guards are
  defence in depth and the test
  (`add_output_with_no_bus_to_put_it_on_says_so_instead_of_opening_empty`) pins
  the path a person actually takes, not the branch the sweep named. **The
  sweep's line "OK can silently do nothing" was right about the SHAPE and wrong
  about the reachability.**

- **m8: the canvas half is not independently reproducible.** Both the frame and
  the canvas now POST a private message instead of calling `SetFocus` inside
  `WM_SETFOCUS`, matching the pattern `tree_panel.c` measured. But reverting
  the canvas half does not fail any test: the nested call takes, here, on this
  build. It is applied for consistency with the measured case and because the
  failure mode is silent, not because it was observed. `focus_arriving_at_the_
  canvas_lands_on_a_node_and_not_on_the_pane` asserts the PROPERTY (the
  platform is asked which window really holds focus) and passes either way.

- **m10 and m22 are fixed by inspection, with no test.** m10 removes
  `OFN_OVERWRITEPROMPT` from the audio-output picker, which asked "replace it?"
  about a file the collision policy never replaces; a common-dialog flag is not
  observable from a test that must not open a system modal. m22 puts the
  literal `L"0"` through `apr_str_number()`; English and the current digit
  policy render both as "0", so no assertion can tell them apart until the
  Arabic-Indic decision actually differs. Both are recorded here rather than
  claimed as pinned.

- **M8's related note is still open.** `UI_PANE_RECORDING`,
  `UI_DESC_RECORDING`, `UI_HEALTH_*` and `UI_STATUS_IDLE` remain referenced
  nowhere in `src/`. They look like a node-window health badge and a pane that
  does not exist -- design decisions, not defects -- and this pass left them
  alone rather than inventing a use for them.

- **M11 is NOT fixed.** Error reasons are still English prose in a translated
  frame. The change belongs in `platform/err.c`, which this pass does not own.
  m4 and M12 remove the two worst UI symptoms of it (a real reason now reaches
  the user instead of "not available yet"; a cancel no longer quotes an
  internal literal back at the person who chose it), but the mechanism gap is
  untouched.

**New test seams, each the smallest thing that made an unassertable property
assertable:** `apr_controller_last_balloon()` / `apr_controller_balloon_count()`
(every test runs with `APPRECORDER_NO_TRAY`, so there is no shell icon to
watch), `apr_controller_test_set_close_wait_ms()` (the give-up path is otherwise
thirty seconds of stalled disk away), `apr_dlg_test_fail_next()` and
`apr_dlg_test_set_session_path()` (a system modal owns the thread that opened
it, so File > Open was a route no test could enter).

**One ordering bug found by a test rather than by reading:** M8's
`refresh_views()` was first placed before the "recording started" announcement.
Rebuilding two views destroys and recreates every node window, which takes long
enough that an observer watching `apr_controller_recording()` sees "it is
recording" measurably before it can hear "recording started". It now runs after
the sentence.
