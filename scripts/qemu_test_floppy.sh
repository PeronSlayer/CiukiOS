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
BOOT_MARKER="${BOOT_MARKER:-[BOOT] CiukiOS pre-Alpha v0.5.0}"
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

if grep -Fq "$BOOT_MARKER" "$LOG_FILE"; then
  echo "[qemu-test-floppy] PASS (boot marker detected)"
  exit 0
fi

echo "[qemu-test-floppy] FAIL (boot marker not detected)" >&2
tail -n 80 "$LOG_FILE" >&2 || true
exit 1
