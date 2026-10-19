"""Build the original DirectDraw builtin, census coverage and scalar audit."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import json
from pathlib import Path
import re
import subprocess
from scripts.win32 import stubs, gdi32
from scripts import build_kernel
ROOT=Path(__file__).resolve().parents[2]
SOURCES=('objects.c','device.c','surface.c','blit.c','native.c')
PUBLIC={'DirectDrawCreate','DirectDrawCreateEx','DirectDrawCreateClipper','DirectDrawEnumerateA',
        'DirectDrawEnumerateW','DirectDrawEnumerateExA','DirectDrawEnumerateExW'}

def build(out,base=0x73000000,flags=None):
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    directory=out/'ddraw-objects';directory.mkdir(exist_ok=True)
    core=ROOT/'sdk/win32'
    flags=[*gdi32.FLAGS,'-I'+str(core/'include'),'-I'+str(ROOT/'src/kernel/include')]
    objects=[];exports={}
    sources=[core/'ddraw'/n for n in SOURCES]+[core/'services/bytes.c',core/'services/stack.S',
        core/'kernel32/compiler.c',core/'kernel32/compiler.S',core/'advapi32/gate.S']
    for index,source in enumerate(sources):
        obj=directory/f'{index}.obj'
        subprocess.run(['clang',*flags,'-c',str(source),'-o',str(obj)],check=True)
        objects.append(str(obj))
        if source.parent.name=='ddraw':
            nm=subprocess.run(['llvm-nm','--defined-only',str(obj)],check=True,capture_output=True,text=True).stdout
            for name,arity in re.findall(r'\bT _([A-Za-z][A-Za-z0-9_]*)@(\d+)\s*$',nm,re.M):
                if name in PUBLIC or name.startswith('w32_ddraw_'):exports[name]=f'_{name}@{arity}'
    imports=stubs.imports((ROOT/'docs/design/research/win32-layer-2026-10-14.md').read_text()).get('ddraw.dll',set())
    imports|=stubs.imports((ROOT/'docs/design/win32-census/fixtures.txt').read_text()).get('ddraw.dll',set())
    if imports-PUBLIC:raise ValueError('Unimplemented DirectDraw census imports: '+str(sorted(imports-PUBLIC)))
    libraries=[]
    declarations={'ntdll':['w32_get_services','w32_shim_current','memcpy','memset'],
        'kernel32':['GetModuleHandleA@4','LoadLibraryA@4','GetProcAddress@8','CreateThread@24','WaitForSingleObject@8','CloseHandle@4'],
        'gdi32':['w32_dib_validate','w32_dib_raw','w32_dib_rgb','w32_dib_encode','w32_dib_put','w32_gdi_rop3',
                 'w32_gdi_surface_dc@20','w32_gdi_bind_surface@8','DeleteDC@4','StretchDIBits@52']}
    for dll,names in declarations.items():
        definition=directory/(dll+'.def');definition.write_text('LIBRARY '+dll+'.dll\nEXPORTS\n'+''.join(' '+n+'\n' for n in names))
        lib=directory/(dll+'.lib');subprocess.run(['llvm-dlltool','-m','i386','-k','-d',str(definition),'-l',str(lib)],check=True);libraries.append(str(lib))
    definition=out/'ddraw.def';definition.write_text('LIBRARY ddraw.dll\nEXPORTS\n'+''.join(f' {name}={target}\n' for name,target in sorted(exports.items())))
    rc=directory/'version.rc';rc.write_text('1 VERSIONINFO\nFILEVERSION 0,0,0,0\nPRODUCTVERSION 0,0,0,0\nFILEFLAGSMASK 0x3f\nFILEFLAGS 0\nFILEOS 0x40004\nFILETYPE 2\nBEGIN\nBLOCK "StringFileInfo"\nBEGIN\nBLOCK "040904E4"\nBEGIN\nVALUE "FileDescription", "CiukiOS DirectDraw\\0"\nEND\nEND\nEND\n')
    res=directory/'version.res';subprocess.run(['llvm-rc','/fo',str(res),str(rc)],check=True)
    image=out/'ddraw.dll'
    subprocess.run(['lld-link','/machine:x86','/dll','/entry:DDrawDllMain@12','/nodefaultlib','/safeseh:no','/timestamp:0',
        '/incremental:no','/base:'+hex(base),'/subsystem:windows,4.0','/osversion:4.0','/def:'+str(definition),
        '/out:'+str(image),*objects,str(res),*libraries],check=True)
    dis=subprocess.run(['llvm-objdump','-d','--no-show-raw-insn',str(image)],check=True,capture_output=True,text=True).stdout
    bad=build_kernel.audit_disassembly(dis,{})
    if bad:raise ValueError('DirectDraw scalar instruction audit failed: '+str(bad[:20]))
    raw=image.read_bytes();digest=hashlib.sha256(raw).hexdigest()
    (out/'ddraw-audit.json').write_text(json.dumps({'flags':flags,'fpu_simd_instructions':bad,'sha256':digest},indent=2)+'\n')
    (out/'ddraw-coverage.json').write_text(json.dumps({'schema':1,'scope':'provisional f5-02 census, not installed-game runtime evidence',
        'exports':[{'name':n,'status':'implemented'} for n in sorted(imports)],'native_integration':'USER lease/clip seam pending lead'},indent=2)+'\n')
    pe=int.from_bytes(raw[60:64],'little')
    return {'name':'ddraw.dll','path':'ddraw.dll','base':base,'size':int.from_bytes(raw[pe+80:pe+84],'little'),
            'sha256':digest,'version':stubs.image_version(raw)}

if __name__=='__main__':print(json.dumps(build(ROOT/'build/host/win32/ddraw/builtin'),indent=2))

def build_exercise(out):
    """One authored PE, selected at runtime; no image variant or guest run."""
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    flags=[*gdi32.FLAGS,'-I'+str(ROOT/'sdk/win32/include')]
    obj=out/'ddraw-exercise.obj'
    subprocess.run(['clang',*flags,'-c',str(ROOT/'tests/host/win32/ddraw/exercise.c'),'-o',str(obj)],check=True)
    libraries=[]
    for dll,names in {'ddraw':['DirectDrawCreate@12','DirectDrawCreateClipper@12'],
                      'gdi32':['SetPixel@16'],
                      'user32':['RegisterClassA@4','CreateWindowExA@48','DefWindowProcA@16','DestroyWindow@4','PeekMessageA@20','DispatchMessageA@4'],
                      'kernel32':['Sleep@4','ExitProcess@4']}.items():
        definition=out/('exercise-'+dll+'.def');definition.write_text('LIBRARY '+dll+'.dll\nEXPORTS\n'+''.join(' '+n+'\n' for n in names))
        library=definition.with_suffix('.lib');subprocess.run(['llvm-dlltool','-m','i386','-k','-d',str(definition),'-l',str(library)],check=True);libraries.append(str(library))
    image=out/'ddraw-exercise.exe'
    subprocess.run(['lld-link','/machine:x86','/entry:entry@0','/nodefaultlib','/safeseh:no','/timestamp:0',
        '/incremental:no','/base:0x00400000','/subsystem:windows,4.0','/osversion:4.0','/out:'+str(image),str(obj),*libraries],check=True)
    return image
