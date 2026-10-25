/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   mouse.c for the Openbox window manager
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

#include "openbox.h"
#include "config.h"
#include "actions.h"
#include "event.h"
#include "client.h"
#include "grab.h"
#include "backend.h"
#include "frame.h"
#include "translate.h"
#include "mouse.h"
#include "gettext.h"
#include "obt/display.h"

#include <glib.h>

typedef struct {
    guint state;
    guint button;
    GSList *actions[OB_NUM_MOUSE_ACTIONS]; /* lists of Action pointers */
} ObMouseBinding;

/* Array of GSList*s of ObMouseBinding*s. */
static GSList *bound_contexts[OB_FRAME_NUM_CONTEXTS];
/* TRUE when we have a grab on the pointer and need to replay the pointer event
   to send it to other applications */
static gboolean replay_pointer_needed;

/* this is the static button from mouse_event, moved here so that event.c can clear it */
guint button;

ObFrameContext mouse_button_frame_context(ObFrameContext context,
                                          guint button,
                                          guint state)
{
    GSList *it;
    ObFrameContext x = context;

    for (it = bound_contexts[context]; it; it = g_slist_next(it)) {
        ObMouseBinding *b = it->data;

        if (b->button == button && b->state == state)
            return context;
    }

    switch (context) {
    case OB_FRAME_CONTEXT_NONE:
    case OB_FRAME_CONTEXT_DESKTOP:
    case OB_FRAME_CONTEXT_CLIENT:
    case OB_FRAME_CONTEXT_TITLEBAR:
    case OB_FRAME_CONTEXT_FRAME:
    case OB_FRAME_CONTEXT_MOVE_RESIZE:
    case OB_FRAME_CONTEXT_LEFT:
    case OB_FRAME_CONTEXT_RIGHT:
    case OB_FRAME_CONTEXT_DOCK:
        break;
    case OB_FRAME_CONTEXT_ROOT:
        x = OB_FRAME_CONTEXT_DESKTOP;
        break;
    case OB_FRAME_CONTEXT_BOTTOM:
    case OB_FRAME_CONTEXT_BLCORNER:
    case OB_FRAME_CONTEXT_BRCORNER:
        x = OB_FRAME_CONTEXT_BOTTOM;
        break;
    case OB_FRAME_CONTEXT_TLCORNER:
    case OB_FRAME_CONTEXT_TRCORNER:
    case OB_FRAME_CONTEXT_TOP:
    case OB_FRAME_CONTEXT_MAXIMIZE:
    case OB_FRAME_CONTEXT_ALLDESKTOPS:
    case OB_FRAME_CONTEXT_SHADE:
    case OB_FRAME_CONTEXT_ICONIFY:
    case OB_FRAME_CONTEXT_ICON:
    case OB_FRAME_CONTEXT_CLOSE:
        x = OB_FRAME_CONTEXT_TITLEBAR;
        break;
    case OB_FRAME_NUM_CONTEXTS:
        g_assert_not_reached();
    }

    /* allow for multiple levels of fall-through */
    if (x != context)
        return mouse_button_frame_context(x, button, state);
    else
        return x;
}

void mouse_grab_for_client(ObClient *client, gboolean grab)
{
    gint i;
    GSList *it;

    for (i = 0; i < OB_FRAME_NUM_CONTEXTS; ++i)
        for (it = bound_contexts[i]; it; it = g_slist_next(it)) {
            /* grab/ungrab the button */
            ObMouseBinding *b = it->data;
            guintptr over;
            gboolean hold;

            if (FRAME_CONTEXT(i, client)) {
                over = client->frame->window;
                hold = FALSE;
            } else if (CLIENT_CONTEXT(i, client)) {
                over = client->window;
                hold = TRUE; /* the press is replayed or not in event */
            } else continue;

            if (grab)
                ob_backend->bind_button(over, b->button, b->state, hold,
                                        OB_CURSOR_NONE);
            else
                ob_backend->unbind_button(over, b->button, b->state);
        }
}

static void grab_all_clients(gboolean grab)
{
    GList *it;

    for (it = client_list; it; it = g_list_next(it))
        mouse_grab_for_client(it->data, grab);
}

void mouse_unbind_all(void)
{
    gint i;
    GSList *it;

    for(i = 0; i < OB_FRAME_NUM_CONTEXTS; ++i) {
        for (it = bound_contexts[i]; it; it = g_slist_next(it)) {
            ObMouseBinding *b = it->data;
            gint j;

            for (j = 0; j < OB_NUM_MOUSE_ACTIONS; ++j) {
                GSList *jt;

                for (jt = b->actions[j]; jt; jt = g_slist_next(jt))
                    actions_act_unref(jt->data);
                g_slist_free(b->actions[j]);
            }
            g_slice_free(ObMouseBinding, b);
        }
        g_slist_free(bound_contexts[i]);
        bound_contexts[i] = NULL;
    }
}

static ObUserAction mouse_action_to_user_action(ObMouseAction a)
{
    switch (a) {
    case OB_MOUSE_ACTION_PRESS: return OB_USER_ACTION_MOUSE_PRESS;
    case OB_MOUSE_ACTION_RELEASE: return OB_USER_ACTION_MOUSE_RELEASE;
    case OB_MOUSE_ACTION_CLICK: return OB_USER_ACTION_MOUSE_CLICK;
    case OB_MOUSE_ACTION_DOUBLE_CLICK:
        return OB_USER_ACTION_MOUSE_DOUBLE_CLICK;
    case OB_MOUSE_ACTION_MOTION: return OB_USER_ACTION_MOUSE_MOTION;
    default:
        g_assert_not_reached();
    }
}

static gboolean fire_binding(ObMouseAction a, ObFrameContext context,
                             ObClient *c, guint state,
                             guint button, gint x, gint y)
{
    GSList *it;
    ObMouseBinding *b;

    for (it = bound_contexts[context]; it; it = g_slist_next(it)) {
        b = it->data;
        if (b->state == state && b->button == button)
            break;
    }
    /* if not bound, then nothing to do! */
    if (it == NULL) return FALSE;

    actions_run_acts(b->actions[a], mouse_action_to_user_action(a),
                     state, x, y, button, context, c);
    return TRUE;
}

void mouse_replay_pointer(void)
{
    if (replay_pointer_needed) {
        /* replay the pointer event before any windows move */
        ob_backend->pass_click(event_time());
        replay_pointer_needed = FALSE;
    }
}

gboolean mouse_event(ObClient *client, const ObInputEvent *e)
{
    static guint32 ltime;
    static guint state = 0, lbutton = 0;
    static guintptr lwindow = 0;
    static gint px, py, pwx = -1, pwy = -1, lx = -10, ly = -10;
    gboolean used = FALSE;

    ObFrameContext context;
    gboolean click = FALSE;
    gboolean dclick = FALSE;

    switch (e->type) {
    case OB_INPUT_BUTTON_PRESS:
        context = frame_context(client, (Window)e->over,
                                e->x, e->y);
        context = mouse_button_frame_context(context, e->button,
                                             e->mods);

        px = e->root_x;
        py = e->root_y;
        if (!button) pwx = e->x;
        if (!button) pwy = e->y;
        button = e->button;
        state = e->mods;

        /* if the binding was in a client context, then we need to call
           XAllowEvents with ReplayPointer at some point, to send the event
           through to the client.  when this happens though depends.  if
           windows are going to be moved on screen, then the click will end
           up going somewhere wrong, set that we need it, and if nothing
           else causes the replay pointer to be run, then we will do it
           after all the actions are finished.

           (We do it after all the actions because FocusIn interrupts
           dragging for kdesktop, so if we send the button event now, and
           then they get a focus event after, it breaks.  Instead, wait to send
           the button press until after the actions when possible.)
        */
        if (CLIENT_CONTEXT(context, client))
            replay_pointer_needed = TRUE;

        used = fire_binding(OB_MOUSE_ACTION_PRESS, context,
                            client, e->mods,
                            e->button,
                            e->root_x, e->root_y) || used;

        /* if the bindings grab the pointer, there won't be a ButtonRelease
           event for us */
        if (grab_on_pointer())
            button = 0;

        /* replay the pointer event if it hasn't been replayed yet (i.e. no
           windows were moved) */
        mouse_replay_pointer();

        /* in the client context, we won't get a button release because of the
           way it is grabbed, so just fake one */
        if (!CLIENT_CONTEXT(context, client))
            break;

    case OB_INPUT_BUTTON_RELEASE:
        /* use where the press occured in the window */
        context = frame_context(client, (Window)e->over, pwx, pwy);
        context = mouse_button_frame_context(context, e->button,
                                             e->mods);

        if (e->button == button)
            pwx = pwy = -1;

        if (e->button == button) {
            /* clicks are only valid if its released over the window */
            if (e->inside) {
                click = TRUE;
                /* double clicks happen if there were 2 in a row! */
                if (lbutton == button &&
                    lwindow == e->over &&
                    e->time - config_mouse_dclicktime <=
                    ltime &&
                    ABS(e->x - lx) < 8 &&
                    ABS(e->y - ly) < 8)
                {
                    dclick = TRUE;
                    lbutton = 0;
                } else {
                    lbutton = button;
                    lwindow = e->over;
                    lx = e->x;
                    ly = e->y;
                }
            } else {
                lbutton = 0;
                lwindow = 0;
            }

            button = 0;
            state = 0;
            ltime = e->time;
        }
        used = fire_binding(OB_MOUSE_ACTION_RELEASE, context,
                            client, e->mods,
                            e->button,
                            e->root_x,
                            e->root_y) || used;
        if (click)
            used = fire_binding(OB_MOUSE_ACTION_CLICK, context,
                                client, e->mods,
                                e->button,
                                e->root_x,
                                e->root_y) || used;
        if (dclick)
            used = fire_binding(OB_MOUSE_ACTION_DOUBLE_CLICK, context,
                                client, e->mods,
                                e->button,
                                e->root_x,
                                e->root_y) || used;
        break;

    case OB_INPUT_MOTION:
        if (button) {
            context = frame_context(client, (Window)e->over, pwx, pwy);
            context = mouse_button_frame_context(context, button, state);

            if (ABS(e->root_x - px) >= config_mouse_threshold ||
                ABS(e->root_y - py) >= config_mouse_threshold) {

                /* You can't drag on buttons */
                if (context == OB_FRAME_CONTEXT_MAXIMIZE ||
                    context == OB_FRAME_CONTEXT_ALLDESKTOPS ||
                    context == OB_FRAME_CONTEXT_SHADE ||
                    context == OB_FRAME_CONTEXT_ICONIFY ||
                    context == OB_FRAME_CONTEXT_ICON ||
                    context == OB_FRAME_CONTEXT_CLOSE)
                    break;

                used = fire_binding(OB_MOUSE_ACTION_MOTION, context,
                                    client, state, button, px, py);
                button = 0;
                state = 0;
            }
        }
        break;

    default:
        g_assert_not_reached();
    }
    return used;
}

gboolean mouse_bind(const gchar *buttonstr, ObFrameContext context,
                    ObMouseAction mact, ObActionsAct *action)
{
    guint state = 0, button = 0;
    ObMouseBinding *b;
    GSList *it;

    g_assert(context != OB_FRAME_CONTEXT_NONE);

    if (!translate_button(buttonstr, &state, &button)) {
        g_message(_("Invalid button \"%s\" in mouse binding"), buttonstr);
        return FALSE;
    }

    for (it = bound_contexts[context]; it; it = g_slist_next(it)) {
        b = it->data;
        if (b->state == state && b->button == button) {
            b->actions[mact] = g_slist_append(b->actions[mact], action);
            return TRUE;
        }
    }

    /* add the binding */
    b = g_slice_new0(ObMouseBinding);
    b->state = state;
    b->button = button;
    b->actions[mact] = g_slist_append(NULL, action);
    bound_contexts[context] = g_slist_append(bound_contexts[context], b);

    return TRUE;
}

void mouse_startup(gboolean reconfig)
{
    grab_all_clients(TRUE);
}

void mouse_shutdown(gboolean reconfig)
{
    grab_all_clients(FALSE);
    mouse_unbind_all();
}
