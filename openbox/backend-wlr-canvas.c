/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-wlr-canvas.c for the Lumen window manager

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

/* Canvases as scene trees. A canvas is placed like an X window: x, y is
   the outside corner of its border, and what it shows (and its children,
   and a decoration's client) sits inside the border. */

#include "backend-wlr-int.h"

#include <string.h>
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>

/* a picture the core drew, as a buffer the scene can show */

typedef struct {
    struct wlr_buffer base;
    guint8 *data;
    gsize stride;
} ImageBuffer;

static void image_destroy(struct wlr_buffer *b)
{
    ImageBuffer *ib = wl_container_of(b, ib, base);

    g_free(ib->data);
    g_free(ib);
}

static bool image_begin_access(struct wlr_buffer *b, uint32_t flags,
                               void **data, uint32_t *format, size_t *stride)
{
    ImageBuffer *ib = wl_container_of(b, ib, base);

    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
    *data = ib->data;
    /* cairo's premultiplied ARGB32 */
    *format = DRM_FORMAT_ARGB8888;
    *stride = ib->stride;
    return true;
}

static void image_end_access(struct wlr_buffer *b)
{
}

static const struct wlr_buffer_impl image_impl = {
    .destroy = image_destroy,
    .begin_data_ptr_access = image_begin_access,
    .end_data_ptr_access = image_end_access,
};

static struct wlr_buffer *image_buffer(const ObImage *img)
{
    ImageBuffer *ib = g_new0(ImageBuffer, 1);
    gint y;

    wlr_buffer_init(&ib->base, &image_impl, img->width, img->height);
    ib->stride = img->width * 4;
    ib->data = g_malloc(ib->stride * img->height);
    for (y = 0; y < img->height; ++y)
        memcpy(ib->data + y * ib->stride,
               (const guint8*)img->data + y * img->stride, ib->stride);
    return &ib->base;
}

/* canvases */

static WlrCanvas *canvas(ObCanvasId c)
{
    WlrCanvas *w = (WlrCanvas*)c;

    g_assert(w && w->node.kind == WLR_NODE_CANVAS);
    return w;
}

WlrLayer lw_canvas_layer(ObCanvasLayer layer)
{
    switch (layer) {
    case OB_CANVAS_DECOR:
    case OB_CANVAS_DIALOG:
        return WLR_LAYER_WINDOWS;
    case OB_CANVAS_PANEL:
        return WLR_LAYER_PANEL;
    case OB_CANVAS_POPUP:
        return WLR_LAYER_POPUP;
    case OB_CANVAS_MENU:
        return WLR_LAYER_MENU;
    case OB_CANVAS_OVERLAY:
        return WLR_LAYER_OVERLAY;
    }
    g_assert_not_reached();
    return WLR_LAYER_OVERLAY;
}

/*! Where what it shows begins: inside its border. */
static struct wlr_scene_tree *content(WlrCanvas *c)
{
    return c->content;
}

static WlrCanvas *canvas_make(struct wlr_scene_tree *parent,
                              ObCanvasLayer layer, ObCanvasFlags flags)
{
    WlrCanvas *c = g_new0(WlrCanvas, 1);

    c->node.kind = WLR_NODE_CANVAS;
    c->layer = layer;
    c->flags = flags;
    c->tree = wlr_scene_tree_create(parent);
    c->tree->node.data = c;
    c->content = wlr_scene_tree_create(c->tree);
    c->content->node.data = c;
    c->width = c->height = 1;
    c->cursor = OB_CURSOR_NONE;
    /* like an X window, it shows once it is asked to */
    wlr_scene_node_set_enabled(&c->tree->node, FALSE);
    return c;
}

ObCanvasId lw_canvas_new(ObCanvasLayer layer, ObSurfaceId owner,
                          ObCanvasFlags flags)
{
    WlrCanvas *c = canvas_make(lw_layers[lw_canvas_layer(layer)], layer,
                               flags);

    if (owner)
        c->owner = (WlrSurface*)owner;
    if (layer == OB_CANVAS_DIALOG)
        c->dialog = lw_surface_new_dialog(c);
    return (ObCanvasId)c;
}

ObCanvasId lw_canvas_new_child(ObCanvasId parent, ObCanvasFlags flags)
{
    WlrCanvas *p = canvas(parent);
    WlrCanvas *c = canvas_make(content(p), p->layer, flags);

    c->parent = p;
    p->children = g_slist_append(p->children, c);
    return (ObCanvasId)c;
}

void lw_canvas_free(ObCanvasId id)
{
    WlrCanvas *c = canvas(id);

    while (c->children)
        lw_canvas_free((ObCanvasId)c->children->data);
    if (c->parent)
        c->parent->children = g_slist_remove(c->parent->children, c);
    lw_input_forget(id);
    if (c->dialog) {
        lw_surface_free_dialog(c->dialog);
        c->dialog = NULL;
    }
    /* a client still inside a decoration being freed is moved out first by
       surface_unframe(); destroying the tree destroys what is left */
    wlr_scene_node_destroy(&c->tree->node);
    if (c->input) g_array_free(c->input, TRUE);
    g_free(c->title);
    c->node.kind = 0;
    g_free(c);
}

gboolean lw_canvas_has_alpha(ObCanvasId c)
{
    /* everything composites */
    return !!(canvas(c)->flags & OB_CANVAS_ALPHA);
}

static void color_of(guint32 argb, float color[4])
{
    gfloat a = ((argb >> 24) & 0xff) / 255.0;

    /* the scene takes premultiplied colors */
    color[0] = ((argb >> 16) & 0xff) / 255.0 * a;
    color[1] = ((argb >> 8) & 0xff) / 255.0 * a;
    color[2] = (argb & 0xff) / 255.0 * a;
    color[3] = a;
}

static void place_border(WlrCanvas *c)
{
    gint b = c->border_width;
    gint w = c->width + 2 * b, h = c->height + 2 * b;
    float color[4];
    gint i;

    if (!b) {
        for (i = 0; i < 4; ++i)
            if (c->border[i]) {
                wlr_scene_node_destroy(&c->border[i]->node);
                c->border[i] = NULL;
            }
        wlr_scene_node_set_position(&c->content->node, 0, 0);
        return;
    }

    color_of(c->border_argb, color);
    for (i = 0; i < 4; ++i) {
        if (!c->border[i]) {
            c->border[i] = wlr_scene_rect_create(c->tree, 1, 1, color);
            c->border[i]->node.data = c;
            /* below what it shows */
            wlr_scene_node_lower_to_bottom(&c->border[i]->node);
        }
        wlr_scene_rect_set_color(c->border[i], color);
    }
    /* top, bottom, left, right */
    wlr_scene_rect_set_size(c->border[0], w, b);
    wlr_scene_node_set_position(&c->border[0]->node, 0, 0);
    wlr_scene_rect_set_size(c->border[1], w, b);
    wlr_scene_node_set_position(&c->border[1]->node, 0, h - b);
    wlr_scene_rect_set_size(c->border[2], b, h - 2 * b);
    wlr_scene_node_set_position(&c->border[2]->node, 0, b);
    wlr_scene_rect_set_size(c->border[3], b, h - 2 * b);
    wlr_scene_node_set_position(&c->border[3]->node, w - b, b);
    wlr_scene_node_set_position(&c->content->node, b, b);
}

void lw_canvas_place(ObCanvasId id, const Rect *outer, const Rect *inner)
{
    WlrCanvas *c = canvas(id);

    if (outer) {
        lw_canvas_move(id, outer->x, outer->y);
        lw_canvas_resize(id, outer->width, outer->height);
    }
    if (inner && c->owner && c->owner->frame == c) {
        c->owner->x = inner->x;
        c->owner->y = inner->y;
        lw_surface_configure((ObSurfaceId)c->owner, NULL);
    }
}

void lw_canvas_move(ObCanvasId id, gint x, gint y)
{
    WlrCanvas *c = canvas(id);

    c->x = x;
    c->y = y;
    wlr_scene_node_set_position(&c->tree->node, x, y);
    lw_pointer_refresh(0);
}

void lw_canvas_resize(ObCanvasId id, gint width, gint height)
{
    WlrCanvas *c = canvas(id);

    c->width = MAX(width, 1);
    c->height = MAX(height, 1);
    if (c->fill)
        wlr_scene_rect_set_size(c->fill, c->width, c->height);
    place_border(c);
    lw_pointer_refresh(0);
}

void lw_canvas_show(ObCanvasId id, gboolean show)
{
    WlrCanvas *c = canvas(id);

    c->shown = show;
    wlr_scene_node_set_enabled(&c->tree->node, show);
    lw_pointer_refresh(0);
}

void lw_canvas_present(ObCanvasId id, const ObImage *img, const Rect *damage)
{
    WlrCanvas *c = canvas(id);
    struct wlr_buffer *b;

    if (c->flags & OB_CANVAS_INVISIBLE) return;
    if (c->fill)
        wlr_scene_node_set_enabled(&c->fill->node, FALSE);
    b = image_buffer(img);
    if (!c->picture) {
        c->picture = wlr_scene_buffer_create(content(c), b);
        c->picture->node.data = c;
        /* above the fill, below the children */
        wlr_scene_node_lower_to_bottom(&c->picture->node);
        if (c->fill)
            wlr_scene_node_lower_to_bottom(&c->fill->node);
        if (c->opacity_set)
            wlr_scene_buffer_set_opacity(c->picture, c->opacity);
    } else if (damage) {
        pixman_region32_t region;

        pixman_region32_init_rect(&region, damage->x, damage->y,
                                  damage->width, damage->height);
        wlr_scene_buffer_set_buffer_with_damage(c->picture, b, &region);
        pixman_region32_fini(&region);
    } else
        wlr_scene_buffer_set_buffer(c->picture, b);
    wlr_scene_node_set_enabled(&c->picture->node, TRUE);
    wlr_buffer_drop(b);
}

void lw_canvas_fill(ObCanvasId id, guint32 argb)
{
    WlrCanvas *c = canvas(id);
    float color[4];

    if (c->flags & OB_CANVAS_INVISIBLE) return;
    if (c->picture)
        wlr_scene_node_set_enabled(&c->picture->node, FALSE);
    color_of(argb, color);
    if (!c->fill) {
        c->fill = wlr_scene_rect_create(content(c), c->width, c->height,
                                        color);
        c->fill->node.data = c;
        wlr_scene_node_lower_to_bottom(&c->fill->node);
    } else
        wlr_scene_rect_set_color(c->fill, color);
    wlr_scene_node_set_enabled(&c->fill->node, TRUE);
}

void lw_canvas_set_border(ObCanvasId id, gint width, guint32 argb)
{
    WlrCanvas *c = canvas(id);

    c->border_width = MAX(width, 0);
    c->border_argb = argb;
    place_border(c);
}

void lw_canvas_set_outline(ObCanvasId c, const ObImage *mask)
{
    /* every canvas can show transparency here, so the core draws its
       outline into the picture instead (OB_CANVAS_ALPHA) */
}

void lw_canvas_set_input(ObCanvasId id, const Rect *rects, guint n)
{
    WlrCanvas *c = canvas(id);

    if (c->input) g_array_free(c->input, TRUE);
    c->input = NULL;
    if (rects) {
        c->input = g_array_sized_new(FALSE, FALSE, sizeof(Rect), n);
        g_array_append_vals(c->input, rects, n);
    }
    lw_pointer_refresh(0);
}

gboolean lw_canvas_accepts(WlrCanvas *c, gint x, gint y)
{
    guint i;

    if (!c->input) return TRUE;
    for (i = 0; i < c->input->len; ++i) {
        const Rect *r = &g_array_index(c->input, Rect, i);
        if (RECT_CONTAINS(*r, x, y)) return TRUE;
    }
    return FALSE;
}

void lw_canvas_layout_pos(WlrCanvas *c, gint *x, gint *y)
{
    int lx, ly;

    wlr_scene_node_coords(&c->content->node, &lx, &ly);
    *x = lx;
    *y = ly;
}

void lw_canvas_set_strut(ObCanvasId c, const StrutPartial *strut)
{
    /* the core reserves room for its own shelf and dock itself; other
       clients' panels come through layer-shell */
}

void lw_canvas_set_shape(ObCanvasId c, const Rect *inner, gboolean bounding,
                          gboolean input, const Rect *rects, guint n)
{
    /* Wayland clients are rectangles that draw their own transparency */
}

static void set_buffer_opacity(struct wlr_scene_buffer *b, int x, int y,
                               void *data)
{
    wlr_scene_buffer_set_opacity(b, *(gfloat*)data);
}

void lw_canvas_set_opacity(ObCanvasId id, gboolean set, guint32 opacity)
{
    WlrCanvas *c = canvas(id);

    c->opacity_set = set;
    c->opacity = set ? (gdouble)opacity / 0xffffffffu : 1.0;
    /* the decoration's pictures and the client inside it */
    wlr_scene_node_for_each_buffer(&c->tree->node, set_buffer_opacity,
                                   &c->opacity);
}

/* dialogs */

void lw_dialog_set_title(ObCanvasId id, const gchar *title)
{
    WlrCanvas *c = canvas(id);

    g_free(c->title);
    c->title = g_strdup(title);
}

void lw_dialog_set_size(ObCanvasId id, gint width, gint height,
                         gboolean managed)
{
    WlrCanvas *c = canvas(id);

    c->dialog_width = width;
    c->dialog_height = height;
    if (managed) {
        ObConfigureRequest req;

        /* ask the core for the size, as a client would */
        memset(&req, 0, sizeof(req));
        req.fields = OB_CONFIGURE_WIDTH | OB_CONFIGURE_HEIGHT;
        req.want.width = width;
        req.want.height = height;
        lw_begin(0);
        lw_events->request_configure(id, &req);
        lw_end();
    } else
        lw_canvas_resize(id, width, height);
}

void lw_dialog_set_parent(ObCanvasId id, ObSurfaceId parent,
                           ObSurfaceId group, gboolean modal)
{
    WlrCanvas *c = canvas(id);

    c->dialog_parent = parent;
    c->dialog_modal = modal;
}
