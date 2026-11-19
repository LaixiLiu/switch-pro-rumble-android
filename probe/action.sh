#!/system/bin/sh

# Manual one-shot Switch Pro rumble probe. No background process is started.
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

# Linux hid-nintendo.c rumble encoding, default frequencies (160/320 Hz),
# approximately 30% amplitude. Report 0x10 + packet counter + 8 rumble bytes.
printf '\x10\x00\x00\x61\x80\x56\x00\x61\x80\x56' > "$target"
sleep 0.15

# Send explicit zero-amplitude rumble frame immediately after the pulse.
printf '\x10\x01\x00\x01\x00\x40\x00\x01\x00\x40' > "$target"
echo "Rumble pulse and stop reports sent."
