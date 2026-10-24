"""Build the authored CD player and w32-media CD fixture; no guest runs."""
# SPDX-License-Identifier: LGPL-2.1-or-later
from pathlib import Path
import re
import subprocess
from scripts.win32 import gdi32
ROOT = Path(__file__).resolve().parents[2]


def build_one(out, source, name):
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    obj = out / (name + '.obj')
    subprocess.run(['clang', *gdi32.FLAGS, '-I'+str(ROOT/'sdk/win32/include'),
                    '-I'+str(ROOT/'src/kernel/include'), '-c', str(ROOT/source), '-o', str(obj)], check=True)
    # LLVM lowers sufficiently large automatic array initializers to memcpy.
    helper = out / (name + '-memory.c')
    helper.write_text('typedef unsigned int size_t;\n'
                      'void *memcpy(void *d,const void *s,size_t n){volatile unsigned char *a=d;const volatile unsigned char *b=s;for(size_t i=0;i<n;i++)a[i]=b[i];return d;}\n'
                      'void *memset(void *d,int c,size_t n){volatile unsigned char *a=d;for(size_t i=0;i<n;i++)a[i]=(unsigned char)c;return d;}\n')
    helper_obj = helper.with_suffix('.obj')
    subprocess.run(['clang', *gdi32.FLAGS, '-c', str(helper), '-o', str(helper_obj)], check=True)
    symbols = subprocess.check_output(['llvm-nm', '-u', str(obj)], text=True)
    groups = {}
    kernel = {'GetModuleHandleA', 'ExitProcess', 'GetCommandLineA', 'Sleep', 'VirtualAlloc', 'GetPrivateProfileStringA', 'WritePrivateProfileSectionA'}
    for symbol, arity in re.findall(r'\bU _([A-Za-z][A-Za-z0-9_]*)@(\d+)\s*$', symbols, re.M):
        dll = 'kernel32' if symbol in kernel else 'winmm' if symbol.startswith('mci') else 'user32'
        groups.setdefault(dll, []).append(f'{symbol}@{arity}')
    libs = []
    for dll, names in groups.items():
        definition = out / (name + '-' + dll + '.def')
        definition.write_text(f'LIBRARY {dll}.dll\nEXPORTS\n'+''.join(' '+s+'\n' for s in sorted(names)))
        library = definition.with_suffix('.lib')
        subprocess.run(['llvm-dlltool', '-m', 'i386', '-k', '-d', str(definition), '-l', str(library)], check=True)
        libs.append(str(library))
    image = out / (name + '.exe')
    subprocess.run(['lld-link', '/machine:x86', '/entry:entry@0', '/nodefaultlib', '/safeseh:no', '/timestamp:0',
                    '/incremental:no', '/base:0x00400000', '/subsystem:windows,4.0', '/osversion:4.0',
                    '/out:'+str(image), str(obj), str(helper_obj), *libs], check=True)
    return image


def build(out):
    return [build_one(out, 'apps/desktop/cdplayer.c', 'cdplayer'),
            build_one(out, 'src/probes/win32_media/cdaudio.c', 'media-cdaudio')]
