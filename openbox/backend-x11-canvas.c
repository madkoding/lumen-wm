/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-x11-canvas.c for the Lumen window manager

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

/* Canvases on X11: what the core draws (frames and their parts, menus,
   popups, the shelf, the dock) is a window, and a picture the core hands
   over becomes its background pixmap, so X keeps it on screen without
   asking again. */

#include "backend-x11-int.h"
#include "openbox.h"
#include "obrender/render.h"
#include "obt/display.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <cairo.h>
#include <cairo-xlib.h>

typedef struct {
    Window window;
    Window parent;          /*!< None for a top-level canvas */
    Visual *visual;
    gint depth;
    gboolean input_only;
    Pixmap background;
} Canvas;

/* every canvas, by window */
static GHashTable *canvases;

static Canvas *find(ObCanvasId c)
{
    Window w = (Window)c;
    return canvases ? g_hash_table_lookup(canvases, &w) : NULL;
}

static Canvas *add(Window w, Window parent, Visual *visual, gint depth,
                   gboolean input_only)
{
    Canvas *cv = g_slice_new0(Canvas);

    cv->window = w;
    cv->parent = parent;
    cv->visual = visual;
    cv->depth = depth;
    cv->input_only = input_only;
    if (!canvases)
        canvases = g_hash_table_new(g_int_hash, g_int_equal);
    g_hash_table_insert(canvases, &cv->window, cv);
    return cv;
}

static void forget(Canvas *cv)
{
    g_hash_table_remove(canvases, &cv->window);
    if (cv->background)
        XFreePixmap(obt_display, cv->background);
    g_slice_free(Canvas, cv);
}

static gulong event_mask(ObCanvasFlags flags)
{
    gulong mask = 0;

    if (flags & OB_CANVAS_INPUT_BUTTONS)
        mask |= ButtonPressMask | ButtonReleaseMask;
    if (flags & OB_CANVAS_INPUT_CROSSING)
        mask |= EnterWindowMask | LeaveWindowMask;
    if (flags & OB_CANVAS_INPUT_MOTION)
        mask |= PointerMotionMask;
    if (flags & OB_CANVAS_INPUT_DRAG)
        mask |= ButtonMotionMask;
    return mask;
}

/* a 32-bit visual, when a compositor will blend it */
static gboolean alpha_visual(Visual **visual, Colormap *cmap)
{
    static Colormap alpha_cmap;
    XVisualInfo vi;
    gchar *cm = g_strdup_printf("_NET_WM_CM_S%d", ob_screen);
    gboolean ok = XGetSelectionOwner(obt_display,
                                     XInternAtom(obt_display, cm, False))
        != None &&
        XMatchVisualInfo(obt_display, ob_screen, 32, TrueColor, &vi);

    g_free(cm);
    if (!ok) return FALSE;
    if (!alpha_cmap)
        alpha_cmap = XCreateColormap(obt_display, obt_root(ob_screen),
                                     vi.visual, AllocNone);
    *visual = vi.visual;
    *cmap = alpha_cmap;
    return TRUE;
}

ObCanvasId x11_canvas_new(ObCanvasLayer layer, ObSurfaceId owner,
                          ObCanvasFlags flags)
{
    XSetWindowAttributes attrib;
    XWindowAttributes wattrib;
    gulong mask;
    Visual *visual = RrVisual(ob_rr_inst);
    Colormap cmap = RrColormap(ob_rr_inst);
    gint depth = RrDepth(ob_rr_inst);
    Window w;

    if (layer == OB_CANVAS_DECOR) {
        w = x11_decor_new(owner);
        if (XGetWindowAttributes(obt_display, w, &wattrib)) {
            visual = wattrib.visual;
            depth = wattrib.depth;
        }
        add(w, None, visual, depth, FALSE);
        return (ObCanvasId)w;
    }

    mask = CWEventMask | CWColormap | CWBorderPixel;
    attrib.event_mask = event_mask(flags);
    attrib.border_pixel = 0;
    if ((flags & OB_CANVAS_ALPHA) && alpha_visual(&visual, &cmap)) {
        depth = 32;
        mask |= CWBackPixmap;
        attrib.background_pixmap = None;
    }
    attrib.colormap = cmap;
    /* menus are mapped by the window manager itself, which doesn't
       manage them; everything else stays out of other window managers'
       way too */
    if (layer != OB_CANVAS_MENU) {
        mask |= CWOverrideRedirect;
        attrib.override_redirect = True;
    }
    if (layer == OB_CANVAS_PANEL) {
        /* clicks on what sits in it (dock apps) stay there */
        mask |= CWDontPropagate;
        attrib.do_not_propagate_mask = ButtonPressMask | ButtonReleaseMask |
            ButtonMotionMask;
    }

    w = XCreateWindow(obt_display, obt_root(ob_screen), 0, 0, 1, 1, 0,
                      depth, InputOutput, visual, mask, &attrib);
    add(w, None, visual, depth, FALSE);
    return (ObCanvasId)w;
}

ObCanvasId x11_canvas_new_child(ObCanvasId parent, ObCanvasFlags flags)
{
    XSetWindowAttributes attrib;
    gulong mask = CWEventMask;
    Canvas *p = find(parent);
    Window w;

    g_return_val_if_fail(p != NULL, OB_CANVAS_NONE);

    attrib.event_mask = event_mask(flags);

    if (flags & OB_CANVAS_INVISIBLE) {
        w = XCreateWindow(obt_display, p->window, 0, 0, 1, 1, 0, 0,
                          InputOnly, CopyFromParent, mask, &attrib);
        add(w, p->window, NULL, 0, TRUE);
        return (ObCanvasId)w;
    }

    /* the parts draw in the default visual; inside a 32-bit parent they
       need their own colormap and border */
    mask |= CWColormap | CWBorderPixel;
    attrib.colormap = RrColormap(ob_rr_inst);
    attrib.border_pixel = BlackPixel(obt_display, ob_screen);
    if (p->depth == 32 && RrDepth(ob_rr_inst) != 32) {
        mask |= CWBackPixel;
        attrib.background_pixel = BlackPixel(obt_display, ob_screen);
    }
    w = XCreateWindow(obt_display, p->window, 0, 0, 1, 1, 0,
                      RrDepth(ob_rr_inst), InputOutput, RrVisual(ob_rr_inst),
                      mask, &attrib);
    add(w, p->window, RrVisual(ob_rr_inst), RrDepth(ob_rr_inst), FALSE);
    return (ObCanvasId)w;
}

static gboolean is_inside(Canvas *cv, Window ancestor)
{
    while (cv && cv->parent) {
        if (cv->parent == ancestor) return TRUE;
        cv = find(cv->parent);
    }
    return FALSE;
}

void x11_canvas_free(ObCanvasId c)
{
    Canvas *cv = find(c);
    GHashTableIter it;
    gpointer key, value;
    GSList *gone = NULL, *l;

    XDestroyWindow(obt_display, (Window)c);
    x11_decor_free((Window)c);
    if (!cv) return;

    /* X destroyed the parts with it */
    g_hash_table_iter_init(&it, canvases);
    while (g_hash_table_iter_next(&it, &key, &value))
        if (is_inside(value, cv->window))
            gone = g_slist_prepend(gone, value);
    for (l = gone; l; l = g_slist_next(l))
        forget(l->data);
    g_slist_free(gone);
    forget(cv);
}

gboolean x11_canvas_has_alpha(ObCanvasId c)
{
    Canvas *cv = find(c);
    return cv && cv->depth == 32 && RrDepth(ob_rr_inst) != 32;
}

void x11_canvas_present(ObCanvasId c, const ObImage *img, const Rect *damage)
{
    Canvas *cv = find(c);
    cairo_surface_t *src, *dst;
    cairo_t *cr;
    Pixmap pm;

    g_return_if_fail(cv != NULL && !cv->input_only);
    (void)damage;

    if (img->width <= 0 || img->height <= 0) return;

    pm = XCreatePixmap(obt_display, cv->window, img->width, img->height,
                       cv->depth);
    src = cairo_image_surface_create_for_data((guchar*)img->data,
                                              CAIRO_FORMAT_ARGB32,
                                              img->width, img->height,
                                              img->stride);
    dst = cairo_xlib_surface_create(obt_display, pm, cv->visual,
                                    img->width, img->height);
    cr = cairo_create(dst);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(dst);
    cairo_surface_destroy(src);

    XSetWindowBackgroundPixmap(obt_display, cv->window, pm);
    XClearWindow(obt_display, cv->window);
    /* free the old one after the new one shows */
    if (cv->background)
        XFreePixmap(obt_display, cv->background);
    cv->background = pm;
}

gulong x11_pixel(guint32 argb)
{
    Visual *v = RrVisual(ob_rr_inst);
    XColor xc;

    if (v->class == TrueColor) {
        gulong pixel = 0;
        gulong masks[3] = { v->red_mask, v->green_mask, v->blue_mask };
        guint32 values[3] = { (argb >> 16) & 0xff, (argb >> 8) & 0xff,
                              argb & 0xff };
        gint i;

        for (i = 0; i < 3; ++i) {
            gulong m = masks[i];
            gint shift = 0, bits = 0;

            while (m && !(m & 1)) { m >>= 1; ++shift; }
            while (m & 1) { m >>= 1; ++bits; }
            pixel |= (gulong)((values[i] >> (8 - MIN(bits, 8))) <<
                              (shift + MAX(bits - 8, 0))) &
                masks[i];
        }
        return pixel;
    }

    xc.red = ((argb >> 16) & 0xff) * 0x101;
    xc.green = ((argb >> 8) & 0xff) * 0x101;
    xc.blue = (argb & 0xff) * 0x101;
    xc.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(obt_display, RrColormap(ob_rr_inst), &xc))
        xc.pixel = 0;
    return xc.pixel;
}

void x11_canvas_fill(ObCanvasId c, guint32 argb)
{
    Canvas *cv = find(c);

    XSetWindowBackground(obt_display, (Window)c, x11_pixel(argb));
    XClearWindow(obt_display, (Window)c);
    if (cv && cv->background) {
        XFreePixmap(obt_display, cv->background);
        cv->background = None;
    }
}

void x11_canvas_set_border(ObCanvasId c, gint width, guint32 argb)
{
    XSetWindowBorderWidth(obt_display, (Window)c, width);
    if (width)
        XSetWindowBorder(obt_display, (Window)c, x11_pixel(argb));
}

void x11_canvas_set_outline(ObCanvasId c, const ObImage *mask)
{
    Pixmap bits;
    gchar *data;
    gint x, y, stride;

    if (!mask) {
        XShapeCombineMask(obt_display, (Window)c, ShapeBounding, 0, 0,
                          None, ShapeSet);
        return;
    }

    /* a bitmap, rows of whole bytes, least significant bit first */
    stride = (mask->width + 7) / 8;
    data = g_new0(gchar, stride * mask->height);
    for (y = 0; y < mask->height; ++y) {
        const guint32 *row = (const guint32*)
            ((const guchar*)mask->data + y * mask->stride);
        for (x = 0; x < mask->width; ++x)
            if (row[x] >> 24)
                data[y * stride + x / 8] |= 1 << (x % 8);
    }
    bits = XCreateBitmapFromData(obt_display, (Window)c, data,
                                 mask->width, mask->height);
    XShapeCombineMask(obt_display, (Window)c, ShapeBounding, 0, 0, bits,
                      ShapeSet);
    XFreePixmap(obt_display, bits);
    g_free(data);
}
