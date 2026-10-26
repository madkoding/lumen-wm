/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   render.c for the Openbox window manager
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

#include "render.h"
#include "pixels.h"
#include "gradient.h"
#include "font.h"
#include "mask.h"
#include "color.h"
#include "image.h"
#include "theme.h"

#include <glib.h>

#ifdef HAVE_STDLIB_H
#  include <stdlib.h>
#endif


gboolean RrPaintImage(RrAppearance *a, gint w, gint h)
{
    gint i, force_transfer = 0, stride;
    RrRect tarea; /* area in which to draw textures */
    RrPixel32 *out, *in;
    cairo_t *cr = NULL;
    gboolean resized;

    if (w <= 0 || h <= 0) return FALSE;

    if (a->surface.parentx < 0 || a->surface.parenty < 0) {
        /* ob_debug("Invalid parent co-ordinates\n"); */
        return FALSE;
    }

    if (a->surface.grad == RR_SURFACE_PARENTREL &&
        (a->surface.parentx >= a->surface.parent->w ||
         a->surface.parenty >= a->surface.parent->h))
    {
        return FALSE;
    }

    resized = (a->w != w || a->h != h);
    a->w = w;
    a->h = h;

    if (resized) {
        g_free(a->surface.pixel_data);
        a->surface.pixel_data = g_new(RrPixel32, w * h);
    }
    if (!a->image || cairo_image_surface_get_width(a->image) != w ||
        cairo_image_surface_get_height(a->image) != h)
    {
        if (a->image) cairo_surface_destroy(a->image);
        a->image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    }

    RrRender(a, w, h);

    {
        gint l, t, r, b;
        RrMargins(a, &l, &t, &r, &b);
        RECT_SET(tarea, l, t, w - l - r, h - t - b);
    }

    /* pictures go into the pixels, where parent-relative children see
       them */
    for (i = 0; i < a->textures; i++) {
        RrRect narea = tarea;

        if (a->texture[i].type == RR_TEXTURE_IMAGE) {
            RrTextureImage *img = &a->texture[i].data.image;
            narea.x += img->tx;
            narea.width -= img->tx;
            narea.y += img->ty;
            narea.height -= img->ty;
            if (img->twidth)
                narea.width = MIN(narea.width, img->twidth);
            if (img->theight)
                narea.height = MIN(narea.height, img->theight);
            RrImageDrawImage(a->surface.pixel_data, img, a->w, a->h, &narea);
            force_transfer = 1;
        }
        else if (a->texture[i].type == RR_TEXTURE_RGBA) {
            RrTextureRGBA *rgb = &a->texture[i].data.rgba;
            narea.x += rgb->tx;
            narea.width -= rgb->tx;
            narea.y += rgb->ty;
            narea.height -= rgb->ty;
            if (rgb->twidth)
                narea.width = MIN(narea.width, rgb->twidth);
            if (rgb->theight)
                narea.height = MIN(narea.height, rgb->theight);
            RrImageDrawRGBA(a->surface.pixel_data, rgb, a->w, a->h, &narea);
            force_transfer = 1;
        }
    }

    /* the picture shown is the pixels, opaque */
    cairo_surface_flush(a->image);
    out = (RrPixel32*)cairo_image_surface_get_data(a->image);
    stride = cairo_image_surface_get_stride(a->image) / sizeof(RrPixel32);
    in = a->surface.pixel_data;
    for (i = 0; i < h; ++i) {
        gint x;
        for (x = 0; x < w; ++x)
            out[i * stride + x] = in[i * w + x] |
                (0xffu << RrDefaultAlphaOffset);
    }
    g_assert(stride == w);

    /* a solid surface's bevel was drawn with lines, unless a picture
       covered it */
    if (!force_transfer)
        RrRenderSolidRelief(a, out, w, h);

    /* text, lines and masks go on top */
    for (i = 0; i < a->textures; i++) {
        switch (a->texture[i].type) {
        case RR_TEXTURE_TEXT:
            if (!cr) {
                cairo_surface_mark_dirty(a->image);
                cr = cairo_create(a->image);
            }
            RrFontDraw(cr, &a->texture[i].data.text, &tarea);
            break;
        case RR_TEXTURE_LINE_ART:
        {
            RrPixels p = { out, w, h };
            if (cr) cairo_surface_flush(a->image);
            RrPixelLine(&p, a->texture[i].data.lineart.x1,
                        a->texture[i].data.lineart.y1,
                        a->texture[i].data.lineart.x2,
                        a->texture[i].data.lineart.y2,
                        RrPixelOf(a->texture[i].data.lineart.color));
            if (cr) cairo_surface_mark_dirty(a->image);
            break;
        }
        case RR_TEXTURE_MASK:
            if (cr) cairo_surface_flush(a->image);
            RrPixmapMaskDraw(out, w, h, &a->texture[i].data.mask, &tarea);
            if (cr) cairo_surface_mark_dirty(a->image);
            break;
        case RR_TEXTURE_NONE:
        case RR_TEXTURE_IMAGE:
        case RR_TEXTURE_RGBA:
            break;
        case RR_TEXTURE_NUM_TYPES:
            g_assert_not_reached();
        }
    }
    if (cr) cairo_destroy(cr);
    cairo_surface_mark_dirty(a->image);
    return TRUE;
}

RrAppearance *RrAppearanceNew(const RrInstance *inst, gint numtex)
{
  RrAppearance *out;

  out = g_slice_new0(RrAppearance);
  out->inst = inst;
  out->textures = numtex;
  out->surface.bevel_light_adjust = 128;
  out->surface.bevel_dark_adjust = 64;
  if (numtex) out->texture = g_new0(RrTexture, numtex);

  return out;
}

void RrAppearanceRemoveTextures(RrAppearance *a)
{
    g_free(a->texture);
    a->textures = 0;
}

void RrAppearanceAddTextures(RrAppearance *a, gint numtex)
{
    g_assert(a->textures == 0);

    a->textures = numtex;
    if (numtex) a->texture = g_new0(RrTexture, numtex);
}

void RrAppearanceClearTextures(RrAppearance *a)
{
    memset(a->texture, 0, a->textures * sizeof(RrTexture));
}

/* deep copy of orig, means reset ref to 1 on copy
 * and copy each thing memwise. */
RrAppearance *RrAppearanceCopy(RrAppearance *orig)
{
    RrSurface *spo, *spc;
    RrAppearance *copy = g_slice_new(RrAppearance);

    copy->inst = orig->inst;

    spo = &(orig->surface);
    spc = &(copy->surface);
    spc->grad = spo->grad;
    spc->relief = spo->relief;
    spc->bevel = spo->bevel;
    if (spo->primary != NULL)
        spc->primary = RrColorNew(copy->inst,
                                  spo->primary->r,
                                  spo->primary->g,
                                  spo->primary->b);
    else spc->primary = NULL;

    if (spo->secondary != NULL)
        spc->secondary = RrColorNew(copy->inst,
                                    spo->secondary->r,
                                    spo->secondary->g,
                                    spo->secondary->b);
    else spc->secondary = NULL;

    if (spo->border_color != NULL)
        spc->border_color = RrColorNew(copy->inst,
                                       spo->border_color->r,
                                       spo->border_color->g,
                                       spo->border_color->b);
    else spc->border_color = NULL;

    if (spo->interlace_color != NULL)
        spc->interlace_color = RrColorNew(copy->inst,
                                       spo->interlace_color->r,
                                       spo->interlace_color->g,
                                       spo->interlace_color->b);
    else spc->interlace_color = NULL;

    if (spo->bevel_dark != NULL)
        spc->bevel_dark = RrColorNew(copy->inst,
                                     spo->bevel_dark->r,
                                     spo->bevel_dark->g,
                                     spo->bevel_dark->b);
    else spc->bevel_dark = NULL;

    if (spo->bevel_light != NULL)
        spc->bevel_light = RrColorNew(copy->inst,
                                      spo->bevel_light->r,
                                      spo->bevel_light->g,
                                      spo->bevel_light->b);
    else spc->bevel_light = NULL;

    if (spo->split_primary != NULL)
        spc->split_primary = RrColorNew(copy->inst,
                                        spo->split_primary->r,
                                        spo->split_primary->g,
                                        spo->split_primary->b);
    else spc->split_primary = NULL;

    if (spo->split_secondary != NULL)
        spc->split_secondary = RrColorNew(copy->inst,
                                        spo->split_secondary->r,
                                        spo->split_secondary->g,
                                        spo->split_secondary->b);
    else spc->split_secondary = NULL;

    spc->interlaced = spo->interlaced;
    spc->bevel_light_adjust = spo->bevel_light_adjust;
    spc->bevel_dark_adjust = spo->bevel_dark_adjust;
    spc->border = spo->border;
    spc->parent = NULL;
    spc->parentx = spc->parenty = 0;
    spc->pixel_data = NULL;

    copy->textures = orig->textures;
    copy->texture = g_memdup(orig->texture,
                             orig->textures * sizeof(RrTexture));
    copy->image = NULL;
    copy->w = copy->h = 0;
    return copy;
}

/* now decrements ref counter, and frees only if ref <= 0 */
void RrAppearanceFree(RrAppearance *a)
{
    if (a) {
        RrSurface *p;
        if (a->image) cairo_surface_destroy(a->image);
        if (a->textures)
            g_free(a->texture);
        p = &a->surface;
        RrColorFree(p->primary);
        RrColorFree(p->secondary);
        RrColorFree(p->border_color);
        RrColorFree(p->interlace_color);
        RrColorFree(p->bevel_dark);
        RrColorFree(p->bevel_light);
        RrColorFree(p->split_primary);
        RrColorFree(p->split_secondary);
        g_free(p->pixel_data);
        p->pixel_data = NULL;
        g_slice_free(RrAppearance, a);
    }
}

void RrMargins (RrAppearance *a, gint *l, gint *t, gint *r, gint *b)
{
    *l = *t = *r = *b = 0;

    if (a->surface.grad != RR_SURFACE_PARENTREL) {
        if (a->surface.relief != RR_RELIEF_FLAT) {
            switch (a->surface.bevel) {
            case RR_BEVEL_1:
                *l = *t = *r = *b = 1;
                break;
            case RR_BEVEL_2:
                *l = *t = *r = *b = 2;
                break;
            case RR_BEVEL_NUM_TYPES:
                g_assert_not_reached();
            }
        } else if (a->surface.border) {
            *l = *t = *r = *b = 1;
        }
    }
}

void RrMinSize(RrAppearance *a, gint *w, gint *h)
{
    *w = RrMinWidth(a);
    *h = RrMinHeight(a);
}

gint RrMinWidth(RrAppearance *a)
{
    gint i;
    RrSize *m;
    gint l, t, r, b;
    gint w = 0;

    RrMargins(a, &l, &t, &r, &b);

    for (i = 0; i < a->textures; ++i) {
        switch (a->texture[i].type) {
        case RR_TEXTURE_NONE:
            break;
        case RR_TEXTURE_MASK:
            w = MAX(w, a->texture[i].data.mask.mask->width);
            break;
        case RR_TEXTURE_TEXT:
            m = RrFontMeasureString(a->texture[i].data.text.font,
                                    a->texture[i].data.text.string,
                                    a->texture[i].data.text.shadow_offset_x,
                                    a->texture[i].data.text.shadow_offset_y,
                                    a->texture[i].data.text.flow,
                                    a->texture[i].data.text.maxwidth);
            w = MAX(w, m->width);
            g_slice_free(RrSize, m);
            break;
        case RR_TEXTURE_RGBA:
            w += MAX(w, a->texture[i].data.rgba.width);
            break;
        case RR_TEXTURE_IMAGE:
            /* images resize so they don't contribute anything to the min */
            break;
        case RR_TEXTURE_LINE_ART:
            w = MAX(w, MAX(a->texture[i].data.lineart.x1 - l - r,
                           a->texture[i].data.lineart.x2 - l - r));
            break;
        case RR_TEXTURE_NUM_TYPES:
            g_assert_not_reached();
        }
    }

    w += l + r;

    if (w < 1) w = 1;
    return w;
}

gint RrMinHeight(RrAppearance *a)
{
    gint i;
    gint l, t, r, b;
    RrSize *m;
    gint h = 0;

    RrMargins(a, &l, &t, &r, &b);

    for (i = 0; i < a->textures; ++i) {
        switch (a->texture[i].type) {
        case RR_TEXTURE_NONE:
            break;
        case RR_TEXTURE_MASK:
            h = MAX(h, a->texture[i].data.mask.mask->height);
            break;
        case RR_TEXTURE_TEXT:
            if (a->texture[i].data.text.flow) {
                g_assert(a->texture[i].data.text.string != NULL);

                m = RrFontMeasureString
                    (a->texture[i].data.text.font,
                     a->texture[i].data.text.string,
                     a->texture[i].data.text.shadow_offset_x,
                     a->texture[i].data.text.shadow_offset_y,
                     a->texture[i].data.text.flow,
                     a->texture[i].data.text.maxwidth);
                h += MAX(h, m->height);
                g_slice_free(RrSize, m);
            }
            else
                h += MAX(h,
                         RrFontHeight
                         (a->texture[i].data.text.font,
                          a->texture[i].data.text.shadow_offset_y));
            break;
        case RR_TEXTURE_RGBA:
            h += MAX(h, a->texture[i].data.rgba.height);
            break;
        case RR_TEXTURE_IMAGE:
            /* images resize so they don't contribute anything to the min */
            break;
        case RR_TEXTURE_LINE_ART:
            h = MAX(h, MAX(a->texture[i].data.lineart.y1 - t - b,
                           a->texture[i].data.lineart.y2 - t - b));
            break;
        case RR_TEXTURE_NUM_TYPES:
            g_assert_not_reached();
        }
    }

    h += t + b;

    if (h < 1) h = 1;
    return h;
}
