#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${CC:-gcc}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# The production C source targets 32-bit DOS addresses. Suppress the expected
# pointer-width warning when this host-only fixture is compiled by 64-bit GCC.
"$CC" -std=c99 -Wall -Wextra -Werror -Wno-int-to-pointer-cast \
    -I"$ROOT/src/vm" "$ROOT/src/vm/test_session_gpu_legacy.c" \
    -o "$TMP/test_session_gpu_legacy"
"$TMP/test_session_gpu_legacy"
