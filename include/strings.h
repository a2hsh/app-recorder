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
 *   right, actions on the left).
 *
 *   AN EARLIER VERSION OF THIS PARAGRAPH SAID WS_EX_LAYOUTRTL "does NOT mirror
 *   anything we paint ourselves". THAT IS BACKWARDS and it was corrected in
 *   ui_app.h and in design 6.2; the sentence is left here only long enough to
 *   say so. The style gives the window a MIRRORED DEVICE CONTEXT -- our own
 *   drawing is exactly what it reflects -- and it is INHERITED by children.
 *   So a painted window never carries it, and the canvas and its node windows
 *   mirror through layout arithmetic instead (apr_ui_mirror_rect).
 *   Hardcoding a flow direction anywhere is a review failure.
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
 * ids sit at 1300+ and the canvas's own at 1400+, so that the CLI block, the
 * plural bases, the UI shell and the canvas can each grow without four agents
 * renumbering each other. Error reasons -- the sentences errmsg.h resolves an
 * AprErr into -- sit at 1800+ (one per hand-tabled WASAPI code) and 1850+
 * (one per error kind, plus the refusals a raise site names for itself).
 * Keep every id inside [APR_STR_ID_MIN, APR_STR_ID_MAX).
 * ------------------------------------------------------------------------- */

#define APR_STR_ID_MIN 1000
#define APR_STR_ID_MAX 2000

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

/* ---- the product itself, and what both front ends share.
 *
 * ACTION_NAME_* is one entry per registered action, and it lives here rather
 * than beside the encoder because an encoder's vtable must not carry prose:
 * AprActionVTable::display_name_id names one of these and the CLI and both UI
 * panes resolve it at the point of display. They are in the order
 * core/registry.c lists the actions. Adding an encoder adds one line here and
 * one in every LANGUAGE block of res/strings.rc, and nothing else. ---- */
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
    X(WARN_SOURCE_RECOVERED,     1032)                                         \
    X(WARN_EXCLUSION_HELD,       1033)                                         \
    X(ERR_FILE_OPEN,             1040)                                         \
    X(ERR_UNKNOWN_FORMAT,        1041)                                         \
    X(ERR_NO_SOURCES,            1042)                                         \
    X(ERR_SOURCE_ALREADY_ON_BUS, 1043)                                         \
    X(NODE_KIND_SOURCE,          1050)                                         \
    X(NODE_KIND_BUS,             1051)                                         \
    X(NODE_KIND_ACTION,          1052)                                         \
    X(UIA_SOURCE_FEEDS,          1053)                                         \
    X(UIA_BUS_FEEDS,             1054)                                         \
    X(ACTION_NAME_WAV,           1055)                                         \
    X(ACTION_NAME_MP3,           1056)                                         \
    X(ACTION_NAME_OGG,           1057)                                         \
    X(ACTION_NAME_NONE,          1058)

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
    X(CLI_OPT_OUT_TOKENS,          1173)                                      \
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
    X(CLI_VERSION_LINE,            1098)                                      \
    X(CLI_OPT_FAKE_HEALTH,         1099)

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
    X(ERR_OPTION_NOT_FOR_COMMAND,  1120)                                     \
    X(ERR_OUTPUT_UNSUPPORTED,      1121)                                     \
    X(ERR_NOTHING_WAS_WRITTEN,     1122)

/* ---- the command line: warnings, progress, and the two listings. ---- */
#define APR_STR_LIST_CLI_MSG(X)                                                \
    X(WARN_SYSTEM_CAPTURE_SCOPE,   1130)                                      \
    X(WARN_SYSTEM_CAPTURE_TREE,    1131)                                      \
    X(WARN_SYSTEM_CAPTURE_MEMBERS, 1132)                                      \
    X(WARN_SYSTEM_CAPTURE_LAUNCHER, 1133)                                     \
    X(WARN_STILL_FINISHING,        1134)                                      \
    X(WARN_ACTION_FAILED,          1135)                                      \
    X(WARN_OUTPUT_RENAMED,         1136)                                      \
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
    X(CLI_DEFAULT_BUS_NAME,        1164)                                     \
    X(WARN_BUS_DROPPED,            1165)                                     \
    X(WARN_OUTPUT_DEGRADED,        1166)

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
    X(UI_MENU_DISCONNECT,          1326)                                      \
    X(UI_MENU_RENAME_BUS,          1327)                                      \
    X(UI_MENU_REMOVE_OUTPUT,       1328)                                      \
    X(UI_MENU_RECORDING,           1330)                                      \
    X(UI_MENU_RECORD_START,        1331)                                      \
    X(UI_MENU_RECORD_STOP,         1332)                                      \
    X(UI_MENU_VIEW,                1340)                                      \
    X(UI_MENU_VIEW_TREE,           1341)                                      \
    X(UI_MENU_VIEW_DARK,           1342)                                      \
    X(UI_MENU_VIEW_NEXT_PANE,      1343)                                      \
    X(UI_MENU_HIDE_TO_TRAY,        1344)                                      \
    X(UI_MENU_HELP,                1350)                                      \
    X(UI_MENU_HELP_KEYS,           1351)                                      \
    X(UI_MENU_HELP_ABOUT,          1352)

/* ---- the canvas: what a screen reader says when it lands on a node, and
 * what it says when the graph changes under the user's hands.
 *
 * THESE ARE SENTENCES, NOT LABELS, and that is deliberate. A node on the
 * canvas is not read by looking at it; it is read by being focused, so the
 * name has to carry the node's identity AND its kind, and the description has
 * to carry its EDGES -- what it feeds, what feeds it -- because an edge is
 * drawn on the parent window and has no element of its own to land on.
 *
 * The list inserts (%1!s! in UI_NODE_DESC_*) are built by folding the two
 * UI_LIST_* patterns below over the names. That is the only sanctioned way to
 * join user data in this product: the separator and the "and" are catalog
 * entries, so a translator controls both, rather than a comma frozen into C.
 * ---- */
#define APR_STR_LIST_UI_NODE(X)                                               \
    X(UI_NODE_SOURCE,              1400)                                      \
    X(UI_NODE_SOURCE_GAIN,         1401)                                      \
    X(UI_NODE_BUS,                 1402)                                      \
    X(UI_NODE_ACTION,              1403)                                      \
    X(UI_NODE_LABEL_SOURCE,        1404)                                      \
    X(UI_NODE_LABEL_BUS,           1405)                                      \
    X(UI_NODE_LABEL_ACTION,        1406)                                      \
    X(UI_NODE_LABEL_SOURCE_GAIN,   1407)                                      \
    X(UI_NODE_DESC_SOURCE_FEEDING, 1410)                                      \
    X(UI_NODE_DESC_SOURCE_ALONE,   1411)                                      \
    X(UI_NODE_DESC_BUS_FULL,       1412)                                      \
    X(UI_NODE_DESC_BUS_NO_SOURCES, 1413)                                      \
    X(UI_NODE_DESC_BUS_NO_OUTPUTS, 1414)                                      \
    X(UI_NODE_DESC_BUS_EMPTY,      1415)                                      \
    X(UI_NODE_DESC_ACTION,         1416)                                      \
    X(UI_LIST_PAIR,                1420)                                      \
    X(UI_LIST_MORE,                1421)                                      \
    X(UI_ANN_CONNECT_START,        1430)                                      \
    X(UI_ANN_CONNECT_DONE,         1431)                                      \
    X(UI_ANN_CONNECT_ALREADY,      1432)                                      \
    X(UI_ANN_CONNECT_REFUSED,      1433)                                      \
    X(UI_ANN_DISCONNECT_START,     1434)                                      \
    X(UI_ANN_DISCONNECT_DONE,      1435)                                      \
    X(UI_ANN_DISCONNECT_NONE,      1436)                                      \
    X(UI_ANN_CANCELLED,            1437)                                      \
    X(UI_ANN_REMOVED,              1438)                                      \
    X(UI_ANN_REMOVE_REFUSED,       1439)                                      \
    X(UI_ANN_GAIN,                 1440)                                      \
    X(UI_ANN_GAIN_NO_EDGE,         1441)                                      \
    X(UI_ANN_NO_EDGE_DOWN,         1442)                                      \
    X(UI_ANN_NO_EDGE_UP,           1443)                                      \
    X(UI_ANN_CANVAS_EMPTY,         1444)                                      \
    X(UI_ANN_NOT_YET,              1445)                                      \
    X(UI_ANN_CONNECT_NOT_SOURCE,   1446)                                      \
    X(UI_ANN_GAIN_NOT_SOURCE,      1447)                                      \
    X(UI_ANN_EDIT_FAILED,          1448)

/* ---- one line per canvas operation, for Help > Keyboard Shortcuts.
 *
 * There is exactly one binding table (ui_canvas.h) and it is the thing that
 * dispatches keys AND the thing this screen renders, so a shortcut cannot be
 * documented as one key and implemented as another. Every operation the canvas
 * can perform has an entry here, and tests/test_ui_canvas.c fails if one does
 * not -- which is how "no mouse-only paths" stops being a promise. ---- */
#define APR_STR_LIST_UI_KEYS(X)                                               \
    X(UI_KEY_NEXT_NODE,            1460)                                      \
    X(UI_KEY_PREV_NODE,            1461)                                      \
    X(UI_KEY_NEXT_IN_COLUMN,       1462)                                      \
    X(UI_KEY_PREV_IN_COLUMN,       1463)                                      \
    X(UI_KEY_DOWNSTREAM,           1464)                                      \
    X(UI_KEY_UPSTREAM,             1465)                                      \
    X(UI_KEY_FIRST,                1466)                                      \
    X(UI_KEY_LAST,                 1467)                                      \
    X(UI_KEY_CONNECT,              1468)                                      \
    X(UI_KEY_DISCONNECT,           1469)                                      \
    X(UI_KEY_REMOVE,               1470)                                      \
    X(UI_KEY_ADD_SOURCE,           1471)                                      \
    X(UI_KEY_ADD_BUS,              1472)                                      \
    X(UI_KEY_ADD_ACTION,           1473)                                      \
    X(UI_KEY_GAIN_UP,              1474)                                      \
    X(UI_KEY_GAIN_DOWN,            1475)                                      \
    X(UI_KEY_CANCEL,               1476)                                      \
    X(UI_KEY_DESCRIBE,             1477)                                      \
    X(UI_KEYNAME_TAB,              1480)                                      \
    X(UI_KEYNAME_ENTER,            1481)                                      \
    X(UI_KEYNAME_SPACE,            1482)                                      \
    X(UI_KEYNAME_DELETE,           1483)                                      \
    X(UI_KEYNAME_ESCAPE,           1484)                                      \
    X(UI_KEYNAME_HOME,             1485)                                      \
    X(UI_KEYNAME_END,              1486)                                      \
    X(UI_KEYNAME_LEFT,             1487)                                      \
    X(UI_KEYNAME_RIGHT,            1488)                                      \
    X(UI_KEYNAME_UP,               1489)                                      \
    X(UI_KEYNAME_DOWN,             1490)                                      \
    X(UI_KEYNAME_PLUS,             1491)                                      \
    X(UI_KEYNAME_MINUS,            1492)                                      \
    X(UI_KEYNAME_NUM_PLUS,         1493)                                      \
    X(UI_KEYNAME_NUM_MINUS,        1494)                                      \
    X(UI_KEYNAME_PERIOD,           1495)                                      \
    X(UI_KEYNAME_FUNCTION,         1496)

/* ---- the structure panel: what the TreeView says out loud.
 *
 * A TreeView item's TEXT IS ITS ACCESSIBLE NAME -- there is no second string a
 * screen reader reads instead. So each entry here is the whole row: what the
 * node is, what kind it is, what it is connected to, and any state that
 * changes what the recording will contain. A row that reads "Teams" tells a
 * screen reader user nothing they did not already know; a row that reads
 * "Teams, application, muted in Windows so it records silence, feeding Main
 * Mix" tells them the thing they cannot see.
 *
 * THE STATE PHRASES (UI_TREE_STATE_*) ARE INSERTS, NOT SUFFIXES GLUED IN C.
 * They are complete phrases occupying one positional insert, so a translator
 * can move them anywhere in the sentence -- which Arabic needs, because an
 * adjectival phrase follows its noun there and a state clause does not sit
 * where English puts it. The rule this respects is "a translator must be able
 * to reorder", not "one row is one catalog entry regardless of cost": four
 * whole-sentence variants beat sixteen. ---- */
#define APR_STR_LIST_UI_TREE(X)                                               \
    X(UI_TREE_NAME,                1500)                                      \
    X(UI_TREE_DESC,                1501)                                      \
    X(UI_TREE_EMPTY,               1502)                                      \
    X(UI_TREE_UNASSIGNED_GROUP,    1503)                                      \
    X(UI_TREE_BUS,                 1504)                                      \
    X(UI_TREE_BUS_RECORDING,       1505)                                      \
    X(UI_TREE_SOURCE,              1506)                                      \
    X(UI_TREE_SOURCE_SHARED,       1507)                                      \
    X(UI_TREE_SOURCE_STATE,        1508)                                      \
    X(UI_TREE_SOURCE_STATE_SHARED, 1509)                                      \
    X(UI_TREE_SOURCE_UNUSED,       1510)                                      \
    X(UI_TREE_SOURCE_UNUSED_STATE, 1511)                                      \
    X(UI_TREE_STATE_MUTED,         1512)                                      \
    X(UI_TREE_STATE_EXITED,        1513)                                      \
    X(UI_TREE_ACTION,              1514)                                      \
    X(UI_TREE_ACTION_FAILED,       1515)

/* ---- session files: the options, and everything a load has to be able to
 * say when what the file describes is not what the machine currently has.
 *
 * THE RESOLUTION MESSAGES ARE WHOLE SENTENCES CARRYING TWO NAMES, and that is
 * not verbosity. "Teams could not be found" is useless; "the session recorded
 * Teams at C:\Apps\Teams\Teams.exe and it is now running from
 * D:\Program Files\Teams\Teams.exe" tells the reader what happened and lets
 * them decide whether it is the same program. Every substitution the resolver
 * can make has an entry here that names both halves.
 *
 * The EXCLUDE entries are separate from the ones in APR_STR_LIST_CLI_MSG on
 * purpose: those describe a capture the user just asked for on the command
 * line, and these describe one a FILE asked for, which is the case design
 * 4.1.1 says must never proceed without confirmation. Different situation,
 * different sentence. ---- */
#define APR_STR_LIST_SESSION(X)                                               \
    X(CLI_CMD_SAVE_SESSION,          1520)                                    \
    X(CLI_OPT_SESSION,               1521)                                    \
    X(CLI_OPT_ALLOW_SYSTEM_CAPTURE,  1522)                                    \
    X(CLI_OPT_ALLOW_MISSING,         1523)                                    \
    X(ERR_SESSION_NOT_FOUND,         1524)                                    \
    X(ERR_SESSION_UNREADABLE,        1525)                                    \
    X(ERR_SESSION_NOT_JSON,          1526)                                    \
    X(ERR_SESSION_TRUNCATED,         1527)                                    \
    X(ERR_SESSION_NOT_A_SESSION,     1528)                                    \
    X(ERR_SESSION_TOO_NEW,           1529)                                    \
    X(ERR_SESSION_TOO_OLD,           1530)                                    \
    X(ERR_SESSION_BAD_FIELD,         1531)                                    \
    X(ERR_SESSION_BAD_VALUE,         1532)                                    \
    X(ERR_SESSION_TOO_MANY,          1533)                                    \
    X(ERR_SESSION_DANGLING_REF,      1534)                                    \
    X(ERR_SESSION_WITH_SOURCES,      1535)                                    \
    X(ERR_SESSION_NEEDED,            1536)                                    \
    X(ERR_SESSION_NOT_WRITTEN,       1537)                                    \
    X(WARN_SESSION_FROM_NEWER,       1538)                                    \
    X(WARN_SESSION_UNKNOWN_KEYS,     1539)                                    \
    X(ERR_SESSION_SOURCE_MISSING,    1540)                                    \
    X(ERR_SESSION_DEVICE_MISSING,    1541)                                    \
    X(ERR_SESSION_AMBIGUOUS,         1542)                                    \
    X(ERR_SESSION_AMBIGUOUS_PIDS,    1543)                                    \
    X(ERR_SESSION_NEEDS_CONSENT,     1544)                                    \
    X(WARN_SESSION_MOVED,            1545)                                    \
    X(WARN_SESSION_BY_WINDOW_CLASS,  1546)                                    \
    X(WARN_SESSION_FIRST_OF_MANY,    1547)                                    \
    X(WARN_SESSION_DEVICE_BY_NAME,   1548)                                    \
    X(WARN_SESSION_DROPPED,          1549)                                    \
    X(STATUS_SESSION_LOADED,         1550)                                    \
    X(STATUS_SESSION_SAVED,          1551)                                   \
    X(ERR_SESSION_NOT_USABLE,        1552)


/* ---- recording: what the window and the notification area say while a
 * session is actually running.
 *
 * EVERY ONE OF THESE IS SPOKEN, not merely displayed. The author is blind, so
 * a colour change is not a state change: a recording that started, a source
 * that died, a file that stopped being written all have to arrive as text a
 * screen reader reads. They reach it two ways -- a live-region change on the
 * status readout while the window is in front, and a notification-area balloon
 * when it is not, because a live region on an unfocused background window is
 * not reliably announced by any reader. ---- */
#define APR_STR_LIST_UI_REC(X)                                          \
    X(UI_STATUS_IDLE,               1600)                                 \
    X(UI_STATUS_RECORDING,          1601)                                 \
    X(UI_STATUS_FINISHING,          1602)                                 \
    X(UI_TIME_ELAPSED,              1603)                                 \
    X(UI_ANN_RECORD_STARTED,        1604)                                 \
    X(UI_ANN_RECORD_STOPPED,        1605)                                 \
    X(UI_ANN_RECORD_INCOMPLETE,     1606)                                 \
    X(UI_ANN_RECORD_FAILED,         1607)                                 \
    X(UI_ANN_ARM_FAILED,            1608)                                 \
    X(UI_ANN_SOURCE_DIED,           1609)                                 \
    X(UI_ANN_SOURCE_MUTED,          1610)                                 \
    X(UI_ANN_ACTION_FAILED,         1611)                                 \
    X(UI_ANN_ALREADY_RECORDING,     1612)                                 \
    X(UI_ANN_NOT_RECORDING,         1613)                                 \
    X(UI_ANN_NOTHING_TO_RECORD,     1614)                                 \
    X(UI_ANN_BUSY_RECORDING,        1615)                                 \
    X(UI_HEALTH_OK,                 1616)                                 \
    X(UI_HEALTH_MUTED,              1617)                                 \
    X(UI_HEALTH_DEAD,               1618)                                 \
    X(UI_PANE_RECORDING,            1619)                                 \
    X(UI_DESC_RECORDING,            1620)                                 \
    X(UI_CLOSE_TITLE,               1621)                                 \
    X(UI_CLOSE_BODY,                1622)                                 \
    X(UI_CLOSE_STOP_AND_EXIT,       1623)                                 \
    X(UI_CLOSE_KEEP_RECORDING,      1624)                                 \
    X(UI_CLOSE_TO_TRAY,             1625)                                 \
    X(UI_ANN_CLOSING_FILES,         1626)                                 \
    X(UI_ANN_OUTPUT_RENAMED,        1627)                                  \
    X(UI_ANN_CLOSE_TIMEOUT,         1628)                                  \
    X(UI_ANN_NO_TRAY,               1629)                                  \
    X(UI_ANN_SOURCE_RECOVERED,      1630)                                  \
    X(UI_ANN_EXCLUSION_HELD,        1631)


/* ---- the dialogs: the half of the product that turns "navigate a graph" into
 * "build one".
 *
 * Dialogs are standard Win32 dialog templates with standard controls, because
 * those carry the MSAA/UIA a screen reader has decades of tuning for. That
 * means every label here is also an ACCESSIBLE NAME, so each one is written to
 * be heard on its own rather than read next to a control: "Bitrate in
 * kilobits per second, 0 for the format's own default", not "Bitrate".
 *
 * The EXCLUDE entries (UI_DLG_SYSTEM_*) are worded to design 4.1.1: it records
 * EVERYTHING the machine plays and it walks the target's process tree, so
 * naming a launcher holds back everything the launcher started. Never word it
 * as "everything except X", and never make it a default. ---- */
#define APR_STR_LIST_UI_DLG(X)                                          \
    X(UI_DLG_OK,                    1650)                                 \
    X(UI_DLG_CANCEL,                1651)                                 \
    X(UI_DLG_CLOSE,                 1652)                                 \
    X(UI_DLG_REFRESH,               1653)                                 \
    X(UI_DLG_BROWSE,                1654)                                 \
    X(UI_DLG_ADD_SOURCE_TITLE,      1655)                                 \
    X(UI_DLG_SOURCE_KIND,           1656)                                 \
    X(UI_DLG_KIND_APP,              1657)                                 \
    X(UI_DLG_KIND_DEVICE,           1658)                                 \
    X(UI_DLG_KIND_SYSTEM,           1659)                                 \
    X(UI_DLG_APP_LIST,              1660)                                 \
    X(UI_DLG_DEVICE_LIST,           1661)                                 \
    X(UI_DLG_APP_ROW,               1662)                                 \
    X(UI_DLG_APP_STATE_ACTIVE,      1663)                                 \
    X(UI_DLG_APP_STATE_IDLE,        1664)                                 \
    X(UI_DLG_APP_STATE_MUTED,       1665)                                 \
    X(UI_DLG_DEVICE_ROW,            1666)                                 \
    X(UI_DLG_DEVICE_ROW_DEFAULT,    1667)                                 \
    X(UI_DLG_NO_APPS,               1668)                                 \
    X(UI_DLG_NO_DEVICES,            1669)                                 \
    X(UI_DLG_SOURCE_NAME,           1670)                                 \
    X(UI_DLG_ADD_BUS_TITLE,         1671)                                 \
    X(UI_DLG_RENAME_BUS_TITLE,      1672)                                 \
    X(UI_DLG_BUS_NAME,              1673)                                 \
    X(UI_DLG_ADD_OUTPUT_TITLE,      1674)                                 \
    X(UI_DLG_OUT_BUS,               1675)                                 \
    X(UI_DLG_OUT_FORMAT,            1676)                                 \
    X(UI_DLG_OUT_PATH,              1677)                                 \
    X(UI_DLG_OUT_BITRATE,           1678)                                 \
    X(UI_DLG_OUT_QUALITY,           1679)                                 \
    X(UI_DLG_SAVE_AUDIO_TITLE,      1680)                                 \
    X(UI_DLG_FILTER_AUDIO,          1681)                                 \
    X(UI_DLG_FILTER_ALL,            1682)                                 \
    X(UI_DLG_FILTER_SESSION,        1683)                                 \
    X(UI_DLG_OPEN_SESSION_TITLE,    1684)                                 \
    X(UI_DLG_SAVE_SESSION_TITLE,    1685)                                 \
    X(UI_DLG_REMOVE_TITLE,          1686)                                 \
    X(UI_DLG_REMOVE_CONFIRM,        1687)                                 \
    X(UI_DLG_SYSTEM_TITLE,          1688)                                 \
    X(UI_DLG_SYSTEM_BODY,           1689)                                 \
    X(UI_DLG_SYSTEM_TREE,           1690)                                 \
    X(UI_DLG_SYSTEM_TREE_NONE,      1691)                                 \
    X(UI_DLG_SYSTEM_CONSENT,        1692)                                 \
    X(UI_DLG_SYSTEM_TARGET,         1693)                                 \
    X(UI_DLG_RESOLVE_TITLE,         1694)                                 \
    X(UI_DLG_RESOLVE_LIST,          1695)                                 \
    X(UI_DLG_RESOLVE_SUMMARY,       1696)                                 \
    X(UI_DLG_RESOLVE_OK,            1697)                                 \
    X(UI_DLG_RESOLVE_CANDIDATE,     1698)                                 \
    X(UI_DLG_RESOLVE_LOAD,          1699)                                 \
    X(UI_DLG_KEYS_TITLE,            1700)                                 \
    X(UI_DLG_KEYS_LIST,             1701)                                 \
    X(UI_DLG_KEYS_ROW,              1702)                                 \
    X(UI_DLG_KEYS_COL_OP,           1703)                                 \
    X(UI_DLG_KEYS_COL_KEY,          1704)                                 \
    X(UI_DLG_ABOUT_TITLE,           1705)                                 \
    X(UI_DLG_ABOUT_BODY,            1706)                                 \
    X(UI_DLG_NAME_NEEDED,           1707)                                 \
    X(UI_DLG_PATH_NEEDED,           1708)                                 \
    X(UI_DLG_PICK_NEEDED,           1709)                                 \
    X(UI_DLG_NO_BUSES,              1710)                                 \
    X(UI_DLG_SOURCE_ADDED,          1711)                                 \
    X(UI_DLG_BUS_ADDED,             1712)                                 \
    X(UI_DLG_BUS_RENAMED,           1713)                                 \
    X(UI_DLG_OUTPUT_ADDED,          1714)                                 \
    X(UI_DLG_ADD_FAILED,            1715)                                 \
    X(UI_DLG_SESSION_LOADED,        1716)                                 \
    X(UI_DLG_SESSION_SAVED,         1717)                                 \
    X(UI_DLG_SESSION_FAILED,        1718)                                 \
    X(UI_DLG_SESSION_SAVE_FAILED,   1719)                                 \
    X(UI_DLG_KEY_COMBO,             1720)                                 \
    X(UI_DLG_KEY_CTRL,              1721)                                 \
    X(UI_DLG_KEY_SHIFT,             1722)                                 \
    X(UI_DLG_KEY_ALT,               1723)                                 \
    X(UI_DLG_OUTPUT_REMOVED,        1724)                                 \
    X(UI_DLG_PICK_OUTPUT,           1725)                                 \
    X(UI_DLG_OUTPUT_ROW,            1726)                                      \
    X(UI_DLG_RESOLVE_CONSENT,     1727)                                    \
    X(UI_DLG_OUT_PATH_TOKENS,     1728)                                     \
    X(UI_DLG_NO_OUTPUTS,          1729)                                     \
    X(UI_DLG_SESSION_CANCELLED,   1730)                                     \
    X(UI_DLG_DISCARD_TITLE,       1731)                                     \
    X(UI_DLG_DISCARD_BODY,        1732)                                     \
    X(UI_DLG_DISCARD_OK,          1733)                                     \
    X(UI_DLG_FORMAT_NEEDED,       1734)                                     \
    X(UI_DLG_CREATE_FAILED,       1735)


/* ---- the notification area.
 *
 * A recorder runs for hours minimised, so the tray is the app's primary
 * surface during a session rather than a convenience. Two things here are
 * accessibility features and not decoration:
 *
 *   UI_TRAY_TIP_* is a LIVE STATUS READOUT. Windows has a keyboard path to the
 *   notification area (Windows+B, then the arrow keys), so a tooltip that says
 *   "recording, 12:34" lets the author check on a session from anywhere
 *   without opening a window or interrupting what he is doing.
 *
 *   UI_TRAY_INFO_* are balloon notifications, and they are the channel for
 *   anything that happens while the window is NOT in front -- a source died,
 *   an encoder stopped. Screen readers announce those reliably, where a live
 *   region on a background window is not announced at all. ---- */
#define APR_STR_LIST_UI_TRAY(X)                                         \
    X(UI_TRAY_ICON_NAME,            1750)                                 \
    X(UI_TRAY_TIP_IDLE,             1751)                                 \
    X(UI_TRAY_TIP_RECORDING,        1752)                                 \
    X(UI_TRAY_TIP_FINISHING,        1753)                                 \
    X(UI_TRAY_MENU_SHOW,            1754)                                 \
    X(UI_TRAY_MENU_START,           1755)                                 \
    X(UI_TRAY_MENU_STOP,            1756)                                 \
    X(UI_TRAY_MENU_OPEN,            1757)                                 \
    X(UI_TRAY_MENU_QUIT,            1758)                                 \
    X(UI_TRAY_INFO_TITLE,           1759)                                 \
    X(UI_TRAY_INFO_STARTED,         1760)                                 \
    X(UI_TRAY_INFO_STOPPED,         1761)                                 \
    X(UI_TRAY_INFO_SOURCE_DIED,     1762)                                 \
    X(UI_TRAY_INFO_SOURCE_MUTED,    1763)                                 \
    X(UI_TRAY_INFO_ACTION_FAILED,   1764)                                 \
    X(UI_TRAY_INFO_MINIMIZED,       1765)                                 \
    X(UI_TRAY_INFO_ARM_FAILED,      1766)                                 \
    X(UI_TRAY_INFO_OUTPUT_RENAMED,  1767)                                 \
    X(UI_TRAY_INFO_SOURCE_RECOVERED, 1768)                                \
    X(UI_TRAY_INFO_EXCLUSION_HELD,  1769)

/* ---- WHY SOMETHING FAILED, in the user's language.
 *
 * These are the `%2!s!` of "Could not open %1!s! for writing: %2!s!" and of
 * every other frame that reports a failure. They used to be English prose in
 * a hand-written table in src/platform/err.c, handed straight into a
 * translated frame -- so the moment Arabic ships, every failure sentence
 * would have been half Arabic and half English (BUGS.md M11). The author is
 * blind; this is the sentence he hears when something has gone wrong.
 *
 * A REASON IS AN INSERT, NOT A SUFFIX GLUED IN C, and that is the same
 * licence APR_STR_LIST_UI_TREE's state phrases take: each entry is a complete
 * clause occupying one positional insert, so a translator may move it
 * anywhere in the frame sentence. What it must never be is a fragment the
 * code concatenates.
 *
 * ONE ENTRY PER HAND-TABLED WASAPI CODE. Windows ships no message resource
 * for facility 0x889, so these are the sentences apprecorder writes itself --
 * which is exactly why they need to be here rather than in a .c file.
 * Ordinary HRESULTs and Win32 codes are NOT tabled: FormatMessageW already
 * describes those in the user's own language, and errmsg.h asks it for the
 * language apprecorder is displaying in. src/platform/err.c owns the mapping
 * from code to id; test_strings.c fails if a tabled code has no id, and
 * test_err.c fails if the English text here and the table's own English text
 * ever drift apart. ---- */
#define APR_STR_LIST_ERR_HR(X)                                          \
    X(ERR_HR_E_NOT_INITIALIZED,           1800)                           \
    X(ERR_HR_E_ALREADY_INITIALIZED,       1801)                           \
    X(ERR_HR_E_WRONG_ENDPOINT_TYPE,       1802)                           \
    X(ERR_HR_E_DEVICE_INVALIDATED,        1803)                           \
    X(ERR_HR_E_NOT_STOPPED,               1804)                           \
    X(ERR_HR_E_BUFFER_TOO_LARGE,          1805)                           \
    X(ERR_HR_E_OUT_OF_ORDER,              1806)                           \
    X(ERR_HR_E_UNSUPPORTED_FORMAT,        1807)                           \
    X(ERR_HR_E_INVALID_SIZE,              1808)                           \
    X(ERR_HR_E_DEVICE_IN_USE,             1809)                           \
    X(ERR_HR_E_BUFFER_OPERATION_PENDING,  1810)                           \
    X(ERR_HR_E_THREAD_NOT_REGISTERED,     1811)                           \
    X(ERR_HR_E_EXCLUSIVE_MODE_NOT_ALLOWED,1812)                           \
    X(ERR_HR_E_ENDPOINT_CREATE_FAILED,    1813)                           \
    X(ERR_HR_E_SERVICE_NOT_RUNNING,       1814)                           \
    X(ERR_HR_E_EVENTHANDLE_NOT_EXPECTED,  1815)                           \
    X(ERR_HR_E_EXCLUSIVE_MODE_ONLY,       1816)                           \
    X(ERR_HR_E_BUFDURATION_PERIOD_NOT_EQUAL, 1817)                        \
    X(ERR_HR_E_EVENTHANDLE_NOT_SET,       1818)                           \
    X(ERR_HR_E_INCORRECT_BUFFER_SIZE,     1819)                           \
    X(ERR_HR_E_BUFFER_SIZE_ERROR,         1820)                           \
    X(ERR_HR_E_CPUUSAGE_EXCEEDED,         1821)                           \
    X(ERR_HR_E_BUFFER_ERROR,              1822)                           \
    X(ERR_HR_E_BUFFER_SIZE_NOT_ALIGNED,   1823)                           \
    X(ERR_HR_E_INVALID_DEVICE_PERIOD,     1824)                           \
    X(ERR_HR_E_INVALID_STREAM_FLAG,       1825)                           \
    X(ERR_HR_E_ENDPOINT_OFFLOAD_NOT_CAPABLE, 1826)                        \
    X(ERR_HR_E_OUT_OF_OFFLOAD_RESOURCES,  1827)                           \
    X(ERR_HR_E_OFFLOAD_MODE_ONLY,         1828)                           \
    X(ERR_HR_E_NONOFFLOAD_MODE_ONLY,      1829)                           \
    X(ERR_HR_E_RESOURCES_INVALIDATED,     1830)                           \
    X(ERR_HR_E_RAW_MODE_UNSUPPORTED,      1831)                           \
    X(ERR_HR_E_ENGINE_PERIODICITY_LOCKED, 1832)                           \
    X(ERR_HR_E_ENGINE_FORMAT_LOCKED,      1833)                           \
    X(ERR_HR_E_HEADTRACKING_ENABLED,      1834)                           \
    X(ERR_HR_E_HEADTRACKING_UNSUPPORTED,  1835)                           \
    X(ERR_HR_E_EFFECT_NOT_AVAILABLE,      1836)                           \
    X(ERR_HR_E_EFFECT_STATE_READ_ONLY,    1837)                           \
    X(ERR_HR_S_BUFFER_EMPTY,              1838)                           \
    X(ERR_HR_S_THREAD_ALREADY_REGISTERED, 1839)                           \
    X(ERR_HR_S_POSITION_STALLED,          1840)

/* ---- the same job for errors that carry no WASAPI code.
 *
 * The first block is one sentence per AprErrKind, and it is the floor: every
 * error that reaches a user resolves to SOMETHING here, so no failure can
 * fall through to an English literal. ERR_REASON_HRESULT / _WIN32 / _ERRNO
 * are the last resort for a numeric code Windows would not describe in the
 * display language -- they keep the code, as an identifier, rather than
 * swallowing it.
 *
 * The second block is for refusals where the KIND IS TOO COARSE TO BE USEFUL.
 * "That is not possible in the state this session is in" is true of a full
 * bus and of six other things; "that bus already holds as many sources as it
 * can mix" is the one the user can act on. A raise site names one of these
 * with APR_ERR_SAY, which costs one integer store and is therefore still safe
 * on a capture pump. Most raise sites should NOT name one: their text is
 * diagnostic, it goes to the log, and a user never sees it. ---- */
#define APR_STR_LIST_ERR_REASON(X)                                      \
    X(ERR_REASON_UNKNOWN,           1850)                                 \
    X(ERR_REASON_HRESULT,           1851)                                 \
    X(ERR_REASON_WIN32,             1852)                                 \
    X(ERR_REASON_ERRNO,             1853)                                 \
    X(ERR_REASON_INVALID_ARG,       1854)                                 \
    X(ERR_REASON_NO_MEMORY,         1855)                                 \
    X(ERR_REASON_STATE,             1856)                                 \
    X(ERR_REASON_NOT_FOUND,         1857)                                 \
    X(ERR_REASON_UNSUPPORTED,       1858)                                 \
    X(ERR_REASON_TIMEOUT,           1859)                                 \
    X(ERR_REASON_OVERRUN,           1860)                                 \
    X(ERR_REASON_IO,                1861)                                 \
    X(ERR_REASON_BUSY,              1862)                                 \
    X(ERR_REASON_TOO_MANY_SOURCES,  1870)                                 \
    X(ERR_REASON_TOO_MANY_BUSES,    1871)                                 \
    X(ERR_REASON_BUS_FULL_SOURCES,  1872)                                 \
    X(ERR_REASON_BUS_FULL_OUTPUTS,  1873)                                 \
    X(ERR_REASON_RATE_MISMATCH,     1874)                                 \
    X(ERR_REASON_ALREADY_ON_BUS,    1875)                                 \
    X(ERR_REASON_NOT_ON_BUS,        1876)                                 \
    X(ERR_REASON_NAME_NEEDED,       1877)

/* Every plain string, in declaration order. This is what C, the generated
 * enum and tests/test_strings.c walk; the groups above exist only so that
 * res/strings.rc can emit them in rc.exe-sized pieces. */
#define APR_STR_LIST(X)                                                        \
    APR_STR_LIST_CORE(X)                                                       \
    APR_STR_LIST_CLI(X)                                                        \
    APR_STR_LIST_CLI_ERR(X)                                                    \
    APR_STR_LIST_CLI_MSG(X)                                                   \
    APR_STR_LIST_UI(X)                                                         \
    APR_STR_LIST_UI_NODE(X)                                                    \
    APR_STR_LIST_UI_KEYS(X)                                    \
    APR_STR_LIST_UI_TREE(X)                                                    \
    APR_STR_LIST_SESSION(X)                                                    \
    APR_STR_LIST_UI_REC(X)                                                     \
    APR_STR_LIST_UI_DLG(X)                                                     \
    APR_STR_LIST_UI_TRAY(X)                                                    \
    APR_STR_LIST_ERR_HR(X)                                                     \
    APR_STR_LIST_ERR_REASON(X)


/* X(NAME, base) -- a plural string. Referenced in C as APR_S_NAME, which is
 * the BASE id; the six CLDR categories live at base+APR_PLURAL_ZERO through
 * base+APR_PLURAL_OTHER. Never look a plural id up directly with apr_str();
 * use apr_str_plural(). */
#define APR_STR_PLURAL_LIST(X)                                                 \
    X(N_SOURCES,                 1200)                                         \
    X(N_BUSES,                   1208)                                         \
    X(N_FRAMES_LOST,             1216)                                         \
    X(N_OTHER_BUSES,             1224)                                         \
    X(N_OUTPUTS,                 1232)

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
