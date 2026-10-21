"""Build the original scalar GDI builtin and audit its actual i686 instructions."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts.win32 import stubs
from scripts import build_kernel

SOURCES = ('dib.c', 'region.c', 'objects.c', 'clip.c', 'bitmap.c', 'palette.c',
           'draw.c', 'text.c', 'font.c', 'compat.c', 'upper.c', 'metafile.c',
           'uniscribe.c', 'bridge.c', 'pixelformat.c', 'compiler.c', 'compiler.S')
C_EXPORTS = ('w32_dib_validate','w32_dib_raw','w32_dib_rgb','w32_dib_encode',
             'w32_dib_put','w32_gdi_rop3','w32_dib_blt')
FLAGS = ['--target=i686-pc-windows-msvc', '-march=pentiumpro', '-O2',
         '-mno-sse', '-mno-sse2', '-mno-mmx', '-mno-80387', '-ffreestanding',
         '-fno-builtin', '-fno-stack-protector', '-fno-vectorize', '-fno-slp-vectorize',
         '-Wall', '-Wextra', '-Werror']

def build(out, base=0x72000000, flags=None):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    core = ROOT/'sdk/win32'
    objects, exports = [], {}
    sources = [core/'gdi32'/n for n in SOURCES]
    sources += [core/'kernel32/compiler.c', core/'kernel32/compiler.S', core/'services/bytes.c', core/'services/stack.S']
    for i, source in enumerate(sources):
        obj = out/f'gdi32-{i}.obj'
        subprocess.run(['clang', *FLAGS, '-Wno-unused-command-line-argument',
                        '-I'+str(core/'include'), '-c', str(source), '-o', str(obj)], check=True)
        objects.append(str(obj))
        nm = subprocess.run(['llvm-nm', '--defined-only', str(obj)], check=True, capture_output=True, text=True).stdout
        if source.parent.name == 'gdi32' and source.name not in ('compiler.c', 'compiler.S'):
            for name, arity in re.findall(r'\bT _([A-Za-z][A-Za-z0-9_]*)@(\d+)\s*$', nm, re.M):
                if name != 'GdiDllMain':
                    exports[name] = f'_{name}@{arity}'
            for name in C_EXPORTS:
                if re.search(r'\bT _'+name+r'\s*$',nm,re.M):
                    exports[name]=name
    from scripts.win32 import user32 as user_generator
    wide = user_generator.wide_gate(objects, exports, out, 'gdi32.dll', (), FLAGS)
    imports = stubs.imports((ROOT/'docs/design/research/win32-layer-2026-10-14.md').read_text()).get('gdi32.dll', set())
    imports |= stubs.imports((ROOT/'docs/design/win32-census/fixtures.txt').read_text()).get('gdi32.dll', set())
    imports |= stubs.acceptance_imports('gdi32.dll')  # f5-15B
    missing = sorted(imports-exports.keys())
    source = out/'gdi32-remainder.c'
    source.write_text('__declspec(dllimport) void w32_stub_hit(volatile unsigned *,const char *,const char *);\n'+''.join(
        f'static volatile unsigned once{i}; void missing{i}(void) {{ w32_stub_hit(&once{i},"gdi32.dll","{name}"); __builtin_trap(); }}\n'
        for i, name in enumerate(missing)))
    obj = out/'gdi32-remainder.obj'
    subprocess.run(['clang', *FLAGS, '-c', str(source), '-o', str(obj)], check=True)
    objects.append(str(obj))
    for dll, declarations in [('ntdll', ' w32_stub_hit\n w32_get_services\n w32_stricmp\n memcpy\n memset\n'
                                        ' w32_shim_stub\n w32_shim_w_stub\n w32_server_exchange\n'),
                              ('kernel32', ' MapViewOfFile@20\n UnmapViewOfFile@4\n CreateFileA@28\n ReadFile@20\n GetFileSize@8\n'
                                           ' SetFilePointer@16\n CloseHandle@4\n CopyFileA@12\n DeleteFileA@4\n'
                                           ' GetModuleHandleA@4\n LoadLibraryA@4\n GetProcAddress@8\n SetLastError@4\n WriteFile@20\n')]:
        definition = out/f'gdi32-{dll}.def'
        definition.write_text(f'LIBRARY {dll}.dll\nEXPORTS\n'+declarations)
        subprocess.run(['llvm-dlltool', '-m', 'i386', '-k', '-d', str(definition), '-l', str(out/f'gdi32-{dll}.lib')], check=True)
    definition = out/'gdi32.def'
    definition.write_text('LIBRARY gdi32.dll\nEXPORTS\n'+''.join(f' {n}={target}\n' for n,target in sorted(exports.items()))+
                          ''.join(f' {name}=missing{i}\n' for i,name in enumerate(missing)))
    resource_source = out/'gdi32-version.rc'
    resource_source.write_text('// SPDX-License-Identifier: LGPL-2.1-or-later\n1 VERSIONINFO\nFILEVERSION 0,0,0,0\nPRODUCTVERSION 0,0,0,0\n'
        'FILEFLAGSMASK 0x3f\nFILEFLAGS 0\nFILEOS 0x40004\nFILETYPE 2\nBEGIN\nBLOCK "StringFileInfo"\nBEGIN\n'
        'BLOCK "040904E4"\nBEGIN\nVALUE "FileDescription", "CiukiOS scalar GDI engine\\0"\nEND\nEND\n'
        'BLOCK "VarFileInfo"\nBEGIN\nVALUE "Translation", 0x409,1252\nEND\nEND\n')
    resource = out/'gdi32-version.res'
    subprocess.run(['llvm-rc', '/fo', str(resource), str(resource_source)], check=True)
    image = out/'gdi32.dll'
    subprocess.run(['lld-link', '/machine:x86', '/dll', '/entry:GdiDllMain@12', '/nodefaultlib', '/safeseh:no',
                    '/timestamp:0', '/incremental:no', '/base:'+hex(base), '/subsystem:windows,4.0', '/osversion:4.0',
                    '/def:'+str(definition), '/out:'+str(image), *objects, str(resource),
                    str(out/'gdi32-ntdll.lib'), str(out/'gdi32-kernel32.lib')], check=True)
    dis = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', str(image)], check=True, capture_output=True, text=True).stdout
    bad = build_kernel.audit_disassembly(dis, {})
    if bad:
        raise ValueError('GDI i686 FPU/SIMD audit failed: '+str(bad[:20]))
    (out/'gdi32-audit.json').write_text(json.dumps({'flags':FLAGS, 'fpu_simd_instructions':bad, 'sha256':hashlib.sha256(image.read_bytes()).hexdigest()}, indent=2)+'\n')
    from scripts.win32 import user32 as user_generator
    upper = []
    for row in user_generator.upper_decisions('gdi32.dll'):
        if row['name'] not in exports:
            raise ValueError('gdi32 lacks the decided reused-DLL export '+row['name'])
        upper.append(row)
    (out/'gdi32-coverage.json').write_text(json.dumps({'schema':1, 'scope':'provisional research union plus authored fixtures',
        'exports':[{'name':n, 'status':'implemented' if n in exports else 'W21 nonreturning stub',
                    'owner':'f5-10C' if re.match(r'(?:CreateFont|CreateScalableFont|EnumFont|GetTextMetrics|AddFont|RemoveFont|GetChar|GetOutline)', n) else 'f5-10B'} for n in sorted(imports)],
        'upper_scope':'f5-20B: core-implement and core-w21-stub decisions of config/wine-upper.json owned by gdi32.dll',
        'upper':upper, 'wide_gate':wide}, indent=2)+'\n')
    raw=image.read_bytes()
    pe=int.from_bytes(raw[60:64], 'little')
    return {'name':'gdi32.dll','path':'gdi32.dll','base':base,'size':int.from_bytes(raw[pe+80:pe+84], 'little'),
            'sha256':hashlib.sha256(raw).hexdigest(),'version':stubs.image_version(raw)}

if __name__ == '__main__':
    print(json.dumps(build(ROOT/'build/host/win32/gdi32/builtin'), indent=2))
