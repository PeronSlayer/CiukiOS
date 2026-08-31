#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

LOG_FILE="${LOG_FILE:-build/floppy/qemu-stage1.log}"
TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-12}"

echo "[qemu-test-stage1] running FAT12 Stage1 loader-scaffold regression"
mkdir -p "$(dirname "$LOG_FILE")"
rm -f "$LOG_FILE"

if ! LOG_FILE="$LOG_FILE" QEMU_TIMEOUT_SEC="$TIMEOUT_SEC" bash scripts/qemu_run_floppy.sh --test; then
  echo "[qemu-test-stage1] FAIL (floppy test harness failed)" >&2
  tail -n 120 "$LOG_FILE" >&2 || true
  exit 1
fi

echo "[qemu-test-stage1] PASS (FAT12 Stage1 loads and reaches the documented loader-only fatal path)"
