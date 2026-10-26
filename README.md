<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo/lumen-dark.svg">
    <img src="docs/logo/lumen-light.svg" alt="Lumen Window Manager" width="440">
  </picture>
</p>

# Lumen Window Manager

**A light and fast window manager for X11 and Wayland, with colour themes and a floating shelf.** Lumen is a
heavily modified fork of [Openbox](http://openbox.org): it keeps Openbox's speed, themes and `rc.xml`, and adds a
dock, a music player, the weather and a status area with wifi, volume and power menus.

![Lumen on Wayland: the launcher menu open over the shelf, with a terminal and an editor](docs/screenshots/lumen-wayland.png)

*Lumen as a Wayland compositor: the shelf's launcher menu, foot and Mousepad, with the Onyx theme, Sansation and
Papirus. The same setup runs on X11.*

## Features

- 🚀 **Dock**: pinned apps and open windows; click to open, focus or minimise, drag to reorder.
- 🎵 **Music player** and ⛅ **weather**, fed by small scripts; each hides while it has nothing to show.
- 🕒 **Status area**: clock, date, and wifi, volume, power and battery menus.
- 🎨 **Follows your theme**: the shelf and the rounded menus take the colours of your Openbox theme.
- 🪟 **X11 and Wayland**: one binary, one `rc.xml`; layer-shell, foreign-toplevel and XWayland on Wayland.
- 🧩 **Openbox compatible**: installs `lumen-wm` with an `openbox` command, and reads `~/.config/openbox` when
  `~/.config/lumen` is missing. The shelf is off by default, so Lumen starts out as plain Openbox.

## Install

On Debian or Ubuntu:

```sh
sudo apt-get install build-essential meson ninja-build pkg-config gettext \
  libglib2.0-dev libxml2-dev libpango1.0-dev libcairo2-dev libx11-dev libxft-dev \
  libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libsm-dev librsvg2-dev libimlib2-dev
# Wayland (optional): wlroots 0.17
sudo apt-get install libwlroots-dev libwayland-dev wayland-protocols libxkbcommon-dev \
  libxcb-icccm4-dev libxcb-ewmh-dev

git clone https://github.com/madkoding/lumen-wm && cd lumen-wm
meson setup build --prefix=/usr --sysconfdir=/etc
meson compile -C build && sudo meson install -C build
```

## Run

Pick **Lumen** or **Lumen (Wayland)** in your display manager, or run `lumen-wm`. It runs on X11 when `DISPLAY`
is set and as a Wayland compositor otherwise; `LUMEN_BACKEND=x11` or `wayland` forces one.

Turn the shelf on in `~/.config/lumen/rc.xml` (copy `/etc/xdg/openbox/rc.xml` if you don't have it) and apply:

```xml
<shelf>
  <enabled>yes</enabled>
</shelf>
```

```sh
lumen-wm --reconfigure
```

## Documentation

- [Configuration](docs/configuration.md): themes, pins, fonts and icons, menus, wifi, volume, power, music,
  weather, troubleshooting and every shelf option.
- [Shelf internals](doc/shelf.md) and [screenshots for r/unixporn](doc/screenshots.md).
- [X11 and Wayland backends](docs/design/backends.md) and the [roadmap](docs/design/roadmap.md).
- Development: the contract tests in [`tests/contract`](tests/contract) check the same behaviour on X11
  (`run.sh`) and Wayland (`run-wayland.sh`); [`tools/shelf-themes.sh`](tools/shelf-themes.sh) shoots every
  theme and checks contrast. Building from git: [`README.GIT`](README.GIT).
- [Kitties](https://github.com/madkoding/kitties) ships Lumen with the shelf already set up.

## Design

Lumen's look is based on [LumaUI](https://hub.simplyluma.com/developer/agents/design-with-lumaui.md), Luma's
design system: content sits in rounded **islands**, colours come from a few tokens with one accent per view, and
text keeps at least 4.5:1 contrast.

## License and credits

Lumen is based on Openbox and, like it, is free software under the GNU General Public License, version 2 or
(at your option) any later version. See [`COPYING`](COPYING). Lumen keeps the original copyright notices and
[`AUTHORS`](AUTHORS), and its own changes, the shelf included, are released under the same licence.

- Openbox: Copyright (C) 2004 Mikael Magnusson, Copyright (C) 2002 Dana Jansens, and all the contributors in
  `AUTHORS`. Upstream: <http://openbox.org>.
- Design: Lumen's look (islands, menus, colour tokens and contrast) is based on
  [LumaUI](https://hub.simplyluma.com/developer/agents/design-with-lumaui.md), Luma's design system. Lumen reimplements that design in C on Openbox; it does not ship Luma
  code, logos or artwork. Lumen is not affiliated with Luma or Openbox.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
