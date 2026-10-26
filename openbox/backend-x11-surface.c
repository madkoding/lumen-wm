/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-x11-surface.c for the Lumen window manager

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

/* Managed windows and the frames around them, on X11: the frame is a
   window the client is reparented into, and the core's decorations are its
   children until they draw without Xlib. */

#include "backend-x11-int.h"
#include "openbox.h"
#include "obrender/render.h"
#include "obt/display.h"
#include "obt/prop.h"
#include "obt/xqueue.h"

#ifdef SHAPE
#include <X11/extensions/shape.h>
#endif

#define CLIENT_EVENTMASK (PropertyChangeMask | StructureNotifyMask | \
                          ColormapChangeMask)
#define CLIENT_NOPROPAGATEMASK (ButtonPressMask | ButtonReleaseMask | \
                                ButtonMotionMask)
#define FRAME_EVENTMASK (EnterWindowMask | LeaveWindowMask | \
                         ButtonPressMask | ButtonReleaseMask | \
                         SubstructureRedirectMask | FocusChangeMask)

typedef struct {
    Window frame;
    Window owner;
    Colormap colormap;  /*!< for an owner with another visual, or None */
} Decor;

/* Decor by frame, and by owner */
static GHashTable *decor_frames, *decor_owners;

void x11_surface_adopt(ObSurfaceId s, gboolean internal)
{
    XSetWindowAttributes attrib;

    attrib.event_mask = CLIENT_EVENTMASK | (internal ? KeyPressMask : 0);
    attrib.do_not_propagate_mask = CLIENT_NOPROPAGATEMASK;
    XChangeWindowAttributes(obt_display, (Window)s,
                            CWEventMask|CWDontPropagate, &attrib);

    /* if the window manager exits, the window should not be destroyed and
       should be reparented back to root automatically, unless it is one of
       ours */
    if (!internal)
        XChangeSaveSet(obt_display, (Window)s, SetModeInsert);

#ifdef SHAPE
    if (obt_display_extension_shape)
        XShapeSelectInput(obt_display, (Window)s, ShapeNotifyMask);
#endif
}

void x11_surface_unwatch(ObSurfaceId s, gboolean internal)
{
    XSelectInput(obt_display, (Window)s, NoEventMask);
    if (!internal)
        XChangeSaveSet(obt_display, (Window)s, SetModeDelete);
}

void x11_surface_frame(ObSurfaceId s, ObCanvasId frame)
{
    XReparentWindow(obt_display, (Window)s, (Window)frame, 0, 0);
}

static gboolean find_reparent(XEvent *e, gpointer data)
{
    const Decor *d = data;

    /* Find ReparentNotify events for the window that aren't being
       reparented into the frame, thus the client reparenting itself off the
       frame. */
    return e->type == ReparentNotify && e->xreparent.window == d->owner &&
        e->xreparent.parent != d->frame;
}

void x11_surface_unframe(ObSurfaceId s, const Rect *area)
{
    Window w = (Window)s;
    Decor *d = decor_owners ? g_hash_table_lookup(decor_owners, &w) : NULL;

    /* according to the ICCCM - if the client doesn't reparent itself,
       then we will reparent the window to root for them */
    if (!d || !xqueue_exists_local(find_reparent, d))
        XReparentWindow(obt_display, w, obt_root(ob_screen),
                        area->x, area->y);
}

void x11_surface_release(ObSurfaceId s, gboolean exiting)
{
    Window w = (Window)s;

    if (!exiting) {
        /* these values should not be persisted across a window
           unmapping/mapping */
        OBT_PROP_ERASE(w, NET_WM_DESKTOP);
        OBT_PROP_ERASE(w, NET_WM_STATE);
        OBT_PROP_ERASE(w, WM_STATE);
    } else {
        /* if we're left in an unmapped state, the client wont be mapped.
           this is bad, since we will no longer be managing the window on
           restart */
        XMapWindow(obt_display, w);
    }

    /* these should not be left on the window ever.  other window managers
       don't necessarily use them and it will mess them up (like compiz) */
    OBT_PROP_ERASE(w, NET_WM_VISIBLE_NAME);
    OBT_PROP_ERASE(w, NET_WM_VISIBLE_ICON_NAME);
}

void x11_surface_set_border(ObSurfaceId s, gint width)
{
    XSetWindowBorderWidth(obt_display, (Window)s, width);
}

void x11_surface_configure(ObSurfaceId s, const Rect *area)
{
    XMoveResizeWindow(obt_display, (Window)s,
                      area->x, area->y, area->width, area->height);
}

void x11_surface_send_geometry(ObSurfaceId s, const Rect *area, gint border)
{
    XEvent event;

    event.type = ConfigureNotify;
    event.xconfigure.display = obt_display;
    event.xconfigure.event = (Window)s;
    event.xconfigure.window = (Window)s;

    /* root window real coords */
    event.xconfigure.x = area->x;
    event.xconfigure.y = area->y;
    event.xconfigure.width = area->width;
    event.xconfigure.height = area->height;
    event.xconfigure.border_width = border;
    event.xconfigure.above = None;
    event.xconfigure.override_redirect = FALSE;
    XSendEvent(event.xconfigure.display, event.xconfigure.window,
               FALSE, StructureNotifyMask, &event);
}

void x11_surface_show(ObSurfaceId s, gboolean show)
{
    if (show)
        XMapWindow(obt_display, (Window)s);
    else
        XUnmapWindow(obt_display, (Window)s);
}

void x11_get_geometry(ObSurfaceId s, ObSurfaceInfo *info)
{
    XWindowAttributes wattrib;

    if (XGetWindowAttributes(obt_display, (Window)s, &wattrib)) {
        RECT_SET(info->area, wattrib.x, wattrib.y,
                 wattrib.width, wattrib.height);
        info->border = wattrib.border_width;
        info->argb = wattrib.depth == 32;
    } else {
        RECT_SET(info->area, 0, 0, 1, 1);
        info->border = 0;
        info->argb = FALSE;
    }
}

ObCanvasId x11_canvas_new(ObCanvasLayer layer, ObSurfaceId owner)
{
    XSetWindowAttributes attrib;
    XWindowAttributes wattrib;
    gulong mask;
    Visual *visual = NULL;
    Decor *d;

    /* only frames are made here so far */
    g_return_val_if_fail(layer == OB_CANVAS_DECOR, OB_CANVAS_NONE);

    d = g_slice_new0(Decor);
    d->owner = (Window)owner;

    /* a client with a 32-bit visual gets a frame with its visual, unless
       we're already running at 32 bit depth */
    if (RrDepth(ob_rr_inst) != 32 &&
        XGetWindowAttributes(obt_display, d->owner, &wattrib) &&
        wattrib.depth == 32)
        visual = wattrib.visual;

    mask = CWEventMask;
    attrib.event_mask = FRAME_EVENTMASK;
    if (visual) {
        mask |= CWColormap | CWBackPixel | CWBorderPixel;
        /* create a colormap with the visual */
        d->colormap = attrib.colormap =
            XCreateColormap(obt_display, obt_root(ob_screen),
                            visual, AllocNone);
        attrib.background_pixel = BlackPixel(obt_display, ob_screen);
        attrib.border_pixel = BlackPixel(obt_display, ob_screen);
    }
    d->frame = XCreateWindow(obt_display, obt_root(ob_screen), 0, 0, 1, 1, 0,
                             (visual ? 32 : RrDepth(ob_rr_inst)), InputOutput,
                             (visual ? visual : RrVisual(ob_rr_inst)),
                             mask, &attrib);

    if (!decor_frames) {
        decor_frames = g_hash_table_new(g_int_hash, g_int_equal);
        decor_owners = g_hash_table_new(g_int_hash, g_int_equal);
    }
    g_hash_table_insert(decor_frames, &d->frame, d);
    g_hash_table_insert(decor_owners, &d->owner, d);
    return (ObCanvasId)d->frame;
}

void x11_canvas_free(ObCanvasId c)
{
    Window w = (Window)c;
    Decor *d = decor_frames ? g_hash_table_lookup(decor_frames, &w) : NULL;

    XDestroyWindow(obt_display, w);
    if (d) {
        if (d->colormap)
            XFreeColormap(obt_display, d->colormap);
        g_hash_table_remove(decor_frames, &d->frame);
        g_hash_table_remove(decor_owners, &d->owner);
        g_slice_free(Decor, d);
    }
}

void x11_canvas_place(ObCanvasId c, const Rect *outer, const Rect *inner)
{
    Window w = (Window)c;

    if (outer)
        XMoveResizeWindow(obt_display, w, outer->x, outer->y,
                          outer->width, outer->height);
    if (inner) {
        Decor *d = decor_frames ? g_hash_table_lookup(decor_frames, &w) : NULL;
        if (d)
            XMoveWindow(obt_display, d->owner, inner->x, inner->y);
    }
}

void x11_canvas_show(ObCanvasId c, gboolean show)
{
    if (show)
        XMapWindow(obt_display, (Window)c);
    else
        XUnmapWindow(obt_display, (Window)c);
}

void x11_publish_surface_extents(ObSurfaceId s, const Strut *extents)
{
    gulong vals[4];

    vals[0] = extents->left;
    vals[1] = extents->right;
    vals[2] = extents->top;
    vals[3] = extents->bottom;
    OBT_PROP_SETA32((Window)s, NET_FRAME_EXTENTS, CARDINAL, vals, 4);
    OBT_PROP_SETA32((Window)s, KDE_NET_WM_FRAME_STRUT, CARDINAL, vals, 4);
}

void x11_get_shape(ObSurfaceId s, ObSurfaceInfo *info)
{
    info->shaped = FALSE;
    info->shaped_input = FALSE;
#ifdef SHAPE
    if (obt_display_extension_shape) {
        gint foo;
        guint ufoo;
        gint b;

        XShapeQueryExtents(obt_display, (Window)s, &b, &foo,
                           &foo, &ufoo, &ufoo, &foo, &foo, &foo, &ufoo,
                           &ufoo);
        info->shaped = !!b;
#ifdef ShapeInput
        { /* when some smart people added ShapeInput they forgot to update XShapeQueryExtents or
             add a new one for input shapes, so we get to do this instead, yay! */
            int nrects, ordering;
            Window root;
            gint x, y;
            guint w = 0, h = 0, bw, depth;
            XRectangle *input_rect;

            XGetGeometry(obt_display, (Window)s, &root, &x, &y, &w, &h,
                         &bw, &depth);
            input_rect = XShapeGetRectangles(obt_display, (Window)s,
                                             ShapeInput, &nrects, &ordering);
            if (nrects == 1 && input_rect->width == w &&
                               input_rect->height == h &&
                               /* openbox sets the border width of client windows to 0 */
                               input_rect->x == 0 && input_rect->y == 0) {
                info->shaped_input = FALSE;
            } else {
                info->shaped_input = TRUE;
            }
            XFree(input_rect);
        }
#endif
    }
#endif
}

void x11_canvas_set_shape(ObCanvasId c, const Rect *inner, gboolean bounding,
                          gboolean input, const Rect *rects, guint n)
{
#ifdef SHAPE
    Window w = (Window)c;
    Decor *d = decor_frames ? g_hash_table_lookup(decor_frames, &w) : NULL;
    const int kinds[] = {
        ShapeBounding,
#ifdef ShapeInput
        ShapeInput,
#endif
    };
    guint k, i;

    if (!d) return;

    for (k = 0; k < G_N_ELEMENTS(kinds); ++k) {
        if (!(kinds[k] == ShapeBounding ? bounding : input)) {
            /* clear the shape on the frame window */
            XShapeCombineMask(obt_display, w, kinds[k], inner->x, inner->y,
                              None, ShapeSet);
        } else {
            XRectangle *xrect = g_new(XRectangle, n);

            /* make the frame's shape match the clients */
            XShapeCombineShape(obt_display, w, kinds[k], inner->x, inner->y,
                               d->owner, kinds[k], ShapeSet);
            for (i = 0; i < n; ++i) {
                xrect[i].x = rects[i].x;
                xrect[i].y = rects[i].y;
                xrect[i].width = rects[i].width;
                xrect[i].height = rects[i].height;
            }
            XShapeCombineRectangles(obt_display, w, kinds[k], 0, 0, xrect, n,
                                    ShapeUnion, Unsorted);
            g_free(xrect);
        }
    }
#else
    (void)c; (void)inner; (void)bounding; (void)input; (void)rects; (void)n;
#endif
}
