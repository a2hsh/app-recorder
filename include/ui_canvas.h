/*
 * ui_canvas.h -- the signal-flow canvas: the pane that makes this a recorder
 * you can see rather than a form you fill in.
 *
 * ===========================================================================
 * THE ONE DECISION EVERYTHING ELSE FOLLOWS: A NODE IS A REAL WINDOW
 *
 *   Every node on this canvas is an actual Win32 child HWND (ui_node.h), not
 *   an owner-drawn rectangle and not a Direct2D primitive. That is the whole
 *   reason the canvas can be a canvas AND be readable:
 *
 *     - MSAA/UIA exposure, focus, and tab order come from the platform. There
 *       is no hand-written IRawElementProviderFragment anywhere in this
 *       product, and therefore no permanent ownership of every accessibility
 *       bug in one.
 *     - Full visual control is retained anyway, because we still paint every
 *       pixel of the node in its own WM_PAINT.
 *
 *   DO NOT "OPTIMISE" NODES INTO PAINTED RECTANGLES. It looks like a free win
 *   -- fewer handles, one paint pass -- and it silently deletes the property
 *   the application exists to have. Design 6.1 records the two alternatives
 *   that were considered and rejected, so that this is not relitigated.
 *
 *   Edges are the other half of that trade. An edge has no window, so it has
 *   no element to land on, so IT MUST BE IN THE NODES' ACCESSIBLE TEXT: each
 *   node's name says what it is and its description says what it feeds and
 *   what feeds it ("Teams, source" / "Feeds Main Mix and Teams Only."). An
 *   edge that is only painted does not exist for this application's first
 *   user.
 *
 * ===========================================================================
 * DIRECTION -- READ ui_app.h CONVENTION 1 FIRST, THEN THIS
 *
 *   The canvas is a painted window, so it NEVER carries WS_EX_LAYOUTRTL and
 *   neither does any node. The style hands out a mirrored device context and
 *   is inherited by children, so setting it here would reflect every node and
 *   every glyph inside it.
 *
 *   Signal flow is a LAYOUT PARAMETER: sources on the left and actions on the
 *   right in English, sources on the RIGHT and actions on the LEFT in Arabic.
 *   apr_canvas_node_rect() is pure and takes the direction as an argument,
 *   which is what lets tests/test_ui_canvas.c check both geometries in one
 *   process, and it mirrors through apr_ui_mirror_rect() -- the product's
 *   single mirroring site -- rather than by subtracting coordinates by hand.
 *
 *   LOGICAL ORDER NEVER MIRRORS. Child z-order, tab order and the
 *   accessibility tree stay source -> bus -> action in both languages. A
 *   screen reader user's world must not reverse because the interface language
 *   changed; only the picture does.
 *
 * ===========================================================================
 * THE KEYBOARD MODEL IS THE PRIMARY INTERFACE, AND IT IS DATA
 *
 *   The binding table below is the single source of truth: it is what
 *   dispatches a key press AND what Help > Keyboard Shortcuts renders. A
 *   shortcut therefore cannot be documented as one key and implemented as
 *   another, and an operation cannot quietly become mouse-only --
 *   tests/test_ui_canvas.c asserts that every AprCanvasOp has at least one
 *   binding and that every binding has a catalog label.
 *
 *   The shape of it:
 *
 *     Tab / Shift+Tab      the next / previous node in LOGICAL order, and at
 *                          either end of the list, OUT of the pane. Never
 *                          mirrors.
 *     Up / Down            within one column -- the vertical axis does not
 *                          mirror in any language.
 *     Left / Right         ALONG THE SIGNAL FLOW, and these two DO mirror,
 *                          because they are geometric: the arrow that points
 *                          downstream on screen is the arrow that goes
 *                          downstream. In Arabic that is Left.
 *     Ctrl+Down / Ctrl+Up  the same two moves stated LOGICALLY -- "go to what
 *                          this feeds" / "go to what feeds this" -- and these
 *                          never mirror. Both routes exist on purpose: a
 *                          sighted user reaches for the arrow that points the
 *                          right way, and a screen reader user should not have
 *                          to know which way the picture happens to run.
 *     Ctrl+E               connect. Two steps: mark one end, move, press
 *                          again. Escape cancels. The mouse does the same two
 *                          steps, so there is one code path and one set of
 *                          announcements.
 *     Ctrl+Shift+E         disconnect, the same two steps.
 *     Delete               remove the focused node.
 *     Plus / Minus         the focused source's level, in whole decibels.
 *     Enter                say the node and its edges again.
 *
 *   EVERY ONE OF THESE ANNOUNCES. A graph edit the user cannot hear is a graph
 *   edit that did not happen as far as they are concerned, so every operation
 *   that changes the model rewrites the affected nodes' accessible text, fires
 *   the MSAA name-change event, moves focus onto the node whose meaning
 *   changed, and hands the sentence to the announcement sink.
 *
 * THREAD SAFETY: everything here is UI-thread only, like every other window.
 * The graph pointer handed to apr_canvas_set_graph is borrowed and must not be
 * mutated from another thread while the canvas holds it.
 */
#ifndef APPRECORDER_UI_CANVAS_H
#define APPRECORDER_UI_CANVAS_H

#include <windows.h>
#include <stddef.h>

#include "graph.h"
#include "strings.h"
#include "ui_app.h"
#include "ui_theme.h"

/* ---------------------------------------------------------------------------
 * Columns
 *
 * Three lanes, always all three, even when one is empty. A lane that appears
 * and disappears as the graph is built moves every other node sideways under
 * the user's hands, and "the picture moved" is expensive for a magnifier user
 * and meaningless to a screen reader user.
 * ------------------------------------------------------------------------- */

typedef enum AprCanvasColumn {
    APR_COL_SOURCE = 0,
    APR_COL_BUS    = 1,
    APR_COL_ACTION = 2,
    APR_COL_COUNT  = 3
} AprCanvasColumn;

/* ---------------------------------------------------------------------------
 * Layout -- pure, exactly like apr_ui_layout()
 * ------------------------------------------------------------------------- */

typedef struct AprCanvasLayoutIn {
    RECT     viewport;             /* the canvas client rect, origin at 0,0  */
    AprUiDir dir;
    int      count[APR_COL_COUNT]; /* nodes in each lane                     */

    /* All device pixels, already scaled -- they come straight from
     * apr_theme_metrics(). Nothing here multiplies by a DPI factor. */
    int node_w;
    int node_h;
    int gap_x;
    int gap_y;
    int margin;

    /* How far the content is scrolled, in device pixels, measured from the
     * client area's LEFT edge in both directions. Not "from the leading edge":
     * the mirroring has already happened by the time the scroll is applied, so
     * one sign convention covers both languages and there is no second place
     * for a direction test to hide. */
    int scroll_x;
    int scroll_y;
} AprCanvasLayoutIn;

/* The full extent of the drawing, before scrolling and never smaller than the
 * viewport. Pure. */
void apr_canvas_content_size(const AprCanvasLayoutIn *in, SIZE *out);

/* Where node `index` of `column` sits, in canvas client coordinates with the
 * current scroll applied. Returns 0 (and leaves `out` empty) for an index
 * outside the column.
 *
 * PURE, AND THAT IS THE POINT: direction is an argument, so a test drives LTR
 * and RTL in one process and neither answer can come from a global. */
int apr_canvas_node_rect(const AprCanvasLayoutIn *in, AprCanvasColumn column,
                         int index, RECT *out);

/* ---------------------------------------------------------------------------
 * Operations and their keys
 * ------------------------------------------------------------------------- */

typedef enum AprCanvasOp {
    APR_CANVAS_OP_NONE = 0,
    APR_CANVAS_OP_NEXT_NODE,
    APR_CANVAS_OP_PREV_NODE,
    APR_CANVAS_OP_NEXT_IN_COLUMN,
    APR_CANVAS_OP_PREV_IN_COLUMN,
    APR_CANVAS_OP_DOWNSTREAM,
    APR_CANVAS_OP_UPSTREAM,
    APR_CANVAS_OP_FIRST,
    APR_CANVAS_OP_LAST,
    APR_CANVAS_OP_CONNECT,
    APR_CANVAS_OP_DISCONNECT,
    APR_CANVAS_OP_REMOVE,
    APR_CANVAS_OP_ADD_SOURCE,
    APR_CANVAS_OP_ADD_BUS,
    APR_CANVAS_OP_ADD_ACTION,
    APR_CANVAS_OP_GAIN_UP,
    APR_CANVAS_OP_GAIN_DOWN,
    APR_CANVAS_OP_CANCEL,
    APR_CANVAS_OP_DESCRIBE,
    APR_CANVAS_OP_COUNT
} AprCanvasOp;

/* APR_KMOD_CTRL / SHIFT / ALT moved to ui_app.h, which this file includes.
 * Both binding tables and the one key-name formatter need them, and the frame
 * cannot include this header. The names and values are unchanged. */

typedef struct AprCanvasBinding {
    AprCanvasOp op;
    UINT        vk;
    UINT        mods;      /* APR_KMOD_* */

    /* Nonzero when this binding is GEOMETRIC and therefore mirrors: VK_LEFT
     * and VK_RIGHT swap in an RTL layout. Nothing else in the table mirrors,
     * because nothing else in the table is about which way the picture runs. */
    int mirrored;

    /* Nonzero when the FRAME delivers this one, not us: it is in the frame's
     * accelerator table, so TranslateAccelerator turns it into a WM_COMMAND
     * before any window sees the keystroke, and it reaches the canvas through
     * apr_canvas_command() instead. The row is still here because Help >
     * Keyboard Shortcuts must show the key the user actually presses, and
     * because the operation itself is identical either way. */
    int platform;

    AprStrId label;        /* what this operation IS, in the user's language */
} AprCanvasBinding;

size_t                  apr_canvas_binding_count(void);
const AprCanvasBinding *apr_canvas_binding_at(size_t index);

/* The operation `vk` + `mods` means in `dir`, or APR_CANVAS_OP_NONE.
 * Pure; `platform` rows are matched too, so a caller can tell that Tab is
 * spoken for even though it never arrives. */
AprCanvasOp apr_canvas_op_for_key(UINT vk, UINT mods, AprUiDir dir);

/* The modifier bits currently held down, from GetKeyState. */
UINT apr_canvas_current_mods(void);

/* ---------------------------------------------------------------------------
 * The canvas
 *
 * apr_canvas_create() is declared by ui_app.h, because the frame calls it
 * before anything includes this header.
 * ------------------------------------------------------------------------- */

/* Where an announcement goes. The frame wires this to the status bar; a test
 * wires it to a buffer. The canvas always keeps the last one regardless (see
 * apr_canvas_last_announcement) so that "the user was told" is a property that
 * can be asserted rather than reviewed.
 *
 * THIS IS THE ONLY WAY OUT. The canvas raises no accessibility event of its
 * own for an announcement: a reader answers a live-region event by reading the
 * element's NAME, and this element's name is the PANE's name, so doing it here
 * made readers speak "Signal flow" instead of the sentence. One owner for
 * "say this to the user" -- the status bar's live region -- and this is the
 * road to it. An embedder that leaves the sink unwired gets no announcements.
 *
 * What the canvas still does by itself is change the NAME of the node the edit
 * changed, which is the mechanism every reader honours on a focused element. */
typedef void (*AprCanvasAnnounceFn)(void *user, const wchar_t *text);

void apr_canvas_set_announce(HWND canvas, AprCanvasAnnounceFn fn, void *user);

/* The most recent announcement, or an empty string. Returns its length. */
size_t apr_canvas_last_announcement(HWND canvas, wchar_t *buf, size_t cch);

/* "I changed the model." Raised AFTER an edit has completed and this canvas
 * has already rewritten its own names, descriptions and node windows -- never
 * from inside one.
 *
 * WHY IT EXISTS: the canvas owns keys the frame's accelerator table does not
 * claim (Ctrl+Shift+E disconnects, plus and minus change a level) and it
 * completes a connection on a mouse click. Every one of those edits the graph
 * without any WM_COMMAND reaching the controller, so the OTHER view -- the
 * tree panel -- had no way to hear about it and went on describing edges that
 * no longer existed.
 *
 * THE LISTENER MUST NOT REBUILD THIS CANVAS. The canvas has already re-synced
 * itself; destroying its node windows from here would pull them out from under
 * the operation that is about to put focus on one. Refresh the other views. */
typedef void (*AprCanvasEditFn)(void *user);

void apr_canvas_set_edit_sink(HWND canvas, AprCanvasEditFn fn, void *user);

/* Adopt a model. Borrowed, not owned; pass NULL to show an empty canvas.
 * Rebuilds the node windows immediately. */
void apr_canvas_set_graph(HWND canvas, AprGraph *g);
AprGraph *apr_canvas_graph(HWND canvas);

/* Re-sync the node windows to the model. Call after anything mutates the graph
 * behind the canvas's back. Keeps focus on the same model node when it
 * survives. */
void apr_canvas_rebuild(HWND canvas);

/* Nodes, in LOGICAL order -- every source, then every bus, then every bus's
 * outputs. This order is the tab order, the z-order and the accessibility
 * order, in both directions. */
size_t apr_canvas_node_count(HWND canvas);
HWND   apr_canvas_node_at(HWND canvas, size_t index);

/* The node that has focus, or the one that would take it. NULL when empty. */
HWND apr_canvas_focused_node(HWND canvas);
int  apr_canvas_focus_node(HWND canvas, size_t index);

/* AGREE WITH THE OTHER VIEW WITHOUT STEALING THE KEYBOARD.
 *
 * Move the canvas's idea of "the current node" to `index`, scroll it into
 * view, repaint -- and DO NOT call SetFocus unless this pane already had it.
 *
 * This exists because the tree panel emits a selection on EVERY caret move,
 * and answering that with a focus change made the Structure panel unusable:
 * press Down in the tree, focus was yanked onto a canvas node, the reader
 * announced the canvas instead of the row, and the next Down drove the canvas.
 * Every row past the first was unreachable.
 *
 * Focus follows only a real ACTIVATION -- Enter, or a double click -- which is
 * what apr_tree_panel_set_activate_sink reports separately. Because focus
 * arriving at this pane later lands on `cur`, setting it here is also what
 * makes F6 from the tree land on the row the user was standing on. */
int apr_canvas_set_current_node(HWND canvas, size_t index);

/* Perform `op`. Returns nonzero when it was handled -- including when it was
 * handled by refusing and saying why, which is the interesting case. */
int apr_canvas_perform(HWND canvas, AprCanvasOp op);

/* Route a frame WM_COMMAND (APR_CMD_CONNECT, APR_CMD_REMOVE, APR_CMD_ADD_*)
 * to the equivalent operation. Returns nonzero when it was ours.
 *
 * The frame owns the menu and the accelerator table, so those keys never reach
 * this window as keystrokes -- they arrive as commands. This is the seam. */
int apr_canvas_command(HWND canvas, int command_id);

/* The pending end of a half-made connection, or NULL. Exposed because "half
 * done" is a state the user is IN, and a test that cannot see it cannot prove
 * Escape gets them out of it. */
HWND apr_canvas_pending_node(HWND canvas);

/* ---------------------------------------------------------------------------
 * The level range one press of + or - moves through, in TENTHS of a decibel so
 * the arithmetic stays integer everywhere except the single conversion to and
 * from linear gain.
 *
 * Public because the sentence the canvas speaks after a level change contains
 * this number, so anything asserting on that sentence -- or documenting the
 * key -- has to be able to say what one press does without knowing a second
 * copy of it.
 * ------------------------------------------------------------------------- */

#define APR_CANVAS_GAIN_MIN_DB10  (-600)
#define APR_CANVAS_GAIN_MAX_DB10  (120)
#define APR_CANVAS_GAIN_STEP_DB10 (10)

/* ---------------------------------------------------------------------------
 * The same three things, as messages
 *
 * MOVING FOCUS IS THE UI THREAD'S PRIVILEGE. SetFocus from another thread does
 * nothing, silently, so a caller that is not the window's own thread -- a test
 * driving the real code path, a future scripting surface, an automation client
 * -- cannot call the functions above directly and get the real behaviour.
 * SendMessage marshals onto the owning thread, so these three do.
 *
 * They are not a back door: they run exactly the functions above, on the
 * thread those functions require, and nothing else.
 * ------------------------------------------------------------------------- */

#define APR_CANVAS_WM_PERFORM    (WM_APP + 0x10)  /* wParam: AprCanvasOp     */
#define APR_CANVAS_WM_FOCUS_NODE (WM_APP + 0x11)  /* wParam: node index      */
#define APR_CANVAS_WM_SET_GRAPH  (WM_APP + 0x12)  /* lParam: AprGraph *      */
#define APR_CANVAS_WM_SET_CUR    (WM_APP + 0x13)  /* wParam: node index      */

/* Private, and posted to ourselves rather than acted on directly: SetFocus
 * called from inside WM_SETFOCUS is SWALLOWED -- the outer SetFocus reasserts
 * its own target as it unwinds (design 6.3). The frame does the same thing
 * with its own private message. */
#define APR_CANVAS_WM_ENTER_PANE (WM_APP + 0x14)

#endif /* APPRECORDER_UI_CANVAS_H */
