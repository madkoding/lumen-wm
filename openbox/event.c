/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   event.c for the Openbox window manager
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

#include "event.h"
#include "debug.h"
#include "window.h"
#include "openbox.h"
#include "dock.h"
#include "shelf.h"
#include "actions.h"
#include "client.h"
#include "config.h"
#include "screen.h"
#include "frame.h"
#include "grab.h"
#include "menu.h"
#include "prompt.h"
#include "menuframe.h"
#include "keyboard.h"
#include "mouse.h"
#include "focus.h"
#include "focus_cycle.h"
#include "moveresize.h"
#include "group.h"
#include "stacking.h"
#include "ping.h"
#include "backend-x11.h"
#include "obt/display.h"
#include "obt/xqueue.h"
#include "obt/prop.h"
#include "obt/keyboard.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <glib.h>

#ifdef HAVE_SYS_SELECT_H
#  include <sys/select.h>
#endif
#ifdef HAVE_SIGNAL_H
#  include <signal.h>
#endif
#ifdef HAVE_UNISTD_H
#  include <unistd.h> /* for usleep() */
#endif

#ifdef USE_SM
#include <X11/ICE/ICElib.h>
#endif

typedef struct
{
    gboolean ignored;
} ObEventData;

typedef struct
{
    ObClient *client;
    Time time;
    gulong serial;
} ObFocusDelayData;

typedef struct
{
    gulong start; /* inclusive */
    gulong end;   /* inclusive */
} ObSerialRange;

static void event_process(const XEvent *e, gpointer data);
static void event_handle_root(XEvent *e);
static gboolean event_handle_menu_input(const ObInputEvent *e);
static void event_handle_menu(ObMenuFrame *frame, const ObInputEvent *e);
static gboolean event_handle_prompt(ObPrompt *p, XEvent *e);
static void event_handle_dock(ObDock *s, const ObInputEvent *e);
static void event_handle_dockapp(ObDockApp *app, XEvent *e);
static void event_handle_client(ObClient *c, XEvent *e);
static gboolean event_handle_user_input(ObClient *client, XEvent *e);
static gboolean is_enter_focus_event_ignored(gulong serial);
static void event_ignore_enter_range(gulong start, gulong end);

static void focus_delay_dest(gpointer data);
static void unfocus_delay_dest(gpointer data);
static gboolean focus_delay_func(gpointer data);
static gboolean unfocus_delay_func(gpointer data);
static void focus_delay_client_dest(ObClient *client, gpointer data);

Time event_last_user_time = CurrentTime;

/*! The time of the current X event (if it had a timestamp) */
static Time event_curtime = CurrentTime;
/*! The source time that started the current X event (user-provided, so not
  to be trusted) */
static Time event_sourcetime = CurrentTime;

/*! The serial of the current X event */
static gulong event_curserial;
static gboolean focus_left_screen = FALSE;
static gboolean waiting_for_focusin = FALSE;
/*! A list of ObSerialRanges which are to be ignored for mouse enter events */
static GSList *ignore_serials = NULL;
static guint focus_delay_timeout_id = 0;
static ObClient *focus_delay_timeout_client = NULL;
static guint unfocus_delay_timeout_id = 0;
static ObClient *unfocus_delay_timeout_client = NULL;

extern guint button;

#ifdef USE_SM
static gboolean ice_handler(GIOChannel *source, GIOCondition cond,
                            gpointer conn)
{
    Bool b;
    IceProcessMessages(conn, NULL, &b);
    return TRUE; /* don't remove the event source */
}

static void ice_watch(IceConn conn, IcePointer data, Bool opening,
                      IcePointer *watch_data)
{
    static guint id = 0;

    if (opening) {
        GIOChannel *ch;

        ch = g_io_channel_unix_new(IceConnectionNumber(conn));
        id = g_io_add_watch(ch, G_IO_IN, ice_handler, conn);
        g_io_channel_unref(ch);
    } else if (id) {
        g_source_remove(id);
        id = 0;
    }
}
#endif

void event_startup(gboolean reconfig)
{
    if (reconfig) return;

    xqueue_add_callback(event_process, NULL);

#ifdef USE_SM
    IceAddConnectionWatch(ice_watch, NULL);
#endif

#ifdef XKB
    XkbSetDetectableAutoRepeat(obt_display, True, NULL);
#endif

    client_add_destroy_notify(focus_delay_client_dest, NULL);
}

void event_shutdown(gboolean reconfig)
{
    if (reconfig) return;

#ifdef USE_SM
    IceRemoveConnectionWatch(ice_watch, NULL);
#endif

    client_remove_destroy_notify(focus_delay_client_dest);
}

static Window event_get_window(XEvent *e)
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
#ifdef XKB
        if (obt_display_extension_xkb &&
            e->type == obt_display_extension_xkb_basep)
        {
            switch (((XkbAnyEvent*)e)->xkb_type) {
            case XkbBellNotify:
                window = ((XkbBellNotifyEvent*)e)->window;
                break;
            default:
                window = None;
            }
        } else
#endif
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

static void event_set_curtime(XEvent *e)
{
    Time t = backend_x11_event_time(e);

    /* watch that if we get an event earlier than the last specified user_time,
       which can happen if the clock goes backwards, we erase the last
       specified user_time */
    if (t && event_last_user_time && event_time_after(event_last_user_time, t))
        event_reset_user_time();

    event_sourcetime = CurrentTime;
    event_curtime = t;
}

static void event_hack_mods(XEvent *e)
{
    switch (e->type) {
    case ButtonPress:
    case ButtonRelease:
        e->xbutton.state = obt_keyboard_only_modmasks(e->xbutton.state);
        break;
    case KeyPress:
        break;
    case KeyRelease:
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
            ObWindow *w = window_find(e->xfocus.window);
            if (!w || !WINDOW_IS_CLIENT(w))
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

static void event_process(const XEvent *ec, gpointer data)
{
    XEvent ee, *e;
    Window window;
    ObClient *client = NULL;
    ObDock *dock = NULL;
    ObDockApp *dockapp = NULL;
    ObWindow *obwin = NULL;
    ObMenuFrame *menu = NULL;
    ObPrompt *prompt = NULL;
    gboolean used;

    /* make a copy we can mangle */
    ee = *ec;
    e = &ee;

    window = event_get_window(e);
    if (shelf_event(e))
        return;
    if (window == obt_root(ob_screen))
        /* don't do any lookups, waste of cpu */;
    else if ((obwin = window_find(window))) {
        switch (obwin->type) {
        case OB_WINDOW_CLASS_DOCK:
            dock = WINDOW_AS_DOCK(obwin);
            break;
        case OB_WINDOW_CLASS_CLIENT:
            client = WINDOW_AS_CLIENT(obwin);
            /* events on clients can be events on prompt windows too */
            prompt = client->prompt;
            break;
        case OB_WINDOW_CLASS_MENUFRAME:
            menu = WINDOW_AS_MENUFRAME(obwin);
            break;
        case OB_WINDOW_CLASS_INTERNAL:
            /* we don't do anything with events directly on these windows */
            break;
        case OB_WINDOW_CLASS_PROMPT:
            prompt = WINDOW_AS_PROMPT(obwin);
            break;
        }
    }
    else
        dockapp = dock_find_dockapp(window);

    event_set_curtime(e);
    event_curserial = e->xany.serial;
    event_hack_mods(e);

    /* deal with it in the kernel */

    if (e->type == FocusIn) {
        print_focusevent(e);
        if (!wanted_focusevent(e, FALSE)) {
            if (waiting_for_focusin) {
                /* We were waiting for this FocusIn, since we got a FocusOut
                   earlier, but it went to a window that isn't a client. */
                ob_debug_type(OB_DEBUG_FOCUS,
                              "Focus went to an unmanaged window 0x%x !",
                              e->xfocus.window);
                ob_backend_events.focus_lost(TRUE);
            }
        }
        else if (client && e->xfocus.detail == NotifyInferior) {
            ob_debug_type(OB_DEBUG_FOCUS, "Focus went to the frame window");

            focus_left_screen = FALSE;

            ob_backend_events.focus_lost(FALSE);

            /* We don't get a FocusOut for this case, because it's just moving
               from our Inferior up to us. This happens when iconifying a
               window with RevertToParent focus */
            ob_backend_events.focus_left((ObSurfaceId)client->window);
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
                    ob_backend_events.focus_lost(FALSE);
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
            ob_backend_events.focus_changed(0);
        }
        else if (client != focus_client) {
            focus_left_screen = FALSE;
            ob_backend_events.focus_changed((ObSurfaceId)client->window);
        }

        waiting_for_focusin = FALSE;
    } else if (e->type == FocusOut) {
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
            ob_backend_events.focus_changed(0);
        } else {
            /* Focus moved, so mark that we are waiting to process that
               FocusIn */
            waiting_for_focusin = TRUE;

            /* nothing is focused right now, but will be again shortly */
            ob_backend_events.focus_changed(0);
        }

        if (client)
            ob_backend_events.focus_left((ObSurfaceId)client->window);
    }
    else if (client)
        event_handle_client(client, e);
    else if (dockapp)
        event_handle_dockapp(dockapp, e);
    else if ((dock || menu) &&
             (e->type == EnterNotify || e->type == LeaveNotify ||
              e->type == MotionNotify))
    {
        ObInputEvent input;

        if (backend_x11_input_event(e, FALSE, &input)) {
            if (dock)
                event_handle_dock(dock, &input);
            else
                event_handle_menu(menu, &input);
        }
    }
    else if (window == obt_root(ob_screen))
        event_handle_root(e);
    else if (e->type == MapRequest)
        backend_x11_map_event(e);
    else if (e->type == MappingNotify) {
        /* keyboard layout changes for modifier mapping changes. reload the
           modifier map, and rebind all the key bindings as appropriate */
        if (config_keyboard_rebind_on_mapping_notify) {
            ob_debug("Keyboard map changed. Reloading keyboard bindings.");
            ob_set_state(OB_STATE_RECONFIGURING);
            obt_keyboard_reload();
            keyboard_rebind();
            ob_set_state(OB_STATE_RUNNING);
        }
    }
    else if (e->type == ClientMessage) {
        /* This is for _NET_WM_REQUEST_FRAME_EXTENTS messages. They come for
           windows that are not managed yet. */
        if (e->xclient.message_type ==
            OBT_PROP_ATOM(NET_REQUEST_FRAME_EXTENTS))
        {
            /* Pretend to manage the client, getting information used to
               determine its decorations */
            ObClient *c = client_fake_manage(e->xclient.window);
            gulong vals[4];

            /* set the frame extents on the window */
            vals[0] = c->frame->size.left;
            vals[1] = c->frame->size.right;
            vals[2] = c->frame->size.top;
            vals[3] = c->frame->size.bottom;
            OBT_PROP_SETA32(e->xclient.window, NET_FRAME_EXTENTS,
                            CARDINAL, vals, 4);

            /* Free the pretend client */
            client_fake_unmanage(c);
        }
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
#ifdef SYNC
    else if (obt_display_extension_sync &&
             e->type == obt_display_extension_sync_basep + XSyncAlarmNotify)
    {
        backend_x11_sync_alarm(e);
    }
#endif

    if (e->type == ButtonPress || e->type == ButtonRelease) {
        ObWindow *w;
        static guint pressed = 0;

        event_sourcetime = event_curtime;

        /* If the button press was on some non-root window, or was physically
           on the root window... */
        if (window != obt_root(ob_screen) ||
            e->xbutton.subwindow == None ||
            /* ...or if it is related to the last button press we handled... */
            pressed == e->xbutton.button ||
            /* ...or it if it was physically on an openbox
               internal window... */
            ((w = window_find(e->xbutton.subwindow)) &&
             (WINDOW_IS_INTERNAL(w) || WINDOW_IS_DOCK(w))))
            /* ...then process the event, otherwise ignore it */
        {
            used = event_handle_user_input(client, e);

            if (prompt && !used)
                used = event_handle_prompt(prompt, e);

            if (e->type == ButtonPress)
                pressed = e->xbutton.button;
            /* We ignored the release event so make sure we don't think
               the button is still pressed */
            else if (e->type == ButtonRelease)
                button = 0;
        }
    }
    else if (e->type == KeyPress || e->type == KeyRelease ||
             e->type == MotionNotify)
    {
        event_sourcetime = event_curtime;

        used = event_handle_user_input(client, e);

        if (prompt && !used)
            used = event_handle_prompt(prompt, e);
    }

    /* show any debug prompts that are queued */
    ob_debug_show_prompts();

    /* if something happens and it's not from an XEvent, then we don't know
       the time, so clear it here until the next event is handled */
    event_curtime = event_sourcetime = CurrentTime;
    event_curserial = 0;
}

static void event_handle_root(XEvent *e)
{
    Atom msgtype;

    switch(e->type) {
    case SelectionClear:
        ob_debug("Another WM has requested to replace us. Exiting.");
        ob_exit_replace();
        break;

    case ClientMessage:
        if (e->xclient.format != 32) break;

        msgtype = e->xclient.message_type;
        if (msgtype == OBT_PROP_ATOM(NET_CURRENT_DESKTOP)) {
            guint d = e->xclient.data.l[0];
            if (d < screen_num_desktops) {
                if (e->xclient.data.l[1] == 0)
                    ob_debug_type(OB_DEBUG_APP_BUGS,
                                  "_NET_CURRENT_DESKTOP message is missing "
                                  "a timestamp");
                else
                    event_sourcetime = e->xclient.data.l[1];
                screen_set_desktop(d, TRUE);
            }
        } else if (msgtype == OBT_PROP_ATOM(NET_NUMBER_OF_DESKTOPS)) {
            guint d = e->xclient.data.l[0];
            if (d > 0 && d <= 1000)
                screen_set_num_desktops(d);
        } else if (msgtype == OBT_PROP_ATOM(NET_SHOWING_DESKTOP)) {
            ObScreenShowDestopMode show_mode;
            if (e->xclient.data.l[0] != 0)
                show_mode = SCREEN_SHOW_DESKTOP_UNTIL_WINDOW;
            else
                show_mode = SCREEN_SHOW_DESKTOP_NO;
            screen_show_desktop(show_mode, NULL);
        } else if (msgtype == OBT_PROP_ATOM(OB_CONTROL)) {
            ob_debug("OB_CONTROL: %d", e->xclient.data.l[0]);
            if (e->xclient.data.l[0] == 1)
                ob_reconfigure();
            else if (e->xclient.data.l[0] == 2)
                ob_restart();
            else if (e->xclient.data.l[0] == 3)
                ob_exit(0);
        } else if (msgtype == OBT_PROP_ATOM(WM_PROTOCOLS)) {
            if ((Atom)e->xclient.data.l[0] == OBT_PROP_ATOM(NET_WM_PING))
                ping_got_pong(e->xclient.data.l[1]);
        }
        break;
    case PropertyNotify:
        if (e->xproperty.atom == OBT_PROP_ATOM(NET_DESKTOP_NAMES)) {
            ob_debug("UPDATE DESKTOP NAMES");
            screen_update_desktop_names();
        }
        else if (e->xproperty.atom == OBT_PROP_ATOM(NET_DESKTOP_LAYOUT))
            screen_update_layout();
        break;
    case ConfigureNotify:
#ifdef XRANDR
        XRRUpdateConfiguration(e);
#endif
        screen_resize();
        break;
    default:
        ;
    }
}

void event_enter_client(ObClient *client)
{
    g_assert(config_focus_follow);

    if (is_enter_focus_event_ignored(event_curserial)) {
        ob_debug_type(OB_DEBUG_FOCUS, "Ignoring enter event with serial %lu "
                      "on client 0x%x", event_curserial, client->window);
        return;
    }

    ob_debug_type(OB_DEBUG_FOCUS, "using enter event with serial %lu "
                  "on client 0x%x", event_curserial, client->window);

    if (client_enter_focusable(client) && client_can_focus(client)) {
        if (config_focus_delay) {
            ObFocusDelayData *data;

            if (focus_delay_timeout_id)
                g_source_remove(focus_delay_timeout_id);

            data = g_slice_new(ObFocusDelayData);
            data->client = client;
            data->time = event_time();
            data->serial = event_curserial;

            focus_delay_timeout_id = g_timeout_add_full(G_PRIORITY_DEFAULT,
                                                        config_focus_delay,
                                                        focus_delay_func,
                                                        data,
                                                        focus_delay_dest);
            focus_delay_timeout_client = client;
        } else {
            ObFocusDelayData data;
            data.client = client;
            data.time = event_time();
            data.serial = event_curserial;
            focus_delay_func(&data);
        }
    }
}

void event_leave_client(ObClient *client)
{
    g_assert(config_focus_follow);

    if (is_enter_focus_event_ignored(event_curserial)) {
        ob_debug_type(OB_DEBUG_FOCUS, "Ignoring leave event with serial %lu\n"
                      "on client 0x%x", event_curserial, client->window);
        return;
    }

    if (client == focus_client) {
        if (config_focus_delay) {
            ObFocusDelayData *data;

            if (unfocus_delay_timeout_id)
                g_source_remove(unfocus_delay_timeout_id);

            data = g_slice_new(ObFocusDelayData);
            data->client = client;
            data->time = event_time();
            data->serial = event_curserial;

            unfocus_delay_timeout_id = g_timeout_add_full(G_PRIORITY_DEFAULT,
                                                          config_focus_delay,
                                                          unfocus_delay_func,
                                                          data,
                                                          unfocus_delay_dest);
            unfocus_delay_timeout_client = client;
        } else {
            ObFocusDelayData data;
            data.client = client;
            data.time = event_time();
            data.serial = event_curserial;
            unfocus_delay_func(&data);
        }
    }
}

static gboolean *context_to_button(ObFrame *f, ObFrameContext con, gboolean press)
{
    if (press) {
        switch (con) {
        case OB_FRAME_CONTEXT_MAXIMIZE:
            return &f->max_press;
        case OB_FRAME_CONTEXT_CLOSE:
            return &f->close_press;
        case OB_FRAME_CONTEXT_ICONIFY:
            return &f->iconify_press;
        case OB_FRAME_CONTEXT_ALLDESKTOPS:
            return &f->desk_press;
        case OB_FRAME_CONTEXT_SHADE:
            return &f->shade_press;
        default:
            return NULL;
        }
    } else {
        switch (con) {
        case OB_FRAME_CONTEXT_MAXIMIZE:
            return &f->max_hover;
        case OB_FRAME_CONTEXT_CLOSE:
            return &f->close_hover;
        case OB_FRAME_CONTEXT_ICONIFY:
            return &f->iconify_hover;
        case OB_FRAME_CONTEXT_ALLDESKTOPS:
            return &f->desk_hover;
        case OB_FRAME_CONTEXT_SHADE:
            return &f->shade_hover;
        default:
            return NULL;
        }
    }
}

/* The pointer over a client's frame: the title bar buttons light up and
   press, and focus follows the mouse. */
static void event_handle_frame_pointer(ObClient *client,
                                       const ObInputEvent *e)
{
    ObFrameContext con;
    gboolean *but;
    static gint px = -1, py = -1;
    static guint pb = 0;
    static ObFrameContext pcon = OB_FRAME_CONTEXT_NONE;

    switch (e->type) {
    case OB_INPUT_BUTTON_PRESS:
        /* save where the press occured for the first button pressed */
        if (!pb) {
            pb = e->button;
            px = e->x;
            py = e->y;

            pcon = frame_context(client, (Window)e->over, px, py);
            pcon = mouse_button_frame_context(pcon, e->button,
                                              e->mods);
        }
    case OB_INPUT_BUTTON_RELEASE:
        /* Wheel buttons don't draw because they are an instant click, so it
           is a waste of resources to go drawing it.
           if the user is doing an interactive thing, or has a menu open then
           the mouse is grabbed (possibly) and if we get these events we don't
           want to deal with them
        */
        if (!(e->button == 4 || e->button == 5) &&
            !grab_on_keyboard())
        {
            /* use where the press occured */
            con = frame_context(client, (Window)e->over, px, py);
            con = mouse_button_frame_context(con, e->button,
                                             e->mods);

            /* button presses on CLIENT_CONTEXTs are not accompanied by a
               release because they are Replayed to the client */
            if ((e->type == OB_INPUT_BUTTON_RELEASE || CLIENT_CONTEXT(con, client)) &&
                e->button == pb)
                pb = 0, px = py = -1, pcon = OB_FRAME_CONTEXT_NONE;

            but = context_to_button(client->frame, con, TRUE);
            if (but) {
                *but = (e->type == OB_INPUT_BUTTON_PRESS);
                frame_adjust_state(client->frame);
            }
        }
        break;
    case OB_INPUT_MOTION:
        /* when there is a grab on the pointer, we won't get enter/leave
           notifies, but we still get motion events */
        if (grab_on_pointer()) break;

        con = frame_context(client, (Window)e->over,
                            e->x, e->y);
        switch (con) {
        case OB_FRAME_CONTEXT_TITLEBAR:
        case OB_FRAME_CONTEXT_TLCORNER:
        case OB_FRAME_CONTEXT_TRCORNER:
            /* we've left the button area inside the titlebar */
            if (client->frame->max_hover || client->frame->desk_hover ||
                client->frame->shade_hover || client->frame->iconify_hover ||
                client->frame->close_hover)
            {
                client->frame->max_hover =
                    client->frame->desk_hover =
                    client->frame->shade_hover =
                    client->frame->iconify_hover =
                    client->frame->close_hover = FALSE;
                frame_adjust_state(client->frame);
            }
            break;
        default:
            but = context_to_button(client->frame, con, FALSE);
            if (but && !*but && !pb) {
                *but = TRUE;
                frame_adjust_state(client->frame);
            }
            break;
        }
        break;
    case OB_INPUT_LEAVE:
        con = frame_context(client, (Window)e->over,
                            e->x, e->y);
        switch (con) {
        case OB_FRAME_CONTEXT_TITLEBAR:
        case OB_FRAME_CONTEXT_TLCORNER:
        case OB_FRAME_CONTEXT_TRCORNER:
            /* we've left the button area inside the titlebar */
            client->frame->max_hover =
                client->frame->desk_hover =
                client->frame->shade_hover =
                client->frame->iconify_hover =
                client->frame->close_hover = FALSE;
            if (e->crossing == OB_CROSSING_GRAB) {
                client->frame->max_press =
                    client->frame->desk_press =
                    client->frame->shade_press =
                    client->frame->iconify_press =
                    client->frame->close_press = FALSE;
            }
            break;
        case OB_FRAME_CONTEXT_FRAME:
            /* When the mouse leaves an animating window, don't use the
               corresponding enter events. Pretend like the animating window
               doesn't even exist..! */
            if (frame_iconify_animating(client->frame))
                event_end_ignore_all_enters(event_start_ignore_all_enters());

            ob_debug_type(OB_DEBUG_FOCUS,
                          "Leave mode %d inferior %d on %lx",
                          e->crossing, e->inferior,
                          (client?client->window:0));
            if (grab_on_keyboard())
                break;
            if (config_focus_follow &&
                /* leave inferior events can happen when the mouse goes onto
                   the window's border and then into the window before the
                   delay is up */
                !e->inferior)
            {
                if (config_focus_delay && focus_delay_timeout_id)
                    g_source_remove(focus_delay_timeout_id);
                if (config_unfocus_leave)
                    event_leave_client(client);
            }
            break;
        default:
            but = context_to_button(client->frame, con, FALSE);
            if (but) {
                *but = FALSE;
                if (e->crossing == OB_CROSSING_GRAB) {
                    but = context_to_button(client->frame, con, TRUE);
                    *but = FALSE;
                }
                frame_adjust_state(client->frame);
            }
            break;
        }
        break;
    case OB_INPUT_ENTER:
    {
        con = frame_context(client, (Window)e->over,
                            e->x, e->y);
        switch (con) {
        case OB_FRAME_CONTEXT_FRAME:
            if (grab_on_keyboard())
                break;
            if (e->crossing == OB_CROSSING_GRAB ||
                (e->crossing == OB_CROSSING_UNGRAB &&
                 /* ungrab enters are used when _under_ mouse is being used */
                 !(config_focus_follow && config_focus_under_mouse)) ||
                /*ignore enters when we're already in the window */
                e->inferior)
            {
                ob_debug_type(OB_DEBUG_FOCUS,
                              "Enter mode %d inferior %d on %lx IGNORED",
                              e->crossing, e->inferior,
                              client?client->window:0);
            }
            else {
                ob_debug_type(OB_DEBUG_FOCUS,
                              "Enter mode %d inferior %d on %lx, "
                              "focusing window",
                              e->crossing, e->inferior,
                              (client?client->window:0));
                if (config_focus_follow) {
                    if (config_focus_delay && unfocus_delay_timeout_id)
                        g_source_remove(unfocus_delay_timeout_id);
                    event_enter_client(client);
                }
            }
            break;
        default:
            but = context_to_button(client->frame, con, FALSE);
            if (but) {
                *but = TRUE;
                if (e->crossing == OB_CROSSING_UNGRAB) {
                    but = context_to_button(client->frame, con, TRUE);
                    *but = (con == pcon);
                }
                frame_adjust_state(client->frame);
            }
            break;
        }
        break;
    }
    default:
        break;
    }
}

static void event_handle_client(ObClient *client, XEvent *e)
{
    switch (e->type) {
    case ButtonPress:
    case ButtonRelease:
    case MotionNotify:
    case EnterNotify:
    case LeaveNotify:
    {
        ObInputEvent input;

        backend_x11_input_event(e, FALSE, &input);
        event_handle_frame_pointer(client, &input);
        break;
    }
    case ConfigureRequest:
        backend_x11_configure_request(&e->xconfigurerequest);
        break;
    case ReparentNotify:
        /* this is when the client is first taken captive in the frame */
        if (e->xreparent.parent == client->frame->window) break;

        /*
          This event is quite rare and is usually handled in unmapHandler.
          However, if the window is unmapped when the reparent event occurs,
          the window manager never sees it because an unmap event is not sent
          to an already unmapped window.
        */
        backend_x11_map_event(e);
        break;
    case UnmapNotify:
    case DestroyNotify:
    case MapRequest:
        backend_x11_map_event(e);
        break;
    case ClientMessage:
        /* validate cuz we query stuff off the client here */
        if (!client_validate(client)) break;

        if (e->xclient.format != 32) return;

        /* requests pagers and clients make go through the backend */
        backend_x11_client_message(&e->xclient);
        break;
    case PropertyNotify:
        /* validate cuz we query stuff off the client here */
        if (!client_validate(client)) break;

        /* what the client says about itself goes through the backend */
        backend_x11_property_notify(&e->xproperty);
        break;
    case ColormapNotify:
        backend_x11_colormap_notify(&e->xcolormap);
        break;
    default:
        /* the client changed its outline */
        backend_x11_shape_notify(e);
        break;
    }
}

static void event_handle_dock(ObDock *s, const ObInputEvent *e)
{
    switch (e->type) {
    case OB_INPUT_ENTER:
        dock_hide(FALSE);
        break;
    case OB_INPUT_LEAVE:
        /* don't hide when moving into a dock app */
        if (!e->inferior)
            dock_hide(TRUE);
        break;
    }
}

static void event_handle_dockapp(ObDockApp *app, XEvent *e)
{
    switch (e->type) {
    case MotionNotify:
        dock_app_drag(app, &e->xmotion);
        break;
    case UnmapNotify:
        if (app->ignore_unmaps) {
            app->ignore_unmaps--;
            break;
        }
        dock_unmanage(app, TRUE);
        break;
    case DestroyNotify:
    case ReparentNotify:
        dock_unmanage(app, FALSE);
        break;
    case ConfigureNotify:
        dock_app_configure(app, e->xconfigure.width, e->xconfigure.height);
        break;
    }
}

static ObMenuFrame* find_active_menu(void)
{
    GList *it;
    ObMenuFrame *ret = NULL;

    for (it = menu_frame_visible; it; it = g_list_next(it)) {
        ret = it->data;
        if (ret->selected)
            break;
        ret = NULL;
    }
    return ret;
}

static ObMenuFrame* find_active_or_last_menu(void)
{
    ObMenuFrame *ret = NULL;

    ret = find_active_menu();
    if (!ret && menu_frame_visible)
        ret = menu_frame_visible->data;
    return ret;
}

static gboolean event_handle_prompt(ObPrompt *p, XEvent *e)
{
    ObInputEvent input;

    if (!backend_x11_input_event(e, FALSE, &input))
        return FALSE;
    switch (input.type) {
    case OB_INPUT_BUTTON_PRESS:
    case OB_INPUT_BUTTON_RELEASE:
    case OB_INPUT_MOTION:
        return prompt_mouse_event(p, &input);
    case OB_INPUT_KEY_PRESS:
        return prompt_key_event(p, &input);
    default:
        return FALSE;
    }
}

static gboolean event_handle_menu_input(const ObInputEvent *ev)
{
    gboolean ret = FALSE;

    if (ev->type == OB_INPUT_BUTTON_RELEASE || ev->type == OB_INPUT_BUTTON_PRESS) {
        ObMenuEntryFrame *e;

        if ((ev->button < 4 || ev->button > 5) &&
            ((ev->type == OB_INPUT_BUTTON_RELEASE && menu_hide_delay_reached()) ||
             ev->type == OB_INPUT_BUTTON_PRESS))
        {
            if ((e = menu_entry_frame_under(ev->root_x,
                                            ev->root_y)))
            {
                if (ev->type == OB_INPUT_BUTTON_PRESS) {
                    /* We know this is a new press, so we don't have to
                     * block release events anymore */
                    menu_hide_delay_reset();

                    if (e->frame->child)
                        menu_frame_select(e->frame->child, NULL, TRUE);
                }
                menu_frame_select(e->frame, e, TRUE);
                if (ev->type == OB_INPUT_BUTTON_RELEASE)
                    menu_entry_frame_execute(e, ev->mods);
            }
            else
                menu_frame_hide_all();
        }
        ret = TRUE;
    }
    else if (ev->type == OB_INPUT_KEY_PRESS || ev->type == OB_INPUT_KEY_RELEASE) {
        guint mods;
        ObMenuFrame *frame;

        /* get the modifiers */
        mods = obt_keyboard_only_modmasks(ev->mods);

        frame = find_active_or_last_menu();
        if (frame == NULL)
            g_assert_not_reached(); /* there is no active menu */

        /* Allow control while going thru the menu */
        else if (ev->type == OB_INPUT_KEY_PRESS && (mods & ~ControlMask) == 0) {
            gunichar unikey;
            KeySym sym;

            frame->got_press = TRUE;
            frame->press_keycode = ev->keycode;
            frame->press_doexec = FALSE;

            sym = ev->keysym;

            if (sym == XK_Escape) {
                menu_frame_hide_all();
                ret = TRUE;
            }

            else if (sym == XK_Left) {
                /* Left goes to the parent menu */
                if (frame->parent) {
                    /* remove focus from the child */
                    menu_frame_select(frame, NULL, TRUE);
                    /* and put it in the parent */
                    menu_frame_select(frame->parent, frame->parent->selected,
                                      TRUE);
                }
                ret = TRUE;
            }

            else if (sym == XK_Right || sym == XK_Return || sym == XK_KP_Enter)
            {
                /* Right and enter goes to the selected submenu.
                   Enter executes instead if it's not on a submenu. */

                if (frame->selected) {
                    const ObMenuEntryType t = frame->selected->entry->type;

                    if (t == OB_MENU_ENTRY_TYPE_SUBMENU) {
                        /* make sure it is visible */
                        menu_frame_select(frame, frame->selected, TRUE);
                        /* move focus to the child menu */
                        menu_frame_select_next(frame->child);
                    }
                    else if (sym != XK_Right) {
                        frame->press_doexec = TRUE;
                    }
                }
                ret = TRUE;
            }

            else if (sym == XK_Up) {
                menu_frame_select_previous(frame);
                ret = TRUE;
            }

            else if (sym == XK_Down) {
                menu_frame_select_next(frame);
                ret = TRUE;
            }

            else if (sym == XK_Home) {
                menu_frame_select_first(frame);
                ret = TRUE;
            }

            else if (sym == XK_End) {
                menu_frame_select_last(frame);
                ret = TRUE;
            }

            /* keyboard accelerator shortcuts. (if it was a valid key) */
            else if (frame->entries &&
                     (unikey = ev->text))
            {
                GList *start;
                GList *it;
                ObMenuEntryFrame *found = NULL;
                guint num_found = 0;

                /* start after the selected one */
                start = frame->entries;
                if (frame->selected) {
                    for (it = start; frame->selected != it->data;
                         it = g_list_next(it))
                        g_assert(it != NULL); /* nothing was selected? */
                    /* next with wraparound */
                    start = g_list_next(it);
                    if (start == NULL) start = frame->entries;
                }

                it = start;
                do {
                    ObMenuEntryFrame *e = it->data;
                    gunichar entrykey = 0;

                    if (e->entry->type == OB_MENU_ENTRY_TYPE_NORMAL)
                        entrykey = e->entry->data.normal.shortcut;
                    else if (e->entry->type == OB_MENU_ENTRY_TYPE_SUBMENU)
                        entrykey = e->entry->data.submenu.submenu->shortcut;

                    if (unikey == entrykey) {
                        if (found == NULL) found = e;
                        ++num_found;
                    }

                    /* next with wraparound */
                    it = g_list_next(it);
                    if (it == NULL) it = frame->entries;
                } while (it != start);

                if (found) {
                    menu_frame_select(frame, found, TRUE);

                    if (num_found == 1) {
                        if (found->entry->type == OB_MENU_ENTRY_TYPE_SUBMENU) {
                            /* move focus to the child menu */
                            menu_frame_select_next(frame->child);
                        }
                        else {
                            frame->press_doexec = TRUE;
                        }
                    }
                    ret = TRUE;
                }
            }
        }

        /* Use KeyRelease events for running things so that the key release
           doesn't get sent to the focused application.

           Allow ControlMask only, and don't bother if the menu is empty */
        else if (ev->type == OB_INPUT_KEY_RELEASE && (mods & ~ControlMask) == 0) {
            if (frame->press_keycode == ev->keycode &&
                frame->got_press &&
                frame->press_doexec)
            {
                if (frame->selected)
                    menu_entry_frame_execute(frame->selected, ev->mods);
            }
        }
    }

    return ret;
}

static void event_handle_menu(ObMenuFrame *frame, const ObInputEvent *ev)
{
    ObMenuFrame *f;
    ObMenuEntryFrame *e;
    Window over = (Window)ev->over, next = (Window)ev->next_over;

    switch (ev->type) {
    case OB_INPUT_MOTION:
        /* We need to catch motion in addition to enters because
           it is possible for the menu to be opened under the mouse cursor, and
           moving the mouse should select the item. */
    case OB_INPUT_ENTER:
        if ((e = g_hash_table_lookup(menu_frame_map, &over))) {
            if (e->ignore_enters)
                --e->ignore_enters;
            else if (!(f = find_active_menu()) ||
                     f == e->frame ||
                     f->parent == e->frame ||
                     f->child == e->frame)
                menu_frame_select(e->frame, e, FALSE);
        }
        break;
    case OB_INPUT_LEAVE:
        /* ignore leaves when we're already in the window */
        if (ev->inferior)
            break;

        if ((e = g_hash_table_lookup(menu_frame_map, &over))) {
            ObMenuEntryFrame *n;

            /* if the pointer goes on to another entry of the menu, that one
               gets selected, else select nothing in the menu */
            if (!(next &&
                  (n = g_hash_table_lookup(menu_frame_map, &next)) &&
                  n->frame == e->frame && !n->ignore_enters))
                menu_frame_select(e->frame, NULL, FALSE);
        }
        break;
    default:
        break;
    }
}

static gboolean event_handle_user_input(ObClient *client, XEvent *e)
{
    ObInputEvent input;

    g_assert(e->type == ButtonPress || e->type == ButtonRelease ||
             e->type == MotionNotify || e->type == KeyPress ||
             e->type == KeyRelease);

    backend_x11_input_event(e, moveresize_latest_motion(), &input);

    if (menu_frame_visible) {
        /* menus have keyboard accelerators: ask what the key types
           (menus are always used through a grab, so use its input
           context) */
        if (e->type == KeyPress)
            input.text = obt_keyboard_keypress_to_unichar(
                grab_input_context(), e);
        if (event_handle_menu_input(&input))
            /* don't use the event if the menu used it, but if the menu
               didn't use it and it's a keypress that is bound, it will
               close the menu and be used */
            return TRUE;
    }

    /* if the keyboard interactive action uses the event then dont
       use it for bindings. likewise is moveresize uses the event. */
    if (actions_interactive_input_event(&input) || moveresize_event(&input))
        return TRUE;

    if (moveresize_in_progress)
        /* make further actions work on the client being
           moved/resized */
        client = moveresize_client;

    if (e->type == ButtonPress ||
        e->type == ButtonRelease ||
        e->type == MotionNotify)
    {
        /* the frame may not be "visible" but they can still click on it
           in the case where it is animating before disappearing */
        if (!client || !frame_iconify_animating(client->frame))
            return mouse_event(client, &input);
    } else
        return keyboard_event((focus_cycle_target ? focus_cycle_target :
                               (client ? client : focus_client)), &input);

    return FALSE;
}

static void focus_delay_dest(gpointer data)
{
    g_slice_free(ObFocusDelayData, data);
    focus_delay_timeout_id = 0;
    focus_delay_timeout_client = NULL;
}

static void unfocus_delay_dest(gpointer data)
{
    g_slice_free(ObFocusDelayData, data);
    unfocus_delay_timeout_id = 0;
    unfocus_delay_timeout_client = NULL;
}

static gboolean focus_delay_func(gpointer data)
{
    ObFocusDelayData *d = data;
    Time old = event_curtime; /* save the curtime */

    event_curtime = d->time;
    event_curserial = d->serial;
    if (client_focus(d->client) && config_focus_raise)
        stacking_raise(CLIENT_AS_WINDOW(d->client));
    event_curtime = old;

    return FALSE; /* no repeat */
}

static gboolean unfocus_delay_func(gpointer data)
{
    ObFocusDelayData *d = data;
    Time old = event_curtime; /* save the curtime */

    event_curtime = d->time;
    event_curserial = d->serial;
    focus_nothing();
    event_curtime = old;

    return FALSE; /* no repeat */
}

static void focus_delay_client_dest(ObClient *client, gpointer data)
{
    if (focus_delay_timeout_client == client && focus_delay_timeout_id)
        g_source_remove(focus_delay_timeout_id);
    if (unfocus_delay_timeout_client == client && unfocus_delay_timeout_id)
        g_source_remove(unfocus_delay_timeout_id);
}

void event_halt_focus_delay(void)
{
    /* ignore all enter events up till the event which caused this to occur */
    if (event_curserial) event_ignore_enter_range(1, event_curserial);
    if (focus_delay_timeout_id) g_source_remove(focus_delay_timeout_id);
    if (unfocus_delay_timeout_id) g_source_remove(unfocus_delay_timeout_id);
}

gulong event_start_ignore_all_enters(void)
{
    return NextRequest(obt_display);
}

static void event_ignore_enter_range(gulong start, gulong end)
{
    ObSerialRange *r;

    g_assert(start != 0);
    g_assert(end != 0);

    r = g_slice_new(ObSerialRange);
    r->start = start;
    r->end = end;
    ignore_serials = g_slist_prepend(ignore_serials, r);

    ob_debug_type(OB_DEBUG_FOCUS, "ignoring enters from %lu until %lu",
                  r->start, r->end);

    /* increment the serial so we don't ignore events we weren't meant to */
    OBT_PROP_ERASE(screen_support_win, MOTIF_WM_HINTS);
}

void event_end_ignore_all_enters(gulong start)
{
    /* Use (NextRequest-1) so that we ignore up to the current serial only.
       Inside event_ignore_enter_range, we increment the serial by one, but if
       we ignore that serial too, then any enter events generated by mouse
       movement will be ignored until we create some further network traffic.
       Instead ignore up to NextRequest-1, then when we increment the serial,
       we will be *past* the range of ignored serials */
    event_ignore_enter_range(start, NextRequest(obt_display)-1);
}

static gboolean is_enter_focus_event_ignored(gulong serial)
{
    GSList *it, *next;

    for (it = ignore_serials; it; it = next) {
        ObSerialRange *r = it->data;

        next = g_slist_next(it);

        if ((glong)(serial - r->end) > 0) {
            /* past the end */
            ignore_serials = g_slist_delete_link(ignore_serials, it);
            g_slice_free(ObSerialRange, r);
        }
        else if ((glong)(serial - r->start) >= 0)
            return TRUE;
    }
    return FALSE;
}

void event_cancel_all_key_grabs(void)
{
    if (actions_interactive_act_running()) {
        actions_interactive_cancel_act();
        ob_debug("KILLED interactive action");
    }
    else if (menu_frame_visible) {
        menu_frame_hide_all();
        ob_debug("KILLED open menus");
    }
    else if (moveresize_in_progress) {
        moveresize_end(TRUE);
        ob_debug("KILLED interactive moveresize");
    }
    else if (grab_on_keyboard()) {
        ungrab_keyboard();
        ob_debug("KILLED active grab on keyboard");
    }
    else
        ungrab_passive_key();

    XSync(obt_display, FALSE);
}

gboolean event_time_after(guint32 t1, guint32 t2)
{
    g_assert(t1 != CurrentTime);
    g_assert(t2 != CurrentTime);

    /*
      Timestamp values wrap around (after about 49.7 days). The server, given
      its current time is represented by timestamp T, always interprets
      timestamps from clients by treating half of the timestamp space as being
      later in time than T.
      - http://tronche.com/gui/x/xlib/input/pointer-grabbing.html
    */

    /* TIME_HALF is not half of the number space of a Time type variable.
     * Rather, it is half the number space of a timestamp value, which is
     * always 32 bits. */
#define TIME_HALF (guint32)(1 << 31)

    if (t2 >= TIME_HALF)
        /* t2 is in the second half so t1 might wrap around and be smaller than
           t2 */
        return t1 >= t2 || t1 < (t2 + TIME_HALF);
    else
        /* t2 is in the first half so t1 has to come after it */
        return t1 >= t2 && t1 < (t2 + TIME_HALF);
}

Time event_time(void)
{
    /* some events don't come with timestamps, so ask for one, and keep it
       for the rest of this event */
    if (!event_curtime)
        event_curtime = (*ob_backend->time)();
    return event_curtime;
}

Time event_source_time(void)
{
    return event_sourcetime;
}

void event_set_source_time(Time t)
{
    event_sourcetime = t;
}

void event_reset_time(void)
{
    event_curtime = (*ob_backend->time)();
}

void event_update_user_time(void)
{
    event_last_user_time = event_time();
}

void event_reset_user_time(void)
{
    event_last_user_time = CurrentTime;
}
