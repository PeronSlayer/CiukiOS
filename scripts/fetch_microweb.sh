#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION=2.1
DEST="$ROOT_DIR/build/external/microweb/$VERSION/payload"
WORK="$ROOT_DIR/build/external/microweb/$VERSION/work"
CACHE="${CIUKIOS_DOWNLOAD_CACHE_DIR:-$ROOT_DIR/build/downloads}"
BIN="$CACHE/microweb-$VERSION.zip"
SRC="$CACHE/microweb-$VERSION-src.zip"
BIN_URL="https://github.com/jhhoward/MicroWeb/releases/download/v$VERSION/MicroWeb.zip"
SRC_URL="https://github.com/jhhoward/MicroWeb/archive/refs/tags/v$VERSION.zip"
BIN_SHA=fd0b48540b447e3d1399f9af478c5ee504cb6879f2a7e0449d76ba95d5e51459
SRC_SHA=debd12d647ce7ce6da353d58f5c6ac4f5dddd328e265bcf50d41aaeefaaa79b5

mkdir -p "$CACHE" "$DEST"
fetch_verified() {
  local url="$1" sha="$2" path="$3"
  if ! echo "$sha  $path" | sha256sum -c --status 2>/dev/null; then
    curl -fL --retry 3 -o "$path.tmp" "$url"
    echo "$sha  $path.tmp" | sha256sum -c --status
    mv -f "$path.tmp" "$path"
  fi
}
fetch_verified "$BIN_URL" "$BIN_SHA" "$BIN"
fetch_verified "$SRC_URL" "$SRC_SHA" "$SRC"

for name in CGA.dat EGA.dat Default.dat LowRes.dat; do
  unzip -p "$BIN" "$name" > "$DEST/$name"
  test -s "$DEST/$name"
done
unzip -p "$SRC" "MicroWeb-$VERSION/COPYING.md" > "$DEST/COPYING.md"
rm -f "$DEST/MICROWEB.EXE"

rm -rf -- "$WORK"
mkdir -p "$WORK"
unzip -oq "$SRC" -d "$WORK"
python3 - "$WORK/MicroWeb-$VERSION" <<'PY'
import re
import shutil
import sys
from pathlib import Path

root = Path(sys.argv[1])
makefile = root / 'project/DOS/Makefile'
source = makefile.read_text().replace('\\', '/')
source = source.replace('-DCFG_H="tcp.cfg"', '''-DCFG_H='"tcp.cfg"' ''')
source = source.replace('wasm -0 $(memory_model) $[*',
                        'wasm -fo=$@ -0 $(memory_model) $[*')
source = source.replace('wpp $[* $(tcp_compile_options)',
                        'wpp -fo=$@ $[* $(tcp_compile_options)')
makefile.write_text(source)
shutil.copyfile(root / 'src/Microweb.cpp', root / 'src/MicroWeb.cpp')
net_header = root / 'src/DOS/DOSNet.h'
source = net_header.read_text()
source = source.replace('#define TCP_RECV_BUFFER_SIZE  (16384)',
                        '#define TCP_RECV_BUFFER_SIZE  (8192)')
source = source.replace('#define MAX_CONCURRENT_HTTP_REQUESTS 2',
                        '#define MAX_CONCURRENT_HTTP_REQUESTS 1')
net_header.write_text(source)
(root / 'project/DOS/tcp.cfg').symlink_to('TCP.CFG')

for directory in (root / 'lib/mTCP/TCPINC', root / 'lib/mTCP/TCPLIB'):
    for item in list(directory.iterdir()):
        if not item.is_file() or item.is_symlink():
            continue
        aliases = {item.name.lower(), item.name.title(), item.name.capitalize()}
        if directory.name == 'TCPLIB':
            aliases.add(item.stem.lower() + item.suffix)
        for name in aliases:
            target = directory / name
            if name != item.name and not target.exists():
                target.symlink_to(item.name)

for item in list((root / 'src').rglob('*')) + list((root / 'lib/mTCP/TCPLIB').iterdir()):
    if not item.is_file() or item.is_symlink() or item.suffix.lower() not in ('.h', '.cpp', '.c'):
        continue
    for name in re.findall(r'#\s*include\s*["<]([^">]+)[">]',
                           item.read_text(errors='replace')):
        relative = Path(name.replace('\\', '/'))
        for candidate_dir in (item.parent / relative.parent,
                              root / 'lib/mTCP/TCPINC'):
            if not candidate_dir.exists():
                continue
            target = candidate_dir / relative.name
            if target.exists():
                break
            candidates = [p for p in candidate_dir.iterdir()
                          if not p.is_symlink() and p.name.casefold() == relative.name.casefold()]
            if len(candidates) == 1:
                target.symlink_to(candidates[0].name)
                break
PY

export WATCOM="${WATCOM:-/opt/watcom}"
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h"
(
  cd "$WORK/MicroWeb-$VERSION/project/DOS"
  wmake -f Makefile > "$WORK/build.log" 2>&1
) || { tail -n 35 "$WORK/build.log" >&2; exit 1; }
cp "$WORK/MicroWeb-$VERSION/project/DOS/MicroWeb.exe" "$DEST/MICROWEB.EXE"

python3 - "$WORK/MicroWeb-$VERSION" "$DEST/SOURCE.ZIP" <<'PY'
import sys
import zipfile
from pathlib import Path

root, output = Path(sys.argv[1]), Path(sys.argv[2])
with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
    for path in root.rglob('*'):
        if not path.is_file() or path.is_symlink() or path.suffix.lower() in ('.obj', '.o', '.exe'):
            continue
        if path.name in ('map.map', 'build.log'):
            continue
        archive.write(path, f'{root.name}/{path.relative_to(root)}')
    archive.writestr(f'{root.name}/CIUKIOS-BUILD.TXT',
                     'CiukiOS memory profile: one HTTP socket and an 8 KiB receive buffer.\n'
                     'Build with OpenWatcom 2.0: cd project/DOS && wmake -f Makefile.\n'
                     'The Linux case aliases used during this build are created by\n'
                     'scripts/fetch_microweb.sh in the CiukiOS source repository.\n')
PY
echo "[fetch-microweb] MicroWeb $VERSION: verified release, CiukiOS memory build and corresponding source"
