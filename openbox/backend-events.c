/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-events.c for the Lumen window manager

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

/* The core half of event handling: what the core does when a backend
   reports something (ObBackendEvents). event.c, the X11 half, still handles
   what hasn't moved here yet (docs/design/backends.md, step 7). */

#include "backend.h"
#include "client.h"
#include "event.h"
#include "focus.h"
#include "window.h"
#include "debug.h"
#include "config.h"
#include "screen.h"

static ObClient *client_of(ObSurfaceId s)
{
    ObWindow *w = window_find((Window)s);
    return w && WINDOW_IS_CLIENT(w) ? WINDOW_AS_CLIENT(w) : NULL;
}

static void surface_changed(ObSurfaceId s, ObSurfaceInfoFields changed,
                            guint32 time)
{
    ObClient *client = client_of(s);

    if (!client) return;

    if (changed & OB_SURFACE_INFO_SIZE) {
        int x, y, w, h, lw, lh;

        ob_debug("Update NORMAL hints");
        client_update_normal_hints(client);
        /* normal hints can make a window non-resizable */
        client_setup_decor_and_functions(client, FALSE);

        x = client->area.x;
        y = client->area.y;
        w = client->area.width;
        h = client->area.height;

        /* apply the new normal hints */
        client_try_configure(client, &x, &y, &w, &h, &lw, &lh, FALSE);
        /* make sure the window is visible, and if the window is resized
           off-screen due to the normal hints changing then this will push
           it back onto the screen. */
        client_find_onscreen(client, &x, &y, w, h, FALSE);

        /* make sure the client's sizes are within its bounds, but don't
           make it reply with a configurenotify unless something changed.
           emacs will update its normal hints every time it receives a
           configurenotify */
        client_configure(client, x, y, w, h, FALSE, TRUE, FALSE);
    }
    if (changed & OB_SURFACE_INFO_DECOR) {
        client_get_mwm_hints(client);
        /* This can override some mwm hints */
        client_get_type_and_transientness(client);

        /* Apply the changes to the window */
        client_setup_decor_and_functions(client, TRUE);
    }
    if (changed & OB_SURFACE_INFO_HINTS)
        client_update_wmhints(client);
    if (changed & OB_SURFACE_INFO_PARENT) {
        /* get the transient-ness first, as this affects if the client
           decides to be transient for the group or not in
           client_update_transient_for() */
        client_get_type_and_transientness(client);
        client_update_transient_for(client);
        /* type may have changed, so update the layer */
        client_calc_layer(client);
        client_setup_decor_and_functions(client, TRUE);
    }
    if (changed & OB_SURFACE_INFO_TITLE)
        client_update_title(client);
    if (changed & OB_SURFACE_INFO_PROTOCOLS)
        client_update_protocols(client);
    if (changed & OB_SURFACE_INFO_STRUT)
        client_update_strut(client);
    if (changed & OB_SURFACE_INFO_ICON)
        client_update_icons(client);
    if (changed & OB_SURFACE_INFO_ICON_AREA)
        client_update_icon_geometry(client);
    if (changed & OB_SURFACE_INFO_USER_TIME && client == focus_client) {
        ObSurfaceInfo info = { 0 };

        ob_backend->surface_info(s, OB_SURFACE_INFO_USER_TIME, &info);
        if (info.has_user_time && info.user_time &&
            !event_time_after(info.user_time, time) &&
            (!event_last_user_time ||
             event_time_after(info.user_time, event_last_user_time)))
        {
            event_last_user_time = info.user_time;
        }
    }
}

static void request_states(ObSurfaceId s, ObStateAction action,
                           ObSurfaceStates first, ObSurfaceStates second,
                           ObRequestSource from)
{
    ObClient *client = client_of(s);
    gulong ignore_start = 0;

    (void)from;
    if (!client) return;

    ob_debug("net_wm_state %s 0x%x 0x%x for 0x%lx",
             (action == OB_STATE_ACTION_REMOVE ? "Remove" :
              action == OB_STATE_ACTION_ADD ? "Add" : "Toggle"),
             first, second, client->window);

    /* ignore enter events caused by these like ob actions do */
    if (!config_focus_under_mouse)
        ignore_start = event_start_ignore_all_enters();
    client_set_state(client, action, first, second);
    if (!config_focus_under_mouse)
        event_end_ignore_all_enters(ignore_start);
}

static void request_iconify(ObSurfaceId s, gboolean iconic)
{
    ObClient *client = client_of(s);

    if (client)
        client_set_wm_state(client, iconic ? IconicState : NormalState);
}

static void request_activate(ObSurfaceId s, ObRequestSource from,
                             guint32 time)
{
    ObClient *client = client_of(s);

    if (!client) return;

    ob_debug("net_active_window for 0x%lx source=%s",
             client->window,
             (from == OB_REQUEST_FROM_APPLICATION ? "application" :
              from == OB_REQUEST_FROM_PAGER ? "user" : "unknown"));
    if (from != OB_REQUEST_FROM_UNKNOWN) {
        /* we can not trust the timestamp from applications.
           e.g. chromium passes a very old timestamp.  openbox thinks
           the window will get focus and calls XSetInputFocus with the
           (old) timestamp, which doesn't end up moving focus at all.
           but the window is raised, not hilited, etc, as if it was
           really going to get focus.

           so do not use this timestamp in event_curtime, as this would
           be used in XSetInputFocus.
        */
        event_set_source_time(time);
        if (time == 0)
            ob_debug_type(OB_DEBUG_APP_BUGS,
                          "_NET_ACTIVE_WINDOW message for window %s is"
                          " missing a timestamp", client->title);
    } else
        ob_debug_type(OB_DEBUG_APP_BUGS,
                      "_NET_ACTIVE_WINDOW message for window %s is "
                      "missing source indication", client->title);
    /* TODO(danakj) This should use the source to determine if a user
       requested the activation, however GTK+ applications seem unable to
       make this distinction ever (including panels such as xfce4-panel and
       gnome-panel). So we are left just assuming all activations are from
       the user.
    */
    client_activate(client, FALSE, FALSE, TRUE, TRUE, TRUE);
}

static void request_close(ObSurfaceId s, ObRequestSource from)
{
    ObClient *client = client_of(s);

    (void)from;
    if (!client) return;

    ob_debug("net_close_window for 0x%lx", client->window);
    client_close(client);
}

static void request_surface_desktop(ObSurfaceId s, guint desktop)
{
    ObClient *client = client_of(s);

    if (client && (desktop < screen_num_desktops || desktop == DESKTOP_ALL))
        client_set_desktop(client, desktop, FALSE, FALSE);
}

const ObBackendEvents ob_backend_events = {
    .surface_changed = surface_changed,
    .request_states = request_states,
    .request_iconify = request_iconify,
    .request_activate = request_activate,
    .request_close = request_close,
    .request_surface_desktop = request_surface_desktop,
};
