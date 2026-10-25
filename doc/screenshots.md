# Screenshots

How to take screenshots of Lumen for the README and for posts on
[r/unixporn](https://www.reddit.com/mod/unixporn/rules/).

> The subreddit's rules change. Read the live rules page before each post; the
> checklist below is a summary and may be out of date.

## r/unixporn checklist

- **Title starts with the WM in brackets**: `[Lumen] <short description>`.
  Lumen is new, so mention its base once: `[Lumen] My Openbox fork with a
  LumaUI-style shelf`.
- **Details comment**: right after posting, comment with the setup (see the
  template below). Posts without details get removed.
- **A real screenshot of your own desktop**: a screen capture, not a phone
  photo, mock-up or render. Show something more than a stock setup or a
  wallpaper change.
- **Safe for work**: no nudity or suggestive wallpaper, no gore, nothing
  offensive in window titles, file names, chats or browser tabs.
- **Credit what is not yours**: wallpaper source and artist, icon theme, fonts,
  the themes Lumen bundles.
- **Self-promotion**: Lumen is your own project. Post it as your rice (or with
  the release/project flair if the sub has one), stay active as a regular
  member and do not spam the same shots.
- **Pick the right flair** (screenshot, workflow for a video, etc.).
- **No private data**: crop or blank out user names, host names, IPs, Wi-Fi
  names, e-mail and notifications.

## What to shoot

Take every shot at the native resolution of one monitor (1920x1080 or more),
PNG, no scaling, with `scrot`, `maim` or `import -window root`.

1. **Clean desktop**: the wallpaper and the shelf (dock, music player, weather,
   clock), no windows. This is the cover image.
2. **Busy desktop**: two or three real windows (terminal running `fastfetch`
   or `btop`, a file manager, an editor) with focused and unfocused title bars,
   so the theme shows both states.
3. **Menus open**: the root menu and one shelf menu (network, volume or
   power) with the highlight visible.
4. **Themes grid**: `tools/shelf-themes.sh` already shoots the shelf under each
   bundled theme. Use it for the README, and only as an extra image in a post,
   since it runs on Xvfb with sample data rather than a real desktop.
5. **Optional short video** (workflow flair): switching workspaces, opening a
   menu, a window snapping into place. 20 to 40 seconds, no audio.

Post 1 to 3 as a gallery with 1 as the first image.

### Before shooting

- Set the font to **Sansation** in `rc.xml` and in the apps you show.
- Use one theme for the whole set (for example Onyx) so the shots match.
- Run picom (`COMPOSITOR=1` in the tool) for rounded, anti-aliased menus.
- Pick a wallpaper you may share, and note its source and artist.
- Hide desktop notifications and clear the terminal history on screen.

## Details comment template

```text
Lumen Window Manager (based on Openbox): https://github.com/madkoding/lumen-wm

- WM: Lumen <version>, theme <Onyx>
- Shelf: built in (dock, music, weather, clock)
- Compositor: picom
- Font: Sansation
- Icons: <icon theme>
- Terminal: <terminal> + <shell>, fetch: fastfetch
- Wallpaper: <title> by <artist>, <link>
- Distro: <distro>
- Dotfiles: <link>
```
