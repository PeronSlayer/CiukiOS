#!/usr/bin/env python3
"""Generate deterministic, named, nonreturning i386 PE stubs from import metadata.

Accepts census.py text, DLL!symbol lists, and the checked-in research union.
No executable input is read. Unknown conventions never guess stack cleanup.
Only explicit ordinal assignments are compatibility claims; winmm #2 is opaque.
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright 2026 CiukiOS contributors.
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
if __package__ in (None,''): sys.path.insert(0,str(ROOT))
IDENT=re.compile(r'[A-Za-z_?$@][A-Za-z0-9_?$@]*\Z')
DLL=re.compile(r'[A-Za-z0-9_-]+\.(?:dll|drv|acm)\Z',re.I)
# Explicit public assignments checked against upstream Wine specs on 2026-10-14.
ORDINALS={
 'wsock32.dll':dict(zip(('accept','bind','closesocket','connect','getpeername','getsockname','getsockopt','htonl','htons','inet_addr','inet_ntoa','ioctlsocket','listen','ntohl','ntohs','recv','recvfrom','select','send','sendto','setsockopt','shutdown','socket'),range(1,24))) | {
     'gethostbyaddr':51,'gethostbyname':52,'getprotobyname':53,'getprotobynumber':54,
     'getservbyname':55,'getservbyport':56,'gethostname':57,'WSAGetLastError':111,
     'WSASetLastError':112,'WSAStartup':115,'WSACleanup':116},
 'comctl32.dll':{'InitCommonControls':17},
 'dplayx.dll':{'DirectPlayCreate':1,'DirectPlayEnumerateA':2,'DirectPlayLobbyCreateA':4},
}

def imports(text):
    result={}
    def add(dll,symbol):
        dll=dll.lower()
        if not dll.endswith(('.dll','.drv')): dll+='.dll'
        if not DLL.fullmatch(dll): raise ValueError('invalid DLL name: '+dll)
        if symbol.startswith('#'):
            if not symbol[1:].isdigit() or not 1<=int(symbol[1:])<=65535: raise ValueError('invalid ordinal: '+symbol)
        elif not IDENT.fullmatch(symbol): raise ValueError('invalid export name: '+symbol)
        result.setdefault(dll,set()).add(symbol)
    for line in text.splitlines():
        m=re.match(r'(?:STATIC|DELAY)\s+(\S+)\s+->\s+(.*)',line)
        if m:
            for s in m[2].split(','):
                s=s.strip()
                if s and not s.startswith('<'): add(m[1],s)
        m=re.match(r'- \*\*([\w.-]+\.(?:dll|drv))\*\* \(\d+\): (.*)',line)
        if m:
            for s in re.findall(r'`([^`]+)`',m[2]): add(m[1],s)
        # Plain metadata lists and the research's backtick-only common set;
        # prose examples such as "DLL!function" are not census evidence.
        if line.lstrip().startswith('`') or re.fullmatch(r'[\w.!#$?@\s,]+',line):
            for dll,symbol in re.findall(r'([\w.-]+)!([\w?$@]+|#\d+)',line): add(dll,symbol)
    return result

def generate(inputs,out,implemented=None):
    from scripts.win32 import stub_signatures
    all_imports={}
    for path in inputs:
        for dll,names in imports(Path(path).read_text()).items(): all_imports.setdefault(dll,set()).update(names)
    implemented=implemented or {}
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    manifest={'schema':1,'license':'LGPL-2.1-or-later','stub_behavior':'diagnose-once; typed failure; no-defined-failure or unknown ABI raises STATUS_NOT_IMPLEMENTED','dlls':[]}
    for dll,names in sorted(all_imports.items()):
        stem=dll.rsplit('.',1)[0]
        assignments=ORDINALS.get(dll,{})
        # Collapse known ordinal/name aliases into one exported function.
        by_ordinal={v:k for k,v in assignments.items()}
        normalized=set(by_ordinal.get(int(n[1:]),n) if n.startswith('#') else n for n in names)
        entries=[];c=['/* SPDX-License-Identifier: LGPL-2.1-or-later */',
          '/* Copyright 2026 CiukiOS contributors. Generated; do not edit. */',
          stub_signatures.DECLARATIONS]
        definition=['; SPDX-License-Identifier: LGPL-2.1-or-later','LIBRARY '+dll,'EXPORTS']
        for i,name in enumerate(sorted(normalized)):
            ordinal=int(name[1:]) if name.startswith('#') else assignments.get(name)
            symbol=f'w32_stub_{i:04d}'
            implementation=implemented.get(dll,{}).get(name)
            audit=None
            if implementation:
                # Use a PE forwarder to an implementation; do not generate a
                # competing placeholder for an already implemented export.
                if not re.fullmatch(r'[\w-]+\.[\w?$@]+',implementation): raise ValueError('invalid implementation forwarder')
                target=implementation
            else:
                source,target,audit=stub_signatures.body(dll,name,symbol,i)
                c.append(source)
            export=name if not name.startswith('#') else f'W32_Ordinal_{ordinal}'
            line=f'    {export}={target}'
            if ordinal is not None: line+=f' @{ordinal}'
            if name.startswith('#'): line+=' NONAME'
            definition.append(line)
            entries.append({'name':None if name.startswith('#') else name,'ordinal':ordinal,
                            'implementation':implementation,'diagnostic':name,'audit':audit})
        (out/(stem+'.c')).write_text('\n'.join(c)+'\n')
        (out/(stem+'.def')).write_text('\n'.join(definition)+'\n')
        (out/(stem+'.rc')).write_text(
            '// SPDX-License-Identifier: LGPL-2.1-or-later\n'
            'LANGUAGE 9, 1\n1 VERSIONINFO\nFILEVERSION 0,0,0,0\nPRODUCTVERSION 0,0,0,0\n'
            'FILEFLAGSMASK 0x3f\nFILEFLAGS 0\nFILEOS 0x40004\nFILETYPE 2\nFILESUBTYPE 0\n'
            'BEGIN\n BLOCK "StringFileInfo"\n BEGIN\n  BLOCK "040904E4"\n  BEGIN\n'
            f'   VALUE "FileDescription", "CiukiOS placeholder {dll}\\0"\n'
            '   VALUE "FileVersion", "0.0.0.0\\0"\n   VALUE "ProductName", "CiukiOS\\0"\n'
            '   VALUE "ProductVersion", "0.0.0.0\\0"\n  END\n END\n'
            ' BLOCK "VarFileInfo"\n BEGIN\n  VALUE "Translation", 0x409, 1252\n END\nEND\n')
        manifest['dlls'].append({'name':dll,'exports':entries})
    (out/'stubs.json').write_text(json.dumps(manifest,sort_keys=True,indent=2)+'\n')
    return manifest

def build(out,ntdll_import_library,start=0x70100000):
    out=Path(out);description=json.loads((out/'stubs.json').read_text());store={'schema':1,'dlls':[]}
    for dll in description['dlls']:
        name=dll['name'];stem=name.rsplit('.',1)[0];obj=out/(stem+'.obj');image=out/name
        subprocess.run(['clang','--target=i686-pc-windows-msvc','-march=pentiumpro','-ffreestanding',
                        '-fno-stack-protector','-mno-sse','-mno-sse2','-O1','-c',str(out/(stem+'.c')),'-o',str(obj)],check=True,timeout=60)
        resource=out/(stem+'.res')
        subprocess.run(['llvm-rc','/fo',str(resource),str(out/(stem+'.rc'))],check=True,timeout=60)
        subprocess.run(['lld-link','/machine:x86','/dll','/noentry','/nodefaultlib','/safeseh:no','/timestamp:0',
                        '/incremental:no','/fixed','/base:'+hex(start),'/def:'+str(out/(stem+'.def')),
                        '/out:'+str(image),str(obj),str(resource),str(ntdll_import_library)],check=True,timeout=60)
        raw=image.read_bytes();pe=int.from_bytes(raw[60:64],'little');size=int.from_bytes(raw[pe+24+56:pe+24+60],'little')
        if start+size>0x7ffe0000: raise ValueError('builtin region exhausted')
        store['dlls'].append({'name':name,'path':name,'base':start,'size':size,'sha256':hashlib.sha256(raw).hexdigest(),
                              'version':[0,0,0,0]})
        start+=(size+65535)&~65535
    (out/'builtin-store.json').write_text(json.dumps(store,sort_keys=True,indent=2)+'\n')
    return store

def image_version(raw):
    """Read the fixed FILEVERSION through the bounded PE resource tree."""
    from scripts.win32 import census
    pe=census.PE(raw)
    pe.census()  # Check all tables too, not only the certificate/header prefix.
    rva,size=pe.dirs[2]
    def relative(off,n):
        if off<0 or off+n>size: raise ValueError('builtin version resource bounds')
        return pe.read(rva+off,n)
    def entry(off,key,directory):
        h=relative(off,16);count=int.from_bytes(h[12:14],'little')+int.from_bytes(h[14:16],'little')
        for i in range(count):
            e=relative(off+16+i*8,8)
            if int.from_bytes(e[:4],'little')==key:
                value=int.from_bytes(e[4:],'little')
                if bool(value&0x80000000)!=directory: raise ValueError('builtin version tree')
                return value&0x7fffffff
        raise ValueError('builtin version resource missing')
    t=entry(0,16,True);name=entry(t,1,True);leaf=entry(name,1033,False)
    data=relative(leaf,16);blob=pe.read(int.from_bytes(data[:4],'little'),int.from_bytes(data[4:8],'little'))
    key='VS_VERSION_INFO'.encode('utf-16le')+b'\0\0';offset=(6+len(key)+3)&~3
    if len(blob)<offset+52 or blob[6:6+len(key)]!=key or int.from_bytes(blob[:2],'little')>len(blob) or int.from_bytes(blob[2:4],'little')!=52 or int.from_bytes(blob[4:6],'little')!=0 or int.from_bytes(blob[offset:offset+4],'little')!=0xfeef04bd:
        raise ValueError('builtin fixed version')
    ms=int.from_bytes(blob[offset+8:offset+12],'little');ls=int.from_bytes(blob[offset+12:offset+16],'little')
    return [ms>>16,ms&65535,ls>>16,ls&65535]

def verify_store(path):
    path=Path(path);data=json.loads(path.read_text());ranges=[];names=set()
    from scripts.win32 import upper_dlls
    upper_dlls.reject_unqualified_wine(path.parent,data)
    for dll in data['dlls']:
        name=dll['name'].lower();base=dll['base'];size=dll['size']
        if dll['path']!=dll['name']: raise ValueError('builtin path')
        image=path.parent/dll['path']
        if name in names or not DLL.fullmatch(name): raise ValueError('builtin names')
        if base&65535 or not 0x70000000<=base<base+size<=0x7ffe0000: raise ValueError('builtin range')
        if any(base<b+n and b<base+size for b,n in ranges): raise ValueError('builtin overlap')
        if len(dll['version'])!=4 or any(type(v)!=int or not 0<=v<=65535 for v in dll['version']): raise ValueError('builtin version')
        if not image.resolve().is_relative_to(path.parent.resolve()): raise ValueError('builtin path')
        raw=image.read_bytes()
        if hashlib.sha256(raw).hexdigest()!=dll['sha256']: raise ValueError('builtin SHA256')
        if len(raw)<64 or raw[:2]!=b'MZ': raise ValueError('builtin PE')
        p=int.from_bytes(raw[60:64],'little')
        if p+84>len(raw) or raw[p:p+4]!=b'PE\0\0' or int.from_bytes(raw[p+52:p+56],'little')!=base or int.from_bytes(raw[p+80:p+84],'little')!=size: raise ValueError('builtin image metadata')
        if image_version(raw)!=dll['version']: raise ValueError('builtin version mismatch')
        names.add(name);ranges.append((base,size))
    if (path.parent/'codecs-inventory.json').exists() or any(d.get('origin')=='CiukiOS bounded P5 implementation' for d in data['dlls']):
        from scripts.win32 import codecs
        codecs.verify_sources()
        expected={m if m.endswith('.acm') else m+'.dll' for m in codecs.MODULES}
        records={d['name']:d for d in data['dlls'] if d.get('origin')=='CiukiOS bounded P5 implementation'}
        if set(records)!=expected: raise ValueError('P5 builtin inventory')
        inventory=json.loads((path.parent/'codecs-inventory.json').read_text())
        if inventory['dlls']!=list(records.values()): raise ValueError('P5 image inventory mismatch')
        for dll in records.values():
            if dll.get('license')!='LGPL-2.1-or-later' or dll.get('wine_reused') is not False: raise ValueError('P5 license/origin')
            if dll.get('interface_sha256')!=codecs.sha(ROOT/'third_party/wine/codecs-sources.json'): raise ValueError('P5 interface pin')
            required={'LICENSE','COPYING.LIB','AUTHORS','SOURCE-OFFER.txt','PATCHES.md'}
            if set(dll.get('notices',{}))!=required: raise ValueError('P5 notices')
            for notice,digest in dll['notices'].items():
                if codecs.sha(ROOT/'third_party/wine'/notice)!=digest: raise ValueError('P5 notice hash')
        audit=codecs.audit_imports(path.parent,list(records.values()))
        if any(e['missing'] for entries in audit.values() for e in entries): raise ValueError('P5 unresolved imports')
    upper_dlls.verify_builtins(path.parent,data)
    return len(names)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('census',type=Path,nargs='*')
    ap.add_argument('--output',type=Path,default=ROOT/'build/host/win32/stubs')
    ap.add_argument('--implemented',type=Path,help='JSON DLL -> export -> PE forwarder')
    ap.add_argument('--build',type=Path,metavar='NTDLL_LIB')
    ap.add_argument('--verify-store',type=Path)
    args=ap.parse_args()
    if args.verify_store: print('Verified',verify_store(args.verify_store),'builtins');return
    if not args.census: ap.error('at least one census text is required')
    manifest=generate(args.census,args.output,json.loads(args.implemented.read_text()) if args.implemented else None)
    if args.build: build(args.output,args.build)
    print('Generated',sum(len(d['exports']) for d in manifest['dlls']),'exports in',len(manifest['dlls']),'DLLs')

if __name__=='__main__': main()
