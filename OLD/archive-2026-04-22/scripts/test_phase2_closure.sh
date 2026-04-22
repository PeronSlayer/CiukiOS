#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "[phase2] INT21 matrix gate"
bash "$PROJECT_DIR/scripts/check_int21_matrix.sh"

echo "[phase2] deterministic MZ regression"
bash "$PROJECT_DIR/scripts/test_mz_regression.sh"

echo "[phase2] real EXE corpus harness"
bash "$PROJECT_DIR/scripts/test_mz_runtime_corpus.sh"

echo "[PASS] phase2 closure gate"
