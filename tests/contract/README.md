# Contract tests

What Lumen must do, written as cases a client can check, so the same cases run
on every backend: X11 and Wayland.

- `cases.c`: the cases. They only use `driver.h`.
- `driver.h`: what a driver offers: make windows, send the requests a pager or a
  taskbar sends, read back what the window manager did. `ct_driver_caps()`
  says what a backend can do at all; a case that needs more is skipped
  (`CT_REQUIRE`), never failed.
- `driver-x11.c`: the X11 driver, on Xlib and EWMH.
- `driver-wayland.c`: the Wayland driver: xdg-shell and layer-shell windows,
  a virtual keyboard and pointer, and `lumen_state_v1`
  (`protocols/lumen-state-v1.xml`), which Lumen offers only with
  `LUMEN_TEST_PROTOCOL` set, to read back what EWMH carries on X11.
  xdg-shell lets no client place itself, pick its desktop or take the focus,
  so those cases skip there.
- `contract.c`: the runner; it prints TAP.

## Run

    meson compile -C build contract-x11
    tests/contract/run.sh $PREFIX            # every case
    tests/contract/run.sh $PREFIX iconify_hides

`run.sh` starts Xvfb and the installed `lumen-wm` with `data/rc.xml`, then the
cases. Set `CONTRACT` when the build directory isn't `build`; `ninja -C build contract` runs
them against the build's prefix. The cases that type
and click need XTest (`libxtst-dev`) at build time, and skip without it. CI runs it in
`.github/workflows/shelf-themes.yml`.

On Wayland, with Lumen built with `-Dwayland`:

    meson compile -C build contract-wayland
    tests/contract/run-wayland.sh $PREFIX

`run-wayland.sh` starts `lumen-wm` on a headless wlroots output (no display
needed) and runs the same cases; `ninja -C build contract-wayland-run` does it
against the build's prefix, and `WMARGS=--debug` passes flags to Lumen.

Cases written for another configuration name it in `ct_cases` (the third
field). `run.sh` then starts Lumen again with `data/rc.xml` changed by
`configs/NAME.sed` and runs them with `contract-x11 --config NAME`;
`run-wayland.sh` does the same.

## Adding a case

Write it against `driver.h` only, wait with `CT_EVENTUALLY` (the window
manager answers asynchronously), and destroy every window the case made.
