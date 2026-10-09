#!/usr/bin/env bash
# Boot the canonical F0 image interactively (TCG, pentium3, qemu-t23-like
# profile) on a disposable qcow2 overlay. Developer convenience only; the
# evidence path is scripts/test/run.py.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="${CIUKIOS_F0_IMAGE:-$root/build/f0/ciukios.img}"
request="${1:-}"
[[ -f "$image" ]] || { echo "[run-f0] missing $image; run make build-full" >&2; exit 1; }
if pgrep -x qemu-system-i386 >/dev/null; then
    echo "[run-f0] another QEMU is running; one at a time" >&2
    exit 1
fi
run="$root/build/f0/interactive"
rm -rf "$run"
mkdir -p "$run"
qemu-img create -q -f qcow2 -b "$image" -F raw "$run/run.qcow2"
args=(-machine pc-i440fx-9.2,accel=tcg -cpu pentium3 -m "${CIUKIOS_F0_MEM:-512}"
      -drive "file=$run/run.qcow2,format=qcow2,if=ide" -vga std
      -serial "file:$run/serial.log" -no-reboot)
[[ -n "$request" ]] && args+=(-fw_cfg "name=opt/it.alcybercloud.ciukios/test,string=$request")
echo "[run-f0] serial log: $run/serial.log"
exec systemd-run --user --scope -q -p MemoryMax=1500M -p MemorySwapMax=0 -- \
    qemu-system-i386 "${args[@]}"
