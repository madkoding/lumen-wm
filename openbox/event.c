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
#include "backend.h"
#include "obt/keyboard.h"

#include <X11/keysym.h>
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
    guint32 time;
    gulong serial;
} ObFocusDelayData;

typedef struct
{
    gulong start; /* inclusive */
    gulong end;   /* inclusive */
} ObSerialRange;

static gboolean event_handle_menu_input(const ObInputEvent *e);
static void event_handle_menu(ObMenuFrame *frame, const ObInputEvent *e);
static gboolean event_handle_prompt(ObPrompt *p, const ObInputEvent *e);
static void event_handle_dock(ObDock *s, const ObInputEvent *e);
static gboolean event_handle_user_input(ObClient *client,
                                        const ObInputEvent *e);
static gboolean is_enter_focus_event_ignored(gulong serial);
static void event_ignore_enter_range(gulong start, gulong end);

static void focus_delay_dest(gpointer data);
static void unfocus_delay_dest(gpointer data);
static gboolean focus_delay_func(gpointer data);
static gboolean unfocus_delay_func(gpointer data);
static void focus_delay_client_dest(ObClient *client, gpointer data);

guint32 event_last_user_time = 0;

/*! The time of the current event (if it had a timestamp) */
static guint32 event_curtime = 0;
/*! The source time that started the current event (user-provided, so not
  to be trusted) */
static guint32 event_sourcetime = 0;

/*! The serial of the current event */
static gulong event_curserial;
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

#ifdef USE_SM
    IceAddConnectionWatch(ice_watch, NULL);
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

void event_begin(guint32 time, gulong serial)
{
    /* watch that if we get an event earlier than the last specified user_time,
       which can happen if the clock goes backwards, we erase the last
       specified user_time */
    if (time && event_last_user_time &&
        event_time_after(event_last_user_time, time))
        event_reset_user_time();

    event_sourcetime = 0;
    event_curtime = time;
    event_curserial = serial;
}

void event_end(void)
{
    /* show any debug prompts that are queued */
    ob_debug_show_prompts();

    /* if something happens and it's not from an event, then we don't know
       the time, so clear it here until the next event is handled */
    event_curtime = event_sourcetime = 0;
    event_curserial = 0;
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

static gboolean event_handle_prompt(ObPrompt *p, const ObInputEvent *e)
{
    switch (e->type) {
    case OB_INPUT_BUTTON_PRESS:
    case OB_INPUT_BUTTON_RELEASE:
    case OB_INPUT_MOTION:
        return prompt_mouse_event(p, e);
    case OB_INPUT_KEY_PRESS:
        return prompt_key_event(p, e);
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

static gboolean event_handle_user_input(ObClient *client,
                                        const ObInputEvent *e)
{
    if (menu_frame_visible)
        if (event_handle_menu_input(e))
            /* don't use the event if the menu used it, but if the menu
               didn't use it and it's a keypress that is bound, it will
               close the menu and be used */
            return TRUE;

    /* if the keyboard interactive action uses the event then dont
       use it for bindings. likewise is moveresize uses the event. */
    if (actions_interactive_input_event(e) || moveresize_event(e))
        return TRUE;

    if (moveresize_in_progress)
        /* make further actions work on the client being
           moved/resized */
        client = moveresize_client;

    if (e->type == OB_INPUT_BUTTON_PRESS ||
        e->type == OB_INPUT_BUTTON_RELEASE ||
        e->type == OB_INPUT_MOTION)
    {
        /* the frame may not be "visible" but they can still click on it
           in the case where it is animating before disappearing */
        if (!client || !frame_iconify_animating(client->frame))
            return mouse_event(client, e);
    } else
        return keyboard_event((focus_cycle_target ? focus_cycle_target :
                               (client ? client : focus_client)), e);

    return FALSE;
}

void event_input(const ObInputEvent *e)
{
    ObWindow *obwin;
    ObClient *client = NULL;
    ObDock *dock = NULL;
    ObDockApp *dockapp = NULL;
    ObMenuFrame *menu = NULL;
    ObPrompt *prompt = NULL;
    gboolean used;

    if (shelf_input(e))
        return;

    if ((obwin = window_find((Window)e->over))) {
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
        dockapp = dock_find_dockapp((Window)e->over);

    if (client) {
        if (e->type != OB_INPUT_KEY_PRESS && e->type != OB_INPUT_KEY_RELEASE)
            event_handle_frame_pointer(client, e);
    }
    else if (dockapp) {
        if (e->type == OB_INPUT_MOTION)
            dock_app_drag(dockapp, e->root_x, e->root_y);
    }
    else if (dock || menu) {
        if (e->type == OB_INPUT_ENTER || e->type == OB_INPUT_LEAVE ||
            e->type == OB_INPUT_MOTION)
        {
            if (dock)
                event_handle_dock(dock, e);
            else
                event_handle_menu(menu, e);
        }
    }

    if (e->type == OB_INPUT_BUTTON_PRESS ||
        e->type == OB_INPUT_BUTTON_RELEASE)
    {
        ObWindow *w;
        static guint pressed = 0;

        event_sourcetime = event_curtime;

        /* If the button press was not physically on some window over the
           desktop... */
        if (!e->under ||
            /* ...or if it is related to the last button press we handled... */
            pressed == e->button ||
            /* ...or it if it was physically on an openbox
               internal window... */
            ((w = window_find((Window)e->under)) &&
             (WINDOW_IS_INTERNAL(w) || WINDOW_IS_DOCK(w))))
            /* ...then process the event, otherwise ignore it */
        {
            used = event_handle_user_input(client, e);

            if (prompt && !used)
                used = event_handle_prompt(prompt, e);

            if (e->type == OB_INPUT_BUTTON_PRESS)
                pressed = e->button;
            /* We ignored the release event so make sure we don't think
               the button is still pressed */
            else
                button = 0;
        }
    }
    else if (e->type == OB_INPUT_KEY_PRESS ||
             e->type == OB_INPUT_KEY_RELEASE ||
             e->type == OB_INPUT_MOTION)
    {
        event_sourcetime = event_curtime;

        used = event_handle_user_input(client, e);

        if (prompt && !used)
            used = event_handle_prompt(prompt, e);
    }
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
    guint32 old = event_curtime; /* save the curtime */

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
    guint32 old = event_curtime; /* save the curtime */

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
    return ob_backend->request_serial();
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
    ob_backend->request_serial_bump();
}

void event_end_ignore_all_enters(gulong start)
{
    /* Use (NextRequest-1) so that we ignore up to the current serial only.
       Inside event_ignore_enter_range, we increment the serial by one, but if
       we ignore that serial too, then any enter events generated by mouse
       movement will be ignored until we create some further network traffic.
       Instead ignore up to NextRequest-1, then when we increment the serial,
       we will be *past* the range of ignored serials */
    event_ignore_enter_range(start, ob_backend->request_serial()-1);
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

    ob_backend->flush(TRUE);
}

gboolean event_time_after(guint32 t1, guint32 t2)
{
    g_assert(t1 != 0);
    g_assert(t2 != 0);

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

guint32 event_time(void)
{
    /* some events don't come with timestamps, so ask for one, and keep it
       for the rest of this event */
    if (!event_curtime)
        event_curtime = (*ob_backend->time)();
    return event_curtime;
}

guint32 event_source_time(void)
{
    return event_sourcetime;
}

void event_set_source_time(guint32 t)
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
    event_last_user_time = 0;
}
