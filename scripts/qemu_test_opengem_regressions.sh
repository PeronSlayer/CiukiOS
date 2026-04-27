#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "[qemu-test-opengem-regressions] running OG-P2-01 regression lock"
CIUKIOS_STAGE2_AUTORUN=1 bash scripts/opengem_regression_lock.sh --no-build
