/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   dock.c for the Openbox window manager
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

#include "debug.h"
#include "dock.h"
#include "screen.h"
#include "config.h"
#include "grab.h"
#include "backend.h"
#include "openbox.h"
#include "obrender/theme.h"
#include "paint.h"

#define DOCK_INPUT (OB_CANVAS_INPUT_BUTTONS | OB_CANVAS_INPUT_CROSSING)

static ObDock *dock;
static guint show_timeout_id;
static guint hide_timeout_id;

StrutPartial dock_strut;

static void dock_app_grab_button(ObDockApp *app, gboolean grab)
{
    if (grab) {
        ob_backend->bind_button(app->icon_win, config_dock_app_move_button,
                                config_dock_app_move_modifiers, FALSE,
                                OB_CURSOR_MOVE);
    } else {
        ob_backend->unbind_button(app->icon_win,
                                  config_dock_app_move_button,
                                  config_dock_app_move_modifiers);
    }
}

static guint window_hash(Window *w) { return *w; }
static gboolean window_comp(Window *w1, Window *w2) { return *w1 == *w2; }

void dock_startup(gboolean reconfig)
{
    if (reconfig) {
        GList *it;

        ob_backend->canvas_set_border(dock->frame, ob_rr_theme->obwidth,
            paint_argb(ob_rr_theme->osd_border_color));

        RrAppearanceFree(dock->a_frame);
        dock->a_frame = RrAppearanceCopy(ob_rr_theme->osd_bg);

        stacking_add(DOCK_AS_WINDOW(dock));

        dock_configure();
        dock_hide(TRUE);

        for (it = dock->dock_apps; it; it = g_list_next(it))
            dock_app_grab_button(it->data, TRUE);
        return;
    }

    STRUT_PARTIAL_SET(dock_strut, 0, 0, 0, 0,
                      0, 0, 0, 0, 0, 0, 0, 0);

    dock = g_slice_new0(ObDock);
    dock->obwin.type = OB_WINDOW_CLASS_DOCK;

    dock->hidden = TRUE;

    dock->dock_map = g_hash_table_new((GHashFunc)window_hash,
                                      (GEqualFunc)window_comp);

    /* clicks on the dock apps stay with them */
    dock->frame = (Window)ob_backend->canvas_new(OB_CANVAS_PANEL,
                                                 OB_SURFACE_NONE, DOCK_INPUT);
    dock->a_frame = RrAppearanceCopy(ob_rr_theme->osd_bg);
    ob_backend->canvas_set_border(dock->frame, ob_rr_theme->obwidth,
        paint_argb(ob_rr_theme->osd_border_color));

    window_add(&dock->frame, DOCK_AS_WINDOW(dock));
    stacking_add(DOCK_AS_WINDOW(dock));
}

void dock_shutdown(gboolean reconfig)
{
    if (reconfig) {
        GList *it;

        stacking_remove(DOCK_AS_WINDOW(dock));

        for (it = dock->dock_apps; it; it = g_list_next(it))
            dock_app_grab_button(it->data, FALSE);
        return;
    }

    g_hash_table_destroy(dock->dock_map);

    ob_backend->canvas_free(dock->frame);
    RrAppearanceFree(dock->a_frame);
    window_remove(dock->frame);
    stacking_remove(dock);
    g_slice_free(ObDock, dock);
    dock = NULL;
}

void dock_manage(Window icon_win, Window name_win)
{
    ObDockApp *app;
    ObSurfaceInfo info = { 0 };

    app = g_slice_new0(ObDockApp);
    app->name_win = name_win;
    app->icon_win = icon_win;

    ob_backend->surface_info(app->name_win, OB_SURFACE_INFO_CLASS, &info);
    app->name = g_strdup(info.instance ? info.instance : "");
    app->class = g_strdup(info.app_id ? info.app_id : "");

    ob_backend->surface_info(app->icon_win, OB_SURFACE_INFO_GEOMETRY, &info);
    if (info.has_area) {
        app->w = info.area.width;
        app->h = info.area.height;
    } else {
        app->w = app->h = 64;
    }
    backend_surface_info_clear(&info);

    dock->dock_apps = g_list_append(dock->dock_apps, app);
    g_hash_table_insert(dock->dock_map, &app->icon_win, app);
    dock_configure();

    ob_backend->dockapp_adopt(app->icon_win, app->name_win, dock->frame,
                              app->x, app->y);
    /*
      This is the same case as in frame.c for client windows. When Openbox is
      starting, the window is already mapped so we see unmap events occur for
      it. There are 2 unmap events generated that we see, one with the 'event'
      member set the root window, and one set to the client, but both get
      handled and need to be ignored.
    */
    if (ob_state() == OB_STATE_STARTING)
        app->ignore_unmaps += 2;

    dock_app_grab_button(app, TRUE);

    ob_debug("Managed Dock App: 0x%lx 0x%lx (%s)",
             app->icon_win, app->name_win, app->class);

    grab_server(FALSE);
}

void dock_unmanage_all(void)
{
    while (dock->dock_apps)
        dock_unmanage(dock->dock_apps->data, TRUE);
}

void dock_unmanage(ObDockApp *app, gboolean reparent)
{
    dock_app_grab_button(app, FALSE);
    ob_backend->dockapp_release(app->icon_win, app->name_win, reparent);

    dock->dock_apps = g_list_remove(dock->dock_apps, app);
    g_hash_table_remove(dock->dock_map, &app->icon_win);
    dock_configure();

    ob_debug("Unmanaged Dock App: 0x%lx (%s)", app->icon_win, app->class);

    g_free(app->name);
    g_free(app->class);
    g_slice_free(ObDockApp, app);
}

void dock_configure(void)
{
    GList *it;
    gint hspot, vspot;
    gint gravity;
    gint l, r, t, b;
    gint strw, strh;
    const Rect *a;
    gint hidesize;

    RrMargins(dock->a_frame, &l, &t, &r, &b);
    hidesize = MAX(1, ob_rr_theme->obwidth);

    dock->area.width = dock->area.height = 0;

    /* get the size */
    for (it = dock->dock_apps; it; it = g_list_next(it)) {
        ObDockApp *app = it->data;
        switch (config_dock_orient) {
        case OB_ORIENTATION_HORZ:
            dock->area.width += app->w;
            dock->area.height = MAX(dock->area.height, app->h);
            break;
        case OB_ORIENTATION_VERT:
            dock->area.width = MAX(dock->area.width, app->w);
            dock->area.height += app->h;
            break;
        }
    }

    if (dock->dock_apps) {
        dock->area.width += l + r;
        dock->area.height += t + b;
    }

    hspot = l;
    vspot = t;

    /* position the apps */
    for (it = dock->dock_apps; it; it = g_list_next(it)) {
        ObDockApp *app = it->data;
        switch (config_dock_orient) {
        case OB_ORIENTATION_HORZ:
            app->x = hspot;
            app->y = (dock->area.height - app->h) / 2;
            hspot += app->w;
            break;
        case OB_ORIENTATION_VERT:
            app->x = (dock->area.width - app->w) / 2;
            app->y = vspot;
            vspot += app->h;
            break;
        }

        {
            Rect r;

            RECT_SET(r, app->x, app->y, app->w, app->h);
            ob_backend->surface_configure(app->icon_win, &r);
        }
    }

    /* used for calculating offsets */
    dock->area.width += ob_rr_theme->obwidth * 2;
    dock->area.height += ob_rr_theme->obwidth * 2;

    a = screen_physical_area_all_monitors();

    /* calculate position */
    if (config_dock_floating) {
        dock->area.x = config_dock_x;
        dock->area.y = config_dock_y;
        gravity = NorthWestGravity;
    } else {
        switch (config_dock_pos) {
        case OB_DIRECTION_NORTHWEST:
            dock->area.x = 0;
            dock->area.y = 0;
            gravity = NorthWestGravity;
            break;
        case OB_DIRECTION_NORTH:
            dock->area.x = a->width / 2;
            dock->area.y = 0;
            gravity = NorthGravity;
            break;
        case OB_DIRECTION_NORTHEAST:
            dock->area.x = a->width;
            dock->area.y = 0;
            gravity = NorthEastGravity;
            break;
        case OB_DIRECTION_WEST:
            dock->area.x = 0;
            dock->area.y = a->height / 2;
            gravity = WestGravity;
            break;
        case OB_DIRECTION_EAST:
            dock->area.x = a->width;
            dock->area.y = a->height / 2;
            gravity = EastGravity;
            break;
        case OB_DIRECTION_SOUTHWEST:
            dock->area.x = 0;
            dock->area.y = a->height;
            gravity = SouthWestGravity;
            break;
        case OB_DIRECTION_SOUTH:
            dock->area.x = a->width / 2;
            dock->area.y = a->height;
            gravity = SouthGravity;
            break;
        case OB_DIRECTION_SOUTHEAST:
            dock->area.x = a->width;
            dock->area.y = a->height;
            gravity = SouthEastGravity;
            break;
        default:
            g_assert_not_reached();
        }
    }

    switch(gravity) {
    case NorthGravity:
    case CenterGravity:
    case SouthGravity:
        dock->area.x -= dock->area.width / 2;
        break;
    case NorthEastGravity:
    case EastGravity:
    case SouthEastGravity:
        dock->area.x -= dock->area.width;
        break;
    }
    switch(gravity) {
    case WestGravity:
    case CenterGravity:
    case EastGravity:
        dock->area.y -= dock->area.height / 2;
        break;
    case SouthWestGravity:
    case SouthGravity:
    case SouthEastGravity:
        dock->area.y -= dock->area.height;
        break;
    }

    if (config_dock_hide && dock->hidden) {
        if (!config_dock_floating) {
            switch (config_dock_pos) {
            case OB_DIRECTION_NORTHWEST:
                switch (config_dock_orient) {
                case OB_ORIENTATION_HORZ:
                    dock->area.y -= dock->area.height - hidesize;
                    break;
                case OB_ORIENTATION_VERT:
                    dock->area.x -= dock->area.width - hidesize;
                    break;
                }
                break;
            case OB_DIRECTION_NORTH:
                dock->area.y -= dock->area.height - hidesize;
                break;
            case OB_DIRECTION_NORTHEAST:
                switch (config_dock_orient) {
                case OB_ORIENTATION_HORZ:
                    dock->area.y -= dock->area.height - hidesize;
                    break;
                case OB_ORIENTATION_VERT:
                    dock->area.x += dock->area.width - hidesize;
                    break;
                }
                break;
            case OB_DIRECTION_WEST:
                dock->area.x -= dock->area.width - hidesize;
                break;
            case OB_DIRECTION_EAST:
                dock->area.x += dock->area.width - hidesize;
                break;
            case OB_DIRECTION_SOUTHWEST:
                switch (config_dock_orient) {
                case OB_ORIENTATION_HORZ:
                    dock->area.y += dock->area.height - hidesize;
                    break;
                case OB_ORIENTATION_VERT:
                    dock->area.x -= dock->area.width - hidesize;
                    break;
                } break;
            case OB_DIRECTION_SOUTH:
                dock->area.y += dock->area.height - hidesize;
                break;
            case OB_DIRECTION_SOUTHEAST:
                switch (config_dock_orient) {
                case OB_ORIENTATION_HORZ:
                    dock->area.y += dock->area.height - hidesize;
                    break;
                case OB_ORIENTATION_VERT:
                    dock->area.x += dock->area.width - hidesize;
                    break;
                }
                break;
            }
        }
    }

    if (!config_dock_floating && config_dock_hide) {
        strw = hidesize;
        strh = hidesize;
    } else {
        strw = dock->area.width;
        strh = dock->area.height;
    }

    /* set the strut */
    if (!dock->dock_apps) {
        STRUT_PARTIAL_SET(dock_strut, 0, 0, 0, 0,
                          0, 0, 0, 0, 0, 0, 0, 0);
    }
    else if (config_dock_floating || config_dock_nostrut) {
        STRUT_PARTIAL_SET(dock_strut, 0, 0, 0, 0,
                          0, 0, 0, 0, 0, 0, 0, 0);
    }
    else {
        switch (config_dock_pos) {
        case OB_DIRECTION_NORTHWEST:
            switch (config_dock_orient) {
            case OB_ORIENTATION_HORZ:
                STRUT_PARTIAL_SET(dock_strut, 0, strh, 0, 0,
                                  0, 0, dock->area.x, dock->area.x
                                  + dock->area.width - 1, 0, 0, 0, 0);
                break;
            case OB_ORIENTATION_VERT:
                STRUT_PARTIAL_SET(dock_strut, strw, 0, 0, 0,
                                  dock->area.y, dock->area.y
                                  + dock->area.height - 1, 0, 0, 0, 0, 0, 0);
                break;
            }
            break;
        case OB_DIRECTION_NORTH:
            STRUT_PARTIAL_SET(dock_strut, 0, strh, 0, 0,
                              0, 0, dock->area.x, dock->area.x
                              + dock->area.width - 1, 0, 0, 0, 0);
            break;
        case OB_DIRECTION_NORTHEAST:
            switch (config_dock_orient) {
            case OB_ORIENTATION_HORZ:
                STRUT_PARTIAL_SET(dock_strut, 0, strh, 0, 0,
                                  0, 0, dock->area.x, dock->area.x
                                  + dock->area.width -1, 0, 0, 0, 0);
                break;
            case OB_ORIENTATION_VERT:
                STRUT_PARTIAL_SET(dock_strut, 0, 0, strw, 0,
                                  0, 0, 0, 0, dock->area.y, dock->area.y
                                  + dock->area.height - 1, 0, 0);
                break;
            }
            break;
        case OB_DIRECTION_WEST:
            STRUT_PARTIAL_SET(dock_strut, strw, 0, 0, 0,
                              dock->area.y, dock->area.y
                              + dock->area.height - 1, 0, 0, 0, 0, 0, 0);
            break;
        case OB_DIRECTION_EAST:
            STRUT_PARTIAL_SET(dock_strut, 0, 0, strw, 0,
                              0, 0, 0, 0, dock->area.y, dock->area.y
                              + dock->area.height - 1, 0, 0);
            break;
        case OB_DIRECTION_SOUTHWEST:
            switch (config_dock_orient) {
            case OB_ORIENTATION_HORZ:
                STRUT_PARTIAL_SET(dock_strut, 0, 0, 0, strh,
                                  0, 0, 0, 0, 0, 0, dock->area.x, dock->area.x
                                  + dock->area.width - 1);
                break;
            case OB_ORIENTATION_VERT:
                STRUT_PARTIAL_SET(dock_strut, strw, 0, 0, 0,
                                  dock->area.y, dock->area.y
                                  + dock->area.height - 1, 0, 0, 0, 0, 0, 0);
                break;
            }
            break;
        case OB_DIRECTION_SOUTH:
            STRUT_PARTIAL_SET(dock_strut, 0, 0, 0, strh,
                              0, 0, 0, 0, 0, 0, dock->area.x, dock->area.x
                              + dock->area.width - 1);
            break;
        case OB_DIRECTION_SOUTHEAST:
            switch (config_dock_orient) {
            case OB_ORIENTATION_HORZ:
                STRUT_PARTIAL_SET(dock_strut, 0, 0, 0, strh,
                                  0, 0, 0, 0, 0, 0, dock->area.x,
                                  dock->area.x + dock->area.width - 1);
                break;
            case OB_ORIENTATION_VERT:
                STRUT_PARTIAL_SET(dock_strut, 0, 0, strw, 0,
                                  0, 0, 0, 0, dock->area.y, dock->area.y
                                  + dock->area.height - 1, 0, 0);
                break;
            }
            break;
        }
    }

    /* not used for actually sizing shit */
    dock->area.width -= ob_rr_theme->obwidth * 2;
    dock->area.height -= ob_rr_theme->obwidth * 2;

    if (dock->dock_apps) {
        g_assert(dock->area.width > 0);
        g_assert(dock->area.height > 0);

        paint_place(dock->frame, dock->area.x, dock->area.y,
                    dock->area.width, dock->area.height);

        paint_canvas(dock->frame, dock->a_frame, dock->area.width,
                     dock->area.height);
        ob_backend->canvas_show(dock->frame, TRUE);
    } else
        ob_backend->canvas_show(dock->frame, FALSE);

    /* but they are useful outside of this function! but don't add it if the
       dock is actually not visible */
    if (dock->dock_apps) {
        dock->area.width += ob_rr_theme->obwidth * 2;
        dock->area.height += ob_rr_theme->obwidth * 2;
    }

    /* screen_resize() depends on this function to call screen_update_areas(),
       so if this changes, also update screen_resize(). */
    screen_update_areas();
}

void dock_app_configure(ObDockApp *app, gint w, gint h)
{
    app->w = w;
    app->h = h;
    dock_configure();
}

void dock_app_drag(ObDockApp *app, gint x, gint y)
{
    ObDockApp *over = NULL;
    GList *it;
    gboolean after;
    gboolean stop;

    /* are we on top of the dock? */
    if (!(x >= dock->area.x &&
          y >= dock->area.y &&
          x < dock->area.x + dock->area.width &&
          y < dock->area.y + dock->area.height))
        return;

    x -= dock->area.x;
    y -= dock->area.y;

    /* which dock app are we on top of? */
    stop = FALSE;
    for (it = dock->dock_apps; it; it = g_list_next(it)) {
        over = it->data;
        switch (config_dock_orient) {
        case OB_ORIENTATION_HORZ:
            if (x >= over->x && x < over->x + over->w)
                stop = TRUE;
            break;
        case OB_ORIENTATION_VERT:
            if (y >= over->y && y < over->y + over->h)
                stop = TRUE;
            break;
        }
        /* dont go to it->next! */
        if (stop) break;
    }
    if (!it || app == over) return;

    x -= over->x;
    y -= over->y;

    switch (config_dock_orient) {
    case OB_ORIENTATION_HORZ:
        after = (x > over->w / 2);
        break;
    case OB_ORIENTATION_VERT:
        after = (y > over->h / 2);
        break;
    default:
        g_assert_not_reached();
    }

    /* remove before doing the it->next! */
    dock->dock_apps = g_list_remove(dock->dock_apps, app);

    if (after) it = it->next;

    dock->dock_apps = g_list_insert_before(dock->dock_apps, it, app);
    dock_configure();
}

static gboolean hide_timeout(gpointer data)
{
    /* hide */
    dock->hidden = TRUE;
    dock_configure();

    return FALSE; /* don't repeat */
}

static gboolean show_timeout(gpointer data)
{
    /* show */
    dock->hidden = FALSE;
    dock_configure();

    return FALSE; /* don't repeat */
}

static void destroy_timeout(gpointer data)
{
    gint *id = data;
    *id = 0;
}

void dock_hide(gboolean hide)
{
    if (!hide) {
        if (dock->hidden && config_dock_hide) {
            show_timeout_id = g_timeout_add_full(G_PRIORITY_DEFAULT,
                                                 config_dock_show_delay,
                                                 show_timeout, &show_timeout_id, destroy_timeout);
        } else if (!dock->hidden && config_dock_hide && hide_timeout_id) {
            if (hide_timeout_id) g_source_remove(hide_timeout_id);
        }
    } else {
        if (!dock->hidden && config_dock_hide) {
            hide_timeout_id = g_timeout_add_full(G_PRIORITY_DEFAULT,
                                                 config_dock_hide_delay,
                                                 hide_timeout, &hide_timeout_id, destroy_timeout);
        } else if (dock->hidden && config_dock_hide && show_timeout_id) {
            if (show_timeout_id) g_source_remove(show_timeout_id);
        }
    }
}

void dock_get_area(Rect *a)
{
    RECT_SET(*a, dock->area.x, dock->area.y,
             dock->area.width, dock->area.height);
}

void dock_raise_dock(void)
{
    stacking_raise(DOCK_AS_WINDOW(dock));
}

void dock_lower_dock(void)
{
    stacking_lower(DOCK_AS_WINDOW(dock));
}

ObDockApp* dock_find_dockapp(Window xwin)
{
    return g_hash_table_lookup(dock->dock_map, &xwin);
}
