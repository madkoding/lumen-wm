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
#include "backend-x11.h"
#include "openbox.h"
#include "debug.h"
#include "screen.h"
#include "obrender/render.h"
#include "obt/display.h"
#include "obt/xqueue.h"
#include "obt/prop.h"
#include "obt/keyboard.h"

#include <X11/Xatom.h>
#include <string.h>

static const ObBackendEvents *events;

static Window root(void);

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
    guint w, h, i, j;
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
            xicon = RrPixmapToRGBA(ob_rr_inst,
                                   hints->icon_pixmap,
                                   (hints->flags & IconMaskHint ?
                                    hints->icon_mask : None),
                                   (gint*)&w, (gint*)&h, &data);
            obt_display_ignore_errors(FALSE);

            if (xicon) {
                if (w > 0 && h > 0) {
                    /* from ObRender's bit order */
                    for (j = 0; j < w*h; ++j)
                        data[j] =
                            (((data[j] >> RrDefaultAlphaOffset) & 0xff) << 24) +
                            (((data[j] >> RrDefaultRedOffset) & 0xff) << 16) +
                            (((data[j] >> RrDefaultGreenOffset) & 0xff) << 8) +
                            (((data[j] >> RrDefaultBlueOffset) & 0xff) << 0);
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
    return 0;
}

gboolean backend_x11_property_notify(const XPropertyEvent *e)
{
    ObSurfaceInfoFields f = property_fields(e->atom);
    struct skip_property s;

    if (!f) return FALSE;

    /* ignore changes to some properties if there is another change
       coming in the queue */
    s.window = e->window;
    s.prop = e->atom;
    if (!xqueue_exists_local(skip_property_change, &s))
        events->surface_changed((ObSurfaceId)e->window, f, e->time);
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

gboolean backend_x11_client_message(const XClientMessageEvent *e)
{
    const ObSurfaceId s = (ObSurfaceId)e->window;
    const Atom msgtype = e->message_type;

    if (msgtype == OBT_PROP_ATOM(WM_CHANGE_STATE)) {
        if (!more_client_message_event(e->window, msgtype)) {
            if (e->data.l[0] == IconicState)
                events->request_iconify(s, TRUE);
            else if (e->data.l[0] == NormalState)
                events->request_iconify(s, FALSE);
        }
    } else if (msgtype == OBT_PROP_ATOM(NET_WM_DESKTOP)) {
        if (!more_client_message_event(e->window, msgtype))
            events->request_surface_desktop(s, (guint)e->data.l[0]);
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
        events->request_states(s, a, atom_state(e->data.l[1]),
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
        events->request_geometry(s, &req, request_source(flags >> 12 & 0xf));
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
        events->request_restack(s, (guintptr)e->data.l[1], mode,
                                request_source(e->data.l[0]));
    } else if (msgtype == OBT_PROP_ATOM(NET_CLOSE_WINDOW)) {
        events->request_close(s, request_source(e->data.l[1]));
    } else if (msgtype == OBT_PROP_ATOM(NET_ACTIVE_WINDOW)) {
        const ObRequestSource from = request_source(e->data.l[0]);
        events->request_activate(s, from,
                                 from == OB_REQUEST_FROM_UNKNOWN ?
                                 0 : (guint32)e->data.l[1]);
    } else if (msgtype == OBT_PROP_ATOM(NET_WM_MOVERESIZE)) {
        const glong dir = e->data.l[2];

        ob_debug("net_wm_moveresize for 0x%lx direction %ld",
                 e->window, dir);
        /* ObMoveResize has _NET_WM_MOVERESIZE's order */
        if (dir >= OB_MOVERESIZE_SIZE_TOPLEFT && dir <= OB_MOVERESIZE_CANCEL)
            events->request_move_resize(s, (ObMoveResize)dir, e->data.l[0],
                                        e->data.l[1], e->data.l[3]);
    } else
        return FALSE;
    return TRUE;
}

void backend_x11_configure_request(const XConfigureRequestEvent *e)
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

    events->request_configure((ObSurfaceId)e->window, &req);
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
                        confine ? root() : None, ob_cursor(cursor),
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
                    GrabModeAsync, None, ob_cursor(cursor));
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
    .surface_info = x11_surface_info,

    .restack = x11_restack,

    .grab_pointer = x11_grab_pointer,
    .grab_keyboard = x11_grab_keyboard,
    .grab_server = x11_grab_server,
    .bind_key = x11_bind_key,
    .unbind_all_keys = x11_unbind_all_keys,
    .bind_button = x11_bind_button,
    .unbind_button = x11_unbind_button,
    .pass_click = x11_pass_click,

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
};

void backend_x11_map_event(const XEvent *e)
{
    switch (e->type) {
    case MapRequest:
        ob_debug("MapRequest for 0x%lx", e->xmaprequest.window);
        events->surface_new((ObSurfaceId)e->xmaprequest.window);
        break;
    case UnmapNotify:
        ob_debug("UnmapNotify for window 0x%lx eventwin 0x%lx "
                 "fromconfigure %d", e->xunmap.window, e->xunmap.event,
                 e->xunmap.from_configure);
        events->surface_withdrawn((ObSurfaceId)e->xunmap.window);
        break;
    case DestroyNotify:
        ob_debug("DestroyNotify for window 0x%lx", e->xdestroywindow.window);
        events->surface_gone((ObSurfaceId)e->xdestroywindow.window);
        break;
    case ReparentNotify:
        ob_debug("ReparentNotify for window 0x%lx", e->xreparent.window);
        events->surface_gone((ObSurfaceId)e->xreparent.window);
        break;
    }
}
