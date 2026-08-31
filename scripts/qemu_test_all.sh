#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

usage() {
  cat <<'TXT'
Usage: scripts/qemu_test_all.sh

Runs the focused full/full-CD, DOS compatibility, external shell, runtime
positive/negative and ownership aggregate. This runner accepts no options.
TXT
}

if [[ $# -gt 0 ]]; then
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[qemu-test-all] ERROR: unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
fi

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

  if [[ "${cmd[0]}" == */* ]]; then
    if [[ ! -x "${cmd[0]}" ]]; then
      echo "[qemu-test-all] ERROR: missing executable: ${cmd[0]}" >&2
      return 2
    fi
  elif ! command -v "${cmd[0]}" >/dev/null 2>&1; then
    echo "[qemu-test-all] ERROR: missing command: ${cmd[0]}" >&2
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

skip_test() {
  local label="$1"
  local reason="$2"
  echo "[qemu-test-all] $label SKIP ($reason)"
}

overall_rc=0

# Focused aggregate: validate the active full/full-CD profiles and the runtime
# ownership contract without pulling long-running game/audio taxonomy lanes.
if ! run_test "serial normalization self-test" "scripts/serial_log_normalize.py" --self-test; then
  echo "[qemu-test-all] FAIL (serial normalization is a prerequisite for every marker gate)" >&2
  exit 1
fi

full_image_ready=0
if run_test "full image smoke test" "scripts/qemu_test_full.sh"; then
  full_image_ready=1
else
  overall_rc=1
  skip_test "full CuteMouse external workflow" "full image producer failed"
  skip_test "full DOS compatibility smoke test" "full image producer failed"
  skip_test "full external shell/runtime ownership smoke test" "full image producer failed"
fi

if (( full_image_ready )); then
  run_test "Phase 5 runtime ownership boundary" "scripts/verify_phase5_runtime_ownership.sh" --no-build || overall_rc=1
  DO_BUILD=0 run_test "full CuteMouse external workflow" bash "scripts/qemu_test_full_cutemouse.sh" --no-build || overall_rc=1
  run_test "full DOS compatibility smoke test" "scripts/qemu_test_full_dos_compat_smoke.sh" --no-build || overall_rc=1
  SHELL_COM_BOOT_AUTORUN=1 run_test "full external shell/runtime ownership smoke test" bash "scripts/qemu_test_full_shell_com.sh" --no-build || overall_rc=1
fi

run_test "full runtime positive/negative probe" "scripts/qemu_test_full_runtime_probe.sh" || overall_rc=1

full_cd_ready=0
if run_test "full-cd image smoke test" "scripts/qemu_run_full_cd.sh" --test; then
  full_cd_ready=1
else
  overall_rc=1
  skip_test "full-cd COM/MZ/runtime ownership smoke test" "full-CD image producer failed"
fi
if (( full_cd_ready )); then
  run_test "full-cd COM/MZ/runtime ownership smoke test" bash "scripts/qemu_test_full_cd_shell_com_boot.sh" --no-build || overall_rc=1
fi

if [[ $overall_rc -eq 0 ]]; then
  echo "[qemu-test-all] PASS (all configured tests passed)"
  exit 0
fi

echo "[qemu-test-all] FAIL (one or more configured tests failed)" >&2
exit 1
