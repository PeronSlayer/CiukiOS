#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "[qemu-test-full-stage1] legacy target: running the active external-shell/runtime regression"
SHELL_COM_BOOT_AUTORUN=1 exec bash scripts/qemu_test_full_shell_com.sh "$@"
