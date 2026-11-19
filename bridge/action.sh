#!/system/bin/sh
# Switch Pro Controller Rumble Bridge - Magisk Action.
# Key-driven menu (Magisk's Action stdin is not a TTY on this device):
#   Volume Up   = move to next item
#   Volume Down = confirm selection
# Physical key codes are inverted on this Meizu unit (verified by capture):
#   physical Volume-Up   -> KEY_VOLUMEDOWN 0073
#   physical Volume-Down -> KEY_VOLUMEUP   0072
MODDIR=${0%/*}
BIN="$MODDIR/bin/switch_rumble_bridge"
PIDFILE=/data/local/tmp/switch_rumble_bridge.pid
LOG=/data/local/tmp/switch_rumble_bridge.log
GAINFILE=/data/local/tmp/switch_rumble_bridge.gain
MENU_TIMEOUT=90

alive() { [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; }

state_text() {
  if alive; then echo "RUNNING (pid $(cat "$PIDFILE"))"; else echo "stopped"; fi
}

list_devices() {
  echo 'Bluetooth HID devices:'
  FOUND=0
  for HID_PATH in /sys/bus/hid/devices/0005:*; do
    [ -e "$HID_PATH" ] || continue
    HID_ID=${HID_PATH##*/}
    NAME=$(sed -n 's/^HID_NAME=//p' "$HID_PATH/uevent" 2>/dev/null | head -n 1)
    [ -n "$NAME" ] || NAME=$(cat "$HID_PATH/name" 2>/dev/null)
    [ -n "$NAME" ] || NAME='(unnamed HID device)'
    MARK=''
    case "$HID_ID" in
      0005:057E:2009.*) MARK='  <-- Switch Pro Controller (supported)' ;;
    esac
    echo "  $NAME$MARK"
    FOUND=1
  done
  [ "$FOUND" = 1 ] || echo '  (none connected)'
}

# Wait for one volume-key press. Prints NEXT or CHOOSE; empty on timeout.
wait_key() {
  line=$(timeout "$1" getevent -q 2>/dev/null | grep -m1 -E '0001 007[23] 00000001$')
  [ -n "$line" ] || return 1
  code=$(echo "$line" | awk '{print $3}')
  case "$code" in
    0073) echo NEXT ;;
    0072) echo CHOOSE ;;
    *) return 1 ;;
  esac
}

gain_label() {
  case "$1" in
    70) echo '弱' ;;
    100) echo '标准' ;;
    130) echo '强' ;;
    160) echo '超强' ;;
    *) echo '自定义' ;;
  esac
}

restore_volume() {
  [ -n "$VOL_SAVED" ] && settings put system volume_music "$VOL_SAVED" 2>/dev/null
}

[ -f "$BIN" ] && [ ! -x "$BIN" ] && chmod 755 "$BIN" 2>/dev/null
if [ ! -x "$BIN" ]; then
  echo "ERROR: $BIN missing"
  exit 1
fi

VOL_SAVED=$(settings get system volume_music 2>/dev/null)
trap restore_volume EXIT HUP INT TERM

GAIN=$(cat "$GAINFILE" 2>/dev/null)
case "$GAIN" in 70|100|130|160) ;; *) GAIN=100 ;; esac

N=6
sel=1

echo '=== Switch Pro Controller Rumble Bridge ==='
echo "Bridge: $(state_text)"
list_devices
echo
echo '音量+ = 下一项    音量- = 确认    (90 秒无操作自动退出)'
echo

while :; do
  case "$sel" in
    1) label='启动桥接' ;;
    2) label="震动强度: $(gain_label "$GAIN") (${GAIN}%)  [确认切换下一档]" ;;
    3) label='震动自测 (约 4 秒)' ;;
    4) label='查看状态和日志' ;;
    5) label='停止桥接' ;;
    *) label='退出' ;;
  esac
  printf '> 选择: [%d/%d] %s\n' "$sel" "$N" "$label"

  key=$(wait_key $MENU_TIMEOUT) || { echo '未检测到按键，退出。'; exit 0; }
  if [ "$key" = NEXT ]; then
    sel=$(( sel % N + 1 ))
    continue
  fi

  echo
  case "$sel" in
    1)
      if alive; then
        echo "Bridge 已在运行 (pid $(cat "$PIDFILE"))。"
        exit 0
      fi
      rm -f "$PIDFILE"
      : > "$LOG"
      setsid "$BIN" --gain "$GAIN" >> "$LOG" 2>&1 &
      echo $! > "$PIDFILE"
      sleep 1
      if alive; then
        echo "Bridge 已启动 (pid $(cat "$PIDFILE")), 强度 $(gain_label "$GAIN") (${GAIN}%)。"
        echo '再点 Action 可停止。'
        echo '--- 日志 ---'
        cat "$LOG"
      else
        echo '启动失败，日志：'
        cat "$LOG"
        rm -f "$PIDFILE"
        exit 1
      fi
      ;;
    2)
      case "$GAIN" in
        70) GAIN=100 ;;
        100) GAIN=130 ;;
        130) GAIN=160 ;;
        *) GAIN=70 ;;
      esac
      echo "$GAIN" > "$GAINFILE"
      echo "震动强度已设为: $(gain_label "$GAIN") (${GAIN}%)"
      echo '（下次启动桥接时生效；运行中请先停止再启动）'
      sel=1
      continue
      ;;
    3)
      if alive; then
        echo '桥接运行中，先停止再自测（菜单选「停止桥接」）。'
        exit 1
      fi
      "$BIN" --gain "$GAIN" --selftest 0x8000 0x8000 1800
      echo "自测结束 (rc=$?)，当前强度 $(gain_label "$GAIN") (${GAIN}%)。"
      ;;
    4)
      echo "Bridge: $(state_text)"
      echo "强度: $(gain_label "$GAIN") (${GAIN}%)"
      list_devices
      echo '--- 日志末尾 ---'
      tail -n 15 "$LOG" 2>/dev/null || echo '(无日志)'
      ;;
    5)
      if alive; then
        PID=$(cat "$PIDFILE")
        kill "$PID" 2>/dev/null
        sleep 1
        kill -9 "$PID" 2>/dev/null
        rm -f "$PIDFILE"
        echo 'Bridge 已停止，手柄恢复直连。'
      else
        echo 'Bridge 未在运行。'
      fi
      ;;
    *)
      echo '退出。'
      ;;
  esac
  exit 0
done
