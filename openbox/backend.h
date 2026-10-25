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
    OB_SURFACE_STATE_SKIP_PAGER     = 1 << 11
} ObSurfaceStates;

/*! Who asked, so focus stealing prevention can weigh it. */
typedef enum {
    OB_REQUEST_FROM_APPLICATION,
    OB_REQUEST_FROM_PAGER
} ObRequestSource;

/*! Which parts of ObSurfaceInfo changed. */
typedef enum {
    OB_SURFACE_INFO_TITLE    = 1 << 0,
    OB_SURFACE_INFO_CLASS    = 1 << 1,
    OB_SURFACE_INFO_TYPE     = 1 << 2,
    OB_SURFACE_INFO_PARENT   = 1 << 3,
    OB_SURFACE_INFO_SIZE     = 1 << 4,
    OB_SURFACE_INFO_STRUT    = 1 << 5,
    OB_SURFACE_INFO_ICON     = 1 << 6,
    OB_SURFACE_INFO_URGENT   = 1 << 7,
    OB_SURFACE_INFO_DECOR    = 1 << 8,
    OB_SURFACE_INFO_ALL      = (1 << 9) - 1
} ObSurfaceInfoFields;

/*! What a client says about itself. The backend reads it from WM_NORMAL_HINTS,
  _NET_WM_* and friends, or from xdg_toplevel requests. */
typedef struct _ObSurfaceInfo {
    gchar *title;
    gchar *app_id;          /*!< WM_CLASS class, or xdg app_id */
    gchar *instance;        /*!< WM_CLASS name; NULL on Wayland */
    gchar *role;            /*!< WM_WINDOW_ROLE; NULL on Wayland */
    ObSurfaceType type;
    ObSurfaceId parent;     /*!< transient for, or xdg parent */
    gboolean modal;

    gboolean has_position;  /*!< the client asked for want.x and want.y */
    Rect want;              /*!< where and how big it would like to be */
    Size min, max, base, inc;
    gfloat min_ratio, max_ratio; /*!< 0 for none */

    StrutPartial strut;     /*!< docks and panels; zero for none */
    gboolean undecorated;   /*!< asks for no decorations of ours */
    gboolean urgent;

    gboolean accepts_focus; /*!< WM_HINTS input */
    gboolean takes_focus;   /*!< WM_TAKE_FOCUS */
    gboolean can_close;     /*!< WM_DELETE_WINDOW; always on Wayland */
    gint pid;               /*!< 0 when not known */
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

    guint (*output_count)(void);
    void (*output_area)(guint output, Rect *area);

    /* --- managed surfaces --- */

    /*! Place the client's own area, in layout coordinates, and tell it its
      size. The frame around it is placed with canvas_place(). */
    void (*surface_configure)(ObSurfaceId s, const Rect *area);
    void (*surface_show)(ObSurfaceId s, gboolean show);
    /*! Give it the keyboard. Returns FALSE when it can't take it. */
    gboolean (*surface_focus)(ObSurfaceId s, guint32 time);
    /*! Ask it to close; it may refuse or hang. */
    void (*surface_close)(ObSurfaceId s, guint32 time);
    /*! Disconnect its client. */
    void (*surface_kill)(ObSurfaceId s);
    void (*surface_set_states)(ObSurfaceId s, ObSurfaceStates states);
    /*! Ask whether it still answers; the answer comes as pong(). */
    void (*surface_ping)(ObSurfaceId s, guint32 serial);

    /* --- stacking --- */

    /*! Set the stacking order of everything the core shows: managed
      surfaces together with the canvases they own, bottom first. A canvas
      id and a surface id never collide. */
    void (*restack)(const guintptr *bottom_to_top, guint n);

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

    /* --- input --- */

    gboolean (*grab_pointer)(gboolean grab, ObCursor cursor);
    gboolean (*grab_keyboard)(gboolean grab);
    /*! Make sure a key binding reaches the core whichever surface has the
      focus. keysym and modifiers are xkbcommon's. */
    void (*bind_key)(guint32 keysym, guint mods);
    void (*unbind_all_keys)(void);
    void (*pointer_position)(gint *x, gint *y);
    void (*pointer_warp)(gint x, gint y);
    void (*set_cursor)(ObCanvasId c, ObCursor cursor);

    /* --- state for pagers and taskbars --- */

    /*! Managed surfaces in the order they were mapped, and bottom first. */
    void (*publish_clients)(const ObSurfaceId *mapped, guint n,
                            const ObSurfaceId *stacked, guint m);
    void (*publish_active)(ObSurfaceId s);
    void (*publish_desktops)(guint count, guint current,
                             const gchar *const *names);
    void (*publish_surface_desktop)(ObSurfaceId s, guint desktop);
    /*! One area per desktop. */
    void (*publish_workareas)(const Rect *areas, guint n);
};

/*! What a backend tells the core. The core fills it in; the backend calls
  it from its event source. */
struct _ObBackendEvents {
    /*! A client wants to be shown. The core decides where, configures it and
      shows it, or leaves it alone (for one, when it is a dock app). */
    void (*surface_new)(ObSurfaceId s, const ObSurfaceInfo *info);
    void (*surface_gone)(ObSurfaceId s);
    void (*surface_changed)(ObSurfaceId s, const ObSurfaceInfo *info,
                            ObSurfaceInfoFields changed);
    /*! The client hid itself (X11 unmap, a null buffer on Wayland). */
    void (*surface_withdrawn)(ObSurfaceId s);
    void (*pong)(ObSurfaceId s, guint32 serial);

    /*! fields: which of want's x, y, width, height it asked for, as
      1 << 0 .. 1 << 3. */
    void (*request_configure)(ObSurfaceId s, const Rect *want, guint fields);
    void (*request_states)(ObSurfaceId s, ObSurfaceStates on,
                           ObSurfaceStates off, ObRequestSource from);
    void (*request_activate)(ObSurfaceId s, ObRequestSource from,
                             guint32 time);
    void (*request_close)(ObSurfaceId s, ObRequestSource from);
    /*! An interactive move or resize the client started (xdg_toplevel.move,
      _NET_WM_MOVERESIZE). cursor names the edge, or OB_CURSOR_MOVE. */
    void (*request_move_resize)(ObSurfaceId s, ObCursor cursor,
                                guint32 time);
    void (*request_desktop)(guint desktop, guint32 time);
    void (*request_surface_desktop)(ObSurfaceId s, guint desktop);

    /*! Another surface took the focus by itself (X11 only). */
    void (*focus_changed)(ObSurfaceId s);
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

#endif
