#!/usr/bin/env bash
# Capture the COM1 output of a physical CiukiOS machine through an RS-232
# to USB adapter (38400 8N1, no flow control: docs/design/f0-acceptance.md).
# Usage: scripts/test/serial_capture.sh <run-id> [/dev/ttyUSB0]
# Start it BEFORE powering the laptop on; stop with Ctrl-C. The raw log
# goes to legacy/local/physical/<run-id>/serial.log (never published).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
run="${1:-}"
dev="${2:-/dev/ttyUSB0}"
[[ "$run" =~ ^[0-9a-f]{8}$ ]] || { echo "usage: $0 <8-hex-run-id> [device]" >&2; exit 2; }
[[ -c "$dev" ]] || { echo "[serial] $dev is not a character device (adapter not plugged in?)" >&2; exit 1; }
out="$root/legacy/local/physical/$run"
mkdir -p "$out"
if [[ -e "$out/serial.log" ]]; then
    echo "[serial] $out/serial.log exists; choose a new run id" >&2
    exit 1
fi
# 38400 baud, 8 data bits, 1 stop bit, no parity, no RTS/CTS or XON/XOFF, raw.
stty -F "$dev" 38400 cs8 -cstopb -parenb -crtscts -ixon -ixoff raw -echo
printf '[serial] capturing %s at 38400 8N1 into %s (Ctrl-C to stop)\n' "$dev" "$out/serial.log"
date -u +'%Y-%m-%dT%H:%M:%SZ' > "$out/capture-start.txt"
# tee keeps the raw bytes on disk while showing them; records are ASCII lines.
exec cat "$dev" | tee "$out/serial.log"
