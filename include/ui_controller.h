/*
 * ui_controller.h -- the thing that was missing.
 *
 * ===========================================================================
 * WHAT IT IS
 *
 *   Until this file the UI had a frame, a canvas, a tree panel and no
 *   CONTROLLER: nothing owned a graph, nothing answered a menu command,
 *   nothing could record. The two views could navigate a model somebody else
 *   built and rewire it, and that was the whole product.
 *
 *   This is the missing half. It owns the graph, answers every APR_CMD_*,
 *   drives the dialogs that build a graph from nothing, loads and saves
 *   sessions, runs a recording through core/runner.c, and keeps the frame, the
 *   canvas, the tree panel and the notification area saying the same thing.
 *
 * ===========================================================================
 * RECORDING RUNS OFF THE UI THREAD. THE UI OBSERVES.
 *
 *   A message loop that blocks is a window that has stopped answering the
 *   accessibility tree, so the recording loop gets its own thread
 *   (apr_runner_run_async). Everything it has to say arrives as a POSTED
 *   message carrying a copy of the notice -- never a pointer into the graph,
 *   never a SendMessage from the runner thread, which would deadlock the
 *   moment the UI thread was itself waiting on the runner.
 *
 *   THE GRAPH IS NOT TOUCHED WHILE A RECORDING RUNS. graph.h says the shape
 *   must not change while a tick is in flight, so every editing command is
 *   disabled for the duration and says why out loud when it is invoked
 *   anyway -- greyed AND spoken, because grey alone says nothing to this
 *   application's first user.
 *
 * ===========================================================================
 * NOTHING IS LOST TO A CLOSE. THAT IS THE POINT OF THE CLOSE HANDLER.
 *
 *   WM_CLOSE while recording asks a real question with three real answers:
 *   stop and close (finalize, then exit), leave it recording in the
 *   notification area, or do nothing. Whichever the user picks, no path
 *   reaches DestroyWindow with an action unfinalized.
 *
 *   WM_ENDSESSION is not a question. Windows terminates the process shortly
 *   after the handler returns, so the handler blocks until every file is
 *   closed -- exactly what the CLI's console control handler does for
 *   CTRL_CLOSE_EVENT, and for exactly the same reason.
 *
 * THREADING: everything here is UI-thread only except the runner observer,
 * which posts.
 */
#ifndef APPRECORDER_UI_CONTROLLER_H
#define APPRECORDER_UI_CONTROLLER_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "capture.h"
#include "err.h"
#include "graph.h"
#include "ui_app.h"
#include "ui_tray.h"

/* Posted to the frame by the runner's observer, from the runner's thread.
 * lParam is a heap AprRunNotice the UI thread frees. */
#define APR_CTL_WM_NOTICE (WM_APP + 0x50)

/* The one-second clock that keeps the status bar and the tray tooltip honest.
 * It does NOT announce -- see apr_ui_app_set_status_text. */
#define APR_CTL_TIMER_CLOCK 0x101

typedef struct AprController AprController;

/* Adopts `app`: installs the command, close and message handlers, wires the
 * canvas's announcement sink to the status bar, wires the tree panel's
 * selection to the canvas, creates the notification-area icon, and starts with
 * an empty graph. */
AprErr apr_controller_create(AprUiApp *app, AprController **out);

/* Stops any recording (finalizing every action) before it frees anything. */
void apr_controller_destroy(AprController *c);

AprGraph *apr_controller_graph(AprController *c);

/* Replace the model. The controller takes ownership; the previous graph is
 * destroyed. Refused while recording. */
AprErr apr_controller_set_graph(AprController *c, AprGraph *g);

int apr_controller_recording(const AprController *c);

/* Nonzero while the recording is PAUSED -- once the loop has actually paused,
 * not once one was asked for (runner.h). A recording that is paused is still a
 * recording: apr_controller_recording() stays nonzero, every editing command
 * stays refused, and the files stay open. */
int apr_controller_paused(const AprController *c);

/* The model changed behind the controller's back -- re-read it.
 *
 * NOT a test hook. The canvas owns keystrokes the frame's accelerator table
 * does not claim (Ctrl+Shift+E disconnects, +/- change a level), so it can
 * legitimately edit the graph without a command ever reaching here. Without
 * this, "Start Recording" would stay greyed after the edit that gave the
 * session its first output, and the tree panel would still be showing the old
 * shape.
 *
 * Safe from any thread: it marshals onto the window's own thread, because
 * refreshing a view from anywhere else is how a UI stops answering UI
 * Automation. */
void apr_controller_model_changed(AprController *c);

/* ---------------------------------------------------------------------------
 * Building a graph -- the verbs, without the choosers
 *
 * THE DIALOG CHOOSES; THESE DO THE WORK. The same split as the session pair
 * below, for the same two reasons: a modal dialog owns the thread that opened
 * it, so the only way to drive the real path from a test is to enter it below
 * the chooser; and a later scripting surface wants "add this source" without a
 * picker in front of it.
 *
 * Everything a user receives is in here -- the model change, both views, the
 * menu states, and the sentence that goes to the status bar's live region --
 * so no second route can announce something different.
 * ------------------------------------------------------------------------- */

AprErr apr_controller_add_source(AprController *c, const wchar_t *name,
                                 const AprCaptureConfig *cfg);
AprErr apr_controller_add_bus(AprController *c, const wchar_t *name);

/* ---------------------------------------------------------------------------
 * Sessions
 *
 * The DIALOG chooses which file; these do the work. Splitting it that way is
 * what makes loading and saving testable at all -- a modal file picker cannot
 * be answered from the thread that opened it -- and it is also the right shape
 * for a later scripting surface, which wants the verb without the chooser.
 * ------------------------------------------------------------------------- */

/* Load `path`, resolve every source against the live machine, and adopt the
 * result as the model.
 *
 * `interactive` decides what happens when the file does not describe the
 * machine it is being opened on: nonzero shows the resolve report and asks,
 * zero takes what resolved and reports the rest through the return value. A
 * session containing an EXCLUDE source is NEVER loaded non-interactively --
 * design 4.1.1 says a file is not consent, and there is nobody to ask. */
AprErr apr_controller_open_session(AprController *c, const wchar_t *path,
                                   int interactive);

/* Load `path` and SAY what happened -- loaded, declined, or the file's own
 * fault -- in the same words File > Open uses. This is the whole of that
 * command minus the chooser, and it exists because the process has a second
 * way in: `apprecorder my.json` opens the window on a double-clicked session
 * (frontend.h), and a startup path that reported nothing would be a window
 * that came up empty with no explanation. Returns 1 always, like every other
 * command handler. */
int apr_controller_open_session_and_report(AprController *c,
                                           const wchar_t *path);

/* Write the live graph to `path` and remember it as the current session. */
AprErr apr_controller_save_session(AprController *c, const wchar_t *path);

/* The session file currently open, or an empty string. */
const wchar_t *apr_controller_session_path(const AprController *c);

/* Milliseconds of the recording in flight, or of the one that just finished. */
int64_t apr_controller_elapsed_ms(const AprController *c);

/* Run one command, exactly as the menu, an accelerator or the notification
 * area would. This IS the command handler -- a test driving it is driving the
 * real path and not a parallel one. */
int apr_controller_command(AprController *c, int command_id);

/* The last thing the controller said out loud, which is also what went to the
 * status bar's live region. "The user was told" is otherwise a property only a
 * human with a screen reader can check. */
/* TEST ONLY. A balloon is raised only when no better channel exists -- see
 * a_better_channel_exists() in controller.c. A test cannot reliably make
 * itself the foreground window, so it forces the answer: -1 real, 0 the
 * window is not in front, 1 it is. */
void apr_controller_test_set_foreground(AprController *c, int state);

/* TEST ONLY. How long "stop recording and close" waits for the encoders before
 * it gives up. -1 restores the real thirty seconds.
 *
 * The give-up path is otherwise thirty seconds of stalled disk away, and what
 * it SAYS is the thing worth asserting: it used to replay "the window will
 * close once the files are written" and then never close, which is
 * indistinguishable from a hung application. */
void apr_controller_test_set_close_wait_ms(AprController *c, int ms);

size_t apr_controller_last_announcement(const AprController *c,
                                        wchar_t *buf, size_t cch);

/* TEST ONLY. The last sentence sent to the notification area, and how many
 * have been sent.
 *
 * "It went out on the channel that can actually reach the user" is the whole
 * of the tray contract, and it was otherwise unassertable: the events that
 * matter most happen while the window is HIDDEN, and every test runs with
 * APPRECORDER_NO_TRAY set (AGENTS.md rule 1) so there is no shell icon to
 * watch. The count is the other half -- a balloon raised while the window is
 * in FRONT is a duplicate a screen reader reads twice, and that has to be
 * assertable too. */
size_t apr_controller_last_balloon(const AprController *c,
                                   wchar_t *buf, size_t cch);
unsigned apr_controller_balloon_count(const AprController *c);

/* What the notification area's tooltip is currently saying, as a state.
 *
 * Same argument as the balloon seams above: APPRECORDER_NO_TRAY is set for
 * every test (AGENTS.md rule 1), so there is deliberately no shell icon to
 * observe, and "the tooltip a screen reader reads on Windows+B says PAUSED"
 * would otherwise be checkable only by a human with a mouse. The tray object
 * exists either way and is the thing that formats the tooltip; this is what it
 * was last told. */
AprTrayState apr_controller_tray_state(const AprController *c);

/* ---------------------------------------------------------------------------
 * Pure
 * ------------------------------------------------------------------------- */

/* "01:12:30" -- hours, minutes and seconds, each through apr_str_number() so
 * the digit-shaping decision stays in one function, and assembled by a catalog
 * format so a locale that writes durations differently can. */
size_t apr_controller_format_elapsed(int64_t ms, wchar_t *buf, size_t cch);

#endif /* APPRECORDER_UI_CONTROLLER_H */
