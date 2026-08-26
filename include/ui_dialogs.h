/*
 * ui_dialogs.h -- the half of the product that turns "navigate a graph" into
 * "build one".
 *
 * ===========================================================================
 * REAL DIALOGS, REAL CONTROLS. THIS IS THE WHOLE DESIGN DECISION.
 *
 *   Every dialog here is a genuine Win32 dialog created from a DLGTEMPLATE,
 *   holding genuine BUTTON, EDIT, STATIC, LISTBOX and COMBOBOX controls. Not a
 *   custom window with painted rectangles, not an owner-drawn list.
 *
 *   That is not conservatism. A standard control ships with the MSAA/UIA
 *   provider Microsoft wrote and screen readers have had decades to tune
 *   against: NVDA knows what a LISTBOX is, knows how to read one item as the
 *   caret moves, knows that an EDIT with a label before it is named by that
 *   label, knows that a dialog announces itself when it opens and restores
 *   focus when it closes. Every one of those behaviours would have to be
 *   rebuilt, badly, behind a custom window -- and each one would be a silent
 *   regression to anyone testing by eye. AGENTS.md rule 5 is the short form.
 *
 *   A LISTBOX rather than a LISTVIEW for the pickers, deliberately: a listbox
 *   item's TEXT IS ITS ACCESSIBLE NAME, so a row can be a whole sentence
 *   ("Chrome, process 8412, playing audio now") rather than four columns a
 *   reader has to be driven across.
 *
 * ===========================================================================
 * THE TEMPLATES ARE BUILT AT RUNTIME, AND THAT IS AN i18n DECISION
 *
 *   A dialog in the .rc would carry its English text in the resource, which
 *   AGENTS.md rule 6 forbids, and setting every string again in WM_INITDIALOG
 *   would mean the .rc text existed only to be overwritten -- present in the
 *   binary, wrong in every language, and easy to forget one of.
 *
 *   So the template is assembled in memory from catalog strings at the moment
 *   the dialog opens. There is exactly one copy of each string, it is in
 *   res/strings.rc with every other string, and a language change is picked up
 *   the next time the dialog opens.
 *
 *   FONT: the templates ask for DS_SHELLFONT / "MS Shell Dlg", which Windows
 *   maps to the shell's own UI font for the current script. That deliberately
 *   sidesteps design 6.2's font-coverage trap: we are not choosing a face and
 *   hoping it covers Arabic, we are asking the system for the face it already
 *   uses for dialogs.
 *
 *   DIRECTION: the dialog window gets WS_EX_LAYOUTRTL in an RTL interface and
 *   its children inherit it, which is exactly right HERE and only here -- see
 *   ui_app.h CONVENTION 1. We paint nothing inside a dialog, so the mirrored
 *   device context has nothing of ours to mirror wrongly, and the standard
 *   controls' own right-to-left behaviour is what we want.
 *
 * ===========================================================================
 * THE PURE HALF IS SEPARATE ON PURPOSE
 *
 *   The functions that turn a discovered application, an endpoint or a
 *   resolution report into the sentence a screen reader reads take no window
 *   and touch no global. tests/test_ui_dialogs.c asserts on those directly, so
 *   "the row says the application is muted" is a property a test can prove
 *   rather than something a human has to hear.
 *
 * THREADING: UI thread only. Everything here resolves catalog strings and most
 * of it enumerates the audio engine.
 */
#ifndef APPRECORDER_UI_DIALOGS_H
#define APPRECORDER_UI_DIALOGS_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "action.h"
#include "discover.h"
#include "graph.h"
#include "session.h"
#include "strings.h"
#include "ui_canvas.h"

/* ---------------------------------------------------------------------------
 * The pure half -- one row of a list, as a whole sentence
 * ------------------------------------------------------------------------- */

/* "Chrome, process 8412, playing audio now" -- name, pid and the ONE state
 * that changes what a recording will contain. Muted beats active: an
 * application that is playing but muted records silence, and that is the fact
 * worth saying (design 4.1 #5, measured). */
size_t apr_dlg_app_row(const AprAudioApp *a, wchar_t *buf, size_t cch);

/* "Chat Mic (TC-Helicon GoXLR)", or the same with ", the default capture
 * device" folded in. */
size_t apr_dlg_endpoint_row(const AprAudioEndpoint *e, wchar_t *buf, size_t cch);

/* One line of the resolve report: what the session asked for, what was used
 * instead, or why nothing was. Reuses the session catalog's sentences, which
 * already name BOTH halves -- see session.h on why "Teams could not be found"
 * is not good enough. */
size_t apr_dlg_resolution_row(const AprSessionResolution *r,
                              wchar_t *buf, size_t cch);

/* "Ctrl+Shift+E", "Delete", "Enter" -- the key a user actually presses, built
 * from the modifier names in the catalog rather than from a "+" in code. */
size_t apr_dlg_key_name(UINT vk, UINT mods, wchar_t *buf, size_t cch);

/* "Connect: Ctrl+E" -- one row of Help > Keyboard Shortcuts, from the canvas's
 * single binding table. */
size_t apr_dlg_binding_row(const AprCanvasBinding *b, wchar_t *buf, size_t cch);

/* ---------------------------------------------------------------------------
 * Adding a source
 * ------------------------------------------------------------------------- */

typedef enum AprDlgSourceKind {
    APR_DLG_SRC_APP = 0,
    APR_DLG_SRC_DEVICE,
    APR_DLG_SRC_SYSTEM_MINUS_TREE
} AprDlgSourceKind;

typedef struct AprDlgSource {
    AprDlgSourceKind kind;
    uint32_t         pid;                                 /* APP, SYSTEM     */
    wchar_t          endpoint_id[APR_DISC_ENDPOINT_CCH];  /* DEVICE          */
    wchar_t          name[APR_NAME_CCH];                  /* what to call it */
} AprDlgSource;

/* Choose an application currently rendering audio, or a capture endpoint.
 * Returns nonzero when the user accepted; `out` is untouched otherwise.
 *
 * The EXCLUDE option is on this dialog but is NOT reachable by accident: it
 * puts a second, separate confirmation in front of the user that says what it
 * records and enumerates the target's process tree, and it will not proceed
 * without an explicit checkbox (design 4.1.1). */
int apr_dlg_add_source(HWND owner, AprDlgSource *out);

/* ---------------------------------------------------------------------------
 * Buses
 * ------------------------------------------------------------------------- */

/* One named-text prompt, used for "add a bus" and "rename this bus". `title`
 * and `label` are catalog ids; `name` is in/out and is pre-filled. */
int apr_dlg_name_prompt(HWND owner, AprStrId title, AprStrId label,
                        wchar_t *name, size_t cch);

/* ---------------------------------------------------------------------------
 * Adding an output
 * ------------------------------------------------------------------------- */

typedef struct AprDlgOutput {
    AprBusId bus;
    char     action_id[16];                  /* registry id, never guessed  */
    wchar_t  path[APR_DISC_PATH_CCH];
    int      bitrate_kbps;
    int      quality;
} AprDlgOutput;

/* THE FORMAT LIST COMES FROM THE REGISTRY, never from a list in this file.
 * A format this build cannot write must not be offerable, and a format added
 * to src/actions must appear here with no edit -- which is the same contract
 * the CLI's --format already has. */
int apr_dlg_add_output(HWND owner, const AprGraph *g, AprBusId prefer,
                       AprDlgOutput *out);

/* Which of a bus's outputs to remove. Returns nonzero with `*out_index` set. */
int apr_dlg_pick_output(HWND owner, const AprGraph *g, AprBusId bus,
                        size_t *out_index);

/* ---------------------------------------------------------------------------
 * Files
 * ------------------------------------------------------------------------- */

int apr_dlg_choose_session(HWND owner, int for_saving, wchar_t *path, size_t cch);

/* ---------------------------------------------------------------------------
 * Saying things
 * ------------------------------------------------------------------------- */

/* A question with two named answers. NOT MessageBox: the buttons are named
 * from the catalog with the actual verbs ("Stop recording and close"), because
 * "Yes" and "No" against a sentence a reader has already moved past is exactly
 * the case where a screen reader user has to go back and re-read. */
int apr_dlg_confirm(HWND owner, AprStrId title, const wchar_t *question,
                    AprStrId accept_label, AprStrId reject_label);

/* Three answers, for the close-while-recording case: stop and close, close to
 * the notification area and keep recording, or do nothing. Returns
 * APR_DLG_CLOSE_*. */
#define APR_DLG_CLOSE_CANCEL   0
#define APR_DLG_CLOSE_STOP     1
#define APR_DLG_CLOSE_TO_TRAY  2
int apr_dlg_close_while_recording(HWND owner);

/* A statement with one button. */
void apr_dlg_say(HWND owner, AprStrId title, const wchar_t *text);

/* Help > Keyboard Shortcuts, rendered from the canvas binding table so a
 * shortcut cannot be documented as one key and implemented as another. */
void apr_dlg_keyboard_help(HWND owner);

void apr_dlg_about(HWND owner);

/* The resolve report, in full: every source, what was asked for, what was
 * substituted, and the rival candidates by pid. Returns nonzero if the user
 * chose to load it anyway. `needs_consent` adds the EXCLUDE confirmation. */
int apr_dlg_resolve_report(HWND owner, const AprSessionResolveReport *rep);

#endif /* APPRECORDER_UI_DIALOGS_H */
