/*
 * test_runner.h -- apprecorder's single-header test harness.
 *
 * No external framework: the project targets a sub-1MB binary and a build that
 * needs nothing but the Windows SDK, and the test tree is held to the same bar.
 *
 * USAGE
 *
 *     #include "test_runner.h"
 *     #include "ringbuf.h"
 *
 *     TEST(write_then_read_roundtrips)
 *     {
 *         ASSERT_NOT_NULL(rb);
 *         ASSERT_EQ_INT(4, rb_read(&rd, buf, 4, NULL));
 *     }
 *
 * That is the whole API surface for authors. `main` is supplied by this header;
 * every TEST() in the translation unit self-registers before main runs (via a
 * CRT static-initializer slot), so there is no list to keep up to date and no
 * way to write a test that silently never runs.
 *
 * CMake builds one executable per tests/test_*.c, so a hang or a crash in one
 * file cannot hide the results of another.
 *
 * SEMANTICS
 *
 *   - An ASSERT_* failure prints file(line), the expression, and the expected
 *     vs. actual values, then RETURNS from the test case. Later assertions in
 *     that case do not run (they would typically fault). Other cases still run.
 *   - The process exit code is the number of failed cases, capped at 125, so
 *     ctest reports failure.
 *   - `test_xxx.exe <substring>` runs only the cases whose name contains
 *     <substring>. `test_xxx.exe --list` prints the case names.
 *
 * THREAD SAFETY: assertions must be evaluated on the thread running the case.
 * Worker threads should record failures into variables the case asserts on
 * after joining.
 */
#ifndef APPRECORDER_TEST_RUNNER_H
#define APPRECORDER_TEST_RUNNER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <math.h>
#include <time.h>

/* This header defines helpers that any single test file will only partly use.
 * C4505 (unreferenced static function removed) is expected and not a defect. */
#pragma warning(disable : 4505)

#ifndef TR_MAX_TESTS
#define TR_MAX_TESTS 256
#endif

typedef void (*TrTestFn)(void);

typedef struct TrTest {
    const char *name;
    TrTestFn    fn;
} TrTest;

static TrTest g_tr_tests[TR_MAX_TESTS];
static int    g_tr_test_count;
static int    g_tr_case_failed;   /* failing assertions in the running case */
static long   g_tr_case_asserts;  /* assertions executed in the running case */

static void tr_register(const char *name, TrTestFn fn)
{
    if (g_tr_test_count >= TR_MAX_TESTS) {
        printf("test_runner: more than TR_MAX_TESTS (%d) tests in one file\n",
               TR_MAX_TESTS);
        exit(2);
    }
    g_tr_tests[g_tr_test_count].name = name;
    g_tr_tests[g_tr_test_count].fn   = fn;
    g_tr_test_count++;
}

/* ------------------------------------------------------------------------
 * Self-registration.
 *
 * A pointer parked in .CRT$XCU is called by the CRT before main. The
 * /include: pragma pins the symbol so /OPT:REF cannot drop it in a Release
 * test build -- without it a Release run would silently execute zero tests.
 * main() additionally fails outright when nothing registered.
 * ------------------------------------------------------------------------ */
#pragma section(".CRT$XCU", read)

#if defined(_M_IX86)
#define TR_SYM_PREFIX "_"        /* x86 decorates cdecl symbols with '_' */
#else
#define TR_SYM_PREFIX ""
#endif

#define TR_CTOR(fn)                                                       \
    static void fn(void);                                                 \
    __declspec(allocate(".CRT$XCU")) void (*fn##_ptr)(void) = fn;         \
    __pragma(comment(linker, "/include:" TR_SYM_PREFIX #fn "_ptr"))       \
    static void fn(void)

/* Declare a test case. The body follows in braces. */
#define TEST(name)                                                        \
    static void tr_case_##name(void);                                     \
    TR_CTOR(tr_ctor_##name) { tr_register(#name, tr_case_##name); }       \
    static void tr_case_##name(void)

/* ------------------------------------------------------------------------
 * Failure reporting. MSVC "file(line):" form so editors can jump to it.
 * ------------------------------------------------------------------------ */

static void tr_fail_head(const char *file, int line, const char *expr)
{
    g_tr_case_failed++;
    printf("%s(%d): FAILED  %s\n", file, line, expr);
}

static void tr_fail_ll(const char *file, int line, const char *expr,
                       const char *relation, long long expected, long long actual)
{
    tr_fail_head(file, line, expr);
    printf("      expected: %s%lld\n", relation, expected);
    printf("        actual: %lld\n", actual);
}

static void tr_fail_ull(const char *file, int line, const char *expr,
                        const char *relation, unsigned long long expected,
                        unsigned long long actual)
{
    tr_fail_head(file, line, expr);
    printf("      expected: %s%llu\n", relation, expected);
    printf("        actual: %llu\n", actual);
}

static void tr_fail_dbl(const char *file, int line, const char *expr,
                        double expected, double actual, double eps)
{
    tr_fail_head(file, line, expr);
    printf("      expected: %.17g (+/- %.17g)\n", expected, eps);
    printf("        actual: %.17g  (differs by %.17g)\n",
           actual, fabs(actual - expected));
}

static void tr_fail_ptr(const char *file, int line, const char *expr,
                        const char *expected, const void *actual)
{
    tr_fail_head(file, line, expr);
    printf("      expected: %s\n", expected);
    printf("        actual: %p\n", actual);
}

static void tr_fail_str(const char *file, int line, const char *expr,
                        const char *expected, const char *actual)
{
    tr_fail_head(file, line, expr);
    printf("      expected: [%s]\n", expected ? expected : "(null)");
    printf("        actual: [%s]\n", actual ? actual : "(null)");
}

static void tr_fail_wstr(const char *file, int line, const char *expr,
                         const wchar_t *expected, const wchar_t *actual)
{
    tr_fail_head(file, line, expr);
    printf("      expected: [%ls]\n", expected ? expected : L"(null)");
    printf("        actual: [%ls]\n", actual ? actual : L"(null)");
}

/* Reports the first differing byte, which is what actually locates a bug in a
 * PCM buffer -- a hex dump of 4096 bytes does not. */
static void tr_fail_mem(const char *file, int line, const char *expr,
                        const void *expected, const void *actual, size_t n)
{
    const unsigned char *e = (const unsigned char *)expected;
    const unsigned char *a = (const unsigned char *)actual;
    size_t i;
    tr_fail_head(file, line, expr);
    if (!e || !a) {
        printf("      one of the buffers is NULL (expected=%p actual=%p)\n",
               expected, actual);
        return;
    }
    for (i = 0; i < n; i++) {
        if (e[i] != a[i]) {
            printf("      %zu bytes compared; first difference at offset %zu\n", n, i);
            printf("      expected: 0x%02X\n", e[i]);
            printf("        actual: 0x%02X\n", a[i]);
            return;
        }
    }
    printf("      %zu bytes compared; buffers are equal (length mismatch?)\n", n);
}

/* ------------------------------------------------------------------------
 * Assertions. Each returns from the enclosing test case on failure.
 * ------------------------------------------------------------------------ */

#define TR_COUNT() (g_tr_case_asserts++)

#define ASSERT_TRUE(expr)                                                  \
    do { TR_COUNT();                                                       \
         if (!(expr)) {                                                    \
             tr_fail_ll(__FILE__, __LINE__, "ASSERT_TRUE(" #expr ")",      \
                        "", 1, 0);                                         \
             return;                                                       \
         } } while (0)

#define ASSERT_FALSE(expr)                                                 \
    do { TR_COUNT();                                                       \
         if ((expr)) {                                                     \
             tr_fail_ll(__FILE__, __LINE__, "ASSERT_FALSE(" #expr ")",     \
                        "", 0, 1);                                         \
             return;                                                       \
         } } while (0)

#define TR_ASSERT_REL_I(exp, act, op, opname, macro)                       \
    do { long long tr_e = (long long)(exp);                                \
         long long tr_a = (long long)(act);                                \
         TR_COUNT();                                                       \
         if (!(tr_a op tr_e)) {                                            \
             tr_fail_ll(__FILE__, __LINE__,                                \
                        macro "(" #exp ", " #act ")", opname, tr_e, tr_a); \
             return;                                                       \
         } } while (0)

#define ASSERT_EQ_INT(expected, actual) TR_ASSERT_REL_I(expected, actual, ==, "",     "ASSERT_EQ_INT")
#define ASSERT_NE_INT(expected, actual) TR_ASSERT_REL_I(expected, actual, !=, "not ", "ASSERT_NE_INT")
#define ASSERT_LT_INT(bound, actual)    TR_ASSERT_REL_I(bound, actual, <,  "< ",  "ASSERT_LT_INT")
#define ASSERT_LE_INT(bound, actual)    TR_ASSERT_REL_I(bound, actual, <=, "<= ", "ASSERT_LE_INT")
#define ASSERT_GT_INT(bound, actual)    TR_ASSERT_REL_I(bound, actual, >,  "> ",  "ASSERT_GT_INT")
#define ASSERT_GE_INT(bound, actual)    TR_ASSERT_REL_I(bound, actual, >=, ">= ", "ASSERT_GE_INT")

#define ASSERT_EQ_U64(expected, actual)                                    \
    do { unsigned long long tr_e = (unsigned long long)(expected);         \
         unsigned long long tr_a = (unsigned long long)(actual);           \
         TR_COUNT();                                                       \
         if (tr_e != tr_a) {                                               \
             tr_fail_ull(__FILE__, __LINE__,                               \
                         "ASSERT_EQ_U64(" #expected ", " #actual ")",      \
                         "", tr_e, tr_a);                                  \
             return;                                                       \
         } } while (0)

/* Float/double equality within an absolute epsilon. There is no default
 * epsilon on purpose: the right tolerance is a property of the computation,
 * and a hidden default is how a sync test quietly stops testing anything. */
#define ASSERT_NEAR(expected, actual, eps)                                 \
    do { double tr_e = (double)(expected);                                 \
         double tr_a = (double)(actual);                                   \
         double tr_p = (double)(eps);                                      \
         TR_COUNT();                                                       \
         if (!(fabs(tr_a - tr_e) <= tr_p)) {                               \
             tr_fail_dbl(__FILE__, __LINE__,                               \
                         "ASSERT_NEAR(" #expected ", " #actual ", " #eps ")", \
                         tr_e, tr_a, tr_p);                                \
             return;                                                       \
         } } while (0)

#define ASSERT_NULL(ptr)                                                   \
    do { const void *tr_p = (const void *)(ptr);                           \
         TR_COUNT();                                                       \
         if (tr_p != NULL) {                                               \
             tr_fail_ptr(__FILE__, __LINE__, "ASSERT_NULL(" #ptr ")",      \
                         "NULL", tr_p);                                    \
             return;                                                       \
         } } while (0)

#define ASSERT_NOT_NULL(ptr)                                               \
    do { const void *tr_p = (const void *)(ptr);                           \
         TR_COUNT();                                                       \
         if (tr_p == NULL) {                                               \
             tr_fail_ptr(__FILE__, __LINE__, "ASSERT_NOT_NULL(" #ptr ")",  \
                         "non-NULL", tr_p);                                \
             return;                                                       \
         } } while (0)

#define ASSERT_MEM_EQ(expected, actual, nbytes)                            \
    do { const void *tr_e = (const void *)(expected);                      \
         const void *tr_a = (const void *)(actual);                        \
         size_t tr_n = (size_t)(nbytes);                                   \
         TR_COUNT();                                                       \
         if (!tr_e || !tr_a || memcmp(tr_e, tr_a, tr_n) != 0) {            \
             tr_fail_mem(__FILE__, __LINE__,                               \
                         "ASSERT_MEM_EQ(" #expected ", " #actual ", " #nbytes ")", \
                         tr_e, tr_a, tr_n);                                \
             return;                                                       \
         } } while (0)

#define ASSERT_STR_EQ(expected, actual)                                    \
    do { const char *tr_e = (expected);                                    \
         const char *tr_a = (actual);                                      \
         TR_COUNT();                                                       \
         if (!tr_e || !tr_a || strcmp(tr_e, tr_a) != 0) {                  \
             tr_fail_str(__FILE__, __LINE__,                               \
                         "ASSERT_STR_EQ(" #expected ", " #actual ")",      \
                         tr_e, tr_a);                                      \
             return;                                                       \
         } } while (0)

#define ASSERT_WSTR_EQ(expected, actual)                                   \
    do { const wchar_t *tr_e = (expected);                                 \
         const wchar_t *tr_a = (actual);                                   \
         TR_COUNT();                                                       \
         if (!tr_e || !tr_a || wcscmp(tr_e, tr_a) != 0) {                  \
             tr_fail_wstr(__FILE__, __LINE__,                              \
                          "ASSERT_WSTR_EQ(" #expected ", " #actual ")",    \
                          tr_e, tr_a);                                     \
             return;                                                       \
         } } while (0)

/* Unconditional failure, e.g. in a branch that should be unreachable. */
#define FAIL(msg)                                                          \
    do { TR_COUNT();                                                       \
         tr_fail_head(__FILE__, __LINE__, "FAIL(" #msg ")");               \
         printf("      %s\n", (msg));                                      \
         return;                                                           \
    } while (0)

/* ------------------------------------------------------------------------
 * Driver.
 *
 * Every case is timed and the figure is printed beside it. A suite that takes
 * forty seconds says nothing about WHICH case took them, and the reason the UI
 * suites drifted into minutes unnoticed is that nobody could see the
 * distribution without instrumenting by hand. clock() is plenty here: the
 * numbers being read are seconds, not microseconds.
 * ------------------------------------------------------------------------ */

static long tr_millis(void)
{
    return (long)((double)clock() * 1000.0 / (double)CLOCKS_PER_SEC);
}

int main(int argc, char **argv)
{
    const char *filter = NULL;
    int i, ran = 0, failed = 0, list_only = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) list_only = 1;
        else filter = argv[i];
    }

    if (g_tr_test_count == 0) {
        printf("test_runner: no tests registered -- this is a build defect, "
               "not an empty suite.\n");
        return 2;
    }

    if (list_only) {
        for (i = 0; i < g_tr_test_count; i++) printf("%s\n", g_tr_tests[i].name);
        return 0;
    }

    printf("running %d test%s\n", g_tr_test_count,
           g_tr_test_count == 1 ? "" : "s");

    for (i = 0; i < g_tr_test_count; i++) {
        long ms;

        if (filter && strstr(g_tr_tests[i].name, filter) == NULL) continue;
        ran++;
        printf("[ RUN      ] %s\n", g_tr_tests[i].name);
        g_tr_case_failed  = 0;
        g_tr_case_asserts = 0;
        fflush(stdout);
        ms = tr_millis();
        g_tr_tests[i].fn();
        ms = tr_millis() - ms;
        if (g_tr_case_failed) {
            failed++;
            printf("[   FAILED ] %s (%ld assertion%s run, %ld ms)\n",
                   g_tr_tests[i].name, g_tr_case_asserts,
                   g_tr_case_asserts == 1 ? "" : "s", ms);
        } else {
            printf("[       OK ] %s (%ld assertion%s, %ld ms)\n",
                   g_tr_tests[i].name, g_tr_case_asserts,
                   g_tr_case_asserts == 1 ? "" : "s", ms);
        }
        fflush(stdout);
    }

    if (filter && ran == 0) {
        printf("test_runner: filter \"%s\" matched no tests\n", filter);
        return 2;
    }

    printf("\n%d run, %d passed, %d failed\n", ran, ran - failed, failed);
    return failed > 125 ? 125 : failed;
}

#endif /* APPRECORDER_TEST_RUNNER_H */
