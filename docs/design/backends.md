# Backends: one window manager for X11 and Wayland

Lumen has run on X11 only, as Openbox always has. The goal is one window manager, with the same rc.xml, themes,
shelf and menus, running either as an X11 window manager or as a Wayland compositor. The window manager logic
(placement, focus, stacking, desktops, actions) is written once. A backend carries it to the display server.

This document covers what goes where, how the code gets there without breaking X11, and what each backend has to
provide. The interface is [`openbox/backend.h`](../../openbox/backend.h). The behaviour that must not change is
pinned by the contract tests in [`tests/contract`](../../tests/contract).

## Where things are now

About 30 000 lines of C in `openbox/`, plus `obt/` (X helpers) and `obrender/` (themes). Xlib calls are spread
through the core rather than kept behind a layer:

| Area | Files | X11 it uses |
| --- | --- | --- |
| Frames | `frame.c`, `framerender.c` | reparenting, a dozen subwindows per frame, Shape, Xlib pixmaps |
| Clients | `client.c` | ICCCM/EWMH properties, WM_PROTOCOLS, XKillClient, XSetInputFocus |
| Screen | `screen.c` | root properties, Xinerama/RandR, the support window |
| Events | `event.c` | the XEvent dispatch for everything below |
| Move/resize | `moveresize.c` | XSync counters, pointer warps |
| Input | `grab.c`, `keyboard.c`, `mouse.c` | passive and active grabs |
| Stacking | `stacking.c` | XRestackWindows |
| Drawing | `obrender/`, `menuframe.c`, `shelf.c`, popups | Xft, Xlib pixmaps, cairo-xlib |

Most of the rest (`place.c`, `focus.c`, `actions/`, `config.c`, `menu.c`, `resist.c`) is logic that only touches X
through those files.

## The layers

```
                 rc.xml, menu.xml, themes
                           │
 ┌─────────────────────────┴──────────────────────────┐
 │ core: clients, placement, focus, stacking, desktops,│
 │       actions, moveresize, shelf, menus, prompts    │
 └──────┬───────────────────────────────▲──────────────┘
        │ ObBackend (requests)          │ ObBackendEvents (what happened)
 ┌──────▼───────────────────────────────┴──────────────┐
 │ backend-x11            │ backend-wlroots            │
 │ Xlib, EWMH/ICCCM,      │ wlr_scene, xdg-shell,      │
 │ RandR, XSync, Shape    │ layer-shell, XWayland      │
 └────────────────────────┴────────────────────────────┘
 render: obrender + cairo draw into plain ARGB images, shown with canvas_present()
```

Rules:

- The core includes `backend.h`, never `X11/*` or `wayland-*`. A file that still includes X11 headers hasn't been
  migrated yet.
- A backend never decides. It doesn't place or focus a window on its own; it reports what happened and does
  what it is told.
- Ids are opaque. `ObSurfaceId` is an X `Window` for one backend and a pointer for the other.
- What one backend can't do is a capability (`ObBackendCaps`), not an `#ifdef` in the core.

## What each request means per backend

| `ObBackend` | X11 | Wayland (wlroots) |
| --- | --- | --- |
| `surface_configure` | XMoveResizeWindow plus a synthetic ConfigureNotify | `wlr_xdg_toplevel_set_size`; position in the scene graph |
| `surface_show` | map/unmap, WM_STATE | enable/disable the scene node |
| `surface_focus` | XSetInputFocus, WM_TAKE_FOCUS | `wlr_seat_keyboard_notify_enter`, activated state |
| `surface_close` | WM_DELETE_WINDOW | `xdg_toplevel.close` |
| `surface_kill` | XKillClient | `wl_client_destroy` |
| `surface_set_states` | `_NET_WM_STATE` | xdg_toplevel states (maximized, fullscreen, activated, tiled) |
| `surface_ping` | `_NET_WM_PING` | `xdg_wm_base.ping` |
| `restack` | XRestackWindows | `wlr_scene_node_place_below`, one node after the other |
| `canvas_*` | an override-redirect or frame window, image put with XPutImage or cairo-xlib, Shape for input | a `wlr_scene_buffer`; panels and popups as layer-shell-like internal nodes |
| `canvas_set_strut` | `_NET_WM_STRUT_PARTIAL` on the shelf window | the exclusive zone, which shrinks the usable area |
| `bind_key` | XGrabKey on the root | nothing: the compositor sees every key first |
| `grab_pointer` / `grab_keyboard` | XGrabPointer / XGrabKeyboard | route seat events to the core only |
| `pointer_warp` | XWarpPointer | `wlr_cursor_warp` |
| `publish_*` | root properties (`_NET_CLIENT_LIST`, `_NET_ACTIVE_WINDOW`, `_NET_WORKAREA`…) | `wlr-foreign-toplevel-management` and `ext-workspace`, for taskbars and pagers |
| `output_*` | RandR / Xinerama | `wlr_output_layout` |

## What changes for whom

- **Users** keep rc.xml, menu.xml and themes. Key bindings use keysyms through xkbcommon on both backends, as
  `obt/keyboard.c` already does.
- **Clients that place themselves** can't on Wayland. Placement rules in rc.xml (`<applications>`) still work,
  because the core places them.
- **Dock apps** (withdrawn windows) are X11 only (`OB_BACKEND_CAP_DOCKAPPS`). Under Wayland they work through
  XWayland, if at all.
- **The shelf and menus** stay internal, drawn by the core into canvases, so they look the same everywhere.
  External panels (waybar and similar) work through layer-shell on Wayland.
- **Decorations**: server side on both. On Wayland, `xdg-decoration` asks clients not to draw their own, and the
  ones that insist go undecorated (`OB_BACKEND_CAP_DECOR_HINTS`).

## How to get there

Every step is its own small PR, and the contract tests stay green on X11 throughout.

1. **Contract tests** (done, `tests/contract`).
2. **This design** and `backend.h`, with no change in behaviour.
3. **Move the core onto the interface**, one area at a time. Each PR adds the backend-x11 functions it needs
   (moving the Xlib code out of the core file, not rewriting it) and switches the callers:
   1. `backend-x11.c` skeleton: `startup`, `shutdown` and `event_source`, wrapping `obt_display` and `xqueue`
      (done). `openbox.c` starts the backend through `backend_startup()`. The other `ObBackend` functions stay
      NULL until the step that moves their callers, and `time` comes with the event split (7).
   2. Outputs: `screen.c` stops calling Xinerama and asks `output_count` and `output_area` instead (done).
      `--debug-xinerama` still splits a single output in two, now in the core.
   3. Publishing: the root properties move out of `screen.c`, `focus.c`, `stacking.c` and `client.c` (done).
      The client and stacking lists, the desktop count, current desktop and names, and the work areas are
      published separately, at the points where the core changes each.
   4. Stacking: `restack` replaces XRestackWindows (done). It keeps X's shape: a reference window and the
      windows to put below it, top first, so the core still restacks one layer at a time.
   5. Surfaces: `surface_*` replaces the XSetInputFocus, WM_PROTOCOLS and kill calls in `client.c` (done for
      focus, close, kill, `_NET_WM_STATE` and ping). `surface_configure` and `surface_show` come with the frames,
      which still reparent and map the client themselves, and focusing nothing (`focus.c`'s fallback to the
      support window) comes with the support window.
   6. Client info: property reading in `client.c` becomes `ObSurfaceInfo`, filled by the backend's `surface_info`
      for the fields asked for. Done for the titles, WM_PROTOCOLS, struts, the size hints (WM_NORMAL_HINTS, checked
      for sanity in the backend), WM_HINTS, the window type, Motif hints and transient-for, the class, role and
      session ids, the icons, the startup id, the requested desktop, the initial states and the user time; the core
      keeps the fallbacks (a title from WM_CLASS, a legacy strut spanning the screen, a dialog type for transients)
      and publishes the visible titles with `publish_surface_titles`, decides whether WM_CLIENT_MACHINE is another
      host, and builds the icon images from the ARGB sizes the backend hands over (`ObIcon`), publishing the
      theme's icon with `publish_surface_icon` for a window that has none. Still read in `client.c`: the geometry,
      shape, colormap, opacity and sync counter, which go with the frames and move/resize.
   7. Requests: `event.c` splits in two. The X11 half turns XEvents into `ObBackendEvents`, and the core half
      (`backend-events.c`) handles them. While `event.c` still reads the X queue, it hands each kind of event
      that has moved to a `backend_x11_*` function (`backend-x11.h`). Done for property changes on clients
      (`surface_changed`, with the queue compression for titles and icons in the backend; opacity and the
      sync counter stay in `event.c` until the frames and move/resize move) and for the client messages
      about states, iconifying, activating, closing and desktops (`request_*`; `client_set_state()` now
      takes `ObSurfaceStates`). Configure requests, `_NET_MOVERESIZE_WINDOW` and
      `_NET_RESTACK_WINDOW` are `request_configure`, `request_geometry` and `request_restack`, with
      `ObConfigureRequest` and `ObStackMode` in place of X's value masks and stack modes. Map requests,
      unmaps, destroys and reparents out of the frame are `surface_new`, `surface_withdrawn` and
      `surface_gone` (`backend_x11_map_event`); the core still counts the unmaps it causes itself
      (`ignore_unmaps`), and dock apps stay X11-only in `event.c`. Focus changes a client makes itself are
      `focus_changed`, `focus_left` and `focus_lost`; the FocusIn/FocusOut bookkeeping (the queue look-ahead,
      focus on another screen) stays in `event.c` and moves to the backend with the event queue. Next: the
      interactive `_NET_WM_MOVERESIZE` with move/resize (9).
   8. Input: `grab.c`, `keyboard.c`, `mouse.c`. The contract cases type and click with XTest (a key binding,
      Alt-Tab, Alt-drag). Done: the active grabs (`grab_keyboard`, `grab_pointer`, `grab_server`); `grab.c`
      keeps counting nested grabs and picking the ungrab time. Next: the passive key and button grabs
      (`bind_key`, and the button bindings on frames), then the replayed clicks in `mouse.c`.
   9. Move/resize: XSync and pointer warps go behind the backend.
4. **Render without Xlib**: obrender, frames, the shelf, menus and popups draw into cairo image surfaces and hand
   them to `canvas_present`. Themes are unchanged, and `tools/shelf-themes.sh` checks that they still read.
5. **backend-wlroots**: a compositor on wlroots' scene graph with xdg-shell, xdg-decoration, layer-shell,
   foreign-toplevel, ext-workspace and XWayland. The contract tests get a Wayland driver that runs on wlroots'
   headless backend, and both drivers run in CI.

Step 3 is the long one. The order puts the steps with the least behaviour first, and leaves the event split,
which carries the most behaviour, for after the smaller seams have shaped the interface.

## Testing

- A behaviour change gets a contract case first (`tests/contract/cases.c`). Cases talk only to
  `tests/contract/driver.h`, so the Wayland driver runs the same ones.
- A case that needs something a backend can't do is skipped there (`CT_REQUIRE`), not failed. The backend
  capability and the driver capability say the same thing.
- `tools/shelf-themes.sh` keeps the drawing honest through the render move.

## Open questions

- **Build:** autotools today. A wlroots backend is much easier with meson (wayland-scanner, wlroots' own
  pkg-config). The switch could happen before step 5, without changing behaviour.
- **One binary or two:** `lumen-wm` could pick its backend at runtime (`WAYLAND_DISPLAY` unset and `DISPLAY` set
  → X11), or ship as `lumen-wm` and `lumen-wl`. Runtime choice needs both sets of libraries installed.
- **wlroots version:** the API changes between releases. The backend targets one version, pinned in the build.
