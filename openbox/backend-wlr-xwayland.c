/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-wlr-xwayland.c for the Lumen window manager

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

/* X11 clients through Xwayland. wlroots' own X window manager speaks X to
   them; here they become surfaces the core manages like xdg toplevels,
   reading what they say about themselves from the properties the xwm
   keeps. Override-redirect windows (menus, tooltips) aren't managed: they
   show where they ask, above the windows. Xwayland starts the first time
   an X11 client connects. */

#include "backend-wlr-int.h"

#ifdef USE_XWAYLAND

#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>
#include <wlr/xwayland.h>

static struct wlr_xwayland *xwayland;
static struct wl_listener new_surface, ready;

/* the atoms the xwm doesn't name for us */
enum {
    ATOM_TYPE_DESKTOP, ATOM_TYPE_DOCK, ATOM_TYPE_TOOLBAR, ATOM_TYPE_MENU,
    ATOM_TYPE_UTILITY, ATOM_TYPE_SPLASH, ATOM_TYPE_DIALOG, ATOM_TYPE_NORMAL,
    ATOM_TAKE_FOCUS, ATOM_DELETE_WINDOW, ATOM_PING,
    NUM_ATOMS
};
static const gchar *const atom_names[NUM_ATOMS] = {
    "_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_WM_WINDOW_TYPE_DOCK",
    "_NET_WM_WINDOW_TYPE_TOOLBAR", "_NET_WM_WINDOW_TYPE_MENU",
    "_NET_WM_WINDOW_TYPE_UTILITY", "_NET_WM_WINDOW_TYPE_SPLASH",
    "_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_WINDOW_TYPE_NORMAL",
    "WM_TAKE_FOCUS", "WM_DELETE_WINDOW", "_NET_WM_PING"
};
static xcb_atom_t atoms[NUM_ATOMS];

/*! An override-redirect window: no frame, where it asks. */
typedef struct {
    WlrNodeData node;           /* first: its tree's node data */
    struct wlr_xwayland_surface *xw;
    struct wlr_scene_tree *tree;
    struct wl_listener associate, dissociate, map, unmap, destroy;
    struct wl_listener set_geometry;
} Unmanaged;

static gboolean has_atom(const xcb_atom_t *list, size_t n, guint which)
{
    xcb_atom_t a = g_atomic_int_get((gint*)&atoms[which]);
    size_t i;

    if (!a) return FALSE;
    for (i = 0; i < n; ++i)
        if (list[i] == a) return TRUE;
    return FALSE;
}

/* override-redirect windows */

static void or_set_geometry(struct wl_listener *l, void *data)
{
    Unmanaged *u = wl_container_of(l, u, set_geometry);

    if (u->tree)
        wlr_scene_node_set_position(&u->tree->node, u->xw->x, u->xw->y);
}

static void or_map(struct wl_listener *l, void *data)
{
    Unmanaged *u = wl_container_of(l, u, map);

    u->tree = wlr_scene_subsurface_tree_create(lw_layers[WLR_LAYER_POPUP],
                                               u->xw->surface);
    u->tree->node.data = &u->node;
    wlr_scene_node_set_position(&u->tree->node, u->xw->x, u->xw->y);
    u->set_geometry.notify = or_set_geometry;
    wl_signal_add(&u->xw->events.set_geometry, &u->set_geometry);
    /* a menu that types, like dmenu */
    if (wlr_xwayland_or_surface_wants_focus(u->xw))
        lw_keyboard_focus(u->xw->surface);
    lw_pointer_refresh(0);
}

static void or_unmap(struct wl_listener *l, void *data)
{
    Unmanaged *u = wl_container_of(l, u, unmap);
    WlrSurface *f;

    wl_list_remove(&u->set_geometry.link);
    if (u->tree) wlr_scene_node_destroy(&u->tree->node);
    u->tree = NULL;
    if (lw_seat->keyboard_state.focused_surface == u->xw->surface) {
        /* back to what the core focused */
        f = lw_focused_surface();
        lw_keyboard_focus(f ? lw_surface_wl(f) : NULL);
    }
    lw_pointer_refresh(0);
}

static void or_associate(struct wl_listener *l, void *data)
{
    Unmanaged *u = wl_container_of(l, u, associate);

    u->map.notify = or_map;
    wl_signal_add(&u->xw->surface->events.map, &u->map);
    u->unmap.notify = or_unmap;
    wl_signal_add(&u->xw->surface->events.unmap, &u->unmap);
}

static void or_dissociate(struct wl_listener *l, void *data)
{
    Unmanaged *u = wl_container_of(l, u, dissociate);

    wl_list_remove(&u->map.link);
    wl_list_remove(&u->unmap.link);
}

static void or_destroy(struct wl_listener *l, void *data)
{
    Unmanaged *u = wl_container_of(l, u, destroy);

    wl_list_remove(&u->associate.link);
    wl_list_remove(&u->dissociate.link);
    wl_list_remove(&u->destroy.link);
    g_free(u);
}

static void new_unmanaged(struct wlr_xwayland_surface *xw)
{
    Unmanaged *u = g_new0(Unmanaged, 1);

    /* input goes straight to it, like a layer surface's */
    u->node.kind = WLR_NODE_LAYER;
    u->xw = xw;
    u->associate.notify = or_associate;
    wl_signal_add(&xw->events.associate, &u->associate);
    u->dissociate.notify = or_dissociate;
    wl_signal_add(&xw->events.dissociate, &u->dissociate);
    u->destroy.notify = or_destroy;
    wl_signal_add(&xw->events.destroy, &u->destroy);
}

/* managed X11 clients */

static void xw_tree_destroyed(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, tree_destroy);

    wl_list_remove(&s->tree_destroy.link);
    s->tree = NULL;
}

static void xw_map(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, map);

    /* it shows once the core frames it */
    s->tree = wlr_scene_subsurface_tree_create(lw_layers[WLR_LAYER_WINDOWS],
                                               s->xw->surface);
    s->tree->node.data = s;
    wlr_scene_node_set_enabled(&s->tree->node, FALSE);
    s->tree_destroy.notify = xw_tree_destroyed;
    wl_signal_add(&s->tree->node.events.destroy, &s->tree_destroy);
    lw_surface_mapped(s);
}

static void xw_unmap(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, unmap);

    lw_surface_unmapped(s);
    if (s->tree) wlr_scene_node_destroy(&s->tree->node);
}

static void xw_associate(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, associate);

    s->map.notify = xw_map;
    wl_signal_add(&s->xw->surface->events.map, &s->map);
    s->unmap.notify = xw_unmap;
    wl_signal_add(&s->xw->surface->events.unmap, &s->unmap);
}

static void xw_dissociate(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, dissociate);

    wl_list_remove(&s->map.link);
    wl_list_remove(&s->unmap.link);
}

static void xw_request_configure(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, request_configure);
    struct wlr_xwayland_surface_configure_event *e = data;
    ObConfigureRequest req;

    if (!s->managed) {
        /* before it shows, it may be where it likes */
        wlr_xwayland_surface_configure(s->xw, e->x, e->y, e->width,
                                       e->height);
        return;
    }
    memset(&req, 0, sizeof(req));
    if (e->mask & XCB_CONFIG_WINDOW_X) req.fields |= OB_CONFIGURE_X;
    if (e->mask & XCB_CONFIG_WINDOW_Y) req.fields |= OB_CONFIGURE_Y;
    if (e->mask & XCB_CONFIG_WINDOW_WIDTH) req.fields |= OB_CONFIGURE_WIDTH;
    if (e->mask & XCB_CONFIG_WINDOW_HEIGHT)
        req.fields |= OB_CONFIGURE_HEIGHT;
    RECT_SET(req.want, e->x, e->y, e->width, e->height);
    lw_begin(0);
    lw_events->request_configure(lw_surface_id(s), &req);
    lw_end();
    /* it hears where it ended up, even if nothing moved */
    lw_xw_configure(s);
}

static void xw_request_move(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, request_move);

    lw_surface_request_move(s);
}

static void xw_request_resize(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, request_resize);
    struct wlr_xwayland_resize_event *e = data;
    /* _NET_WM_MOVERESIZE's directions, from the top left clockwise */
    static const guint32 edges[] = {
        WLR_EDGE_TOP | WLR_EDGE_LEFT, WLR_EDGE_TOP,
        WLR_EDGE_TOP | WLR_EDGE_RIGHT, WLR_EDGE_RIGHT,
        WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT, WLR_EDGE_BOTTOM,
        WLR_EDGE_BOTTOM | WLR_EDGE_LEFT, WLR_EDGE_LEFT
    };

    lw_surface_request_resize(s, e->edges < G_N_ELEMENTS(edges) ?
                              edges[e->edges] : e->edges);
}

static void xw_request_maximize(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, request_maximize);

    lw_surface_request_states(s, OB_SURFACE_STATE_MAXIMIZED_HORZ,
                              OB_SURFACE_STATE_MAXIMIZED_VERT,
                              s->xw->maximized_horz || s->xw->maximized_vert);
}

static void xw_request_fullscreen(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, request_fullscreen);

    lw_surface_request_states(s, OB_SURFACE_STATE_FULLSCREEN, 0,
                              s->xw->fullscreen);
}

static void xw_request_minimize(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, request_minimize);
    struct wlr_xwayland_minimize_event *e = data;

    if (!s->managed) return;
    lw_begin(0);
    lw_events->request_iconify(lw_surface_id(s), e->minimize);
    lw_end();
}

static void xw_request_activate(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, request_activate);

    if (!s->managed) return;
    lw_begin(0);
    lw_events->request_activate(lw_surface_id(s),
                                OB_REQUEST_FROM_APPLICATION, 0);
    lw_end();
}

static void xw_set_title(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, set_title);

    lw_surface_changed(s, OB_SURFACE_INFO_TITLE);
}

static void xw_set_class(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, set_app_id);

    lw_surface_changed(s, OB_SURFACE_INFO_CLASS);
    lw_toplevel_app_id(lw_surface_id(s), s->xw->class);
}

static void xw_set_parent(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, set_parent);

    lw_surface_changed(s, OB_SURFACE_INFO_PARENT);
}

static void xw_set_hints(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, set_hints);

    lw_surface_changed(s, OB_SURFACE_INFO_HINTS);
}

static void xw_set_decorations(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, set_decorations);

    lw_surface_changed(s, OB_SURFACE_INFO_DECOR);
}

static void xw_destroy(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, destroy);

    wl_list_remove(&s->associate.link);
    wl_list_remove(&s->dissociate.link);
    wl_list_remove(&s->destroy.link);
    wl_list_remove(&s->request_configure.link);
    wl_list_remove(&s->request_move.link);
    wl_list_remove(&s->request_resize.link);
    wl_list_remove(&s->request_maximize.link);
    wl_list_remove(&s->request_fullscreen.link);
    wl_list_remove(&s->request_minimize.link);
    wl_list_remove(&s->request_activate.link);
    wl_list_remove(&s->set_title.link);
    wl_list_remove(&s->set_app_id.link);
    wl_list_remove(&s->set_parent.link);
    wl_list_remove(&s->set_hints.link);
    wl_list_remove(&s->set_decorations.link);
    s->xw->data = NULL;
    lw_surface_free(s);
}

static void new_managed(struct wlr_xwayland_surface *xw)
{
    WlrSurface *s = lw_surface_new();

    s->xw = xw;
    xw->data = s;
    s->associate.notify = xw_associate;
    wl_signal_add(&xw->events.associate, &s->associate);
    s->dissociate.notify = xw_dissociate;
    wl_signal_add(&xw->events.dissociate, &s->dissociate);
    s->destroy.notify = xw_destroy;
    wl_signal_add(&xw->events.destroy, &s->destroy);
    s->request_configure.notify = xw_request_configure;
    wl_signal_add(&xw->events.request_configure, &s->request_configure);
    s->request_move.notify = xw_request_move;
    wl_signal_add(&xw->events.request_move, &s->request_move);
    s->request_resize.notify = xw_request_resize;
    wl_signal_add(&xw->events.request_resize, &s->request_resize);
    s->request_maximize.notify = xw_request_maximize;
    wl_signal_add(&xw->events.request_maximize, &s->request_maximize);
    s->request_fullscreen.notify = xw_request_fullscreen;
    wl_signal_add(&xw->events.request_fullscreen, &s->request_fullscreen);
    s->request_minimize.notify = xw_request_minimize;
    wl_signal_add(&xw->events.request_minimize, &s->request_minimize);
    s->request_activate.notify = xw_request_activate;
    wl_signal_add(&xw->events.request_activate, &s->request_activate);
    s->set_title.notify = xw_set_title;
    wl_signal_add(&xw->events.set_title, &s->set_title);
    s->set_app_id.notify = xw_set_class;
    wl_signal_add(&xw->events.set_class, &s->set_app_id);
    s->set_parent.notify = xw_set_parent;
    wl_signal_add(&xw->events.set_parent, &s->set_parent);
    s->set_hints.notify = xw_set_hints;
    wl_signal_add(&xw->events.set_hints, &s->set_hints);
    s->set_decorations.notify = xw_set_decorations;
    wl_signal_add(&xw->events.set_decorations, &s->set_decorations);
}

static void xwayland_new_surface(struct wl_listener *l, void *data)
{
    struct wlr_xwayland_surface *xw = data;

    if (xw->override_redirect)
        new_unmanaged(xw);
    else
        new_managed(xw);
}

/*! Atoms are the server's: look them up once, on a connection of our own.
  In a thread, as Xwayland answers only while the compositor runs. */
static gpointer intern_atoms(gpointer display)
{
    xcb_connection_t *c;
    xcb_intern_atom_cookie_t cookies[NUM_ATOMS];
    guint i;

    c = xcb_connect(display, NULL);
    if (!xcb_connection_has_error(c)) {
        for (i = 0; i < NUM_ATOMS; ++i)
            cookies[i] = xcb_intern_atom(c, FALSE, strlen(atom_names[i]),
                                         atom_names[i]);
        for (i = 0; i < NUM_ATOMS; ++i) {
            xcb_intern_atom_reply_t *r =
                xcb_intern_atom_reply(c, cookies[i], NULL);
            g_atomic_int_set((gint*)&atoms[i], r ? r->atom : XCB_ATOM_NONE);
            free(r);
        }
    }
    xcb_disconnect(c);
    g_free(display);
    return NULL;
}

static void xwayland_ready(struct wl_listener *l, void *data)
{
    wlr_xwayland_set_seat(xwayland, lw_seat);
    g_thread_unref(g_thread_new("atoms", intern_atoms,
                                g_strdup(xwayland->display_name)));
}

/* what surface.c asks */

WlrSurface *lw_xw_surface_of(struct wlr_surface *ws)
{
    struct wlr_xwayland_surface *xw =
        wlr_xwayland_surface_try_from_wlr_surface(ws);

    return xw && !xw->override_redirect ? xw->data : NULL;
}

struct wlr_surface *lw_xw_wl(WlrSurface *s)
{
    return s->xw->surface;
}

gboolean lw_xw_names(WlrSurface *s, const gchar **title,
                     const gchar **app_id)
{
    *title = s->xw->title;
    *app_id = s->xw->class;
    return TRUE;
}

void lw_xw_configure(WlrSurface *s)
{
    gint x = s->x, y = s->y;

    /* where it is on screen, which X11 clients place their menus by */
    if (s->tree)
        wlr_scene_node_coords(&s->tree->node, &x, &y);
    if (s->width > 0 && s->height > 0)
        wlr_xwayland_surface_configure(s->xw, x, y, s->width, s->height);
}

void lw_xw_activate(WlrSurface *s, gboolean active)
{
    wlr_xwayland_surface_activate(s->xw, active);
    if (active)
        wlr_xwayland_surface_restack(s->xw, NULL, XCB_STACK_MODE_ABOVE);
}

void lw_xw_close(WlrSurface *s)
{
    wlr_xwayland_surface_close(s->xw);
}

void lw_xw_kill(WlrSurface *s)
{
    if (s->xw->pid > 0) kill(s->xw->pid, SIGKILL);
}

void lw_xw_set_states(WlrSurface *s, ObSurfaceStates states)
{
    wlr_xwayland_surface_set_maximized(s->xw,
        (states & OB_SURFACE_STATE_MAXIMIZED_HORZ) &&
        (states & OB_SURFACE_STATE_MAXIMIZED_VERT));
    wlr_xwayland_surface_set_fullscreen(s->xw,
        !!(states & OB_SURFACE_STATE_FULLSCREEN));
    wlr_xwayland_surface_set_minimized(s->xw,
        !!(states & OB_SURFACE_STATE_ICONIC));
}

void lw_xw_info(WlrSurface *s, ObSurfaceInfoFields fields,
                ObSurfaceInfo *info)
{
    struct wlr_xwayland_surface *xw = s->xw;

    if (fields & OB_SURFACE_INFO_TITLE) {
        g_free(info->title);
        info->title = g_strdup(xw->title);
    }
    if (fields & OB_SURFACE_INFO_CLASS) {
        g_free(info->app_id);
        info->app_id = g_strdup(xw->class);
        g_free(info->instance);
        info->instance = g_strdup(xw->instance);
        g_free(info->role);
        info->role = g_strdup(xw->role);
    }
    if (fields & OB_SURFACE_INFO_TYPE) {
        static const ObSurfaceType types[] = {
            OB_SURFACE_TYPE_DESKTOP, OB_SURFACE_TYPE_DOCK,
            OB_SURFACE_TYPE_TOOLBAR, OB_SURFACE_TYPE_MENU,
            OB_SURFACE_TYPE_UTILITY, OB_SURFACE_TYPE_SPLASH,
            OB_SURFACE_TYPE_DIALOG, OB_SURFACE_TYPE_NORMAL
        };
        size_t i;
        guint t;

        /* the first one it names that we know */
        info->has_type = FALSE;
        for (i = 0; i < xw->window_type_len && !info->has_type; ++i)
            for (t = ATOM_TYPE_DESKTOP; t <= ATOM_TYPE_NORMAL; ++t)
                if (has_atom(&xw->window_type[i], 1, t)) {
                    info->has_type = TRUE;
                    info->type = types[t];
                    break;
                }
    }
    if (fields & OB_SURFACE_INFO_PARENT) {
        WlrSurface *p = xw->parent ? xw->parent->data : NULL;
        info->has_parent = p != NULL;
        info->parent = p ? lw_surface_id(p) : OB_SURFACE_NONE;
        info->parent_is_group = FALSE;
    }
    if ((fields & OB_SURFACE_INFO_SIZE) && xw->size_hints) {
        const xcb_size_hints_t *h = xw->size_hints;

        info->size_hints = TRUE;
        info->has_position = !!(h->flags & (XCB_ICCCM_SIZE_HINT_US_POSITION |
                                            XCB_ICCCM_SIZE_HINT_P_POSITION));
        info->has_size = !!(h->flags & (XCB_ICCCM_SIZE_HINT_US_SIZE |
                                        XCB_ICCCM_SIZE_HINT_P_SIZE));
        RECT_SET(info->want, xw->x, xw->y, xw->width, xw->height);
        if (h->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE)
            SIZE_SET(info->min, h->min_width, h->min_height);
        if ((h->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) &&
            h->max_width > 0 && h->max_height > 0)
            SIZE_SET(info->max, h->max_width, h->max_height);
        if (h->flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE)
            SIZE_SET(info->base, h->base_width, h->base_height);
        if ((h->flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC) &&
            h->width_inc > 0 && h->height_inc > 0)
            SIZE_SET(info->inc, h->width_inc, h->height_inc);
        if ((h->flags & XCB_ICCCM_SIZE_HINT_P_ASPECT) &&
            h->min_aspect_den > 0 && h->max_aspect_den > 0)
        {
            info->min_ratio = (gfloat)h->min_aspect_num / h->min_aspect_den;
            info->max_ratio = (gfloat)h->max_aspect_num / h->max_aspect_den;
        }
        if (h->flags & XCB_ICCCM_SIZE_HINT_P_WIN_GRAVITY) {
            info->has_gravity = TRUE;
            info->gravity = h->win_gravity;
        }
    } else if (fields & OB_SURFACE_INFO_SIZE) {
        info->has_size = TRUE;
        RECT_SET(info->want, xw->x, xw->y, xw->width, xw->height);
    }
    if ((fields & OB_SURFACE_INFO_HINTS) && xw->hints) {
        const xcb_icccm_wm_hints_t *h = xw->hints;

        info->hints = TRUE;
        if (h->flags & XCB_ICCCM_WM_HINT_INPUT)
            info->accepts_focus = !!h->input;
        info->urgent = !!(h->flags & XCB_ICCCM_WM_HINT_X_URGENCY);
        if (h->flags & XCB_ICCCM_WM_HINT_STATE) {
            info->has_initial_state = TRUE;
            info->starts_iconic =
                h->initial_state == XCB_ICCCM_WM_STATE_ICONIC;
        }
    }
    if (fields & OB_SURFACE_INFO_PROTOCOLS) {
        info->takes_focus = has_atom(xw->protocols, xw->protocols_len,
                                     ATOM_TAKE_FOCUS);
        info->can_close = TRUE;
        /* the xwm pings, not the core */
        info->can_ping = FALSE;
        info->pid = xw->pid;
    }
    if (fields & OB_SURFACE_INFO_DECOR) {
        if (xw->decorations & (WLR_XWAYLAND_SURFACE_DECORATIONS_NO_TITLE |
                               WLR_XWAYLAND_SURFACE_DECORATIONS_NO_BORDER))
        {
            info->decor_hints.flags = OB_MWM_FLAG_DECORATIONS;
            info->decor_hints.decorations =
                (xw->decorations & WLR_XWAYLAND_SURFACE_DECORATIONS_NO_BORDER)
                ? 0 : OB_MWM_DECOR_BORDER;
        }
    }
    if ((fields & OB_SURFACE_INFO_STRUT) && xw->strut_partial) {
        const xcb_ewmh_wm_strut_partial_t *p = xw->strut_partial;
        STRUT_PARTIAL_SET(info->strut, p->left, p->top, p->right, p->bottom,
                          p->left_start_y, p->left_end_y,
                          p->top_start_x, p->top_end_x,
                          p->right_start_y, p->right_end_y,
                          p->bottom_start_x, p->bottom_end_x);
    }
    if (fields & OB_SURFACE_INFO_STATES) {
        info->states = 0;
        if (xw->maximized_horz)
            info->states |= OB_SURFACE_STATE_MAXIMIZED_HORZ;
        if (xw->maximized_vert)
            info->states |= OB_SURFACE_STATE_MAXIMIZED_VERT;
        if (xw->fullscreen) info->states |= OB_SURFACE_STATE_FULLSCREEN;
        if (xw->minimized) info->states |= OB_SURFACE_STATE_ICONIC;
        if (xw->modal) info->states |= OB_SURFACE_STATE_MODAL;
    }
    if (fields & OB_SURFACE_INFO_STARTUP) {
        g_free(info->startup_id);
        info->startup_id = g_strdup(xw->startup_id);
    }
    if (fields & OB_SURFACE_INFO_GEOMETRY) {
        info->has_area = s->mapped;
        RECT_SET(info->area, xw->x, xw->y, MAX(xw->width, 1),
                 MAX(xw->height, 1));
        info->border = 0;
        info->argb = xw->has_alpha;
    }
}

void lw_xw_workareas(const Rect *areas, guint n)
{
    struct wlr_box *boxes;
    guint i;

    if (!xwayland || !xwayland->xwm || !n) return;
    boxes = g_new(struct wlr_box, n);
    for (i = 0; i < n; ++i) {
        boxes[i].x = areas[i].x;
        boxes[i].y = areas[i].y;
        boxes[i].width = areas[i].width;
        boxes[i].height = areas[i].height;
    }
    wlr_xwayland_set_workareas(xwayland, boxes, n);
    g_free(boxes);
}

void lw_xwayland_startup(void)
{
    xwayland = wlr_xwayland_create(lw_display, lw_compositor, TRUE);
    if (!xwayland) {
        g_message("Xwayland could not start; X11 clients won't run");
        return;
    }
    new_surface.notify = xwayland_new_surface;
    wl_signal_add(&xwayland->events.new_surface, &new_surface);
    ready.notify = xwayland_ready;
    wl_signal_add(&xwayland->events.ready, &ready);
    /* for what the user starts from here */
    g_setenv("DISPLAY", xwayland->display_name, TRUE);
    g_message("X11 clients on DISPLAY=%s", xwayland->display_name);
}

void lw_xwayland_shutdown(void)
{
    if (!xwayland) return;
    wl_list_remove(&new_surface.link);
    wl_list_remove(&ready.link);
    wlr_xwayland_destroy(xwayland);
    xwayland = NULL;
}

#else /* !USE_XWAYLAND */

void lw_xwayland_startup(void) {}
void lw_xwayland_shutdown(void) {}
WlrSurface *lw_xw_surface_of(struct wlr_surface *ws) { return NULL; }
struct wlr_surface *lw_xw_wl(WlrSurface *s) { return NULL; }
gboolean lw_xw_names(WlrSurface *s, const gchar **title,
                     const gchar **app_id) { return FALSE; }
void lw_xw_configure(WlrSurface *s) {}
void lw_xw_activate(WlrSurface *s, gboolean active) {}
void lw_xw_close(WlrSurface *s) {}
void lw_xw_kill(WlrSurface *s) {}
void lw_xw_set_states(WlrSurface *s, ObSurfaceStates states) {}
void lw_xw_info(WlrSurface *s, ObSurfaceInfoFields fields,
                ObSurfaceInfo *info) {}
void lw_xw_workareas(const Rect *areas, guint n) {}

#endif
