"""Build the nine runtime-selected F5 core PEs; resolve every actual import."""
# SPDX-License-Identifier: LGPL-2.1-or-later
from pathlib import Path
import re
import subprocess
from scripts.win32 import census, gdi32

ROOT = Path(__file__).resolve().parents[2]
NAMES = ('fullscreen', 'registry', 'audio', 'gdi32', 'crash', 'user32',
         'codecs', 'ddraw', 'input')


def owner(name):
    if name in ('memcpy', 'memset'):
        return 'ntdll'
    if name.startswith('Reg') and name != 'RegisterClassA':
        return 'advapi32'
    if name.startswith(('acm',)):
        return 'msacm32'
    if name in ('ICOpen', 'ICClose', 'ICSendMessage', 'ICDecompress'):
        return 'msvfw32'
    if name.startswith(('waveOut', 'CiukiAudio')):
        return 'winmm'
    if name.startswith(('DirectDraw', 'w32_ddraw_')):
        return 'ddraw'
    if name.startswith('DirectInput'):
        return 'dinput8'
    if name.startswith('w32_gdi_') or name in set(re.findall(
            r'\bW32API\s+(\w+)\s*\(', (ROOT/'sdk/win32/include/w32/gdi32.h').read_text())):
        return 'gdi32'
    if name in ('MapVirtualKeyExA', 'ToAsciiEx', 'GetKeyboardLayoutList') or name in set(re.findall(
            r'\bW32API\s+(\w+)\s*\(', (ROOT/'sdk/win32/include/w32/user32.h').read_text())):
        return 'user32'
    return 'kernel32'


def build(out, dlls=None):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    images = []
    for name in NAMES:
        obj = out/(name+'-exercise.obj')
        subprocess.run(['clang', *gdi32.FLAGS, '-I'+str(ROOT/'sdk/win32/include'),
                        '-I'+str(ROOT/'src/kernel/include'), '-c',
                        str(ROOT/f'src/probes/win32_{name}/exercise.c'), '-o', str(obj)], check=True)
        symbols = subprocess.run(['llvm-nm', '-u', str(obj)], check=True,
                                 capture_output=True, text=True).stdout
        table = {}
        for line in symbols.splitlines():
            match = re.fullmatch(r'\s*U _([A-Za-z][A-Za-z0-9_]*)(@\d+)?\s*', line)
            if not match:
                raise ValueError(f'{name}: unexpected PE import: {line}')
            symbol, arity = match.groups()
            if not arity and symbol not in ('memcpy', 'memset'):
                raise ValueError(f'{name}: unexpected cdecl import: {line}')
            table.setdefault(owner(symbol), []).append(symbol+(arity or ''))
        libraries = []
        for dll, entries in sorted(table.items()):
            definition = out/f'{name}-exercise-{dll}.def'
            definition.write_text(f'LIBRARY {dll}.dll\nEXPORTS\n'+''.join(' '+e+'\n' for e in sorted(entries)))
            library = definition.with_suffix('.lib')
            subprocess.run(['llvm-dlltool', '-m', 'i386', '-k', '-d', str(definition),
                            '-l', str(library)], check=True)
            libraries.append(str(library))
        image = out/(name+'-exercise.exe')
        subprocess.run(['lld-link', '/machine:x86', '/entry:entry@0', '/nodefaultlib', '/safeseh:no',
                        '/timestamp:0', '/incremental:no', '/base:0x00400000', '/subsystem:windows,4.0',
                        '/osversion:4.0', '/out:'+str(image), str(obj), *libraries], check=True)
        if dlls:
            for imported in census.inspect(image.read_bytes())['imports']:
                provider = Path(dlls)/imported['dll'].lower()
                exports = {n for row in census.inspect(provider.read_bytes())['exports'] for n in row['names']}
                missing = set(imported['symbols'])-exports
                if missing:
                    raise ValueError(f'{name}: unresolved {provider.name}: {sorted(missing)}')
        images.append(image)
    return images
