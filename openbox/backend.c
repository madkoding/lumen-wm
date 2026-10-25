/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend.c for the Lumen window manager

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

#include "backend.h"

#include <string.h>

const ObBackend *ob_backend = NULL;

/* the core still reads X events itself (event.c); these get filled in as
   it moves onto the backend */
const ObBackendEvents ob_backend_events = { NULL };

gboolean backend_startup(const gchar *display, gboolean replace)
{
    /* X11 is the only backend so far */
    ob_backend = &ob_backend_x11;
    return ob_backend->startup(display, replace, &ob_backend_events);
}

void backend_shutdown(void)
{
    if (ob_backend) ob_backend->shutdown();
    ob_backend = NULL;
}

void backend_listen(void)
{
    GSource *source = ob_backend->event_source();
    g_source_attach(source, NULL);
    g_source_unref(source);
}

void backend_surface_info_clear(ObSurfaceInfo *info)
{
    guint i;

    g_free(info->title);
    g_free(info->icon_title);
    g_free(info->app_id);
    g_free(info->instance);
    g_free(info->role);
    g_free(info->group_app_id);
    g_free(info->group_instance);
    g_free(info->session_id);
    g_free(info->command);
    g_free(info->machine);
    for (i = 0; i < info->n_icons; ++i)
        g_free(info->icons[i].argb);
    g_free(info->icons);
    memset(info, 0, sizeof(*info));
}
