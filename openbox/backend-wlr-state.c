/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-wlr-state.c for the Lumen window manager

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

/* lumen_state_v1, for the contract tests only (LUMEN_TEST_PROTOCOL): what
   the core publishes for pagers, as EWMH carries it on X11, and the
   requests a pager sends. A client hears only about its own windows. */

#include "backend-wlr-int.h"
#include "lumen-state-v1-protocol.h"

#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>

static struct wl_global *global;

static gpointer dup(gconstpointer p, gsize size)
{
    gpointer d = g_malloc(size);

    if (size) memcpy(d, p, size);
    return d;
}

/* what the core published */
static GArray *stacking;        /* ObSurfaceId, bottom first */
static ObSurfaceId active;
static guint desktop_count = 1, current_desktop;
static Rect *workareas;
static guint n_workareas;
static GHashTable *desktops;    /* ObSurfaceId -> desktop + 1 */
static GHashTable *titles;      /* ObSurfaceId -> gchar* */
static GHashTable *extents;     /* ObSurfaceId -> Strut* */

void lw_state_stacking(const ObSurfaceId *ids, guint n)
{
    g_array_set_size(stacking, 0);
    g_array_append_vals(stacking, ids, n);
}

void lw_state_active(ObSurfaceId id)
{
    active = id;
}

void lw_state_desktop_count(guint count)
{
    desktop_count = count;
}

void lw_state_current_desktop(guint desktop)
{
    current_desktop = desktop;
}

void lw_state_workareas(const Rect *areas, guint n)
{
    g_free(workareas);
    workareas = dup(areas, n * sizeof(Rect));
    n_workareas = n;
}

void lw_state_surface_desktop(ObSurfaceId id, guint desktop)
{
    g_hash_table_insert(desktops, (gpointer)id,
                        GUINT_TO_POINTER(desktop + 1));
}

void lw_state_surface_titles(ObSurfaceId id, const gchar *title,
                             const gchar *icon)
{
    if (title)
        g_hash_table_insert(titles, (gpointer)id, g_strdup(title));
}

void lw_state_surface_extents(ObSurfaceId id, const Strut *e)
{
    g_hash_table_insert(extents, (gpointer)id, dup(e, sizeof(*e)));
}

void lw_state_forget(ObSurfaceId id)
{
    g_hash_table_remove(desktops, (gpointer)id);
    g_hash_table_remove(titles, (gpointer)id);
    g_hash_table_remove(extents, (gpointer)id);
}

/* reading it back */

static gboolean node_visible(struct wlr_scene_node *n)
{
    for (; n; n = n->parent ? &n->parent->node : NULL)
        if (!n->enabled) return FALSE;
    return TRUE;
}

/*! Menus, popups, panels: canvases that are no window's, on screen. */
static guint popups(void)
{
    guint count = 0, l;

    for (l = WLR_LAYER_PANEL; l < WLR_NUM_LAYERS; ++l) {
        struct wlr_scene_node *n;

        wl_list_for_each(n, &lw_layers[l]->children, link) {
            WlrNodeData *d = n->data;
            WlrCanvas *c = (WlrCanvas*)d;
            if (d && d->kind == WLR_NODE_CANVAS && n->enabled &&
                c->x + c->width > 0 && c->y + c->height > 0)
                ++count;
        }
    }
    return count;
}

static void send_window(struct wl_resource *r, WlrSurface *s, gint stack)
{
    ObSurfaceId id = lw_surface_id(s);
    struct wl_resource *surface;
    const Strut *e = g_hash_table_lookup(extents, (gpointer)id);
    guint32 states = 0;
    gint x, y;
    gpointer d;

    if (!s->toplevel) return;
    surface = s->toplevel->base->surface->resource;
    /* only its own */
    if (wl_resource_get_client(surface) != wl_resource_get_client(r))
        return;

    if ((s->states & OB_SURFACE_STATE_MAXIMIZED_HORZ) &&
        (s->states & OB_SURFACE_STATE_MAXIMIZED_VERT))
        states |= LUMEN_STATE_V1_STATE_MAXIMIZED;
    if (s->states & OB_SURFACE_STATE_FULLSCREEN)
        states |= LUMEN_STATE_V1_STATE_FULLSCREEN;
    if (s->states & OB_SURFACE_STATE_ICONIC)
        states |= LUMEN_STATE_V1_STATE_ICONIC;
    if (s->states & OB_SURFACE_STATE_ABOVE)
        states |= LUMEN_STATE_V1_STATE_ABOVE;
    if (active == id)
        states |= LUMEN_STATE_V1_STATE_FOCUSED;
    if (s->tree && node_visible(&s->tree->node))
        states |= LUMEN_STATE_V1_STATE_VIEWABLE;
    d = g_hash_table_lookup(desktops, (gpointer)id);

    lumen_state_v1_send_window(r, surface, states,
                               d ? (gint)GPOINTER_TO_UINT(d) - 1 : 0,
                               stack, g_hash_table_lookup(titles,
                                                          (gpointer)id));
    x = s->x;
    y = s->y;
    if (s->tree) wlr_scene_node_coords(&s->tree->node, &x, &y);
    lumen_state_v1_send_frame(r, x - (e ? e->left : 0), y - (e ? e->top : 0),
                              s->width + (e ? e->left + e->right : 0),
                              s->height + (e ? e->top + e->bottom : 0));
    lumen_state_v1_send_client(r, x, y, s->width, s->height);
}

static void state_refresh(struct wl_client *client, struct wl_resource *r)
{
    struct wlr_box box;
    struct wlr_surface *kb = lw_seat->keyboard_state.focused_surface;
    WlrSurface *ks = kb ? lw_surface_of(kb) : NULL;
    guint i;

    wlr_output_layout_get_box(lw_layout, NULL, &box);
    lumen_state_v1_send_screen(r, 0, 0, box.x + box.width,
                               box.y + box.height);
    if (current_desktop < n_workareas) {
        const Rect *a = &workareas[current_desktop];
        lumen_state_v1_send_workarea(r, a->x, a->y, a->width, a->height);
    } else
        lumen_state_v1_send_workarea(r, 0, 0, box.x + box.width,
                                     box.y + box.height);
    lumen_state_v1_send_desktops(r, desktop_count, current_desktop);
    /* the keys go to the window manager when no client has them */
    lumen_state_v1_send_focus(r, !active && (!ks || !ks->toplevel));
    lumen_state_v1_send_popups(r, popups());
    for (i = 0; i < stacking->len; ++i) {
        WlrSurface *s =
            lw_surface_lookup(g_array_index(stacking, ObSurfaceId, i));
        if (s) send_window(r, s, i);
    }
    lumen_state_v1_send_done(r);
}

/* a pager's requests */

static ObSurfaceId id_from(struct wl_resource *surface)
{
    WlrSurface *s = lw_surface_of(wlr_surface_from_resource(surface));

    return s && s->managed ? lw_surface_id(s) : OB_SURFACE_NONE;
}

static void state_activate(struct wl_client *client, struct wl_resource *r,
                           struct wl_resource *surface)
{
    ObSurfaceId id = id_from(surface);

    if (!id) return;
    lw_begin(0);
    lw_events->request_activate(id, OB_REQUEST_FROM_PAGER, 0);
    lw_end();
}

static void state_close(struct wl_client *client, struct wl_resource *r,
                        struct wl_resource *surface)
{
    ObSurfaceId id = id_from(surface);

    if (!id) return;
    lw_begin(0);
    lw_events->request_close(id, OB_REQUEST_FROM_PAGER);
    lw_end();
}

static void state_set_state(struct wl_client *client, struct wl_resource *r,
                            struct wl_resource *surface, uint32_t state,
                            uint32_t action)
{
    ObSurfaceId id = id_from(surface);
    ObSurfaceStates first = 0, second = 0;

    if (!id) return;
    switch (state) {
    case LUMEN_STATE_V1_STATE_MAXIMIZED:
        first = OB_SURFACE_STATE_MAXIMIZED_VERT;
        second = OB_SURFACE_STATE_MAXIMIZED_HORZ;
        break;
    case LUMEN_STATE_V1_STATE_FULLSCREEN:
        first = OB_SURFACE_STATE_FULLSCREEN;
        break;
    case LUMEN_STATE_V1_STATE_ICONIC:
        first = OB_SURFACE_STATE_ICONIC;
        break;
    case LUMEN_STATE_V1_STATE_ABOVE:
        first = OB_SURFACE_STATE_ABOVE;
        break;
    default:
        return;
    }
    lw_begin(0);
    lw_events->request_states(id, action == LUMEN_STATE_V1_ACTION_ADD ?
                              OB_STATE_ACTION_ADD :
                              action == LUMEN_STATE_V1_ACTION_TOGGLE ?
                              OB_STATE_ACTION_TOGGLE : OB_STATE_ACTION_REMOVE,
                              first, second, OB_REQUEST_FROM_PAGER);
    lw_end();
}

static void state_raise(struct wl_client *client, struct wl_resource *r,
                        struct wl_resource *surface)
{
    ObSurfaceId id = id_from(surface);

    if (!id) return;
    lw_begin(0);
    lw_events->request_restack(id, 0, OB_STACK_ABOVE, OB_REQUEST_FROM_PAGER);
    lw_end();
}

static void state_lower(struct wl_client *client, struct wl_resource *r,
                        struct wl_resource *surface)
{
    ObSurfaceId id = id_from(surface);
    ObConfigureRequest req;

    if (!id) return;
    memset(&req, 0, sizeof(req));
    req.fields = OB_CONFIGURE_STACK;
    req.stack = OB_STACK_BELOW;
    lw_begin(0);
    lw_events->request_configure(id, &req);
    lw_end();
}

static void state_move_resize(struct wl_client *client,
                              struct wl_resource *r,
                              struct wl_resource *surface, int32_t x,
                              int32_t y, int32_t width, int32_t height)
{
    ObSurfaceId id = id_from(surface);
    ObConfigureRequest req;

    if (!id) return;
    memset(&req, 0, sizeof(req));
    req.fields = OB_CONFIGURE_X | OB_CONFIGURE_Y | OB_CONFIGURE_WIDTH |
        OB_CONFIGURE_HEIGHT | OB_CONFIGURE_GRAVITY;
    RECT_SET(req.want, x, y, width, height);
    req.gravity = 1; /* NorthWest: x, y is the frame's corner */
    lw_begin(0);
    lw_events->request_geometry(id, &req, OB_REQUEST_FROM_PAGER);
    lw_end();
}

static void state_keyboard_move(struct wl_client *client,
                                struct wl_resource *r,
                                struct wl_resource *surface)
{
    ObSurfaceId id = id_from(surface);

    if (!id) return;
    lw_begin(0);
    lw_events->request_move_resize(id, OB_MOVERESIZE_MOVE_KEYBOARD, 0, 0, 0);
    lw_end();
}

static void state_desktop(struct wl_client *client, struct wl_resource *r,
                          uint32_t desktop)
{
    lw_begin(0);
    lw_events->request_desktop(desktop, 0);
    lw_end();
}

static void state_window_desktop(struct wl_client *client,
                                 struct wl_resource *r,
                                 struct wl_resource *surface,
                                 int32_t desktop)
{
    ObSurfaceId id = id_from(surface);

    if (!id) return;
    lw_begin(0);
    /* DESKTOP_ALL for every desktop */
    lw_events->request_surface_desktop(id, desktop < 0 ? 0xffffffff :
                                       (guint)desktop);
    lw_end();
}

static void state_destroy(struct wl_client *client, struct wl_resource *r)
{
    wl_resource_destroy(r);
}

static const struct lumen_state_v1_interface state_impl = {
    .destroy = state_destroy,
    .refresh = state_refresh,
    .activate = state_activate,
    .close = state_close,
    .set_state = state_set_state,
    .raise = state_raise,
    .lower = state_lower,
    .move_resize = state_move_resize,
    .keyboard_move = state_keyboard_move,
    .desktop = state_desktop,
    .window_desktop = state_window_desktop,
};

static void state_bind(struct wl_client *client, void *data, uint32_t version,
                       uint32_t id)
{
    struct wl_resource *r =
        wl_resource_create(client, &lumen_state_v1_interface, version, id);

    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &state_impl, NULL, NULL);
}

void lw_state_startup(void)
{
    stacking = g_array_new(FALSE, FALSE, sizeof(ObSurfaceId));
    desktops = g_hash_table_new(g_direct_hash, g_direct_equal);
    titles = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                   g_free);
    extents = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                    g_free);
    if (g_getenv("LUMEN_TEST_PROTOCOL"))
        global = wl_global_create(lw_display, &lumen_state_v1_interface, 1,
                                  NULL, state_bind);
}

void lw_state_shutdown(void)
{
    if (global) wl_global_destroy(global);
    global = NULL;
    g_array_free(stacking, TRUE);
    g_hash_table_unref(desktops);
    g_hash_table_unref(titles);
    g_hash_table_unref(extents);
    g_free(workareas);
    workareas = NULL;
    n_workareas = 0;
}
