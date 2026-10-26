/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-wlr-surface.c for the Lumen window manager

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

/* The surfaces the core manages: xdg toplevels, and the dialogs the core
   draws itself (prompts), which it manages like any client. A toplevel's
   scene tree goes into its decoration canvas once the core frames it. */

#include "backend-wlr-int.h"

#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>

static struct wlr_xdg_shell *xdg_shell;
static struct wl_listener new_xdg_surface;
static struct wlr_xdg_decoration_manager_v1 *decorations;
static struct wl_listener new_decoration;
static void decoration_new(struct wl_listener *l, void *data);
static struct wlr_xdg_activation_v1 *activation;
static struct wl_listener request_activate;
/* the WlrSurfaces alive, to check the core's ids against */
static GHashTable *alive;
/* mapped toplevels, in the order they mapped */
static GList *mapped;
/* what the core gave the keyboard */
static WlrSurface *focused;
/* telling the core where the focus went */
static struct wl_event_source *focus_report;
static ObSurfaceId focus_from, focus_to;
/* telling the core the focused window went away */
static struct wl_event_source *revert_report;
static void focus_reverted(WlrSurface *s);

static WlrSurface *surface(ObSurfaceId id)
{
    WlrNodeData *d = (WlrNodeData*)id;

    if (!id) return NULL;
    if (d->kind == WLR_NODE_CANVAS)
        return ((WlrCanvas*)id)->dialog;
    if (!g_hash_table_contains(alive, d)) return NULL;
    return (WlrSurface*)id;
}

/*! The id the core knows s by: the canvas, for a dialog. */
static ObSurfaceId id_of(WlrSurface *s)
{
    return s->dialog ? (ObSurfaceId)s->dialog : (ObSurfaceId)s;
}

static struct wlr_scene_tree *tree_of(WlrSurface *s)
{
    return s->dialog ? s->dialog->tree : s->tree;
}

static void emit_changed(WlrSurface *s, ObSurfaceInfoFields fields)
{
    if (!s->managed) return;
    lw_begin(0);
    lw_events->surface_changed(id_of(s), fields, lw_now());
    lw_end();
}

WlrSurface *lw_surface_lookup(ObSurfaceId id)
{
    return surface(id);
}

ObSurfaceId lw_surface_id(WlrSurface *s)
{
    return id_of(s);
}

void lw_surface_changed(WlrSurface *s, ObSurfaceInfoFields fields)
{
    emit_changed(s, fields);
}

struct wlr_surface *lw_surface_wl(WlrSurface *s)
{
    if (s && s->toplevel) return s->toplevel->base->surface;
    if (s && s->xw) return lw_xw_wl(s);
    return NULL;
}

gboolean lw_surface_names(ObSurfaceId id, const gchar **title,
                          const gchar **app_id)
{
    WlrSurface *s = surface(id);

    if (s && s->toplevel) {
        *title = s->toplevel->title;
        *app_id = s->toplevel->app_id;
        return TRUE;
    }
    if (s && s->xw) return lw_xw_names(s, title, app_id);
    return FALSE;
}

struct wlr_output *lw_surface_output(ObSurfaceId id)
{
    WlrSurface *s = surface(id);
    gint x, y;

    if (!s || !tree_of(s)) return NULL;
    wlr_scene_node_coords(&tree_of(s)->node, &x, &y);
    /* where its middle is */
    return wlr_output_layout_output_at(lw_layout, x + s->width / 2,
                                       y + s->height / 2);
}

WlrSurface *lw_surface_of(struct wlr_surface *ws)
{
    struct wlr_xdg_surface *x;
    WlrSurface *s;

    if (ws && (s = lw_xw_surface_of(ws))) return s;
    while (ws) {
        ws = wlr_surface_get_root_surface(ws);
        x = wlr_xdg_surface_try_from_wlr_surface(ws);
        if (!x) return NULL;
        if (x->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
            return x->data;
        if (x->role != WLR_XDG_SURFACE_ROLE_POPUP || !x->popup)
            return NULL;
        ws = x->popup->parent;
    }
    return NULL;
}

/* the client's own requests */

static void surface_commit(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, commit);
    struct wlr_xdg_surface *x = s->toplevel->base;
    struct wlr_box g;

    if (x->initial_commit) {
        /* the client picks its size until the core decides */
        wlr_xdg_toplevel_set_size(s->toplevel, 0, 0);
        s->initial_states = 0;
        if (s->toplevel->requested.maximized)
            s->initial_states |= OB_SURFACE_STATE_MAXIMIZED_HORZ |
                OB_SURFACE_STATE_MAXIMIZED_VERT;
        if (s->toplevel->requested.fullscreen)
            s->initial_states |= OB_SURFACE_STATE_FULLSCREEN;
        if (s->toplevel->requested.minimized)
            s->initial_states |= OB_SURFACE_STATE_ICONIC;
        if (s->decoration)
            wlr_xdg_toplevel_decoration_v1_set_mode(s->decoration,
                WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        return;
    }
    if (!s->managed || !x->surface->mapped) return;

    /* new size limits, which the size may have to follow */
    if (s->toplevel->current.min_width != s->min_width ||
        s->toplevel->current.min_height != s->min_height ||
        s->toplevel->current.max_width != s->max_width ||
        s->toplevel->current.max_height != s->max_height)
    {
        s->min_width = s->toplevel->current.min_width;
        s->min_height = s->toplevel->current.min_height;
        s->max_width = s->toplevel->current.max_width;
        s->max_height = s->toplevel->current.max_height;
        emit_changed(s, OB_SURFACE_INFO_SIZE);
    }

    /* a size it picked itself, not the one it was told (the whole surface,
       for a client that never sets its geometry) */
    wlr_xdg_surface_get_geometry(x, &g);
    if (g.width != s->width || g.height != s->height)
    {
        if (x->current.configure_serial == x->scheduled_serial ||
            !x->configure_idle)
        {
            ObConfigureRequest req;

            memset(&req, 0, sizeof(req));
            req.fields = OB_CONFIGURE_WIDTH | OB_CONFIGURE_HEIGHT;
            req.want.width = g.width;
            req.want.height = g.height;
            if (req.want.width > 0 && req.want.height > 0) {
                lw_begin(0);
                lw_events->request_configure(id_of(s), &req);
                lw_end();
            }
        }
    }
}

void lw_surface_mapped(WlrSurface *s)
{
    s->mapped = TRUE;
    mapped = g_list_append(mapped, s);
    lw_begin(0);
    lw_events->surface_new(id_of(s));
    lw_end();
}

static void surface_map(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, map);
    struct wlr_box g;

    /* the size it shows with, until the core configures another */
    wlr_xdg_surface_get_geometry(s->toplevel->base, &g);
    s->width = g.width;
    s->height = g.height;
    lw_surface_mapped(s);
}

void lw_surface_unmapped(WlrSurface *s)
{
    s->mapped = FALSE;
    mapped = g_list_remove(mapped, s);
    if (s->managed) {
        lw_begin(0);
        lw_events->surface_withdrawn(id_of(s));
        lw_end();
    }
    focus_reverted(s);
}

static void surface_unmap(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, unmap);

    lw_surface_unmapped(s);
}

WlrSurface *lw_surface_new(void)
{
    WlrSurface *s = g_new0(WlrSurface, 1);

    s->node.kind = WLR_NODE_SURFACE;
    g_hash_table_add(alive, s);
    return s;
}

void lw_surface_free(WlrSurface *s)
{
    if (s->managed) {
        lw_begin(0);
        lw_events->surface_gone(id_of(s));
        lw_end();
    }
    lw_state_forget(id_of(s));
    lw_input_forget((guintptr)s);
    focus_reverted(s);
    mapped = g_list_remove(mapped, s);
    if (s->ping_check) wl_event_source_remove(s->ping_check);
    g_hash_table_remove(alive, s);
    s->node.kind = 0;
    g_free(s);
}

static void surface_destroy(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, destroy);

    wl_list_remove(&s->map.link);
    wl_list_remove(&s->unmap.link);
    wl_list_remove(&s->commit.link);
    wl_list_remove(&s->destroy.link);
    wl_list_remove(&s->request_move.link);
    wl_list_remove(&s->request_resize.link);
    wl_list_remove(&s->request_maximize.link);
    wl_list_remove(&s->request_fullscreen.link);
    wl_list_remove(&s->request_minimize.link);
    wl_list_remove(&s->set_title.link);
    wl_list_remove(&s->set_app_id.link);
    wl_list_remove(&s->set_parent.link);
    lw_surface_free(s);
}

void lw_surface_request_move(WlrSurface *s)
{
    gint x, y;

    if (!s->managed) return;
    lw_pointer_position(&x, &y);
    lw_begin(0);
    lw_events->request_move_resize(id_of(s), OB_MOVERESIZE_MOVE, x, y,
                                    lw_seat->pointer_state.grab_button);
    lw_end();
}

static void request_move(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, request_move);

    lw_surface_request_move(s);
}

void lw_surface_request_resize(WlrSurface *s, guint32 edges)
{
    ObMoveResize edge;
    gint x, y;

    if (!s->managed) return;
    switch (edges) {
    case WLR_EDGE_TOP: edge = OB_MOVERESIZE_SIZE_TOP; break;
    case WLR_EDGE_BOTTOM: edge = OB_MOVERESIZE_SIZE_BOTTOM; break;
    case WLR_EDGE_LEFT: edge = OB_MOVERESIZE_SIZE_LEFT; break;
    case WLR_EDGE_RIGHT: edge = OB_MOVERESIZE_SIZE_RIGHT; break;
    case WLR_EDGE_TOP | WLR_EDGE_LEFT: edge = OB_MOVERESIZE_SIZE_TOPLEFT; break;
    case WLR_EDGE_TOP | WLR_EDGE_RIGHT:
        edge = OB_MOVERESIZE_SIZE_TOPRIGHT; break;
    case WLR_EDGE_BOTTOM | WLR_EDGE_LEFT:
        edge = OB_MOVERESIZE_SIZE_BOTTOMLEFT; break;
    default: edge = OB_MOVERESIZE_SIZE_BOTTOMRIGHT; break;
    }
    lw_pointer_position(&x, &y);
    lw_begin(0);
    lw_events->request_move_resize(id_of(s), edge, x, y,
                                    lw_seat->pointer_state.grab_button);
    lw_end();
}

static void request_resize(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, request_resize);
    const struct wlr_xdg_toplevel_resize_event *e = data;

    lw_surface_request_resize(s, e->edges);
}

void lw_surface_request_states(WlrSurface *s, ObSurfaceStates first,
                               ObSurfaceStates second, gboolean on)
{
    if (!s->managed) {
        /* xdg-shell wants an answer to every state request */
        if (s->toplevel && s->toplevel->base->initialized)
            wlr_xdg_surface_schedule_configure(s->toplevel->base);
        return;
    }
    lw_begin(0);
    lw_events->request_states(id_of(s), on ? OB_STATE_ACTION_ADD :
                               OB_STATE_ACTION_REMOVE, first, second,
                               OB_REQUEST_FROM_APPLICATION);
    lw_end();
    /* the core may have said no */
    if (s->toplevel && s->toplevel->base->initialized)
        wlr_xdg_surface_schedule_configure(s->toplevel->base);
}

static void request_maximize(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, request_maximize);

    lw_surface_request_states(s, OB_SURFACE_STATE_MAXIMIZED_HORZ,
                   OB_SURFACE_STATE_MAXIMIZED_VERT,
                   s->toplevel->requested.maximized);
}

static void request_fullscreen(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, request_fullscreen);

    lw_surface_request_states(s, OB_SURFACE_STATE_FULLSCREEN, 0,
                   s->toplevel->requested.fullscreen);
}

static void request_minimize(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, request_minimize);

    if (!s->managed) {
        lw_surface_request_states(s, 0, 0, FALSE);
        return;
    }
    lw_begin(0);
    lw_events->request_iconify(id_of(s), TRUE);
    lw_end();
}

static void set_title(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, set_title);

    emit_changed(s, OB_SURFACE_INFO_TITLE);
}

static void set_app_id(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, set_app_id);

    emit_changed(s, OB_SURFACE_INFO_CLASS);
    lw_toplevel_app_id(id_of(s), s->toplevel->app_id);
}

static void set_parent(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, set_parent);

    emit_changed(s, OB_SURFACE_INFO_PARENT);
}

static void tree_destroyed(struct wl_listener *l, void *data)
{
    WlrSurface *s = wl_container_of(l, s, tree_destroy);

    wl_list_remove(&s->tree_destroy.link);
    s->tree = NULL;
}

static void new_toplevel(struct wlr_xdg_toplevel *t)
{
    WlrSurface *s = lw_surface_new();
    struct wlr_surface *ws = t->base->surface;

    s->toplevel = t;
    t->base->data = s;

    /* it shows once the core frames it */
    s->tree = wlr_scene_xdg_surface_create(lw_layers[WLR_LAYER_WINDOWS],
                                           t->base);
    s->tree->node.data = s;
    wlr_scene_node_set_enabled(&s->tree->node, FALSE);
    s->tree_destroy.notify = tree_destroyed;
    wl_signal_add(&s->tree->node.events.destroy, &s->tree_destroy);

    s->map.notify = surface_map;
    wl_signal_add(&ws->events.map, &s->map);
    s->unmap.notify = surface_unmap;
    wl_signal_add(&ws->events.unmap, &s->unmap);
    s->commit.notify = surface_commit;
    wl_signal_add(&ws->events.commit, &s->commit);
    s->destroy.notify = surface_destroy;
    wl_signal_add(&t->base->events.destroy, &s->destroy);
    s->request_move.notify = request_move;
    wl_signal_add(&t->events.request_move, &s->request_move);
    s->request_resize.notify = request_resize;
    wl_signal_add(&t->events.request_resize, &s->request_resize);
    s->request_maximize.notify = request_maximize;
    wl_signal_add(&t->events.request_maximize, &s->request_maximize);
    s->request_fullscreen.notify = request_fullscreen;
    wl_signal_add(&t->events.request_fullscreen, &s->request_fullscreen);
    s->request_minimize.notify = request_minimize;
    wl_signal_add(&t->events.request_minimize, &s->request_minimize);
    s->set_title.notify = set_title;
    wl_signal_add(&t->events.set_title, &s->set_title);
    s->set_app_id.notify = set_app_id;
    wl_signal_add(&t->events.set_app_id, &s->set_app_id);
    s->set_parent.notify = set_parent;
    wl_signal_add(&t->events.set_parent, &s->set_parent);
}

static void new_popup(struct wlr_xdg_popup *p)
{
    struct wlr_xdg_surface *parent =
        wlr_xdg_surface_try_from_wlr_surface(p->parent);
    struct wlr_scene_tree *parent_tree;
    WlrSurface *owner;

    if (!parent) return;
    parent_tree = parent->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL ?
        ((WlrSurface*)parent->data)->tree : parent->data;
    if (!parent_tree) return;
    p->base->data = wlr_scene_xdg_surface_create(parent_tree, p->base);

    /* keep it on its output */
    if ((owner = lw_surface_of(p->parent)) && owner->tree) {
        gint lx, ly, w, h;
        struct wlr_box box;
        int px, py;

        wlr_scene_node_coords(&parent_tree->node, &px, &py);
        lw_pointer_position(&lx, &ly);
        ob_backend->layout_size(&w, &h);
        box.x = -px;
        box.y = -py;
        box.width = w;
        box.height = h;
        wlr_xdg_popup_unconstrain_from_box(p, &box);
    }
}

static void xdg_new_surface(struct wl_listener *l, void *data)
{
    struct wlr_xdg_surface *x = data;

    if (x->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
        new_toplevel(x->toplevel);
    else if (x->role == WLR_XDG_SURFACE_ROLE_POPUP)
        new_popup(x->popup);
}

/* xdg-activation: a client brings one of its windows forward, as an
   application sends _NET_ACTIVE_WINDOW on X11 */
static void activation_request(struct wl_listener *l, void *data)
{
    const struct wlr_xdg_activation_v1_request_activate_event *e = data;
    WlrSurface *s = lw_surface_of(e->surface);

    if (!s || !s->managed) return;
    lw_begin(0);
    lw_events->request_activate(id_of(s), OB_REQUEST_FROM_APPLICATION, 0);
    lw_end();
}

void lw_surfaces_startup(void)
{
    alive = g_hash_table_new(g_direct_hash, g_direct_equal);
    xdg_shell = wlr_xdg_shell_create(lw_display, 6);
    new_xdg_surface.notify = xdg_new_surface;
    wl_signal_add(&xdg_shell->events.new_surface, &new_xdg_surface);
    decorations = wlr_xdg_decoration_manager_v1_create(lw_display);
    new_decoration.notify = decoration_new;
    wl_signal_add(&decorations->events.new_toplevel_decoration,
                  &new_decoration);
    activation = wlr_xdg_activation_v1_create(lw_display);
    request_activate.notify = activation_request;
    wl_signal_add(&activation->events.request_activate, &request_activate);
}

void lw_surfaces_shutdown(void)
{
    wl_list_remove(&new_xdg_surface.link);
    wl_list_remove(&new_decoration.link);
    wl_list_remove(&request_activate.link);
    if (focus_report) wl_event_source_remove(focus_report);
    focus_report = NULL;
    if (revert_report) wl_event_source_remove(revert_report);
    revert_report = NULL;
    g_list_free(mapped);
    mapped = NULL;
    g_hash_table_unref(alive);
    alive = NULL;
}

/* xdg-decoration: Lumen draws the decorations, so it always asks for
   them server side */

typedef struct {
    struct wlr_xdg_toplevel_decoration_v1 *d;
    struct wl_listener request_mode, destroy;
} Decoration;

static void decoration_request_mode(struct wl_listener *l, void *data)
{
    Decoration *d = wl_container_of(l, d, request_mode);

    if (d->d->toplevel->base->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(d->d,
            WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void decoration_destroy(struct wl_listener *l, void *data)
{
    Decoration *d = wl_container_of(l, d, destroy);
    WlrSurface *s = d->d->toplevel->base->data;

    if (s && s->decoration == d->d) {
        s->decoration = NULL;
        emit_changed(s, OB_SURFACE_INFO_DECOR);
    }
    wl_list_remove(&d->request_mode.link);
    wl_list_remove(&d->destroy.link);
    g_free(d);
}

static void decoration_new(struct wl_listener *l, void *data)
{
    struct wlr_xdg_toplevel_decoration_v1 *wd = data;
    Decoration *d = g_new0(Decoration, 1);
    WlrSurface *s = wd->toplevel->base->data;

    d->d = wd;
    if (s) {
        s->decoration = wd;
        emit_changed(s, OB_SURFACE_INFO_DECOR);
    }
    d->request_mode.notify = decoration_request_mode;
    wl_signal_add(&wd->events.request_mode, &d->request_mode);
    d->destroy.notify = decoration_destroy;
    wl_signal_add(&wd->events.destroy, &d->destroy);
    decoration_request_mode(&d->request_mode, NULL);
}

/* dialogs */

WlrSurface *lw_surface_new_dialog(WlrCanvas *c)
{
    WlrSurface *s = lw_surface_new();

    s->dialog = c;
    return s;
}

void lw_surface_free_dialog(WlrSurface *s)
{
    focus_reverted(s);
    if (s->managed) {
        lw_begin(0);
        lw_events->surface_gone(id_of(s));
        lw_end();
    }
    lw_state_forget(id_of(s));
    g_hash_table_remove(alive, s);
    s->node.kind = 0;
    g_free(s);
}

/* what the core asks */

ObSurfaceId *lw_existing_surfaces(guint *n)
{
    /* clients connect after the compositor starts */
    *n = 0;
    return NULL;
}

ObSurfaceKind lw_surface_kind(ObSurfaceId id, ObSurfaceId *icon)
{
    WlrSurface *s = surface(id);

    *icon = OB_SURFACE_NONE;
    if (!s) return OB_SURFACE_KIND_GONE;
    if ((s->toplevel || s->xw) && !s->mapped) return OB_SURFACE_KIND_GONE;
    return OB_SURFACE_KIND_CLIENT;
}

void lw_surface_adopt(ObSurfaceId id, gboolean internal)
{
    WlrSurface *s = surface(id);

    if (!s) return;
    s->managed = TRUE;
    s->internal = internal;
    if (s->toplevel && wl_resource_get_version(s->toplevel->resource) >=
        XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
    {
        /* what it may ask for */
        wlr_xdg_toplevel_set_wm_capabilities(s->toplevel,
            WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
            WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
            WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
    }
}

void lw_surface_unwatch(ObSurfaceId id, gboolean internal)
{
    WlrSurface *s = surface(id);

    if (s) s->managed = FALSE;
}

void lw_surface_frame(ObSurfaceId id, ObCanvasId frame)
{
    WlrSurface *s = surface(id);
    WlrCanvas *f = (WlrCanvas*)frame;
    struct wlr_scene_tree *t;

    if (!s || !(t = tree_of(s))) return;
    s->frame = f;
    wlr_scene_node_reparent(&t->node, f->content);
    /* above the decoration's own parts */
    wlr_scene_node_raise_to_top(&t->node);
    wlr_scene_node_set_position(&t->node, 0, 0);
}

void lw_surface_unframe(ObSurfaceId id, const Rect *area)
{
    WlrSurface *s = surface(id);
    struct wlr_scene_tree *t;

    if (!s) return;
    s->frame = NULL;
    if (!(t = tree_of(s))) return;
    wlr_scene_node_reparent(&t->node, lw_layers[WLR_LAYER_WINDOWS]);
    wlr_scene_node_set_position(&t->node, area->x, area->y);
    wlr_scene_node_set_enabled(&t->node, FALSE);
}

void lw_surface_release(ObSurfaceId id, gboolean exiting)
{
    WlrSurface *s = surface(id);

    if (!s) return;
    s->managed = FALSE;
    if (s->toplevel && s->toplevel->base->initialized)
        wlr_xdg_toplevel_set_activated(s->toplevel, FALSE);
    if (s->xw) lw_xw_activate(s, FALSE);
}

gboolean lw_surface_alive(ObSurfaceId id, gint ignore_hides)
{
    WlrSurface *s = surface(id);

    if (!s) return FALSE;
    return !(s->toplevel || s->xw) || s->mapped;
}

void lw_surface_configure(ObSurfaceId id, const Rect *area)
{
    WlrSurface *s = surface(id);
    struct wlr_scene_tree *t;

    if (!s) return;
    if (area) {
        s->x = area->x;
        s->y = area->y;
        if (s->dialog) {
            lw_canvas_resize((ObCanvasId)s->dialog, area->width,
                              area->height);
        } else if (s->toplevel && s->toplevel->base->initialized &&
                   (area->width != s->width || area->height != s->height))
        {
            wlr_xdg_toplevel_set_size(s->toplevel, area->width,
                                      area->height);
        }
        s->width = area->width;
        s->height = area->height;
    }
    /* the scene keeps a client's window geometry at its tree's 0, 0, past
       any shadows it draws around it */
    if ((t = tree_of(s)))
        wlr_scene_node_set_position(&t->node, s->x, s->y);
    /* X11 clients hear where they are too */
    if (s->xw) lw_xw_configure(s);
    lw_pointer_refresh(0);
}

void lw_surface_send_geometry(ObSurfaceId id, const Rect *area, gint border)
{
    WlrSurface *s = surface(id);

    /* where the core says it is, which X11 clients want to know */
    if (s && s->xw) lw_xw_configure(s);
}

void lw_surface_show(ObSurfaceId id, gboolean show)
{
    WlrSurface *s = surface(id);
    struct wlr_scene_tree *t;

    if (!s) return;
    s->shown = show;
    if ((t = tree_of(s)))
        wlr_scene_node_set_enabled(&t->node, show);
    /* a hidden window loses the focus, as an unmapped one does on X11 */
    if (!show) focus_reverted(s);
    if (s->toplevel && s->toplevel->base->initialized &&
        wl_resource_get_version(s->toplevel->resource) >=
        XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION)
        wlr_xdg_toplevel_set_suspended(s->toplevel, !show);
    lw_pointer_refresh(0);
}

WlrSurface *lw_focused_surface(void)
{
    return focused;
}

/* the focus moves when the core says, and the core hears it moved after
   that, as it does on X11 from FocusIn and FocusOut */

static void report_focus(void *data)
{
    focus_report = NULL;
    lw_begin(0);
    if (focus_from && focus_from != focus_to && surface(focus_from))
        lw_events->focus_left(focus_from);
    lw_events->focus_changed(surface(focus_to) ? focus_to : OB_SURFACE_NONE);
    lw_end();
    focus_from = focus_to;
}

static void report_revert(void *data)
{
    revert_report = NULL;
    /* unless the core gave it to another meanwhile */
    if (focused) return;
    lw_begin(0);
    lw_events->focus_lost(FALSE);
    lw_end();
}

/*! The focused window went away. On X11 the focus reverts to the root and
  the core hears it from FocusOut; here it hears it the same way, once the
  window is let go. */
static void focus_reverted(WlrSurface *s)
{
    if (focused != s) return;
    focused = NULL;
    if (!revert_report && lw_display)
        revert_report = wl_event_loop_add_idle(
            wl_display_get_event_loop(lw_display), report_revert, NULL);
}

gboolean lw_surface_focus(ObSurfaceId id, guint32 time, gboolean input,
                          gboolean take_focus)
{
    WlrSurface *s = surface(id);

    if (id && !s) return FALSE;
    if (focused && focused != s && focused->xw)
        lw_xw_activate(focused, FALSE);
    if (s && s->xw) lw_xw_activate(s, TRUE);
    focused = s;
    /* the core's own dialogs take their keys through the core */
    lw_keyboard_focus(s && input ? lw_surface_wl(s) : NULL);
    focus_to = id;
    if (!focus_report)
        focus_report = wl_event_loop_add_idle(
            wl_display_get_event_loop(lw_display), report_focus, NULL);
    return TRUE;
}

void lw_surface_close(ObSurfaceId id, guint32 time)
{
    WlrSurface *s = surface(id);

    if (s && s->toplevel)
        wlr_xdg_toplevel_send_close(s->toplevel);
    else if (s && s->xw)
        lw_xw_close(s);
}

void lw_surface_kill(ObSurfaceId id)
{
    WlrSurface *s = surface(id);

    if (s && s->toplevel)
        wl_client_destroy(wl_resource_get_client(s->toplevel->resource));
    else if (s && s->xw)
        lw_xw_kill(s);
}

void lw_surface_set_states(ObSurfaceId id, ObSurfaceStates states)
{
    WlrSurface *s = surface(id);
    struct wlr_xdg_toplevel *t;

    if (!s) return;
    s->states = states;
    lw_toplevel_states(id, states);
    if (s->xw) lw_xw_set_states(s, states);
    if (!(t = s->toplevel) || !t->base->initialized) return;
    wlr_xdg_toplevel_set_maximized(t,
        (states & OB_SURFACE_STATE_MAXIMIZED_HORZ) &&
        (states & OB_SURFACE_STATE_MAXIMIZED_VERT));
    wlr_xdg_toplevel_set_fullscreen(t, !!(states & OB_SURFACE_STATE_FULLSCREEN));
    wlr_xdg_toplevel_set_activated(t, !!(states & OB_SURFACE_STATE_FOCUSED));
}

/* pings: wlroots answers pongs itself and only reports a timeout, so look
   for the answer a few times */

static int ping_check(void *data)
{
    WlrSurface *s = data;

    if (!s->toplevel) return 0;
    if (s->toplevel->base->client->ping_serial == 0) {
        wl_event_source_remove(s->ping_check);
        s->ping_check = NULL;
        if (s->managed) {
            lw_begin(0);
            lw_events->pong(id_of(s), s->ping_serial);
            lw_end();
        }
        return 0;
    }
    wl_event_source_timer_update(s->ping_check, 100);
    return 0;
}

void lw_surface_ping(ObSurfaceId id, guint32 serial)
{
    WlrSurface *s = surface(id);

    if (!s) return;
    if (!s->toplevel) {
        /* the core's own dialogs always answer, and X11 clients are
           pinged by Xwayland's window manager */
        lw_begin(0);
        lw_events->pong(id, serial);
        lw_end();
        return;
    }
    s->ping_serial = serial;
    wlr_xdg_surface_ping(s->toplevel->base);
    if (!s->ping_check)
        s->ping_check = wl_event_loop_add_timer(
            wl_display_get_event_loop(lw_display), ping_check, s);
    wl_event_source_timer_update(s->ping_check, 100);
}

static void info_dialog(WlrSurface *s, ObSurfaceInfoFields fields,
                        ObSurfaceInfo *info)
{
    WlrCanvas *c = s->dialog;

    if (fields & OB_SURFACE_INFO_TITLE) {
        g_free(info->title);
        info->title = g_strdup(c->title);
    }
    if (fields & OB_SURFACE_INFO_CLASS) {
        g_free(info->app_id);
        info->app_id = g_strdup("Lumen");
        g_free(info->instance);
        info->instance = g_strdup("prompt");
    }
    if (fields & OB_SURFACE_INFO_TYPE) {
        info->has_type = TRUE;
        info->type = OB_SURFACE_TYPE_DIALOG;
    }
    if (fields & OB_SURFACE_INFO_PARENT) {
        info->has_parent = c->dialog_parent != OB_SURFACE_NONE;
        info->parent = c->dialog_parent;
        info->parent_is_group = FALSE;
    }
    if (fields & OB_SURFACE_INFO_SIZE) {
        info->size_hints = TRUE;
        info->has_size = TRUE;
        RECT_SET(info->want, 0, 0, c->dialog_width, c->dialog_height);
        SIZE_SET(info->min, c->dialog_width, c->dialog_height);
        SIZE_SET(info->max, c->dialog_width, c->dialog_height);
        SIZE_SET(info->base, -1, -1);
        SIZE_SET(info->inc, 1, 1);
    }
    if (fields & OB_SURFACE_INFO_STATES)
        info->states = c->dialog_modal ? OB_SURFACE_STATE_MODAL : 0;
    if (fields & OB_SURFACE_INFO_GEOMETRY) {
        info->has_area = TRUE;
        RECT_SET(info->area, c->x, c->y, c->dialog_width, c->dialog_height);
        info->border = 0;
    }
}

void lw_surface_info(ObSurfaceId id, ObSurfaceInfoFields fields,
                      ObSurfaceInfo *info)
{
    WlrSurface *s = surface(id);
    struct wlr_xdg_toplevel *t;

    if (fields & OB_SURFACE_INFO_HINTS) {
        info->hints = FALSE;
        info->accepts_focus = TRUE;
        info->urgent = FALSE;
        info->has_initial_state = FALSE;
        info->group = OB_SURFACE_NONE;
    }
    if (fields & OB_SURFACE_INFO_PROTOCOLS) {
        info->takes_focus = FALSE;
        info->can_close = TRUE;
        info->can_ping = TRUE;
        info->sync_resize = FALSE;
        info->pid = 0;
    }
    if (fields & OB_SURFACE_INFO_SIZE) {
        info->size_hints = FALSE;
        info->has_position = info->has_size = FALSE;
        SIZE_SET(info->min, 0, 0);
        SIZE_SET(info->max, G_MAXINT, G_MAXINT);
        SIZE_SET(info->base, -1, -1);
        SIZE_SET(info->inc, 1, 1);
        info->min_ratio = info->max_ratio = 0;
        info->has_gravity = FALSE;
    }
    if (fields & OB_SURFACE_INFO_TYPE) {
        info->has_type = FALSE;
        info->type_override = FALSE;
    }
    if (fields & OB_SURFACE_INFO_DECOR) {
        memset(&info->decor_hints, 0, sizeof(info->decor_hints));
        info->undecorated = FALSE;
    }
    if (fields & OB_SURFACE_INFO_STRUT) {
        memset(&info->strut, 0, sizeof(info->strut));
        info->strut_whole_edges = FALSE;
    }
    if (fields & OB_SURFACE_INFO_DESKTOP) info->has_desktop = FALSE;
    if (fields & OB_SURFACE_INFO_USER_TIME) info->has_user_time = FALSE;
    if (fields & OB_SURFACE_INFO_SYNC) info->sync_counter = 0;
    if (fields & OB_SURFACE_INFO_OPACITY) info->has_opacity = FALSE;
    if (fields & OB_SURFACE_INFO_SHAPE)
        info->shaped = info->shaped_input = FALSE;
    if (fields & OB_SURFACE_INFO_ICON_AREA)
        RECT_SET(info->icon_area, 0, 0, 0, 0);
    if (fields & OB_SURFACE_INFO_STATES) info->states = 0;
    if (fields & OB_SURFACE_INFO_GEOMETRY) info->has_area = FALSE;

    if (!s) return;
    if (s->dialog) {
        info_dialog(s, fields, info);
        return;
    }
    if (s->xw) {
        lw_xw_info(s, fields, info);
        return;
    }
    t = s->toplevel;

    if (fields & OB_SURFACE_INFO_TITLE) {
        g_free(info->title);
        info->title = g_strdup(t->title);
    }
    if (fields & OB_SURFACE_INFO_CLASS) {
        g_free(info->app_id);
        info->app_id = g_strdup(t->app_id);
    }
    if (fields & OB_SURFACE_INFO_TYPE) {
        info->has_type = TRUE;
        info->type = t->parent ? OB_SURFACE_TYPE_DIALOG :
            OB_SURFACE_TYPE_NORMAL;
    }
    if (fields & OB_SURFACE_INFO_PARENT) {
        WlrSurface *p = t->parent ? t->parent->base->data : NULL;
        info->has_parent = p != NULL;
        info->parent = p ? id_of(p) : OB_SURFACE_NONE;
        info->parent_is_group = FALSE;
    }
    if (fields & OB_SURFACE_INFO_SIZE) {
        struct wlr_box g;
        gint minw = t->current.min_width, minh = t->current.min_height;
        gint maxw = t->current.max_width, maxh = t->current.max_height;

        wlr_xdg_surface_get_geometry(t->base, &g);
        info->size_hints = TRUE;
        info->has_size = g.width > 0 && g.height > 0;
        RECT_SET(info->want, 0, 0, g.width, g.height);
        SIZE_SET(info->min, MAX(minw, 0), MAX(minh, 0));
        SIZE_SET(info->max, maxw > 0 ? maxw : G_MAXINT,
                 maxh > 0 ? maxh : G_MAXINT);
    }
    if (fields & OB_SURFACE_INFO_PROTOCOLS) {
        pid_t pid;
        wl_client_get_credentials(wl_resource_get_client(t->resource),
                                  &pid, NULL, NULL);
        info->pid = pid;
    }
    if ((fields & OB_SURFACE_INFO_DECOR) && !s->decoration) {
        /* without xdg-decoration it draws its own, like a client with
           Motif hints for no decorations */
        info->decor_hints.flags = OB_MWM_FLAG_DECORATIONS;
        info->decor_hints.decorations = 0;
    }
    if (fields & OB_SURFACE_INFO_STATES)
        info->states = s->initial_states;
    if (fields & OB_SURFACE_INFO_GEOMETRY) {
        struct wlr_box g;
        wlr_xdg_surface_get_geometry(t->base, &g);
        info->has_area = s->mapped;
        RECT_SET(info->area, 0, 0, MAX(g.width, 1), MAX(g.height, 1));
        info->border = 0;
        info->argb = TRUE;
    }
}

/* stacking */

static struct wlr_scene_node *stack_node(guintptr id)
{
    WlrNodeData *d = (WlrNodeData*)id;
    struct wlr_scene_tree *t = NULL;

    if (!id) return NULL;
    if (d->kind == WLR_NODE_CANVAS) {
        WlrCanvas *c = (WlrCanvas*)id;
        /* a dialog's canvas inside its frame stacks with the frame */
        if (c->dialog && c->dialog->frame) c = c->dialog->frame;
        t = c->tree;
    } else if (d->kind == WLR_NODE_SURFACE &&
               g_hash_table_contains(alive, d))
    {
        WlrSurface *s = (WlrSurface*)id;
        t = s->frame ? s->frame->tree : tree_of(s);
    }
    if (!t || t->node.parent != lw_layers[WLR_LAYER_WINDOWS]) return NULL;
    return &t->node;
}

void lw_restack(guintptr above, const guintptr *top_first, guint n)
{
    struct wlr_scene_node *prev = stack_node(above);
    guint i;

    for (i = 0; i < n; ++i) {
        struct wlr_scene_node *node = stack_node(top_first[i]);

        if (!node || node == prev) continue;
        if (prev)
            wlr_scene_node_place_below(node, prev);
        else
            wlr_scene_node_raise_to_top(node);
        prev = node;
    }
    lw_pointer_refresh(0);
}
