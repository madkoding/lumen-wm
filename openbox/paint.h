/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   paint.h for the Lumen window manager

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

/* Drawing the theme's appearances onto canvases: obrender draws a picture,
   the backend shows it. */

#ifndef __paint_h
#define __paint_h

#include "backend.h"
#include "obrender/render.h"

/*! Draw a at w x h and show it on c. A parent-relative appearance needs its
  parent drawn first, as with RrPaint. */
void paint_canvas(ObCanvasId c, RrAppearance *a, gint w, gint h);

/*! A theme color as a canvas color (opaque ARGB). */
guint32 paint_argb(const RrColor *c);

/*! Show color c all over canvas w. */
void paint_fill(ObCanvasId w, const RrColor *c);

#endif
