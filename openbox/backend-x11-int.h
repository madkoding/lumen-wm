/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-x11-int.h for the Lumen window manager

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

/* What the X11 backend's files share. Only backend-x11*.c include this. */

#ifndef __backend_x11_int_h
#define __backend_x11_int_h

#include "backend.h"

#include <X11/Xlib.h>

/* backend-x11.c */
/*! What the core gave startup(). */
extern const ObBackendEvents *x11_events;
Cursor x11_cursor(ObCursor cursor);
/*! The event's timestamp, or CurrentTime when it has none. */
guint32 x11_event_time(const XEvent *e);
/*! Read a key, button, motion or crossing event; FALSE for any other. */
gboolean x11_input_event(const XEvent *e, ObInputEvent *out);
/*! A property of a managed window changed. Returns FALSE when it isn't one
  the core hears about. */
gboolean x11_property_notify(const XPropertyEvent *e);
/*! A client message about a managed window. Returns FALSE when it isn't
  one the core hears about. */
gboolean x11_client_message(const XClientMessageEvent *e);
/*! A managed window configures itself. */
void x11_configure_request(const XConfigureRequestEvent *e);
/*! A window asks to be mapped (MapRequest), or a managed window was
  unmapped, destroyed or reparented out of its frame (UnmapNotify,
  DestroyNotify, ReparentNotify). */
void x11_map_event(const XEvent *e);
/*! A managed window's colormap changed. */
void x11_colormap_notify(const XColormapEvent *e);
/*! A managed window's shape changed (ShapeNotify). Returns FALSE when e
  isn't that. */
gboolean x11_shape_notify(const XEvent *e);
/*! The input method context for keys typed during a grab (menus). */
struct _ObtIC *x11_grab_input_context(void);
/*! An XSync alarm went off. */
void x11_sync_alarm(const XEvent *e);

/* backend-x11-canvas.c: what the core draws */
ObCanvasId x11_canvas_new(ObCanvasLayer layer, ObSurfaceId owner,
                          ObCanvasFlags flags);
ObCanvasId x11_canvas_new_child(ObCanvasId parent, ObCanvasFlags flags);
void x11_canvas_free(ObCanvasId c);
gboolean x11_canvas_has_alpha(ObCanvasId c);
void x11_canvas_present(ObCanvasId c, const ObImage *img, const Rect *damage);
void x11_canvas_move(ObCanvasId c, gint x, gint y);
void x11_canvas_resize(ObCanvasId c, gint width, gint height);
void x11_canvas_set_input(ObCanvasId c, const Rect *rects, guint n);
gboolean x11_screen_read(const Rect *area, ObImage *img);
void x11_dialog_set_title(ObCanvasId c, const gchar *title);
void x11_dialog_set_size(ObCanvasId c, gint width, gint height,
                         gboolean managed);
void x11_dialog_set_parent(ObCanvasId c, ObSurfaceId parent,
                           ObSurfaceId group, gboolean modal);
void x11_canvas_fill(ObCanvasId c, guint32 argb);
void x11_canvas_set_border(ObCanvasId c, gint width, guint32 argb);
void x11_canvas_set_outline(ObCanvasId c, const ObImage *mask);
/*! The pixel for argb in the default visual. */
gulong x11_pixel(guint32 argb);

/* backend-x11-event.c: reading the event queue */
void x11_events_listen(void);
void x11_events_stop(void);

/* backend-x11-surface.c: managed windows and the frames around them */
ObSurfaceId *x11_existing_surfaces(guint *n);
/*! The managed window w is, or whose frame it is; None for any other. */
Window x11_managed_of(Window w);
/*! Whether w is a dock app's icon window. */
gboolean x11_is_dockapp(Window w);
/*! Whether parent is w's frame. */
gboolean x11_is_frame_of(Window parent, Window w);
ObSurfaceKind x11_surface_kind(ObSurfaceId s, ObSurfaceId *icon);
void x11_dockapp_adopt(ObSurfaceId icon, ObSurfaceId name, ObCanvasId dock,
                       gint x, gint y);
void x11_dockapp_release(ObSurfaceId icon, ObSurfaceId name,
                         gboolean reparent);
void x11_surface_adopt(ObSurfaceId s, gboolean internal);
void x11_surface_unwatch(ObSurfaceId s, gboolean internal);
void x11_surface_frame(ObSurfaceId s, ObCanvasId frame);
void x11_surface_unframe(ObSurfaceId s, const Rect *area);
void x11_surface_release(ObSurfaceId s, gboolean exiting);
gboolean x11_surface_alive(ObSurfaceId s, gint ignore_hides);
void x11_surface_set_border(ObSurfaceId s, gint width);
void x11_surface_configure(ObSurfaceId s, const Rect *area);
void x11_surface_send_geometry(ObSurfaceId s, const Rect *area, gint border);
void x11_surface_show(ObSurfaceId s, gboolean show);
void x11_get_geometry(ObSurfaceId s, ObSurfaceInfo *info);
/*! A frame for owner, with the owner's visual when it has alpha. */
Window x11_decor_new(ObSurfaceId owner);
/*! Forget w if it is a frame; the window is destroyed by the caller. */
void x11_decor_free(Window w);
void x11_canvas_place(ObCanvasId c, const Rect *outer, const Rect *inner);
void x11_canvas_show(ObCanvasId c, gboolean show);
void x11_canvas_set_shape(ObCanvasId c, const Rect *inner, gboolean bounding,
                          gboolean input, const Rect *rects, guint n);
void x11_get_shape(ObSurfaceId s, ObSurfaceInfo *info);
void x11_surface_install_colors(ObSurfaceId s);
void x11_colormap_changed(Window w, Colormap colormap);
void x11_publish_surface_extents(ObSurfaceId s, const Strut *extents);
void x11_publish_surface_actions(ObSurfaceId s, ObSurfaceActions actions);
void x11_publish_surface_app(ObSurfaceId s, const ObSurfaceApp *app);
void x11_publish_surface_hidden(ObSurfaceId s, gboolean hidden);

#endif
