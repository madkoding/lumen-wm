/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-wlr.c for the Lumen window manager

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

/* The wlroots backend: Lumen as a Wayland compositor. This file starts the
   display, the outputs and the scene, and runs wlroots' event loop inside
   the core's GLib main loop. */

#include "backend-wlr-int.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>

const ObBackendEvents *lw_events = NULL;
struct wl_display *lw_display = NULL;
struct wlr_scene *lw_scene = NULL;
struct wlr_scene_tree *lw_layers[WLR_NUM_LAYERS];

static struct wlr_backend *backend;
static struct wlr_renderer *renderer;
static struct wlr_allocator *allocator;
struct wlr_output_layout *lw_layout;
struct wlr_compositor *lw_compositor;
static struct wlr_scene_output_layout *scene_layout;
static struct wl_listener new_output;
static gboolean started = FALSE;
static gchar *running_pid_file;  /* for --reconfigure and --exit */
static gulong serial = 1;

typedef struct {
    struct wlr_output *output;
    struct wlr_scene_output *scene_output;
    struct wl_listener frame, request_state, destroy;
} WlrOutput;

static GList *outputs;          /* WlrOutput, in the order they came */
static Rect *output_areas;
static guint n_output_areas;

guint32 lw_now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void lw_begin(guint32 time)
{
    lw_events->begin(time ? time : lw_now(), serial);
}

void lw_end(void)
{
    lw_events->end();
}

/* outputs */

static void output_frame(struct wl_listener *l, void *data)
{
    WlrOutput *o = wl_container_of(l, o, frame);
    struct timespec now;

    wlr_scene_output_commit(o->scene_output, NULL);
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(o->scene_output, &now);
}

static void output_request_state(struct wl_listener *l, void *data)
{
    WlrOutput *o = wl_container_of(l, o, request_state);
    const struct wlr_output_event_request_state *e = data;

    wlr_output_commit_state(o->output, e->state);
}

static void outputs_changed(void)
{
    if (!started) return;
    lw_begin(0);
    lw_events->outputs_changed();
    lw_end();
}

static void output_destroy(struct wl_listener *l, void *data)
{
    WlrOutput *o = wl_container_of(l, o, destroy);

    wl_list_remove(&o->frame.link);
    wl_list_remove(&o->request_state.link);
    wl_list_remove(&o->destroy.link);
    outputs = g_list_remove(outputs, o);
    g_free(o);
    outputs_changed();
}

static void output_new(struct wl_listener *l, void *data)
{
    struct wlr_output *output = data;
    struct wlr_output_mode *mode;
    struct wlr_output_layout_output *lo;
    WlrOutput *o;

    wlr_output_init_render(output, allocator, renderer);

    wlr_output_enable(output, TRUE);
    if ((mode = wlr_output_preferred_mode(output)))
        wlr_output_set_mode(output, mode);
    if (!wlr_output_commit(output)) {
        g_message("Output %s could not be turned on", output->name);
        return;
    }

    o = g_new0(WlrOutput, 1);
    o->output = output;
    o->frame.notify = output_frame;
    wl_signal_add(&output->events.frame, &o->frame);
    o->request_state.notify = output_request_state;
    wl_signal_add(&output->events.request_state, &o->request_state);
    o->destroy.notify = output_destroy;
    wl_signal_add(&output->events.destroy, &o->destroy);
    outputs = g_list_append(outputs, o);

    lo = wlr_output_layout_add_auto(lw_layout, output);
    o->scene_output = wlr_scene_output_create(lw_scene, output);
    wlr_scene_output_layout_add_output(scene_layout, lo, o->scene_output);

    outputs_changed();
}

static guint lw_output_count(void)
{
    GList *it;
    guint i;

    g_free(output_areas);
    n_output_areas = MAX(g_list_length(outputs), 1);
    output_areas = g_new0(Rect, n_output_areas);
    for (it = outputs, i = 0; it; it = g_list_next(it), ++i) {
        WlrOutput *o = it->data;
        struct wlr_box box;

        wlr_output_layout_get_box(lw_layout, o->output, &box);
        RECT_SET(output_areas[i], box.x, box.y, box.width, box.height);
    }
    /* no output yet: pretend there is one, so the core has a screen */
    if (!outputs)
        RECT_SET(output_areas[0], 0, 0, 1024, 768);
    return n_output_areas;
}

static void lw_output_area(guint output, Rect *area)
{
    g_return_if_fail(output < n_output_areas);
    *area = output_areas[output];
}

static void lw_layout_size(gint *width, gint *height)
{
    struct wlr_box box;

    wlr_output_layout_get_box(lw_layout, NULL, &box);
    if (wlr_box_empty(&box)) {
        *width = 1024;
        *height = 768;
    } else {
        /* the core's layout starts at 0, 0 */
        *width = box.x + box.width;
        *height = box.y + box.height;
    }
}

/* lifetime */

static void log_glib(enum wlr_log_importance level, const char *fmt,
                     va_list args)
{
    gchar *s = g_strdup_vprintf(fmt, args);

    if (level == WLR_ERROR)
        g_message("wlroots: %s", s);
    else
        g_debug("wlroots: %s", s);
    g_free(s);
}

/*! Where the instance running on a Wayland socket keeps its pid. */
static gchar *pid_file(const gchar *socket)
{
    gchar *base = g_path_get_basename(socket);
    gchar *name = g_strdup_printf("lumen-%s.pid", base);
    gchar *path = g_build_filename(g_get_user_runtime_dir(), name, NULL);

    g_free(base);
    g_free(name);
    return path;
}

static gboolean lw_startup(const gchar *display, ObStartupFlags flags,
                            const ObBackendEvents *events)
{
    const gchar *socket;
    struct wlr_scene_tree *root;
    gint i;

    lw_events = events;
    /* --reconfigure and --exit only signal the running instance */
    if (flags & OB_STARTUP_REMOTE) return TRUE;
    wlr_log_init(g_getenv("LUMEN_WLR_DEBUG") ? WLR_DEBUG : WLR_ERROR,
                 log_glib);

    lw_display = wl_display_create();
    backend = wlr_backend_autocreate(lw_display, NULL);
    if (!backend) {
        g_message("wlroots could not open a backend");
        return FALSE;
    }
    renderer = wlr_renderer_autocreate(backend);
    if (!renderer) {
        g_message("wlroots could not create a renderer");
        return FALSE;
    }
    wlr_renderer_init_wl_display(renderer, lw_display);
    allocator = wlr_allocator_autocreate(backend, renderer);

    lw_compositor = wlr_compositor_create(lw_display, 5, renderer);
    wlr_subcompositor_create(lw_display);
    wlr_data_device_manager_create(lw_display);

    lw_layout = wlr_output_layout_create();
    wlr_xdg_output_manager_v1_create(lw_display, lw_layout);
    wlr_screencopy_manager_v1_create(lw_display);
    new_output.notify = output_new;
    wl_signal_add(&backend->events.new_output, &new_output);

    lw_scene = wlr_scene_create();
    scene_layout = wlr_scene_attach_output_layout(lw_scene, lw_layout);
    root = &lw_scene->tree;
    for (i = 0; i < WLR_NUM_LAYERS; ++i)
        lw_layers[i] = wlr_scene_tree_create(root);

    lw_surfaces_startup();
    lw_layers_startup();
    lw_toplevels_startup();
    lw_xwayland_startup();
    lw_input_startup(backend);

    socket = wl_display_add_socket_auto(lw_display);
    if (!socket) {
        g_message("Could not make a Wayland socket");
        return FALSE;
    }
    if (!wlr_backend_start(backend)) {
        g_message("wlroots could not start its backend");
        return FALSE;
    }
    /* what Lumen starts (autostart, actions) runs on this display */
    g_setenv("WAYLAND_DISPLAY", socket, TRUE);
    g_message("Running on WAYLAND_DISPLAY=%s", socket);
    {
        gchar *pid = g_strdup_printf("%d\n", (gint)getpid());
        running_pid_file = pid_file(socket);
        g_file_set_contents(running_pid_file, pid, -1, NULL);
        g_free(pid);
    }

    /* let the outputs come before the core asks about them */
    wl_event_loop_dispatch(wl_display_get_event_loop(lw_display), 0);
    return TRUE;
}

static void lw_shutdown(void)
{
    if (!lw_display) return;
    if (running_pid_file) {
        unlink(running_pid_file);
        g_free(running_pid_file);
        running_pid_file = NULL;
    }
    wl_display_destroy_clients(lw_display);
    lw_input_shutdown();
    lw_xwayland_shutdown();
    lw_toplevels_shutdown();
    lw_layers_shutdown();
    lw_surfaces_shutdown();
    wl_list_remove(&new_output.link);
    wlr_backend_destroy(backend);
    wl_display_destroy(lw_display);
    wlr_scene_node_destroy(&lw_scene->tree.node);
    wlr_output_layout_destroy(lw_layout);
    lw_display = NULL;
    g_free(output_areas);
    output_areas = NULL;
}

static gboolean lw_claim_screen(void)
{
    /* the compositor is the window manager */
    started = TRUE;
    return TRUE;
}

static void lw_release_screen(void)
{
    started = FALSE;
}

typedef struct {
    GSource source;
    gpointer fd;
} WlrSource;

static gboolean source_prepare(GSource *s, gint *timeout)
{
    wl_display_flush_clients(lw_display);
    *timeout = -1;
    return FALSE;
}

static gboolean source_check(GSource *s)
{
    WlrSource *ws = (WlrSource*)s;

    return g_source_query_unix_fd(s, ws->fd) & G_IO_IN;
}

static gboolean source_dispatch(GSource *s, GSourceFunc cb, gpointer data)
{
    wl_event_loop_dispatch(wl_display_get_event_loop(lw_display), 0);
    wl_display_flush_clients(lw_display);
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs source_funcs = {
    source_prepare, source_check, source_dispatch, NULL
};

static GSource *lw_event_source(void)
{
    GSource *s = g_source_new(&source_funcs, sizeof(WlrSource));
    struct wl_event_loop *loop = wl_display_get_event_loop(lw_display);

    ((WlrSource*)s)->fd =
        g_source_add_unix_fd(s, wl_event_loop_get_fd(loop), G_IO_IN);
    g_source_set_name(s, "wlroots");
    /* what clients do comes before the core's timers */
    g_source_set_priority(s, G_PRIORITY_HIGH);
    return s;
}

static void lw_request_running(guint what)
{
    const gchar *socket = g_getenv("WAYLAND_DISPLAY");
    gchar *path, *text = NULL;
    gint pid = 0;

    if (!socket) socket = "wayland-0";
    path = pid_file(socket);
    if (g_file_get_contents(path, &text, NULL, NULL))
        pid = atoi(text);
    if (pid <= 0)
        g_message("No Lumen runs on WAYLAND_DISPLAY=%s", socket);
    /* restarting reconfigures here: the windows live in this compositor */
    else if (kill(pid, what == 3 ? SIGTERM : SIGUSR2) < 0)
        g_message("Could not reach Lumen (pid %d): %s", pid,
                  g_strerror(errno));
    g_free(text);
    g_free(path);
}

static void lw_previous_state(ObPreviousState *state)
{
    memset(state, 0, sizeof(*state));
}

static gchar **lw_desktop_names(void)
{
    return NULL;
}

static gboolean lw_desktop_layout(ObDesktopLayout *l)
{
    return FALSE;
}

static void lw_font_settings(RrFontSettings *fs)
{
    fs->dpi = 96;
    fs->antialias = CAIRO_ANTIALIAS_GRAY;
    fs->hint_style = CAIRO_HINT_STYLE_SLIGHT;
    fs->subpixel_order = CAIRO_SUBPIXEL_ORDER_DEFAULT;
}

static gboolean lw_screen_read(const Rect *area, ObImage *img)
{
    /* everything composites, so nothing draws what is under it itself */
    return FALSE;
}

static void lw_flush(gboolean wait)
{
    wl_display_flush_clients(lw_display);
}

static gulong lw_request_serial(void)
{
    return serial;
}

static void lw_request_serial_bump(void)
{
    ++serial;
}

static void lw_grab_server(gboolean grab)
{
}

/* what Wayland has no use for, or that waits for its protocol */

static void nothing_surface(ObSurfaceId s) {}
static void nothing_surface_bool(ObSurfaceId s, gboolean b) {}
static void nothing_surface_int(ObSurfaceId s, gint i) {}
static void nothing_surface_uint(ObSurfaceId s, guint i) {}
static void nothing_ids(const ObSurfaceId *s, guint n) {}
static void nothing_uint(guint u) {}
static void nothing_bool(gboolean b) {}
static void nothing_string(const gchar *s) {}
static void nothing_size(gint w, gint h) {}
static void nothing_surface_geometry(ObSurfaceId s, const Rect *a, gint b) {}
static void nothing_names(const gchar *const *names) {}
static void nothing_icon(ObSurfaceId s, const ObIcon *icon) {}
static void nothing_titles(ObSurfaceId s, const gchar *t, const gchar *i) {}
static void nothing_extents(ObSurfaceId s, const Strut *e) {}
static void nothing_actions(ObSurfaceId s, ObSurfaceActions a) {}
static void nothing_app(ObSurfaceId s, const ObSurfaceApp *a) {}
static void nothing_dockapp_adopt(ObSurfaceId i, ObSurfaceId n, ObCanvasId d,
                                  gint x, gint y) {}
static void nothing_dockapp_release(ObSurfaceId i, ObSurfaceId n,
                                    gboolean r) {}
static gboolean no_sync_start(ObSurfaceId s, guint32 c) { return FALSE; }
static void nothing_sync_request(ObSurfaceId s, guint32 t) {}
static void nothing(void) {}

const ObBackend ob_backend_wlr = {
    .name = "wlroots",
    .caps = 0,

    .startup = lw_startup,
    .shutdown = lw_shutdown,
    .claim_screen = lw_claim_screen,
    .release_screen = lw_release_screen,
    .event_source = lw_event_source,
    .time = lw_now,
    .request_running = lw_request_running,
    .previous_state = lw_previous_state,
    .desktop_names = lw_desktop_names,
    .desktop_layout = lw_desktop_layout,

    .output_count = lw_output_count,
    .output_area = lw_output_area,
    .layout_size = lw_layout_size,

    .existing_surfaces = lw_existing_surfaces,
    .surface_kind = lw_surface_kind,
    .dockapp_adopt = nothing_dockapp_adopt,
    .dockapp_release = nothing_dockapp_release,
    .surface_adopt = lw_surface_adopt,
    .surface_unwatch = lw_surface_unwatch,
    .surface_frame = lw_surface_frame,
    .surface_unframe = lw_surface_unframe,
    .surface_release = lw_surface_release,
    .surface_alive = lw_surface_alive,
    .surface_set_border = nothing_surface_int,
    .surface_configure = lw_surface_configure,
    .surface_install_colors = nothing_surface,
    .surface_send_geometry = lw_surface_send_geometry,
    .surface_show = lw_surface_show,
    .surface_focus = lw_surface_focus,
    .surface_close = lw_surface_close,
    .surface_kill = lw_surface_kill,
    .surface_set_states = lw_surface_set_states,
    .surface_ping = lw_surface_ping,
    .sync_start = no_sync_start,
    .sync_request = nothing_sync_request,
    .sync_stop = nothing,
    .surface_info = lw_surface_info,

    .restack = lw_restack,

    .canvas_new = lw_canvas_new,
    .canvas_new_child = lw_canvas_new_child,
    .canvas_free = lw_canvas_free,
    .canvas_has_alpha = lw_canvas_has_alpha,
    .canvas_place = lw_canvas_place,
    .canvas_move = lw_canvas_move,
    .canvas_resize = lw_canvas_resize,
    .canvas_show = lw_canvas_show,
    .canvas_present = lw_canvas_present,
    .canvas_fill = lw_canvas_fill,
    .canvas_set_border = lw_canvas_set_border,
    .canvas_set_outline = lw_canvas_set_outline,
    .canvas_set_input = lw_canvas_set_input,
    .canvas_set_strut = lw_canvas_set_strut,
    .reserved_edges = lw_reserved_edges,
    .canvas_set_shape = lw_canvas_set_shape,
    .canvas_set_opacity = lw_canvas_set_opacity,
    .font_settings = lw_font_settings,
    .screen_read = lw_screen_read,

    .dialog_set_title = lw_dialog_set_title,
    .dialog_set_size = lw_dialog_set_size,
    .dialog_set_parent = lw_dialog_set_parent,

    .grab_pointer = lw_grab_pointer,
    .grab_keyboard = lw_grab_keyboard,
    .grab_server = lw_grab_server,
    .modkey_mask = lw_modkey_mask,
    .only_modmasks = lw_only_modmasks,
    .keymap_reload = lw_keymap_reload,
    .key_named = lw_key_named,
    .bind_key = lw_bind_key,
    .unbind_all_keys = lw_unbind_all_keys,
    .bind_button = lw_bind_button,
    .unbind_button = lw_unbind_button,
    .pass_click = lw_pass_click,
    .pointer_position = lw_pointer_position,
    .pointer_warp = lw_pointer_warp,
    .flush = lw_flush,
    .set_cursor = lw_set_cursor,
    .request_serial = lw_request_serial,
    .request_serial_bump = lw_request_serial_bump,

    /* foreign-toplevel and ext-workspace carry these, later in phase 5 */
    .publish_clients = lw_publish_clients,
    .publish_stacking = lw_publish_stacking,
    .publish_active = lw_publish_active,
    .publish_desktop_count = nothing_uint,
    .publish_current_desktop = nothing_uint,
    .publish_desktop_names = nothing_names,
    .publish_surface_desktop = nothing_surface_uint,
    .publish_surface_icon = nothing_icon,
    .publish_surface_titles = lw_publish_surface_titles,
    .publish_workareas = lw_xw_workareas,
    .publish_layout_size = nothing_size,
    .publish_last_desktop = nothing_uint,
    .publish_showing_desktop = nothing_bool,
    .publish_config_file = nothing_string,
    .publish_theme = nothing_string,
    .publish_surface_extents = nothing_extents,
    .publish_surface_actions = nothing_actions,
    .publish_surface_app = nothing_app,
    .publish_surface_hidden = nothing_surface_bool,
};
