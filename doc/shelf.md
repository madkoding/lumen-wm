# Shelf

An optional bar drawn by Openbox itself: two rounded islands held 16 px off
the bottom edge, centred on the primary monitor.

- **Dock island**: an applications tile (opens `root-menu`), a rule, then one
  40 px tile per window. Left click activates or minimises, right click opens
  the client menu. A bar under the tile marks the focused window.
- **Status island**: time, date, and wifi, volume and power glyphs.

Enable it in `rc.xml`:

```xml
<shelf>
  <enabled>yes</enabled>
  <material>theme</material>  <!-- theme (the Openbox theme's colours) | dark | light -->
  <font></font>  <!-- empty = the system sans (fontconfig); or a family -->
  <colors><!-- optional #rrggbb overrides: body, text, hover, accent --></colors>
</shelf>
```

### Pinned applications

`<pins>` lists applications that stay in the dock whether or not they are
running, in order, before the windows nobody pinned:

```xml
<shelf>
  <iconTheme>Lucide</iconTheme>
  <pins>
    <pin>
      <name>Files</name>
      <icon>system-file-manager</icon>  <!-- a bare name, or a file path -->
      <command>pcmanfm</command>
      <class>Pcmanfm</class>            <!-- WM_CLASS or name; default: the command's first word -->
    </pin>
  </pins>
</shelf>
```

A bare icon name is searched in `$XDG_DATA_DIRS/icons/<iconTheme>/{scalable,
48x48, 64x64, 32x32, small}/*/<name>.{svg,png}`. A click on a pin that is not
running starts its command; on a running one it focuses its window (or
minimises it when it has the focus). The running mark is drawn only while a
window of the pin's class is open.

### Media and weather islands

`<media>` and `<weather>` add two islands between the dock and the clock, each
filled by a command and hidden while it prints nothing.

```xml
<media>
  <command>kitty-shelf-media</command>
  <previous>playerctl previous</previous>
  <toggle>playerctl play-pause</toggle>
  <next>playerctl next</next>
  <interval>3</interval>   <!-- seconds; default 3 -->
</media>
<weather>
  <command>kitty-shelf-weather</command>
  <interval>900</interval> <!-- seconds; default 900 -->
</weather>
```

The media command prints four lines: `Playing` or `Paused`, the title, the
artist and a path to the cover (a file; empty for none). The island shows the
cover, the title over the artist and three round buttons that run `<previous>`,
`<toggle>` and `<next>` (and refresh the island right after).

The weather command prints the temperature, the sky, then up to three lines
`day|temperature|glyph` with glyph one of `sun`, `cloud`, `cloud-sun`, `rain`,
`snow`, `storm`, `fog`:

```
59°
Light rain
Fri|74°|cloud-sun
Sat|69°|sun
Sun|67°|cloud
```

Commands run asynchronously (a slow one never stalls the window manager) and
only one run of each is in flight at a time.

The shelf takes its height from the work area (a bottom strut), is hidden
while a fullscreen window has focus, and needs cairo and pangocairo to build.

## Without a compositor

Rounded corners and the soft shadow need alpha. When a compositing manager owns
`_NET_WM_CM_S<n>` the shelf uses a 32-bit window. Otherwise it reads the
wallpaper under itself once at start-up (and whenever the row changes width)
and paints the islands over that picture, so it looks the same on a plain X
server. A wallpaper that changes afterwards shows through only after the next
restart or reconfigure.

## Measurements

40 px tiles, 8 px apart, 8 px island padding (56 px high), 16 px radius,
material `#21252b` (dark) or `#eceef1` (light), or the theme's menu colours (`theme`), 1 px stroke, shadows
`0 3px 8px` and `0 22px 52px -15px`, 18x3 running mark, text in the font of
your fontconfig default unless `<font>` names a family (21 px time, 11 px date).
