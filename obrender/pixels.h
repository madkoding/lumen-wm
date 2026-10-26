/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   pixels.h for the Lumen window manager

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

/* Drawing single pixels into an opaque ARGB image, the way X draws
   zero-width lines and rectangles, so what used to go through a GC looks the
   same. Internal to obrender. */

#ifndef __pixels_h
#define __pixels_h

#include "render.h"
#include "color.h"

typedef struct {
    RrPixel32 *data;
    gint width, height;     /*!< the row stride is width */
} RrPixels;

static inline RrPixel32 RrPixelOf(const RrColor *c)
{
    return (0xffu << RrDefaultAlphaOffset) |
        ((RrPixel32)c->r << RrDefaultRedOffset) |
        ((RrPixel32)c->g << RrDefaultGreenOffset) |
        ((RrPixel32)c->b << RrDefaultBlueOffset);
}

static inline void RrPixelPut(RrPixels *p, gint x, gint y, RrPixel32 v)
{
    if (x >= 0 && y >= 0 && x < p->width && y < p->height)
        p->data[y * p->width + x] = v;
}

/*! A zero-width line with both ends drawn, as XDrawLine with CapButt. */
static inline void RrPixelLine(RrPixels *p, gint x1, gint y1,
                               gint x2, gint y2, RrPixel32 v)
{
    gint dx = ABS(x2 - x1), dy = -ABS(y2 - y1);
    gint sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1;
    gint err = dx + dy, e2;

    for (;;) {
        RrPixelPut(p, x1, y1, v);
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

#endif
