/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-x11.c for the Lumen window manager

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

/* The X11 backend: Xlib, ICCCM and EWMH, through obt. The core's own X code
   moves here one area at a time (docs/design/backends.md). */

#include "backend.h"
#include "obt/display.h"
#include "obt/xqueue.h"

static const ObBackendEvents *events;

/* the outputs as output_count() last found them */
static Rect *outputs;
static guint n_outputs;

static gboolean x11_startup(const gchar *display, gboolean replace,
                            const ObBackendEvents *ev)
{
    /* taking over from a running window manager is still screen.c's */
    (void)replace;
    events = ev;
    return obt_display_open(display);
}

static void x11_shutdown(void)
{
    obt_display_close();
    events = NULL;
    g_free(outputs);
    outputs = NULL;
    n_outputs = 0;
}

static GSource *x11_event_source(void)
{
    /* event.c reads the queue through its xqueue callback */
    return xqueue_source_new();
}

static guint x11_output_count(void)
{
    Screen *scr = DefaultScreenOfDisplay(obt_display);
#ifdef XINERAMA
    XineramaScreenInfo *info;
    gint n;
#endif

    g_free(outputs);
#ifdef XINERAMA
    if (obt_display_extension_xinerama &&
        (info = XineramaQueryScreens(obt_display, &n)))
    {
        gint i;

        n_outputs = n;
        outputs = g_new(Rect, n_outputs);
        for (i = 0; i < n; ++i)
            RECT_SET(outputs[i], info[i].x_org, info[i].y_org,
                     info[i].width, info[i].height);
        XFree(info);
        return n_outputs;
    }
#endif
    n_outputs = 1;
    outputs = g_new(Rect, 1);
    RECT_SET(outputs[0], 0, 0, WidthOfScreen(scr), HeightOfScreen(scr));
    return n_outputs;
}

static void x11_output_area(guint output, Rect *area)
{
    g_return_if_fail(output < n_outputs);
    *area = outputs[output];
}

const ObBackend ob_backend_x11 = {
    .name = "x11",
    .caps = OB_BACKEND_CAP_CLIENT_POSITION |
            OB_BACKEND_CAP_DECOR_HINTS |
            OB_BACKEND_CAP_ROOT_STATE |
            OB_BACKEND_CAP_DOCKAPPS |
            OB_BACKEND_CAP_SYNC_RESIZE,

    .startup = x11_startup,
    .shutdown = x11_shutdown,
    .event_source = x11_event_source,

    .output_count = x11_output_count,
    .output_area = x11_output_area,
};
