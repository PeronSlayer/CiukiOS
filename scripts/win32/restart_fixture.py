"""Build the runtime selected, authored restart observer fixture."""
# SPDX-License-Identifier: LGPL-2.1-or-later
from pathlib import Path
import re
import subprocess
from scripts.win32 import gdi32, census
ROOT = Path(__file__).resolve().parents[2]
NAME = 'restart-exercise.exe'
ADVAPI32 = {'RegCreateKeyExA','RegOpenKeyExA','RegSetValueExA','RegQueryValueExA','RegCloseKey'}
KERNEL32 = {'ExitProcess','GetCommandLineA','VirtualAlloc','Sleep','GetTickCount',
            'CreateFileA','ReadFile','WriteFile','FlushFileBuffers','CloseHandle',
            'DeleteFileA','GetFileAttributesA','GetLastError','GlobalAlloc','GlobalFree',
            'GlobalLock','GlobalUnlock','GlobalSize','GlobalAddAtomA','GlobalDeleteAtom'}


def build(out, dlls=None):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    objects = []
    imports = {n: set() for n in ('kernel32','user32','advapi32')}
    for source in ('exercise','guest'):
        obj = out/f'restart-{source}.obj'
        subprocess.run(['clang',*gdi32.FLAGS,'-I'+str(ROOT/'sdk/win32/include'),
                        '-I'+str(ROOT/'src/kernel/include'),'-c',
                        str(ROOT/f'src/probes/win32_restart/{source}.c'),'-o',str(obj)],check=True)
        objects.append(str(obj))
        text = subprocess.run(['llvm-nm','-u',str(obj)],check=True,capture_output=True,text=True).stdout
        for symbol in re.findall(r'\bU (\S+)',text):
            if symbol in ('_w32_restart_exercise','_w32_restart_helper'):
                continue
            match = re.fullmatch(r'_([A-Za-z][A-Za-z0-9_]*)@(\d+)',symbol)
            if not match:
                raise ValueError('unexpected restart fixture import: '+symbol)
            name, arity = match.groups()
            dll = 'advapi32' if name in ADVAPI32 else 'kernel32' if name in KERNEL32 else 'user32'
            imports[dll].add(f'{name}@{arity}')
    libraries = []
    for dll, names in imports.items():
        definition = out/f'restart-{dll}.def'
        definition.write_text(f'LIBRARY {dll}.dll\nEXPORTS\n'+''.join(f' {n}\n' for n in sorted(names)))
        library = definition.with_suffix('.lib')
        subprocess.run(['llvm-dlltool','-m','i386','-k','-d',str(definition),'-l',str(library)],check=True)
        libraries.append(str(library))
    image = out/NAME
    subprocess.run(['lld-link','/machine:x86','/entry:entry@0','/nodefaultlib','/safeseh:no',
                    '/timestamp:0','/incremental:no','/base:0x00400000','/subsystem:windows,4.0',
                    '/osversion:4.0','/out:'+str(image),*objects,*libraries],check=True)
    if dlls:
        exports = {dll: {n for row in census.inspect((Path(dlls)/f'{dll}.dll').read_bytes())['exports']
                         for n in row['names']} for dll in imports}
        for row in census.inspect(image.read_bytes())['imports']:
            for symbol in row['symbols']:
                if symbol not in exports.get(row['dll'].lower().removesuffix('.dll'),set()):
                    raise ValueError(f"missing restart fixture export: {row['dll']}!{symbol}")
    return image
