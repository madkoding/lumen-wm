/* Lumen contract tests: the driver interface.

   The contract cases (cases.c) say what the window manager must do, in terms
   a client can see. They never touch a display protocol themselves: a driver
   creates the windows, sends the requests and reads back what the window
   manager did. driver-x11.c is the X11 driver; a Wayland driver implements the
   same functions, and a case that needs something a backend cannot offer
   asks for it with CT_REQUIRE(). */

#ifndef lumen__contract_driver_h
#define lumen__contract_driver_h

#include <stdbool.h>

typedef struct { int x, y, w, h; } ct_rect;

typedef struct ct_win ct_win;

/* What a backend lets a client do or see. X11 has them all; xdg-shell, for
   one, does not let a client place itself. */
typedef enum {
    CT_CAP_CLIENT_POSITION = 1 << 0, /* a client can ask for a position */
    CT_CAP_DESKTOPS        = 1 << 1, /* virtual desktops (workspaces) */
    CT_CAP_STRUTS          = 1 << 2, /* docks that reserve screen edges */
    CT_CAP_STACKING        = 1 << 3, /* the full stacking order is visible */
} ct_caps;

typedef enum {
    CT_STATE_MAXIMIZED,
    CT_STATE_FULLSCREEN,
    CT_STATE_ICONIC,
    CT_STATE_ABOVE,
} ct_state;

typedef enum {
    CT_WIN_NORMAL,
    CT_WIN_DOCK,   /* a panel; give it a strut with ct_window_set_strut() */
    CT_WIN_DIALOG, /* give it a parent with ct_window_set_parent() */
} ct_win_type;

/* Connect to the running window manager. Returns false when there is none. */
bool ct_driver_open(void);
void ct_driver_close(void);
const char *ct_driver_name(void);
ct_caps ct_driver_caps(void);

/* Handle what the window manager sent: configure, close requests... */
void ct_pump(void);

/* title may be NULL for a window that gives none */
ct_win *ct_window_new(ct_win_type type, const char *title, ct_rect want);
void ct_window_set_min_size(ct_win *w, int minw, int minh);
void ct_window_set_parent(ct_win *w, ct_win *parent);
void ct_window_set_title(ct_win *w, const char *title);
/* The application it belongs to (WM_CLASS on X11, app_id on Wayland). */
void ct_window_set_app_id(ct_win *w, const char *app_id);
/* Ask for no decorations from the window manager (Motif hints on X11,
   client-side xdg-decoration on Wayland). */
void ct_window_set_undecorated(ct_win *w);
/* Reserve this many pixels at the top of the screen (a dock). */
void ct_window_set_strut(ct_win *w, int top);
/* Before mapping: ask to show up in this state, or on this desktop. */
void ct_window_set_initial_state(ct_win *w, ct_state s);
void ct_window_set_initial_desktop(ct_win *w, int d);
void ct_window_map(ct_win *w);
/* The client hides the window by itself (ICCCM withdraw). */
void ct_window_withdraw(ct_win *w);
/* The handle stays valid, to ask what the window manager did. */
void ct_window_destroy(ct_win *w);

/* What the window manager did with the window. */
bool ct_window_managed(ct_win *w);
bool ct_window_viewable(ct_win *w);
bool ct_window_focused(ct_win *w);
bool ct_window_has_state(ct_win *w, ct_state s);
/* the client area, in root coordinates */
ct_rect ct_window_client(ct_win *w);
/* the client area plus the decorations */
ct_rect ct_window_frame(ct_win *w);
/* the title shown to taskbars is exactly this */
bool ct_window_title_is(ct_win *w, const char *title);
/* -1 for a window on every desktop */
int ct_window_desktop(ct_win *w);
/* position in the stacking order of managed windows, 0 being the bottom;
   -1 when it is not there */
int ct_window_stack_index(ct_win *w);
/* the window asked to close itself and is gone */
bool ct_window_closed(ct_win *w);

/* Requests, as a pager or a taskbar would send them. */
void ct_request_state(ct_win *w, ct_state s, bool on);
/* The window asks to go below every other window. */
void ct_request_lower(ct_win *w);
/* A pager raises the window, or moves and resizes it. */
void ct_pager_raise(ct_win *w);
void ct_pager_move_resize(ct_win *w, ct_rect r);
/* Turn the state off when it is on, on when it is off. */
void ct_request_toggle_state(ct_win *w, ct_state s);
void ct_request_activate(ct_win *w);
void ct_request_close(ct_win *w);
void ct_request_move_resize(ct_win *w, ct_rect r);
void ct_request_desktop(int d);
void ct_request_window_desktop(ct_win *w, int d);

ct_rect ct_screen(void);
ct_rect ct_workarea(void);
int ct_desktop_count(void);
int ct_current_desktop(void);

#endif
