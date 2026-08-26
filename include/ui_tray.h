/*
 * ui_tray.h -- the notification area icon, its menu, its live tooltip, and the
 * balloons that are the only reliable way to say something while the window is
 * not in front.
 *
 * ===========================================================================
 * WHY A RECORDER'S TRAY ICON IS NOT A CONVENIENCE
 *
 *   A recording runs for hours and the window is minimised for almost all of
 *   them. For that whole time the tray icon IS the application: it is where
 *   the state lives, where the controls are, and where anything that goes
 *   wrong has to surface.
 *
 * ===========================================================================
 * THE TOOLTIP IS A STATUS READOUT, AND THAT IS AN ACCESSIBILITY FEATURE
 *
 *   Windows has a keyboard path into the notification area -- Windows+B, then
 *   the arrow keys -- and a screen reader reads each icon's tooltip as the
 *   focus lands on it. So a tooltip that says "apprecorder -- recording,
 *   01:12:30" means the author can check on a session from inside any other
 *   application, without raising a window, without interrupting what he is
 *   doing, and without anything having to be spoken at him unprompted.
 *
 *   That is why apr_tray_set_status takes the elapsed time and why it is
 *   called every second rather than only on a state change. A stale tooltip
 *   is worse than none: it answers the question wrongly.
 *
 * ===========================================================================
 * BALLOONS ARE THE BACKGROUND-ANNOUNCEMENT CHANNEL
 *
 *   The canvas announces through a live region (EVENT_OBJECT_LIVEREGIONCHANGED)
 *   and through focus moves, and both work well -- while the window is in
 *   front. A live-region change on an UNFOCUSED BACKGROUND WINDOW is not
 *   reliably announced by any screen reader, which is precisely the state this
 *   application spends its recording in.
 *
 *   So anything that happens while we are not foreground goes out as a
 *   notification-area balloon (NIF_INFO). Screen readers announce those
 *   reliably, they are visible to sighted users too, and they use the same API
 *   as the icon -- no new dependency, no text-to-speech anywhere in this
 *   product.
 *
 *   THIS PRODUCT NEVER SPEAKS DIRECTLY. No SAPI, no nvdaControllerClient, no
 *   Tolk. UIA and the shell hand SEMANTICS to the screen reader, which then
 *   applies the user's own voice, rate, verbosity, interruption rules and
 *   BRAILLE ROUTING. Speaking directly bypasses every one of those and only
 *   works for the readers we happen to have a driver for. It also means that
 *   with no screen reader running there is simply no consumer and therefore no
 *   speech -- no detection logic to write and nothing to go wrong.
 *
 * ===========================================================================
 * KEYBOARD
 *
 *   Windows+B then arrows reaches the icon. Enter or Space on it is the
 *   default action (show the window). Shift+F10 or the Applications key opens
 *   the context menu -- the shell delivers both as WM_CONTEXTMENU through the
 *   callback message, so the menu is not a right-click feature that a keyboard
 *   happens to reach; it is the same one code path.
 *
 *   The menu is a real HMENU shown with TrackPopupMenuEx. Not owner-drawn:
 *   a standard menu is one of the best-supported things a screen reader meets.
 *
 * THREADING: create, destroy and every update must happen on the thread that
 * owns `owner`. Shell_NotifyIcon posts its callbacks to that window.
 */
#ifndef APPRECORDER_UI_TRAY_H
#define APPRECORDER_UI_TRAY_H

#include <windows.h>

#include "err.h"
#include "strings.h"

/* The private window message the shell sends us for icon activity. It is in
 * the WM_APP range and the frame routes it straight to apr_tray_on_message. */
#define APR_TRAY_WM_ICON (WM_APP + 0x40)

typedef enum AprTrayState {
    APR_TRAY_IDLE = 0,
    APR_TRAY_RECORDING,
    APR_TRAY_FINISHING
} AprTrayState;

typedef struct AprTray AprTray;

/* Commands the menu can raise. They are ordinary APR_CMD_* values from
 * ui_app.h, delivered to the frame as WM_COMMAND, so the tray adds no second
 * dispatch path and an operation cannot behave differently depending on which
 * surface invoked it. */
AprErr apr_tray_create(HWND owner, AprTray **out);
void   apr_tray_destroy(AprTray *t);

/* Handle APR_TRAY_WM_ICON. Returns nonzero when it was handled. */
int apr_tray_on_message(AprTray *t, WPARAM wp, LPARAM lp);

/* The shell restarts (Explorer crashed, or the user signed in again) and every
 * icon has to be added back. The frame registers for "TaskbarCreated" and
 * calls this; without it the icon silently disappears for the rest of the
 * session, which for a recorder means losing the only surface it has.
 * Returns nonzero when the message was that one. */
int apr_tray_on_taskbar_created(AprTray *t, UINT msg);

/* Update the live tooltip. `elapsed` is the already-formatted duration and is
 * ignored unless the state is RECORDING. Cheap enough to call every second,
 * which is what the frame does. */
void apr_tray_set_status(AprTray *t, AprTrayState state, const wchar_t *elapsed);

AprTrayState apr_tray_state(const AprTray *t);

/* A balloon. `title` is a catalog id; `text` is already formatted, because it
 * usually contains a source name. See the header note on why this exists. */
void apr_tray_notify(AprTray *t, AprStrId title, const wchar_t *text);

/* The exact tooltip text currently published, for tests -- "the tray says what
 * is happening" is otherwise only checkable by a human with a mouse. Returns
 * the characters written. */
size_t apr_tray_tip(const AprTray *t, wchar_t *buf, size_t cch);

/* The context menu, built fresh from the catalog each time so that the
 * recording items are enabled correctly and a language change is picked up.
 * Exposed for tests, which assert every item has text and a mnemonic. */
HMENU apr_tray_build_menu(const AprTray *t);

/* What the menu offers depends on whether a recording is running; the frame
 * tells the tray rather than the tray reaching into a controller. */
void apr_tray_set_can_record(AprTray *t, int can_start, int can_stop);

#endif /* APPRECORDER_UI_TRAY_H */
