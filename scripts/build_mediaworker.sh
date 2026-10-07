#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
DJGPP_ROOT="${CIUKIOS_DJGPP_ROOT:-$ROOT_DIR/build/external/djgpp}"
DJGPP_CC="${DJGPP_CC:-$DJGPP_ROOT/bin/i586-pc-msdosdjgpp-gcc}"
OUT_DIR="${1:-$ROOT_DIR/build/mediaworker}"
[[ -x "$DJGPP_CC" ]] || { echo "[build-mediaworker] ERROR: missing compiler $DJGPP_CC" >&2; exit 1; }
mkdir -p "$OUT_DIR"
flags=(-O2 -std=gnu99 -fno-strict-aliasing -I "$ROOT_DIR/src/media")
echo "[build-mediaworker] CC src/media/mediawork.c"
"$DJGPP_CC" "${flags[@]}" -c src/media/mediawork.c -o "$OUT_DIR/mediawork.o"
echo "[build-mediaworker] CC src/media/decoder.c"
"$DJGPP_CC" "${flags[@]}" -c src/media/decoder.c -o "$OUT_DIR/decoder.o"
echo "[build-mediaworker] LINK $OUT_DIR/MEDIAWORK.EXE"
"$DJGPP_CC" -O2 "$OUT_DIR/mediawork.o" "$OUT_DIR/decoder.o" -lm -o "$OUT_DIR/MEDIAWORK.EXE"
echo "[build-mediaworker] done"
