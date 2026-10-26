/* Lumen contract tests: the Wayland driver.

   Plays a client and a pager at once, as the X11 driver does: it makes
   xdg-shell windows drawn in one colour (layer-shell surfaces for docks and
   popups), types and clicks with a virtual keyboard and pointer, and reads
   back what the window manager did through lumen_state_v1, which Lumen
   offers when LUMEN_TEST_PROTOCOL is set (protocols/lumen-state-v1.xml).

   xdg-shell does not let a client place itself, pick its desktop or take
   the focus, so those cases skip. */

#define _GNU_SOURCE
#include "driver.h"

#include "lumen-state-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

struct ct_win {
    ct_win_type type;
    ct_rect want;
    int minw, minh;
    ct_win *parent;
    bool undecorated;
    int strut;
    ct_state initial[4];
    int n_initial;

    struct wl_surface *surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct zwlr_layer_surface_v1 *layer;
    /* the size the window manager asked for, 0 for the client's own */
    int conf_w, conf_h;
    /* drawn and shown */
    bool shown, waiting;
    int w, h;
    bool destroyed, closed, clicked;
    int sync_requests;

    /* what the window manager said in the last refresh */
    bool listed;
    uint32_t states;
    int desktop, stack;
    char *title;
    ct_rect frame, client;

    ct_win *next;
};

static struct wl_display *dpy;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct xdg_wm_base *wm_base;
static struct zxdg_decoration_manager_v1 *decorations;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct xdg_activation_v1 *activation;
static struct zwp_virtual_keyboard_manager_v1 *vkbd_manager;
static struct zwlr_virtual_pointer_manager_v1 *vptr_manager;
static struct zwp_virtual_keyboard_v1 *vkbd;
static struct zwlr_virtual_pointer_v1 *vptr;
static struct lumen_state_v1 *state;
static struct xkb_context *xkb;
static struct xkb_keymap *keymap;
static struct xkb_state *xkb_state;

static ct_win *windows;
/* the pointer: where the driver put it, and what it is over */
static int ptr_x, ptr_y;
static ct_win *under;
static uint32_t press_serial;

/* the last refresh */
static ct_rect screen, workarea;
static int n_desktops = 1, desktop;
static bool nothing_focused;
static int popups;
static ct_win *last;

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static ct_win *window_of(struct wl_surface *s)
{
    for (ct_win *w = windows; w; w = w->next)
        if (s && w->surface == s) return w;
    return NULL;
}

/* drawing */

static void buffer_release(void *data, struct wl_buffer *b)
{
    wl_buffer_destroy(b);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void draw(ct_win *w, int width, int height)
{
    int stride = width * 4, size = stride * height;
    int fd = memfd_create("lumen-contract", MFD_CLOEXEC);
    struct wl_shm_pool *pool;
    struct wl_buffer *b;
    uint32_t *px;

    if (fd < 0 || ftruncate(fd, size) < 0) {
        perror("lumen contract: shm");
        exit(2);
    }
    px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < width * height; ++i) px[i] = 0xff4c6a92;
    munmap(px, size);
    pool = wl_shm_create_pool(shm, fd, size);
    b = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                  WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b, &buffer_listener, NULL);

    wl_surface_attach(w->surface, b, 0, 0);
    wl_surface_damage_buffer(w->surface, 0, 0, width, height);
    wl_surface_commit(w->surface);
    w->w = width;
    w->h = height;
    w->shown = true;
}

/* what the window manager sends a client */

static void wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
    xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_configure(void *data, struct xdg_surface *x, uint32_t serial)
{
    ct_win *w = data;

    xdg_surface_ack_configure(x, serial);
    if (!w->waiting && !w->shown) return;
    w->waiting = false;
    draw(w, w->conf_w > 0 ? w->conf_w : w->want.w,
         w->conf_h > 0 ? w->conf_h : w->want.h);
}

static const struct xdg_surface_listener xdg_listener = {
    .configure = xdg_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    ct_win *w = data;

    w->conf_w = width;
    w->conf_h = height;
    /* xdg-shell resizes are always synced: the window manager waits for
       the client to draw each size it asks for */
    if (width > 0 && height > 0) ++w->sync_requests;
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
    ct_win *w = data;

    /* as the X11 driver's client does with WM_DELETE_WINDOW */
    ct_window_destroy(w);
    w->closed = true;
}

static void toplevel_bounds(void *data, struct xdg_toplevel *t,
                            int32_t width, int32_t height)
{
}

static void toplevel_caps(void *data, struct xdg_toplevel *t,
                          struct wl_array *caps)
{
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_caps,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *l,
                            uint32_t serial, uint32_t width, uint32_t height)
{
    ct_win *w = data;

    zwlr_layer_surface_v1_ack_configure(l, serial);
    w->conf_w = width;
    w->conf_h = height;
    if (!w->waiting && !w->shown) return;
    w->waiting = false;
    draw(w, width ? (int)width : w->want.w, height ? (int)height : w->want.h);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *l)
{
    ct_window_destroy(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

/* the pointer, to see clicks and press serials */

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
    under = window_of(s);
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *s)
{
    under = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
                           wl_fixed_t x, wl_fixed_t y)
{
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t st)
{
    if (st != WL_POINTER_BUTTON_STATE_PRESSED) return;
    press_serial = serial;
    if (under) under->clicked = true;
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{
}

static void pointer_frame(void *data, struct wl_pointer *p) {}
static void pointer_axis_source(void *data, struct wl_pointer *p,
                                uint32_t source) {}
static void pointer_axis_stop(void *data, struct wl_pointer *p,
                              uint32_t time, uint32_t axis) {}
static void pointer_axis_discrete(void *data, struct wl_pointer *p,
                                  uint32_t axis, int32_t discrete) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void seat_caps(void *data, struct wl_seat *s, uint32_t caps)
{
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}

static void seat_name(void *data, struct wl_seat *s, const char *name) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps,
    .name = seat_name,
};

/* what lumen_state_v1 says */

static void st_screen(void *data, struct lumen_state_v1 *s, int32_t x,
                      int32_t y, int32_t w, int32_t h)
{
    screen = (ct_rect){ x, y, w, h };
}

static void st_workarea(void *data, struct lumen_state_v1 *s, int32_t x,
                        int32_t y, int32_t w, int32_t h)
{
    workarea = (ct_rect){ x, y, w, h };
}

static void st_desktops(void *data, struct lumen_state_v1 *s, uint32_t count,
                        uint32_t current)
{
    n_desktops = count;
    desktop = current;
}

static void st_focus(void *data, struct lumen_state_v1 *s, uint32_t nothing)
{
    nothing_focused = nothing;
}

static void st_popups(void *data, struct lumen_state_v1 *s, uint32_t count)
{
    popups = count;
}

static void st_window(void *data, struct lumen_state_v1 *s,
                      struct wl_surface *surface, uint32_t states,
                      int32_t d, int32_t stack, const char *title)
{
    ct_win *w = window_of(surface);

    last = w;
    if (!w) return;
    w->listed = true;
    w->states = states;
    w->desktop = d;
    w->stack = stack;
    free(w->title);
    w->title = title ? strdup(title) : NULL;
}

static void st_frame(void *data, struct lumen_state_v1 *s, int32_t x,
                     int32_t y, int32_t w, int32_t h)
{
    if (last) last->frame = (ct_rect){ x, y, w, h };
}

static void st_client(void *data, struct lumen_state_v1 *s, int32_t x,
                      int32_t y, int32_t w, int32_t h)
{
    if (last) last->client = (ct_rect){ x, y, w, h };
}

static void st_done(void *data, struct lumen_state_v1 *s)
{
    last = NULL;
}

static const struct lumen_state_v1_listener state_listener = {
    .screen = st_screen,
    .workarea = st_workarea,
    .desktops = st_desktops,
    .focus = st_focus,
    .popups = st_popups,
    .window = st_window,
    .frame = st_frame,
    .client = st_client,
    .done = st_done,
};

/*! Ask the window manager what it did, and wait for the answer. */
static void refresh(void)
{
    for (ct_win *w = windows; w; w = w->next) w->listed = false;
    lumen_state_v1_refresh(state);
    wl_display_roundtrip(dpy);
}

/* connecting */

static void global_add(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version)
{
#define BIND(var, interface, v) \
    if (!strcmp(iface, interface.name)) \
        var = wl_registry_bind(r, name, &interface, \
                               version < (v) ? version : (v))
    BIND(compositor, wl_compositor_interface, 4);
    BIND(shm, wl_shm_interface, 1);
    BIND(seat, wl_seat_interface, 5);
    BIND(wm_base, xdg_wm_base_interface, 5);
    BIND(decorations, zxdg_decoration_manager_v1_interface, 1);
    BIND(layer_shell, zwlr_layer_shell_v1_interface, 3);
    BIND(activation, xdg_activation_v1_interface, 1);
    BIND(vkbd_manager, zwp_virtual_keyboard_manager_v1_interface, 1);
    BIND(vptr_manager, zwlr_virtual_pointer_manager_v1_interface, 1);
    BIND(state, lumen_state_v1_interface, 1);
#undef BIND
}

static void global_remove(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
    .global = global_add,
    .global_remove = global_remove,
};

/*! Give the virtual keyboard a keymap of its own, as every virtual keyboard
  does; the window manager reads the keys through their keysyms. */
static bool keyboard_open(void)
{
    struct xkb_rule_names names = { .layout = "us" };
    char *map;
    size_t size;
    int fd;

    xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    keymap = xkb ? xkb_keymap_new_from_names(xkb, &names,
                                             XKB_KEYMAP_COMPILE_NO_FLAGS)
        : NULL;
    if (!keymap) return false;
    xkb_state = xkb_state_new(keymap);
    map = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    size = strlen(map) + 1;
    fd = memfd_create("lumen-contract-keymap", MFD_CLOEXEC);
    if (fd < 0 || write(fd, map, size) != (ssize_t)size) {
        free(map);
        return false;
    }
    free(map);
    vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
        vkbd_manager, seat);
    zwp_virtual_keyboard_v1_keymap(vkbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                   fd, size);
    close(fd);
    return true;
}

bool ct_driver_open(void)
{
    dpy = wl_display_connect(NULL);
    if (!dpy) return false;
    registry = wl_display_get_registry(dpy);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!compositor || !shm || !wm_base || !state) {
        if (!state)
            fprintf(stderr, "# no lumen_state_v1: is Lumen running with "
                    "LUMEN_TEST_PROTOCOL=1?\n");
        wl_display_disconnect(dpy);
        dpy = NULL;
        return false;
    }
    xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    lumen_state_v1_add_listener(state, &state_listener, NULL);
    if (seat) {
        wl_seat_add_listener(seat, &seat_listener, NULL);
        if (vptr_manager)
            vptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
                vptr_manager, seat);
        if (vkbd_manager && !keyboard_open()) vkbd = NULL;
    }
    wl_display_roundtrip(dpy);
    refresh();
    return true;
}

void ct_driver_close(void)
{
    if (!dpy) return;
    while (windows) {
        ct_win *w = windows;
        windows = w->next;
        ct_window_destroy(w);
        free(w->title);
        free(w);
    }
    wl_display_roundtrip(dpy);
    wl_display_disconnect(dpy);
    dpy = NULL;
}

const char *ct_driver_name(void)
{
    return "wayland";
}

ct_caps ct_driver_caps(void)
{
    ct_caps caps = CT_CAP_DESKTOPS | CT_CAP_STACKING | CT_CAP_SYNC_RESIZE;

    if (layer_shell) caps |= CT_CAP_STRUTS;
    if (vkbd && vptr && pointer) caps |= CT_CAP_INPUT;
    return caps;
}

void ct_pump(void)
{
    wl_display_roundtrip(dpy);
}

/* making windows */

ct_win *ct_window_new(ct_win_type type, const char *title, ct_rect want)
{
    ct_win *w = calloc(1, sizeof(*w));

    w->type = type;
    w->want = want;
    w->surface = wl_compositor_create_surface(compositor);
    if (type == CT_WIN_DOCK || type == CT_WIN_POPUP) {
        /* a panel, or something that places itself over everything */
        w->layer = zwlr_layer_shell_v1_get_layer_surface(
            layer_shell, w->surface, NULL,
            type == CT_WIN_DOCK ? ZWLR_LAYER_SHELL_V1_LAYER_TOP :
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
            type == CT_WIN_DOCK ? "dock" : "popup");
        zwlr_layer_surface_v1_add_listener(w->layer, &layer_listener, w);
        zwlr_layer_surface_v1_set_size(w->layer, want.w, want.h);
        if (type == CT_WIN_DOCK)
            zwlr_layer_surface_v1_set_anchor(w->layer,
                ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    } else {
        w->xdg = xdg_wm_base_get_xdg_surface(wm_base, w->surface);
        xdg_surface_add_listener(w->xdg, &xdg_listener, w);
        w->toplevel = xdg_surface_get_toplevel(w->xdg);
        xdg_toplevel_add_listener(w->toplevel, &toplevel_listener, w);
        if (title) xdg_toplevel_set_title(w->toplevel, title);
    }
    w->next = windows;
    windows = w;
    return w;
}

void ct_window_set_min_size(ct_win *w, int minw, int minh)
{
    w->minw = minw;
    w->minh = minh;
    if (!w->toplevel) return;
    xdg_toplevel_set_min_size(w->toplevel, minw, minh);
    /* it is part of the next commit; a client shows it with the size it
       draws next */
    if (w->shown) draw(w, w->w, w->h);
}

void ct_window_set_parent(ct_win *w, ct_win *parent)
{
    w->parent = parent;
    if (w->toplevel && parent && parent->toplevel)
        xdg_toplevel_set_parent(w->toplevel, parent->toplevel);
}

void ct_window_set_title(ct_win *w, const char *title)
{
    if (w->toplevel) xdg_toplevel_set_title(w->toplevel, title);
    wl_display_flush(dpy);
}

void ct_window_set_app_id(ct_win *w, const char *app_id)
{
    if (w->toplevel) xdg_toplevel_set_app_id(w->toplevel, app_id);
}

void ct_window_set_undecorated(ct_win *w)
{
    /* a client that draws its own decorations does not ask for any */
    w->undecorated = true;
}

void ct_window_set_strut(ct_win *w, int top)
{
    w->strut = top;
    if (w->layer) zwlr_layer_surface_v1_set_exclusive_zone(w->layer, top);
}

void ct_window_set_initial_state(ct_win *w, ct_state s)
{
    if (w->n_initial < 4) w->initial[w->n_initial++] = s;
}

void ct_window_set_initial_desktop(ct_win *w, int d)
{
    /* xdg-shell has no way to ask; the case does not run (no
       CT_CAP_CLIENT_DESKTOP) */
    (void)w; (void)d;
}

void ct_window_set_sync(ct_win *w)
{
    /* every xdg-shell client syncs its resizes */
    (void)w;
}

static void activate(ct_win *w);

void ct_window_map(ct_win *w)
{
    if (w->destroyed) return;
    if (w->shown) {
        /* shown already: it asks to come back, as mapping an iconic window
           does on X11 */
        activate(w);
        return;
    }
    if (w->toplevel) {
        if (decorations && !w->undecorated && !w->decoration) {
            w->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
                decorations, w->toplevel);
            zxdg_toplevel_decoration_v1_set_mode(w->decoration,
                ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
        for (int i = 0; i < w->n_initial; ++i)
            switch (w->initial[i]) {
            case CT_STATE_MAXIMIZED:
                xdg_toplevel_set_maximized(w->toplevel); break;
            case CT_STATE_FULLSCREEN:
                xdg_toplevel_set_fullscreen(w->toplevel, NULL); break;
            case CT_STATE_ICONIC:
                xdg_toplevel_set_minimized(w->toplevel); break;
            case CT_STATE_ABOVE:
                break;
            }
    }
    /* the first commit has no buffer; it draws when it is configured */
    w->waiting = true;
    wl_surface_commit(w->surface);
    wl_display_flush(dpy);
}

void ct_window_take_focus(ct_win *w)
{
    (void)w;
}

void ct_window_withdraw(ct_win *w)
{
    if (w->destroyed || !w->shown) return;
    wl_surface_attach(w->surface, NULL, 0, 0);
    wl_surface_commit(w->surface);
    w->shown = false;
    w->waiting = false;
    w->conf_w = w->conf_h = 0;
    wl_display_flush(dpy);
}

void ct_window_destroy(ct_win *w)
{
    if (w->destroyed) return;
    if (under == w) under = NULL;
    if (w->decoration) zxdg_toplevel_decoration_v1_destroy(w->decoration);
    if (w->toplevel) xdg_toplevel_destroy(w->toplevel);
    if (w->xdg) xdg_surface_destroy(w->xdg);
    if (w->layer) zwlr_layer_surface_v1_destroy(w->layer);
    wl_surface_destroy(w->surface);
    w->decoration = NULL;
    w->toplevel = NULL;
    w->xdg = NULL;
    w->layer = NULL;
    w->surface = NULL;
    w->destroyed = true;
    w->shown = false;
    w->listed = false;
    wl_display_flush(dpy);
}

/* what the window manager did */

bool ct_window_managed(ct_win *w)
{
    refresh();
    return w->listed;
}

bool ct_window_viewable(ct_win *w)
{
    if (!w->toplevel) return w->shown;
    refresh();
    return w->listed && (w->states & LUMEN_STATE_V1_STATE_VIEWABLE);
}

bool ct_window_focused(ct_win *w)
{
    refresh();
    return w->listed && (w->states & LUMEN_STATE_V1_STATE_FOCUSED);
}

bool ct_nothing_focused(void)
{
    refresh();
    return nothing_focused;
}

int ct_popup_count(void)
{
    refresh();
    return popups;
}

bool ct_wm_announced(void)
{
    /* it is the compositor, and says what it supports by its globals */
    return dpy && wm_base && state;
}

static uint32_t state_bit(ct_state s)
{
    switch (s) {
    case CT_STATE_MAXIMIZED: return LUMEN_STATE_V1_STATE_MAXIMIZED;
    case CT_STATE_FULLSCREEN: return LUMEN_STATE_V1_STATE_FULLSCREEN;
    case CT_STATE_ICONIC: return LUMEN_STATE_V1_STATE_ICONIC;
    case CT_STATE_ABOVE: return LUMEN_STATE_V1_STATE_ABOVE;
    }
    return 0;
}

bool ct_window_has_state(ct_win *w, ct_state s)
{
    refresh();
    return w->listed && (w->states & state_bit(s));
}

ct_rect ct_window_client(ct_win *w)
{
    refresh();
    if (!w->toplevel && w->shown)
        return (ct_rect){ 0, 0, w->w, w->h };
    return w->client;
}

ct_rect ct_window_frame(ct_win *w)
{
    refresh();
    if (!w->toplevel && w->shown)
        return (ct_rect){ 0, 0, w->w, w->h };
    return w->frame;
}

bool ct_window_title_is(ct_win *w, const char *title)
{
    refresh();
    return w->listed && w->title && !strcmp(w->title, title);
}

int ct_window_desktop(ct_win *w)
{
    refresh();
    return w->desktop;
}

int ct_window_stack_index(ct_win *w)
{
    refresh();
    return w->listed ? w->stack : -1;
}

bool ct_window_closed(ct_win *w)
{
    return w->closed;
}

int ct_window_sync_requests(ct_win *w)
{
    return w->sync_requests;
}

void ct_window_set_opacity(ct_win *w, double opacity)
{
    (void)w; (void)opacity;
}

double ct_window_shown_opacity(ct_win *w)
{
    (void)w;
    return -1;
}

bool ct_window_clicked(ct_win *w)
{
    ct_pump();
    return w->clicked;
}

/* requests */

static void set_state(ct_win *w, ct_state s, uint32_t action)
{
    if (!w->surface) return;
    lumen_state_v1_set_state(state, w->surface, state_bit(s), action);
    wl_display_flush(dpy);
}

void ct_request_state(ct_win *w, ct_state s, bool on)
{
    /* what xdg-shell lets the client ask for itself; the rest as a pager */
    if (w->toplevel && s == CT_STATE_MAXIMIZED) {
        if (on) xdg_toplevel_set_maximized(w->toplevel);
        else xdg_toplevel_unset_maximized(w->toplevel);
    } else if (w->toplevel && s == CT_STATE_FULLSCREEN) {
        if (on) xdg_toplevel_set_fullscreen(w->toplevel, NULL);
        else xdg_toplevel_unset_fullscreen(w->toplevel);
    } else if (w->toplevel && s == CT_STATE_ICONIC && on)
        xdg_toplevel_set_minimized(w->toplevel);
    else {
        set_state(w, s, on ? LUMEN_STATE_V1_ACTION_ADD :
                  LUMEN_STATE_V1_ACTION_REMOVE);
        return;
    }
    wl_display_flush(dpy);
}

void ct_request_toggle_state(ct_win *w, ct_state s)
{
    set_state(w, s, LUMEN_STATE_V1_ACTION_TOGGLE);
}

void ct_request_activate(ct_win *w)
{
    if (!w->surface) return;
    lumen_state_v1_activate(state, w->surface);
    wl_display_flush(dpy);
}

void ct_request_close(ct_win *w)
{
    if (!w->surface) return;
    lumen_state_v1_close(state, w->surface);
    wl_display_flush(dpy);
}

void ct_request_move_resize(ct_win *w, ct_rect r)
{
    /* a client cannot place itself; it resizes by drawing a new size */
    if (!w->shown) return;
    draw(w, r.w, r.h);
    wl_display_flush(dpy);
}

void ct_request_keyboard_move(ct_win *w)
{
    if (!w->surface) return;
    lumen_state_v1_keyboard_move(state, w->surface);
    wl_display_flush(dpy);
}

void ct_request_lower(ct_win *w)
{
    if (!w->surface) return;
    lumen_state_v1_lower(state, w->surface);
    wl_display_flush(dpy);
}

void ct_pager_raise(ct_win *w)
{
    if (!w->surface) return;
    lumen_state_v1_raise(state, w->surface);
    wl_display_flush(dpy);
}

void ct_pager_move_resize(ct_win *w, ct_rect r)
{
    if (!w->surface) return;
    lumen_state_v1_move_resize(state, w->surface, r.x, r.y, r.w, r.h);
    wl_display_flush(dpy);
}

void ct_request_desktop(int d)
{
    lumen_state_v1_desktop(state, d);
    wl_display_flush(dpy);
}

void ct_request_window_desktop(ct_win *w, int d)
{
    if (!w->surface) return;
    lumen_state_v1_window_desktop(state, w->surface, d);
    wl_display_flush(dpy);
}

/* xdg-activation: the client asks to be brought forward */

static void token_done(void *data, struct xdg_activation_token_v1 *t,
                       const char *token)
{
    ct_win *w = data;

    if (w->surface) xdg_activation_v1_activate(activation, token, w->surface);
    xdg_activation_token_v1_destroy(t);
}

static const struct xdg_activation_token_v1_listener token_listener = {
    .done = token_done,
};

static void activate(ct_win *w)
{
    struct xdg_activation_token_v1 *t;

    if (!activation || !w->surface) return;
    t = xdg_activation_v1_get_activation_token(activation);
    xdg_activation_token_v1_add_listener(t, &token_listener, w);
    xdg_activation_token_v1_set_surface(t, w->surface);
    xdg_activation_token_v1_commit(t);
    wl_display_roundtrip(dpy);
}

ct_rect ct_screen(void)
{
    refresh();
    return screen;
}

ct_rect ct_workarea(void)
{
    refresh();
    return workarea;
}

int ct_desktop_count(void)
{
    refresh();
    return n_desktops;
}

int ct_current_desktop(void)
{
    refresh();
    return desktop;
}

/* input */

/*! Let the window manager handle what was sent, and the client hear it. */
static void input_sync(void)
{
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
}

static xkb_keycode_t keycode_of(const char *name, bool *shift)
{
    xkb_keysym_t sym = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
    xkb_keycode_t min = xkb_keymap_min_keycode(keymap);
    xkb_keycode_t max = xkb_keymap_max_keycode(keymap);

    if (sym == XKB_KEY_NoSymbol)
        sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
    for (int level = 0; level < 2; ++level)
        for (xkb_keycode_t k = min; k <= max; ++k) {
            const xkb_keysym_t *syms;
            int n = xkb_keymap_key_get_syms_by_level(keymap, k, 0, level,
                                                     &syms);
            for (int i = 0; i < n; ++i)
                if (syms[i] == sym) {
                    *shift = level == 1;
                    return k;
                }
        }
    fprintf(stderr, "# no key for %s\n", name);
    return 0;
}

static void key(xkb_keycode_t k, bool press)
{
    if (!k) return;
    /* evdev codes, 8 below xkb's */
    zwp_virtual_keyboard_v1_key(vkbd, now_ms(), k - 8,
                                press ? WL_KEYBOARD_KEY_STATE_PRESSED :
                                WL_KEYBOARD_KEY_STATE_RELEASED);
    /* a virtual keyboard keeps its modifiers itself, and says when they
       change */
    if (xkb_state_update_key(xkb_state, k, press ? XKB_KEY_DOWN : XKB_KEY_UP))
        zwp_virtual_keyboard_v1_modifiers(vkbd,
            xkb_state_serialize_mods(xkb_state, XKB_STATE_MODS_DEPRESSED),
            xkb_state_serialize_mods(xkb_state, XKB_STATE_MODS_LATCHED),
            xkb_state_serialize_mods(xkb_state, XKB_STATE_MODS_LOCKED),
            xkb_state_serialize_layout(xkb_state, XKB_STATE_LAYOUT_EFFECTIVE));
    input_sync();
}

static xkb_keycode_t mod_key(char m)
{
    bool shift;
    const char *name = m == 'C' ? "Control_L" : m == 'A' ? "Alt_L" :
        m == 'S' ? "Shift_L" : m == 'W' ? "Super_L" : NULL;

    return name ? keycode_of(name, &shift) : 0;
}

static void mods(const char *m, size_t n, bool press)
{
    if (press)
        for (size_t i = 0; i < n; ++i) key(mod_key(m[i]), true);
    else
        for (size_t i = n; i > 0; --i) key(mod_key(m[i - 1]), false);
}

static const char *split_keys(const char *keys, char *m, size_t *n)
{
    *n = 0;
    while (keys[0] && keys[1] == '-' && *n < 7) {
        m[(*n)++] = keys[0];
        keys += 2;
    }
    return keys;
}

void ct_input_keys(const char *keys)
{
    char m[8];
    size_t n;
    bool shift = false;
    const char *name = split_keys(keys, m, &n);
    xkb_keycode_t k;

    if (!vkbd) return;
    k = keycode_of(name, &shift);
    mods(m, n, true);
    if (shift) key(mod_key('S'), true);
    key(k, true);
    key(k, false);
    if (shift) key(mod_key('S'), false);
    mods(m, n, false);
}

static void move_to(int x, int y)
{
    ct_rect s = screen;

    ptr_x = x;
    ptr_y = y;
    if (s.w <= 0 || s.h <= 0) s = ct_screen();
    zwlr_virtual_pointer_v1_motion_absolute(vptr, now_ms(), x, y, s.w, s.h);
    zwlr_virtual_pointer_v1_frame(vptr);
    input_sync();
}

static void button(int b, bool press)
{
    static const uint32_t codes[] = { 0, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT };

    zwlr_virtual_pointer_v1_button(vptr, now_ms(),
                                   b > 0 && b < 4 ? codes[b] : BTN_LEFT,
                                   press ? WL_POINTER_BUTTON_STATE_PRESSED :
                                   WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(vptr);
    input_sync();
}

void ct_input_drag(ct_win *w, const char *m, int dx, int dy)
{
    ct_rect c = ct_window_client(w);
    int x = c.x + c.w / 2, y = c.y + c.h / 2;
    const int steps = 10;

    if (!vptr) return;
    move_to(x, y);
    mods(m, strlen(m), true);
    button(1, true);
    for (int i = 1; i <= steps; ++i)
        move_to(x + dx * i / steps, y + dy * i / steps);
    button(1, false);
    mods(m, strlen(m), false);
}

void ct_input_client_drag(ct_win *w, ct_edge edge, int dx, int dy)
{
    ct_rect c = ct_window_client(w);
    int x = c.x + c.w / 2, y = c.y + c.h / 2;
    const int steps = 10;

    if (!vptr || !w->toplevel) return;
    move_to(x, y);
    button(1, true);
    /* a client-side title bar or border asks with the press's serial */
    if (edge == CT_EDGE_MOVE)
        xdg_toplevel_move(w->toplevel, seat, press_serial);
    else
        xdg_toplevel_resize(w->toplevel, seat, press_serial,
                            XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
    input_sync();
    for (int i = 1; i <= steps; ++i)
        move_to(x + dx * i / steps, y + dy * i / steps);
    button(1, false);
}

void ct_input_point(ct_win *w)
{
    ct_rect c = w ? ct_window_client(w) : ct_screen();
    const int steps = 5, x = ptr_x, y = ptr_y;

    if (!vptr) return;
    /* glide there, so it crosses what is on the way */
    for (int i = 1; i <= steps; ++i)
        move_to(x + (c.x + c.w / 2 - x) * i / steps,
                y + (c.y + c.h / 2 - y) * i / steps);
}

void ct_input_click(ct_win *w, int x, int y)
{
    ct_rect c = ct_window_client(w);

    if (!vptr) return;
    move_to(c.x + x, c.y + y);
    button(1, true);
    button(1, false);
}

void ct_input_title_click(ct_win *w, int count)
{
    ct_rect f = ct_window_frame(w), c = ct_window_client(w);

    if (!vptr) return;
    move_to(f.x + f.w / 2, f.y + (c.y - f.y) / 2);
    for (int i = 0; i < count; ++i) {
        button(1, true);
        button(1, false);
    }
}

void ct_input_root_click(int b)
{
    ct_rect s = ct_screen();

    if (!vptr) return;
    /* the middle of the screen, where no window is */
    move_to(s.x + s.w / 2, s.y + s.h / 2);
    button(b, true);
    button(b, false);
}
