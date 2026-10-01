#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

WINDOW_SIZE="${CIUKIOS_RECORD_WINDOW_SIZE:-1280x800}"
WINDOW_POS="${CIUKIOS_RECORD_WINDOW_POS:-80,80}"
GUEST_SIZE="${CIUKIOS_RECORD_GUEST_SIZE:-1024x768}"
DO_BUILD=0
HOST_GL=on
ACCEL_MODE=kvm

usage() {
  cat <<'TXT'
Usage: scripts/qemu_record_full.sh [--build] [--size WIDTHxHEIGHT]
       [--position X,Y] [--guest-size WIDTHxHEIGHT] [--software-display]
       [--vga-fast]

Opens the full CiukiOS image for video recording. GTK scales the guest into a
fixed X11/XWayland window, including when a DOS game changes VGA mode. Uses
KVM, the existing NE2000 Internet NAT, sound and host OpenGL presentation.
The guest VGA is emulated; host OpenGL does not provide DOS 3D acceleration.
--vga-fast selects the measured TCG planar-VGA path for Doom-vanille only;
QEMU 11.1 is not stable with the local original Doom binary in this mode.

Default window: 1280x800 at 80,80; preferred guest EDID: 1024x768.
Press Ctrl+Alt+G to release the guest mouse. Use --build to refresh the image.
TXT
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) DO_BUILD=1; shift ;;
    --size) WINDOW_SIZE="${2:-}"; shift 2 ;;
    --position) WINDOW_POS="${2:-}"; shift 2 ;;
    --guest-size) GUEST_SIZE="${2:-}"; shift 2 ;;
    --software-display) HOST_GL=off; shift ;;
    --vga-fast) ACCEL_MODE=vga-fast; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "[qemu-record] ERROR: unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

[[ "$WINDOW_SIZE" =~ ^([0-9]{3,4})x([0-9]{3,4})$ ]] || {
  echo "[qemu-record] ERROR: --size must be WIDTHxHEIGHT" >&2; exit 1;
}
WINDOW_W="${BASH_REMATCH[1]}"
WINDOW_H="${BASH_REMATCH[2]}"
[[ "$GUEST_SIZE" =~ ^[0-9]{3,4}x[0-9]{3,4}$ ]] || {
  echo "[qemu-record] ERROR: --guest-size must be WIDTHxHEIGHT" >&2; exit 1;
}
[[ "$WINDOW_POS" =~ ^([0-9]{1,4}),([0-9]{1,4})$ ]] || {
  echo "[qemu-record] ERROR: --position must be X,Y" >&2; exit 1;
}
WINDOW_X="${BASH_REMATCH[1]}"
WINDOW_Y="${BASH_REMATCH[2]}"

command -v xdotool >/dev/null || {
  echo "[qemu-record] ERROR: xdotool is required to hold window geometry" >&2; exit 1;
}
command -v python3 >/dev/null || {
  echo "[qemu-record] ERROR: python3 is required for the QEMU monitor" >&2; exit 1;
}
[[ -n "${DISPLAY:-}" ]] || {
  echo "[qemu-record] ERROR: X11/XWayland DISPLAY is required" >&2; exit 1;
}

if (( DO_BUILD )); then
  if [[ "${CIUKIOS_FETCH_COSTA:-1}" == "1" ]]; then
    bash scripts/fetch_costa.sh
  fi
  bash scripts/fetch_network_stack.sh
  bash scripts/fetch_microweb.sh
  bash scripts/build_full.sh
fi

echo "[qemu-record] guest EDID $GUEST_SIZE; window ${WINDOW_SIZE}+${WINDOW_X}+${WINDOW_Y}"
echo "[qemu-record] accelerator=$ACCEL_MODE; GTK OpenGL presentation=$HOST_GL; NE2000 user NAT"

MONITOR_SOCK="${QEMU_RECORD_MONITOR_SOCKET:-$ROOT_DIR/build/full/qemu-record-$$.sock}"
[[ "$MONITOR_SOCK" != *[[:space:]]* ]] || {
  echo "[qemu-record] ERROR: monitor socket path cannot contain whitespace" >&2; exit 1;
}
[[ ! -e "$MONITOR_SOCK" && ! -S "$MONITOR_SOCK" ]] || {
  echo "[qemu-record] ERROR: monitor socket already exists: $MONITOR_SOCK" >&2; exit 1;
}
mkdir -p "$(dirname "$MONITOR_SOCK")"
if [[ " ${QEMU_EXTRA_ARGS:-} " == *" -monitor "* ]]; then
  echo "[qemu-record] ERROR: QEMU_EXTRA_ARGS cannot set a second monitor" >&2
  exit 1
fi
export QEMU_EXTRA_ARGS="${QEMU_EXTRA_ARGS:-} -S -monitor unix:${MONITOR_SOCK},server,nowait"

QEMU_VIDEO_SIZE="$GUEST_SIZE" \
QEMU_NETWORK_MODE=user \
QEMU_ACCEL_MODE="$ACCEL_MODE" \
bash scripts/qemu_run_full.sh --no-build \
  --display "gtk,gl=$HOST_GL,zoom-to-fit=on,show-menubar=off,window-close=off" &
VM_PID=$!

cleanup() {
  if kill -0 "$VM_PID" 2>/dev/null; then
    kill "$VM_PID" 2>/dev/null || true
  fi
  [[ ! -S "$MONITOR_SOCK" ]] || unlink "$MONITOR_SOCK"
}
trap cleanup EXIT INT TERM

WINDOW_ID=""
for (( attempt=0; attempt<100; attempt++ )); do
  if ! kill -0 "$VM_PID" 2>/dev/null; then
    wait "$VM_PID"
    exit $?
  fi
  WINDOW_ID="$(xdotool search --pid "$VM_PID" --name '^CiukiOS$' 2>/dev/null | head -n 1 || true)"
  [[ -n "$WINDOW_ID" ]] && break
  sleep 0.2
done
[[ -n "$WINDOW_ID" ]] || {
  echo "[qemu-record] ERROR: QEMU window did not appear" >&2
  cleanup
  exit 1
}

echo "[qemu-record] window id $WINDOW_ID; holding client area at $WINDOW_SIZE"
xdotool windowmove "$WINDOW_ID" "$WINDOW_X" "$WINDOW_Y" >/dev/null 2>&1 || true
xdotool windowsize "$WINDOW_ID" "$WINDOW_W" "$WINDOW_H" >/dev/null 2>&1 || true
for (( attempt=0; attempt<50; attempt++ )); do
  geometry="$(xdotool getwindowgeometry --shell "$WINDOW_ID" 2>/dev/null || true)"
  actual_w="$(sed -n 's/^WIDTH=//p' <<< "$geometry")"
  actual_h="$(sed -n 's/^HEIGHT=//p' <<< "$geometry")"
  [[ "$actual_w" == "$WINDOW_W" && "$actual_h" == "$WINDOW_H" ]] && break
  sleep 0.1
done
[[ "$actual_w" == "$WINDOW_W" && "$actual_h" == "$WINDOW_H" ]] || {
  echo "[qemu-record] ERROR: window manager did not set $WINDOW_SIZE" >&2
  exit 1
}
for (( attempt=0; attempt<50; attempt++ )); do
  [[ -S "$MONITOR_SOCK" ]] && break
  sleep 0.1
done
[[ -S "$MONITOR_SOCK" ]] || {
  echo "[qemu-record] ERROR: QEMU monitor did not appear" >&2
  exit 1
}
python3 - "$MONITOR_SOCK" <<'PY'
import socket
import sys

with socket.socket(socket.AF_UNIX) as connection:
    connection.settimeout(5)
    connection.connect(sys.argv[1])
    greeting = b''
    while b'(qemu)' not in greeting:
        chunk = connection.recv(4096)
        if not chunk:
            raise SystemExit('QEMU monitor closed before boot')
        greeting += chunk
    connection.sendall(b'cont\n')
PY
echo "[qemu-record] guest boot starts with the window already at $WINDOW_SIZE"
while kill -0 "$VM_PID" 2>/dev/null; do
  geometry="$(xdotool getwindowgeometry --shell "$WINDOW_ID" 2>/dev/null || true)"
  actual_w="$(sed -n 's/^WIDTH=//p' <<< "$geometry")"
  actual_h="$(sed -n 's/^HEIGHT=//p' <<< "$geometry")"
  if [[ "$actual_w" != "$WINDOW_W" || "$actual_h" != "$WINDOW_H" ]]; then
    xdotool windowsize "$WINDOW_ID" "$WINDOW_W" "$WINDOW_H" >/dev/null 2>&1 || true
  fi
  sleep 0.5
done
wait "$VM_PID"
