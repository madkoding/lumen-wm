# Roadmap: X11 and Wayland

The plan behind each step is in [backends.md](backends.md). This page tracks where the work stands and roughly how
much is left. Every step is one or more small PRs, and the contract tests (`tests/contract`, 41 cases) stay green
on X11 throughout.

Sizes count PRs and are estimates: a step can split once its code is open.

| Phase | State | PRs done | PRs left (est.) |
| --- | --- | --- | --- |
| 1. Contract tests | done | 1 | 0 |
| 2. Backend design (`backend.h`) | done | 1 | 0 |
| 3. Core onto the interface | in progress | 28 | 10–14 |
| 4. Render without Xlib | pending | 0 | 5–8 |
| 5. Wayland backend (wlroots) | pending | 0 | 10–15 |

Of about 55–65 PRs in total, 30 are merged (#15–#44). Phases 4 and 5 are bigger per PR than phase 3, so by
effort the work is about a third done.

## 1. Contract tests (done)

- [x] Driver interface, X11 driver on Xvfb, CI job (#15). Cases have been added since, one before each behaviour
  that a refactor touches.

## 2. Backend design (done)

- [x] `docs/design/backends.md` and `openbox/backend.h`, with no change in behaviour (#16).

## 3. Move the core onto the interface (in progress)

Done:

- [x] Backend skeleton: startup, display and event source (#17).
- [x] Outputs: monitors from the backend instead of Xinerama/RandR (#18).
- [x] Publishing pager state: client and stacking lists, desktops, work areas (#19).
- [x] Stacking through `restack` (#20).
- [x] Surfaces: focus, close, kill and ping (#21).
- [x] Client info through `surface_info`: titles, protocols, struts, size hints, WM_HINTS, window type, Motif
  hints, transient-for, class, role, session ids, icons, startup id, desktop, initial states, user time
  (#22–#27).
- [x] Event split, core half in `backend-events.c`: property changes, client requests, configure, move/resize
  and restack requests, map/unmap/destroy, focus changes (#28–#32).
- [x] Grabs and bindings: keyboard, pointer and server grabs; key and button bindings (#33, #35).
- [x] Held clicks, client-started move/resize, pointer warps (#36).
- [x] Synced resizes (XSync) (#37), client opacity (#38).
- [x] Claiming the screen: support window, WM_Sn and `--replace`, `_NET_SUPPORTED` (#40).
- [x] Timestamps from the backend (#41).
- [x] Input as `ObInputEvent`: move/resize (#42), bindings and window cycling (#43), menus and prompts (#44).
- [x] A bug found on the way: a stale `_NET_ACTIVE_WINDOW` after closing the last window (#39).

Left:

- [ ] **Pointer crossings** (enter/leave), 1–2 PRs: title bar button hover, focus follows mouse, menu item
  selection, dock auto-hide.
- [ ] **Frames as canvases**, 3–5 PRs, the largest step left. `frame.c` holds most of the Xlib still in the core
  (about 220 calls). Reparenting and the frame's subwindows go into the backend; the core finds the part of the
  frame under the pointer from coordinates instead of from which subwindow got the event. This brings
  `surface_configure` and `surface_show`, and with them the client geometry, shape and colormap still read in
  `client.c`.
- [ ] **Internal windows as canvases**, 2–3 PRs: menus, the shelf, the window cycling popup and indicator,
  prompts, other popups, the dock.
- [ ] **Leftovers**, 1–2 PRs: the support window still used by `stacking.c`, `grab.c` and `event.c`; the window
  map in `window.c`; the rest of `screen.c` and `openbox.c`.
- [ ] **The backend reads the event queue**, 2–3 PRs: the X11 half of `event.c` moves into `backend-x11.c`,
  FocusIn/FocusOut bookkeeping included. Phase 3 is done when no core file includes an X11 header.

## 4. Render without Xlib (pending, 5–8 PRs)

- [ ] obrender draws into cairo image surfaces instead of Xlib pixmaps, and fonts go through cairo/pango
  instead of Xft.
- [ ] `canvas_present` in the X11 backend puts the image on screen.
- [ ] Frames, menus, the shelf and popups switch over one at a time.
- [ ] Themes stay as they are; `tools/shelf-themes.sh` checks each switch.

## 5. Wayland backend (pending, 10–15 PRs)

- [ ] Decide the build (autotools or meson) and whether to ship one binary or two (see the open questions in
  [backends.md](backends.md)).
- [ ] `backend-wlroots` skeleton: outputs, the scene graph, the seat, keyboard bindings through xkbcommon.
- [ ] Windows through xdg-shell, with server-side decorations through xdg-decoration.
- [ ] Shelf and popups as scene nodes; layer-shell for external panels.
- [ ] Taskbars and pagers through foreign-toplevel and ext-workspace.
- [ ] XWayland for X11 applications.
- [ ] A Wayland contract driver on wlroots' headless backend, with both drivers running in CI.
- [ ] Packaging and docs.

## What the user sees, and when

- During phases 3 and 4 Lumen keeps working as it does today on X11. Nothing changes for users except bug fixes
  found along the way.
- The first Wayland session comes partway through phase 5, once windows, decorations and input work. The shelf,
  external panels and XWayland follow.
