#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

run_test() {
  local label="$1"
  shift
  local cmd=("$@")
  local start_ts
  local end_ts
  local elapsed
  local rc

  if [[ ${#cmd[@]} -eq 0 ]]; then
    echo "[qemu-test-all] ERROR: no command provided for test: $label" >&2
    return 2
  fi

  if [[ ! -x "${cmd[0]}" ]]; then
    echo "[qemu-test-all] ERROR: missing executable script: ${cmd[0]}" >&2
    return 2
  fi

  echo "[qemu-test-all] running $label"
  start_ts="$(date +%s)"
  set +e
  "${cmd[@]}"
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

# Focused aggregate: validate active "full" and "full-cd" profiles only
run_test "full image smoke test" "scripts/qemu_test_full.sh" || overall_rc=1
run_test "full DOS compatibility smoke test" "scripts/qemu_test_full_dos_compat_smoke.sh" || overall_rc=1
run_test "full-cd image smoke test" "scripts/qemu_run_full_cd.sh" --test || overall_rc=1

if [[ $overall_rc -eq 0 ]]; then
  echo "[qemu-test-all] PASS (all configured tests passed)"
  exit 0
fi

echo "[qemu-test-all] FAIL (one or more configured tests failed)" >&2
exit 1
