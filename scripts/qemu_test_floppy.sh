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

if ! QEMU_CMD="$(pick_qemu)"; then
  echo "[qemu-test-floppy] ERROR: QEMU not found (set QEMU_BIN to override)." >&2
  exit 1
fi

echo "[qemu-test-floppy] build step"
make build-floppy

IMG="build/floppy/ciukios-floppy.img"
if [[ ! -f "$IMG" ]]; then
  echo "[qemu-test-floppy] ERROR: image not found: $IMG" >&2
  exit 1
fi

TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-8}"
STAGE0_MARKER="${STAGE0_MARKER:-[BOOT0] CiukiOS stage0 ready}"
STAGE1_MARKER="${STAGE1_MARKER:-[STAGE1] CiukiOS stage1 running}"
LOG_FILE="${LOG_FILE:-build/floppy/qemu-floppy.log}"
echo "[qemu-test-floppy] running smoke test with $QEMU_CMD (timeout=${TIMEOUT_SEC}s)"

mkdir -p "$(dirname "$LOG_FILE")"
rm -f "$LOG_FILE"

set +e
timeout "$TIMEOUT_SEC" "$QEMU_CMD" \
  -M pc \
  -cpu pentium3 \
  -m 64 \
  -drive file="$IMG",format=raw,if=floppy \
  -boot a \
  -nographic \
  -serial mon:stdio \
  -no-reboot \
  -no-shutdown \
  >"$LOG_FILE" 2>&1
RC=$?
set -e

if [[ $RC -ne 0 && $RC -ne 124 ]]; then
  echo "[qemu-test-floppy] FAIL (qemu exit code: $RC)" >&2
  tail -n 60 "$LOG_FILE" >&2 || true
  exit "$RC"
fi

if grep -Fq "$STAGE0_MARKER" "$LOG_FILE" && grep -Fq "$STAGE1_MARKER" "$LOG_FILE"; then
  echo "[qemu-test-floppy] PASS (stage0 and stage1 markers detected)"
  exit 0
fi

echo "[qemu-test-floppy] FAIL (stage0/stage1 marker not detected)" >&2
tail -n 80 "$LOG_FILE" >&2 || true
exit 1
