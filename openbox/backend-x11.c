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
#include "backend-x11-int.h"
#include "openbox.h"
#include "debug.h"
#include "screen.h"
#include "event.h"
#include "version.h"
#include "gettext.h"
#include "obrender/render.h"
#include "obt/display.h"
#include "obt/xqueue.h"
#include "obt/prop.h"
#include "obt/keyboard.h"

#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#if USE_XCURSOR
#include <X11/Xcursor/Xcursor.h>
#endif
#ifdef SHAPE
#include <X11/extensions/shape.h>
#endif
#ifdef SYNC
#include <X11/extensions/sync.h>
#endif
#include <string.h>
#include <unistd.h>

const ObBackendEvents *x11_events;
static gboolean replace; /* take over from a running window manager */

/* never shown: it holds the WM_Sn selection, takes the focus when no
   client has it, and anchors the stacking order */
Window screen_support_win = None;

static Window root(void);

/* the outputs as output_count() last found them */
static Rect *outputs;
static guint n_outputs;

static Cursor cursors[OB_NUM_CURSORS];

/* the input method context for keys typed during a grab */
static ObtIC *grab_ic = NULL;

static Cursor load_cursor(const gchar *name, guint fontval)
{
    Cursor c = None;

#if USE_XCURSOR
    c = XcursorLibraryLoadCursor(obt_display, name);
#endif
    if (c == None)
        c = XCreateFontCursor(obt_display, fontval);
    return c;
}

Cursor x11_cursor(ObCursor cursor)
{
    g_assert(cursor < OB_NUM_CURSORS);
    return cursors[cursor];
}

static gboolean x11_startup(const gchar *display, ObStartupFlags flags,
                            const ObBackendEvents *ev)
{
    replace = !!(flags & OB_STARTUP_REPLACE);
    x11_events = ev;
    if (!obt_display_open(display))
        return FALSE;

    ob_screen = DefaultScreen(obt_display);

    XSynchronize(obt_display, !!(flags & OB_STARTUP_SYNC));

#ifdef XKB
    /* a held key repeats presses only, so bindings can tell them apart */
    XkbSetDetectableAutoRepeat(obt_display, True, NULL);
#endif

    /* check for locale support */
    if (!XSupportsLocale())
        g_message(_("X server does not support locale."));
    if (!XSetLocaleModifiers(""))
        g_message(_("Cannot set locale modifiers for the X server."));

    /* set the DISPLAY environment variable for any lauched children, to the
       display we're using, so they open in the right place. */
    g_setenv("DISPLAY", DisplayString(obt_display), TRUE);

    /* create available cursors */
    cursors[OB_CURSOR_NONE] = None;
    cursors[OB_CURSOR_POINTER] = load_cursor("left_ptr", XC_left_ptr);
    cursors[OB_CURSOR_BUSYPOINTER] = load_cursor("left_ptr_watch",XC_left_ptr);
    cursors[OB_CURSOR_BUSY] = load_cursor("watch", XC_watch);
    cursors[OB_CURSOR_MOVE] = load_cursor("fleur", XC_fleur);
    cursors[OB_CURSOR_NORTH] = load_cursor("top_side", XC_top_side);
    cursors[OB_CURSOR_NORTHEAST] = load_cursor("top_right_corner",
                                               XC_top_right_corner);
    cursors[OB_CURSOR_EAST] = load_cursor("right_side", XC_right_side);
    cursors[OB_CURSOR_SOUTHEAST] = load_cursor("bottom_right_corner",
                                               XC_bottom_right_corner);
    cursors[OB_CURSOR_SOUTH] = load_cursor("bottom_side", XC_bottom_side);
    cursors[OB_CURSOR_SOUTHWEST] = load_cursor("bottom_left_corner",
                                               XC_bottom_left_corner);
    cursors[OB_CURSOR_WEST] = load_cursor("left_side", XC_left_side);
    cursors[OB_CURSOR_NORTHWEST] = load_cursor("top_left_corner",
                                               XC_top_left_corner);
    return TRUE;
}

static void x11_set_cursor(ObCanvasId c, ObCursor cursor)
{
    XSetWindowAttributes a;

    /* OB_CANVAS_NONE is the desktop */
    a.cursor = x11_cursor(cursor);
    XChangeWindowAttributes(obt_display, c ? (Window)c : root(), CWCursor,
                            &a);
}

static void x11_request_running(guint what)
{
    OBT_PROP_MSG(ob_screen, root(), OB_CONTROL, what, 0, 0, 0, 0);
    XFlush(obt_display);
}

static void x11_previous_state(ObPreviousState *st)
{
    guint32 d, xid;
    gchar **names;

    st->has_names = OBT_PROP_GETSS_UTF8(root(), NET_DESKTOP_NAMES, &names);
    if (st->has_names)
        g_strfreev(names);
    st->has_count = OBT_PROP_GET32(root(), NET_NUMBER_OF_DESKTOPS, CARDINAL,
                                   &d);
    st->count = st->has_count ? d : 0;
    st->has_current = OBT_PROP_GET32(root(), NET_CURRENT_DESKTOP, CARDINAL,
                                     &d);
    st->current = st->has_current ? d : 0;
    st->has_last = OBT_PROP_GET32(root(), OB_LAST_DESKTOP, CARDINAL, &d);
    st->last = st->has_last ? d : 0;
    st->active = OBT_PROP_GET32(root(), NET_ACTIVE_WINDOW, WINDOW, &xid) ?
        (ObSurfaceId)xid : OB_SURFACE_NONE;
}

static gchar **x11_desktop_names(void)
{
    gchar **names;

    if (!OBT_PROP_GETSS(root(), NET_DESKTOP_NAMES, &names))
        return NULL;
    return names;
}

static gboolean x11_desktop_layout(ObDesktopLayout *l)
{
    guint32 *data;
    guint num;
    gboolean ok = FALSE;

    if (!OBT_PROP_GETA32(root(), NET_DESKTOP_LAYOUT, CARDINAL, &data, &num))
        return FALSE;

    if (num == 3 || num == 4) {
        ok = TRUE;
        if (data[0] == OBT_PROP_ATOM(NET_WM_ORIENTATION_VERT))
            l->orientation = OB_ORIENTATION_VERT;
        else if (data[0] == OBT_PROP_ATOM(NET_WM_ORIENTATION_HORZ))
            l->orientation = OB_ORIENTATION_HORZ;
        else
            ok = FALSE;

        if (num < 4)
            l->start_corner = OB_CORNER_TOPLEFT;
        else if (data[3] == OBT_PROP_ATOM(NET_WM_TOPLEFT))
            l->start_corner = OB_CORNER_TOPLEFT;
        else if (data[3] == OBT_PROP_ATOM(NET_WM_TOPRIGHT))
            l->start_corner = OB_CORNER_TOPRIGHT;
        else if (data[3] == OBT_PROP_ATOM(NET_WM_BOTTOMRIGHT))
            l->start_corner = OB_CORNER_BOTTOMRIGHT;
        else if (data[3] == OBT_PROP_ATOM(NET_WM_BOTTOMLEFT))
            l->start_corner = OB_CORNER_BOTTOMLEFT;
        else
            ok = FALSE;

        l->columns = data[1];
        l->rows = data[2];
    }
    g_free(data);
    return ok;
}

static void x11_layout_size(gint *w, gint *h)
{
    *w = WidthOfScreen(ScreenOfDisplay(obt_display, ob_screen));
    *h = HeightOfScreen(ScreenOfDisplay(obt_display, ob_screen));
}

static guint x11_reserved_edges(const StrutPartial **edges)
{
    /* docks and panels are clients here, with struts of their own */
    *edges = NULL;
    return 0;
}

static void x11_publish_layout_size(gint w, gint h)
{
    gulong geometry[2];

    geometry[0] = w;
    geometry[1] = h;
    OBT_PROP_SETA32(root(), NET_DESKTOP_GEOMETRY, CARDINAL, geometry, 2);
}

static void x11_publish_last_desktop(guint desktop)
{
    OBT_PROP_SET32(root(), OB_LAST_DESKTOP, CARDINAL, desktop);
}

static void x11_publish_showing_desktop(gboolean showing)
{
    OBT_PROP_SET32(root(), NET_SHOWING_DESKTOP, CARDINAL, !!showing);
}

static void x11_publish_config_file(const gchar *path)
{
    if (path)
        OBT_PROP_SETS(root(), OB_CONFIG_FILE, path);
    else
        OBT_PROP_ERASE(root(), OB_CONFIG_FILE);
}

static void x11_publish_theme(const gchar *name)
{
    OBT_PROP_SETS(root(), OB_THEME, name);
}

static void x11_shutdown(void)
{
    x11_events_stop();
    obt_display_close();
    x11_events = NULL;
    g_free(outputs);
    outputs = NULL;
    n_outputs = 0;
}

/* --- time --- */

guint32 x11_event_time(const XEvent *e)
{
    Time t = CurrentTime;

    /* grab the lasttime and hack up the state */
    switch (e->type) {
    case ButtonPress:
    case ButtonRelease:
        t = e->xbutton.time;
        break;
    case KeyPress:
        t = e->xkey.time;
        break;
    case KeyRelease:
        t = e->xkey.time;
        break;
    case MotionNotify:
        t = e->xmotion.time;
        break;
    case PropertyNotify:
        t = e->xproperty.time;
        break;
    case EnterNotify:
    case LeaveNotify:
        t = e->xcrossing.time;
        break;
    default:
#ifdef SYNC
        if (obt_display_extension_sync &&
            e->type == obt_display_extension_sync_basep + XSyncAlarmNotify)
        {
            t = ((const XSyncAlarmNotifyEvent*)e)->time;
        }
#endif
        /* if more event types are anticipated, get their timestamp
           explicitly */
        break;
    }

    return t;
}

static gboolean find_timestamp(XEvent *e, gpointer data)
{
    Time *t = data;

    *t = x11_event_time(e);
    return *t != CurrentTime;
}

static gboolean find_enter(XEvent *e, gpointer data)
{
    if (e->type != EnterNotify) return FALSE;
    *(Window*)data = e->xcrossing.window;
    return TRUE;
}

static gboolean translate_input(const XEvent *e, ObInputEvent *out)
{
    out->crossing = OB_CROSSING_NORMAL;
    out->inferior = FALSE;
    out->next_over = 0;
    out->under = 0;

    switch (e->type) {
    case KeyPress:
    case KeyRelease:
        out->type = e->type == KeyPress ? OB_INPUT_KEY_PRESS
                                        : OB_INPUT_KEY_RELEASE;
        out->time = e->xkey.time;
        out->mods = e->xkey.state;
        out->keycode = e->xkey.keycode;
        out->keysym = e->type == KeyPress ?
            obt_keyboard_keypress_to_keysym((XEvent*)e) : NoSymbol;
        out->key_mod = obt_keyboard_keyevent_to_modmask((XEvent*)e);
        out->text = 0;
        out->button = 0;
        out->root_x = e->xkey.x_root;
        out->root_y = e->xkey.y_root;
        out->over = e->xkey.window;
        out->x = e->xkey.x;
        out->y = e->xkey.y;
        out->inside = FALSE;
        return TRUE;
    case ButtonPress:
    case ButtonRelease:
        out->type = e->type == ButtonPress ? OB_INPUT_BUTTON_PRESS
                                           : OB_INPUT_BUTTON_RELEASE;
        out->time = e->xbutton.time;
        out->mods = e->xbutton.state;
        out->keycode = 0;
        out->keysym = NoSymbol;
        out->key_mod = 0;
        out->text = 0;
        out->button = e->xbutton.button;
        out->root_x = e->xbutton.x_root;
        out->root_y = e->xbutton.y_root;
        out->over = e->xbutton.window;
        out->under = e->xbutton.window == root() ?
            e->xbutton.subwindow : None;
        out->x = e->xbutton.x;
        out->y = e->xbutton.y;
        {
            gint junk1, junk2;
            Window wjunk;
            guint ujunk, b, w, h;
            /* this can cause errors to occur when the window closes */
            obt_display_ignore_errors(TRUE);
            junk1 = XGetGeometry(obt_display, e->xbutton.window,
                                 &wjunk, &junk1, &junk2, &w, &h, &b, &ujunk);
            obt_display_ignore_errors(FALSE);
            out->inside = junk1 &&
                e->xbutton.x >= (signed)-b && e->xbutton.y >= (signed)-b &&
                e->xbutton.x < (signed)(w+b) && e->xbutton.y < (signed)(h+b);
        }
        return TRUE;
    case MotionNotify:
        out->type = OB_INPUT_MOTION;
        out->time = e->xmotion.time;
        out->mods = e->xmotion.state;
        out->keycode = 0;
        out->keysym = NoSymbol;
        out->key_mod = 0;
        out->text = 0;
        out->button = 0;
        out->root_x = e->xmotion.x_root;
        out->root_y = e->xmotion.y_root;
        out->over = e->xmotion.window;
        out->x = e->xmotion.x;
        out->y = e->xmotion.y;
        out->inside = FALSE;
        return TRUE;
    case EnterNotify:
    case LeaveNotify:
        out->type = e->type == EnterNotify ? OB_INPUT_ENTER : OB_INPUT_LEAVE;
        out->time = e->xcrossing.time;
        out->mods = e->xcrossing.state;
        out->keycode = 0;
        out->keysym = NoSymbol;
        out->key_mod = 0;
        out->text = 0;
        out->button = 0;
        out->root_x = e->xcrossing.x_root;
        out->root_y = e->xcrossing.y_root;
        out->over = e->xcrossing.window;
        out->x = e->xcrossing.x;
        out->y = e->xcrossing.y;
        out->inside = FALSE;
        out->crossing = e->xcrossing.mode == NotifyGrab ? OB_CROSSING_GRAB :
            e->xcrossing.mode == NotifyUngrab ? OB_CROSSING_UNGRAB :
            OB_CROSSING_NORMAL;
        out->inferior = e->xcrossing.detail == NotifyInferior;
        if (e->type == LeaveNotify) {
            Window next = None;

            /* the enter that goes with this leave is queued with it */
            xqueue_exists_local(find_enter, &next);
            out->next_over = next;
        }
        return TRUE;
    default:
        return FALSE;
    }
}

gboolean x11_input_event(const XEvent *e, ObInputEvent *out)
{
    if (!translate_input(e, out)) return FALSE;
    out->desktop = out->over == obt_root(ob_screen);
    return TRUE;
}

static guint32 x11_time(void)
{
    Time t = CurrentTime;

    /* Some events don't come with timestamps :(
       ...but we can get one anyways >:)
       Generate a timestamp so there is guaranteed at least one in the queue
       eventually */
    XChangeProperty(obt_display, screen_support_win,
                    OBT_PROP_ATOM(WM_CLASS), OBT_PROP_ATOM(STRING),
                    8, PropModeAppend, NULL, 0);

    /* Grab the first timestamp available */
    xqueue_exists(find_timestamp, &t);
    return t;
}

/* --- the screen --- */

/*! The event mask to grab on the root window */
#define ROOT_EVENTMASK (StructureNotifyMask | PropertyChangeMask | \
                        EnterWindowMask | LeaveWindowMask | \
                        SubstructureRedirectMask | FocusChangeMask | \
                        ButtonPressMask | ButtonReleaseMask)

static gboolean replace_wm(void)
{
    gchar *wm_sn;
    Atom wm_sn_atom;
    Window current_wm_sn_owner;
    Time timestamp;

    wm_sn = g_strdup_printf("WM_S%d", ob_screen);
    wm_sn_atom = XInternAtom(obt_display, wm_sn, FALSE);
    g_free(wm_sn);

    current_wm_sn_owner = XGetSelectionOwner(obt_display, wm_sn_atom);
    if (current_wm_sn_owner == screen_support_win)
        current_wm_sn_owner = None;
    if (current_wm_sn_owner) {
        if (!replace) {
            g_message(_("A window manager is already running on screen %d"),
                      ob_screen);
            return FALSE;
        }
        obt_display_ignore_errors(TRUE);

        /* We want to find out when the current selection owner dies */
        XSelectInput(obt_display, current_wm_sn_owner, StructureNotifyMask);
        XSync(obt_display, FALSE);

        obt_display_ignore_errors(FALSE);
        if (obt_display_error_occured)
            current_wm_sn_owner = None;
    }

    timestamp = event_time();

    XSetSelectionOwner(obt_display, wm_sn_atom, screen_support_win,
                       timestamp);

    if (XGetSelectionOwner(obt_display, wm_sn_atom) != screen_support_win) {
        g_message(_("Could not acquire window manager selection on screen %d"),
                  ob_screen);
        return FALSE;
    }

    /* Wait for old window manager to go away */
    if (current_wm_sn_owner) {
      gulong wait = 0;
      const gulong timeout = G_USEC_PER_SEC * 15; /* wait for 15s max */
      ObtXQueueWindowType wt;

      wt.window = current_wm_sn_owner;
      wt.type = DestroyNotify;

      while (wait < timeout) {
          /* Checks the local queue and incoming events for this event */
          if (xqueue_exists_local(xqueue_match_window_type, &wt))
              break;
          g_usleep(G_USEC_PER_SEC / 10);
          wait += G_USEC_PER_SEC / 10;
      }

      if (wait >= timeout) {
          g_message(_("The WM on screen %d is not exiting"), ob_screen);
          return FALSE;
      }
    }

    /* Send client message indicating that we are now the WM */
    obt_prop_message(ob_screen, obt_root(ob_screen), OBT_PROP_ATOM(MANAGER),
                     timestamp, wm_sn_atom, screen_support_win, 0, 0,
                     SubstructureNotifyMask);

    return TRUE;
}

static gboolean x11_claim_screen(void)
{
    XSetWindowAttributes attrib;
    pid_t pid;
    gint i, num_support;
    gulong *supported;

    /* create the netwm support window */
    attrib.override_redirect = TRUE;
    attrib.event_mask = PropertyChangeMask;
    screen_support_win = XCreateWindow(obt_display, obt_root(ob_screen),
                                       -100, -100, 1, 1, 0,
                                       CopyFromParent, InputOutput,
                                       CopyFromParent,
                                       CWEventMask | CWOverrideRedirect,
                                       &attrib);
    XMapWindow(obt_display, screen_support_win);
    XLowerWindow(obt_display, screen_support_win);

    if (!replace_wm()) {
        XDestroyWindow(obt_display, screen_support_win);
        return FALSE;
    }

    obt_display_ignore_errors(TRUE);
    XSelectInput(obt_display, obt_root(ob_screen), ROOT_EVENTMASK);
    obt_display_ignore_errors(FALSE);
    if (obt_display_error_occured) {
        g_message(_("A window manager is already running on screen %d"),
                  ob_screen);

        XDestroyWindow(obt_display, screen_support_win);
        return FALSE;
    }

    /* set the OPENBOX_PID hint */
    pid = getpid();
    OBT_PROP_SET32(obt_root(ob_screen), OPENBOX_PID, CARDINAL, pid);

    /* set supporting window */
    OBT_PROP_SET32(obt_root(ob_screen),
                   NET_SUPPORTING_WM_CHECK, WINDOW, screen_support_win);

    /* set properties on the supporting window */
    OBT_PROP_SETS(screen_support_win, NET_WM_NAME, "Openbox");
    OBT_PROP_SET32(screen_support_win, NET_SUPPORTING_WM_CHECK,
                   WINDOW, screen_support_win);

    /* set the _NET_SUPPORTED_ATOMS hint */

    /* this is all the atoms after NET_SUPPORTED in the ObtPropAtoms enum */
    num_support = OBT_PROP_NUM_ATOMS - OBT_PROP_NET_SUPPORTED - 1;
    i = 0;
    supported = g_new(gulong, num_support);
    supported[i++] = OBT_PROP_ATOM(NET_SUPPORTING_WM_CHECK);
    supported[i++] = OBT_PROP_ATOM(NET_WM_FULL_PLACEMENT);
    supported[i++] = OBT_PROP_ATOM(NET_CURRENT_DESKTOP);
    supported[i++] = OBT_PROP_ATOM(NET_NUMBER_OF_DESKTOPS);
    supported[i++] = OBT_PROP_ATOM(NET_DESKTOP_GEOMETRY);
    supported[i++] = OBT_PROP_ATOM(NET_DESKTOP_VIEWPORT);
    supported[i++] = OBT_PROP_ATOM(NET_ACTIVE_WINDOW);
    supported[i++] = OBT_PROP_ATOM(NET_WORKAREA);
    supported[i++] = OBT_PROP_ATOM(NET_CLIENT_LIST);
    supported[i++] = OBT_PROP_ATOM(NET_CLIENT_LIST_STACKING);
    supported[i++] = OBT_PROP_ATOM(NET_DESKTOP_NAMES);
    supported[i++] = OBT_PROP_ATOM(NET_CLOSE_WINDOW);
    supported[i++] = OBT_PROP_ATOM(NET_DESKTOP_LAYOUT);
    supported[i++] = OBT_PROP_ATOM(NET_SHOWING_DESKTOP);
    supported[i++] = OBT_PROP_ATOM(NET_WM_NAME);
    supported[i++] = OBT_PROP_ATOM(NET_WM_VISIBLE_NAME);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ICON_NAME);
    supported[i++] = OBT_PROP_ATOM(NET_WM_VISIBLE_ICON_NAME);
    supported[i++] = OBT_PROP_ATOM(NET_WM_DESKTOP);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STRUT);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STRUT_PARTIAL);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ICON);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ICON_GEOMETRY);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_DESKTOP);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_DOCK);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_TOOLBAR);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_MENU);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_UTILITY);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_SPLASH);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_DIALOG);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_NORMAL);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ALLOWED_ACTIONS);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_OPACITY);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_MOVE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_RESIZE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_MINIMIZE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_SHADE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_MAXIMIZE_HORZ);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_MAXIMIZE_VERT);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_FULLSCREEN);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_CHANGE_DESKTOP);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_CLOSE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_ABOVE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_BELOW);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_MODAL);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_MAXIMIZED_VERT);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_MAXIMIZED_HORZ);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_SHADED);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_SKIP_TASKBAR);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_SKIP_PAGER);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_HIDDEN);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_FULLSCREEN);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_ABOVE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_BELOW);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_DEMANDS_ATTENTION);
    supported[i++] = OBT_PROP_ATOM(NET_MOVERESIZE_WINDOW);
    supported[i++] = OBT_PROP_ATOM(NET_WM_MOVERESIZE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_USER_TIME);
/*
    supported[i++] = OBT_PROP_ATOM(NET_WM_USER_TIME_WINDOW);
*/
    supported[i++] = OBT_PROP_ATOM(NET_FRAME_EXTENTS);
    supported[i++] = OBT_PROP_ATOM(NET_REQUEST_FRAME_EXTENTS);
    supported[i++] = OBT_PROP_ATOM(NET_RESTACK_WINDOW);
    supported[i++] = OBT_PROP_ATOM(NET_STARTUP_ID);
#ifdef SYNC
    supported[i++] = OBT_PROP_ATOM(NET_WM_SYNC_REQUEST);
    supported[i++] = OBT_PROP_ATOM(NET_WM_SYNC_REQUEST_COUNTER);
#endif
    supported[i++] = OBT_PROP_ATOM(NET_WM_PID);
    supported[i++] = OBT_PROP_ATOM(NET_WM_PING);

    supported[i++] = OBT_PROP_ATOM(KDE_WM_CHANGE_STATE);
    supported[i++] = OBT_PROP_ATOM(KDE_NET_WM_FRAME_STRUT);
    supported[i++] = OBT_PROP_ATOM(KDE_NET_WM_WINDOW_TYPE_OVERRIDE);

    supported[i++] = OBT_PROP_ATOM(OB_WM_ACTION_UNDECORATE);
    supported[i++] = OBT_PROP_ATOM(OB_WM_STATE_UNDECORATED);
    supported[i++] = OBT_PROP_ATOM(OPENBOX_PID);
    supported[i++] = OBT_PROP_ATOM(OB_THEME);
    supported[i++] = OBT_PROP_ATOM(OB_CONFIG_FILE);
    supported[i++] = OBT_PROP_ATOM(OB_LAST_DESKTOP);
    supported[i++] = OBT_PROP_ATOM(OB_CONTROL);
    supported[i++] = OBT_PROP_ATOM(OB_VERSION);
    supported[i++] = OBT_PROP_ATOM(OB_APP_ROLE);
    supported[i++] = OBT_PROP_ATOM(OB_APP_TITLE);
    supported[i++] = OBT_PROP_ATOM(OB_APP_NAME);
    supported[i++] = OBT_PROP_ATOM(OB_APP_CLASS);
    supported[i++] = OBT_PROP_ATOM(OB_APP_GROUP_NAME);
    supported[i++] = OBT_PROP_ATOM(OB_APP_GROUP_CLASS);
    supported[i++] = OBT_PROP_ATOM(OB_APP_TYPE);
    g_assert(i == num_support);

    OBT_PROP_SETA32(obt_root(ob_screen),
                    NET_SUPPORTED, ATOM, supported, num_support);
    g_free(supported);

    OBT_PROP_SETS(RootWindow(obt_display, ob_screen), OB_VERSION,
                  OPENBOX_VERSION);

    return TRUE;
}

static void x11_release_screen(void)
{
    XSelectInput(obt_display, obt_root(ob_screen), NoEventMask);

    /* we're not running here no more! */
    OBT_PROP_ERASE(obt_root(ob_screen), OPENBOX_PID);
    /* not without us */
    OBT_PROP_ERASE(obt_root(ob_screen), NET_SUPPORTED);
    /* don't keep this mode */
    OBT_PROP_ERASE(obt_root(ob_screen), NET_SHOWING_DESKTOP);

    if (grab_ic) {
        obt_keyboard_context_unref(grab_ic);
        grab_ic = NULL;
    }
    XDestroyWindow(obt_display, screen_support_win);
    screen_support_win = None;

    /* reset focus to root */
    XSetInputFocus(obt_display, PointerRoot, RevertToNone, CurrentTime);
}

static GSource *x11_event_source(void)
{
    x11_events_listen();
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

/* managed surfaces */

static gboolean x11_surface_focus(ObSurfaceId s, guint32 time,
                                  gboolean input, gboolean take_focus)
{
    obt_display_ignore_errors(TRUE);

    if (input)
        /* This can cause a BadMatch error with CurrentTime, or if an app
           passed in a bad time for _NET_WM_ACTIVE_WINDOW. */
        XSetInputFocus(obt_display, s ? (Window)s : screen_support_win,
                       RevertToPointerRoot, time);

    if (take_focus) {
        XEvent ce;
        ce.xclient.type = ClientMessage;
        ce.xclient.message_type = OBT_PROP_ATOM(WM_PROTOCOLS);
        ce.xclient.display = obt_display;
        ce.xclient.window = (Window)s;
        ce.xclient.format = 32;
        ce.xclient.data.l[0] = OBT_PROP_ATOM(WM_TAKE_FOCUS);
        ce.xclient.data.l[1] = time;
        ce.xclient.data.l[2] = 0l;
        ce.xclient.data.l[3] = 0l;
        ce.xclient.data.l[4] = 0l;
        XSendEvent(obt_display, (Window)s, FALSE, NoEventMask, &ce);
    }

    obt_display_ignore_errors(FALSE);
    return !obt_display_error_occured;
}

static void x11_surface_close(ObSurfaceId s, guint32 time)
{
    OBT_PROP_MSG_TO((Window)s, (Window)s, WM_PROTOCOLS,
                    OBT_PROP_ATOM(WM_DELETE_WINDOW), time, 0, 0, 0,
                    NoEventMask);
}

static void x11_surface_kill(ObSurfaceId s)
{
    XKillClient(obt_display, (Window)s);
}

static void x11_surface_set_states(ObSurfaceId s, ObSurfaceStates st)
{
    gulong netstate[12];
    guint num = 0;

    if (st & OB_SURFACE_STATE_MODAL)
        netstate[num++] = OBT_PROP_ATOM(NET_WM_STATE_MODAL);
    if (st & OB_SURFACE_STATE_SHADED)
        netstate[num++] = OBT_PROP_ATOM(NET_WM_STATE_SHADED);
    if (st & OB_SURFACE_STATE_ICONIC)
        netstate[num++] = OBT_PROP_ATOM(NET_WM_STATE_HIDDEN);
    if (st & OB_SURFACE_STATE_SKIP_TASKBAR)
        netstate[num++] = OBT_PROP_ATOM(NET_WM_STATE_SKIP_TASKBAR);
    if (st & OB_SURFACE_STATE_SKIP_PAGER)
        netstate[num++] = OBT_PROP_ATOM(NET_WM_STATE_SKIP_PAGER);
    if (st & OB_SURFACE_STATE_FULLSCREEN)
        netstate[num++] = OBT_PROP_ATOM(NET_WM_STATE_FULLSCREEN);
    if (st & OB_SURFACE_STATE_MAXIMIZED_VERT)
        netstate[num++] = OBT_PROP_ATOM(NET_WM_STATE_MAXIMIZED_VERT);
    if (st & OB_SURFACE_STATE_MAXIMIZED_HORZ)
        netstate[num++] = OBT_PROP_ATOM(NET_WM_STATE_MAXIMIZED_HORZ);
    if (st & OB_SURFACE_STATE_ABOVE)
        netstate[num++] = OBT_PROP_ATOM(NET_WM_STATE_ABOVE);
    if (st & OB_SURFACE_STATE_BELOW)
        netstate[num++] = OBT_PROP_ATOM(NET_WM_STATE_BELOW);
    if (st & OB_SURFACE_STATE_URGENT)
        netstate[num++] = OBT_PROP_ATOM(NET_WM_STATE_DEMANDS_ATTENTION);
    if (st & OB_SURFACE_STATE_UNDECORATED)
        netstate[num++] = OBT_PROP_ATOM(OB_WM_STATE_UNDECORATED);
    OBT_PROP_SETA32((Window)s, NET_WM_STATE, ATOM, netstate, num);
}

static void x11_surface_ping(ObSurfaceId s, guint32 serial)
{
    OBT_PROP_MSG_TO((Window)s, (Window)s, WM_PROTOCOLS,
                    OBT_PROP_ATOM(NET_WM_PING), serial, (Window)s, 0, 0,
                    NoEventMask);
}

static void get_titles(Window w, ObSurfaceInfo *info)
{
    g_free(info->title);
    if (!OBT_PROP_GETS_UTF8(w, NET_WM_NAME, &info->title) &&
        !OBT_PROP_GETS(w, WM_NAME, &info->title))
        info->title = NULL;

    g_free(info->icon_title);
    if (!OBT_PROP_GETS_UTF8(w, NET_WM_ICON_NAME, &info->icon_title) &&
        !OBT_PROP_GETS(w, WM_ICON_NAME, &info->icon_title))
        info->icon_title = NULL;
}

static void get_protocols(Window w, ObSurfaceInfo *info)
{
    guint32 *proto;
    guint num_ret, i;

    info->takes_focus = FALSE;
    info->can_close = FALSE;
    info->can_ping = FALSE;
    info->sync_resize = FALSE;

    if (OBT_PROP_GETA32(w, WM_PROTOCOLS, ATOM, &proto, &num_ret)) {
        for (i = 0; i < num_ret; ++i) {
            if (proto[i] == OBT_PROP_ATOM(WM_DELETE_WINDOW))
                info->can_close = TRUE;
            else if (proto[i] == OBT_PROP_ATOM(WM_TAKE_FOCUS))
                info->takes_focus = TRUE;
            else if (proto[i] == OBT_PROP_ATOM(NET_WM_PING))
                info->can_ping = TRUE;
#ifdef SYNC
            else if (proto[i] == OBT_PROP_ATOM(NET_WM_SYNC_REQUEST))
                info->sync_resize = TRUE;
#endif
        }
        g_free(proto);
    }
}

static void get_strut(Window w, ObSurfaceInfo *info)
{
    guint num;
    guint32 *data;
    gboolean got = FALSE;

    info->strut_whole_edges = FALSE;

    if (OBT_PROP_GETA32(w, NET_WM_STRUT_PARTIAL, CARDINAL, &data, &num)) {
        if (num == 12) {
            got = TRUE;
            STRUT_PARTIAL_SET(info->strut,
                              data[0], data[2], data[1], data[3],
                              data[4], data[5], data[8], data[9],
                              data[6], data[7], data[10], data[11]);
        }
        g_free(data);
    }

    if (!got && OBT_PROP_GETA32(w, NET_WM_STRUT, CARDINAL, &data, &num)) {
        if (num == 4) {
            got = TRUE;
            info->strut_whole_edges = TRUE;
            STRUT_PARTIAL_SET(info->strut,
                              data[0], data[2], data[1], data[3],
                              0, 0, 0, 0, 0, 0, 0, 0);
        }
        g_free(data);
    }

    if (!got)
        STRUT_PARTIAL_SET(info->strut, 0, 0, 0, 0,
                          0, 0, 0, 0, 0, 0, 0, 0);
}

/* sizes past this are taken as a broken client */
#define MAX_HINT 4096

static void get_size_hints(Window w, ObSurfaceInfo *info)
{
    XSizeHints size = {0};
    glong ret;

    info->min_ratio = 0.0f;
    info->max_ratio = 0.0f;
    SIZE_SET(info->inc, 1, 1);
    SIZE_SET(info->base, -1, -1);
    SIZE_SET(info->min, 0, 0);
    SIZE_SET(info->max, G_MAXINT, G_MAXINT);
    info->size_hints = FALSE;

    if (!XGetWMNormalHints(obt_display, w, &size, &ret))
        return;

    if ((size.flags & PAspect &&
         ((unsigned int)size.min_aspect.x > MAX_HINT ||
          (unsigned int)size.min_aspect.y > MAX_HINT)) ||
        (size.flags & PMinSize &&
         ((unsigned int)size.min_width > MAX_HINT ||
          (unsigned int)size.min_height > MAX_HINT)) ||
        (size.flags & PMaxSize &&
         ((unsigned int)size.max_width > MAX_HINT ||
          (unsigned int)size.max_height > MAX_HINT)) ||
        (size.flags & PBaseSize &&
         ((unsigned int)size.base_width > MAX_HINT ||
          (unsigned int)size.base_height > MAX_HINT)) ||
        (size.flags & PResizeInc && size.width_inc && size.height_inc &&
         ((unsigned int)size.width_inc > MAX_HINT ||
          (unsigned int)size.height_inc > MAX_HINT)))
        return;

    info->size_hints = TRUE;
    info->has_position = !!(size.flags & (PPosition|USPosition));
    info->has_size = !!(size.flags & (PSize|USSize));
    info->has_gravity = !!(size.flags & PWinGravity);
    if (info->has_gravity)
        info->gravity = size.win_gravity;

    if (size.flags & PAspect) {
        if (size.min_aspect.y)
            info->min_ratio = (gfloat) size.min_aspect.x / size.min_aspect.y;
        if (size.max_aspect.y)
            info->max_ratio = (gfloat) size.max_aspect.x / size.max_aspect.y;
    }
    if (size.flags & PMinSize)
        SIZE_SET(info->min, size.min_width, size.min_height);
    if (size.flags & PMaxSize)
        SIZE_SET(info->max, size.max_width, size.max_height);
    if (size.flags & PBaseSize)
        SIZE_SET(info->base, size.base_width, size.base_height);
    if (size.flags & PResizeInc && size.width_inc && size.height_inc)
        SIZE_SET(info->inc, size.width_inc, size.height_inc);
}

static void get_hints(Window w, ObSurfaceInfo *info)
{
    XWMHints *hints;

    /* assume a window takes input if it doesn't specify */
    info->accepts_focus = TRUE;
    info->hints = FALSE;

    if ((hints = XGetWMHints(obt_display, w)) == NULL)
        return;

    info->hints = TRUE;
    if (hints->flags & InputHint)
        info->accepts_focus = hints->input;
    info->has_initial_state = !!(hints->flags & StateHint);
    info->starts_iconic = info->has_initial_state &&
        hints->initial_state == IconicState;
    info->urgent = !!(hints->flags & XUrgencyHint);
    info->group = hints->flags & WindowGroupHint ?
        (ObSurfaceId)hints->window_group : OB_SURFACE_NONE;
    info->icon_in_hints = !!(hints->flags & IconPixmapHint);
    XFree(hints);
}

static void get_type(Window w, ObSurfaceInfo *info)
{
    static const struct { int atom; ObSurfaceType type; } types[] = {
        { OBT_PROP_NET_WM_WINDOW_TYPE_DESKTOP, OB_SURFACE_TYPE_DESKTOP },
        { OBT_PROP_NET_WM_WINDOW_TYPE_DOCK, OB_SURFACE_TYPE_DOCK },
        { OBT_PROP_NET_WM_WINDOW_TYPE_TOOLBAR, OB_SURFACE_TYPE_TOOLBAR },
        { OBT_PROP_NET_WM_WINDOW_TYPE_MENU, OB_SURFACE_TYPE_MENU },
        { OBT_PROP_NET_WM_WINDOW_TYPE_UTILITY, OB_SURFACE_TYPE_UTILITY },
        { OBT_PROP_NET_WM_WINDOW_TYPE_SPLASH, OB_SURFACE_TYPE_SPLASH },
        { OBT_PROP_NET_WM_WINDOW_TYPE_DIALOG, OB_SURFACE_TYPE_DIALOG },
        { OBT_PROP_NET_WM_WINDOW_TYPE_NORMAL, OB_SURFACE_TYPE_NORMAL },
    };
    guint num, i, j;
    guint32 *val;

    info->has_type = FALSE;
    info->type_override = FALSE;

    if (!OBT_PROP_GETA32(w, NET_WM_WINDOW_TYPE, ATOM, &val, &num))
        return;

    /* use the first value that we know about in the array */
    for (i = 0; i < num && !info->has_type; ++i) {
        if (val[i] == OBT_PROP_ATOM(KDE_NET_WM_WINDOW_TYPE_OVERRIDE))
            info->type_override = TRUE;
        for (j = 0; j < G_N_ELEMENTS(types); ++j)
            if (val[i] == obt_prop_atom(types[j].atom)) {
                info->has_type = TRUE;
                info->type = types[j].type;
                break;
            }
    }
    g_free(val);
}

static void get_decor_hints(Window w, ObSurfaceInfo *info)
{
    guint num;
    guint32 *hints;

    info->decor_hints.flags = 0; /* default to none */

    if (OBT_PROP_GETA32(w, MOTIF_WM_HINTS, MOTIF_WM_HINTS, &hints, &num)) {
        if (num >= OB_MWM_ELEMENTS) {
            info->decor_hints.flags = hints[0];
            info->decor_hints.functions = hints[1];
            info->decor_hints.decorations = hints[2];
        }
        g_free(hints);
    }
}

static void get_parent(Window w, ObSurfaceInfo *info)
{
    Window t = None;

    info->has_parent = XGetTransientForHint(obt_display, w, &t);
    info->parent = OB_SURFACE_NONE;
    info->parent_is_group = FALSE;
    if (!info->has_parent)
        return;

    /* Setting the transient_for to Root is actually illegal, however
       applications from time have done this to specify transient for
       their group */
    if (t == root())
        info->parent_is_group = TRUE;
    else if (t != w) /* can't be transient to itself! */
        info->parent = (ObSurfaceId)t;
}

static void get_class(Window w, gchar **app_id, gchar **instance)
{
    gchar **ss;

    g_free(*app_id);
    g_free(*instance);
    *app_id = *instance = NULL;

    if (OBT_PROP_GETSS_TYPE(w, WM_CLASS, STRING_NO_CC, &ss)) {
        if (ss[0]) {
            *instance = g_strdup(ss[0]);
            if (ss[1])
                *app_id = g_strdup(ss[1]);
        }
        g_strfreev(ss);
    }
}

static void get_ids(Window w, ObSurfaceInfo *info)
{
    guint32 leader, pid;
    gchar **ss;

    if (!OBT_PROP_GET32(w, WM_CLIENT_LEADER, WINDOW, &leader))
        leader = None;

    g_free(info->session_id);
    info->session_id = NULL;
    if (!OBT_PROP_GETS_XPCS(leader && leader != w ? leader : w, SM_CLIENT_ID,
                            &info->session_id))
        info->session_id = NULL;

    get_class(w, &info->app_id, &info->instance);
    if (leader)
        get_class(leader, &info->group_app_id, &info->group_instance);
    else {
        g_free(info->group_app_id);
        g_free(info->group_instance);
        info->group_app_id = info->group_instance = NULL;
    }

    g_free(info->role);
    if (!OBT_PROP_GETS_XPCS(w, WM_WINDOW_ROLE, &info->role))
        info->role = NULL;

    g_free(info->command);
    info->command = NULL;
    if ((leader && OBT_PROP_GETSS(leader, WM_COMMAND, &ss)) ||
        OBT_PROP_GETSS(w, WM_COMMAND, &ss))
    {
        /* merge/mash them all together */
        if (ss[0])
            info->command = g_strjoinv("", ss);
        g_strfreev(ss);
    }

    g_free(info->machine);
    info->pid = 0;
    if (!(leader && OBT_PROP_GETS(leader, WM_CLIENT_MACHINE, &info->machine))
        && !OBT_PROP_GETS(w, WM_CLIENT_MACHINE, &info->machine))
        info->machine = NULL;
    /* the PID requires that the WM_CLIENT_MACHINE be set */
    else if (OBT_PROP_GET32(w, NET_WM_PID, CARDINAL, &pid))
        info->pid = pid;
}

static void add_icon(ObSurfaceInfo *info, gint w, gint h, guint32 *argb)
{
    info->icons = g_renew(ObIcon, info->icons, info->n_icons + 1);
    info->icons[info->n_icons].width = w;
    info->icons[info->n_icons].height = h;
    info->icons[info->n_icons].argb = argb;
    ++info->n_icons;
}

static void get_icons(Window win, ObSurfaceInfo *info)
{
    guint num;
    guint32 *data, *argb;
    guint w, h, i;
    XWMHints *hints;

    for (i = 0; i < info->n_icons; ++i)
        g_free(info->icons[i].argb);
    g_free(info->icons);
    info->icons = NULL;
    info->n_icons = 0;

    if (OBT_PROP_GETA32(win, NET_WM_ICON, CARDINAL, &data, &num)) {
        /* figure out how many valid icons are in here */
        i = 0;
        while (i + 2 < num) { /* +2 is to make sure there is a w and h */
            w = data[i++];
            h = data[i++];
            /* watch for the data being too small for the specified size,
               or for zero sized icons. */
            if (i + w*h > num || w == 0 || h == 0) {
                i += w*h;
                continue;
            }
            argb = g_new(guint32, w*h);
            memcpy(argb, &data[i], w*h * sizeof(guint32));
            add_icon(info, w, h, argb);
            i += w*h;
        }
        g_free(data);
    }

    /* if we didn't find an image from the NET_WM_ICON stuff, then try the
       legacy X hints */
    if (info->n_icons == 0 && (hints = XGetWMHints(obt_display, win))) {
        if (hints->flags & IconPixmapHint) {
            gboolean xicon;
            obt_display_ignore_errors(TRUE);
            xicon = x11_pixmap_argb(hints->icon_pixmap,
                                    (hints->flags & IconMaskHint ?
                                     hints->icon_mask : None),
                                    (gint*)&w, (gint*)&h, &data);
            obt_display_ignore_errors(FALSE);

            if (xicon) {
                if (w > 0 && h > 0) {
                    add_icon(info, w, h, data);
                } else
                    g_free(data);
            }
        }
        XFree(hints);
    }
}

static void get_icon_area(Window w, ObSurfaceInfo *info)
{
    guint num;
    guint32 *data;

    RECT_SET(info->icon_area, 0, 0, 0, 0);

    if (OBT_PROP_GETA32(w, NET_WM_ICON_GEOMETRY, CARDINAL, &data, &num)) {
        if (num == 4)
            /* don't let them set it with an area < 0 */
            RECT_SET(info->icon_area, data[0], data[1],
                     MAX(data[2],0), MAX(data[3],0));
        g_free(data);
    }
}

static void get_states(Window w, ObSurfaceInfo *info)
{
    static const struct { int atom; ObSurfaceStates state; } states[] = {
        { OBT_PROP_NET_WM_STATE_MODAL, OB_SURFACE_STATE_MODAL },
        { OBT_PROP_NET_WM_STATE_SHADED, OB_SURFACE_STATE_SHADED },
        { OBT_PROP_NET_WM_STATE_HIDDEN, OB_SURFACE_STATE_ICONIC },
        { OBT_PROP_NET_WM_STATE_SKIP_TASKBAR, OB_SURFACE_STATE_SKIP_TASKBAR },
        { OBT_PROP_NET_WM_STATE_SKIP_PAGER, OB_SURFACE_STATE_SKIP_PAGER },
        { OBT_PROP_NET_WM_STATE_FULLSCREEN, OB_SURFACE_STATE_FULLSCREEN },
        { OBT_PROP_NET_WM_STATE_MAXIMIZED_VERT,
          OB_SURFACE_STATE_MAXIMIZED_VERT },
        { OBT_PROP_NET_WM_STATE_MAXIMIZED_HORZ,
          OB_SURFACE_STATE_MAXIMIZED_HORZ },
        { OBT_PROP_NET_WM_STATE_ABOVE, OB_SURFACE_STATE_ABOVE },
        { OBT_PROP_NET_WM_STATE_BELOW, OB_SURFACE_STATE_BELOW },
        { OBT_PROP_NET_WM_STATE_DEMANDS_ATTENTION, OB_SURFACE_STATE_URGENT },
        { OBT_PROP_OB_WM_STATE_UNDECORATED, OB_SURFACE_STATE_UNDECORATED },
    };
    guint32 *state;
    guint num, i, j;

    info->states = 0;
    if (!OBT_PROP_GETA32(w, NET_WM_STATE, ATOM, &state, &num))
        return;
    for (i = 0; i < num; ++i)
        for (j = 0; j < G_N_ELEMENTS(states); ++j)
            if (state[i] == obt_prop_atom(states[j].atom)) {
                info->states |= states[j].state;
                break;
            }
    g_free(state);
}

static void x11_surface_info(ObSurfaceId s, ObSurfaceInfoFields fields,
                             ObSurfaceInfo *info)
{
    if (fields & OB_SURFACE_INFO_TITLE)
        get_titles((Window)s, info);
    if (fields & OB_SURFACE_INFO_CLASS)
        get_ids((Window)s, info);
    if (fields & OB_SURFACE_INFO_PROTOCOLS)
        get_protocols((Window)s, info);
    if (fields & OB_SURFACE_INFO_STRUT)
        get_strut((Window)s, info);
    if (fields & OB_SURFACE_INFO_SIZE)
        get_size_hints((Window)s, info);
    if (fields & OB_SURFACE_INFO_HINTS)
        get_hints((Window)s, info);
    if (fields & OB_SURFACE_INFO_TYPE)
        get_type((Window)s, info);
    if (fields & OB_SURFACE_INFO_DECOR)
        get_decor_hints((Window)s, info);
    if (fields & OB_SURFACE_INFO_PARENT)
        get_parent((Window)s, info);
    if (fields & OB_SURFACE_INFO_ICON)
        get_icons((Window)s, info);
    if (fields & OB_SURFACE_INFO_ICON_AREA)
        get_icon_area((Window)s, info);
    if (fields & OB_SURFACE_INFO_STARTUP) {
        g_free(info->startup_id);
        if (!OBT_PROP_GETS_UTF8((Window)s, NET_STARTUP_ID, &info->startup_id))
            info->startup_id = NULL;
    }
    if (fields & OB_SURFACE_INFO_DESKTOP) {
        guint32 d;
        info->has_desktop =
            OBT_PROP_GET32((Window)s, NET_WM_DESKTOP, CARDINAL, &d);
        if (info->has_desktop)
            info->desktop = d;
    }
    if (fields & OB_SURFACE_INFO_STATES)
        get_states((Window)s, info);
    if (fields & OB_SURFACE_INFO_USER_TIME) {
        guint32 t;
        info->has_user_time =
            OBT_PROP_GET32((Window)s, NET_WM_USER_TIME, CARDINAL, &t);
        if (info->has_user_time)
            info->user_time = t;
    }
    if (fields & OB_SURFACE_INFO_SYNC) {
        guint32 c;
        info->sync_counter = 0;
#ifdef SYNC
        if (OBT_PROP_GET32((Window)s, NET_WM_SYNC_REQUEST_COUNTER, CARDINAL,
                           &c))
            info->sync_counter = c;
#endif
    }
    if (fields & OB_SURFACE_INFO_OPACITY) {
        guint32 o;
        info->has_opacity =
            OBT_PROP_GET32((Window)s, NET_WM_WINDOW_OPACITY, CARDINAL, &o);
        if (info->has_opacity)
            info->opacity = o;
    }
    if (fields & OB_SURFACE_INFO_GEOMETRY)
        x11_get_geometry(s, info);
    if (fields & OB_SURFACE_INFO_SHAPE)
        x11_get_shape(s, info);
}

/* --- events --- */

struct skip_property {
    Window window;
    Atom prop;
};

static gboolean is_title(Atom a)
{
    return a == OBT_PROP_ATOM(NET_WM_NAME) ||
        a == OBT_PROP_ATOM(WM_NAME) ||
        a == OBT_PROP_ATOM(NET_WM_ICON_NAME) ||
        a == OBT_PROP_ATOM(WM_ICON_NAME);
}

/* another change to the same thing is already queued */
static gboolean skip_property_change(XEvent *e, gpointer data)
{
    const struct skip_property s = *(struct skip_property*)data;

    if (e->type == PropertyNotify && e->xproperty.window == s.window) {
        const Atom a = e->xproperty.atom;
        const Atom b = s.prop;

        /* these are all updated together */
        if (is_title(a) && is_title(b))
            return TRUE;
        else if (a == b && a == OBT_PROP_ATOM(NET_WM_ICON))
            return TRUE;
    }
    return FALSE;
}

static ObSurfaceInfoFields property_fields(Atom a)
{
    if (a == XA_WM_NORMAL_HINTS) return OB_SURFACE_INFO_SIZE;
    if (a == OBT_PROP_ATOM(MOTIF_WM_HINTS)) return OB_SURFACE_INFO_DECOR;
    if (a == XA_WM_HINTS) return OB_SURFACE_INFO_HINTS;
    if (a == XA_WM_TRANSIENT_FOR) return OB_SURFACE_INFO_PARENT;
    if (is_title(a)) return OB_SURFACE_INFO_TITLE;
    if (a == OBT_PROP_ATOM(WM_PROTOCOLS)) return OB_SURFACE_INFO_PROTOCOLS;
    if (a == OBT_PROP_ATOM(NET_WM_STRUT) ||
        a == OBT_PROP_ATOM(NET_WM_STRUT_PARTIAL))
        return OB_SURFACE_INFO_STRUT;
    if (a == OBT_PROP_ATOM(NET_WM_ICON)) return OB_SURFACE_INFO_ICON;
    if (a == OBT_PROP_ATOM(NET_WM_ICON_GEOMETRY))
        return OB_SURFACE_INFO_ICON_AREA;
    if (a == OBT_PROP_ATOM(NET_WM_USER_TIME)) return OB_SURFACE_INFO_USER_TIME;
#ifdef SYNC
    if (a == OBT_PROP_ATOM(NET_WM_SYNC_REQUEST_COUNTER))
        return OB_SURFACE_INFO_SYNC;
#endif
    if (a == OBT_PROP_ATOM(NET_WM_WINDOW_OPACITY))
        return OB_SURFACE_INFO_OPACITY;
    return 0;
}

gboolean x11_property_notify(const XPropertyEvent *e)
{
    ObSurfaceInfoFields f = property_fields(e->atom);
    struct skip_property s;

    if (!f) return FALSE;

    /* ignore changes to some properties if there is another change
       coming in the queue */
    s.window = e->window;
    s.prop = e->atom;
    if (!xqueue_exists_local(skip_property_change, &s))
        x11_events->surface_changed((ObSurfaceId)e->window, f, e->time);
    return TRUE;
}

static ObSurfaceStates atom_state(Atom a)
{
    if (a == OBT_PROP_ATOM(NET_WM_STATE_MODAL))
        return OB_SURFACE_STATE_MODAL;
    if (a == OBT_PROP_ATOM(NET_WM_STATE_MAXIMIZED_VERT))
        return OB_SURFACE_STATE_MAXIMIZED_VERT;
    if (a == OBT_PROP_ATOM(NET_WM_STATE_MAXIMIZED_HORZ))
        return OB_SURFACE_STATE_MAXIMIZED_HORZ;
    if (a == OBT_PROP_ATOM(NET_WM_STATE_SHADED))
        return OB_SURFACE_STATE_SHADED;
    if (a == OBT_PROP_ATOM(NET_WM_STATE_SKIP_TASKBAR))
        return OB_SURFACE_STATE_SKIP_TASKBAR;
    if (a == OBT_PROP_ATOM(NET_WM_STATE_SKIP_PAGER))
        return OB_SURFACE_STATE_SKIP_PAGER;
    if (a == OBT_PROP_ATOM(NET_WM_STATE_HIDDEN))
        return OB_SURFACE_STATE_ICONIC;
    if (a == OBT_PROP_ATOM(NET_WM_STATE_FULLSCREEN))
        return OB_SURFACE_STATE_FULLSCREEN;
    if (a == OBT_PROP_ATOM(NET_WM_STATE_ABOVE))
        return OB_SURFACE_STATE_ABOVE;
    if (a == OBT_PROP_ATOM(NET_WM_STATE_BELOW))
        return OB_SURFACE_STATE_BELOW;
    if (a == OBT_PROP_ATOM(NET_WM_STATE_DEMANDS_ATTENTION))
        return OB_SURFACE_STATE_URGENT;
    if (a == OBT_PROP_ATOM(OB_WM_STATE_UNDECORATED))
        return OB_SURFACE_STATE_UNDECORATED;
    return 0;
}

static ObRequestSource request_source(glong l)
{
    return l == 1 ? OB_REQUEST_FROM_APPLICATION :
        l == 2 ? OB_REQUEST_FROM_PAGER : OB_REQUEST_FROM_UNKNOWN;
}

/* another message of the same kind for the same window is already queued */
static gboolean more_client_message_event(Window window, Atom msgtype)
{
    ObtXQueueWindowMessage wm;
    wm.window = window;
    wm.message = msgtype;
    return xqueue_exists_local(xqueue_match_window_message, &wm);
}

gboolean x11_client_message(const XClientMessageEvent *e)
{
    const ObSurfaceId s = (ObSurfaceId)e->window;
    const Atom msgtype = e->message_type;

    if (msgtype == OBT_PROP_ATOM(WM_CHANGE_STATE)) {
        if (!more_client_message_event(e->window, msgtype)) {
            if (e->data.l[0] == IconicState)
                x11_events->request_iconify(s, TRUE);
            else if (e->data.l[0] == NormalState)
                x11_events->request_iconify(s, FALSE);
        }
    } else if (msgtype == OBT_PROP_ATOM(NET_WM_DESKTOP)) {
        if (!more_client_message_event(e->window, msgtype))
            x11_events->request_surface_desktop(s, (guint)e->data.l[0]);
    } else if (msgtype == OBT_PROP_ATOM(NET_WM_STATE)) {
        const Atom action = e->data.l[0];
        ObStateAction a;

        /* can't compress these */
        if (action == OBT_PROP_ATOM(NET_WM_STATE_ADD))
            a = OB_STATE_ACTION_ADD;
        else if (action == OBT_PROP_ATOM(NET_WM_STATE_REMOVE))
            a = OB_STATE_ACTION_REMOVE;
        else if (action == OBT_PROP_ATOM(NET_WM_STATE_TOGGLE))
            a = OB_STATE_ACTION_TOGGLE;
        else
            /* an invalid action was passed to the client message, ignore
               it */
            return TRUE;
        x11_events->request_states(s, a, atom_state(e->data.l[1]),
                               atom_state(e->data.l[2]),
                               request_source(e->data.l[3]));
    } else if (msgtype == OBT_PROP_ATOM(NET_MOVERESIZE_WINDOW)) {
        ObConfigureRequest req;
        const glong flags = e->data.l[0];

        req.fields = 0;
        if (flags & 0xff) {
            req.fields |= OB_CONFIGURE_GRAVITY;
            req.gravity = flags & 0xff;
        }
        if (flags & 1 << 8) req.fields |= OB_CONFIGURE_X;
        if (flags & 1 << 9) req.fields |= OB_CONFIGURE_Y;
        if (flags & 1 << 10) req.fields |= OB_CONFIGURE_WIDTH;
        if (flags & 1 << 11) req.fields |= OB_CONFIGURE_HEIGHT;
        RECT_SET(req.want, e->data.l[1], e->data.l[2],
                 e->data.l[3], e->data.l[4]);
        x11_events->request_geometry(s, &req, request_source(flags >> 12 & 0xf));
    } else if (msgtype == OBT_PROP_ATOM(NET_RESTACK_WINDOW)) {
        const glong detail = e->data.l[2];
        ObStackMode mode;

        if (detail == Above) mode = OB_STACK_ABOVE;
        else if (detail == Below) mode = OB_STACK_BELOW;
        else if (detail == TopIf) mode = OB_STACK_TOP_IF;
        else if (detail == BottomIf) mode = OB_STACK_BOTTOM_IF;
        else if (detail == Opposite) mode = OB_STACK_OPPOSITE;
        else {
            ob_debug_type(OB_DEBUG_APP_BUGS,
                          "_NET_RESTACK_WINDOW sent for window 0x%lx "
                          "with invalid detail %ld", e->window, detail);
            return TRUE;
        }
        x11_events->request_restack(s, (guintptr)e->data.l[1], mode,
                                request_source(e->data.l[0]));
    } else if (msgtype == OBT_PROP_ATOM(NET_CLOSE_WINDOW)) {
        x11_events->request_close(s, request_source(e->data.l[1]));
    } else if (msgtype == OBT_PROP_ATOM(NET_ACTIVE_WINDOW)) {
        const ObRequestSource from = request_source(e->data.l[0]);
        x11_events->request_activate(s, from,
                                 from == OB_REQUEST_FROM_UNKNOWN ?
                                 0 : (guint32)e->data.l[1]);
    } else if (msgtype == OBT_PROP_ATOM(NET_WM_MOVERESIZE)) {
        const glong dir = e->data.l[2];

        ob_debug("net_wm_moveresize for 0x%lx direction %ld",
                 e->window, dir);
        /* ObMoveResize has _NET_WM_MOVERESIZE's order */
        if (dir >= OB_MOVERESIZE_SIZE_TOPLEFT && dir <= OB_MOVERESIZE_CANCEL)
            x11_events->request_move_resize(s, (ObMoveResize)dir, e->data.l[0],
                                        e->data.l[1], e->data.l[3]);
    } else
        return FALSE;
    return TRUE;
}

void x11_configure_request(const XConfigureRequestEvent *e)
{
    ObConfigureRequest req;

    req.fields = 0;
    if (e->value_mask & CWX) req.fields |= OB_CONFIGURE_X;
    if (e->value_mask & CWY) req.fields |= OB_CONFIGURE_Y;
    if (e->value_mask & CWWidth) req.fields |= OB_CONFIGURE_WIDTH;
    if (e->value_mask & CWHeight) req.fields |= OB_CONFIGURE_HEIGHT;
    if (e->value_mask & CWBorderWidth) req.fields |= OB_CONFIGURE_BORDER;
    if (e->value_mask & CWSibling) req.fields |= OB_CONFIGURE_SIBLING;
    if (e->value_mask & CWStackMode) req.fields |= OB_CONFIGURE_STACK;
    RECT_SET(req.want, e->x, e->y, e->width, e->height);
    req.border = e->border_width;
    req.sibling = (guintptr)e->above;
    switch (e->detail) {
    case Below: req.stack = OB_STACK_BELOW; break;
    case TopIf: req.stack = OB_STACK_TOP_IF; break;
    case BottomIf: req.stack = OB_STACK_BOTTOM_IF; break;
    case Opposite: req.stack = OB_STACK_OPPOSITE; break;
    default: req.stack = OB_STACK_ABOVE; break;
    }
    req.gravity = 0;

    x11_events->request_configure((ObSurfaceId)e->window, &req);
}

/* compositors read it off the frame */
static void x11_canvas_set_opacity(ObCanvasId c, gboolean set,
                                   guint32 opacity)
{
    if (set)
        OBT_PROP_SET32((Window)c, NET_WM_WINDOW_OPACITY, CARDINAL, opacity);
    else
        OBT_PROP_ERASE((Window)c, NET_WM_WINDOW_OPACITY);
}

static guint x11_modkey_mask(ObModkey key)
{
    static const ObtModkeysKey keys[] = {
        [OB_MODKEY_SHIFT] = OBT_KEYBOARD_MODKEY_SHIFT,
        [OB_MODKEY_CONTROL] = OBT_KEYBOARD_MODKEY_CONTROL,
        [OB_MODKEY_ALT] = OBT_KEYBOARD_MODKEY_ALT,
        [OB_MODKEY_META] = OBT_KEYBOARD_MODKEY_META,
        [OB_MODKEY_SUPER] = OBT_KEYBOARD_MODKEY_SUPER,
        [OB_MODKEY_HYPER] = OBT_KEYBOARD_MODKEY_HYPER
    };
    return obt_keyboard_modkey_to_modmask(keys[key]);
}

static gboolean x11_key_named(const gchar *name, guint *keycode)
{
    KeySym sym = XStringToKeysym(name);

    if (sym == NoSymbol) return FALSE;
    *keycode = XKeysymToKeycode(obt_display, sym);
    return TRUE;
}

static void x11_flush(gboolean wait)
{
    if (wait)
        XSync(obt_display, FALSE);
    else
        XFlush(obt_display);
}

static gulong x11_request_serial(void)
{
    return NextRequest(obt_display);
}

static void x11_request_serial_bump(void)
{
    OBT_PROP_ERASE(screen_support_win, MOTIF_WM_HINTS);
}

static void x11_restack(guintptr above, const guintptr *top_first, guint n)
{
    Window *win = g_new(Window, n + 1);
    guint i;

    for (i = 0; i < n; ++i) win[i + 1] = top_first[i];
    /* the top is just below the support window, which stays above every
       managed window */
    win[0] = above ? (Window)above : screen_support_win;
    XRestackWindows(obt_display, win, n + 1);
    g_free(win);
}

/* EWMH root properties, for pagers and taskbars */

static Window root(void)
{
    return obt_root(DefaultScreen(obt_display));
}

static void set_window_list(Atom prop, const ObSurfaceId *wins, guint n)
{
    gulong *v = n ? g_new(gulong, n) : NULL;
    guint i;

    for (i = 0; i < n; ++i) v[i] = wins[i];
    obt_prop_set_array32(root(), prop, OBT_PROP_ATOM(WINDOW), v, n);
    g_free(v);
}

static void x11_publish_clients(const ObSurfaceId *mapped, guint n)
{
    set_window_list(OBT_PROP_ATOM(NET_CLIENT_LIST), mapped, n);
}

static void x11_publish_stacking(const ObSurfaceId *stacked, guint n)
{
    set_window_list(OBT_PROP_ATOM(NET_CLIENT_LIST_STACKING), stacked, n);
}

static void x11_publish_active(ObSurfaceId s)
{
    OBT_PROP_SET32(root(), NET_ACTIVE_WINDOW, WINDOW, (Window)s);
}

static void x11_publish_desktop_count(guint count)
{
    /* no viewports, so every desktop's is at 0,0 */
    gulong *viewport = g_new0(gulong, count * 2);

    OBT_PROP_SET32(root(), NET_NUMBER_OF_DESKTOPS, CARDINAL, count);
    OBT_PROP_SETA32(root(), NET_DESKTOP_VIEWPORT, CARDINAL, viewport,
                    count * 2);
    g_free(viewport);
}

static void x11_publish_current_desktop(guint desktop)
{
    OBT_PROP_SET32(root(), NET_CURRENT_DESKTOP, CARDINAL, desktop);
}

static void x11_publish_desktop_names(const gchar *const *names)
{
    OBT_PROP_SETSS(root(), NET_DESKTOP_NAMES, names);
}

static void x11_publish_surface_desktop(ObSurfaceId s, guint desktop)
{
    OBT_PROP_SET32((Window)s, NET_WM_DESKTOP, CARDINAL, desktop);
}

static void x11_publish_surface_icon(ObSurfaceId s, const ObIcon *icon)
{
    gulong *ldata; /* use a long here to satisfy OBT_PROP_SETA32 */
    gint i, n = icon->width * icon->height;

    ldata = g_new(gulong, n + 2);
    ldata[0] = icon->width;
    ldata[1] = icon->height;
    for (i = 0; i < n; ++i)
        ldata[i+2] = icon->argb[i];
    OBT_PROP_SETA32((Window)s, NET_WM_ICON, CARDINAL, ldata, n + 2);
    g_free(ldata);
}

static void x11_publish_surface_titles(ObSurfaceId s, const gchar *title,
                                       const gchar *icon)
{
    if (title)
        OBT_PROP_SETS((Window)s, NET_WM_VISIBLE_NAME, title);
    if (icon)
        OBT_PROP_SETS((Window)s, NET_WM_VISIBLE_ICON_NAME, icon);
}

static void x11_publish_workareas(const Rect *areas, guint n)
{
    gulong *dims = g_new(gulong, 4 * n);
    guint i;

    for (i = 0; i < n; ++i) {
        dims[i*4+0] = areas[i].x;
        dims[i*4+1] = areas[i].y;
        dims[i*4+2] = areas[i].width;
        dims[i*4+3] = areas[i].height;
    }
    OBT_PROP_SETA32(root(), NET_WORKAREA, CARDINAL, dims, 4 * n);
    g_free(dims);
}

#define GRAB_PTR_MASK (ButtonPressMask | ButtonReleaseMask | PointerMotionMask)

/* the core's grabs go on the support window, which never maps */
static gboolean x11_grab_pointer(gboolean grab, gboolean owner_events,
                                 gboolean confine, ObCursor cursor,
                                 guint32 time)
{
    if (!grab) {
        XUngrabPointer(obt_display, time);
        return TRUE;
    }
    return XGrabPointer(obt_display, screen_support_win, owner_events,
                        GRAB_PTR_MASK, GrabModeAsync, GrabModeAsync,
                        confine ? root() : None, x11_cursor(cursor),
                        time) == Success;
}

static gboolean x11_grab_keyboard(gboolean grab, guint32 time)
{
    if (!grab) {
        XUngrabKeyboard(obt_display, time);
        return TRUE;
    }
    return XGrabKeyboard(obt_display, screen_support_win, False,
                         GrabModeAsync, GrabModeAsync, time) == Success;
}

/* Every combination of the lock keys, so a binding works with any of them
   on. The keyboard map can change, so look them up each time. */
#define N_LOCK_MASKS 8
static void lock_masks(guint list[N_LOCK_MASKS])
{
    const guint num =
        obt_keyboard_modkey_to_modmask(OBT_KEYBOARD_MODKEY_NUMLOCK);
    const guint caps =
        obt_keyboard_modkey_to_modmask(OBT_KEYBOARD_MODKEY_CAPSLOCK);
    const guint scroll =
        obt_keyboard_modkey_to_modmask(OBT_KEYBOARD_MODKEY_SCROLLLOCK);
    guint i;

    for (i = 0; i < N_LOCK_MASKS; ++i)
        list[i] = (i & 1 ? num : 0) | (i & 2 ? caps : 0) |
            (i & 4 ? scroll : 0);
}

static void x11_bind_key(guint keycode, guint mods)
{
    guint locks[N_LOCK_MASKS], i;

    lock_masks(locks);
    /* can get BadAccess' from these */
    obt_display_ignore_errors(TRUE);
    for (i = 0; i < N_LOCK_MASKS; ++i)
        XGrabKey(obt_display, keycode, mods | locks[i], root(), FALSE,
                 GrabModeAsync, GrabModeAsync);
    obt_display_ignore_errors(FALSE);
    if (obt_display_error_occured)
        ob_debug("Failed to grab keycode %d modifiers %d", keycode, mods);
}

static void x11_unbind_all_keys(void)
{
    XUngrabKey(obt_display, AnyKey, AnyModifier, root());
}

static void x11_bind_button(guintptr over, guint button, guint mods,
                            gboolean hold, ObCursor cursor)
{
    /* a held press is all a sync grab can catch; event.c makes up the
       release */
    const guint mask = hold ? ButtonPressMask :
        ButtonPressMask | ButtonMotionMask | ButtonReleaseMask;
    guint locks[N_LOCK_MASKS], i;

    lock_masks(locks);
    /* can get BadAccess from these */
    obt_display_ignore_errors(TRUE);
    for (i = 0; i < N_LOCK_MASKS; ++i)
        XGrabButton(obt_display, button, mods | locks[i], (Window)over,
                    False, mask, hold ? GrabModeSync : GrabModeAsync,
                    GrabModeAsync, None, x11_cursor(cursor));
    obt_display_ignore_errors(FALSE);
    if (obt_display_error_occured)
        ob_debug("Failed to grab button %d modifiers %d", button, mods);
}

static void x11_unbind_button(guintptr over, guint button, guint mods)
{
    guint locks[N_LOCK_MASKS], i;

    lock_masks(locks);
    for (i = 0; i < N_LOCK_MASKS; ++i)
        XUngrabButton(obt_display, button, mods | locks[i], (Window)over);
}

static void x11_pass_click(guint32 time)
{
    XAllowEvents(obt_display, ReplayPointer, time);
}

static gboolean x11_pointer_position(gint *x, gint *y)
{
    /* we don't care about any of these return values, but we can't pass
       NULL */
    Window w;
    guint u;
    gint i, j;
    gboolean ret;

    ret = !!XQueryPointer(obt_display, root(), &w, &w, x, y, &j, &j, &u);
    if (!ret) {
        for (i = 0; i < ScreenCount(obt_display); ++i)
            if (obt_root(i) != root())
                if ((ret = XQueryPointer(obt_display, obt_root(i),
                                         &w, &w, x, y, &j, &j, &u)))
                    break;
    }
    return ret;
}

static void x11_pointer_warp(gint x, gint y, gboolean swallow)
{
    XWarpPointer(obt_display, None, root(), 0, 0, 0, 0, x, y);
    if (swallow) {
        XEvent ce;

        XSync(obt_display, FALSE);
        while (xqueue_remove_local(&ce, xqueue_match_type,
                                   GINT_TO_POINTER(MotionNotify)));
    }
}

#ifdef SYNC
/* the resize being synced: its window, counter, alarm and the value it
   was last asked to reach */
static Window sync_window = None;
static XSyncCounter sync_counter = None;
static XSyncAlarm sync_alarm = None;
static gulong sync_value;
#endif

static gboolean x11_sync_start(ObSurfaceId s, guint32 counter)
{
#ifdef SYNC
    XSyncValue val;
    XSyncAlarmAttributes aa;

    if (!obt_display_extension_sync || !counter) return FALSE;
    if (sync_alarm != None) XSyncDestroyAlarm(obt_display, sync_alarm);

    sync_window = (Window)s;
    sync_counter = counter;

    /* set the counter to an initial value */
    XSyncIntToValue(&val, 0);
    XSyncSetCounter(obt_display, sync_counter, val);

    /* this will be incremented when we tell the client what we're
       looking for */
    sync_value = 0;

    /* the next sequence we're waiting for with the alarm */
    XSyncIntToValue(&val, 1);

    /* set an alarm on the counter */
    aa.trigger.counter = sync_counter;
    aa.trigger.wait_value = val;
    aa.trigger.value_type = XSyncAbsolute;
    aa.trigger.test_type = XSyncPositiveTransition;
    aa.events = True;
    XSyncIntToValue(&aa.delta, 1);
    sync_alarm = XSyncCreateAlarm(obt_display,
                                  XSyncCACounter |
                                  XSyncCAValue |
                                  XSyncCAValueType |
                                  XSyncCATestType |
                                  XSyncCADelta |
                                  XSyncCAEvents,
                                  &aa);
    return sync_alarm != None;
#else
    (void)s; (void)counter;
    return FALSE;
#endif
}

static void x11_sync_request(ObSurfaceId s, guint32 time)
{
#ifdef SYNC
    XEvent ce;
    XSyncValue val;

    if ((Window)s != sync_window || sync_alarm == None) return;

    /* increment the value we're waiting for */
    ++sync_value;
    XSyncIntToValue(&val, sync_value);

    /* tell the client what we're waiting for */
    ce.xclient.type = ClientMessage;
    ce.xclient.message_type = OBT_PROP_ATOM(WM_PROTOCOLS);
    ce.xclient.display = obt_display;
    ce.xclient.window = sync_window;
    ce.xclient.format = 32;
    ce.xclient.data.l[0] = OBT_PROP_ATOM(NET_WM_SYNC_REQUEST);
    ce.xclient.data.l[1] = time;
    ce.xclient.data.l[2] = XSyncValueLow32(val);
    ce.xclient.data.l[3] = XSyncValueHigh32(val);
    ce.xclient.data.l[4] = 0l;
    XSendEvent(obt_display, sync_window, FALSE, NoEventMask, &ce);
#else
    (void)s; (void)time;
#endif
}

static void x11_sync_stop(void)
{
#ifdef SYNC
    /* turn off the alarm */
    if (sync_alarm != None) {
        XSyncDestroyAlarm(obt_display, sync_alarm);
        sync_alarm = None;
    }
    sync_window = None;
    sync_counter = None;
#endif
}

void x11_sync_alarm(const XEvent *e)
{
#ifdef SYNC
    const XSyncAlarmNotifyEvent *se = (const XSyncAlarmNotifyEvent*)e;

    if (sync_alarm != None && se->alarm == sync_alarm)
        x11_events->surface_synced((ObSurfaceId)sync_window);
#else
    (void)e;
#endif
}

static void x11_grab_server(gboolean grab)
{
    if (grab) {
        XGrabServer(obt_display);
        XSync(obt_display, FALSE);
    } else {
        XUngrabServer(obt_display);
        XFlush(obt_display);
    }
}

const ObBackend ob_backend_x11 = {
    .name = "x11",
    .caps = OB_BACKEND_CAP_CLIENT_POSITION |
            OB_BACKEND_CAP_DECOR_HINTS |
            OB_BACKEND_CAP_ROOT_STATE |
            OB_BACKEND_CAP_DOCKAPPS |
            OB_BACKEND_CAP_SYNC_RESIZE |
            OB_BACKEND_CAP_REAP_CHILDREN |
            OB_BACKEND_CAP_RESTART,

    .startup = x11_startup,
    .shutdown = x11_shutdown,
    .event_source = x11_event_source,
    .time = x11_time,
    .request_running = x11_request_running,
    .previous_state = x11_previous_state,
    .desktop_names = x11_desktop_names,
    .desktop_layout = x11_desktop_layout,
    .claim_screen = x11_claim_screen,
    .release_screen = x11_release_screen,

    .output_count = x11_output_count,
    .output_area = x11_output_area,
    .layout_size = x11_layout_size,
    .reserved_edges = x11_reserved_edges,

    .existing_surfaces = x11_existing_surfaces,
    .surface_kind = x11_surface_kind,
    .dockapp_adopt = x11_dockapp_adopt,
    .dockapp_release = x11_dockapp_release,
    .surface_adopt = x11_surface_adopt,
    .surface_unwatch = x11_surface_unwatch,
    .surface_frame = x11_surface_frame,
    .surface_unframe = x11_surface_unframe,
    .surface_release = x11_surface_release,
    .surface_alive = x11_surface_alive,
    .surface_set_border = x11_surface_set_border,
    .surface_configure = x11_surface_configure,
    .surface_send_geometry = x11_surface_send_geometry,
    .surface_show = x11_surface_show,
    .surface_install_colors = x11_surface_install_colors,
    .surface_focus = x11_surface_focus,
    .surface_close = x11_surface_close,
    .surface_kill = x11_surface_kill,
    .surface_set_states = x11_surface_set_states,
    .surface_ping = x11_surface_ping,
    .sync_start = x11_sync_start,
    .sync_request = x11_sync_request,
    .sync_stop = x11_sync_stop,
    .surface_info = x11_surface_info,

    .restack = x11_restack,
    .flush = x11_flush,
    .set_cursor = x11_set_cursor,
    .request_serial = x11_request_serial,
    .request_serial_bump = x11_request_serial_bump,

    .canvas_new = x11_canvas_new,
    .canvas_new_child = x11_canvas_new_child,
    .canvas_has_alpha = x11_canvas_has_alpha,
    .canvas_free = x11_canvas_free,
    .canvas_place = x11_canvas_place,
    .canvas_move = x11_canvas_move,
    .canvas_resize = x11_canvas_resize,
    .canvas_show = x11_canvas_show,
    .canvas_set_shape = x11_canvas_set_shape,
    .canvas_set_opacity = x11_canvas_set_opacity,

    .font_settings = x11_font_settings,
    .screen_read = x11_screen_read,

    .dialog_set_title = x11_dialog_set_title,
    .dialog_set_size = x11_dialog_set_size,
    .dialog_set_parent = x11_dialog_set_parent,
    .canvas_present = x11_canvas_present,
    .canvas_fill = x11_canvas_fill,
    .canvas_set_border = x11_canvas_set_border,
    .canvas_set_outline = x11_canvas_set_outline,
    .canvas_set_input = x11_canvas_set_input,

    .grab_pointer = x11_grab_pointer,
    .grab_keyboard = x11_grab_keyboard,
    .grab_server = x11_grab_server,
    .modkey_mask = x11_modkey_mask,
    .only_modmasks = obt_keyboard_only_modmasks,
    .keymap_reload = obt_keyboard_reload,
    .key_named = x11_key_named,
    .bind_key = x11_bind_key,
    .unbind_all_keys = x11_unbind_all_keys,
    .bind_button = x11_bind_button,
    .unbind_button = x11_unbind_button,
    .pass_click = x11_pass_click,
    .pointer_position = x11_pointer_position,
    .pointer_warp = x11_pointer_warp,

    .publish_clients = x11_publish_clients,
    .publish_stacking = x11_publish_stacking,
    .publish_active = x11_publish_active,
    .publish_desktop_count = x11_publish_desktop_count,
    .publish_current_desktop = x11_publish_current_desktop,
    .publish_desktop_names = x11_publish_desktop_names,
    .publish_surface_desktop = x11_publish_surface_desktop,
    .publish_surface_icon = x11_publish_surface_icon,
    .publish_surface_titles = x11_publish_surface_titles,
    .publish_workareas = x11_publish_workareas,
    .publish_surface_extents = x11_publish_surface_extents,
    .publish_layout_size = x11_publish_layout_size,
    .publish_last_desktop = x11_publish_last_desktop,
    .publish_showing_desktop = x11_publish_showing_desktop,
    .publish_config_file = x11_publish_config_file,
    .publish_theme = x11_publish_theme,
    .publish_surface_actions = x11_publish_surface_actions,
    .publish_surface_app = x11_publish_surface_app,
    .publish_surface_hidden = x11_publish_surface_hidden,
};

void x11_colormap_notify(const XColormapEvent *e)
{
    x11_colormap_changed(e->window, e->colormap);
}

ObtIC *x11_grab_input_context(void)
{
    /* grabs go to the support window */
    if (!grab_ic)
        grab_ic = obt_keyboard_context_new(root(), screen_support_win);
    return grab_ic;
}

gboolean x11_shape_notify(const XEvent *e)
{
#ifdef SHAPE
    if (obt_display_extension_shape &&
        e->type == obt_display_extension_shape_basep)
    {
        const XShapeEvent *se = (const XShapeEvent*)e;

        x11_events->surface_changed((ObSurfaceId)se->window,
                                OB_SURFACE_INFO_SHAPE, se->time);
        return TRUE;
    }
#endif
    return FALSE;
}

void x11_map_event(const XEvent *e)
{
    switch (e->type) {
    case MapRequest:
        ob_debug("MapRequest for 0x%lx", e->xmaprequest.window);
        x11_events->surface_new((ObSurfaceId)e->xmaprequest.window);
        break;
    case UnmapNotify:
        ob_debug("UnmapNotify for window 0x%lx eventwin 0x%lx "
                 "fromconfigure %d", e->xunmap.window, e->xunmap.event,
                 e->xunmap.from_configure);
        x11_events->surface_withdrawn((ObSurfaceId)e->xunmap.window);
        break;
    case DestroyNotify:
        ob_debug("DestroyNotify for window 0x%lx", e->xdestroywindow.window);
        x11_events->surface_gone((ObSurfaceId)e->xdestroywindow.window);
        break;
    case ReparentNotify:
        ob_debug("ReparentNotify for window 0x%lx", e->xreparent.window);
        x11_events->surface_gone((ObSurfaceId)e->xreparent.window);
        break;
    }
}
