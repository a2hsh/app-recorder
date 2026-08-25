/*
 * ui_node.h -- one node on the canvas: one real Win32 child window.
 *
 * ===========================================================================
 * WHY A WINDOW AND NOT A RECTANGLE
 *
 *   Because a window is the unit the accessibility stack understands. A real
 *   HWND gets, with no code from us: an MSAA/UIA element, a place in the
 *   focus chain, a place in the tab order, hit testing, and a WM_PAINT in
 *   which we still control every pixel. A painted rectangle gets none of
 *   those and would have to have all of them written by hand, in C, against
 *   three COM interfaces (design 6.1). AGENTS.md rule 5 makes this a hard
 *   rule, not a preference.
 *
 *   The cost is one HWND per node. A recorder with 64 sources, 32 buses and
 *   their outputs is a few hundred windows -- ordinary for a Win32 dialog and
 *   nothing at all next to what it buys.
 *
 * ===========================================================================
 * WHAT A NODE SAYS
 *
 *   NAME:        who it is and what kind of thing it is -- "Teams, source".
 *                Kind is in the text because kind is otherwise carried only by
 *                colour, and colour is never the only carrier of a fact.
 *   DESCRIPTION: its EDGES, and how to walk them -- "Feeds Main Mix. Press
 *                Control and Down Arrow to move to what it feeds." An edge is
 *                painted on the canvas and has no element of its own, so if it
 *                is not in this sentence it does not exist for a screen reader
 *                user.
 *
 *   Both are built by the canvas from catalog entries with positional inserts
 *   and handed here as text, because they contain user data (a bus name, a
 *   file name) that no catalog can hold. Nothing in this file concatenates a
 *   sentence.
 *
 * ===========================================================================
 * PAINTING
 *
 *   Colours and spacing come from AprTheme and nowhere else -- no literal RGB
 *   and no literal pixel count appears in node_window.c. The node never gets
 *   WS_EX_LAYOUTRTL (it is a window we paint; see ui_app.h CONVENTION 1); it
 *   mirrors by asking apr_ui_dir() where its leading edge is and drawing text
 *   with DT_RIGHT | DT_RTLREADING when that edge is the right one.
 */
#ifndef APPRECORDER_UI_NODE_H
#define APPRECORDER_UI_NODE_H

#include <windows.h>
#include <stdint.h>

#include "ui_theme.h"

typedef enum AprNodeKind {
    APR_NODE_SOURCE = 0,
    APR_NODE_BUS    = 1,
    APR_NODE_ACTION = 2
} AprNodeKind;

/* Register the node window class once per module instance. Idempotent. */
int apr_node_register_class(HINSTANCE inst);

/* Create a node as a child of `canvas`. `model_id` is the graph's own id for
 * the source or bus (stable for the session, so a node can be found again
 * after a rebuild); `sub_id` distinguishes the outputs of one bus and is 0
 * otherwise. The node is created hidden and unpositioned -- the canvas places
 * it, because layout is the canvas's job and a node that moved itself would be
 * a second layout site. */
HWND apr_node_create(HWND canvas, AprTheme *theme, AprNodeKind kind,
                     uint32_t model_id, int sub_id, int ctrl_id);

/* Nonzero when `hwnd` is one of ours. */
int apr_node_is_node(HWND hwnd);

AprNodeKind apr_node_kind(HWND node);
uint32_t    apr_node_model_id(HWND node);
int         apr_node_sub_id(HWND node);

/* The two lines the node DRAWS. `title` is user data (a bus name, a file
 * name); `detail` is the kind word, and the level for a source. Both are
 * copied. */
void apr_node_set_text(HWND node, const wchar_t *title, const wchar_t *detail);

/* The two things the node SAYS. Both already formatted from the catalog by the
 * caller; see the header note. Setting the name also fires the MSAA
 * name-change event, because a name that changed silently is a change the user
 * did not hear. */
void apr_node_set_accessible(HWND node, const wchar_t *name, const wchar_t *desc);

/* Marks this node as the held end of a half-made connection: drawn with the
 * accent border, and something Escape can get the user out of. */
void apr_node_set_pending(HWND node, int pending);
int  apr_node_pending(HWND node);

/* Re-read the theme after a DPI or dark-mode change. */
void apr_node_set_theme(HWND node, AprTheme *theme);

/* Fire EVENT_OBJECT_FOCUS for this node without moving the OS focus. Used when
 * the node already has focus and the user asked to hear it again -- a screen
 * reader re-reads on the event, not on the SetFocus. */
void apr_node_announce_self(HWND node);

#endif /* APPRECORDER_UI_NODE_H */
