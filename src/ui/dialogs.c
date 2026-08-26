/*
 * dialogs.c -- real Win32 dialogs, built from the string catalog at runtime.
 *
 * See ui_dialogs.h for why they are real dialogs and why the templates are
 * assembled in memory instead of living in the .rc. What follows is the part
 * that is easy to get wrong.
 *
 * ---------------------------------------------------------------------------
 * THE TEMPLATE FORMAT IS UNFORGIVING ABOUT ALIGNMENT
 *
 *   A DLGTEMPLATE is a header, then three variable-length strings, then one
 *   DLGITEMTEMPLATE per control -- and EVERY DLGITEMTEMPLATE must start on a
 *   DWORD boundary. Get that wrong and CreateDialogIndirect does not fail with
 *   an error; it reads the next control's style word out of the middle of the
 *   previous control's caption and produces a dialog full of garbage, or
 *   nothing at all, with GetLastError saying zero. dt_align() is called before
 *   every item and before the creation-data word for exactly that reason.
 *
 * ---------------------------------------------------------------------------
 * ORDER IN THE TEMPLATE IS TAB ORDER IS READING ORDER
 *
 *   There is no separate tab-order table: the dialog manager walks controls in
 *   template order. So the order the items are added below is the order a
 *   screen reader user meets them, and it is always label-then-control --
 *   which is also how the dialog manager decides an EDIT's accessible name.
 *   A STATIC placed immediately BEFORE an EDIT in the template becomes that
 *   EDIT's label. Placing it after, or between the edit and its label, leaves
 *   the edit nameless and a reader says "edit" and nothing else.
 *
 * ---------------------------------------------------------------------------
 * WHY THERE IS NO MessageBox ANYWHERE IN THIS FILE
 *
 *   MessageBox's buttons are "Yes"/"No"/"OK". A screen reader user arriving at
 *   a button has usually moved past the sentence that gave those words their
 *   meaning, so answering means navigating back to re-read the body. Naming
 *   the buttons with the actual verb -- "Stop recording and close" -- makes the
 *   button self-describing, which is the whole reason confirm() here builds a
 *   dialog rather than calling the one the OS provides.
 */
#include "ui_dialogs.h"

#include <commctrl.h>
#include <commdlg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "log.h"
#include "outpath.h"
#include "ui_app.h"

#pragma comment(lib, "comdlg32.lib")

/* Windows' predefined control-class atoms, as they appear in a template. */
#define ATOM_BUTTON   0x0080
#define ATOM_EDIT     0x0081
#define ATOM_STATIC   0x0082
#define ATOM_LISTBOX  0x0083
#define ATOM_COMBOBOX 0x0085

#define IDC_LIST      2001
#define IDC_LIST_LBL  2002
#define IDC_NAME      2003
#define IDC_NAME_LBL  2004
#define IDC_KIND_APP  2005
#define IDC_KIND_DEV  2006
#define IDC_KIND_SYS  2007
#define IDC_REFRESH   2008
#define IDC_FORMAT    2009
#define IDC_PATH      2010
#define IDC_BROWSE    2011
#define IDC_BITRATE   2012
#define IDC_QUALITY   2013
#define IDC_BUS       2014
#define IDC_BODY      2015
#define IDC_CONSENT   2016
#define IDC_TREE      2017
#define IDC_THIRD     2018
#define IDC_SUMMARY   2019

#define DLG_TEXT_CCH  1024

/* ==========================================================================
 * Template builder
 * ======================================================================== */

typedef struct DlgBuf {
    unsigned char *p;
    size_t         used;
    size_t         cap;
    size_t         count_off;   /* where the item count word lives */
    WORD           items;
    int            overflow;
} DlgBuf;

static void dt_raw(DlgBuf *b, const void *src, size_t n)
{
    if (b->overflow || b->used + n > b->cap) { b->overflow = 1; return; }
    memcpy(b->p + b->used, src, n);
    b->used += n;
}

static void dt_w(DlgBuf *b, WORD v)  { dt_raw(b, &v, sizeof v); }
static void dt_dw(DlgBuf *b, DWORD v) { dt_raw(b, &v, sizeof v); }
static void dt_s(DlgBuf *b, short v)  { dt_raw(b, &v, sizeof v); }

static void dt_sz(DlgBuf *b, const wchar_t *s)
{
    size_t n = s ? wcslen(s) : 0;
    if (s && n) dt_raw(b, s, n * sizeof(wchar_t));
    dt_w(b, 0);
}

/* Every DLGITEMTEMPLATE starts on a DWORD boundary. See the file header. */
static void dt_align(DlgBuf *b)
{
    while (b->used & 3u) dt_w(b, 0);
}

static void dt_begin(DlgBuf *b, unsigned char *storage, size_t cap,
                     DWORD style, DWORD ex, short cx, short cy,
                     const wchar_t *title)
{
    memset(b, 0, sizeof *b);
    b->p = storage;
    b->cap = cap;

    if (apr_ui_dir() == APR_DIR_RTL) ex |= WS_EX_LAYOUTRTL;

    dt_dw(b, style | DS_SETFONT | DS_MODALFRAME | DS_FIXEDSYS |
             WS_POPUP | WS_CAPTION | WS_SYSMENU);
    dt_dw(b, ex);
    b->count_off = b->used;
    dt_w(b, 0);            /* item count, patched in dt_end */
    dt_s(b, 0);            /* x, y -- DS_CENTER places it */
    dt_s(b, 0);
    dt_s(b, cx);
    dt_s(b, cy);
    dt_w(b, 0);            /* no menu */
    dt_w(b, 0);            /* default dialog class */
    dt_sz(b, title ? title : L"");
    /* DS_SETFONT + DS_FIXEDSYS is DS_SHELLFONT: Windows maps "MS Shell Dlg"
     * to the shell's real UI font for the current script, so we never pick a
     * face and hope it covers Arabic (design 6.2's font trap). */
    dt_w(b, 9);
    dt_sz(b, L"MS Shell Dlg");
}

static void dt_item(DlgBuf *b, DWORD style, DWORD ex, short x, short y,
                    short cx, short cy, WORD id, WORD atom,
                    const wchar_t *text)
{
    dt_align(b);
    dt_dw(b, style | WS_CHILD | WS_VISIBLE);
    dt_dw(b, ex);
    dt_s(b, x);
    dt_s(b, y);
    dt_s(b, cx);
    dt_s(b, cy);
    dt_w(b, id);
    dt_w(b, 0xFFFF);
    dt_w(b, atom);
    dt_sz(b, text ? text : L"");
    /* The creation-data WORD follows the title IMMEDIATELY. It must not be
     * preceded by padding: Windows reads the very next word after the title as
     * the creation-data count, so a pad here is read as that count, leaving the
     * parser two bytes short and treating our creation word as the next item's
     * style DWORD. The template is then garbage and DialogBoxIndirectParam
     * fails outright.
     *
     * This was a real bug, and a nasty one: whether it fired depended on
     * whether a title happened to end DWORD-aligned, so it worked for some
     * strings and not others -- which makes it LANGUAGE-DEPENDENT. It would
     * have broken differently in Arabic.
     *
     * Alignment for the next item belongs after this word, and dt_item's own
     * leading dt_align() already does it. */
    dt_w(b, 0);            /* no creation data */
    b->items++;
}

/* "The last dialog was REJECTED, not cancelled." Set by dt_end and dlg_run --
 * the two places a dialog can fail to appear -- and read by whoever has to
 * tell the user. UI-thread only, like everything else in this file. */
static int g_dlg_failed;

/* Test seam, the same shape as action_wav.c's write gate: force the failure
 * that is otherwise only reachable by corrupting a template. Without it, "the
 * user is told when a window cannot open" is a property only a bug can
 * demonstrate. */
static int g_dlg_fail_next;

static const DLGTEMPLATE *dt_end(DlgBuf *b)
{
    /* Cleared here rather than in dlg_run, because dt_end is the FIRST of the
     * two failure points and the one whose failure means dlg_run never runs. */
    g_dlg_failed = 0;

    if (b->overflow) {
        APR_WARN(L"dialog template overflowed its buffer; no dialog was built");
        g_dlg_failed = 1;
        return NULL;
    }
    memcpy(b->p + b->count_off, &b->items, sizeof b->items);
    return (const DLGTEMPLATE *)b->p;
}

/* DialogBoxIndirectParamW returns -1 when the dialog could not be created at
 * all. Every caller here compares the result against IDOK, which quietly folds
 * "the template was rejected" into "the user pressed Cancel" -- and that is
 * exactly how a malformed template reaches a user as SILENCE: no window, no
 * message, nothing to hear. It is how the creation-data alignment bug in
 * dt_item() stayed invisible.
 *
 * The wrapper keeps each caller's contract and makes the failure loud. */
void apr_dlg_test_fail_next(int on) { g_dlg_fail_next = on ? 1 : 0; }

int apr_dlg_last_failed(void) { return g_dlg_failed; }

static INT_PTR dlg_run(const DLGTEMPLATE *t, HWND owner, DLGPROC proc,
                       LPARAM param)
{
    INT_PTR r;

    if (g_dlg_fail_next) {
        APR_WARN(L"dialog: apr_dlg_test_fail_next is set; refusing to create");
        g_dlg_failed = 1;
        return -1;
    }

    SetLastError(0);
    r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), t, owner, proc, param);
    if (r == -1) {
        APR_WARN(L"dialog could not be created (GetLastError=%lu); the template "
                 L"was rejected -- this is a bug, not a cancelled dialog",
                 (unsigned long)GetLastError());
        /* AND THE LOG IS NOT A CHANNEL TO THE USER. This failure used to be
         * loud in a file nobody has open and silent everywhere a person is
         * standing: the key did nothing, no window appeared, nothing was said.
         * The caller reads this and announces it. */
        g_dlg_failed = 1;
    }
    return r;
}

/* ==========================================================================
 * Small shared helpers
 * ======================================================================== */

static const wchar_t *num_of(int64_t v, wchar_t *buf, size_t cch)
{
    apr_str_number(v, buf, cch);
    return buf;
}

/* A listbox item's text IS its accessible name, so every list here is filled
 * with whole sentences and this is the only place that adds one. */
static void lb_add(HWND lb, const wchar_t *text, LPARAM data)
{
    LRESULT i = SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)text);
    if (i >= 0) SendMessageW(lb, LB_SETITEMDATA, (WPARAM)i, data);
}

static int lb_selected_data(HWND lb, LPARAM *out)
{
    LRESULT sel = SendMessageW(lb, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return 0;
    if (out) *out = (LPARAM)SendMessageW(lb, LB_GETITEMDATA, (WPARAM)sel, 0);
    return 1;
}

/* ==========================================================================
 * The pure half -- rows, as whole sentences
 * ======================================================================== */

size_t apr_dlg_app_row(const AprAudioApp *a, wchar_t *buf, size_t cch)
{
    const wchar_t *args[3];
    wchar_t pid[32];

    if (!buf || cch == 0) return 0;
    buf[0] = 0;
    if (!a) return 0;

    args[0] = a->display[0] ? a->display : a->exe;
    args[1] = num_of((int64_t)a->pid, pid, 32);
    /* MUTED BEATS ACTIVE. An application that is playing and muted records
     * pure silence, and that is the fact that changes what the file contains
     * (design 4.1 #5, measured). Saying "playing audio now" about it would be
     * true and useless. */
    args[2] = apr_str(a->muted  ? APR_S_UI_DLG_APP_STATE_MUTED
                    : a->active ? APR_S_UI_DLG_APP_STATE_ACTIVE
                                : APR_S_UI_DLG_APP_STATE_IDLE);
    return apr_str_format(APR_S_UI_DLG_APP_ROW, buf, cch, args, 3);
}

size_t apr_dlg_endpoint_row(const AprAudioEndpoint *e, wchar_t *buf, size_t cch)
{
    const wchar_t *args[1];

    if (!buf || cch == 0) return 0;
    buf[0] = 0;
    if (!e) return 0;

    args[0] = e->name;
    return apr_str_format(e->is_default ? APR_S_UI_DLG_DEVICE_ROW_DEFAULT
                                        : APR_S_UI_DLG_DEVICE_ROW,
                          buf, cch, args, 1);
}

size_t apr_dlg_resolution_row(const AprSessionResolution *r,
                              wchar_t *buf, size_t cch)
{
    const wchar_t *args[3];
    wchar_t        pid[32];
    AprStrId       id;
    size_t         n;

    if (!buf || cch == 0) return 0;
    buf[0] = 0;
    if (!r) return 0;

    /* The session catalog already owns these sentences, and every one of them
     * names BOTH halves -- see session.h on why "Teams could not be found" is
     * not a sentence a person can act on. Reusing them is also what stops the
     * UI and the CLI describing the same outcome two different ways.
     *
     * THE ARGUMENT COUNT IS PER SENTENCE AND IS NOT NEGOTIABLE. strings.h:
     * FORMAT_MESSAGE_ARGUMENT_ARRAY indexes the array directly and the API
     * carries no count, so a sentence with %3!s! against a two-element array
     * reads past the end and FormatMessageW cannot detect it. The string layer
     * refuses rather than doing that -- which turns the bug into a visible
     * placeholder instead of a crash -- but the fix is to pass the right
     * number here, so each case states its own.
     *
     * Every number goes through apr_str_number() first (AGENTS.md rule 6). */
    args[0] = r->name;
    args[1] = L"";
    args[2] = L"";

    switch (r->status) {
    case APR_SESSION_MOVED:
        /* "%1 is now running from %2, not %3 where the session recorded it." */
        id = APR_S_WARN_SESSION_MOVED;
        args[1] = r->substituted;
        args[2] = r->wanted;
        n = 3;
        break;

    case APR_SESSION_BY_WINDOW_CLASS:
        id = APR_S_WARN_SESSION_BY_WINDOW_CLASS;
        args[1] = num_of((int64_t)r->chosen_pid, pid, 32);
        n = 2;
        break;

    case APR_SESSION_FIRST_OF_MANY:
        id = APR_S_WARN_SESSION_FIRST_OF_MANY;
        args[1] = num_of((int64_t)r->chosen_pid, pid, 32);
        n = 2;
        break;

    case APR_SESSION_DEVICE_BY_NAME:
        id = APR_S_WARN_SESSION_DEVICE_BY_NAME;
        n = 1;
        break;

    case APR_SESSION_NOT_RUNNING:
        id = APR_S_ERR_SESSION_SOURCE_MISSING;
        args[1] = r->wanted;
        n = 2;
        break;

    case APR_SESSION_DEVICE_ABSENT:
        id = APR_S_ERR_SESSION_DEVICE_MISSING;
        n = 1;
        break;

    case APR_SESSION_AMBIGUOUS:
        id = APR_S_ERR_SESSION_AMBIGUOUS;
        n = 1;
        break;

    case APR_SESSION_NEEDS_CONSENT:
        /* Deliberately NOT the command line's version of this sentence, which
         * tells the reader to add --allow-system-capture. There is no command
         * line here; the confirmation is a dialog. */
        id = APR_S_UI_DLG_RESOLVE_CONSENT;
        args[1] = r->wanted[0] ? r->wanted : r->name;
        n = 2;
        break;

    default:
        /* Every source gets a row, including the ones that were found exactly
         * as asked. A report that listed only problems would leave a screen
         * reader user counting to work out which sources were fine. */
        id = APR_S_UI_DLG_RESOLVE_OK;
        n = 1;
        break;
    }

    return apr_str_format(id, buf, cch, args, n);
}

/* The names of keys that are not a printable character.
 *
 * THESE ARE CATALOG ENTRIES NOW, and the reasoning that kept them as literals
 * does not survive contact with rule 6. The old comment argued that "a
 * translated Delete that no keyboard sends is worse than an English one that
 * matches the legend on the key" -- which may well be the right ANSWER, but it
 * is a translation decision, and freezing it in C is what leaves the Help
 * window reading half in one language and half in another while the modifiers
 * beside it (Ctrl, Shift, Alt) translate perfectly. A translator who agrees
 * with the old argument writes "Delete" in the Arabic block and gets exactly
 * the old behaviour; nobody else has to.
 *
 * A LETTER OR A DIGIT STAYS A CHARACTER. 'E' is not a word and has nothing to
 * translate, so it is rendered, not looked up. */
static const wchar_t *vk_name(UINT vk, wchar_t *scratch, size_t cch)
{
    AprStrId id = APR_S__NONE;

    switch (vk) {
    case VK_TAB:        id = APR_S_UI_KEYNAME_TAB;       break;
    case VK_RETURN:     id = APR_S_UI_KEYNAME_ENTER;     break;
    case VK_SPACE:      id = APR_S_UI_KEYNAME_SPACE;     break;
    case VK_DELETE:     id = APR_S_UI_KEYNAME_DELETE;    break;
    case VK_ESCAPE:     id = APR_S_UI_KEYNAME_ESCAPE;    break;
    case VK_HOME:       id = APR_S_UI_KEYNAME_HOME;      break;
    case VK_END:        id = APR_S_UI_KEYNAME_END;       break;
    case VK_LEFT:       id = APR_S_UI_KEYNAME_LEFT;      break;
    case VK_RIGHT:      id = APR_S_UI_KEYNAME_RIGHT;     break;
    case VK_UP:         id = APR_S_UI_KEYNAME_UP;        break;
    case VK_DOWN:       id = APR_S_UI_KEYNAME_DOWN;      break;
    case VK_OEM_PLUS:   id = APR_S_UI_KEYNAME_PLUS;      break;
    case VK_OEM_MINUS:  id = APR_S_UI_KEYNAME_MINUS;     break;
    case VK_ADD:        id = APR_S_UI_KEYNAME_NUM_PLUS;  break;
    case VK_SUBTRACT:   id = APR_S_UI_KEYNAME_NUM_MINUS; break;
    case VK_OEM_PERIOD: id = APR_S_UI_KEYNAME_PERIOD;    break;
    default: break;
    }
    if (id != APR_S__NONE) return apr_str(id);

    if (vk >= VK_F1 && vk <= VK_F24) {
        /* "F%1!s!" -- one catalog format with the number as an insert, and the
         * number through apr_str_number() like every other digit. */
        const wchar_t *args[1];
        wchar_t n[24];
        apr_str_number((int64_t)(vk - VK_F1 + 1u), n, 24);
        args[0] = n;
        apr_str_format(APR_S_UI_KEYNAME_FUNCTION, scratch, cch, args, 1);
        return scratch;
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        scratch[0] = (wchar_t)vk;
        scratch[1] = 0;
        return scratch;
    }
    _snwprintf_s(scratch, cch, _TRUNCATE, L"0x%02X", vk);
    return scratch;
}

size_t apr_dlg_key_name(UINT vk, UINT mods, wchar_t *buf, size_t cch)
{
    wchar_t scratch[32];
    wchar_t acc[128];
    const wchar_t *args[2];

    if (!buf || cch == 0) return 0;
    buf[0] = 0;

    lstrcpynW(acc, vk_name(vk, scratch, 32), 128);

    /* Folded outermost-modifier-last so the result reads Ctrl+Shift+E. Each
     * step is one catalog format with two inserts, which is the only sanctioned
     * way to join anything in this product -- a "+" written in C here would be
     * a "+" in every language, and the modifier order in some locales is not
     * ours to fix from inside a string literal. */
    if (mods & APR_KMOD_SHIFT) {
        args[0] = apr_str(APR_S_UI_DLG_KEY_SHIFT);
        args[1] = acc;
        apr_str_format(APR_S_UI_DLG_KEY_COMBO, buf, cch, args, 2);
        lstrcpynW(acc, buf, 128);
    }
    if (mods & APR_KMOD_ALT) {
        args[0] = apr_str(APR_S_UI_DLG_KEY_ALT);
        args[1] = acc;
        apr_str_format(APR_S_UI_DLG_KEY_COMBO, buf, cch, args, 2);
        lstrcpynW(acc, buf, 128);
    }
    if (mods & APR_KMOD_CTRL) {
        args[0] = apr_str(APR_S_UI_DLG_KEY_CTRL);
        args[1] = acc;
        apr_str_format(APR_S_UI_DLG_KEY_COMBO, buf, cch, args, 2);
        lstrcpynW(acc, buf, 128);
    }

    lstrcpynW(buf, acc, (int)cch);
    return wcslen(buf);
}

size_t apr_dlg_binding_row(const AprCanvasBinding *b, wchar_t *buf, size_t cch)
{
    wchar_t key[128];
    const wchar_t *args[2];

    if (!buf || cch == 0) return 0;
    buf[0] = 0;
    if (!b) return 0;

    apr_dlg_key_name(b->vk, b->mods, key, 128);
    args[0] = apr_str(b->label);
    args[1] = key;
    return apr_str_format(APR_S_UI_DLG_KEYS_ROW, buf, cch, args, 2);
}

/* ==========================================================================
 * A dialog that is one message and one or three buttons
 * ======================================================================== */

typedef struct SayState {
    const wchar_t *body;
    AprStrId       title;
    AprStrId       accept;
    AprStrId       reject;
    AprStrId       third;
    int            answer;
} SayState;

static INT_PTR CALLBACK say_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    SayState *st = (SayState *)GetWindowLongPtrW(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)lp);
        st = (SayState *)lp;
        if (st) SetDlgItemTextW(dlg, IDC_BODY, st->body);
        /* Focus the SAFE answer, not the destructive one. A dialog that opens
         * with "Stop recording" under the fingers is one Enter away from
         * ending a session the user meant to keep. */
        {
            HWND safe = GetDlgItem(dlg, IDCANCEL);
            if (safe) SetFocus(safe);
        }
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:      if (st) st->answer = 1; EndDialog(dlg, 1); return TRUE;
        case IDC_THIRD: if (st) st->answer = 2; EndDialog(dlg, 2); return TRUE;
        case IDCANCEL:  if (st) st->answer = 0; EndDialog(dlg, 0); return TRUE;
        default: break;
        }
        break;

    default:
        break;
    }
    return FALSE;
}

short apr_dlg_button_width(const wchar_t *label)
{
    size_t n = label ? wcslen(label) : 0;
    long w;

    /* One average character of the dialog font is one horizontal BASE UNIT,
     * and a dialog unit is a quarter of that -- so four dlu per character is
     * the average and five is the honest allowance for a face whose glyphs are
     * wider than the average, which is every Arabic face at this size. Plus
     * the platform's own padding either side of the caption.
     *
     * NEVER SIZE A CONTROL TO FIT ITS ENGLISH STRING (AGENTS.md rule 6). The
     * fixed 96 units this replaces clipped "Stop the recording and close" in
     * ENGLISH; the Arabic for it would not have had a chance. */
    w = (long)(n * 5u) + 16;
    if (w < APR_DLG_BUTTON_MIN_DU) w = APR_DLG_BUTTON_MIN_DU;
    if (w > 400) w = 400;
    return (short)w;
}

static int say_dialog(HWND owner, AprStrId title, const wchar_t *body,
                      AprStrId accept, AprStrId reject, AprStrId third)
{
    unsigned char storage[4096];
    DlgBuf b;
    SayState st;
    const DLGTEMPLATE *t;
    const wchar_t *lbl[3];
    short bw[3];
    short y = 8;
    short n = 0, i, bx, total = 0, width;

    memset(&st, 0, sizeof st);
    st.body = body ? body : L"";
    st.title = title;
    st.accept = accept;
    st.reject = reject;
    st.third = third;

    /* MEASURE FIRST, THEN LAY OUT. Each button gets the width its OWN caption
     * needs and the dialog grows to hold them, rather than every caption being
     * cut to one number chosen against the shortest English word. */
    if (accept) lbl[n++] = apr_str(accept);
    if (third)  lbl[n++] = apr_str(third);
    lbl[n++] = apr_str(reject);
    for (i = 0; i < n; ++i) {
        bw[i] = apr_dlg_button_width(lbl[i]);
        total = (short)(total + bw[i] + 6);
    }

    width = 320;
    if (total + 16 > width) width = (short)(total + 16);

    dt_begin(&b, storage, sizeof storage, DS_CENTER, 0, width, 108,
             apr_str(title));

    /* A multi-line STATIC: Arabic needs more vertical room than English at the
     * same point size, so this is sized for the taller case rather than for
     * the string in front of us (AGENTS.md rule 6). */
    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, y, (short)(width - 16), 48,
            IDC_BODY, ATOM_STATIC, L"");
    y = 64;

    bx = (short)(width - 4 - total);
    i = 0;
    if (accept) {
        dt_item(&b, BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, bx, y,
                bw[i], 18, IDOK, ATOM_BUTTON, lbl[i]);
        bx = (short)(bx + bw[i] + 6);
        i++;
    }
    if (third) {
        dt_item(&b, BS_PUSHBUTTON | WS_TABSTOP, 0, bx, y, bw[i], 18,
                IDC_THIRD, ATOM_BUTTON, lbl[i]);
        bx = (short)(bx + bw[i] + 6);
        i++;
    }
    dt_item(&b, BS_PUSHBUTTON | WS_TABSTOP, 0, bx, y, bw[i], 18,
            IDCANCEL, ATOM_BUTTON, lbl[i]);

    t = dt_end(&b);
    if (!t) return 0;

    return (int)dlg_run(t, owner, say_proc, (LPARAM)&st);
}

void apr_dlg_say(HWND owner, AprStrId title, const wchar_t *text)
{
    (void)say_dialog(owner, title, text, 0, APR_S_UI_DLG_CLOSE, 0);
}

int apr_dlg_confirm(HWND owner, AprStrId title, const wchar_t *question,
                    AprStrId accept_label, AprStrId reject_label)
{
    return say_dialog(owner, title, question, accept_label, reject_label, 0) == 1;
}

int apr_dlg_close_while_recording(HWND owner)
{
    int r = say_dialog(owner, APR_S_UI_CLOSE_TITLE, apr_str(APR_S_UI_CLOSE_BODY),
                       APR_S_UI_CLOSE_STOP_AND_EXIT, APR_S_UI_CLOSE_KEEP_RECORDING,
                       APR_S_UI_CLOSE_TO_TRAY);
    if (r == 1) return APR_DLG_CLOSE_STOP;
    if (r == 2) return APR_DLG_CLOSE_TO_TRAY;
    return APR_DLG_CLOSE_CANCEL;
}

/* ==========================================================================
 * A dialog that is a label, a list and OK/Cancel
 * ======================================================================== */

typedef void (*FillFn)(HWND lb, void *user);

typedef struct ListState {
    AprStrId  title;
    AprStrId  list_label;
    FillFn    fill;
    void     *user;
    LPARAM    picked;
    int       have;
    AprStrId  empty_msg;
} ListState;

static INT_PTR CALLBACK list_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    ListState *st = (ListState *)GetWindowLongPtrW(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)lp);
        st = (ListState *)lp;
        if (st) {
            HWND lb = GetDlgItem(dlg, IDC_LIST);
            apr_ui_set_accessible_name(lb, st->list_label);
            st->fill(lb, st->user);
            SendMessageW(lb, LB_SETCURSEL, 0, 0);
            SetFocus(lb);
        }
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_LIST:
            /* Double-click accepts, exactly as the keyboard's Enter does, so
             * there is one code path and not a mouse-only shortcut. */
            if (HIWORD(wp) == LBN_DBLCLK) {
                PostMessageW(dlg, WM_COMMAND, IDOK, 0);
                return TRUE;
            }
            break;
        case IDOK:
            if (st && lb_selected_data(GetDlgItem(dlg, IDC_LIST), &st->picked)) {
                st->have = 1;
                EndDialog(dlg, 1);
            } else {
                apr_dlg_say(dlg, APR_S_UI_DLG_REMOVE_TITLE,
                            apr_str(APR_S_UI_DLG_PICK_NEEDED));
            }
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        default:
            break;
        }
        break;

    default:
        break;
    }
    return FALSE;
}

static int list_dialog(HWND owner, ListState *st)
{
    unsigned char storage[4096];
    DlgBuf b;
    const DLGTEMPLATE *t;

    dt_begin(&b, storage, sizeof storage, DS_CENTER, 0, 320, 190,
             apr_str(st->title));

    /* Label first, then the control it labels: that order is what makes the
     * dialog manager treat the STATIC as the LISTBOX's name. */
    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 8, 304, 18, IDC_LIST_LBL,
            ATOM_STATIC, apr_str(st->list_label));
    dt_item(&b, LBS_NOTIFY | LBS_HASSTRINGS | WS_BORDER | WS_VSCROLL |
                WS_TABSTOP | WS_GROUP,
            0, 8, 28, 304, 116, IDC_LIST, ATOM_LISTBOX, L"");
    dt_item(&b, BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, 118, 156, 92, 18,
            IDOK, ATOM_BUTTON, apr_str(APR_S_UI_DLG_OK));
    dt_item(&b, BS_PUSHBUTTON | WS_TABSTOP, 0, 218, 156, 92, 18,
            IDCANCEL, ATOM_BUTTON, apr_str(APR_S_UI_DLG_CANCEL));

    t = dt_end(&b);
    if (!t) return 0;
    return (int)dlg_run(t, owner,
                                        list_proc, (LPARAM)st) == 1;
}

/* ==========================================================================
 * Adding a source
 * ======================================================================== */

#define DLG_MAX_APPS 128
#define DLG_MAX_DEVS 64

typedef struct AddSourceState {
    AprAudioApp      apps[DLG_MAX_APPS];
    size_t           app_count;
    AprAudioEndpoint devs[DLG_MAX_DEVS];
    size_t           dev_count;
    AprDlgSource     result;
    int              have;
} AddSourceState;

static void refresh_lists(AddSourceState *st)
{
    AprErr e;

    st->app_count = 0;
    st->dev_count = 0;

    e = apr_enum_audio_apps(st->apps, DLG_MAX_APPS, &st->app_count);
    if (apr_failed(&e)) { APR_LOG_ERR(APR_LOG_WARN, &e); st->app_count = 0; }
    if (st->app_count > DLG_MAX_APPS) st->app_count = DLG_MAX_APPS;

    e = apr_enum_capture_endpoints(st->devs, DLG_MAX_DEVS, &st->dev_count);
    if (apr_failed(&e)) { APR_LOG_ERR(APR_LOG_WARN, &e); st->dev_count = 0; }
    if (st->dev_count > DLG_MAX_DEVS) st->dev_count = DLG_MAX_DEVS;
}

static int selected_kind(HWND dlg)
{
    if (IsDlgButtonChecked(dlg, IDC_KIND_DEV) == BST_CHECKED) return APR_DLG_SRC_DEVICE;
    if (IsDlgButtonChecked(dlg, IDC_KIND_SYS) == BST_CHECKED) return APR_DLG_SRC_SYSTEM_MINUS_TREE;
    return APR_DLG_SRC_APP;
}

static void fill_source_list(HWND dlg, AddSourceState *st)
{
    HWND     lb   = GetDlgItem(dlg, IDC_LIST);
    int      kind = selected_kind(dlg);
    AprStrId label;
    wchar_t  row[DLG_TEXT_CCH];
    size_t   i;

    SendMessageW(lb, LB_RESETCONTENT, 0, 0);

    label = (kind == APR_DLG_SRC_DEVICE) ? APR_S_UI_DLG_DEVICE_LIST
          : (kind == APR_DLG_SRC_SYSTEM_MINUS_TREE) ? APR_S_UI_DLG_SYSTEM_TARGET
          : APR_S_UI_DLG_APP_LIST;

    SetDlgItemTextW(dlg, IDC_LIST_LBL, apr_str(label));
    /* The label changed, so the control's NAME changed. A screen reader reads
     * the name it was given, not the static beside it, so it has to be
     * re-applied or the list keeps announcing itself as the previous kind. */
    apr_ui_set_accessible_name(lb, label);

    if (kind == APR_DLG_SRC_DEVICE) {
        for (i = 0; i < st->dev_count; i++) {
            apr_dlg_endpoint_row(&st->devs[i], row, DLG_TEXT_CCH);
            lb_add(lb, row, (LPARAM)i);
        }
        if (st->dev_count == 0) lb_add(lb, apr_str(APR_S_UI_DLG_NO_DEVICES), -1);
    } else {
        for (i = 0; i < st->app_count; i++) {
            apr_dlg_app_row(&st->apps[i], row, DLG_TEXT_CCH);
            lb_add(lb, row, (LPARAM)i);
        }
        if (st->app_count == 0) lb_add(lb, apr_str(APR_S_UI_DLG_NO_APPS), -1);
    }
    SendMessageW(lb, LB_SETCURSEL, 0, 0);
}

/* Pre-fill the name box from whatever is selected, so the common case needs no
 * typing at all -- and so the name a screen reader reads back is the one the
 * user just chose rather than an empty box. */
static void sync_name(HWND dlg, AddSourceState *st)
{
    LPARAM  idx;
    int     kind = selected_kind(dlg);
    wchar_t name[APR_NAME_CCH];

    if (!lb_selected_data(GetDlgItem(dlg, IDC_LIST), &idx) || idx < 0) return;
    name[0] = 0;

    if (kind == APR_DLG_SRC_DEVICE) {
        if ((size_t)idx < st->dev_count)
            lstrcpynW(name, st->devs[idx].name, APR_NAME_CCH);
    } else if ((size_t)idx < st->app_count) {
        const AprAudioApp *a = &st->apps[idx];
        lstrcpynW(name, a->display[0] ? a->display : a->exe, APR_NAME_CCH);
    }
    if (name[0]) SetDlgItemTextW(dlg, IDC_NAME, name);
}

/* ---- the EXCLUDE confirmation -------------------------------------------
 *
 * Design 4.1.1: EXCLUDE mode records EVERYTHING the machine is playing and it
 * WALKS THE TARGET'S PROCESS TREE, so naming a terminal or a launcher holds
 * back every program it has started -- and every program it starts a second
 * from now. It must never be a default and never proceed without a
 * confirmation the person actually saw, which is what the checkbox is.
 */
typedef struct ConsentState {
    uint32_t pid;
    wchar_t  target[APR_NAME_CCH];
    wchar_t  tree[DLG_TEXT_CCH];
} ConsentState;

static void build_tree_sentence(ConsentState *cs)
{
    uint32_t pids[64];
    size_t   n = 0, i, listed = 0;
    wchar_t  names[DLG_TEXT_CCH];
    const wchar_t *args[2];
    AprErr   e;

    names[0] = 0;
    e = apr_enum_process_tree(cs->pid, pids, 64, &n);
    if (apr_failed(&e)) n = 0;

    for (i = 1; i < n && i < 64; i++) {   /* [0] is the target itself */
        wchar_t leaf[APR_DISC_NAME_CCH];
        if (apr_process_image_name(pids[i], leaf, APR_DISC_NAME_CCH) == 0) continue;
        if (listed) {
            wchar_t joined[DLG_TEXT_CCH];
            const wchar_t *pair[2];
            pair[0] = names;
            pair[1] = leaf;
            apr_str_format(APR_S_UI_LIST_MORE, joined, DLG_TEXT_CCH, pair, 2);
            lstrcpynW(names, joined, DLG_TEXT_CCH);
        } else {
            lstrcpynW(names, leaf, DLG_TEXT_CCH);
        }
        listed++;
    }

    args[0] = cs->target;
    args[1] = names;
    if (listed) {
        apr_str_format(APR_S_UI_DLG_SYSTEM_TREE, cs->tree, DLG_TEXT_CCH, args, 2);
    } else {
        apr_str_format(APR_S_UI_DLG_SYSTEM_TREE_NONE, cs->tree, DLG_TEXT_CCH,
                       args, 1);
    }
}

static INT_PTR CALLBACK consent_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    ConsentState *cs = (ConsentState *)GetWindowLongPtrW(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)lp);
        cs = (ConsentState *)lp;
        if (cs) SetDlgItemTextW(dlg, IDC_TREE, cs->tree);
        EnableWindow(GetDlgItem(dlg, IDOK), FALSE);
        SetFocus(GetDlgItem(dlg, IDC_CONSENT));
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CONSENT:
            /* The accept button does not exist as a target until the box is
             * ticked. Consent that can be given by pressing Enter twice is not
             * consent. */
            EnableWindow(GetDlgItem(dlg, IDOK),
                         IsDlgButtonChecked(dlg, IDC_CONSENT) == BST_CHECKED);
            return TRUE;
        case IDOK:
            if (IsDlgButtonChecked(dlg, IDC_CONSENT) != BST_CHECKED) return TRUE;
            EndDialog(dlg, 1);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        default:
            break;
        }
        break;

    default:
        break;
    }
    return FALSE;
}

static int ask_system_consent(HWND owner, uint32_t pid, const wchar_t *target)
{
    unsigned char storage[4096];
    DlgBuf b;
    ConsentState cs;
    const DLGTEMPLATE *t;

    memset(&cs, 0, sizeof cs);
    cs.pid = pid;
    lstrcpynW(cs.target, target ? target : L"", APR_NAME_CCH);
    build_tree_sentence(&cs);

    dt_begin(&b, storage, sizeof storage, DS_CENTER, 0, 340, 168,
             apr_str(APR_S_UI_DLG_SYSTEM_TITLE));

    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 8, 324, 52, IDC_BODY, ATOM_STATIC,
            apr_str(APR_S_UI_DLG_SYSTEM_BODY));
    dt_item(&b, SS_LEFT, 0, 8, 64, 324, 42, IDC_TREE, ATOM_STATIC, L"");
    dt_item(&b, BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP | WS_GROUP,
            0, 8, 110, 324, 20, IDC_CONSENT, ATOM_BUTTON,
            apr_str(APR_S_UI_DLG_SYSTEM_CONSENT));
    dt_item(&b, BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, 138, 138, 92, 18,
            IDOK, ATOM_BUTTON, apr_str(APR_S_UI_DLG_OK));
    dt_item(&b, BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 238, 138, 92, 18,
            IDCANCEL, ATOM_BUTTON, apr_str(APR_S_UI_DLG_CANCEL));

    t = dt_end(&b);
    if (!t) return 0;
    return (int)dlg_run(t, owner,
                                        consent_proc, (LPARAM)&cs) == 1;
}

static INT_PTR CALLBACK add_source_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    AddSourceState *st = (AddSourceState *)GetWindowLongPtrW(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)lp);
        st = (AddSourceState *)lp;
        CheckRadioButton(dlg, IDC_KIND_APP, IDC_KIND_SYS, IDC_KIND_APP);
        if (st) {
            fill_source_list(dlg, st);
            sync_name(dlg, st);
        }
        SetFocus(GetDlgItem(dlg, IDC_KIND_APP));
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_KIND_APP:
        case IDC_KIND_DEV:
        case IDC_KIND_SYS:
            if (st) { fill_source_list(dlg, st); sync_name(dlg, st); }
            return TRUE;

        case IDC_REFRESH:
            if (st) { refresh_lists(st); fill_source_list(dlg, st); sync_name(dlg, st); }
            return TRUE;

        case IDC_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE && st) sync_name(dlg, st);
            if (HIWORD(wp) == LBN_DBLCLK) {
                PostMessageW(dlg, WM_COMMAND, IDOK, 0);
                return TRUE;
            }
            break;

        case IDOK: {
            LPARAM idx = -1;
            int kind;

            if (!st) return TRUE;
            kind = selected_kind(dlg);
            if (!lb_selected_data(GetDlgItem(dlg, IDC_LIST), &idx) || idx < 0) {
                apr_dlg_say(dlg, APR_S_UI_DLG_ADD_SOURCE_TITLE,
                            apr_str(APR_S_UI_DLG_PICK_NEEDED));
                return TRUE;
            }

            memset(&st->result, 0, sizeof st->result);
            st->result.kind = (AprDlgSourceKind)kind;
            GetDlgItemTextW(dlg, IDC_NAME, st->result.name, APR_NAME_CCH);
            if (!st->result.name[0]) {
                apr_dlg_say(dlg, APR_S_UI_DLG_ADD_SOURCE_TITLE,
                            apr_str(APR_S_UI_DLG_NAME_NEEDED));
                SetFocus(GetDlgItem(dlg, IDC_NAME));
                return TRUE;
            }

            if (kind == APR_DLG_SRC_DEVICE) {
                if ((size_t)idx >= st->dev_count) return TRUE;
                lstrcpynW(st->result.endpoint_id, st->devs[idx].id,
                          APR_DISC_ENDPOINT_CCH);
            } else {
                if ((size_t)idx >= st->app_count) return TRUE;
                st->result.pid = st->apps[idx].pid;
                if (kind == APR_DLG_SRC_SYSTEM_MINUS_TREE) {
                    const AprAudioApp *a = &st->apps[idx];
                    if (!ask_system_consent(dlg, a->pid,
                                            a->display[0] ? a->display : a->exe)) {
                        return TRUE;   /* refused; the dialog stays open */
                    }
                }
            }
            st->have = 1;
            EndDialog(dlg, 1);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;

        default:
            break;
        }
        break;

    default:
        break;
    }
    return FALSE;
}

int apr_dlg_add_source(HWND owner, AprDlgSource *out)
{
    static AddSourceState st;   /* ~40 KB of fixed arrays; not a stack object */
    unsigned char storage[6144];
    DlgBuf b;
    const DLGTEMPLATE *t;

    if (!out) return 0;
    memset(&st, 0, sizeof st);
    refresh_lists(&st);

    dt_begin(&b, storage, sizeof storage, DS_CENTER, 0, 340, 244,
             apr_str(APR_S_UI_DLG_ADD_SOURCE_TITLE));

    /* WS_GROUP on the first radio and on whatever follows the last one is what
     * makes the three a single arrow-key group -- which is how a screen reader
     * user chooses among them, and how the set gets announced as "1 of 3". */
    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 6, 324, 14, IDC_LIST_LBL + 100,
            ATOM_STATIC, apr_str(APR_S_UI_DLG_SOURCE_KIND));
    dt_item(&b, BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 0, 12, 22, 320, 16,
            IDC_KIND_APP, ATOM_BUTTON, apr_str(APR_S_UI_DLG_KIND_APP));
    dt_item(&b, BS_AUTORADIOBUTTON, 0, 12, 40, 320, 16,
            IDC_KIND_DEV, ATOM_BUTTON, apr_str(APR_S_UI_DLG_KIND_DEVICE));
    dt_item(&b, BS_AUTORADIOBUTTON | BS_MULTILINE, 0, 12, 58, 320, 16,
            IDC_KIND_SYS, ATOM_BUTTON, apr_str(APR_S_UI_DLG_KIND_SYSTEM));

    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 80, 324, 14, IDC_LIST_LBL,
            ATOM_STATIC, apr_str(APR_S_UI_DLG_APP_LIST));
    dt_item(&b, LBS_NOTIFY | LBS_HASSTRINGS | WS_BORDER | WS_VSCROLL |
                WS_TABSTOP | WS_GROUP,
            0, 8, 96, 324, 84, IDC_LIST, ATOM_LISTBOX, L"");
    dt_item(&b, BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, 8, 184, 120, 18,
            IDC_REFRESH, ATOM_BUTTON, apr_str(APR_S_UI_DLG_REFRESH));

    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 208, 100, 14, IDC_NAME_LBL,
            ATOM_STATIC, apr_str(APR_S_UI_DLG_SOURCE_NAME));
    dt_item(&b, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 0, 110, 206, 222, 14,
            IDC_NAME, ATOM_EDIT, L"");

    dt_item(&b, BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, 138, 224, 92, 18,
            IDOK, ATOM_BUTTON, apr_str(APR_S_UI_DLG_OK));
    dt_item(&b, BS_PUSHBUTTON | WS_TABSTOP, 0, 238, 224, 92, 18,
            IDCANCEL, ATOM_BUTTON, apr_str(APR_S_UI_DLG_CANCEL));

    t = dt_end(&b);
    if (!t) return 0;

    if (dlg_run(t, owner,
                                add_source_proc, (LPARAM)&st) != 1) {
        return 0;
    }
    if (!st.have) return 0;
    *out = st.result;
    return 1;
}

/* ==========================================================================
 * A name prompt -- add a bus, rename a bus
 * ======================================================================== */

typedef struct NameState {
    AprStrId label;
    wchar_t  text[APR_NAME_CCH];
} NameState;

static INT_PTR CALLBACK name_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    NameState *st = (NameState *)GetWindowLongPtrW(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)lp);
        st = (NameState *)lp;
        if (st) {
            SetDlgItemTextW(dlg, IDC_NAME, st->text);
            apr_ui_set_accessible_name(GetDlgItem(dlg, IDC_NAME), st->label);
        }
        SetFocus(GetDlgItem(dlg, IDC_NAME));
        SendDlgItemMessageW(dlg, IDC_NAME, EM_SETSEL, 0, -1);
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            if (!st) return TRUE;
            GetDlgItemTextW(dlg, IDC_NAME, st->text, APR_NAME_CCH);
            if (!st->text[0]) {
                apr_dlg_say(dlg, APR_S_UI_DLG_ADD_BUS_TITLE,
                            apr_str(APR_S_UI_DLG_NAME_NEEDED));
                SetFocus(GetDlgItem(dlg, IDC_NAME));
                return TRUE;
            }
            EndDialog(dlg, 1);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        default:
            break;
        }
        break;

    default:
        break;
    }
    return FALSE;
}

int apr_dlg_name_prompt(HWND owner, AprStrId title, AprStrId label,
                        wchar_t *name, size_t cch)
{
    unsigned char storage[2048];
    DlgBuf b;
    NameState st;
    const DLGTEMPLATE *t;

    if (!name || cch == 0) return 0;
    memset(&st, 0, sizeof st);
    st.label = label;
    lstrcpynW(st.text, name, APR_NAME_CCH);

    dt_begin(&b, storage, sizeof storage, DS_CENTER, 0, 280, 84, apr_str(title));

    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 10, 264, 16, IDC_NAME_LBL,
            ATOM_STATIC, apr_str(label));
    dt_item(&b, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | WS_GROUP,
            0, 8, 30, 264, 14, IDC_NAME, ATOM_EDIT, L"");
    dt_item(&b, BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, 78, 56, 92, 18,
            IDOK, ATOM_BUTTON, apr_str(APR_S_UI_DLG_OK));
    dt_item(&b, BS_PUSHBUTTON | WS_TABSTOP, 0, 178, 56, 92, 18,
            IDCANCEL, ATOM_BUTTON, apr_str(APR_S_UI_DLG_CANCEL));

    t = dt_end(&b);
    if (!t) return 0;
    if (dlg_run(t, owner,
                                name_proc, (LPARAM)&st) != 1) {
        return 0;
    }
    lstrcpynW(name, st.text, (int)cch);
    return 1;
}

/* ==========================================================================
 * Adding an output
 * ======================================================================== */

typedef struct OutputState {
    const AprGraph *g;
    AprBusId        prefer;
    AprDlgOutput    result;
} OutputState;

/* The file-type filter is built from the REGISTRY's extensions, so a format
 * this build cannot write is never offered and a new encoder needs no edit
 * here. The one visible string is a catalog entry; the pattern beside it is
 * machine syntax, not prose, and is the only thing assembled in code. */
static void build_audio_filter(wchar_t *buf, size_t cch)
{
    size_t used = 0, i, n = apr_action_count();
    wchar_t pattern[256];
    size_t plen = 0;

    pattern[0] = 0;
    for (i = 0; i < n; i++) {
        const AprActionVTable *vt = apr_action_at(i);
        if (!vt || !vt->extension || !vt->extension[0]) continue;
        if (plen) {
            if (plen + 1 < 256) pattern[plen++] = L';';
        }
        plen += (size_t)_snwprintf_s(pattern + plen, 256 - plen, _TRUNCATE,
                                     L"*.%ls", vt->extension);
    }
    pattern[plen] = 0;

#define PUSH(s)                                                                \
    do {                                                                       \
        size_t l_ = wcslen(s);                                                 \
        if (used + l_ + 2 >= cch) { buf[used] = 0; buf[used + 1] = 0; return; } \
        memcpy(buf + used, (s), l_ * sizeof(wchar_t));                          \
        used += l_;                                                             \
        buf[used++] = 0;                                                        \
    } while (0)

    PUSH(apr_str(APR_S_UI_DLG_FILTER_AUDIO));
    PUSH(pattern);
    PUSH(apr_str(APR_S_UI_DLG_FILTER_ALL));
    PUSH(L"*.*");
#undef PUSH
    buf[used] = 0;   /* the second terminator that ends the whole filter */
}

static void browse_for_audio(HWND dlg)
{
    OPENFILENAMEW ofn;
    wchar_t path[APR_DISC_PATH_CCH];
    wchar_t filter[512];

    memset(&ofn, 0, sizeof ofn);
    memset(filter, 0, sizeof filter);
    path[0] = 0;
    GetDlgItemTextW(dlg, IDC_PATH, path, APR_DISC_PATH_CCH);
    build_audio_filter(filter, 512 - 2);

    ofn.lStructSize     = sizeof ofn;
    ofn.hwndOwner       = dlg;
    ofn.lpstrFilter     = filter;
    ofn.lpstrFile       = path;
    ofn.nMaxFile        = APR_DISC_PATH_CCH;
    ofn.lpstrTitle      = apr_str(APR_S_UI_DLG_SAVE_AUDIO_TITLE);
    /* NO OFN_OVERWRITEPROMPT, and its absence is the honest answer.
     *
     * The shell's prompt asks "replace it?" and offers Yes/No -- but nothing
     * downstream ever replaces anything: outpath.h's collision policy KEEPS
     * the existing recording and moves the new take aside to "-2", and the
     * runner announces that when it happens. Answering Yes therefore did not
     * replace the file, so the dialog was describing behaviour the product
     * does not have. This picker names a TEMPLATE anyway ({date}, {n}), and
     * the file it expands to is not chosen until the recording starts. */
    ofn.Flags           = OFN_PATHMUSTEXIST |
                          OFN_NOCHANGEDIR | OFN_EXPLORER;

    if (GetSaveFileNameW(&ofn)) SetDlgItemTextW(dlg, IDC_PATH, path);
    /* Focus goes back to the box the path landed in, so a screen reader reads
     * the chosen path rather than leaving the user at the browse button
     * wondering whether anything happened. */
    SetFocus(GetDlgItem(dlg, IDC_PATH));
}

static INT_PTR CALLBACK output_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    OutputState *st = (OutputState *)GetWindowLongPtrW(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG: {
        HWND fmt, bus;
        size_t i, n;

        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)lp);
        st = (OutputState *)lp;
        fmt = GetDlgItem(dlg, IDC_FORMAT);
        bus = GetDlgItem(dlg, IDC_BUS);
        apr_ui_set_accessible_name(fmt, APR_S_UI_DLG_OUT_FORMAT);
        apr_ui_set_accessible_name(bus, APR_S_UI_DLG_OUT_BUS);
        apr_ui_set_accessible_name(GetDlgItem(dlg, IDC_PATH), APR_S_UI_DLG_OUT_PATH);
        apr_ui_set_accessible_name(GetDlgItem(dlg, IDC_BITRATE), APR_S_UI_DLG_OUT_BITRATE);
        apr_ui_set_accessible_name(GetDlgItem(dlg, IDC_QUALITY), APR_S_UI_DLG_OUT_QUALITY);

        /* THE REGISTRY IS THE LIST. Never a hardcoded set of formats. */
        n = apr_action_count();
        for (i = 0; i < n; i++) {
            const AprActionVTable *vt = apr_action_at(i);
            LRESULT k;
            if (!vt || !vt->extension || !vt->extension[0]) continue;  /* skip "none" */
            k = SendMessageW(fmt, CB_ADDSTRING, 0,
                             (LPARAM)apr_str(vt->display_name_id));
            if (k >= 0) SendMessageW(fmt, CB_SETITEMDATA, (WPARAM)k, (LPARAM)i);
        }
        SendMessageW(fmt, CB_SETCURSEL, 0, 0);

        if (st && st->g) {
            n = apr_graph_bus_count(st->g);
            for (i = 0; i < n; i++) {
                AprBus *b = apr_graph_bus_at(st->g, i);
                LRESULT k;
                if (!b) continue;
                k = SendMessageW(bus, CB_ADDSTRING, 0, (LPARAM)apr_bus_name(b));
                if (k < 0) continue;
                SendMessageW(bus, CB_SETITEMDATA, (WPARAM)k, (LPARAM)apr_bus_id(b));
                if (apr_bus_id(b) == st->prefer) {
                    SendMessageW(bus, CB_SETCURSEL, (WPARAM)k, 0);
                }
            }
            if (SendMessageW(bus, CB_GETCURSEL, 0, 0) == CB_ERR)
                SendMessageW(bus, CB_SETCURSEL, 0, 0);
        }
        /* A DEFAULT THAT CANNOT OVERWRITE ANYTHING. The field used to open
         * empty, which made "type a name" the first step of every recording
         * and made reusing yesterday's name the path of least resistance --
         * which is how a take gets lost. This is a timestamped template
         * (outpath.h); it is editable, and {ext} follows the format combo
         * above so changing the format does not leave a stale extension. */
        {
            wchar_t deft[APR_OUT_PATH_CCH];
            apr_out_default_template(deft, APR_OUT_PATH_CCH);
            SetDlgItemTextW(dlg, IDC_PATH, deft);
        }
        /* The tokens are on screen as a label AND on the path field as its
         * accessible description, because a static beside a control is not
         * something a screen reader reads when you tab into the control. */
        apr_ui_set_accessible_description(GetDlgItem(dlg, IDC_PATH),
                                          APR_S_UI_DLG_OUT_PATH_TOKENS);
        /* apr_str_number(), not L"0". Every digit a user sees goes through
         * that one function -- it is where the Western-versus-Arabic-Indic
         * decision lives (design 6.2) -- and a literal here is a zero that
         * would stay Western when every other number in the window changed. */
        {
            wchar_t zero[24];
            apr_str_number(0, zero, 24);
            SetDlgItemTextW(dlg, IDC_BITRATE, zero);
            SetDlgItemTextW(dlg, IDC_QUALITY, zero);
        }
        SetFocus(bus);
        return FALSE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE:
            browse_for_audio(dlg);
            return TRUE;

        case IDOK: {
            LRESULT k;
            const AprActionVTable *vt;
            wchar_t num[32];

            if (!st) return TRUE;
            memset(&st->result, 0, sizeof st->result);

            GetDlgItemTextW(dlg, IDC_PATH, st->result.path, APR_DISC_PATH_CCH);
            if (!st->result.path[0]) {
                apr_dlg_say(dlg, APR_S_UI_DLG_ADD_OUTPUT_TITLE,
                            apr_str(APR_S_UI_DLG_PATH_NEEDED));
                SetFocus(GetDlgItem(dlg, IDC_PATH));
                return TRUE;
            }

            /* A BARE `return TRUE` HERE IS A BUTTON THAT DOES NOTHING.
             *
             * With no selection in a combo -- which is what an empty registry
             * or a failed populate leaves behind -- OK used to swallow the
             * press and stay put: no dialog closed, no model changed, nothing
             * said. For a listener that is a dead key, and a dead key on OK is
             * the shape of "the application is broken". Every refusal below
             * now names what is missing and puts focus on it. */
            k = SendDlgItemMessageW(dlg, IDC_BUS, CB_GETCURSEL, 0, 0);
            if (k == CB_ERR) {
                apr_dlg_say(dlg, APR_S_UI_DLG_ADD_OUTPUT_TITLE,
                            apr_str(APR_S_UI_DLG_NO_BUSES));
                SetFocus(GetDlgItem(dlg, IDC_BUS));
                return TRUE;
            }
            st->result.bus = (AprBusId)SendDlgItemMessageW(dlg, IDC_BUS,
                                                           CB_GETITEMDATA,
                                                           (WPARAM)k, 0);

            k = SendDlgItemMessageW(dlg, IDC_FORMAT, CB_GETCURSEL, 0, 0);
            if (k == CB_ERR) {
                apr_dlg_say(dlg, APR_S_UI_DLG_ADD_OUTPUT_TITLE,
                            apr_str(APR_S_UI_DLG_FORMAT_NEEDED));
                SetFocus(GetDlgItem(dlg, IDC_FORMAT));
                return TRUE;
            }
            k = SendDlgItemMessageW(dlg, IDC_FORMAT, CB_GETITEMDATA, (WPARAM)k, 0);
            vt = apr_action_at((size_t)k);
            if (!vt || !vt->id) {
                apr_dlg_say(dlg, APR_S_UI_DLG_ADD_OUTPUT_TITLE,
                            apr_str(APR_S_UI_DLG_FORMAT_NEEDED));
                SetFocus(GetDlgItem(dlg, IDC_FORMAT));
                return TRUE;
            }
            strncpy_s(st->result.action_id, sizeof st->result.action_id,
                      vt->id, _TRUNCATE);

            GetDlgItemTextW(dlg, IDC_BITRATE, num, 32);
            st->result.bitrate_kbps = _wtoi(num);
            GetDlgItemTextW(dlg, IDC_QUALITY, num, 32);
            st->result.quality = _wtoi(num);

            EndDialog(dlg, 1);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;

        default:
            break;
        }
        break;

    default:
        break;
    }
    return FALSE;
}

int apr_dlg_add_output(HWND owner, const AprGraph *g, AprBusId prefer,
                       AprDlgOutput *out)
{
    unsigned char storage[6144];
    DlgBuf b;
    OutputState st;
    const DLGTEMPLATE *t;

    if (!out || !g) return 0;
    if (apr_graph_bus_count(g) == 0) {
        apr_dlg_say(owner, APR_S_UI_DLG_ADD_OUTPUT_TITLE,
                    apr_str(APR_S_UI_DLG_NO_BUSES));
        return 0;
    }

    memset(&st, 0, sizeof st);
    st.g = g;
    st.prefer = prefer;

    dt_begin(&b, storage, sizeof storage, DS_CENTER, 0, 340, 216,
             apr_str(APR_S_UI_DLG_ADD_OUTPUT_TITLE));

    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 8, 324, 14, IDC_BUS + 100,
            ATOM_STATIC, apr_str(APR_S_UI_DLG_OUT_BUS));
    dt_item(&b, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
            0, 8, 24, 324, 120, IDC_BUS, ATOM_COMBOBOX, L"");

    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 46, 324, 14, IDC_FORMAT + 100,
            ATOM_STATIC, apr_str(APR_S_UI_DLG_OUT_FORMAT));
    dt_item(&b, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
            0, 8, 62, 324, 120, IDC_FORMAT, ATOM_COMBOBOX, L"");

    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 84, 324, 14, IDC_PATH + 100,
            ATOM_STATIC, apr_str(APR_S_UI_DLG_OUT_PATH));
    dt_item(&b, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | WS_GROUP,
            0, 8, 100, 226, 14, IDC_PATH, ATOM_EDIT, L"");
    dt_item(&b, BS_PUSHBUTTON | WS_TABSTOP, 0, 240, 99, 92, 18,
            IDC_BROWSE, ATOM_BUTTON, apr_str(APR_S_UI_DLG_BROWSE));

    /* Twenty-six units, not the fourteen the English sentence needs on one
     * line: Arabic wants more vertical room at the same point size, and a
     * label sized to its English is a label that clips when translated
     * (AGENTS.md rule 6). */
    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 118, 324, 26, IDC_PATH + 200,
            ATOM_STATIC, apr_str(APR_S_UI_DLG_OUT_PATH_TOKENS));

    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 148, 254, 14, IDC_BITRATE + 100,
            ATOM_STATIC, apr_str(APR_S_UI_DLG_OUT_BITRATE));
    dt_item(&b, ES_NUMBER | WS_BORDER | WS_TABSTOP, 0, 266, 146, 66, 14,
            IDC_BITRATE, ATOM_EDIT, L"");

    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 168, 254, 14, IDC_QUALITY + 100,
            ATOM_STATIC, apr_str(APR_S_UI_DLG_OUT_QUALITY));
    dt_item(&b, ES_NUMBER | WS_BORDER | WS_TABSTOP, 0, 266, 166, 66, 14,
            IDC_QUALITY, ATOM_EDIT, L"");

    dt_item(&b, BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, 138, 192, 92, 18,
            IDOK, ATOM_BUTTON, apr_str(APR_S_UI_DLG_OK));
    dt_item(&b, BS_PUSHBUTTON | WS_TABSTOP, 0, 238, 192, 92, 18,
            IDCANCEL, ATOM_BUTTON, apr_str(APR_S_UI_DLG_CANCEL));

    t = dt_end(&b);
    if (!t) return 0;
    if (dlg_run(t, owner,
                                output_proc, (LPARAM)&st) != 1) {
        return 0;
    }
    *out = st.result;
    return 1;
}

/* ==========================================================================
 * Which output to remove
 * ======================================================================== */

typedef struct PickOutState {
    const AprGraph *g;
    AprBusId        bus;
} PickOutState;

static void fill_outputs(HWND lb, void *user)
{
    PickOutState *p = (PickOutState *)user;
    AprBus *b = p->g ? apr_graph_bus(p->g, p->bus) : NULL;
    size_t i, n;

    if (!b) return;
    n = apr_bus_action_count(b);
    for (i = 0; i < n; i++) {
        const AprActionVTable *vt = apr_bus_action_at(b, i);
        const wchar_t *args[2];
        wchar_t row[DLG_TEXT_CCH];
        args[0] = vt ? apr_str(vt->display_name_id) : L"";
        args[1] = apr_bus_name(b);
        apr_str_format(APR_S_UI_DLG_OUTPUT_ROW, row, DLG_TEXT_CCH, args, 2);
        lb_add(lb, row, (LPARAM)i);
    }
}

int apr_dlg_pick_output(HWND owner, const AprGraph *g, AprBusId bus,
                        size_t *out_index)
{
    ListState st;
    PickOutState p;

    if (!g || !out_index) return 0;
    p.g = g;
    p.bus = bus;

    memset(&st, 0, sizeof st);
    st.title = APR_S_UI_DLG_REMOVE_TITLE;
    st.list_label = APR_S_UI_DLG_PICK_OUTPUT;
    st.fill = fill_outputs;
    st.user = &p;

    if (!list_dialog(owner, &st) || !st.have) return 0;
    *out_index = (size_t)st.picked;
    return 1;
}

/* ==========================================================================
 * Session files
 * ======================================================================== */

/* TEST ONLY. See apr_dlg_test_set_session_path: the common file picker is a
 * system modal that owns the thread that opened it, so File > Open is
 * otherwise a route no test can enter. */
static wchar_t g_session_path_override[APR_DISC_PATH_CCH];
static int     g_session_path_set;

void apr_dlg_test_set_session_path(const wchar_t *path)
{
    if (path && path[0]) {
        lstrcpynW(g_session_path_override, path, APR_DISC_PATH_CCH);
        g_session_path_set = 1;
    } else {
        g_session_path_override[0] = 0;
        g_session_path_set = 0;
    }
}

int apr_dlg_choose_session(HWND owner, int for_saving, wchar_t *path, size_t cch)
{
    OPENFILENAMEW ofn;
    wchar_t filter[256];
    size_t used = 0;
    BOOL ok;

    if (!path || cch < 4) return 0;

    /* Consumed once, so a test arms it for exactly the press it means. */
    if (g_session_path_set) {
        lstrcpynW(path, g_session_path_override, (int)cch);
        g_session_path_set = 0;
        return 1;
    }

    memset(&ofn, 0, sizeof ofn);
    memset(filter, 0, sizeof filter);

#define PUSHF(s)                                                               \
    do {                                                                       \
        size_t l_ = wcslen(s);                                                 \
        if (used + l_ + 2 >= 254) break;                                        \
        memcpy(filter + used, (s), l_ * sizeof(wchar_t));                       \
        used += l_;                                                             \
        filter[used++] = 0;                                                     \
    } while (0)
    PUSHF(apr_str(APR_S_UI_DLG_FILTER_SESSION));
    PUSHF(L"*.json");
    PUSHF(apr_str(APR_S_UI_DLG_FILTER_ALL));
    PUSHF(L"*.*");
#undef PUSHF
    filter[used] = 0;

    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = (DWORD)cch;
    ofn.lpstrTitle  = apr_str(for_saving ? APR_S_UI_DLG_SAVE_SESSION_TITLE
                                         : APR_S_UI_DLG_OPEN_SESSION_TITLE);
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER |
                (for_saving ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
                            : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));

    ok = for_saving ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    return ok ? 1 : 0;
}

/* ==========================================================================
 * The resolve report
 *
 * session.h goes to real trouble to return WHAT WAS ASKED FOR, WHAT WAS USED
 * INSTEAD, and EVERY RIVAL CANDIDATE WITH ITS PID -- explicitly so that a UI
 * can show it rather than reduce the whole thing to "failed". So this shows
 * all of it: one row per source saying what happened, and a row per rival
 * underneath the ambiguous ones.
 * ======================================================================== */

typedef struct ResolveState {
    const AprSessionResolveReport *rep;
    wchar_t summary[DLG_TEXT_CCH];
} ResolveState;

static INT_PTR CALLBACK resolve_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    ResolveState *st = (ResolveState *)GetWindowLongPtrW(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG: {
        HWND lb;
        size_t i, k;

        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)lp);
        st = (ResolveState *)lp;
        lb = GetDlgItem(dlg, IDC_LIST);
        apr_ui_set_accessible_name(lb, APR_S_UI_DLG_RESOLVE_LIST);
        SetDlgItemTextW(dlg, IDC_SUMMARY, st ? st->summary : L"");

        if (st && st->rep) {
            for (i = 0; i < st->rep->count; i++) {
                const AprSessionResolution *r = &st->rep->items[i];
                wchar_t row[DLG_TEXT_CCH];
                apr_dlg_resolution_row(r, row, DLG_TEXT_CCH);
                lb_add(lb, row, (LPARAM)i);

                /* The rivals, by pid and path. This is the list design 9 asks
                 * a UI to turn into "which one did you mean", and it is
                 * useless as a count. */
                for (k = 0; k < r->candidates_listed; k++) {
                    const wchar_t *args[2];
                    wchar_t pid[32], line[DLG_TEXT_CCH];
                    args[0] = num_of((int64_t)r->candidates[k].pid, pid, 32);
                    args[1] = r->candidates[k].path;
                    apr_str_format(APR_S_UI_DLG_RESOLVE_CANDIDATE, line,
                                   DLG_TEXT_CCH, args, 2);
                    lb_add(lb, line, (LPARAM)i);
                }
            }
        }
        SendMessageW(lb, LB_SETCURSEL, 0, 0);
        SetFocus(lb);
        return FALSE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK)     { EndDialog(dlg, 1); return TRUE; }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        break;

    default:
        break;
    }
    return FALSE;
}

int apr_dlg_resolve_report(HWND owner, const AprSessionResolveReport *rep)
{
    unsigned char storage[4096];
    DlgBuf b;
    ResolveState st;
    const DLGTEMPLATE *t;
    const wchar_t *args[3];
    wchar_t a[32], c[32], d[32];

    if (!rep) return 0;
    memset(&st, 0, sizeof st);
    st.rep = rep;

    args[0] = num_of((int64_t)rep->ok, a, 32);
    args[1] = num_of((int64_t)rep->substituted, c, 32);
    args[2] = num_of((int64_t)rep->failed, d, 32);
    apr_str_format(APR_S_UI_DLG_RESOLVE_SUMMARY, st.summary, DLG_TEXT_CCH,
                   args, 3);

    dt_begin(&b, storage, sizeof storage, DS_CENTER, 0, 400, 214,
             apr_str(APR_S_UI_DLG_RESOLVE_TITLE));

    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 8, 384, 26, IDC_SUMMARY,
            ATOM_STATIC, L"");
    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 36, 384, 14, IDC_LIST_LBL,
            ATOM_STATIC, apr_str(APR_S_UI_DLG_RESOLVE_LIST));
    dt_item(&b, LBS_NOTIFY | LBS_HASSTRINGS | WS_BORDER | WS_VSCROLL |
                WS_HSCROLL | WS_TABSTOP | WS_GROUP,
            0, 8, 52, 384, 128, IDC_LIST, ATOM_LISTBOX, L"");
    dt_item(&b, BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, 198, 188, 92, 18,
            IDOK, ATOM_BUTTON, apr_str(APR_S_UI_DLG_RESOLVE_LOAD));
    dt_item(&b, BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 298, 188, 92, 18,
            IDCANCEL, ATOM_BUTTON, apr_str(APR_S_UI_DLG_CANCEL));

    t = dt_end(&b);
    if (!t) return 0;
    return (int)dlg_run(t, owner,
                                        resolve_proc, (LPARAM)&st) == 1;
}

/* ==========================================================================
 * Help
 * ======================================================================== */

static void fill_keys(HWND lb, void *user)
{
    size_t i, n = apr_canvas_binding_count();

    (void)user;
    for (i = 0; i < n; i++) {
        const AprCanvasBinding *bd = apr_canvas_binding_at(i);
        wchar_t row[DLG_TEXT_CCH];
        if (!bd) continue;
        apr_dlg_binding_row(bd, row, DLG_TEXT_CCH);
        lb_add(lb, row, (LPARAM)i);
    }
}

static INT_PTR CALLBACK help_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        HWND lb = GetDlgItem(dlg, IDC_LIST);
        (void)lp;
        apr_ui_set_accessible_name(lb, APR_S_UI_DLG_KEYS_LIST);
        fill_keys(lb, NULL);
        SendMessageW(lb, LB_SETCURSEL, 0, 0);
        SetFocus(lb);
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

void apr_dlg_keyboard_help(HWND owner)
{
    unsigned char storage[4096];
    DlgBuf b;
    const DLGTEMPLATE *t;

    dt_begin(&b, storage, sizeof storage, DS_CENTER, 0, 360, 216,
             apr_str(APR_S_UI_DLG_KEYS_TITLE));
    dt_item(&b, SS_LEFT | WS_GROUP, 0, 8, 8, 344, 14, IDC_LIST_LBL,
            ATOM_STATIC, apr_str(APR_S_UI_DLG_KEYS_LIST));
    dt_item(&b, LBS_NOTIFY | LBS_HASSTRINGS | WS_BORDER | WS_VSCROLL |
                WS_HSCROLL | WS_TABSTOP | WS_GROUP,
            0, 8, 24, 344, 160, IDC_LIST, ATOM_LISTBOX, L"");
    dt_item(&b, BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, 260, 192, 92, 18,
            IDCANCEL, ATOM_BUTTON, apr_str(APR_S_UI_DLG_CLOSE));

    t = dt_end(&b);
    if (!t) return;
    (void)dlg_run(t, owner,
                                  help_proc, 0);
}

void apr_dlg_about(HWND owner)
{
    wchar_t body[DLG_TEXT_CCH];
    const wchar_t *args[2];

    args[0] = apr_str(APR_S_APP_NAME);
    args[1] = apr_str(APR_S_APP_TAGLINE);
    apr_str_format(APR_S_UI_DLG_ABOUT_BODY, body, DLG_TEXT_CCH, args, 2);
    apr_dlg_say(owner, APR_S_UI_DLG_ABOUT_TITLE, body);
}
