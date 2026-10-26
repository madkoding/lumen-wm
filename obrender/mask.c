/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   mask.c for the Openbox window manager
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

#include "render.h"
#include "color.h"
#include "mask.h"
#include "pixels.h"

RrPixmapMask *RrPixmapMaskNew(const RrInstance *inst,
                              gint w, gint h, const gchar *data)
{
    RrPixmapMask *m = g_slice_new(RrPixmapMask);
    m->inst = inst;
    m->width = w;
    m->height = h;
    /* round up to nearest byte */
    m->data = g_memdup(data, (w + 7) / 8 * h);
    return m;
}

void RrPixmapMaskFree(RrPixmapMask *m)
{
    if (m) {
        g_free(m->data);
        g_slice_free(RrPixmapMask, m);
    }
}

void RrPixmapMaskDraw(RrPixel32 *out, gint w, gint h,
                      const RrTextureMask *m, const RrRect *area)
{
    RrPixels p = { out, w, h };
    RrPixel32 c;
    gint x, y, i, j, stride;

    if (m->mask == NULL) return; /* no mask given */

    x = area->x + (area->width - m->mask->width) / 2;
    y = area->y + (area->height - m->mask->height) / 2;

    if (x < 0) x = 0;
    if (y < 0) y = 0;

    /* the bitmap's rows are whole bytes, least significant bit first */
    c = RrPixelOf(m->color);
    stride = (m->mask->width + 7) / 8;
    for (j = 0; j < m->mask->height; ++j)
        for (i = 0; i < m->mask->width; ++i)
            if (m->mask->data[j * stride + i / 8] & (1 << (i % 8)))
                RrPixelPut(&p, x + i, y + j, c);
}

RrPixmapMask *RrPixmapMaskCopy(const RrPixmapMask *src)
{
    RrPixmapMask *m = g_slice_new(RrPixmapMask);
    m->inst = src->inst;
    m->width = src->width;
    m->height = src->height;
    /* round up to nearest byte */
    m->data = g_memdup(src->data, (src->width + 7) / 8 * src->height);
    return m;
}
