#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
IMG="${IMG:-build/full/ciukios-full.img}"
COSTA_BOOT_ISO="${COSTA_BOOT_ISO:-}"
PREFIX="${COSTA_TEST_PREFIX:-build/full/qemu-full-costa}"
SERIAL_LOG="${PREFIX}.serial.log"
STDERR_LOG="${PREFIX}.stderr.log"
SCREENSHOT="${PREFIX}.ppm"
CURSOR_SCREENSHOT="${PREFIX}.cursor.ppm"
CALC_SCREENSHOT="${PREFIX}.calc.ppm"
META_LOG="${PREFIX}.meta"
MON_SOCK="${COSTA_MON_SOCK:-/tmp/ciukios-full-costa.$$.monitor.sock}"
QEMU_TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-120}"
COSTA_OBSERVE_SEC="${COSTA_OBSERVE_SEC:-22}"
COSTA_CURSOR_SETTLE_SEC="${COSTA_CURSOR_SETTLE_SEC:-2}"
COSTA_CALC_OBSERVE_SEC="${COSTA_CALC_OBSERVE_SEC:-15}"
QEMU_TEST_ACCEL="${QEMU_TEST_ACCEL:-kvm}"
QEMU_TEST_MEMORY_MB="${QEMU_TEST_MEMORY_MB:-256}"

fail() {
  echo "[costa-test] FAIL: $*" >&2
  exit 1
}

pick_qemu() {
  if [[ -n "${QEMU_BIN:-}" ]]; then
    command -v "$QEMU_BIN" || return 1
  elif command -v qemu-system-i386 >/dev/null 2>&1; then
    command -v qemu-system-i386
  else
    command -v qemu-system-x86_64
  fi
}

hmp() {
  local command="$1"
  printf '%s\n' "$command" | socat - "UNIX-CONNECT:$MON_SOCK" >/dev/null
}

wait_for_socket() {
  local i
  for i in $(seq 1 100); do
    [[ -S "$MON_SOCK" ]] && return 0
    sleep 0.1
  done
  return 1
}

wait_for_shell() {
  local i
  for i in $(seq 1 240); do
    if [[ -s "$SERIAL_LOG" ]] && strings -a "$SERIAL_LOG" | rg -q 'S+H+E+L+L+.*A+P+P+S+>+'; then
      return 0
    fi
    sleep 0.25
  done
  return 1
}

send_text_and_enter() {
  local text="$1"
  local char
  for ((i = 0; i < ${#text}; i++)); do
    char="${text:i:1}"
    case "$char" in
      ' ') hmp 'sendkey spc' ;;
      '.') hmp 'sendkey dot' ;;
      [a-z0-9]) hmp "sendkey $char" ;;
      *) fail "unsupported command character: $char" ;;
    esac
  done
  hmp 'sendkey ret'
}

QEMU_PID=""
cleanup() {
  if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" >/dev/null 2>&1; then
    hmp quit >/dev/null 2>&1 || true
    wait "$QEMU_PID" >/dev/null 2>&1 || true
  fi
  if [[ -S "$MON_SOCK" ]]; then
    rm -f -- "$MON_SOCK"
  fi
}
trap cleanup EXIT

for command_name in curl sha256sum unzip nasm mcopy mdir mmd socat strings rg python3; do
  command -v "$command_name" >/dev/null 2>&1 || fail "missing required command: $command_name"
done
QEMU_CMD="$(pick_qemu)" || fail "qemu-system-i386/x86_64 is unavailable"

case "$QEMU_TEST_ACCEL" in
  kvm|tcg) ;;
  *) fail "QEMU_TEST_ACCEL must be kvm or tcg" ;;
esac
if [[ ! "$QEMU_TEST_MEMORY_MB" =~ ^[0-9]+$ ]] \
  || (( QEMU_TEST_MEMORY_MB < 16 || QEMU_TEST_MEMORY_MB > 4096 )); then
  fail "QEMU_TEST_MEMORY_MB must be an integer from 16 to 4096"
fi

if [[ "$DO_BUILD" == "1" ]]; then
  bash scripts/fetch_costa.sh
  bash scripts/build_full.sh
fi

[[ -f "$IMG" ]] || fail "missing image: $IMG"
for image_path in \
  ::APPS/COSTA/COSTA.EXE \
  ::APPS/COSTA/DESKTOP.EXE \
  ::APPS/COSTA/DATA/FONTDATA.BSV \
  ::APPS/COSTA/DATA/FONTINFO.BSV; do
  mdir -i "$IMG" "$image_path" >/dev/null 2>&1 \
    || fail "missing Costa payload in image: $image_path"
done

mkdir -p "$(dirname "$PREFIX")"
rm -f -- \
  "$SERIAL_LOG" "$STDERR_LOG" "$SCREENSHOT" "$CURSOR_SCREENSHOT" \
  "$CALC_SCREENSHOT" "$META_LOG"

boot_args=(-drive "file=$IMG,format=raw,if=ide" -boot c)
if [[ -n "$COSTA_BOOT_ISO" ]]; then
  [[ -f "$COSTA_BOOT_ISO" ]] || fail "missing CD ISO: $COSTA_BOOT_ISO"
  boot_args=(-cdrom "$COSTA_BOOT_ISO" -boot d)
fi

timeout "$QEMU_TIMEOUT_SEC" "$QEMU_CMD" \
  -accel "$QEMU_TEST_ACCEL" \
  -machine pc,vmport=off,i8042=on \
  -cpu pentium3 \
  -m "$QEMU_TEST_MEMORY_MB" \
  "${boot_args[@]}" \
  -snapshot \
  -display none \
  -serial "file:$SERIAL_LOG" \
  -monitor "unix:$MON_SOCK,server,nowait" \
  -audiodev driver=none,id=audio0 \
  -no-reboot \
  -no-shutdown \
  >/dev/null 2>"$STDERR_LOG" &
QEMU_PID=$!

wait_for_socket || fail "QEMU monitor socket did not appear"
wait_for_shell || fail "CiukiOS shell prompt did not appear"
send_text_and_enter costa
sleep "$COSTA_OBSERVE_SEC"
hmp "screendump \"$ROOT_DIR/$SCREENSHOT\""
[[ -s "$SCREENSHOT" ]] || fail "QEMU did not create the Costa screenshot"

# A static before/after pair proves that IRQ12 motion updates both the INT 33h
# position and the software cursor in QuickBASIC SCREEN 9 (BIOS mode 10h).
hmp 'mouse_move 80 40 0'
sleep "$COSTA_CURSOR_SETTLE_SEC"
hmp "screendump \"$ROOT_DIR/$CURSOR_SCREENSHOT\""
[[ -s "$CURSOR_SCREENSHOT" ]] || fail "QEMU did not create the moved-cursor screenshot"

# Keyboard navigation is deterministic in the stock Costa desktop: the second
# icon is Calculator, Enter opens its confirmation dialog, and Enter launches
# CALC.EXE through COSTA.EXE and DATA/RUN.DAT.
hmp 'sendkey tab'
hmp 'sendkey tab'
hmp 'sendkey ret'
sleep 2
hmp 'sendkey ret'
sleep "$COSTA_CALC_OBSERVE_SEC"
hmp "screendump \"$ROOT_DIR/$CALC_SCREENSHOT\""
[[ -s "$CALC_SCREENSHOT" ]] || fail "QEMU did not create the Calculator screenshot"

if strings -a "$SERIAL_LOG" | rg -qi \
  'Runtime error|Path/File access|Bad file name|Out of memory|EXEC FAIL|Invalid executable'; then
  fail "Costa reported a DOS/runtime error (see $SERIAL_LOG)"
fi

MZ_RUNS="$(strings -a "$SERIAL_LOG" | rg -c '^\[MZ\] run$' || true)"
[[ "$MZ_RUNS" =~ ^[0-9]+$ ]] || fail "could not count Costa MZ launches"
((MZ_RUNS >= 3)) || fail "Calculator did not launch (expected at least 3 MZ runs, got $MZ_RUNS)"

python3 - "$SCREENSHOT" "$CURSOR_SCREENSHOT" "$CALC_SCREENSHOT" "$META_LOG" <<'PY'
from collections import Counter
from pathlib import Path
import sys

desktop_path = Path(sys.argv[1])
cursor_path = Path(sys.argv[2])
calc_path = Path(sys.argv[3])
meta_path = Path(sys.argv[4])


def read_ppm(path):
    parts = path.read_bytes().split(maxsplit=4)
    if len(parts) != 5 or parts[0] != b"P6":
        raise SystemExit(f"[costa-test] FAIL: {path} is not a binary PPM")
    width = int(parts[1])
    height = int(parts[2])
    maximum = int(parts[3])
    pixels = parts[4]
    expected = width * height * 3
    if (width, height, maximum) != (640, 350, 255):
        raise SystemExit(
            f"[costa-test] FAIL: expected 640x350x255, got "
            f"{width}x{height}x{maximum} in {path}"
        )
    if len(pixels) != expected:
        raise SystemExit(
            f"[costa-test] FAIL: truncated screenshot {path} "
            f"({len(pixels)} bytes, expected {expected})"
        )
    return width, height, pixels


width, height, pixels = read_ppm(desktop_path)
cursor_width, cursor_height, cursor_pixels = read_ppm(cursor_path)
_, _, calc_pixels = read_ppm(calc_path)

colors = Counter(zip(pixels[0::3], pixels[1::3], pixels[2::3]))
non_black = width * height - colors[(0, 0, 0)]
dominant_desktop = colors.most_common(1)[0][1]
if len(colors) < 8:
    raise SystemExit(f"[costa-test] FAIL: only {len(colors)} colors; Costa desktop was not rendered")
if non_black < width * height // 2:
    raise SystemExit("[costa-test] FAIL: screenshot is predominantly black")
if dominant_desktop < width * height // 3:
    raise SystemExit("[costa-test] FAIL: Costa desktop background was not detected")

cursor_changes = {
    (channel // 3 % width, channel // 3 // width)
    for channel, (before, after) in enumerate(zip(pixels, cursor_pixels))
    if before != after
}
if not 20 <= len(cursor_changes) <= 128:
    raise SystemExit(
        f"[costa-test] FAIL: expected a small moving cursor delta, "
        f"got {len(cursor_changes)} changed pixels"
    )

calc_colors = Counter(zip(calc_pixels[0::3], calc_pixels[1::3], calc_pixels[2::3]))
calc_black = calc_colors[(0, 0, 0)]
calc_non_black = width * height - calc_black
calc_dominant = calc_colors.most_common(1)[0][1]
calc_changed = sum(
    before != after
    for before, after in zip(
        zip(pixels[0::3], pixels[1::3], pixels[2::3]),
        zip(calc_pixels[0::3], calc_pixels[1::3], calc_pixels[2::3]),
    )
)
if len(calc_colors) < 4:
    raise SystemExit("[costa-test] FAIL: Calculator palette collapsed")
if calc_non_black < width * height // 2:
    raise SystemExit("[costa-test] FAIL: Calculator rendering is predominantly black/corrupt")
if calc_dominant < width * height // 4:
    raise SystemExit("[costa-test] FAIL: Calculator work area was not detected")
if calc_changed < width * height // 4:
    raise SystemExit("[costa-test] FAIL: Calculator did not replace the desktop view")

meta_path.write_text(
    f"width={width}\nheight={height}\ncolors={len(colors)}\n"
    f"non_black_pixels={non_black}\ndominant_desktop_pixels={dominant_desktop}\n"
    f"cursor_changed_pixels={len(cursor_changes)}\n"
    f"calculator_colors={len(calc_colors)}\ncalculator_non_black_pixels={calc_non_black}\n"
    f"calculator_dominant_pixels={calc_dominant}\ncalculator_changed_pixels={calc_changed}\n"
    f"calculator_black_pixels={calc_black}\n",
    encoding="ascii",
)
PY

hmp quit
wait "$QEMU_PID"
QEMU_PID=""

echo "[costa-test] PASS: Costa desktop, mouse cursor, and Calculator rendered at 640x350"
echo "[costa-test] desktop screenshot: $SCREENSHOT"
echo "[costa-test] moved-cursor screenshot: $CURSOR_SCREENSHOT"
echo "[costa-test] Calculator screenshot: $CALC_SCREENSHOT"
echo "[costa-test] metadata: $META_LOG"
