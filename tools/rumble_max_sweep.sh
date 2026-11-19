#!/system/bin/sh
# Switch Pro Controller: beyond-max-amplitude test + resonance frequency sweep.
# All steps use max table amplitude (1003) unless noted; encodings follow
# Linux hid-nintendo.c tables. Each step: 10 reports x 50 ms (~500 ms
# vibration), stop report, 400 ms pause.
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
    printf '\x10\x00\x00\x01\x40\x40\x00\x01\x40\x40' >&3 2>/dev/null || true
}
trap stop_rumble EXIT HUP INT TERM

send_step() {
    label=$1; shift
    i=0
    while [ "$i" -lt 10 ]; do
        printf "\\x10\\x$(printf '%02x' "$pk")\\x$1\\x$2\\x$3\\x$4\\x$1\\x$2\\x$3\\x$4" >&3
        pk=$(( (pk + 1) % 16 ))
        i=$(( i + 1 ))
        sleep 0.05
    done
    stop_rumble
    sleep 0.4
    echo "step done: $label"
}

# --- Part 1: can amplitude exceed the table max? (160/320 Hz fixed) ---
send_step "A1 baseline level5"   00 c9 40 72
send_step "A2 overspec amp+"     00 e9 40 80
send_step "A3 overspec amp++"    00 f9 40 90
send_step "A4 baseline repeat"   00 c9 40 72

# --- Part 2: frequency sweep at table-max amplitude ---
send_step "B1 41/82 Hz"          04 c8 01 72
send_step "B2 82/164 Hz"         84 c8 21 72
send_step "B3 102/199 Hz"        a8 c8 2b 72
send_step "B4 123/247 Hz"        d0 c8 34 72
send_step "B5 203/406 Hz"        2c c9 4b 72
send_step "B6 226/453 Hz"        40 c9 50 72
send_step "B7 313/626 Hz"        7c c9 5f 72

trap - EXIT HUP INT TERM
exec 3>&-
echo "All levels complete."
