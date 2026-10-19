"""Build original DirectSound/WinMM facades and explicit W21 remainder."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import json
from pathlib import Path
import re
import subprocess
from scripts.win32 import stubs
ROOT=Path(__file__).resolve().parents[2]
# Export inventory only, checked against the pinned dsound.spec. No Wine code.
DS_ORDINALS={'DirectSoundCreate':1,'DirectSoundEnumerateA':2,'DirectSoundEnumerateW':3,
             'DirectSoundCaptureCreate':6,'DirectSoundCaptureEnumerateA':7,
             'DirectSoundCaptureEnumerateW':8,'DirectSoundCreate8':11,'DirectSoundCaptureCreate8':12}

def build(out,base,flags):
    out=Path(out)
    src=ROOT/'sdk/win32/audio'
    imported=stubs.imports((ROOT/'docs/design/research/win32-layer-2026-10-14.md').read_text())
    # Actual i386 import decoration. These are original kernel32 endpoints.
    imports={'GetProcessHeap':0,'HeapAlloc':12,'HeapFree':12,'CreateThread':24,
             'SetThreadPriority':8,'ResumeThread':4,'TerminateThread':8,'CloseHandle':4,
             'LoadLibraryA':4,'GetTickCount':0,'Sleep':4,'GetModuleHandleA':4,'GetProcAddress':8,'SetEvent':4,'PulseEvent':4,
             'InitializeCriticalSection':4,'EnterCriticalSection':4,'LeaveCriticalSection':4,
             'CreateFileA':28,'GetFileSize':8,'ReadFile':20,'SetFilePointer':16,
             'GlobalMemoryStatus':4,'CreateFileMappingA':24,'MapViewOfFile':20,
             'FindResourceA':12,'LoadResource':8,'LockResource':4,'SizeofResource':8}
    definition=out/'audio-kernel32.def'
    definition.write_text('LIBRARY kernel32.dll\nEXPORTS\n'+''.join(f' {n}@{a}\n' for n,a in imports.items()))
    subprocess.run(['llvm-dlltool','-m','i386','-k','-d',str(definition),'-l',str(out/'audio-kernel32.lib')],check=True)
    definition=out/'audio-winmm-imports.def'
    definition.write_text('LIBRARY winmm.dll\nEXPORTS\n CiukiAudioAcquire@8\n CiukiAudioLock@0\n CiukiAudioUnlock@0\n wa_alloc\n wa_free\n wa_private_bytes\n')
    subprocess.run(['llvm-dlltool','-m','i386','-k','-d',str(definition),'-l',str(out/'audio-winmm.lib')],check=True)
    result=[]
    for dll,units in [('winmm.dll',['files','mix','runtime','wave','timers','misc','mmio','playsound','midi','synth']),('dsound.dll',['dsound','mix'])]:
        objects=[];exports={};public=set()
        for unit in units:
            obj=out/f'audio-{unit}.obj'
            subprocess.run(['clang',*flags,'-Wno-missing-field-initializers','-c',str(src/(unit+'.c')),'-o',str(obj)],check=True)
            objects.append(str(obj))
            symbols=subprocess.run(['llvm-nm','--defined-only',str(obj)],check=True,capture_output=True,text=True).stdout
            for name,n in re.findall(r'\bT _([A-Za-z][A-Za-z0-9_]*)@(\d+)\s*$',symbols,re.M):
                if name not in ('AudioDllMain',):exports[name]=f'_{name}@{n}';public.add(name)
            if dll=='winmm.dll':
                for name in ('wa_alloc','wa_free','wa_private_bytes'):
                    if re.search(r'\bT _'+name+r'\s*$',symbols,re.M):exports[name]=name
        if dll=='winmm.dll':
            helper=out/'audio-helpers.c';helper.write_text('int _fltused;\n')
            obj=out/'audio-helpers.obj';subprocess.run(['clang',*flags,'-c',str(helper),'-o',str(obj)],check=True);objects.append(str(obj))
            # MSVC i386 arithmetic helpers already supplied by kernel32's build.
            objects += [str(out/'kernel32-compiler.c.obj'),str(out/'kernel32-compiler.S.obj')]
        obj=out/(dll+'-bytes.obj')
        subprocess.run(['clang',*flags,'-c',str(ROOT/'sdk/win32/services/bytes.c'),'-o',str(obj)],check=True)
        objects.append(str(obj))
        if dll=='dsound.dll':objects.append(str(out/'audio-helpers.obj'))
        missing=sorted(imported.get(dll,set())-public)
        remainder=out/(dll+'-remainder.c')
        remainder.write_text('__declspec(dllimport) void w32_stub_hit(volatile unsigned *,const char *,const char *);\n'+''.join(f'static volatile unsigned hit{i}; void missing{i}(void){{w32_stub_hit(&hit{i},"{dll}","{name}");__builtin_trap();}}\n' for i,name in enumerate(missing)))
        obj=out/(dll+'-remainder.obj');subprocess.run(['clang',*flags,'-c',str(remainder),'-o',str(obj)],check=True);objects.append(str(obj))
        definition=out/(dll+'.def');definition.write_text('LIBRARY '+dll+'\nEXPORTS\n'+''.join(f' {name}={symbol}'+(f' @{DS_ORDINALS[name]}' if dll=='dsound.dll' and name in DS_ORDINALS else '')+'\n' for name,symbol in sorted(exports.items()) if name!='_fltused')+''.join((f' ordinal_{name[1:]}=missing{i} @{name[1:]} NONAME' if name.startswith('#') else f' {name}=missing{i}')+'\n' for i,name in enumerate(missing)))
        image=out/dll
        subprocess.run(['lld-link','/machine:x86','/dll','/entry:AudioDllMain@12' if dll=='winmm.dll' else '/noentry','/nodefaultlib','/safeseh:no','/timestamp:0','/incremental:no','/fixed','/base:'+hex(base),'/def:'+str(definition),'/out:'+str(image),*objects,str(out/'generated'/(dll[:-4]+'.res')),str(out/'ntdll.lib'),str(out/'audio-kernel32.lib'),*([str(out/'audio-winmm.lib')] if dll=='dsound.dll' else [])],check=True)
        raw=image.read_bytes()
        from scripts.win32.census import PE
        size=PE(raw).size
        result.append({'name':dll,'path':dll,'base':base,'size':size,'sha256':hashlib.sha256(raw).hexdigest(),'version':stubs.image_version(raw)})
        (out/(dll+'-coverage.json')).write_text(json.dumps({n:'implemented' if n in public else 'W21 stub' for n in sorted(imported.get(dll,set()))},indent=2)+'\n')
        base+=(size+65535)&~65535
    return result
