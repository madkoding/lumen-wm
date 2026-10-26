#!/bin/sh
# Run the contract tests on Wayland: starts Lumen on a headless wlroots
# output with the default configuration, then the cases; then, for each
# configs/NAME.sed, Lumen again with data/rc.xml changed by it, and the cases
# written for NAME. As run.sh does on X11.
#
#   tests/contract/run-wayland.sh [PREFIX [CASE...]]
#
# PREFIX is where Lumen is installed (default /usr/local), built with
# -Dwayland. CONTRACT is the contract-wayland binary (default:
# build/contract-wayland, from `meson setup build`). WMARGS go to Lumen, as
# in WMARGS=--debug.
set -u
PREFIX=${1:-/usr/local}
[ $# -gt 0 ] && shift
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CONTRACT=${CONTRACT:-$ROOT/build/contract-wayland}
WORK=$(mktemp -d)
wm=
# dash's jobs -p is empty inside a trap, so kill by pid
trap 'kill $wm 2>/dev/null; rm -rf "$WORK"' EXIT

WM=$PREFIX/bin/lumen-wm
[ -x "$WM" ] || WM=$PREFIX/bin/openbox

# a runtime directory of its own, so the socket is the only one there
export XDG_RUNTIME_DIR=$WORK/run
mkdir -m 700 "$XDG_RUNTIME_DIR"
unset DISPLAY WAYLAND_DISPLAY

mkdir -p "$WORK/home"
start_wm() {
    rm -f "$XDG_RUNTIME_DIR"/wayland-*
    HOME=$WORK/home XDG_CONFIG_HOME=$WORK/home/.config \
        XDG_DATA_DIRS=$PREFIX/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share} \
        LUMEN_BACKEND=wayland LUMEN_TEST_PROTOCOL=1 \
        WLR_BACKENDS=headless WLR_RENDERER=${WLR_RENDERER:-pixman} \
        WLR_HEADLESS_OUTPUTS=1 \
        "$WM" --sm-disable ${WMARGS:-} --config-file "$1" >"$WORK/wm.log" 2>&1 &
    wm=$!
    i=0
    until ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -q '^wayland-[0-9]*$' ||
          [ $i -ge 50 ]; do
        sleep 0.1; i=$((i + 1))
    done
    WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
    export WAYLAND_DISPLAY
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
