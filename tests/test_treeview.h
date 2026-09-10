/*
 * test_treeview.h -- why a row of a live TreeView was never reached.
 *
 * Two suites drive the Structure panel by keyboard and then assert that Down
 * Arrow stood on EVERY model row: tests/test_ui_tree.c over a bare panel and
 * tests/test_ui_behaviour.c with a real controller behind it. Both guard the
 * same regression -- BUGS.md C2, the panel that yanked focus into the canvas
 * on every arrow press, which made everything past the first row unreachable
 * for the author.
 *
 * When such a walk comes up short there are exactly TWO reasons, and they are
 * different defects:
 *
 *   - the control does not hold the row at all (tree_panel.c drops a row whose
 *     label came out empty, which is a product defect), or
 *   - it holds it under a COLLAPSED ancestor. Down walks VISIBLE items, so one
 *     collapsed parent hides a whole subtree -- without moving focus, so the
 *     focus assertions above still pass and only the completeness one fails.
 *     A stray Left arrow does this, and a Left arrow the test did not send is
 *     the desktop reaching into the fixture (tests/test_window.h).
 *
 * "expected 1, actual 0" cannot tell those apart, and the second one reads
 * exactly like the focus-steal regression coming back. So the answer is
 * computed once, here, rather than written out twice.
 */
#ifndef APPRECORDER_TEST_TREEVIEW_H
#define APPRECORDER_TEST_TREEVIEW_H

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

/* The live item carrying this model row index (its lParam), or NULL when the
 * control does not hold that row. */
static HTREEITEM apr_test_tv_item_for_row(HWND tv, int row)
{
    HTREEITEM stack[256];
    size_t    top = 0;
    HTREEITEM it = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_ROOT, 0);

    while (it || top > 0) {
        TVITEMW   q;
        HTREEITEM kid, next;

        if (!it) { it = stack[--top]; continue; }

        memset(&q, 0, sizeof q);
        q.mask  = TVIF_PARAM;
        q.hItem = it;
        if (SendMessageW(tv, TVM_GETITEMW, 0, (LPARAM)&q) &&
            (int)q.lParam == row) {
            return it;
        }

        next = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_NEXT,
                                       (LPARAM)it);
        kid  = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_CHILD,
                                       (LPARAM)it);
        if (kid) {
            if (next && top < 256) stack[top++] = next;
            it = kid;
        } else {
            it = next;
        }
    }
    return NULL;
}

/* Print the reason Down Arrow never stood on `row`. See the header note. */
static void apr_test_tv_explain_unreached(HWND tv, int row)
{
    HTREEITEM it = apr_test_tv_item_for_row(tv, row);
    HTREEITEM up;

    if (!it) {
        printf("      row %d was never reached: the control does not hold it "
               "at all\n", row);
        return;
    }

    for (up = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_PARENT,
                                      (LPARAM)it);
         up;
         up = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_PARENT,
                                      (LPARAM)up)) {
        UINT state = (UINT)SendMessageW(tv, TVM_GETITEMSTATE, (WPARAM)up,
                                        (LPARAM)TVIS_EXPANDED);
        if (!(state & TVIS_EXPANDED)) {
            printf("      row %d was never reached: an ancestor of it is "
                   "COLLAPSED, so Down cannot walk into it -- something "
                   "outside this test pressed Left or clicked on the tree\n",
                   row);
            return;
        }
    }

    printf("      row %d was never reached, and it is present with every "
           "ancestor expanded -- the caret walk itself did not get there\n",
           row);
}

#endif /* APPRECORDER_TEST_TREEVIEW_H */
