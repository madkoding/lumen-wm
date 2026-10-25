/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   backend.h for the Lumen window manager

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

/* The line between the window manager and the display protocol.

   The core (placement, focus, stacking, desktops, actions, the shelf and the
   menus) decides; a backend carries those decisions to the display server and
   brings back what clients and input devices do. backend-x11 speaks Xlib and
   EWMH, as Openbox always has; backend-wlroots will be a Wayland compositor
   built on wlroots, with XWayland surfaces handed to the core like any other.

   Nothing here may name an X or a Wayland type. See docs/design/backends.md
   for how the core moves onto this interface. */

#ifndef __backend_h
#define __backend_h

#include "geom.h"
#include "misc.h"
#include "mwm.h"

#include <glib.h>

/*! A surface the core manages: an X client window, an xdg toplevel or an
  XWayland surface. Only the backend that made it knows what is behind it. */
typedef guintptr ObSurfaceId;
/*! A surface the core draws itself: a frame's decorations, a menu, a popup,
  the shelf. */
typedef guintptr ObCanvasId;

typedef struct _ObBackend       ObBackend;
typedef struct _ObBackendEvents ObBackendEvents;

#define OB_SURFACE_NONE ((ObSurfaceId)0)
#define OB_CANVAS_NONE ((ObCanvasId)0)

/*! What a backend can do at all; the core works around what it can't. The
  contract tests skip their cases the same way (tests/contract/driver.h). */
typedef enum {
    /*! clients may ask for a position (X11 can, xdg-shell can't) */
    OB_BACKEND_CAP_CLIENT_POSITION = 1 << 0,
    /*! clients that want no decorations of ours can say so per window
      (Motif hints on X11, xdg-decoration on Wayland) */
    OB_BACKEND_CAP_DECOR_HINTS     = 1 << 1,
    /*! the backend publishes the root window properties pagers read, so
      the core must keep them up to date */
    OB_BACKEND_CAP_ROOT_STATE      = 1 << 2,
    /*! withdrawn dock apps (X11 only) */
    OB_BACKEND_CAP_DOCKAPPS        = 1 << 3,
    /*! XSync-style counters to pace interactive resizes */
    OB_BACKEND_CAP_SYNC_RESIZE     = 1 << 4,
} ObBackendCaps;

typedef enum {
    OB_SURFACE_TYPE_NORMAL,
    OB_SURFACE_TYPE_DIALOG,
    OB_SURFACE_TYPE_UTILITY,
    OB_SURFACE_TYPE_TOOLBAR,
    OB_SURFACE_TYPE_MENU,
    OB_SURFACE_TYPE_SPLASH,
    OB_SURFACE_TYPE_DOCK,
    OB_SURFACE_TYPE_DESKTOP
} ObSurfaceType;

/*! States the core tells a client it is in. */
typedef enum {
    OB_SURFACE_STATE_MAXIMIZED_HORZ = 1 << 0,
    OB_SURFACE_STATE_MAXIMIZED_VERT = 1 << 1,
    OB_SURFACE_STATE_FULLSCREEN     = 1 << 2,
    OB_SURFACE_STATE_ICONIC         = 1 << 3,
    OB_SURFACE_STATE_SHADED         = 1 << 4,
    OB_SURFACE_STATE_ABOVE          = 1 << 5,
    OB_SURFACE_STATE_BELOW          = 1 << 6,
    OB_SURFACE_STATE_STICKY         = 1 << 7,
    OB_SURFACE_STATE_URGENT         = 1 << 8,
    OB_SURFACE_STATE_FOCUSED        = 1 << 9,
    OB_SURFACE_STATE_SKIP_TASKBAR   = 1 << 10,
    OB_SURFACE_STATE_SKIP_PAGER     = 1 << 11,
    OB_SURFACE_STATE_MODAL          = 1 << 12,
    /*! the core is drawing no decorations for it */
    OB_SURFACE_STATE_UNDECORATED    = 1 << 13
} ObSurfaceStates;

/*! Who asked, so focus stealing prevention can weigh it. */
typedef enum {
    OB_REQUEST_FROM_UNKNOWN,
    OB_REQUEST_FROM_APPLICATION,
    OB_REQUEST_FROM_PAGER
} ObRequestSource;

/*! Where a client asks to be stacked, relative to a sibling or to every
  other window (X's stack modes). */
typedef enum {
    OB_STACK_ABOVE,
    OB_STACK_BELOW,
    OB_STACK_TOP_IF,        /*!< raise if something covers it */
    OB_STACK_BOTTOM_IF,     /*!< lower if it covers something */
    OB_STACK_OPPOSITE       /*!< whichever of those two applies */
} ObStackMode;

/*! Which parts of an ObConfigureRequest the client gave. */
typedef enum {
    OB_CONFIGURE_X       = 1 << 0,
    OB_CONFIGURE_Y       = 1 << 1,
    OB_CONFIGURE_WIDTH   = 1 << 2,
    OB_CONFIGURE_HEIGHT  = 1 << 3,
    OB_CONFIGURE_BORDER  = 1 << 4, /*!< X11 border width */
    OB_CONFIGURE_STACK   = 1 << 5,
    OB_CONFIGURE_SIBLING = 1 << 6, /*!< stack relative to sibling */
    OB_CONFIGURE_GRAVITY = 1 << 7  /*!< use gravity for this request */
} ObConfigureFields;

/*! A client or a pager asks for a new geometry or stacking. */
typedef struct _ObConfigureRequest {
    ObConfigureFields fields;
    Rect want;              /*!< the client's own area */
    gint border;
    ObStackMode stack;
    guintptr sibling;
    gint gravity;           /*!< X's win_gravity values */
} ObConfigureRequest;

/*! What to do with the states in a request. */
typedef enum {
    OB_STATE_ACTION_REMOVE,
    OB_STATE_ACTION_ADD,
    OB_STATE_ACTION_TOGGLE
} ObStateAction;

/*! Which parts of ObSurfaceInfo changed. */
typedef enum {
    OB_SURFACE_INFO_TITLE    = 1 << 0,
    OB_SURFACE_INFO_CLASS    = 1 << 1, /*!< class, role and session ids */
    OB_SURFACE_INFO_TYPE     = 1 << 2,
    OB_SURFACE_INFO_PARENT   = 1 << 3,
    OB_SURFACE_INFO_SIZE     = 1 << 4,
    OB_SURFACE_INFO_STRUT    = 1 << 5,
    OB_SURFACE_INFO_ICON     = 1 << 6,
    OB_SURFACE_INFO_HINTS    = 1 << 7, /*!< focus, urgency, group, iconic */
    OB_SURFACE_INFO_DECOR    = 1 << 8,
    OB_SURFACE_INFO_PROTOCOLS = 1 << 9,
    OB_SURFACE_INFO_ICON_AREA = 1 << 10, /*!< where a taskbar shows it */
    OB_SURFACE_INFO_STARTUP  = 1 << 11, /*!< startup notification id */
    OB_SURFACE_INFO_DESKTOP  = 1 << 12, /*!< the desktop it asks for */
    OB_SURFACE_INFO_STATES   = 1 << 13, /*!< the states it maps in */
    OB_SURFACE_INFO_USER_TIME = 1 << 14,
    OB_SURFACE_INFO_SYNC     = 1 << 15, /*!< its resize sync counter */
    OB_SURFACE_INFO_OPACITY  = 1 << 16, /*!< how opaque it asks to be */
    OB_SURFACE_INFO_ALL      = (1 << 17) - 1
} ObSurfaceInfoFields;

/*! One size of a client's icon. */
typedef struct _ObIcon {
    gint width, height;
    /*! width * height pixels, 0xAARRGGBB, not premultiplied (as in
      _NET_WM_ICON) */
    guint32 *argb;
} ObIcon;

/*! What a client says about itself. The backend reads it from WM_NORMAL_HINTS,
  _NET_WM_* and friends, or from xdg_toplevel requests. */
typedef struct _ObSurfaceInfo {
    gchar *title;           /*!< NULL when it has none */
    gchar *icon_title;      /*!< what to show when iconic; NULL for none */
    gchar *app_id;          /*!< WM_CLASS class, or xdg app_id */
    gchar *instance;        /*!< WM_CLASS name; NULL on Wayland */
    gchar *role;            /*!< WM_WINDOW_ROLE; NULL on Wayland */
    /*! the class and name of its group leader (X11); NULL for none */
    gchar *group_app_id, *group_instance;
    gchar *session_id;      /*!< SM_CLIENT_ID (X11); NULL for none */
    /*! the command that started it, its words run together (X11) */
    gchar *command;
    /*! the host it runs on, as it says (WM_CLIENT_MACHINE); NULL when it
      doesn't say */
    gchar *machine;
    gboolean has_type;      /*!< FALSE: it named no type we know */
    ObSurfaceType type;
    /*! it wants no decorations and no functions at all
      (_KDE_NET_WM_WINDOW_TYPE_OVERRIDE) */
    gboolean type_override;
    /*! what it says about decorations and functions; flags 0 for nothing.
      Motif hints on X11; a client-side xdg-decoration on Wayland is
      decorations 0 */
    ObMwmHints decor_hints;
    gboolean has_parent;    /*!< it is transient for something */
    ObSurfaceId parent;     /*!< transient for, or xdg parent */
    /*! transient for its whole group (WM_TRANSIENT_FOR the root, X11) */
    gboolean parent_is_group;

    /*! FALSE when it gave no size hints, or nonsense ones; the sizes below
      are then the defaults, and has_position, has_size and gravity are
      not set */
    gboolean size_hints;
    gboolean has_position;  /*!< the client asked for want.x and want.y */
    gboolean has_size;      /*!< the client asked for want's size */
    Rect want;              /*!< where and how big it would like to be */
    /*! defaults: min 0x0, max G_MAXINT, base -1x-1 (none), inc 1x1 */
    Size min, max, base, inc;
    gfloat min_ratio, max_ratio; /*!< 0 for none */
    gboolean has_gravity;
    gint gravity;           /*!< X's win_gravity values, which the core uses */

    StrutPartial strut;     /*!< docks and panels; zero for none */
    /*! the strut spans whole edges (legacy _NET_WM_STRUT): its start and
      end are not set, and the core fills them in from the screen */
    gboolean strut_whole_edges;
    gboolean undecorated;   /*!< asks for no decorations of ours */

    /*! FALSE when it gave no WM_HINTS (X11); then only accepts_focus is
      set */
    gboolean hints;
    gboolean accepts_focus; /*!< WM_HINTS input; TRUE when not given */
    gboolean urgent;
    gboolean has_initial_state;
    gboolean starts_iconic; /*!< wants to be mapped iconic */
    /*! the leader of its window group (X11), or OB_SURFACE_NONE */
    ObSurfaceId group;
    gboolean icon_in_hints; /*!< WM_HINTS carry an icon pixmap (X11) */

    gboolean takes_focus;   /*!< WM_TAKE_FOCUS */
    gboolean can_close;     /*!< WM_DELETE_WINDOW; always on Wayland */
    gboolean can_ping;      /*!< _NET_WM_PING; always on Wayland */
    gboolean sync_resize;   /*!< _NET_WM_SYNC_REQUEST */
    gint pid;               /*!< 0 when not known; X11 needs machine too */

    gchar *startup_id;      /*!< for startup notification; NULL for none */
    gboolean has_desktop;
    guint desktop;          /*!< DESKTOP_ALL for every desktop */
    /*! what it asks to be in when it shows up (_NET_WM_STATE on X11, the
      xdg_toplevel requests made before the first commit on Wayland) */
    ObSurfaceStates states;
    gboolean has_user_time;
    /*! when the user last used it, in event time; 0 asks not to be
      focused */
    guint32 user_time;
    /*! what it syncs resizes with (an XSync counter on X11); 0 for
      nothing. Only with OB_BACKEND_CAP_SYNC_RESIZE. */
    guint32 sync_counter;
    gboolean has_opacity;   /*!< FALSE: it leaves that to the compositor */
    guint32 opacity;        /*!< 0 is clear, 0xffffffff opaque */

    ObIcon *icons;          /*!< every size it offers; NULL for none */
    guint n_icons;
    /*! where its taskbar entry is, for iconify animations; zero for none */
    Rect icon_area;
} ObSurfaceInfo;

/*! Where on screen a canvas lives; this is its stacking layer. */
typedef enum {
    OB_CANVAS_DECOR,        /*!< a frame: below and around its client */
    OB_CANVAS_PANEL,        /*!< the shelf; may reserve an edge */
    OB_CANVAS_POPUP,        /*!< menus, focus cycling, prompts */
    OB_CANVAS_OVERLAY       /*!< above everything: move/resize outlines */
} ObCanvasLayer;

/*! A picture the core drew, in premultiplied ARGB32 (cairo's
  CAIRO_FORMAT_ARGB32), to show in a canvas. */
typedef struct _ObImage {
    const guint32 *data;
    gint width, height;
    gint stride;            /*!< in bytes */
} ObImage;

/*! What the core asks of the display server. A backend that has nothing to
  do for one (key grabs on Wayland, where the compositor sees every key) does
  nothing. While the core moves onto this interface, a function the core does
  not call yet may be NULL; docs/design/backends.md says which. */
struct _ObBackend {
    const gchar *name;
    ObBackendCaps caps;

    /* --- lifetime --- */

    /*! Connect and take over the screen, and report to events from then on.
      replace: replace a running window manager (X11), or fail when one is
      running. */
    gboolean (*startup)(const gchar *display, gboolean replace,
                        const ObBackendEvents *events);
    void (*shutdown)(void);
    /*! The source that delivers the backend's events to the main loop. */
    GSource *(*event_source)(void);
    /*! Milliseconds since some fixed point, comparable to event times. */
    guint32 (*time)(void);

    /* --- outputs --- */

    /*! Look at the outputs again and say how many there are; at least
      one. */
    guint (*output_count)(void);
    /*! An output's area in layout coordinates, as output_count() last
      found it. */
    void (*output_area)(guint output, Rect *area);

    /* --- managed surfaces --- */

    /*! Place the client's own area, in layout coordinates, and tell it its
      size. The frame around it is placed with canvas_place(). */
    void (*surface_configure)(ObSurfaceId s, const Rect *area);
    void (*surface_show)(ObSurfaceId s, gboolean show);
    /*! Give it the keyboard. input: hand it the keyboard focus; take_focus:
      tell it to take the focus itself (ICCCM WM_TAKE_FOCUS, X11 only).
      Returns FALSE when it couldn't be focused. */
    gboolean (*surface_focus)(ObSurfaceId s, guint32 time, gboolean input,
                              gboolean take_focus);
    /*! Ask it to close; it may refuse or hang. */
    void (*surface_close)(ObSurfaceId s, guint32 time);
    /*! Disconnect its client. */
    void (*surface_kill)(ObSurfaceId s);
    /*! Tell it every state it is in now. */
    void (*surface_set_states)(ObSurfaceId s, ObSurfaceStates states);
    /*! Ask whether it still answers; the answer comes as pong(). */
    void (*surface_ping)(ObSurfaceId s, guint32 serial);
    /*! Synced resizes (OB_BACKEND_CAP_SYNC_RESIZE), one surface at a time:
      start syncing with the counter surface_info() gave, ask the client
      to say when it has drawn the next size it is configured to (the
      answer comes as surface_synced()), and stop. sync_start is FALSE
      when it can't sync (X11: no XSync extension). */
    gboolean (*sync_start)(ObSurfaceId s, guint32 counter);
    void (*sync_request)(ObSurfaceId s, guint32 time);
    void (*sync_stop)(void);
    /*! Read what the client says about itself now, for the given fields
      only; the others are left as they are. Free the strings with
      backend_surface_info_clear(). */
    void (*surface_info)(ObSurfaceId s, ObSurfaceInfoFields fields,
                         ObSurfaceInfo *info);

    /* --- stacking --- */

    /*! Stack these right below above, the first one highest, as
      XRestackWindows() does. Ids are managed surfaces, the canvases they
      own, or the backend's own windows; a canvas id and a surface id never
      collide. above is OB_SURFACE_NONE to put them at the top. */
    void (*restack)(guintptr above, const guintptr *top_first, guint n);

    /* --- canvases: what the core draws --- */

    /*! owner: the surface a decoration frames, or OB_SURFACE_NONE. */
    ObCanvasId (*canvas_new)(ObCanvasLayer layer, ObSurfaceId owner);
    void (*canvas_free)(ObCanvasId c);
    /*! outer: the whole canvas. For a decoration, inner is the hole the
      client shows through, relative to outer; NULL otherwise. */
    void (*canvas_place)(ObCanvasId c, const Rect *outer, const Rect *inner);
    void (*canvas_show)(ObCanvasId c, gboolean show);
    /*! Show this picture, scaled by the output's scale; damage, in canvas
      coordinates, says what changed (NULL for all of it). */
    void (*canvas_present)(ObCanvasId c, const ObImage *img,
                           const Rect *damage);
    /*! Where it takes input, relative to it; NULL for all of it, n = 0 for
      nowhere (click-through). */
    void (*canvas_set_input)(ObCanvasId c, const Rect *rects, guint n);
    /*! Reserve an edge of the output, as a strut or a layer-shell
      exclusive zone. */
    void (*canvas_set_strut)(ObCanvasId c, const StrutPartial *strut);
    /*! Show it, and the surface a decoration frames, this opaque (as
      ObSurfaceInfo's opacity); set FALSE leaves that to the compositor.
      Until the frames are canvases, the core passes a frame's window, as
      it does to restack. */
    void (*canvas_set_opacity)(ObCanvasId c, gboolean set, guint32 opacity);

    /* --- input --- */

    /*! Send every pointer event to the core, whatever is under the
      pointer, or stop. owner_events: the core's own canvases still get the
      pointer over them (menus); confine: keep it on the screen. time: the
      event that asked. FALSE when the grab fails (another client holds
      it). */
    gboolean (*grab_pointer)(gboolean grab, gboolean owner_events,
                             gboolean confine, ObCursor cursor,
                             guint32 time);
    gboolean (*grab_keyboard)(gboolean grab, guint32 time);
    /*! Hold the other clients still while the core does several things
      that must look like one (X11 XGrabServer; nothing on Wayland). The
      core counts nested grabs. */
    void (*grab_server)(gboolean grab);
    /*! Make sure a key binding reaches the core whichever surface has the
      focus, with any lock key on or off. keycode and mods are xkb's (on
      X11, the server's keycodes and modifier masks). */
    void (*bind_key)(guint keycode, guint mods);
    void (*unbind_all_keys)(void);
    /*! Make sure a button binding over a canvas or a surface reaches the
      core, with any lock key on or off. hold: over a surface, hold the
      press until the core says whether the client gets it; otherwise the
      press, the drag and the release go to the core. cursor: shown while
      the button is down. */
    void (*bind_button)(guintptr over, guint button, guint mods,
                        gboolean hold, ObCursor cursor);
    void (*unbind_button)(guintptr over, guint button, guint mods);
    /*! Let the press a hold binding held reach the client after all
      (X11 XAllowEvents ReplayPointer). time: the press's. */
    void (*pass_click)(guint32 time);
    /*! Where the pointer is, in layout coordinates. FALSE when it isn't
      over any output of ours (X11: on another screen). */
    gboolean (*pointer_position)(gint *x, gint *y);
    /*! Move the pointer there. swallow: drop the motion this makes, so the
      core doesn't take it for the user's. */
    void (*pointer_warp)(gint x, gint y, gboolean swallow);
    void (*set_cursor)(ObCanvasId c, ObCursor cursor);

    /* --- state for pagers and taskbars --- */

    /*! Managed surfaces in the order they were mapped. */
    void (*publish_clients)(const ObSurfaceId *mapped, guint n);
    /*! Managed surfaces, bottom first. */
    void (*publish_stacking)(const ObSurfaceId *stacked, guint n);
    /*! OB_SURFACE_NONE when nothing has the focus. */
    void (*publish_active)(ObSurfaceId s);
    void (*publish_desktop_count)(guint count);
    void (*publish_current_desktop)(guint desktop);
    /*! NULL terminated. */
    void (*publish_desktop_names)(const gchar *const *names);
    /*! desktop is DESKTOP_ALL for a surface on every desktop. */
    void (*publish_surface_desktop)(ObSurfaceId s, guint desktop);
    /*! The icon the core gave it, when it has none of its own. */
    void (*publish_surface_icon)(ObSurfaceId s, const ObIcon *icon);
    /*! The title the core shows for it, with any " (host)" or
      "Not Responding"; icon is NULL to leave that one as it is. */
    void (*publish_surface_titles)(ObSurfaceId s, const gchar *title,
                                   const gchar *icon);
    /*! One area per desktop. */
    void (*publish_workareas)(const Rect *areas, guint n);
};

/*! What a backend tells the core. The core fills it in; the backend calls
  it from its event source. */
struct _ObBackendEvents {
    /*! A client wants to be shown (X11 MapRequest, a first buffer on
      Wayland). The core asks surface_info() about a new one and decides
      where, configures it and shows it, or leaves it alone (for one, when
      it is a dock app); one it already manages is shown again if it was
      iconified. */
    void (*surface_new)(ObSurfaceId s);
    /*! The client is gone (destroyed, or on X11 reparented away). */
    void (*surface_gone)(ObSurfaceId s);
    /*! The client changed what it says about itself; ask surface_info()
      for the new values. time: when, in event time. */
    void (*surface_changed)(ObSurfaceId s, ObSurfaceInfoFields changed,
                            guint32 time);
    /*! The client hid itself (X11 unmap, a null buffer on Wayland). */
    void (*surface_withdrawn)(ObSurfaceId s);
    void (*pong)(ObSurfaceId s, guint32 serial);
    /*! It drew the size sync_request() asked about. */
    void (*surface_synced)(ObSurfaceId s);

    /*! The client configures itself (X11 ConfigureRequest; on Wayland, a
      new size it commits on its own). */
    void (*request_configure)(ObSurfaceId s, const ObConfigureRequest *req);
    /*! A pager moves or resizes it (_NET_MOVERESIZE_WINDOW). Only the
      geometry and gravity fields are used. */
    void (*request_geometry)(ObSurfaceId s, const ObConfigureRequest *req,
                             ObRequestSource from);
    /*! A pager restacks it (_NET_RESTACK_WINDOW); sibling may be 0. */
    void (*request_restack)(ObSurfaceId s, guintptr sibling,
                            ObStackMode mode, ObRequestSource from);
    /*! first and second are one state each, or 0; they are applied in that
      order, as _NET_WM_STATE does. */
    void (*request_states)(ObSurfaceId s, ObStateAction action,
                           ObSurfaceStates first, ObSurfaceStates second,
                           ObRequestSource from);
    /*! The client asks to be iconified, or shown again (ICCCM
      WM_CHANGE_STATE, xdg_toplevel.set_minimized). */
    void (*request_iconify)(ObSurfaceId s, gboolean iconic);
    /*! time: when the requester says the user asked; 0 when it doesn't
      say. */
    void (*request_activate)(ObSurfaceId s, ObRequestSource from,
                             guint32 time);
    void (*request_close)(ObSurfaceId s, ObRequestSource from);
    /*! An interactive move or resize the client started (xdg_toplevel.move
      and resize, _NET_WM_MOVERESIZE), or OB_MOVERESIZE_CANCEL to stop the
      one going on. x, y: where the pointer is, in layout coordinates;
      button: the one held down, or 0 for the keyboard. */
    void (*request_move_resize)(ObSurfaceId s, ObMoveResize edge, gint x,
                                gint y, guint button);
    void (*request_desktop)(guint desktop, guint32 time);
    /*! desktop is DESKTOP_ALL for every desktop; it may be out of range. */
    void (*request_surface_desktop)(ObSurfaceId s, guint desktop);

    /*! The focus moved by itself (X11 only: a client took it, or it
      reverted). s has it now, or 0 when nothing the core manages has it. */
    void (*focus_changed)(ObSurfaceId s);
    /*! s lost the focus; the core stops showing it as focused unless it is
      the focused one again. */
    void (*focus_left)(ObSurfaceId s);
    /*! The focus went nowhere (X11: it reverted to the root or to a window
      that isn't managed); the core gives it to a window. refocus: the one
      that had it may get it back. */
    void (*focus_lost)(gboolean refocus);
    void (*key)(guint32 keysym, guint mods, gboolean press, guint32 time);
    /*! over: the canvas or surface under the pointer, x and y relative to
      it. */
    void (*button)(guintptr over, gint x, gint y, guint button, guint mods,
                   gboolean press, guint32 time);
    void (*motion)(guintptr over, gint x, gint y, guint mods, guint32 time);
    void (*enter)(guintptr over, gboolean entered, guint32 time);
    void (*outputs_changed)(void);
};

/*! The backend in use, set by backend_startup(). */
extern const ObBackend *ob_backend;
/*! What the core does with backend events. */
extern const ObBackendEvents ob_backend_events;

extern const ObBackend ob_backend_x11;

/*! Pick the backend and start it. */
gboolean backend_startup(const gchar *display, gboolean replace);
void backend_shutdown(void);
/*! Start delivering the backend's events to the main loop. */
void backend_listen(void);
/*! Free the strings in info and zero it. */
void backend_surface_info_clear(ObSurfaceInfo *info);

#endif
