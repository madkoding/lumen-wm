/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-wlr-layer.c for the Lumen window manager

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

/* layer-shell: the bars, backgrounds, launchers and lock screens other
   programs put on an output. The core doesn't manage them: the backend
   arranges them by their anchors, and their exclusive zones reach the core
   as reserved edges, like the struts of docks on X11. */

#include "backend-wlr-int.h"

#include <string.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>

typedef struct {
    WlrNodeData node;           /* first: the trees' node data */
    struct wlr_layer_surface_v1 *wlr;
    struct wlr_scene_layer_surface_v1 *scene;
    /* its popups, in the popup layer, where it sits */
    struct wlr_scene_tree *popups;
    struct wl_listener map, unmap, commit, destroy, new_popup;
    struct wl_listener output_destroy;
} WlrLayerSurface;

static struct wlr_layer_shell_v1 *shell;
static struct wl_listener new_surface;
/* background, bottom, top and overlay, around the canvas layers */
static struct wlr_scene_tree *trees[4];
static GList *layers;           /* WlrLayerSurface */
static GArray *reserved;        /* StrutPartial, for the core */
/* the layer surface that has the keyboard; exclusive: it keeps it until it
   lets go */
static struct wlr_surface *keyboard;
static gboolean keyboard_exclusive;

static void take_keyboard(WlrLayerSurface *ls, gboolean exclusive)
{
    keyboard = ls ? ls->wlr->surface : NULL;
    keyboard_exclusive = ls && exclusive;
    lw_keyboard_refocus();
}

struct wlr_surface *lw_layer_keyboard(gboolean *exclusive)
{
    if (exclusive) *exclusive = keyboard_exclusive;
    return keyboard;
}

void lw_layer_clicked(struct wlr_surface *s)
{
    struct wlr_layer_surface_v1 *l;
    GList *it;

    if (!s) {
        /* elsewhere: a launcher that took the keyboard on demand lets go */
        if (keyboard && !keyboard_exclusive) take_keyboard(NULL, FALSE);
        return;
    }
    s = wlr_surface_get_root_surface(s);
    if (!(l = wlr_layer_surface_v1_try_from_wlr_surface(s)) ||
        l->current.keyboard_interactive ==
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE ||
        keyboard_exclusive)
        return;
    for (it = layers; it; it = g_list_next(it)) {
        WlrLayerSurface *ls = it->data;
        if (ls->wlr == l) take_keyboard(ls, FALSE);
    }
}

guint lw_reserved_edges(const StrutPartial **edges)
{
    *edges = reserved ? (const StrutPartial*)reserved->data : NULL;
    return reserved ? reserved->len : 0;
}

/* arranging */

static void place_popups(WlrLayerSurface *ls)
{
    gint x, y;

    wlr_scene_node_coords(&ls->scene->tree->node, &x, &y);
    wlr_scene_node_set_position(&ls->popups->node, x, y);
}

/*! What the exclusive zones left of an output, as struts from the edges of
  the layout. */
static gboolean reserve(const struct wlr_box *full,
                        const struct wlr_box *usable, StrutPartial *s)
{
    gint width, height;

    ob_backend->layout_size(&width, &height);
    memset(s, 0, sizeof(*s));
    if (usable->x > full->x) {
        s->left = usable->x;
        s->left_start = full->y;
        s->left_end = full->y + full->height - 1;
    }
    if (usable->y > full->y) {
        s->top = usable->y;
        s->top_start = full->x;
        s->top_end = full->x + full->width - 1;
    }
    if (usable->x + usable->width < full->x + full->width) {
        s->right = width - (usable->x + usable->width);
        s->right_start = full->y;
        s->right_end = full->y + full->height - 1;
    }
    if (usable->y + usable->height < full->y + full->height) {
        s->bottom = height - (usable->y + usable->height);
        s->bottom_start = full->x;
        s->bottom_end = full->x + full->width - 1;
    }
    return s->left || s->top || s->right || s->bottom;
}

static void arrange(void)
{
    struct wlr_output_layout_output *lo;
    GArray *was = reserved;
    GList *it;

    reserved = g_array_new(FALSE, FALSE, sizeof(StrutPartial));
    wl_list_for_each(lo, &lw_layout->outputs, link) {
        struct wlr_box full, usable;
        StrutPartial s;
        gint layer;

        wlr_output_layout_get_box(lw_layout, lo->output, &full);
        usable = full;
        /* the top ones first: their zones push the ones under them in */
        for (layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
             layer >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND; --layer)
            for (it = layers; it; it = g_list_next(it)) {
                WlrLayerSurface *ls = it->data;
                if (ls->wlr->output == lo->output &&
                    ls->wlr->current.layer == (guint)layer &&
                    ls->wlr->surface->mapped)
                    wlr_scene_layer_surface_v1_configure(ls->scene, &full,
                                                         &usable);
            }
        if (reserve(&full, &usable, &s))
            g_array_append_val(reserved, s);
    }
    for (it = layers; it; it = g_list_next(it))
        place_popups(it->data);

    if (!was || was->len != reserved->len ||
        memcmp(was->data, reserved->data, was->len * sizeof(StrutPartial)))
    {
        lw_begin(0);
        lw_events->reserved_changed();
        lw_end();
    }
    if (was) g_array_free(was, TRUE);
}

/* the client's own requests */

static void layer_commit(struct wl_listener *l, void *data)
{
    WlrLayerSurface *ls = wl_container_of(l, ls, commit);
    struct wlr_layer_surface_v1 *w = ls->wlr;

    if (!w->initialized) return;
    if (w->initial_commit) {
        /* the first configure, before it shows */
        struct wlr_box full, usable;

        wlr_output_layout_get_box(lw_layout, w->output, &full);
        usable = full;
        wlr_scene_layer_surface_v1_configure(ls->scene, &full, &usable);
        return;
    }
    if (w->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER)
        wlr_scene_node_reparent(&ls->scene->tree->node,
                                trees[w->current.layer]);
    if (w->current.committed & WLR_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY)
    {
        if (w->current.keyboard_interactive ==
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE &&
            w->surface->mapped)
            take_keyboard(ls, TRUE);
        else if (keyboard == w->surface &&
                 w->current.keyboard_interactive ==
                 ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
            take_keyboard(NULL, FALSE);
    }
    if (w->current.committed && w->surface->mapped)
        arrange();
}

static void layer_map(struct wl_listener *l, void *data)
{
    WlrLayerSurface *ls = wl_container_of(l, ls, map);
    struct wlr_layer_surface_v1 *w = ls->wlr;

    arrange();
    /* launchers and lock screens */
    if (w->current.keyboard_interactive ==
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE &&
        w->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
        take_keyboard(ls, TRUE);
    lw_pointer_refresh(0);
}

static void layer_unmap(struct wl_listener *l, void *data)
{
    WlrLayerSurface *ls = wl_container_of(l, ls, unmap);

    if (keyboard == ls->wlr->surface) take_keyboard(NULL, FALSE);
    arrange();
    lw_pointer_refresh(0);
}

static void layer_new_popup(struct wl_listener *l, void *data)
{
    WlrLayerSurface *ls = wl_container_of(l, ls, new_popup);
    struct wlr_xdg_popup *p = data;
    struct wlr_box box;
    gint x, y;

    p->base->data = wlr_scene_xdg_surface_create(ls->popups, p->base);
    place_popups(ls);
    /* keep it on the output */
    wlr_scene_node_coords(&ls->scene->tree->node, &x, &y);
    wlr_output_layout_get_box(lw_layout, ls->wlr->output, &box);
    box.x -= x;
    box.y -= y;
    wlr_xdg_popup_unconstrain_from_box(p, &box);
}

static void layer_destroy(struct wl_listener *l, void *data)
{
    WlrLayerSurface *ls = wl_container_of(l, ls, destroy);

    if (keyboard == ls->wlr->surface) take_keyboard(NULL, FALSE);
    wl_list_remove(&ls->map.link);
    wl_list_remove(&ls->unmap.link);
    wl_list_remove(&ls->commit.link);
    wl_list_remove(&ls->destroy.link);
    wl_list_remove(&ls->new_popup.link);
    wl_list_remove(&ls->output_destroy.link);
    wlr_scene_node_destroy(&ls->popups->node);
    layers = g_list_remove(layers, ls);
    g_free(ls);
    arrange();
}

static void layer_output_destroy(struct wl_listener *l, void *data)
{
    WlrLayerSurface *ls = wl_container_of(l, ls, output_destroy);

    /* its output went away, and it goes with it */
    wl_list_remove(&ls->output_destroy.link);
    wl_list_init(&ls->output_destroy.link);
    ls->wlr->output = NULL;
    wlr_layer_surface_v1_destroy(ls->wlr);
}

static void layer_new(struct wl_listener *l, void *data)
{
    struct wlr_layer_surface_v1 *w = data;
    WlrLayerSurface *ls;

    if (!w->output) {
        /* where the pointer is */
        gint x, y;
        lw_pointer_position(&x, &y);
        w->output = wlr_output_layout_output_at(lw_layout, x, y);
        if (!w->output)
            w->output = wlr_output_layout_get_center_output(lw_layout);
    }
    if (!w->output) {
        wlr_layer_surface_v1_destroy(w);
        return;
    }

    ls = g_new0(WlrLayerSurface, 1);
    ls->node.kind = WLR_NODE_LAYER;
    ls->wlr = w;
    w->data = ls;
    ls->scene = wlr_scene_layer_surface_v1_create(trees[w->pending.layer], w);
    ls->scene->tree->node.data = &ls->node;
    ls->popups = wlr_scene_tree_create(lw_layers[WLR_LAYER_POPUP]);
    ls->popups->node.data = &ls->node;

    ls->map.notify = layer_map;
    wl_signal_add(&w->surface->events.map, &ls->map);
    ls->unmap.notify = layer_unmap;
    wl_signal_add(&w->surface->events.unmap, &ls->unmap);
    ls->commit.notify = layer_commit;
    wl_signal_add(&w->surface->events.commit, &ls->commit);
    ls->destroy.notify = layer_destroy;
    wl_signal_add(&w->events.destroy, &ls->destroy);
    ls->new_popup.notify = layer_new_popup;
    wl_signal_add(&w->events.new_popup, &ls->new_popup);
    ls->output_destroy.notify = layer_output_destroy;
    wl_signal_add(&w->output->events.destroy, &ls->output_destroy);
    layers = g_list_append(layers, ls);
}

void lw_layers_startup(void)
{
    struct wlr_scene_tree *root = &lw_scene->tree;

    shell = wlr_layer_shell_v1_create(lw_display, 4);
    new_surface.notify = layer_new;
    wl_signal_add(&shell->events.new_surface, &new_surface);

    /* backgrounds under everything, bars above the windows and under the
       core's panels, overlays (lock screens) on top */
    trees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] = wlr_scene_tree_create(root);
    wlr_scene_node_place_below(&trees[0]->node,
                               &lw_layers[WLR_LAYER_DESKTOP]->node);
    trees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] = wlr_scene_tree_create(root);
    wlr_scene_node_place_above(&trees[1]->node,
                               &lw_layers[WLR_LAYER_DESKTOP]->node);
    trees[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = wlr_scene_tree_create(root);
    wlr_scene_node_place_above(&trees[2]->node,
                               &lw_layers[WLR_LAYER_WINDOWS]->node);
    trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = wlr_scene_tree_create(root);
    wlr_scene_node_place_above(&trees[3]->node,
                               &lw_layers[WLR_LAYER_OVERLAY]->node);
}

void lw_layers_shutdown(void)
{
    wl_list_remove(&new_surface.link);
    keyboard = NULL;
    if (reserved) g_array_free(reserved, TRUE);
    reserved = NULL;
}
