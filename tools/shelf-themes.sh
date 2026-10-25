#!/bin/bash
# Take a screenshot of the shelf under every bundled Openbox theme and check
# that its text reads on its islands (contrast >= 4.5:1).
#
#   tools/shelf-themes.sh [PREFIX [OUTPUT.png]]
#
# PREFIX is where Openbox is installed (default /usr/local); OUTPUT defaults to
# docs/screenshots/shelf-themes.png. Needs Xvfb, xdotool, xterm and ImageMagick.
# Exits non-zero when a theme fails the contrast check.
set -u
PREFIX=${1:-/usr/local}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${2:-$ROOT/docs/screenshots/shelf-themes.png}
WORK=$(mktemp -d)
DISP=:97
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$WORK"' EXIT

cat >"$WORK/media" <<'X'
#!/bin/sh
printf 'Playing\nMidnight City\nM83\n\n'
X
cat >"$WORK/weather" <<'X'
#!/bin/sh
printf '59°\nLight rain\nFri|74°|cloud-sun\nSat|69°|sun\nSun|67°|cloud\n'
X
chmod +x "$WORK/media" "$WORK/weather"

status=0
shots=()
for dir in "$ROOT"/themes/*/; do
    t=$(basename "$dir")
    [ -d "$dir/openbox-3" ] || continue
    home=$WORK/home-$t
    mkdir -p "$home/.config/openbox" "$home/.themes"
    cp -r "$ROOT"/themes/* "$home/.themes/"
    python3 - "$ROOT/data/rc.xml" "$home/.config/openbox/rc.xml" "$t" \
              "$WORK" <<'PY'
import re, sys
src, dst, theme, work = sys.argv[1:5]
s = open(src).read()
s = re.sub(r'(<theme>\s*<name>)[^<]*', r'\g<1>' + theme, s, count=1)
s = s.replace('<enabled>no</enabled>', '<enabled>yes</enabled>', 1)
s = re.sub(r'(<shelf>.*?)<pins>.*?</pins>', r'\1<pins>'
           '<pin><name>Terminal</name><command>xterm</command>'
           '<class>XTerm</class></pin>'
           '<pin><name>Files</name><command>true</command></pin></pins>',
           s, count=1, flags=re.S)
s = s.replace('</shelf>',
              '<media><command>%s/media</command><previous>true</previous>'
              '<toggle>true</toggle><next>true</next></media>'
              '<weather><command>%s/weather</command></weather></shelf>'
              % (work, work), 1)
open(dst, 'w').write(s)
PY
    Xvfb $DISP -screen 0 1000x420x24 >/dev/null 2>&1 &
    xp=$!
    sleep 1
    export DISPLAY=$DISP
    display -window root -size 1000x420 gradient:'#6d8fb5-#3b5173' &
    HOME=$home OB_SHELF_DEBUG=1 "$PREFIX/bin/openbox" \
        --config-file "$home/.config/openbox/rc.xml" 2>"$WORK/$t.log" &
    op=$!
    sleep 1
    xterm -geometry 60x8+60+40 -T Focused &
    xterm -geometry 50x6+420+120 -T Other &
    sleep 1.5
    xdotool mousemove 175 372; xdotool mousemove 181 376; sleep 0.7
    import -window root "$WORK/$t.png"
    kill $op $xp 2>/dev/null; wait 2>/dev/null

    line=$(grep -m1 shelf-material "$WORK/$t.log")
    c=$(sed -n 's/.* contrast=\([0-9.]*\) .*/\1/p' <<<"$line")
    if [ -z "$c" ] || awk "BEGIN{exit !($c < 4.5)}"; then
        echo "FAIL $t: text contrast ${c:-unknown}" >&2
        status=1
    else
        echo "ok   $t: text contrast $c"
    fi
    shots+=("$WORK/$t.png")
done

montage -label '%t' "${shots[@]}" -tile 2x -geometry 900x+8+8 -pointsize 18 \
        -background '#1b1d21' -fill white "$OUT"
echo "wrote $OUT"
exit $status
