/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-x11.h for the Lumen window manager

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

/* What event.c hands to the X11 backend while it still reads the X event
   queue itself. Each function turns one kind of XEvent into ObBackendEvents
   and goes away once the backend reads the queue (docs/design/backends.md,
   step 7). Only event.c includes this. */

#ifndef __backend_x11_h
#define __backend_x11_h

#include <X11/Xlib.h>
#include <glib.h>

/*! A property of a managed window changed. Returns FALSE when it isn't one
  the backend reports, so event.c handles it. */
gboolean backend_x11_property_notify(const XPropertyEvent *e);
/*! A client message about a managed window. Returns FALSE when it isn't
  one the backend reports, so event.c handles it. */
gboolean backend_x11_client_message(const XClientMessageEvent *e);
/*! A managed window configures itself. */
void backend_x11_configure_request(const XConfigureRequestEvent *e);
/*! A window asks to be mapped (MapRequest), or a managed window was
  unmapped, destroyed or reparented out of its frame (UnmapNotify,
  DestroyNotify, ReparentNotify). */
void backend_x11_map_event(const XEvent *e);
/*! The event's timestamp, or CurrentTime when it has none. */
guint32 backend_x11_event_time(const XEvent *e);
/*! Read a key, button, motion or crossing event; FALSE for any other. */
gboolean backend_x11_input_event(const XEvent *e, ObInputEvent *out);
/*! A managed window's colormap changed. */
void backend_x11_colormap_notify(const XColormapEvent *e);
/*! A managed window's shape changed (ShapeNotify). Returns FALSE when e
  isn't that. */
gboolean backend_x11_shape_notify(const XEvent *e);
/*! The input method context for keys typed during a grab (menus). */
struct _ObtIC *backend_x11_grab_input_context(void);
/*! An XSync alarm went off. */
void backend_x11_sync_alarm(const XEvent *e);

#endif
