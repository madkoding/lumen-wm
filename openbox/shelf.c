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
#include "screen.h"
#include "obt/display.h"
#include "obrender/render.h"

#include <X11/extensions/shape.h>
#include <cairo.h>
#include <cairo-xlib.h>
#include <pango/pangocairo.h>
#include <math.h>
#include <string.h>
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
#define CONTROLS_W  (3 * GLYPH + 2 * GLYPH_GAP)
/* Room the shadow needs left and right of the islands, and above them */
#define SIDE        24
#define LIP         2
#define TICK_MS     250

StrutPartial shelf_strut;

typedef struct {
    gdouble r, g, b;
} Rgb;

typedef struct {
    gboolean light;
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
} Material;

#define RGB(r, g, b) { (r) / 255.0, (g) / 255.0, (b) / 255.0 }

static const Material mat_dark = {
    FALSE, RGB(0x21, 0x25, 0x2b), RGB(0xe7, 0xe9, 0xe8),
    0.045, RGB(255, 255, 255), RGB(9, 11, 14), 0.38, 0.72,
    0.10, RGB(255, 255, 255), 0.22, RGB(255, 255, 255)
};
static const Material mat_light = {
    TRUE, RGB(0xec, 0xee, 0xf1), RGB(0x2b, 0x30, 0x36),
    0.82, RGB(255, 255, 255), RGB(20, 22, 26), 0.14, 0.40,
    0.07, RGB(25, 27, 31), 0.22, RGB(30, 34, 39)
};

typedef struct {
    ObClient *client;
    gint      x;         /* in window coordinates */
} Tile;

static struct {
    Window           win;
    gboolean         argb;       /* a compositor is running */
    gboolean         shown;
    Visual          *visual;
    Colormap         cmap;
    gint             wx, wy, ww, wh;
    gint             dock_x, dock_w, status_x, status_w, island_y;
    GArray          *tiles;      /* Tile, the dock's app tiles */
    gint             hover;      /* -1 nothing, 0 the apps tile, n the nth app */
    gchar           *sig;
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
                   gdouble dy, gdouble blur, gdouble spread,
                   Rgb col, gdouble alpha, gint ww, gint wh)
{
    cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, ww, wh);
    cairo_t *mc = cairo_create(mask);
    gint stride = cairo_image_surface_get_stride(mask);
    guint8 *data, *packed;
    gint yy;

    cairo_set_source_rgba(mc, 0, 0, 0, 1);
    rrect(mc, x - spread, y + dy - spread, w + 2 * spread, h + 2 * spread,
          MAX(0, ISLAND_R + spread));
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
static cairo_surface_t *icon_surface(ObClient *c)
{
    RrImagePic *best = NULL;
    RrImageSet *set;
    cairo_surface_t *s;
    guint32 *dst;
    gint i, n;

    if (!c->icon_set || !(set = c->icon_set->set)) return NULL;
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

static void draw_tile(cairo_t *cr, ObClient *c, gdouble x, gdouble y,
                      const Material *m)
{
    cairo_surface_t *ic = icon_surface(c);

    cairo_save(cr);
    rrect(cr, x, y, TILE, TILE, TILE_RADIUS);
    cairo_clip(cr);
    if (ic) {
        gint w = cairo_image_surface_get_width(ic);
        gint h = cairo_image_surface_get_height(ic);
        cairo_scale(cr, (gdouble)TILE / w, (gdouble)TILE / h);
        cairo_set_source_surface(cr, ic, x * w / TILE, y * h / TILE);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
        cairo_paint_with_alpha(cr, c->iconic ? 0.5 : 1.0);
    } else {
        gchar init[8] = "?";
        const gchar *name = c->class && *c->class ? c->class : c->name;
        if (name && *name)
            g_unichar_to_utf8(g_unichar_toupper(g_utf8_get_char(name)), init),
            init[g_utf8_next_char(name) - name] = 0;
        cairo_set_source_rgba(cr, m->ink.r, m->ink.g, m->ink.b, 0.14);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_save(cr);
        centred_text(cr, init, 20, PANGO_WEIGHT_SEMIBOLD, x + TILE / 2.0,
                     y + TILE / 2.0, m->ink, c->iconic ? 0.5 : 1);
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

static void layout(void)
{
    const Rect *mon = screen_physical_area_monitor(screen_monitor_primary(FALSE));
    GList *it;
    gint n = 0, row_w, row_x, bottom, x;

    if (S.tiles) g_array_set_size(S.tiles, 0);
    else S.tiles = g_array_new(FALSE, TRUE, sizeof(Tile));

    for (it = client_list; it; it = g_list_next(it)) {
        Tile t = { it->data, 0 };
        if (listed(t.client)) { g_array_append_val(S.tiles, t); ++n; }
    }

    S.dock_w = 2 * PAD + TILE;
    if (n) S.dock_w += DIVIDER_W + n * TILE + (n - 1) * TILE_GAP;
    S.status_w = 2 * PAD + CLOCK_W + DIVIDER_W + CONTROLS_W;
    row_w = S.dock_w + ISLAND_GAP + S.status_w;
    row_x = mon->x + (mon->width - row_w) / 2;
    bottom = mon->y + mon->height;

    S.ww = row_w + 2 * SIDE;
    S.wx = row_x - SIDE;
    S.island_y = LIP;
    S.wh = LIP + ISLAND_H + EDGE_INSET;
    S.wy = bottom - S.wh;
    S.dock_x = SIDE;
    S.status_x = SIDE + S.dock_w + ISLAND_GAP;

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

static void paint(void)
{
    cairo_surface_t *img, *dst;
    cairo_t *cr;
    const Material *m = S.m;
    gint i, y = S.island_y;
    time_t now = time(NULL);
    struct tm tm;
    gchar tbuf[32], dbuf[32];
    gdouble cx;

    if (!S.ready || !S.shown) return;

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
        gint xs[2] = { S.dock_x, S.status_x };
        gint ws[2] = { S.dock_w, S.status_w };
        for (i = 0; i < 2; ++i) {
            shadow(cr, xs[i], y, ws[i], ISLAND_H, 22, 52, -15, m->shadow,
                   m->shadow_mid_a, S.ww, S.wh);
            shadow(cr, xs[i], y, ws[i], ISLAND_H, 3, 8, 0, m->shadow,
                   m->shadow_near_a, S.ww, S.wh);
        }
        for (i = 0; i < 2; ++i) {
            rrect(cr, xs[i], y, ws[i], ISLAND_H, ISLAND_R);
            set_rgb(cr, m->body, 1);
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
            set_rgb(cr, m->hover, m->hover_a);
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
            gboolean focused = (t->client == focus_client);

            if (S.hover == i + 1) {
                rrect(cr, tx, ty, TILE, TILE, TILE_RADIUS);
                set_rgb(cr, m->hover, m->hover_a);
                cairo_fill(cr);
            }
            draw_tile(cr, t->client, tx, ty, m);
            /* the running mark: 18x3 on the island's lower edge */
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
            set_rgb(cr, m->ink, focused ? 1.0 : 0.45);
            cairo_fill(cr);
        }
    }

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
        gdouble gx = S.status_x + PAD + CLOCK_W + DIVIDER_W;
        gdouble gy = y + (ISLAND_H - GLYPH) / 2.0;
        cairo_rectangle(cr, dx, y + (ISLAND_H - DIVIDER_H) / 2.0, 1, DIVIDER_H);
        set_rgb(cr, m->divider, m->divider_a);
        cairo_fill(cr);
        glyph_wifi(cr, gx, gy, m->ink);
        glyph_volume(cr, gx + GLYPH + GLYPH_GAP, gy, m->ink);
        glyph_power(cr, gx + 2 * (GLYPH + GLYPH_GAP), gy, m->ink);
    }
    cairo_destroy(cr);

    dst = cairo_xlib_surface_create(obt_display, S.win, S.visual, S.ww, S.wh);
    cr = cairo_create(dst);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(dst);
    cairo_surface_destroy(dst);
    cairo_surface_destroy(img);
    XFlush(obt_display);
}

/* ---- window --------------------------------------------------------------- */

static void shape_input(void)
{
    if (!obt_display_extension_shape) return;
    XRectangle r[2] = {
        { S.dock_x,   S.island_y, S.dock_w,   ISLAND_H },
        { S.status_x, S.island_y, S.status_w, ISLAND_H }
    };
    XShapeCombineRectangles(obt_display, S.win, ShapeInput, 0, 0, r, 2,
                            ShapeSet, YXBanded);
}

static void snapshot(void)
{
    XImage *im;

    if (S.argb) return;
    if (S.bg) { cairo_surface_destroy(S.bg); S.bg = NULL; }
    im = XGetImage(obt_display, obt_root(ob_screen), S.wx, S.wy, S.ww, S.wh,
                   AllPlanes, ZPixmap);
    if (!im) return;
    if (im->bits_per_pixel == 32) {
        gint y;
        S.bg = cairo_image_surface_create(CAIRO_FORMAT_RGB24, S.ww, S.wh);
        cairo_surface_flush(S.bg);
        for (y = 0; y < S.wh; ++y)
            memcpy(cairo_image_surface_get_data(S.bg) +
                   y * cairo_image_surface_get_stride(S.bg),
                   im->data + y * im->bytes_per_line, 4 * S.ww);
        cairo_surface_mark_dirty(S.bg);
    }
    XDestroyImage(im);
}

static void place(void)
{
    layout();
    XMoveResizeWindow(obt_display, S.win, S.wx, S.wy, S.ww, S.wh);
    shape_input();
}

static gboolean snap_cb(gpointer data)
{
    S.snap_id = 0;
    snapshot();
    S.ready = TRUE;
    S.shown = !covered_by_fullscreen();
    if (S.shown) XMapRaised(obt_display, S.win);
    paint();
    return FALSE;
}

/* The wallpaper under the islands is read with our window out of the way. */
static void resnap(guint delay)
{
    if (S.argb) { S.ready = TRUE; return; }
    XUnmapWindow(obt_display, S.win);
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
    g_string_append_printf(g, "|%d|%d", covered_by_fullscreen(), S.hover);
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
        XUnmapWindow(obt_display, S.win);
        S.shown = FALSE;
    } else if (!fs && !S.shown) {
        S.shown = TRUE;
        place();
        XMapRaised(obt_display, S.win);
    }
    if (S.shown) {
        gint ow = S.ww, ox = S.wx;
        place();
        if (ow != S.ww || ox != S.wx) {
            /* the row changed width, so the picture behind it has to too */
            resnap(80);
            return TRUE;
        }
        paint();
    }
    return TRUE;
}

void shelf_startup(gboolean reconfig)
{
    XSetWindowAttributes attr;
    XVisualInfo vi;
    Display *d = obt_display;
    gchar *cm = g_strdup_printf("_NET_WM_CM_S%d", ob_screen);
    gulong mask;

    memset(&S, 0, sizeof S);
    S.hover = -1;
    STRUT_PARTIAL_SET(shelf_strut, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (!config_shelf_enabled) { g_free(cm); return; }

    S.m = config_shelf_material &&
          !g_ascii_strcasecmp(config_shelf_material, "light") ?
          &mat_light : &mat_dark;
    S.argb = XGetSelectionOwner(d, XInternAtom(d, cm, False)) != None &&
             XMatchVisualInfo(d, ob_screen, 32, TrueColor, &vi);
    g_free(cm);

    attr.override_redirect = True;
    attr.border_pixel = 0;
    attr.background_pixmap = None;
    attr.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                      LeaveWindowMask;
    mask = CWOverrideRedirect | CWBorderPixel | CWBackPixmap | CWEventMask;
    if (S.argb) {
        S.visual = vi.visual;
        S.cmap = XCreateColormap(d, obt_root(ob_screen), vi.visual, AllocNone);
        attr.colormap = S.cmap;
        mask |= CWColormap;
        layout();
        S.win = XCreateWindow(d, obt_root(ob_screen), S.wx, S.wy, S.ww, S.wh,
                              0, 32, InputOutput, vi.visual, mask, &attr);
    } else {
        S.visual = DefaultVisual(d, ob_screen);
        layout();
        S.win = XCreateWindow(d, obt_root(ob_screen), S.wx, S.wy, S.ww, S.wh,
                              0, CopyFromParent, InputOutput, CopyFromParent,
                              mask, &attr);
    }
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
    XDestroyWindow(obt_display, S.win);
    if (S.cmap) XFreeColormap(obt_display, S.cmap);
    if (S.bg) cairo_surface_destroy(S.bg);
    if (S.tiles) g_array_free(S.tiles, TRUE);
    g_free(S.sig);
    STRUT_PARTIAL_SET(shelf_strut, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    memset(&S, 0, sizeof S);
}

/* ---- events --------------------------------------------------------------- */

static gint hit(gint x, gint y)
{
    gint i;
    if (y < S.island_y + PAD || y >= S.island_y + PAD + TILE) return -1;
    if (x >= S.dock_x + PAD && x < S.dock_x + PAD + TILE) return 0;
    for (i = 0; i < (gint)S.tiles->len; ++i) {
        Tile *t = &g_array_index(S.tiles, Tile, i);
        if (x >= t->x && x < t->x + TILE) return i + 1;
    }
    return -1;
}

static void show_menu(const gchar *name, gint rx, gint ry, ObClient *c)
{
    GravityPoint pos = { { 0, }, };
    pos.x.pos = rx;
    pos.y.pos = ry;
    menu_show((gchar*)name, &pos, screen_monitor_primary(FALSE), FALSE, TRUE,
              c);
}

gboolean shelf_event(const XEvent *e)
{
    gint h;

    if (!S.win || e->xany.window != S.win) return FALSE;

    switch (e->type) {
    case Expose:
        if (!e->xexpose.count) paint();
        break;
    case MotionNotify:
        h = hit(e->xmotion.x, e->xmotion.y);
        if (h != S.hover) { S.hover = h; paint(); }
        break;
    case LeaveNotify:
        if (S.hover != -1) { S.hover = -1; paint(); }
        break;
    case ButtonPress:
        h = hit(e->xbutton.x, e->xbutton.y);
        if (h == 0 && e->xbutton.button == 1) {
            show_menu("root-menu", e->xbutton.x_root,
                      S.wy + S.island_y - 4, NULL);
        } else if (h > 0) {
            ObClient *c = g_array_index(S.tiles, Tile, h - 1).client;
            if (!g_list_find(client_list, c)) break;
            if (e->xbutton.button == 1) {
                if (c == focus_client && !c->iconic)
                    client_iconify(c, TRUE, TRUE, TRUE);
                else
                    client_activate(c, TRUE, TRUE, TRUE, TRUE, TRUE);
            } else if (e->xbutton.button == 3) {
                show_menu("client-menu", e->xbutton.x_root,
                          S.wy + S.island_y - 4, c);
            }
        }
        break;
    }
    return TRUE;
}
