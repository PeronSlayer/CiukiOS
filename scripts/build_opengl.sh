#!/usr/bin/env bash
# Build the first CiukiOS software OpenGL runtime: a 32-bit DOS/4GW TinyGL
# static library and a VGA demo that runs in an M4 DOS window.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
OUT="${1:-build/full/obj/gl}"
ARCHIVE=build/downloads/TinyGL-0.4.1.tar.gz
URL=https://bellard.org/TinyGL/TinyGL-0.4.1.tar.gz
SHA=b314f019a778d583a3778cb14bcc2a57626a8f866cbed0693705bb1bb785b16f
export WATCOM="${WATCOM:-/opt/watcom}"
export INCLUDE="$WATCOM/h"
export PATH="$WATCOM/binl64:$PATH"
mkdir -p "$OUT" build/downloads
if [[ ! -f "$ARCHIVE" ]]; then curl -fL --retry 3 "$URL" -o "$ARCHIVE"; fi
printf '%s  %s\n' "$SHA" "$ARCHIVE" | sha256sum -c - >/dev/null
tar -xzf "$ARCHIVE" -C "$OUT"
UPSTREAM="$OUT/TinyGL"

# TinyGL's upstream GCC source uses a GNU variadic macro and the identifier
# "near", which OpenWatcom reserves. Keep the port mechanical and reproducible.
python3 - "$UPSTREAM" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
for path in list((root / 'src').glob('*.[ch]')) + list((root / 'include').rglob('*.h')):
    source = path.read_text(encoding='latin1')
    source = re.sub(r'\bnear\b', 'znear', source)
    if path.name == 'zgl.h':
        source = '#ifndef M_PI\n#define M_PI 3.14159265358979323846\n#endif\n' + source
        source, count = re.subn(
            r'#ifdef DEBUG\n\n#define dprintf.*?\n#endif',
            '#define dprintf ciuki_dprintf\nvoid ciuki_dprintf(const char *format, ...);',
            source, flags=re.S)
        if count != 1:
            raise SystemExit('TinyGL port: expected debug macro was not found')
    path.write_text(source, encoding='latin1')
(root / 'src' / 'ciuki_port.c').write_text(
    'void ciuki_dprintf(const char *format, ...) { (void)format; }\n')
PY

objects=()
for source in "$UPSTREAM"/src/*.c; do
    case "$source" in */glx.c|*/nglx.c) continue ;; esac
    object="$OUT/$(basename "${source%.c}").obj"
    wcc386 -q -zq -ot -i="$UPSTREAM/include" -fo="$object" "$source"
    objects+=("+$object")
done
wcc386 -q -zq -ot -i="$UPSTREAM/include" -i="$UPSTREAM/src" \
    -i=src/probes/gl -fo="$OUT/ciukgl_runtime.obj" src/probes/gl/ciukgl_runtime.c
objects+=("+$OUT/ciukgl_runtime.obj")
wlib -q -n -b "$OUT/TINYGL.LIB" "${objects[@]}"
wcc386 -q -zq -ot -i="$UPSTREAM/include" -i=src/probes/gl \
    -fo="$OUT/ciukgl_demo.obj" src/probes/gl/ciukgl_demo.c
wlink system dos4g option quiet name "$OUT/GLDEMO.EXE" \
    file "$OUT/ciukgl_demo.obj" library "$OUT/TINYGL.LIB"
cp "$UPSTREAM/LICENSE" "$OUT/LICENSE.TXT"
cp "$UPSTREAM/include/GL/gl.h" "$OUT/GL.H"
cp src/probes/gl/ciukgl.h "$OUT/CIUKGL.H"
echo "[build-opengl] TinyGL 0.4.1 software subset: $OUT/TINYGL.LIB and GLDEMO.EXE"
