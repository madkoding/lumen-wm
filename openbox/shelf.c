/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   shelf.c for the Openbox window manager

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   See the COPYING file for a copy of the GNU General Public License.
*/

#include "shelf.h"
#include "openbox.h"
#include "config.h"
#include "client.h"
#include "focus.h"
#include "menu.h"
#include "menuframe.h"
#include "screen.h"
#include "paint.h"
#include "obrender/render.h"
#include "obrender/font.h"
#include "obrender/theme.h"

#include <cairo.h>
#include <pango/pangocairo.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <glib/gstdio.h>
#include <time.h>

/* The measurements of the "Studio" desktop: 40px tiles 8px apart in islands
   of 16px radius, held 16px off the edge of the screen. */
#define TILE        40
#define TILE_GAP    8
#define TILE_RADIUS 11
#define PAD         8
#define ISLAND_H    (TILE + 2 * PAD)
#define ISLAND_R    16
#define ISLAND_GAP  8
#define EDGE_INSET  16
#define DIVIDER_H   24
#define DIVIDER_W   (1 + 2 * 8)
#define CLOCK_W     78
#define GLYPH       15
#define GLYPH_GAP   10
/* Room the shadow needs left and right of the islands, and above them */
#define SIDE        24
#define LIP         2
#define TICK_MS     250
#define ART         40
#define BTN         32
#define BTN_GAP     6
#define DAY_W       46
#define WPAD        16
#define MENU_GAP    10    /* between a menu and the island it opens from */
#define HIT_NET     (-90)
#define HIT_VOL     (-91)
#define HIT_POWER   (-92)
#define HIT_BATT    (-93)
#define HIT_PREV    (-100)
#define HIT_TOGGLE  (-101)
#define HIT_NEXT    (-102)

StrutPartial shelf_strut;

/* the commands behind the entries of the status glyphs' menus */
static GPtrArray *sys_cmds;

typedef struct {
    gdouble r, g, b;
} Rgb;

typedef enum { G_SOLID, G_VERT, G_HORIZ, G_DIAG, G_CROSS, G_MIRROR } GradKind;

/* A fill: one colour, or stops of the theme's gradient (0, 0.5 and 1; a
   split gradient has a hard edge at 0.5 between c[1] and c[2]). */
typedef struct {
    GradKind kind;
    gboolean split;
    Rgb      c[4];
} Fill;

typedef struct {
    gboolean light;
    Fill     fill;       /* the island's gradient; body is its mean */
    Rgb      body;       /* island material */
    Rgb      ink;
    gdouble  stroke_a;   /* white stroke alpha */
    Rgb      stroke;
    Rgb      shadow;
    gdouble  shadow_near_a, shadow_mid_a;
    gdouble  hover_a;
    Rgb      hover;
    gdouble  divider_a;
    Rgb      divider;
    Rgb      accent;     /* the running mark of the focused window */
    Fill     hover_fill;
    Fill     accent_fill;
} Material;

#define RGB(r, g, b) { (r) / 255.0, (g) / 255.0, (b) / 255.0 }

static const Material mat_dark = {
    FALSE, { G_SOLID }, RGB(0x21, 0x25, 0x2b), RGB(0xe7, 0xe9, 0xe8),
    0.045, RGB(255, 255, 255), RGB(9, 11, 14), 0.38, 0.72,
    0.10, RGB(255, 255, 255), 0.22, RGB(255, 255, 255),
    RGB(0xe7, 0xe9, 0xe8), { G_SOLID }, { G_SOLID }
};
static const Material mat_light = {
    TRUE, { G_SOLID }, RGB(0xec, 0xee, 0xf1), RGB(0x2b, 0x30, 0x36),
    0.82, RGB(255, 255, 255), RGB(20, 22, 26), 0.14, 0.40,
    0.07, RGB(25, 27, 31), 0.22, RGB(30, 34, 39),
    RGB(0x2b, 0x30, 0x36), { G_SOLID }, { G_SOLID }
};

/* The material taken from the Openbox theme in use, see theme_material(). */
static Material mat_theme;

static void set_rgb(cairo_t *cr, Rgb c, gdouble a);

static Rgb rr_rgb(const RrColor *c)
{
    Rgb o = { RrColorRed(c) / 255.0, RrColorGreen(c) / 255.0,
              RrColorBlue(c) / 255.0 };
    return o;
}

static gdouble linear(gdouble v)
{
    return v <= 0.03928 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

/* WCAG relative luminance */
static gdouble luminance(Rgb c)
{
    return 0.2126 * linear(c.r) + 0.7152 * linear(c.g) + 0.0722 * linear(c.b);
}

static gdouble contrast(Rgb a, Rgb b)
{
    gdouble la = luminance(a), lb = luminance(b);
    return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}

/* the colour when it reads on the body, else black or white, whichever does */
static Rgb readable(Rgb c, Rgb body, gdouble min)
{
    const Rgb white = RGB(255, 255, 255), black = RGB(0, 0, 0);
    if (contrast(c, body) >= min) return c;
    return contrast(white, body) > contrast(black, body) ? white : black;
}

static Rgb mix(Rgb a, Rgb b)
{
    Rgb o = { (a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2 };
    return o;
}

/* the gradient of an Openbox appearance, as a Fill */
static Fill rr_fill(const RrAppearance *a)
{
    const RrSurface *sf = &a->surface;
    Fill f;

    memset(&f, 0, sizeof f);
    f.c[0] = f.c[1] = f.c[2] = f.c[3] = rr_rgb(sf->primary);
    if (!sf->secondary) return f;
    f.c[1] = f.c[2] = f.c[3] = rr_rgb(sf->secondary);
    switch (sf->grad) {
    case RR_SURFACE_SPLIT_VERTICAL:
        f.kind = G_VERT;
        f.split = TRUE;
        if (sf->split_primary && sf->split_secondary) {
            f.c[2] = rr_rgb(sf->split_primary);
            f.c[3] = rr_rgb(sf->split_secondary);
        }
        break;
    case RR_SURFACE_HORIZONTAL:        f.kind = G_HORIZ; break;
    case RR_SURFACE_DIAGONAL:          f.kind = G_DIAG; break;
    case RR_SURFACE_CROSS_DIAGONAL:    f.kind = G_CROSS; break;
    case RR_SURFACE_MIRROR_HORIZONTAL:
    case RR_SURFACE_PYRAMID:           f.kind = G_MIRROR; break;
    case RR_SURFACE_VERTICAL:          f.kind = G_VERT; break;
    default:                           break;
    }
    return f;
}

static Rgb fill_mean(const Fill *f)
{
    return f->kind == G_SOLID ? f->c[0] : mix(f->c[0], f->c[3]);
}

/* make the fill the source of cr for a box */
static void fill_source(cairo_t *cr, const Fill *f, gdouble x, gdouble y,
                        gdouble w, gdouble h, gdouble alpha)
{
    cairo_pattern_t *p;
    gdouble x1 = x, y1 = y, x2 = x, y2 = y + h;

    if (f->kind == G_SOLID) { set_rgb(cr, f->c[0], alpha); return; }
    if (f->kind == G_HORIZ)      { x2 = x + w; y2 = y; }
    else if (f->kind == G_DIAG)  { x2 = x + w; }
    else if (f->kind == G_CROSS) { x1 = x + w; x2 = x; }
    p = cairo_pattern_create_linear(x1, y1, x2, y2);
    #define STOP(o, c) cairo_pattern_add_color_stop_rgba(p, o, c.r, c.g, c.b, alpha)
    if (f->kind == G_MIRROR) {
        STOP(0, f->c[0]); STOP(0.5, f->c[1]); STOP(1, f->c[0]);
    } else if (f->split) {
        STOP(0, f->c[0]); STOP(0.5, f->c[1]);
        STOP(0.5, f->c[2]); STOP(1, f->c[3]);
    } else {
        STOP(0, f->c[0]); STOP(1, f->c[1]);
    }
    #undef STOP
    cairo_set_source(cr, p);
    cairo_pattern_destroy(p);
}

static void hover_source(cairo_t *cr, const Material *m, gdouble x,
                         gdouble y)
{
    if (m->hover_fill.kind != G_SOLID)
        fill_source(cr, &m->hover_fill, x, y, TILE, TILE, m->hover_a);
    else
        set_rgb(cr, m->hover, m->hover_a);
}

/* Build the shelf's material from the colours of the Openbox theme: the
   island is the menu's background, the text its text, the hover its selected
   item and the running mark the title bar of the focused window. */
static const Material *theme_material(void)
{
    const RrTheme *t = ob_rr_theme;
    Material *m = &mat_theme;
    Rgb body, hover, accent;

    if (!t || !t->a_menu || !t->a_menu_selected || !t->a_focused_title ||
        !t->menu_color || !t->menu_border_color || !t->menu_selected_color)
        return &mat_dark;

    m->fill = rr_fill(t->a_menu);
    m->hover_fill = rr_fill(t->a_menu_selected);
    body = fill_mean(&m->fill);
    hover = fill_mean(&m->hover_fill);
    m->accent_fill = rr_fill(t->a_focused_title);
    accent = fill_mean(&m->accent_fill);

    m->body = body;
    m->light = luminance(body) > 0.4;
    m->ink = readable(rr_rgb(t->menu_color), body, 4.5);
    m->stroke = rr_rgb(t->menu_border_color);
    m->stroke_a = contrast(m->stroke, body) > 1.15 ? 0.85 : 0.0;
    m->shadow = m->light ? mat_light.shadow : mat_dark.shadow;
    m->shadow_near_a = m->light ? mat_light.shadow_near_a :
                                  mat_dark.shadow_near_a;
    m->shadow_mid_a = m->light ? mat_light.shadow_mid_a :
                                 mat_dark.shadow_mid_a;
    /* the selected item, but only when it stands out from the island */
    if (contrast(hover, body) >= 1.12 && contrast(m->ink, hover) >= 3.0) {
        m->hover = hover;
        m->hover_a = 0.9;
    } else {
        m->hover_fill.kind = G_SOLID;
        m->hover = m->ink;
        m->hover_a = 0.10;
    }
    m->divider = m->ink;
    m->divider_a = 0.22;
    m->accent = contrast(accent, body) >= 3.0 ? accent : m->ink;
    if (m->accent.r != accent.r || m->accent.g != accent.g ||
        m->accent.b != accent.b)
        m->accent_fill.kind = G_SOLID;
    return m;
}

static gboolean parse_hex(const gchar *s, Rgb *out)
{
    guint r, g, b;
    gchar tail;
    if (!s || sscanf(s, " #%2x%2x%2x %c", &r, &g, &b, &tail) != 3)
        return FALSE;
    out->r = r / 255.0; out->g = g / 255.0; out->b = b / 255.0;
    return TRUE;
}

/* the material with the colours of <colors> laid over it */
static const Material *override_material(const Material *base)
{
    static Material m;
    Rgb c;
    gboolean any = FALSE;

    m = *base;
    if (parse_hex(config_shelf_color_body, &c)) {
        m.body = c; m.fill.kind = G_SOLID; m.fill.c[0] = c;
        m.light = luminance(c) > 0.4;
        if (!parse_hex(config_shelf_color_text, &m.ink))
            m.ink = readable(m.ink, c, 4.5);
        any = TRUE;
    }
    if (parse_hex(config_shelf_color_text, &c)) { m.ink = c; any = TRUE; }
    if (parse_hex(config_shelf_color_hover, &c)) {
        m.hover = c; m.hover_a = 0.9; m.hover_fill.kind = G_SOLID; any = TRUE;
    }
    if (parse_hex(config_shelf_color_accent, &c)) {
        m.accent = c; m.accent_fill.kind = G_SOLID; any = TRUE;
    }
    return any ? &m : base;
}

/* OB_SHELF_DEBUG=1: print the material, for tools/shelf-themes.sh */
static void dump_material(const Material *m)
{
    g_printerr("shelf-material light=%d body=%.3f,%.3f,%.3f "
               "ink=%.3f,%.3f,%.3f contrast=%.2f hover=%.2f accent=%.2f "
               "hover_ink=%.2f\n",
               m->light, m->body.r, m->body.g, m->body.b,
               m->ink.r, m->ink.g, m->ink.b, contrast(m->ink, m->body),
               contrast(m->hover_fill.kind != G_SOLID ?
                        fill_mean(&m->hover_fill) : m->hover, m->body),
               contrast(m->accent, m->body),
               m->hover_a < 0.5 ? 21.0 :
               contrast(m->ink, m->hover_fill.kind != G_SOLID ?
                        fill_mean(&m->hover_fill) : m->hover));
}

typedef struct {
    ObShelfPin *pin;     /* the pinned application, or NULL */
    RrImage    *pin_icon;
    ObClient   *client;  /* its window; NULL for a pin that is not running */
    gint        x;       /* in window coordinates */
} Tile;

/* What a command prints, kept and refreshed on a timer. */
typedef struct {
    const gchar *command;
    guint        interval;
    gchar       *out;        /* everything it printed, or NULL */
    guint        timer;
    gboolean     running;
} Feed;

typedef struct {
    gchar *day, *temp, *glyph;
} Day;

static struct {
    ObWindowId           win;
    Feed             media, weather, net, vol;
    gboolean         has_media, has_weather;
    gchar           *art_file;   /* the picture the media island shows */
    RrImage         *art;
    gint             media_x, media_w, weather_x, weather_w;
    gint             text_w;     /* the title's room in the media island */
    gboolean         argb;       /* a compositor is running */
    gboolean         shown;
    gint             wx, wy, ww, wh;
    gint             dock_x, dock_w, status_x, status_w, island_y;
    GArray          *tiles;      /* Tile, the dock's app tiles */
    GHashTable      *pin_icons;  /* ObShelfPin* -> RrImage* (or NULL) */
    gint             hover;      /* -1 nothing, 0 the apps tile, n the nth app */
    gchar           *sig;
    gint             press, press_x;  /* a tile held down with button 1 */
    gboolean         dragging;
    gint             batt_pct;   /* -1 when there is no battery */
    gboolean         batt_charging;
    guint            pins_rev;   /* bumped when the pins change */
    guint            tick_id;
    guint            snap_id;
    cairo_surface_t *bg;         /* what the wallpaper looks like under us */
    const Material  *m;
    gboolean         ready;
} S;

/* ---- drawing helpers ---------------------------------------------------- */

static void rrect(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h,
                  gdouble r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,          G_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2,   G_PI);
    cairo_arc(cr, x + r,     y + r,     r, G_PI,       3 * G_PI / 2);
    cairo_close_path(cr);
}

static void set_rgb(cairo_t *cr, Rgb c, gdouble a)
{
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a);
}

/* A box blur in three passes closely follows a gaussian of this sigma. */
static void box_blur(guint8 *a, gint w, gint h, gint r)
{
    guint8 *tmp = g_malloc(MAX(w, h));
    gint pass, x, y;
    if (r < 1) { g_free(tmp); return; }
    for (pass = 0; pass < 3; ++pass) {
        for (y = 0; y < h; ++y) {
            guint8 *row = a + y * w;
            gint sum = 0, n = 2 * r + 1;
            for (x = -r; x <= r; ++x) sum += (x >= 0 && x < w ? row[x] : 0);
            for (x = 0; x < w; ++x) {
                tmp[x] = sum / n;
                sum += (x + r + 1 < w ? row[x + r + 1] : 0)
                     - (x - r >= 0 ? row[x - r] : 0);
            }
            memcpy(row, tmp, w);
        }
        for (x = 0; x < w; ++x) {
            gint sum = 0, n = 2 * r + 1;
            for (y = -r; y <= r; ++y)
                sum += (y >= 0 && y < h ? a[y * w + x] : 0);
            for (y = 0; y < h; ++y) {
                tmp[y] = sum / n;
                sum += (y + r + 1 < h ? a[(y + r + 1) * w + x] : 0)
                     - (y - r >= 0 ? a[(y - r) * w + x] : 0);
            }
            for (y = 0; y < h; ++y) a[y * w + x] = tmp[y];
        }
    }
    g_free(tmp);
}

/* css: box-shadow: 0 <dy>px <blur>px <spread>px colour, blur radius = 2 sigma */
static void shadow(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h,
                   gdouble r, gdouble dy, gdouble blur, gdouble spread,
                   Rgb col, gdouble alpha, gint ww, gint wh)
{
    cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, ww, wh);
    cairo_t *mc = cairo_create(mask);
    gint stride = cairo_image_surface_get_stride(mask);
    guint8 *data, *packed;
    gint yy;

    cairo_set_source_rgba(mc, 0, 0, 0, 1);
    rrect(mc, x - spread, y + dy - spread, w + 2 * spread, h + 2 * spread,
          MAX(0, r + spread));
    cairo_fill(mc);
    cairo_destroy(mc);
    cairo_surface_flush(mask);
    data = cairo_image_surface_get_data(mask);
    packed = g_malloc(ww * wh);
    for (yy = 0; yy < wh; ++yy) memcpy(packed + yy * ww, data + yy * stride, ww);
    box_blur(packed, ww, wh, MAX(1, (gint)(blur / 2.0 * 0.6 + 0.5)));
    for (yy = 0; yy < wh; ++yy) memcpy(data + yy * stride, packed + yy * ww, ww);
    g_free(packed);
    cairo_surface_mark_dirty(mask);

    set_rgb(cr, col, alpha);
    cairo_mask_surface(cr, mask, 0, 0);
    cairo_surface_destroy(mask);
}

static PangoLayout *text(cairo_t *cr, const gchar *str, gdouble px,
                         PangoWeight weight)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd =
        pango_font_description_from_string(config_shelf_font &&
                                           *config_shelf_font ?
                                           config_shelf_font : "Sans");
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    pango_font_description_set_weight(fd, weight);
    pango_layout_set_font_description(l, fd);
    pango_layout_set_text(l, str, -1);
    pango_font_description_free(fd);
    return l;
}

static void centred_text(cairo_t *cr, const gchar *str, gdouble px,
                         PangoWeight weight, gdouble cx, gdouble cy,
                         Rgb col, gdouble alpha)
{
    PangoLayout *l = text(cr, str, px, weight);
    PangoRectangle ink, logical;
    pango_layout_get_pixel_extents(l, &ink, &logical);
    set_rgb(cr, col, alpha);
    cairo_move_to(cr, cx - logical.width / 2.0, cy - logical.height / 2.0);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

/* Lucide-style glyphs, 24 unit grid drawn with a 2 unit round stroke. */
static void glyph_begin(cairo_t *cr, gdouble x, gdouble y, Rgb ink)
{
    cairo_new_path(cr);   /* text leaves a current point behind */
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, GLYPH / 24.0, GLYPH / 24.0);
    cairo_set_line_width(cr, 2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    set_rgb(cr, ink, 1);
}

static void glyph_wifi(cairo_t *cr, gdouble x, gdouble y, Rgb ink)
{
    glyph_begin(cr, x, y, ink);
    cairo_arc(cr, 12, 20, 0.01, 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_new_path(cr); cairo_arc(cr, 12, 19, 8.5, G_PI * 1.2, G_PI * 1.8);
    cairo_stroke(cr);
    cairo_new_path(cr); cairo_arc(cr, 12, 18.5, 4.8, G_PI * 1.2, G_PI * 1.8);
    cairo_stroke(cr);
    cairo_new_path(cr); cairo_arc(cr, 12, 18.5, 13, G_PI * 1.2, G_PI * 1.8);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void glyph_volume(cairo_t *cr, gdouble x, gdouble y, Rgb ink)
{
    glyph_begin(cr, x, y, ink);
    cairo_move_to(cr, 11, 4.7); cairo_line_to(cr, 6.5, 9); cairo_line_to(cr, 3, 9);
    cairo_line_to(cr, 3, 15); cairo_line_to(cr, 6.5, 15); cairo_line_to(cr, 11, 19.3);
    cairo_close_path(cr); cairo_stroke(cr);
    cairo_new_path(cr); cairo_arc(cr, 11, 12, 6, -G_PI / 4, G_PI / 4);
    cairo_stroke(cr);
    cairo_new_path(cr); cairo_arc(cr, 11, 12, 10.5, -G_PI / 4, G_PI / 4);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void glyph_battery(cairo_t *cr, gdouble x, gdouble y, Rgb ink,
                          gint pct, gboolean charging)
{
    glyph_begin(cr, x, y, ink);
    rrect(cr, 2, 6.5, 17, 11, 2.5);
    cairo_stroke(cr);
    cairo_move_to(cr, 22, 10.5); cairo_line_to(cr, 22, 13.5);
    cairo_stroke(cr);
    if (charging) {
        cairo_move_to(cr, 11.5, 8); cairo_line_to(cr, 8.5, 12.5);
        cairo_line_to(cr, 12.5, 12.5); cairo_line_to(cr, 9.5, 17);
        cairo_stroke(cr);
    } else {
        rrect(cr, 4.5, 9, MAX(1.5, 12 * CLAMP(pct, 0, 100) / 100.0), 6, 1);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

static void glyph_power(cairo_t *cr, gdouble x, gdouble y, Rgb ink)
{
    glyph_begin(cr, x, y, ink);
    cairo_move_to(cr, 12, 2); cairo_line_to(cr, 12, 12); cairo_stroke(cr);
    cairo_new_path(cr);
    cairo_arc(cr, 12, 13, 9, -G_PI * 0.30, G_PI * 1.30 - 2 * G_PI + 2 * G_PI);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void glyph_apps(cairo_t *cr, gdouble cx, gdouble cy, Rgb ink)
{
    gint i;
    for (i = 0; i < 4; ++i) {
        cairo_arc(cr, cx + (i % 2 ? 5 : -5), cy + (i / 2 ? 5 : -5), 2.6, 0,
                  2 * G_PI);
        set_rgb(cr, ink, 1);
        cairo_fill(cr);
    }
}

/* The best picture of a client's icon as a premultiplied cairo surface. */
static cairo_surface_t *icon_surface(RrImage *image)
{
    RrImagePic *best = NULL;
    RrImageSet *set;
    cairo_surface_t *s;
    guint32 *dst;
    gint i, n;

    if (!image || !(set = image->set)) return NULL;
    for (i = 0; i < set->n_original; ++i) {
        RrImagePic *p = set->original[i];
        if (!best ||
            (best->width < TILE && p->width > best->width) ||
            (p->width >= TILE && p->width < best->width))
            best = p;
    }
    if (!best || best->width < 1 || best->height < 1) return NULL;
    s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                   best->width, best->height);
    dst = (guint32*)cairo_image_surface_get_data(s);
    n = best->width * best->height;
    cairo_surface_flush(s);
    for (i = 0; i < n; ++i) {
        guint32 p = best->data[i];
        guint a = p >> 24, r = (p >> 16) & 0xff, g = (p >> 8) & 0xff,
              b = p & 0xff;
        dst[i] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) |
                 (b * a / 255);
    }
    cairo_surface_mark_dirty(s);
    return s;
}

/* The label's first letter, for a tile without a picture */
static const gchar *tile_label(const Tile *t)
{
    ObClient *c = t->client;
    if (t->pin && t->pin->name && *t->pin->name) return t->pin->name;
    if (c) return c->class && *c->class ? c->class : c->name;
    return t->pin ? t->pin->command : NULL;
}

static void draw_tile(cairo_t *cr, const Tile *t, gdouble x, gdouble y,
                      const Material *m)
{
    ObClient *c = t->client;
    gboolean iconic = c && c->iconic;
    cairo_surface_t *ic = icon_surface(t->pin_icon ? t->pin_icon :
                                       c ? c->icon_set : NULL);

    cairo_save(cr);
    rrect(cr, x, y, TILE, TILE, TILE_RADIUS);
    cairo_clip(cr);
    if (ic) {
        gint w = cairo_image_surface_get_width(ic);
        gint h = cairo_image_surface_get_height(ic);
        cairo_scale(cr, (gdouble)TILE / w, (gdouble)TILE / h);
        cairo_set_source_surface(cr, ic, x * w / TILE, y * h / TILE);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
        cairo_paint_with_alpha(cr, iconic ? 0.5 : 1.0);
    } else {
        gchar init[8] = "?";
        const gchar *name = tile_label(t);
        if (name && *name)
            g_unichar_to_utf8(g_unichar_toupper(g_utf8_get_char(name)), init),
            init[g_utf8_next_char(name) - name] = 0;
        cairo_set_source_rgba(cr, m->ink.r, m->ink.g, m->ink.b, 0.14);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_save(cr);
        centred_text(cr, init, 20, PANGO_WEIGHT_SEMIBOLD, x + TILE / 2.0,
                     y + TILE / 2.0, m->ink, iconic ? 0.5 : 1);
    }
    cairo_restore(cr);
    if (ic) cairo_surface_destroy(ic);
}

/* ---- layout --------------------------------------------------------------- */

static gboolean listed(ObClient *c)
{
    return !c->skip_taskbar &&
           (c->desktop == DESKTOP_ALL || c->desktop == screen_desktop);
}

static gboolean covered_by_fullscreen(void)
{
    return focus_client && focus_client->fullscreen &&
           (focus_client->desktop == DESKTOP_ALL ||
            focus_client->desktop == screen_desktop);
}

/* The pin's window class: named in rc.xml, else the command's first word */
static gboolean pin_matches(const ObShelfPin *p, const ObClient *c)
{
    gchar *word = NULL;
    const gchar *want = p->wmclass;
    gboolean ok;

    if (!want || !*want) {
        const gchar *sp = strpbrk(p->command, " \t");
        gchar *first = sp ? g_strndup(p->command, sp - p->command) :
                       g_strdup(p->command);
        word = g_path_get_basename(first);
        g_free(first);
        want = word;
    }
    ok = (c->class && !g_ascii_strcasecmp(c->class, want)) ||
         (c->name && !g_ascii_strcasecmp(c->name, want));
    g_free(word);
    return ok;
}

static gboolean pinned(const ObClient *c)
{
    GSList *it;
    for (it = config_shelf_pins; it; it = g_slist_next(it))
        if (pin_matches(it->data, c)) return TRUE;
    return FALSE;
}

/* ---- the media and weather islands --------------------------------------- */

/* The nth line a feed printed, stripped ("" when it has none) */
static gchar *feed_line(const Feed *f, gint n)
{
    gchar **v;
    gchar *r;
    if (!f->out) return g_strdup("");
    v = g_strsplit(f->out, "\n", 0);
    r = g_strdup(g_strv_length(v) > (guint)n ? v[n] : "");
    g_strfreev(v);
    return g_strstrip(r);
}

static gboolean media_shown(void)
{
    gchar *st;
    gboolean ok;
    if (!S.has_media) return FALSE;
    st = feed_line(&S.media, 0);
    ok = !g_ascii_strcasecmp(st, "playing") || !g_ascii_strcasecmp(st, "paused");
    g_free(st);
    return ok;
}

static gboolean media_playing(void)
{
    gchar *st = feed_line(&S.media, 0);
    gboolean ok = !g_ascii_strcasecmp(st, "playing");
    g_free(st);
    return ok;
}

static gboolean weather_shown(void)
{
    gchar *t;
    gboolean ok;
    if (!S.has_weather) return FALSE;
    t = feed_line(&S.weather, 0);
    ok = !!*t;
    g_free(t);
    return ok;
}

/* The forecast days ("day|temperature|glyph" lines): up to three */
static gint weather_days(Day days[3])
{
    gint i, n = 0;
    for (i = 2; i < 5; ++i) {
        gchar *l = feed_line(&S.weather, i);
        gchar **v = g_strsplit(l, "|", 3);
        if (g_strv_length(v) == 3) {
            days[n].day = g_strdup(v[0]);
            days[n].temp = g_strdup(v[1]);
            days[n].glyph = g_strdup(v[2]);
            ++n;
        }
        g_strfreev(v);
        g_free(l);
    }
    return n;
}

static void days_free(Day days[3], gint n)
{
    while (n--) {
        g_free(days[n].day);
        g_free(days[n].temp);
        g_free(days[n].glyph);
    }
}

static gint text_width(cairo_t *cr, const gchar *str, gdouble px,
                       PangoWeight weight)
{
    PangoLayout *l = text(cr, str, px, weight);
    PangoRectangle ink, logical;
    pango_layout_get_pixel_extents(l, &ink, &logical);
    g_object_unref(l);
    return logical.width;
}

/* Sizes of the two islands, from what their commands printed */
static void layout_islands(void)
{
    cairo_surface_t *sc = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(sc);

    S.media_w = S.weather_w = 0;
    if (media_shown()) {
        gchar *title = feed_line(&S.media, 1), *artist = feed_line(&S.media, 2);
        gint tw = MAX(text_width(cr, title, 15, PANGO_WEIGHT_SEMIBOLD),
                      text_width(cr, artist, 13, PANGO_WEIGHT_NORMAL));
        S.text_w = CLAMP(tw, 60, 150);
        S.media_w = PAD + ART + 12 + S.text_w + 16 +
                    3 * BTN + 2 * BTN_GAP + PAD;
        g_free(title);
        g_free(artist);
    }
    if (weather_shown()) {
        gchar *temp = feed_line(&S.weather, 0), *sky = feed_line(&S.weather, 1);
        Day days[3];
        gint lw = MAX(text_width(cr, temp, 26, PANGO_WEIGHT_SEMIBOLD),
                      text_width(cr, sky, 12, PANGO_WEIGHT_NORMAL));
        gint n = weather_days(days);
        lw = MIN(lw, 130);
        S.weather_w = 2 * WPAD + lw +
                      (n ? DIVIDER_W + n * DAY_W + (n - 1) * 4 - 8 : 0);
        days_free(days, n);
        g_free(temp);
        g_free(sky);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(sc);
}

/* The first battery the kernel lists, or -1 */
static void battery_poll(void)
{
    GDir *dir;
    const gchar *name, *root;

    S.batt_pct = -1;
    S.batt_charging = FALSE;
    if (!config_shelf_battery) return;
    root = config_shelf_battery_dir && *config_shelf_battery_dir ?
           config_shelf_battery_dir : "/sys/class/power_supply";
    dir = g_dir_open(root, 0, NULL);
    while (dir && (name = g_dir_read_name(dir))) {
        gchar *base = g_build_filename(root, name, NULL);
        gchar *f, *type = NULL, *cap = NULL, *st = NULL;

        f = g_build_filename(base, "type", NULL);
        g_file_get_contents(f, &type, NULL, NULL);
        g_free(f);
        f = g_build_filename(base, "capacity", NULL);
        g_file_get_contents(f, &cap, NULL, NULL);
        g_free(f);
        f = g_build_filename(base, "status", NULL);
        g_file_get_contents(f, &st, NULL, NULL);
        g_free(f);
        if (type && cap && g_ascii_isdigit(*cap) &&
            g_str_has_prefix(type, "Battery")) {
            S.batt_pct = CLAMP((gint)g_ascii_strtoll(cap, NULL, 10), 0, 100);
            S.batt_charging = st && g_str_has_prefix(st, "Charging");
        }
        g_free(base); g_free(type); g_free(cap); g_free(st);
        if (S.batt_pct >= 0) break;
    }
    if (dir) g_dir_close(dir);
}

/* A column of the status island: the glyph, and under it a value when the
   config can tell one. Wider when there are values to show. */
static gint col_w(void)
{
    return config_shelf_battery ||
           (config_shelf_network && config_shelf_network->list) ||
           (config_shelf_volume && config_shelf_volume->level) ?
           34 : GLYPH + GLYPH_GAP;
}

static gint battery_w(void)
{
    return S.batt_pct >= 0 ? col_w() : 0;
}

/* The active network's signal, from the "in-use:signal:name" lines */
static gchar *net_text(void)
{
    gchar **lines, *res = NULL;
    gint i;

    if (!S.net.out) return NULL;
    lines = g_strsplit(S.net.out, "\n", -1);
    for (i = 0; lines[i] && !res; ++i) {
        gchar *c = lines[i], *e;
        if (*c != '*' || !(c = strchr(c, ':'))) continue;
        for (e = ++c; g_ascii_isdigit(*e); ++e);
        if (e > c && *e == ':') res = g_strdup_printf("%.*s%%", (gint)(e - c), c);
    }
    g_strfreev(lines);
    return res;
}

/* The volume as the command prints it: a number gets a percent sign */
static gchar *vol_text(void)
{
    gchar *t, *e;

    if (!S.vol.out || !*S.vol.out) return NULL;
    t = g_strdup(S.vol.out);
    t[strcspn(t, "\n \t")] = 0;
    for (e = t; g_ascii_isdigit(*e); ++e);
    if (e > t && !*e) {
        gchar *p = g_strdup_printf("%s%%", t);
        g_free(t);
        return p;
    }
    if (strlen(t) > 5) t[5] = 0;
    return t;
}

static void layout(void)
{
    const Rect *mon = screen_physical_area_monitor(screen_monitor_primary(FALSE));
    GList *it;
    GSList *is;
    gint n = 0, row_w, row_x, bottom, x;

    if (S.tiles) g_array_set_size(S.tiles, 0);
    else S.tiles = g_array_new(FALSE, TRUE, sizeof(Tile));

    /* the pins, each with its most recently focused window if it has one */
    for (is = config_shelf_pins; is; is = g_slist_next(is)) {
        Tile t = { is->data, g_hash_table_lookup(S.pin_icons, is->data),
                   NULL, 0 };
        for (it = focus_order; it; it = g_list_next(it))
            if (listed(it->data) && pin_matches(t.pin, it->data)) {
                t.client = it->data;
                break;
            }
        g_array_append_val(S.tiles, t);
        ++n;
    }
    /* then the windows no pin claims */
    for (it = client_list; it; it = g_list_next(it)) {
        Tile t = { NULL, NULL, it->data, 0 };
        if (listed(t.client) && !pinned(t.client)) {
            g_array_append_val(S.tiles, t);
            ++n;
        }
    }

    S.dock_w = 2 * PAD + TILE;
    if (n) S.dock_w += DIVIDER_W + n * TILE + (n - 1) * TILE_GAP;
    layout_islands();
    battery_poll();
    S.status_w = 2 * PAD + CLOCK_W + DIVIDER_W + battery_w() + 3 * col_w();
    row_w = S.dock_w + ISLAND_GAP + S.status_w;
    if (S.media_w) row_w += ISLAND_GAP + S.media_w;
    if (S.weather_w) row_w += ISLAND_GAP + S.weather_w;
    row_x = mon->x + (mon->width - row_w) / 2;
    bottom = mon->y + mon->height;

    S.ww = row_w + 2 * SIDE;
    S.wx = row_x - SIDE;
    S.island_y = LIP;
    S.wh = LIP + ISLAND_H + EDGE_INSET;
    S.wy = bottom - S.wh;
    S.dock_x = SIDE;
    x = SIDE + S.dock_w + ISLAND_GAP;
    S.media_x = x;
    if (S.media_w) x += S.media_w + ISLAND_GAP;
    S.weather_x = x;
    if (S.weather_w) x += S.weather_w + ISLAND_GAP;
    S.status_x = x;

    x = S.dock_x + PAD + TILE + DIVIDER_W;
    for (n = 0; n < (gint)S.tiles->len; ++n) {
        g_array_index(S.tiles, Tile, n).x = x;
        x += TILE + TILE_GAP;
    }

    STRUT_PARTIAL_SET(shelf_strut, 0, 0, 0,
                      EDGE_INSET + ISLAND_H + ISLAND_GAP,
                      0, 0, 0, 0, 0, 0,
                      mon->x, mon->x + mon->width - 1);
}

/* ---- painting ------------------------------------------------------------- */

/* Glyphs of the media buttons: filled, centred on (cx, cy), 12 px high. */
static void glyph_prev(cairo_t *cr, gdouble cx, gdouble cy, Rgb ink)
{
    set_rgb(cr, ink, 1);
    cairo_rectangle(cr, cx - 6, cy - 6, 2, 12);
    cairo_fill(cr);
    cairo_move_to(cr, cx + 6, cy - 6);
    cairo_line_to(cr, cx + 6, cy + 6);
    cairo_line_to(cr, cx - 3, cy);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void glyph_next(cairo_t *cr, gdouble cx, gdouble cy, Rgb ink)
{
    set_rgb(cr, ink, 1);
    cairo_rectangle(cr, cx + 4, cy - 6, 2, 12);
    cairo_fill(cr);
    cairo_move_to(cr, cx - 6, cy - 6);
    cairo_line_to(cr, cx - 6, cy + 6);
    cairo_line_to(cr, cx + 3, cy);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void glyph_pause(cairo_t *cr, gdouble cx, gdouble cy, Rgb ink)
{
    set_rgb(cr, ink, 1);
    rrect(cr, cx - 5, cy - 6, 3.5, 12, 1);
    cairo_fill(cr);
    rrect(cr, cx + 1.5, cy - 6, 3.5, 12, 1);
    cairo_fill(cr);
}

static void glyph_play(cairo_t *cr, gdouble cx, gdouble cy, Rgb ink)
{
    set_rgb(cr, ink, 1);
    cairo_move_to(cr, cx - 4, cy - 7);
    cairo_line_to(cr, cx - 4, cy + 7);
    cairo_line_to(cr, cx + 7, cy);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void cloud_shape(cairo_t *cr, gdouble cx, gdouble cy, gdouble k)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx - 5 * k, cy + 1 * k, 4 * k, 0, 2 * G_PI);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx - 0.5 * k, cy - 2 * k, 5 * k, 0, 2 * G_PI);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx + 5 * k, cy + 1 * k, 4 * k, 0, 2 * G_PI);
    cairo_rectangle(cr, cx - 5 * k, cy + 1 * k, 10 * k, 4 * k);
}

/* A weather glyph in a 20 px box centred on (cx, cy). */
static void glyph_weather(cairo_t *cr, const gchar *kind, gdouble cx,
                          gdouble cy, Rgb ink)
{
    gboolean sun = !strcmp(kind, "sun") || !strcmp(kind, "cloud-sun");
    gboolean cloud = !strcmp(kind, "cloud") || !strcmp(kind, "cloud-sun") ||
                     !strcmp(kind, "rain") || !strcmp(kind, "snow") ||
                     !strcmp(kind, "storm");
    gint i;

    cairo_save(cr);
    set_rgb(cr, ink, 1);
    if (sun) {
        gdouble sx = cloud ? cx + 4 : cx, sy = cloud ? cy - 4 : cy;
        gdouble r = cloud ? 3.2 : 4.2;
        cairo_arc(cr, sx, sy, r, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_set_line_width(cr, 1.5);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        for (i = 0; i < 8; ++i) {
            gdouble a = i * G_PI / 4;
            if (cloud && (i == 2 || i == 3 || i == 4 || i == 5)) continue;
            cairo_move_to(cr, sx + cos(a) * (r + 2), sy + sin(a) * (r + 2));
            cairo_line_to(cr, sx + cos(a) * (r + 4), sy + sin(a) * (r + 4));
        }
        cairo_stroke(cr);
    }
    if (cloud) {
        gdouble k = !strcmp(kind, "cloud-sun") ? 0.62 : 0.8;
        gdouble ox = !strcmp(kind, "cloud-sun") ? -2 : 0;
        gdouble oy = !strcmp(kind, "cloud-sun") ? 3 : 0;
        if (!strcmp(kind, "rain") || !strcmp(kind, "snow") ||
            !strcmp(kind, "storm")) oy = -3;
        cloud_shape(cr, cx + ox, cy + oy, k);
        cairo_fill(cr);
        if (!strcmp(kind, "rain")) {
            cairo_set_line_width(cr, 1.6);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            for (i = 0; i < 3; ++i) {
                cairo_move_to(cr, cx - 4 + i * 4 + 1, cy + 3);
                cairo_line_to(cr, cx - 4 + i * 4 - 1, cy + 7);
            }
            cairo_stroke(cr);
        } else if (!strcmp(kind, "snow")) {
            for (i = 0; i < 3; ++i) {
                cairo_arc(cr, cx - 4 + i * 4, cy + 5 + (i % 2), 1.2, 0,
                          2 * G_PI);
                cairo_fill(cr);
            }
        } else if (!strcmp(kind, "storm")) {
            cairo_move_to(cr, cx + 1, cy + 2);
            cairo_line_to(cr, cx - 3, cy + 6);
            cairo_line_to(cr, cx, cy + 6);
            cairo_line_to(cr, cx - 2, cy + 9);
            cairo_line_to(cr, cx + 3, cy + 5);
            cairo_line_to(cr, cx, cy + 5);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
    } else if (!strcmp(kind, "fog")) {
        cairo_set_line_width(cr, 1.8);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        for (i = 0; i < 3; ++i) {
            cairo_move_to(cr, cx - 7, cy - 4 + i * 4);
            cairo_line_to(cr, cx + 7, cy - 4 + i * 4);
        }
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void paint_media(cairo_t *cr, const Material *m, gint y)
{
    gdouble x = S.media_x + PAD, cy = y + ISLAND_H / 2.0;
    gchar *title = feed_line(&S.media, 1), *artist = feed_line(&S.media, 2);
    PangoLayout *l;
    PangoRectangle ink, lg;
    gint i;
    gboolean playing = media_playing();
    cairo_surface_t *ic = icon_surface(S.art);

    /* the cover, or a note on a tinted square */
    cairo_save(cr);
    rrect(cr, x, y + PAD, ART, ART, 9);
    cairo_clip(cr);
    if (ic) {
        gint w = cairo_image_surface_get_width(ic);
        gint h = cairo_image_surface_get_height(ic);
        cairo_translate(cr, x, y + PAD);
        cairo_scale(cr, (gdouble)ART / w, (gdouble)ART / h);
        cairo_set_source_surface(cr, ic, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
        cairo_paint(cr);
    } else {
        set_rgb(cr, m->ink, 0.14);
        cairo_paint(cr);
    }
    cairo_restore(cr);
    if (ic) cairo_surface_destroy(ic);

    /* title over artist */
    x += ART + 12;
    l = text(cr, title, 15, PANGO_WEIGHT_SEMIBOLD);
    pango_layout_set_width(l, S.text_w * PANGO_SCALE);
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    pango_layout_get_pixel_extents(l, &ink, &lg);
    set_rgb(cr, m->ink, 1);
    cairo_move_to(cr, x, cy - 1 - lg.height);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    l = text(cr, artist, 13, PANGO_WEIGHT_NORMAL);
    pango_layout_set_width(l, S.text_w * PANGO_SCALE);
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    set_rgb(cr, m->ink, 0.62);
    cairo_move_to(cr, x, cy + 1);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    g_free(title);
    g_free(artist);

    /* previous, play/pause, next: round buttons */
    x += S.text_w + 16;
    for (i = 0; i < 3; ++i) {
        gdouble bx = x + i * (BTN + BTN_GAP) + BTN / 2.0;
        cairo_arc(cr, bx, cy, BTN / 2.0, 0, 2 * G_PI);
        set_rgb(cr, m->ink, (i == 1 ? 0.18 : 0.09) +
                (S.hover == HIT_PREV - i ? 0.10 : 0));
        cairo_fill(cr);
        if (i == 0) glyph_prev(cr, bx, cy, m->ink);
        else if (i == 1) (playing ? glyph_pause : glyph_play)(cr, bx, cy, m->ink);
        else glyph_next(cr, bx, cy, m->ink);
    }
}

static void paint_weather(cairo_t *cr, const Material *m, gint y)
{
    gdouble x = S.weather_x + WPAD, cy = y + ISLAND_H / 2.0;
    gchar *temp = feed_line(&S.weather, 0), *sky = feed_line(&S.weather, 1);
    Day days[3];
    gint i, n = weather_days(days);
    gint lw = S.weather_w - 2 * WPAD -
              (n ? DIVIDER_W + n * DAY_W + (n - 1) * 4 - 8 : 0);
    PangoLayout *l;
    PangoRectangle ink, lg;

    l = text(cr, temp, 26, PANGO_WEIGHT_SEMIBOLD);
    pango_layout_get_pixel_extents(l, &ink, &lg);
    set_rgb(cr, m->ink, 1);
    cairo_move_to(cr, x, cy - lg.height + 5);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    l = text(cr, sky, 12, PANGO_WEIGHT_NORMAL);
    pango_layout_set_width(l, lw * PANGO_SCALE);
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    set_rgb(cr, m->ink, 0.62);
    cairo_move_to(cr, x, cy + 5);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);

    if (n) {
        gdouble dx = x + lw + 12;
        cairo_rectangle(cr, dx, y + (ISLAND_H - DIVIDER_H) / 2.0, 1, DIVIDER_H);
        set_rgb(cr, m->divider, m->divider_a);
        cairo_fill(cr);
        dx += DIVIDER_W - 8;
        for (i = 0; i < n; ++i) {
            gdouble mx = dx + i * (DAY_W + 4) + DAY_W / 2.0;
            centred_text(cr, days[i].day, 11, PANGO_WEIGHT_NORMAL, mx,
                         y + PAD + 4, m->ink, 0.62);
            glyph_weather(cr, days[i].glyph, mx, cy + 1, m->ink);
            centred_text(cr, days[i].temp, 13, PANGO_WEIGHT_SEMIBOLD, mx,
                         y + ISLAND_H - PAD - 4, m->ink, 1);
        }
    }
    days_free(days, n);
    g_free(temp);
    g_free(sky);
}

static void update_art(void);

static void paint(void)
{
    cairo_surface_t *img;
    cairo_t *cr;
    ObImage pic;
    const Material *m = S.m;
    gint i, y = S.island_y;
    time_t now = time(NULL);
    struct tm tm;
    gchar tbuf[32], dbuf[32];
    gdouble cx;

    if (!S.ready || !S.shown) return;
    update_art();

    img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, S.ww, S.wh);
    cr = cairo_create(img);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

    if (!S.argb && S.bg) {
        cairo_set_source_surface(cr, S.bg, 0, 0);
        cairo_paint(cr);
    } else if (!S.argb) {
        cairo_set_source_rgb(cr, m->body.r, m->body.g, m->body.b);
        cairo_paint(cr);
    }

    /* islands: shadow, material, stroke */
    {
        gint xs[4] = { S.dock_x, S.media_x, S.weather_x, S.status_x };
        gint ws[4] = { S.dock_w, S.media_w, S.weather_w, S.status_w };
        for (i = 0; i < 4; ++i) {
            if (!ws[i]) continue;
            shadow(cr, xs[i], y, ws[i], ISLAND_H, ISLAND_R, 22, 52, -15, m->shadow,
                   m->shadow_mid_a, S.ww, S.wh);
            shadow(cr, xs[i], y, ws[i], ISLAND_H, ISLAND_R, 3, 8, 0, m->shadow,
                   m->shadow_near_a, S.ww, S.wh);
        }
        for (i = 0; i < 4; ++i) {
            if (!ws[i]) continue;
            rrect(cr, xs[i], y, ws[i], ISLAND_H, ISLAND_R);
            if (m->fill.kind == G_SOLID) set_rgb(cr, m->body, 1);
            else fill_source(cr, &m->fill, xs[i], y, ws[i], ISLAND_H, 1);
            cairo_fill(cr);
            rrect(cr, xs[i] + 0.5, y + 0.5, ws[i] - 1, ISLAND_H - 1,
                  ISLAND_R - 0.5);
            cairo_set_line_width(cr, 1);
            set_rgb(cr, m->stroke, m->stroke_a);
            cairo_stroke(cr);
        }
    }

    /* the dock: apps tile, rule, one tile per window */
    {
        gdouble ax = S.dock_x + PAD, ay = y + PAD;
        if (S.hover == 0) {
            rrect(cr, ax, ay, TILE, TILE, TILE_RADIUS);
            hover_source(cr, m, ax, ay);
            cairo_fill(cr);
        }
        glyph_apps(cr, ax + TILE / 2.0, ay + TILE / 2.0, m->ink);

        if (S.tiles->len) {
            gdouble dx = ax + TILE + 8;
            cairo_rectangle(cr, dx, y + (ISLAND_H - DIVIDER_H) / 2.0, 1,
                            DIVIDER_H);
            set_rgb(cr, m->divider, m->divider_a);
            cairo_fill(cr);
        }
        for (i = 0; i < (gint)S.tiles->len; ++i) {
            Tile *t = &g_array_index(S.tiles, Tile, i);
            gdouble tx = t->x, ty = y + PAD;
            gboolean focused = (t->client && t->client == focus_client);

            if (S.hover == i + 1) {
                rrect(cr, tx, ty, TILE, TILE, TILE_RADIUS);
                hover_source(cr, m, tx, ty);
                cairo_fill(cr);
            }
            draw_tile(cr, t, tx, ty, m);
            /* the running mark: 18x3 on the island's lower edge; a pin that
               is not running has none */
            if (!t->client) continue;
            cairo_new_path(cr);
            cairo_move_to(cr, tx + TILE / 2.0 - 9, y + ISLAND_H);
            cairo_line_to(cr, tx + TILE / 2.0 - 9, y + ISLAND_H - 1);
            cairo_arc(cr, tx + TILE / 2.0 - 7, y + ISLAND_H - 1, 2,
                      G_PI, 1.5 * G_PI);
            cairo_line_to(cr, tx + TILE / 2.0 + 7, y + ISLAND_H - 3);
            cairo_arc(cr, tx + TILE / 2.0 + 7, y + ISLAND_H - 1, 2,
                      1.5 * G_PI, 2 * G_PI);
            cairo_line_to(cr, tx + TILE / 2.0 + 9, y + ISLAND_H);
            cairo_close_path(cr);
            if (!focused)
                set_rgb(cr, m->ink, 0.45);
            else if (m->accent_fill.kind != G_SOLID)
                fill_source(cr, &m->accent_fill, tx + TILE / 2.0 - 9,
                            y + ISLAND_H - 3, 18, 3, 1.0);
            else
                set_rgb(cr, m->accent, 1.0);
            cairo_fill(cr);
        }
    }

    if (S.media_w) paint_media(cr, m, y);
    if (S.weather_w) paint_weather(cr, m, y);

    /* the status island: clock, rule, glyphs */
    localtime_r(&now, &tm);
    strftime(tbuf, sizeof tbuf, "%H:%M", &tm);
    strftime(dbuf, sizeof dbuf, "%a %-d %b", &tm);
    cx = S.status_x + PAD + CLOCK_W / 2.0;
    centred_text(cr, tbuf, 21, PANGO_WEIGHT_SEMIBOLD, cx, y + 17, m->ink, 1);
    centred_text(cr, dbuf, 11, PANGO_WEIGHT_MEDIUM, cx, y + ISLAND_H - 17,
                 m->ink, 0.62);
    {
        gdouble dx = S.status_x + PAD + CLOCK_W + 8;
        gint cw = col_w();
        gdouble gx = S.status_x + PAD + CLOCK_W + DIVIDER_W;
        gchar *bt = S.batt_pct >= 0 ?
                    g_strdup_printf("%d%%", S.batt_pct) : NULL;
        gchar *nt = net_text(), *vt = vol_text();
        gdouble gy_text = y + 13, gy_bare = y + (ISLAND_H - GLYPH) / 2.0;

        cairo_rectangle(cr, dx, y + (ISLAND_H - DIVIDER_H) / 2.0, 1, DIVIDER_H);
        set_rgb(cr, m->divider, m->divider_a);
        cairo_fill(cr);
        if (bt) {
            gdouble cx0 = gx + cw / 2.0;
            glyph_battery(cr, cx0 - GLYPH / 2.0, gy_text, m->ink,
                          S.batt_pct, S.batt_charging);
            centred_text(cr, bt, 10, PANGO_WEIGHT_MEDIUM, cx0, y + 38, m->ink,
                         0.7);
            gx += cw;
        }
        glyph_wifi(cr, gx + (cw - GLYPH) / 2.0, nt ? gy_text : gy_bare, m->ink);
        if (nt)
            centred_text(cr, nt, 10, PANGO_WEIGHT_MEDIUM, gx + cw / 2.0, y + 38,
                         m->ink, 0.7);
        gx += cw;
        glyph_volume(cr, gx + (cw - GLYPH) / 2.0, vt ? gy_text : gy_bare,
                     m->ink);
        if (vt)
            centred_text(cr, vt, 10, PANGO_WEIGHT_MEDIUM, gx + cw / 2.0, y + 38,
                         m->ink, 0.7);
        gx += cw;
        glyph_power(cr, gx + (cw - GLYPH) / 2.0, gy_bare, m->ink);
        g_free(bt); g_free(nt); g_free(vt);
    }
    cairo_destroy(cr);

    cairo_surface_flush(img);
    pic.data = (const guint32*)cairo_image_surface_get_data(img);
    pic.width = S.ww;
    pic.height = S.wh;
    pic.stride = cairo_image_surface_get_stride(img);
    ob_backend->canvas_present(S.win, &pic, NULL);
    cairo_surface_destroy(img);
    ob_backend->flush(FALSE);
}

/* ---- window --------------------------------------------------------------- */

static void shape_input(void)
{
    Rect r[4] = {
        { S.dock_x,    S.island_y, S.dock_w,    ISLAND_H },
        { S.media_x,   S.island_y, S.media_w,   ISLAND_H },
        { S.weather_x, S.island_y, S.weather_w, ISLAND_H },
        { S.status_x,  S.island_y, S.status_w,  ISLAND_H }
    };
    gint n = 0, i;
    for (i = 0; i < 4; ++i)
        if (r[i].width) r[n++] = r[i];
    /* the pointer goes through between the islands */
    ob_backend->canvas_set_input(S.win, r, n);
}

static void snapshot(void)
{
    Rect area;
    ObImage im;
    gint y;

    if (S.argb) return;
    if (S.bg) { cairo_surface_destroy(S.bg); S.bg = NULL; }
    RECT_SET(area, S.wx, S.wy, S.ww, S.wh);
    if (!ob_backend->screen_read(&area, &im)) return;
    S.bg = cairo_image_surface_create(CAIRO_FORMAT_RGB24, S.ww, S.wh);
    cairo_surface_flush(S.bg);
    for (y = 0; y < S.wh; ++y)
        memcpy(cairo_image_surface_get_data(S.bg) +
               y * cairo_image_surface_get_stride(S.bg),
               (const guchar*)im.data + y * im.stride, 4 * S.ww);
    cairo_surface_mark_dirty(S.bg);
    g_free((guint32*)im.data);
}

static void place(void)
{
    layout();
    paint_place(S.win, S.wx, S.wy, S.ww, S.wh);
    shape_input();
}

/* above every window */
static void show_raised(void)
{
    guintptr w = S.win;

    ob_backend->restack(OB_SURFACE_NONE, &w, 1);
    ob_backend->canvas_show(S.win, TRUE);
}

static gboolean snap_cb(gpointer data)
{
    S.snap_id = 0;
    snapshot();
    S.ready = TRUE;
    S.shown = !covered_by_fullscreen();
    if (S.shown) show_raised();
    paint();
    return FALSE;
}

/* The wallpaper under the islands is read with our window out of the way. */
static void resnap(guint delay)
{
    if (S.argb) { S.ready = TRUE; return; }
    ob_backend->canvas_show(S.win, FALSE);
    S.shown = FALSE;
    if (S.snap_id) g_source_remove(S.snap_id);
    S.snap_id = g_timeout_add(delay, snap_cb, NULL);
}

static gchar *signature(void)
{
    GString *g = g_string_new(NULL);
    GList *it;
    time_t now = time(NULL);
    struct tm tm;
    gchar buf[16];

    localtime_r(&now, &tm);
    strftime(buf, sizeof buf, "%H:%M", &tm);
    g_string_append(g, buf);
    battery_poll();
    g_string_append_printf(g, "|%d|%d|%u|b:%d%d", covered_by_fullscreen(),
                           S.hover, S.pins_rev, S.batt_pct, S.batt_charging);
    g_string_append_printf(g, "|m:%s|w:%s|n:%s|v:%s",
                           S.media.out ? S.media.out : "",
                           S.weather.out ? S.weather.out : "",
                           S.net.out ? S.net.out : "",
                           S.vol.out ? S.vol.out : "");
    for (it = client_list; it; it = g_list_next(it)) {
        ObClient *c = it->data;
        if (!listed(c)) continue;
        g_string_append_printf(g, "|%p:%d:%d:%p", (void*)c,
                               c == focus_client, c->iconic,
                               (void*)c->icon_set);
    }
    return g_string_free(g, FALSE);
}

static gboolean tick(gpointer data)
{
    gchar *sig;
    gboolean fs;

    if (!S.ready) return TRUE;
    sig = signature();
    if (S.sig && !strcmp(sig, S.sig)) { g_free(sig); return TRUE; }
    g_free(S.sig);
    S.sig = sig;

    fs = covered_by_fullscreen();
    if (fs && S.shown) {
        ob_backend->canvas_show(S.win, FALSE);
        S.shown = FALSE;
    } else if (!fs && !S.shown) {
        S.shown = TRUE;
        place();
        show_raised();
    }
    if (S.shown) {
        gint ow = S.ww, ox = S.wx;
        place();
        if ((ow != S.ww || ox != S.wx) && !S.argb) {
            /* the row changed width, so the picture behind it has to too;
               with alpha there is no picture, only the new row to paint */
            resnap(80);
            return TRUE;
        }
        paint();
    }
    return TRUE;
}

/* A pin's icon: a file, or a name found under the icon theme's folders
   (scalable first, then the sizes, in any context directory). */
static gchar *find_icon_file(const gchar *icon)
{
    static const gchar *sizes[] = { "scalable", "48x48", "64x64", "32x32",
                                    "small", NULL };
    static const gchar *exts[] = { "svg", "png", NULL };
    const gchar * const *dirs = g_get_system_data_dirs();
    gint d, z, e;

    if (!icon || !*icon) return NULL;
    if (g_path_is_absolute(icon))
        return g_file_test(icon, G_FILE_TEST_IS_REGULAR) ?
               g_strdup(icon) : NULL;
    if (!config_shelf_icon_theme || !*config_shelf_icon_theme) return NULL;

    for (d = 0; dirs[d]; ++d)
        for (z = 0; sizes[z]; ++z) {
            gchar *base = g_build_filename(dirs[d], "icons",
                                           config_shelf_icon_theme,
                                           sizes[z], NULL);
            GDir *dir = g_dir_open(base, 0, NULL);
            const gchar *ctx;
            while (dir && (ctx = g_dir_read_name(dir)))
                for (e = 0; exts[e]; ++e) {
                    gchar *f = g_strdup_printf("%s/%s/%s.%s", base, ctx,
                                               icon, exts[e]);
                    if (g_file_test(f, G_FILE_TEST_IS_REGULAR)) {
                        if (dir) g_dir_close(dir);
                        g_free(base);
                        return f;
                    }
                    g_free(f);
                }
            if (dir) g_dir_close(dir);
            g_free(base);
        }
    return NULL;
}

static void load_pin_icons(void)
{
    GSList *it;

    S.pin_icons = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (it = config_shelf_pins; it; it = g_slist_next(it)) {
        ObShelfPin *p = it->data;
        gchar *file = find_icon_file(p->icon);
        RrImage *im = file ? RrImageNewFromName(ob_rr_icons, file) : NULL;
        g_free(file);
        g_hash_table_insert(S.pin_icons, p, im);
    }
}

static void unload_pin_icons(void)
{
    GHashTableIter it;
    gpointer k, v;

    if (!S.pin_icons) return;
    g_hash_table_iter_init(&it, S.pin_icons);
    while (g_hash_table_iter_next(&it, &k, &v))
        if (v) RrImageUnref(v);
    g_hash_table_destroy(S.pin_icons);
    S.pin_icons = NULL;
}

/* ---- the commands behind the islands -------------------------------------- */

typedef struct {
    Feed    *feed;
    GString *buf;
} Run;

static void feed_done(Feed *f, GString *buf)
{
    gchar *out = g_strdup(buf->str);
    if (!g_utf8_validate(out, -1, NULL)) *out = 0;
    g_strstrip(out);
    if (g_strcmp0(f->out, out)) {
        g_free(f->out);
        f->out = out;
    } else {
        g_free(out);
    }
    f->running = FALSE;
}

static gboolean run_read(GIOChannel *ch, GIOCondition cond, gpointer data)
{
    Run *r = data;
    gchar chunk[512];
    gsize got;
    gboolean eof = FALSE;

    /* drain what is there first: the pipe can say HUP with data still in it */
    for (;;) {
        GIOStatus st = g_io_channel_read_chars(ch, chunk, sizeof chunk, &got,
                                               NULL);
        if (st == G_IO_STATUS_NORMAL) {
            g_string_append_len(r->buf, chunk, got);
            if (r->buf->len > 4096) { eof = TRUE; break; }
        } else {
            if (st == G_IO_STATUS_EOF || st == G_IO_STATUS_ERROR) eof = TRUE;
            break;
        }
    }
    if (!eof) return TRUE;

    /* S is zeroed when the shelf shuts down: the feed is then not ours */
    if (S.win && (r->feed == &S.media || r->feed == &S.weather ||
                  r->feed == &S.net || r->feed == &S.vol))
        feed_done(r->feed, r->buf);
    g_string_free(r->buf, TRUE);
    g_io_channel_shutdown(ch, FALSE, NULL);
    g_slice_free(Run, r);
    return FALSE;
}

/* Runs the feed's command without waiting for it (a slow one must not stall
   the window manager); Openbox reaps the child. */
static gboolean refresh_feed(Feed *f)
{
    gchar *argv[] = { "/bin/sh", "-c", (gchar*)f->command, NULL };
    gint out;
    GIOChannel *ch;
    Run *r;

    if (f->running || !f->command) return TRUE;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL,
                                  NULL, NULL, NULL, NULL, &out, NULL, NULL))
        return TRUE;
    f->running = TRUE;
    r = g_slice_new0(Run);
    r->feed = f;
    r->buf = g_string_new(NULL);
    ch = g_io_channel_unix_new(out);
    g_io_channel_set_close_on_unref(ch, TRUE);
    g_io_channel_set_encoding(ch, NULL, NULL);
    g_io_channel_set_flags(ch, G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, run_read, r);
    g_io_channel_unref(ch);
    return TRUE;
}

static gboolean feed_timer(gpointer data)
{
    return refresh_feed(data);
}

static void start_feeds(void)
{
    if (config_shelf_media) {
        S.has_media = TRUE;
        S.media.command = config_shelf_media->command;
        S.media.interval = config_shelf_media->interval;
        refresh_feed(&S.media);
        S.media.timer = g_timeout_add_seconds(S.media.interval, feed_timer,
                                              &S.media);
    }
    if (config_shelf_network && config_shelf_network->list) {
        S.net.command = config_shelf_network->list;
        S.net.interval = 10;
        refresh_feed(&S.net);
        S.net.timer = g_timeout_add_seconds(S.net.interval, feed_timer, &S.net);
    }
    if (config_shelf_volume && config_shelf_volume->level) {
        S.vol.command = config_shelf_volume->level;
        S.vol.interval = 3;
        refresh_feed(&S.vol);
        S.vol.timer = g_timeout_add_seconds(S.vol.interval, feed_timer, &S.vol);
    }
    if (config_shelf_weather) {
        S.has_weather = TRUE;
        S.weather.command = config_shelf_weather->command;
        S.weather.interval = config_shelf_weather->interval;
        refresh_feed(&S.weather);
        S.weather.timer = g_timeout_add_seconds(S.weather.interval,
                                                feed_timer, &S.weather);
    }
}

static void stop_feeds(void)
{
    if (S.media.timer) g_source_remove(S.media.timer);
    if (S.weather.timer) g_source_remove(S.weather.timer);
    if (S.net.timer) g_source_remove(S.net.timer);
    if (S.vol.timer) g_source_remove(S.vol.timer);
    g_free(S.net.out);
    g_free(S.vol.out);
    g_free(S.media.out);
    g_free(S.weather.out);
    if (S.art) RrImageUnref(S.art);
    g_free(S.art_file);
}

/* The cover the player printed (a file), loaded when it changes */
static void update_art(void)
{
    gchar *file = media_shown() ? feed_line(&S.media, 3) : g_strdup("");
    if (g_strcmp0(file, S.art_file ? S.art_file : "")) {
        if (S.art) RrImageUnref(S.art);
        S.art = NULL;
        g_free(S.art_file);
        S.art_file = file;
        if (*file && g_file_test(file, G_FILE_TEST_IS_REGULAR))
            S.art = RrImageNewFromName(ob_rr_icons, file);
    } else {
        g_free(file);
    }
}

static void launch(const gchar *command);

static gboolean refresh_status_later(gpointer data)
{
    if (S.net.command) refresh_feed(&S.net);
    if (S.vol.command) refresh_feed(&S.vol);
    return FALSE;
}

static gboolean refresh_media_later(gpointer data)
{
    if (S.has_media) refresh_feed(&S.media);
    return FALSE;
}

static void launch(const gchar *command)
{
    gchar *argv[] = { "/bin/sh", "-c", (gchar*)command, NULL };
    GError *err = NULL;

    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH |
                       G_SPAWN_STDOUT_TO_DEV_NULL |
                       G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, &err)) {
        g_message("Failed to run shelf pin \"%s\": %s", command,
                  err->message);
        g_error_free(err);
    }
}

void shelf_startup(gboolean reconfig)
{
    memset(&S, 0, sizeof S);
    S.hover = -1;
    STRUT_PARTIAL_SET(shelf_strut, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (!config_shelf_enabled) return;

    if (config_shelf_material &&
        !g_ascii_strcasecmp(config_shelf_material, "light"))
        S.m = &mat_light;
    else if (config_shelf_material &&
             !g_ascii_strcasecmp(config_shelf_material, "dark"))
        S.m = &mat_dark;
    else
        S.m = theme_material();
    S.m = override_material(S.m);
    if (g_getenv("OB_SHELF_DEBUG")) dump_material(S.m);

    /* drawn with real alpha when a compositor runs, else over a copy of the
       wallpaper */
    S.win = (ObWindowId)ob_backend->canvas_new(OB_CANVAS_PANEL, OB_SURFACE_NONE,
                                           OB_CANVAS_INPUT_BUTTONS |
                                           OB_CANVAS_INPUT_MOTION |
                                           OB_CANVAS_INPUT_CROSSING |
                                           OB_CANVAS_ALPHA);
    S.argb = ob_backend->canvas_has_alpha(S.win);
    load_pin_icons();
    start_feeds();
    layout();
    paint_place(S.win, S.wx, S.wy, S.ww, S.wh);
    shape_input();
    screen_update_areas();
    resnap(1200);
    if (S.argb) snap_cb(NULL);
    S.tick_id = g_timeout_add(TICK_MS, tick, NULL);
}

void shelf_shutdown(gboolean reconfig)
{
    if (!S.win) return;
    if (S.tick_id) g_source_remove(S.tick_id);
    if (S.snap_id) g_source_remove(S.snap_id);
    ob_backend->canvas_free(S.win);
    if (S.bg) cairo_surface_destroy(S.bg);
    if (S.tiles) g_array_free(S.tiles, TRUE);
    if (sys_cmds) { g_ptr_array_free(sys_cmds, TRUE); sys_cmds = NULL; }
    stop_feeds();
    unload_pin_icons();
    g_free(S.sig);
    STRUT_PARTIAL_SET(shelf_strut, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    memset(&S, 0, sizeof S);
}

/* ---- events --------------------------------------------------------------- */

static gint hit(gint x, gint y)
{
    gint i;
    if (S.media_w && y >= S.island_y + (ISLAND_H - BTN) / 2 &&
        y < S.island_y + (ISLAND_H + BTN) / 2) {
        gint bx = S.media_x + PAD + ART + 12 + S.text_w + 16;
        for (i = 0; i < 3; ++i)
            if (x >= bx + i * (BTN + BTN_GAP) &&
                x < bx + i * (BTN + BTN_GAP) + BTN)
                return HIT_PREV - i;
    }
    if (y >= S.island_y + PAD && y < S.island_y + PAD + TILE) {
        gint cw = col_w();
        gint gx = S.status_x + PAD + CLOCK_W + DIVIDER_W;
        if (S.batt_pct >= 0) {
            if (x >= gx && x < gx + cw) return HIT_BATT;
            gx += cw;
        }
        for (i = 0; i < 3; ++i)
            if (x >= gx + i * cw && x < gx + (i + 1) * cw)
                return HIT_NET - i;
    }
    if (y < S.island_y + PAD || y >= S.island_y + PAD + TILE) return -1;
    if (x >= S.dock_x + PAD && x < S.dock_x + PAD + TILE) return 0;
    for (i = 0; i < (gint)S.tiles->len; ++i) {
        Tile *t = &g_array_index(S.tiles, Tile, i);
        if (x >= t->x && x < t->x + TILE) return i + 1;
    }
    return -1;
}

/* The menu is not "user positioned": Openbox then keeps it on screen, which
   from the bottom edge means opening upwards. */
static void show_menu(const gchar *name, gint rx, gint ry, ObClient *c)
{
    GravityPoint pos = { { 0, }, };
    pos.x.pos = rx;
    pos.y.pos = ry;
    /* opened by the pointer: nothing is selected, so a menu's info line is
       not shown as if it were picked */
    menu_show((gchar*)name, &pos, screen_monitor_primary(FALSE), TRUE, FALSE,
              c);
}

/* ---- pinning from the dock ------------------------------------------------ */

#define SHELF_MENU_NAME "shelf-menu"
enum { SM_PIN = 1, SM_UNPIN, SM_WINDOW };
#define SM_COMMAND  100   /* ids from here run sys_cmds[id - SM_COMMAND] */

static ObMenu *shelf_menu;
static ObShelfPin *menu_pin;     /* the tile the menu was opened on */
static ObClient   *menu_client;

/* One pin per line: name|icon|command|class. '|' and newlines can not be
   part of a field. */
static void clean_field(GString *g, const gchar *f)
{
    for (; f && *f; ++f)
        g_string_append_c(g, *f == '|' || *f == '\n' ? ' ' : *f);
}

static void save_pins(void)
{
    GString *g;
    GSList *it;
    gchar *dir;

    if (!config_shelf_pins_file || !*config_shelf_pins_file) return;
    g = g_string_new("# name|icon|command|class, written by the shelf\n");
    for (it = config_shelf_pins; it; it = g_slist_next(it)) {
        ObShelfPin *p = it->data;
        clean_field(g, p->name);    g_string_append_c(g, '|');
        clean_field(g, p->icon);    g_string_append_c(g, '|');
        clean_field(g, p->command); g_string_append_c(g, '|');
        clean_field(g, p->wmclass); g_string_append_c(g, '\n');
    }
    dir = g_path_get_dirname(config_shelf_pins_file);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    if (!g_file_set_contents(config_shelf_pins_file, g->str, g->len, NULL))
        g_message("Failed to write the shelf pins to %s",
                  config_shelf_pins_file);
    g_string_free(g, TRUE);
}

static void pins_changed(void)
{
    save_pins();
    S.pins_rev++;
    g_free(S.sig);
    S.sig = NULL;   /* repaint on the next tick */
}

/* The window's command line, to start it again: every argument, quoted. The
   first one alone when the line is long or the process is gone. */
static gchar *client_command(const ObClient *c, gchar **exe)
{
    gchar *path = g_strdup_printf("/proc/%d/cmdline", (gint)c->pid);
    gchar *data = NULL;
    gsize len = 0, off;
    GString *cmd = NULL;

    if (c->pid > 0 && g_file_get_contents(path, &data, &len, NULL) && len &&
        *data) {
        cmd = g_string_new(NULL);
        *exe = g_strdup(data);
        for (off = 0; off < len; off += strlen(data + off) + 1) {
            gchar *q = g_shell_quote(data + off);
            if (cmd->len) {
                if (cmd->len + strlen(q) > 480) { g_free(q); break; }
                g_string_append_c(cmd, ' ');
            }
            g_string_append(cmd, q);
            g_free(q);
        }
    }
    g_free(data);
    g_free(path);
    if (!cmd) {
        gchar *low = g_ascii_strdown(c->class ? c->class : c->name, -1);
        *exe = g_strdup(low);
        return low;
    }
    return g_string_free(cmd, FALSE);
}

static void pin_client(ObClient *c)
{
    ObShelfPin *p;
    gchar *low, *base, *file, *exe = NULL;

    if (!c || pinned(c)) return;
    p = g_slice_new0(ObShelfPin);
    p->command = client_command(c, &exe);
    p->wmclass = g_strdup(c->class ? c->class : c->name);
    p->name = g_strdup(p->wmclass);
    low = g_ascii_strdown(p->wmclass, -1);
    base = g_path_get_basename(exe);
    g_free(exe);
    /* the icon is the first of the class and the command that the theme has */
    file = find_icon_file(low);
    if (file) p->icon = g_strdup(low);
    else {
        g_free(file);
        file = find_icon_file(base);
        p->icon = g_strdup(file ? base : low);
    }
    g_free(file);
    g_free(low);
    g_free(base);
    config_shelf_pins = g_slist_append(config_shelf_pins, p);
    {
        gchar *f = find_icon_file(p->icon);
        RrImage *im = f ? RrImageNewFromName(ob_rr_icons, f) : NULL;
        g_free(f);
        g_hash_table_insert(S.pin_icons, p, im);
    }
    pins_changed();
}

static void unpin(ObShelfPin *p)
{
    RrImage *im;

    if (!p || !g_slist_find(config_shelf_pins, p)) return;
    config_shelf_pins = g_slist_remove(config_shelf_pins, p);
    if ((im = g_hash_table_lookup(S.pin_icons, p))) RrImageUnref(im);
    g_hash_table_remove(S.pin_icons, p);
    g_free(p->name);
    g_free(p->icon);
    g_free(p->command);
    g_free(p->wmclass);
    g_slice_free(ObShelfPin, p);
    pins_changed();
}

static void shelf_menu_execute(ObMenuEntry *e, ObMenuFrame *f, ObClient *c,
                               guint state, gpointer data)
{
    if (!S.win) return;
    switch (e->id) {
    case SM_PIN:   pin_client(menu_client); break;
    case SM_UNPIN: unpin(menu_pin); break;
    default:
        if (e->id >= SM_COMMAND && sys_cmds &&
            (guint)(e->id - SM_COMMAND) < sys_cmds->len)
        {
            launch(g_ptr_array_index(sys_cmds, e->id - SM_COMMAND));
            g_timeout_add(700, refresh_status_later, NULL);
        }
    }
    menu_pin = NULL;
    menu_client = NULL;
    menu_frame_hide_all();
}

/* Above the tile that was clicked: below it would be under the shelf */
static void shelf_menu_place(ObMenuFrame *frame, gint *x, gint *y,
                             gboolean mouse, gpointer data)
{
    gint dx, dy;

    *y -= frame->area.height;
    menu_frame_move_on_screen(frame, *x, *y, &dx, &dy);
    *x += dx;
    *y += dy;
}

gint shelf_menu_limit(void)
{
    if (!S.win || !S.shown) return -1;
    return S.wy + S.island_y - 8;
}

/* the text of a menu entry, in the theme's font, laid out in a box */
static void menu_text(cairo_t *cr, const gchar *text, const RrFont *font,
                      RrJustify justify, gdouble x, gdouble y, gdouble w,
                      gdouble h, Rgb c, gdouble alpha)
{
    PangoLayout *l;
    gint lw, lh;

    if (!text || !*text || w <= 0) return;
    l = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(l, font->font_desc);
    pango_layout_set_text(l, text, -1);
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(l, (gint)(w * PANGO_SCALE));
    pango_layout_set_alignment(l, justify == RR_JUSTIFY_CENTER ?
                               PANGO_ALIGN_CENTER : justify == RR_JUSTIFY_RIGHT ?
                               PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    pango_layout_get_pixel_size(l, &lw, &lh);
    set_rgb(cr, c, alpha);
    cairo_move_to(cr, x, y + floor((h - lh) / 2.0));
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

/* a 1-bit mask of the theme (the client menu's glyphs), centred in a box */
static void menu_mask(cairo_t *cr, const RrPixmapMask *m, gdouble x,
                      gdouble y, gdouble size, Rgb c, gdouble alpha)
{
    gint i, j, stride;
    gdouble ox, oy;

    if (!m || !m->data) return;
    stride = (m->width + 7) / 8;
    ox = floor(x + (size - m->width) / 2.0);
    oy = floor(y + (size - m->height) / 2.0);
    set_rgb(cr, c, alpha);
    for (j = 0; j < m->height; ++j)
        for (i = 0; i < m->width; ++i)
            if (m->data[j * stride + i / 8] & (1 << (i % 8)))
                cairo_rectangle(cr, ox + i, oy + j, 1, 1);
    cairo_fill(cr);
}

void shelf_menu_paint(ObMenuFrame *f)
{
    const RrTheme *t = ob_rr_theme;
    gint w = f->area.width, h = f->area.height, b = t->mbwidth;
    gint s = f->shadow, ww = w + 2 * s, wh = h + 2 * s;
    gint pad = MENU_ITEM_PADDING;
    gdouble r = MIN(config_shelf_menu_radius, MIN(w, h) / 2.0);
    gdouble ir = CLAMP(r - config_shelf_menu_padding, 4, 8);
    Fill bg = rr_fill(t->a_menu), sel = rr_fill(t->a_menu_selected);
    Rgb bgm = fill_mean(&bg), selm = fill_mean(&sel);
    Rgb ink = readable(rr_rgb(t->menu_color), bgm, 4.5);
    cairo_surface_t *dst;
    ObImage img;
    cairo_t *cr;
    GList *it;

    if (w <= 0 || h <= 0) return;
    dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ww, wh);
    cr = cairo_create(dst);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* a soft shadow like the islands': a wide one and a close one */
    if (s) {
        shadow(cr, s, s, w, h, r, 8, 22, -4, mat_dark.shadow, 0.34, ww, wh);
        shadow(cr, s, s, w, h, r, 1, 4, 0, mat_dark.shadow, 0.22, ww, wh);
    }
    cairo_translate(cr, s, s);

    /* the box: the border, then the background inside it; the background
       has the size of the items' own, so gradients line up */
    if (b > 0) {
        rrect(cr, 0, 0, w, h, r);
        set_rgb(cr, rr_rgb(t->menu_border_color), 1);
        cairo_fill(cr);
    }
    rrect(cr, b, b, w - 2 * b, h - 2 * b, MAX(r - b, 0));
    fill_source(cr, &bg, b, b, w - 2 * b, h - 2 * b, 1);
    cairo_fill(cr);
    if (b == 0) {
        rrect(cr, 0.5, 0.5, w - 1, h - 1, MAX(r - 0.5, 0));
        cairo_set_line_width(cr, 1);
        set_rgb(cr, ink, 0.12);
        cairo_stroke(cr);
    }

    for (it = f->entries; it; it = g_list_next(it)) {
        ObMenuEntryFrame *e = it->data;
        ObMenuEntry *me = e->entry;
        gdouble ex = b + e->area.x, ey = b + e->area.y;
        gdouble ew = e->area.width, eh = e->area.height;
        gboolean selected = e == f->selected, enabled = TRUE;
        const gchar *label = NULL;
        Rgb c;
        gdouble isz, calpha;

        if (me->type == OB_MENU_ENTRY_TYPE_SEPARATOR) {
            if (me->data.separator.label) {
                /* a title: quiet text on the menu, not a box */
                menu_text(cr, me->data.separator.label, t->menu_title_font,
                          t->a_menu_text_title->texture[0].data.text.justify,
                          ex + pad + 2, ey, ew - 2 * pad - 4, eh, ink, 0.55);
            } else {
                cairo_rectangle(cr, ex + pad + 2, floor(ey + eh / 2.0),
                                ew - 2 * pad - 4, 1);
                set_rgb(cr, ink, 0.14);
                cairo_fill(cr);
            }
            continue;
        }

        if (me->type == OB_MENU_ENTRY_TYPE_NORMAL) {
            enabled = me->data.normal.enabled;
            label = me->data.normal.label;
        } else if (me->data.submenu.submenu) {
            label = me->data.submenu.submenu->title;
        }
        if (selected && enabled) {
            rrect(cr, ex, ey, ew, eh, MIN(ir, eh / 2.0));
            fill_source(cr, &sel, ex, ey, ew, eh, 1);
            cairo_fill(cr);
        }
        /* the theme's colours, as long as they read on what is under them;
           a disabled entry that would vanish is the text, dimmed */
        {
            gdouble alpha = 1;
            if (!enabled) {
                c = rr_rgb(t->menu_disabled_color);
                if (contrast(c, bgm) < 3.0) { c = ink; alpha = 0.45; }
            } else if (selected) {
                c = readable(rr_rgb(t->menu_selected_color), selm, 4.5);
            } else {
                c = ink;
            }
            calpha = alpha;
        }

        isz = eh - f->item_margin.top - f->item_margin.bottom;
        if (me->data.normal.icon) {
            cairo_surface_t *ic = icon_surface(me->data.normal.icon);
            if (ic) {
                gint iw = cairo_image_surface_get_width(ic);
                gint ih = cairo_image_surface_get_height(ic);
                cairo_save(cr);
                cairo_translate(cr, ex + pad, ey + f->item_margin.top);
                cairo_scale(cr, isz / iw, isz / ih);
                cairo_set_source_surface(cr, ic, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr),
                                         CAIRO_FILTER_BEST);
                cairo_paint_with_alpha(cr, enabled ?
                                       me->data.normal.icon_alpha / 255.0 :
                                       0.5);
                cairo_restore(cr);
                cairo_surface_destroy(ic);
            }
        } else if (me->type == OB_MENU_ENTRY_TYPE_NORMAL &&
                   me->data.normal.mask) {
            menu_mask(cr, me->data.normal.mask, ex + pad,
                      ey + f->item_margin.top, isz, c, calpha);
        }

        menu_text(cr, label, t->menu_font, RR_JUSTIFY_LEFT, ex + f->text_x,
                  ey, f->text_w - (me->type == OB_MENU_ENTRY_TYPE_SUBMENU ?
                                   eh : 0), eh, c, calpha);

        if (me->type == OB_MENU_ENTRY_TYPE_SUBMENU) {
            /* the arrow: a small chevron at the right end */
            gdouble ax = ex + ew - pad - 6, ay = ey + eh / 2.0;
            cairo_new_path(cr);
            cairo_move_to(cr, ax - 2.5, ay - 4.5);
            cairo_line_to(cr, ax + 2, ay);
            cairo_line_to(cr, ax - 2.5, ay + 4.5);
            cairo_set_line_width(cr, 1.6);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            set_rgb(cr, c, 0.8 * calpha);
            cairo_stroke(cr);
        }
    }

    cairo_destroy(cr);
    cairo_surface_flush(dst);
    img.data = (const guint32*)cairo_image_surface_get_data(dst);
    img.width = ww;
    img.height = wh;
    img.stride = cairo_image_surface_get_stride(dst);
    ob_backend->canvas_present(f->window, &img, NULL);
    cairo_surface_destroy(dst);
}

void shelf_menu_startup(void)
{
    shelf_menu = menu_new(SHELF_MENU_NAME, "Dock", FALSE, NULL);
    menu_set_execute_func(shelf_menu, shelf_menu_execute);
    menu_set_place_func(shelf_menu, shelf_menu_place);
}

/* ---- the status glyphs' menus --------------------------------------------- */

static void sys_menu_begin(void)
{
    menu_clear_entries(shelf_menu);
    menu_pin = NULL;
    menu_client = NULL;
    if (sys_cmds) g_ptr_array_free(sys_cmds, TRUE);
    sys_cmds = g_ptr_array_new_with_free_func(g_free);
}

static void sys_menu_add(const gchar *label, const gchar *command,
                         gboolean enabled)
{
    ObMenuEntry *e;
    gint id = SM_COMMAND + sys_cmds->len;

    g_ptr_array_add(sys_cmds, g_strdup(command ? command : ""));
    e = menu_add_normal(shelf_menu, id, label, NULL, FALSE);
    e->data.normal.enabled = enabled && command;
}

/* A line that only informs: a title, never a selectable entry */
static void sys_menu_info(const gchar *label)
{
    menu_add_separator(shelf_menu, -1, label);
}

static gchar *replace_all(const gchar *s, const gchar *what, const gchar *with)
{
    gchar **parts = g_strsplit(s, what, -1);
    gchar *r = g_strjoinv(with, parts);
    g_strfreev(parts);
    return r;
}

/* "in-use:name" lines; a ':' in the name is escaped with a backslash */
static void network_menu(void)
{
    gchar *argv[] = { "/bin/sh", "-c", config_shelf_network->list, NULL };
    gchar *out = NULL, **lines;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             NULL);
    gint i, n = 0;

    g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                 &out, NULL, NULL, NULL);
    lines = g_strsplit(out ? out : "", "\n", -1);
    for (i = 0; lines[i] && n < 20; ++i) {
        gchar *colon = strchr(lines[i], ':');
        gboolean active;
        GString *name;
        gchar *c, *label, *cmd = NULL, *q, *signal = NULL, *secure = NULL;

        if (!colon || !colon[1]) continue;
        active = lines[i][0] == '*';
        name = g_string_new(NULL);
        c = colon + 1;
        /* "in-use:signal:name" when the command prints the signal too */
        {
            gchar *e = c;
            while (g_ascii_isdigit(*e)) ++e;
            if (e > c && *e == ':' && e[1]) {
                signal = g_strndup(c, e - c);
                c = e + 1;
            }
        }
        /* a ':' that is not escaped now ends the security column */
        {
            gchar *e = c;
            while (*e && *e != ':') { if (*e == '\\' && e[1]) ++e; ++e; }
            if (signal && *e == ':' && e[1]) {
                secure = g_strndup(c, e - c);
                c = e + 1;
            }
        }
        for (; *c; ++c) {
            if (*c == '\\' && c[1]) ++c;
            g_string_append_c(name, *c);
        }
        if (g_hash_table_contains(seen, name->str)) {
            g_string_free(name, TRUE);
            g_free(signal);
            g_free(secure);
            continue;
        }
        g_hash_table_add(seen, g_strdup(name->str));
        label = g_strdup_printf("%s%s%s%s%s", active ? "\342\234\223 " : "",
                                name->str, signal ? "   " : "",
                                signal ? signal : "", signal ? "%" : "");
        g_free(signal);
        if (config_shelf_network->connect && !active) {
            gboolean locked = secure && *secure && strcmp(secure, "--") &&
                              config_shelf_network->secured &&
                              config_shelf_network->password;
            q = g_shell_quote(name->str);
            if (locked) {
                /* ask first; the answer reaches the command as "$pw" */
                gchar *t = replace_all(config_shelf_network->secured, "%p",
                                       "\"$pw\""), *c2 = replace_all(t, "%s", q);
                cmd = g_strdup_printf("pw=$(%s) || exit 0; %s",
                                      config_shelf_network->password, c2);
                g_free(t);
                g_free(c2);
            } else {
                cmd = replace_all(config_shelf_network->connect, "%s", q);
            }
            g_free(q);
        }
        g_free(secure);
        sys_menu_add(label, cmd, TRUE);
        g_free(label);
        g_free(cmd);
        g_string_free(name, TRUE);
        ++n;
    }
    if (!n) sys_menu_info("No networks found");
    g_strfreev(lines);
    g_free(out);
    g_hash_table_destroy(seen);
}

/* Level (when the config can print it) and the steps of the volume */
static void volume_menu(void)
{
    ObShelfVolume *v = config_shelf_volume;

    if (v->level) {
        gchar *argv[] = { "/bin/sh", "-c", v->level, NULL };
        gchar *out = NULL, *l;
        g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                     &out, NULL, NULL, NULL);
        if (out) g_strstrip(out);
        l = g_strdup_printf("Volume %s", out && *out ? out : "unknown");
        sys_menu_info(l);
        g_free(l);
        g_free(out);
    }
    if (v->up) sys_menu_add("Louder", v->up, TRUE);
    if (v->down) sys_menu_add("Quieter", v->down, TRUE);
    if (v->toggle) sys_menu_add("Mute / unmute", v->toggle, TRUE);
}

static void show_status_menu(gint h, gint rx, gint ry)
{
    GSList *it;

    if (!shelf_menu) return;
    if (h == HIT_NET && config_shelf_network) {
        sys_menu_begin();
        network_menu();
    } else if (h == HIT_VOL && config_shelf_volume) {
        sys_menu_begin();
        volume_menu();
    } else if (h == HIT_POWER && config_shelf_power) {
        sys_menu_begin();
        for (it = config_shelf_power; it; it = g_slist_next(it)) {
            ObShelfPower *p = it->data;
            sys_menu_add(p->name, p->command, TRUE);
        }
    } else if (h == HIT_BATT && S.batt_pct >= 0) {
        gchar *l;
        sys_menu_begin();
        l = g_strdup_printf("%d%%, %s", S.batt_pct,
                            S.batt_charging ? "charging" : "on battery");
        sys_menu_info(l);
        g_free(l);
    } else {
        return;
    }
    show_menu(SHELF_MENU_NAME, rx, ry, NULL);
}

static void show_tile_menu(Tile *t, gint rx, gint ry)
{
    if (!shelf_menu) return;
    menu_clear_entries(shelf_menu);
    menu_pin = t->pin;
    menu_client = t->client;
    if (t->pin)
        menu_add_normal(shelf_menu, SM_UNPIN, "Unpin from dock", NULL, FALSE);
    else
        menu_add_normal(shelf_menu, SM_PIN, "Pin to dock", NULL, FALSE);
    if (t->client) {
        menu_add_separator(shelf_menu, -1, NULL);
        menu_add_submenu(shelf_menu, SM_WINDOW, "client-menu");
    }
    show_menu(SHELF_MENU_NAME, rx, ry, t->client);
}

/* Left click on a tile: start it, or focus it, or minimise it */
static void tile_click(Tile *t)
{
    ObClient *c = t->client;

    if (!c) {
        if (t->pin) launch(t->pin->command);
        return;
    }
    if (!g_list_find(client_list, c)) return;
    if (c == focus_client && !c->iconic)
        client_iconify(c, TRUE, TRUE, TRUE);
    else
        client_activate(c, TRUE, TRUE, TRUE, TRUE, TRUE);
}

/* Drops a dragged pin where another one is */
static void move_pin(gint from, gint to)
{
    Tile *a = &g_array_index(S.tiles, Tile, from - 1);
    Tile *b = &g_array_index(S.tiles, Tile, to - 1);
    ObShelfPin *p = a->pin;
    gint at;

    if (!p || !b->pin || p == b->pin) return;
    at = g_slist_index(config_shelf_pins, b->pin);
    config_shelf_pins = g_slist_remove(config_shelf_pins, p);
    config_shelf_pins = g_slist_insert(config_shelf_pins, p, at);
    pins_changed();
}

gboolean shelf_input(const ObInputEvent *e)
{
    gint h;

    if (!S.win || e->over != (guintptr)S.win) return FALSE;

    switch (e->type) {
    case OB_INPUT_MOTION:
        h = hit(e->x, e->y);
        if (S.press && ABS(e->x - S.press_x) > 6) S.dragging = TRUE;
        if (h != S.hover) { S.hover = h; paint(); }
        break;
    case OB_INPUT_BUTTON_RELEASE:
        if (e->button == 1 && S.press) {
            gint from = S.press;
            S.press = 0;
            h = hit(e->x, e->y);
            if (!S.dragging) {
                if (h == from) tile_click(&g_array_index(S.tiles, Tile, from - 1));
            } else if (h > 0) {
                move_pin(from, h);
            }
            S.dragging = FALSE;
        }
        break;
    case OB_INPUT_LEAVE:
        if (S.hover != -1) { S.hover = -1; paint(); }
        break;
    case OB_INPUT_BUTTON_PRESS:
        h = hit(e->x, e->y);
        if (h == 0 && e->button == 1) {
            show_menu("root-menu", S.wx + S.dock_x,
                      S.wy + S.island_y - MENU_GAP, NULL);
        } else if (h <= HIT_NET && h >= HIT_BATT) {
            guint b = e->button;
            if (h == HIT_VOL && config_shelf_volume) {
                const gchar *cmd = b == 2 ? config_shelf_volume->toggle :
                                   b == 4 ? config_shelf_volume->up :
                                   b == 5 ? config_shelf_volume->down : NULL;
                if (cmd) {
                    launch(cmd);
                    g_timeout_add(200, refresh_status_later, NULL);
                } else if (b == 1)
                    show_status_menu(h, e->root_x,
                                     S.wy + S.island_y - MENU_GAP);
            } else if (b == 1) {
                show_status_menu(h, e->root_x,
                                 S.wy + S.island_y - MENU_GAP);
            }
        } else if (h <= HIT_PREV && e->button == 1) {
            const gchar *cmd = h == HIT_PREV ? config_shelf_media->previous :
                               h == HIT_TOGGLE ? config_shelf_media->toggle :
                               config_shelf_media->next;
            if (cmd) {
                launch(cmd);
                /* show what the click changed (the track, play or pause) */
                g_timeout_add(600, refresh_media_later, NULL);
            }
        } else if (h > 0) {
            Tile *t = &g_array_index(S.tiles, Tile, h - 1);
            if (e->button == 3) {
                show_tile_menu(t, S.wx + t->x, S.wy + S.island_y - MENU_GAP);
            } else if (e->button == 1) {
                /* acts on release: a drag moves a pin instead */
                S.press = h;
                S.press_x = e->x;
                S.dragging = FALSE;
            }
        }
        break;
    default:
        break;
    }
    return TRUE;
}
