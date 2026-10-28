"""Build the authored IS5 replacement; no commercial input is used."""
# SPDX-License-Identifier: LGPL-2.1-or-later
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def build(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    flags = ['--target=i686-pc-windows-msvc', '-march=pentiumpro', '-ffreestanding',
             '-fno-builtin', '-fno-stack-protector', '-mno-stack-arg-probe',
             '-mno-sse', '-mno-sse2', '-mno-mmx', '-O1', '-Wall', '-Wextra', '-Werror',
             '-I' + str(ROOT / 'sdk/win32/include'), '-I' + str(ROOT / 'src/kernel/include')]
    objects, defined, undefined = [], set(), set()
    for name in ('archive', 'handoff', 'main'):
        obj = out / ('is5-' + name + '.obj')
        source = ROOT / 'sdk/win32/is5boot' / (name + '.c')
        subprocess.run(['clang', *flags, '-c', source, '-o', obj], check=True)
        objects.append(obj)
        symbols = subprocess.check_output(['llvm-nm', obj], text=True)
        defined.update(re.findall(r'\b[TDRB] (\S+)', symbols))
        undefined.update(re.findall(r'\bU (\S+)', symbols))
    imports = {}
    for symbol in sorted(undefined - defined):
        match = re.fullmatch(r'_([A-Za-z]\w*)@(\d+)', symbol)
        if not match:
            raise ValueError('unexpected helper import: ' + symbol)
        name, count = match.groups()
        dll = 'user32' if name == 'MessageBoxA' else 'kernel32'
        imports.setdefault(dll, []).append(name + '@' + count)
    libraries = []
    for dll, names in imports.items():
        definition = out / ('is5-' + dll + '.def')
        definition.write_text('LIBRARY ' + dll + '.dll\nEXPORTS\n' + ''.join(' ' + n + '\n' for n in names))
        library = definition.with_suffix('.lib')
        subprocess.run(['llvm-dlltool', '-m', 'i386', '-k', '-d', definition, '-l', library], check=True)
        libraries.append(library)
    image = out / 'is5boot.exe'
    subprocess.run(['lld-link', '/machine:x86', '/entry:entry@0', '/nodefaultlib', '/safeseh:no',
                    '/timestamp:0', '/incremental:no', '/base:0x00400000', '/subsystem:windows,4.0',
                    # Bounded directory/Huffman workspaces use <32 KiB of
                    # stack; keep them inside the initial commit without CRT probes.
                    '/stack:1048576,65536',
                    '/osversion:4.0', '/out:' + str(image), *objects, *libraries], check=True)
    return image


if __name__ == '__main__':
    print(build(ROOT / 'build/apps/win32'))
