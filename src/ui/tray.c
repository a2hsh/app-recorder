/*
 * tray.c -- the notification area icon. See ui_tray.h for why it matters more
 * than a tray icon usually does; what follows is the mechanics that bite.
 *
 * ---------------------------------------------------------------------------
 * THREE SHELL BEHAVIOURS THAT ARE NOT OPTIONAL TO HANDLE
 *
 *   1. NOTIFYICON_VERSION_4 CHANGES THE CALLBACK'S ARGUMENTS. Without the
 *      NIM_SETVERSION call, wParam is the icon id and lParam is a mouse
 *      message; with it, wParam packs the SCREEN coordinates and lParam packs
 *      the notification event. Version 4 is what we want -- it is the one that
 *      delivers WM_CONTEXTMENU for the keyboard's Applications key, and the
 *      one whose coordinates are already in screen space -- but reading the
 *      arguments the old way against a version-4 icon produces a menu in the
 *      wrong place and a keyboard path that never fires.
 *
 *   2. TrackPopupMenu NEEDS SetForegroundWindow BEFORE AND A NULL MESSAGE
 *      AFTER. Without the first, the menu appears and then dismisses itself
 *      the moment the mouse moves. Without the second, the menu does not go
 *      away when the user clicks elsewhere. This is a documented pair of
 *      quirks that is as old as the tray itself and has never been fixed.
 *
 *   3. THE SHELL CAN RESTART. Explorer crashes, or the user signs out and back
 *      in, and every tray icon is gone. The shell broadcasts a registered
 *      message, "TaskbarCreated"; an application that does not listen for it
 *      silently loses its icon for the rest of the session. For a recorder
 *      that runs for hours, that is losing the only surface it has.
 *
 * ---------------------------------------------------------------------------
 * WHY NO GUID
 *
 *   NIF_GUID gives an icon a stable identity across restarts, which sounds
 *   right, and then refuses to appear at all if the executable is moved or
 *   rebuilt to a different path -- the shell remembers the association by
 *   path. During development that is an icon that mysteriously stops working.
 *   uID is enough for one process with one icon.
 */
#include "ui_tray.h"

#include <shellapi.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "ui_app.h"

#pragma comment(lib, "shell32.lib")

#define APR_TRAY_UID 1

#define APR_TRAY_TIP_CCH  128
#define APR_TRAY_INFO_CCH 256

struct AprTray {
    HWND         owner;
    NOTIFYICONDATAW nid;
    AprTrayState state;
    int          added;
    int          can_start;
    int          can_stop;
    UINT         msg_taskbar_created;
    wchar_t      tip[APR_TRAY_TIP_CCH];
};

/* --------------------------------------------------------------------------
 * Adding and removing
 * ----------------------------------------------------------------------- */

static HICON tray_icon(void)
{
    /* The application's own icon where the .rc supplies one, the system's
     * otherwise. Never NULL: an icon-less tray entry is a blank cell the
     * keyboard can land on and a screen reader cannot describe. */
    HICON ic = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    if (!ic) ic = LoadIconW(NULL, IDI_APPLICATION);
    return ic;
}

static void tray_fill(AprTray *t)
{
    memset(&t->nid, 0, sizeof t->nid);
    t->nid.cbSize = sizeof t->nid;
    t->nid.hWnd   = t->owner;
    t->nid.uID    = APR_TRAY_UID;
    t->nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    t->nid.uCallbackMessage = APR_TRAY_WM_ICON;
    t->nid.hIcon  = tray_icon();
    lstrcpynW(t->nid.szTip, t->tip, (int)(sizeof t->nid.szTip / sizeof(wchar_t)));
}

static void tray_add(AprTray *t)
{
    tray_fill(t);
    if (!Shell_NotifyIconW(NIM_ADD, &t->nid)) {
        /* Not fatal. The window still works; what is lost is the surface the
         * user reaches while it is minimised, and that is worth a log line
         * somebody can find rather than a failed startup. */
        APR_WARN(L"tray: Shell_NotifyIcon(NIM_ADD) failed (%lu)",
                 (unsigned long)GetLastError());
        t->added = 0;
        return;
    }
    /* Version 4 or the keyboard's Applications key never reaches us. See the
     * file header. */
    t->nid.uVersion = NOTIFYICON_VERSION_4;
    (void)Shell_NotifyIconW(NIM_SETVERSION, &t->nid);
    t->added = 1;
}

AprErr apr_tray_create(HWND owner, AprTray **out)
{
    AprTray *t;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"apr_tray_create: out is NULL");
    *out = NULL;
    if (!owner || !IsWindow(owner)) {
        return APR_ERR(APR_E_INVALID_ARG, L"apr_tray_create: no owner window");
    }

    t = (AprTray *)calloc(1, sizeof *t);
    if (!t) return APR_ERR(APR_E_NO_MEMORY, L"apr_tray_create: out of memory");

    t->owner = owner;
    t->state = APR_TRAY_IDLE;
    t->msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    lstrcpynW(t->tip, apr_str(APR_S_UI_TRAY_TIP_IDLE), APR_TRAY_TIP_CCH);

    /* A TEST MUST NEVER PUT AN ICON IN A REAL PERSON'S NOTIFICATION AREA.
     *
     * Three suites build a real controller, and a controller builds a real
     * tray. Running the suite therefore fired real shell notifications at the
     * author -- announced aloud by his screen reader, every run. He asked
     * "I'm still getting them, what do I do?" while the notifications were
     * coming from test_ui_behaviour.exe rather than from the application.
     *
     * The whole object stays real so the tests still exercise its logic;
     * only the shell registration is skipped. Everything downstream is
     * already gated on `added`, so tip updates, menus and notifications all
     * become no-ops with no further checks.
     *
     * Set for every test by CMake, so a new UI suite cannot forget it. */
    if (GetEnvironmentVariableW(L"APPRECORDER_NO_TRAY", NULL, 0) == 0) {
        tray_add(t);
    } else {
        APR_INFO(L"tray: APPRECORDER_NO_TRAY is set; no shell icon registered");
    }
    *out = t;
    return apr_ok();
}

void apr_tray_destroy(AprTray *t)
{
    if (!t) return;
    if (t->added) {
        t->nid.uFlags = 0;
        (void)Shell_NotifyIconW(NIM_DELETE, &t->nid);
    }
    free(t);
}

/* DID THE SHELL ACTUALLY TAKE THE ICON?
 *
 * NIM_ADD is allowed to fail -- Explorer restarting, a policy, an
 * over-full notification area -- and that failure is deliberately not fatal,
 * because the window still works. But it makes ONE thing unsafe: hiding the
 * window. Hidden with no icon, the application has no surface at all: not
 * Alt+Tab, not Windows+B, nothing -- while it is still recording. So the
 * hiding paths have to be able to ask. */
int apr_tray_is_registered(const AprTray *t)
{
    return t && t->added;
}

int apr_tray_on_taskbar_created(AprTray *t, UINT msg)
{
    if (!t || !t->msg_taskbar_created || msg != t->msg_taskbar_created) return 0;
    t->added = 0;
    tray_add(t);
    return 1;
}

/* --------------------------------------------------------------------------
 * Status
 * ----------------------------------------------------------------------- */

void apr_tray_set_status(AprTray *t, AprTrayState state, const wchar_t *elapsed)
{
    wchar_t tip[APR_TRAY_TIP_CCH];
    const wchar_t *args[1];

    if (!t) return;
    t->state = state;

    switch (state) {
    case APR_TRAY_RECORDING:
        args[0] = elapsed ? elapsed : L"";
        apr_str_format(APR_S_UI_TRAY_TIP_RECORDING, tip, APR_TRAY_TIP_CCH,
                       args, 1);
        break;
    case APR_TRAY_FINISHING:
        lstrcpynW(tip, apr_str(APR_S_UI_TRAY_TIP_FINISHING), APR_TRAY_TIP_CCH);
        break;
    default:
        lstrcpynW(tip, apr_str(APR_S_UI_TRAY_TIP_IDLE), APR_TRAY_TIP_CCH);
        break;
    }

    /* Publish only on a real change. NIM_MODIFY once a second forever is
     * cheap, but a tooltip that is rewritten while a screen reader is reading
     * it can be cut off mid-sentence. */
    if (CompareStringOrdinal(tip, -1, t->tip, -1, FALSE) == CSTR_EQUAL) return;

    lstrcpynW(t->tip, tip, APR_TRAY_TIP_CCH);
    if (!t->added) return;

    t->nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    lstrcpynW(t->nid.szTip, t->tip,
              (int)(sizeof t->nid.szTip / sizeof(wchar_t)));
    (void)Shell_NotifyIconW(NIM_MODIFY, &t->nid);
}

AprTrayState apr_tray_state(const AprTray *t)
{
    return t ? t->state : APR_TRAY_IDLE;
}

size_t apr_tray_tip(const AprTray *t, wchar_t *buf, size_t cch)
{
    if (!buf || cch == 0) return 0;
    buf[0] = 0;
    if (!t) return 0;
    lstrcpynW(buf, t->tip, (int)cch);
    return wcslen(buf);
}

void apr_tray_notify(AprTray *t, AprStrId title, const wchar_t *text)
{
    if (!t || !t->added || !text) return;

    t->nid.uFlags = NIF_INFO;
    t->nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    lstrcpynW(t->nid.szInfoTitle, apr_str(title),
              (int)(sizeof t->nid.szInfoTitle / sizeof(wchar_t)));
    lstrcpynW(t->nid.szInfo, text,
              (int)(sizeof t->nid.szInfo / sizeof(wchar_t)));
    (void)Shell_NotifyIconW(NIM_MODIFY, &t->nid);
}

void apr_tray_set_can_record(AprTray *t, int can_start, int can_stop)
{
    if (!t) return;
    t->can_start = can_start;
    t->can_stop  = can_stop;
}

/* --------------------------------------------------------------------------
 * The menu
 * ----------------------------------------------------------------------- */

HMENU apr_tray_build_menu(const AprTray *t)
{
    HMENU m = CreatePopupMenu();

    if (!m) return NULL;

    /* Show the window is the DEFAULT item, which is what Enter on the icon
     * does and what a screen reader announces as the default. */
    AppendMenuW(m, MF_STRING, APR_CMD_SHOW_WINDOW,
                apr_str(APR_S_UI_TRAY_MENU_SHOW));
    SetMenuDefaultItem(m, APR_CMD_SHOW_WINDOW, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    /* Disabled, never absent: grey says "not now" to someone who can see it,
     * and a screen reader reads "unavailable" -- an item that vanishes says
     * nothing at all. Same rule the menu bar follows (ui_app.h). */
    AppendMenuW(m, MF_STRING | (t && t->can_start ? MF_ENABLED : MF_GRAYED),
                APR_CMD_RECORD_START, apr_str(APR_S_UI_TRAY_MENU_START));
    AppendMenuW(m, MF_STRING | (t && t->can_stop ? MF_ENABLED : MF_GRAYED),
                APR_CMD_RECORD_STOP, apr_str(APR_S_UI_TRAY_MENU_STOP));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, APR_CMD_FILE_OPEN,
                apr_str(APR_S_UI_TRAY_MENU_OPEN));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, APR_CMD_FILE_EXIT,
                apr_str(APR_S_UI_TRAY_MENU_QUIT));
    return m;
}

static void show_menu(AprTray *t, int x, int y)
{
    HMENU m = apr_tray_build_menu(t);
    UINT  flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY;
    int   cmd;

    if (!m) return;

    if (apr_ui_dir() == APR_DIR_RTL) flags |= TPM_LAYOUTRTL | TPM_RIGHTALIGN;

    /* Both halves of the ancient tray-menu dance. See the file header. */
    SetForegroundWindow(t->owner);
    cmd = (int)TrackPopupMenuEx(m, flags, x, y, t->owner, NULL);
    PostMessageW(t->owner, WM_NULL, 0, 0);
    DestroyMenu(m);

    /* Delivered as an ordinary WM_COMMAND, so the tray shares the frame's one
     * dispatch path and an operation cannot behave differently depending on
     * which surface raised it. */
    if (cmd) PostMessageW(t->owner, WM_COMMAND, (WPARAM)cmd, 0);
}

int apr_tray_on_message(AprTray *t, WPARAM wp, LPARAM lp)
{
    UINT event;

    if (!t) return 0;

    /* NOTIFYICON_VERSION_4 packing: wParam is the screen position, lParam the
     * event in its low word. */
    event = (UINT)LOWORD(lp);

    switch (event) {
    case WM_CONTEXTMENU:
        /* Right-click AND the keyboard's Applications key / Shift+F10 both
         * arrive here, which is exactly why the menu is not a mouse feature. */
        show_menu(t, (int)(short)LOWORD(wp), (int)(short)HIWORD(wp));
        return 1;

    case NIN_SELECT:
    case NIN_KEYSELECT:
        PostMessageW(t->owner, WM_COMMAND, (WPARAM)APR_CMD_SHOW_WINDOW, 0);
        return 1;

    default:
        break;
    }
    return 0;
}
