/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   event.h for the Openbox window manager
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

#ifndef __events_h
#define __events_h

#include <glib.h>

struct _ObClient;
struct _ObInputEvent;

/*! The amount of time before a window appears that is checked for user input
    to determine if the user is working in another window */
#define OB_EVENT_USER_TIME_DELAY (1000) /* milliseconds */

/*! The last user-interaction time, as given by the clients */
extern guint32 event_last_user_time;

void event_startup(gboolean reconfig);
void event_shutdown(gboolean reconfig);

/*! The backend starts and ends reporting one event
  (ObBackendEvents.begin and end): event_time() and the serial of
  ignored crossings come from it. */
void event_begin(guint32 time, gulong serial);
void event_end(void);

/*! What the user did: the pointer over a frame, a menu or the dock, and
  bindings, menus, prompts and move/resize. */
void event_input(const struct _ObInputEvent *e);

/*! Make as if the mouse just entered the client, use only when using focus
    follows mouse */
void event_enter_client(struct _ObClient *client);

/*! Make as if the mouse just left the client, use only when using focus
    follows mouse */
void event_leave_client(struct _ObClient *client);

/*! Make mouse focus not move at all from the stuff that happens between these
    two function calls. */
gulong event_start_ignore_all_enters(void);
void event_end_ignore_all_enters(gulong start);

/*! End *all* active and passive grabs on the keyboard
    This is called in situations where if there is a grab going on, then
    we need to cancel it. If we move focus during the grab, applications
    will get NotifyWhileGrabbed events and ignore them!

    Actions should not rely on being able to move focus during an
    interactive grab. */
void event_cancel_all_key_grabs(void);

/* Halts any focus delay in progress, use this when the user is selecting a
   window for focus */
void event_halt_focus_delay(void);

/*! Compare t1 and t2, taking into account wraparound. True if t1
    comes at the same time or later than t2. */
gboolean event_time_after(guint32 t1, guint32 t2);

/*! guint32 at which the current event occured.  If this is not known, this
  is a time at or after it, but at or before any other events we will process
*/
guint32 event_time(void);

/*! Force event_time() to skip the current timestamp and look for the next
  one. */
void event_reset_time(void);

/*! A time at which an event happened that caused this current event to be
  generated.  This is a user-provided time and not to be trusted.
  Returns 0 if there was no source time provided.
*/
guint32 event_source_time(void);
/*! The time a request says the user asked for it, for focus stealing
  prevention; only for the event being handled. */
void event_set_source_time(guint32 t);

/*! Update the timestamp for when the user has last used the focused window.
  This updates the timestamp to the time of the last event, given by
  event_time().
*/
void event_update_user_time(void);

/*! Reset the timestamp for when the user has last used the focused window. */
void event_reset_user_time(void);

#endif
