"""Build the runtime-selected kernel32 PE and verify its builtin imports."""
# SPDX-License-Identifier: LGPL-2.1-or-later
from pathlib import Path
import re
import subprocess
from scripts.win32 import census, gdi32

ROOT = Path(__file__).resolve().parents[2]


def build(out, dlls=None):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    obj = out/'kernel32-exercise.obj'
    subprocess.run(['clang', *gdi32.FLAGS, '-I'+str(ROOT/'sdk/win32/include'),
                    '-I'+str(ROOT/'src/kernel/include'), '-c',
                    str(ROOT/'src/probes/win32_kernel32/exercise.c'), '-o', str(obj)], check=True)
    symbols = subprocess.run(['llvm-nm', '-u', str(obj)], capture_output=True, text=True, check=True).stdout
    names = re.findall(r'\bU _([A-Za-z][A-Za-z0-9_]*@\d+)\s*$', symbols, re.M)
    if len(names) != len(symbols.splitlines()):
        raise ValueError('kernel32 fixture has a non-stdcall import: '+symbols)
    definition = out/'kernel32-exercise.def'
    definition.write_text('LIBRARY kernel32.dll\nEXPORTS\n'+''.join(' '+n+'\n' for n in names))
    library = out/'kernel32-exercise.lib'
    subprocess.run(['llvm-dlltool','-m','i386','-k','-d',str(definition),'-l',str(library)],check=True)
    image = out/'kernel32-exercise.exe'
    subprocess.run(['lld-link','/machine:x86','/entry:entry@0','/nodefaultlib','/safeseh:no',
                    '/timestamp:0','/incremental:no','/base:0x00400000','/subsystem:windows,4.0',
                    '/osversion:4.0','/out:'+str(image),str(obj),str(library)],check=True)
    if dlls:
        exports = {n for row in census.inspect((Path(dlls)/'kernel32.dll').read_bytes())['exports'] for n in row['names']}
        missing = [n for row in census.inspect(image.read_bytes())['imports'] for n in row['symbols'] if n not in exports]
        if missing:
            raise ValueError('kernel32 fixture imports missing exports: '+str(missing))
    return image
