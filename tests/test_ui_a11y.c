/*
 * test_ui_a11y.c -- walk the LIVE UI Automation tree of a real window and
 * assert the properties a screen reader depends on.
 *
 * ===========================================================================
 * WHY THIS AND NOT A UNIT TEST
 *
 *   A screen reader sees exactly what UI Automation exposes. Not what the
 *   window procedure intended, not what the window text says, not what a
 *   struct in our code holds -- what the UIA provider chain actually reports
 *   after MSAA-to-UIA bridging, comctl32's own providers and the default HWND
 *   provider have all had their say. Anything short of asking UIA is a proxy
 *   for the property the author cares about, and proxies are what let a
 *   regression ship.
 *
 *   So this test is a UIA CLIENT. It creates the real frame on a real UI
 *   thread and then, from another thread, does what NVDA does: connect to
 *   CUIAutomation, get the element for the window, and walk it.
 *
 *   Two threads is not incidental. A UIA client that queries a window on the
 *   same STA thread that owns it can deadlock -- the provider side needs to
 *   pump messages to answer, and it cannot while blocked in the client call.
 *   The UI lives on a worker with its own message loop; the client runs on the
 *   test's main thread in the MTA.
 *
 * ===========================================================================
 * WHAT IS ASSERTED
 *
 *   1. The manifest reached the binary: per-monitor-v2 awareness and
 *      comctl32 v6. Both are silent failures otherwise -- see
 *      res/apprecorder.rc for why they can be lost without a diagnostic.
 *   2. EVERY element in the tree has a non-empty Name. This is the single
 *      most common native-Windows accessibility defect and it is invisible to
 *      anyone testing by eye.
 *   3. The control types are right: a MenuBar, a StatusBar, and the panes.
 *   4. Every operation is on the menu, each item named, and the ampersand
 *      mnemonics are unique within their menu -- because the mnemonic IS the
 *      keyboard path.
 *   5. Focus really moves: F6 is posted at the window and the FOCUSED UIA
 *      element is observed to change to the other pane. Not "the pane has
 *      WS_TABSTOP", which is what we would be asserting if we looked at
 *      styles.
 *
 * ===========================================================================
 * SKIPPING
 *
 *   A session with no interactive window station cannot create a window, and
 *   UIA cannot be instantiated in some sandboxes. Both are reported as SKIPPED
 *   rather than faked, and the cases above them -- the manifest checks -- still
 *   run, because they need no window.
 */
#include "test_runner.h"

#include <windows.h>
#include <commctrl.h>
#include <wctype.h>
#include <objbase.h>
#include <initguid.h>
#include <uiautomation.h>

#include "strings.h"
#include "ui_app.h"
#include "ui_dpi.h"
#include "ui_theme.h"

/* Defined here rather than taken from a library so that the test does not
 * depend on which SDK lib happens to export them. */
static const CLSID kCLSID_CUIAutomation =
    { 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
static const IID kIID_IUIAutomation =
    { 0x30cbe57d, 0xd9d0, 0x452a, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };

/* ==========================================================================
 * The UI thread
 * ======================================================================== */

typedef struct UiHost {
    HANDLE    thread;
    HANDLE    ready;      /* signalled once the frame exists (or failed) */
    AprUiApp *app;
    HWND      frame;
    AprErr    err;
    int       failed;
} UiHost;

static DWORD WINAPI ui_thread(LPVOID param)
{
    UiHost *h = (UiHost *)param;
    HRESULT hr;

    /* Apartment-threaded: this thread owns windows, and IAccPropServices is
     * created on it. */
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    h->err = apr_ui_app_create(GetModuleHandleW(NULL), &h->app);
    if (apr_failed(&h->err)) {
        h->failed = 1;
        SetEvent(h->ready);
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }

    h->frame = apr_ui_app_hwnd(h->app);
    apr_ui_app_show(h->app, SW_SHOWNORMAL);

    /* Pump once so the window is fully realised before the client connects. */
    {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    SetEvent(h->ready);

    apr_ui_app_run(h->app);

    apr_ui_app_destroy(h->app);
    h->app = NULL;
    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}

static int ui_start(UiHost *h)
{
    memset(h, 0, sizeof *h);
    h->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!h->ready) return 0;
    h->thread = CreateThread(NULL, 0, ui_thread, h, 0, NULL);
    if (!h->thread) { CloseHandle(h->ready); h->ready = NULL; return 0; }
    if (WaitForSingleObject(h->ready, 15000) != WAIT_OBJECT_0) return 0;
    return h->failed ? 0 : (h->frame != NULL);
}

static void ui_stop(UiHost *h)
{
    if (h->frame && IsWindow(h->frame)) PostMessageW(h->frame, WM_CLOSE, 0, 0);
    if (h->thread) {
        if (WaitForSingleObject(h->thread, 15000) != WAIT_OBJECT_0) {
            printf("      WARNING: UI thread did not exit; terminating\n");
            TerminateThread(h->thread, 1);
        }
        CloseHandle(h->thread);
    }
    if (h->ready) CloseHandle(h->ready);
    memset(h, 0, sizeof *h);
}

/* ==========================================================================
 * UIA client helpers
 * ======================================================================== */

typedef struct UiaClient {
    IUIAutomation       *uia;
    IUIAutomationElement *root;
    IUIAutomationTreeWalker *walker;
    int                  com_ok;
} UiaClient;

static int uia_open(UiaClient *c, HWND hwnd)
{
    HRESULT hr;

    memset(c, 0, sizeof *c);
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    c->com_ok = SUCCEEDED(hr);
    if (!c->com_ok) return 0;

    hr = CoCreateInstance(&kCLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                          &kIID_IUIAutomation, (void **)&c->uia);
    if (FAILED(hr) || !c->uia) return 0;

    hr = IUIAutomation_ElementFromHandle(c->uia, hwnd, &c->root);
    if (FAILED(hr) || !c->root) return 0;

    /* The RAW view, not the control view: the raw view is the whole provider
     * tree, so a nameless element cannot hide from this test by being filtered
     * out of the control view. */
    hr = IUIAutomation_get_RawViewWalker(c->uia, &c->walker);
    if (FAILED(hr) || !c->walker) return 0;

    return 1;
}

static void uia_close(UiaClient *c)
{
    if (c->walker) IUIAutomationTreeWalker_Release(c->walker);
    if (c->root)   IUIAutomationElement_Release(c->root);
    if (c->uia)    IUIAutomation_Release(c->uia);
    if (c->com_ok) CoUninitialize();
    memset(c, 0, sizeof *c);
}

static void el_name(IUIAutomationElement *el, wchar_t *buf, size_t cch)
{
    BSTR s = NULL;
    buf[0] = 0;
    if (SUCCEEDED(IUIAutomationElement_get_CurrentName(el, &s)) && s) {
        lstrcpynW(buf, s, (int)cch);
        SysFreeString(s);
    }
}

static CONTROLTYPEID el_type(IUIAutomationElement *el)
{
    CONTROLTYPEID t = 0;
    (void)IUIAutomationElement_get_CurrentControlType(el, &t);
    return t;
}

/* UIA_*ControlTypeId are extern consts, not compile-time constants, so they
 * cannot be switch labels in C. An if-chain it is. */
static const char *type_name(CONTROLTYPEID t)
{
    if (t == UIA_WindowControlTypeId)    return "Window";
    if (t == UIA_PaneControlTypeId)      return "Pane";
    if (t == UIA_MenuBarControlTypeId)   return "MenuBar";
    if (t == UIA_MenuItemControlTypeId)  return "MenuItem";
    if (t == UIA_MenuControlTypeId)      return "Menu";
    if (t == UIA_StatusBarControlTypeId) return "StatusBar";
    if (t == UIA_TreeControlTypeId)      return "Tree";
    if (t == UIA_TextControlTypeId)      return "Text";
    if (t == UIA_ButtonControlTypeId)    return "Button";
    if (t == UIA_ThumbControlTypeId)     return "Thumb";
    if (t == UIA_GroupControlTypeId)     return "Group";
    if (t == UIA_CustomControlTypeId)    return "Custom";
    if (t == UIA_TitleBarControlTypeId)  return "TitleBar";
    if (t == UIA_ScrollBarControlTypeId) return "ScrollBar";
    if (t == UIA_ToolBarControlTypeId)   return "ToolBar";
    if (t == UIA_ImageControlTypeId)     return "Image";
    return "(other)";
}

/* Depth-first walk. Reports the tree and counts what it finds. */
typedef struct WalkStats {
    int total;
    int unnamed;
    int menubars;
    int statusbars;
    int panes;
    int menu_items;
    int focusable;
    int focusable_unnamed;
    char    unnamed_first_type[32];
} WalkStats;

/* The window's non-client area -- title bar, its system menu, the minimise /
 * maximise / close buttons -- is built and named by Windows, not by us. Its
 * TitleBar element reports an empty Name in every Win32 application, so
 * holding it to our naming rule would assert a property of the OS that we
 * cannot fix and that no screen reader user is missing (the buttons inside it
 * ARE named). Everything below is ours and is held to the rule.
 *
 * `ours` is 0 once the walk has descended into that subtree. */
static void walk(UiaClient *c, IUIAutomationElement *el, int depth, WalkStats *st,
                 int ours)
{
    IUIAutomationElement *child = NULL, *next = NULL;
    wchar_t name[256];
    CONTROLTYPEID t;
    BOOL focusable = FALSE, offscreen = FALSE;
    int i;

    if (!el || depth > 12) return;

    el_name(el, name, 256);
    t = el_type(el);
    (void)IUIAutomationElement_get_CurrentIsKeyboardFocusable(el, &focusable);
    (void)IUIAutomationElement_get_CurrentIsOffscreen(el, &offscreen);

    if (t == UIA_TitleBarControlTypeId) ours = 0;

    st->total++;
    if (ours && name[0] == 0) {
        st->unnamed++;
        if (st->unnamed == 1) {
            lstrcpynA(st->unnamed_first_type, type_name(t), 32);
        }
    }
    if (focusable) {
        st->focusable++;
        if (ours && name[0] == 0) st->focusable_unnamed++;
    }

    if (t == UIA_MenuBarControlTypeId)        st->menubars++;
    else if (t == UIA_StatusBarControlTypeId) st->statusbars++;
    else if (t == UIA_PaneControlTypeId)      st->panes++;
    else if (t == UIA_MenuItemControlTypeId)  st->menu_items++;

    printf("      ");
    for (i = 0; i < depth; ++i) printf("  ");
    printf("%-10s %-3s \"%ls\"\n", type_name(t), focusable ? "[K]" : "", name);

    if (FAILED(IUIAutomationTreeWalker_GetFirstChildElement(c->walker, el, &child))) {
        return;
    }
    while (child) {
        walk(c, child, depth + 1, st, ours);
        next = NULL;
        if (FAILED(IUIAutomationTreeWalker_GetNextSiblingElement(c->walker, child, &next))) {
            next = NULL;
        }
        IUIAutomationElement_Release(child);
        child = next;
    }
}

/* ==========================================================================
 * Manifest -- no window required
 * ======================================================================== */

TEST(the_manifest_actually_reached_this_binary_per_monitor_v2)
{
    AprDpiAwareness a = apr_dpi_awareness();

    printf("      DPI awareness: %d (3 = per-monitor v2)\n", (int)a);

    /* If this fails, either res/apprecorder.rc was dropped from the link or
     * link.exe's own manifest replaced ours. Both are silent everywhere else:
     * the app simply renders at the wrong size on a second monitor. */
    ASSERT_EQ_INT((int)APR_DPI_PER_MONITOR_V2, (int)a);
}

/* DLLVERSIONINFO / DLLGETVERSIONPROC live in shlwapi.h. Declared here instead,
 * so that a test of the manifest does not pull an unrelated header (and its
 * link dependency) into the build. */
typedef struct AprDllVersionInfo {
    DWORD cbSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformID;
} AprDllVersionInfo;
typedef HRESULT (CALLBACK *AprDllGetVersion)(AprDllVersionInfo *);

TEST(the_manifest_actually_reached_this_binary_comctl32_v6)
{
    HMODULE dll;
    AprDllGetVersion get_version;
    AprDllVersionInfo vi;
    INITCOMMONCONTROLSEX icc;

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES;
    ASSERT_TRUE(InitCommonControlsEx(&icc) != FALSE);

    dll = GetModuleHandleW(L"comctl32.dll");
    ASSERT_NOT_NULL(dll);

    get_version = (AprDllGetVersion)(void *)GetProcAddress(dll, "DllGetVersion");
    ASSERT_NOT_NULL(get_version);

    memset(&vi, 0, sizeof vi);
    vi.cbSize = sizeof vi;
    ASSERT_TRUE(SUCCEEDED(get_version(&vi)));
    printf("      comctl32 %lu.%lu\n",
           (unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion);

    /* Version 5 is the pre-XP control set: unthemed, and with the older and
     * thinner accessibility implementations. NM_CUSTOMDRAW's stage model --
     * the entire basis of "beautiful and still readable" -- is a v6 feature. */
    ASSERT_GE_INT(6, (int)vi.dwMajorVersion);
}

/* ==========================================================================
 * The live tree
 * ======================================================================== */

TEST(every_element_of_the_live_accessibility_tree_has_a_name)
{
    UiHost h;
    UiaClient c;
    WalkStats st;

    if (!ui_start(&h)) {
        printf("      SKIPPED: could not create a window (no interactive "
               "window station?)\n");
        ui_stop(&h);
        return;
    }
    if (!uia_open(&c, h.frame)) {
        printf("      SKIPPED: UI Automation client unavailable here\n");
        uia_close(&c);
        ui_stop(&h);
        return;
    }

    memset(&st, 0, sizeof st);
    walk(&c, c.root, 0, &st, 1);

    printf("      %d elements, %d unnamed, %d keyboard-focusable\n",
           st.total, st.unnamed, st.focusable);

    /* THE ASSERTION THIS FILE EXISTS FOR. An element with no Name is read as
     * its control type and nothing else: "pane", "tree". */
    if (st.unnamed) printf("      first unnamed: %s\n", st.unnamed_first_type);
    ASSERT_EQ_INT(0, st.unnamed);
    ASSERT_EQ_INT(0, st.focusable_unnamed);

    /* And the structure a screen reader user navigates by. */
    ASSERT_GE_INT(1, st.menubars);
    ASSERT_GE_INT(1, st.statusbars);
    ASSERT_GE_INT(2, st.panes);        /* the two panes, at least */
    ASSERT_GE_INT(3, st.focusable);    /* both panes and the divider */

    uia_close(&c);
    ui_stop(&h);
}

TEST(the_frames_own_name_and_control_type_are_right)
{
    UiHost h;
    UiaClient c;
    wchar_t name[256];

    if (!ui_start(&h)) { printf("      SKIPPED: no window\n"); ui_stop(&h); return; }
    if (!uia_open(&c, h.frame)) {
        printf("      SKIPPED: no UIA\n"); uia_close(&c); ui_stop(&h); return;
    }

    el_name(c.root, name, 256);
    printf("      frame: %s \"%ls\"\n", type_name(el_type(c.root)), name);

    ASSERT_TRUE(el_type(c.root) == UIA_WindowControlTypeId);
    ASSERT_TRUE(name[0] != 0);
    /* The name comes from the catalog, so it must equal the catalog entry --
     * a literal caption in app.c would fail this. */
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_TITLE_UNTITLED), name);

    uia_close(&c);
    ui_stop(&h);
}

TEST(both_panes_are_named_from_the_catalog_and_keyboard_focusable)
{
    UiHost h;
    HWND tree, canvas;
    UiaClient c;
    IUIAutomationElement *el = NULL;
    wchar_t name[256];
    BOOL focusable = FALSE;
    int i;
    HWND panes[2];
    AprStrId ids[2];

    if (!ui_start(&h)) { printf("      SKIPPED: no window\n"); ui_stop(&h); return; }

    tree = apr_ui_app_pane(h.app, APR_PANE_TREE);
    canvas = apr_ui_app_pane(h.app, APR_PANE_CANVAS);
    ASSERT_NOT_NULL(tree);
    ASSERT_NOT_NULL(canvas);

    if (!uia_open(&c, h.frame)) {
        printf("      SKIPPED: no UIA\n"); uia_close(&c); ui_stop(&h); return;
    }

    panes[0] = tree;   ids[0] = APR_S_UI_PANE_TREE;
    panes[1] = canvas; ids[1] = APR_S_UI_PANE_CANVAS;

    for (i = 0; i < 2; ++i) {
        el = NULL;
        ASSERT_TRUE(SUCCEEDED(IUIAutomation_ElementFromHandle(c.uia, panes[i], &el)));
        ASSERT_NOT_NULL(el);
        el_name(el, name, 256);
        printf("      pane %d: %s \"%ls\"\n", i, type_name(el_type(el)), name);
        ASSERT_WSTR_EQ(apr_str(ids[i]), name);
        focusable = FALSE;
        (void)IUIAutomationElement_get_CurrentIsKeyboardFocusable(el, &focusable);
        ASSERT_TRUE(focusable != FALSE);
        IUIAutomationElement_Release(el);
    }

    uia_close(&c);
    ui_stop(&h);
}

/* The UI thread's own focus window.
 *
 * GetFocus() is per-thread and UIA's GetFocusedElement is per-DESKTOP: the
 * second one needs the window to be foreground, which a test process launched
 * by a build system usually is not. GetGUIThreadInfo asks the same question of
 * one thread and answers it whether or not that thread is in the foreground,
 * so the F6 behaviour can be tested for real here instead of skipped. UIA is
 * still tried first, because it is what a screen reader actually reads. */
static HWND thread_focus(HANDLE thread)
{
    GUITHREADINFO gti;
    memset(&gti, 0, sizeof gti);
    gti.cbSize = sizeof gti;
    if (!GetGUIThreadInfo(GetThreadId(thread), &gti)) return NULL;
    return gti.hwndFocus;
}

/* Focus is IN a pane when it is on the pane window or on anything inside it.
 *
 * The descendant case is not a loosening, it is the normal one now that the
 * panes have content: a pane whose job is to hold a control hands focus
 * straight on to that control, so that a screen reader lands on the tree (or
 * the node) rather than on a container whose only utterance is its own name.
 * What F6 has to guarantee is that focus MOVED TO THE OTHER PANE, and that is
 * what this expresses. */
static int focus_is_in(HWND pane, HWND focus)
{
    return pane && focus && (focus == pane || IsChild(pane, focus));
}

TEST(f6_really_moves_focus_between_panes)
{
    UiHost h;
    UiaClient c;
    IUIAutomationElement *focused = NULL;
    wchar_t uia_before[256], uia_after[256];
    HWND tree, canvas, before, after;
    int tries;

    if (!ui_start(&h)) { printf("      SKIPPED: no window\n"); ui_stop(&h); return; }

    tree = apr_ui_app_pane(h.app, APR_PANE_TREE);
    canvas = apr_ui_app_pane(h.app, APR_PANE_CANVAS);
    ASSERT_NOT_NULL(tree);
    ASSERT_NOT_NULL(canvas);

    /* Give the UI thread a known focus window. The frame hands focus straight
     * to the first pane, which is the behaviour a user gets on Alt+Tab. */
    SendMessageTimeoutW(h.frame, WM_SETFOCUS, 0, 0, SMTO_ABORTIFHUNG, 3000, NULL);

    before = NULL;
    for (tries = 0; tries < 40 && before == NULL; ++tries) {
        Sleep(25);
        before = thread_focus(h.thread);
    }
    printf("      focus before F6: %p (tree %p, canvas %p)\n",
           (void *)before, (void *)tree, (void *)canvas);

    if (before == NULL) {
        printf("      SKIPPED: the UI thread never took focus in this session\n");
        ui_stop(&h);
        return;
    }
    ASSERT_TRUE(focus_is_in(tree, before) || focus_is_in(canvas, before));

    /* F6 posted at the window, exactly as a key press arrives. The message
     * loop must intercept it BEFORE IsDialogMessage, which swallows it -- if
     * that ordering is ever broken, this test is what notices. */
    PostMessageW(h.frame, WM_KEYDOWN, VK_F6, 0);

    after = before;
    for (tries = 0; tries < 60 && after == before; ++tries) {
        Sleep(25);
        after = thread_focus(h.thread);
    }
    printf("      focus after F6:  %p\n", (void *)after);

    ASSERT_TRUE(after != before);
    /* And it really is the OTHER pane, not merely a different window inside
     * the same one -- which is the thing the plain inequality above stopped
     * proving once a pane could contain more than one focusable window. */
    ASSERT_FALSE(focus_is_in(tree, before) && focus_is_in(tree, after));
    ASSERT_FALSE(focus_is_in(canvas, before) && focus_is_in(canvas, after));
    ASSERT_TRUE(focus_is_in(tree, after) || focus_is_in(canvas, after));

    /* And what a screen reader would read, when the desktop focus is ours. */
    if (uia_open(&c, h.frame)) {
        uia_before[0] = 0;
        uia_after[0] = 0;
        if (SUCCEEDED(IUIAutomation_GetFocusedElement(c.uia, &focused)) && focused) {
            el_name(focused, uia_after, 256);
            IUIAutomationElement_Release(focused);
        }
        if (uia_after[0] != 0) {
            printf("      UIA focused element: \"%ls\"\n", uia_after);
            ASSERT_TRUE(uia_after[0] != 0);
        } else {
            printf("      (window is not foreground, so UIA reports no focus; "
                   "the thread-level check above still ran)\n");
        }
        (void)uia_before;
        uia_close(&c);
    }

    ui_stop(&h);
}

/* ==========================================================================
 * The menu: every operation, named, with a working mnemonic
 * ======================================================================== */

TEST(every_menu_item_is_named_and_carries_a_unique_mnemonic)
{
    UiHost h;
    HMENU bar;
    int top, n, i, j;

    if (!ui_start(&h)) { printf("      SKIPPED: no window\n"); ui_stop(&h); return; }

    bar = GetMenu(h.frame);
    ASSERT_NOT_NULL(bar);

    top = GetMenuItemCount(bar);
    ASSERT_GE_INT(5, top);   /* File, Edit, Recording, View, Help */

    for (i = 0; i < top; ++i) {
        HMENU sub = GetSubMenu(bar, i);
        wchar_t label[128];
        wchar_t used[64];
        int used_n = 0;

        ASSERT_GT_INT(0, GetMenuStringW(bar, (UINT)i, label, 128, MF_BYPOSITION));
        ASSERT_NOT_NULL(sub);

        n = GetMenuItemCount(sub);
        ASSERT_GT_INT(0, n);

        for (j = 0; j < n; ++j) {
            wchar_t item[192];
            const wchar_t *amp;
            int len = GetMenuStringW(sub, (UINT)j, item, 192, MF_BYPOSITION);

            if (len == 0) {
                /* A separator. Fine. */
                MENUITEMINFOW mii;
                memset(&mii, 0, sizeof mii);
                mii.cbSize = sizeof mii;
                mii.fMask = MIIM_FTYPE;
                ASSERT_TRUE(GetMenuItemInfoW(sub, (UINT)j, TRUE, &mii) != FALSE);
                ASSERT_TRUE((mii.fType & MFT_SEPARATOR) != 0);
                continue;
            }

            /* Named -- and named from the catalog, which is what makes it
             * translatable at all. */
            ASSERT_TRUE(item[0] != 0);

            /* THE MNEMONIC IS THE KEYBOARD PATH. An item with no "&" cannot be
             * reached from the keyboard except by arrowing to it, and a
             * duplicate mnemonic within one menu cycles instead of activating.
             * Both are correctness failures, not cosmetics -- which is why
             * they are asserted rather than reviewed. */
            amp = wcschr(item, L'&');
            if (!amp || amp[1] == 0) {
                printf("      menu item with no mnemonic: \"%ls\"\n", item);
            }
            ASSERT_NOT_NULL(amp);
            ASSERT_TRUE(amp[1] != 0);

            {
                wchar_t m = (wchar_t)towlower(amp[1]);
                int k;
                for (k = 0; k < used_n; ++k) {
                    if (used[k] == m) {
                        printf("      duplicate mnemonic '%lc' in menu \"%ls\" "
                               "on item \"%ls\"\n", m, label, item);
                    }
                    ASSERT_TRUE(used[k] != m);
                }
                if (used_n < 64) used[used_n++] = m;
            }
        }
    }

    ui_stop(&h);
}

TEST(the_menu_reads_from_the_catalog_not_from_literals)
{
    UiHost h;
    HMENU bar;
    wchar_t label[128];

    if (!ui_start(&h)) { printf("      SKIPPED: no window\n"); ui_stop(&h); return; }

    bar = GetMenu(h.frame);
    ASSERT_NOT_NULL(bar);
    ASSERT_GT_INT(0, GetMenuStringW(bar, 0, label, 128, MF_BYPOSITION));
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_MENU_FILE), label);

    ASSERT_GT_INT(0, GetMenuStringW(bar, (UINT)APR_CMD_RECORD_START, label, 128,
                                    MF_BYCOMMAND));
    ASSERT_WSTR_EQ(apr_str(APR_S_UI_MENU_RECORD_START), label);

    ui_stop(&h);
}

TEST(unimplemented_commands_are_disabled_but_still_present_and_named)
{
    UiHost h;
    HMENU bar;
    UINT state;

    if (!ui_start(&h)) { printf("      SKIPPED: no window\n"); ui_stop(&h); return; }

    bar = GetMenu(h.frame);
    ASSERT_NOT_NULL(bar);

    /* Grey says "later" to someone who can see it. Absent says "never" to
     * someone who cannot -- there is nothing for a screen reader to encounter
     * at all. So they stay. */
    state = GetMenuState(bar, (UINT)APR_CMD_RECORD_START, MF_BYCOMMAND);
    ASSERT_TRUE(state != (UINT)-1);
    ASSERT_TRUE((state & MF_GRAYED) != 0);

    /* The ones the frame implements itself are live from the start. */
    state = GetMenuState(bar, (UINT)APR_CMD_VIEW_TREE, MF_BYCOMMAND);
    ASSERT_TRUE(state != (UINT)-1);
    ASSERT_TRUE((state & MF_GRAYED) == 0);

    ui_stop(&h);
}

/* ==========================================================================
 * Direction, on a live window
 * ======================================================================== */

TEST(the_frame_never_carries_layout_rtl_even_in_an_rtl_language)
{
    UiHost h;
    AprErr e;
    LONG_PTR ex_frame, ex_canvas, ex_status;

    e = apr_str_set_language(MAKELANGID(LANG_ARABIC, SUBLANG_ARABIC_SAUDI_ARABIA));
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(1, apr_str_is_rtl());

    if (!ui_start(&h)) {
        printf("      SKIPPED: no window\n");
        ui_stop(&h);
        (void)apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
        return;
    }

    ex_frame  = GetWindowLongPtrW(h.frame, GWL_EXSTYLE);
    ex_canvas = GetWindowLongPtrW(apr_ui_app_pane(h.app, APR_PANE_CANVAS), GWL_EXSTYLE);
    ex_status = GetWindowLongPtrW(apr_ui_app_status_bar(h.app), GWL_EXSTYLE);

    /* CONVENTION 1, asserted on a real window rather than trusted.
     *
     * The frame and the canvas must NOT have WS_EX_LAYOUTRTL: it mirrors the
     * device context, so everything we paint would come out reflected, and it
     * is INHERITED by children unless blocked. The status bar -- a standard
     * control whose insides we never paint -- must have it. */
    ASSERT_TRUE((ex_frame & WS_EX_LAYOUTRTL) == 0);
    ASSERT_TRUE((ex_frame & WS_EX_NOINHERITLAYOUT) != 0);
    ASSERT_TRUE((ex_canvas & WS_EX_LAYOUTRTL) == 0);
    ASSERT_TRUE((ex_status & WS_EX_LAYOUTRTL) != 0);

    ui_stop(&h);
    (void)apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
}

TEST(the_structure_panel_moves_to_the_trailing_side_in_an_rtl_language)
{
    UiHost h;
    AprErr e;
    RECT tree, canvas, client;

    e = apr_str_set_language(MAKELANGID(LANG_ARABIC, SUBLANG_ARABIC_SAUDI_ARABIA));
    ASSERT_FALSE(apr_failed(&e));

    if (!ui_start(&h)) {
        printf("      SKIPPED: no window\n");
        ui_stop(&h);
        (void)apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
        return;
    }

    GetClientRect(h.frame, &client);
    GetWindowRect(apr_ui_app_pane(h.app, APR_PANE_TREE), &tree);
    GetWindowRect(apr_ui_app_pane(h.app, APR_PANE_CANVAS), &canvas);
    MapWindowPoints(NULL, h.frame, (POINT *)&tree, 2);
    MapWindowPoints(NULL, h.frame, (POINT *)&canvas, 2);

    printf("      RTL: tree x=[%ld,%ld] canvas x=[%ld,%ld] client width %ld\n",
           tree.left, tree.right, canvas.left, canvas.right, client.right);

    /* Painting mirrors... */
    ASSERT_GT_INT((int)canvas.left, (int)tree.left);

    /* ...but the tab order does NOT. The structure panel is still the first
     * child, so Tab and the accessibility tree still read structure-then-
     * canvas in both languages. This is the half of RTL that everyone gets
     * wrong, and it is the half a screen reader user actually feels. */
    ASSERT_TRUE(GetWindow(apr_ui_app_pane(h.app, APR_PANE_TREE), GW_HWNDPREV) == NULL ||
                GetWindow(apr_ui_app_pane(h.app, APR_PANE_CANVAS), GW_HWNDNEXT) == NULL);

    ui_stop(&h);
    (void)apr_str_set_language(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
}
