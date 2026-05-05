#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "[qemu-test-full-runtime-probe] running probe-enabled full build and smoke"
CIUKIOS_STAGE1_RUNTIME_PROBE=1 LOG_FILE=build/full/qemu-full-runtime-probe.log STAGE1_MARKER="[RTP] OK" bash scripts/qemu_run_full.sh --test

if ! grep -Fq "[RTP] B" build/full/qemu-full-runtime-probe.log; then
  echo "[qemu-test-full-runtime-probe] FAIL: probe begin marker missing" >&2
  exit 1
fi

if ! grep -Fq "[RTP] T" build/full/qemu-full-runtime-probe.log; then
  echo "[qemu-test-full-runtime-probe] FAIL: probe table marker missing" >&2
  exit 1
fi

if ! grep -Fq "[RTP] C" build/full/qemu-full-runtime-probe.log; then
  echo "[qemu-test-full-runtime-probe] FAIL: probe service-call marker missing" >&2
  exit 1
fi

if ! grep -Fq "[RTP] OK" build/full/qemu-full-runtime-probe.log; then
  echo "[qemu-test-full-runtime-probe] FAIL: probe success marker missing" >&2
  exit 1
fi

runtime_size="$(stat -c%s build/full/obj/runtime.bin)"
if (( runtime_size > 512 )); then
  echo "[qemu-test-full-runtime-probe] FAIL: runtime.bin size=$runtime_size max=512" >&2
  exit 1
fi

GOOD_IMG="build/full/ciukios-full-runtime-probe-good.img"
cp build/full/ciukios-full.img "$GOOD_IMG"
restore_runtime_probe_image() {
  if [[ -f "$GOOD_IMG" ]]; then
    cp "$GOOD_IMG" build/full/ciukios-full.img
  fi
}
trap restore_runtime_probe_image EXIT

echo "BADRT001" > build/full/runtime-corrupt.bin
mcopy -o -i build/full/ciukios-full.img build/full/runtime-corrupt.bin ::SYSTEM/RUNTIME.BIN

echo "[qemu-test-full-runtime-probe] running corrupt-runtime fallback smoke"
LOG_FILE=build/full/qemu-full-runtime-probe-corrupt.log STAGE1_MARKER="[RTP] BAD" bash scripts/qemu_run_full.sh --test --no-build

if ! grep -Fq "[STAGE1-SERIAL] READY" build/full/qemu-full-runtime-probe-corrupt.log; then
  echo "[qemu-test-full-runtime-probe] FAIL: fallback boot marker missing after corrupt runtime" >&2
  exit 1
fi

echo "[qemu-test-full-runtime-probe] PASS runtime probe success and corrupt fallback"
