#!/usr/bin/env python3
"""Build f5-07A PE facades and account for every named census import."""
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
from scripts.win32 import census, stubs, stub_signatures

CORE = ROOT / 'sdk/win32'
CENSUS = ROOT / 'docs/design/research/win32-layer-2026-10-14.md'
DLLS = ('advapi32.dll', 'version.dll', 'lz32.dll')

def run(command):
    subprocess.run(list(map(str, command)), check=True)

def build(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    names = stubs.imports(CENSUS.read_text())
    required = out / 'required.txt'
    required.write_text('\n'.join(dll + '!' + name for dll in DLLS for name in sorted(names.get(dll, ()))) + '\n')
    stubs.generate([required], out)
    # Only imports used by these facades, with actual i386 stdcall arities.
    imports = {'CreateFileA': 28, 'CreateFileW': 28, 'GetFileSize': 8,
               'SetFilePointer': 16, 'ReadFile': 20, 'CloseHandle': 4,
               'GetProcessHeap': 0, 'HeapAlloc': 12, 'HeapFree': 12,
               'SetLastError': 4, 'OpenFile': 12, '_lread': 12, '_lwrite': 12,
               '_llseek': 12, '_lclose': 4, 'WideCharToMultiByte': 32,
               'MultiByteToWideChar': 24}
    definition = out / 'kernel32-imports.def'
    definition.write_text('LIBRARY kernel32.dll\nEXPORTS\n' + ''.join(f' {n}@{a}\n' for n, a in imports.items()))
    run(['llvm-dlltool', '-m', 'i386', '-k', '-d', definition, '-l', out/'kernel32.lib'])
    definition = out/'ntdll-imports.def'
    definition.write_text('LIBRARY ntdll.dll\nEXPORTS\n w32_stub_hit\n w32_shim_stub\n w32_report_refusal\n w32_shim_w_stub\n')
    run(['llvm-dlltool', '-m', 'i386', '-d', definition, '-l', out/'ntdll.lib'])
    store = {'schema': 1, 'dlls': []}
    coverage = {}
    base = 0x71000000
    for dll in DLLS:
        stem = dll[:-4]
        sources = [CORE/'services/bytes.c', CORE/'services/stack.S']
        if stem == 'advapi32':
            sources += [CORE/'advapi32'/n for n in ('registry.c','text.c','rpc.c','gate.S')]
        elif stem == 'version':
            sources += [CORE/'version/version.c', CORE/'services/files.c', CORE/'advapi32/text.c']
        else:
            sources += [CORE/'lz32/lz32.c', CORE/'lz32/decode.c', CORE/'services/files.c']
            sources += [CORE/'lz32/mspack'/n for n in ('kwajd.c','szddd.c','lzssd.c','mszipd.c')]
        objects = []
        implemented = set()
        wide = []
        for i, source in enumerate(sources):
            obj = out/f'{stem}-{i}.obj'
            flags = ['--target=i686-pc-windows-msvc', '-march=pentiumpro', '-mno-sse', '-mno-sse2',
                     '-mno-mmx','-ffreestanding','-fno-builtin','-fno-stack-protector','-O1',
                     '-Wall','-Wextra','-Werror','-I'+str(CORE/'include'), '-I'+str(ROOT/'src/kernel/include')]
            if source.suffix == '.S':
                flags += ['-Wno-unused-command-line-argument']
            if stem == 'lz32':
                flags += ['-DMSPACK_NO_DEFAULT_SYSTEM', '-DSIZEOF_OFF_T=8',
                          '-I'+str(CORE/'lz32/compat'), '-I'+str(CORE/'lz32/mspack')]
                if source.parent.name == 'mspack':
                    flags += ['-Wno-unused-parameter','-Wno-sign-compare']
            run(['clang', *flags, '-c', source, '-o', obj])
            objects.append(obj)
            nm = subprocess.run(['llvm-nm','--defined-only',str(obj)],check=True,capture_output=True,text=True).stdout
            # Public facade implementations have stdcall decoration; helper
            # functions and the cdecl libmspack API never become PE exports.
            if source.name in ('registry.c','version.c','lz32.c'):
                implemented.update(re.findall(r'\b_([A-Za-z][A-Za-z0-9_]*)@\d+\b', nm))
                for name,bytes_ in re.findall(r'\bT _([A-Za-z][A-Za-z0-9_]*W)@(\d+)\b',nm):
                    run(['llvm-objcopy','--redefine-sym',f'_{name}@{bytes_}=_w32_real_{name}@{bytes_}',obj])
                    wide.append((name,int(bytes_)))
        gates=out/(stem+'-wide.c')
        gates.write_text(stub_signatures.DECLARATIONS+'\n__declspec(dllimport) int w32_shim_w_stub(const char *);\n'+
            '\n'.join(stub_signatures.wide_gate(dll,n,b,i) for i,(n,b) in enumerate(wide))+'\n')
        obj=out/(stem+'-wide.obj')
        run(['clang',*flags,'-c',gates,'-o',obj]);objects.append(obj)
        unresolved = names.get(dll,set()) - implemented
        source = out/(stem+'-stubs.c')
        bodies=[stub_signatures.body(dll,n,f'stub{i}',i) for i,n in enumerate(sorted(unresolved))]
        source.write_text(stub_signatures.DECLARATIONS+'\n'+'\n'.join(b[0] for b in bodies))
        obj = out/(stem+'-stubs.obj')
        run(['clang','--target=i686-pc-windows-msvc','-march=pentiumpro','-ffreestanding','-fno-stack-protector','-O1','-c',source,'-o',obj])
        objects.append(obj)
        definition = out/(stem+'.def')
        definition.write_text('LIBRARY '+dll+'\nEXPORTS\n'+''.join(' '+n+'\n' for n in sorted(implemented))+
                              ''.join(f' {n}={bodies[i][1]}\n' for i,n in enumerate(sorted(unresolved))))
        resource = out/(stem+'.res')
        rc = out/(stem+'.rc')
        rc.write_text(rc.read_text().replace('placeholder', 'system facade'))
        run(['llvm-rc','/fo',resource,rc])
        image = out/dll
        run(['lld-link','/machine:x86','/dll','/noentry','/nodefaultlib','/safeseh:no',
             '/base:'+hex(base),'/timestamp:0','/incremental:no','/subsystem:windows,4.0',
             '/osversion:4.0','/def:'+str(definition),'/out:'+str(image),*objects,resource,
             out/'ntdll.lib',*([out/'kernel32.lib'] if stem != 'advapi32' else [])])
        raw = image.read_bytes()
        pe = census.PE(raw)
        coverage[dll] = {n: 'implemented' if n in implemented else 'W21 stub' for n in sorted(names.get(dll,()))}
        actual = {n for e in census.inspect(raw)['exports'] for n in e['names']}
        if not set(coverage[dll]) <= actual:
            raise ValueError('missing census exports')
        store['dlls'].append({'name':dll,'path':dll,'base':base,'size':pe.size,
                              'sha256':hashlib.sha256(raw).hexdigest(),'version':[0,0,0,0]})
        base += (pe.size+65535)&~65535
    (out/'builtin-store.json').write_text(json.dumps(store,indent=2,sort_keys=True)+'\n')
    (out/'coverage.json').write_text(json.dumps(coverage,indent=2,sort_keys=True)+'\n')
    stubs.verify_store(out/'builtin-store.json')
    return store

if __name__ == '__main__':
    build(Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'build/apps/w32srv/builtin')
