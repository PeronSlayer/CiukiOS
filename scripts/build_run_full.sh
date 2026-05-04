#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "[build-run-full] build step"
bash scripts/build_full.sh

echo "[build-run-full] run step"
exec bash scripts/qemu_run_full.sh --no-build "$@"
