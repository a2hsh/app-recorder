/*
 * ui_tree_panel.h -- the structure panel: a real Win32 TreeView over the same
 * graph the canvas draws.
 *
 * ===========================================================================
 * THIS IS NOT AN ACCESSIBILITY FALLBACK, AND THE DESIGN SAYS SO IN WRITING
 *
 *   Design 6.1 considered "canvas for sighted users, tree for blind users" and
 *   REJECTED it (option C): separate-but-equal, and it guarantees the canvas
 *   never gets fixed. The canvas is accessible on its own terms -- real child
 *   HWNDs, real UIA names, real keyboard navigation.
 *
 *   So the tree is a SECOND REAL VIEW, and it is the better tool for three
 *   jobs that have nothing to do with sight:
 *
 *     - jumping to any node in a forty-node graph in one keystroke, including
 *       by typing the first letters of its name (a TreeView does that for
 *       free, and no canvas does);
 *     - seeing the whole session at once, the way a layers panel does;
 *     - reading the graph in a linear order, which is faster than walking
 *       edges when what you want is "what is this session recording".
 *
 *   Both views are projections of graph.h. NEITHER DERIVES FROM THE OTHER.
 *
 * ===========================================================================
 * SELECTION IS A MODEL IDENTITY, NOT A WINDOW
 *
 *   The two views cannot exchange HWNDs or HTREEITEMs -- one has neither of
 *   the other's -- and if either kept "the other view's current thing" they
 *   would be derived from each other, which is the thing design 6.1 refused.
 *
 *   So a selection is an AprTreeSel: a small value naming a MODEL object by
 *   the ids graph.h guarantees are stable for the session. The tree emits one
 *   whenever the user moves the caret (apr_tree_panel_set_selection_sink) and
 *   accepts one at any time (apr_tree_panel_select). A controller -- today the
 *   app, later a session layer -- wires the two directions together. Echoes
 *   are suppressed inside apr_tree_panel_select, so a round trip terminates
 *   without either view knowing the other exists.
 *
 *   A SELECTION CARRIES ITS BUS, and that is not redundancy. A source may feed
 *   several buses, so it appears in the tree once per bus; "the selected
 *   source" is ambiguous but "this source, on this bus" is not, and it is also
 *   exactly the identity of the EDGE the canvas draws.
 *
 * ===========================================================================
 * THE ROW LIST IS A PURE FUNCTION, WHICH IS WHY THE ORDER IS TESTABLE
 *
 *   apr_tree_panel_rows() turns a graph into the rows the panel shows, in
 *   display order, with no window involved. apr_tree_panel_label() turns one
 *   row into the text a screen reader reads, likewise.
 *
 *   That split is deliberate. "Structure is source -> bus -> action and it does
 *   NOT flip in Arabic" is then a property of a pure function that a test can
 *   assert exhaustively in one process in both languages, rather than
 *   something inferred from the pixels of a live control.
 *
 * ===========================================================================
 * THREADING
 *
 *   The panel belongs to the thread that created it. Every function here may
 *   be called from another thread EXCEPT the two pure ones, which touch no
 *   window at all: the rest marshal through SendMessageW, so they run on the
 *   owning thread and return only once it has finished. Do not call any of
 *   them from an audio callback -- they resolve catalog strings, and
 *   apr_str() locks and may allocate.
 */
#ifndef APPRECORDER_UI_TREE_PANEL_H
#define APPRECORDER_UI_TREE_PANEL_H

#include <windows.h>
#include <stddef.h>

#include "graph.h"
#include "ui_theme.h"

/* ---------------------------------------------------------------------------
 * A row
 * ------------------------------------------------------------------------- */

typedef enum AprTreeRowKind {
    APR_TREE_ROW_NONE = 0,

    /* Model rows. These are the ones a selection means something for. */
    APR_TREE_ROW_BUS,
    APR_TREE_ROW_SOURCE,
    APR_TREE_ROW_ACTION,

    /* Structural rows. Real, named, reachable -- but they name no model
     * object, so a view that receives one as a selection should ignore it
     * rather than guess. */
    APR_TREE_ROW_UNASSIGNED,  /* header over sources wired to no bus at all */
    APR_TREE_ROW_EMPTY        /* the whole-session empty state              */
} AprTreeRowKind;

typedef struct AprTreeSel {
    AprTreeRowKind kind;
    AprBusId       bus;     /* BUS/ACTION rows, and the bus a SOURCE row sits
                             * under. 0 for a source on no bus at all.      */
    AprSourceId    source;  /* SOURCE rows; 0 otherwise                     */
    size_t         action;  /* ACTION rows: index within `bus`              */
} AprTreeSel;

typedef struct AprTreeRow {
    AprTreeSel sel;
    int        depth;       /* 0 at the root of the tree                    */
} AprTreeRow;

/* Rows the panel will hold. APR_MAX_BUSES buses, each with its own header,
 * APR_MAX_SOURCES_PER_BUS sources and APR_MAX_ACTIONS_PER_BUS outputs, plus
 * the unassigned group and every source in it. Fixed, like every other
 * capacity in this codebase (graph.h): a UI that reallocates while a screen
 * reader is walking it buys nothing but a lifetime problem. */
#define APR_TREE_MAX_ROWS \
    (APR_MAX_BUSES * (1 + APR_MAX_SOURCES_PER_BUS + APR_MAX_ACTIONS_PER_BUS) \
     + APR_MAX_SOURCES + 2)

/* ---------------------------------------------------------------------------
 * The projection -- pure, no window, no globals
 * ------------------------------------------------------------------------- */

/* Every row of the panel, in display order, depth first.
 *
 * ORDER IS SOURCE -> BUS -> ACTION AND IT IS LANGUAGE-INDEPENDENT. Within a
 * bus the sources feeding it come first and the files it writes come last,
 * because that is the direction the audio travels, and a screen reader user
 * reading top to bottom is reading the signal chain. Nothing here consults
 * apr_str_is_rtl(); only the painting mirrors (design 6.2, AGENTS.md rule 6).
 *
 * Returns the number of rows, which may exceed `cap`; at most `cap` are
 * written. `out` may be NULL to count only. `g` may be NULL, which produces
 * the single empty-state row -- a panel with nothing in it must still say
 * something a screen reader can read.
 *
 * Each written row is zeroed in full first, PADDING INCLUDED, so two row lists
 * built from the same shape compare equal byte for byte. */
size_t apr_tree_panel_rows(const AprGraph *g, AprTreeRow *out, size_t cap);

/* The text of one row, in the current language.
 *
 * A TREEVIEW ITEM'S TEXT IS ITS ACCESSIBLE NAME -- there is no second string a
 * screen reader reads instead -- so this is a whole sentence and not a label:
 * what the node is, its kind, what it is wired to, and any state that changes
 * what the recording will contain. A muted application records silence and a
 * closed one keeps recording silence, and both look perfectly healthy from
 * every other angle, so both are in the row.
 *
 * Writes at most `cch` characters including the terminator, always
 * NUL-terminates when cch >= 1, and returns the characters written. */
size_t apr_tree_panel_label(const AprGraph *g, const AprTreeSel *sel,
                            wchar_t *buf, size_t cch);

/* ---------------------------------------------------------------------------
 * The panel
 *
 * apr_tree_panel_create() is declared in ui_app.h, beside apr_canvas_create(),
 * because the frame is what calls it. Everything else is here.
 * ------------------------------------------------------------------------- */

/* Point the panel at a model and rebuild. NULL detaches it and leaves the
 * empty state. The panel does NOT own the graph and never mutates it. */
void apr_tree_panel_set_graph(HWND panel, AprGraph *g);

AprGraph *apr_tree_panel_graph(HWND panel);

/* Rebuild from the model. Call after anything that changes the graph's shape.
 * Keeps the caret on the same model object where that object still exists. */
void apr_tree_panel_refresh(HWND panel);

/* ---------------------------------------------------------------------------
 * Selection -- see the header note
 * ------------------------------------------------------------------------- */

/* Nonzero if something is selected; `out` is filled either way (kind
 * APR_TREE_ROW_NONE when nothing is). */
int apr_tree_panel_get_selection(HWND panel, AprTreeSel *out);

/* Move the caret to the row naming `sel`. Returns nonzero if such a row
 * exists. Does NOT call the selection sink -- a view echoing a selection back
 * at whoever sent it is how two views deadlock or flicker. */
int apr_tree_panel_select(HWND panel, const AprTreeSel *sel);

/* Called when the USER moves the caret, never for apr_tree_panel_select. */
typedef void (*AprTreeSelFn)(HWND panel, const AprTreeSel *sel, void *user);
void apr_tree_panel_set_selection_sink(HWND panel, AprTreeSelFn fn, void *user);

/* Called when the user ACTIVATES a row -- Enter, or a double click. Never for
 * a caret move and never for apr_tree_panel_select.
 *
 * THE TWO SIGNALS ARE SEPARATE BECAUSE MOVING IS NOT CHOOSING, and conflating
 * them made this panel unbrowsable. A listener that answered every caret move
 * by moving focus meant one Down moved the caret, focus jumped to the other
 * view, and the next Down drove that view instead: every row past the first
 * was out of reach, and these rows' sentences are the panel's entire purpose.
 *
 * So: answer the SELECTION sink by keeping the other view's highlight in step
 * -- quietly, without touching focus -- and answer THIS one by going there.
 * A structural row (kind APR_TREE_ROW_UNASSIGNED / _EMPTY) names no model
 * object and never reaches this sink at all. */
void apr_tree_panel_set_activate_sink(HWND panel, AprTreeSelFn fn, void *user);

/* The TreeView itself. For tests and for anything that needs to talk to the
 * control directly; the panel keeps owning it. */
HWND apr_tree_panel_treeview(HWND panel);

#endif /* APPRECORDER_UI_TREE_PANEL_H */
