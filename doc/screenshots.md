# Screenshots

How to take screenshots of Lumen for the README and for posts on
[r/unixporn](https://www.reddit.com/mod/unixporn/rules/).

> Summary of the subreddit's rules as of 2026-10-25. Read the live rules page
> before each post.

## r/unixporn rules that apply

- **On-topic**: a screenshot of your own system (or a workflow video). No
  concepts, mock-ups, WSL or Termux.
- **Tag the WM in the title**: `[Lumen] <short description>`. Do not tag the OS
  or other software. A discussion post starts with `| ` instead.
- **Significant customization**: show configuration you did. A near-default
  setup, someone else's setup, or only a new wallpaper, theme, icons or
  terminal colors is not allowed.
- **Window variety**: at least two windows from at least two different
  applications. Everything running in a terminal counts as one application;
  add a file manager, browser or editor. Widgets and the shelf alone may not
  count.
- **Details comment**: recommended, not required. Follow the sub's
  [template](https://reddit.com/r/unixporn/wiki/info/template); the one below
  fits it.
- **No NSFW** of any kind, wallpaper included.
- **Material / `[OC]` posts** (for example, announcing Lumen as a tool) must
  offer something unique; the mods may ask for the source.

Good practice, not rules: credit the wallpaper's artist, and crop or blank out
user names, host names, Wi-Fi names and notifications.

## What to shoot

Take every shot at the native resolution of one monitor (1920x1080 or more),
PNG, no scaling, with `scrot`, `maim` or `import -window root` on X11, or `grim` on Wayland.

1. **Cover**: the shelf (dock, music player, weather, clock) with two
   applications open, for example a terminal running `fastfetch` and a file
   manager or browser. One focused and one unfocused, so both title bar
   states show.
2. **Menus**: the root menu and one shelf menu (network, volume or power)
   open, still with two applications on screen.
3. **Workspaces**: a second workspace with an editor and a terminal tiled
   side by side.
4. **Optional workflow video**: switching workspaces, opening a menu, a window
   snapping into place. 20 to 40 seconds, no audio.

Every image in the post must pass the window-variety rule on its own. Post 1
to 3 as a gallery with 1 first.

`tools/shelf-themes.sh` shoots the shelf under each bundled theme. It runs on
Xvfb with sample data and shows no applications, so use it for the README
only, not for a post.

### Before shooting

- Set the font to **Sansation** in `rc.xml` and in the apps you show.
- Use one theme for the whole set (for example Onyx) so the shots match.
- Run picom for rounded, anti-aliased menus.
- Configure the apps you show too (terminal colors, file manager theme), so
  the shot reads as a full rice and not a theme swap.
- Pick a SFW wallpaper you may share, and note its source and artist.
- Hide desktop notifications and clear the terminal history on screen.

## Details comment template

```text
Lumen Window Manager (based on Openbox): https://github.com/madkoding/lumen-wm

- WM: Lumen <version>, theme <Onyx>
- Bar/dock: Lumen's built-in shelf (music, weather, clock)
- Compositor: picom
- Font: Sansation
- Icons: <icon theme>
- Terminal: <terminal> + <shell>, fetch: fastfetch
- Apps: <file manager>, <browser/editor>
- Wallpaper: <title> by <artist>, <link>
- Distro: <distro>
- Dotfiles: <link>
```
