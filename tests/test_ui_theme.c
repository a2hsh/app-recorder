/*
 * test_ui_theme.c -- the parts of the UI foundation that can be tested without
 * a window: DPI arithmetic, the layout function, the token set, contrast, and
 * font coverage.
 *
 * The windowed half lives in test_ui_a11y.c. The split is deliberate: these
 * cases must run on any machine, including one with no interactive window
 * station, so that a UI regression is caught even where a real window cannot
 * be created.
 *
 * WHAT THESE TESTS ARE ACTUALLY DEFENDING
 *
 *   - that layout mirroring goes through ONE function, driven by a direction
 *     PARAMETER, so a test can run both directions in one process (an `if
 *     (rtl)` scattered through paint code cannot be tested this way, which is
 *     most of why it is forbidden);
 *   - that a hairline, a border and a focus ring never round away to nothing
 *     at a fractional scale factor;
 *   - that secondary text still meets WCAG AA, which is the contrast everyone
 *     regresses first;
 *   - that the font actually chosen can render the script actually running,
 *     asked of the font rather than assumed from its name.
 */
#include "test_runner.h"

#include <windows.h>

#include "strings.h"
#include "ui_app.h"
#include "ui_darkmode.h"
#include "ui_dpi.h"
#include "ui_theme.h"

#define LANG_EN MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)
#define LANG_AR MAKELANGID(LANG_ARABIC, SUBLANG_ARABIC_SAUDI_ARABIA)

/* ==========================================================================
 * DPI arithmetic
 * ======================================================================== */

TEST(scale_is_identity_at_96_and_linear_above_it)
{
    ASSERT_EQ_INT(10, apr_dpi_scale(10, 96));
    ASSERT_EQ_INT(20, apr_dpi_scale(10, 192));
    ASSERT_EQ_INT(15, apr_dpi_scale(10, 144));
    ASSERT_EQ_INT(5,  apr_dpi_scale(10, 48));

    /* dpi 0 must not divide by zero and must not scale by zero. */
    ASSERT_EQ_INT(10, apr_dpi_scale(10, 0));
}

TEST(scale_rounds_to_nearest_rather_than_truncating)
{
    /* 1 logical px at 125% is 1.25 -> 1; at 150% it is 1.5 -> 2. Truncation
     * would give 1 in both, which is how a 2px focus ring becomes a 1px one
     * at 150% and looks like a rendering bug rather than a rounding one. */
    ASSERT_EQ_INT(1, apr_dpi_scale(1, 120));
    ASSERT_EQ_INT(2, apr_dpi_scale(1, 144));
    ASSERT_EQ_INT(3, apr_dpi_scale(2, 144));
}

TEST(hairlines_never_round_away_at_any_scale)
{
    UINT dpi;
    /* The whole point of apr_dpi_scale_min: a 1px border that becomes 0px is
     * invisible, and it is invisible precisely on the fractional scale factors
     * most laptops ship with. */
    for (dpi = 96; dpi <= 384; dpi += 4) {
        ASSERT_GE_INT(1, apr_dpi_scale_min(1, dpi, 1));
        ASSERT_GE_INT(2, apr_dpi_scale_min(2, dpi, 2));
    }
    /* A zero input stays zero -- the floor applies to real measurements, not
     * to "no border". */
    ASSERT_EQ_INT(0, apr_dpi_scale_min(0, 240, 1));
}

TEST(font_height_is_negative_and_proportional)
{
    /* Negative lfHeight means "character height". A positive value is a cell
     * height and silently produces text about 25% smaller than requested. */
    ASSERT_EQ_INT(-13, apr_dpi_font_height(10, 96));
    ASSERT_EQ_INT(-27, apr_dpi_font_height(10, 192));
    ASSERT_TRUE(apr_dpi_font_height(10, 96) < 0);
}

TEST(rescale_moves_between_two_nonstandard_dpis)
{
    ASSERT_EQ_INT(200, apr_dpi_rescale(100, 96, 192));
    ASSERT_EQ_INT(100, apr_dpi_rescale(200, 192, 96));
    ASSERT_EQ_INT(150, apr_dpi_rescale(100, 96, 144));
    ASSERT_EQ_INT(100, apr_dpi_rescale(100, 0, 0));
}

TEST(system_dpi_is_plausible)
{
    UINT dpi = apr_dpi_system();
    ASSERT_GE_INT(48, (int)dpi);
    ASSERT_LE_INT(960, (int)dpi);
}

/* ==========================================================================
 * Layout -- both directions, one process
 * ======================================================================== */

static void layout_defaults(AprUiLayoutIn *in, AprUiDir dir, int w, int h)
{
    memset(in, 0, sizeof *in);
    in->client.left = 0;
    in->client.top = 0;
    in->client.right = w;
    in->client.bottom = h;
    in->dir = dir;
    in->tree_w = 280;
    in->pane_min_w = 160;
    in->splitter_w = 6;
    in->status_h = 26;
    in->tree_visible = 1;
}

TEST(ltr_puts_the_structure_panel_on_the_left)
{
    AprUiLayoutIn in;
    AprUiRects r;

    layout_defaults(&in, APR_DIR_LTR, 1000, 700);
    apr_ui_layout(&in, &r);

    ASSERT_EQ_INT(0, (int)r.tree.left);
    ASSERT_EQ_INT(280, (int)r.tree.right);
    ASSERT_EQ_INT(280, (int)r.splitter.left);
    ASSERT_EQ_INT(286, (int)r.canvas.left);
    ASSERT_EQ_INT(1000, (int)r.canvas.right);
}

TEST(rtl_puts_the_structure_panel_on_the_right)
{
    AprUiLayoutIn in;
    AprUiRects r;

    layout_defaults(&in, APR_DIR_RTL, 1000, 700);
    apr_ui_layout(&in, &r);

    /* The mirror image of the LTR case, exactly. Same widths, opposite side. */
    ASSERT_EQ_INT(720, (int)r.tree.left);
    ASSERT_EQ_INT(1000, (int)r.tree.right);
    ASSERT_EQ_INT(714, (int)r.splitter.left);
    ASSERT_EQ_INT(720, (int)r.splitter.right);
    ASSERT_EQ_INT(0, (int)r.canvas.left);
    ASSERT_EQ_INT(714, (int)r.canvas.right);
}

TEST(the_two_directions_are_exact_mirrors_of_each_other)
{
    AprUiLayoutIn a, b;
    AprUiRects ra, rb;

    layout_defaults(&a, APR_DIR_LTR, 1000, 700);
    layout_defaults(&b, APR_DIR_RTL, 1000, 700);
    apr_ui_layout(&a, &ra);
    apr_ui_layout(&b, &rb);

    /* Widths are direction-independent; only position moves. If this ever
     * fails, some size has become a function of direction, which is the bug
     * that makes an Arabic build subtly different rather than mirrored. */
    ASSERT_EQ_INT((int)(ra.tree.right - ra.tree.left),
                  (int)(rb.tree.right - rb.tree.left));
    ASSERT_EQ_INT((int)(ra.canvas.right - ra.canvas.left),
                  (int)(rb.canvas.right - rb.canvas.left));
    ASSERT_EQ_INT((int)(ra.splitter.right - ra.splitter.left),
                  (int)(rb.splitter.right - rb.splitter.left));

    ASSERT_EQ_INT(1000 - (int)ra.tree.right, (int)rb.tree.left);
    ASSERT_EQ_INT(1000 - (int)ra.canvas.right, (int)rb.canvas.left);
}

TEST(panes_tile_the_body_with_no_gap_and_no_overlap)
{
    AprUiDir dirs[2];
    int i;

    dirs[0] = APR_DIR_LTR;
    dirs[1] = APR_DIR_RTL;

    for (i = 0; i < 2; ++i) {
        AprUiLayoutIn in;
        AprUiRects r;
        int total;

        layout_defaults(&in, dirs[i], 1000, 700);
        apr_ui_layout(&in, &r);

        total = (int)(r.tree.right - r.tree.left)
              + (int)(r.splitter.right - r.splitter.left)
              + (int)(r.canvas.right - r.canvas.left);
        ASSERT_EQ_INT(1000, total);

        /* Vertically every pane stops exactly where the status bar starts. */
        ASSERT_EQ_INT((int)r.status.top, (int)r.tree.bottom);
        ASSERT_EQ_INT((int)r.status.top, (int)r.canvas.bottom);
        ASSERT_EQ_INT(0, (int)r.status.left);
        ASSERT_EQ_INT(1000, (int)r.status.right);
        ASSERT_EQ_INT(700, (int)r.status.bottom);
        ASSERT_EQ_INT(26, (int)(r.status.bottom - r.status.top));
    }
}

TEST(hiding_the_structure_panel_gives_the_canvas_everything)
{
    AprUiLayoutIn in;
    AprUiRects r;

    layout_defaults(&in, APR_DIR_RTL, 1000, 700);
    in.tree_visible = 0;
    apr_ui_layout(&in, &r);

    ASSERT_EQ_INT(0, (int)(r.tree.right - r.tree.left));
    ASSERT_EQ_INT(0, (int)(r.splitter.right - r.splitter.left));
    ASSERT_EQ_INT(1000, (int)(r.canvas.right - r.canvas.left));
    ASSERT_EQ_INT(674, (int)(r.canvas.bottom - r.canvas.top));
}

TEST(a_window_too_narrow_for_both_panes_drops_the_panel_entirely)
{
    AprUiLayoutIn in;
    AprUiRects r;

    /* Not zero-width: a zero-width window would still be in the tab order and
     * still be in the accessibility tree, which is a focus trap containing
     * nothing. Dropping it is the accessible answer, not the lazy one. */
    layout_defaults(&in, APR_DIR_LTR, 200, 400);
    apr_ui_layout(&in, &r);

    ASSERT_EQ_INT(0, (int)(r.tree.right - r.tree.left));
    ASSERT_EQ_INT(200, (int)(r.canvas.right - r.canvas.left));
}

TEST(the_panel_shrinks_before_the_canvas_does)
{
    AprUiLayoutIn in;
    AprUiRects r;

    layout_defaults(&in, APR_DIR_LTR, 400, 400);
    apr_ui_layout(&in, &r);

    /* 400 = tree + 6 + canvas, canvas never below pane_min_w. */
    ASSERT_GE_INT(160, (int)(r.canvas.right - r.canvas.left));
    ASSERT_LT_INT(280, (int)(r.tree.right - r.tree.left));
}

TEST(mirroring_is_the_identity_in_ltr_and_an_involution_in_rtl)
{
    RECT bounds, r, original;

    bounds.left = 0; bounds.top = 0; bounds.right = 500; bounds.bottom = 100;
    r.left = 30; r.top = 10; r.right = 130; r.bottom = 60;
    original = r;

    apr_ui_mirror_rect(&r, &bounds, APR_DIR_LTR);
    ASSERT_EQ_INT(original.left, r.left);
    ASSERT_EQ_INT(original.right, r.right);

    apr_ui_mirror_rect(&r, &bounds, APR_DIR_RTL);
    ASSERT_EQ_INT(370, (int)r.left);
    ASSERT_EQ_INT(470, (int)r.right);
    /* Vertical is untouched -- only the horizontal axis mirrors. */
    ASSERT_EQ_INT(10, (int)r.top);
    ASSERT_EQ_INT(60, (int)r.bottom);

    apr_ui_mirror_rect(&r, &bounds, APR_DIR_RTL);
    ASSERT_EQ_INT(original.left, r.left);
    ASSERT_EQ_INT(original.right, r.right);
}

TEST(lead_x_measures_from_the_leading_edge_in_both_directions)
{
    RECT bounds;

    bounds.left = 0; bounds.top = 0; bounds.right = 500; bounds.bottom = 100;

    ASSERT_EQ_INT(20, apr_ui_lead_x(&bounds, 20, 100, APR_DIR_LTR));
    ASSERT_EQ_INT(380, apr_ui_lead_x(&bounds, 20, 100, APR_DIR_RTL));
}

TEST(direction_comes_from_the_string_layer_and_nowhere_else)
{
    AprErr e;

    e = apr_str_set_language(LANG_EN);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(APR_DIR_LTR, (int)apr_ui_dir());
    ASSERT_EQ_INT(0, apr_str_is_rtl());

    e = apr_str_set_language(LANG_AR);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(1, apr_str_is_rtl());
    ASSERT_EQ_INT(APR_DIR_RTL, (int)apr_ui_dir());

    e = apr_str_set_language(LANG_EN);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(APR_DIR_LTR, (int)apr_ui_dir());
}

/* ==========================================================================
 * Theme tokens
 * ======================================================================== */

TEST(theme_creates_and_scales_every_metric_with_dpi)
{
    AprTheme *t96 = NULL, *t192 = NULL;
    AprErr e;
    const AprThemeMetrics *a, *b;

    e = apr_theme_create(96, &t96);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_NOT_NULL(t96);
    e = apr_theme_create(192, &t192);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_NOT_NULL(t192);

    a = apr_theme_metrics(t96);
    b = apr_theme_metrics(t192);

    ASSERT_EQ_INT(2 * a->space_md, b->space_md);
    ASSERT_EQ_INT(2 * a->space_xl, b->space_xl);
    ASSERT_EQ_INT(2 * a->node_w, b->node_w);
    ASSERT_EQ_INT(2 * a->tree_w, b->tree_w);
    ASSERT_GT_INT(a->row_h, b->row_h);

    apr_theme_destroy(t96);
    apr_theme_destroy(t192);
}

TEST(set_dpi_gives_the_same_theme_as_creating_at_that_dpi)
{
    AprTheme *moved = NULL, *born = NULL;
    AprErr e;
    const AprThemeMetrics *a, *b;

    /* A window dragged between monitors must end up identical to one that
     * started on the second monitor. If this drifts, the app looks different
     * depending on where it was opened, which is the classic v1 DPI bug
     * arriving through our own code instead of the OS's. */
    e = apr_theme_create(96, &moved);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_theme_set_dpi(moved, 144);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_theme_create(144, &born);
    ASSERT_FALSE(apr_failed(&e));

    a = apr_theme_metrics(moved);
    b = apr_theme_metrics(born);
    ASSERT_EQ_INT(144, (int)apr_theme_dpi(moved));
    ASSERT_MEM_EQ(b, a, sizeof *a);

    apr_theme_destroy(moved);
    apr_theme_destroy(born);
}

TEST(borders_and_focus_rings_survive_every_scale_factor)
{
    UINT dpi;

    for (dpi = 96; dpi <= 288; dpi += 24) {
        AprTheme *t = NULL;
        AprErr e = apr_theme_create(dpi, &t);
        const AprThemeMetrics *m;

        ASSERT_FALSE(apr_failed(&e));
        m = apr_theme_metrics(t);
        ASSERT_GE_INT(1, m->border);
        ASSERT_GE_INT(2, m->focus_ring);
        ASSERT_GE_INT(1, m->focus_gap);
        ASSERT_GE_INT(1, m->edge_w);
        ASSERT_GE_INT(3, m->port_r);
        ASSERT_GE_INT(4, m->splitter_w);
        apr_theme_destroy(t);
    }
}

TEST(a_row_always_holds_a_line_of_text_with_room_to_spare)
{
    UINT dpi;

    /* This is the mechanical form of "never size a control to fit its English
     * string": row height is derived from the FONT's metrics plus padding, so
     * it is the same number whatever text goes in it -- and it is checked
     * against the font that was actually selected, including a fallback face
     * with taller metrics. */
    for (dpi = 96; dpi <= 288; dpi += 48) {
        AprTheme *t = NULL;
        AprErr e = apr_theme_create(dpi, &t);
        const AprThemeMetrics *m;
        int line;

        ASSERT_FALSE(apr_failed(&e));
        m = apr_theme_metrics(t);
        line = apr_theme_line_height(t, APR_FONT_BODY);
        ASSERT_GT_INT(0, line);
        ASSERT_GE_INT(line + 2 * m->space_sm, m->row_h);
        ASSERT_GE_INT(m->hit_min, m->row_h);
        ASSERT_GE_INT(line, m->status_h);
        ASSERT_GE_INT(2 * line, m->node_min_h);
        apr_theme_destroy(t);
    }
}

TEST(block_height_depends_on_the_font_and_never_on_a_string)
{
    AprTheme *t = NULL;
    AprErr e = apr_theme_create(96, &t);
    int one, two;

    ASSERT_FALSE(apr_failed(&e));
    one = apr_theme_block_height(t, APR_FONT_BODY, 1, 8);
    two = apr_theme_block_height(t, APR_FONT_BODY, 2, 8);
    ASSERT_GT_INT(one, two);
    ASSERT_EQ_INT(apr_theme_line_height(t, APR_FONT_BODY), two - one);
    /* Never below a row: a one-line block and a row must line up. */
    ASSERT_GE_INT(apr_theme_metrics(t)->row_h, one);
    apr_theme_destroy(t);
}

TEST(the_brush_and_pen_caches_return_the_same_handle_for_the_same_colour)
{
    AprTheme *t = NULL;
    AprErr e = apr_theme_create(96, &t);
    HBRUSH b1, b2, b3;
    HPEN p1, p2;

    ASSERT_FALSE(apr_failed(&e));
    b1 = apr_theme_brush(t, RGB(1, 2, 3));
    b2 = apr_theme_brush(t, RGB(1, 2, 3));
    b3 = apr_theme_brush(t, RGB(3, 2, 1));
    ASSERT_NOT_NULL(b1);
    ASSERT_TRUE(b1 == b2);
    ASSERT_TRUE(b1 != b3);

    p1 = apr_theme_pen(t, RGB(1, 2, 3), 2);
    p2 = apr_theme_pen(t, RGB(1, 2, 3), 2);
    ASSERT_NOT_NULL(p1);
    ASSERT_TRUE(p1 == p2);
    /* Same colour, different width, is a different pen. */
    ASSERT_TRUE(p1 != apr_theme_pen(t, RGB(1, 2, 3), 3));

    apr_theme_destroy(t);
}

/* ==========================================================================
 * Contrast
 * ======================================================================== */

static double srgb_channel(int v)
{
    double c = v / 255.0;
    return (c <= 0.03928) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
}

static double luminance(COLORREF c)
{
    return 0.2126 * srgb_channel(GetRValue(c))
         + 0.7152 * srgb_channel(GetGValue(c))
         + 0.0722 * srgb_channel(GetBValue(c));
}

static double contrast(COLORREF a, COLORREF b)
{
    double la = luminance(a), lb = luminance(b);
    double hi = (la > lb) ? la : lb;
    double lo = (la > lb) ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

static void check_contrast(AprTheme *t, const char *what)
{
    const AprThemePalette *c = apr_theme_palette(t);

    if (apr_theme_high_contrast(t)) {
        printf("      (high contrast active: palette is the user's, not ours)\n");
        return;
    }

    printf("      %s: text %.2f, dim %.2f, on-accent %.2f, focus %.2f\n", what,
           contrast(c->text, c->surface),
           contrast(c->text_dim, c->surface),
           contrast(c->text_on_accent, c->accent),
           contrast(c->focus, c->focus_inner));

    /* AA for body text. */
    ASSERT_TRUE(contrast(c->text, c->surface) >= 4.5);
    ASSERT_TRUE(contrast(c->text, c->window_bg) >= 4.5);
    ASSERT_TRUE(contrast(c->text, c->surface_alt) >= 4.5);
    /* Secondary text is the one that always regresses. It is still body text. */
    ASSERT_TRUE(contrast(c->text_dim, c->surface) >= 4.5);
    /* Text on an accent fill -- a selected node, a primary button. */
    ASSERT_TRUE(contrast(c->text_on_accent, c->accent) >= 4.5);
    ASSERT_TRUE(contrast(c->text_sel, c->surface_sel) >= 4.5);
    /* Non-text contrast (AA for UI components) is 3:1. */
    ASSERT_TRUE(contrast(c->accent, c->surface) >= 3.0);
    ASSERT_TRUE(contrast(c->border_strong, c->surface) >= 3.0);

    /* THE FOCUS RING, ASSERTED AS A PROPERTY RATHER THAN AS A COLOUR.
     *
     * This started as `contrast(focus, accent) >= 3.0` and FAILED in both
     * palettes, which is the useful kind of test failure: no single colour can
     * clear 3:1 against a white surface AND a mid-blue accent AND a selection
     * fill. The fix was a two-tone ring, and the property that actually
     * guarantees a visible focus indicator is that for EVERY surface it can
     * land on, at least ONE of the two ring colours reaches 3:1. */
    {
        COLORREF under[5];
        int u;
        under[0] = c->surface;
        under[1] = c->window_bg;
        under[2] = c->accent;
        under[3] = c->surface_sel;
        under[4] = c->surface_alt;
        for (u = 0; u < 5; ++u) {
            double a = contrast(c->focus, under[u]);
            double b = contrast(c->focus_inner, under[u]);
            ASSERT_TRUE((a >= 3.0) || (b >= 3.0));
        }
        /* And the two rings must contrast with each other, or the pair
         * degenerates into one colour and the guarantee above is luck. */
        ASSERT_TRUE(contrast(c->focus, c->focus_inner) >= 3.0);
    }

    ASSERT_TRUE(contrast(c->danger, c->surface) >= 4.5);
    ASSERT_TRUE(contrast(c->warn, c->surface) >= 4.5);
    ASSERT_TRUE(contrast(c->ok, c->surface) >= 4.5);
    ASSERT_TRUE(contrast(c->node_source, c->surface) >= 3.0);
    ASSERT_TRUE(contrast(c->node_bus, c->surface) >= 3.0);
    ASSERT_TRUE(contrast(c->node_action, c->surface) >= 3.0);

    ASSERT_TRUE(c->focus != c->accent);
    /* Selected-and-focused must differ from selected-but-not-focused. */
    ASSERT_TRUE(c->surface_sel != c->surface_sel_bg);
}

TEST(light_palette_meets_wcag_aa)
{
    AprTheme *t = NULL;
    AprErr e = apr_theme_create(96, &t);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_theme_set_dark(t, 0);
    ASSERT_FALSE(apr_failed(&e));
    check_contrast(t, "light");
    apr_theme_destroy(t);
}

TEST(dark_palette_meets_wcag_aa)
{
    AprTheme *t = NULL;
    AprErr e = apr_theme_create(96, &t);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_theme_set_dark(t, 1);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_TRUE(apr_theme_is_dark(t) || apr_theme_high_contrast(t));
    check_contrast(t, "dark");
    apr_theme_destroy(t);
}

TEST(dark_follows_the_system_when_no_override_is_set)
{
    AprTheme *t = NULL;
    AprErr e = apr_theme_create(96, &t);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_theme_set_dark(t, 1);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_theme_set_dark(t, -1);
    ASSERT_FALSE(apr_failed(&e));

    if (!apr_theme_high_contrast(t)) {
        ASSERT_EQ_INT(apr_darkmode_system_prefers_dark(), apr_theme_is_dark(t));
    }
    apr_theme_destroy(t);
}

/* ==========================================================================
 * Fonts and script coverage
 * ======================================================================== */

TEST(every_font_role_resolves_to_a_real_face)
{
    AprTheme *t = NULL;
    AprErr e = apr_theme_create(96, &t);
    int role;

    ASSERT_FALSE(apr_failed(&e));
    for (role = 0; role < APR_FONT_COUNT; ++role) {
        const wchar_t *face = apr_theme_font_face(t, (AprFontRole)role);
        ASSERT_NOT_NULL(apr_theme_font(t, (AprFontRole)role));
        ASSERT_NOT_NULL(face);
        ASSERT_TRUE(face[0] != 0);
        ASSERT_GT_INT(0, apr_theme_line_height(t, (AprFontRole)role));
    }
    printf("      body face: %ls\n", apr_theme_font_face(t, APR_FONT_BODY));
    apr_theme_destroy(t);
}

TEST(the_chosen_font_can_render_the_arabic_actually_in_the_catalog)
{
    AprTheme *t = NULL;
    AprErr e;
    wchar_t arabic[128];
    int n;

    /* The sample comes from the RESOURCE, not from a literal in this file. A
     * wide literal here would be checked against a font while the real strings
     * -- which travelled through rc.exe and LoadStringW -- were never checked,
     * so a mis-decoded resource would sail past. */
    n = apr_str_probe(LANG_AR, APR_S_APP_NAME, arabic, 128);
    ASSERT_GT_INT(0, n);

    e = apr_str_set_language(LANG_AR);
    ASSERT_FALSE(apr_failed(&e));

    e = apr_theme_create(96, &t);
    ASSERT_FALSE(apr_failed(&e));

    printf("      Arabic UI face: %ls\n", apr_theme_font_face(t, APR_FONT_BODY));

    /* THE ASSERTION THAT MATTERS. Not "the face name looks right" -- the font
     * itself is asked whether it has glyphs for this text. */
    ASSERT_TRUE(apr_theme_font_covers(t, APR_FONT_BODY, arabic));
    ASSERT_TRUE(apr_theme_font_covers(t, APR_FONT_BODY_STRONG, arabic));
    ASSERT_TRUE(apr_theme_font_covers(t, APR_FONT_HEADING, arabic));
    ASSERT_TRUE(apr_theme_font_covers(t, APR_FONT_SMALL, arabic));
    ASSERT_TRUE(apr_theme_font_covers(t, APR_FONT_TITLE, arabic));

    apr_theme_destroy(t);
    e = apr_str_set_language(LANG_EN);
    ASSERT_FALSE(apr_failed(&e));
}

TEST(design_6_2_is_wrong_segoe_ui_variable_does_not_cover_arabic)
{
    /* Recorded as a test rather than only as a comment, so that if a future
     * Windows adds Arabic to the family this fails and someone re-reads the
     * decision instead of inheriting it. Informational either way: the theme
     * checks coverage at runtime and does the right thing regardless. */
    HDC dc;
    LOGFONTW lf;
    HFONT f, old;
    wchar_t arabic[128];
    WORD idx[128];
    int n, i, missing = 0;

    n = apr_str_probe(LANG_AR, APR_S_APP_NAME, arabic, 128);
    ASSERT_GT_INT(0, n);

    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -13;
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, L"Segoe UI Variable Text", LF_FACESIZE);
    f = CreateFontIndirectW(&lf);
    ASSERT_NOT_NULL(f);

    dc = GetDC(NULL);
    if (!dc) { DeleteObject(f); SKIP("no screen DC"); return; }
    old = (HFONT)SelectObject(dc, f);
    if (GetGlyphIndicesW(dc, arabic, n, idx, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR) {
        for (i = 0; i < n; ++i) {
            if (arabic[i] >= 0x0600 && arabic[i] <= 0x06FF && idx[i] == 0xFFFF) {
                missing++;
            }
        }
    }
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    DeleteObject(f);

    printf("      Segoe UI Variable Text: %d Arabic code points with no glyph\n",
           missing);
    /* No assertion on `missing` -- the point is the number, printed on every
     * run. The behavioural guarantee is the previous test. */
}

TEST(a_language_change_refreshes_the_theme_without_recreating_it)
{
    AprTheme *t = NULL;
    AprErr e = apr_theme_create(96, &t);
    wchar_t latin_face[LF_FACESIZE], rtl_face[LF_FACESIZE];

    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(0, apr_theme_is_rtl(t));
    lstrcpynW(latin_face, apr_theme_font_face(t, APR_FONT_BODY), LF_FACESIZE);

    e = apr_str_set_language(LANG_AR);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_theme_refresh(t);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(1, apr_theme_is_rtl(t));
    lstrcpynW(rtl_face, apr_theme_font_face(t, APR_FONT_BODY), LF_FACESIZE);
    printf("      latin face %ls -> rtl face %ls\n", latin_face, rtl_face);

    e = apr_str_set_language(LANG_EN);
    ASSERT_FALSE(apr_failed(&e));
    e = apr_theme_refresh(t);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(0, apr_theme_is_rtl(t));

    apr_theme_destroy(t);
}

/* ==========================================================================
 * Dark mode -- the isolated, must-never-be-fatal module
 * ======================================================================== */

TEST(darkmode_never_fails_however_unavailable_it_is)
{
    /* The entire contract: no function here returns an error, every one is
     * safe on a NULL or dead window, and calling them in any order on any
     * build does nothing worse than leave the chrome light. Exercised rather
     * than asserted, because "does not crash and does not return an error" is
     * the whole guarantee. */
    apr_darkmode_init();
    apr_darkmode_init();
    printf("      darkmode available: %d, system prefers dark: %d\n",
           apr_darkmode_available(), apr_darkmode_system_prefers_dark());

    apr_darkmode_set_app_mode(APR_APPMODE_FORCE_DARK);
    apr_darkmode_set_app_mode(APR_APPMODE_FORCE_LIGHT);
    apr_darkmode_set_app_mode(APR_APPMODE_DEFAULT);
    apr_darkmode_apply_to_window(NULL, 1);
    apr_darkmode_apply_to_window((HWND)(ULONG_PTR)0xDEAD, 1);
    apr_darkmode_flush_menu_theme();

    ASSERT_TRUE(apr_darkmode_available() == 0 || apr_darkmode_available() == 1);
}

TEST(only_the_immersive_colour_setting_change_counts_as_a_theme_flip)
{
    ASSERT_TRUE(apr_darkmode_is_color_scheme_change(WM_SETTINGCHANGE,
                                                    (LPARAM)L"ImmersiveColorSet"));
    ASSERT_FALSE(apr_darkmode_is_color_scheme_change(WM_SETTINGCHANGE,
                                                     (LPARAM)L"Environment"));
    ASSERT_FALSE(apr_darkmode_is_color_scheme_change(WM_SETTINGCHANGE, 0));
    ASSERT_FALSE(apr_darkmode_is_color_scheme_change(WM_PAINT,
                                                     (LPARAM)L"ImmersiveColorSet"));
}
