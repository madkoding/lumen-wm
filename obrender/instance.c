/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   instance.c for the Openbox window manager
   Copyright (c) 2006        Mikael Magnusson
   Copyright (c) 2003-2007   Dana Jansens

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
#include "instance.h"

static RrInstance *definst = NULL;


#ifdef DEBUG
#include "color.h"
#endif
static void
dest(gpointer data)
{
#ifdef DEBUG
    RrColor *c = data;
    if (c->refcount > 0)
        g_error("color %d (%d,%d,%d) in hash table with %d "
                "leftover references",
                c->id, RrColorRed(c), RrColorGreen(c), RrColorBlue(c),
                c->refcount);
#endif
}

#if 0
static void f(gpointer key, gpointer value, gpointer n)
{
    RrColor *c = value;
    if (c->id == *(gint*)n)
        g_message("color %d has %d references", c->id, c->refcount);
}

void print_refs(gint id)
{
    g_hash_table_foreach(RrColorHash(definst), f, &id);
}
#endif

/* Text is drawn with cairo into images, with the font settings the display
   server shares. Xft measured a font's ascent and descent unhinted, and laid
   out text hinted: hint_metrics picks which. */
static PangoContext *font_context(const RrFontSettings *fs,
                                  gboolean hint_metrics)
{
    PangoContext *ctx;
    cairo_font_options_t *opts;

    ctx = pango_font_map_create_context(pango_cairo_font_map_get_default());
    pango_cairo_context_set_resolution(ctx, fs && fs->dpi > 0 ? fs->dpi : 96);

    opts = cairo_font_options_create();
    cairo_font_options_set_hint_metrics(opts, hint_metrics ?
                                        CAIRO_HINT_METRICS_ON :
                                        CAIRO_HINT_METRICS_OFF);
    if (fs) {
        cairo_font_options_set_antialias(opts, fs->antialias);
        cairo_font_options_set_hint_style(opts, fs->hint_style);
        cairo_font_options_set_subpixel_order(opts, fs->subpixel_order);
    }
    pango_cairo_context_set_font_options(ctx, opts);
    cairo_font_options_destroy(opts);
    return ctx;
}

RrInstance* RrInstanceNew (const RrFontSettings *settings)
{
    definst = g_slice_new(RrInstance);
    definst->pango = font_context(settings, TRUE);
    definst->pango_metrics = font_context(settings, FALSE);
    definst->color_hash = g_hash_table_new_full(g_int_hash, g_int_equal,
                                                NULL, dest);
    return definst;
}

void RrInstanceFree (RrInstance *inst)
{
    if (inst) {
        if (inst == definst) definst = NULL;
        g_hash_table_destroy(inst->color_hash);
        g_object_unref(inst->pango);
        g_object_unref(inst->pango_metrics);
        g_slice_free(RrInstance, inst);
    }
}

GHashTable* RrColorHash (const RrInstance *inst)
{
    return (inst ? inst : definst)->color_hash;
}
