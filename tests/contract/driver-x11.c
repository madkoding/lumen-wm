/* Lumen contract tests: the X11 driver.

   Plays a client and a pager at once: it creates plain Xlib windows, sends
   the EWMH requests a pager or a taskbar sends, and reads back what the window
   manager did through the EWMH root and window properties. */

#include "driver.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#ifdef SYNC
#include <X11/extensions/sync.h>
#endif
#ifdef CT_HAVE_XTEST
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct ct_win {
    Window id;
    bool destroyed, closed, clicked;
    unsigned long sync_counter; /* an XSyncCounter, or 0 */
    int sync_requests;
    XSizeHints hints;
    ct_win *next;
};

static Display *dpy;
static Window root;
static ct_win *windows;

enum {
    WM_PROTOCOLS, WM_DELETE_WINDOW,
    NET_SUPPORTING_WM_CHECK, NET_CLIENT_LIST, NET_CLIENT_LIST_STACKING,
    NET_ACTIVE_WINDOW, NET_CLOSE_WINDOW, NET_WORKAREA,
    NET_NUMBER_OF_DESKTOPS, NET_CURRENT_DESKTOP, NET_WM_DESKTOP,
    NET_WM_NAME, NET_FRAME_EXTENTS, NET_WM_STRUT, NET_WM_STRUT_PARTIAL,
    NET_WM_STATE, NET_WM_STATE_MAXIMIZED_VERT, NET_WM_STATE_MAXIMIZED_HORZ,
    NET_WM_STATE_FULLSCREEN, NET_WM_STATE_HIDDEN, NET_WM_STATE_ABOVE,
    NET_WM_WINDOW_TYPE, NET_WM_WINDOW_TYPE_NORMAL, NET_WM_WINDOW_TYPE_DOCK,
    NET_WM_WINDOW_TYPE_DIALOG, UTF8_STRING, NET_WM_VISIBLE_NAME,
    MOTIF_WM_HINTS, NET_RESTACK_WINDOW, NET_MOVERESIZE_WINDOW,
    NET_WM_MOVERESIZE, NET_WM_SYNC_REQUEST, NET_WM_SYNC_REQUEST_COUNTER,
    NET_WM_WINDOW_OPACITY, NET_SUPPORTED, NET_WM_STATE_MAX,
    NUM_ATOMS
};

static char *atom_names[NUM_ATOMS] = {
    "WM_PROTOCOLS", "WM_DELETE_WINDOW",
    "_NET_SUPPORTING_WM_CHECK", "_NET_CLIENT_LIST", "_NET_CLIENT_LIST_STACKING",
    "_NET_ACTIVE_WINDOW", "_NET_CLOSE_WINDOW", "_NET_WORKAREA",
    "_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP", "_NET_WM_DESKTOP",
    "_NET_WM_NAME", "_NET_FRAME_EXTENTS", "_NET_WM_STRUT",
    "_NET_WM_STRUT_PARTIAL",
    "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_VERT",
    "_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_FULLSCREEN",
    "_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_ABOVE",
    "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_NORMAL",
    "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DIALOG", "UTF8_STRING",
    "_NET_WM_VISIBLE_NAME", "_MOTIF_WM_HINTS", "_NET_RESTACK_WINDOW",
    "_NET_MOVERESIZE_WINDOW", "_NET_WM_MOVERESIZE", "_NET_WM_SYNC_REQUEST",
    "_NET_WM_SYNC_REQUEST_COUNTER", "_NET_WM_WINDOW_OPACITY",
    "_NET_SUPPORTED", "_NET_WM_STATE",
};

static Atom atoms[NUM_ATOMS];

/* a window can be gone by the time we ask about it */
static int ignore_errors(Display *d, XErrorEvent *e)
{
    (void)d; (void)e;
    return 0;
}

/* Reads a 32-bit property; returns how many items, *out to be XFree()d. */
static unsigned long get_prop(Window w, int prop, Atom type,
                              unsigned long **out)
{
    Atom ret_type;
    int format;
    unsigned long n = 0, after;
    unsigned char *data = NULL;

    *out = NULL;
    if (XGetWindowProperty(dpy, w, atoms[prop], 0, 1024, False, type,
                           &ret_type, &format, &n, &after, &data) != Success
        || format != 32 || !data) {
        if (data) XFree(data);
        return 0;
    }
    *out = (unsigned long *)data;
    return n;
}

static long get_card(Window w, int prop, long fallback)
{
    unsigned long *v;
    long r = fallback;
    if (get_prop(w, prop, AnyPropertyType, &v) >= 1) r = (long)v[0];
    if (v) XFree(v);
    return r;
}

static int index_in(Window w, int prop)
{
    unsigned long *v;
    unsigned long n = get_prop(root, prop, XA_WINDOW, &v);
    int r = -1;
    for (unsigned long i = 0; i < n; ++i)
        if (v[i] == w) { r = (int)i; break; }
    if (v) XFree(v);
    return r;
}

static void send_root(Window w, int type, long a, long b, long c, long d)
{
    XEvent e;
    memset(&e, 0, sizeof(e));
    e.xclient.type = ClientMessage;
    e.xclient.window = w;
    e.xclient.message_type = atoms[type];
    e.xclient.format = 32;
    e.xclient.data.l[0] = a;
    e.xclient.data.l[1] = b;
    e.xclient.data.l[2] = c;
    e.xclient.data.l[3] = d;
    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(dpy);
}

bool ct_driver_open(void)
{
    unsigned long *check;
    struct timespec step = { 0, 100 * 1000000 };

    if (!(dpy = XOpenDisplay(NULL))) return false;
    XSetErrorHandler(ignore_errors);
    root = DefaultRootWindow(dpy);
    XInternAtoms(dpy, atom_names, NUM_ATOMS, False, atoms);

    /* give a window manager that is starting up five seconds */
    for (int i = 0; i < 50; ++i) {
        if (get_prop(root, NET_SUPPORTING_WM_CHECK, XA_WINDOW, &check) >= 1) {
            XFree(check);
            /* and a moment to set up the rest of the root properties */
            nanosleep(&step, NULL);
            return true;
        }
        nanosleep(&step, NULL);
    }
    return false;
}

void ct_driver_close(void)
{
    while (windows) {
        ct_win *w = windows;
        windows = w->next;
        free(w);
    }
    XCloseDisplay(dpy);
}

const char *ct_driver_name(void)
{
    return "x11";
}

ct_caps ct_driver_caps(void)
{
    return CT_CAP_CLIENT_POSITION | CT_CAP_DESKTOPS | CT_CAP_STRUTS |
        CT_CAP_STACKING | CT_CAP_CLIENT_FOCUS | CT_CAP_OPACITY
#ifdef CT_HAVE_XTEST
        | CT_CAP_INPUT
#endif
#ifdef SYNC
        | CT_CAP_SYNC_RESIZE
#endif
        ;
}

void ct_pump(void)
{
    XSync(dpy, False);
    while (XPending(dpy)) {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == ButtonPress)
            for (ct_win *w = windows; w; w = w->next)
                if (w->id == e.xbutton.window) w->clicked = true;
#ifdef SYNC
        if (e.type == ClientMessage &&
            e.xclient.message_type == atoms[WM_PROTOCOLS] &&
            (Atom)e.xclient.data.l[0] == atoms[NET_WM_SYNC_REQUEST])
            for (ct_win *w = windows; w; w = w->next)
                if (w->id == e.xclient.window && w->sync_counter) {
                    /* "drawn": move the counter to the value asked for */
                    XSyncValue v;
                    XSyncIntsToValue(&v, (unsigned)e.xclient.data.l[2],
                                     (int)e.xclient.data.l[3]);
                    XSyncSetCounter(dpy, w->sync_counter, v);
                    ++w->sync_requests;
                }
#endif
        if (e.type != ClientMessage ||
            e.xclient.message_type != atoms[WM_PROTOCOLS] ||
            (Atom)e.xclient.data.l[0] != atoms[WM_DELETE_WINDOW])
            continue;
        /* a well behaved client closes when asked */
        for (ct_win *w = windows; w; w = w->next)
            if (w->id == e.xclient.window && !w->destroyed) {
                XDestroyWindow(dpy, w->id);
                w->destroyed = w->closed = true;
            }
    }
    XFlush(dpy);
}

static void set_type(ct_win *w, int type)
{
    Atom a = atoms[type];
    XChangeProperty(dpy, w->id, atoms[NET_WM_WINDOW_TYPE], XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&a, 1);
}

ct_win *ct_window_new(ct_win_type type, const char *title, ct_rect want)
{
    ct_win *w = calloc(1, sizeof(*w));
    XWMHints wmhints = { .flags = InputHint, .input = True };
    Atom protocols[] = { atoms[WM_DELETE_WINDOW] };

    w->id = XCreateSimpleWindow(dpy, root, want.x, want.y, want.w, want.h, 0,
                                0, WhitePixel(dpy, DefaultScreen(dpy)));
    if (type == CT_WIN_POPUP) {
        XSetWindowAttributes attr = { .override_redirect = True };
        XChangeWindowAttributes(dpy, w->id, CWOverrideRedirect, &attr);
    }
    XSelectInput(dpy, w->id,
                 StructureNotifyMask | PropertyChangeMask | ButtonPressMask);
    if (title)
        ct_window_set_title(w, title);
    XSetWMHints(dpy, w->id, &wmhints);
    XSetWMProtocols(dpy, w->id, protocols, 1);

    w->hints.flags = PPosition | PSize;
    w->hints.x = want.x;
    w->hints.y = want.y;
    w->hints.width = want.w;
    w->hints.height = want.h;
    XSetWMNormalHints(dpy, w->id, &w->hints);

    set_type(w, type == CT_WIN_DOCK ? NET_WM_WINDOW_TYPE_DOCK :
             type == CT_WIN_DIALOG ? NET_WM_WINDOW_TYPE_DIALOG :
             NET_WM_WINDOW_TYPE_NORMAL);

    w->next = windows;
    windows = w;
    return w;
}

void ct_window_set_min_size(ct_win *w, int minw, int minh)
{
    w->hints.flags |= PMinSize;
    w->hints.min_width = minw;
    w->hints.min_height = minh;
    XSetWMNormalHints(dpy, w->id, &w->hints);
}

void ct_window_set_parent(ct_win *w, ct_win *parent)
{
    XSetTransientForHint(dpy, w->id, parent->id);
}

void ct_window_set_sync(ct_win *w)
{
#ifdef SYNC
    Atom protocols[] = { atoms[WM_DELETE_WINDOW], atoms[NET_WM_SYNC_REQUEST] };
    XSyncValue zero;
    int ev, err;
    unsigned long id;

    if (!XSyncQueryExtension(dpy, &ev, &err)) return;
    XSyncIntToValue(&zero, 0);
    w->sync_counter = XSyncCreateCounter(dpy, zero);
    id = w->sync_counter;
    XChangeProperty(dpy, w->id, atoms[NET_WM_SYNC_REQUEST_COUNTER],
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&id, 1);
    XSetWMProtocols(dpy, w->id, protocols, 2);
#else
    (void)w;
#endif
}

void ct_window_set_opacity(ct_win *w, double opacity)
{
    unsigned long v = (unsigned long)(opacity * 0xffffffffu);

    if (opacity < 0)
        XDeleteProperty(dpy, w->id, atoms[NET_WM_WINDOW_OPACITY]);
    else
        XChangeProperty(dpy, w->id, atoms[NET_WM_WINDOW_OPACITY],
                        XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *)&v, 1);
    XFlush(dpy);
}

/* The window's frame: its ancestor right below the root. */
static Window top_level(Window win)
{
    Window rt, parent, *children;
    unsigned int n;

    while (XQueryTree(dpy, win, &rt, &parent, &children, &n)) {
        if (children) XFree(children);
        if (parent == root || parent == None) break;
        win = parent;
    }
    return win;
}

double ct_window_shown_opacity(ct_win *w)
{
    unsigned long *v;
    double r = -1;

    /* compositors read it off the frame, so the window manager copies it
       there */
    if (get_prop(top_level(w->id), NET_WM_WINDOW_OPACITY, XA_CARDINAL, &v))
        r = (double)(v[0] & 0xffffffffu) / 0xffffffffu;
    if (v) XFree(v);
    return r;
}

int ct_window_sync_requests(ct_win *w)
{
    ct_pump();
    return w->sync_requests;
}

void ct_window_set_undecorated(ct_win *w)
{
    /* flags: MWM_HINTS_DECORATIONS; decorations: none */
    long hints[5] = { 1 << 1, 0, 0, 0, 0 };
    XChangeProperty(dpy, w->id, atoms[MOTIF_WM_HINTS], atoms[MOTIF_WM_HINTS],
                    32, PropModeReplace, (unsigned char *)hints, 5);
}

void ct_window_set_app_id(ct_win *w, const char *app_id)
{
    XClassHint hint = { .res_name = (char *)"contract",
                        .res_class = (char *)app_id };
    XSetClassHint(dpy, w->id, &hint);
}

void ct_window_set_initial_state(ct_win *w, ct_state s)
{
    Atom st[2];
    int n = 0;

    switch (s) {
    case CT_STATE_MAXIMIZED:
        st[n++] = atoms[NET_WM_STATE_MAXIMIZED_VERT];
        st[n++] = atoms[NET_WM_STATE_MAXIMIZED_HORZ];
        break;
    case CT_STATE_FULLSCREEN: st[n++] = atoms[NET_WM_STATE_FULLSCREEN]; break;
    case CT_STATE_ICONIC:     st[n++] = atoms[NET_WM_STATE_HIDDEN]; break;
    case CT_STATE_ABOVE:      st[n++] = atoms[NET_WM_STATE_ABOVE]; break;
    }
    XChangeProperty(dpy, w->id, atoms[NET_WM_STATE], XA_ATOM, 32,
                    PropModeAppend, (unsigned char *)st, n);
}

void ct_window_set_initial_desktop(ct_win *w, int d)
{
    long v = d;
    XChangeProperty(dpy, w->id, atoms[NET_WM_DESKTOP], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&v, 1);
}

void ct_window_set_title(ct_win *w, const char *title)
{
    XStoreName(dpy, w->id, title);
    XChangeProperty(dpy, w->id, atoms[NET_WM_NAME], atoms[UTF8_STRING], 8,
                    PropModeReplace, (const unsigned char *)title,
                    (int)strlen(title));
}

void ct_window_set_strut(ct_win *w, int top)
{
    long strut[12] = { 0 };
    strut[2] = top;                              /* top */
    strut[8] = 0;                                /* top_start_x */
    strut[9] = DisplayWidth(dpy, DefaultScreen(dpy)) - 1; /* top_end_x */
    XChangeProperty(dpy, w->id, atoms[NET_WM_STRUT_PARTIAL], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)strut, 12);
    XChangeProperty(dpy, w->id, atoms[NET_WM_STRUT], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)strut, 4);
}

void ct_window_map(ct_win *w)
{
    XMapWindow(dpy, w->id);
    XFlush(dpy);
}

void ct_window_take_focus(ct_win *w)
{
    XSetInputFocus(dpy, w->id, RevertToPointerRoot, CurrentTime);
    XFlush(dpy);
}

void ct_window_withdraw(ct_win *w)
{
    XWithdrawWindow(dpy, w->id, DefaultScreen(dpy));
    XFlush(dpy);
}

void ct_window_destroy(ct_win *w)
{
    if (!w->destroyed) XDestroyWindow(dpy, w->id);
    w->destroyed = true;
    XFlush(dpy);
}

bool ct_window_managed(ct_win *w)
{
    return index_in(w->id, NET_CLIENT_LIST) >= 0;
}

bool ct_window_viewable(ct_win *w)
{
    XWindowAttributes a;
    if (w->destroyed || !XGetWindowAttributes(dpy, w->id, &a)) return false;
    return a.map_state == IsViewable;
}

bool ct_window_clicked(ct_win *w)
{
    ct_pump();
    return w->clicked;
}

bool ct_window_focused(ct_win *w)
{
    return !w->destroyed &&
        (Window)get_card(root, NET_ACTIVE_WINDOW, 0) == w->id;
}

static bool ct_has_name(Window w)
{
    Atom ret_type;
    int format;
    unsigned long n = 0, after;
    unsigned char *data = NULL;

    XGetWindowProperty(dpy, w, atoms[NET_WM_NAME], 0, 64, False,
                       atoms[UTF8_STRING], &ret_type, &format, &n, &after,
                       &data);
    if (data) XFree(data);
    return n > 0;
}

bool ct_wm_announced(void)
{
    unsigned long *check = NULL, *self = NULL, *supported = NULL;
    unsigned long n;
    char sel[16];
    Window owner;
    bool r = false;

    /* ICCCM: it holds WM_Sn with the window it names in
       _NET_SUPPORTING_WM_CHECK, and that window names itself (EWMH) */
    snprintf(sel, sizeof(sel), "WM_S%d", DefaultScreen(dpy));
    owner = XGetSelectionOwner(dpy, XInternAtom(dpy, sel, False));
    if (owner != None &&
        get_prop(root, NET_SUPPORTING_WM_CHECK, XA_WINDOW, &check) >= 1 &&
        check[0] == owner &&
        get_prop(owner, NET_SUPPORTING_WM_CHECK, XA_WINDOW, &self) >= 1 &&
        self[0] == owner &&
        ct_has_name(owner))
    {
        /* and it says what it supports */
        n = get_prop(root, NET_SUPPORTED, XA_ATOM, &supported);
        for (unsigned long i = 0; i < n; ++i)
            if (supported[i] == atoms[NET_WM_STATE_MAX]) r = true;
    }
    if (check) XFree(check);
    if (self) XFree(self);
    if (supported) XFree(supported);
    return r;
}

bool ct_nothing_focused(void)
{
    Window f;
    int revert;

    if (get_card(root, NET_ACTIVE_WINDOW, 0) != 0) return false;
    XGetInputFocus(dpy, &f, &revert);
    for (ct_win *w = windows; w; w = w->next)
        if (!w->destroyed && w->id == f) return false;
    /* keys go to the window manager, not to whatever is under the
       pointer */
    return f != None && f != PointerRoot;
}

static bool has_atom(ct_win *w, int state)
{
    unsigned long *v;
    unsigned long n = get_prop(w->id, NET_WM_STATE, XA_ATOM, &v);
    bool r = false;
    for (unsigned long i = 0; i < n; ++i)
        if (v[i] == atoms[state]) r = true;
    if (v) XFree(v);
    return r;
}

bool ct_window_has_state(ct_win *w, ct_state s)
{
    switch (s) {
    case CT_STATE_MAXIMIZED:
        return has_atom(w, NET_WM_STATE_MAXIMIZED_VERT) &&
            has_atom(w, NET_WM_STATE_MAXIMIZED_HORZ);
    case CT_STATE_FULLSCREEN: return has_atom(w, NET_WM_STATE_FULLSCREEN);
    case CT_STATE_ICONIC:     return has_atom(w, NET_WM_STATE_HIDDEN);
    case CT_STATE_ABOVE:      return has_atom(w, NET_WM_STATE_ABOVE);
    }
    return false;
}

ct_rect ct_window_client(ct_win *w)
{
    ct_rect r = { 0, 0, 0, 0 };
    Window rt, child;
    int x, y;
    unsigned int width, height, border, depth;

    if (w->destroyed ||
        !XGetGeometry(dpy, w->id, &rt, &x, &y, &width, &height, &border,
                      &depth) ||
        !XTranslateCoordinates(dpy, w->id, root, 0, 0, &r.x, &r.y, &child))
        return r;
    r.w = (int)width;
    r.h = (int)height;
    return r;
}

ct_rect ct_window_frame(ct_win *w)
{
    ct_rect r = ct_window_client(w);
    unsigned long *e;
    if (get_prop(w->id, NET_FRAME_EXTENTS, XA_CARDINAL, &e) == 4) {
        /* left, right, top, bottom */
        r.x -= (int)e[0];
        r.y -= (int)e[2];
        r.w += (int)(e[0] + e[1]);
        r.h += (int)(e[2] + e[3]);
    }
    if (e) XFree(e);
    return r;
}

bool ct_window_title_is(ct_win *w, const char *title)
{
    Atom ret_type;
    int format;
    unsigned long n = 0, after;
    unsigned char *data = NULL;
    bool same;

    if (XGetWindowProperty(dpy, w->id, atoms[NET_WM_VISIBLE_NAME], 0, 1024,
                           False, atoms[UTF8_STRING], &ret_type, &format, &n,
                           &after, &data) != Success || format != 8 || !data) {
        if (data) XFree(data);
        return false;
    }
    same = n == strlen(title) && !memcmp(data, title, n);
    XFree(data);
    return same;
}

int ct_window_desktop(ct_win *w)
{
    long d = get_card(w->id, NET_WM_DESKTOP, -2);
    return d == 0xFFFFFFFFL || d == -1 ? -1 : (int)d;
}

int ct_window_stack_index(ct_win *w)
{
    return index_in(w->id, NET_CLIENT_LIST_STACKING);
}

bool ct_window_closed(ct_win *w)
{
    return w->closed;
}

static void request_state(ct_win *w, ct_state s, long action)
{
    int a = 0, b = 0;

    switch (s) {
    case CT_STATE_ICONIC:     a = NET_WM_STATE_HIDDEN; break;
    case CT_STATE_MAXIMIZED:
        a = NET_WM_STATE_MAXIMIZED_VERT;
        b = NET_WM_STATE_MAXIMIZED_HORZ;
        break;
    case CT_STATE_FULLSCREEN: a = NET_WM_STATE_FULLSCREEN; break;
    case CT_STATE_ABOVE:      a = NET_WM_STATE_ABOVE; break;
    }
    /* source 2: a pager, so no focus stealing prevention gets in the way */
    send_root(w->id, NET_WM_STATE, action, (long)atoms[a],
              b ? (long)atoms[b] : 0, 2);
}

void ct_request_state(ct_win *w, ct_state s, bool on)
{
    if (s == CT_STATE_ICONIC) {
        /* ICCCM: iconify through WM_CHANGE_STATE; deiconify by activating */
        if (on) {
            XIconifyWindow(dpy, w->id, DefaultScreen(dpy));
            XFlush(dpy);
        } else
            ct_request_activate(w);
        return;
    }
    request_state(w, s, on ? 1 : 0); /* _NET_WM_STATE_ADD or _REMOVE */
}

void ct_request_toggle_state(ct_win *w, ct_state s)
{
    request_state(w, s, 2); /* _NET_WM_STATE_TOGGLE */
}

void ct_request_activate(ct_win *w)
{
    send_root(w->id, NET_ACTIVE_WINDOW, 2, CurrentTime, 0, 0);
}

void ct_request_close(ct_win *w)
{
    send_root(w->id, NET_CLOSE_WINDOW, CurrentTime, 2, 0, 0);
}

void ct_request_move_resize(ct_win *w, ct_rect r)
{
    /* the client configures itself; the window manager decides */
    XMoveResizeWindow(dpy, w->id, r.x, r.y, (unsigned)r.w, (unsigned)r.h);
    XFlush(dpy);
}

void ct_request_keyboard_move(ct_win *w)
{
    /* direction 10: MOVE_KEYBOARD; no pointer position or button */
    send_root(w->id, NET_WM_MOVERESIZE, 0, 0, 10, 0);
}

void ct_request_lower(ct_win *w)
{
    XWindowChanges ch = { .stack_mode = Below };
    XConfigureWindow(dpy, w->id, CWStackMode, &ch);
    XFlush(dpy);
}

void ct_pager_raise(ct_win *w)
{
    /* source 2: a pager; no sibling */
    send_root(w->id, NET_RESTACK_WINDOW, 2, 0, Above, 0);
}

void ct_pager_move_resize(ct_win *w, ct_rect r)
{
    XEvent e;
    memset(&e, 0, sizeof(e));
    e.xclient.type = ClientMessage;
    e.xclient.window = w->id;
    e.xclient.message_type = atoms[NET_MOVERESIZE_WINDOW];
    e.xclient.format = 32;
    /* x, y, width and height given, from a pager, NorthWest gravity */
    e.xclient.data.l[0] = NorthWestGravity | 0xf << 8 | 2 << 12;
    e.xclient.data.l[1] = r.x;
    e.xclient.data.l[2] = r.y;
    e.xclient.data.l[3] = r.w;
    e.xclient.data.l[4] = r.h;
    XSendEvent(dpy, root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &e);
    XFlush(dpy);
}

void ct_request_desktop(int d)
{
    send_root(root, NET_CURRENT_DESKTOP, d, CurrentTime, 0, 0);
}

void ct_request_window_desktop(ct_win *w, int d)
{
    send_root(w->id, NET_WM_DESKTOP, d, 2, 0, 0);
}

ct_rect ct_screen(void)
{
    int s = DefaultScreen(dpy);
    return (ct_rect){ 0, 0, DisplayWidth(dpy, s), DisplayHeight(dpy, s) };
}

ct_rect ct_workarea(void)
{
    unsigned long *v;
    unsigned long n = get_prop(root, NET_WORKAREA, XA_CARDINAL, &v);
    unsigned long d = (unsigned long)ct_current_desktop();
    ct_rect r = ct_screen();
    if (n >= (d + 1) * 4)
        r = (ct_rect){ (int)v[d * 4], (int)v[d * 4 + 1],
                       (int)v[d * 4 + 2], (int)v[d * 4 + 3] };
    if (v) XFree(v);
    return r;
}

int ct_desktop_count(void)
{
    return (int)get_card(root, NET_NUMBER_OF_DESKTOPS, 1);
}

int ct_current_desktop(void)
{
    return (int)get_card(root, NET_CURRENT_DESKTOP, 0);
}

#ifdef CT_HAVE_XTEST
/* Leave a person's gap between two key or button events. */
static void fake_sync(void)
{
    struct timespec gap = { 0, 20 * 1000 * 1000 };
    XSync(dpy, False);
    nanosleep(&gap, NULL);
}

/* Press (or release) the modifiers in mods, a string of C, A, S and W. */
static void fake_mods(const char *mods, size_t n, Bool press)
{
    for (size_t i = 0; i < n; ++i) {
        KeySym sym = mods[i] == 'C' ? XK_Control_L :
            mods[i] == 'A' ? XK_Alt_L :
            mods[i] == 'S' ? XK_Shift_L :
            mods[i] == 'W' ? XK_Super_L : NoSymbol;
        if (sym == NoSymbol) continue;
        XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, sym), press, 0);
        fake_sync();
    }
}

/* "C-A-Right" -> mods "CA", key "Right" */
static const char *split_keys(const char *keys, char *mods, size_t *n)
{
    *n = 0;
    while (keys[0] && keys[1] == '-') {
        mods[(*n)++] = keys[0];
        keys += 2;
    }
    return keys;
}

void ct_input_keys(const char *keys)
{
    char mods[8];
    size_t n;
    const char *name = split_keys(keys, mods, &n);
    KeyCode code = XKeysymToKeycode(dpy, XStringToKeysym(name));

    fake_mods(mods, n, True);
    XTestFakeKeyEvent(dpy, code, True, 0);
    fake_sync();
    XTestFakeKeyEvent(dpy, code, False, 0);
    fake_sync();
    /* release in the opposite order */
    for (size_t i = n; i > 0; --i)
        fake_mods(&mods[i - 1], 1, False);
    XFlush(dpy);
}

void ct_input_drag(ct_win *w, const char *mods, int dx, int dy)
{
    ct_rect c = ct_window_client(w);
    int x = c.x + c.w / 2, y = c.y + c.h / 2;
    const int steps = 10;

    XTestFakeMotionEvent(dpy, -1, x, y, 0);
    fake_mods(mods, strlen(mods), True);
    XTestFakeButtonEvent(dpy, 1, True, 0);
    fake_sync();
    for (int i = 1; i <= steps; ++i) {
        XTestFakeMotionEvent(dpy, -1, x + dx * i / steps, y + dy * i / steps,
                             0);
        fake_sync();
    }
    XTestFakeButtonEvent(dpy, 1, False, 0);
    fake_sync();
    fake_mods(mods, strlen(mods), False);
    XFlush(dpy);
}
void ct_input_client_drag(ct_win *w, ct_edge edge, int dx, int dy)
{
    /* _NET_WM_MOVERESIZE directions */
    const long dir = edge == CT_EDGE_MOVE ? 8 : 4;
    ct_rect c = ct_window_client(w);
    int x = c.x + c.w / 2, y = c.y + c.h / 2;
    const int steps = 10;

    XTestFakeMotionEvent(dpy, -1, x, y, 0);
    fake_sync();
    XTestFakeButtonEvent(dpy, 1, True, 0);
    fake_sync();
    /* as toolkits do: let go of the press's implicit grab, then ask */
    XUngrabPointer(dpy, CurrentTime);
    send_root(w->id, NET_WM_MOVERESIZE, x, y, dir, 1);
    fake_sync();
    for (int i = 1; i <= steps; ++i) {
        XTestFakeMotionEvent(dpy, -1, x + dx * i / steps, y + dy * i / steps,
                             0);
        fake_sync();
        /* a client keeps answering while it is dragged */
        ct_pump();
    }
    XTestFakeButtonEvent(dpy, 1, False, 0);
    fake_sync();
}

void ct_input_click(ct_win *w, int x, int y)
{
    ct_rect c = ct_window_client(w);

    XTestFakeMotionEvent(dpy, -1, c.x + x, c.y + y, 0);
    fake_sync();
    XTestFakeButtonEvent(dpy, 1, True, 0);
    fake_sync();
    XTestFakeButtonEvent(dpy, 1, False, 0);
    XFlush(dpy);
}
void ct_input_title_click(ct_win *w, int count)
{
    ct_rect f = ct_window_frame(w), c = ct_window_client(w);

    /* the middle of the decorations above the client */
    XTestFakeMotionEvent(dpy, -1, f.x + f.w / 2, f.y + (c.y - f.y) / 2, 0);
    for (int i = 0; i < count; ++i) {
        fake_sync();
        XTestFakeButtonEvent(dpy, 1, True, 0);
        fake_sync();
        XTestFakeButtonEvent(dpy, 1, False, 0);
    }
    XFlush(dpy);
}
#else
void ct_input_keys(const char *keys) { (void)keys; }
void ct_input_title_click(ct_win *w, int count) { (void)w; (void)count; }
void ct_input_click(ct_win *w, int x, int y) { (void)w; (void)x; (void)y; }
void ct_input_client_drag(ct_win *w, ct_edge edge, int dx, int dy)
{
    (void)w; (void)edge; (void)dx; (void)dy;
}
void ct_input_drag(ct_win *w, const char *mods, int dx, int dy)
{
    (void)w; (void)mods; (void)dx; (void)dy;
}
#endif
