#!/usr/bin/env python3
"""Canonical test-only PE bootstrap and the unmodified f5-02 nested SEH image."""
# SPDX-License-Identifier: MIT
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts.win32 import census, fixtures, toolchain


def build():
    _, errors, _ = toolchain.verify(probe=False)
    if errors:
        raise RuntimeError('\n'.join(errors))
    out = ROOT/'build/host/win32/core'
    out.mkdir(parents=True, exist_ok=True)
    spec = next(s for s in fixtures.manifests() if s['id'] == 'seh-nested')
    exe = fixtures.compiled(spec, out)
    flags = ['--target=i686-pc-windows-msvc', '-march=pentiumpro', '-mno-sse',
             '-mno-sse2', '-mno-mmx', '-ffreestanding', '-fno-builtin',
             '-fno-stack-protector', '-O1', '-Wall', '-Wextra', '-Werror',
             '-I', str(ROOT/'src/kernel/include')]
    objects = []
    for source in ('core_loader.c', 'core_gate.s'):
        obj = out/(Path(source).stem+'.obj')
        subprocess.run(['clang', *flags, *(['-Wno-unused-command-line-argument'] if source.endswith('.s') else []), '-c', str(fixtures.SOURCES/source), '-o', str(obj)], check=True)
        objects.append(str(obj))
    loader = out/'core-loader.dll'
    subprocess.run(['lld-link', '/machine:x86', '/nodefaultlib', '/entry:entry',
                    '/dll', '/base:0x70010000', '/subsystem:console,4.0', '/osversion:4.0',
                    '/timestamp:0', '/incremental:no', '/safeseh:no',
                    '/export:CoreResult,DATA', '/export:CoreRelease,DATA',
                    '/out:'+str(loader),
                    '/implib:'+str(out/'core-loader.lib'), *objects], check=True)
    pe = census.PE(loader.read_bytes())
    rva, _ = pe.dirs[0]
    header = pe.words(rva, 10)
    nf, nn, functions, names, ordinals = header[5:]
    exports = {}
    for i, name_rva in enumerate(pe.words(names, nn)):
        index = struct.unpack('<H', pe.read(ordinals+i*2, 2))[0]
        if index >= nf:
            raise ValueError('fixture export ordinal')
        exports[pe.string(name_rva)] = pe.words(functions+index*4, 1)[0]
    for name in ('CoreResult', 'CoreRelease'):
        if name not in exports:
            raise ValueError('missing fixture data export '+name)
    result = {'loader': str(loader), 'exe': str(exe), 'base': pe.base, **exports}
    (out/'layout.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


if __name__ == '__main__':
    print(json.dumps(build(), indent=2))
