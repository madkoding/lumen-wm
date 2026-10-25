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

    make tests/contract/contract-x11
    tests/contract/run.sh $PREFIX            # every case
    tests/contract/run.sh $PREFIX iconify_hides

`run.sh` starts Xvfb and the installed `lumen-wm` with `data/rc.xml`, then the
cases. Set `CONTRACT` when the binary was built out of tree. CI runs it in
`.github/workflows/shelf-themes.yml`.

## Adding a case

Write it against `driver.h` only, wait with `CT_EVENTUALLY` (the window
manager answers asynchronously), and destroy every window the case made.
