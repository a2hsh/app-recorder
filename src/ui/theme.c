/*
 * theme.c -- the token set.
 *
 * ===========================================================================
 * FINDING: "Segoe UI Variable covers Arabic" (design 6.2) IS NOT TRUE
 *
 *   Measured on Windows 11 build 26200 by asking the font itself, through
 *   GetGlyphIndicesW with GGI_MARK_NONEXISTING_GLYPHS, for the Arabic text
 *   that is already in res/strings.rc: the Segoe UI Variable faces report
 *   0xFFFF -- no glyph -- for every Arabic code point. The family ships Latin,
 *   Greek and Cyrillic; Arabic is not in it.
 *
 *   Why this matters more than it looks: GDI's font linking will quietly
 *   substitute another face for the missing characters, so the text still
 *   APPEARS. What changes is the metrics -- the substituted face has different
 *   ascent, descent and line gap -- so a layout measured against Segoe UI
 *   Variable's metrics clips the Arabic that is actually drawn. That is
 *   precisely the failure mode AGENTS.md rule 6 names ("never size a control
 *   to fit its English string"), arriving through the font rather than through
 *   the string.
 *
 *   So the face is chosen per script and the choice is VERIFIED, not assumed:
 *   Segoe UI Variable while the UI is Latin, Segoe UI (which does carry
 *   Arabic) while it is not, and if the preferred face turns out not to cover
 *   the running language we fall back and say so in the log. The check asks the
 *   font for glyph indices of the running language's own name, which the OS
 *   supplies in that language's own script -- see script_sample() below.
 *
 * ===========================================================================
 * WHERE THE COLOURS COME FROM
 *
 *   Hand-picked, then checked for contrast rather than eyeballed. Every
 *   text-on-surface pair below meets WCAG AA (4.5:1) at normal weight and
 *   size, including text_dim, which is the one everybody gets wrong. That is
 *   asserted in tests/test_ui_theme.c using the WCAG relative-luminance
 *   formula, so a future "let's soften the secondary text" change fails the
 *   build instead of shipping.
 *
 *   In high contrast none of it applies: GetSysColor wins outright. See the
 *   header.
 */
#include "ui_theme.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "strings.h"

/* --------------------------------------------------------------------------
 * Logical (96 DPI) metric constants. The ONLY place these numbers appear.
 * ----------------------------------------------------------------------- */

#define L_SPACE_XS       2
#define L_SPACE_SM       6
#define L_SPACE_MD      12
#define L_SPACE_LG      20
#define L_SPACE_XL      32

#define L_BORDER         1
#define L_BORDER_STRONG  2
#define L_FOCUS_RING     2
#define L_FOCUS_GAP      2
#define L_CORNER         6

#define L_ROW_H         28
#define L_HIT_MIN       24
#define L_CARET_W        1

#define L_PANE_MIN_W   160
#define L_TREE_W       280
#define L_SPLITTER_W     6
#define L_STATUS_H      26

#define L_NODE_W       220
#define L_NODE_MIN_H    72
#define L_NODE_GAP_X   120
#define L_NODE_GAP_Y    24
#define L_NODE_PAD      12
#define L_PORT_R         5
#define L_EDGE_W         2
#define L_EDGE_ARROW    10

/* Point sizes. 10pt body rather than Windows' 9pt default: the extra point
 * costs nothing and is the cheapest legibility win available. */
#define PT_BODY      10
#define PT_SMALL      9
#define PT_HEADING   13
#define PT_TITLE     18

#define APR_THEME_BRUSH_CACHE 24
#define APR_THEME_PEN_CACHE   24

typedef struct BrushSlot { COLORREF color; HBRUSH b; } BrushSlot;
typedef struct PenSlot   { COLORREF color; int width; HPEN p; } PenSlot;

struct AprTheme {
    UINT dpi;
    int  dark;              /* effective */
    int  dark_override;     /* -1 follow system, else forced */
    int  high_contrast;
    int  rtl;

    AprThemePalette c;
    AprThemeMetrics m;

    HFONT   font[APR_FONT_COUNT];
    wchar_t face[APR_FONT_COUNT][LF_FACESIZE];
    int     line_h[APR_FONT_COUNT];

    BrushSlot brush[APR_THEME_BRUSH_CACHE];
    int       brush_n;
    PenSlot   pen[APR_THEME_PEN_CACHE];
    int       pen_n;
};

/* --------------------------------------------------------------------------
 * Font selection
 * ----------------------------------------------------------------------- */

static int face_exists(const wchar_t *face)
{
    LOGFONTW lf;
    HDC dc;
    HFONT f, old;
    wchar_t got[LF_FACESIZE];
    int ok = 0;

    memset(&lf, 0, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfHeight = -12;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);

    dc = GetDC(NULL);
    if (!dc) return 0;

    /* CreateFontIndirect never fails: it substitutes. So ask the DC what it
     * actually selected and compare. This is the only reliable presence test
     * that does not depend on enumeration quirks around variable-font
     * families, several of which do not enumerate under their marketing name. */
    f = CreateFontIndirectW(&lf);
    if (f) {
        old = (HFONT)SelectObject(dc, f);
        got[0] = 0;
        if (GetTextFaceW(dc, LF_FACESIZE, got) > 0) {
            ok = (_wcsicmp(got, face) == 0);
        }
        SelectObject(dc, old);
        DeleteObject(f);
    }
    ReleaseDC(NULL, dc);
    return ok;
}

static int font_covers(HFONT font, const wchar_t *text)
{
    HDC dc;
    HFONT old;
    WORD idx[128];
    size_t n, i;
    DWORD got;
    int ok = 1;

    if (!font || !text || !*text) return 1;

    n = wcslen(text);
    if (n > 128) n = 128;

    dc = GetDC(NULL);
    if (!dc) return 1;   /* cannot tell; do not fail closed on a missing DC */

    old = (HFONT)SelectObject(dc, font);
    got = GetGlyphIndicesW(dc, text, (int)n, idx, GGI_MARK_NONEXISTING_GLYPHS);
    if (got == GDI_ERROR) {
        ok = 1;
    } else {
        for (i = 0; i < n; ++i) {
            /* Space and the bidi/format controls legitimately have no glyph in
             * some faces and are not evidence of missing coverage. */
            wchar_t ch = text[i];
            if (ch == L' ' || ch == L'\t' || ch == 0x200E || ch == 0x200F ||
                (ch >= 0x202A && ch <= 0x202E)) {
                continue;
            }
            if (idx[i] == 0xFFFF) { ok = 0; break; }
        }
    }
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    return ok;
}

/* A sample of the script the UI is currently running in.
 *
 * Taken from the OS, not from a literal in this file and not from the catalog:
 * GetLocaleInfoW(LOCALE_SNATIVELANGUAGENAME) returns a language's name written
 * in its OWN script -- Arabic letters for ar-SA -- which is exactly the
 * question a coverage probe needs to ask, and it is right even for a language
 * whose catalog entries are not translated yet.
 *
 * The catalog could not answer it: an untranslated Arabic string falls back to
 * English, so probing the catalog while running in Arabic would test Latin
 * coverage and always pass. A wide literal here would be worse still -- it
 * would put user-invisible Arabic in a .c file (AGENTS.md rule 6) and would be
 * checked against a font while the real, resource-loaded strings were not.
 */
static const wchar_t *script_sample(void)
{
    static wchar_t sample[96];
    static LANGID cached = 0;
    LANGID lang = apr_str_language();

    if (lang != cached || sample[0] == 0) {
        sample[0] = 0;
        if (GetLocaleInfoW(MAKELCID(lang, SORT_DEFAULT), LOCALE_SNATIVELANGUAGENAME,
                           sample, (int)(sizeof sample / sizeof sample[0])) <= 0) {
            /* No locale data: fall back to catalog text, which is at worst the
             * primary language and therefore a no-op check rather than a wrong
             * one. */
            lstrcpynW(sample, apr_str(APR_S_APP_TAGLINE),
                      (int)(sizeof sample / sizeof sample[0]));
        }
        cached = lang;
    }
    return sample;
}

static void pick_face(int prefer_variable, const wchar_t *variable_face,
                      int weight, int point, UINT dpi,
                      wchar_t out_face[LF_FACESIZE])
{
    /* Preference order. Segoe UI Variable first when the running script is one
     * it covers, Segoe UI otherwise; then the last-resort faces, which are
     * present on every Windows since XP. */
    static const wchar_t *const fallback[] = {
        L"Segoe UI", L"Tahoma", L"Microsoft Sans Serif", L"Arial"
    };
    size_t i;
    HFONT probe;
    LOGFONTW lf;
    const wchar_t *sample = script_sample();

    if (prefer_variable && face_exists(variable_face)) {
        memset(&lf, 0, sizeof lf);
        lf.lfHeight = apr_dpi_font_height(point, dpi);
        lf.lfWeight = weight;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lstrcpynW(lf.lfFaceName, variable_face, LF_FACESIZE);
        probe = CreateFontIndirectW(&lf);
        if (probe) {
            int covers = font_covers(probe, sample);
            DeleteObject(probe);
            if (covers) {
                lstrcpynW(out_face, variable_face, LF_FACESIZE);
                return;
            }
            APR_INFO(L"theme: %s does not cover the running script; falling back",
                     variable_face);
        }
    }

    for (i = 0; i < sizeof fallback / sizeof fallback[0]; ++i) {
        if (face_exists(fallback[i])) {
            lstrcpynW(out_face, fallback[i], LF_FACESIZE);
            return;
        }
    }
    lstrcpynW(out_face, L"Segoe UI", LF_FACESIZE);
}

static void free_fonts(AprTheme *t)
{
    int i;
    for (i = 0; i < APR_FONT_COUNT; ++i) {
        if (t->font[i]) { DeleteObject(t->font[i]); t->font[i] = NULL; }
    }
}

static AprErr build_fonts(AprTheme *t)
{
    struct { AprFontRole role; int point; int weight; const wchar_t *variable; } spec[] = {
        { APR_FONT_BODY,        PT_BODY,    FW_NORMAL,   L"Segoe UI Variable Text"    },
        { APR_FONT_BODY_STRONG, PT_BODY,    FW_SEMIBOLD, L"Segoe UI Variable Text"    },
        { APR_FONT_SMALL,       PT_SMALL,   FW_NORMAL,   L"Segoe UI Variable Small"   },
        { APR_FONT_HEADING,     PT_HEADING, FW_SEMIBOLD, L"Segoe UI Variable Display" },
        { APR_FONT_TITLE,       PT_TITLE,   FW_SEMIBOLD, L"Segoe UI Variable Display" }
    };
    HDC dc;
    int i;

    free_fonts(t);

    for (i = 0; i < APR_FONT_COUNT; ++i) {
        LOGFONTW lf;

        pick_face(1, spec[i].variable, spec[i].weight, spec[i].point, t->dpi,
                  t->face[spec[i].role]);

        memset(&lf, 0, sizeof lf);
        lf.lfHeight = apr_dpi_font_height(spec[i].point, t->dpi);
        lf.lfWeight = spec[i].weight;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = OUT_TT_PRECIS;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
        lstrcpynW(lf.lfFaceName, t->face[spec[i].role], LF_FACESIZE);

        t->font[spec[i].role] = CreateFontIndirectW(&lf);
        if (!t->font[spec[i].role]) {
            return APR_ERR_LAST(L"CreateFontIndirectW failed for role %d", (int)spec[i].role);
        }
    }

    /* Line heights come from the font, never from a string. */
    dc = GetDC(NULL);
    if (dc) {
        for (i = 0; i < APR_FONT_COUNT; ++i) {
            TEXTMETRICW tm;
            HFONT old = (HFONT)SelectObject(dc, t->font[i]);
            if (GetTextMetricsW(dc, &tm)) {
                t->line_h[i] = (int)(tm.tmHeight + tm.tmExternalLeading);
            } else {
                t->line_h[i] = apr_dpi_scale(16, t->dpi);
            }
            SelectObject(dc, old);
        }
        ReleaseDC(NULL, dc);
    } else {
        for (i = 0; i < APR_FONT_COUNT; ++i) t->line_h[i] = apr_dpi_scale(16, t->dpi);
    }

    return apr_ok();
}

/* --------------------------------------------------------------------------
 * Metrics
 * ----------------------------------------------------------------------- */

static void build_metrics(AprTheme *t)
{
    UINT d = t->dpi;
    AprThemeMetrics *m = &t->m;

    m->space_xs = apr_dpi_scale(L_SPACE_XS, d);
    m->space_sm = apr_dpi_scale(L_SPACE_SM, d);
    m->space_md = apr_dpi_scale(L_SPACE_MD, d);
    m->space_lg = apr_dpi_scale(L_SPACE_LG, d);
    m->space_xl = apr_dpi_scale(L_SPACE_XL, d);

    /* _min, not _scale: a hairline that rounds to zero disappears, and a
     * border or focus ring that disappears at 125% is invisible exactly where
     * it is most needed. */
    m->border        = apr_dpi_scale_min(L_BORDER, d, 1);
    m->border_strong = apr_dpi_scale_min(L_BORDER_STRONG, d, 2);
    m->focus_ring    = apr_dpi_scale_min(L_FOCUS_RING, d, 2);
    m->focus_gap     = apr_dpi_scale_min(L_FOCUS_GAP, d, 1);
    m->corner        = apr_dpi_scale(L_CORNER, d);

    m->row_h   = apr_dpi_scale(L_ROW_H, d);
    m->hit_min = apr_dpi_scale(L_HIT_MIN, d);
    m->caret_w = apr_dpi_scale_min(L_CARET_W, d, 1);

    m->pane_min_w = apr_dpi_scale(L_PANE_MIN_W, d);
    m->tree_w     = apr_dpi_scale(L_TREE_W, d);
    m->splitter_w = apr_dpi_scale_min(L_SPLITTER_W, d, 4);
    m->status_h   = apr_dpi_scale(L_STATUS_H, d);

    m->node_w      = apr_dpi_scale(L_NODE_W, d);
    m->node_min_h  = apr_dpi_scale(L_NODE_MIN_H, d);
    m->node_gap_x  = apr_dpi_scale(L_NODE_GAP_X, d);
    m->node_gap_y  = apr_dpi_scale(L_NODE_GAP_Y, d);
    m->node_pad    = apr_dpi_scale(L_NODE_PAD, d);
    m->port_r      = apr_dpi_scale_min(L_PORT_R, d, 3);
    m->edge_w      = apr_dpi_scale_min(L_EDGE_W, d, 1);
    m->edge_arrow  = apr_dpi_scale(L_EDGE_ARROW, d);

    /* A row must hold a line of body text with breathing room whatever the
     * font turned out to be -- including a fallback face with taller metrics,
     * which is exactly what happens when the UI switches to Arabic. */
    {
        int need = t->line_h[APR_FONT_BODY] + 2 * m->space_sm;
        if (m->row_h < need) m->row_h = need;
        if (m->row_h < m->hit_min) m->row_h = m->hit_min;
        if (m->status_h < t->line_h[APR_FONT_BODY] + 2 * m->space_xs) {
            m->status_h = t->line_h[APR_FONT_BODY] + 2 * m->space_xs;
        }
        need = 2 * (t->line_h[APR_FONT_BODY] + m->space_xs) + 2 * m->node_pad;
        if (m->node_min_h < need) m->node_min_h = need;
    }
}

/* --------------------------------------------------------------------------
 * Palette
 * ----------------------------------------------------------------------- */

static void palette_light(AprThemePalette *c)
{
    c->window_bg      = RGB(0xF3, 0xF3, 0xF3);
    c->surface        = RGB(0xFF, 0xFF, 0xFF);
    c->surface_alt    = RGB(0xF7, 0xF7, 0xF9);
    c->surface_hot    = RGB(0xEC, 0xEC, 0xF2);
    c->surface_sel    = RGB(0xD6, 0xE4, 0xFA);
    c->surface_sel_bg = RGB(0xE6, 0xE6, 0xEA);

    c->text           = RGB(0x18, 0x18, 0x1B);
    c->text_dim       = RGB(0x55, 0x55, 0x5E);  /* 7.0:1 on surface */
    c->text_sel       = RGB(0x10, 0x1A, 0x2C);
    c->text_on_accent = RGB(0xFF, 0xFF, 0xFF);
    c->text_disabled  = RGB(0x8A, 0x8A, 0x92);

    c->border         = RGB(0xD8, 0xD8, 0xDE);
    c->border_strong  = RGB(0x82, 0x82, 0x8C);  /* 3.8:1 on surface */
    c->accent         = RGB(0x0F, 0x53, 0xA8);  /* 6.6:1 against white */
    c->focus          = RGB(0x0B, 0x0B, 0x10);  /* outer ring */
    c->focus_inner    = RGB(0xFF, 0xFF, 0xFF);  /* inner ring */

    c->ok             = RGB(0x0B, 0x66, 0x23);
    c->warn           = RGB(0x8A, 0x54, 0x00);
    c->danger         = RGB(0xA8, 0x14, 0x14);

    c->node_source    = RGB(0x1B, 0x5E, 0x20);
    c->node_bus       = RGB(0x0F, 0x53, 0xA8);
    c->node_action    = RGB(0x6A, 0x1B, 0x9A);
    c->edge           = RGB(0x77, 0x77, 0x82);
    c->edge_active    = RGB(0x0F, 0x53, 0xA8);
}

static void palette_dark(AprThemePalette *c)
{
    c->window_bg      = RGB(0x1B, 0x1B, 0x1F);
    c->surface        = RGB(0x25, 0x25, 0x2A);
    c->surface_alt    = RGB(0x2C, 0x2C, 0x32);
    c->surface_hot    = RGB(0x34, 0x34, 0x3C);
    c->surface_sel    = RGB(0x1E, 0x3A, 0x5F);
    c->surface_sel_bg = RGB(0x35, 0x35, 0x3D);

    c->text           = RGB(0xF2, 0xF2, 0xF4);
    c->text_dim       = RGB(0xB6, 0xB6, 0xC0);  /* 7.4:1 on surface */
    c->text_sel       = RGB(0xFF, 0xFF, 0xFF);
    c->text_on_accent = RGB(0x0B, 0x0B, 0x0E);
    c->text_disabled  = RGB(0x77, 0x77, 0x80);

    c->border         = RGB(0x3C, 0x3C, 0x44);
    c->border_strong  = RGB(0x7C, 0x7C, 0x88);  /* 3.7:1 on surface */
    c->accent         = RGB(0x7C, 0xB3, 0xFF);  /* 7.5:1 on surface */
    c->focus          = RGB(0xFF, 0xFF, 0xFF);  /* outer ring */
    c->focus_inner    = RGB(0x0B, 0x0B, 0x10);  /* inner ring */

    c->ok             = RGB(0x76, 0xD2, 0x8B);
    c->warn           = RGB(0xF0, 0xC0, 0x6A);
    c->danger         = RGB(0xFF, 0x8B, 0x8B);

    c->node_source    = RGB(0x86, 0xD6, 0x94);
    c->node_bus       = RGB(0x7C, 0xB3, 0xFF);
    c->node_action    = RGB(0xC9, 0xA2, 0xF5);
    c->edge           = RGB(0x86, 0x86, 0x92);
    c->edge_active    = RGB(0x7C, 0xB3, 0xFF);
}

static void palette_high_contrast(AprThemePalette *c)
{
    /* The user has told the OS which colours they can see. Use those, all of
     * them, and add nothing. */
    c->window_bg      = GetSysColor(COLOR_WINDOW);
    c->surface        = GetSysColor(COLOR_WINDOW);
    c->surface_alt    = GetSysColor(COLOR_WINDOW);
    c->surface_hot    = GetSysColor(COLOR_HIGHLIGHT);
    c->surface_sel    = GetSysColor(COLOR_HIGHLIGHT);
    c->surface_sel_bg = GetSysColor(COLOR_HIGHLIGHT);

    c->text           = GetSysColor(COLOR_WINDOWTEXT);
    c->text_dim       = GetSysColor(COLOR_WINDOWTEXT);   /* not dimmed: dimming
                                                          * is a contrast loss */
    c->text_sel       = GetSysColor(COLOR_HIGHLIGHTTEXT);
    c->text_on_accent = GetSysColor(COLOR_HIGHLIGHTTEXT);
    c->text_disabled  = GetSysColor(COLOR_GRAYTEXT);

    c->border         = GetSysColor(COLOR_WINDOWTEXT);
    c->border_strong  = GetSysColor(COLOR_WINDOWTEXT);
    c->accent         = GetSysColor(COLOR_HOTLIGHT);
    c->focus          = GetSysColor(COLOR_WINDOWTEXT);
    c->focus_inner    = GetSysColor(COLOR_WINDOW);

    c->ok             = GetSysColor(COLOR_WINDOWTEXT);
    c->warn           = GetSysColor(COLOR_WINDOWTEXT);
    c->danger         = GetSysColor(COLOR_WINDOWTEXT);

    c->node_source    = GetSysColor(COLOR_WINDOWTEXT);
    c->node_bus       = GetSysColor(COLOR_WINDOWTEXT);
    c->node_action    = GetSysColor(COLOR_WINDOWTEXT);
    c->edge           = GetSysColor(COLOR_WINDOWTEXT);
    c->edge_active    = GetSysColor(COLOR_HOTLIGHT);
}

static int system_high_contrast(void)
{
    HIGHCONTRASTW hc;
    memset(&hc, 0, sizeof hc);
    hc.cbSize = sizeof hc;
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0)) return 0;
    return (hc.dwFlags & HCF_HIGHCONTRASTON) ? 1 : 0;
}

/* The system dark-mode preference, read from the documented registry value
 * rather than from uxtheme's undocumented ShouldAppsUseDarkMode.
 *
 * darkmode.c owns the undocumented surface and may fail to resolve it on any
 * Windows build; the THEME must never depend on that, because a failed ordinal
 * lookup would then silently paint a light UI on a machine set to dark. This
 * value has been in the same place since 1809 and is what the Settings toggle
 * writes. */
static int system_prefers_dark(void)
{
    HKEY key;
    DWORD value = 1, cb = sizeof value, type = 0;
    int dark = 0;

    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, L"AppsUseLightTheme", NULL, &type,
                             (LPBYTE)&value, &cb) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            dark = (value == 0);
        }
        RegCloseKey(key);
    }
    return dark;
}

static void build_palette(AprTheme *t)
{
    if (t->high_contrast) {
        palette_high_contrast(&t->c);
    } else if (t->dark) {
        palette_dark(&t->c);
    } else {
        palette_light(&t->c);
    }
}

static void free_gdi_cache(AprTheme *t)
{
    int i;
    for (i = 0; i < t->brush_n; ++i) DeleteObject(t->brush[i].b);
    for (i = 0; i < t->pen_n; ++i) DeleteObject(t->pen[i].p);
    t->brush_n = 0;
    t->pen_n = 0;
}

static void recompute_effective_dark(AprTheme *t)
{
    if (t->high_contrast) {
        /* High contrast supplies both a light and a dark scheme through
         * GetSysColor; "dark" then means whatever the user's scheme says, and
         * we take it from the window colour rather than from our own toggle. */
        COLORREF w = GetSysColor(COLOR_WINDOW);
        t->dark = (GetRValue(w) + GetGValue(w) + GetBValue(w)) < (3 * 128);
    } else if (t->dark_override >= 0) {
        t->dark = t->dark_override ? 1 : 0;
    } else {
        t->dark = system_prefers_dark();
    }
}

/* --------------------------------------------------------------------------
 * Public
 * ----------------------------------------------------------------------- */

AprErr apr_theme_create(UINT dpi, AprTheme **out)
{
    AprTheme *t;
    AprErr e;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"apr_theme_create: out is NULL");
    *out = NULL;

    t = (AprTheme *)calloc(1, sizeof *t);
    if (!t) return APR_ERR(APR_E_NO_MEMORY, L"apr_theme_create: out of memory");

    t->dpi = (dpi == 0) ? apr_dpi_system() : dpi;
    t->dark_override = -1;
    t->high_contrast = system_high_contrast();
    t->rtl = apr_str_is_rtl();
    recompute_effective_dark(t);
    build_palette(t);

    e = build_fonts(t);
    if (apr_failed(&e)) { apr_theme_destroy(t); return e; }
    build_metrics(t);

    APR_INFO(L"theme: dpi=%u dark=%d high-contrast=%d rtl=%d body-face=%s",
             t->dpi, t->dark, t->high_contrast, t->rtl, t->face[APR_FONT_BODY]);

    *out = t;
    return apr_ok();
}

void apr_theme_destroy(AprTheme *t)
{
    if (!t) return;
    free_gdi_cache(t);
    free_fonts(t);
    free(t);
}

AprErr apr_theme_set_dpi(AprTheme *t, UINT dpi)
{
    AprErr e;

    if (!t) return APR_ERR(APR_E_INVALID_ARG, L"apr_theme_set_dpi: theme is NULL");
    if (dpi == 0) dpi = apr_dpi_system();
    if (dpi == t->dpi) return apr_ok();

    t->dpi = dpi;
    e = build_fonts(t);
    if (apr_failed(&e)) return e;
    build_metrics(t);
    return apr_ok();
}

AprErr apr_theme_refresh(AprTheme *t)
{
    int was_dark, was_hc, was_rtl;
    AprErr e;

    if (!t) return APR_ERR(APR_E_INVALID_ARG, L"apr_theme_refresh: theme is NULL");

    was_dark = t->dark;
    was_hc = t->high_contrast;
    was_rtl = t->rtl;

    t->high_contrast = system_high_contrast();
    t->rtl = apr_str_is_rtl();
    recompute_effective_dark(t);

    if (t->dark != was_dark || t->high_contrast != was_hc) {
        free_gdi_cache(t);   /* cached objects carry the old palette */
        build_palette(t);
    }

    /* A language change can change which face covers the script, so the fonts
     * are rebuilt when direction flips. Cheap, and the alternative is Arabic
     * drawn through GDI font linking with Latin metrics -- see the note at the
     * top of this file. */
    if (t->rtl != was_rtl) {
        e = build_fonts(t);
        if (apr_failed(&e)) return e;
        build_metrics(t);
    }
    return apr_ok();
}

AprErr apr_theme_set_dark(AprTheme *t, int dark_or_minus_one)
{
    if (!t) return APR_ERR(APR_E_INVALID_ARG, L"apr_theme_set_dark: theme is NULL");
    t->dark_override = (dark_or_minus_one < 0) ? -1 : (dark_or_minus_one ? 1 : 0);
    return apr_theme_refresh(t);
}

const AprThemePalette *apr_theme_palette(const AprTheme *t) { return t ? &t->c : NULL; }
const AprThemeMetrics *apr_theme_metrics(const AprTheme *t) { return t ? &t->m : NULL; }
UINT apr_theme_dpi(const AprTheme *t)            { return t ? t->dpi : APR_DPI_DEFAULT; }
int  apr_theme_is_dark(const AprTheme *t)        { return t ? t->dark : 0; }
int  apr_theme_high_contrast(const AprTheme *t)  { return t ? t->high_contrast : 0; }
int  apr_theme_is_rtl(const AprTheme *t)         { return t ? t->rtl : 0; }

HFONT apr_theme_font(const AprTheme *t, AprFontRole role)
{
    if (!t || role < 0 || role >= APR_FONT_COUNT) return NULL;
    return t->font[role];
}

const wchar_t *apr_theme_font_face(const AprTheme *t, AprFontRole role)
{
    if (!t || role < 0 || role >= APR_FONT_COUNT) return L"";
    return t->face[role];
}

int apr_theme_px(const AprTheme *t, int logical)
{
    return apr_dpi_scale(logical, t ? t->dpi : APR_DPI_DEFAULT);
}

int apr_theme_line_height(const AprTheme *t, AprFontRole role)
{
    if (!t || role < 0 || role >= APR_FONT_COUNT) return 0;
    return t->line_h[role];
}

int apr_theme_block_height(const AprTheme *t, AprFontRole role, int lines, int pad)
{
    int h;
    if (!t || role < 0 || role >= APR_FONT_COUNT) return 0;
    if (lines < 1) lines = 1;
    if (pad < 0) pad = 0;
    h = lines * t->line_h[role] + 2 * pad;
    if (h < t->m.row_h) h = t->m.row_h;
    return h;
}

int apr_theme_text_width(const AprTheme *t, AprFontRole role, const wchar_t *text)
{
    HDC dc;
    HFONT old;
    SIZE sz;
    int w = 0;

    if (!t || !text || role < 0 || role >= APR_FONT_COUNT) return 0;
    dc = GetDC(NULL);
    if (!dc) return 0;
    old = (HFONT)SelectObject(dc, t->font[role]);
    if (GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz)) w = sz.cx;
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    return w;
}

int apr_theme_font_covers(const AprTheme *t, AprFontRole role, const wchar_t *text)
{
    if (!t || role < 0 || role >= APR_FONT_COUNT) return 0;
    return font_covers(t->font[role], text);
}

HBRUSH apr_theme_brush(AprTheme *t, COLORREF color)
{
    int i;
    HBRUSH b;

    if (!t) return NULL;
    for (i = 0; i < t->brush_n; ++i) {
        if (t->brush[i].color == color) return t->brush[i].b;
    }
    b = CreateSolidBrush(color);
    if (!b) return NULL;
    if (t->brush_n < APR_THEME_BRUSH_CACHE) {
        t->brush[t->brush_n].color = color;
        t->brush[t->brush_n].b = b;
        t->brush_n++;
        return b;
    }
    /* Cache full. Rather than leak, replace the oldest slot: the cache exists
     * so that a WM_PAINT does not allocate, and a painter using more than
     * APR_THEME_BRUSH_CACHE distinct colours has a palette problem, not a
     * cache-size problem. */
    DeleteObject(t->brush[0].b);
    t->brush[0].color = color;
    t->brush[0].b = b;
    return b;
}

HPEN apr_theme_pen(AprTheme *t, COLORREF color, int width)
{
    int i;
    HPEN p;

    if (!t) return NULL;
    if (width < 1) width = 1;
    for (i = 0; i < t->pen_n; ++i) {
        if (t->pen[i].color == color && t->pen[i].width == width) return t->pen[i].p;
    }
    p = CreatePen(PS_SOLID, width, color);
    if (!p) return NULL;
    if (t->pen_n < APR_THEME_PEN_CACHE) {
        t->pen[t->pen_n].color = color;
        t->pen[t->pen_n].width = width;
        t->pen[t->pen_n].p = p;
        t->pen_n++;
        return p;
    }
    DeleteObject(t->pen[0].p);
    t->pen[0].color = color;
    t->pen[0].width = width;
    t->pen[0].p = p;
    return p;
}

void apr_theme_draw_focus(AprTheme *t, HDC dc, const RECT *r)
{
    HGDIOBJ old_brush;
    RECT q;
    int i;
    COLORREF ring[2];

    if (!t || !dc || !r) return;

    /* One focus indicator for the whole product, drawn as two concentric rings
     * in colours that contrast with each other -- see the note on `focus` in
     * ui_theme.h. Two reasons this is not DrawFocusRect:
     *
     *   1. DrawFocusRect XORs a dotted line, which is close to invisible on a
     *      dark surface or an accent fill;
     *   2. an XORed indicator inverts whatever is beneath it, so its contrast
     *      is a property of the artwork rather than of the palette, and cannot
     *      be asserted in a test.
     *
     * Drawn INSIDE the control's bounds so a neighbour never clips it. */
    ring[0] = t->c.focus;
    ring[1] = t->c.focus_inner;

    old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    q = *r;
    InflateRect(&q, -(t->m.focus_ring / 2 + t->m.focus_gap),
                    -(t->m.focus_ring / 2 + t->m.focus_gap));

    for (i = 0; i < 2; ++i) {
        HPEN pen, old_pen;

        if (q.right - q.left <= t->m.focus_ring ||
            q.bottom - q.top <= t->m.focus_ring) {
            break;
        }
        pen = apr_theme_pen(t, ring[i], t->m.focus_ring);
        if (!pen) break;
        old_pen = (HPEN)SelectObject(dc, pen);
        if (t->m.corner > 0) {
            RoundRect(dc, q.left, q.top, q.right, q.bottom,
                      t->m.corner, t->m.corner);
        } else {
            Rectangle(dc, q.left, q.top, q.right, q.bottom);
        }
        SelectObject(dc, old_pen);
        InflateRect(&q, -t->m.focus_ring, -t->m.focus_ring);
    }

    SelectObject(dc, old_brush);
}
