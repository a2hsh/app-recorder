/*
 * ui_theme.h -- the token set. Colours, metrics and fonts, in one place, so
 * that canvas.c, node_window.c and tree_panel.c consume tokens instead of
 * inventing constants.
 *
 * ===========================================================================
 * WHY TOKENS AND NOT CONSTANTS
 *
 *   Three things have to change together at runtime and each of them touches
 *   every drawn pixel: DPI (per monitor, mid-session), dark mode (the user
 *   flips it in Settings while we are running) and high contrast (an
 *   accessibility mode that overrides both). A literal RGB or a literal 8 in a
 *   paint handler is unreachable from all three. A token is one struct field
 *   that all three update.
 *
 *   So: NO COLOUR AND NO SPACING LITERAL BELONGS IN A PAINT HANDLER. If a
 *   token is missing, add it here rather than hardcoding it there -- that is
 *   the same rule AGENTS.md section 3 applies to ring buffers, applied to
 *   pixels.
 *
 * ===========================================================================
 * METRICS ARE DEVICE PIXELS, ALREADY SCALED
 *
 *   Every field of AprThemeMetrics is in DEVICE pixels for this theme's
 *   current DPI, not logical pixels. The scaling has already happened. Call
 *   sites therefore never multiply, which means a call site cannot forget to.
 *
 *   The price is that the numbers are only valid while `t->dpi` is what it is.
 *   Do not cache them across a WM_DPICHANGED; re-read them from the theme,
 *   which apr_theme_set_dpi has by then recomputed. apr_theme_px() is there
 *   for the rare measurement that is not a token.
 *
 * ===========================================================================
 * HIGH CONTRAST IS NOT A COLOUR SCHEME
 *
 *   When the system is in high contrast, the palette below is REPLACED by
 *   GetSysColor values -- not tinted, not adjusted. A user in high contrast
 *   has told the OS which exact colours they can see, and any "improvement" we
 *   make to them is us overriding an accessibility setting with a taste
 *   preference. apr_theme_high_contrast() reports it so a painter can also
 *   drop decorative fills that only work in a palette we no longer control.
 *
 * ===========================================================================
 * FONTS AND ARABIC
 *
 *   The design says Segoe UI Variable "covers Arabic". MEASURED ON WINDOWS 11
 *   BUILD 26200: IT DOES NOT. Five of the Arabic code points in the catalog
 *   have no glyph in Segoe UI Variable Text; the family ships Latin, Greek and
 *   Cyrillic. So the face is chosen per script and the choice is CHECKED at
 *   theme creation rather than assumed from a version number: Segoe UI Variable
 *   while it has the glyphs, Segoe UI (which does carry Arabic) when it does
 *   not. See theme.c and tests/test_ui_theme.c, which asks the font itself.
 *
 *   AND: never size a control to the extent of its English string. Arabic at
 *   the same point size is taller -- ascenders, descenders and diacritics all
 *   go further from the baseline. apr_theme_line_height() is derived from the
 *   FONT's metrics, not from any string, which is what makes it safe; there is
 *   deliberately no "height that fits this text" call, because someone would
 *   feed it English.
 *
 * THREAD SAFETY: an AprTheme is owned by one thread, normally the UI thread.
 * GDI objects inside it are not shared.
 */
#ifndef APPRECORDER_UI_THEME_H
#define APPRECORDER_UI_THEME_H

#include <windows.h>

#include "err.h"
#include "ui_dpi.h"

/* ---------------------------------------------------------------------------
 * Fonts
 * ------------------------------------------------------------------------- */

typedef enum AprFontRole {
    APR_FONT_BODY = 0,      /* everything, unless there is a reason        */
    APR_FONT_BODY_STRONG,   /* the same size, semibold -- node titles      */
    APR_FONT_SMALL,         /* secondary detail; never the only copy of a
                             * fact, because it is the first thing a
                             * low-vision user loses                       */
    APR_FONT_HEADING,       /* section headings                            */
    APR_FONT_TITLE,         /* one per window at most                      */
    APR_FONT_COUNT
} AprFontRole;

/* ---------------------------------------------------------------------------
 * Colours
 *
 * Semantic names, never "grey2". A name that says what a colour is FOR
 * survives a dark-mode flip; a name that says what it looks like does not.
 * ------------------------------------------------------------------------- */

typedef struct AprThemePalette {
    COLORREF window_bg;      /* the frame behind everything                */
    COLORREF surface;        /* a pane or a card sitting on window_bg      */
    COLORREF surface_alt;    /* alternating rows, sunken wells             */
    COLORREF surface_hot;    /* pointer is over it                         */
    COLORREF surface_sel;    /* selected, focused                          */
    COLORREF surface_sel_bg; /* selected, unfocused -- MUST differ from
                              * surface_sel or focus becomes invisible     */

    COLORREF text;
    COLORREF text_dim;       /* secondary. Still >= 4.5:1 on its surface.  */
    COLORREF text_sel;
    COLORREF text_on_accent;
    COLORREF text_disabled;

    COLORREF border;
    COLORREF border_strong;
    COLORREF accent;

    /* THE FOCUS RING IS TWO COLOURS, AND THAT IS NOT DECORATION.
     *
     * A single-colour ring cannot meet 3:1 against everything it may land on:
     * a dark ring disappears on a dark accent fill, a light one disappears on
     * a light surface, and the focus indicator is exactly the thing that must
     * never disappear. So the ring is drawn twice, `focus` outside and
     * `focus_inner` inside, chosen to contrast with EACH OTHER -- which means
     * whatever is underneath, one of the two is visible against it.
     *
     * This is what Windows 11 itself does, and tests/test_ui_theme.c asserts
     * the property rather than the colours: for every surface in this palette,
     * at least one of the two ring colours reaches 3:1. */
    COLORREF focus;
    COLORREF focus_inner;

    COLORREF ok;
    COLORREF warn;
    COLORREF danger;

    COLORREF node_source;    /* the three node kinds are colour-coded, and */
    COLORREF node_bus;       /* colour is never the ONLY carrier of that   */
    COLORREF node_action;    /* fact -- the UIA name says the kind too.    */
    COLORREF edge;
    COLORREF edge_active;
} AprThemePalette;

/* ---------------------------------------------------------------------------
 * Metrics -- DEVICE pixels at the theme's current DPI
 * ------------------------------------------------------------------------- */

typedef struct AprThemeMetrics {
    /* Spacing scale. Generous on purpose: the design asks for it, and it is
     * also what makes a magnified view legible and a translated label fit. */
    int space_xs;    /*  2 logical */
    int space_sm;    /*  6         */
    int space_md;    /* 12         */
    int space_lg;    /* 20         */
    int space_xl;    /* 32         */

    int border;          /* hairline; never rounds to 0 */
    int border_strong;
    int focus_ring;      /* focus indicator thickness; never rounds to 0 */
    int focus_gap;       /* gap between control edge and ring            */
    int corner;          /* corner radius                               */

    int row_h;           /* minimum height of an interactive row        */
    int hit_min;         /* minimum hit target in either axis           */
    int caret_w;

    int pane_min_w;      /* a pane narrower than this is unusable       */
    int tree_w;          /* default width of the structure panel        */
    int splitter_w;
    int status_h;

    int node_w;          /* default node box                            */
    int node_min_h;
    int node_gap_x;      /* between columns of the graph                */
    int node_gap_y;      /* between nodes in a column                   */
    int node_pad;        /* inside a node                               */
    int port_r;          /* edge attachment point radius                */
    int edge_w;          /* edge stroke                                 */
    int edge_arrow;      /* arrowhead length                            */
} AprThemeMetrics;

/* ---------------------------------------------------------------------------
 * The theme
 * ------------------------------------------------------------------------- */

typedef struct AprTheme AprTheme;

/* Create a theme for `dpi`. Reads the system's dark-mode preference and high
 * contrast state, picks fonts, and scales every metric.
 *
 * `dpi` of 0 means the system DPI, which is only ever right before the first
 * window exists. */
AprErr apr_theme_create(UINT dpi, AprTheme **out);
void   apr_theme_destroy(AprTheme *t);

/* Re-scale for a new DPI: recreates every font, recomputes every metric.
 * Colours do not depend on DPI and are left alone. Cheap enough to call from
 * WM_DPICHANGED, which is the only place it should be called. */
AprErr apr_theme_set_dpi(AprTheme *t, UINT dpi);

/* Re-read the system: dark mode, high contrast, and the UI direction from
 * apr_str_is_rtl(). Call from WM_SETTINGCHANGE and WM_THEMECHANGED, and after
 * apr_str_set_language().
 *
 * Direction is included here on purpose: apr_str_is_rtl() is the ONLY source
 * of direction in the product (strings.h), and a theme that cached it at
 * creation would become a second source the moment the language changed. */
AprErr apr_theme_refresh(AprTheme *t);

/* Force dark on or off regardless of the system preference; -1 restores
 * "follow the system". Has no effect while high contrast is active, which is
 * deliberate -- see the header note. */
AprErr apr_theme_set_dark(AprTheme *t, int dark_or_minus_one);

/* ---------------------------------------------------------------------------
 * Reading the tokens
 * ------------------------------------------------------------------------- */

const AprThemePalette *apr_theme_palette(const AprTheme *t);
const AprThemeMetrics *apr_theme_metrics(const AprTheme *t);

UINT apr_theme_dpi(const AprTheme *t);
int  apr_theme_is_dark(const AprTheme *t);
int  apr_theme_high_contrast(const AprTheme *t);

/* Nonzero when the interface runs right to left. Forwarded from
 * apr_str_is_rtl() and refreshed by apr_theme_refresh(); it is a convenience,
 * NOT a second source of truth. */
int apr_theme_is_rtl(const AprTheme *t);

HFONT apr_theme_font(const AprTheme *t, AprFontRole role);

/* The face name actually selected for `role`, for diagnostics and tests. */
const wchar_t *apr_theme_font_face(const AprTheme *t, AprFontRole role);

/* A logical measurement in device pixels at this theme's DPI. For the
 * occasional number that is genuinely not a token. If you find yourself
 * calling this with the same constant twice, it was a token. */
int apr_theme_px(const AprTheme *t, int logical);

/* Full line height of `role`, from the FONT's metrics: ascent + descent +
 * external leading. Independent of any string, which is what makes it safe to
 * size a control with -- see the Arabic note in the header comment. */
int apr_theme_line_height(const AprTheme *t, AprFontRole role);

/* Height of a box holding `lines` lines of `role` plus `pad` device pixels
 * above and below, never less than metrics->row_h.
 *
 * There is deliberately NO variant that takes a string. Sizing a control to
 * the text currently in it is exactly how a layout ends up fitting English and
 * clipping Arabic. */
int apr_theme_block_height(const AprTheme *t, AprFontRole role, int lines, int pad);

/* Width of `text` in `role`, for painting decisions only -- eliding, centring,
 * deciding whether a tooltip is needed. Never for sizing a control. */
int apr_theme_text_width(const AprTheme *t, AprFontRole role, const wchar_t *text);

/* Nonzero when every character of `text` has a glyph in `role`'s font. Used at
 * theme creation to pick a face that can actually render the running language,
 * and exposed so a test can assert it for the catalog's own Arabic. */
int apr_theme_font_covers(const AprTheme *t, AprFontRole role, const wchar_t *text);

/* ---------------------------------------------------------------------------
 * Cached GDI objects
 *
 * Painters need brushes and pens constantly and must not create them per
 * WM_PAINT. The theme owns them and frees them with itself; a returned handle
 * is valid until the theme is destroyed or its palette changes, so do not
 * DeleteObject one and do not hold one across apr_theme_refresh().
 * ------------------------------------------------------------------------- */

HBRUSH apr_theme_brush(AprTheme *t, COLORREF color);
HPEN   apr_theme_pen(AprTheme *t, COLORREF color, int width);

/* ---------------------------------------------------------------------------
 * Focus
 *
 * One function, so that every focus indicator in the product looks and behaves
 * identically. A focus ring drawn by hand in one control and by DrawFocusRect
 * in another is how a keyboard user loses track of where they are.
 * ------------------------------------------------------------------------- */

void apr_theme_draw_focus(AprTheme *t, HDC dc, const RECT *r);

#endif /* APPRECORDER_UI_THEME_H */
