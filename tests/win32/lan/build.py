#!/usr/bin/env python3
"""Build authored LAN PEs only; no guest, game payload, or image build."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'build/host/lan2-pe'
SOURCE=ROOT/'build/external/wine-10.0'
INCLUDE=ROOT/'build/win32/upper/objects/include'

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    results={}
    for name in ('ipx','directplay'):
        obj=OUT/(name+'.obj')
        subprocess.run(['clang','--target=i686-pc-windows-msvc','-march=pentiumpro',
            '-ffreestanding','-fno-builtin','-fno-stack-protector','-mno-sse','-mno-sse2','-mno-mmx',
            '-O1','-Wall','-Wextra','-Werror','-Wno-pragma-pack','-D__WINESRC__','-D__WINE_PE_BUILD','-D_MSVCR_VER=0','-D_ACRTIMP=',
            '-I'+str(INCLUDE),'-I'+str(SOURCE/'include'),'-I'+str(SOURCE/'include/msvcrt'),'-I'+str(Path(__file__).parent),
            '-c',str(Path(__file__).with_name(name+'.c')),'-o',str(obj)],check=True)
        undefined=subprocess.check_output(['llvm-nm','-u',obj],text=True)
        imports={}
        for symbol in re.findall(r'\bU (\S+)',undefined):
            match=re.fullmatch(r'(?:__imp_)?_([A-Za-z]\w*)@(\d+)',symbol)
            if not match:raise ValueError('unexpected fixture import '+symbol)
            function,size=match.groups()
            dll='dplayx' if function.startswith('DirectPlay') else 'ws2_32' if function in (
                'WSAStartup','WSACleanup','socket','bind','getsockname','htons','setsockopt','sendto','recvfrom','select','closesocket','__WSAFDIsSet') else 'kernel32'
            imports.setdefault(dll,set()).add(function+'@'+size)
        libraries=[]
        for dll,symbols in sorted(imports.items()):
            definition=OUT/(name+'-'+dll+'.def');library=definition.with_suffix('.lib')
            definition.write_text('LIBRARY '+dll+'.dll\nEXPORTS\n'+''.join(' '+s+'\n' for s in sorted(symbols)))
            subprocess.run(['llvm-dlltool','-m','i386','-k','-d',str(definition),'-l',str(library)],check=True);libraries.append(str(library))
        pe=OUT/('lan2-'+name+'.exe')
        subprocess.run(['lld-link','/machine:x86','/entry:entry@0','/nodefaultlib','/safeseh:no',
            '/timestamp:0','/incremental:no','/base:0x00400000','/subsystem:console,4.0',
            '/osversion:4.0','/out:'+str(pe),str(obj),*libraries],check=True)
        results[pe.name]={'sha256':hashlib.sha256(pe.read_bytes()).hexdigest(),'bytes':pe.stat().st_size,'imports':{k:sorted(v) for k,v in imports.items()}}
    (OUT/'build.json').write_text(json.dumps(results,indent=2,sort_keys=True)+'\n')
    print(json.dumps(results,sort_keys=True))
if __name__=='__main__':main()
