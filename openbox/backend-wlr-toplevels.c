/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-wlr-toplevels.c for the Lumen window manager

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

/* What taskbars see (wlr-foreign-toplevel-management): one handle per
   window the core manages, kept up to date from what the core publishes,
   as _NET_CLIENT_LIST and friends are on X11. What a taskbar asks for goes
   to the core as a pager's request. */

#include "backend-wlr-int.h"

#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>

typedef struct {
    ObSurfaceId id;
    struct wlr_foreign_toplevel_handle_v1 *wlr;
    struct wlr_output *output;
    struct wl_listener activate, close, maximize, minimize, fullscreen;
} Handle;

static struct wlr_foreign_toplevel_manager_v1 *manager;
static GHashTable *handles;     /* ObSurfaceId -> Handle */
static ObSurfaceId active;

static void handle_activate(struct wl_listener *l, void *data)
{
    Handle *h = wl_container_of(l, h, activate);

    lw_begin(0);
    lw_events->request_activate(h->id, OB_REQUEST_FROM_PAGER, 0);
    lw_end();
}

static void handle_close(struct wl_listener *l, void *data)
{
    Handle *h = wl_container_of(l, h, close);

    lw_begin(0);
    lw_events->request_close(h->id, OB_REQUEST_FROM_PAGER);
    lw_end();
}

/*! first and second: one state each, as the core takes them */
static void request_states(Handle *h, ObSurfaceStates first,
                           ObSurfaceStates second, gboolean on)
{
    lw_begin(0);
    lw_events->request_states(h->id, on ? OB_STATE_ACTION_ADD :
                              OB_STATE_ACTION_REMOVE, first, second,
                              OB_REQUEST_FROM_PAGER);
    lw_end();
}

static void handle_maximize(struct wl_listener *l, void *data)
{
    Handle *h = wl_container_of(l, h, maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *e = data;

    request_states(h, OB_SURFACE_STATE_MAXIMIZED_HORZ,
                   OB_SURFACE_STATE_MAXIMIZED_VERT, e->maximized);
}

static void handle_fullscreen(struct wl_listener *l, void *data)
{
    Handle *h = wl_container_of(l, h, fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *e = data;

    request_states(h, OB_SURFACE_STATE_FULLSCREEN, 0, e->fullscreen);
}

static void handle_minimize(struct wl_listener *l, void *data)
{
    Handle *h = wl_container_of(l, h, minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *e = data;

    lw_begin(0);
    lw_events->request_iconify(h->id, e->minimized);
    lw_end();
}

/*! Taskbars show the windows of their own output. */
static void update_output(Handle *h)
{
    struct wlr_output *o = lw_surface_output(h->id);

    if (o == h->output) return;
    if (h->output)
        wlr_foreign_toplevel_handle_v1_output_leave(h->wlr, h->output);
    if (o) wlr_foreign_toplevel_handle_v1_output_enter(h->wlr, o);
    h->output = o;
}

static Handle *handle_new(ObSurfaceId id, struct wlr_xdg_toplevel *t)
{
    Handle *h = g_new0(Handle, 1);

    h->id = id;
    h->wlr = wlr_foreign_toplevel_handle_v1_create(manager);
    if (t->title) wlr_foreign_toplevel_handle_v1_set_title(h->wlr, t->title);
    if (t->app_id)
        wlr_foreign_toplevel_handle_v1_set_app_id(h->wlr, t->app_id);
    h->activate.notify = handle_activate;
    wl_signal_add(&h->wlr->events.request_activate, &h->activate);
    h->close.notify = handle_close;
    wl_signal_add(&h->wlr->events.request_close, &h->close);
    h->maximize.notify = handle_maximize;
    wl_signal_add(&h->wlr->events.request_maximize, &h->maximize);
    h->minimize.notify = handle_minimize;
    wl_signal_add(&h->wlr->events.request_minimize, &h->minimize);
    h->fullscreen.notify = handle_fullscreen;
    wl_signal_add(&h->wlr->events.request_fullscreen, &h->fullscreen);
    update_output(h);
    return h;
}

static void handle_free(gpointer data)
{
    Handle *h = data;

    wl_list_remove(&h->activate.link);
    wl_list_remove(&h->close.link);
    wl_list_remove(&h->maximize.link);
    wl_list_remove(&h->minimize.link);
    wl_list_remove(&h->fullscreen.link);
    wlr_foreign_toplevel_handle_v1_destroy(h->wlr);
    g_free(h);
}

/* what the core publishes */

void lw_publish_clients(const ObSurfaceId *ids, guint n)
{
    GHashTable *now = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, handle_free);
    guint i;

    for (i = 0; i < n; ++i) {
        gpointer key = (gpointer)ids[i];
        gpointer h;
        struct wlr_xdg_toplevel *t;

        if ((h = g_hash_table_lookup(handles, key))) {
            g_hash_table_steal(handles, key);
            g_hash_table_insert(now, key, h);
        }
        else if ((t = lw_surface_toplevel(ids[i])))
            /* the core's own dialogs aren't for taskbars */
            g_hash_table_insert(now, key, handle_new(ids[i], t));
    }
    /* what is left went away */
    g_hash_table_unref(handles);
    handles = now;
    if (active && !g_hash_table_contains(handles, (gpointer)active))
        active = 0;
}

void lw_publish_stacking(const ObSurfaceId *ids, guint n)
{
    GHashTableIter it;
    gpointer h;

    /* windows moved, maybe to another output */
    g_hash_table_iter_init(&it, handles);
    while (g_hash_table_iter_next(&it, NULL, &h))
        update_output(h);
}

void lw_publish_active(ObSurfaceId id)
{
    Handle *h;

    if ((h = g_hash_table_lookup(handles, (gpointer)active)))
        wlr_foreign_toplevel_handle_v1_set_activated(h->wlr, FALSE);
    active = id;
    if ((h = g_hash_table_lookup(handles, (gpointer)active))) {
        wlr_foreign_toplevel_handle_v1_set_activated(h->wlr, TRUE);
        update_output(h);
    }
}

void lw_publish_surface_titles(ObSurfaceId id, const gchar *title,
                               const gchar *icon)
{
    Handle *h = g_hash_table_lookup(handles, (gpointer)id);

    if (h && title) wlr_foreign_toplevel_handle_v1_set_title(h->wlr, title);
}

void lw_toplevel_app_id(ObSurfaceId id, const gchar *app_id)
{
    Handle *h = g_hash_table_lookup(handles, (gpointer)id);

    if (h && app_id) wlr_foreign_toplevel_handle_v1_set_app_id(h->wlr, app_id);
}

void lw_toplevel_states(ObSurfaceId id, ObSurfaceStates states)
{
    Handle *h = g_hash_table_lookup(handles, (gpointer)id);

    if (!h) return;
    wlr_foreign_toplevel_handle_v1_set_maximized(h->wlr,
        (states & OB_SURFACE_STATE_MAXIMIZED_HORZ) &&
        (states & OB_SURFACE_STATE_MAXIMIZED_VERT));
    wlr_foreign_toplevel_handle_v1_set_minimized(h->wlr,
        !!(states & OB_SURFACE_STATE_ICONIC));
    wlr_foreign_toplevel_handle_v1_set_fullscreen(h->wlr,
        !!(states & OB_SURFACE_STATE_FULLSCREEN));
}

void lw_toplevels_startup(void)
{
    manager = wlr_foreign_toplevel_manager_v1_create(lw_display);
    handles = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                    handle_free);
    active = 0;
}

void lw_toplevels_shutdown(void)
{
    g_hash_table_unref(handles);
    handles = NULL;
}
