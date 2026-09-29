#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

usage() {
  cat <<'TXT'
Usage: scripts/qemu_test_all.sh

Runs the full/full-CD, DOS compatibility, bundled-application, external shell,
runtime positive/negative and ownership aggregate. This runner accepts no
options. Set CIUKIOS_TEST_BUNDLED_APPS=0 only for a deliberately shortened
infrastructure lane.
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

# Release aggregate: validate the active full/full-CD profiles, the runtime
# ownership contract, and every bundled interactive application for which the
# full image contains a payload.  This deliberately catches cross-application
# regressions that isolated kernel probes cannot see.
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
  DO_BUILD=0 run_test "generic graphics-child text-mode restoration" bash "scripts/qemu_test_full_video_restore.sh" --no-build || overall_rc=1

  if [[ "${CIUKIOS_TEST_BUNDLED_APPS:-1}" == "1" ]]; then
    if mdir -i build/full/ciukios-full.img ::APPS/COSTA/COSTA.EXE >/dev/null 2>&1; then
      DO_BUILD=0 run_test "Costa desktop/cursor/calculator workflow" bash "scripts/qemu_test_full_costa.sh" || overall_rc=1
    else
      skip_test "Costa desktop/cursor/calculator workflow" "payload absent from full image"
    fi

    if mdir -i build/full/ciukios-full.img ::APPS/DOSNAV/DN.COM >/dev/null 2>&1; then
      echo "[qemu-test-all] DOSNavigator is covered by the same-boot DOS compatibility workflow"
    fi

    if mdir -i build/full/ciukios-full.img ::APPS/WOLF3D/WOLF3D.EXE >/dev/null 2>&1; then
      DO_BUILD=0 run_test "Wolf3D visual gameplay workflow" make qemu-test-full-wolf3d-taxonomy || overall_rc=1
      DO_BUILD=0 run_test "Wolf3D protected AC97 audio workflow" bash "scripts/qemu_test_full_wolf3d_audio.sh" --no-build || overall_rc=1
    else
      skip_test "Wolf3D visual gameplay workflow" "payload absent from full image"
    fi

    if mdir -i build/full/ciukios-full.img ::APPS/DOOM/DOOM.EXE >/dev/null 2>&1; then
      DO_BUILD=0 run_test "Doom visual/audio gameplay workflow" bash "scripts/qemu_test_full_doom_audio.sh" --no-build || overall_rc=1
    else
      skip_test "Doom visual gameplay workflow" "payload absent from full image"
    fi

    if mdir -i build/full/ciukios-full.img ::APPS/DOOMVAN/PCDOOM.EXE >/dev/null 2>&1; then
      DO_BUILD=0 run_test "doom-vanille legacy-VGA performance workflow" bash "scripts/qemu_test_full_doomvan_performance.sh" --no-build || overall_rc=1
      DO_BUILD=0 run_test "doom-vanille protected AC97 audio workflow" bash "scripts/qemu_test_full_doomvan_audio.sh" --no-build || overall_rc=1
      DO_BUILD=0 run_test "doom-vanille automatic memory workflow" make qemu-test-full-doomvan-memory || overall_rc=1
    else
      skip_test "doom-vanille workflows" "payload absent from full image"
    fi
  else
    skip_test "bundled application regression matrix" "CIUKIOS_TEST_BUNDLED_APPS=0"
  fi
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
