"""f5-10C2: the w32-dialogs guest fixture (one authored PE, selected at run time).

src/probes/win32_dialogs/exercise.c and exercise.rc become
/System/Win32/Fixtures/dialogs-exercise.exe; the kernel probe reads its result
block (controller.h). No image variant, no guest run here."""
# SPDX-License-Identifier: LGPL-2.1-or-later
from pathlib import Path
import re
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts.win32 import gdi32

SOURCE = ROOT/'src/probes/win32_dialogs'
KERNEL32 = {'ExitProcess', 'GetModuleHandleA', 'Sleep', 'VirtualAlloc',
            'GetLastError', 'SetLastError',
            'FindResourceA', 'LoadResource', 'LockResource', 'SizeofResource'}
GDI32 = {'CreateFontIndirectA', 'DeleteObject'}
NAME = 'dialogs-exercise.exe'


def imports(obj):
    """Undefined stdcall symbols of the object, grouped by builtin DLL."""
    names = subprocess.run(['llvm-nm', '-u', str(obj)], check=True, capture_output=True, text=True).stdout
    table = {'kernel32': [], 'gdi32': [], 'user32': []}
    for name, arity in re.findall(r'\bU _([A-Za-z][A-Za-z0-9_]*)@(\d+)\s*$', names, re.M):
        dll = 'kernel32' if name in KERNEL32 else 'gdi32' if name in GDI32 or name.startswith('w32_gdi_') else 'user32'
        table[dll].append(f'{name}@{arity}')
    leftover = [l for l in names.split() if l not in ('U',) and not re.fullmatch(r'_[A-Za-z][A-Za-z0-9_]*@\d+', l)]
    if leftover:
        raise ValueError('dialogs fixture has non-stdcall imports: '+str(leftover))
    return table


def resolve(image, dlls):
    """Every import of the fixture must be a real export of the built DLLs."""
    from scripts.win32 import census
    info = census.inspect(Path(image).read_bytes())
    exports = {}
    for dll in ('kernel32', 'gdi32', 'user32'):
        rows = census.inspect((Path(dlls)/f'{dll}.dll').read_bytes())['exports']
        exports[dll] = {n for row in rows for n in row['names']}
    missing = [f"{row['dll']}!{name}" for row in info['imports'] for name in row['symbols']
               if name not in exports.get(row['dll'].lower().removesuffix('.dll'), set())]
    if missing:
        raise ValueError('dialogs fixture imports missing exports: '+', '.join(missing))


def build(out, dlls=None):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    flags = [*gdi32.FLAGS, '-I'+str(ROOT/'sdk/win32/include'), '-I'+str(ROOT/'src/kernel/include')]
    obj = out/'dialogs-exercise.obj'
    subprocess.run(['clang', *flags, '-c', str(SOURCE/'exercise.c'), '-o', str(obj)], check=True)
    res = out/'dialogs-exercise.res'
    subprocess.run(['llvm-rc', '/fo', str(res), str(SOURCE/'exercise.rc')], check=True)
    libraries = []
    for dll, names in imports(obj).items():
        definition = out/f'dialogs-{dll}.def'
        definition.write_text(f'LIBRARY {dll}.dll\nEXPORTS\n'+''.join(f' {n}\n' for n in sorted(names)))
        library = definition.with_suffix('.lib')
        subprocess.run(['llvm-dlltool', '-m', 'i386', '-k', '-d', str(definition), '-l', str(library)], check=True)
        libraries.append(str(library))
    image = out/NAME
    subprocess.run(['lld-link', '/machine:x86', '/entry:entry@0', '/nodefaultlib', '/safeseh:no', '/timestamp:0',
                    '/incremental:no', '/base:0x00400000', '/subsystem:windows,4.0', '/osversion:4.0',
                    '/out:'+str(image), str(obj), str(res), *libraries], check=True)
    if dlls:
        resolve(image, dlls)
    return image


if __name__ == '__main__':
    print(build(ROOT/'build/host/win32/dialogs-fixture'))
