/*
 * join.c -- see include/join.h. Two functions, and the reason they are not
 * open-coded at each of the four sites is that the failure mode is a wait
 * result nobody looked at.
 */
#include "join.h"

AprJoin apr_join_wait(HANDLE h, DWORD timeout_ms)
{
    if (!h) return APR_JOIN_EXITED;
    return WaitForSingleObject(h, timeout_ms) == WAIT_OBJECT_0
               ? APR_JOIN_EXITED
               : APR_JOIN_ABANDONED;
}

AprJoin apr_join_wait2(HANDLE a, HANDLE b, DWORD timeout_ms)
{
    HANDLE w[2];
    DWORD  n = 0, r;

    if (a) w[n++] = a;
    if (b) w[n++] = b;
    if (n == 0) return APR_JOIN_EXITED;

    r = WaitForMultipleObjects(n, w, FALSE, timeout_ms);
    return (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + n) ? APR_JOIN_EXITED
                                                         : APR_JOIN_ABANDONED;
}
