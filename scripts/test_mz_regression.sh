#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build/tests"
OUT_BIN="$BUILD_DIR/mz_regression"

mkdir -p "$BUILD_DIR"

clang -std=c11 -Wall -Wextra -I"$PROJECT_DIR/stage2/include" \
    "$PROJECT_DIR/stage2/tests/mz_regression.c" \
    "$PROJECT_DIR/stage2/src/dos_mz.c" \
    -o "$OUT_BIN"

"$OUT_BIN"
