#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

IMG="${CIUKIOS_FULL_IMG:-$ROOT_DIR/build/full/ciukios-full.img}"
QEMU_CMD="${QEMU_SYSTEM_I386:-qemu-system-i386}"
NORMALIZER="$ROOT_DIR/scripts/serial_log_normalize.py"
DO_BUILD=1
[[ "${1:-}" == "--no-build" ]] && DO_BUILD=0

if (( DO_BUILD )); then
  bash scripts/build_full.sh
fi

for command_name in "$QEMU_CMD" cp ps python3 socat mtype mcopy nasm; do
  command -v "$command_name" >/dev/null 2>&1 \
    || { echo "[hardware-controls] ERROR missing command: $command_name" >&2; exit 1; }
done
[[ -f "$IMG" ]] || { echo "[hardware-controls] ERROR missing image: $IMG" >&2; exit 1; }

test_dir="$(mktemp -d /tmp/ciukios-hardware-controls.XXXXXX)"
test_img="$test_dir/ciukios-full.img"
serial_log="$test_dir/serial.log"
stderr_log="$test_dir/qemu.stderr.log"
monitor_sock="$test_dir/monitor.sock"
qemu_pid=0

cleanup() {
  if (( qemu_pid != 0 )) && kill -0 "$qemu_pid" >/dev/null 2>&1; then
    if [[ -S "$monitor_sock" ]]; then
      printf 'quit\n' | socat - UNIX-CONNECT:"$monitor_sock" >/dev/null 2>&1 || true
    fi
    kill "$qemu_pid" >/dev/null 2>&1 || true
    wait "$qemu_pid" >/dev/null 2>&1 || true
  fi
  if [[ "${HARDWARE_CONTROLS_KEEP_ARTIFACTS:-0}" == 1 ]]; then
    echo "[hardware-controls] artifacts: $test_dir"
  elif [[ -d "$test_dir" && "$test_dir" == /tmp/ciukios-hardware-controls.* ]]; then
    rm -rf -- "$test_dir"
  fi
}
trap cleanup EXIT

fail() {
  echo "[hardware-controls] ERROR $1" >&2
  if [[ -S "$monitor_sock" ]]; then
    printf 'stop\ninfo registers\ninfo pic\n' \
      | socat - UNIX-CONNECT:"$monitor_sock" >"$test_dir/failure-registers.log" 2>&1 || true
    printf 'screendump %s/failure.ppm\npmemsave 0 1048576 %s/failure-memory.bin\n' "$test_dir" "$test_dir" \
      | socat - UNIX-CONNECT:"$monitor_sock" >/dev/null 2>&1 || true
  fi
  [[ -f "$serial_log" ]] && "$NORMALIZER" "$serial_log" | tail -n 100 >&2 || true
  [[ -f "$stderr_log" ]] && tail -n 40 "$stderr_log" >&2 || true
  exit 1
}

hmp() {
  printf '%s\n' "$1" | socat - UNIX-CONNECT:"$monitor_sock" >/dev/null
}

send_text() {
  local value="$1" i ch key
  for ((i=0; i<${#value}; i++)); do
    ch="${value:i:1}"
    case "$ch" in
      ' ') key=spc ;;
      [a-z0-9]) key="$ch" ;;
      *) fail "unsupported test input character: $ch" ;;
    esac
    hmp "sendkey $key 20"
    sleep 0.04
  done
  hmp 'sendkey ret 20'
}

wait_serial() {
  local offset="$1" pattern="$2" timeout_sec="$3" start
  start="$(date +%s)"
  while (( $(date +%s) - start < timeout_sec )); do
    if [[ -f "$serial_log" ]] \
        && "$NORMALIZER" --offset "$offset" "$serial_log" | grep -aFq -- "$pattern"; then
      return 0
    fi
    sleep 0.10
  done
  return 1
}

cp --reflink=auto -- "$IMG" "$test_img"
nasm -f bin scripts/fixtures/vbe_reject.asm -o "$test_dir/vberej.com"
mcopy -o -i "$test_img" "$test_dir/vberej.com" ::APPS/VBEREJ.COM
nasm -f bin scripts/fixtures/com_return.asm -o "$test_dir/nearret.com"
nasm -f bin -DFAR_RETURN=1 scripts/fixtures/com_return.asm -o "$test_dir/farret.com"
mcopy -o -i "$test_img" "$test_dir/nearret.com" "$test_dir/farret.com" ::APPS/
"$QEMU_CMD" \
  -accel tcg \
  -machine pc,vmport=off,i8042=on \
  -cpu pentium3 \
  -m 256 \
  -drive "file=$test_img,format=raw,if=ide" \
  -boot c \
  -display none \
  -chardev "file,id=ser0,path=$serial_log" \
  -serial chardev:ser0 \
  -monitor "unix:$monitor_sock,server,nowait" \
  -no-reboot \
  >/dev/null 2>"$stderr_log" &
qemu_pid=$!

for _ in $(seq 1 200); do
  [[ -S "$monitor_sock" ]] && break
  sleep 0.05
done
[[ -S "$monitor_sock" ]] || fail "QEMU monitor did not start"
wait_serial 0 'CiukiOS SHELL C:\APPS>' 30 || fail "initial shell prompt not reached"

for return_kind in near far; do
  offset="$(wc -c < "$serial_log")"
  send_text "${return_kind}ret"
  wait_serial "$offset" '[COMRET]' 15 || fail "$return_kind RET fixture did not execute"
  wait_serial "$offset" 'CiukiOS SHELL C:\APPS>' 15 \
    || fail "COM $return_kind RET did not restore the parent shell"
done
echo '[hardware-controls] PASS COM near RET and legacy RETF terminate through DOS'

offset="$(wc -c < "$serial_log")"
send_text 'vgasetup status'
wait_serial "$offset" 'Safe status: no BIOS VBE or embedded-controller probe executed' 15 \
  || fail "VGASETUP STATUS did not use its hardware-safe status path"
wait_serial "$offset" 'CiukiOS SHELL C:\APPS>' 15 || fail "VGASETUP STATUS did not return to the shell"
# Serial output alone missed the regression: it was printed, then mode 03h
# erased it on the physical display. Check page-zero VGA memory after return.
hmp "pmemsave 0xb8000 4000 \"$test_dir/vgasetup.vram\""
python3 - "$test_dir/vgasetup.vram" <<'PY'
import pathlib
import sys
text = pathlib.Path(sys.argv[1]).read_bytes()[::2].decode('cp437')
assert 'VGASETUP commands:' in text, 'VGASETUP help was erased from VGA page zero'
assert 'vgasetup text 25|50' in text, 'VGASETUP options are not visible'
print('[hardware-controls] PASS VGASETUP output remains in visible VGA memory')
PY

offset="$(wc -c < "$serial_log")"
send_text 'vgasetup detect'
wait_serial "$offset" 'VBE BIOS version 0x' 15 || fail "explicit VBE BIOS detection failed"
wait_serial "$offset" 'Generic hardware: safe VGA default; banked VBE profiles available' 15 \
  || fail "generic safe-VGA/VBE detection was not reported"
wait_serial "$offset" 'CiukiOS SHELL C:\APPS>' 15 || fail "DETECT did not return to the shell"

offset="$(wc -c < "$serial_log")"
send_text 'vgasetup modes'
wait_serial "$offset" 'Available VBE video modes:' 20 \
  || fail "VBE mode/EDID capability enumeration failed"
wait_serial "$offset" 'CiukiOS SHELL C:\APPS>' 20 || fail "MODES did not return to the shell"

offset="$(wc -c < "$serial_log")"
send_text 'vgasetup text 50'
wait_serial "$offset" 'CiukiOS 80x50 text console applied' 15 \
  || fail "80x50 mode was not applied"
wait_serial "$offset" 'CiukiOS SHELL C:\APPS>' 15 || fail "TEXT 50 did not return to the shell"

offset="$(wc -c < "$serial_log")"
send_text 'vgasetup status'
wait_serial "$offset" 'CiukiOS text console: 80x50 rows' 15 \
  || fail "80x50 mode did not survive child exit and shell restoration"

offset="$(wc -c < "$serial_log")"
send_text 'vgasetup win 800'
wait_serial "$offset" 'Windows banked VBE 800x600x8 profile installed' 15 \
  || fail "Windows 800x600 profile was not installed"
wait_serial "$offset" 'CiukiOS SHELL C:\APPS>' 30 || fail "shared 800x600 shell did not return"

offset="$(wc -c < "$serial_log")"
send_text 'vgasetup win safe'
wait_serial "$offset" 'Windows safe VGA 640x480 profile installed' 15 \
  || fail "Windows safe recovery profile was not restored"
wait_serial "$offset" 'CiukiOS SHELL C:\APPS>' 30 || fail "VGA text recovery did not return"

offset="$(wc -c < "$serial_log")"
send_text 'vgasetup refresh 60'
wait_serial "$offset" 'Refresh AUTO/60 selected' 15 || fail "safe LCD refresh selection failed"

offset="$(wc -c < "$serial_log")"
send_text 'vgasetup brightness 50'
wait_serial "$offset" 'Brightness has no generic VBE control' 15 \
  || fail "non-ThinkPad brightness safety fallback failed"

# Exercise the actual preview, including rejected firmware operations. The
# previous gate only saved profiles, so it could not catch a crashing preview.
offset="$(wc -c < "$serial_log")"
send_text 'vgasetup test 0103'
wait_serial "$offset" 'Press any key to switch to this mode' 15 \
  || fail "MODETEST did not reach its preview prompt"
hmp 'sendkey spc 20'
sleep 2
hmp "screendump $test_dir/mode103.ppm"
[[ "$(sed -n '2p' "$test_dir/mode103.ppm")" == '800 600' ]] \
  || fail "MODETEST did not enter 800x600"
hmp 'sendkey spc 20'
wait_serial "$offset" 'CiukiOS SHELL C:\APPS>' 15 \
  || fail "MODETEST did not restore a working shell"

offset="$(wc -c < "$serial_log")"
send_text 'vberej'
wait_serial "$offset" '[VBEREJ] Installed' 15 || fail "VBE rejection fixture did not install"
for rejected_operation in mode bank; do
  offset="$(wc -c < "$serial_log")"
  send_text 'vgasetup test 0103'
  wait_serial "$offset" 'Press any key to switch to this mode' 15 \
    || fail "MODETEST did not reach the $rejected_operation rejection fixture"
  hmp 'sendkey spc 20'
  wait_serial "$offset" 'VBE mode or bank request failed; text mode restored' 15 \
    || fail "MODETEST ignored a rejected $rejected_operation request"
  wait_serial "$offset" 'CiukiOS SHELL C:\APPS>' 15 \
    || fail "MODETEST did not return after rejected $rejected_operation"
done
echo '[hardware-controls] PASS real VBE preview and rejected modeset/bank recovery'

# The new interactive preview uses its own renderer. Exercise its failure
# paths too: it must leave both saved profiles intact when firmware refuses.
offset="$(wc -c < "$serial_log")"
send_text 'vberej'
wait_serial "$offset" '[VBEREJ] Installed' 15 || fail "interactive VBE rejection fixture did not install"
wait_serial "$offset" 'CiukiOS SHELL C:\APPS>' 15 || fail "rejection fixture did not return"
mtype -i "$test_img" ::WINDOWS/SYSTEM.INI > "$test_dir/before-rejected-menu.ini"
for rejected_operation in mode bank; do
  offset="$(wc -c < "$serial_log")"
  send_text 'vgasetup'
  wait_serial "$offset" '[VGASETUP] MENU READY' 15 || fail "interactive display menu did not open"
  hmp 'sendkey 2 20'
  sleep 0.2
  hmp 'sendkey ret 20'
  wait_serial "$offset" 'VBE mode unavailable' 20 \
    || fail "interactive preview did not handle rejected $rejected_operation"
  hmp "screendump $test_dir/rejected-menu-$rejected_operation.ppm"
  [[ "$(sed -n '2p' "$test_dir/rejected-menu-$rejected_operation.ppm")" == '720 400' ]] \
    || fail "interactive preview did not restore readable VGA text"
  offset="$(wc -c < "$serial_log")"
  hmp 'sendkey spc 20'
  wait_serial "$offset" '[VGASETUP] MENU READY' 15 || fail "menu did not recover after rejected preview"
  hmp 'sendkey esc 20'
  wait_serial "$offset" 'CiukiOS SHELL C:\APPS>' 15 || fail "rejected preview did not return to the shell"
  [[ "$(mtype -i "$test_img" ::SYSTEM/VIDEO/DISPLAY.CFG)" == TEXT ]] \
    || fail "rejected preview changed the shell setting"
  mtype -i "$test_img" ::WINDOWS/SYSTEM.INI > "$test_dir/after-rejected-menu.ini"
  cmp -s "$test_dir/before-rejected-menu.ini" "$test_dir/after-rejected-menu.ini" \
    || fail "rejected preview changed the Windows setting"
done
echo '[hardware-controls] PASS interactive preview recovers from rejected modeset/bank without saving'

offset="$(wc -c < "$serial_log")"
send_text 'reboot'
wait_serial "$offset" 'rebooting...' 10 || fail "reboot command was not dispatched"

for _ in $(seq 1 100); do
  qemu_state="$(ps -o stat= -p "$qemu_pid" 2>/dev/null | tr -d '[:space:]' || true)"
  if [[ -z "$qemu_state" || "$qemu_state" == Z* ]]; then
    wait "$qemu_pid" || true
    qemu_pid=0
    persisted_text="$(mtype -i "$test_img" ::SYSTEM/VIDEO/VGASET.CFG | tr -d '\r\n')"
    [[ "$persisted_text" == 50 ]] || fail "text profile was not persisted to the FAT volume"
    persisted_windows="$(mtype -i "$test_img" ::WINDOWS/SYSTEM.INI | tr -d '\r')"
    grep -Fxq 'display.drv=vga.drv' <<< "$persisted_windows" \
      || fail "transactional WIN SAFE profile did not persist"
    echo "[hardware-controls] PASS VBE status, persistent 80x50 console and safe display controls"
    echo "[hardware-controls] PASS chipset/8042 reset reached QEMU system-reset path"
    exit 0
  fi
  sleep 0.10
done
if [[ -S "$monitor_sock" ]]; then
  printf 'stop\ninfo registers\n' \
    | socat - UNIX-CONNECT:"$monitor_sock" >"$test_dir/reboot-registers.log" 2>&1 || true
  cat "$test_dir/reboot-registers.log" >&2 || true
fi
fail "reboot did not assert a hardware reset under -no-reboot"
