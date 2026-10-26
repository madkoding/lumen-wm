# Roadmap: X11 and Wayland

The plan behind each step is in [backends.md](backends.md). This page tracks where the work stands and roughly how
much is left. The contract tests (`tests/contract`, 42 cases) stay green on X11 throughout.

| Phase | State | PRs done | PRs left (est.) |
| --- | --- | --- | --- |
| 1. Contract tests | done | 1 | 0 |
| 2. Backend design (`backend.h`) | done | 1 | 0 |
| 3. Core onto the interface | done | 29 | 0 |
| 4. Render without Xlib, windows as canvases | done | 1 | 0 |
| 5. Wayland backend (wlroots) | in progress | 0 | 1 |

Phase 3 ended with one PR for all that was left; from here each phase is one PR,
with a semantic commit per step inside it. Phase 4 moved the frames' and the
internal windows' X windows too, since making them canvases and drawing them through `canvas_present` is one
change.

## 1. Contract tests (done)

- [x] Driver interface, X11 driver on Xvfb, CI job (#15). Cases have been added since, one before each behaviour
  that a refactor touches.

## 2. Backend design (done)

- [x] `docs/design/backends.md` and `openbox/backend.h`, with no change in behaviour (#16).

## 3. Move the core onto the interface (done)

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

- [x] Pointer crossings (enter/leave): title bar button hover, focus follows mouse, menu item selection, dock
  auto-hide. The contract tests can run cases under another configuration (`configs/*.sed`), starting with focus
  following the mouse.
- [x] Frames through the backend: reparenting, frame extents, shapes, colormaps, borders and geometry
  (`surface_frame`, `canvas_set_shape`, `surface_configure`...). `client.c` has no Xlib left.
- [x] Leftovers: the window map (`window.c`), the root's state (`screen.c`), the support window (`stacking.c`,
  `grab.c`), startup and `--replace` (`openbox.c`), dock apps (`dock.c`).
- [x] The backend reads the event queue: one input entry point for the core (`event_input`), the root's
  messages, FocusIn/FocusOut bookkeeping and dock apps all come through `ObBackendEvents`
  (`backend-x11-event.c`). `event.c` reads no XEvent.

At the end of phase 3, what still called Xlib in the core was the windows the core draws, which phase 4 turned into canvases: the frames'
decoration windows (`frame.c`, `framerender.c`), menus, the shelf, the window cycling popup and indicator,
prompts, other popups and the dock's own window. `startupnotify.c` keeps feeding libsn from the X queue until
phase 5 decides what launch feedback is on Wayland.

## 4. Render without Xlib, windows as canvases (done, one PR)

- [x] obrender draws into cairo image surfaces instead of Xlib pixmaps, fonts go through cairo/pango, and it
  needs no display (colors, font settings and XBM masks without X).
- [x] `canvas_present` in the X11 backend puts the image on screen and keeps it there (no expose handling).
- [x] Frames are a tree of child canvases: the part under the pointer is still the canvas that got the event.
- [x] Menus (with round corners and translucency), the shelf, popups, the focus indicator, prompts and the dock
  are canvases.
- [x] Core headers stop using X types (`Window`, `Time`, gravities, size hint bits).
- [x] Themes stay as they are; `tools/shelf-themes.sh` (with and without a compositor) checks each switch.

Still X11 in the core, for phase 5: keysyms and modifier masks (`obt/keyboard`, to xkbcommon), XSMP through ICE
(`session.c`), libsn (`startupnotify.c`) and Xrm for theme files (`obrender/theme.c`).

Found on the way, to fix with a contract case first: since phase 3 a window with a position hint counts as
placed by the user whether it set the user or the program bit, and a size hint never counts as the user's (`ObClientRequested`
keeps that behaviour for now).

## 5. Wayland backend (in progress, one PR)

- [x] Build with meson instead of autotools; it installs the same tree.
- [ ] Decide whether to ship one binary or two (see the open questions in [backends.md](backends.md)).
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
