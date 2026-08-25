/*
 * ui_app.h -- the frame: window class, message loop, layout, and the two
 * conventions every later UI file inherits (direction, and accessible names).
 *
 * ===========================================================================
 * CONVENTION 1 -- DIRECTION. READ THIS BEFORE PAINTING ANYTHING.
 *
 *   apr_str_is_rtl() is the only source of direction in the product. This
 *   header adds no second one; apr_ui_dir() forwards it.
 *
 *   WS_EX_LAYOUTRTL IS APPLIED PER STANDARD CONTROL, NEVER TO A WINDOW WE
 *   PAINT. That is a sharper rule than design 6.2 states, and the difference
 *   matters:
 *
 *     - Design 6.2 says LAYOUTRTL "does not mirror anything we paint
 *       ourselves". It is the opposite. A window with WS_EX_LAYOUTRTL gets a
 *       MIRRORED DEVICE CONTEXT: every coordinate we pass to a GDI call is
 *       reflected about the client area's vertical centre line, bitmaps come
 *       out flipped, and BitBlt from an unmirrored source is reversed. It is
 *       our own painting that it mirrors, and it mirrors it in a way that is
 *       wrong for artwork.
 *
 *     - Worse, the style is INHERITED. A child created under a LAYOUTRTL
 *       parent gets it too, unless the parent carries WS_EX_NOINHERITLAYOUT.
 *       So "set it on the frame and forget it" silently mirrors the canvas and
 *       every node window as well.
 *
 *   Hence the rule. The frame is NOT LAYOUTRTL and carries
 *   WS_EX_NOINHERITLAYOUT so nothing can acquire it by accident. Child
 *   PLACEMENT is mirrored by apr_ui_layout(), which computes rectangles from a
 *   leading edge. Standard controls that must lay out their own insides right
 *   to left -- a TreeView's indent guides, a status bar's parts, a scroll bar's
 *   side -- get WS_EX_LAYOUTRTL individually via apr_ui_apply_rtl(), which is
 *   safe precisely because we never paint inside them. Custom-painted windows
 *   mirror their own geometry with apr_ui_mirror_rect().
 *
 *   AND THE PART THAT IS NOT ABOUT PIXELS: logical order never flips. Child
 *   z-order, tab order, the accessibility tree and the TreeView all stay
 *   source -> bus -> action in both directions. A screen reader user's
 *   navigation must not reverse when the interface language changes; only the
 *   painting does. apr_ui_layout() mirrors rectangles and touches nothing
 *   else, which is what keeps those two facts from drifting apart.
 *
 * ===========================================================================
 * CONVENTION 2 -- EVERY CONTROL HAS A NAME, AND THE NAME COMES FROM THE
 * CATALOG
 *
 *   A control with no accessible name is read by a screen reader as its
 *   control type and nothing else: "pane", "tree". That is the single most
 *   common accessibility defect in native Windows apps and it is invisible to
 *   anyone testing by eye.
 *
 *   So: every window this product creates is named through
 *   apr_ui_set_accessible_name(), with an AprStrId -- never a literal, per
 *   AGENTS.md rule 6 -- and tests/test_ui_a11y.c walks the live UI Automation
 *   tree and fails if any element's Name is empty. That test is the real
 *   guarantee; this convention is how you pass it.
 *
 *   Names are re-applied on a language change, which is why they are stored as
 *   ids rather than resolved once at creation.
 *
 * ===========================================================================
 * KEYBOARD
 *
 *   Every operation is on the menu, and every menu item has a mnemonic and an
 *   accelerator. That is not belt and braces: the menu bar is what makes an
 *   operation DISCOVERABLE to a screen reader user, and the accelerator is
 *   what makes it fast. An operation that exists only as a canvas gesture does
 *   not exist.
 *
 *   F6 / Shift+F6 cycle panes -- the standard Windows idiom, and the thing
 *   that lets Tab stay meaningful INSIDE a pane. Both are handled by the
 *   frame, so a pane never has to implement it.
 */
#ifndef APPRECORDER_UI_APP_H
#define APPRECORDER_UI_APP_H

#include <windows.h>

#include "err.h"
#include "strings.h"
#include "ui_theme.h"

/* ---------------------------------------------------------------------------
 * Direction
 * ------------------------------------------------------------------------- */

typedef enum AprUiDir {
    APR_DIR_LTR = 0,
    APR_DIR_RTL = 1
} AprUiDir;

/* The interface direction. Forwards apr_str_is_rtl(); do not cache it across a
 * language change. */
AprUiDir apr_ui_dir(void);

/* Reflect `r` about the vertical centre line of `bounds`. Identity when
 * `dir` is LTR, so a caller may call it unconditionally -- and should, because
 * an `if (rtl)` at a call site is a direction source that apr_str_is_rtl()
 * does not control. */
void apr_ui_mirror_rect(RECT *r, const RECT *bounds, AprUiDir dir);

/* The left edge, in client coordinates, of a box `w` wide placed `lead` pixels
 * from the LEADING edge of `bounds` -- the left in LTR, the right in RTL. */
int apr_ui_lead_x(const RECT *bounds, int lead, int w, AprUiDir dir);

/* Set or clear WS_EX_LAYOUTRTL on a STANDARD control.
 *
 * NEVER call this on a window whose WM_PAINT we write. See CONVENTION 1. */
void apr_ui_apply_rtl(HWND control, AprUiDir dir);

/* ---------------------------------------------------------------------------
 * Layout -- a pure function, so it is testable without a window
 *
 * The frame is: a menu bar (owned by the OS, not laid out here), a structure
 * panel on the LEADING side, a splitter, the canvas filling the rest, and a
 * status bar along the bottom.
 *
 *   LTR                              RTL
 *   +----------+------------------+  +------------------+----------+
 *   | tree     | canvas           |  | canvas           | tree     |
 *   +----------+------------------+  +------------------+----------+
 *   | status                      |  | status                      |
 *   +-----------------------------+  +-----------------------------+
 *
 * Bottom and top do not mirror; only the horizontal axis does. The status bar
 * is full width in both.
 * ------------------------------------------------------------------------- */

typedef struct AprUiLayoutIn {
    RECT     client;      /* the frame's client rect, origin at 0,0        */
    AprUiDir dir;
    int      tree_w;      /* device px; clamped against pane_min_w         */
    int      pane_min_w;
    int      splitter_w;
    int      status_h;
    int      tree_visible;/* 0 hides the panel and the splitter entirely   */
} AprUiLayoutIn;

typedef struct AprUiRects {
    RECT tree;
    RECT splitter;
    RECT canvas;
    RECT status;
} AprUiRects;

/* Pure. No window handles, no globals, no direction lookup -- `dir` is an
 * input precisely so a test can drive both directions in one process. */
void apr_ui_layout(const AprUiLayoutIn *in, AprUiRects *out);

/* ---------------------------------------------------------------------------
 * Accessible names
 *
 * Both of these are best-effort and never fail the caller: an accessibility
 * annotation that could not be attached must not take down the window it was
 * describing. They log instead.
 * ------------------------------------------------------------------------- */

/* Give `hwnd` the accessible name `id` resolves to, in the current language.
 * Also sets the window text where that is the control's natural name, so the
 * name survives even if the annotation service is unavailable. */
void apr_ui_set_accessible_name(HWND hwnd, AprStrId id);

/* The accessible DESCRIPTION -- the longer sentence a screen reader reads
 * after the name. Use it for what a pane is FOR and how to move around it. */
void apr_ui_set_accessible_description(HWND hwnd, AprStrId id);

/* Same, for text that is not in the catalog because it is user data: a bus
 * name, a file path. The CALLER must have built it from catalog entries -- see
 * strings.h; this is not a licence to concatenate. */
void apr_ui_set_accessible_name_text(HWND hwnd, const wchar_t *text);

/* The DESCRIPTION, for a sentence that already contains user data -- a canvas
 * node's edges, say. Same contract as the name form: the caller formatted it
 * from the catalog, this is not a licence to concatenate.
 *
 * It lives here rather than in canvas.c because IAccPropServices is created
 * once per UI thread and released with the app on that same thread. A second
 * copy of that lifetime in another file is the rule-3 failure this avoids. */
void apr_ui_set_accessible_description_text(HWND hwnd, const wchar_t *text);

/* Override the MSAA role a window reports -- ROLE_SYSTEM_GROUPING for a canvas
 * node, say. Without this a custom-class window is a nameless "pane" whatever
 * it actually is, and the UIA control type a screen reader announces is wrong
 * in a way no visual review can see.
 *
 * `msaa_role` is a ROLE_SYSTEM_* value from oleacc.h. The UIA bridge maps it to
 * a ControlType; tests/test_ui_canvas.c asserts the mapped result rather than
 * the input, because the mapping is the OS's, not ours. */
void apr_ui_set_accessible_role(HWND hwnd, long msaa_role);

/* ---------------------------------------------------------------------------
 * Command ids
 *
 * Menu item, accelerator and WM_COMMAND all use the same id, so there is one
 * number per operation and no table mapping one to another.
 * ------------------------------------------------------------------------- */

#define APR_CMD_FILE_NEW        0x0101
#define APR_CMD_FILE_OPEN       0x0102
#define APR_CMD_FILE_SAVE       0x0103
#define APR_CMD_FILE_SAVE_AS    0x0104
#define APR_CMD_FILE_EXIT       0x0105

#define APR_CMD_ADD_SOURCE      0x0201
#define APR_CMD_ADD_BUS         0x0202
#define APR_CMD_ADD_ACTION      0x0203
#define APR_CMD_CONNECT         0x0204
#define APR_CMD_REMOVE          0x0205

#define APR_CMD_RECORD_START    0x0301
#define APR_CMD_RECORD_STOP     0x0302

#define APR_CMD_VIEW_TREE       0x0401
#define APR_CMD_VIEW_DARK       0x0402
#define APR_CMD_NEXT_PANE       0x0403
#define APR_CMD_PREV_PANE       0x0404

#define APR_CMD_HELP_KEYS       0x0501
#define APR_CMD_HELP_ABOUT      0x0502

/* ---------------------------------------------------------------------------
 * Panes
 * ------------------------------------------------------------------------- */

typedef enum AprUiPaneSlot {
    APR_PANE_TREE = 0,     /* the structure panel -- leading side */
    APR_PANE_CANVAS = 1,   /* the signal-flow canvas              */
    APR_PANE_COUNT = 2
} AprUiPaneSlot;

/* ---------------------------------------------------------------------------
 * The app
 * ------------------------------------------------------------------------- */

typedef struct AprUiApp AprUiApp;

/* A command handler. Return nonzero if the command was handled.
 *
 * The frame owns no model. Everything that changes the graph is routed out
 * through this so that app.c does not have to include graph.h and so that the
 * canvas, the tree panel and a future scripting surface all reach the same
 * operations by the same route. A command with no handler is shown DISABLED --
 * greyed but present, named, and reachable by a screen reader, which is the
 * correct way to say "not yet" to someone who cannot see that it is grey. */
typedef int (*AprUiCommandFn)(AprUiApp *app, int command_id, void *user);

/* Create the frame. Registers the window classes, builds the menu from the
 * catalog, creates the status bar and the two panes, applies the theme and
 * dark mode. Does not show the window.
 *
 * MUST be called on a thread that has an apartment-threaded COM apartment and
 * a message queue; that thread owns the window from then on. */
AprErr apr_ui_app_create(HINSTANCE inst, AprUiApp **out);

/* Destroys the window if it still exists, then the app. Safe with NULL. */
void apr_ui_app_destroy(AprUiApp *app);

HWND      apr_ui_app_hwnd(const AprUiApp *app);
HWND      apr_ui_app_pane(const AprUiApp *app, AprUiPaneSlot slot);
HWND      apr_ui_app_status_bar(const AprUiApp *app);
AprTheme *apr_ui_app_theme(AprUiApp *app);

void apr_ui_app_set_command_handler(AprUiApp *app, AprUiCommandFn fn, void *user);

/* Enable or disable one command everywhere it appears -- menu and
 * accelerator. Disabled is not hidden, deliberately: see AprUiCommandFn. */
void apr_ui_app_enable_command(AprUiApp *app, int command_id, int enabled);

void apr_ui_app_show(AprUiApp *app, int cmd_show);

/* Put `id`'s text in the status bar. Status text is also announced, so it must
 * be a whole sentence from the catalog, never a fragment. */
void apr_ui_app_set_status(AprUiApp *app, AprStrId id);

/* Re-run the layout. Called for you on WM_SIZE and WM_DPICHANGED; call it
 * yourself after showing or hiding a pane. */
void apr_ui_app_relayout(AprUiApp *app);

/* Ask the frame to close, from any thread. */
void apr_ui_app_request_close(const AprUiApp *app);

/* The message loop. Runs until the frame is destroyed; returns the WM_QUIT
 * exit code. Handles accelerators and dialog-style Tab navigation.
 *
 * This is the entry point a future WinMain calls. There is deliberately no
 * WinMain in this library: an entry point in a static library collides with
 * every test's main(), and which front end owns the process is not this
 * module's decision to make. */
int apr_ui_app_run(AprUiApp *app);

/* ---------------------------------------------------------------------------
 * What the next two agents implement
 *
 * app.c calls these when the corresponding source file exists (CMake defines
 * APR_HAVE_UI_CANVAS / APR_HAVE_UI_TREE_PANEL from its presence, the same way
 * the action registry discovers encoders). Until then app.c creates a named,
 * focusable placeholder pane, so the accessibility tree is complete and the
 * a11y test is meaningful from day one.
 *
 * Each returns a child window of `parent`, already themed. Return NULL to fall
 * back to the placeholder. The frame positions it; do not call SetWindowPos on
 * yourself. Name yourself with apr_ui_set_accessible_name -- the frame does
 * not do it for you, because only you know whether a sub-control needs the
 * name instead.
 * ------------------------------------------------------------------------- */

HWND apr_canvas_create(HWND parent, AprTheme *theme);
HWND apr_tree_panel_create(HWND parent, AprTheme *theme);

#endif /* APPRECORDER_UI_APP_H */
