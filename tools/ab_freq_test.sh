#!/system/bin/sh
# A/B frequency test: alternate 160/320 Hz vs 150 Hz at max amplitude.
# Each burst 1.5 s (30 reports x 50 ms), 2 s silence between bursts.
# Sequence: A B A B  (A = 160/320 Hz, B = 150 Hz)
set -eu

target=""
for link in /sys/class/hidraw/hidraw*; do
    [ -e "$link" ] || continue
    resolved="$(readlink -f "$link" 2>/dev/null || true)"
    case "$resolved" in
        *0005:057E:2009.*/hidraw/*)
            target="/dev/${link##*/}"
            break
            ;;
    esac
done
[ -n "$target" ] && [ -c "$target" ] || { echo "controller not found"; exit 1; }
echo "node: $target"

exec 3>"$target"
pk=0

neutral() {
    printf '\x10\x00\x00\x01\x40\x40\x00\x01\x40\x40' >&3 2>/dev/null || true
}
trap neutral EXIT HUP INT TERM

burst() {
    label=$1; b0=$2; b1=$3; b2=$4; b3=$5
    echo "== $label START ($b0 $b1 $b2 $b3) =="
    i=0
    while [ "$i" -lt 30 ]; do
        printf "\\x10\\x$(printf '%02x' "$pk")\\x$b0\\x$b1\\x$b2\\x$b3\\x$b0\\x$b1\\x$b2\\x$b3" >&3
        pk=$(( (pk + 1) % 16 ))
        i=$(( i + 1 ))
        sleep 0.05
    done
    neutral
    echo "== $label END =="
}

sleep 1
burst "A1 160/320Hz" 00 c9 40 72
sleep 2
burst "B1 150Hz"     74 c8 3d 72
sleep 2
burst "A2 160/320Hz" 00 c9 40 72
sleep 2
burst "B2 150Hz"     74 c8 3d 72

trap - EXIT HUP INT TERM
exec 3>&-
echo "done."
