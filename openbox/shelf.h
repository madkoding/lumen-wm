/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   shelf.h for the Openbox window manager

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

#ifndef __shelf_h
#define __shelf_h

#include "geom.h"

#include <glib.h>
#include <X11/Xlib.h>

/*! The space the shelf takes from the work area (bottom edge only) */
extern StrutPartial shelf_strut;

/*! The screen y a submenu should not go below so the shelf stays clear, or
  -1 when the shelf is not shown */
gint shelf_menu_limit(void);

/*! When a compositor runs, the 32-bit visual and colormap a menu frame can use
  to be drawn with real alpha: its rounded corners are then anti-aliased by
  cairo instead of cut with a shape mask. FALSE when there is none. */
gboolean shelf_menu_argb(Visual **visual, Colormap *cmap);

struct _ObMenuFrame;

/*! Paints a whole menu frame on its ARGB window with cairo: the rounded box
  with the theme's border and background, then every entry (the selected one
  on a rounded highlight, titles, separators, icons, submenu arrows). Its
  entries' windows only take input. */
void shelf_menu_paint(struct _ObMenuFrame *frame, Visual *visual);

/*! The shelf is two floating islands at the bottom of the screen: a dock
  listing the running applications, and a clock with status glyphs. It is
  drawn by the window manager itself with cairo, and is off unless the
  <shelf> section of rc.xml enables it. */
void shelf_startup(gboolean reconfig);
void shelf_shutdown(gboolean reconfig);

/*! Registers the menu of the dock tiles (Pin, Unpin). Called by menu_startup */
void shelf_menu_startup(void);

/*! Gives the shelf a chance to take an X event. Returns TRUE when the event
  was for the shelf and must not be processed any further. */
gboolean shelf_event(const XEvent *e);

#endif
