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
#include "obt/prop.h"

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

/* managed surfaces */

static gboolean x11_surface_focus(ObSurfaceId s, guint32 time,
                                  gboolean input, gboolean take_focus)
{
    obt_display_ignore_errors(TRUE);

    if (input)
        /* This can cause a BadMatch error with CurrentTime, or if an app
           passed in a bad time for _NET_WM_ACTIVE_WINDOW. */
        XSetInputFocus(obt_display, (Window)s, RevertToPointerRoot, time);

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

static void x11_restack(guintptr above, const guintptr *top_first, guint n)
{
    Window *win = g_new(Window, n + 1);
    guint i;

    for (i = 0; i < n; ++i) win[i + 1] = top_first[i];
    if (above) {
        win[0] = above;
        XRestackWindows(obt_display, win, n + 1);
    } else if (n) {
        XRaiseWindow(obt_display, win[1]);
        XRestackWindows(obt_display, win + 1, n);
    }
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

    .surface_focus = x11_surface_focus,
    .surface_close = x11_surface_close,
    .surface_kill = x11_surface_kill,
    .surface_set_states = x11_surface_set_states,
    .surface_ping = x11_surface_ping,

    .restack = x11_restack,

    .publish_clients = x11_publish_clients,
    .publish_stacking = x11_publish_stacking,
    .publish_active = x11_publish_active,
    .publish_desktop_count = x11_publish_desktop_count,
    .publish_current_desktop = x11_publish_current_desktop,
    .publish_desktop_names = x11_publish_desktop_names,
    .publish_surface_desktop = x11_publish_surface_desktop,
    .publish_workareas = x11_publish_workareas,
};
