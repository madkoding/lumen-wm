#!/bin/sh
# Run the contract tests on X11: starts Xvfb and Lumen with the default
# configuration, then the cases; then, for each configs/NAME.sed, Lumen again
# with data/rc.xml changed by it, and the cases written for NAME.
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
xvfb=
wm=
# dash's jobs -p is empty inside a trap, so kill by pid
trap 'kill $wm $xvfb 2>/dev/null; rm -rf "$WORK"' EXIT

WM=$PREFIX/bin/lumen-wm
[ -x "$WM" ] || WM=$PREFIX/bin/openbox

if [ -e "/tmp/.X11-unix/X${DISP#:}" ]; then
    echo "Bail out! display $DISP is in use; set DISP to another one"
    exit 2
fi
Xvfb $DISP -screen 0 1280x800x24 -nolisten tcp >"$WORK/xvfb.log" 2>&1 &
xvfb=$!
i=0
until [ -e "/tmp/.X11-unix/X${DISP#:}" ] || [ $i -ge 50 ]; do
    sleep 0.1; i=$((i + 1))
done
export DISPLAY=$DISP

mkdir -p "$WORK/home"
start_wm() {
    HOME=$WORK/home XDG_CONFIG_HOME=$WORK/home/.config \
        XDG_DATA_DIRS=$PREFIX/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share} \
        "$WM" --sm-disable --config-file "$1" >"$WORK/wm.log" 2>&1 &
    wm=$!
}
stop_wm() {
    kill $wm 2>/dev/null
    wait $wm 2>/dev/null
    wm=
}
run_cases() {
    "$CONTRACT" "$@"
    s=$?
    if [ $s -ne 0 ]; then
        echo "# window manager log:"
        sed 's/^/#   /' "$WORK/wm.log"
        [ $s -gt $status ] && status=$s
    fi
}

status=0
start_wm "$ROOT/data/rc.xml"
run_cases "$@"
for conf in "$HERE"/configs/*.sed; do
    [ -e "$conf" ] || continue
    name=$(basename "$conf" .sed)
    stop_wm
    sed -f "$conf" "$ROOT/data/rc.xml" >"$WORK/rc-$name.xml"
    if cmp -s "$ROOT/data/rc.xml" "$WORK/rc-$name.xml"; then
        echo "Bail out! configs/$name.sed changes nothing in data/rc.xml"
        exit 2
    fi
    start_wm "$WORK/rc-$name.xml"
    run_cases --config "$name" "$@"
done
exit $status
