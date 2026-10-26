/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-wlr-int.h for the Lumen window manager

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

/* What the wlroots backend's files share. Only backend-wlr*.c include this.

   Lumen is the compositor here: one wl_display, wlroots' scene graph for
   what is on screen, and the core's decisions carried out on scene nodes.
   The scene has one tree per canvas layer, bottom to top: the desktop,
   the windows (frames and what they hold, stacked by restack()), panels,
   popups, menus and overlays. */

#ifndef __backend_wlr_int_h
#define __backend_wlr_int_h

#include "backend.h"
#include "obrender/render.h"

#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

struct wlr_xdg_toplevel;
struct wlr_xdg_toplevel_decoration_v1;
struct wlr_surface;
struct wlr_output;

typedef struct _WlrCanvas WlrCanvas;
typedef struct _WlrSurface WlrSurface;

/*! The scene's layers, bottom first. */
typedef enum {
    WLR_LAYER_DESKTOP,
    WLR_LAYER_WINDOWS,
    WLR_LAYER_PANEL,
    WLR_LAYER_POPUP,
    WLR_LAYER_MENU,
    WLR_LAYER_OVERLAY,
    WLR_NUM_LAYERS
} WlrLayer;

/*! What a scene node's data points to, so input can find its owner. */
typedef enum {
    WLR_NODE_CANVAS = 1,
    WLR_NODE_SURFACE,
    WLR_NODE_LAYER              /* a layer-shell surface or its popups */
} WlrNodeKind;

typedef struct _WlrNodeData {
    WlrNodeKind kind;
} WlrNodeData;

struct _WlrCanvas {
    WlrNodeData node;           /* first: the tree's node data */
    ObCanvasLayer layer;
    ObCanvasFlags flags;
    WlrCanvas *parent;
    GSList *children;
    WlrSurface *owner;          /* a decoration's client */
    /* everything it shows, moved and stacked as one; the client of a
       decoration goes in it too, above the decoration's pictures */
    struct wlr_scene_tree *tree;
    /* inside the border: what it shows, its children and a client */
    struct wlr_scene_tree *content;
    struct wlr_scene_rect *fill;        /* canvas_fill */
    struct wlr_scene_buffer *picture;   /* canvas_present */
    struct wlr_scene_rect *border[4];   /* canvas_set_border */
    gint border_width;
    guint32 border_argb;
    gint x, y, width, height;
    gboolean shown;
    /* where it takes input, relative to it; NULL for all of it */
    GArray *input;
    ObCursor cursor;
    gboolean opacity_set;
    gfloat opacity;
    /* OB_CANVAS_DIALOG: the client it is, to the core, and what it says
       about itself */
    WlrSurface *dialog;
    gchar *title;
    gint dialog_width, dialog_height;
    ObSurfaceId dialog_parent;
    gboolean dialog_modal;
};

struct _WlrSurface {
    WlrNodeData node;
    struct wlr_xdg_toplevel *toplevel;  /* NULL for a dialog canvas */
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    WlrCanvas *dialog;
    struct wlr_scene_tree *tree;        /* the client's surfaces */
    WlrCanvas *frame;
    gboolean mapped, managed, shown;
    gboolean internal;                  /* the core takes its keys */
    /* the size the core configured, and where the client sits in its frame */
    gint x, y, width, height;
    ObSurfaceStates states;
    /* states it asked for before it first showed */
    ObSurfaceStates initial_states;

    struct wl_listener map, unmap, commit, destroy;
    struct wl_listener request_move, request_resize, request_maximize;
    struct wl_listener request_fullscreen, request_minimize;
    struct wl_listener set_title, set_app_id, set_parent;
    struct wl_listener tree_destroy;
    /* waiting for the answer to a ping */
    struct wl_event_source *ping_check;
    guint32 ping_serial;
};

/* backend-wlr.c */
extern const ObBackendEvents *lw_events;
extern struct wl_display *lw_display;
extern struct wlr_scene *lw_scene;
extern struct wlr_scene_tree *lw_layers[WLR_NUM_LAYERS];
extern struct wlr_seat *lw_seat;
extern struct wlr_output_layout *lw_layout;
guint32 lw_now(void);
/*! Start one event for the core, and end it. */
void lw_begin(guint32 time);
void lw_end(void);

/* backend-wlr-canvas.c: what the core draws */
WlrLayer lw_canvas_layer(ObCanvasLayer layer);
ObCanvasId lw_canvas_new(ObCanvasLayer layer, ObSurfaceId owner,
                          ObCanvasFlags flags);
ObCanvasId lw_canvas_new_child(ObCanvasId parent, ObCanvasFlags flags);
void lw_canvas_free(ObCanvasId c);
gboolean lw_canvas_has_alpha(ObCanvasId c);
void lw_canvas_place(ObCanvasId c, const Rect *outer, const Rect *inner);
void lw_canvas_move(ObCanvasId c, gint x, gint y);
void lw_canvas_resize(ObCanvasId c, gint width, gint height);
void lw_canvas_show(ObCanvasId c, gboolean show);
void lw_canvas_present(ObCanvasId c, const ObImage *img, const Rect *damage);
void lw_canvas_fill(ObCanvasId c, guint32 argb);
void lw_canvas_set_border(ObCanvasId c, gint width, guint32 argb);
void lw_canvas_set_outline(ObCanvasId c, const ObImage *mask);
void lw_canvas_set_input(ObCanvasId c, const Rect *rects, guint n);
void lw_canvas_set_strut(ObCanvasId c, const StrutPartial *strut);
void lw_canvas_set_shape(ObCanvasId c, const Rect *inner, gboolean bounding,
                          gboolean input, const Rect *rects, guint n);
void lw_canvas_set_opacity(ObCanvasId c, gboolean set, guint32 opacity);
void lw_dialog_set_title(ObCanvasId c, const gchar *title);
void lw_dialog_set_size(ObCanvasId c, gint width, gint height,
                         gboolean managed);
void lw_dialog_set_parent(ObCanvasId c, ObSurfaceId parent,
                           ObSurfaceId group, gboolean modal);
/*! Whether a point, relative to the canvas, is where it takes input. */
gboolean lw_canvas_accepts(WlrCanvas *c, gint x, gint y);
/*! The canvas's position in layout coordinates. */
void lw_canvas_layout_pos(WlrCanvas *c, gint *x, gint *y);

/* backend-wlr-surface.c: xdg toplevels and the dialogs the core makes */
void lw_surfaces_startup(void);
void lw_surfaces_shutdown(void);
/*! The surface a wlr_surface belongs to, or NULL. */
WlrSurface *lw_surface_of(struct wlr_surface *s);
/*! The wlr_surface to give the keyboard and pointer to, or NULL. */
struct wlr_surface *lw_surface_wl(WlrSurface *s);
/*! The xdg toplevel a managed surface is, or NULL (a dialog of the core's
  own). */
struct wlr_xdg_toplevel *lw_surface_toplevel(ObSurfaceId id);
/*! The output under its middle, or NULL. */
struct wlr_output *lw_surface_output(ObSurfaceId id);
/*! A dialog canvas becomes a surface the core can manage. */
WlrSurface *lw_surface_new_dialog(WlrCanvas *c);
void lw_surface_free_dialog(WlrSurface *s);
ObSurfaceId *lw_existing_surfaces(guint *n);
ObSurfaceKind lw_surface_kind(ObSurfaceId s, ObSurfaceId *icon);
void lw_surface_adopt(ObSurfaceId s, gboolean internal);
void lw_surface_unwatch(ObSurfaceId s, gboolean internal);
void lw_surface_frame(ObSurfaceId s, ObCanvasId frame);
void lw_surface_unframe(ObSurfaceId s, const Rect *area);
void lw_surface_release(ObSurfaceId s, gboolean exiting);
gboolean lw_surface_alive(ObSurfaceId s, gint ignore_hides);
void lw_surface_configure(ObSurfaceId s, const Rect *area);
void lw_surface_show(ObSurfaceId s, gboolean show);
gboolean lw_surface_focus(ObSurfaceId s, guint32 time, gboolean input,
                           gboolean take_focus);
void lw_surface_close(ObSurfaceId s, guint32 time);
void lw_surface_kill(ObSurfaceId s);
void lw_surface_set_states(ObSurfaceId s, ObSurfaceStates states);
void lw_surface_ping(ObSurfaceId s, guint32 serial);
void lw_surface_info(ObSurfaceId s, ObSurfaceInfoFields fields,
                      ObSurfaceInfo *info);
void lw_restack(guintptr above, const guintptr *top_first, guint n);
/*! The surface that has the keyboard, or NULL. */
WlrSurface *lw_focused_surface(void);

/* backend-wlr-input.c: the seat, keyboards, the pointer and bindings */
void lw_input_startup(struct wlr_backend *backend);
void lw_input_shutdown(void);
gboolean lw_grab_pointer(gboolean grab, gboolean owner_events,
                          gboolean confine, ObCursor cursor, guint32 time);
gboolean lw_grab_keyboard(gboolean grab, guint32 time);
guint lw_modkey_mask(ObModkey key);
guint lw_only_modmasks(guint state);
void lw_keymap_reload(void);
gboolean lw_key_named(const gchar *name, guint *keycode);
void lw_bind_key(guint keycode, guint mods);
void lw_unbind_all_keys(void);
void lw_bind_button(guintptr over, guint button, guint mods, gboolean hold,
                     ObCursor cursor);
void lw_unbind_button(guintptr over, guint button, guint mods);
void lw_pass_click(guint32 time);
gboolean lw_pointer_position(gint *x, gint *y);
void lw_pointer_warp(gint x, gint y, gboolean swallow);
void lw_set_cursor(ObCanvasId c, ObCursor cursor);
/*! Give the keyboard to a client's surface, or to nobody. A layer surface
  holding it keeps it until it lets go. */
void lw_keyboard_focus(struct wlr_surface *s);
/*! A layer surface took the keyboard or let go of it. */
void lw_keyboard_refocus(void);
/*! Look at what is under the pointer again (it moved, or the scene
  changed under it). */
void lw_pointer_refresh(guint32 time);
/*! Something that held the pointer or the keyboard went away. */
void lw_input_forget(guintptr id);

/* backend-wlr-layer.c: layer-shell */
void lw_layers_startup(void);
void lw_layers_shutdown(void);
/*! The layer surface that has the keyboard, or NULL; exclusive: it keeps
  it, and key bindings don't fire. */
struct wlr_surface *lw_layer_keyboard(gboolean *exclusive);
/*! A press on s, a layer surface, or elsewhere for NULL. */
void lw_layer_clicked(struct wlr_surface *s);
guint lw_reserved_edges(const StrutPartial **edges);

/* backend-wlr-toplevels.c: what taskbars see */
void lw_toplevels_startup(void);
void lw_toplevels_shutdown(void);
void lw_publish_clients(const ObSurfaceId *ids, guint n);
void lw_publish_stacking(const ObSurfaceId *ids, guint n);
void lw_publish_active(ObSurfaceId id);
void lw_publish_surface_titles(ObSurfaceId id, const gchar *title,
                               const gchar *icon);
void lw_toplevel_app_id(ObSurfaceId id, const gchar *app_id);
void lw_toplevel_states(ObSurfaceId id, ObSurfaceStates states);

#endif
