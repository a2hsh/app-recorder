/*
 * strings.c -- the localization layer. See include/strings.h for the rules
 * this file exists to enforce; what follows is why it is built the way it is.
 *
 * ---------------------------------------------------------------------------
 * WHY NOT LoadStringW
 *
 * LoadStringW is the obvious call and it is the wrong one here, for two
 * independent reasons.
 *
 * 1. It picks the language for you, out of the resource loader's own search
 *    order (thread preferred UI languages, then process, then system, then
 *    neutral), and it silently falls back. That is fine for display -- we want
 *    a fallback -- but it makes the completeness check a lie: probing for an
 *    Arabic string that does not exist returns the English one, so a check
 *    built on LoadStringW passes on an untranslated build. apr_str_probe()
 *    therefore reads the resource directly and matches the language EXACTLY,
 *    using EnumResourceLanguagesW to confirm the block really carries that
 *    language rather than trusting FindResourceExW's fallback behaviour.
 *
 * 2. It forces a copy into a caller buffer of a guessed size, or the
 *    undocumented cch==0 form that hands back a pointer into the resource that
 *    is NOT NUL-terminated (STRINGTABLE entries are length-prefixed and packed
 *    end to end). Every call site would then need its own buffer and its own
 *    truncation policy. Caching one NUL-terminated copy per id, once, is
 *    smaller at every call site and is what makes `SetWindowTextW(h,
 *    apr_str(ID))` legal.
 *
 * ---------------------------------------------------------------------------
 * WHY FormatMessageW AND NOT A HAND-ROLLED FORMATTER
 *
 * Positional inserts are the whole requirement -- a translator must be able to
 * move %2 in front of %1 -- and FormatMessageW implements them natively with
 * FORMAT_MESSAGE_FROM_STRING. Writing our own would mean owning a parser for
 * the exact thing the platform already does correctly.
 *
 * Design 6.2 specifies "%1$s". That spelling is POSIX; Win32 does not
 * implement it and neither does the MSVC CRT's swprintf. The Win32 spelling of
 * the same idea is %1!s!, and that is what the catalog uses. The design's
 * intent -- positional, reorderable -- is met exactly; only the spelling
 * differs, and the header says so where translators and UI authors will read
 * it. (If we had followed the design literally, "%1$s" would have reached
 * FormatMessageW as insert 1 followed by a literal "$s".)
 *
 * Measured behaviour of FormatMessageW + FORMAT_MESSAGE_FROM_STRING +
 * FORMAT_MESSAGE_ARGUMENT_ARRAY, pinned by tests/test_strings.c:
 *
 *   - Inserts are RANDOM ACCESS by number, not sequential consumption.
 *     "%2!s! %1!s!" reorders correctly.
 *   - AN INSERT MAY BE REPEATED. "%1!s! ... %1!s!" expands the same argument
 *     twice. Nothing is consumed.
 *   - AN ARGUMENT MAY BE UNREFERENCED and is simply ignored -- which is what
 *     makes a zero plural form that never mentions the count ("No sources")
 *     work while the count is still passed.
 *   - A REFERENCED INSERT WITH NO ARGUMENT IS NOT SAFE. With
 *     FORMAT_MESSAGE_ARGUMENT_ARRAY the array is indexed directly, so %3!s!
 *     against a two-element array reads past the end and dereferences whatever
 *     it finds -- a crash or worse, not an error return. FormatMessageW cannot
 *     detect this; there is no argument count in the API. THIS FILE MUST
 *     THEREFORE COUNT THE INSERTS ITSELF (fmt_max_insert) AND REFUSE, and the
 *     internal argument array is over-allocated to APR_STR_MAX_ARGS and
 *     zero-filled so that even a future escape can only ever meet a NULL.
 *
 * ---------------------------------------------------------------------------
 * WHY EVERY INSERT IS !s!
 *
 * FORMAT_MESSAGE_ARGUMENT_ARRAY hands FormatMessageW an array it walks as if
 * it were a va_list. Mixed widths (%1!d! next to %2!s!) then depend on how the
 * formatter advances between elements, which is not documented and differs
 * between architectures. Rendering every number to text first (apr_str_number)
 * makes every element a pointer, which removes the question entirely -- and it
 * puts Western vs Arabic-Indic digit choice in one function instead of inside
 * kernel32.
 */

#include "strings.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "log.h"

/* The module that carries the resources. Not GetModuleHandleW(NULL): this
 * keeps working if the catalog is ever linked into something that is not the
 * process image. __ImageBase is emitted by the MSVC linker. */
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define APR_STR_MODULE ((HMODULE)&__ImageBase)

/* ---------------------------------------------------------------------------
 * Declared languages.
 *
 * Each row must match a LANGUAGE block in res/strings.rc EXACTLY -- the
 * primary/sublang pair, not just the primary. `coverage` is the build gate:
 * flipping ar-SA to APR_STR_COMPLETE makes tests/test_strings.c demand every
 * id in Arabic, which is what the translation pass has to clear.
 *
 * Index 0 is the primary language and the fallback for every other one.
 * ------------------------------------------------------------------------- */
static const AprStrLanguage g_languages[] = {
    { MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),          L"en-US",
      APR_STR_COMPLETE, 0 },
    { MAKELANGID(LANG_ARABIC,  SUBLANG_ARABIC_SAUDI_ARABIA), L"ar-SA",
      APR_STR_PARTIAL,  1 },
};

#define APR_STR_LANG_COUNT ((int)(sizeof g_languages / sizeof g_languages[0]))
#define APR_STR_PRIMARY    (g_languages[0].langid)

/* ---------------------------------------------------------------------------
 * The catalog, as data, generated from the same two lists the .rc uses.
 * ------------------------------------------------------------------------- */

#define APR_STR__ENTRY(name, id)  { (AprStrId)(id), "APR_S_" #name, 0 },
#define APR_STR__PENTRY(name, id) { (AprStrId)(id), "APR_S_" #name, 1 },

static const AprStrEntry g_catalog[] = {
    APR_STR_LIST(APR_STR__ENTRY)
    APR_STR_PLURAL_LIST(APR_STR__PENTRY)
};

#undef APR_STR__ENTRY
#undef APR_STR__PENTRY

#define APR_STR_CATALOG_COUNT ((int)(sizeof g_catalog / sizeof g_catalog[0]))

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

#define APR_STR_SLOTS (APR_STR_ID_MAX - APR_STR_ID_MIN)

static SRWLOCK  g_lock = SRWLOCK_INIT;
static wchar_t *g_cache[APR_STR_SLOTS];
/* 1 when the cached text came from somewhere other than the current language.
 * Plural selection reads this so that a form falling back to English is chosen
 * with English rules instead of being indexed with Arabic ones. */
static unsigned char g_fell_back[APR_STR_SLOTS];
static LANGID   g_lang;
static int      g_initialised;

/* Returned instead of NULL. Deliberately hideous: a missing string should be
 * impossible to miss in a screenshot and impossible to mistake for copy. */
static const wchar_t k_bad_id[]    = L"!!apr_str: id out of range!!";
static const wchar_t k_no_memory[] = L"!!apr_str: out of memory!!";
static const wchar_t k_bad_args[]  = L"!!apr_str: not enough arguments!!";
static const wchar_t k_bad_format[]= L"!!apr_str: format failed!!";

/* ---------------------------------------------------------------------------
 * Raw resource access -- exact language, no fallback
 * ------------------------------------------------------------------------- */

typedef struct LangProbe {
    LANGID want;
    int    found;
} LangProbe;

static BOOL CALLBACK lang_probe_cb(HMODULE module, LPCWSTR type, LPCWSTR name,
                                   WORD lang, LONG_PTR param)
{
    LangProbe *p = (LangProbe *)param;
    (void)module; (void)type; (void)name;
    if (lang == p->want) {
        p->found = 1;
        return FALSE;  /* stop enumerating */
    }
    return TRUE;
}

/* Whether STRINGTABLE block `block` genuinely exists for `lang`.
 *
 * FindResourceExW's documented behaviour when the requested language is absent
 * is the loader's fallback search, which is exactly what a completeness check
 * must not accept. Enumerating the languages actually present on the resource
 * is unambiguous regardless of how that fallback is specified this week. */
static int block_has_language(WORD block, LANGID lang)
{
    LangProbe probe;
    probe.want  = (WORD)lang;
    probe.found = 0;
    /* Returns FALSE both for "no such resource" and for "callback stopped the
     * enumeration"; `found` is the answer either way. */
    (void)EnumResourceLanguagesW(APR_STR_MODULE, RT_STRING,
                                 MAKEINTRESOURCEW(block), lang_probe_cb,
                                 (LONG_PTR)&probe);
    return probe.found;
}

/* Points `*out` at the (not NUL-terminated) resource text for `id` in exactly
 * `lang`, and returns its length in characters. Returns -1 when this language
 * has no entry -- including the "block exists, slot is empty" case, which is
 * how an untranslated id appears once the .rc has emitted nothing for it.
 *
 * STRINGTABLE layout: ids are grouped 16 to a block, block number is
 * id/16 + 1, and a block is 16 records of {WORD cch; WCHAR text[cch];} laid
 * end to end, including empty ones. */
static int res_lookup(LANGID lang, unsigned id, const wchar_t **out)
{
    HRSRC          res;
    HGLOBAL        glob;
    const wchar_t *p;
    WORD           block = (WORD)((id / 16) + 1);
    unsigned       index = id % 16;
    unsigned       i;
    int            cch;

    *out = NULL;

    if (!block_has_language(block, lang))
        return -1;

    res = FindResourceExW(APR_STR_MODULE, RT_STRING, MAKEINTRESOURCEW(block),
                          lang);
    if (!res)
        return -1;

    glob = LoadResource(APR_STR_MODULE, res);
    if (!glob)
        return -1;

    p = (const wchar_t *)LockResource(glob);
    if (!p)
        return -1;

    for (i = 0; i < index; i++)
        p += (size_t)1 + (size_t)*p;

    cch = (int)*p;
    if (cch == 0)
        return -1;   /* present block, absent entry */

    *out = p + 1;
    return cch;
}

int apr_str_probe(LANGID lang, AprStrId id, wchar_t *buf, size_t cch)
{
    const wchar_t *text;
    int            len;
    size_t         n;

    if (!buf || cch == 0)
        return -1;
    buf[0] = L'\0';

    if ((int)id < APR_STR_ID_MIN || (int)id >= APR_STR_ID_MAX)
        return -1;

    len = res_lookup(lang, (unsigned)id, &text);
    if (len < 0)
        return -1;

    n = (size_t)len;
    if (n > cch - 1)
        n = cch - 1;
    memcpy(buf, text, n * sizeof(wchar_t));
    buf[n] = L'\0';
    return (int)n;
}

/* ---------------------------------------------------------------------------
 * Language selection
 * ------------------------------------------------------------------------- */

static int lang_index(LANGID lang)
{
    int i;
    for (i = 0; i < APR_STR_LANG_COUNT; i++)
        if (PRIMARYLANGID(g_languages[i].langid) == PRIMARYLANGID(lang))
            return i;
    return -1;
}

/* Exclusive lock held. */
static void cache_reset_locked(void)
{
    int i;
    for (i = 0; i < APR_STR_SLOTS; i++) {
        free(g_cache[i]);
        g_cache[i]     = NULL;
        g_fell_back[i] = 0;
    }
}

/* Exclusive lock held. */
static void ensure_initialised_locked(void)
{
    int idx;
    if (g_initialised)
        return;
    idx = lang_index(GetThreadUILanguage());
    g_lang = g_languages[idx < 0 ? 0 : idx].langid;
    g_initialised = 1;
}

AprErr apr_str_set_language(LANGID lang)
{
    int idx;

    AcquireSRWLockExclusive(&g_lock);
    idx = lang_index(lang);
    g_lang = g_languages[idx < 0 ? 0 : idx].langid;
    g_initialised = 1;
    cache_reset_locked();
    ReleaseSRWLockExclusive(&g_lock);

    if (idx < 0)
        return APR_ERR(APR_E_NOT_FOUND,
                       L"no string catalog for language 0x%04X; using %ls",
                       (unsigned)lang, g_languages[0].tag);
    return apr_ok();
}

AprErr apr_str_init(void)
{
    AcquireSRWLockExclusive(&g_lock);
    ensure_initialised_locked();
    ReleaseSRWLockExclusive(&g_lock);
    return apr_ok();
}

LANGID apr_str_language(void)
{
    LANGID l;
    AcquireSRWLockExclusive(&g_lock);
    ensure_initialised_locked();
    l = g_lang;
    ReleaseSRWLockExclusive(&g_lock);
    return l;
}

int apr_str_is_rtl(void)
{
    LANGID l = apr_str_language();
    int    i = lang_index(l);
    return i < 0 ? 0 : g_languages[i].rtl;
}

int apr_str_language_count(void) { return APR_STR_LANG_COUNT; }

const AprStrLanguage *apr_str_language_at(int index)
{
    if (index < 0 || index >= APR_STR_LANG_COUNT)
        return NULL;
    return &g_languages[index];
}

int apr_str_catalog_count(void) { return APR_STR_CATALOG_COUNT; }

const AprStrEntry *apr_str_catalog_at(int index)
{
    if (index < 0 || index >= APR_STR_CATALOG_COUNT)
        return NULL;
    return &g_catalog[index];
}

/* ---------------------------------------------------------------------------
 * Lookup
 * ------------------------------------------------------------------------- */

static wchar_t *dup_range(const wchar_t *text, int cch)
{
    wchar_t *copy = (wchar_t *)malloc(((size_t)cch + 1) * sizeof(wchar_t));
    if (!copy)
        return NULL;
    memcpy(copy, text, (size_t)cch * sizeof(wchar_t));
    copy[cch] = L'\0';
    return copy;
}

/* Exclusive lock held. Fills slot `slot` for id `id`; never leaves it NULL
 * unless allocation failed. */
static void fill_slot_locked(int slot, unsigned id)
{
    const wchar_t *text;
    int            cch;
    wchar_t        placeholder[64];

    cch = res_lookup(g_lang, id, &text);
    if (cch > 0) {
        g_cache[slot]     = dup_range(text, cch);
        g_fell_back[slot] = 0;
        if (g_cache[slot])
            return;
    }

    if (g_lang != APR_STR_PRIMARY) {
        cch = res_lookup(APR_STR_PRIMARY, id, &text);
        if (cch > 0) {
            g_cache[slot]     = dup_range(text, cch);
            g_fell_back[slot] = 1;
            if (g_cache[slot])
                return;
        }
    }

    /* Nothing anywhere. In a language declared COMPLETE this is a build
     * defect that tests/test_strings.c is supposed to have caught; make it
     * loud rather than blank so it cannot survive a screenshot either. */
    _snwprintf_s(placeholder, 64, _TRUNCATE, L"!!apr_str %u missing!!", id);
    g_cache[slot]     = dup_range(placeholder, (int)wcslen(placeholder));
    g_fell_back[slot] = 1;
}

/* Returns the cached text and, optionally, whether it came from the fallback
 * language. Never NULL. */
static const wchar_t *lookup(AprStrId id, int *out_fell_back)
{
    int            slot;
    const wchar_t *p;

    if (out_fell_back)
        *out_fell_back = 0;

    if ((int)id < APR_STR_ID_MIN || (int)id >= APR_STR_ID_MAX)
        return k_bad_id;

    slot = (int)id - APR_STR_ID_MIN;

    AcquireSRWLockShared(&g_lock);
    p = g_cache[slot];
    if (p && out_fell_back)
        *out_fell_back = g_fell_back[slot];
    ReleaseSRWLockShared(&g_lock);
    if (p)
        return p;

    AcquireSRWLockExclusive(&g_lock);
    ensure_initialised_locked();
    if (!g_cache[slot])
        fill_slot_locked(slot, (unsigned)id);
    p = g_cache[slot];
    if (p && out_fell_back)
        *out_fell_back = g_fell_back[slot];
    ReleaseSRWLockExclusive(&g_lock);

    return p ? p : k_no_memory;
}

const wchar_t *apr_str(AprStrId id)
{
    return lookup(id, NULL);
}

/* ---------------------------------------------------------------------------
 * CLDR plural rules
 *
 * Cardinal rules, integers only -- apprecorder counts sources, buses and
 * frames, never 1.5 of them. CLDR's `n` operand is the absolute value, so a
 * negative count uses |n|.
 *
 * Arabic is the reason this module exists in this shape. The two rules that
 * are always got wrong:
 *
 *     few  : n % 100 in 3..10       3, 4, ... 10, 103, 104, ... 110, 203 ...
 *     many : n % 100 in 11..99      11, ... 99, 111, ... 199, 211 ...
 *
 * Note what falls through to `other`: 100, 101, 102, 200, 201, 202, 1000.
 * n=100 is neither few nor many because 100 % 100 == 0. Getting that wrong is
 * invisible in testing that only ever counts to ten.
 * ------------------------------------------------------------------------- */

static AprPluralCategory plural_arabic(uint64_t n)
{
    uint64_t m;
    if (n == 0) return APR_PLURAL_ZERO;
    if (n == 1) return APR_PLURAL_ONE;
    if (n == 2) return APR_PLURAL_TWO;
    m = n % 100;
    if (m >= 3 && m <= 10)  return APR_PLURAL_FEW;
    if (m >= 11 && m <= 99) return APR_PLURAL_MANY;
    return APR_PLURAL_OTHER;
}

static AprPluralCategory plural_english(uint64_t n)
{
    return n == 1 ? APR_PLURAL_ONE : APR_PLURAL_OTHER;
}

AprPluralCategory apr_plural_category(LANGID lang, int64_t n)
{
    /* Negation of INT64_MIN is undefined; go through the unsigned domain. */
    uint64_t abs_n = n < 0 ? (uint64_t)(-(n + 1)) + 1u : (uint64_t)n;

    switch (PRIMARYLANGID(lang)) {
    case LANG_ARABIC:
        return plural_arabic(abs_n);
    default:
        return plural_english(abs_n);
    }
}

const wchar_t *apr_str_plural(AprStrId base_id, int64_t n)
{
    AprPluralCategory cat;
    LANGID            lang = apr_str_language();
    int               fell_back = 0;

    /* Which language will actually supply the text? Ask for the OTHER form --
     * every language's six forms are emitted together by the .rc, so OTHER is
     * present exactly when the rest are. If it falls back, the text will be
     * English and must be selected with English rules. */
    (void)lookup((AprStrId)((int)base_id + APR_PLURAL_OTHER), &fell_back);
    if (fell_back)
        lang = APR_STR_PRIMARY;

    cat = apr_plural_category(lang, n);
    return apr_str((AprStrId)((int)base_id + (int)cat));
}

/* ---------------------------------------------------------------------------
 * Formatting
 * ------------------------------------------------------------------------- */

size_t apr_str_number(int64_t n, wchar_t *buf, size_t cch)
{
    /* 20 digits, a sign and a terminator is the worst int64 case. Rendering
     * into local storage first is not a style choice: _i64tow_s given a buffer
     * that is too small invokes the CRT's invalid-parameter handler, which in
     * a Debug build is a modal assertion dialog -- i.e. a hang, on whatever
     * thread happened to format a number. Never hand it a caller's size. */
    wchar_t tmp[24];
    size_t  len;

    if (!buf || cch == 0)
        return 0;
    buf[0] = L'\0';

    /* Western (Hindu-Arabic) digits, per design 6.2. THE ONLY PLACE a number
     * becomes user-visible digits: if the author asks for Arabic-Indic digits
     * (U+0660..U+0669) in the Arabic UI, map them here and nothing else in the
     * product changes. */
    if (_i64tow_s(n, tmp, sizeof tmp / sizeof tmp[0], 10) != 0)
        return 0;

    len = wcslen(tmp);
    if (len > cch - 1)
        return 0;      /* refused, not truncated: half a number is a lie */

    memcpy(buf, tmp, (len + 1) * sizeof(wchar_t));
    return len;
}

/* Highest insert number referenced by `fmt`, 0 for none.
 *
 * FormatMessageW's escapes: %% is a literal percent, %n a hard line break, %b
 * a space, %. a period, and %0 terminates the message. Only %<digits> is an
 * insert. An insert may be followed by a !printf-spec! which is skipped here
 * because it can itself contain digits (%1!.*s! and friends). */
static int fmt_max_insert(const wchar_t *fmt)
{
    int max = 0;

    while (*fmt) {
        if (*fmt != L'%') {
            fmt++;
            continue;
        }
        fmt++;
        if (*fmt >= L'0' && *fmt <= L'9') {
            int v = 0;
            while (*fmt >= L'0' && *fmt <= L'9') {
                if (v < 1000)                 /* saturate; nothing legitimate */
                    v = v * 10 + (*fmt - L'0');
                fmt++;
            }
            if (v > max)
                max = v;
            if (*fmt == L'!') {               /* skip !printf-spec! */
                fmt++;
                while (*fmt && *fmt != L'!')
                    fmt++;
                if (*fmt)
                    fmt++;
            }
        } else if (*fmt) {
            fmt++;                            /* %%, %n, %b, %. -- not inserts */
        }
    }
    return max;
}

static size_t put_literal(const wchar_t *text, wchar_t *buf, size_t cch)
{
    size_t n = wcslen(text);
    if (!buf || cch == 0)
        return 0;
    if (n > cch - 1)
        n = cch - 1;
    memcpy(buf, text, n * sizeof(wchar_t));
    buf[n] = L'\0';
    return n;
}

size_t apr_str_format_string(const wchar_t *fmt, wchar_t *buf, size_t cch,
                             const wchar_t *const *args, size_t nargs)
{
    /* Over-allocated and zero-filled on purpose: FormatMessageW indexes this
     * array directly and has no idea how long it is, so the only defence
     * against a stray insert number is that every slot it could reach holds
     * something safe. fmt_max_insert below is the real guard; this is the
     * belt to its braces. */
    DWORD_PTR argv[APR_STR_MAX_ARGS];
    size_t    i;
    int       need;
    DWORD     written;

    if (!buf || cch == 0)
        return 0;
    buf[0] = L'\0';

    if (!fmt)
        return put_literal(k_bad_format, buf, cch);

    need = fmt_max_insert(fmt);
    if (need > APR_STR_MAX_ARGS || (size_t)need > nargs) {
        /* A catalog string referencing an insert the caller did not supply.
         * With FORMAT_MESSAGE_ARGUMENT_ARRAY this would be an out-of-bounds
         * read inside kernel32, so it is refused here, loudly, and logged --
         * it means a translator added an insert the call site does not pass,
         * which is a real and recurring localization bug. */
        APR_WARN(L"string format needs %d argument(s), %zu supplied: %ls",
                 need, nargs, fmt);
        return put_literal(k_bad_args, buf, cch);
    }

    for (i = 0; i < APR_STR_MAX_ARGS; i++)
        argv[i] = (DWORD_PTR)(i < nargs && args ? args[i] : L"");

    written = FormatMessageW(FORMAT_MESSAGE_FROM_STRING |
                                 FORMAT_MESSAGE_ARGUMENT_ARRAY,
                             fmt, 0, 0, buf, (DWORD)cch, (va_list *)argv);
    if (written == 0) {
        APR_WARN(L"FormatMessageW refused a catalog string (%lu): %ls",
                 GetLastError(), fmt);
        return put_literal(k_bad_format, buf, cch);
    }

    /* FormatMessageW NUL-terminates, but only within the buffer it was given;
     * be explicit so a truncating call still returns a usable string. */
    if ((size_t)written > cch - 1)
        written = (DWORD)(cch - 1);
    buf[written] = L'\0';
    return written;
}

size_t apr_str_format(AprStrId id, wchar_t *buf, size_t cch,
                      const wchar_t *const *args, size_t nargs)
{
    return apr_str_format_string(apr_str(id), buf, cch, args, nargs);
}

size_t apr_str_plural_format(AprStrId base_id, int64_t n,
                             wchar_t *buf, size_t cch,
                             const wchar_t *const *args, size_t nargs)
{
    const wchar_t *shifted[APR_STR_MAX_ARGS];
    wchar_t        number[32];
    size_t         i, count;

    if (!buf || cch == 0)
        return 0;

    apr_str_number(n, number, 32);

    /* %1 is the count; caller arguments start at %2. */
    shifted[0] = number;
    count = nargs;
    if (count > APR_STR_MAX_ARGS - 1)
        count = APR_STR_MAX_ARGS - 1;
    for (i = 0; i < count; i++)
        shifted[i + 1] = args ? args[i] : L"";

    return apr_str_format_string(apr_str_plural(base_id, n), buf, cch,
                                 shifted, count + 1);
}
