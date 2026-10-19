"""Build original DirectInput builtins; dinput8 forwards to one shared engine."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from scripts.win32 import stubs
ROOT=Path(__file__).resolve().parents[2]

def build(out,base,flags):
    out=Path(out);directory=out/'input-objects';directory.mkdir(parents=True,exist_ok=True)
    imports={
        'kernel32':{'GetProcessHeap':0,'HeapAlloc':12,'HeapFree':12,'SetEvent':4,'GetCurrentProcessId':0,'SetLastError':4},
        'user32':{'CiukiUserInputLock':0,'CiukiUserInputUnlock':0,'CiukiUserInputPump':0,
                  'CiukiUserInputSubscribe':12,'CiukiUserInputSnapshot':4,'CiukiUserInputPointer':8,
                  'IsWindow':4,'GetWindowLongA':8,'GetWindowThreadProcessId':8,'GetKeyNameTextA':12}}
    libraries=[]
    for dll,names in imports.items():
        definition=directory/(dll+'.def')
        definition.write_text('LIBRARY '+dll+'.dll\nEXPORTS\n'+''.join(f' {name}@{arity}\n' for name,arity in names.items()))
        library=directory/(dll+'.lib')
        subprocess.run(['llvm-dlltool','-m','i386','-k','-d',str(definition),'-l',str(library)],check=True)
        libraries.append(str(library))
    objects=[]
    for source in [ROOT/'sdk/win32/input/directinput.c',ROOT/'sdk/win32/input/formats.c',ROOT/'sdk/win32/services/bytes.c']:
        obj=directory/(source.stem+'.obj')
        subprocess.run(['clang',*flags,'-c',str(source),'-o',str(obj)],check=True);objects.append(str(obj))
    exports={'DirectInputCreateA':16,'DirectInputCreateW':16,'DirectInputCreateEx':20,'DirectInput8Create':20,
             'DllCanUnloadNow':0,'DllGetClassObject':12,'DllRegisterServer':0,'DllUnregisterServer':0}
    source_census={}
    for path in ['docs/design/research/win32-layer-2026-10-14.md','docs/design/win32-census/fixtures.txt']:
        for dll,names in stubs.imports((ROOT/path).read_text()).items():source_census.setdefault(dll,set()).update(names)
    result=[]
    for dll in ('dinput','dinput8'):
        definition=directory/(dll+'.def')
        if dll=='dinput':
            lines=[f' {name}=_{name}@{arity}' for name,arity in exports.items()]
            lines += [f' {name} DATA' for name in ('c_dfDIKeyboard','c_dfDIMouse','c_dfDIMouse2')]
        else:
            lines=[f' {name}=dinput.{name}' for name in ('DirectInput8Create','DllCanUnloadNow','DllGetClassObject','DllRegisterServer','DllUnregisterServer')]
        definition.write_text('LIBRARY '+dll+'.dll\nEXPORTS\n'+'\n'.join(lines)+'\n')
        rc=directory/(dll+'.rc')
        rc.write_text((out/'generated/dsound.rc').read_text().replace('placeholder dsound.dll',dll+' system input'))
        resource=directory/(dll+'.res')
        subprocess.run(['llvm-rc','/fo',str(resource),str(directory/(dll+'.rc'))],check=True)
        image=out/(dll+'.dll')
        linked=objects+[str(out/'kernel32-compiler.c.obj'),str(out/'kernel32-compiler.S.obj')]+libraries+[str(out/'ntdll.lib')] if dll=='dinput' else []
        subprocess.run(['lld-link','/machine:x86','/dll','/noentry','/nodefaultlib','/safeseh:no',
            '/fixed','/base:'+hex(base),'/timestamp:0','/incremental:no','/subsystem:windows,4.0',
            '/def:'+str(definition),'/out:'+str(image),*linked,str(resource)],check=True)
        names=source_census.get(dll+'.dll',set()) | (set(exports) if dll=='dinput' else {'DirectInput8Create','DllCanUnloadNow','DllGetClassObject','DllRegisterServer','DllUnregisterServer'})
        available=exports.keys() if dll=='dinput' else {'DirectInput8Create','DllCanUnloadNow','DllGetClassObject','DllRegisterServer','DllUnregisterServer'}
        if names-set(available):raise ValueError('unaccounted input census: '+repr(names-set(available)))
        (out/(dll+'-coverage.json')).write_text(json.dumps({'schema':1,
            'scope':'provisional research union and authored fixtures; original media qualification pending',
            'exports':[{'name':n,'status':'W21 stub' if n=='DllGetClassObject' else 'implemented'} for n in sorted(names)]},indent=2)+'\n')
        raw=image.read_bytes();pe=struct.unpack_from('<I',raw,60)[0];size=struct.unpack_from('<I',raw,pe+80)[0]
        result.append({'name':dll+'.dll','path':dll+'.dll','base':base,'size':size,
            'sha256':hashlib.sha256(raw).hexdigest(),'version':stubs.image_version(raw)})
        base+=(size+65535)&~65535
    return result
