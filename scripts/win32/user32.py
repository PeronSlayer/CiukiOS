"""Build original USER core, typed W21 remainders and per-export accounting."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
from scripts.win32 import stubs
ROOT = Path(__file__).resolve().parents[2]

# These APIs use zero as success (or as a valid menu result). Their W21
# failure must retain the published return convention as well as error 120.
MINUS_ONE_FAILURES = {
    'GetMessageW', 'VkKeyScanW', 'VkKeyScanExW', 'DialogBoxParamA', 'DialogBoxIndirectParamA',
    'ChangeDisplaySettingsA', 'ChangeDisplaySettingsW', 'WaitForInputIdle', 'GetMenuState',
    'GetMenuItemCount', 'GetMenuItemID', 'CheckMenuItem', 'EnableMenuItem',
}

# f5-20B: wsprintfA/W are cdecl (WINAPIV) varargs exports.
CDECL_EXPORTS = ('wsprintfA', 'wsprintfW')
UPPER = ROOT/'config/wine-upper.json'

def upper_decisions(dll='user32.dll'):
    """core-implement/core-w21-stub decisions of the reused Wine DLLs owned by `dll`."""
    rows = {}
    for e in json.loads(UPPER.read_text())['decisions']:
        # The f5-14B decision table (f5-20C added audit 'f5-20b'/'f5-20c' rows).
        if e.get('audit', 'f5-14b') != 'f5-14b':
            continue
        if e['owner'] == dll and e['action'] in ('core-implement', 'core-w21-stub'):
            row = rows.setdefault(e['name'], {'name': e['name'], 'action': e['action'], 'rule': e['rule'],
                                              'importers': set(), 'symbols': set(), 'status': set()})
            row['importers'].add(e['importer'])
            row['symbols'].add(e['symbol'])
            row['status'].add(e.get('f5_20b', 'undecided'))
    return [dict(r, importers=sorted(r['importers']), symbols=sorted(r['symbols']),
                 status=sorted(r['status'])[0]) for _, r in sorted(rows.items())]

def wide_gate(objects,exports,objdir,dll,minus_one,flags):
    """f5-20C (directive f5-20 amendment 2 item 7): the per-application w-stubs
    shim gates every exported stdcall Unicode function, as in KERNEL32/ADVAPI32.
    The implementation is renamed to w32_real_* in every object, so calls inside
    the DLL stay ungated, and the exported name becomes the shim gate."""
    from scripts.win32 import stub_signatures
    wide=sorted((n,int(v.rsplit('@',1)[1])) for n,v in exports.items() if n.endswith('W') and '@' in v)
    renames=[f'--redefine-sym=_{n}@{b}=_w32_real_{n}@{b}' for n,b in wide]
    for obj in objects:
        subprocess.run(['llvm-objcopy',*renames,str(obj)],check=True)
    stem=dll.removesuffix('.dll')
    source=Path(objdir)/(stem+'-wide.c')
    source.write_text(stub_signatures.DECLARATIONS+'\n__declspec(dllimport) int w32_shim_w_stub(const char *);\n'+
        '\n'.join(stub_signatures.wide_gate(dll,n,b,i,4 if n in minus_one else None) for i,(n,b) in enumerate(wide))+'\n')
    obj=Path(objdir)/(stem+'-wide.obj')
    subprocess.run(['clang',*flags,'-c',str(source),'-o',str(obj)],check=True)
    objects.append(str(obj))
    return [n for n,_ in wide]

def failure_return(name):
    if name in MINUS_ONE_FAILURES:
        return '(uintptr_t)-1'
    if name == 'DdeInitializeA':
        return '0x400fu'  # DMLERR_SYS_ERROR, never DMLERR_NO_ERROR.
    return '0'

def icon_header(out):
    spec = importlib.util.spec_from_file_location('u32_portrait', ROOT/'apps/desktop/convert_portrait.py')
    converter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(converter)
    source = (ROOT/'assets/brand/ciuki-logo.png').read_bytes()
    if hashlib.sha256(source).hexdigest() != converter.SOURCE_SHA256:
        raise ValueError('approved Ciuki portrait changed')
    width, height, pixels = converter.decode(source)
    values = []
    for y in range(32):
        for x in range(32):
            at = ((y*height//32)*width + x*width//32)*4
            r,g,b,a = pixels[at:at+4]
            values.append(a<<24 | r<<16 | g<<8 | b)
    (out/'ciuki-icon32.h').write_text('/* Exact approved portrait; nearest-neighbour format conversion. */\n'
        'static const unsigned int ciuki_icon32[1024] = {\n'+
        ',\n'.join(','.join(hex(v)+'u' for v in values[i:i+8]) for i in range(0,1024,8))+'\n};\n')

    spec=importlib.util.spec_from_file_location('u32_tango',ROOT/'apps/desktop/convert_icons.py')
    tango=importlib.util.module_from_spec(spec);spec.loader.exec_module(tango)
    import tarfile
    if hashlib.sha256(tango.ARCHIVE.read_bytes()).hexdigest()!=tango.ARCHIVE_SHA:
        raise ValueError('Tango archive changed')
    with tarfile.open(tango.ARCHIVE) as archive, (out/'ciuki-icon32.h').open('a') as header:
        for key,member in {'error':'status/dialog-error','information':'status/dialog-information',
                           'warning':'status/dialog-warning','question':'apps/help-browser'}.items():
            w,h,pixels=tango.png_rgba(archive.extractfile('tango-icon-theme-0.8.90/32x32/'+member+'.png').read())
            if (w,h)!=(32,32):raise ValueError('Tango size')
            values=[pixels[i+3]<<24|pixels[i]<<16|pixels[i+1]<<8|pixels[i+2] for i in range(0,len(pixels),4)]
            header.write('static const unsigned int tango_'+key+'[1024]={'+','.join(hex(v)+'u' for v in values)+'};\n')

def inventory():
    result = {}
    for line in (ROOT/'tests/host/win32/wine_specs/dlls/user32/user32.spec').read_text().splitlines():
        m = re.match(r'\S+\s+(stdcall|cdecl|varargs)\s+(?:-\S+\s+)*([\w]+)\(([^)]*)\)',line)
        if m:
            result[m[2]] = (m[1], len(m[3].split())*4)
    return result

def build(out, base, flags):
    out=Path(out);core=ROOT/'sdk/win32';objdir=out/'user32-objects';objdir.mkdir(parents=True,exist_ok=True)
    icon_header(objdir)
    flags=[*flags,'-I'+str(core/'user32/compat'),'-I'+str(objdir)]
    sources=sorted((core/'user32').glob('*.c'))+[
        ROOT/'apps/libdeskui/v3.c',ROOT/'apps/desktop/protocol.c',ROOT/'apps/desktop/keymap.c',
        core/'services/bytes.c',core/'services/stack.S',core/'advapi32/rpc.c',core/'advapi32/gate.S']
    objects=[];exports={}
    for i,source in enumerate(sources):
        obj=objdir/f'{i:02d}-{source.stem}.obj'
        subprocess.run(['clang',*flags,'-c',str(source),'-o',str(obj)],check=True)
        objects.append(str(obj))
        nm=subprocess.run(['llvm-nm','--defined-only',str(obj)],capture_output=True,text=True,check=True).stdout
        for name,arity in re.findall(r'\bT _([A-Za-z][A-Za-z0-9_]*)@(\d+)\s*$',nm,re.M):
            exports[name]=f'_{name}@{arity}'
        for name in CDECL_EXPORTS:
            if re.search(r'\bT _'+name+r'\s*$',nm,re.M):
                exports[name]=name
    wide=wide_gate(objects,exports,objdir,'user32.dll',MINUS_ONE_FAILURES,flags)
    names=stubs.imports((ROOT/'docs/design/research/win32-layer-2026-10-14.md').read_text()).get('user32.dll',set())
    names |= stubs.imports((ROOT/'docs/design/win32-census/fixtures.txt').read_text()).get('user32.dll',set())
    names |= stubs.acceptance_imports('user32.dll')  # f5-15B
    specs=inventory()
    unicode={n for n in specs if n.endswith('W') and n[:-1]+'A' in exports and n not in exports}
    missing=sorted((names|unicode)-exports.keys())
    lines=['/* SPDX-License-Identifier: LGPL-2.1-or-later */','#include <w32/kernel32.h>']
    definitions=['LIBRARY user32.dll','EXPORTS']+[f' {n}={v}' for n,v in sorted(exports.items())]
    for i,name in enumerate(missing):
        if name not in specs:raise ValueError('USER export lacks known ABI: '+name)
        convention,arity=specs[name]
        params=','.join(f'uintptr_t a{j}' for j in range(arity//4)) or 'void'
        unused=''.join(f'(void)a{j};' for j in range(arity//4))
        api='W32API ' if convention=='stdcall' else ''
        lines.append(f'uintptr_t {api}u32_stub_{i}({params}) {{ {unused} static unsigned once; '
            f'if(!__atomic_exchange_n(&once,1,__ATOMIC_RELAXED))w32_diag("stub","user32.dll","{name}"); '
            f'SetLastError(120);return {failure_return(name)};}}')
        definitions.append(f' {name}='+ (f'_u32_stub_{i}@{arity}' if convention=='stdcall' else f'u32_stub_{i}'))
    stub=objdir/'remainder.c';stub.write_text('\n'.join(lines)+'\n');obj=objdir/'remainder.obj'
    subprocess.run(['clang',*flags,'-c',str(stub),'-o',str(obj)],check=True);objects.append(str(obj))
    definitions += [' CiukiUserSetGdi=u32_set_gdi']
    definition=out/'user32.def';definition.write_text('\n'.join(definitions)+'\n')
    # Import only public kernel32 exports; ntdll imports use the existing library.
    kernel_imports={'GetLastError':0,'SetLastError':4,'GetCurrentThreadId':0,'GetCurrentProcessId':0,
        'GetTickCount':0,'GetProcessHeap':0,'HeapAlloc':12,'HeapFree':12,
        'GlobalAlloc':8,'GlobalFree':4,'GlobalLock':4,'GlobalUnlock':4,'GlobalSize':4,
        'MultiByteToWideChar':24,'WideCharToMultiByte':32,
        'CreateFileA':28,'ReadFile':20,'WriteFile':20,'FlushFileBuffers':4,'DeleteFileA':4,'GetFileSize':8,'CloseHandle':4,'FindResourceA':12,'LoadResource':8,'SizeofResource':8,'GetProcAddress':8,'GetModuleHandleA':4,'LoadLibraryA':4}
    imp=objdir/'kernel32.def';imp.write_text('LIBRARY kernel32.dll\nEXPORTS\n'+''.join(f' {n}@{a}\n' for n,a in kernel_imports.items()))
    lib=objdir/'kernel32.lib';subprocess.run(['llvm-dlltool','-m','i386','-k','-d',str(imp),'-l',str(lib)],check=True)
    rc=objdir/'version.rc';rc.write_text((out/'generated/user32.rc').read_text().replace('CiukiOS placeholder user32.dll','CiukiOS USER core'))
    res=objdir/'version.res';subprocess.run(['llvm-rc','/fo',str(res),str(rc)],check=True)
    image=out/'user32.dll'
    subprocess.run(['lld-link','/machine:x86','/dll','/entry:UserDllMain@12','/nodefaultlib','/safeseh:no','/timestamp:0',
        '/incremental:no','/fixed','/base:'+hex(base),'/def:'+str(definition),'/out:'+str(image),
        *objects,str(res),str(lib),str(out/'ntdll.lib')],check=True)
    coverage=[{'name':name,'status':'implemented' if name in exports else 'W21 stub'} for name in sorted(names)]
    upper=[]
    for row in upper_decisions():
        if row['name'] not in exports:
            raise ValueError('user32 lacks the decided reused-DLL export '+row['name'])
        upper.append(row)
    (out/'user32-coverage.json').write_text(json.dumps({'schema':1,
        'scope':'provisional research union plus authored fixtures; original acceptance media absent',
        'exports':coverage,'unicode_stubs':sorted(unicode),'wide_gate':wide,
        'upper_scope':'f5-20B: core-implement and core-w21-stub decisions of config/wine-upper.json owned by user32.dll',
        'upper':upper},indent=2)+'\n')
    raw=image.read_bytes();pe=struct.unpack_from('<I',raw,60)[0]
    return {'name':'user32.dll','path':'user32.dll','base':base,'size':struct.unpack_from('<I',raw,pe+80)[0],
        'sha256':hashlib.sha256(raw).hexdigest(),'version':stubs.image_version(raw)}
