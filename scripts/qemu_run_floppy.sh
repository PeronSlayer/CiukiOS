#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

pick_qemu() {
  if [[ -n "${QEMU_BIN:-}" ]]; then
    echo "$QEMU_BIN"
    return
  fi
  if command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "qemu-system-i386"
    return
  fi
  if command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "qemu-system-x86_64"
    return
  fi
  return 1
}

usage() {
  cat << 'TXT'
Usage: scripts/qemu_run_floppy.sh [--test] [--no-build] [--dry-run] [--display <backend>]

Modes:
  default            Visual run mode (GUI window).
  --test             Smoke-test mode (headless with timeout and marker checks).

Options:
  --no-build           Skip image build step.
  --dry-run            Print the QEMU command without running it.
  --display <backend>  QEMU display backend in visual mode (default: gtk).

Environment:
  QEMU_BIN         Override QEMU binary.
  QEMU_EXTRA_ARGS  Extra args appended to QEMU command.
  QEMU_TIMEOUT_SEC Timeout in test mode (default: 8).
  LOG_FILE         Test log path (default: build/floppy/qemu-floppy.log).
  STAGE0_MARKER    Marker 1 for test validation.
  STAGE1_MARKER    Marker 2 for test validation.
TXT
}

MODE="visual"
DO_BUILD=1
DRY_RUN=0
DISPLAY_BACKEND="${QEMU_DISPLAY:-gtk}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --test)
      MODE="test"
      shift
      ;;
    --no-build)
      DO_BUILD=0
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --display)
      DISPLAY_BACKEND="${2:-}"
      if [[ -z "$DISPLAY_BACKEND" ]]; then
        echo "[qemu-run-floppy] ERROR: missing value for --display" >&2
        exit 1
      fi
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[qemu-run-floppy] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if ! QEMU_CMD="$(pick_qemu)"; then
  echo "[qemu-run-floppy] ERROR: QEMU not found (set QEMU_BIN to override)." >&2
  exit 1
fi

if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "[qemu-run-floppy] build step"
  bash scripts/build_floppy.sh
fi

IMG="build/floppy/ciukios-floppy.img"
if [[ ! -f "$IMG" ]]; then
  echo "[qemu-run-floppy] ERROR: image not found: $IMG" >&2
  exit 1
fi

BASE_ARGS=(
  -M pc
  -cpu pentium3
  -m 64
  -drive "file=$IMG,format=raw,if=floppy"
  -boot a
)

if [[ "$MODE" == "test" ]]; then
  TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-8}"
  STAGE0_MARKER="${STAGE0_MARKER:-[BOOT0] CiukiOS stage0 ready}"
  STAGE1_MARKER="${STAGE1_MARKER:-[STAGE1-SERIAL] READY}"
  LOG_FILE="${LOG_FILE:-build/floppy/qemu-floppy.log}"

  QEMU_ARGS=(
    "${BASE_ARGS[@]}"
    -nographic
    -chardev "file,id=ser0,path=$LOG_FILE"
    -serial chardev:ser0
    -monitor none
    -no-reboot
    -no-shutdown
  )

  if [[ -n "${QEMU_EXTRA_ARGS:-}" ]]; then
    # shellcheck disable=SC2206
    EXTRA_ARGS=(${QEMU_EXTRA_ARGS})
    QEMU_ARGS+=("${EXTRA_ARGS[@]}")
  fi

  echo "[qemu-run-floppy] running smoke test with $QEMU_CMD (timeout=${TIMEOUT_SEC}s)"

  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf '[qemu-run-floppy] dry-run:'
    printf ' %q' timeout "$TIMEOUT_SEC" "$QEMU_CMD" "${QEMU_ARGS[@]}"
    printf ' >/dev/null 2>&1 (serial -> %q)\n' "$LOG_FILE"
    exit 0
  fi

  mkdir -p "$(dirname "$LOG_FILE")"
  rm -f "$LOG_FILE"

  set +e
  timeout "$TIMEOUT_SEC" "$QEMU_CMD" "${QEMU_ARGS[@]}" >/dev/null 2>&1
  RC=$?
  set -e

  if [[ $RC -ne 0 && $RC -ne 124 ]]; then
    echo "[qemu-run-floppy] FAIL (qemu exit code: $RC)" >&2
    tail -n 60 "$LOG_FILE" >&2 || true
    exit "$RC"
  fi

  if grep -Fq "$STAGE0_MARKER" "$LOG_FILE" && grep -Fq "$STAGE1_MARKER" "$LOG_FILE"; then
    echo "[qemu-run-floppy] PASS (stage0 and stage1 markers detected)"
    exit 0
  fi

  echo "[qemu-run-floppy] FAIL (stage0/stage1 marker not detected)" >&2
  tail -n 80 "$LOG_FILE" >&2 || true
  exit 1
fi

QEMU_ARGS=(
  "${BASE_ARGS[@]}"
  -display "$DISPLAY_BACKEND"
  -serial stdio
)

if [[ -n "${QEMU_EXTRA_ARGS:-}" ]]; then
  # shellcheck disable=SC2206
  EXTRA_ARGS=(${QEMU_EXTRA_ARGS})
  QEMU_ARGS+=("${EXTRA_ARGS[@]}")
fi

echo "[qemu-run-floppy] starting visual QEMU session"
echo "[qemu-run-floppy] tip: Ctrl+Alt+G releases mouse capture"

if [[ "$DRY_RUN" -eq 1 ]]; then
  printf '[qemu-run-floppy] dry-run:'
  printf ' %q' "$QEMU_CMD" "${QEMU_ARGS[@]}"
  printf '\n'
  exit 0
fi

exec "$QEMU_CMD" "${QEMU_ARGS[@]}"
