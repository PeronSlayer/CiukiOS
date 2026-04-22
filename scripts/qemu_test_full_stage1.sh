#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

LOG_FILE="${LOG_FILE:-build/full/qemu-full-stage1.log}"
TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-20}"

echo "[qemu-test-full-stage1] running full profile stage1 selftest regression (FAT16)"
mkdir -p "$(dirname "$LOG_FILE")"
rm -f "$LOG_FILE"

if ! LOG_FILE="$LOG_FILE" QEMU_TIMEOUT_SEC="$TIMEOUT_SEC" bash scripts/qemu_run_full.sh --test; then
  echo "[qemu-test-full-stage1] FAIL (full test harness failed)" >&2
  tail -n 120 "$LOG_FILE" >&2 || true
  exit 1
fi

check_marker() {
  local marker="$1"
  if ! grep -Fq "$marker" "$LOG_FILE"; then
    echo "[qemu-test-full-stage1] FAIL (missing marker: $marker)" >&2
    tail -n 150 "$LOG_FILE" >&2 || true
    exit 1
  fi
}

check_marker "[STAGE1-SERIAL] READY"
check_marker "[STAGE1-SELFTEST] BEGIN"
check_marker "[DOS21-SERIAL] PASS"
check_marker "[COMDEMO-SERIAL] PASS"
check_marker "[MZDEMO-SERIAL] PASS"
check_marker "[FILEIO-SERIAL] PASS"
check_marker "[FIND-SERIAL] PASS"
check_marker "[GFX-SERIAL] PASS"
check_marker "[STAGE1-SELFTEST] DONE"

echo "[qemu-test-full-stage1] PASS (FAT16 stage1 selftest + INT21h + COM/MZ + file I/O + findfirst + VGA mode13h)"
