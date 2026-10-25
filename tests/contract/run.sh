#!/bin/sh
# Run the contract tests on X11: starts Xvfb and Lumen with the default
# configuration, then the cases.
#
#   tests/contract/run.sh [PREFIX [CASE...]]
#
# PREFIX is where Lumen is installed (default /usr/local). CONTRACT is the
# contract-x11 binary (default: built next to this script by
# `make tests/contract/contract-x11`). Needs Xvfb.
set -u
PREFIX=${1:-/usr/local}
[ $# -gt 0 ] && shift
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CONTRACT=${CONTRACT:-$HERE/contract-x11}
DISP=${DISP:-:96}
WORK=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$WORK"' EXIT

WM=$PREFIX/bin/lumen-wm
[ -x "$WM" ] || WM=$PREFIX/bin/openbox

Xvfb $DISP -screen 0 1280x800x24 -nolisten tcp >"$WORK/xvfb.log" 2>&1 &
i=0
until [ -e "/tmp/.X11-unix/X${DISP#:}" ] || [ $i -ge 50 ]; do
    sleep 0.1; i=$((i + 1))
done
export DISPLAY=$DISP

mkdir -p "$WORK/home"
HOME=$WORK/home XDG_CONFIG_HOME=$WORK/home/.config \
    XDG_DATA_DIRS=$PREFIX/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share} \
    "$WM" --sm-disable --config-file "$ROOT/data/rc.xml" \
    >"$WORK/wm.log" 2>&1 &
wm=$!

"$CONTRACT" "$@"
status=$?
if [ $status -ne 0 ]; then
    echo "# window manager log:"
    sed 's/^/#   /' "$WORK/wm.log"
fi
kill $wm 2>/dev/null
exit $status
