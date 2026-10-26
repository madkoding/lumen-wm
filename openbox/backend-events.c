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
#include "frame.h"
#include "stacking.h"
#include "moveresize.h"

static ObClient *client_of(ObSurfaceId s)
{
    ObWindow *w = window_find((Window)s);
    return w && WINDOW_IS_CLIENT(w) ? WINDOW_AS_CLIENT(w) : NULL;
}

static void surface_new(ObSurfaceId s)
{
    ObClient *client = client_of(s);

    if (!client)
        window_manage((Window)s);
    /* one already managed normally doesn't ask, and when it does it is on
       another desktop or iconified, and only an iconified one is shown */
    else if (client->iconic)
        client_activate(client, FALSE, FALSE, TRUE, TRUE, TRUE);
}

static void surface_gone(ObSurfaceId s)
{
    ObClient *client = client_of(s);

    if (client) client_unmanage(client);
}

static void surface_withdrawn(ObSurfaceId s)
{
    ObClient *client = client_of(s);

    if (!client) return;

    /* the core hid it itself (X11: unmapping it to reparent or iconify) */
    if (client->ignore_unmaps) {
        ob_debug("  ignoring it, %d ignores left", client->ignore_unmaps);
        client->ignore_unmaps--;
        return;
    }
    client_unmanage(client);
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
    if (changed & OB_SURFACE_INFO_SYNC) {
        /* if they are resizing right now this would cause weird behaviour.
           if one day a user reports clients stop resizing, then handle
           this better by syncing with the new counter, but I expect it will
           never happen */
        if (moveresize_client == client)
            moveresize_end(FALSE);
        client_update_sync_request_counter(client);
    }
    if (changed & OB_SURFACE_INFO_OPACITY)
        client_update_opacity(client);
    if (changed & OB_SURFACE_INFO_SHAPE)
        client_update_shape(client);
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
        client_set_wm_state(client, iconic);
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

static void request_configure(ObSurfaceId s, const ObConfigureRequest *req)
{
    ObClient *client = client_of(s);

    if (!client) return;

    /* dont compress these unless you're going to watch for property
       notifies in between (these can change what the configure would
       do to the window).
       also you can't compress stacking events
    */
    {
        gint x, y, w, h;
        gboolean move = FALSE;
        gboolean resize = FALSE;

        /* get the current area */
        RECT_TO_DIMS(client->area, x, y, w, h);

        ob_debug("ConfigureRequest for \"%s\" desktop %d wmstate %d "
                 "visible %d",
                 client->title,
                 screen_desktop, client->wmstate, client->frame->visible);
        ob_debug("                     x %d y %d w %d h %d b %d",
                 x, y, w, h, client->border_width);

        if (req->fields & OB_CONFIGURE_BORDER)
            if (client->border_width != req->border) {
                client->border_width = req->border;

                /* if the border width is changing then that is the same
                   as requesting a resize, but we don't actually change
                   the client's border, so it will change their root
                   coordinates (since they include the border width) and
                   we need to a notify then */
                move = TRUE;
            }

        if (req->fields & OB_CONFIGURE_STACK) {
            ObWindow *sibling = NULL;
            gulong ignore_start;
            gboolean ok = TRUE;

            /* get the sibling */
            if (req->fields & OB_CONFIGURE_SIBLING) {
                ObWindow *win;
                win = window_find((Window)req->sibling);
                if (win && WINDOW_IS_CLIENT(win) &&
                    WINDOW_AS_CLIENT(win) != client)
                {
                    sibling = win;
                }
                else if (win && WINDOW_IS_DOCK(win))
                {
                    sibling = win;
                }
                else
                    /* an invalid sibling was specified so don't restack at
                       all, it won't make sense no matter what we do */
                    ok = FALSE;
            }

            if (ok) {
                if (!config_focus_under_mouse)
                    ignore_start = event_start_ignore_all_enters();
                stacking_restack_request(client, sibling,
                                         req->stack);
                if (!config_focus_under_mouse)
                    event_end_ignore_all_enters(ignore_start);
            }

            /* a stacking change moves the window without resizing */
            move = TRUE;
        }

        if ((req->fields & OB_CONFIGURE_X) ||
            (req->fields & OB_CONFIGURE_Y) ||
            (req->fields & OB_CONFIGURE_WIDTH) ||
            (req->fields & OB_CONFIGURE_HEIGHT))
        {
            /* don't allow clients to move shaded windows (fvwm does this)
            */
            if (req->fields & OB_CONFIGURE_X) {
                if (!client->shaded)
                    x = req->want.x;
                move = TRUE;
            }
            if (req->fields & OB_CONFIGURE_Y) {
                if (!client->shaded)
                    y = req->want.y;
                move = TRUE;
            }

            if (req->fields & OB_CONFIGURE_WIDTH) {
                w = req->want.width;
                resize = TRUE;
            }
            if (req->fields & OB_CONFIGURE_HEIGHT) {
                h = req->want.height;
                resize = TRUE;
            }
        }

        ob_debug("ConfigureRequest x(%d) %d y(%d) %d w(%d) %d h(%d) %d "
                 "move %d resize %d",
                 req->fields & OB_CONFIGURE_X, x,
                 req->fields & OB_CONFIGURE_Y, y,
                 req->fields & OB_CONFIGURE_WIDTH, w,
                 req->fields & OB_CONFIGURE_HEIGHT, h,
                 move, resize);

        /* check for broken apps moving to their root position

           XXX remove this some day...that would be nice. right now all
           kde apps do this when they try activate themselves on another
           desktop. eg. open amarok window on desktop 1, switch to desktop
           2, click amarok tray icon. it will move by its decoration size.
        */
        if (x != client->area.x &&
            x == (client->frame->area.x + client->frame->size.left -
                  (gint)client->border_width) &&
            y != client->area.y &&
            y == (client->frame->area.y + client->frame->size.top -
                  (gint)client->border_width) &&
            w == client->area.width &&
            h == client->area.height)
        {
            ob_debug_type(OB_DEBUG_APP_BUGS,
                          "Application %s is trying to move via "
                          "ConfigureRequest to it's root window position "
                          "but it is not using StaticGravity",
                          client->title);
            /* don't move it */
            x = client->area.x;
            y = client->area.y;

            /* they still requested a move, so don't change whether a
               notify is sent or not */
        }

        /* check for broken apps (java swing) moving to 0,0 when there is a
           strut there.

           XXX remove this some day...that would be nice. but really unexpected
           from Sun Microsystems.
        */
        if (x == 0 && y == 0 && client->gravity == NorthWestGravity &&
            client_normal(client))
        {
            const Rect to = { x, y, w, h };

            /* oldschool fullscreen windows are allowed */
            if (!client_is_oldfullscreen(client, &to)) {
                Rect *r;

                r = screen_area(client->desktop, SCREEN_AREA_ALL_MONITORS,
                                NULL);
                if (r->x || r->y) {
                    /* move the window only to the corner outside struts */
                    x = r->x;
                    y = r->y;

                    ob_debug_type(OB_DEBUG_APP_BUGS,
                                  "Application %s is trying to move via "
                                  "ConfigureRequest to 0,0 using "
                                  "NorthWestGravity, while there is a "
                                  "strut there. "
                                  "Moving buggy app from (0,0) to (%d,%d)",
                                  client->title, r->x, r->y);
                }

                g_slice_free(Rect, r);

                /* they still requested a move, so don't change whether a
                   notify is sent or not */
            }
        }

        {
            gint lw, lh;

            client_try_configure(client, &x, &y, &w, &h, &lw, &lh, FALSE);

            /* if x was not given, then use gravity to figure out the new
               x.  the reference point should not be moved */
            if ((req->fields & OB_CONFIGURE_WIDTH &&
                 !(req->fields & OB_CONFIGURE_X)))
                client_gravity_resize_w(client, &x, client->area.width, w);
            /* same for y */
            if ((req->fields & OB_CONFIGURE_HEIGHT &&
                 !(req->fields & OB_CONFIGURE_Y)))
                client_gravity_resize_h(client, &y, client->area.height,h);

            client_find_onscreen(client, &x, &y, w, h, FALSE);

            ob_debug("Granting ConfigureRequest x %d y %d w %d h %d",
                     x, y, w, h);
            client_configure(client, x, y, w, h, FALSE, TRUE, TRUE);
        }
    }
}

static void request_geometry(ObSurfaceId s, const ObConfigureRequest *req,
                             ObRequestSource from)
{
    ObClient *client = client_of(s);

    (void)from;
    if (!client) return;

    {
        gint ograv, x, y, w, h;

        ograv = client->gravity;

        if (req->fields & OB_CONFIGURE_GRAVITY)
            client->gravity = req->gravity;

        if (req->fields & OB_CONFIGURE_X)
            x = req->want.x;
        else
            x = client->area.x;
        if (req->fields & OB_CONFIGURE_Y)
            y = req->want.y;
        else
            y = client->area.y;

        if (req->fields & OB_CONFIGURE_WIDTH) {
            w = req->want.width;

            /* if x was not given, then use gravity to figure out the new
               x.  the reference point should not be moved */
            if (!(req->fields & OB_CONFIGURE_X))
                client_gravity_resize_w(client, &x, client->area.width, w);
        }
        else
            w = client->area.width;

        if (req->fields & OB_CONFIGURE_HEIGHT) {
            h = req->want.height;

            /* same for y */
            if (!(req->fields & OB_CONFIGURE_Y))
                client_gravity_resize_h(client, &y, client->area.height,h);
        }
        else
            h = client->area.height;

        ob_debug("MOVERESIZE x %d %d y %d %d (gravity %d)",
                 req->fields & OB_CONFIGURE_X, x,
                 req->fields & OB_CONFIGURE_Y, y,
                 client->gravity);

        client_find_onscreen(client, &x, &y, w, h, FALSE);

        client_configure(client, x, y, w, h, FALSE, TRUE, FALSE);

        client->gravity = ograv;
    }
}

static void request_restack(ObSurfaceId s, guintptr sibling_id,
                            ObStackMode mode, ObRequestSource from)
{
    ObClient *client = client_of(s);
    ObWindow *sibling = NULL;
    gulong ignore_start = 0;

    if (!client) return;

    if (from != OB_REQUEST_FROM_PAGER) {
        ob_debug_type(OB_DEBUG_APP_BUGS,
                      "_NET_RESTACK_WINDOW sent for window %s with "
                      "invalid source indication %d",
                      client->title, from);
        return;
    }

    if (sibling_id) {
        ObWindow *win = window_find((Window)sibling_id);
        if (WINDOW_IS_CLIENT(win) &&
            WINDOW_AS_CLIENT(win) != client)
        {
            sibling = win;
        }
        if (WINDOW_IS_DOCK(win))
        {
            sibling = win;
        }
        if (sibling == NULL)
            ob_debug_type(OB_DEBUG_APP_BUGS,
                          "_NET_RESTACK_WINDOW sent for window %s "
                          "with invalid sibling 0x%lx",
                          client->title, (gulong)sibling_id);
    }

    if (!config_focus_under_mouse)
        ignore_start = event_start_ignore_all_enters();
    /* just raise, don't activate */
    stacking_restack_request(client, sibling, mode);
    if (!config_focus_under_mouse)
        event_end_ignore_all_enters(ignore_start);

    /* send a synthetic ConfigureNotify, cuz this is supposed
       to be like a ConfigureRequest. */
    client_reconfigure(client, TRUE);
}

static void focus_changed(ObSurfaceId s)
{
    ObClient *client = s ? client_of(s) : NULL;

    if (!client) {
        focus_set_client(NULL);
        return;
    }
    if (client == focus_client) return;

    frame_adjust_focus(client->frame, TRUE);
    focus_set_client(client);
    client_calc_layer(client);
    client_bring_helper_windows(client);
}

static void focus_left(ObSurfaceId s)
{
    ObClient *client = client_of(s);

    if (client && client != focus_client)
        frame_adjust_focus(client->frame, FALSE);
}

static void focus_lost(gboolean refocus)
{
    focus_fallback(refocus, config_focus_under_mouse, TRUE, TRUE);
}

static void surface_synced(ObSurfaceId s)
{
    ObClient *client = client_of(s);

    if (client && moveresize_in_progress && client == moveresize_client)
        moveresize_synced();
}

static void request_move_resize(ObSurfaceId s, ObMoveResize edge, gint x,
                                gint y, guint button)
{
    ObClient *client = client_of(s);

    if (!client) return;

    if (edge != OB_MOVERESIZE_CANCEL)
        moveresize_start(client, x, y, button, edge);
    else if (moveresize_client)
        moveresize_end(TRUE);
}

const ObBackendEvents ob_backend_events = {
    .surface_new = surface_new,
    .surface_synced = surface_synced,
    .surface_gone = surface_gone,
    .surface_withdrawn = surface_withdrawn,
    .surface_changed = surface_changed,
    .request_states = request_states,
    .request_iconify = request_iconify,
    .request_activate = request_activate,
    .request_close = request_close,
    .request_surface_desktop = request_surface_desktop,
    .request_configure = request_configure,
    .request_geometry = request_geometry,
    .request_restack = request_restack,
    .request_move_resize = request_move_resize,
    .focus_changed = focus_changed,
    .focus_left = focus_left,
    .focus_lost = focus_lost,
};
