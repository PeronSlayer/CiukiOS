#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

run_test() {
  local label="$1"
  local script_path="$2"
  local start_ts
  local end_ts
  local elapsed
  local rc

  if [[ ! -x "$script_path" ]]; then
    echo "[qemu-test-all] ERROR: missing executable script: $script_path" >&2
    return 2
  fi

  echo "[qemu-test-all] running $label"
  start_ts="$(date +%s)"
  set +e
  bash "$script_path"
  rc=$?
  set -e
  end_ts="$(date +%s)"
  elapsed=$((end_ts - start_ts))

  if [[ $rc -eq 0 ]]; then
    echo "[qemu-test-all] $label PASS (${elapsed}s)"
    return 0
  fi

  echo "[qemu-test-all] $label FAIL (${elapsed}s, rc=$rc)" >&2
  return "$rc"
}

overall_rc=0

run_test "floppy image smoke test" "scripts/qemu_test_floppy.sh" || overall_rc=1
run_test "stage1 boot selftest regression" "scripts/qemu_test_stage1.sh" || overall_rc=1
run_test "full image smoke test" "scripts/qemu_test_full.sh" || overall_rc=1
run_test "full stage1 selftest regression" "scripts/qemu_test_full_stage1.sh" || overall_rc=1
run_test "opengem historical regression lock" "scripts/qemu_test_opengem_regressions.sh" || overall_rc=1

if [[ $overall_rc -eq 0 ]]; then
  echo "[qemu-test-all] PASS (all image tests passed)"
  exit 0
fi

echo "[qemu-test-all] FAIL (one or more image tests failed)" >&2
exit 1
