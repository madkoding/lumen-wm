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
#include "obt/prop.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <cairo.h>
#include <cairo-xlib.h>
#include <string.h>

typedef struct {
    Window window;
    Window parent;          /*!< None for a top-level canvas */
    Visual *visual;
    gint depth;
    gboolean input_only;
    Pixmap background;
} Canvas;

/* what the core's pictures are drawn in: the screen's default visual */

Visual *x11_visual(void)
{
    return DefaultVisual(obt_display, ob_screen);
}

gint x11_depth(void)
{
    return DefaultDepth(obt_display, ob_screen);
}

Colormap x11_colormap(void)
{
    return DefaultColormap(obt_display, ob_screen);
}

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

    if (flags & OB_CANVAS_INPUT_PRESS)
        mask |= ButtonPressMask;
    if (flags & OB_CANVAS_INPUT_RELEASE)
        mask |= ButtonReleaseMask;
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
    Visual *visual = x11_visual();
    Colormap cmap = x11_colormap();
    gint depth = x11_depth();
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
       manage them, and dialogs are managed like clients; everything else
       stays out of other window managers' way */
    if (layer != OB_CANVAS_MENU && layer != OB_CANVAS_DIALOG) {
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
    /* so compositors can tell what it is */
    if (layer == OB_CANVAS_MENU)
        OBT_PROP_SET32(w, NET_WM_WINDOW_TYPE, ATOM,
                       OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_POPUP_MENU));
    else if (layer == OB_CANVAS_DIALOG)
        OBT_PROP_SET32(w, NET_WM_WINDOW_TYPE, ATOM,
                       OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_DIALOG));
    else if (layer == OB_CANVAS_PANEL)
        OBT_PROP_SET32(w, NET_WM_WINDOW_TYPE, ATOM,
                       OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_DOCK));
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
    attrib.colormap = x11_colormap();
    attrib.border_pixel = BlackPixel(obt_display, ob_screen);
    if (p->depth == 32 && x11_depth() != 32) {
        mask |= CWBackPixel;
        attrib.background_pixel = BlackPixel(obt_display, ob_screen);
    }
    w = XCreateWindow(obt_display, p->window, 0, 0, 1, 1, 0,
                      x11_depth(), InputOutput, x11_visual(),
                      mask, &attrib);
    add(w, p->window, x11_visual(), x11_depth(), FALSE);
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
    return cv && cv->depth == 32 && x11_depth() != 32;
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
    Visual *v = x11_visual();
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
    if (!XAllocColor(obt_display, x11_colormap(), &xc))
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

void x11_canvas_move(ObCanvasId c, gint x, gint y)
{
    XMoveWindow(obt_display, (Window)c, x, y);
}

void x11_canvas_resize(ObCanvasId c, gint width, gint height)
{
    XResizeWindow(obt_display, (Window)c, width, height);
}

void x11_canvas_set_input(ObCanvasId c, const Rect *rects, guint n)
{
    XRectangle *xr;
    guint i;

    if (!rects) {
        XShapeCombineMask(obt_display, (Window)c, ShapeInput, 0, 0, None,
                          ShapeSet);
        return;
    }
    xr = g_new(XRectangle, MAX(n, 1));
    for (i = 0; i < n; ++i) {
        xr[i].x = rects[i].x;
        xr[i].y = rects[i].y;
        xr[i].width = rects[i].width;
        xr[i].height = rects[i].height;
    }
    XShapeCombineRectangles(obt_display, (Window)c, ShapeInput, 0, 0, xr, n,
                            ShapeSet, Unsorted);
    g_free(xr);
}

void x11_dialog_set_title(ObCanvasId c, const gchar *title)
{
    OBT_PROP_SETS((Window)c, NET_WM_NAME, title);
}

void x11_dialog_set_size(ObCanvasId c, gint width, gint height,
                         gboolean managed)
{
    XSizeHints hints;

    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = width;
    hints.min_height = hints.max_height = height;
    XSetWMNormalHints(obt_display, (Window)c, &hints);

    if (managed) {
        /* send a configure request like a normal client would */
        XConfigureRequestEvent req;

        req.type = ConfigureRequest;
        req.display = obt_display;
        req.parent = obt_root(ob_screen);
        req.window = (Window)c;
        req.width = width;
        req.height = height;
        req.value_mask = CWWidth | CWHeight;
        XSendEvent(req.display, req.window, FALSE, StructureNotifyMask,
                   (XEvent*)&req);
    } else
        XResizeWindow(obt_display, (Window)c, width, height);
}

void x11_dialog_set_parent(ObCanvasId c, ObSurfaceId parent,
                           ObSurfaceId group, gboolean modal)
{
    Window w = (Window)c;
    XWMHints h;
    Atom states[1];

    if (!parent && !group) {
        OBT_PROP_ERASE(w, WM_TRANSIENT_FOR);
        return;
    }
    if (group) {
        /* transient for the window's group */
        h.flags = WindowGroupHint;
        h.window_group = (Window)group;
        parent = (ObSurfaceId)obt_root(ob_screen);
    } else
        h.flags = 0;
    XSetWMHints(obt_display, w, &h);
    OBT_PROP_SET32(w, WM_TRANSIENT_FOR, WINDOW, (Window)parent);

    states[0] = OBT_PROP_ATOM(NET_WM_STATE_MODAL);
    OBT_PROP_SETA32(w, NET_WM_STATE, ATOM, states, modal ? 1 : 0);
}

gboolean x11_screen_read(const Rect *area, ObImage *img)
{
    XImage *im;
    guint32 *data;
    gint y;

    im = XGetImage(obt_display, obt_root(ob_screen), area->x, area->y,
                   area->width, area->height, AllPlanes, ZPixmap);
    if (!im) return FALSE;
    if (im->bits_per_pixel != 32) {
        XDestroyImage(im);
        return FALSE;
    }
    data = g_new(guint32, area->width * area->height);
    for (y = 0; y < area->height; ++y) {
        guint32 *row = data + y * area->width;
        gint x;

        memcpy(row, im->data + y * im->bytes_per_line, 4 * area->width);
        for (x = 0; x < area->width; ++x)
            row[x] |= 0xff000000u;
    }
    XDestroyImage(im);
    img->data = data;
    img->width = area->width;
    img->height = area->height;
    img->stride = area->width * 4;
    return TRUE;
}

/* the pixel's value in a visual's channel, scaled to 8 bits */
static guint32 channel(gulong pixel, gulong mask)
{
    gint shift = 0, bits = 0;

    if (!mask) return 0;
    while (!(mask & 1)) { mask >>= 1; ++shift; }
    while (mask & 1) { mask >>= 1; ++bits; }
    pixel = (pixel >> shift) & ((1ul << bits) - 1);
    return bits >= 8 ? pixel >> (bits - 8) : pixel * 255 / ((1u << bits) - 1);
}

gboolean x11_pixmap_argb(Pixmap pmap, Pixmap mask, gint *w, gint *h,
                         guint32 **data)
{
    Window xr;
    gint xx, xy, x, y;
    guint pw, ph, mw, mh, xb, xd, md;
    XImage *xi, *xm = NULL;
    Visual *v = x11_visual();

    if (!XGetGeometry(obt_display, pmap, &xr, &xx, &xy, &pw, &ph, &xb, &xd))
        return FALSE;
    if (mask) {
        if (!XGetGeometry(obt_display, mask, &xr, &xx, &xy, &mw, &mh, &xb,
                          &md))
            return FALSE;
        if (pw != mw || ph != mh || md != 1)
            return FALSE;
    }
    if (xd != 1 && v->class != TrueColor)
        return FALSE;

    xi = XGetImage(obt_display, pmap, 0, 0, pw, ph, AllPlanes, ZPixmap);
    if (!xi)
        return FALSE;
    if (mask &&
        !(xm = XGetImage(obt_display, mask, 0, 0, mw, mh, AllPlanes, ZPixmap)))
    {
        XDestroyImage(xi);
        return FALSE;
    }

    *data = g_new(guint32, pw * ph);
    for (y = 0; y < (gint)ph; ++y)
        for (x = 0; x < (gint)pw; ++x) {
            gulong p = XGetPixel(xi, x, y);
            guint32 argb;

            if (xd == 1)
                argb = p ? 0xffffffffu : 0xff000000u; /* white or black */
            else
                argb = 0xff000000u | (channel(p, v->red_mask) << 16) |
                    (channel(p, v->green_mask) << 8) |
                    channel(p, v->blue_mask);
            /* transparent where the mask is not set */
            if (xm && !XGetPixel(xm, x, y))
                argb &= 0x00ffffffu;
            (*data)[y * pw + x] = argb;
        }
    *w = pw;
    *h = ph;

    XDestroyImage(xi);
    if (xm) XDestroyImage(xm);
    return TRUE;
}

/* the Xft resources, which X clients share */
static gboolean is_false(const gchar *v)
{
    return !g_ascii_strcasecmp(v, "false") || !strcmp(v, "0") ||
        !g_ascii_strcasecmp(v, "off");
}

void x11_font_settings(RrFontSettings *fs)
{
    const gchar *v;

    fs->dpi = 0;
    fs->antialias = CAIRO_ANTIALIAS_DEFAULT;
    fs->hint_style = CAIRO_HINT_STYLE_DEFAULT;
    fs->subpixel_order = CAIRO_SUBPIXEL_ORDER_DEFAULT;

    if ((v = XGetDefault(obt_display, "Xft", "dpi")))
        fs->dpi = g_ascii_strtod(v, NULL);
    if (fs->dpi <= 0 && DisplayHeightMM(obt_display, ob_screen) > 0)
        fs->dpi = DisplayHeight(obt_display, ob_screen) * 25.4 /
            DisplayHeightMM(obt_display, ob_screen);

    if ((v = XGetDefault(obt_display, "Xft", "antialias")))
        fs->antialias = is_false(v) ? CAIRO_ANTIALIAS_NONE :
            CAIRO_ANTIALIAS_GRAY;
    if ((v = XGetDefault(obt_display, "Xft", "hinting")) && is_false(v))
        fs->hint_style = CAIRO_HINT_STYLE_NONE;
    else if ((v = XGetDefault(obt_display, "Xft", "hintstyle"))) {
        if (!strcmp(v, "hintnone"))
            fs->hint_style = CAIRO_HINT_STYLE_NONE;
        else if (!strcmp(v, "hintslight"))
            fs->hint_style = CAIRO_HINT_STYLE_SLIGHT;
        else if (!strcmp(v, "hintmedium"))
            fs->hint_style = CAIRO_HINT_STYLE_MEDIUM;
        else if (!strcmp(v, "hintfull"))
            fs->hint_style = CAIRO_HINT_STYLE_FULL;
    }
    if ((v = XGetDefault(obt_display, "Xft", "rgba"))) {
        if (!strcmp(v, "rgb"))
            fs->subpixel_order = CAIRO_SUBPIXEL_ORDER_RGB;
        else if (!strcmp(v, "bgr"))
            fs->subpixel_order = CAIRO_SUBPIXEL_ORDER_BGR;
        else if (!strcmp(v, "vrgb"))
            fs->subpixel_order = CAIRO_SUBPIXEL_ORDER_VRGB;
        else if (!strcmp(v, "vbgr"))
            fs->subpixel_order = CAIRO_SUBPIXEL_ORDER_VBGR;
    }
}
