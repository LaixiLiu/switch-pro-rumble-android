#!/system/bin/sh
# Single rumble burst, parametric.
# Usage: rumble_one.sh B0 B1 B2 B3 [packets]
#   B0..B3 = 4 motor bytes in hex (applied to both motors)
#   packets = number of 50 ms reports, default 10 (~500 ms)
# Example (level-5 amplitude at default 160/320 Hz, ~500 ms):
#   sh /data/local/tmp/rumble_one.sh 00 c9 40 72 10
set -eu

b0=${1:?usage: rumble_one.sh B0 B1 B2 B3 [packets]}
b1=${2:?missing B1}
b2=${3:?missing B2}
b3=${4:?missing B3}
n=${5:-10}

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

echo "node: $target  bytes: $b0 $b1 $b2 $b3  packets: $n"
exec 3>"$target"
pk=0

stop_rumble() {
    printf '\x10\x00\x00\x01\x40\x40\x00\x01\x40\x40' >&3 2>/dev/null || true
}
trap stop_rumble EXIT HUP INT TERM

i=0
while [ "$i" -lt "$n" ]; do
    printf "\\x10\\x$(printf '%02x' "$pk")\\x$b0\\x$b1\\x$b2\\x$b3\\x$b0\\x$b1\\x$b2\\x$b3" >&3
    pk=$(( (pk + 1) % 16 ))
    i=$(( i + 1 ))
    sleep 0.05
done

stop_rumble
trap - EXIT HUP INT TERM
exec 3>&-
echo "done."
