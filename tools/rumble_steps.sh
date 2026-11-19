#!/system/bin/sh
# Switch Pro Controller stepped rumble test: 5 intensity levels, small to large.
# Encodings follow Linux hid-nintendo.c (default 160/320 Hz frequencies,
# amplitude bytes taken from the driver's amplitude table).
# Each level: 8 rumble reports at 50 ms (~400 ms vibration), then a stop
# report and a 300 ms pause so the levels are clearly distinguishable.
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

if [ -z "$target" ] || [ ! -c "$target" ]; then
    echo "Switch Pro Controller hidraw node not found (VID:PID 057e:2009)."
    exit 1
fi

echo "Switch Pro Controller: $target"
exec 3>"$target"
pk=0

stop_rumble() {
    # zero-amplitude encoding on both motors
    printf '\x10\x00\x00\x01\x40\x40\x00\x01\x40\x40' >&3 2>/dev/null || true
}
trap stop_rumble EXIT HUP INT TERM

send_step() {
    label=$1; shift
    i=0
    while [ "$i" -lt 8 ]; do
        printf "\\x10\\x$(printf '%02x' "$pk")\\x$1\\x$2\\x$3\\x$4\\x$1\\x$2\\x$3\\x$4" >&3
        pk=$(( (pk + 1) % 16 ))
        i=$(( i + 1 ))
        sleep 0.05
    done
    stop_rumble
    sleep 0.3
    echo "step done: $label"
}

# motor bytes: 00 <01+amp_high> <40+amp_low_hi> <amp_low_lo>, same on both motors
send_step "level 1 (~12%)" 00 21 40 48
send_step "level 2 (~27%)" 00 51 40 54
send_step "level 3 (~46%)" 00 81 40 60
send_step "level 4 (~65%)" 00 a1 40 68
send_step "level 5 (100%)" 00 c9 40 72

trap - EXIT HUP INT TERM
exec 3>&-
echo "All levels complete."
