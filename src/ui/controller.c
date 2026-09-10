/*
 * controller.c -- the graph, the recording, the dialogs and the tray, wired
 * together. See ui_controller.h for what it is and why it did not exist
 * before. What follows is the reasoning behind the parts that look arbitrary.
 *
 * ---------------------------------------------------------------------------
 * A STATE CHANGE IS SAID ONCE, ON WHICHEVER CHANNEL CAN ACTUALLY REACH THE USER
 *
 *   say() writes the sentence to the status bar and fires a live-region
 *   change; that is what a screen reader picks up while the window is in
 *   front. But this application spends its recordings MINIMISED, and a
 *   live-region change on an unfocused background window is not reliably
 *   announced by any reader. So when the window is NOT in front, the sentence
 *   goes out as a notification-area balloon instead -- which readers do
 *   announce reliably and which a sighted user can see too.
 *
 *   INSTEAD, not as well. An earlier version sent both unconditionally, so a
 *   screen reader read every event twice and a test run buried the author's
 *   notification centre during a meeting. A balloon is for something that
 *   would otherwise be MISSED, never for confirming what the user just did.
 *   See a_better_channel_exists().
 *
 *   THIS PRODUCT NEVER SPEAKS DIRECTLY. No SAPI, no nvdaControllerClient, no
 *   Tolk. UIA and the shell hand semantics to the screen reader, which then
 *   applies the user's own voice, rate, verbosity, interruption rules and
 *   braille routing. See ui_tray.h.
 *
 * ---------------------------------------------------------------------------
 * WHY THE ONE-SECOND CLOCK DOES NOT ANNOUNCE
 *
 *   The status bar is a live region, so writing to it can speak. An elapsed
 *   time that spoke once a second would make the application unusable within
 *   a minute. The clock therefore writes with announce=0: the text is there
 *   for anyone who reads the status bar deliberately, and for the tray tooltip
 *   that Windows+B reaches, and it interrupts nobody.
 *
 * ---------------------------------------------------------------------------
 * THE RUNNER'S NOTICES ARE POSTED, NEVER SENT
 *
 *   The observer runs on the recording thread. SendMessage from there would
 *   deadlock the instant the UI thread was itself waiting on the runner --
 *   which is exactly what the close path does. So a notice is copied onto the
 *   heap and posted, and the UI thread frees it. The copy is also why the
 *   notice carries names rather than pointers (runner.h).
 */
#include "ui_controller.h"

#include <shellapi.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "action.h"
#include "capture.h"
#include "clock.h"
#include "discover.h"
#include "errmsg.h"
#include "log.h"
#include "runner.h"
#include "session.h"
#include "strings.h"
#include "ui_canvas.h"
#include "ui_dialogs.h"
#include "ui_node.h"
#include "ui_tray.h"
#include "ui_tree_panel.h"
#include "update.h"
#include "version.h"

/* "The model changed; re-read it." Marshalled rather than called, so any
 * thread may raise it and the work still happens on the window's own. */
#define APR_CTL_WM_REFRESH (WM_APP + 0x51)

/* "Adopt this graph." Same reason, and a sharper one: adopting a graph builds
 * the canvas's node windows, and a window belongs to the thread that created
 * it. Building them on a caller's thread leaves the frame unable to destroy
 * them -- a close that hangs for ever, with no error anywhere. lParam is the
 * AprGraph *. */
#define APR_CTL_WM_SET_GRAPH (WM_APP + 0x52)

#define CTL_TEXT_CCH 1024

/* How long a close path waits for the encoders to finish. Generous: an action
 * flushing a four-second write-behind buffer to a stalled disk is normal, and
 * losing a recording to an impatient timeout is the failure this whole path
 * exists to prevent. */
#define CTL_FINALIZE_WAIT_MS 30000

struct AprController {
    AprUiApp *app;
    HWND      frame;
    HWND      canvas;
    HWND      tree;

    AprGraph *graph;
    AprTray  *tray;

    AprRunner *runner;
    int        recording;
    /* The loop has ACTUALLY paused -- not merely been asked to. The two are a
     * tick apart, and a status line that reported the request would say
     * "paused" while frames were still being written (runner.h). */
    int        paused;
    int        closing;          /* a close is waiting on the files */
    int        allow_close;
    int64_t    last_elapsed_ms;

    wchar_t session_path[APR_DISC_PATH_CCH];
    wchar_t last_said[CTL_TEXT_CCH];

    /* The last sentence handed to the notification area, and the count of
     * them. "It went out on the channel that can reach a hidden window" is
     * otherwise a property only a human watching his own notification centre
     * can check -- and with APPRECORDER_NO_TRAY set for every test (AGENTS.md
     * rule 1) there is deliberately no shell icon to observe. */
    wchar_t last_balloon[CTL_TEXT_CCH];
    unsigned balloons;

    /* The model has been changed since it was last saved or loaded. Kept here
     * rather than in graph.h because "unsaved" is a property of this session
     * DOCUMENT, not of the audio graph -- the CLI builds graphs it never
     * intends to write down. */
    int dirty;

    /* The last open_session ended because the USER SAID NO, not because
     * anything failed. A cancel is not a failure and must not be announced as
     * one, and the return value alone cannot tell them apart. */
    int declined;

    /* -1 = ask the real windowing state. 0/1 force it, for tests only. */
    int fg_override;

    /* -1 = CTL_FINALIZE_WAIT_MS. Tests only: the close-timeout path is thirty
     * seconds of stalled disk away otherwise, and what it SAYS when it gives
     * up is the thing worth asserting. */
    int close_wait_override;

    /* ---- staying up to date (update.h) ------------------------------------
     *
     * All five are touched only on the window's own thread. The check itself
     * runs on a worker and its answer arrives as a posted APR_CTL_WM_UPDATE,
     * exactly as a run notice does, so there is nothing here to synchronise. */
    const AprUpdateHttp *update_http;    /* NULL = the real WinHTTP one   */
    int      update_answer;              /* -1 ask; 0/1 force. Tests only */
    int      update_staged;              /* verified, waiting for exit    */
    wchar_t  update_version[APR_UPDATE_VERSION_CCH];
    wchar_t  update_image[APR_DISC_PATH_CCH];  /* empty = the running exe */

    /* SET WHILE NOTHING IS RECORDING (ui_controller.h). Manual reset, created
     * signalled, cleared by start_recording and set again by
     * recording_finished -- which is the only place that knows the files are
     * closed. It exists so that a test waiting for a take to finish waits on
     * the transition instead of polling a flag against a ceiling that load can
     * cross. */
    HANDLE idle_event;
};

/* Forward-declared because the update section sits below the recording one and
 * the end of a recording is one of its three triggers (update.h section 4).
 * Moving the section up instead would put the least interesting part of this
 * file in front of the most. */
static void start_update_check(AprController *c, AprUpdateWhy why);

/* One session-shaped object at a time; AprSession is a few hundred kilobytes
 * of fixed arrays and does not belong on a 1 MB stack (session.h). */
static AprSession              g_session;
static AprSessionLoadReport    g_load_rep;
static AprSessionResolveReport g_resolve_rep;

/* ==========================================================================
 * Saying things
 * ======================================================================== */

static void say_text(AprController *c, const wchar_t *text)
{
    if (!c || !text) return;
    lstrcpynW(c->last_said, text, CTL_TEXT_CCH);
    apr_ui_app_set_status_text(c->app, text, 1);
}

static void say(AprController *c, AprStrId id, const wchar_t *const *args,
                size_t nargs)
{
    wchar_t text[CTL_TEXT_CCH];
    apr_str_format(id, text, CTL_TEXT_CCH, args, nargs);
    say_text(c, text);
}

static void say0(AprController *c, AprStrId id)
{
    say(c, id, NULL, 0);
}

/* IS THERE A BETTER CHANNEL THAN A BALLOON RIGHT NOW?
 *
 * A balloon is for something the user would otherwise MISS -- never for
 * confirming what they just did. When the window is in front, the status bar
 * live region has already said it, and a balloon is pure duplication: a screen
 * reader reads both, so every event is heard twice. The author put it well
 * while a test run was flooding his notification centre mid-meeting:
 *
 *     "I think notifications only run if we started / stopping recording from
 *      the system tray, or in case of an error, right?"
 *
 * That is exactly right, and it collapses to one test rather than a list of
 * cases: a tray-initiated action implies the window is not in front, so it
 * needs no special handling. Hidden to tray, minimised, or simply behind
 * something else all mean the same thing -- no better channel exists.
 *
 * `fg_override` exists because a test cannot make itself the foreground
 * window reliably: -1 asks the real windowing state, 0 and 1 force it. Same
 * shape as action_wav.c's write gate. */
static int a_better_channel_exists(const AprController *c)
{
    HWND fg;

    if (!c || !c->frame) return 0;
    if (c->fg_override >= 0) return c->fg_override;
    if (!IsWindowVisible(c->frame)) return 0;   /* hidden to the tray */
    if (IsIconic(c->frame)) return 0;           /* minimised */
    fg = GetForegroundWindow();
    return fg == c->frame || IsChild(c->frame, fg);
}

void apr_controller_test_set_foreground(AprController *c, int state)
{
    if (c) c->fg_override = state;
}

void apr_controller_test_set_close_wait_ms(AprController *c, int ms)
{
    if (c) c->close_wait_override = ms;
}

HANDLE apr_controller_test_idle_event(const AprController *c)
{
    return c ? c->idle_event : NULL;
}

static DWORD close_wait_ms(const AprController *c)
{
    if (c->close_wait_override >= 0) return (DWORD)c->close_wait_override;
    return CTL_FINALIZE_WAIT_MS;
}

/* The same sentence on both channels -- but only when the second one is the
 * only one that can reach the user. See a_better_channel_exists(). */
static void say_and_notify(AprController *c, AprStrId in_window, AprStrId balloon,
                           const wchar_t *const *args, size_t nargs)
{
    wchar_t text[CTL_TEXT_CCH];

    say(c, in_window, args, nargs);
    if (a_better_channel_exists(c)) return;
    apr_str_format(balloon, text, CTL_TEXT_CCH, args, nargs);
    lstrcpynW(c->last_balloon, text, CTL_TEXT_CCH);
    c->balloons++;
    apr_tray_notify(c->tray, APR_S_UI_TRAY_INFO_TITLE, text);
}

size_t apr_controller_last_announcement(const AprController *c,
                                        wchar_t *buf, size_t cch)
{
    if (!buf || cch == 0) return 0;
    buf[0] = 0;
    if (!c) return 0;
    lstrcpynW(buf, c->last_said, (int)cch);
    return wcslen(buf);
}

size_t apr_controller_last_balloon(const AprController *c,
                                   wchar_t *buf, size_t cch)
{
    if (!buf || cch == 0) return 0;
    buf[0] = 0;
    if (!c) return 0;
    lstrcpynW(buf, c->last_balloon, (int)cch);
    return wcslen(buf);
}

unsigned apr_controller_balloon_count(const AprController *c)
{
    return c ? c->balloons : 0u;
}

/* ==========================================================================
 * Elapsed time -- pure
 * ======================================================================== */

size_t apr_controller_format_elapsed(int64_t ms, wchar_t *buf, size_t cch)
{
    const wchar_t *args[3];
    wchar_t h[24], m[24], s[24];
    int64_t total;

    if (!buf || cch == 0) return 0;
    buf[0] = 0;
    if (ms < 0) ms = 0;
    total = ms / 1000;

    /* Two digits, and the leading zero is added HERE rather than by a "%02d"
     * somewhere, because apr_str_number() is the only place in the product a
     * number becomes user-visible digits and it is what the Arabic-Indic
     * decision hangs on. Padding a rendered number is safe; formatting one
     * behind its back is not. */
#define TWO(v, dst)                                                            \
    do {                                                                       \
        wchar_t n_[24];                                                        \
        apr_str_number((v), n_, 24);                                            \
        if (wcslen(n_) < 2) {                                                   \
            wchar_t z_[24];                                                     \
            apr_str_number(0, z_, 24);                                          \
            _snwprintf_s((dst), 24, _TRUNCATE, L"%ls%ls", z_, n_);              \
        } else {                                                                \
            lstrcpynW((dst), n_, 24);                                           \
        }                                                                       \
    } while (0)

    TWO(total / 3600, h);
    TWO((total / 60) % 60, m);
    TWO(total % 60, s);
#undef TWO

    args[0] = h;
    args[1] = m;
    args[2] = s;
    return apr_str_format(APR_S_UI_TIME_ELAPSED, buf, cch, args, 3);
}

/* ==========================================================================
 * Views
 * ======================================================================== */

/* THE WINDOW TITLE IS A SENTENCE, NOT A PATH. UI_TITLE_SESSION exists so the
 * frame can read "mix.aprsession -- apprecorder" (and in another language,
 * whatever that language does with a document title); writing the bare path
 * into the frame both bypassed the catalog and made the window's accessible
 * name a file path with no indication of what application it belongs to. */
static void set_session_title(AprController *c, const wchar_t *path)
{
    wchar_t title[CTL_TEXT_CCH];
    const wchar_t *args[1];

    args[0] = path ? path : L"";
    apr_str_format(APR_S_UI_TITLE_SESSION, title, CTL_TEXT_CCH, args, 1);
    apr_ui_app_set_title_text(c->app, title);
}

static void refresh_views(AprController *c)
{
    if (!c) return;
    if (c->canvas) apr_canvas_rebuild(c->canvas);
    if (c->tree)   apr_tree_panel_refresh(c->tree);
}

static int graph_has_output(const AprGraph *g)
{
    size_t i, n;

    if (!g) return 0;
    n = apr_graph_bus_count(g);
    for (i = 0; i < n; i++) {
        AprBus *b = apr_graph_bus_at(g, i);
        if (b && apr_bus_action_count(b) > 0) return 1;
    }
    return 0;
}

/* Editing is impossible while a tick is in flight (graph.h), so the commands
 * that change the shape are greyed for the duration -- AND they say why when
 * invoked anyway, because grey is not a message this application's first user
 * receives. */
static void update_commands(AprController *c)
{
    int rec = c->recording;
    int can_edit = !rec;
    static const int editors[] = {
        APR_CMD_FILE_NEW, APR_CMD_FILE_OPEN, APR_CMD_ADD_SOURCE,
        APR_CMD_ADD_BUS, APR_CMD_ADD_ACTION, APR_CMD_CONNECT,
        APR_CMD_DISCONNECT, APR_CMD_REMOVE, APR_CMD_RENAME_BUS,
        APR_CMD_REMOVE_OUTPUT
    };
    size_t i;

    for (i = 0; i < sizeof editors / sizeof editors[0]; i++) {
        apr_ui_app_enable_command(c->app, editors[i], can_edit);
    }
    apr_ui_app_enable_command(c->app, APR_CMD_FILE_SAVE, can_edit);
    apr_ui_app_enable_command(c->app, APR_CMD_FILE_SAVE_AS, can_edit);
    apr_ui_app_enable_command(c->app, APR_CMD_RECORD_START,
                              !rec && graph_has_output(c->graph));
    apr_ui_app_enable_command(c->app, APR_CMD_RECORD_STOP, rec);
    /* GREYED, NEVER ABSENT, and greyed the right way round: "Pause Recording,
     * unavailable" is how a screen reader user learns the take is ALREADY
     * paused without having to press anything and listen to what happened. */
    apr_ui_app_enable_command(c->app, APR_CMD_RECORD_PAUSE, rec && !c->paused);
    apr_ui_app_enable_command(c->app, APR_CMD_RECORD_RESUME, rec && c->paused);
    apr_ui_app_enable_command(c->app, APR_CMD_HELP_KEYS, 1);
    apr_ui_app_enable_command(c->app, APR_CMD_HELP_ABOUT, 1);
    apr_ui_app_enable_command(c->app, APR_CMD_HELP_DOCS, 1);
    /* NEVER GREYED, not even while recording. Asking whether a newer build
     * exists opens no file and touches no graph; it is APPLYING one that rule
     * 1 forbids, and that refusal is said out loud in apply_update() rather
     * than expressed as a menu item that has quietly gone away. */
    apr_ui_app_enable_command(c->app, APR_CMD_HELP_UPDATE, 1);

    apr_tray_set_can_record(c->tray, !rec && graph_has_output(c->graph), rec);
    apr_tray_set_can_pause(c->tray, rec && !c->paused, rec && c->paused);
}

/* A CHOOSER RETURNED 0. WAS THAT A CANCEL, OR DID NOTHING OPEN?
 *
 * ui_dialogs.h: every chooser returns 0 for both, and folding them together is
 * how a two-byte template bug reached the author as pure silence for a
 * fortnight -- the key did nothing, no window appeared, and the only evidence
 * was a log line in a file nobody had open. A cancel needs no sentence,
 * because the user did it on purpose. A window that could not open does.
 *
 * Returns nonzero when it said something, i.e. when it really was a fault. */
static int dialog_faulted(AprController *c)
{
    if (!apr_dlg_last_failed()) return 0;
    say0(c, APR_S_UI_DLG_CREATE_FAILED);
    return 1;
}

/* Nonzero when it is safe to throw the current session away -- either because
 * there is nothing unsaved in it, or because the user has just said so.
 *
 * FILE > NEW AND FILE > OPEN USED TO DISCARD AN HOUR OF ROUTING WITH NO
 * PROMPT. Ctrl+N is one key away from Ctrl+B on some layouts and one slip
 * away from Ctrl+M on any of them, and the session that vanishes is the
 * artifact you rely on to reproduce a recording. */
static int may_discard(AprController *c)
{
    if (!c->dirty) return 1;
    return apr_dlg_confirm(c->frame, APR_S_UI_DLG_DISCARD_TITLE,
                           apr_str(APR_S_UI_DLG_DISCARD_BODY),
                           APR_S_UI_DLG_DISCARD_OK, APR_S_UI_DLG_CANCEL);
}

/* Nonzero when the command must be refused because a recording is running.
 * Refused OUT LOUD -- silence would be indistinguishable from a bug. */
static int busy(AprController *c)
{
    if (!c->recording) return 0;
    say0(c, APR_S_UI_ANN_BUSY_RECORDING);
    return 1;
}

/* ==========================================================================
 * Building a graph
 * ======================================================================== */

/* THE FRAME IS THE CATALOG'S AND SO IS THE REASON.
 *
 * `id` names the operation ("That source could not be added: %1!s!"); the
 * insert is why. Both halves have to come out of the catalog or the sentence
 * is half translated, which is what BUGS.md M11 was about -- this used to
 * hand apr_err_format()'s English prose, raise site and all, straight into a
 * translated frame. The diagnostic still exists; it goes to the log. */
static void report_failure(AprController *c, AprStrId id, const AprErr *e)
{
    const wchar_t *args[1];
    wchar_t why[512];

    apr_err_reason(e, why, 512);
    args[0] = why;
    say(c, id, args, 1);
}

/* THE VERB, WITHOUT THE CHOOSER. Same split as apr_controller_open_session,
 * and for the same two reasons: a modal cannot be answered from the thread
 * that opened it, so the half that does the work is the only half a test can
 * drive on the real path; and a later scripting surface wants "add this
 * source" without a picker in front of it.
 *
 * Everything a user receives lives HERE -- the model change, both views, the
 * menu states and the sentence -- so the dialog route and any other route
 * cannot drift into announcing different things. */
AprErr apr_controller_add_source(AprController *c, const wchar_t *name,
                                 const AprCaptureConfig *cfg)
{
    AprSourceId id = 0;
    const wchar_t *args[1];
    AprErr e;

    if (!c || !name || !cfg) {
        return APR_ERR(APR_E_INVALID_ARG, L"apr_controller_add_source: NULL");
    }
    if (c->recording) {
        say0(c, APR_S_UI_ANN_BUSY_RECORDING);
        return APR_ERR(APR_E_STATE, L"cannot add a source while recording");
    }

    e = apr_graph_add_source(c->graph, name, cfg, &id);
    if (apr_failed(&e)) {
        APR_LOG_ERR(APR_LOG_ERROR, &e);
        report_failure(c, APR_S_UI_DLG_ADD_FAILED, &e);
        return e;
    }
    APR_INFO(L"add source: added id=%u; graph now holds %u sources",
             (unsigned)id, (unsigned)apr_graph_source_count(c->graph));

    c->dirty = 1;
    refresh_views(c);
    update_commands(c);
    args[0] = name;
    say(c, APR_S_UI_DLG_SOURCE_ADDED, args, 1);
    return apr_ok();
}

static int do_add_source(AprController *c)
{
    AprDlgSource pick;
    AprCaptureConfig cfg;

    if (busy(c)) return 1;
    if (!apr_dlg_add_source(c->frame, &pick)) {
        APR_INFO(L"add source: dialog returned nothing (cancelled, or it could "
                 L"not be created)");
        (void)dialog_faulted(c);
        return 1;
    }
    APR_INFO(L"add source: kind=%d pid=%u name='%s'",
             (int)pick.kind, (unsigned)pick.pid, pick.name);

    memset(&cfg, 0, sizeof cfg);
    switch (pick.kind) {
    case APR_DLG_SRC_DEVICE:
        cfg.kind = APR_SRC_DEVICE;
        cfg.device.endpoint_id = pick.endpoint_id;
        break;
    case APR_DLG_SRC_SYSTEM_MINUS_TREE:
        cfg.kind = APR_SRC_PROCESS;
        cfg.process.pid = pick.pid;
        cfg.process.exclude = 1;
        break;
    default:
        cfg.kind = APR_SRC_PROCESS;
        cfg.process.pid = pick.pid;
        cfg.process.exclude = 0;
        break;
    }

    (void)apr_controller_add_source(c, pick.name, &cfg);
    return 1;
}

/* The verb without the chooser, exactly as apr_controller_add_source. */
AprErr apr_controller_add_bus(AprController *c, const wchar_t *name)
{
    AprBusId id = 0;
    const wchar_t *args[1];
    AprErr e;

    if (!c || !name) {
        return APR_ERR(APR_E_INVALID_ARG, L"apr_controller_add_bus: NULL");
    }
    if (c->recording) {
        say0(c, APR_S_UI_ANN_BUSY_RECORDING);
        return APR_ERR(APR_E_STATE, L"cannot add a bus while recording");
    }

    e = apr_graph_add_bus(c->graph, name, &id);
    if (apr_failed(&e)) {
        report_failure(c, APR_S_UI_DLG_ADD_FAILED, &e);
        return e;
    }

    c->dirty = 1;
    refresh_views(c);
    update_commands(c);
    args[0] = name;
    say(c, APR_S_UI_DLG_BUS_ADDED, args, 1);
    return apr_ok();
}

static int do_add_bus(AprController *c)
{
    wchar_t name[APR_NAME_CCH];

    if (busy(c)) return 1;
    name[0] = 0;
    if (!apr_dlg_name_prompt(c->frame, APR_S_UI_DLG_ADD_BUS_TITLE,
                             APR_S_UI_DLG_BUS_NAME, name, APR_NAME_CCH)) {
        (void)dialog_faulted(c);
        return 1;
    }

    (void)apr_controller_add_bus(c, name);
    return 1;
}

/* The bus a command should act on: whatever the tree panel has selected, or
 * the first bus. A selection carries its bus precisely so that this question
 * has one answer (ui_tree_panel.h). */
static AprBusId current_bus(AprController *c)
{
    AprTreeSel sel;

    if (c->tree && apr_tree_panel_get_selection(c->tree, &sel) && sel.bus) {
        return sel.bus;
    }
    if (apr_graph_bus_count(c->graph) > 0) {
        AprBus *b = apr_graph_bus_at(c->graph, 0);
        if (b) return apr_bus_id(b);
    }
    return 0;
}

static int do_rename_bus(AprController *c)
{
    AprBusId id;
    AprBus  *b;
    wchar_t  name[APR_NAME_CCH];
    const wchar_t *args[1];

    if (busy(c)) return 1;
    id = current_bus(c);
    b = id ? apr_graph_bus(c->graph, id) : NULL;
    if (!b) {
        say0(c, APR_S_UI_DLG_NO_BUSES);
        return 1;
    }

    lstrcpynW(name, apr_bus_name(b), APR_NAME_CCH);
    if (!apr_dlg_name_prompt(c->frame, APR_S_UI_DLG_RENAME_BUS_TITLE,
                             APR_S_UI_DLG_BUS_NAME, name, APR_NAME_CCH)) {
        (void)dialog_faulted(c);
        return 1;
    }
    if (!apr_bus_set_name(b, name)) {
        say0(c, APR_S_UI_DLG_NAME_NEEDED);
        return 1;
    }

    c->dirty = 1;
    refresh_views(c);
    args[0] = name;
    say(c, APR_S_UI_DLG_BUS_RENAMED, args, 1);
    return 1;
}

static int do_add_output(AprController *c)
{
    AprDlgOutput out;
    AprActionConfig cfg;
    const AprActionVTable *vt;
    const wchar_t *args[2];
    AprErr e;

    if (busy(c)) return 1;
    if (!apr_dlg_add_output(c->frame, c->graph, current_bus(c), &out)) {
        (void)dialog_faulted(c);
        return 1;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.out_path     = out.path;
    cfg.sample_rate  = apr_graph_rate(c->graph);
    cfg.channels     = apr_graph_channels(c->graph);
    cfg.bitrate_kbps = out.bitrate_kbps;
    cfg.quality      = out.quality;

    e = apr_graph_add_action(c->graph, out.bus, out.action_id, &cfg);
    if (apr_failed(&e)) {
        report_failure(c, APR_S_UI_DLG_ADD_FAILED, &e);
        return 1;
    }

    c->dirty = 1;
    refresh_views(c);
    update_commands(c);
    vt = apr_action_find(out.action_id);
    args[0] = vt ? apr_str(vt->display_name_id) : L"";
    args[1] = out.path;
    say(c, APR_S_UI_DLG_OUTPUT_ADDED, args, 2);
    return 1;
}

static int do_remove_output(AprController *c)
{
    AprBusId id;
    AprBus  *b;
    size_t   index = 0;
    const AprActionVTable *vt;
    const wchar_t *args[1];
    wchar_t name[APR_NAME_CCH];
    AprErr e;

    if (busy(c)) return 1;
    id = current_bus(c);
    b = id ? apr_graph_bus(c->graph, id) : NULL;
    if (!b) {
        say0(c, APR_S_UI_DLG_NO_BUSES);
        return 1;
    }
    if (apr_bus_action_count(b) == 0) {
        /* TWO STATES, TWO SENTENCES. "There is no bus to record yet" said to
         * someone standing on a bus with no outputs is a statement about their
         * session that is not true, and it sends them off to add the thing
         * they already have. */
        say0(c, APR_S_UI_DLG_NO_OUTPUTS);
        return 1;
    }
    if (!apr_dlg_pick_output(c->frame, c->graph, id, &index)) {
        (void)dialog_faulted(c);
        return 1;
    }

    vt = apr_bus_action_at(b, index);
    lstrcpynW(name, vt ? apr_str(vt->display_name_id) : L"", APR_NAME_CCH);

    /* Between recordings there is nothing open to abandon -- an action's life
     * is one recording now (bus.h) -- so this forgets a plan and touches no
     * file. If a recording WERE under way, apr_bus_remove_action finalizes
     * before it detaches, which is why it is still the only route. */
    e = apr_bus_remove_action(b, index);
    if (apr_failed(&e)) APR_LOG_ERR(APR_LOG_WARN, &e);

    c->dirty = 1;
    refresh_views(c);
    update_commands(c);
    args[0] = name;
    say(c, APR_S_UI_DLG_OUTPUT_REMOVED, args, 1);
    return 1;
}

/* ==========================================================================
 * Sessions
 * ======================================================================== */

static AprStrId load_fault_string(AprSessionFault f)
{
    switch (f) {
    case APR_SESSION_FAULT_FILE_MISSING:    return APR_S_ERR_SESSION_NOT_FOUND;
    case APR_SESSION_FAULT_FILE_UNREADABLE: return APR_S_ERR_SESSION_UNREADABLE;
    case APR_SESSION_FAULT_TOO_LARGE:       return APR_S_ERR_SESSION_UNREADABLE;
    case APR_SESSION_FAULT_NOT_JSON:        return APR_S_ERR_SESSION_NOT_JSON;
    case APR_SESSION_FAULT_TRUNCATED:       return APR_S_ERR_SESSION_TRUNCATED;
    case APR_SESSION_FAULT_NOT_A_SESSION:   return APR_S_ERR_SESSION_NOT_A_SESSION;
    case APR_SESSION_FAULT_TOO_NEW:         return APR_S_ERR_SESSION_TOO_NEW;
    case APR_SESSION_FAULT_TOO_OLD:         return APR_S_ERR_SESSION_TOO_OLD;
    case APR_SESSION_FAULT_BAD_TYPE:        return APR_S_ERR_SESSION_BAD_FIELD;
    case APR_SESSION_FAULT_BAD_VALUE:       return APR_S_ERR_SESSION_BAD_VALUE;
    case APR_SESSION_FAULT_TOO_MANY:        return APR_S_ERR_SESSION_TOO_MANY;
    case APR_SESSION_FAULT_DANGLING_REF:    return APR_S_ERR_SESSION_DANGLING_REF;
    default:                                return APR_S_ERR_SESSION_UNREADABLE;
    }
}

/* Turn a resolved session into a live graph. */
static AprErr session_to_graph(const AprSession *s, AprGraph **out)
{
    AprGraph *g = NULL;
    AprSourceId ids[APR_MAX_SOURCES];
    size_t i, j, k;
    AprErr e;

    memset(ids, 0, sizeof ids);
    e = apr_graph_create(s->sample_rate, s->channels, &g);
    if (apr_failed(&e)) return e;

    for (i = 0; i < s->source_count; i++) {
        const AprSessionSource *ss = &s->sources[i];
        AprCaptureConfig cfg;

        if (!ss->resolved) continue;
        memset(&cfg, 0, sizeof cfg);
        switch (ss->kind) {
        case APR_SESSION_SRC_DEVICE:
            cfg.kind = APR_SRC_DEVICE;
            cfg.device.endpoint_id = ss->resolved_endpoint_id[0]
                                       ? ss->resolved_endpoint_id
                                       : ss->endpoint_id;
            break;
        case APR_SESSION_SRC_FAKE:
            cfg.kind = APR_SRC_FAKE;
            cfg.fake.tone_hz        = ss->fake_hz;
            cfg.fake.rate_error_ppm = ss->fake_ppm;
            cfg.fake.amplitude      = ss->fake_amp;
            break;
        case APR_SESSION_SRC_SYSTEM_MINUS_TREE:
            cfg.kind = APR_SRC_PROCESS;
            cfg.process.pid = ss->resolved_pid;
            cfg.process.exclude = 1;
            break;
        default:
            cfg.kind = APR_SRC_PROCESS;
            cfg.process.pid = ss->resolved_pid;
            cfg.process.exclude = 0;
            break;
        }
        e = apr_graph_add_source(g, ss->name, &cfg, &ids[i]);
        if (apr_failed(&e)) { apr_graph_destroy(g); return e; }
    }

    for (j = 0; j < s->bus_count; j++) {
        const AprSessionBus *sb = &s->buses[j];
        AprBusId bus = 0;

        e = apr_graph_add_bus(g, sb->name, &bus);
        if (apr_failed(&e)) { apr_graph_destroy(g); return e; }

        for (k = 0; k < sb->edge_count; k++) {
            size_t si = sb->edges[k].source_index;
            float gain;
            if (si >= s->source_count || !ids[si]) continue;
            /* Tenths of a dB in the file so no float round-trips through the
             * text (session.h); the conversion to linear happens once, here. */
            gain = (float)pow(10.0, (double)sb->edges[k].gain_db_tenths / 200.0);
            e = apr_graph_connect(g, ids[si], bus, gain);
            if (apr_failed(&e)) { apr_graph_destroy(g); return e; }
        }

        for (k = 0; k < sb->action_count; k++) {
            AprActionConfig cfg;
            memset(&cfg, 0, sizeof cfg);
            cfg.out_path     = sb->actions[k].path;
            cfg.sample_rate  = s->sample_rate;
            cfg.channels     = s->channels;
            cfg.bitrate_kbps = sb->actions[k].bitrate_kbps;
            cfg.quality      = sb->actions[k].quality;
            e = apr_graph_add_action(g, bus, sb->actions[k].id, &cfg);
            if (apr_failed(&e)) { apr_graph_destroy(g); return e; }
        }
    }

    *out = g;
    return apr_ok();
}

AprErr apr_controller_open_session(AprController *c, const wchar_t *path,
                                   int interactive)
{
    AprSessionResolveOptions opt;
    AprGraph *g = NULL;
    AprErr e;

    if (!c || !path) {
        return APR_ERR(APR_E_INVALID_ARG, L"apr_controller_open_session: NULL");
    }
    if (c->recording) {
        return APR_ERR(APR_E_STATE, L"cannot open a session while recording");
    }

    c->declined = 0;
    memset(&g_load_rep, 0, sizeof g_load_rep);
    e = apr_session_load(path, &g_session, &g_load_rep);
    if (apr_failed(&e)) return e;

    /* A SESSION FILE IS NOT CONSENT for system-wide capture (session.h,
     * design 4.1.1). Resolve WITHOUT it first, so that an EXCLUDE source comes
     * back as APR_SESSION_NEEDS_CONSENT and has to be agreed to separately. */
    memset(&opt, 0, sizeof opt);
    opt.allow_missing = 1;
    memset(&g_resolve_rep, 0, sizeof g_resolve_rep);
    (void)apr_session_resolve(&g_session, &opt, &g_resolve_rep);

    if (interactive) {
        /* THE REPORT IS SHOWN WHENEVER IT HAS ANYTHING TO SAY -- not only on
         * failure. session.h returns what was asked for, what was substituted
         * and every rival candidate precisely so a UI can show all of it, and
         * reducing that to "failed" is the thing it exists to prevent. */
        if (g_resolve_rep.substituted || g_resolve_rep.failed ||
            g_resolve_rep.needs_system_capture_consent) {
            if (!apr_dlg_resolve_report(c->frame, &g_resolve_rep)) {
                /* A DELIBERATE CANCEL. Flagged rather than encoded in the
                 * error, because the caller has to be able to tell "you said
                 * no" from "this file is broken" -- announcing the first as
                 * the second told the author that a session he had chosen not
                 * to open had FAILED to open, in the third person, quoting an
                 * untranslatable internal literal back at him. */
                c->declined = 1;
                return APR_ERR(APR_E_STATE, L"the user declined this session");
            }
        }
        if (g_resolve_rep.needs_system_capture_consent) {
            /* Separate, explicit, and asked EVERY time. Convenience is not a
             * reason to widen a privacy scope. */
            if (!apr_dlg_confirm(c->frame, APR_S_UI_DLG_SYSTEM_TITLE,
                                 apr_str(APR_S_UI_DLG_SYSTEM_BODY),
                                 APR_S_UI_DLG_SYSTEM_CONSENT,
                                 APR_S_UI_DLG_CANCEL)) {
                c->declined = 1;
                return APR_ERR(APR_E_STATE, L"system-wide capture was declined");
            }
            opt.allow_system_capture = 1;
            memset(&g_resolve_rep, 0, sizeof g_resolve_rep);
            (void)apr_session_resolve(&g_session, &opt, &g_resolve_rep);
        }
    } else if (g_resolve_rep.needs_system_capture_consent) {
        /* There is nobody to ask, so it does not happen. Failing loudly here
         * is the safe direction: dropping an ordinary source records LESS than
         * was asked for, and enabling this one would record MORE. */
        return APR_ERR(APR_E_STATE,
                       L"this session asks for system-wide capture and nobody "
                       L"can be asked to confirm it");
    }

    e = session_to_graph(&g_session, &g);
    if (apr_failed(&e)) return e;

    e = apr_controller_set_graph(c, g);
    if (apr_failed(&e)) { apr_graph_destroy(g); return e; }

    lstrcpynW(c->session_path, path, APR_DISC_PATH_CCH);
    c->dirty = 0;
    set_session_title(c, path);

    /* ui_controller.h: non-interactive "takes what resolved and reports the
     * rest through the return value". It did not -- it returned ok, and a
     * caller with nobody to ask was told a session had loaded cleanly when
     * sources had been silently dropped from it. The graph IS adopted either
     * way, which is the documented behaviour; what changes is that the caller
     * now finds out. */
    if (!interactive && g_resolve_rep.failed) {
        return APR_ERR(APR_E_NOT_FOUND,
                       L"some of this session's sources are not on this "
                       L"machine and were left out");
    }
    return apr_ok();
}

/* THE VERB PLUS WHAT IT SAYS OUT LOUD, WITHOUT THE CHOOSER.
 *
 * Split out of do_open_session below because the process now has a second way
 * in: `apprecorder my.json` -- a session double-clicked in Explorer -- opens
 * the window on that file (frontend.h), and it must be told about a decline, a
 * malformed file or a missing source in exactly the words File > Open uses.
 * The announcement is the accessibility-critical half of this command, and a
 * second copy of it would be a second copy that drifts. */
int apr_controller_open_session_and_report(AprController *c,
                                           const wchar_t *path)
{
    const wchar_t *args[1];
    AprErr e;

    if (!c || !path) return 1;

    e = apr_controller_open_session(c, path, 1);
    if (apr_failed(&e)) {
        if (c->declined) {
            /* Not a failure. The user pressed Cancel and knows exactly what
             * happened; all they need is confirmation that nothing changed. */
            say0(c, APR_S_UI_DLG_SESSION_CANCELLED);
            return 1;
        }
        /* The load faults get the catalog's own sentence for the fault, which
         * names the file's problem; anything else reports the error text. */
        if (g_load_rep.fault != APR_SESSION_FAULT_NONE) {
            const wchar_t *a[1];
            a[0] = apr_str(load_fault_string(g_load_rep.fault));
            say(c, APR_S_UI_DLG_SESSION_FAILED, a, 1);
        } else {
            report_failure(c, APR_S_UI_DLG_SESSION_FAILED, &e);
        }
        return 1;
    }

    args[0] = path;
    say(c, APR_S_UI_DLG_SESSION_LOADED, args, 1);
    return 1;
}

static int do_open_session(AprController *c)
{
    wchar_t path[APR_DISC_PATH_CCH];

    if (busy(c)) return 1;
    if (!may_discard(c)) return 1;
    path[0] = 0;
    if (!apr_dlg_choose_session(c->frame, 0, path, APR_DISC_PATH_CCH)) return 1;

    return apr_controller_open_session_and_report(c, path);
}

/* The inverse: the live graph written down. Identity capture goes through
 * session.h's own describe functions rather than being gathered here, because
 * that module decides what a source's identity IS and a second gatherer would
 * drift from the matcher the first time either changed. */
static int session_from_graph(AprController *c, AprSession *s)
{
    size_t i, j, k;

    apr_session_init(s);
    s->sample_rate = apr_graph_rate(c->graph);
    s->channels    = apr_graph_channels(c->graph);

    for (i = 0; i < apr_graph_source_count(c->graph) && i < APR_MAX_SOURCES; i++) {
        AprSource *src = apr_graph_source_at(c->graph, i);
        AprSessionSource *ss = &s->sources[s->source_count];
        if (!src) continue;
        const AprCaptureConfig *scfg = apr_source_config(src);

        memset(ss, 0, sizeof *ss);
        _snprintf_s(ss->key, APR_SESSION_KEY_CCH, _TRUNCATE, "s%zu", i);
        lstrcpynW(ss->name, apr_source_name(src), APR_NAME_CCH);

        /* A NAME IS NOT AN IDENTITY. session.h is mostly about this: a pid does
         * not survive lunch, so what gets written down is the executable, its
         * image path and its window class -- and for a device, BOTH the
         * endpoint id and the friendly name, because only the second one can
         * appear in a sentence when the device is unplugged.
         *
         * Gathered by session.h's own describe functions rather than here.
         * That module decides what a source's identity IS, so a second
         * gatherer in the UI would be free to collect a different set from the
         * one the matcher looks at, and the two would drift the first time
         * either changed. */
        switch (apr_source_kind(src)) {
        case APR_SRC_DEVICE:
            ss->kind = APR_SESSION_SRC_DEVICE;
            if (scfg && scfg->device.endpoint_id) {
                (void)apr_session_describe_device(ss, scfg->device.endpoint_id);
            }
            break;
        case APR_SRC_FAKE:
            ss->kind = APR_SESSION_SRC_FAKE;
            if (scfg) {
                ss->fake_hz  = scfg->fake.tone_hz;
                ss->fake_ppm = scfg->fake.rate_error_ppm;
                ss->fake_amp = scfg->fake.amplitude;
            }
            break;
        default:
            /* EXCLUDE mode is written down as itself, never flattened into an
             * ordinary process source -- loading it back has to ask for
             * consent again (design 4.1.1), and it cannot ask about something
             * the file does not say. */
            ss->kind = (scfg && scfg->process.exclude)
                         ? APR_SESSION_SRC_SYSTEM_MINUS_TREE
                         : APR_SESSION_SRC_PROCESS;
            if (scfg) {
                ss->pid = scfg->process.pid;
                (void)apr_session_describe_process(ss, scfg->process.pid);
            }
            break;
        }
        s->source_count++;
    }

    for (j = 0; j < apr_graph_bus_count(c->graph) && j < APR_MAX_BUSES; j++) {
        AprBus *b = apr_graph_bus_at(c->graph, j);
        AprSessionBus *sb = &s->buses[s->bus_count];
        if (!b) continue;
        memset(sb, 0, sizeof *sb);
        lstrcpynW(sb->name, apr_bus_name(b), APR_NAME_CCH);

        for (k = 0; k < apr_bus_source_count(b) &&
                    k < APR_MAX_SOURCES_PER_BUS; k++) {
            AprSource *src = apr_bus_source_at(b, k);
            size_t si;
            if (!src) continue;
            for (si = 0; si < s->source_count; si++) {
                AprSource *cand = apr_graph_source_at(c->graph, si);
                if (cand && apr_source_id(cand) == apr_source_id(src)) break;
            }
            if (si >= s->source_count) continue;
            _snprintf_s(sb->edges[sb->edge_count].key, APR_SESSION_KEY_CCH,
                        _TRUNCATE, "s%zu", si);
            sb->edges[sb->edge_count].source_index = si;
            /* GAIN IS PER EDGE, not per source: one source may sit at -6 dB on
             * the full mix and at unity on its own file (design 3.2). Stored
             * in tenths of a dB so no float round-trips through the text. */
            {
                float lin = apr_bus_gain(b, apr_source_id(src));
                double db = (lin > 0.0f) ? 20.0 * log10((double)lin) : -120.0;
                double t = db * 10.0;
                sb->edges[sb->edge_count].gain_db_tenths =
                    (int32_t)(t < 0 ? t - 0.5 : t + 0.5);
            }
            sb->edge_count++;
        }

        for (k = 0; k < apr_bus_action_count(b) &&
                    k < APR_MAX_ACTIONS_PER_BUS; k++) {
            const AprActionVTable *vt = apr_bus_action_at(b, k);
            if (!vt || !vt->id) continue;
            strncpy_s(sb->actions[sb->action_count].id,
                      sizeof sb->actions[0].id, vt->id, _TRUNCATE);
            /* THE TEMPLATE, not the file the last take happened to land in:
             * a session reopened tomorrow must save under tomorrow's name
             * (bus.h, outpath.h). apr_bus_action_path() is the one the user
             * gave; apr_bus_action_current_path() is where audio went, and
             * writing THAT down would freeze a session to one filename. */
            lstrcpynW(sb->actions[sb->action_count].path,
                      apr_bus_action_path(b, k), APR_DISC_PATH_CCH);
            /* The rest of the spec, which the bus now keeps too. Without
             * these a session reopened at a different bitrate than it was
             * saved at, silently. */
            sb->actions[sb->action_count].bitrate_kbps =
                apr_bus_action_bitrate(b, k);
            sb->actions[sb->action_count].quality = apr_bus_action_quality(b, k);
            sb->action_count++;
        }
        s->bus_count++;
    }
    return 1;
}

AprErr apr_controller_save_session(AprController *c, const wchar_t *path)
{
    AprErr e;

    if (!c || !path || !path[0]) {
        return APR_ERR(APR_E_INVALID_ARG, L"apr_controller_save_session: NULL");
    }
    if (!session_from_graph(c, &g_session)) {
        return APR_ERR(APR_E_STATE, L"this session could not be described");
    }
    e = apr_session_save(&g_session, path);
    if (apr_failed(&e)) return e;

    lstrcpynW(c->session_path, path, APR_DISC_PATH_CCH);
    c->dirty = 0;
    set_session_title(c, path);
    return apr_ok();
}

const wchar_t *apr_controller_session_path(const AprController *c)
{
    return c ? c->session_path : L"";
}

static int do_save_session(AprController *c, int ask)
{
    wchar_t path[APR_DISC_PATH_CCH];
    const wchar_t *args[1];
    AprErr e;

    if (busy(c)) return 1;
    lstrcpynW(path, c->session_path, APR_DISC_PATH_CCH);
    if (ask || !path[0]) {
        if (!apr_dlg_choose_session(c->frame, 1, path, APR_DISC_PATH_CCH)) return 1;
    }

    e = apr_controller_save_session(c, path);
    if (apr_failed(&e)) {
        report_failure(c, APR_S_UI_DLG_SESSION_SAVE_FAILED, &e);
        return 1;
    }

    args[0] = path;
    say(c, APR_S_UI_DLG_SESSION_SAVED, args, 1);
    return 1;
}

/* ==========================================================================
 * Recording
 * ======================================================================== */

/* Runs on the RECORDING thread. Copies and posts; see the file header. */
static void ctl_observer(void *user, const AprRunNotice *n)
{
    AprController *c = (AprController *)user;
    AprRunNotice *copy;

    if (!c || !n || !c->frame) return;
    copy = (AprRunNotice *)malloc(sizeof *copy);
    if (!copy) return;
    *copy = *n;
    if (!PostMessageW(c->frame, APR_CTL_WM_NOTICE, 0, (LPARAM)copy)) {
        free(copy);
    }
}

static void tick_clock(AprController *c)
{
    wchar_t elapsed[64];
    wchar_t text[CTL_TEXT_CCH];
    const wchar_t *args[1];
    int64_t ms;

    if (!c->recording) return;
    ms = apr_runner_elapsed_ms(c->runner);
    c->last_elapsed_ms = ms;
    apr_controller_format_elapsed(ms, elapsed, 64);

    args[0] = elapsed;
    /* THE PAUSED SENTENCE KEEPS BEING WRITTEN, once a second, alongside the
     * one-off announcement. The announcement is what a screen reader speaks at
     * the transition; this is what is THERE when somebody comes back to the
     * window ten minutes later and reads the status bar deliberately -- and a
     * bar still reading "Recording 00:12:04" through a pause is exactly the
     * state this feature must not be able to hide.
     *
     * `ms` is RECORDED time either way (runner.h), so the figure is the length
     * of the file and it stops climbing while paused, which is itself an answer
     * to "is it running" for anyone watching the number. */
    apr_str_format(c->paused ? APR_S_UI_STATUS_PAUSED
                             : APR_S_UI_STATUS_RECORDING,
                   text, CTL_TEXT_CCH, args, 1);
    /* announce = 0. A clock that spoke once a second would make the
     * application unusable inside a minute; the text is here for anyone who
     * reads the status bar deliberately, and for the tray tooltip that
     * Windows+B reaches without disturbing anyone. */
    apr_ui_app_set_status_text(c->app, text, 0);
    apr_tray_set_status(c->tray, c->paused ? APR_TRAY_PAUSED
                                           : APR_TRAY_RECORDING, elapsed);
}

static int start_recording(AprController *c)
{
    AprRunnerConfig cfg;
    AprErr e;

    if (c->recording) { say0(c, APR_S_UI_ANN_ALREADY_RECORDING); return 1; }
    if (!graph_has_output(c->graph)) {
        say0(c, APR_S_UI_ANN_NOTHING_TO_RECORD);
        return 1;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.graph    = c->graph;
    cfg.observer = ctl_observer;
    cfg.user     = c;

    e = apr_runner_create(&cfg, &c->runner);
    if (apr_failed(&e)) {
        report_failure(c, APR_S_UI_ANN_RECORD_FAILED, &e);
        return 1;
    }

    e = apr_runner_run_async(c->runner);
    if (apr_failed(&e)) {
        apr_runner_destroy(c->runner);
        c->runner = NULL;
        report_failure(c, APR_S_UI_ANN_RECORD_FAILED, &e);
        return 1;
    }

    c->recording = 1;
    c->paused    = 0;
    c->last_elapsed_ms = 0;
    /* Beside the flag it mirrors, for the same reason the announcement below
     * is beside it: nothing watching one of the two may see the other
     * disagree. */
    if (c->idle_event) ResetEvent(c->idle_event);

    /* SAID IMMEDIATELY, and the adjacency is deliberate. Anything watching
     * apr_controller_recording() -- a test, a future scripting surface, the
     * tray -- learns the state from the line above; everything between that
     * line and this one is time in which the state is true and the sentence is
     * not yet available. It used to be four calls' worth, one of which rebuilt
     * every node window in two views, and a test lost that race roughly one
     * run in three. */
    say_and_notify(c, APR_S_UI_ANN_RECORD_STARTED, APR_S_UI_TRAY_INFO_STARTED,
                   NULL, 0);

    update_commands(c);
    apr_tray_set_status(c->tray, APR_TRAY_RECORDING, L"");
    SetTimer(c->frame, APR_CTL_TIMER_CLOCK, 1000, NULL);

    /* THE TREE'S "RECORDING" CLAUSES EXIST FOR THIS MOMENT AND NOTHING USED TO
     * REACH THEM. Every bus row picks UI_TREE_BUS_RECORDING over UI_TREE_BUS
     * from apr_bus_running(), and nothing rebuilt the rows when a run started
     * -- so F6 into the panel mid-session and every bus read as idle for the
     * whole recording. The sentences were written, translated and unreachable.
     *
     * LAST, not before the announcement. Rebuilding two views destroys and
     * recreates every node window, which takes long enough that an observer
     * watching apr_controller_recording() -- exactly what a test does -- can
     * see "it is recording" a measurable time before it can hear "recording
     * started". The two facts should arrive together. */
    refresh_views(c);
    return 1;
}

static int stop_recording(AprController *c)
{
    if (!c->recording) { say0(c, APR_S_UI_ANN_NOT_RECORDING); return 1; }
    apr_runner_request_stop(c->runner);
    apr_tray_set_status(c->tray, APR_TRAY_FINISHING, NULL);
    say0(c, APR_S_UI_STATUS_FINISHING);
    return 1;
}

/* ONLY A REQUEST IS MADE HERE. The runner's loop owns the graph between run()
 * and its return, so the transition happens on that thread and comes back as
 * APR_RUN_EV_PAUSED / APR_RUN_EV_RESUMED -- which is where the sentence is
 * said. Announcing here instead would say "paused" while the mixer was still
 * writing frames, and the whole value of announcing a pause is that the
 * sentence and the file agree.
 *
 * The REFUSALS are said here, because nothing downstream will ever hear about
 * them. A key that does nothing and says nothing is indistinguishable from a
 * broken application to somebody working by ear. */
static int pause_recording(AprController *c)
{
    if (!c->recording) { say0(c, APR_S_UI_ANN_NOT_RECORDING); return 1; }
    if (c->paused)     { say0(c, APR_S_UI_ANN_ALREADY_PAUSED); return 1; }
    apr_runner_request_pause(c->runner);
    return 1;
}

static int resume_recording(AprController *c)
{
    if (!c->recording) { say0(c, APR_S_UI_ANN_NOT_RECORDING); return 1; }
    if (!c->paused)    { say0(c, APR_S_UI_ANN_NOT_PAUSED); return 1; }
    apr_runner_request_resume(c->runner);
    return 1;
}

/* The run is over and every file is closed. Runs on the UI thread. */
static void recording_finished(AprController *c)
{
    wchar_t elapsed[64];
    const wchar_t *args[1];
    int incomplete;

    KillTimer(c->frame, APR_CTL_TIMER_CLOCK);
    c->last_elapsed_ms = apr_runner_elapsed_ms(c->runner);
    incomplete = apr_runner_incomplete(c->runner);

    apr_runner_destroy(c->runner);
    c->runner = NULL;
    c->recording = 0;
    c->paused = 0;

    apr_controller_format_elapsed(c->last_elapsed_ms, elapsed, 64);
    args[0] = elapsed;

    update_commands(c);
    apr_tray_set_status(c->tray, APR_TRAY_IDLE, NULL);
    refresh_views(c);

    say_and_notify(c, incomplete ? APR_S_UI_ANN_RECORD_INCOMPLETE
                                 : APR_S_UI_ANN_RECORD_STOPPED,
                   APR_S_UI_TRAY_INFO_STOPPED, args, 1);

    /* AFTER the sentence, not before it. A waiter woken by this handle goes
     * straight on to read the last announcement, and waking it one line
     * earlier would hand it the sentence from before the stop. */
    if (c->idle_event) SetEvent(c->idle_event);

    /* THE THIRD TRIGGER, AND THE ONE THAT NEEDED THE GATE. Start/stop cycling
     * while setting levels is normal, so "check after every stop" would turn
     * ten takes into ten requests; apr_update_due() refuses unless five
     * minutes have passed since the last check actually happened. */
    start_update_check(c, APR_UPDATE_WHY_RECORDING_STOPPED);

    /* A close was waiting on the files. It can proceed now, and only now. */
    if (c->closing) {
        c->allow_close = 1;
        c->closing = 0;
        PostMessageW(c->frame, WM_CLOSE, 0, 0);
    }
}

static void handle_notice(AprController *c, AprRunNotice *n)
{
    const wchar_t *args[2];
    wchar_t why[512];

    switch (n->ev) {
    case APR_RUN_EV_ARM_FAILED:
        /* HONOURS THE TRAY CONTRACT NOW. Arming happens in the first moments
         * of a run, and a run started from the notification area starts with
         * the window HIDDEN by definition -- which is precisely where a status
         * bar live region reaches nobody. This was the one event guaranteed to
         * fire on the channel that could not carry it. */
        say_and_notify(c, APR_S_UI_ANN_ARM_FAILED,
                       APR_S_UI_TRAY_INFO_ARM_FAILED, NULL, 0);
        break;

    case APR_RUN_EV_SOURCE_DIED:
        args[0] = n->name;
        say_and_notify(c, APR_S_UI_ANN_SOURCE_DIED,
                       APR_S_UI_TRAY_INFO_SOURCE_DIED, args, 1);
        refresh_views(c);
        break;

    case APR_RUN_EV_SOURCE_MUTED:
        args[0] = n->name;
        say_and_notify(c, APR_S_UI_ANN_SOURCE_MUTED,
                       APR_S_UI_TRAY_INFO_SOURCE_MUTED, args, 1);
        refresh_views(c);
        break;

    /* THE RECOVERY GOES OUT ON EXACTLY THE CHANNELS THE LOSS DID. A recording
     * started from the notification area runs with the window hidden by
     * definition, which is where a status-bar live region reaches nobody --
     * and a user who heard "Teams has exited" and never hears that it came
     * back will believe the take ended there. */
    case APR_RUN_EV_SOURCE_RECOVERED:
        args[0] = n->name;
        say_and_notify(c, APR_S_UI_ANN_SOURCE_RECOVERED,
                       APR_S_UI_TRAY_INFO_SOURCE_RECOVERED, args, 1);
        refresh_views(c);
        break;

    case APR_RUN_EV_EXCLUSION_HELD:
        args[0] = n->name;
        say_and_notify(c, APR_S_UI_ANN_EXCLUSION_HELD,
                       APR_S_UI_TRAY_INFO_EXCLUSION_HELD, args, 1);
        refresh_views(c);
        break;

    case APR_RUN_EV_ACTION_FAILED:
        /* The reason travelled here from a writer thread as (kind, code) and
         * becomes words HERE, on the UI thread, where apr_str() is allowed --
         * see errmsg.h. Rendering it at the raise site would have needed a
         * lock and an allocation on a thread that must have neither. */
        apr_err_reason(&n->err, why, 512);
        args[0] = n->name;
        args[1] = why;
        /* THE BALLOON IS CONDITIONAL LIKE EVERY OTHER ONE. It used to be
         * raised unconditionally, so in the foreground a screen reader read
         * the failure twice -- once from the live region and once from the
         * shell. A balloon is for something that would otherwise be MISSED
         * (see a_better_channel_exists), and this event is not special. */
        say_and_notify(c, APR_S_UI_ANN_ACTION_FAILED,
                       APR_S_UI_TRAY_INFO_ACTION_FAILED, args, 2);
        /* And the tree's "stopped saving" clause becomes true at this instant;
         * without a rebuild the row goes on claiming the file is being
         * written, for the rest of the session. */
        refresh_views(c);
        break;

    case APR_RUN_EV_OUTPUT_RENAMED:
        /* The name that was asked for was already a recording. It has been
         * kept and this take has moved aside (outpath.h) -- which is only
         * honest if the user is TOLD, and the honest-collision policy is worth
         * nothing if the telling lands on a status bar in a hidden window.
         * This is the one fact the policy insists on, so it goes out on
         * whichever channel can actually reach the user. */
        args[0] = n->path;
        say_and_notify(c, APR_S_UI_ANN_OUTPUT_RENAMED,
                       APR_S_UI_TRAY_INFO_OUTPUT_RENAMED, args, 1);
        break;

    /* BOTH TRANSITIONS, ON WHICHEVER CHANNEL CAN REACH THE USER. A recording
     * paused from the notification area runs with the window hidden by
     * definition, which is where a status-bar live region reaches nobody -- and
     * a pause nobody hears is an hour of a meeting that was never recorded,
     * looking exactly like a recording that is going fine. The resume is the
     * half that is easy to forget, and it is the one that says the take is
     * running again. */
    case APR_RUN_EV_PAUSED:
        /* THE FLAG AND THE SENTENCE ARE ADJACENT, exactly as they are in
         * start_recording and for the same reason: anything watching
         * apr_controller_paused() -- a test, the tray, a future scripting
         * surface -- learns the state from the line above, and every call
         * between that line and the announcement is time in which the state is
         * true and the sentence is not yet available. Rebuilding two views sits
         * on the far side of it, because that destroys and recreates every node
         * window and takes long enough to lose the race. */
        c->paused = 1;
        say_and_notify(c, APR_S_UI_ANN_RECORD_PAUSED,
                       APR_S_UI_TRAY_INFO_PAUSED, NULL, 0);
        update_commands(c);
        tick_clock(c);      /* the readout follows the sentence immediately */
        refresh_views(c);
        break;

    case APR_RUN_EV_RESUMED:
        c->paused = 0;
        say_and_notify(c, APR_S_UI_ANN_RECORD_RESUMED,
                       APR_S_UI_TRAY_INFO_RESUMED, NULL, 0);
        update_commands(c);
        tick_clock(c);
        refresh_views(c);
        break;

    case APR_RUN_EV_FINISHING:
        apr_tray_set_status(c->tray, APR_TRAY_FINISHING, NULL);
        break;

    case APR_RUN_EV_STOPPED:
        recording_finished(c);
        break;

    default:
        break;
    }
}

/* ==========================================================================
 * Closing
 * ======================================================================== */

/* Pump messages while waiting, so the window keeps answering UI Automation --
 * a frozen window is a window a screen reader cannot read, and this wait can
 * legitimately last seconds while an encoder flushes. Bounded, because a
 * genuinely hung disk must not leave a window that cannot be closed at all. */
static int wait_for_files(AprController *c, DWORD limit_ms)
{
    DWORD start = GetTickCount();

    /* POLLS RATHER THAN WAITING ON THE RUNNER'S EVENT, and that is not
     * laziness. Pumping messages here is what keeps the window answering UI
     * Automation while an encoder flushes -- but pumping also lets the posted
     * STOPPED notice run, and handling that notice DESTROYS the runner and
     * closes the very handle a MsgWaitForMultipleObjects would still be
     * holding. Waiting on a closed handle does not fail loudly; it returns
     * WAIT_FAILED for ever and the window hangs until the timeout. Watching
     * c->recording instead has no handle to outlive.
     *
     * Bounded, because a genuinely hung disk must not leave a window that
     * cannot be closed at all. */
    for (;;) {
        MSG msg;

        if (!c->recording || !c->runner) return 1;

        /* THE DEADLINE IS CHECKED BEFORE THE WAIT, NOT AFTER IT.
         *
         * It used to be the last line of the loop, which meant every call
         * spent one unconditional MsgWaitForMultipleObjects -- up to 50 ms of
         * pumping -- no matter what limit_ms said. A budget of zero therefore
         * did not mean "do not wait"; it meant "wait 50 ms", and 50 ms is
         * ample for the posted STOPPED notice to be dispatched right here, run
         * recording_finished(), and clear c->recording. The caller then took
         * the success path with the timeout path never reached.
         *
         * That is a bound that is not a bound, and it is why
         * a_close_that_runs_out_of_patience_says_that_rather_than_repeating_-
         * itself failed about one run in eight: it sets the budget to 0,
         * expects the give-up sentence, and got "Recording stopped" whenever
         * the encoders happened to finish inside that 50 ms window. The
         * machine deciding which sentence the user hears is not a test defect
         * to be widened away.
         *
         * The recording having ALREADY finished is checked above and returns
         * success, so an expired budget here always means what it says: the
         * files were still being written when the time ran out. */
        if (GetTickCount() - start >= limit_ms) return 0;

        (void)MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            /* WM_QUIT IS NOT OURS TO SWALLOW, AND SWALLOWING IT LEFT A ZOMBIE.
             *
             * This drain runs the normal case of "stop recording and close":
             * recording_finished() posts WM_CLOSE, this same drain dispatches
             * it, DestroyWindow runs, WM_DESTROY calls PostQuitMessage -- and
             * then PeekMessage here retrieves the WM_QUIT that was meant for
             * apr_ui_app_run and drops it on the floor. The window is gone,
             * the process is not, apr_controller_destroy never runs, the tray
             * icon is a ghost and a second launch coexists with the first.
             *
             * Put it back and stop pumping. The files are closed by
             * construction at this point -- WM_CLOSE only got posted from
             * recording_finished. */
            if (msg.message == WM_QUIT) {
                PostQuitMessage((int)msg.wParam);
                return 1;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!c->recording) return 1;
    }
}

static int on_close(AprUiApp *app, AprUiCloseReason why, void *user)
{
    AprController *c = (AprController *)user;
    int choice;

    (void)app;
    if (!c) return 1;

    if (why == APR_UI_CLOSE_SESSION_END) {
        /* NOT a question. Windows kills the process shortly after this
         * returns, so the files have to be closed HERE. No dialog, no pumping
         * -- just stop and block, exactly as the CLI's console control handler
         * does for CTRL_CLOSE_EVENT. */
        if (c->recording && c->runner) {
            apr_runner_request_stop(c->runner);
            (void)apr_runner_wait(c->runner, CTL_FINALIZE_WAIT_MS);
        }
        return 1;
    }

    if (c->allow_close) return 1;
    if (!c->recording) return 1;

    choice = apr_dlg_close_while_recording(c->frame);
    if (choice == APR_DLG_CLOSE_CANCEL) return 0;

    if (choice == APR_DLG_CLOSE_TO_TRAY) {
        /* Same rule as Ctrl+Shift+H, and it matters more here because a
         * recording IS in flight: with no icon the window stays up rather than
         * becoming unreachable. The recording keeps running either way, which
         * is what the user asked for. */
        if (!apr_tray_is_registered(c->tray)) {
            say0(c, APR_S_UI_ANN_NO_TRAY);
            return 0;
        }
        /* The recording keeps running and the window goes away. The balloon is
         * not decoration: without it the application has silently vanished,
         * and the notification area is not somewhere a person thinks to look
         * unless they are told. */
        ShowWindow(c->frame, SW_HIDE);
        apr_tray_notify(c->tray, APR_S_UI_TRAY_INFO_TITLE,
                        apr_str(APR_S_UI_TRAY_INFO_MINIMIZED));
        return 0;
    }

    /* Stop and close. The window stays up, saying so, until the files are
     * written -- a recorder that exits while an encoder is still flushing has
     * lost the recording, which is the failure this whole path exists for. */
    say0(c, APR_S_UI_ANN_CLOSING_FILES);
    apr_runner_request_stop(c->runner);
    c->closing = 1;

    if (wait_for_files(c, close_wait_ms(c))) {
        c->allow_close = 1;
        /* recording_finished() has usually already run from the posted STOPPED
         * notice; if the wait won the race, let the posted close happen. */
        return c->recording ? 0 : 1;
    }
    /* The wait expired. Say SO -- not the sentence that means "this is still
     * in progress", which is what used to be replayed here: hearing "the
     * window will close once the files are written" a second time and then
     * having nothing close is indistinguishable from a hung application. */
    say0(c, APR_S_UI_ANN_CLOSE_TIMEOUT);
    c->closing = 0;
    return 0;
}

/* ==========================================================================
 * Staying up to date
 *
 * WHAT THIS HALF OWNS, AND WHAT IT DOES NOT.
 *
 *   It owns rule 4 of update.h -- announced, and keyboard-reachable -- and
 *   nothing else. Whether a check is due, whether a signature is genuine,
 *   whether a payload matches the hash that was signed, and how the file moves
 *   are all update.c's, and none of it is re-decided here.
 *
 *   WHY EVERY OUTCOME GOES THROUGH say_and_notify. This application spends its
 *   recordings minimised, so the moment an update is found is exactly the
 *   moment a status-bar live region reaches nobody. Same argument as the
 *   source-died and output-renamed events, and the same one call, so a future
 *   sixth outcome cannot quietly pick the wrong channel.
 *
 *   AND WHY THE REFUSAL IS ITS OWN SENTENCE. "The update failed" would be true
 *   of a flat wifi and of a forged release, and only one of those is worth
 *   interrupting somebody for.
 * ======================================================================== */

/* The image the swap will move aside. A test may point this somewhere safe --
 * a suite must never be one bug away from renaming its own executable. */
static size_t ctl_update_image(const AprController *c, wchar_t *buf, size_t cch)
{
    if (c->update_image[0]) {
        lstrcpynW(buf, c->update_image, (int)cch);
        return wcslen(buf);
    }
    return apr_update_image_path(buf, cch);
}

/* Runs on the CHECK'S OWN THREAD. Copies and posts; it must not touch a
 * window, exactly like the runner's observer. */
static void ctl_update_done(void *user, const AprUpdateResult *r)
{
    /* THE WINDOW HANDLE, NOT THE CONTROLLER, AND THAT IS THE POINT.
     *
     * The check thread is detached and can outlive this controller: the
     * startup check is fired from apr_controller_create and a window closed a
     * second later would free the controller while a socket was still timing
     * out. Holding an AprController * here would then be a use-after-free on
     * the very next line. An HWND is a VALUE -- PostMessageW to a window that
     * has gone simply fails, and the copy is freed. The worst case is a stale
     * handle that Windows has recycled inside this process, and the worst that
     * does is deliver a WM_APP message some control ignores, which leaks one
     * allocation rather than corrupting memory. */
    HWND             frame = (HWND)user;
    AprUpdateResult *copy;

    if (!frame || !r || !IsWindow(frame)) return;
    /* EVERY result is posted, including the dull ones. Deciding what is worth
     * saying is handle_update's job and it lives on the UI thread, which is
     * what lets tests/test_ui_update.c drive every outcome through the real
     * code rather than through a filter that only exists on a worker. */
    copy = (AprUpdateResult *)malloc(sizeof *copy);
    if (!copy) return;
    *copy = *r;
    if (!PostMessageW(frame, APR_CTL_WM_UPDATE, 0, (LPARAM)copy)) free(copy);
}

static void start_update_check(AprController *c, AprUpdateWhy why)
{
    AprErr e;

    if (!c || !c->frame) return;
    e = apr_update_check_async(c->update_http, (int64_t)time(NULL), why,
                               ctl_update_done, (void *)c->frame);
    /* A check that could not even be started is exactly as uninteresting as
     * one that could not reach the network (update.h section 7). */
    if (apr_failed(&e)) APR_LOG_ERR(APR_LOG_INFO, &e);
}

/* Download it, prove it, and leave it beside the executable. Runs on the
 * window's thread; the download is bounded by update_http.c's timeouts. */
static void apply_update(AprController *c, const AprUpdateManifest *m)
{
    wchar_t        image[APR_DISC_PATH_CCH];
    wchar_t        why[512];
    const wchar_t *args[2];
    AprErr         e;

    /* RULE 1, AND IT IS SAID OUT LOUD. The recording is not stopped, not
     * paused, and not interrupted -- the take wins, and the person is told
     * why nothing happened rather than being left with a menu item that did
     * nothing. */
    if (!apr_update_may_apply(c->recording)) {
        say0(c, APR_S_UPDATE_BUSY_RECORDING);
        return;
    }

    if (!ctl_update_image(c, image, APR_DISC_PATH_CCH)) {
        say0(c, APR_S_UPDATE_FAILED);
        return;
    }

    args[0] = m->version;
    say(c, APR_S_UPDATE_DOWNLOADING, args, 1);

    e = apr_update_stage(c->update_http, image, m);
    if (apr_failed(&e)) {
        APR_LOG_ERR(APR_LOG_ERROR, &e);
        apr_err_reason(&e, why, 512);
        args[0] = why;
        /* A payload that does not match the SIGNED hash is the security
         * sentence, not the network one. They are told apart by the reason
         * the raise site named, so the two can never be merged by accident. */
        if (apr_err_reason_id(&e) == APR_S_ERR_UPDATE_PAYLOAD) {
            say_and_notify(c, APR_S_UPDATE_REFUSED, APR_S_UPDATE_TRAY_REFUSED,
                           args, 1);
        } else {
            say(c, APR_S_UPDATE_FAILED, args, 1);
        }
        return;
    }

    /* Staged and proven. THE SWAP HAPPENS ON EXIT, not now: renaming the
     * running image is legal but a program that did it mid-session would be
     * describing itself with a version it is no longer running. */
    c->update_staged = 1;
    lstrcpynW(c->update_version, m->version, APR_UPDATE_VERSION_CCH);
    args[0] = m->version;
    say_and_notify(c, APR_S_UPDATE_READY, APR_S_UPDATE_TRAY_READY, args, 1);
}

/* Whether to ask, and what the answer was. -1 means ask a real dialog. */
static int update_accepted(AprController *c, const AprUpdateManifest *m)
{
    wchar_t        body[CTL_TEXT_CCH];
    const wchar_t *args[3];

    if (c->update_answer >= 0) return c->update_answer;

    args[0] = m->version;
    args[1] = APR_VERSION_STRING;
    args[2] = m->notes;
    apr_str_format(APR_S_UPDATE_PROMPT_BODY, body, CTL_TEXT_CCH, args, 3);

    /* ASKED, NEVER ASSUMED. A binary that swaps itself unasked is worse than
     * a prompt -- and this prompt is a standard dialog with standard buttons,
     * which is what makes it announced and answerable from the keyboard with
     * nothing needed from us (ui_dialogs.h). */
    return apr_dlg_confirm(c->frame, APR_S_UPDATE_PROMPT_TITLE, body,
                           APR_S_UPDATE_PROMPT_INSTALL,
                           APR_S_UPDATE_PROMPT_LATER);
}

static void handle_update(AprController *c, const AprUpdateResult *r)
{
    wchar_t        why[512];
    const wchar_t *args[2];

    /* A CHECK NOBODY ASKED FOR ONLY SPEAKS WHEN IT HAS NEWS.
     *
     * "You are up to date", said out loud every five minutes for as long as
     * the application is open, would make it unusable inside an hour -- the
     * same argument the one-second clock's announce=0 rests on. A check the
     * USER asked for always answers, because a command that appears to do
     * nothing is indistinguishable from a broken one to somebody working by
     * ear. So the filter is on WHO ASKED, not on the outcome. */
    if (r->why != APR_UPDATE_WHY_USER &&
        r->outcome != APR_UPDATE_AVAILABLE && r->outcome != APR_UPDATE_REFUSED) {
        return;
    }

    switch (r->outcome) {
    case APR_UPDATE_AVAILABLE:
        args[0] = r->manifest.version;
        args[1] = APR_VERSION_STRING;
        say_and_notify(c, APR_S_UPDATE_AVAILABLE, APR_S_UPDATE_TRAY_AVAILABLE,
                       args, 2);
        if (update_accepted(c, &r->manifest)) apply_update(c, &r->manifest);
        break;

    case APR_UPDATE_REFUSED:
        /* LOUD. Somebody published something this build will not run, or
         * something on the way here changed it. Either way it is the one
         * update outcome worth interrupting a person for. */
        apr_err_reason(&r->err, why, 512);
        args[0] = why;
        say_and_notify(c, APR_S_UPDATE_REFUSED, APR_S_UPDATE_TRAY_REFUSED,
                       args, 1);
        break;

    case APR_UPDATE_DISABLED:
        say0(c, APR_S_UPDATE_OFF);
        break;

    case APR_UPDATE_UP_TO_DATE:
        args[0] = APR_VERSION_STRING;
        say(c, APR_S_UPDATE_UP_TO_DATE, args, 1);
        break;

    case APR_UPDATE_NONE:
    default:
        /* Offline, blocked, a release still being uploaded, or no release key
         * compiled in. A check that could not run is not news -- but if a
         * person pressed the menu item, saying nothing is worse than saying
         * the little that is known, so the installed version is reported. */
        args[0] = APR_VERSION_STRING;
        say(c, APR_S_UPDATE_LINE_CURRENT, args, 1);
        break;
    }
}

/* Help -> Documentation. Hands APR_PROJECT_URL to whatever the user has set as
 * their browser.
 *
 * IT REPORTS ITS OWN FAILURE, and that is the whole reason this is not a
 * one-line ShellExecuteW at the call site. A browser that does not open leaves
 * NOTHING on screen and nothing spoken: no window appears, no error is raised,
 * and a sighted user at least sees that nothing happened. Somebody working by
 * screen reader gets silence that is indistinguishable from a menu item that
 * did its job. So the failure path names the address out loud, which is also
 * the only form of the answer that is any use without a browser. */
static int open_documentation(AprController *c)
{
    /* ShellExecuteW's success test is "> 32". The return is an HINSTANCE for
     * compatibility with 16-bit Windows and is not a handle. */
    HINSTANCE r = ShellExecuteW(c->frame, L"open", APR_PROJECT_URL,
                                NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32) return 1;

    {
        const wchar_t *args[1];
        args[0] = APR_PROJECT_URL;
        APR_WARN(L"ui: no browser opened for %ls (ShellExecuteW -> %p)",
                 APR_PROJECT_URL, (void *)r);
        say(c, APR_S_UI_DLG_DOCS_FAILED, args, 1);
    }
    return 1;
}

/* The user asked. Bypasses the five-minute interval (but not the opt-out) and
 * says so first, because a menu item that appears to do nothing for two
 * seconds is a menu item somebody presses again. */
static int do_check_update(AprController *c)
{
    say0(c, APR_S_UPDATE_CHECKING);
    start_update_check(c, APR_UPDATE_WHY_USER);
    return 1;
}

int apr_controller_update_staged(const AprController *c)
{
    return c ? c->update_staged : 0;
}

void apr_controller_test_set_update_http(AprController *c,
                                         const AprUpdateHttp *http)
{
    if (c) c->update_http = http;
}

void apr_controller_test_set_update_answer(AprController *c, int answer)
{
    if (c) c->update_answer = answer;
}

void apr_controller_test_set_update_image(AprController *c, const wchar_t *path)
{
    if (!c) return;
    if (path && path[0]) lstrcpynW(c->update_image, path, APR_DISC_PATH_CCH);
    else                 c->update_image[0] = L'\0';
}

void apr_controller_test_deliver_update(AprController *c,
                                        const AprUpdateResult *r)
{
    AprUpdateResult *copy;

    if (!c || !r) return;
    if (!c->frame || !IsWindow(c->frame)) { handle_update(c, r); return; }

    /* THROUGH THE REAL MESSAGE, on the window's own thread. A test calling
     * handle_update() directly from its own thread would be touching the
     * status bar and the tray from the wrong one, which is the class of defect
     * this codebase already fixed once in apr_controller_set_graph. Sent
     * rather than posted so the announcement has happened by the time this
     * returns -- no poll, no ceiling. The handler frees the copy, exactly as
     * it does for one the check thread posted. */
    copy = (AprUpdateResult *)malloc(sizeof *copy);
    if (!copy) return;
    *copy = *r;
    SendMessageW(c->frame, APR_CTL_WM_UPDATE, 0, (LPARAM)copy);
}

/* ==========================================================================
 * Commands
 * ======================================================================== */

int apr_controller_command(AprController *c, int command_id)
{
    if (!c) return 0;

    switch (command_id) {
    case APR_CMD_FILE_NEW: {
        AprGraph *g = NULL;
        AprErr e;
        if (busy(c)) return 1;
        if (!may_discard(c)) return 1;
        e = apr_graph_create(48000, 2, &g);
        if (apr_failed(&e)) { report_failure(c, APR_S_UI_DLG_ADD_FAILED, &e); return 1; }
        (void)apr_controller_set_graph(c, g);
        c->session_path[0] = 0;
        c->dirty = 0;
        apr_ui_app_set_title_text(c->app, apr_str(APR_S_UI_TITLE_UNTITLED));
        say0(c, APR_S_UI_STATUS_READY);
        return 1;
    }
    case APR_CMD_FILE_OPEN:    return do_open_session(c);
    case APR_CMD_FILE_SAVE:    return do_save_session(c, 0);
    case APR_CMD_FILE_SAVE_AS: return do_save_session(c, 1);

    case APR_CMD_ADD_SOURCE:    return do_add_source(c);
    case APR_CMD_ADD_BUS:       return do_add_bus(c);
    case APR_CMD_ADD_ACTION:    return do_add_output(c);
    case APR_CMD_RENAME_BUS:    return do_rename_bus(c);
    case APR_CMD_REMOVE_OUTPUT: return do_remove_output(c);

    case APR_CMD_CONNECT:
    case APR_CMD_DISCONNECT:
    case APR_CMD_REMOVE:
        if (busy(c)) return 1;
        if (c->canvas && apr_canvas_command(c->canvas, command_id)) {
            c->dirty = 1;
            refresh_views(c);
            update_commands(c);
            return 1;
        }
        return 1;

    case APR_CMD_RECORD_START:  return start_recording(c);
    case APR_CMD_RECORD_PAUSE:  return pause_recording(c);
    case APR_CMD_RECORD_RESUME: return resume_recording(c);
    case APR_CMD_RECORD_STOP:   return stop_recording(c);

    case APR_CMD_SHOW_WINDOW:
        ShowWindow(c->frame, SW_SHOW);
        ShowWindow(c->frame, SW_RESTORE);
        SetForegroundWindow(c->frame);
        return 1;

    case APR_CMD_HIDE_TO_TRAY:
        /* NEVER HIDE INTO NOTHING. If the shell refused the icon -- Explorer
         * crashed before its TaskbarCreated broadcast, a policy, a full
         * notification area -- then hiding leaves the application with no
         * surface whatsoever: not Alt+Tab, not Windows+B, nothing, while it is
         * still recording. Refuse, and say why. */
        if (!apr_tray_is_registered(c->tray)) {
            say0(c, APR_S_UI_ANN_NO_TRAY);
            return 1;
        }
        ShowWindow(c->frame, SW_HIDE);
        apr_tray_notify(c->tray, APR_S_UI_TRAY_INFO_TITLE,
                        apr_str(APR_S_UI_TRAY_INFO_MINIMIZED));
        return 1;

    case APR_CMD_HELP_KEYS:
        apr_dlg_keyboard_help(c->frame);
        return 1;
    case APR_CMD_HELP_ABOUT:
        apr_dlg_about(c->frame);
        return 1;
    case APR_CMD_HELP_DOCS:
        return open_documentation(c);
    case APR_CMD_HELP_UPDATE:
        return do_check_update(c);

    default:
        break;
    }
    return 0;
}

static int on_command(AprUiApp *app, int command_id, void *user)
{
    AprController *c = (AprController *)user;
    int handled;

    (void)app;
    handled = apr_controller_command(c, command_id);

    /* Whatever just happened, what is possible NEXT may have changed -- a
     * command that added the session's first output has to un-grey Start
     * Recording, and one that removed the last one has to grey it again. Doing
     * it here rather than in ten places is what stops a menu lying. */
    if (handled) update_commands(c);
    return handled;
}

/* Adopt a graph. ALWAYS runs on the window's own thread -- either because the
 * caller was already on it or because apr_controller_set_graph marshalled. */
static int ctl_adopt_graph(AprController *c, AprGraph *g)
{
    AprGraph *old;

    if (!c) return 0;
    old = c->graph;
    c->graph = g;

    if (c->canvas) apr_canvas_set_graph(c->canvas, g);
    if (c->tree)   apr_tree_panel_set_graph(c->tree, g);
    if (old && old != g) apr_graph_destroy(old);

    update_commands(c);
    return 1;
}

/* ==========================================================================
 * Messages the frame does not own
 * ======================================================================== */

static LRESULT on_message(AprUiApp *app, UINT msg, WPARAM wp, LPARAM lp,
                          int *handled, void *user)
{
    AprController *c = (AprController *)user;

    (void)app;
    if (!c) return 0;

    if (msg == APR_CTL_WM_SET_GRAPH) {
        *handled = 1;
        return (LRESULT)(INT_PTR)ctl_adopt_graph(c, (AprGraph *)lp);
    }
    if (msg == APR_CTL_WM_REFRESH) {
        refresh_views(c);
        update_commands(c);
        *handled = 1;
        return 0;
    }
    if (msg == APR_CTL_WM_NOTICE) {
        AprRunNotice *n = (AprRunNotice *)lp;
        if (n) { handle_notice(c, n); free(n); }
        *handled = 1;
        return 0;
    }
    if (msg == APR_CTL_WM_UPDATE) {
        AprUpdateResult *r = (AprUpdateResult *)lp;
        if (r) { handle_update(c, r); free(r); }
        *handled = 1;
        return 0;
    }
    if (msg == APR_TRAY_WM_ICON) {
        *handled = apr_tray_on_message(c->tray, wp, lp);
        return 0;
    }
    if (msg == WM_TIMER && wp == APR_CTL_TIMER_CLOCK) {
        tick_clock(c);
        *handled = 1;
        return 0;
    }
    if (msg == WM_TIMER && wp == APR_CTL_TIMER_UPDATE) {
        /* The timer does not decide anything -- apr_update_due() is asked
         * every time, against the timestamp on disk. See update.h section 4. */
        start_update_check(c, APR_UPDATE_WHY_TIMER);
        *handled = 1;
        return 0;
    }
    if (apr_tray_on_taskbar_created(c->tray, msg)) {
        *handled = 1;
        return 0;
    }
    return 0;
}

/* ==========================================================================
 * Views, wired
 * ======================================================================== */

static void on_canvas_say(void *user, const wchar_t *text)
{
    AprController *c = (AprController *)user;
    if (!c || !text) return;
    lstrcpynW(c->last_said, text, CTL_TEXT_CCH);
    apr_ui_app_set_status_text(c->app, text, 1);

    /* The canvas announces on every edit, so this is also the moment to
     * re-check what is possible next. It matters because the canvas owns keys
     * the frame's accelerator table does not claim -- Ctrl+Shift+E disconnects,
     * +/- change a level -- so an edit can happen without any command reaching
     * the controller, and "Start Recording" must not stay greyed after the
     * edit that gave the session its first output. Cheap: it sets menu states
     * and touches no model. */
    update_commands(c);
}

/* Which canvas node a tree selection names, or -1. A selection names a MODEL
 * object by id (ui_tree_panel.h), so neither view holds a handle from the
 * other; this translates one to the other and nothing more. */
static int canvas_node_for(AprController *c, const AprTreeSel *sel)
{
    size_t i, n;
    AprNodeKind want;
    uint32_t id;
    int sub = 0;

    if (!c || !c->canvas || !sel) return -1;

    switch (sel->kind) {
    case APR_TREE_ROW_BUS:    want = APR_NODE_BUS;    id = sel->bus; break;
    case APR_TREE_ROW_SOURCE: want = APR_NODE_SOURCE; id = sel->source; break;
    case APR_TREE_ROW_ACTION: want = APR_NODE_ACTION; id = sel->bus;
                              sub = (int)sel->action; break;
    default:
        /* A structural row names no model object. Ignore it rather than guess
         * -- ui_tree_panel.h asks for exactly that. */
        return -1;
    }

    n = apr_canvas_node_count(c->canvas);
    for (i = 0; i < n; i++) {
        HWND w = apr_canvas_node_at(c->canvas, i);
        if (apr_node_kind(w) == want && apr_node_model_id(w) == id &&
            apr_node_sub_id(w) == sub) {
            return (int)i;
        }
    }
    return -1;
}

/* THE CARET MOVED. KEEP THE OTHER VIEW IN STEP AND DO NOT TOUCH FOCUS.
 *
 * This used to answer every caret move with an unconditional SetFocus on the
 * canvas node, which made the Structure panel impossible to browse: F6 into
 * the tree, press Down, and focus was yanked to the canvas -- the reader
 * announced the canvas node instead of the row, and the next Down drove the
 * canvas. Everything past the first row was unreachable, and the tree's
 * sentences are the whole reason the panel exists.
 *
 * Setting the canvas's CURRENT node rather than its focus keeps the two views
 * agreeing, keeps the node scrolled into view, and means that when focus does
 * arrive at the canvas -- by F6, by Tab -- it lands on the node the user was
 * standing on in the tree. Going there NOW is what apr_tree_panel activation
 * is for, below. */
static void on_tree_select(HWND panel, const AprTreeSel *sel, void *user)
{
    AprController *c = (AprController *)user;
    int i = canvas_node_for(c, sel);

    (void)panel;
    if (i >= 0) apr_canvas_set_current_node(c->canvas, (size_t)i);
}

/* THE USER CHOSE THIS ROW -- Enter, or a double click. Now focus moves. */
static void on_tree_activate(HWND panel, const AprTreeSel *sel, void *user)
{
    AprController *c = (AprController *)user;
    int i = canvas_node_for(c, sel);

    (void)panel;
    if (i >= 0) apr_canvas_focus_node(c->canvas, (size_t)i);
}

/* THE CANVAS EDITED THE MODEL WITHOUT A COMMAND PASSING THROUGH HERE.
 *
 * Ctrl+Shift+E, plus and minus and a mouse click completing an edge all change
 * the graph inside the canvas's own window procedure -- no accelerator, no
 * WM_COMMAND, nothing that reaches apr_controller_command. The tree panel is a
 * projection of the same model and had no way to hear, so it went on saying
 * "Mic -- feeds Voice Mix" about an edge that had been cut.
 *
 * Only the TREE is rebuilt here, deliberately: ui_canvas.h forbids rebuilding
 * the canvas from this callback, and the canvas has already re-synced itself
 * anyway. */
static void on_canvas_edit(void *user)
{
    AprController *c = (AprController *)user;

    if (!c) return;
    c->dirty = 1;
    if (c->tree) apr_tree_panel_refresh(c->tree);
    update_commands(c);
}

/* ==========================================================================
 * Lifetime
 * ======================================================================== */

AprErr apr_controller_create(AprUiApp *app, AprController **out)
{
    AprController *c;
    AprErr e;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"apr_controller_create: out is NULL");
    *out = NULL;
    if (!app) return APR_ERR(APR_E_INVALID_ARG, L"apr_controller_create: no app");

    c = (AprController *)calloc(1, sizeof *c);
    if (!c) return APR_ERR(APR_E_NO_MEMORY, L"apr_controller_create: out of memory");

    c->app    = app;
    c->frame  = apr_ui_app_hwnd(app);
    c->canvas = apr_ui_app_pane(app, APR_PANE_CANVAS);
    c->tree   = apr_ui_app_pane(app, APR_PANE_TREE);

    e = apr_graph_create(48000, 2, &c->graph);
    if (apr_failed(&e)) { free(c); return e; }

    /* Not fatal if the shell refuses the icon: the window still works. */
    (void)apr_tray_create(c->frame, &c->tray);

    c->fg_override = -1;   /* calloc gives 0, which would MEAN something */
    c->close_wait_override = -1;
    c->update_answer = -1; /* likewise: 0 would mean "always decline" */

    /* Manual reset, created SIGNALLED: nothing is recording yet. Not fatal if
     * the handle cannot be made -- it is a test seam, and a controller that
     * refused to exist because of one would be the worse failure. */
    c->idle_event = CreateEventW(NULL, TRUE, TRUE, NULL);
    apr_ui_app_set_command_handler(app, on_command, c);
    apr_ui_app_set_close_handler(app, on_close, c);
    apr_ui_app_set_message_handler(app, on_message, c);

    if (c->canvas) {
        apr_canvas_set_announce(c->canvas, on_canvas_say, c);
        apr_canvas_set_graph(c->canvas, c->graph);
    }
    if (c->canvas) apr_canvas_set_edit_sink(c->canvas, on_canvas_edit, c);
    if (c->tree) {
        apr_tree_panel_set_graph(c->tree, c->graph);
        apr_tree_panel_set_selection_sink(c->tree, on_tree_select, c);
        apr_tree_panel_set_activate_sink(c->tree, on_tree_activate, c);
    }

    update_commands(c);
    apr_ui_app_set_status(app, APR_S_UI_STATUS_READY);

    /* STAYING UP TO DATE (update.h). Three triggers, one gate: this one, the
     * five-minute timer below, and the end of every recording all call
     * apr_update_due() against the timestamp stored on disk, so a restart loop
     * cannot become a request storm and neither can start/stop cycling.
     *
     * Started here rather than in main() so that everything it can say has a
     * window to say it in -- an update prompt with no parent is a dialog a
     * screen reader user meets with no context. */
    /* A TEST MUST LEAVE NO TRACE ON THE MACHINE RUNNING IT -- the same rule
     * that keeps a suite's tray icon out of a real notification area, and it
     * broke the moment update_key.c stopped being zeros.
     *
     * Eight suites build a real controller. With a real key compiled in, every
     * one of those processes began a genuine HTTPS request and rewrote
     * LastCheck under HKCU on the developer's own machine. Nothing was
     * announced, because the URL still 404s -- which is exactly why it went
     * unnoticed rather than why it was harmless.
     *
     * Set by CMake for every test, present and future, so a new UI suite
     * cannot forget. The gate is here rather than inside update.c because the
     * `update` COMMAND must still work when a person runs it deliberately;
     * what must not happen on its own is the unattended cadence. */
    if (GetEnvironmentVariableW(L"APPRECORDER_NO_UPDATE", NULL, 0) == 0) {
        start_update_check(c, APR_UPDATE_WHY_STARTUP);
        SetTimer(c->frame, APR_CTL_TIMER_UPDATE, APR_UPDATE_INTERVAL_MS, NULL);
    } else {
        APR_INFO(L"update: APPRECORDER_NO_UPDATE is set; no automatic checks");
    }

    *out = c;
    return apr_ok();
}

void apr_controller_destroy(AprController *c)
{
    if (!c) return;

    /* NEVER abandon a recording. apr_runner_destroy stops the loop and waits
     * for it, and the loop finalizes every action before it returns. */
    if (c->runner) {
        apr_runner_request_stop(c->runner);
        apr_runner_destroy(c->runner);
        c->runner = NULL;
    }
    c->recording = 0;
    c->paused    = 0;
    if (c->idle_event) SetEvent(c->idle_event);

    if (c->canvas) apr_canvas_set_announce(c->canvas, NULL, NULL);
    if (c->canvas) apr_canvas_set_edit_sink(c->canvas, NULL, NULL);
    if (c->canvas) apr_canvas_set_graph(c->canvas, NULL);
    if (c->tree) {
        apr_tree_panel_set_selection_sink(c->tree, NULL, NULL);
        apr_tree_panel_set_activate_sink(c->tree, NULL, NULL);
        apr_tree_panel_set_graph(c->tree, NULL);
    }

    if (c->frame && IsWindow(c->frame)) KillTimer(c->frame, APR_CTL_TIMER_UPDATE);

    /* THE SWAP, ON EXIT, AND ONLY HERE. Every recording has been stopped and
     * every file closed by the block above, so rule 1 is satisfied by
     * construction rather than by a check that could be forgotten. Windows
     * permits renaming a running image, which is why this needs no second
     * process; the previous build is kept as .old until the new one has
     * started once (update.h section 5). */
    if (c->update_staged) {
        wchar_t image[APR_DISC_PATH_CCH];
        if (ctl_update_image(c, image, APR_DISC_PATH_CCH)) {
            AprErr se = apr_update_swap(image, c->update_version);
            if (apr_failed(&se)) APR_LOG_ERR(APR_LOG_ERROR, &se);
        }
        c->update_staged = 0;
    }

    apr_tray_destroy(c->tray);
    apr_graph_destroy(c->graph);
    /* Last: it is set above so that anything still waiting on it is released
     * before the handle it is waiting on goes away. */
    if (c->idle_event) CloseHandle(c->idle_event);
    free(c);
}

AprGraph *apr_controller_graph(AprController *c) { return c ? c->graph : NULL; }

AprErr apr_controller_set_graph(AprController *c, AprGraph *g)
{
    if (!c) return APR_ERR(APR_E_INVALID_ARG, L"apr_controller_set_graph: NULL");
    if (c->recording) {
        return APR_ERR(APR_E_STATE, L"cannot replace the graph while recording");
    }

    /* ADOPTING A GRAPH BUILDS WINDOWS, and a window belongs to the thread that
     * created it. Doing that on a caller's thread leaves the frame holding
     * children it cannot destroy, which surfaces as a close that never
     * completes -- no error, no diagnostic, just a window that will not go
     * away. So a call from anywhere else marshals; a call from the window's
     * own thread costs one comparison. */
    if (c->frame && IsWindow(c->frame) &&
        GetWindowThreadProcessId(c->frame, NULL) != GetCurrentThreadId()) {
        return (int)(INT_PTR)SendMessageW(c->frame, APR_CTL_WM_SET_GRAPH, 0,
                                          (LPARAM)g)
                 ? apr_ok()
                 : APR_ERR(APR_E_STATE, L"the window refused the new model");
    }
    return ctl_adopt_graph(c, g) ? apr_ok()
                                 : APR_ERR(APR_E_STATE, L"could not adopt");
}

int apr_controller_recording(const AprController *c)
{
    return c ? c->recording : 0;
}

int apr_controller_paused(const AprController *c)
{
    return c ? c->paused : 0;
}

AprTrayState apr_controller_tray_state(const AprController *c)
{
    return c ? apr_tray_state(c->tray) : APR_TRAY_IDLE;
}

void apr_controller_model_changed(AprController *c)
{
    if (!c || !c->frame || !IsWindow(c->frame)) return;
    /* SendMessage, not Post: a caller that has just edited the graph is
     * usually about to say so out loud, and it needs the views to already
     * agree by then. It is a marshal onto the same thread in the common case,
     * so it costs nothing there. */
    SendMessageW(c->frame, APR_CTL_WM_REFRESH, 0, 0);
}

int64_t apr_controller_elapsed_ms(const AprController *c)
{
    if (!c) return 0;
    if (c->recording && c->runner) return apr_runner_elapsed_ms(c->runner);
    return c->last_elapsed_ms;
}
