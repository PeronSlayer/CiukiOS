#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

SRC="src/com/shell.asm"
OUT="build/full/obj/shell.com"

if [[ ! -f "$SRC" ]]; then
  echo "[build-shell-com] ERROR: source not found: $SRC" >&2
  exit 1
fi

mkdir -p "$(dirname "$OUT")"

echo "[build-shell-com] assembling $SRC"
nasm -f bin "$SRC" -o "$OUT"
echo "[build-shell-com] done: $OUT"
