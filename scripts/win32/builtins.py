#!/usr/bin/env python3
"""Build the freestanding CiukiOS core, authored DLLs and census placeholders."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path: sys.path.insert(0,str(ROOT))
from scripts.win32 import stubs, kernel32, gdi32, user32, ddraw

def build(out=None):
    out=Path(out or ROOT/'build/apps/win32');out.mkdir(parents=True,exist_ok=True)
    core=ROOT/'sdk/win32'
    flags=['--target=i686-pc-windows-msvc','-march=pentiumpro','-ffreestanding','-fno-builtin',
           '-fno-stack-protector','-fno-omit-frame-pointer','-mno-sse','-mno-sse2','-mno-mmx',
           '-O1','-Wall','-Wextra','-Werror','-I'+str(core/'include'),'-I'+str(ROOT/'src/kernel/include')]
    objects=[]
    sources=sorted((core/'ldr').glob('*.c'))+sorted((core/'ntdll').glob('*.c'))+[
        core/'ntdll/context.S',core/'ntdll/runtime.S',core/'services/ciuki.c',core/'services/start.c']
    for source in sources:
        obj=out/(source.parent.name+'-'+source.stem+'.obj')
        subprocess.run(['clang',*flags,'-c',str(source),'-o',str(obj)],check=True);objects.append(str(obj))
    subprocess.run(['llvm-rc','/fo',str(out/'version.res'),str(core/'ntdll/version.rc')],check=True)
    subprocess.run(['lld-link','/machine:x86','/dll','/entry:CiukiDllMain@12','/nodefaultlib','/safeseh:no',
        '/base:0x70000000','/subsystem:windows,4.0','/osversion:4.0','/timestamp:0','/incremental:no',
        '/def:'+str(core/'ntdll/ntdll.def'),'/export:CiukiProcessStart=_CiukiProcessStart@4',
        '/out:'+str(out/'ntdll.dll'),*objects,str(out/'version.res')],check=True)
    raw=(out/'ntdll.dll').read_bytes();pe=struct.unpack_from('<I',raw,60)[0]
    record={'name':'ntdll.dll','path':'ntdll.dll','base':0x70000000,
        'size':struct.unpack_from('<I',raw,pe+80)[0],'sha256':hashlib.sha256(raw).hexdigest(),
        'version':stubs.image_version(raw)}
    # Keep core implementations authoritative if the census later adds ntdll.
    generated=out/'generated'
    from scripts.win32 import upper_dlls
    research=ROOT/'docs/design/research/win32-layer-2026-10-14.md'
    extra={dll:set(names) for dll,names in upper_dlls.census_stub_amendments().items()}
    # f5-15B: the original acceptance installers' imports extend the W21 set
    # of the census DLLs only (never a title-private DLL).
    for dll,names in stubs.acceptance_extra(stubs.imports(research.read_text())).items():
        extra.setdefault(dll,set()).update(names)
    stubs.generate([research],generated,extra={dll:sorted(names) for dll,names in extra.items()})
    store=stubs.build(generated,out/'ntdll.lib')
    import shutil
    for dll in store['dlls']:shutil.copyfile(generated/dll['path'],out/dll['path'])
    kernel_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    for i,dll in enumerate(store['dlls']):
        if dll['name']=='kernel32.dll':store['dlls'][i]=kernel32.build(out,kernel_base,flags)
        if dll['name']=='wsock32.dll':store['dlls'][i]=kernel32.build_offline(out,dll['base'],flags)
    from scripts.win32 import audio
    audio_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    audio_dlls=audio.build(out,audio_base,flags)
    store['dlls']=[d for d in store['dlls'] if d['name'] not in ('dsound.dll','winmm.dll')]+audio_dlls
    gdi_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    for i,dll in enumerate(store['dlls']):
        if dll['name']=='gdi32.dll':store['dlls'][i]=gdi32.build(out,gdi_base)
    user_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    for i,dll in enumerate(store['dlls']):
        if dll['name']=='user32.dll':store['dlls'][i]=user32.build(out,user_base,flags)
    from scripts.win32 import help as htmlhelp
    help_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    store['dlls']=[d for d in store['dlls'] if d['name']!='hhctrl.ocx']+[htmlhelp.build(out,help_base)]
    ddraw_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    for i,dll in enumerate(store['dlls']):
        if dll['name']=='ddraw.dll':store['dlls'][i]=ddraw.build(out,ddraw_base)
    from scripts.win32 import input as directinput
    input_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    input_dlls=directinput.build(out,input_base,flags)
    store['dlls']=[d for d in store['dlls'] if d['name'] not in ('dinput.dll','dinput8.dll')]+input_dlls
    from scripts.win32 import codecs
    codec_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    codec_dlls=codecs.build(out,codec_base,flags)
    names={d['name'] for d in codec_dlls}
    store['dlls']=[d for d in store['dlls'] if d['name'] not in names]+codec_dlls
    upper_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    store['dlls']+=upper_dlls.build(out,upper_base,flags)
    # f5-20C: W21 stubs for the delay-loaded DLLs Windows 98 lacks (uxtheme, secur32, ...).
    stub_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    store['dlls']+=upper_dlls.build_absent_stubs(out,stub_base,flags)
    # f5-12B: the avivideo MCI driver, appended last so that no earlier
    # builtin base moves.
    mci_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    store['dlls'].append(audio.build_mci_avi(out,mci_base,flags))
    # f5-20A: the f5-07A ADVAPI32/VERSION/LZ32 facades replace their census
    # placeholders, at the facade bases (0x71000000 up, below the reused
    # Wine region), so the loader and the reused-DLL audit see one core.
    from scripts.win32 import facades
    facade_store=facades.build(out/'facades')
    for facade in facade_store['dlls']:
        shutil.copyfile(out/'facades'/facade['path'],out/facade['path'])
        replacement=dict(facade,version=stubs.image_version((out/facade['path']).read_bytes()))
        store['dlls']=[replacement if d['name']==facade['name'] else d for d in store['dlls']]
    shutil.copyfile(out/'facades'/'upper-coverage.json',out/'facades-upper-coverage.json')
    from scripts.win32 import shell
    shell_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    store['dlls'].append(shell.build(out,shell_base,flags))
    from scripts.win32 import opengl32
    gl_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    store['dlls']=[d for d in store['dlls'] if d['name']!='opengl32.dll']+[opengl32.build(out,gl_base,flags)]
    store['dlls'].insert(0,record)
    store['msvcrt_policy']='msvcrt.dll: Wine 10.0 PE selected by measurement (config/wine-upper.json, f5-14B); not staged until qualified'
    manifest=out/'builtin-store.json';manifest.write_text(json.dumps(store,sort_keys=True,indent=2)+'\n')
    stubs.verify_store(manifest)
    binary=struct.pack('<II',1,len(store['dlls']))
    for dll in store['dlls']:
        binary+=struct.pack('<64s65s3xII4H',dll['name'].encode(),dll['sha256'].encode(),dll['base'],dll['size'],*dll['version'])
    (out/'BUILTIN.BIN').write_bytes(binary)
    from scripts.win32 import fixtures
    fixture_source=ROOT/'build/host/win32/image-fixtures'
    hashes=fixtures.build(fixture_source)
    # Runtime-selected desktop v3 probe peer; raw production ABI, no imports.
    peer=out/'desktop-v3-peer.obj'
    subprocess.run(['clang',*flags,'-c',str(ROOT/'src/probes/desktop_v3/peer.c'),'-o',str(peer)],check=True)
    peer_exe=fixture_source/'desktop-v3.exe'
    subprocess.run(['lld-link','/machine:x86','/entry:entry@0','/nodefaultlib','/safeseh:no',
        '/base:0x00400000','/subsystem:windows,4.0','/osversion:4.0','/timestamp:0',
        '/incremental:no','/out:'+str(peer_exe),str(peer)],check=True)
    hashes[peer_exe.name]=hashlib.sha256(peer_exe.read_bytes()).hexdigest()
    gl_exercise=opengl32.build_exercise(fixture_source,out)
    hashes[gl_exercise.name]=hashlib.sha256(gl_exercise.read_bytes()).hexdigest()
    # f5-10C2: the w32-dialogs guest fixture, its imports checked against the
    # builtins just built.
    from scripts.win32 import dialogs_fixture
    dialogs=dialogs_fixture.build(fixture_source,out)
    hashes[dialogs.name]=hashlib.sha256(dialogs.read_bytes()).hexdigest()
    # f5-20D: the w32-upper-dlls guest fixture; its table is the qualified
    # set of config/wine-upper.json, so it does not depend on the upper build.
    from scripts.win32 import upper_fixture
    upper=upper_fixture.build(fixture_source,out)
    hashes[upper.name]=hashlib.sha256(upper.read_bytes()).hexdigest()
    from scripts.win32 import kernel32_fixture
    kernel32_exercise=kernel32_fixture.build(fixture_source,out)
    hashes[kernel32_exercise.name]=hashlib.sha256(kernel32_exercise.read_bytes()).hexdigest()
    from scripts.win32 import core_observer_fixtures
    for image in core_observer_fixtures.build(fixture_source,out):
        hashes[image.name]=hashlib.sha256(image.read_bytes()).hexdigest()
    from scripts.win32 import cd_media
    for image in cd_media.build(fixture_source):
        hashes[image.name]=hashlib.sha256(image.read_bytes()).hexdigest()
    # f5-15A2: offline pinned NSIS and authored CAB/INF/self-extractor inputs.
    from scripts.win32 import installer_fixture
    installer=installer_fixture.build(fixture_source/'installers')
    for name, digest in installer['files'].items():
        shutil.copyfile(fixture_source/'installers'/name,fixture_source/name)
        hashes[name]=digest
    from scripts.win32 import restart_fixture
    restart=restart_fixture.build(fixture_source,out)
    hashes[restart.name]=hashlib.sha256(restart.read_bytes()).hexdigest()
    # Same authored PE bytes under a separate, explicitly shimmed identity.
    shutil.copyfile(fixture_source/'console.exe',fixture_source/'shim-fixture.exe')
    hashes['shim-fixture.exe']=hashlib.sha256((fixture_source/'shim-fixture.exe').read_bytes()).hexdigest()
    (out/'fixtures').mkdir(exist_ok=True)
    for name in hashes:shutil.copyfile(fixture_source/name,out/'fixtures'/name)
    (out/'fixtures.json').write_text(json.dumps({'schema':1,'files':hashes},sort_keys=True,indent=2)+'\n')
    return manifest

if __name__=='__main__': print(build())
