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
Usage: scripts/qemu_run_full.sh [--test] [--no-build] [--dry-run] [--display <backend>]

Modes:
  default            Visual run mode (GUI window).
  --test             Smoke-test mode (headless with timeout).

Options:
  --no-build           Skip image build step.
  --dry-run            Print the QEMU command without running it.
  --display <backend>  QEMU display backend in visual mode (default: gtk).

Environment:
  QEMU_BIN         Override QEMU binary.
  QEMU_EXTRA_ARGS  Extra args appended to QEMU command.
  QEMU_TIMEOUT_SEC Timeout in test mode (default: 8).
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
        echo "[qemu-run-full] ERROR: missing value for --display" >&2
        exit 1
      fi
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[qemu-run-full] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if ! QEMU_CMD="$(pick_qemu)"; then
  echo "[qemu-run-full] ERROR: QEMU not found (set QEMU_BIN to override)." >&2
  exit 1
fi

if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "[qemu-run-full] build step"
  bash scripts/build_full.sh
fi

IMG="build/full/ciukios-full.img"
if [[ ! -f "$IMG" ]]; then
  echo "[qemu-run-full] ERROR: image not found: $IMG" >&2
  exit 1
fi

BASE_ARGS=(
  -M pc
  -cpu pentium3
  -m 128
  -drive "file=$IMG,format=raw,if=ide"
  -boot c
)

if [[ "$MODE" == "test" ]]; then
  TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-8}"

  QEMU_ARGS=(
    "${BASE_ARGS[@]}"
    -nographic
    -serial mon:stdio
    -no-reboot
    -no-shutdown
  )

  if [[ -n "${QEMU_EXTRA_ARGS:-}" ]]; then
    # shellcheck disable=SC2206
    EXTRA_ARGS=(${QEMU_EXTRA_ARGS})
    QEMU_ARGS+=("${EXTRA_ARGS[@]}")
  fi

  echo "[qemu-run-full] running smoke test with $QEMU_CMD (timeout=${TIMEOUT_SEC}s)"

  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf '[qemu-run-full] dry-run:'
    printf ' %q' timeout "$TIMEOUT_SEC" "$QEMU_CMD" "${QEMU_ARGS[@]}"
    printf '\n'
    exit 0
  fi

  set +e
  timeout "$TIMEOUT_SEC" "$QEMU_CMD" "${QEMU_ARGS[@]}"
  RC=$?
  set -e

  if [[ $RC -eq 0 || $RC -eq 124 ]]; then
    echo "[qemu-run-full] PASS (smoke execution completed)"
    exit 0
  fi

  echo "[qemu-run-full] FAIL (qemu exit code: $RC)" >&2
  exit "$RC"
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

echo "[qemu-run-full] starting visual QEMU session"
echo "[qemu-run-full] note: full image is still an early scaffold"

if [[ "$DRY_RUN" -eq 1 ]]; then
  printf '[qemu-run-full] dry-run:'
  printf ' %q' "$QEMU_CMD" "${QEMU_ARGS[@]}"
  printf '\n'
  exit 0
fi

exec "$QEMU_CMD" "${QEMU_ARGS[@]}"
