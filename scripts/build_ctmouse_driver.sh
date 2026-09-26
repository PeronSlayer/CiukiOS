#!/usr/bin/env bash
# Rebuild the pinned, separately licensed CuteMouse without executing DOS code.
set -euo pipefail
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
CTMOUSE_BUILD="${CIUKIOS_CTMOUSE_BUILD:-$ROOT_DIR/build/external/ctmouse-pinned}"
CTMOUSE_AS="${CIUKIOS_JWASM:-$ROOT_DIR/build/external/JWasm/build/GccUnixR/jwasm}"
[[ -x "$CTMOUSE_AS" ]] || { echo "JWasm not found: $CTMOUSE_AS" >&2; exit 1; }
python3 - "$ROOT_DIR/assets/drivers/ctmouse/CTMSRC.ZIP" "$CTMOUSE_BUILD" <<'PY'
import hashlib
from pathlib import Path
import sys
import zipfile

archive, destination = map(Path, sys.argv[1:])
assert hashlib.sha256(archive.read_bytes()).hexdigest() == '83c578487a1a53c986a40f4b1354510ce300a6d4e515a44e364c137387199ec2'
destination.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(archive) as files:
    for item in files.infolist():
        target = destination / item.filename
        if not target.resolve().is_relative_to(destination.resolve()):
            raise ValueError('Source archive contains a path outside the build folder')
    files.extractall(destination)
# The upstream archive carries a prebuilt EXE; force source compilation.
for name in ('ctmouse.exe', 'ctmouse.bin', 'bin2exe'):
    (destination / name).unlink(missing_ok=True)
PY
make -C "$CTMOUSE_BUILD" AS="$CTMOUSE_AS"
cmp "$CTMOUSE_BUILD/ctmouse.exe" "$ROOT_DIR/assets/drivers/ctmouse/CTMOUSE.EXE"
sha256sum "$CTMOUSE_BUILD/ctmouse.exe"
