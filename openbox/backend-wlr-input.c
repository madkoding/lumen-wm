/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend-wlr-input.c for the Lumen window manager

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

/* The seat: keyboards, the pointer, and which of their events go to the
   core and which to clients. As a compositor sees every key and click, the
   X11 grabs become rules here:

   - a key goes to the core while it holds the keyboard, when it is bound,
     when it is released after its press went to the core, or when no
     client has the keyboard; otherwise to the focused client.
   - a press goes to the core while it holds the pointer, when a button
     binding covers what is under the pointer, or when it lands on a canvas
     that takes presses; the core then has the pointer until every button
     is up (X's implicit grab). Otherwise the client under the pointer gets
     it. A "hold" binding on a client keeps the press until the core says
     whether the client gets it after all (pass_click). */

#include "backend-wlr-int.h"

#include <linux/input-event-codes.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>

struct wlr_seat *lw_seat = NULL;

static struct wlr_cursor *cursor;
static struct wlr_xcursor_manager *xcursors;
static struct wl_listener new_input, request_cursor, request_selection;
static struct wl_listener motion, motion_absolute, button, axis, frame;
static GList *keyboards;
static struct xkb_keymap *keymap;

typedef struct {
    struct wlr_keyboard *wlr;
    struct wl_listener key, modifiers, destroy;
} Keyboard;

typedef struct {
    guint keycode, mods;
} KeyBinding;

typedef struct {
    guintptr over;
    guint button, mods;
    gboolean hold;
    ObCursor cursor;
} ButtonBinding;

static GArray *key_bindings;
static GList *button_bindings;
/* keys whose press went to the core, so their release does too */
static GArray *core_keys;

/* the core holds the keyboard / the pointer (grab_keyboard, grab_pointer) */
static gboolean kbd_grabbed;
static gboolean ptr_grabbed, ptr_owner_events;
static ObCursor grab_cursor;

/* a press went to the core: it gets the pointer until the buttons are up */
static guint buttons_down;      /* bit per X button */
static gboolean implicit;       /* the core has the buttons */
static guintptr implicit_over;
static gboolean implicit_desktop;
/* a press held for pass_click */
static gboolean held;
static struct wlr_pointer_button_event held_press;
static struct wlr_surface *held_surface;

/* what the pointer is over, for crossings */
static guintptr crossing_over;
/* motion the core caused itself (pointer_warp) */
static gboolean swallow_motion;

static void deliver(ObInputEvent *e)
{
    lw_begin(e->time);
    lw_events->input(e);
    lw_end();
}

static guint mods_now(void)
{
    struct wlr_keyboard *k = wlr_seat_get_keyboard(lw_seat);
    guint m = k ? wlr_keyboard_get_modifiers(k) : 0;
    guint i;

    /* X's button masks too, for drags */
    for (i = 1; i <= 5; ++i)
        if (buttons_down & (1 << i)) m |= 1 << (7 + i);
    return m;
}

/* keyboards */

guint lw_modkey_mask(ObModkey key)
{
    /* xkb's default keymaps put the modifier keys where X does */
    switch (key) {
    case OB_MODKEY_SHIFT: return OB_MOD_SHIFT;
    case OB_MODKEY_CONTROL: return OB_MOD_CONTROL;
    case OB_MODKEY_ALT:
    case OB_MODKEY_META: return OB_MOD_MOD1;
    case OB_MODKEY_SUPER:
    case OB_MODKEY_HYPER: return OB_MOD_MOD4;
    }
    return 0;
}

guint lw_only_modmasks(guint state)
{
    /* no Caps Lock or Num Lock (Mod2), and no buttons */
    return state & (OB_MOD_SHIFT | OB_MOD_CONTROL | OB_MOD_MOD1 |
                    OB_MOD_MOD3 | OB_MOD_MOD4 | OB_MOD_MOD5);
}

static guint key_mod(xkb_keysym_t sym)
{
    switch (sym) {
    case XKB_KEY_Shift_L: case XKB_KEY_Shift_R: return OB_MOD_SHIFT;
    case XKB_KEY_Control_L: case XKB_KEY_Control_R: return OB_MOD_CONTROL;
    case XKB_KEY_Alt_L: case XKB_KEY_Alt_R:
    case XKB_KEY_Meta_L: case XKB_KEY_Meta_R: return OB_MOD_MOD1;
    case XKB_KEY_Super_L: case XKB_KEY_Super_R:
    case XKB_KEY_Hyper_L: case XKB_KEY_Hyper_R: return OB_MOD_MOD4;
    case XKB_KEY_Caps_Lock: return OB_MOD_LOCK;
    case XKB_KEY_Num_Lock: return OB_MOD_MOD2;
    }
    return 0;
}

static struct xkb_keymap *make_keymap(void)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    /* the XKB_DEFAULT_* environment variables pick the layout */
    struct xkb_keymap *map = xkb_keymap_new_from_names(
        ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

    xkb_context_unref(ctx);
    return map;
}

void lw_keymap_reload(void)
{
    struct xkb_keymap *map = make_keymap();
    GList *it;

    if (!map) return;
    if (keymap) xkb_keymap_unref(keymap);
    keymap = map;
    for (it = keyboards; it; it = g_list_next(it))
        wlr_keyboard_set_keymap(((Keyboard*)it->data)->wlr, keymap);
}

gboolean lw_key_named(const gchar *name, guint *keycode)
{
    xkb_keysym_t sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
    xkb_keycode_t k, min, max;

    if (sym == XKB_KEY_NoSymbol) return FALSE;
    *keycode = 0;
    if (!keymap) return TRUE;
    min = xkb_keymap_min_keycode(keymap);
    max = xkb_keymap_max_keycode(keymap);
    for (k = min; k <= max; ++k) {
        const xkb_keysym_t *syms;
        gint n = xkb_keymap_key_get_syms_by_level(keymap, k, 0, 0, &syms);
        gint i;

        for (i = 0; i < n; ++i)
            if (syms[i] == sym ||
                xkb_keysym_to_lower(syms[i]) == xkb_keysym_to_lower(sym))
            {
                *keycode = k;
                return TRUE;
            }
    }
    return TRUE;
}

void lw_bind_key(guint keycode, guint mods)
{
    KeyBinding b = { keycode, mods };

    g_array_append_val(key_bindings, b);
}

void lw_unbind_all_keys(void)
{
    g_array_set_size(key_bindings, 0);
}

static gboolean key_bound(guint keycode, guint mods)
{
    guint i;

    mods = lw_only_modmasks(mods);
    for (i = 0; i < key_bindings->len; ++i) {
        KeyBinding *b = &g_array_index(key_bindings, KeyBinding, i);
        if (b->keycode == keycode && b->mods == mods) return TRUE;
    }
    return FALSE;
}

static gboolean core_key_take(guint keycode, gboolean remove)
{
    guint i;

    for (i = 0; i < core_keys->len; ++i)
        if (g_array_index(core_keys, guint, i) == keycode) {
            if (remove) g_array_remove_index_fast(core_keys, i);
            return TRUE;
        }
    return FALSE;
}

static void keyboard_key(struct wl_listener *l, void *data)
{
    Keyboard *k = wl_container_of(l, k, key);
    const struct wlr_keyboard_key_event *ev = data;
    guint keycode = ev->keycode + 8;
    gboolean press = ev->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    guint mods = wlr_keyboard_get_modifiers(k->wlr);
    WlrSurface *focus = lw_focused_surface();
    gboolean to_core;

    if (press)
        to_core = kbd_grabbed || key_bound(keycode, mods) ||
            !focus || focus->internal;
    else
        to_core = core_key_take(keycode, TRUE) || kbd_grabbed;

    if (to_core) {
        ObInputEvent e;
        const xkb_keysym_t *syms;
        xkb_keysym_t sym = XKB_KEY_NoSymbol;

        if (press && !core_key_take(keycode, FALSE))
            g_array_append_val(core_keys, keycode);
        if (xkb_keymap_key_get_syms_by_level(k->wlr->keymap, keycode, 0, 0,
                                             &syms) > 0)
            sym = syms[0];

        memset(&e, 0, sizeof(e));
        e.type = press ? OB_INPUT_KEY_PRESS : OB_INPUT_KEY_RELEASE;
        e.time = ev->time_msec;
        e.mods = mods;
        e.keycode = keycode;
        e.keysym = sym;
        e.key_mod = key_mod(sym);
        if (press && kbd_grabbed)
            e.text = xkb_state_key_get_utf32(k->wlr->xkb_state, keycode);
        lw_pointer_position(&e.root_x, &e.root_y);
        e.over = (guintptr)(focus ? focus : NULL);
        deliver(&e);
        return;
    }
    wlr_seat_set_keyboard(lw_seat, k->wlr);
    wlr_seat_keyboard_notify_key(lw_seat, ev->time_msec, ev->keycode,
                                 ev->state);
}

static void keyboard_modifiers(struct wl_listener *l, void *data)
{
    Keyboard *k = wl_container_of(l, k, modifiers);

    wlr_seat_set_keyboard(lw_seat, k->wlr);
    wlr_seat_keyboard_notify_modifiers(lw_seat, &k->wlr->modifiers);
}

static void keyboard_destroy(struct wl_listener *l, void *data)
{
    Keyboard *k = wl_container_of(l, k, destroy);

    wl_list_remove(&k->key.link);
    wl_list_remove(&k->modifiers.link);
    wl_list_remove(&k->destroy.link);
    keyboards = g_list_remove(keyboards, k);
    g_free(k);
}

static void new_keyboard(struct wlr_input_device *dev)
{
    Keyboard *k = g_new0(Keyboard, 1);

    k->wlr = wlr_keyboard_from_input_device(dev);
    if (!keymap) keymap = make_keymap();
    wlr_keyboard_set_keymap(k->wlr, keymap);
    wlr_keyboard_set_repeat_info(k->wlr, 25, 600);
    k->key.notify = keyboard_key;
    wl_signal_add(&k->wlr->events.key, &k->key);
    k->modifiers.notify = keyboard_modifiers;
    wl_signal_add(&k->wlr->events.modifiers, &k->modifiers);
    k->destroy.notify = keyboard_destroy;
    wl_signal_add(&dev->events.destroy, &k->destroy);
    keyboards = g_list_append(keyboards, k);
    wlr_seat_set_keyboard(lw_seat, k->wlr);
}

gboolean lw_grab_keyboard(gboolean grab, guint32 time)
{
    kbd_grabbed = grab;
    if (!grab) g_array_set_size(core_keys, 0);
    return TRUE;
}

void lw_keyboard_focus(struct wlr_surface *s)
{
    struct wlr_keyboard *k = wlr_seat_get_keyboard(lw_seat);

    if (!s) {
        wlr_seat_keyboard_notify_clear_focus(lw_seat);
        return;
    }
    if (k)
        wlr_seat_keyboard_notify_enter(lw_seat, s, k->keycodes,
                                       k->num_keycodes, &k->modifiers);
    else
        wlr_seat_keyboard_notify_enter(lw_seat, s, NULL, 0, NULL);
}

/* what is under the pointer */

typedef struct {
    WlrCanvas *canvas;          /* the canvas hit, if any */
    WlrSurface *surface;        /* the client hit, if any */
    struct wlr_surface *wl;     /* the client's wl_surface hit */
    gdouble sx, sy;             /* where in wl */
} Hit;

static void owner_of(struct wlr_scene_node *n, Hit *hit)
{
    for (; n; n = n->parent ? &n->parent->node : NULL) {
        WlrNodeData *d = n->data;
        if (!d) continue;
        if (d->kind == WLR_NODE_CANVAS && !hit->canvas) {
            hit->canvas = (WlrCanvas*)d;
            return;
        }
        if (d->kind == WLR_NODE_SURFACE && !hit->surface) {
            hit->surface = (WlrSurface*)d;
            /* keep going: its frame is the canvas around it */
        }
    }
}

static void hit_test(Hit *hit)
{
    struct wlr_scene_node *n;
    gdouble nx, ny;

    memset(hit, 0, sizeof(*hit));
    n = wlr_scene_node_at(&lw_scene->tree.node, cursor->x, cursor->y,
                          &nx, &ny);
    if (!n) return;
    if (n->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_surface *ss =
            wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(n));
        if (ss) {
            hit->wl = ss->surface;
            hit->sx = nx;
            hit->sy = ny;
        }
    }
    owner_of(n, hit);
    if (hit->surface) {
        /* a client inside a frame: the frame is not what was hit */
        hit->canvas = NULL;
    }
    /* canvases can take input in only part of themselves */
    while (hit->canvas && !hit->surface) {
        gint x, y;
        lw_canvas_layout_pos(hit->canvas, &x, &y);
        if (lw_canvas_accepts(hit->canvas, cursor->x - x, cursor->y - y))
            break;
        hit->canvas = hit->canvas->parent;
    }
}

/*! The innermost canvas from c up that takes any of flags. */
static WlrCanvas *taker(WlrCanvas *c, ObCanvasFlags flags)
{
    for (; c; c = c->parent)
        if (c->flags & flags) return c;
    return NULL;
}

/*! The canvas holding a client: its decoration. */
static WlrCanvas *frame_of(const Hit *hit)
{
    if (hit->surface) return hit->surface->frame;
    return hit->canvas;
}

static guintptr hit_id(const Hit *hit)
{
    if (hit->surface)
        return hit->surface->dialog ? (guintptr)hit->surface->dialog :
            (guintptr)hit->surface;
    return (guintptr)hit->canvas;
}

static void event_at(ObInputEvent *e, guintptr over, gboolean desktop)
{
    WlrNodeData *d = (WlrNodeData*)over;

    e->root_x = cursor->x;
    e->root_y = cursor->y;
    e->over = over;
    e->desktop = desktop || !over;
    e->x = e->root_x;
    e->y = e->root_y;
    e->inside = TRUE;
    if (!over) return;
    if (d->kind == WLR_NODE_CANVAS) {
        WlrCanvas *c = (WlrCanvas*)over;
        gint x, y;

        lw_canvas_layout_pos(c, &x, &y);
        e->x -= x;
        e->y -= y;
        e->inside = e->x >= 0 && e->y >= 0 && e->x < c->width &&
            e->y < c->height;
    } else if (d->kind == WLR_NODE_SURFACE) {
        WlrSurface *s = (WlrSurface*)over;
        if (s->frame) {
            gint x, y;
            lw_canvas_layout_pos(s->frame, &x, &y);
            e->x -= x + s->x;
            e->y -= y + s->y;
            e->inside = e->x >= 0 && e->y >= 0 && e->x < s->width &&
                e->y < s->height;
        }
    }
}

static gboolean is_inside(guintptr outer, guintptr inner)
{
    WlrNodeData *d = (WlrNodeData*)inner;
    WlrCanvas *c;

    if (!outer || !inner) return FALSE;
    if (d->kind == WLR_NODE_SURFACE) {
        WlrSurface *s = (WlrSurface*)inner;
        if ((guintptr)s->frame == outer) return TRUE;
        c = s->frame;
    } else
        c = ((WlrCanvas*)inner)->parent;
    for (; c; c = c->parent)
        if ((guintptr)c == outer) return TRUE;
    return FALSE;
}

static void crossing(guintptr from, guintptr to, guint32 time,
                     ObCrossingMode mode)
{
    ObInputEvent e;

    if (from == to) return;
    if (from) {
        memset(&e, 0, sizeof(e));
        e.type = OB_INPUT_LEAVE;
        e.time = time;
        e.mods = mods_now();
        e.crossing = mode;
        e.inferior = is_inside(from, to);
        e.next_over = to;
        event_at(&e, from, FALSE);
        deliver(&e);
    }
    if (to) {
        memset(&e, 0, sizeof(e));
        e.type = OB_INPUT_ENTER;
        e.time = time;
        e.mods = mods_now();
        e.crossing = mode;
        e.inferior = is_inside(to, from);
        event_at(&e, to, FALSE);
        deliver(&e);
    }
}

static void show_cursor(ObCursor c);
static ObCursor desktop_cursor;

static void set_client_pointer(const Hit *hit, guint32 time)
{
    if (hit->wl) {
        wlr_seat_pointer_notify_enter(lw_seat, hit->wl, hit->sx, hit->sy);
        wlr_seat_pointer_notify_motion(lw_seat, time, hit->sx, hit->sy);
    } else {
        wlr_seat_pointer_notify_clear_focus(lw_seat);
    }
}

static void pointer_moved(guint32 time)
{
    Hit hit;
    guintptr over;
    WlrCanvas *c;

    if (!time) time = lw_now();
    hit_test(&hit);

    if (ptr_grabbed || implicit) {
        ObInputEvent e;
        guintptr to;

        memset(&e, 0, sizeof(e));
        e.type = OB_INPUT_MOTION;
        e.time = time;
        e.mods = mods_now();
        if (implicit)
            to = implicit_over;
        else if (ptr_owner_events && hit.canvas && !hit.surface)
            to = (guintptr)hit.canvas;
        else
            to = 0;
        /* menus see the pointer come and go while the core holds it */
        if (ptr_grabbed && ptr_owner_events) {
            guintptr x = hit.surface ? 0 :
                (guintptr)taker(hit.canvas, OB_CANVAS_INPUT_CROSSING);
            crossing(crossing_over, x, time, OB_CROSSING_NORMAL);
            crossing_over = x;
        }
        event_at(&e, to, implicit ? implicit_desktop : !to);
        deliver(&e);
        return;
    }

    /* crossings: canvases that ask, and clients */
    over = hit.surface ? hit_id(&hit) :
        (guintptr)taker(hit.canvas, OB_CANVAS_INPUT_CROSSING);
    if (!over && hit.canvas)
        over = (guintptr)hit.canvas;
    crossing(crossing_over, over, time, OB_CROSSING_NORMAL);
    crossing_over = over;

    if (hit.surface) {
        set_client_pointer(&hit, time);
        return;
    }
    wlr_seat_pointer_notify_clear_focus(lw_seat);
    c = taker(hit.canvas, OB_CANVAS_INPUT_MOTION);
    show_cursor(hit.canvas ? hit.canvas->cursor : desktop_cursor);
    if (c) {
        ObInputEvent e;

        memset(&e, 0, sizeof(e));
        e.type = OB_INPUT_MOTION;
        e.time = time;
        e.mods = mods_now();
        event_at(&e, (guintptr)c, FALSE);
        deliver(&e);
    }
}

void lw_pointer_refresh(guint32 time)
{
    if (!cursor || swallow_motion) return;
    /* only where nothing holds the pointer: the core's moves and
       restacks change what is under it */
    if (ptr_grabbed || implicit) return;
    pointer_moved(time);
}

static void cursor_motion(struct wl_listener *l, void *data)
{
    struct wlr_pointer_motion_event *e = data;

    wlr_cursor_move(cursor, &e->pointer->base, e->delta_x, e->delta_y);
    pointer_moved(e->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *l, void *data)
{
    struct wlr_pointer_motion_absolute_event *e = data;

    wlr_cursor_warp_absolute(cursor, &e->pointer->base, e->x, e->y);
    pointer_moved(e->time_msec);
}

static guint x_button(guint32 b)
{
    switch (b) {
    case BTN_LEFT: return 1;
    case BTN_MIDDLE: return 2;
    case BTN_RIGHT: return 3;
    case BTN_SIDE: return 8;
    case BTN_EXTRA: return 9;
    }
    return 0;
}

static ButtonBinding *button_bound(guintptr over, guint button, guint mods)
{
    GList *it;

    mods = lw_only_modmasks(mods);
    for (it = button_bindings; it; it = g_list_next(it)) {
        ButtonBinding *b = it->data;
        if (b->over == over && b->button == button && b->mods == mods)
            return b;
    }
    return NULL;
}

/*! The binding covering what was hit, innermost first, and the window it
  is reported on. */
static ButtonBinding *binding_for(const Hit *hit, guint button, guint mods,
                                  guintptr *over)
{
    ButtonBinding *b;
    WlrCanvas *c, *press;

    /* a client: its own bindings, then its frame's */
    if (hit->surface && (b = button_bound(hit_id(hit), button, mods))) {
        *over = hit_id(hit);
        return b;
    }
    press = hit->surface ? NULL : taker(hit->canvas, OB_CANVAS_INPUT_PRESS);
    for (c = frame_of(hit); c; c = c->parent)
        if ((b = button_bound((guintptr)c, button, mods))) {
            /* reported on the part of the frame that takes presses, as X
               reports it on the innermost window that selected them */
            *over = press && is_inside((guintptr)c, (guintptr)press) ?
                (guintptr)press : (guintptr)c;
            return b;
        }
    return NULL;
}

static void core_button(guint button, gboolean press, guint32 time,
                        guintptr over, gboolean desktop)
{
    ObInputEvent e;

    memset(&e, 0, sizeof(e));
    e.type = press ? OB_INPUT_BUTTON_PRESS : OB_INPUT_BUTTON_RELEASE;
    e.time = time;
    e.mods = mods_now();
    e.button = button;
    event_at(&e, over, desktop);
    if (desktop) {
        Hit hit;
        hit_test(&hit);
        e.under = hit_id(&hit);
    }
    deliver(&e);
}

static void cursor_button(struct wl_listener *l, void *data)
{
    struct wlr_pointer_button_event *ev = data;
    gboolean press = ev->state == WLR_BUTTON_PRESSED;
    guint button = x_button(ev->button);
    Hit hit;

    if (button && !press) buttons_down &= ~(1 << button);

    if (!press) {
        if (held) {
            /* the core never let the held press through */
            held = FALSE;
            if (button) core_button(button, FALSE, ev->time_msec,
                                    implicit_over, implicit_desktop);
            if (!buttons_down) implicit = FALSE;
            return;
        }
        if (ptr_grabbed || implicit) {
            if (button)
                core_button(button, FALSE, ev->time_msec,
                            implicit ? implicit_over :
                            (ptr_owner_events ? crossing_over : 0),
                            implicit ? implicit_desktop : FALSE);
            if (!buttons_down && implicit) {
                implicit = FALSE;
                lw_pointer_refresh(ev->time_msec);
            }
            return;
        }
        wlr_seat_pointer_notify_button(lw_seat, ev->time_msec, ev->button,
                                       ev->state);
        return;
    }

    hit_test(&hit);
    if (ptr_grabbed) {
        guintptr over = ptr_owner_events && !hit.surface ?
            (guintptr)taker(hit.canvas, OB_CANVAS_INPUT_PRESS) : 0;
        if (button) buttons_down |= 1 << button;
        if (button) core_button(button, TRUE, ev->time_msec, over, !over);
        return;
    }
    if (implicit) {
        if (button) {
            buttons_down |= 1 << button;
            core_button(button, TRUE, ev->time_msec, implicit_over,
                        implicit_desktop);
        }
        return;
    }

    if (button) {
        ButtonBinding *b;
        guintptr over = 0;
        guint mods = mods_now();

        if ((b = binding_for(&hit, button, mods, &over)) ||
            (!hit.surface &&
             (over = (guintptr)taker(hit.canvas, OB_CANVAS_INPUT_PRESS))) ||
            (!hit.surface && !hit.canvas))
        {
            buttons_down |= 1 << button;
            implicit = TRUE;
            implicit_over = over;
            implicit_desktop = !over;
            if (b && b->hold) {
                held = TRUE;
                held_press = *ev;
                held_surface = hit.wl;
            }
            if (b && b->cursor != OB_CURSOR_NONE) show_cursor(b->cursor);
            core_button(button, TRUE, ev->time_msec, over, !over);
            return;
        }
    }
    /* the client's */
    if (hit.surface) {
        WlrSurface *s = hit.surface;
        if (s->toplevel && !s->toplevel->base->surface->mapped) return;
    }
    wlr_seat_pointer_notify_button(lw_seat, ev->time_msec, ev->button,
                                   ev->state);
}

void lw_pass_click(guint32 time)
{
    if (!held) return;
    held = FALSE;
    implicit = FALSE;
    buttons_down = 0;
    /* the press goes to the client after all, and what follows with it */
    if (held_surface) {
        Hit hit;
        hit_test(&hit);
        set_client_pointer(&hit, held_press.time_msec);
        wlr_seat_pointer_notify_button(lw_seat, held_press.time_msec,
                                       held_press.button, held_press.state);
    }
}

static void cursor_axis(struct wl_listener *l, void *data)
{
    struct wlr_pointer_axis_event *ev = data;
    Hit hit;

    hit_test(&hit);
    if (ptr_grabbed || implicit || !hit.surface) {
        /* scrolling is buttons 4 to 7 to the core, as on X */
        guint b;
        guintptr over;

        if (ev->delta == 0) return;
        if (ev->orientation == WLR_AXIS_ORIENTATION_VERTICAL)
            b = ev->delta < 0 ? 4 : 5;
        else
            b = ev->delta < 0 ? 6 : 7;
        over = implicit ? implicit_over :
            (guintptr)taker(hit.canvas, OB_CANVAS_INPUT_PRESS);
        if (!over && !implicit) {
            ButtonBinding *bb = binding_for(&hit, b, mods_now(), &over);
            if (!bb) over = 0;
        }
        core_button(b, TRUE, ev->time_msec, over, !over);
        core_button(b, FALSE, ev->time_msec, over, !over);
        return;
    }
    wlr_seat_pointer_notify_axis(lw_seat, ev->time_msec, ev->orientation,
                                 ev->delta, ev->delta_discrete, ev->source);
}

static void cursor_frame(struct wl_listener *l, void *data)
{
    wlr_seat_pointer_notify_frame(lw_seat);
}

void lw_bind_button(guintptr over, guint button, guint mods, gboolean hold,
                     ObCursor c)
{
    ButtonBinding *b;

    if (button_bound(over, button, mods)) return;
    b = g_new0(ButtonBinding, 1);
    b->over = over;
    b->button = button;
    b->mods = mods;
    b->hold = hold;
    b->cursor = c;
    button_bindings = g_list_prepend(button_bindings, b);
}

void lw_unbind_button(guintptr over, guint button, guint mods)
{
    ButtonBinding *b = button_bound(over, button, mods);

    if (b) {
        button_bindings = g_list_remove(button_bindings, b);
        g_free(b);
    }
}

void lw_input_forget(guintptr id)
{
    GList *it, *next;

    for (it = button_bindings; it; it = next) {
        ButtonBinding *b = it->data;
        next = g_list_next(it);
        if (b->over == id) {
            button_bindings = g_list_delete_link(button_bindings, it);
            g_free(b);
        }
    }
    if (crossing_over == id) crossing_over = 0;
    if (implicit && implicit_over == id) implicit_over = 0;
}

/* the pointer */

static const gchar *cursor_name(ObCursor c)
{
    switch (c) {
    case OB_CURSOR_NONE:
    case OB_CURSOR_POINTER: return "default";
    case OB_CURSOR_BUSYPOINTER: return "progress";
    case OB_CURSOR_BUSY: return "wait";
    case OB_CURSOR_MOVE: return "move";
    case OB_CURSOR_NORTH: return "n-resize";
    case OB_CURSOR_NORTHEAST: return "ne-resize";
    case OB_CURSOR_EAST: return "e-resize";
    case OB_CURSOR_SOUTHEAST: return "se-resize";
    case OB_CURSOR_SOUTH: return "s-resize";
    case OB_CURSOR_SOUTHWEST: return "sw-resize";
    case OB_CURSOR_WEST: return "w-resize";
    case OB_CURSOR_NORTHWEST: return "nw-resize";
    default: return "default";
    }
}

static void show_cursor(ObCursor c)
{
    wlr_cursor_set_xcursor(cursor, xcursors, cursor_name(c));
}

static ObCursor desktop_cursor = OB_CURSOR_POINTER;

void lw_set_cursor(ObCanvasId id, ObCursor c)
{
    /* the desktop's, for none */
    if (id)
        ((WlrCanvas*)id)->cursor = c;
    else
        desktop_cursor = c;
}

gboolean lw_grab_pointer(gboolean grab, gboolean owner_events,
                          gboolean confine, ObCursor c, guint32 time)
{
    ptr_grabbed = grab;
    ptr_owner_events = owner_events;
    grab_cursor = c;
    if (grab) {
        /* the client under the pointer loses it */
        wlr_seat_pointer_notify_clear_focus(lw_seat);
        crossing(crossing_over, 0, time, OB_CROSSING_GRAB);
        crossing_over = 0;
        show_cursor(c);
    } else {
        lw_pointer_refresh(time);
    }
    return TRUE;
}

gboolean lw_pointer_position(gint *x, gint *y)
{
    *x = cursor->x;
    *y = cursor->y;
    return TRUE;
}

void lw_pointer_warp(gint x, gint y, gboolean swallow)
{
    wlr_cursor_warp_closest(cursor, NULL, x, y);
    if (!swallow) pointer_moved(0);
}

static void seat_request_cursor(struct wl_listener *l, void *data)
{
    struct wlr_seat_pointer_request_set_cursor_event *e = data;

    if (lw_seat->pointer_state.focused_client == e->seat_client)
        wlr_cursor_set_surface(cursor, e->surface, e->hotspot_x,
                               e->hotspot_y);
}

static void seat_request_selection(struct wl_listener *l, void *data)
{
    struct wlr_seat_request_set_selection_event *e = data;

    wlr_seat_set_selection(lw_seat, e->source, e->serial);
}

static void input_new(struct wl_listener *l, void *data)
{
    struct wlr_input_device *dev = data;
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;

    switch (dev->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        new_keyboard(dev);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(cursor, dev);
        break;
    default:
        break;
    }
    if (keyboards) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(lw_seat, caps);
}

void lw_input_startup(struct wlr_backend *backend)
{
    key_bindings = g_array_new(FALSE, FALSE, sizeof(KeyBinding));
    /* bindings are looked up before any keyboard shows up */
    keymap = make_keymap();
    core_keys = g_array_new(FALSE, FALSE, sizeof(guint));

    lw_seat = wlr_seat_create(lw_display, "seat0");
    request_cursor.notify = seat_request_cursor;
    wl_signal_add(&lw_seat->events.request_set_cursor, &request_cursor);
    request_selection.notify = seat_request_selection;
    wl_signal_add(&lw_seat->events.request_set_selection,
                  &request_selection);

    cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(cursor, lw_layout);
    xcursors = wlr_xcursor_manager_create(g_getenv("XCURSOR_THEME"), 24);
    wlr_xcursor_manager_load(xcursors, 1);

    motion.notify = cursor_motion;
    wl_signal_add(&cursor->events.motion, &motion);
    motion_absolute.notify = cursor_motion_absolute;
    wl_signal_add(&cursor->events.motion_absolute, &motion_absolute);
    button.notify = cursor_button;
    wl_signal_add(&cursor->events.button, &button);
    axis.notify = cursor_axis;
    wl_signal_add(&cursor->events.axis, &axis);
    frame.notify = cursor_frame;
    wl_signal_add(&cursor->events.frame, &frame);

    new_input.notify = input_new;
    wl_signal_add(&backend->events.new_input, &new_input);
}

void lw_input_shutdown(void)
{
    wl_list_remove(&new_input.link);
    wl_list_remove(&request_cursor.link);
    wl_list_remove(&request_selection.link);
    wl_list_remove(&motion.link);
    wl_list_remove(&motion_absolute.link);
    wl_list_remove(&button.link);
    wl_list_remove(&axis.link);
    wl_list_remove(&frame.link);
    wlr_xcursor_manager_destroy(xcursors);
    wlr_cursor_destroy(cursor);
    g_list_free_full(button_bindings, g_free);
    button_bindings = NULL;
    g_array_free(key_bindings, TRUE);
    g_array_free(core_keys, TRUE);
    if (keymap) xkb_keymap_unref(keymap);
    keymap = NULL;
}
