/*
 * test_outpath.c -- where a recording is saved, and what its name is.
 *
 * ===========================================================================
 * THE BUG UNDERNEATH THIS FILE
 *
 *   "I recorded a file called test.mp3, stopped, listened to it, and then
 *   recorded again and stopped. Turns out the file was not updated with the
 *   new recording, so I deleted it and recorded again, but the new file wasn't
 *   there."
 *
 *   Two defects met there. The first was a LIFETIME -- the encoder was created
 *   when the output was added and finalized for good at the first stop -- and
 *   it is bus.c's, tested through the graph. The second is this module's: even
 *   with the lifetime right, a second take saved to the same name would have
 *   overwritten the first, and silently.
 *
 *   So the property under test here is not "the name is right". It is THE
 *   EARLIER TAKE IS STILL THERE AFTERWARDS. Every collision case below asserts
 *   that as well as asserting the new name.
 *
 * ===========================================================================
 * SAFETY (AGENTS.md rule 1)
 *
 *   Nothing here opens an audio device, renders a sample, or plays anything.
 *   It is file-system arithmetic. Every file it creates is under the user's
 *   TEMP folder and is deleted before the case returns, including the ones the
 *   writability probe is supposed to clean up after itself -- because whether
 *   it does is one of the things being checked.
 */
#include "test_runner.h"

#include <windows.h>

#include "outpath.h"

#define CCH APR_OUT_PATH_CCH

/* ---------------------------------------------------------------------------
 * Scratch files
 * ------------------------------------------------------------------------- */

static void tmp_dir(wchar_t *buf, size_t cch)
{
    DWORD n = GetTempPathW((DWORD)cch, buf);
    if (n == 0 || n >= cch) wcscpy_s(buf, cch, L".\\");
}

/* A name nothing else in this process or any other is using. */
static void tmp_name(wchar_t *buf, size_t cch, const wchar_t *tag,
                     const wchar_t *ext)
{
    static LONG counter;
    wchar_t dir[CCH];
    tmp_dir(dir, CCH);
    _snwprintf_s(buf, cch, _TRUNCATE, L"%lsapr_out_%ls_%lu_%ld.%ls",
                 dir, tag, GetCurrentProcessId(),
                 InterlockedIncrement(&counter), ext);
}

static int exists(const wchar_t *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static int make_file(const wchar_t *p, const char *content)
{
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wrote = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    WriteFile(h, content, (DWORD)strlen(content), &wrote, NULL);
    CloseHandle(h);
    return 1;
}

static int file_says(const wchar_t *p, const char *content)
{
    char   buf[64];
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD  got = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, buf, (DWORD)sizeof buf - 1, &got, NULL)) got = 0;
    CloseHandle(h);
    buf[got] = '\0';
    return strcmp(buf, content) == 0;
}

static AprOutContext ctx_of(const wchar_t *bus, const wchar_t *ext)
{
    AprOutContext c;
    c.bus_name  = bus;
    c.extension = ext;
    return c;
}

/* ===========================================================================
 * Tokens
 * ========================================================================= */

TEST(a_path_with_no_tokens_is_itself)
{
    /* The property that keeps every session file written before templates
     * existed working exactly as it did: an ordinary Windows path contains no
     * braces, so it expands to itself, character for character. */
    AprOutContext c = ctx_of(L"Main Mix", L"wav");
    wchar_t got[CCH];
    AprErr  e;

    ASSERT_FALSE(apr_out_has_tokens(L"C:\\takes\\mix.wav"));
    e = apr_out_expand(L"C:\\takes\\mix.wav", &c, got, CCH);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_WSTR_EQ(L"C:\\takes\\mix.wav", got);
}

TEST(the_bus_and_the_extension_come_from_the_context)
{
    AprOutContext c = ctx_of(L"Main Mix", L"opus");
    wchar_t got[CCH];
    AprErr  e;

    ASSERT_TRUE(apr_out_has_tokens(L"C:\\takes\\{bus}.{ext}"));
    e = apr_out_expand(L"C:\\takes\\{bus}.{ext}", &c, got, CCH);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_WSTR_EQ(L"C:\\takes\\Main Mix.opus", got);
}

TEST(the_date_and_time_are_the_local_ones_and_hold_no_colon)
{
    /* A colon in a Windows filename does not mean "half past": it opens an
     * alternate data stream, and the recording would go somewhere no player
     * will ever look. */
    AprOutContext c = ctx_of(L"Mix", L"wav");
    SYSTEMTIME lt;
    wchar_t got[CCH], want[CCH];
    AprErr  e;

    GetLocalTime(&lt);
    e = apr_out_expand(L"{date} {time}", &c, got, CCH);
    ASSERT_FALSE(apr_failed(&e));
    printf("      expanded: [%ls]\n", got);
    ASSERT_NULL(wcschr(got, L':'));

    _snwprintf_s(want, CCH, _TRUNCATE, L"%04u-%02u-%02u",
                 lt.wYear, lt.wMonth, lt.wDay);
    /* Only the date half is compared: the second could have ticked over
     * between the two calls, and a test that fails once an hour is worse than
     * one that checks a little less. */
    ASSERT_EQ_INT(0, wcsncmp(want, got, wcslen(want)));
}

TEST(a_bus_name_that_a_filename_cannot_hold_is_made_safe)
{
    /* {bus} expands INSIDE a path the user already chose, so a slash in a bus
     * name would silently redirect the recording into a folder -- or, more
     * likely, fail to create it at all. */
    AprOutContext c = ctx_of(L"Teams / Zoom: take?", L"wav");
    wchar_t got[CCH];
    AprErr  e;

    e = apr_out_expand(L"{bus}.{ext}", &c, got, CCH);
    ASSERT_FALSE(apr_failed(&e));
    printf("      sanitized: [%ls]\n", got);
    ASSERT_WSTR_EQ(L"Teams - Zoom- take-.wav", got);
}

TEST(an_unknown_token_is_left_exactly_as_it_was)
{
    /* A brace is legal in a Windows filename, and a path that worked before
     * templates existed must not stop working because they do. */
    AprOutContext c = ctx_of(L"Mix", L"wav");
    wchar_t got[CCH];
    AprErr  e;

    ASSERT_FALSE(apr_out_has_tokens(L"C:\\{7b1c}\\mix.wav"));
    e = apr_out_expand(L"C:\\{7b1c}\\mix.wav", &c, got, CCH);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_WSTR_EQ(L"C:\\{7b1c}\\mix.wav", got);
}

TEST(a_doubled_brace_is_one_literal_brace)
{
    AprOutContext c = ctx_of(L"Mix", L"wav");
    wchar_t got[CCH];
    AprErr  e;

    e = apr_out_expand(L"{{bus}.{ext}", &c, got, CCH);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_WSTR_EQ(L"{bus}.wav", got);
}

TEST(a_name_that_does_not_fit_is_refused_rather_than_truncated)
{
    /* A truncated path is a different file, and writing a recording into a
     * different file is the failure this whole module exists to prevent. */
    AprOutContext c = ctx_of(L"Mix", L"wav");
    wchar_t got[8];
    AprErr  e;

    e = apr_out_expand(L"C:\\somewhere\\quite\\deep\\mix.wav", &c, got, 8);
    ASSERT_TRUE(apr_failed(&e));
}

/* ===========================================================================
 * The collision policy
 * ========================================================================= */

TEST(a_free_name_is_used_as_it_is)
{
    AprOutContext c = ctx_of(L"Mix", L"wav");
    wchar_t want[CCH], got[CCH];
    int     collided = 1;
    AprErr  e;

    tmp_name(want, CCH, L"free", L"wav");
    e = apr_out_resolve(want, &c, got, CCH, &collided);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_WSTR_EQ(want, got);
    ASSERT_EQ_INT(0, collided);
    /* Resolving creates nothing: the file appears when the encoder opens it. */
    ASSERT_FALSE(exists(got));
}

TEST(a_name_that_is_already_a_recording_is_never_overwritten)
{
    /* THE HEART OF BUG 2. Recording twice to one name must leave two files,
     * and the older one must still contain what it contained. */
    AprOutContext c = ctx_of(L"Mix", L"wav");
    wchar_t first[CCH], got[CCH], expect[CCH];
    size_t  dot;
    int     collided = 0;
    AprErr  e;

    tmp_name(first, CCH, L"taken", L"wav");
    ASSERT_TRUE(make_file(first, "TAKE ONE"));

    e = apr_out_resolve(first, &c, got, CCH, &collided);
    ASSERT_FALSE(apr_failed(&e));
    printf("      asked for: [%ls]\n", first);
    printf("      saved as:  [%ls]\n", got);

    /* The number goes before the extension, not after it: "mix-2.wav" is
     * still a WAV and "mix.wav-2" is not. */
    dot = wcslen(first) - 4;                     /* ".wav" */
    _snwprintf_s(expect, CCH, _TRUNCATE, L"%.*ls-2.wav", (int)dot, first);
    ASSERT_WSTR_EQ(expect, got);

    /* And it was reported, which is what keeps auto-increment from being a
     * silent rename. */
    ASSERT_EQ_INT(1, collided);

    /* The take that was already there is untouched. */
    ASSERT_TRUE(file_says(first, "TAKE ONE"));

    DeleteFileW(first);
}

TEST(the_third_take_of_one_name_gets_the_third_number)
{
    AprOutContext c = ctx_of(L"Mix", L"wav");
    wchar_t first[CCH], second[CCH], got[CCH];
    size_t  dot;
    AprErr  e;

    tmp_name(first, CCH, L"three", L"wav");
    dot = wcslen(first) - 4;
    _snwprintf_s(second, CCH, _TRUNCATE, L"%.*ls-2.wav", (int)dot, first);

    ASSERT_TRUE(make_file(first, "ONE"));
    ASSERT_TRUE(make_file(second, "TWO"));

    e = apr_out_resolve(first, &c, got, CCH, NULL);
    ASSERT_FALSE(apr_failed(&e));
    printf("      saved as: [%ls]\n", got);
    ASSERT_NOT_NULL(wcsstr(got, L"-3.wav"));

    ASSERT_TRUE(file_says(first, "ONE"));
    ASSERT_TRUE(file_says(second, "TWO"));

    DeleteFileW(first);
    DeleteFileW(second);
}

TEST(a_name_with_no_extension_still_gets_a_number)
{
    AprOutContext c = ctx_of(L"Mix", L"wav");
    wchar_t dir[CCH], first[CCH], got[CCH], expect[CCH];
    AprErr  e;

    tmp_dir(dir, CCH);
    _snwprintf_s(first, CCH, _TRUNCATE, L"%lsapr_out_noext_%lu",
                 dir, GetCurrentProcessId());
    ASSERT_TRUE(make_file(first, "ONE"));

    e = apr_out_resolve(first, &c, got, CCH, NULL);
    ASSERT_FALSE(apr_failed(&e));
    _snwprintf_s(expect, CCH, _TRUNCATE, L"%ls-2", first);
    ASSERT_WSTR_EQ(expect, got);

    DeleteFileW(first);
}

TEST(a_dot_in_a_folder_name_is_not_mistaken_for_an_extension)
{
    /* "C:\v1.2\mix" has a dot, and it is not the file's. Inserting the take
     * number there would name a folder that does not exist. */
    AprOutContext c = ctx_of(L"Mix", L"wav");
    wchar_t dir[CCH], sub[CCH], first[CCH], got[CCH], expect[CCH];
    AprErr  e;

    tmp_dir(dir, CCH);
    _snwprintf_s(sub, CCH, _TRUNCATE, L"%lsapr_out_v1.2_%lu",
                 dir, GetCurrentProcessId());
    RemoveDirectoryW(sub);
    ASSERT_TRUE(CreateDirectoryW(sub, NULL) != 0);

    _snwprintf_s(first, CCH, _TRUNCATE, L"%ls\\mix", sub);
    ASSERT_TRUE(make_file(first, "ONE"));

    e = apr_out_resolve(first, &c, got, CCH, NULL);
    ASSERT_FALSE(apr_failed(&e));
    _snwprintf_s(expect, CCH, _TRUNCATE, L"%ls\\mix-2", sub);
    printf("      saved as: [%ls]\n", got);
    ASSERT_WSTR_EQ(expect, got);

    DeleteFileW(first);
    RemoveDirectoryW(sub);
}

TEST(the_counter_token_finds_the_lowest_free_number_on_disk)
{
    /* {n} IS RESOLVED AGAINST THE DISK, not against a counter in memory --
     * which is what makes it give the same answer after a restart. Nothing is
     * written down anywhere for this to work. */
    AprOutContext c = ctx_of(L"Mix", L"wav");
    wchar_t dir[CCH], tmpl[CCH], one[CCH], two[CCH], got[CCH];
    int     collided = 1;
    AprErr  e;

    tmp_dir(dir, CCH);
    _snwprintf_s(tmpl, CCH, _TRUNCATE, L"%lsapr_out_take_%lu_{n}.{ext}",
                 dir, GetCurrentProcessId());
    _snwprintf_s(one, CCH, _TRUNCATE, L"%lsapr_out_take_%lu_1.wav",
                 dir, GetCurrentProcessId());
    _snwprintf_s(two, CCH, _TRUNCATE, L"%lsapr_out_take_%lu_2.wav",
                 dir, GetCurrentProcessId());
    DeleteFileW(one);
    DeleteFileW(two);

    e = apr_out_resolve(tmpl, &c, got, CCH, &collided);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_WSTR_EQ(one, got);
    /* Asking for a number and getting one is not a surprise, so nothing is
     * reported. */
    ASSERT_EQ_INT(0, collided);

    ASSERT_TRUE(make_file(one, "ONE"));
    e = apr_out_resolve(tmpl, &c, got, CCH, &collided);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_WSTR_EQ(two, got);
    ASSERT_EQ_INT(0, collided);
    ASSERT_TRUE(file_says(one, "ONE"));

    DeleteFileW(one);
    DeleteFileW(two);
}

/* ===========================================================================
 * The early check
 * ========================================================================= */

TEST(probing_a_writable_folder_leaves_nothing_behind)
{
    wchar_t path[CCH];
    AprErr  e;

    tmp_name(path, CCH, L"probe", L"wav");
    ASSERT_FALSE(exists(path));
    e = apr_out_probe_writable(path);
    ASSERT_FALSE(apr_failed(&e));
    /* The file it had to create to answer the question is gone again, which
     * is what makes this safe to run the moment a user types a name -- and
     * safe inside --dry-run. */
    ASSERT_FALSE(exists(path));
}

TEST(probing_a_name_that_already_exists_does_not_touch_it)
{
    /* OPEN_ALWAYS, never CREATE_ALWAYS. Answering "yes, writable" by
     * truncating yesterday's recording would be the very data loss this
     * module is here to prevent. */
    wchar_t path[CCH];
    AprErr  e;

    tmp_name(path, CCH, L"probeold", L"wav");
    ASSERT_TRUE(make_file(path, "TAKE ONE"));
    e = apr_out_probe_writable(path);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_TRUE(exists(path));
    ASSERT_TRUE(file_says(path, "TAKE ONE"));
    DeleteFileW(path);
}

TEST(a_folder_that_is_not_there_is_refused_and_the_expanded_name_is_reported)
{
    /* THE EARLY CHECK, and the reason it takes a template rather than a path:
     * probing "mix.{ext}" literally would create a file called exactly that.
     * What comes back is what the user has to be shown. */
    AprOutContext c = ctx_of(L"Main Mix", L"wav");
    wchar_t dir[CCH], tmpl[CCH], shown[CCH];
    AprErr  e;

    tmp_dir(dir, CCH);
    _snwprintf_s(tmpl, CCH, _TRUNCATE, L"%lsapr_out_no_such_dir_%lu\\{bus}.{ext}",
                 dir, GetCurrentProcessId());

    e = apr_out_validate(tmpl, &c, shown, CCH);
    ASSERT_TRUE(apr_failed(&e));
    printf("      refused:  [%ls]\n", shown);
    ASSERT_NOT_NULL(wcsstr(shown, L"Main Mix.wav"));
    ASSERT_NULL(wcschr(shown, L'{'));

    /* And nothing at all was created on the way to finding out. */
    ASSERT_FALSE(exists(tmpl));
}

TEST(a_writable_template_passes_the_early_check_and_creates_nothing)
{
    AprOutContext c = ctx_of(L"Main Mix", L"wav");
    wchar_t dir[CCH], tmpl[CCH], shown[CCH];
    AprErr  e;

    tmp_dir(dir, CCH);
    _snwprintf_s(tmpl, CCH, _TRUNCATE, L"%lsapr_out_ok_%lu_{bus}.{ext}",
                 dir, GetCurrentProcessId());

    e = apr_out_validate(tmpl, &c, shown, CCH);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_FALSE(exists(shown));
    DeleteFileW(shown);
}

/* ===========================================================================
 * The default
 * ========================================================================= */

TEST(the_default_name_is_absolute_timestamped_and_format_following)
{
    /* "A sensible default that never collides." It carries {date} and {time},
     * so two takes a second apart already differ; it carries {bus}, so two
     * buses in one session differ; and it ends in {ext} rather than a fixed
     * extension so that choosing MP3 in the same dialog does not leave a
     * stale ".wav" behind. */
    wchar_t deft[CCH], got[CCH];
    AprOutContext c = ctx_of(L"Main Mix", L"mp3");
    AprErr e;

    apr_out_default_template(deft, CCH);
    printf("      default: [%ls]\n", deft);

    ASSERT_TRUE(apr_out_has_tokens(deft));
    ASSERT_NOT_NULL(wcsstr(deft, L"{date}"));
    ASSERT_NOT_NULL(wcsstr(deft, L"{time}"));
    ASSERT_NOT_NULL(wcsstr(deft, L"{bus}"));
    ASSERT_NOT_NULL(wcsstr(deft, L"{ext}"));

    e = apr_out_expand(deft, &c, got, CCH);
    ASSERT_FALSE(apr_failed(&e));
    printf("      becomes: [%ls]\n", got);
    ASSERT_NULL(wcschr(got, L'{'));
    ASSERT_NOT_NULL(wcsstr(got, L"Main Mix"));
    ASSERT_NOT_NULL(wcsstr(got, L".mp3"));
    /* Rooted, so it does not land in whatever folder the app was started
     * from -- which for a shortcut is nowhere the user can find. */
    ASSERT_TRUE(got[1] == L':' || (got[0] == L'\\' && got[1] == L'\\'));
}

/* ===========================================================================
 * Claiming the name, rather than checking it and hoping (M13)
 * ========================================================================= */

/* apr_out_resolve answers "which name is free". Between that answer and the
 * CreateFileW that used it there was a window, and it was not theoretical:
 * two scheduled tasks starting apprecorder in the same second with the same
 * template both saw "mix.wav" free, both opened it with CREATE_ALWAYS, and
 * both wrote into it. One corrupt take -- and, if the collision policy had
 * already moved an earlier take aside, a second file that is garbage under the
 * name the earlier take used to have.
 *
 * apr_out_open_new closes it by making the create itself the claim. */
TEST(opening_a_new_take_never_touches_a_file_that_is_already_there)
{
    wchar_t taken[CCH], got[CCH], expect[CCH];
    void   *h = NULL;
    int     collided = -1;
    size_t  len;
    AprErr  e;

    tmp_name(taken, CCH, L"claim", L"wav");
    ASSERT_TRUE(make_file(taken, "the take that was already here"));

    /* The same name, handed straight to the opener as if the resolve had been
     * beaten to it. CREATE_ALWAYS would have truncated it here. */
    e = apr_out_open_new(taken, got, CCH, &collided, &h);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_NOT_NULL(h);
    ASSERT_EQ_INT(1, collided);

    /* The earlier take is untouched, byte for byte. THIS is the property. */
    ASSERT_TRUE(file_says(taken, "the take that was already here"));

    /* And the name that came back is the next one down the same ladder the
     * collision policy walks. */
    len = wcslen(taken);
    wcscpy_s(expect, CCH, taken);
    wcscpy_s(expect + len - 4, CCH - (len - 4), L"-2.wav");
    ASSERT_WSTR_EQ(expect, got);
    ASSERT_TRUE(exists(got));

    CloseHandle((HANDLE)h);
    DeleteFileW(taken);
    DeleteFileW(got);
}

TEST(a_free_name_is_opened_as_itself_and_reports_no_collision)
{
    wchar_t want[CCH], got[CCH];
    void   *h = NULL;
    int     collided = -1;
    AprErr  e;

    tmp_name(want, CCH, L"claimfree", L"wav");
    ASSERT_FALSE(exists(want));

    e = apr_out_open_new(want, got, CCH, &collided, &h);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_NOT_NULL(h);
    ASSERT_EQ_INT(0, collided);
    ASSERT_WSTR_EQ(want, got);

    CloseHandle((HANDLE)h);
    DeleteFileW(got);
}

/* The handle is a real one, opened for writing, and the take that was moved
 * aside is what receives the bytes. */
TEST(the_handle_that_comes_back_is_the_one_the_recording_writes_through)
{
    wchar_t taken[CCH], got[CCH];
    void   *h = NULL;
    DWORD   wrote = 0;
    AprErr  e;

    tmp_name(taken, CCH, L"claimwrite", L"wav");
    ASSERT_TRUE(make_file(taken, "first"));

    e = apr_out_open_new(taken, got, CCH, NULL, &h);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_TRUE(WriteFile((HANDLE)h, "second", 6, &wrote, NULL) != 0);
    CloseHandle((HANDLE)h);

    ASSERT_TRUE(file_says(taken, "first"));
    ASSERT_TRUE(file_says(got, "second"));

    DeleteFileW(taken);
    DeleteFileW(got);
}

/* A folder that is not there is still a plain failure with a reason, not a
 * walk up the whole ladder looking for a name that could never work. */
TEST(a_path_that_cannot_be_created_at_all_fails_rather_than_counting)
{
    wchar_t dir[CCH], path[CCH], got[CCH];
    void   *h = (void *)1;
    AprErr  e;

    tmp_dir(dir, CCH);
    _snwprintf_s(path, CCH, _TRUNCATE, L"%lsapr_out_no_such_folder_%lu\\x.wav",
                 dir, GetCurrentProcessId());

    e = apr_out_open_new(path, got, CCH, NULL, &h);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_NULL(h);
}

/* ===========================================================================
 * Asking about one token (m17 needs this to tell two files from one)
 * ========================================================================= */

TEST(a_caller_can_ask_whether_a_template_numbers_itself)
{
    ASSERT_TRUE(apr_out_has_token(L"take{n}.wav", L"n"));
    ASSERT_TRUE(apr_out_has_token(L"take{N}.wav", L"n"));
    ASSERT_FALSE(apr_out_has_token(L"take.wav", L"n"));
    /* {{ is a literal brace and is not a token. */
    ASSERT_FALSE(apr_out_has_token(L"take{{n}.wav", L"n"));
    /* One token, not any token: {bus} does not answer for {n}. */
    ASSERT_FALSE(apr_out_has_token(L"{bus}.wav", L"n"));
    ASSERT_TRUE(apr_out_has_token(L"{bus}.wav", L"bus"));
    ASSERT_FALSE(apr_out_has_token(NULL, L"n"));
    ASSERT_FALSE(apr_out_has_token(L"take{n}.wav", NULL));
}
