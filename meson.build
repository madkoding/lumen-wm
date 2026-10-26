project('lumen-wm', 'c',
  version: '3.7',
  license: 'GPL-2.0-or-later',
  meson_version: '>= 0.62',
  default_options: ['buildtype=debugoptimized', 'warning_level=0'])

# obrender (RR_) and obt (OBT_) keep their own versions for their .so files:
# bump micro and interface/binary age as libtool's -version-info expects.
rr_major = 3
rr_minor = 6
rr_micro = 33
rr_interface_age = 1
rr_binary_age = 1
obt_major = 3
obt_minor = 6
obt_micro = 5
obt_interface_age = 3
obt_binary_age = 3

rr_version = '@0@.@1@'.format(rr_major, rr_minor)
obt_version = '@0@.@1@'.format(obt_major, obt_minor)
# libtool's current:revision:age, as a Linux soname and file version
rr_current = rr_micro - rr_interface_age
rr_age = rr_binary_age - rr_interface_age
rr_soversion = rr_current - rr_age
obt_current = obt_micro - obt_interface_age
obt_age = obt_binary_age - obt_interface_age
obt_soversion = obt_current - obt_age

cc = meson.get_compiler('c')
fs = import('fs')

prefix = get_option('prefix')
bindir = get_option('bindir')
libexecdir = prefix / get_option('libexecdir')
datadir = prefix / get_option('datadir')
localedir = prefix / get_option('localedir')
configdir = prefix / get_option('sysconfdir') / 'xdg'
rcdir = configdir / 'openbox'
docdir = get_option('datadir') / 'doc' / meson.project_name()
themedir = get_option('datadir') / 'themes'
theme = 'Clearlooks'

## dependencies ##

glib = dependency('glib-2.0', version: '>= 2.28.0')
xml = dependency('libxml-2.0', version: '>= 2.6.0')
pango = dependency('pango', version: '>= 1.18.0')
pangocairo = dependency('pangocairo')
cairo = dependency('cairo')
cairo_xlib = dependency('cairo-xlib')
x11 = dependency('x11')
xext = dependency('xext')
xrandr = dependency('xrandr')
xinerama = dependency('xinerama')
xau = dependency('xau')
libm = cc.find_library('m', required: false)
libsn = dependency('libstartup-notification-1.0', version: '>= 0.8',
                   required: get_option('startup-notification'))
xcursor = dependency('xcursor', required: get_option('xcursor'))
imlib2 = dependency('imlib2', required: get_option('imlib2'))
librsvg = dependency('librsvg-2.0', required: get_option('librsvg'))
sm = dependency('sm', required: get_option('session'))
ice = dependency('ice', required: get_option('session'))
# the contract tests type and click with XTest; without it they skip the
# input cases
xtst = dependency('xtst', required: false)

## defines (the same ones configure passed) ##

defs = [
  '-DPACKAGE_NAME="@0@"'.format(meson.project_name()),
  '-DPACKAGE_VERSION="@0@"'.format(meson.project_version()),
  '-DPACKAGE_BUGREPORT="https://github.com/madkoding/lumen-wm/issues"',
  '-DPACKAGE="@0@"'.format(meson.project_name()),
  '-DVERSION="@0@"'.format(meson.project_version()),
  '-fno-strict-aliasing',
]
if get_option('buildtype') == 'debug'
  defs += '-DDEBUG'
else
  defs += ['-DNDEBUG', '-DG_DISABLE_ASSERT']
endif
foreach h : ['ctype.h', 'dirent.h', 'errno.h', 'fcntl.h', 'grp.h', 'locale.h',
             'pwd.h', 'signal.h', 'string.h', 'stdio.h', 'stdlib.h',
             'unistd.h', 'sys/stat.h', 'sys/select.h', 'sys/socket.h',
             'sys/time.h', 'sys/types.h', 'sys/wait.h']
  if cc.has_header(h)
    defs += '-DHAVE_@0@'.format(h.underscorify().to_upper())
  endif
endforeach
# X extensions Lumen always builds with
defs += ['-DXKB', '-DXRANDR', '-DSHAPE', '-DXINERAMA', '-DSYNC', '-DAUTH']
nls = get_option('nls')
if nls
  defs += ['-DENABLE_NLS=1', '-DHAVE_GETTEXT=1', '-DHAVE_DCGETTEXT=1']
endif
if libsn.found()
  defs += '-DUSE_LIBSN'
endif
if xcursor.found()
  defs += '-DUSE_XCURSOR=1'
endif
if imlib2.found()
  defs += '-DUSE_IMLIB2'
endif
if librsvg.found()
  defs += '-DUSE_LIBRSVG'
endif
use_sm = sm.found() and ice.found()
if use_sm
  defs += '-DUSE_SM'
endif
add_project_arguments(defs, language: 'c')

paths = [
  '-DLOCALEDIR="@0@"'.format(localedir),
  '-DDATADIR="@0@"'.format(datadir),
  '-DCONFIGDIR="@0@"'.format(configdir),
]

top = include_directories('.')

# like libtool: point lumen-wm at its libraries unless they are in a system
# directory
libdir = prefix / get_option('libdir')
rpath = libdir in ['/usr/lib', '/usr/lib64', '/lib', '/lib64'] or \
        libdir.startswith('/usr/lib/') ? '' : libdir

## generated headers ##

ver = configuration_data()
ver.set('OB_VERSION', meson.project_version())
ver.set('RR_MAJOR_VERSION', rr_major)
ver.set('RR_MINOR_VERSION', rr_minor)
ver.set('RR_MICRO_VERSION', rr_micro)
ver.set('OBT_MAJOR_VERSION', obt_major)
ver.set('OBT_MINOR_VERSION', obt_minor)
ver.set('OBT_MICRO_VERSION', obt_micro)
version_h = configure_file(input: 'version.h.in', output: 'version.h',
                           configuration: ver)
subdir('obt')
subdir('obrender')

## obt ##

obt_lib = library('obt',
  'obt/display.c',
  'obt/keyboard.c',
  'obt/xml.c',
  'obt/ddparse.c',
  'obt/link.c',
  'obt/paths.c',
  'obt/prop.c',
  'obt/signal.c',
  'obt/xqueue.c',
  obt_version_h,
  c_args: paths + ['-DG_LOG_DOMAIN="Obt"'],
  include_directories: top,
  dependencies: [x11, xext, xrandr, xinerama, glib, xml],
  version: '@0@.@1@.@2@'.format(obt_soversion, obt_age, obt_interface_age),
  soversion: obt_soversion,
  install: true)
obt = declare_dependency(link_with: obt_lib, include_directories: top,
                         sources: obt_version_h,
                         dependencies: [x11, glib, xml])

## obrender ##

obrender_lib = library('obrender',
  'obrender/button.c',
  'obrender/color.c',
  'obrender/font.c',
  'obrender/gradient.c',
  'obrender/image.c',
  'obrender/imagecache.c',
  'obrender/instance.c',
  'obrender/mask.c',
  'obrender/render.c',
  'obrender/theme.c',
  rr_version_h,
  c_args: ['-DG_LOG_DOMAIN="ObRender"', '-DDEFAULT_THEME="@0@"'.format(theme)],
  dependencies: [obt, pango, pangocairo, cairo, glib, xml, imlib2, librsvg],
  version: '@0@.@1@.@2@'.format(rr_soversion, rr_age, rr_interface_age),
  soversion: rr_soversion,
  install: true)
obrender = declare_dependency(link_with: obrender_lib, sources: rr_version_h,
                              dependencies: [obt, pango, pangocairo, cairo])

## lumen-wm ##

executable('lumen-wm',
  'openbox/actions/all.c',
  'openbox/actions/addremovedesktop.c',
  'openbox/actions/breakchroot.c',
  'openbox/actions/close.c',
  'openbox/actions/cyclewindows.c',
  'openbox/actions/debug.c',
  'openbox/actions/decorations.c',
  'openbox/actions/desktop.c',
  'openbox/actions/dock.c',
  'openbox/actions/dockautohide.c',
  'openbox/actions/directionalwindows.c',
  'openbox/actions/execute.c',
  'openbox/actions/exit.c',
  'openbox/actions/focus.c',
  'openbox/actions/focusfallback.c',
  'openbox/actions/focustobottom.c',
  'openbox/actions/fullscreen.c',
  'openbox/actions/growtoedge.c',
  'openbox/actions/iconify.c',
  'openbox/actions/if.c',
  'openbox/actions/kill.c',
  'openbox/actions/layer.c',
  'openbox/actions/lower.c',
  'openbox/actions/maximize.c',
  'openbox/actions/move.c',
  'openbox/actions/moverelative.c',
  'openbox/actions/moveresizeto.c',
  'openbox/actions/movetoedge.c',
  'openbox/actions/omnipresent.c',
  'openbox/actions/raise.c',
  'openbox/actions/raiselower.c',
  'openbox/actions/reconfigure.c',
  'openbox/actions/resize.c',
  'openbox/actions/resizerelative.c',
  'openbox/actions/restart.c',
  'openbox/actions/shade.c',
  'openbox/actions/shadelowerraise.c',
  'openbox/actions/showdesktop.c',
  'openbox/actions/showmenu.c',
  'openbox/actions/unfocus.c',
  'openbox/actions.c',
  'openbox/backend.c',
  'openbox/backend-events.c',
  'openbox/backend-x11.c',
  'openbox/backend-x11-canvas.c',
  'openbox/backend-x11-event.c',
  'openbox/backend-x11-surface.c',
  'openbox/client.c',
  'openbox/client_list_menu.c',
  'openbox/client_list_combined_menu.c',
  'openbox/client_menu.c',
  'openbox/config.c',
  'openbox/debug.c',
  'openbox/dock.c',
  'openbox/shelf.c',
  'openbox/event.c',
  'openbox/focus.c',
  'openbox/focus_cycle.c',
  'openbox/focus_cycle_indicator.c',
  'openbox/focus_cycle_popup.c',
  'openbox/frame.c',
  'openbox/framerender.c',
  'openbox/grab.c',
  'openbox/group.c',
  'openbox/keyboard.c',
  'openbox/keytree.c',
  'openbox/menuframe.c',
  'openbox/menu.c',
  'openbox/mouse.c',
  'openbox/moveresize.c',
  'openbox/openbox.c',
  'openbox/paint.c',
  'openbox/ping.c',
  'openbox/place.c',
  'openbox/place_overlap.c',
  'openbox/prompt.c',
  'openbox/popup.c',
  'openbox/resist.c',
  'openbox/screen.c',
  'openbox/session.c',
  'openbox/stacking.c',
  'openbox/startupnotify.c',
  'openbox/translate.c',
  'openbox/window.c',
  version_h,
  c_args: paths + ['-DG_LOG_DOMAIN="Openbox"'],
  dependencies: [obrender, obt, cairo_xlib, x11, xext, xrandr, xinerama,
                 glib, xml, libm, libsn, xcursor, sm, ice],
  export_dynamic: true,
  install_rpath: rpath,
  install: true)

# Lumen used to be Openbox: keep "openbox" working for existing sessions,
# scripts and configs.
install_symlink('openbox', pointing_to: 'lumen-wm', install_dir: bindir)

## tools ##

executable('gdm-control', 'tools/gdm-control/gdm-control.c',
           dependencies: [xau, x11, glib], install: true)
executable('gnome-panel-control',
           'tools/gnome-panel-control/gnome-panel-control.c',
           dependencies: x11, install: true)
executable('obxprop', 'tools/obxprop/obxprop.c',
           dependencies: [glib, x11], install: true)

## tests ##

rendertest = executable('rendertest', 'obrender/test.c',
  c_args: '-DG_LOG_DOMAIN="RenderTest"',
  dependencies: [obrender, obt, glib, xml])

obt_unittests = executable('obt_unittests',
  'obt/unittest_base.c', 'obt/bsearch_unittest.c',
  c_args: paths + ['-DG_LOG_DOMAIN="Obt-Unittests"'],
  dependencies: [obt, glib],
  export_dynamic: true)
test('obt', obt_unittests)

contract_x11 = executable('contract-x11',
  'tests/contract/contract.c',
  'tests/contract/cases.c',
  'tests/contract/driver-x11.c',
  c_args: ['-std=gnu99'] + (xtst.found() ? ['-DCT_HAVE_XTEST'] : []),
  dependencies: [xtst, xext, x11])
# needs Xvfb, xterm and an installed Lumen: tests/contract/run.sh PREFIX
run_target('contract',
  command: ['sh', '-c', 'CONTRACT="$1" "$2" "$3"', 'sh',
            contract_x11.full_path(),
            meson.project_source_root() / 'tests/contract/run.sh', prefix],
  depends: contract_x11)

## headers and pkg-config ##

install_headers(
  'obrender/color.h',
  'obrender/font.h',
  'obrender/geom.h',
  'obrender/gradient.h',
  'obrender/image.h',
  'obrender/instance.h',
  'obrender/mask.h',
  'obrender/render.h',
  'obrender/theme.h',
  rr_version_h,
  install_dir: get_option('includedir') / 'openbox' / rr_version / 'obrender')
install_headers(
  'obt/link.h',
  'obt/display.h',
  'obt/keyboard.h',
  'obt/xml.h',
  'obt/paths.h',
  'obt/prop.h',
  'obt/signal.h',
  'obt/util.h',
  obt_version_h,
  'obt/xqueue.h',
  install_dir: get_option('includedir') / 'openbox' / obt_version / 'obt')

pc = configuration_data()
pc.set('prefix', prefix)
pc.set('exec_prefix', '${prefix}')
pc.set('libdir', '${prefix}' / get_option('libdir'))
pc.set('includedir', '${prefix}' / get_option('includedir'))
pc.set('X_CFLAGS', '')
pc.set('X_LIBS', '-lX11')
pc.set('RR_VERSION', rr_version)
pc.set('OBT_VERSION', obt_version)
pc.set('PKG_CONFIG_IMLIB', imlib2.found() ? 'imlib2' : '')
pc.set('PKG_CONFIG_LIBRSVG', librsvg.found() ? 'librsvg-2.0' : '')
foreach p : ['obrender/obrender-3.5.pc', 'obt/obt-3.5.pc']
  configure_file(input: p + '.in', output: fs.name(p), configuration: pc,
                 install_dir: get_option('libdir') / 'pkgconfig')
endforeach

## data ##

edit = configuration_data()
edit.set('version', meson.project_version())
edit.set('configdir', configdir)
edit.set('rcdir', rcdir)
edit.set('libexecdir', libexecdir)
edit.set('bindir', prefix / bindir)

foreach s : ['openbox-session', 'openbox-gnome-session', 'openbox-kde-session']
  configure_file(input: 'data/xsession' / s + '.in', output: s,
                 configuration: edit, install_dir: bindir,
                 install_mode: 'rwxr-xr-x')
endforeach
foreach d : ['openbox.desktop', 'openbox-gnome.desktop', 'openbox-kde.desktop']
  configure_file(input: 'data/xsession' / d + '.in', output: d,
                 configuration: edit,
                 install_dir: get_option('datadir') / 'xsessions')
endforeach
configure_file(input: 'data/autostart/autostart.in', output: 'autostart',
               configuration: edit, install_dir: rcdir,
               install_mode: 'rwxr-xr-x')
configure_file(input: 'data/autostart/openbox-autostart.in',
               output: 'openbox-autostart', configuration: edit,
               install_dir: get_option('libexecdir'),
               install_mode: 'rwxr-xr-x')
install_data('data/autostart/openbox-xdg-autostart',
             install_dir: get_option('libexecdir'),
             install_mode: 'rwxr-xr-x')
install_data('data/environment', install_dir: rcdir,
             install_mode: 'rwxr-xr-x')
install_data('data/rc.xml', 'data/menu.xml', install_dir: rcdir)
install_data('data/openbox.desktop',
             install_dir: get_option('datadir') / 'applications')
install_data('data/openbox.png',
             install_dir: get_option('datadir') / 'pixmaps')
install_data('data/gnome-wm-properties/openbox.desktop',
             install_dir: get_option('datadir') / 'gnome' / 'wm-properties')
install_data('data/gnome-session/openbox-gnome.session',
             'data/gnome-session/openbox-gnome-fallback.session',
             install_dir: get_option('datadir') / 'gnome-session' / 'sessions')
install_data('COMPLIANCE', 'README', 'README.md', 'AUTHORS', 'CHANGELOG',
             'COPYING', 'data/rc.xsd', 'data/menu.xsd',
             'doc/rc-mouse-focus.xml', install_dir: docdir)
install_subdir('data/xbm', install_dir: docdir)

foreach m : ['openbox', 'openbox-session', 'openbox-gnome-session',
             'openbox-kde-session', 'obxprop']
  configure_file(input: 'doc' / m + '.1.in', output: m + '.1',
                 configuration: edit,
                 install_dir: get_option('mandir') / 'man1')
endforeach

install_subdir('themes', install_dir: themedir, strip_directory: true,
               exclude_files: 'Makefile')

if nls
  subdir('po')
endif

summary({
  'Startup notification': libsn.found(),
  'X Cursor library': xcursor.found(),
  'Session management': use_sm,
  'Imlib2': imlib2.found(),
  'SVG (librsvg)': librsvg.found(),
  'Translations': nls,
})
