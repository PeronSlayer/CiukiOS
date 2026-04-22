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
  echo "[qemu-test-full] ERROR: QEMU not found (set QEMU_BIN to override)." >&2
  exit 1
fi

echo "[qemu-test-full] build step"
make build-full

IMG="build/full/ciukios-full.img"
if [[ ! -f "$IMG" ]]; then
  echo "[qemu-test-full] ERROR: image not found: $IMG" >&2
  exit 1
fi

TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-8}"
echo "[qemu-test-full] running smoke test with $QEMU_CMD (timeout=${TIMEOUT_SEC}s)"
set +e
timeout "$TIMEOUT_SEC" "$QEMU_CMD" \
  -M pc \
  -cpu pentium3 \
  -m 128 \
  -drive file="$IMG",format=raw,if=ide \
  -boot c \
  -nographic \
  -serial mon:stdio \
  -no-reboot \
  -no-shutdown
RC=$?
set -e

if [[ $RC -eq 0 || $RC -eq 124 ]]; then
  echo "[qemu-test-full] PASS (smoke execution completed)"
  exit 0
fi

echo "[qemu-test-full] FAIL (qemu exit code: $RC)" >&2
exit "$RC"
