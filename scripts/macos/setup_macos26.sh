#!/usr/bin/env bash
set -euo pipefail

INSTALL_MODE=0
if [[ "${1:-}" == "--install" ]]; then
  INSTALL_MODE=1
fi

if ! command -v brew >/dev/null 2>&1; then
  echo "[setup-macos26] Homebrew not found. Install from https://brew.sh first." >&2
  exit 1
fi

PACKAGES=(
  nasm
  qemu
  mtools
  coreutils
  gnu-sed
  findutils
  gawk
)

if [[ "$INSTALL_MODE" -eq 1 ]]; then
  echo "[setup-macos26] Installing required packages via Homebrew..."
  brew install "${PACKAGES[@]}"
fi

echo "[setup-macos26] Toolchain check:"
for p in "${PACKAGES[@]}"; do
  if brew list --versions "$p" >/dev/null 2>&1; then
    echo "  OK  $p"
  else
    echo "  MISSING  $p"
  fi
done

echo "[setup-macos26] Recommended run commands:"
echo "  bash scripts/macos/build_full_macos.sh"
echo "  bash scripts/macos/build_floppy_macos.sh"
echo "  bash scripts/macos/qemu_run_full_macos.sh --test"
echo "  bash scripts/macos/qemu_run_floppy_macos.sh --test"
