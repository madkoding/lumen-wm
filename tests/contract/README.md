# Contract tests

What Lumen must do, written as cases a client can check, so the same cases run
on every backend: X11 today, Wayland later.

- `cases.c`: the cases. They only use `driver.h`.
- `driver.h`: what a driver offers: make windows, send the requests a pager or a
  taskbar sends, read back what the window manager did. `ct_driver_caps()`
  says what a backend can do at all; a case that needs more is skipped
  (`CT_REQUIRE`), never failed.
- `driver-x11.c`: the X11 driver, on Xlib and EWMH.
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

Cases written for another configuration name it in `ct_cases` (the third
field). `run.sh` then starts Lumen again with `data/rc.xml` changed by
`configs/NAME.sed` and runs them with `contract-x11 --config NAME`.

## Adding a case

Write it against `driver.h` only, wait with `CT_EVENTUALLY` (the window
manager answers asynchronously), and destroy every window the case made.
