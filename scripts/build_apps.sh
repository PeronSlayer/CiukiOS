#!/usr/bin/env bash
# Build the desktop application modules (src/apps: Files, Notepad, Tasks, the
# desktop, the Recycle Bin, the Control Panel and Devices)
# with OpenWatcom into flat CAPP images for \SYSTEM\APPS.
#   scripts/build_apps.sh [output-dir]      (default build/full/obj/apps)
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
OUT="${1:-build/full/obj/apps}"
export WATCOM="${WATCOM:-/opt/watcom}"
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h"
mkdir -p "$OUT"
CFLAGS=(-q -2 -ms -s -zl -os -d0 -wx -we -zq -zm -i=src/apps)
nasm -f obj src/apps/app_start.asm -o "$OUT/app_start.obj"
for unit in rt ui sys regstore; do
	wcc "${CFLAGS[@]}" -fo="$OUT/$unit.obj" "src/apps/$unit.c"
done
for app in files ciuknote tasks desktop recycle control devices paint dosvm browser; do
	[[ -f "src/apps/$app.c" ]] || continue
	name="${app^^}.APP"
	units="$OUT/app_start.obj,$OUT/$app.obj,$OUT/rt.obj,$OUT/ui.obj"
	case "$app" in
	files|desktop|recycle|control|devices) units="$units,$OUT/sys.obj" ;;
	esac
	[[ "$app" != control ]] || units="$units,$OUT/regstore.obj"
	wcc "${CFLAGS[@]}" -fo="$OUT/$app.obj" "src/apps/$app.c"
	wlink option quiet format raw bin option offset=0x100 option start=app_entry option eliminate \
		option map="$OUT/$app.map" name "$OUT/$name" \
		file "$units"
	# The memory the module needs: from its PSP-like base to the stack top.
	python3 - "$OUT/$name" "$OUT/$app.map" <<'PY'
import re, sys
image, mapfile = sys.argv[1], sys.argv[2]
end = 0
for line in open(mapfile):
    m = re.match(r'(\w+)\s+\w+\s+DGROUP\s+([0-9a-f]{8})\s+([0-9a-f]{8})', line)
    if m:
        end = max(end, int(m.group(2), 16) + int(m.group(3), 16))
data = bytearray(open(image, 'rb').read())
paras = (end + 15) // 16
if data[:4] != b'CAPP' or paras > 0x1000 or len(data) > 0xFE00:
    sys.exit(f'{image}: bad module (size {len(data)}, paragraphs {paras:#x})')
data[6:8] = paras.to_bytes(2, 'little')
open(image, 'wb').write(data)
print(f'[build-apps] {image}: {len(data)} bytes, {paras * 16} bytes of memory')
PY
done
