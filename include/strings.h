/*
 * strings.h -- apprecorder's localization layer. The single source of truth for
 * every user-facing string in the product.
 *
 * ===========================================================================
 * THE RULE, FIRST, BECAUSE IT IS THE ONE THAT GETS BROKEN
 *
 *   NEVER BUILD A SENTENCE OUT OF FRAGMENTS.
 *
 *   Not with wcscat, not with swprintf, not with "prefix" + apr_str(id), not
 *   by appending ":" or " - " or a trailing period in code. A translator sees
 *   one catalog entry at a time and cannot reorder pieces that were joined in
 *   C. Arabic is verb-initial, its adjectives follow their nouns, and its
 *   genitive construction runs the opposite way from English's -- a fragment
 *   order that reads correctly in English is frequently ungrammatical in
 *   Arabic, and no amount of translation skill fixes that from inside a
 *   fragment.
 *
 *   Write the whole sentence as ONE catalog entry with positional inserts and
 *   pass the variable parts as arguments:
 *
 *       WRONG:  wcscpy(msg, apr_str(APR_S_ERR_FILE_OPEN));
 *               wcscat(msg, L": ");
 *               wcscat(msg, path);
 *
 *       RIGHT:  const wchar_t *a[] = { path, reason };
 *               apr_str_format(APR_S_ERR_FILE_OPEN, msg, 256, a, 2);
 *               // catalog: "Could not open %1!s! for writing: %2!s!"
 *               // a translator is free to write  "...%2!s!... %1!s!..."
 *
 *   The same rule kills "%d items" style plurals. Arabic has SIX plural
 *   categories to English's two, so a count and a noun cannot be glued
 *   together in code either. Go through apr_str_plural / apr_str_plural_format.
 *
 * ===========================================================================
 * HOW THE CATALOG IS BUILT
 *
 *   Strings live in res/strings.rc as Win32 STRINGTABLE resources, one
 *   LANGUAGE block per locale, all of it linked into the single exe. There are
 *   no satellite DLLs; the cost is the string bytes only, which keeps the size
 *   goal in design section 2 intact.
 *
 *   IDs are declared exactly once, in APR_STR_LIST / APR_STR_PLURAL_LIST
 *   below. Both the C enum and every LANGUAGE block in the .rc are GENERATED
 *   from those two lists, so:
 *
 *     - the .rc cannot silently omit an entry: a name with no text macro is a
 *       hard rc.exe error, not an empty string at runtime;
 *     - adding an ID forces every declared language to state, in the .rc,
 *       either what the text is or that it is deliberately untranslated;
 *     - the enum and the resource ids cannot drift apart.
 *
 *   tests/test_strings.c then walks the same two lists at runtime and asserts
 *   every id resolves, per language, against the resource actually linked into
 *   the binary. That test runs under ctest, so a missing translation in a
 *   language declared complete fails `build.cmd Debug test`.
 *
 * ===========================================================================
 * FORMATTING -- WIN32 SPELLING, NOT POSIX
 *
 *   Inserts use FormatMessageW syntax:  %1!s!  %2!s!  ...  NOT POSIX %1$s.
 *   (Design 6.2 writes "%1$s"; that is a POSIX spelling Win32 does not
 *   implement -- see the note at the top of src/i18n/strings.c.)
 *
 *   EVERY insert in this catalog is !s! -- a wide string. Numbers are never
 *   passed as numbers. Format them with apr_str_number() and pass the text.
 *   Two reasons: it removes the argument-width hazard in FormatMessage's
 *   argument array, and it puts digit shaping (Western vs Arabic-Indic) under
 *   our control in one function rather than inside a kernel32 formatter.
 *
 * ===========================================================================
 * DIRECTION
 *
 *   apr_str_is_rtl() is the ONE place layout direction comes from. Signal flow
 *   is left-to-right in English and right-to-left in Arabic (sources on the
 *   right, actions on the left). WS_EX_LAYOUTRTL mirrors standard child
 *   controls for free but does NOT mirror anything we paint ourselves, and the
 *   canvas nodes are custom-painted, so the canvas must ask this and mirror
 *   its own drawing. Hardcoding a flow direction anywhere is a review failure.
 *
 *   Logical order (source -> bus -> action) is language-independent. The
 *   accessibility tree and the TreeView never flip. Only painting does.
 *
 * ===========================================================================
 * THREADING / ALLOCATION
 *
 *   Thread-safe. Lookups take a slim reader lock and, on a cache miss, an
 *   exclusive one and a malloc. That means apr_str() IS NOT SAFE ON AN AUDIO
 *   CALLBACK. Nothing user-facing belongs there anyway; report through
 *   err.h/log.h and localize at the point of display.
 *
 *   Cached strings live for the life of the process (or until the language
 *   changes), so a returned pointer stays valid until apr_str_set_language()
 *   is called. Do not free it.
 */
#ifndef APPRECORDER_STRINGS_H
#define APPRECORDER_STRINGS_H

/* ---------------------------------------------------------------------------
 * THE ID TABLE. One place. Read by C and by res/strings.rc.
 *
 * Everything from here to the end of APR_STR_PLURAL_LIST must stay
 * preprocessor-only: rc.exe reads this header too and cannot parse C.
 *
 * Numbering: singular ids from 1001; plural bases from 1200 spaced by 8 (each
 * plural occupies base+0 .. base+5, one per CLDR category); the UI shell's own
 * ids sit at 1300+ so that the CLI block, the plural bases and the UI block can
 * each grow without three agents renumbering each other. Keep every id inside
 * [APR_STR_ID_MIN, APR_STR_ID_MAX).
 * ------------------------------------------------------------------------- */

#define APR_STR_ID_MIN 1000
#define APR_STR_ID_MAX 1400

/* X(NAME, id) -- a plain string. Referenced in C as APR_S_NAME. */

/* WHY THE LIST IS CUT INTO GROUPS, AND WHY THAT IS NOT COSMETIC
 *
 *   res/strings.rc expands one of these lists inside a single STRINGTABLE, and
 *   rc.exe gives up -- "fatal error RC10056:", with nothing after the colon --
 *   once the text a single macro invocation expands to gets large. Measured on
 *   the 10.0.22621 rc.exe: about 60 entries of ordinary help-text length go
 *   through and about 80 do not. So the catalog is declared in named groups,
 *   each emitted into its own STRINGTABLE block, and APR_STR_LIST is their
 *   sum.
 *
 *   ADDING A STRING: put it in the group it belongs to, and give the .rc its
 *   text in the matching block of every LANGUAGE section. Starting a NEW group
 *   -- which is what a large new area of the product should do rather than
 *   growing an existing group past the limit -- means adding it here, to
 *   APR_STR_LIST at the end of this section, and to the English section of
 *   res/strings.rc. Nothing else in the product knows the groups exist: C code,
 *   the generated enum and tests/test_strings.c all walk APR_STR_LIST.
 * ------------------------------------------------------------------------- */

/* ---- the product itself, and what both front ends share. ---- */
#define APR_STR_LIST_CORE(X)                                                   \
    X(APP_NAME,                  1001)                                         \
    X(APP_TAGLINE,               1002)                                         \
    X(CLI_USAGE_HEADER,          1010)                                         \
    X(CLI_OPT_PID,               1011)                                         \
    X(CLI_OPT_DEVICE,            1012)                                         \
    X(CLI_OPT_OUT,               1013)                                         \
    X(CLI_OPT_FORMAT,            1014)                                         \
    X(CLI_OPT_LANG,              1015)                                         \
    X(STATUS_RECORDING_TO,       1020)                                         \
    X(STATUS_STOPPED,            1021)                                         \
    X(STATUS_FINISHING,          1022)                                         \
    X(WARN_SOURCE_MUTED,         1030)                                         \
    X(WARN_SOURCE_EXITED,        1031)                                         \
    X(ERR_FILE_OPEN,             1040)                                         \
    X(ERR_UNKNOWN_FORMAT,        1041)                                         \
    X(ERR_NO_SOURCES,            1042)                                         \
    X(ERR_SOURCE_ALREADY_ON_BUS, 1043)                                         \
    X(NODE_KIND_SOURCE,          1050)                                         \
    X(NODE_KIND_BUS,             1051)                                         \
    X(NODE_KIND_ACTION,          1052)                                         \
    X(UIA_SOURCE_FEEDS,          1053)                                         \
    X(UIA_BUS_FEEDS,             1054)

/* ---- the command line: usage text, one entry per printed line. ---- */
#define APR_STR_LIST_CLI(X)                                                    \
    X(CLI_COMMANDS_HEADER,         1060)                                      \
    X(CLI_CMD_RECORD,              1061)                                      \
    X(CLI_CMD_LIST_APPS,           1062)                                      \
    X(CLI_CMD_LIST_DEVICES,        1063)                                      \
    X(CLI_CMD_HELP,                1064)                                      \
    X(CLI_CMD_VERSION,             1065)                                      \
    X(CLI_SOURCES_HEADER,          1066)                                      \
    X(CLI_OPT_BUS,                 1067)                                      \
    X(CLI_OPT_EXE,                 1068)                                      \
    X(CLI_OPT_FAKE,                1069)                                      \
    X(CLI_OPT_GAIN,                1070)                                      \
    X(CLI_OPT_SYSTEM_MINUS_TREE,   1071)                                      \
    X(CLI_OUTPUTS_HEADER,          1072)                                      \
    X(CLI_OPT_BITRATE,             1073)                                      \
    X(CLI_OPT_QUALITY,             1074)                                      \
    X(CLI_SESSION_HEADER,          1075)                                      \
    X(CLI_OPT_RATE,                1076)                                      \
    X(CLI_OPT_CHANNELS,            1077)                                      \
    X(CLI_OPT_DURATION,            1078)                                      \
    X(CLI_OPT_DRY_RUN,             1079)                                      \
    X(CLI_OPT_JSON,                1080)                                      \
    X(CLI_OPT_QUIET,               1081)                                      \
    X(CLI_OPT_ALL,                 1082)                                      \
    X(CLI_OPT_LOG_LEVEL,           1083)                                      \
    X(CLI_OPT_LOG_FILE,            1084)                                      \
    X(CLI_EXIT_HEADER,             1085)                                      \
    X(CLI_EXIT_OK,                 1086)                                      \
    X(CLI_EXIT_USAGE,              1087)                                      \
    X(CLI_EXIT_CONFIG,             1088)                                      \
    X(CLI_EXIT_NOT_FOUND,          1089)                                      \
    X(CLI_EXIT_OUTPUT,             1090)                                      \
    X(CLI_EXIT_CAPTURE,            1091)                                      \
    X(CLI_EXIT_INCOMPLETE,         1092)                                      \
    X(CLI_EXIT_INTERNAL,           1093)                                      \
    X(CLI_EXAMPLES_HEADER,         1094)                                      \
    X(CLI_EXAMPLE_ONE,             1095)                                      \
    X(CLI_EXAMPLE_TWO,             1096)                                      \
    X(CLI_STOP_HINT,               1097)                                      \
    X(CLI_VERSION_LINE,            1098)

/* ---- the command line: what it says when it refuses. ---- */
#define APR_STR_LIST_CLI_ERR(X)                                                \
    X(ERR_UNKNOWN_COMMAND,         1100)                                      \
    X(ERR_UNKNOWN_OPTION,          1101)                                      \
    X(ERR_OPTION_NEEDS_VALUE,      1102)                                      \
    X(ERR_BAD_NUMBER,              1103)                                      \
    X(ERR_OUT_OF_RANGE,            1104)                                      \
    X(ERR_GAIN_WITHOUT_SOURCE,     1105)                                      \
    X(ERR_BUS_HAS_NO_SOURCE,       1106)                                      \
    X(ERR_BUS_HAS_NO_OUTPUT,       1107)                                      \
    X(ERR_PID_NOT_RUNNING,         1108)                                      \
    X(ERR_EXE_NOT_PLAYING,         1109)                                      \
    X(ERR_EXE_AMBIGUOUS,           1110)                                      \
    X(ERR_DEVICE_NOT_FOUND,        1111)                                      \
    X(ERR_DEVICE_AMBIGUOUS,        1112)                                      \
    X(ERR_NO_EXTENSION,            1113)                                      \
    X(ERR_OUTPUT_NOT_WRITABLE,     1114)                                      \
    X(ERR_TOO_MANY_BUSES,          1115)                                      \
    X(ERR_TOO_MANY_SOURCES,        1116)                                      \
    X(ERR_TOO_MANY_OUTPUTS,        1117)                                      \
    X(ERR_CAPTURE_START,           1118)                                      \
    X(ERR_DUPLICATE_OUTPUT,        1119)                                      \
    X(ERR_OPTION_NOT_FOR_COMMAND,  1120)

/* ---- the command line: warnings, progress, and the two listings. ---- */
#define APR_STR_LIST_CLI_MSG(X)                                                \
    X(WARN_SYSTEM_CAPTURE_SCOPE,   1130)                                      \
    X(WARN_SYSTEM_CAPTURE_TREE,    1131)                                      \
    X(WARN_SYSTEM_CAPTURE_MEMBERS, 1132)                                      \
    X(WARN_SYSTEM_CAPTURE_LAUNCHER, 1133)                                     \
    X(WARN_STILL_FINISHING,        1134)                                      \
    X(WARN_ACTION_FAILED,          1135)                                      \
    X(STATUS_DRY_RUN_HEADER,       1140)                                      \
    X(STATUS_DRY_RUN_OK,           1141)                                      \
    X(STATUS_PLAN_SESSION,         1142)                                      \
    X(STATUS_PLAN_BUS,             1143)                                      \
    X(STATUS_PLAN_SOURCE,          1144)                                      \
    X(STATUS_PLAN_OUTPUT,          1145)                                      \
    X(STATUS_WROTE,                1146)                                      \
    X(STATUS_ELAPSED,              1147)                                      \
    X(LIST_APPS_HEADER,            1150)                                      \
    X(LIST_APPS_EMPTY,             1151)                                      \
    X(LIST_APPS_ROW,               1152)                                      \
    X(LIST_APPS_ROW_MUTED,         1153)                                      \
    X(LIST_APPS_ROW_IDLE,          1154)                                      \
    X(LIST_DEVICES_HEADER,         1155)                                      \
    X(LIST_DEVICES_EMPTY,          1156)                                      \
    X(LIST_DEVICES_ROW,            1157)                                      \
    X(LIST_DEVICES_ROW_DEFAULT,    1158)                                      \
    X(LIST_DEVICES_ID_LINE,        1159)                                      \
    X(SOURCE_KIND_PROCESS,         1160)                                      \
    X(SOURCE_KIND_SYSTEM_MINUS_TREE, 1161)                                    \
    X(SOURCE_KIND_DEVICE,          1162)                                      \
    X(SOURCE_KIND_FAKE,            1163)                                      \
    X(CLI_DEFAULT_BUS_NAME,        1164)

/* ---- the window: menu commands, pane names, and what a screen reader
 * reads. Menu text carries its accelerator after a tab so a translator keeps
 * label and shortcut together, and the ampersand marks the mnemonic -- the
 * ampersand is the keyboard path to the item, not decoration. ---- */
#define APR_STR_LIST_UI(X)                                                    \
    X(UI_TITLE_UNTITLED,           1300)                                      \
    X(UI_TITLE_SESSION,            1301)                                      \
    X(UI_PANE_CANVAS,              1302)                                      \
    X(UI_PANE_TREE,                1303)                                      \
    X(UI_PANE_STATUS,              1304)                                      \
    X(UI_DESC_CANVAS,              1305)                                      \
    X(UI_DESC_TREE,                1306)                                      \
    X(UI_STATUS_READY,             1307)                                      \
    X(UI_PANE_SPLITTER,            1308)                                      \
    X(UI_DESC_SPLITTER,            1309)                                      \
    X(UI_MENU_FILE,                1310)                                      \
    X(UI_MENU_FILE_NEW,            1311)                                      \
    X(UI_MENU_FILE_OPEN,           1312)                                      \
    X(UI_MENU_FILE_SAVE,           1313)                                      \
    X(UI_MENU_FILE_SAVE_AS,        1314)                                      \
    X(UI_MENU_FILE_EXIT,           1315)                                      \
    X(UI_MENU_EDIT,                1320)                                      \
    X(UI_MENU_ADD_SOURCE,          1321)                                      \
    X(UI_MENU_ADD_BUS,             1322)                                      \
    X(UI_MENU_ADD_ACTION,          1323)                                      \
    X(UI_MENU_CONNECT,             1324)                                      \
    X(UI_MENU_REMOVE,              1325)                                      \
    X(UI_MENU_RECORDING,           1330)                                      \
    X(UI_MENU_RECORD_START,        1331)                                      \
    X(UI_MENU_RECORD_STOP,         1332)                                      \
    X(UI_MENU_VIEW,                1340)                                      \
    X(UI_MENU_VIEW_TREE,           1341)                                      \
    X(UI_MENU_VIEW_DARK,           1342)                                      \
    X(UI_MENU_VIEW_NEXT_PANE,      1343)                                      \
    X(UI_MENU_HELP,                1350)                                      \
    X(UI_MENU_HELP_KEYS,           1351)                                      \
    X(UI_MENU_HELP_ABOUT,          1352)

/* Every plain string, in declaration order. This is what C, the generated
 * enum and tests/test_strings.c walk; the groups above exist only so that
 * res/strings.rc can emit them in rc.exe-sized pieces. */
#define APR_STR_LIST(X)                                                        \
    APR_STR_LIST_CORE(X)                                                       \
    APR_STR_LIST_CLI(X)                                                        \
    APR_STR_LIST_CLI_ERR(X)                                                    \
    APR_STR_LIST_CLI_MSG(X)                                                   \
    APR_STR_LIST_UI(X)


/* X(NAME, base) -- a plural string. Referenced in C as APR_S_NAME, which is
 * the BASE id; the six CLDR categories live at base+APR_PLURAL_ZERO through
 * base+APR_PLURAL_OTHER. Never look a plural id up directly with apr_str();
 * use apr_str_plural(). */
#define APR_STR_PLURAL_LIST(X)                                                 \
    X(N_SOURCES,                 1200)                                         \
    X(N_BUSES,                   1208)                                         \
    X(N_FRAMES_LOST,             1216)

#ifndef RC_INVOKED

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "err.h"

/* ---------------------------------------------------------------------------
 * Generated ids
 * ------------------------------------------------------------------------- */

#define APR_STR__ENUM_ENTRY(name, id) APR_S_##name = (id),

typedef enum AprStrId {
    APR_S__NONE = 0,
    APR_STR_LIST(APR_STR__ENUM_ENTRY)
    APR_STR_PLURAL_LIST(APR_STR__ENUM_ENTRY)
    APR_S__FORCE_INT = 0x7fffffff
} AprStrId;

#undef APR_STR__ENUM_ENTRY

/* ---------------------------------------------------------------------------
 * CLDR plural categories
 *
 * The order IS the resource layout: a plural entry occupies base+0 through
 * base+5 in exactly this order. Do not reorder.
 *
 * English selects only ONE and OTHER. Arabic selects all six. NOTHING in this
 * API assumes two -- a language added later that uses, say, FEW without TWO
 * needs no code change here, only its own six .rc entries and its rule.
 * ------------------------------------------------------------------------- */
typedef enum AprPluralCategory {
    APR_PLURAL_ZERO  = 0,
    APR_PLURAL_ONE   = 1,
    APR_PLURAL_TWO   = 2,
    APR_PLURAL_FEW   = 3,
    APR_PLURAL_MANY  = 4,
    APR_PLURAL_OTHER = 5,
    APR_PLURAL_COUNT = 6
} AprPluralCategory;

/* Largest insert number any catalog string may use, and therefore the most
 * arguments apr_str_format accepts. Raise it here if a string ever needs more;
 * the internal argument array is sized from it. */
#define APR_STR_MAX_ARGS 8

/* ---------------------------------------------------------------------------
 * Language selection
 * ------------------------------------------------------------------------- */

/* Coverage of a declared language.
 *
 * COMPLETE is a promise checked by tests/test_strings.c: every id in the two
 * lists above must have a non-empty string in that language, or the test fails
 * and so does `build.cmd Debug test`.
 *
 * PARTIAL means "the mechanism is wired, the copy is not written yet".
 * Anything missing falls back to the primary language. Flipping a language
 * from PARTIAL to COMPLETE is a one-line change in src/i18n/strings.c and is
 * the gate the translation pass has to clear. */
typedef enum AprStrCoverage {
    APR_STR_PARTIAL  = 0,
    APR_STR_COMPLETE = 1
} AprStrCoverage;

typedef struct AprStrLanguage {
    LANGID          langid;   /* exactly the LANGUAGE of a block in strings.rc */
    const wchar_t  *tag;      /* BCP-47-ish tag, for the CLI and for logs      */
    AprStrCoverage  coverage;
    int             rtl;      /* 1 when the script runs right to left          */
} AprStrLanguage;

/* One catalog entry as declared in the lists above. */
typedef struct AprStrEntry {
    AprStrId    id;        /* plain string id, or the base id of a plural  */
    const char *name;      /* "APR_S_APP_NAME" -- diagnostics only, ASCII  */
    int         is_plural; /* 1 => occupies id+0 .. id+APR_PLURAL_OTHER    */
} AprStrEntry;

/* Resolve, and adopt, a display language.
 *
 * `lang` is matched to a declared language by PRIMARY language id, so en-GB
 * (0x0809) selects the en-US block and ar-EG selects the ar-SA block. With no
 * match the primary language is adopted instead and APR_E_NOT_FOUND is
 * returned -- the app stays usable and the caller can say so.
 *
 * Resets the string cache; pointers returned by earlier apr_str() calls must
 * not be used afterwards. */
AprErr apr_str_set_language(LANGID lang);

/* Adopt the thread's UI language (GetThreadUILanguage). Idempotent, and called
 * automatically on first lookup, so calling it is optional. The CLI front end
 * deliberately calls apr_str_set_language() with English instead -- design 6.2
 * makes the CLI an automation surface, English by default. */
AprErr apr_str_init(void);

/* The language currently in effect. Always one of the declared languages. */
LANGID apr_str_language(void);

/* Nonzero when the current language is written right to left.
 *
 * This is the only direction source in the product. See DIRECTION above. */
int apr_str_is_rtl(void);

/* ---------------------------------------------------------------------------
 * Lookup
 * ------------------------------------------------------------------------- */

/* The string for `id` in the current language.
 *
 * NEVER returns NULL and never returns an empty string. Resolution order:
 *   1. the current language;
 *   2. the primary language (English);
 *   3. a loud, obviously-wrong placeholder naming the id.
 * A missing string is meant to be visible in a screenshot, not invisible in a
 * layout. Returned text is owned by this module and stays valid until the
 * language changes. */
const wchar_t *apr_str(AprStrId id);

/* The correct plural form of `base_id` for `n`, in the current language.
 *
 * `base_id` is the id from APR_STR_PLURAL_LIST. `n` is the real count -- do
 * not pre-clamp it to 0/1/many, which is precisely the bug this API exists to
 * prevent. Negative counts use |n|, matching CLDR's `n` operand.
 *
 * The category is chosen using the rules of the language that actually
 * supplies the text, so an untranslated Arabic plural falling back to English
 * is selected with English rules rather than indexed with Arabic ones. */
const wchar_t *apr_str_plural(AprStrId base_id, int64_t n);

/* ---------------------------------------------------------------------------
 * Formatting
 *
 * All of these write into caller storage, NUL-terminate whenever cch >= 1, and
 * return the number of characters written excluding the terminator. They never
 * fail silently: a format that cannot be satisfied (too few arguments, a
 * FormatMessage refusal) produces a visible placeholder, never an empty or
 * half-built string.
 * ------------------------------------------------------------------------- */

/* Expand `fmt` -- FormatMessageW syntax, %1!s! .. %8!s! -- with `nargs` wide
 * strings. Public because a caller that already holds a string (typically from
 * apr_str_plural) still needs to insert into it. */
size_t apr_str_format_string(const wchar_t *fmt, wchar_t *buf, size_t cch,
                             const wchar_t *const *args, size_t nargs);

/* apr_str(id), then apr_str_format_string(). */
size_t apr_str_format(AprStrId id, wchar_t *buf, size_t cch,
                      const wchar_t *const *args, size_t nargs);

/* apr_str_plural(base_id, n), then expand.
 *
 * %1 IS ALWAYS THE COUNT, already rendered through apr_str_number(). Caller
 * arguments start at %2. This is a convention, and it is load-bearing: it is
 * why a translator can write a zero form that never mentions the number
 * ("No sources") beside an other form that does, and why the count gets
 * digit-shaped in exactly one place. */
size_t apr_str_plural_format(AprStrId base_id, int64_t n,
                             wchar_t *buf, size_t cch,
                             const wchar_t *const *args, size_t nargs);

/* Render `n` as display text.
 *
 * The single place a number becomes user-visible digits. Western (Hindu-Arabic)
 * digits today, which is standard Saudi UI practice per design 6.2; when the
 * author confirms whether Arabic-Indic digits are wanted, this function is the
 * only thing that changes. */
size_t apr_str_number(int64_t n, wchar_t *buf, size_t cch);

/* Render `scaled` / 10^`decimals` as display text -- a gain in dB, a duration
 * in seconds, anything that is not a whole number.
 *
 * The caller supplies the value ALREADY MULTIPLIED by 10^decimals as an
 * integer, so no float ever reaches this layer and the rounding decision stays
 * where the quantity is understood. `decimals` is 0..6.
 *
 * Lives beside apr_str_number for the same reason apr_str_number exists: this
 * is the only other place a number becomes user-visible digits, so the
 * Western-versus-Arabic-Indic decision and the decimal separator are settled
 * in one file rather than in every caller's swprintf. Returns 0 and writes an
 * empty string if the result would not fit -- half a number is a lie. */
size_t apr_str_number_fixed(int64_t scaled, int decimals,
                            wchar_t *buf, size_t cch);

/* ---------------------------------------------------------------------------
 * Plural rules, exposed
 * ------------------------------------------------------------------------- */

/* The CLDR cardinal plural category of `n` for `lang`.
 *
 * Arabic (ar):  zero n=0; one n=1; two n=2; few n%100 in 3..10;
 *               many n%100 in 11..99; other otherwise (100, 101, 102, 200...).
 * English (en): one n=1; other otherwise.
 * Any other language uses the English rule, which is also CLDR's shape for the
 * large one/other bucket. */
AprPluralCategory apr_plural_category(LANGID lang, int64_t n);

/* ---------------------------------------------------------------------------
 * Introspection -- how the completeness check sees the catalog
 * ------------------------------------------------------------------------- */

int                   apr_str_language_count(void);
const AprStrLanguage *apr_str_language_at(int index);  /* NULL when out of range */

int                   apr_str_catalog_count(void);
const AprStrEntry    *apr_str_catalog_at(int index);   /* NULL when out of range */

/* Read `id` from EXACTLY `lang`, with no fallback of any kind.
 *
 * This is what makes the completeness check honest: LoadStringW and the
 * resource loader's own language search would happily hand back the English
 * string for a missing Arabic one, and the check would pass while the product
 * shipped English text in an Arabic UI.
 *
 * Returns the number of characters written (0 for a present but empty entry),
 * or -1 when this language has no entry for this id. */
int apr_str_probe(LANGID lang, AprStrId id, wchar_t *buf, size_t cch);

#endif /* !RC_INVOKED */
#endif /* APPRECORDER_STRINGS_H */
