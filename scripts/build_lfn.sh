#!/usr/bin/env bash
# Build LFN.COM, the long file name extension of the CiukiDOS kernel
# (src/lfn, docs/long-file-names-2026-09-30.md).
#   scripts/build_lfn.sh [OUTPUT_DIR]
# Public RTSV/CLFN imports are validated at installation. This module builds
# independently of the kernel binary, listing and private memory locations.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
OUT="${1:-build/full/obj/lfn}"
export WATCOM="${WATCOM:-/opt/watcom}"
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h"
mkdir -p "$OUT"
CFLAGS=(-q -2 -ms -s -zl -os -d0 -wx -we -zq -i=src/lfn)
nasm -f obj src/lfn/lfn_start.asm -o "$OUT/lfn_start.obj"
wcc "${CFLAGS[@]}" -fo="$OUT/lfn.obj" src/lfn/lfn.c
wcc "${CFLAGS[@]}" -nt=ITEXT -nc=ICODE -fo="$OUT/lfn_init.obj" src/lfn/lfn_init.c
wlink option quiet format raw bin option offset=0x100 option start=start \
	option map="$OUT/lfn.map" name "$OUT/LFN.COM" \
	file "$OUT/lfn_start.obj,$OUT/lfn.obj,$OUT/lfn_init.obj"
python3 - "$OUT/LFN.COM" "$OUT/lfn.map" <<'PY'
import re, sys
image, mapfile = sys.argv[1], sys.argv[2]
text = open(mapfile).read()
m = re.search(r'resident_end\s*$', text, re.M)
size = len(open(image, 'rb').read())
res = None
for line in text.splitlines():
    if 'resident_end' in line:
        res = int(line.split(':')[-1].split()[0], 16) if ':' in line else None
if size > 0xF000:
    sys.exit(f'{image}: too large ({size} bytes)')
print(f'[build-lfn] {image}: {size} bytes' + (f', resident {res + 0x100 if res is not None else "?"} bytes' if res is not None else ''))
PY
