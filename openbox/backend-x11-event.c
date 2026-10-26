/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-x11-event.c for the Lumen window manager
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

/* The X11 backend reads the event queue: each XEvent becomes what the core
   hears through ObBackendEvents. The FocusIn/FocusOut bookkeeping lives
   here, since only X11 moves the focus behind the window manager's back. */

#include "backend-x11-int.h"
#include "openbox.h"
#include "debug.h"
#include "config.h"
#include "screen.h"
#include "obt/display.h"
#include "obt/xqueue.h"
#include "obt/prop.h"
#include "obt/keyboard.h"

#include <X11/Xlib.h>

static gboolean focus_left_screen = FALSE;
static gboolean waiting_for_focusin = FALSE;

static Window event_get_window(const XEvent *e)
{
    Window window;

    /* pick a window */
    switch (e->type) {
    case SelectionClear:
        window = obt_root(ob_screen);
        break;
    case CreateNotify:
        window = e->xcreatewindow.window;
        break;
    case MapRequest:
        window = e->xmaprequest.window;
        break;
    case MapNotify:
        window = e->xmap.window;
        break;
    case UnmapNotify:
        window = e->xunmap.window;
        break;
    case DestroyNotify:
        window = e->xdestroywindow.window;
        break;
    case ConfigureRequest:
        window = e->xconfigurerequest.window;
        break;
    case ConfigureNotify:
        window = e->xconfigure.window;
        break;
    default:
#ifdef SYNC
        if (obt_display_extension_sync &&
            e->type == obt_display_extension_sync_basep + XSyncAlarmNotify)
        {
            window = None;
        } else
#endif
            window = e->xany.window;
    }
    return window;
}

static void event_hack_mods(XEvent *e)
{
    switch (e->type) {
    case ButtonPress:
    case ButtonRelease:
        e->xbutton.state = obt_keyboard_only_modmasks(e->xbutton.state);
        break;
    case MotionNotify:
        e->xmotion.state = obt_keyboard_only_modmasks(e->xmotion.state);
        /* compress events */
        {
            XEvent ce;
            ObtXQueueWindowType wt;

            wt.window = e->xmotion.window;
            wt.type = MotionNotify;
            while (xqueue_remove_local(&ce, xqueue_match_window_type, &wt)) {
                e->xmotion.x = ce.xmotion.x;
                e->xmotion.y = ce.xmotion.y;
                e->xmotion.x_root = ce.xmotion.x_root;
                e->xmotion.y_root = ce.xmotion.y_root;
            }
        }
        break;
    }
}

static gboolean wanted_focusevent(XEvent *e, gboolean in_client_only)
{
    gint mode = e->xfocus.mode;
    gint detail = e->xfocus.detail;
    Window win = e->xany.window;

    if (e->type == FocusIn) {
        /* These are ones we never want.. */

        /* This means focus was given by a keyboard/mouse grab. */
        if (mode == NotifyGrab)
            return FALSE;
        /* This means focus was given back from a keyboard/mouse grab. */
        if (mode == NotifyUngrab)
            return FALSE;

        /* These are the ones we want.. */

        if (win == obt_root(ob_screen)) {
            /* If looking for a focus in on a client, then always return
               FALSE for focus in's to the root window */
            if (in_client_only)
                return FALSE;
            /* This means focus reverted off of a client */
            else if (detail == NotifyPointerRoot ||
                     detail == NotifyDetailNone ||
                     detail == NotifyInferior ||
                     /* This means focus got here from another screen */
                     detail == NotifyNonlinear)
                return TRUE;
            else
                return FALSE;
        }

        /* It was on a client, was it a valid one?
           It's possible to get a FocusIn event for a client that was managed
           but has disappeared.
        */
        if (in_client_only) {
            if (!x11_managed_of(e->xfocus.window))
                return FALSE;
        }
        else {
            /* This means focus reverted to parent from the client (this
               happens often during iconify animation) */
            if (detail == NotifyInferior)
                return TRUE;
        }

        /* This means focus moved from the root window to a client */
        if (detail == NotifyVirtual)
            return TRUE;
        /* This means focus moved from one client to another */
        if (detail == NotifyNonlinearVirtual)
            return TRUE;

        /* Otherwise.. */
        return FALSE;
    } else {
        g_assert(e->type == FocusOut);

        /* These are ones we never want.. */

        /* This means focus was taken by a keyboard/mouse grab. */
        if (mode == NotifyGrab)
            return FALSE;
        /* This means focus was grabbed on a window and it was released. */
        if (mode == NotifyUngrab)
            return FALSE;

        /* Focus left the root window revertedto state */
        if (win == obt_root(ob_screen))
            return FALSE;

        /* These are the ones we want.. */

        /* This means focus moved from a client to the root window */
        if (detail == NotifyVirtual)
            return TRUE;
        /* This means focus moved from one client to another */
        if (detail == NotifyNonlinearVirtual)
            return TRUE;

        /* Otherwise.. */
        return FALSE;
    }
}

static gboolean event_look_for_focusin(XEvent *e, gpointer data)
{
    return e->type == FocusIn && wanted_focusevent(e, FALSE);
}

static gboolean event_look_for_focusin_client(XEvent *e, gpointer data)
{
    return e->type == FocusIn && wanted_focusevent(e, TRUE);
}

static void print_focusevent(XEvent *e)
{
    gint mode = e->xfocus.mode;
    gint detail = e->xfocus.detail;
    Window win = e->xany.window;
    const gchar *modestr, *detailstr;

    switch (mode) {
    case NotifyNormal:       modestr="NotifyNormal";       break;
    case NotifyGrab:         modestr="NotifyGrab";         break;
    case NotifyUngrab:       modestr="NotifyUngrab";       break;
    case NotifyWhileGrabbed: modestr="NotifyWhileGrabbed"; break;
    default:                 g_assert_not_reached();
    }
    switch (detail) {
    case NotifyAncestor:    detailstr="NotifyAncestor";    break;
    case NotifyVirtual:     detailstr="NotifyVirtual";     break;
    case NotifyInferior:    detailstr="NotifyInferior";    break;
    case NotifyNonlinear:   detailstr="NotifyNonlinear";   break;
    case NotifyNonlinearVirtual: detailstr="NotifyNonlinearVirtual"; break;
    case NotifyPointer:     detailstr="NotifyPointer";     break;
    case NotifyPointerRoot: detailstr="NotifyPointerRoot"; break;
    case NotifyDetailNone:  detailstr="NotifyDetailNone";  break;
    default:                g_assert_not_reached();
    }

    if (mode == NotifyGrab || mode == NotifyUngrab)
        return;

    g_assert(modestr);
    g_assert(detailstr);
    ob_debug_type(OB_DEBUG_FOCUS, "Focus%s 0x%x mode=%s detail=%s",
                  (e->xfocus.type == FocusIn ? "In" : "Out"),
                  win,
                  modestr, detailstr);

}

/* client: the managed window the event is on, or whose frame it is on */
static void focus_in(XEvent *e, Window client)
{
    print_focusevent(e);
    if (!wanted_focusevent(e, FALSE)) {
        if (waiting_for_focusin) {
            /* We were waiting for this FocusIn, since we got a FocusOut
               earlier, but it went to a window that isn't a client. */
            ob_debug_type(OB_DEBUG_FOCUS,
                          "Focus went to an unmanaged window 0x%x !",
                          e->xfocus.window);
            x11_events->focus_lost(TRUE);
        }
    }
    else if (client && e->xfocus.detail == NotifyInferior) {
        ob_debug_type(OB_DEBUG_FOCUS, "Focus went to the frame window");

        focus_left_screen = FALSE;

        x11_events->focus_lost(FALSE);

        /* We don't get a FocusOut for this case, because it's just moving
           from our Inferior up to us. This happens when iconifying a
           window with RevertToParent focus */
        x11_events->focus_left((ObSurfaceId)client);
        /* the focused client has already been forgotten */
    }
    else if (e->xfocus.detail == NotifyPointerRoot ||
             e->xfocus.detail == NotifyDetailNone ||
             e->xfocus.detail == NotifyInferior ||
             e->xfocus.detail == NotifyNonlinear)
    {
        ob_debug_type(OB_DEBUG_FOCUS,
                      "Focus went to root or pointer root/none");

        if (e->xfocus.detail == NotifyInferior ||
            e->xfocus.detail == NotifyNonlinear)
        {
            focus_left_screen = FALSE;
        }

        /* If another FocusIn is in the queue then don't fallback yet. This
           fixes the fun case of:
           window map -> send focusin
           window unmap -> get focusout
           window map -> send focusin
           get first focus out -> fall back to something (new window
             hasn't received focus yet, so something else) -> send focusin
           which means the "something else" is the last thing to get a
           focusin sent to it, so the new window doesn't end up with focus.

           But if the other focus in is something like PointerRoot then we
           still want to fall back.
        */
        if (xqueue_exists_local(event_look_for_focusin_client, NULL)) {
            ob_debug_type(OB_DEBUG_FOCUS,
                          "  but another FocusIn is coming");
        } else {
            /* Focus has been reverted.

               FocusOut events come after UnmapNotify, so we don't need to
               worry about focusing an invalid window
            */

            if (!focus_left_screen)
                x11_events->focus_lost(FALSE);
        }
    }
    else if (!client)
    {
        ob_debug_type(OB_DEBUG_FOCUS,
                      "Focus went to a window that is already gone");

        /* If you send focus to a window and then it disappears, you can
           get the FocusIn for it, after it is unmanaged.
           Just wait for the next FocusOut/FocusIn pair, but make note that
           the window that was focused no longer is. */
        x11_events->focus_changed(0);
    }
    else {
        focus_left_screen = FALSE;
        x11_events->focus_changed((ObSurfaceId)client);
    }

    waiting_for_focusin = FALSE;
}

static void focus_out(XEvent *e, Window client)
{
    print_focusevent(e);
    if (!wanted_focusevent(e, FALSE))
        ; /* skip this one */
    /* Look for the followup FocusIn */
    else if (!xqueue_exists_local(event_look_for_focusin, NULL)) {
        /* There is no FocusIn, this means focus went to a window that
           is not being managed, or a window on another screen. */
        Window win, root;
        gint i;
        guint u;
        obt_display_ignore_errors(TRUE);
        if (XGetInputFocus(obt_display, &win, &i) &&
            XGetGeometry(obt_display, win, &root, &i,&i,&u,&u,&u,&u) &&
            root != obt_root(ob_screen))
        {
            ob_debug_type(OB_DEBUG_FOCUS,
                          "Focus went to another screen !");
            focus_left_screen = TRUE;
        }
        else
            ob_debug_type(OB_DEBUG_FOCUS,
                          "Focus went to a black hole !");
        obt_display_ignore_errors(FALSE);
        /* nothing is focused */
        x11_events->focus_changed(0);
    } else {
        /* Focus moved, so mark that we are waiting to process that
           FocusIn */
        waiting_for_focusin = TRUE;

        /* nothing is focused right now, but will be again shortly */
        x11_events->focus_changed(0);
    }

    if (client)
        x11_events->focus_left((ObSurfaceId)client);
}

static void handle_root(XEvent *e)
{
    Atom msgtype;

    switch(e->type) {
    case SelectionClear:
        x11_events->replaced();
        break;

    case ClientMessage:
        if (e->xclient.format != 32) break;

        msgtype = e->xclient.message_type;
        if (msgtype == OBT_PROP_ATOM(NET_CURRENT_DESKTOP)) {
            if (e->xclient.data.l[1] == 0)
                ob_debug_type(OB_DEBUG_APP_BUGS,
                              "_NET_CURRENT_DESKTOP message is missing "
                              "a timestamp");
            x11_events->request_desktop(e->xclient.data.l[0],
                                        e->xclient.data.l[1]);
        } else if (msgtype == OBT_PROP_ATOM(NET_NUMBER_OF_DESKTOPS)) {
            x11_events->request_desktop_count(e->xclient.data.l[0]);
        } else if (msgtype == OBT_PROP_ATOM(NET_SHOWING_DESKTOP)) {
            x11_events->request_show_desktop(e->xclient.data.l[0] != 0);
        } else if (msgtype == OBT_PROP_ATOM(OB_CONTROL)) {
            ob_debug("OB_CONTROL: %d", e->xclient.data.l[0]);
            x11_events->running_requested(e->xclient.data.l[0]);
        } else if (msgtype == OBT_PROP_ATOM(WM_PROTOCOLS)) {
            if ((Atom)e->xclient.data.l[0] == OBT_PROP_ATOM(NET_WM_PING))
                x11_events->pong((ObSurfaceId)e->xclient.data.l[2],
                                 e->xclient.data.l[1]);
        }
        break;
    case PropertyNotify:
        if (e->xproperty.atom == OBT_PROP_ATOM(NET_DESKTOP_NAMES)) {
            ob_debug("UPDATE DESKTOP NAMES");
            x11_events->desktop_names_changed();
        }
        else if (e->xproperty.atom == OBT_PROP_ATOM(NET_DESKTOP_LAYOUT))
            x11_events->desktop_layout_changed();
        break;
    case ConfigureNotify:
#ifdef XRANDR
        XRRUpdateConfiguration(e);
#endif
        x11_events->outputs_changed();
        break;
    default:
        ;
    }
}

/* a dock app's icon window: the core hears about it as a surface */
static void handle_dockapp(XEvent *e, Window icon)
{
    switch (e->type) {
    case UnmapNotify:
        x11_events->surface_withdrawn((ObSurfaceId)icon);
        break;
    case DestroyNotify:
    case ReparentNotify:
        x11_events->surface_gone((ObSurfaceId)icon);
        break;
    case ConfigureNotify:
        x11_events->surface_changed((ObSurfaceId)icon,
                                    OB_SURFACE_INFO_GEOMETRY, CurrentTime);
        break;
    }
}

/* about a managed window, or on its frame */
static void handle_managed(XEvent *e)
{
    switch (e->type) {
    case ConfigureRequest:
        x11_configure_request(&e->xconfigurerequest);
        break;
    case ReparentNotify:
        /* this is when the client is first taken captive in the frame */
        if (x11_is_frame_of(e->xreparent.parent, e->xreparent.window))
            break;

        /*
          This event is quite rare and is usually handled in unmapHandler.
          However, if the window is unmapped when the reparent event occurs,
          the window manager never sees it because an unmap event is not sent
          to an already unmapped window.
        */
        x11_map_event(e);
        break;
    case UnmapNotify:
    case DestroyNotify:
    case MapRequest:
        x11_map_event(e);
        break;
    case ClientMessage:
        if (e->xclient.format != 32) break;

        /* requests pagers and clients make */
        x11_client_message(&e->xclient);
        break;
    case PropertyNotify:
        /* what the client says about itself */
        x11_property_notify(&e->xproperty);
        break;
    case ColormapNotify:
        x11_colormap_notify(&e->xcolormap);
        break;
    default:
        /* the client changed its outline */
        x11_shape_notify(e);
        break;
    }
}

/* a window nobody manages */
static void handle_unmanaged(XEvent *e, Window window)
{
    if (e->type == MapRequest)
        x11_map_event(e);
    else if (e->type == ClientMessage) {
        /* This is for _NET_WM_REQUEST_FRAME_EXTENTS messages. They come for
           windows that are not managed yet. */
        if (e->xclient.message_type ==
            OBT_PROP_ATOM(NET_REQUEST_FRAME_EXTENTS))
            x11_events->request_frame_extents((ObSurfaceId)window);
    }
    else if (e->type == ConfigureRequest) {
        /* unhandled configure requests must be used to configure the
           window directly */
        XWindowChanges xwc;

        xwc.x = e->xconfigurerequest.x;
        xwc.y = e->xconfigurerequest.y;
        xwc.width = e->xconfigurerequest.width;
        xwc.height = e->xconfigurerequest.height;
        xwc.border_width = e->xconfigurerequest.border_width;
        xwc.sibling = e->xconfigurerequest.above;
        xwc.stack_mode = e->xconfigurerequest.detail;

        /* we are not to be held responsible if someone sends us an
           invalid request! */
        obt_display_ignore_errors(TRUE);
        XConfigureWindow(obt_display, window,
                         e->xconfigurerequest.value_mask, &xwc);
        obt_display_ignore_errors(FALSE);
    }
}

static void event_process(const XEvent *ec, gpointer data)
{
    XEvent ee, *e;
    Window window, client = None;
    ObInputEvent input;

    /* make a copy we can mangle */
    ee = *ec;
    e = &ee;

    window = event_get_window(e);
    if (window != obt_root(ob_screen))
        client = x11_managed_of(window);

    x11_events->begin(x11_event_time(e), e->xany.serial);
    event_hack_mods(e);

    if (e->type == FocusIn)
        focus_in(e, client);
    else if (e->type == FocusOut)
        focus_out(e, client);
    else if (x11_input_event(e, &input)) {
        /* keys typed during a grab go to menus, which want their text */
        if (input.type == OB_INPUT_KEY_PRESS &&
            e->xkey.window == screen_support_win)
            input.text = obt_keyboard_keypress_to_unichar(
                x11_grab_input_context(), e);
        x11_events->input(&input);
    }
    else if (window == obt_root(ob_screen))
        handle_root(e);
    else if (x11_is_dockapp(window))
        handle_dockapp(e, window);
    else if (client)
        handle_managed(e);
    else if (e->type == MappingNotify) {
        /* keyboard layout changes for modifier mapping changes. reload the
           modifier map, and rebind all the key bindings as appropriate */
        if (config_keyboard_rebind_on_mapping_notify) {
            ob_debug("Keyboard map changed. Reloading keyboard bindings.");
            obt_keyboard_reload();
            x11_events->keymap_changed();
        }
    }
#ifdef SYNC
    else if (obt_display_extension_sync &&
             e->type == obt_display_extension_sync_basep + XSyncAlarmNotify)
    {
        x11_sync_alarm(e);
    }
#endif
    else
        handle_unmanaged(e, window);

    x11_events->end();
}

void x11_events_listen(void)
{
    xqueue_add_callback(event_process, NULL);
}

void x11_events_stop(void)
{
    xqueue_remove_callback(event_process, NULL);
}
