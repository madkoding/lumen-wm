/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   test.c for the Openbox window manager
   Copyright (c) 2006        Mikael Magnusson
   Copyright (c) 2003-2007   Dana Jansens
   Copyright (c) 2003        Derek Foreman

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

#include <stdio.h>
#include "render.h"
#include <glib.h>

/* Draws a gradient into an image, without a display, and writes it to
   rendertest.png (or the file named on the command line). */
gint main(gint argc, gchar **argv)
{
    RrInstance *inst;
    RrAppearance *look;
    const gchar *out = argc > 1 ? argv[1] : "rendertest.png";
    gint h = 500, w = 500;

    inst = RrInstanceNew(NULL);

    look = RrAppearanceNew(inst, 0);
    look->surface.grad = RR_SURFACE_MIRROR_HORIZONTAL;
    look->surface.secondary = RrColorParse(inst, "Yellow");
    look->surface.split_secondary = RrColorParse(inst, "Red");
    look->surface.split_primary = RrColorParse(inst, "Green");
    look->surface.primary = RrColorParse(inst, "Blue");
    look->surface.interlaced = FALSE;

    if (!RrPaintImage(look, w, h) ||
        cairo_surface_write_to_png(look->image, out) != CAIRO_STATUS_SUCCESS)
    {
        fprintf(stderr, "couldn't draw into %s\n", out);
        return 1;
    }

    RrAppearanceFree(look);
    RrInstanceFree(inst);
    return 0;
}
