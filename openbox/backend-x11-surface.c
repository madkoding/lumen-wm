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
#include "debug.h"
#include "obrender/render.h"
#include "obt/display.h"
#include "obt/prop.h"
#include "obt/xqueue.h"

#include <X11/Xutil.h>
#include <string.h>

#ifdef SHAPE
#include <X11/extensions/shape.h>
#endif

#define CLIENT_EVENTMASK (PropertyChangeMask | StructureNotifyMask | \
                          ColormapChangeMask)
#define CLIENT_NOPROPAGATEMASK (ButtonPressMask | ButtonReleaseMask | \
                                ButtonMotionMask)
#define DOCKAPP_EVENT_MASK (StructureNotifyMask)
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

/* the windows the core manages, and the dock apps' icon windows */
static GHashTable *managed, *dockapps;

static void add_window(GHashTable **set, Window w)
{
    Window *key = g_new(Window, 1);

    if (!*set)
        *set = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, NULL);
    *key = w;
    g_hash_table_replace(*set, key, key);
}

static void remove_window(GHashTable *set, Window w)
{
    if (set) g_hash_table_remove(set, &w);
}

static gboolean has_window(GHashTable *set, Window w)
{
    return set && g_hash_table_lookup(set, &w);
}

Window x11_managed_of(Window w)
{
    Decor *d;

    if (has_window(managed, w))
        return w;
    d = decor_frames ? g_hash_table_lookup(decor_frames, &w) : NULL;
    return d ? d->owner : None;
}

gboolean x11_is_dockapp(Window w)
{
    return has_window(dockapps, w);
}

gboolean x11_is_frame_of(Window parent, Window w)
{
    Decor *d = decor_owners ? g_hash_table_lookup(decor_owners, &w) : NULL;
    return d && d->frame == parent;
}

typedef struct {
    Window window;
    Colormap colormap;
} WindowColormap;

/* the colormap of each managed window, and the window whose colormap is
   installed */
static GHashTable *colormaps;
static Window colors_owner = None;

ObSurfaceId *x11_existing_surfaces(guint *n)
{
    guint i, j, nchild, count = 0;
    Window w, *children;
    XWMHints *wmhints;
    XWindowAttributes attrib;
    ObSurfaceId *list;

    if (!XQueryTree(obt_display, RootWindow(obt_display, ob_screen),
                    &w, &w, &children, &nchild)) {
        ob_debug("XQueryTree failed in x11_existing_surfaces");
        nchild = 0;
    }

    /* remove all icon windows from the list */
    for (i = 0; i < nchild; i++) {
        if (children[i] == None) continue;
        wmhints = XGetWMHints(obt_display, children[i]);
        if (wmhints) {
            if ((wmhints->flags & IconWindowHint) &&
                (wmhints->icon_window != children[i]))
                for (j = 0; j < nchild; j++)
                    if (children[j] == wmhints->icon_window) {
                        /* XXX watch the window though */
                        children[j] = None;
                        break;
                    }
            XFree(wmhints);
        }
    }

    list = g_new(ObSurfaceId, nchild + 1);
    for (i = 0; i < nchild; ++i) {
        if (children[i] == None) continue;
        if (XGetWindowAttributes(obt_display, children[i], &attrib) &&
            attrib.map_state != IsUnmapped)
            list[count++] = (ObSurfaceId)children[i];
    }

    if (children) XFree(children);
    *n = count;
    return list;
}

static gboolean check_unmap(XEvent *e, gpointer data)
{
    const Window win = *(Window*)data;
    return ((e->type == DestroyNotify && e->xdestroywindow.window == win) ||
            (e->type == UnmapNotify && e->xunmap.window == win));
}

ObSurfaceKind x11_surface_kind(ObSurfaceId s, ObSurfaceId *icon)
{
    Window win = (Window)s;
    XWindowAttributes attrib;
    XWMHints *wmhints;
    gboolean is_dockapp = FALSE;
    Window icon_win = None;

    /* check if it has already been unmapped by the time we started
       mapping. the core grabs the server first, which syncs, so we don't
       have to here */
    if (xqueue_exists_local(check_unmap, &win)) {
        ob_debug("Trying to manage unmapped window. Aborting that.");
        return OB_SURFACE_KIND_GONE;
    }
    if (!XGetWindowAttributes(obt_display, win, &attrib))
        return OB_SURFACE_KIND_GONE;
    if (attrib.override_redirect)
        return OB_SURFACE_KIND_UNMANAGED;

    /* is the window a docking app */
    if ((wmhints = XGetWMHints(obt_display, win))) {
        if ((wmhints->flags & StateHint) &&
            wmhints->initial_state == WithdrawnState)
        {
            if (wmhints->flags & IconWindowHint)
                icon_win = wmhints->icon_window;
            is_dockapp = TRUE;
        }
        XFree(wmhints);
    }
    /* This is a new method to declare that a window is a dockapp, being
       implemented by Windowmaker, to alleviate pain in writing GTK+
       dock apps.
       http://thread.gmane.org/gmane.comp.window-managers.openbox/4881
    */
    if (!is_dockapp) {
        gchar **ss;
        if (OBT_PROP_GETSS_TYPE(win, WM_CLASS, STRING_NO_CC, &ss))
        {
            if (ss[0] && ss[1] && strcmp(ss[1], "DockApp") == 0)
                is_dockapp = TRUE;
            g_strfreev(ss);
        }
    }

    if (!is_dockapp)
        return OB_SURFACE_KIND_CLIENT;
    *icon = (ObSurfaceId)(icon_win ? icon_win : win);
    return OB_SURFACE_KIND_DOCKAPP;
}

void x11_surface_adopt(ObSurfaceId s, gboolean internal)
{
    XSetWindowAttributes attrib;
    XWindowAttributes wattrib;

    add_window(&managed, (Window)s);

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

    if (XGetWindowAttributes(obt_display, (Window)s, &wattrib))
        x11_colormap_changed((Window)s, wattrib.colormap);
}

void x11_surface_unwatch(ObSurfaceId s, gboolean internal)
{
    Window w = (Window)s;

    remove_window(managed, w);
    XSelectInput(obt_display, w, NoEventMask);
    if (!internal)
        XChangeSaveSet(obt_display, w, SetModeDelete);

    if (colormaps) {
        g_hash_table_remove(colormaps, &w);
        if (colors_owner == w)
            colors_owner = None;
    }
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

struct find_destroy_unmap {
    Window window;
    gint ignore_unmaps;
};

static gboolean find_destroy_unmap(XEvent *e, gpointer data)
{
    struct find_destroy_unmap *find = data;
    if (e->type == DestroyNotify)
        return e->xdestroywindow.window == find->window;
    if (e->type == UnmapNotify && e->xunmap.window == find->window)
        /* ignore the first $find->ignore_unmaps$ many unmap events */
        return --find->ignore_unmaps < 0;
    return FALSE;
}

gboolean x11_surface_alive(ObSurfaceId s, gint ignore_hides)
{
    struct find_destroy_unmap find;

    XSync(obt_display, FALSE); /* get all events on the server */

    find.window = (Window)s;
    find.ignore_unmaps = ignore_hides;
    return !xqueue_exists_local(find_destroy_unmap, &find);
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

    info->has_area = XGetWindowAttributes(obt_display, (Window)s, &wattrib);
    if (info->has_area) {
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

Window x11_decor_new(ObSurfaceId owner)
{
    XSetWindowAttributes attrib;
    XWindowAttributes wattrib;
    gulong mask;
    Visual *visual = NULL;
    Decor *d;

    d = g_slice_new0(Decor);
    d->owner = (Window)owner;

    /* a client with a 32-bit visual gets a frame with its visual, unless
       we're already running at 32 bit depth */
    if (x11_depth() != 32 &&
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
                             (visual ? 32 : x11_depth()), InputOutput,
                             (visual ? visual : x11_visual()),
                             mask, &attrib);

    if (!decor_frames) {
        decor_frames = g_hash_table_new(g_int_hash, g_int_equal);
        decor_owners = g_hash_table_new(g_int_hash, g_int_equal);
    }
    g_hash_table_insert(decor_frames, &d->frame, d);
    g_hash_table_insert(decor_owners, &d->owner, d);
    return d->frame;
}

void x11_decor_free(Window w)
{
    Decor *d = decor_frames ? g_hash_table_lookup(decor_frames, &w) : NULL;

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

static Colormap colormap_of(Window w)
{
    WindowColormap *c = (w && colormaps) ? g_hash_table_lookup(colormaps, &w)
                                         : NULL;
    return c ? c->colormap : None;
}

static void install_colormap(Window w, gboolean install)
{
    Colormap c = colormap_of(w);

    if (c == None) {
        if (install)
            XInstallColormap(obt_display, x11_colormap());
        else
            XUninstallColormap(obt_display, x11_colormap());
    } else {
        obt_display_ignore_errors(TRUE);
        if (install)
            XInstallColormap(obt_display, c);
        else
            XUninstallColormap(obt_display, c);
        obt_display_ignore_errors(FALSE);
    }
}

void x11_surface_install_colors(ObSurfaceId s)
{
    /* uninstall the old colormap, and install the new one */
    install_colormap(colors_owner, FALSE);
    colors_owner = (Window)s;
    install_colormap(colors_owner, TRUE);
}

/* a managed window has a new colormap (ColormapNotify) */
void x11_colormap_changed(Window w, Colormap colormap)
{
    gboolean installed;
    WindowColormap *c;

    if (colormap == colormap_of(w)) return;

    ob_debug("Setting colormap of 0x%lx: 0x%lx", w, colormap);

    if (!colormaps)
        colormaps = g_hash_table_new_full(g_int_hash, g_int_equal,
                                          NULL, g_free);
    installed = w == colors_owner;
    if (installed)
        install_colormap(w, FALSE); /* uninstall old one */
    c = g_new(WindowColormap, 1);
    c->window = w;
    c->colormap = colormap;
    g_hash_table_replace(colormaps, &c->window, c);
    if (installed)
        install_colormap(w, TRUE); /* install new one */
}

void x11_publish_surface_actions(ObSurfaceId s, ObSurfaceActions actions)
{
    gulong a[12];
    gint num = 0;

    if (actions & OB_SURFACE_ACTION_CHANGE_DESKTOP)
        a[num++] = OBT_PROP_ATOM(NET_WM_ACTION_CHANGE_DESKTOP);
    if (actions & OB_SURFACE_ACTION_SHADE)
        a[num++] = OBT_PROP_ATOM(NET_WM_ACTION_SHADE);
    if (actions & OB_SURFACE_ACTION_CLOSE)
        a[num++] = OBT_PROP_ATOM(NET_WM_ACTION_CLOSE);
    if (actions & OB_SURFACE_ACTION_MOVE)
        a[num++] = OBT_PROP_ATOM(NET_WM_ACTION_MOVE);
    if (actions & OB_SURFACE_ACTION_MINIMIZE)
        a[num++] = OBT_PROP_ATOM(NET_WM_ACTION_MINIMIZE);
    if (actions & OB_SURFACE_ACTION_RESIZE)
        a[num++] = OBT_PROP_ATOM(NET_WM_ACTION_RESIZE);
    if (actions & OB_SURFACE_ACTION_FULLSCREEN)
        a[num++] = OBT_PROP_ATOM(NET_WM_ACTION_FULLSCREEN);
    if (actions & OB_SURFACE_ACTION_MAXIMIZE) {
        a[num++] = OBT_PROP_ATOM(NET_WM_ACTION_MAXIMIZE_HORZ);
        a[num++] = OBT_PROP_ATOM(NET_WM_ACTION_MAXIMIZE_VERT);
    }
    if (actions & OB_SURFACE_ACTION_ABOVE)
        a[num++] = OBT_PROP_ATOM(NET_WM_ACTION_ABOVE);
    if (actions & OB_SURFACE_ACTION_BELOW)
        a[num++] = OBT_PROP_ATOM(NET_WM_ACTION_BELOW);
    if (actions & OB_SURFACE_ACTION_UNDECORATE)
        a[num++] = OBT_PROP_ATOM(OB_WM_ACTION_UNDECORATE);

    OBT_PROP_SETA32((Window)s, NET_WM_ALLOWED_ACTIONS, ATOM, a, num);
}

void x11_publish_surface_app(ObSurfaceId s, const ObSurfaceApp *app)
{
    Window w = (Window)s;

    OBT_PROP_SETS(w, OB_APP_ROLE, app->role);
    OBT_PROP_SETS(w, OB_APP_NAME, app->name);
    OBT_PROP_SETS(w, OB_APP_CLASS, app->class);
    OBT_PROP_SETS(w, OB_APP_GROUP_NAME, app->group_name);
    OBT_PROP_SETS(w, OB_APP_GROUP_CLASS, app->group_class);
    OBT_PROP_SETS(w, OB_APP_TITLE, app->title);
    OBT_PROP_SETS(w, OB_APP_TYPE, app->type);
}

void x11_publish_surface_hidden(ObSurfaceId s, gboolean hidden)
{
    gulong state[2];

    OBT_PROP_MSG(ob_screen, (Window)s, KDE_WM_CHANGE_STATE,
                 hidden ? IconicState : NormalState, 1, 0, 0, 0);

    state[0] = hidden ? IconicState : NormalState;
    state[1] = None;
    OBT_PROP_SETA32((Window)s, WM_STATE, WM_STATE, state, 2);
}

void x11_dockapp_adopt(ObSurfaceId icon, ObSurfaceId name, ObCanvasId dock,
                       gint x, gint y)
{
    XReparentWindow(obt_display, (Window)icon, (Window)dock, x, y);
    XChangeSaveSet(obt_display, (Window)icon, SetModeInsert);
    XMapWindow(obt_display, (Window)icon);

    if (name != icon) {
        XReparentWindow(obt_display, (Window)name, (Window)dock, -1000, -1000);
        XChangeSaveSet(obt_display, (Window)name, SetModeInsert);
        XMapWindow(obt_display, (Window)name);
    }

    XSync(obt_display, False);

    XSelectInput(obt_display, (Window)icon, DOCKAPP_EVENT_MASK);
    add_window(&dockapps, (Window)icon);
}

void x11_dockapp_release(ObSurfaceId icon, ObSurfaceId name,
                         gboolean reparent)
{
    remove_window(dockapps, (Window)icon);
    XSelectInput(obt_display, (Window)icon, NoEventMask);
    /* remove the window from our save set */
    XChangeSaveSet(obt_display, (Window)icon, SetModeDelete);
    XSync(obt_display, False);

    if (reparent) {
        XReparentWindow(obt_display, (Window)icon, obt_root(ob_screen), 0, 0);
        if (name != icon)
            XReparentWindow(obt_display, (Window)name,
                            obt_root(ob_screen), 0, 0);
    }
}
