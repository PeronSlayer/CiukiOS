"""Build the original kernel32 facade and census-sized W21 remainder."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
from scripts.win32 import stubs,stub_signatures
ROOT=Path(__file__).resolve().parents[2]
FORWARDS={name:'ntdll.'+target for name,target in {
    'InitializeCriticalSection':'RtlInitializeCriticalSection',
    'EnterCriticalSection':'RtlEnterCriticalSection',
    'LeaveCriticalSection':'RtlLeaveCriticalSection',
    'DeleteCriticalSection':'RtlDeleteCriticalSection',
    'RaiseException':'RaiseException', 'RtlUnwind':'RtlUnwind',
    'SetUnhandledExceptionFilter':'SetUnhandledExceptionFilter',
    'UnhandledExceptionFilter':'UnhandledExceptionFilter',
}.items()}
# Required lock entry points are implemented on F1-84, including names outside the census.
REQUIRED_VFS={'LockFile','LockFileEx','UnlockFile','UnlockFileEx'}
UNICODE_STUBS={'CompareStringW','GetLocaleInfoW','GetStringTypeW','LCMapStringW'}
# BOOL-returning critical-section APIs are wrappers, including the BOOLEAN
# to BOOL widening; VOID APIs can forward to the ntdll operations.
def build(out,base,flags):
    out=Path(out);core=ROOT/'sdk/win32';objects=[];exports={};wide=[]
    for source in sorted((core/'kernel32').glob('*.c'))+sorted((core/'kernel32').glob('*.S')):
        obj=out/('kernel32-'+source.stem+source.suffix+'.obj')
        subprocess.run(['clang',*flags,'-c',str(source),'-o',str(obj)],check=True)
        objects.append(str(obj))
        nm=subprocess.run(['llvm-nm','--defined-only',str(obj)],capture_output=True,text=True,check=True).stdout
        for name,bytes_ in re.findall(r'\bT _([A-Za-z][A-Za-z0-9_]*)@(\d+)\s*$',nm,re.M):
            if name.startswith('k32_'):continue
            exports[name]=f'_{name}@{bytes_}'
            if name.endswith('W'):
                subprocess.run(['llvm-objcopy','--redefine-sym',f'_{name}@{bytes_}=_w32_real_{name}@{bytes_}',str(obj)],check=True)
                wide.append((name,int(bytes_)))
    wrappers=[stub_signatures.DECLARATIONS,
        '__declspec(dllimport) int w32_shim_w_stub(const char *);']
    for i,(name,bytes_) in enumerate(wide):
        wrappers.append(stub_signatures.wide_gate('kernel32.dll',name,bytes_,i))
    source=out/'kernel32-wide.c';source.write_text('\n'.join(wrappers)+'\n')
    obj=out/'kernel32-wide.obj'
    subprocess.run(['clang',*flags,'-c',str(source),'-o',str(obj)],check=True);objects.append(str(obj))
    imported=stubs.imports((ROOT/'docs/design/research/win32-layer-2026-10-14.md').read_text())['kernel32.dll']
    imported|=stubs.imports((ROOT/'docs/design/win32-census/fixtures.txt').read_text()).get('kernel32.dll',set())
    missing=sorted((imported|REQUIRED_VFS)-exports.keys()-FORWARDS.keys())
    stub=['/* SPDX-License-Identifier: LGPL-2.1-or-later */',stub_signatures.DECLARATIONS]
    definitions=['; SPDX-License-Identifier: LGPL-2.1-or-later','LIBRARY kernel32.dll','EXPORTS']
    definitions += ['    '+name+'='+value for name,value in sorted(exports.items())]
    definitions += ['    '+name+'='+value for name,value in sorted(FORWARDS.items()) if name not in exports]
    for i,name in enumerate(missing):
        body,target,audit=stub_signatures.body('kernel32.dll',name,f'missing_{i}',i)
        stub.append(body)
        definitions.append(f'    {name}={target}')
    (out/'kernel32-remainder.c').write_text('\n'.join(stub)+'\n')
    obj=out/'kernel32-remainder.obj'
    subprocess.run(['clang',*flags,'-c',str(out/'kernel32-remainder.c'),'-o',str(obj)],check=True);objects.append(str(obj))
    definition=out/'kernel32.def';definition.write_text('\n'.join(definitions)+'\n')
    resource_source=out/'kernel32-version.rc'
    resource_source.write_text((out/'generated/kernel32.rc').read_text().replace('CiukiOS placeholder kernel32.dll','CiukiOS kernel32'))
    resource=out/'kernel32-version.res'
    subprocess.run(['llvm-rc','/fo',str(resource),str(resource_source)],check=True)
    image=out/'kernel32.dll'
    subprocess.run(['lld-link','/machine:x86','/dll','/noentry','/nodefaultlib','/safeseh:no','/timestamp:0',
        '/incremental:no','/fixed','/base:'+hex(base),'/def:'+str(definition),'/out:'+str(image),
        *objects,str(resource),str(out/'ntdll.lib')],check=True)
    raw=image.read_bytes();pe=struct.unpack_from('<I',raw,60)[0];size=struct.unpack_from('<I',raw,pe+80)[0]
    coverage=[]
    for name in sorted(imported):
        row={'name':name,'status':'W21 returning Unicode stub (120)' if name in UNICODE_STUBS else 'implemented' if name in exports else 'ntdll forwarder' if name in FORWARDS else 'W21 typed failure'}
        if name in missing:
            _,_,audit=stub_signatures.body('kernel32.dll',name,'audit',0)
            row['failure']=audit['failure'];row['abi']=audit
            if audit['failure'].startswith('nonreturning'):row['status']='W21 nonreturning stub'
        coverage.append(row)
    (out/'kernel32-coverage.json').write_text(json.dumps({'schema':1,'scope':'provisional research union plus authored fixtures; original acceptance media absent','exports':coverage,'deferred_vfs':[], 'required_vfs':sorted(REQUIRED_VFS)},indent=2)+'\n')
    return {'name':'kernel32.dll','path':'kernel32.dll','base':base,'size':size,
        'sha256':hashlib.sha256(raw).hexdigest(),'version':stubs.image_version(raw)}

def build_offline(out,base,flags):
    out=Path(out);obj=out/'offline-wsock.obj'
    subprocess.run(['clang',*flags,'-c',str(ROOT/'sdk/win32/offline/wsock.c'),'-o',str(obj)],check=True)
    nm=subprocess.run(['llvm-nm','--defined-only',str(obj)],capture_output=True,text=True,check=True).stdout
    exports=dict(re.findall(r'\bT _([A-Za-z][A-Za-z0-9_]*)@(\d+)\s*$',nm,re.M))
    lines=['LIBRARY wsock32.dll','EXPORTS']
    for name,bytes_ in sorted(exports.items()):
        ordinal=stubs.ORDINALS['wsock32.dll'].get(name)
        lines.append(f'    {name}=_{name}@{bytes_}'+(f' @{ordinal}' if ordinal else ''))
    definition=out/'offline-wsock.def';definition.write_text('\n'.join(lines)+'\n')
    image=out/'wsock32.dll'
    subprocess.run(['lld-link','/machine:x86','/dll','/noentry','/nodefaultlib','/safeseh:no','/timestamp:0',
        '/incremental:no','/fixed','/base:'+hex(base),'/def:'+str(definition),'/out:'+str(image),
        str(obj),str(out/'generated/wsock32.res'),str(out/'ntdll.lib')],check=True)
    raw=image.read_bytes();pe=struct.unpack_from('<I',raw,60)[0]
    return {'name':'wsock32.dll','path':'wsock32.dll','base':base,'size':struct.unpack_from('<I',raw,pe+80)[0],
        'sha256':hashlib.sha256(raw).hexdigest(),'version':stubs.image_version(raw)}
