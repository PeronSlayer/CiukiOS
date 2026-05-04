#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "[build-run-full-cd] build step"
bash scripts/build_full_cd.sh

echo "[build-run-full-cd] run step"
exec bash scripts/qemu_run_full_cd.sh --no-build "$@"
