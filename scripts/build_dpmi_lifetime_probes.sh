#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
OUT="${1:-build/tests/dpmi-lifetime}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WATCOM_ROOT="${WATCOM:-/opt/watcom}"
export WATCOM="$WATCOM_ROOT"
export INCLUDE="$WATCOM_ROOT/h"
export PATH="$WATCOM_ROOT/binl64:$WATCOM_ROOT/binl:$PATH"
for item in dpmi_lifetime:DPMILIF dpmi_fault:DPMIFLT dpmi_ports:DPMIPORT; do
    source="${item%%:*}"
    binary="${item##*:}"
    wcc386 -zq -bt=dos -mf -3r -ecc -s -w4 -we \
        -fo="$OUT/$source.obj" "src/probes/vm/$source.c"
    wlink option quiet system dos4g name "$OUT/$binary.EXE" \
        option stack=32768 file "$OUT/$source.obj"
done
printf 'Built HDPMI lifetime probes in %s\n' "$OUT"
