"""Build authored scalar OpenGL 1.1 ABI DLL and fixture, with a SIMD audit."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
from scripts.win32 import stubs
ROOT=Path(__file__).resolve().parents[2]
HERE=ROOT/'sdk/win32/opengl32'
FLAGS=['--target=i686-pc-windows-msvc','-march=pentiumpro','-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-ffp-contract=off','-fno-strict-aliasing','-mno-sse','-mno-sse2','-mno-mmx','-fno-vectorize','-fno-slp-vectorize','-Wall','-Wextra','-Werror','-Wno-unused-function']
def imports(out,dll,names):
 definition=out/(dll+'.def');definition.write_text('LIBRARY '+dll+'.dll\nEXPORTS\n'+''.join(' '+n+'\n' for n in names))
 library=definition.with_suffix('.lib');subprocess.run(['llvm-dlltool','-m','i386','-k','-d',str(definition),'-l',str(library)],check=True);return str(library)
def build(out,base=0x76000000,flags=None):
 out=Path(out);out.mkdir(parents=True,exist_ok=True);objdir=out/'opengl32-objects';objdir.mkdir(exist_ok=True)
 spec=importlib.util.spec_from_file_location('tinygl_recipe',ROOT/'sdk/libgl/build.py');tiny=importlib.util.module_from_spec(spec);spec.loader.exec_module(tiny);tiny.recipe()
 include=['-I'+str(ROOT/'src/kernel/include'),'-I'+str(HERE/'freestanding'),'-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'sdk/libgl/vendor/include')]
 units=list(HERE.glob('*.c'))+[ROOT/'sdk/libgl/software/renderer.c']
 units += [ROOT/'sdk/libgl/vendor/src'/(n+'.c') for n in tiny.SOURCES if n!='msghandling']
 units += [ROOT/'sdk/win32'/n for n in ('kernel32/compiler.c','kernel32/compiler.S','gdi32/compiler.c','gdi32/compiler.S','services/bytes.c','services/stack.S','advapi32/gate.S')]
 objects=[];exports={}
 for i,source in enumerate(units):
  obj=objdir/f'{i}.obj';extra=['-Wno-unused-variable','-Wno-unused-parameter','-Wno-sign-compare','-Wno-implicit-fallthrough','-Wno-misleading-indentation'] if 'vendor' in source.parts else []
  subprocess.run(['clang',*FLAGS,*extra,*include,'-c',str(source),'-o',str(obj)],check=True);objects.append(str(obj))
  if source.parent==HERE:
   nm=subprocess.check_output(['llvm-nm','--defined-only',str(obj)],text=True)
   for name,arity in re.findall(r'\bT _([A-Za-z][A-Za-z0-9_]*)@(\d+)\s*$',nm,re.M):
    if name=='GlDllMain':continue
    public=name[4:] if name.startswith('w32_gl') and name!='w32_gl_swap' else name
    exports[public]=f'_{name}@{arity}'
 libraries=[imports(objdir,'kernel32',['GetModuleHandleA@4','GetProcAddress@8']),imports(objdir,'ntdll',['w32_get_services','w32_strcmp','w32_shim_current']),imports(objdir,'gdi32',['GetPixelFormat@4','w32_gdi_gl_size@12','StretchDIBits@52','CreateDIBSection@24','DeleteObject@4','w32_gdi_gl_glyph@24'])]
 definition=out/'opengl32.def';definition.write_text('LIBRARY opengl32.dll\nEXPORTS\n'+''.join(f' {name}={value}\n' for name,value in sorted(exports.items())))
 rc=objdir/'version.rc';rc.write_text('1 VERSIONINFO\nFILEVERSION 0,0,0,0\nPRODUCTVERSION 0,0,0,0\nFILEFLAGSMASK 0x3f\nFILEFLAGS 0\nFILEOS 0x40004\nFILETYPE 2\nBEGIN\nBLOCK "StringFileInfo"\nBEGIN\nBLOCK "040904E4"\nBEGIN\nVALUE "FileDescription", "CiukiOS software OpenGL (TinyGL)\\0"\nEND\nEND\nEND\n');res=rc.with_suffix('.res');subprocess.run(['llvm-rc','/fo',str(res),str(rc)],check=True)
 image=out/'opengl32.dll';subprocess.run(['lld-link','/machine:x86','/dll','/entry:GlDllMain@12','/nodefaultlib','/safeseh:no','/timestamp:0','/incremental:no','/base:'+hex(base),'/subsystem:windows,4.0','/osversion:4.0','/def:'+str(definition),'/out:'+str(image),*objects,str(res),*libraries],check=True)
 dis=subprocess.check_output(['llvm-objdump','-d','--no-show-raw-insn',str(image)],text=True)
 bad=[line for line in dis.splitlines() if re.search(r'\b(?:xmm|ymm|zmm|mm[0-7])\b',line)]
 if bad:raise ValueError('OpenGL SIMD instruction audit: '+str(bad[:10]))
 raw=image.read_bytes();digest=hashlib.sha256(raw).hexdigest();pe=int.from_bytes(raw[60:64],'little')
 (out/'opengl32-audit.json').write_text(json.dumps({'flags':FLAGS,'simd_instructions':bad,'x87':'existing user-process transform ABI','exports':sorted(exports),'sha256':digest},indent=2)+'\n')
 return {'name':'opengl32.dll','path':'opengl32.dll','base':base,'size':int.from_bytes(raw[pe+80:pe+84],'little'),'sha256':digest,'version':stubs.image_version(raw)}
def build_exercise(out,dlls=None):
 out=Path(out);out.mkdir(parents=True,exist_ok=True);source=ROOT/'src/probes/win32_gl'
 objects=[];table={name:set() for name in ('kernel32','gdi32','user32','opengl32')}
 for name in ('exercise','scenes'):
  obj=out/('gl-'+name+'.obj');subprocess.run(['clang',*FLAGS,'-I'+str(ROOT/'sdk/win32/include'),'-c',str(source/(name+'.c')),'-o',str(obj)],check=True);objects.append(str(obj))
  nm=subprocess.check_output(['llvm-nm','-u',str(obj)],text=True)
  for symbol,arity in re.findall(r'\bU _([A-Za-z][A-Za-z0-9_]*)@(\d+)\s*$',nm,re.M):
   if symbol in ('ExitProcess','VirtualAlloc','Sleep'):dll='kernel32'
   elif symbol in ('ChoosePixelFormat','SetPixelFormat','DescribePixelFormat','SwapBuffers'):dll='gdi32'
   elif symbol.startswith(('gl','wgl')) or symbol=='CiukiGLGetSizes':dll='opengl32'
   else:dll='user32'
   table[dll].add(f'{symbol}@{arity}')
 libraries=[]
 directory=out/'gl-imports';directory.mkdir(exist_ok=True)
 for dll,names in table.items():libraries.append(imports(directory,dll,sorted(names)))
 image=out/'gl-exercise.exe';subprocess.run(['lld-link','/machine:x86','/entry:entry@0','/nodefaultlib','/safeseh:no','/timestamp:0','/incremental:no','/base:0x00400000','/subsystem:windows,4.0','/osversion:4.0','/out:'+str(image),*objects,*libraries],check=True)
 if dlls:
  from scripts.win32 import census
  exports={dll:{name for row in census.inspect((Path(dlls)/(dll+'.dll')).read_bytes())['exports'] for name in row['names']} for dll in table}
  missing=[f"{dll}!{name.split('@')[0]}" for dll,names in table.items() for name in names if name.split('@')[0] not in exports[dll]]
  if missing:raise ValueError('GL fixture unresolved exports: '+str(missing))
 return image

if __name__=='__main__':print(json.dumps(build(ROOT/'build/host/win32/gl/builtin'),indent=2))
