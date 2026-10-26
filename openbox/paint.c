/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   paint.c for the Lumen window manager

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

#include "paint.h"

void paint_canvas(ObCanvasId c, RrAppearance *a, gint w, gint h)
{
    ObImage img;

    if (!RrPaintImage(a, w, h)) return;
    cairo_surface_flush(a->image);

    img.data = (const guint32*)cairo_image_surface_get_data(a->image);
    img.width = w;
    img.height = h;
    img.stride = cairo_image_surface_get_stride(a->image);
    ob_backend->canvas_present(c, &img, NULL);
}

guint32 paint_argb(const RrColor *c)
{
    return 0xff000000u | ((guint32)RrColorRed(c) << 16) |
        ((guint32)RrColorGreen(c) << 8) | (guint32)RrColorBlue(c);
}

void paint_fill(ObCanvasId w, const RrColor *c)
{
    ob_backend->canvas_fill(w, paint_argb(c));
}

void paint_place(ObCanvasId c, gint x, gint y, gint w, gint h)
{
    Rect r;

    RECT_SET(r, x, y, w, h);
    ob_backend->canvas_place(c, &r, NULL);
}
