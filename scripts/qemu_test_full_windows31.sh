#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

MEDIA_DIR="${WINDOWS31_MEDIA_DIR:-$ROOT_DIR/third_party/windows31}"
IMG="${CIUKIOS_FULL_IMG:-$ROOT_DIR/build/full/ciukios-full.img}"
RUNTIME_BIN="$ROOT_DIR/build/full/obj/ciukidos.sys"
RUNTIME_LST="$ROOT_DIR/build/full/obj/ciukidos.lst"
DISPLAY_BACKEND="${QEMU_DISPLAY:-auto}"
DO_BUILD=1
PREPARE_ONLY=0
HEADLESS_SMOKE=0
QEMU_CMD="${QEMU_SYSTEM_I386:-qemu-system-i386}"
QEMU_CPU_MODEL="${QEMU_CPU_MODEL:-pentium3}"
QEMU_MEMORY_MB="${QEMU_MEMORY_MB:-256}"
QEMU_ACCEL_MODE="${QEMU_ACCEL_MODE:-kvm}"
WINDOWS31_SMOKE_KEEP_ARTIFACTS="${WINDOWS31_SMOKE_KEEP_ARTIFACTS:-0}"
SERIAL_NORMALIZER="$ROOT_DIR/scripts/serial_log_normalize.py"

usage() {
  cat <<'TXT'
Usage: scripts/qemu_test_full_windows31.sh [options]

Verifies the Windows 3.1 payload inside the canonical CiukiOS full image and
launches that same image through the common full-profile QEMU runner. It does
not create or use a Windows-specific disk build.

Options:
  --no-build           Reuse build/full/ciukios-full.img.
  --prepare-only       Build and verify without starting QEMU.
  --headless-smoke     Boot WIN in 386 Enhanced Mode twice, exercise mouse and
                       child-window Alt+F4, then return to the CiukiOS shell.
  --display BACKEND    QEMU display backend (default: auto).
  -h, --help           Show this help.

Environment:
  WINDOWS31_MEDIA_DIR  Source directory containing disk01.img ... disk07.img.
  CIUKIOS_FULL_IMG     Canonical full image path.
  QEMU_DISPLAY         Default QEMU display backend.

The full build archives exact copies of the source images under C:\MEDIA\WIN31,
merges their DOS-accessible contents under C:\WIN31SET, and includes the local
installed tree under C:\WINDOWS when third_party/windows31/installed is ready.
TXT
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build)
      DO_BUILD=0
      shift
      ;;
    --prepare-only)
      PREPARE_ONLY=1
      shift
      ;;
    --headless-smoke)
      HEADLESS_SMOKE=1
      shift
      ;;
    --display)
      [[ $# -ge 2 ]] || { echo "[windows31] ERROR: --display requires a value" >&2; exit 2; }
      DISPLAY_BACKEND="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[windows31] ERROR: unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

for command_name in awk cmp mcopy mdir od stat tr; do
  command -v "$command_name" >/dev/null 2>&1 \
    || { echo "[windows31] ERROR: missing command: $command_name" >&2; exit 1; }
done

if (( DO_BUILD )); then
  echo "[windows31] building the canonical full profile"
  CIUKIOS_WINDOWS31_MODE=require bash scripts/build_full.sh
fi

[[ -f "$IMG" ]] \
  || { echo "[windows31] ERROR: canonical full image not found: $IMG" >&2; exit 1; }

verify_dir="$(mktemp -d /tmp/ciukios-windows31-verify.XXXXXX)"
cleanup_verify_dir() {
  if [[ -n "${verify_dir:-}" && -d "$verify_dir" \
      && "$verify_dir" == /tmp/ciukios-windows31-verify.* ]]; then
    rm -rf -- "$verify_dir"
  fi
}
trap cleanup_verify_dir EXIT

for disk_number in 01 02 03 04 05 06 07; do
  source_disk="$MEDIA_DIR/disk${disk_number}.img"
  embedded_disk="$verify_dir/DISK${disk_number}.IMG"
  [[ -f "$source_disk" && "$(stat -c%s "$source_disk")" -eq 1474560 ]] \
    || { echo "[windows31] ERROR: invalid or missing source media: $source_disk" >&2; exit 1; }
  mcopy -o -i "$IMG" "::MEDIA/WIN31/DISK${disk_number}.IMG" "$embedded_disk" >/dev/null 2>&1 \
    || { echo "[windows31] ERROR: DISK${disk_number}.IMG is missing from the full image" >&2; exit 1; }
  cmp -s "$source_disk" "$embedded_disk" \
    || { echo "[windows31] ERROR: embedded DISK${disk_number}.IMG differs from its source" >&2; exit 1; }
done

mdir -i "$IMG" ::WIN31SET/SETUP.EXE >/dev/null 2>&1 \
  || { echo "[windows31] ERROR: C:\\WIN31SET\\SETUP.EXE is missing" >&2; exit 1; }
mdir -i "$IMG" ::SYSTEM/MOUSE.COM >/dev/null 2>&1 \
  || { echo "[windows31] ERROR: resident INT 33h control utility is missing" >&2; exit 1; }
mdir -i "$IMG" ::SYSTEM/DRIVERS/CTMOUSE.EXE >/dev/null 2>&1 \
  || { echo "[windows31] ERROR: optional GPL CuteMouse driver is missing" >&2; exit 1; }

[[ -s "$RUNTIME_BIN" && -s "$RUNTIME_LST" ]] \
  || { echo "[windows31] ERROR: CIUKIDOS runtime artifacts are missing" >&2; exit 1; }
xms_entry_off_hex="$(awk '/jmp short \.entry_dispatch/ {print $2; exit}' "$RUNTIME_LST")"
[[ "$xms_entry_off_hex" =~ ^[0-9A-Fa-f]+$ ]] \
  || { echo "[windows31] ERROR: XMS patch-window offset not found" >&2; exit 1; }
xms_entry_off=$((16#$xms_entry_off_hex))
xms_patch_window="$(od -An -tx1 -j "$xms_entry_off" -N5 "$RUNTIME_BIN" | tr -d ' \n')"
[[ "$xms_patch_window" == "eb03909090" ]] \
  || { echo "[windows31] ERROR: invalid XMS patch window: $xms_patch_window" >&2; exit 1; }
awk '/dos_file_open_mask dw 0/ {found=1} END {exit !found}' "$RUNTIME_LST" \
  || { echo "[windows31] ERROR: EXEC handle-mask state is missing" >&2; exit 1; }

echo "[windows31] PASS canonical full image contains all seven original IMG files"
echo "[windows31] PASS merged installer is available at C:\\WIN31SET"
echo "[windows31] PASS boot-resident INT 33h plus optional CTMOUSE.EXE are available"
echo "[windows31] PASS XMS patch window and EXEC handle cleanup are present"

awk '/cmp ah, 0x5D/ {dispatch=1} /dos_sda:/ {sda=1} END {exit !(dispatch && sda)}' "$RUNTIME_LST" \
  || { echo "[windows31] ERROR: DOS 4+ swappable-data-area support is missing" >&2; exit 1; }
echo "[windows31] PASS DOSMGR and INT 21h/5D06h expose the DOS swappable data area"

if mdir -i "$IMG" ::WINDOWS/WIN.COM >/dev/null 2>&1 \
    && mdir -i "$IMG" ::WINDOWS/SYSTEM/MOUSE.DRV >/dev/null 2>&1; then
  cat <<'TXT'
[windows31] Installed Windows and its Windows mouse driver are ready for
[windows31] 386 Enhanced Mode. At the CiukiOS prompt run:

  CD ..
  CD WINDOWS
  WIN

[windows31] WIN /3 is the explicit Enhanced Mode equivalent; WIN /S is only
[windows31] a diagnostic fallback for Standard Mode.
TXT
else
  cat <<'TXT'
[windows31] Installation media is ready. At the CiukiOS prompt run:

  CD ..
  CD WIN31SET
  SETUP.EXE

Select VGA and Windows 386 Enhanced mode.
TXT
fi

if (( PREPARE_ONLY )); then
  exit 0
fi

if (( HEADLESS_SMOKE )); then
  cleanup_verify_dir
  trap - EXIT
  for command_name in "$QEMU_CMD" cp python3 socat; do
    command -v "$command_name" >/dev/null 2>&1 \
      || { echo "[windows31] ERROR: missing command for headless smoke: $command_name" >&2; exit 1; }
  done
  mdir -i "$IMG" ::WINDOWS/WIN.COM >/dev/null 2>&1 \
    || { echo "[windows31] ERROR: headless smoke requires C:\\WINDOWS\\WIN.COM" >&2; exit 1; }

  smoke_accel_args=()

  case "$QEMU_ACCEL_MODE" in
    kvm)
      [[ -c /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] \
        && "$QEMU_CMD" -accel help 2>/dev/null | grep -Fxq kvm \
        || { echo "[windows31] ERROR: hardware acceleration is required but KVM is unavailable" >&2; exit 1; }
      smoke_accel_args=(-accel kvm)
      ;;
    auto)
      if [[ -c /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] \
          && "$QEMU_CMD" -accel help 2>/dev/null | grep -Fxq kvm; then
        smoke_accel_args=(-accel kvm)
      else
        smoke_accel_args=(-accel 'tcg,one-insn-per-tb=on')
      fi
      ;;
    tcg) smoke_accel_args=(-accel tcg) ;;
    tcg-safe) smoke_accel_args=(-accel 'tcg,one-insn-per-tb=on') ;;
    *) echo "[windows31] ERROR: invalid QEMU_ACCEL_MODE=$QEMU_ACCEL_MODE" >&2; exit 1 ;;
  esac

  smoke_dir="$(mktemp -d /tmp/ciukios-windows31-smoke.XXXXXX)"
  smoke_img="$smoke_dir/ciukios-full.img"
  smoke_serial="$smoke_dir/serial.log"
  smoke_stderr="$smoke_dir/qemu.stderr.log"
  smoke_monitor="$smoke_dir/monitor.sock"
  smoke_qemu_pid=0

  cleanup_smoke() {
    if (( smoke_qemu_pid != 0 )) && kill -0 "$smoke_qemu_pid" >/dev/null 2>&1; then
      if [[ -S "$smoke_monitor" ]]; then
        printf 'quit\n' | socat - UNIX-CONNECT:"$smoke_monitor" >/dev/null 2>&1 || true
      fi
      kill "$smoke_qemu_pid" >/dev/null 2>&1 || true
      wait "$smoke_qemu_pid" >/dev/null 2>&1 || true
    fi
    if [[ "$WINDOWS31_SMOKE_KEEP_ARTIFACTS" == "1" ]]; then
      echo "[windows31] smoke artifacts retained at $smoke_dir"
    elif [[ -d "$smoke_dir" && "$smoke_dir" == /tmp/ciukios-windows31-smoke.* ]]; then
      rm -rf -- "$smoke_dir"
    fi
  }
  trap cleanup_smoke EXIT

  smoke_fail() {
    echo "[windows31] ERROR: Enhanced Mode smoke failed: $1" >&2
    [[ -f "$smoke_stderr" ]] && tail -n 40 "$smoke_stderr" >&2 || true
    [[ -f "$smoke_serial" ]] \
      && "$SERIAL_NORMALIZER" "$smoke_serial" | tail -n 100 >&2 || true
    exit 1
  }

  smoke_hmp() {
    printf '%s\n' "$1" | socat - UNIX-CONNECT:"$smoke_monitor" >/dev/null
  }

  smoke_send_text() {
    local txt="$1" i ch key
    for ((i=0; i<${#txt}; i++)); do
      ch="${txt:i:1}"
      case "$ch" in
        ' ') key=spc ;;
        '.') key=dot ;;
        '/') key=slash ;;
        '-') key=minus ;;
        [A-Z]) key="shift-$(printf '%s' "$ch" | tr 'A-Z' 'a-z')" ;;
        [a-z0-9]) key="$ch" ;;
        *) continue ;;
      esac
      smoke_hmp "sendkey $key 20" || return 1
      sleep 0.08
    done
    smoke_hmp 'sendkey ret 20'
    sleep 0.30
  }

  smoke_wait_serial() {
    local offset="$1" pattern="$2" timeout_sec="$3" start now
    start="$(date +%s)"
    while true; do
      if [[ -f "$smoke_serial" ]] \
          && "$SERIAL_NORMALIZER" --offset "$offset" "$smoke_serial" \
            | grep -aFq -- "$pattern"; then
        return 0
      fi
      now="$(date +%s)"
      (( now - start < timeout_sec )) || return 1
      sleep 0.10
    done
  }

  smoke_wait_vga() {
    local screenshot="$1" timeout_sec="$2" start now
    start="$(date +%s)"
    while true; do
      smoke_hmp "screendump $screenshot" || return 1
      if [[ "$(sed -n '2p' "$screenshot" 2>/dev/null || true)" == "640 480" ]]; then
        return 0
      fi
      now="$(date +%s)"
      (( now - start < timeout_sec )) || return 1
      sleep 0.25
    done
  }

  smoke_has_shell_title_bar() {
    python3 - "$1" <<'PY'
import sys

with open(sys.argv[1], "rb") as ppm:
    if ppm.readline().strip() != b"P6":
        raise SystemExit(1)
    width, height = map(int, ppm.readline().split())
    if ppm.readline().strip() != b"255":
        raise SystemExit(1)
    pixels = ppm.read()

if (width, height) != (720, 400) or len(pixels) != width * height * 3:
    raise SystemExit(1)

title_pixels = zip(pixels[0:width * 16 * 3:3],
                   pixels[1:width * 16 * 3:3],
                   pixels[2:width * 16 * 3:3])
blue_pixels = sum(1 for red, green, blue in title_pixels
                  if blue > red + 40 and blue > green + 40)
raise SystemExit(0 if blue_pixels >= width * 10 else 1)
PY
  }

  smoke_has_single_pointer_delta() {
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys


def read_ppm(path):
    parts = Path(path).read_bytes().split(maxsplit=4)
    if len(parts) != 5 or parts[0] != b"P6":
        raise SystemExit(1)
    return int(parts[1]), int(parts[2]), parts[4]


width, height, before = read_ppm(sys.argv[1])
after_width, after_height, after = read_ppm(sys.argv[2])
if (width, height) != (after_width, after_height):
    raise SystemExit(1)

changed = {
    (index // 3 % width, index // 3 // width)
    for index, (old, new) in enumerate(zip(before, after))
    if old != new
}
remaining = set(changed)
components = []
while remaining:
    stack = [remaining.pop()]
    size = 1
    while stack:
        x, y = stack.pop()
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                neighbor = (x + dx, y + dy)
                if neighbor in remaining:
                    remaining.remove(neighbor)
                    stack.append(neighbor)
                    size += 1
    components.append(size)

# One client-rendered pointer produces exactly its old and new connected
# shapes. A second CiukiDOS XOR cursor would add another pair.
large = sorted(size for size in components if size >= 20)
raise SystemExit(0 if len(large) == 2 and 60 <= len(changed) <= 400 else 1)
PY
  }

  cp --reflink=auto -- "$IMG" "$smoke_img"
  "$QEMU_CMD" \
    "${smoke_accel_args[@]}" \
    -machine pc,vmport=off,i8042=on \
    -cpu "$QEMU_CPU_MODEL" \
    -m "$QEMU_MEMORY_MB" \
    -drive "file=$smoke_img,format=raw,if=ide" \
    -boot c \
    -display none \
    -chardev "file,id=ser0,path=$smoke_serial" \
    -serial chardev:ser0 \
    -monitor "unix:$smoke_monitor,server,nowait" \
    -no-reboot \
    -no-shutdown \
    >/dev/null 2>"$smoke_stderr" &
  smoke_qemu_pid=$!

  for _ in $(seq 1 200); do
    [[ -S "$smoke_monitor" ]] && break
    sleep 0.05
  done
  [[ -S "$smoke_monitor" ]] || smoke_fail "QEMU monitor did not start"
  smoke_wait_serial 0 'CiukiOS SHELL C:\APPS>' 30 \
    || smoke_fail "initial CiukiOS prompt not reached"

  smoke_hmp 'sendkey ret 20'
  sleep 0.50
  smoke_send_text 'cd..'
  sleep 0.50
  smoke_send_text 'cd windows'
  sleep 0.50
  smoke_send_text 'win'
  smoke_wait_vga "$smoke_dir/first.ppm" 45 \
    || smoke_fail "first WIN did not enter 640x480 Enhanced Mode"
  sleep 15
  smoke_hmp "screendump $smoke_dir/mouse-before.ppm"
  smoke_hmp 'mouse_move -180 70'
  sleep 1
  smoke_hmp "screendump $smoke_dir/mouse-after.ppm"
  cmp -s "$smoke_dir/mouse-before.ppm" "$smoke_dir/mouse-after.ppm" \
    && smoke_fail "Windows mouse pointer did not move"
  smoke_has_single_pointer_delta \
    "$smoke_dir/mouse-before.ppm" "$smoke_dir/mouse-after.ppm" \
    || smoke_fail "Windows movement exposed multiple cursor renderers"
  grep -aFq 'Memoria o spazio di indirizzamento insufficiente' "$smoke_serial" \
    && smoke_fail "Windows reported the Enhanced Mode address-space error"

  child_exit_offset="$(wc -c < "$smoke_serial")"
  smoke_hmp 'sendkey alt-f 30'
  sleep 1
  smoke_hmp 'sendkey e 20'
  sleep 1
  smoke_send_text 'calc'
  sleep 6
  smoke_hmp "screendump $smoke_dir/calc-open.ppm"
  [[ "$(sed -n '2p' "$smoke_dir/calc-open.ppm" 2>/dev/null || true)" == "640 480" ]] \
    || smoke_fail "Calculator did not remain in the Windows VGA session"
  cmp -s "$smoke_dir/mouse-after.ppm" "$smoke_dir/calc-open.ppm" \
    && smoke_fail "Calculator did not open from Program Manager"

  smoke_hmp 'sendkey alt-f4 30'
  sleep 3
  smoke_hmp "screendump $smoke_dir/calc-closed.ppm"
  [[ "$(sed -n '2p' "$smoke_dir/calc-closed.ppm" 2>/dev/null || true)" == "640 480" ]] \
    || smoke_fail "Alt+F4 on Calculator terminated the Windows session"
  cmp -s "$smoke_dir/calc-open.ppm" "$smoke_dir/calc-closed.ppm" \
    && smoke_fail "Alt+F4 did not close Calculator"
  "$SERIAL_NORMALIZER" --offset "$child_exit_offset" "$smoke_serial" \
    | grep -aFq 'CiukiOS SHELL C:\windows>' \
    && smoke_fail "Alt+F4 on Calculator unwound WIN to the CiukiOS shell"

  smoke_hmp 'sendkey alt-f 30'
  sleep 1
  smoke_hmp "screendump $smoke_dir/program-menu.ppm"
  cmp -s "$smoke_dir/calc-closed.ppm" "$smoke_dir/program-menu.ppm" \
    && smoke_fail "Program Manager stopped responding after closing Calculator"
  smoke_hmp 'sendkey esc 20'
  sleep 1

  exit_offset="$(wc -c < "$smoke_serial")"
  smoke_hmp 'sendkey alt-f4 30'
  sleep 2
  smoke_hmp 'sendkey ret 30'
  smoke_wait_serial "$exit_offset" 'CiukiOS SHELL C:\windows>' 30 \
    || smoke_fail "Windows did not return cleanly to the CiukiOS shell"
  sleep 1
  smoke_hmp "screendump $smoke_dir/shell-after-exit.ppm"
  smoke_has_shell_title_bar "$smoke_dir/shell-after-exit.ppm" \
    || smoke_fail "CiukiOS shell title bar was not restored after Windows exit"

  # MOUSE.DRV's BIOS callback points into the Windows process.  A post-exit
  # IRQ12 event must stay in CiukiDOS instead of jumping into freed memory.
  smoke_hmp 'mouse_move 80 30'
  sleep 1
  kill -0 "$smoke_qemu_pid" >/dev/null 2>&1 \
    || smoke_fail "mouse movement after Windows exit crashed QEMU"
  smoke_hmp "screendump $smoke_dir/shell-after-mouse.ppm"
  smoke_has_shell_title_bar "$smoke_dir/shell-after-mouse.ppm" \
    || smoke_fail "post-Windows mouse event corrupted the CiukiOS shell"

  smoke_send_text 'win'
  smoke_wait_vga "$smoke_dir/second.ppm" 45 \
    || smoke_fail "second WIN did not re-enter 640x480 Enhanced Mode"
  sleep 15
  grep -aFq 'Memoria o spazio di indirizzamento insufficiente' "$smoke_serial" \
    && smoke_fail "second Enhanced Mode launch reported an address-space error"

  echo "[windows31] PASS WIN starts in 386 Enhanced Mode at 640x480"
  echo "[windows31] PASS Windows owns one moving mouse pointer"
  echo "[windows31] PASS Alt+F4 closes a child window without terminating Program Manager"
  echo "[windows31] PASS clean exit releases the BIOS callback and a second WIN starts"
  cleanup_smoke
  trap - EXIT
  exit 0
fi

cleanup_verify_dir
trap - EXIT
exec bash scripts/qemu_run_full.sh --no-build --display "$DISPLAY_BACKEND"
