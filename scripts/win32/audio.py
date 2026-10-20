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
             'InitializeCriticalSection':4,'EnterCriticalSection':4,'LeaveCriticalSection':4,'GetCurrentThreadId':0,
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
    # f5-12B: MCI core, string parser and the waveaudio/sequencer devices
    # live in winmm; avivideo is mciavi32.dll (build_mci_avi), loaded on use.
    for dll,units in [('winmm.dll',['files','mix','runtime','wave','timers','misc','mmio','playsound','midi','synth','mci','mcistr','mcitime','mciwave','mciseq']),('dsound.dll',['dsound','mix'])]:
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

# f5-12B: the "avivideo" MCI driver. Exact i386 decorations of the actual
# exports it imports (avifil32/msvfw32/msacm32 from scripts/win32/codecs.py,
# winmm above, user32/gdi32/kernel32 original facades).
MCIAVI_IMPORTS={
 'avifil32.dll':{'AVIFileInit':0,'AVIFileOpenA':16,'AVIFileRelease':4,'AVIFileGetStream':16,
     'AVIStreamRelease':4,'AVIStreamInfoA':12,'AVIStreamReadFormat':16,'AVIStreamRead':28,
     'AVIStreamStart':4,'AVIStreamLength':4,'AVIStreamGetFrameOpen':8,'AVIStreamGetFrame':8,
     'AVIStreamGetFrameClose':4},
 'msvfw32.dll':{'DrawDibOpen':0,'DrawDibClose':4,'DrawDibDraw':52},
 'msacm32.dll':{'acmStreamOpen':32,'acmStreamClose':8,'acmStreamPrepareHeader':12,
     'acmStreamUnprepareHeader':12,'acmStreamConvert':12},
 'winmm.dll':{'waveOutOpen':24,'waveOutClose':4,'waveOutPrepareHeader':12,'waveOutUnprepareHeader':12,
     'waveOutWrite':12,'waveOutReset':4,'waveOutPause':4,'waveOutRestart':4,'waveOutGetPosition':12},
 'user32.dll':{'RegisterClassA':4,'CreateWindowExA':48,'DestroyWindow':4,'ShowWindow':8,'GetDC':4,
     'ReleaseDC':8,'BeginPaint':8,'EndPaint':8,'DefWindowProcA':16,'GetClientRect':8,'GetWindowRect':8,
     'SetWindowTextA':8,'SetWindowPos':28,'IsWindow':4,'IsWindowVisible':4,'GetSystemMetrics':4,
     'AdjustWindowRect':12,'InvalidateRect':12,'PostMessageA':16,'GetWindowThreadProcessId':8},
 'gdi32.dll':{'GetStockObject':4},
 'kernel32.dll':{'GetModuleHandleA':4,'GetTickCount':0,'GetCurrentThreadId':0},
}

def build_mci_avi(out,base,flags):
    out=Path(out);src=ROOT/'sdk/win32/audio'
    libraries=[]
    for dll,entries in sorted(MCIAVI_IMPORTS.items()):
        definition=out/('mciavi32-'+dll[:-4]+'.def')
        definition.write_text('LIBRARY '+dll+'\nEXPORTS\n'+''.join(f' {n}@{a}\n' for n,a in sorted(entries.items())))
        library=out/('mciavi32-'+dll[:-4]+'.lib')
        subprocess.run(['llvm-dlltool','-m','i386','-k','-d',str(definition),'-l',str(library)],check=True)
        libraries.append(str(library))
    objects=[]
    for unit,path in (('mciavi',src/'mciavi.c'),('bytes',ROOT/'sdk/win32/services/bytes.c')):
        obj=out/f'mciavi32-{unit}.obj'
        subprocess.run(['clang',*flags,'-Wno-missing-field-initializers','-c',str(path),'-o',str(obj)],check=True);objects.append(str(obj))
    # MSVC i386 64-bit arithmetic helpers, as winmm: from the kernel32 build.
    objects += [str(out/'kernel32-compiler.c.obj'),str(out/'kernel32-compiler.S.obj')]
    symbols=subprocess.run(['llvm-nm','--defined-only',objects[0]],check=True,capture_output=True,text=True).stdout
    exports={name:f'_{name}@{n}' for name,n in re.findall(r'\bT _([A-Za-z][A-Za-z0-9_]*)@(\d+)\s*$',symbols,re.M)}
    if set(exports)!={'CiukiMciDriver'}:raise ValueError('mciavi32 exports: '+str(sorted(exports)))
    definition=out/'mciavi32.dll.def'
    definition.write_text('LIBRARY mciavi32.dll\nEXPORTS\n'+''.join(f' {n}={d}\n' for n,d in sorted(exports.items())))
    rc=out/'mciavi32-version.rc';rc.write_text('LANGUAGE 9,1\n1 VERSIONINFO\nFILEVERSION 1,0,0,0\nPRODUCTVERSION 1,0,0,0\nFILEFLAGSMASK 0x3f\nFILEFLAGS 0\nFILEOS 0x40004\nFILETYPE 2\nBEGIN\nEND\n')
    res=out/'mciavi32-version.res';subprocess.run(['llvm-rc','/fo',str(res),str(rc)],check=True)
    image=out/'mciavi32.dll'
    subprocess.run(['lld-link','/machine:x86','/dll','/noentry','/nodefaultlib','/safeseh:no','/timestamp:0',
        '/incremental:no','/fixed','/base:'+hex(base),'/subsystem:windows,4.0','/osversion:4.0',
        '/def:'+str(definition),'/out:'+str(image),*objects,str(res),*libraries],check=True)
    raw=image.read_bytes()
    from scripts.win32.census import PE
    size=PE(raw).size
    (out/'mciavi32.dll-coverage.json').write_text(json.dumps({'CiukiMciDriver':'implemented'},indent=2)+'\n')
    return {'name':'mciavi32.dll','path':'mciavi32.dll','base':base,'size':size,
            'sha256':hashlib.sha256(raw).hexdigest(),'version':stubs.image_version(raw)}
