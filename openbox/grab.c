/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   grab.c for the Openbox window manager
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

#include "grab.h"
#include "openbox.h"
#include "event.h"
#include "screen.h"
#include "debug.h"
#include "backend.h"
#include "obt/display.h"
#include "obt/keyboard.h"

#include <glib.h>
#include <X11/Xlib.h>

static guint kgrabs = 0;
static guint pgrabs = 0;
/*! The time at which the last grab was made */
static Time  grab_time = CurrentTime;
static gint passive_count = 0;
static ObtIC *ic = NULL;

/* grab.h's grab_keyboard() and grab_pointer() macros would take the backend
   calls of the same names, hence (*ob_backend->grab_...)(). */

static Time ungrab_time(void)
{
    Time t = event_time();
    if (grab_time == CurrentTime ||
        !(t == CurrentTime || event_time_after(t, grab_time)))
        /* When the time moves backward on the server, then we can't use
           the grab time because that will be in the future. So instead we
           have to use CurrentTime.

           "XUngrabPointer does not release the pointer if the specified time
           is earlier than the last-pointer-grab time or is later than the
           current X server time."
        */
        t = CurrentTime; /*grab_time;*/
    return t;
}

static Window grab_window(void)
{
    return screen_support_win;
}

gboolean grab_on_keyboard(void)
{
    return kgrabs > 0;
}

gboolean grab_on_pointer(void)
{
    return pgrabs > 0;
}

ObtIC *grab_input_context(void)
{
    return ic;
}

gboolean grab_keyboard_full(gboolean grab)
{
    gboolean ret = FALSE;

    if (grab) {
        if (kgrabs++ == 0) {
            ret = (*ob_backend->grab_keyboard)(TRUE, event_time());
            if (!ret)
                --kgrabs;
            else {
                passive_count = 0;
                grab_time = event_time();
            }
        } else
            ret = TRUE;
    } else if (kgrabs > 0) {
        if (--kgrabs == 0) {
            (*ob_backend->grab_keyboard)(FALSE, ungrab_time());
        }
        ret = TRUE;
    }

    return ret;
}

gboolean grab_pointer_full(gboolean grab, gboolean owner_events,
                           gboolean confine, ObCursor cur)
{
    gboolean ret = FALSE;

    if (grab) {
        if (pgrabs++ == 0) {
            ret = (*ob_backend->grab_pointer)(TRUE, owner_events, confine,
                                              cur, event_time());
            if (!ret)
                --pgrabs;
            else
                grab_time = event_time();
        } else
            ret = TRUE;
    } else if (pgrabs > 0) {
        if (--pgrabs == 0) {
            (*ob_backend->grab_pointer)(FALSE, FALSE, FALSE,
                                        OB_CURSOR_NONE, ungrab_time());
        }
        ret = TRUE;
    }
    return ret;
}

gint grab_server(gboolean grab)
{
    static guint sgrabs = 0;
    if (grab) {
        if (sgrabs++ == 0)
            ob_backend->grab_server(TRUE);
    } else if (sgrabs > 0) {
        if (--sgrabs == 0)
            ob_backend->grab_server(FALSE);
    }
    return sgrabs;
}

void grab_startup(gboolean reconfig)
{
    ic = obt_keyboard_context_new(obt_root(ob_screen), grab_window());
}

void grab_shutdown(gboolean reconfig)
{
    obt_keyboard_context_unref(ic);
    ic = NULL;

    if (reconfig) return;

    while (ungrab_keyboard());
    while (ungrab_pointer());
    while (grab_server(FALSE));
}

void grab_key_passive_count(int change)
{
    if (grab_on_keyboard()) return;
    passive_count += change;
    if (passive_count < 0) passive_count = 0;
}

void ungrab_passive_key(void)
{
    /*ob_debug("ungrabbing %d passive grabs\n", passive_count);*/
    if (passive_count) {
        /* kill our passive grab */
        (*ob_backend->grab_keyboard)(FALSE, event_time());
        passive_count = 0;
    }
}
