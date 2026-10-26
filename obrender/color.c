/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   color.c for the Openbox window manager
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
#include "color.h"
#include "instance.h"

#include <string.h>

/* "rgb:r/g/b", each 1 to 4 hex digits, as XParseColor reads it */
static gboolean parse_rgb(const gchar *s, guint16 *c)
{
    gint i;

    if (g_ascii_strncasecmp(s, "rgb:", 4)) return FALSE;
    s += 4;
    for (i = 0; i < 3; ++i) {
        guint v = 0;
        gint n = 0;

        while (g_ascii_isxdigit(*s) && n < 4) {
            v = v * 16 + g_ascii_xdigit_value(*s++);
            ++n;
        }
        if (!n || (i < 2 ? *s++ != '/' : *s != '\0')) return FALSE;
        /* scale to 16 bits */
        c[i] = (guint16)(v * 0xffff / ((1u << (4 * n)) - 1));
    }
    return TRUE;
}

/* "#rgb", to 4 digits each: the digits are the high bits, as XParseColor
   reads them ("#abc" is "#a0b0c0") */
static gboolean parse_hash(const gchar *s, guint16 *c)
{
    gint len, n, i, j;

    if (*s++ != '#') return FALSE;
    len = strlen(s);
    if (len % 3 || !len || len > 12) return FALSE;
    n = len / 3;
    for (i = 0; i < 3; ++i) {
        guint v = 0;

        for (j = 0; j < n; ++j, ++s) {
            if (!g_ascii_isxdigit(*s)) return FALSE;
            v = v * 16 + g_ascii_xdigit_value(*s);
        }
        c[i] = (guint16)(v << (16 - 4 * n));
    }
    return TRUE;
}

RrColor *RrColorParse(const RrInstance *inst, gchar *colorname)
{
    PangoColor pc;
    guint16 c[3];
    gboolean ok;

    g_assert(colorname != NULL);
    /* get rgb values from colorname, as XParseColor does: #rgb, rgb:r/g/b
       and the X11 color names */
    if (!(ok = parse_hash(colorname, c)) && !(ok = parse_rgb(colorname, c)) &&
        (ok = pango_color_parse(&pc, colorname)))
    {
        c[0] = pc.red; c[1] = pc.green; c[2] = pc.blue;
    }
    if (!ok) {
        g_message("Unable to parse color '%s'", colorname);
        return NULL;
    }
    return RrColorNew(inst, c[0] >> 8, c[1] >> 8, c[2] >> 8);
}

/*#define NO_COLOR_CACHE*/
#ifdef DEBUG
gint id;
#endif

RrColor *RrColorNew(const RrInstance *inst, gint r, gint g, gint b)
{
    /* this should be replaced with something far cooler */
    RrColor *out = NULL;
    gint key;

    g_assert(r >= 0 && r < 256);
    g_assert(g >= 0 && g < 256);
    g_assert(b >= 0 && b < 256);

    key = (r << 24) + (g << 16) + (b << 8);
#ifndef NO_COLOR_CACHE
    if ((out = g_hash_table_lookup(RrColorHash(inst), &key))) {
        out->refcount++;
    } else {
#endif
        {
            out = g_slice_new(RrColor);
            out->inst = inst;
            out->r = r;
            out->g = g;
            out->b = b;
            out->key = key;
            out->refcount = 1;
#ifdef DEBUG
            out->id = id++;
#endif
#ifndef NO_COLOR_CACHE
            g_hash_table_insert(RrColorHash(inst), &out->key, out);
        }
#endif
    }
    return out;
}

RrColor *RrColorCopy(RrColor* c)
{
    return RrColorNew(c->inst, c->r, c->g, c->b);
}

void RrColorFree(RrColor *c)
{
    if (c) {
        if (--c->refcount < 1) {
#ifndef NO_COLOR_CACHE
            g_assert(g_hash_table_lookup(RrColorHash(c->inst), &c->key));
            g_hash_table_remove(RrColorHash(c->inst), &c->key);
#endif
            g_slice_free(RrColor, c);
        }
    }
}

gint RrColorRed(const RrColor *c)
{
    return c->r;
}

gint RrColorGreen(const RrColor *c)
{
    return c->g;
}

gint RrColorBlue(const RrColor *c)
{
    return c->b;
}
