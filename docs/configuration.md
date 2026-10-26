# Configuring Lumen

Everything lives in `~/.config/lumen/rc.xml` (Lumen falls back to `~/.config/openbox/rc.xml`). If you don't
have one, copy `/etc/xdg/openbox/rc.xml`. The shelf options go inside `<shelf>`; after a change, run
`lumen-wm --reconfigure`. The same file works on X11 and on Wayland.

How the shelf is built (sizes, islands, drawing without a compositor) is in [`doc/shelf.md`](../doc/shelf.md).

## Make it yours

### Pick a look

The shelf takes the colours of your window theme. Change the theme (for example with `obconf`) and the shelf
changes with it. These are the themes that come with Lumen:

![The shelf under each bundled theme](screenshots/shelf-themes.png)

If you prefer fixed colours, choose a material:

```xml
<material>dark</material>    <!-- theme (default) | dark | light -->
```

Or use your own colours. Fill in any of them and leave the rest empty:

```xml
<colors>
  <body>#21252b</body>      <!-- the background of the islands -->
  <text>#e7e9e8</text>      <!-- the text and icons -->
  <hover>#3a3f47</hover>    <!-- the tile under the mouse -->
  <accent>#7aa2f7</accent>  <!-- the mark under the focused app -->
</colors>
```

If a theme's colour would be hard to read, the shelf picks black or white instead.

### Pin your apps

Right click any open app in the dock and choose **Pin to dock**. Choose **Unpin from dock** to remove it.
Drag a pinned app onto another to change the order.

To keep your pins after a restart, tell the shelf where to save them:

```xml
<pinsFile>~/.config/lumen/pins</pinsFile>
```

You can also list them by hand:

```xml
<pins>
  <pin>
    <name>Files</name>
    <icon>system-file-manager</icon>  <!-- an icon name, or the path to an image -->
    <command>pcmanfm</command>        <!-- what runs when you click it -->
  </pin>
</pins>
```

Clicking a pin opens the app. If it is already open, the click brings it to the front, and a second click
minimises it.

### Fonts and icons

```xml
<font>Sansation</font>        <!-- any font installed on your system; empty = the default -->
<iconTheme>Papirus</iconTheme> <!-- the icon theme to look up pin icons in -->
```

### Menus

When the shelf is on, every menu gets rounded corners and more space. With a compositor running (picom,
xcompmgr…), the corners are smooth and there is a soft shadow. Without one, the corners still round but less
smoothly.

![The shelf menus under each bundled theme](screenshots/shelf-themes-menus.png)

```xml
<menuRadius>12</menuRadius>    <!-- how round the corners are; 0 = square -->
<menuPadding>8</menuPadding>   <!-- space around and inside the menus -->
```

## Optional extras

Each extra is a small section inside `<shelf>`. Copy the ones you want. They use common tools (`nmcli`,
`wpctl`, `playerctl`), so install those first.

### Wifi

Click the wifi icon to see nearby networks and connect to one.

```xml
<network>
  <list>nmcli -t -f IN-USE,SIGNAL,SECURITY,SSID dev wifi</list>
  <connect>nmcli dev wifi connect %s</connect>
  <secured>nmcli dev wifi connect %s password %p</secured>
  <password>zenity --entry --hide-text --text=Password</password>
</network>
```

For networks with a password, it asks you for it in a small window. The password is passed to `nmcli` on its
command line, so other users of the machine could see it while it connects.

### Volume

Click for a menu, scroll to change the volume, middle click to mute.

```xml
<volume>
  <level>pamixer --get-volume-human</level>
  <up>wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+</up>
  <down>wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-</down>
  <toggle>wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle</toggle>
</volume>
```

### Power and battery

```xml
<power>
  <item><name>Log out</name><command>lumen-wm --exit</command></item>
  <item><name>Restart</name><command>systemctl reboot</command></item>
  <item><name>Shut down</name><command>systemctl poweroff</command></item>
</power>
<battery>yes</battery>   <!-- only appears on machines with a battery -->
```

### Music player

The player shows what your music app is playing. Save this script as `~/.local/bin/shelf-media` and make it
executable (`chmod +x`):

```sh
#!/bin/bash
# prints: status, title, artist, cover
status=$(playerctl status 2>/dev/null) || exit 0
echo "$status"
playerctl metadata --format $'{{title}}\n{{artist}}\n{{mpris:artUrl}}' | sed 's|^file://||'
```

Then add:

```xml
<media>
  <command>~/.local/bin/shelf-media</command>
  <previous>playerctl previous</previous>
  <toggle>playerctl play-pause</toggle>
  <next>playerctl next</next>
</media>
```

### Weather

The weather area shows whatever a script of yours prints: the temperature, the sky, then up to three
`day|temperature|icon` lines. The icon is one of `sun`, `cloud`, `cloud-sun`, `rain`, `snow`, `storm` or
`fog`.

```
59°
Light rain
Fri|74°|cloud-sun
Sat|69°|sun
Sun|67°|cloud
```

```xml
<weather>
  <command>~/.local/bin/shelf-weather</command>
  <interval>900</interval>   <!-- refresh every 15 minutes -->
</weather>
```

## Troubleshooting

- **The shelf doesn't show up.** Check that `<enabled>yes</enabled>` is inside `<shelf>` and run
  `lumen-wm --reconfigure`. It also hides itself while a window is fullscreen.
- **The menus have rough corners and no shadow.** On X11, start a compositor, such as `picom`. On Wayland Lumen
  is the compositor and draws them round already.
- **A pin shows a letter instead of an icon.** That icon name doesn't exist in your `<iconTheme>`. Try another
  name, or give the path to an image.
- **The player or weather is missing.** Run its command in a terminal. If it prints nothing, the shelf hides
  that area.
- **The colours are hard to read with my theme.** Use `<material>dark</material>` or set your own `<colors>`.

## Full configuration

<details>
<summary>Every shelf option in one place</summary>

```xml
<shelf>
  <enabled>yes</enabled>              <!-- off by default -->
  <material>theme</material>          <!-- theme | dark | light -->
  <colors>                            <!-- optional #rrggbb; empty keeps the material's -->
    <body></body><text></text><hover></hover><accent></accent>
  </colors>
  <font></font>                       <!-- empty = the system's sans font -->
  <iconTheme></iconTheme>             <!-- where bare icon names are looked up -->

  <pins>
    <pin>
      <name>Files</name>
      <icon>system-file-manager</icon>
      <command>pcmanfm</command>
      <class>Pcmanfm</class>          <!-- the window's class; default: the command's first word -->
    </pin>
  </pins>
  <pinsFile>~/.config/lumen/pins</pinsFile>  <!-- pins saved from the dock; replaces <pins> once it exists -->

  <network>
    <list>nmcli -t -f IN-USE,SIGNAL,SECURITY,SSID dev wifi</list>
    <connect>nmcli dev wifi connect %s</connect>          <!-- %s: the network's name -->
    <secured>nmcli dev wifi connect %s password %p</secured> <!-- %p: what <password> prints -->
    <password>zenity --entry --hide-text --text=Password</password>
  </network>
  <volume>
    <level>pamixer --get-volume-human</level>
    <up>wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+</up>
    <down>wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-</down>
    <toggle>wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle</toggle>
  </volume>
  <power>
    <item><name>Log out</name><command>lumen-wm --exit</command></item>
  </power>
  <battery>yes</battery>
  <batteryDir></batteryDir>           <!-- default: found in /sys/class/power_supply -->

  <menuRadius>12</menuRadius>         <!-- 12 with the shelf on, 0 with it off -->
  <menuPadding>8</menuPadding>        <!-- 8 with the shelf on, 0 with it off -->

  <media>
    <command>~/.local/bin/shelf-media</command>
    <previous>playerctl previous</previous>
    <toggle>playerctl play-pause</toggle>
    <next>playerctl next</next>
    <interval>3</interval>            <!-- seconds -->
  </media>
  <weather>
    <command>~/.local/bin/shelf-weather</command>
    <interval>900</interval>          <!-- seconds -->
  </weather>
</shelf>
```

Commands run in the background, so a slow one never freezes the desktop.

</details>
