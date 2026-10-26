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

/* backend-x11-surface.c: managed windows and the frames around them */
void x11_surface_adopt(ObSurfaceId s, gboolean internal);
void x11_surface_unwatch(ObSurfaceId s, gboolean internal);
void x11_surface_frame(ObSurfaceId s, ObCanvasId frame);
void x11_surface_unframe(ObSurfaceId s, const Rect *area);
void x11_surface_release(ObSurfaceId s, gboolean exiting);
void x11_surface_set_border(ObSurfaceId s, gint width);
void x11_surface_configure(ObSurfaceId s, const Rect *area);
void x11_surface_send_geometry(ObSurfaceId s, const Rect *area, gint border);
void x11_surface_show(ObSurfaceId s, gboolean show);
void x11_get_geometry(ObSurfaceId s, ObSurfaceInfo *info);
ObCanvasId x11_canvas_new(ObCanvasLayer layer, ObSurfaceId owner);
void x11_canvas_free(ObCanvasId c);
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
