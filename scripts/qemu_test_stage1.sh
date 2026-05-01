#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

LOG_FILE="${LOG_FILE:-build/floppy/qemu-stage1.log}"
TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-12}"

echo "[qemu-test-stage1] running stage1 boot selftest + deterministic mv/rename marker"
mkdir -p "$(dirname "$LOG_FILE")"
rm -f "$LOG_FILE"

export CIUKIOS_STAGE1_SELFTEST_AUTORUN=1

if ! LOG_FILE="$LOG_FILE" QEMU_TIMEOUT_SEC="$TIMEOUT_SEC" bash scripts/qemu_run_floppy.sh --test; then
  echo "[qemu-test-stage1] FAIL (floppy test harness failed)" >&2
  tail -n 120 "$LOG_FILE" >&2 || true
  exit 1
fi

check_marker() {
  local marker="$1"
  if ! grep -Fq "$marker" "$LOG_FILE"; then
    echo "[qemu-test-stage1] FAIL (missing marker: $marker)" >&2
    tail -n 150 "$LOG_FILE" >&2 || true
    exit 1
  fi
}

check_any_marker() {
  local marker_a="$1"
  local marker_b="$2"
  if grep -Fq "$marker_a" "$LOG_FILE" || grep -Fq "$marker_b" "$LOG_FILE"; then
    return 0
  fi
  echo "[qemu-test-stage1] FAIL (missing markers: $marker_a | $marker_b)" >&2
  tail -n 150 "$LOG_FILE" >&2 || true
  exit 1
}

check_marker_absent() {
  local marker="$1"
  if grep -Fq "$marker" "$LOG_FILE"; then
    echo "[qemu-test-stage1] FAIL (unexpected marker: $marker)" >&2
    tail -n 150 "$LOG_FILE" >&2 || true
    exit 1
  fi
}

check_marker "[STAGE1-SERIAL] READY"
check_any_marker "[STAGE1-SELFTEST] BEGIN" "[S1T] B"
check_marker "[DOS21-SERIAL] PASS"
check_marker "[COMDEMO-SERIAL] PASS"
check_marker "[MZDEMO-SERIAL] PASS"
check_marker "[GFXRECT-SERIAL] PASS"
check_marker "[GFXSTAR-SERIAL] PASS"
check_marker_absent "[GFXRECT-SERIAL] FAIL"
check_marker_absent "[GFXSTAR-SERIAL] FAIL"
check_marker "[GFX-SERIAL] PASS"
check_marker "[MVR] PASS"
check_marker_absent "[MVR] FAIL"
check_any_marker "[STAGE1-SELFTEST] DONE" "[S1T] D"

echo "[qemu-test-stage1] PASS (stage1 selftest + INT21h + COM/MZ via AH=4Bh + file I/O + findfirst/findnext + VGA mode13h)"
