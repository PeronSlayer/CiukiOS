#!/usr/bin/env bash
set -euo pipefail
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 - "$root_dir" "$@" <<'PY'
import argparse, hashlib, json, os, pathlib, subprocess, sys, tarfile
root=pathlib.Path(sys.argv[1])
p=argparse.ArgumentParser(description='Build the experimental V86 session JLM; no installation.')
p.add_argument('--jemm-build',type=pathlib.Path,default=root/'build/external/jemm-monitor')
p.add_argument('--output',type=pathlib.Path,default=root/'build/full/vm-session')
p.add_argument('--kernel-listing',type=pathlib.Path,default=root/'build/full/obj/ciukidos.lst',
               help='CiukiDOS kernel listing: VM manager offsets of InDOS and the FAT cache')
p.add_argument('--debug-no-ring0-switch',action='store_true',
               help='Diagnostic only: suppress ring-0 IRQ switches at a saved HLT frame and count them')
p.add_argument('--switch-trace',action='store_true',
               help='Diagnostic only: record/freeze actual VMM frame switches in CVSWTR01')
a=p.parse_args(sys.argv[2:]);base=a.jemm_build.resolve();out=a.output.resolve()
if root/'build' not in out.parents: p.error('--output must be inside the ignored build directory')
current=(base/'CURRENT').read_text().strip();jemm_output=(base/current).resolve()
if base not in jemm_output.parents: raise SystemExit('Invalid Jemm CURRENT path')
manifest=json.loads((jemm_output/'manifest.json').read_text());work=jemm_output.parent
meta=json.loads((root/'third_party/jemm/UPSTREAM.json').read_text())
if manifest['upstream'] != meta: raise SystemExit('Pinned Jemm build metadata mismatch')
adaptation=manifest.get('adaptation') or []
if isinstance(adaptation,dict): adaptation=[adaptation]
if 'ciukios-vm-scheduler' not in {item.get('name') for item in adaptation}:
    raise SystemExit('CVSESSION requires a Jemm build selected with --ciukios-vm-scheduler')
tools={}
def tree_under(directory):
    trees=[entry for entry in directory.iterdir() if entry.is_dir()]
    if len(trees)!=1: raise SystemExit('Unexpected extracted source layout: '+str(directory))
    return trees[0]
for name,spec in meta['toolchains'].items():
    tree=tree_under(work/name)
    tools[name]=tree/spec['binary']
    if hashlib.sha256(tools[name].read_bytes()).hexdigest()!=manifest['tools'][name]['sha256']:
        raise SystemExit('Built tool hash mismatch: '+str(tools[name]))
include=tree_under(work/'jemm')/'Include'
out.mkdir(parents=True,exist_ok=True)
obj=out/'session.obj';binary=out/'CVSESSION.DLL'
# Video/presenter track: the monitored VGA model, x86 memory-operand emulator,
# virtual BIOS, presenter and ring-0 monitor are freestanding OpenWatcom C.
# nodefaultlibs below guarantees no runtime helper is linked into ring 0.
watcom=pathlib.Path(os.environ.get('WATCOM','/opt/watcom'))
wcc=watcom/'binl64/wcc386'
if not wcc.exists(): wcc=watcom/'binl/wcc386'
if not wcc.exists(): raise SystemExit('OpenWatcom wcc386 is required for the video monitor objects')
video_sources=['virtual_vga.c','virtual_vga_bios.c','vga_presenter.c','vga_x86.c','session_video.c',
               'guest_peripherals.c','session_devices.c','session_audio.c','session_clock.c','session_gpu.c',
               'session_gpu_legacy.c','session_gpu_savage.c','session_gpu_mach64.c','session_disk_ata.c']
# Guest OPL synthesis: DBOPL (GPL-2.0-or-later) from the pinned VSBHDA archive,
# unmodified, plus the ring-0 adapter, compiled by clang to freestanding COFF.
vsbhda=json.loads((root/'third_party/vsbhda/UPSTREAM.json').read_text())
vsbhda_archive=root/'third_party/vsbhda'/vsbhda['archive']
opl_shim=root/'src/vm/opl_shim'
clang=os.environ.get('CLANGXX','clang++')
source_paths=[root/'src/vm'/name for name in
              ['session_jlm.asm','session_abi.inc','session_vmm.inc','session_ivt_baseline.inc','session_launch_trace.inc','session_native_pages.inc',
               'session_disk.inc','session_disk_ata.h',
               'session_framebuffer_cache.inc','session_framebuffer_fill.inc',
               'session_framebuffer_triangle.inc',
               'session_clock.inc','session_clock.h',
               'session_gpu.inc','session_gpu.h',
               'session_gpu_legacy.h','session_gpu_savage.h','session_gpu_mach64.h','session_gpu_mach64.inc',
               'session_native_process.inc',
               'session_scheduler.inc',
               'session_scheduler_abi.inc','session_scheduler.h',
              'session_video.inc','session_video_abi.inc','session_video.h',
              'session_switch_trace.inc',
               'vga_x86.h','virtual_vga.h','virtual_vga_bios.h','vga_presenter.h',
               'guest_peripherals.h','session_devices.h','session_audio.h','session_devices.inc',
               'session_devices_abi.inc','session_opl.cpp',
               *video_sources]]+[root/'src/native/native_entry.asm',
               root/'src/native/native_entry_abi.inc',
               root/'scripts/build_vm_session.sh',vsbhda_archive,
               *sorted(opl_shim.glob('*.h'))]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def snapshot():
    return {'sources':{str(path.relative_to(root)):sha(path) for path in source_paths},
            'include_sources':{str(path.relative_to(include)):sha(path)
                               for path in sorted(include.rglob('*')) if path.is_file()},
            'tools':{name:{'path':str(path),'sha256':sha(path)}
                     for name,path in dict(tools,wcc386=wcc).items()},
            'jemm_build_manifest_sha256':sha(jemm_output/'manifest.json'),
            'diagnostics':{'no_ring0_switch':bool(a.debug_no_ring0_switch),
                           'switch_trace':bool(a.switch_trace)}}
inputs=snapshot()
if inputs['sources'][str(vsbhda_archive.relative_to(root))]!=vsbhda['sha256']:
    raise SystemExit('Pinned VSBHDA archive hash mismatch')
inputs['clang']=subprocess.check_output([clang,'--version'],text=True).splitlines()[0]
(out/'build-inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
dbopl=out/'dbopl-src'
dbopl.mkdir(exist_ok=True)
with tarfile.open(vsbhda_archive,'r:gz') as source:
    for member in source.getmembers():
        for wanted in vsbhda['used_files']:
            if member.name.endswith('/'+wanted):
                if not member.isfile() or member.size>1024*1024: raise SystemExit('Invalid DBOPL member')
                (dbopl/pathlib.Path(wanted).name).write_bytes(source.extractfile(member).read())
clang_flags=['--target=i686-pc-windows-gnu','-march=i686','-mno-sse','-mno-mmx','-ffreestanding',
             '-nostdinc','-nostdinc++','-isystem',str(opl_shim),'-isystem',
             subprocess.check_output([clang,'-print-resource-dir'],text=True).strip()+'/include',
             '-fno-exceptions','-fno-rtti','-fno-pic','-fno-asynchronous-unwind-tables',
             '-fno-unwind-tables','-fno-stack-protector','-fno-threadsafe-statics','-fno-builtin',
             '-O2','-g0','-fno-addrsig','-fno-common','-w','-I'+str(dbopl)]
opl_objects=[]
for name,source_path in (('dbopl',dbopl/'DBOPL.CPP'),('session_opl',root/'src/vm/session_opl.cpp')):
    raw=out/(name+'-clang.obj'); cooked=out/(name+'.obj')
    subprocess.run([clang,*clang_flags,'-c',str(source_path),'-o',str(raw)],check=True)
    subprocess.run(['objcopy','--remove-section=.debug$S','--remove-section=.llvm_addrsig',
                    str(raw),str(cooked)],check=True)
    opl_objects+=['file',str(cooked)]
# The VM manager switches DOS VMs only outside DOS with a clean FAT cache and
# invalidates the resumed VM's cache; it needs these kernel data offsets. A
# missing listing builds a VMM that refuses to create VMs.
import re
wanted={'dos_indos_flag':'KL_INDOS','fat_cache_valid':'KL_FAT_VALID','fat_cache_dirty':'KL_FAT_DIRTY',
        'fat_cache_sector':'KL_FAT_SECTOR','dos_mem_last_mcb_seg':'KL_LAST_MCB',
        'int21_last_ah':'KL_LAST_AH',
        'dos_exec_identity_psp':'KL_EXEC_PSP','current_psp_seg':'KL_CURRENT_PSP',
        'boot_drive':'KL_BOOT_DRIVE','int_default_iret':'KL_DEFAULT_IRET'}
layout={}
if a.kernel_listing.exists():
    pending_label=None
    for line in a.kernel_listing.read_text(errors='replace').splitlines():
        if re.search(r'\bint_default_iret:\s*$',line):
            pending_label='KL_DEFAULT_IRET'
        elif pending_label:
            code=re.match(r'\s*\d+\s+([0-9A-F]{8})\s+CF\b',line)
            if code:
                layout[pending_label]=int(code.group(1),16)
                pending_label=None
        m=re.match(r'\s*\d+\s+([0-9A-F]{8})\s+\S+\s+(?:<\d+>\s+)?(\w+)\s+d[bw]\b',line)
        if m and m.group(2) in wanted: layout[wanted[m.group(2)]]=int(m.group(1),16)
present=len(layout)==len(wanted)
lines=['; Generated from '+str(a.kernel_listing)+' by build_vm_session.sh','KL_PRESENT equ '+('1' if present else '0')]
lines+=[f'{name} equ 0{layout.get(name,0):X}h' for name in wanted.values()]
(out/'kernel_layout.inc').write_text('\n'.join(lines)+'\n')
subprocess.run([str(tools['jwasm']),'-coff','-c','-nologo','-I'+str(include),'-I'+str(root/'src/vm'),
                '-I'+str(root/'src/native'),'-I'+str(out),
                *(['-D','VMM_DEBUG_NO_RING0_SWITCH=1'] if a.debug_no_ring0_switch else []),
                *(['-D','VMM_SWITCH_TRACE=1'] if a.switch_trace else []),
                '-Fo'+str(obj),'-Fl'+str(out/'session.lst'),str(root/'src/vm/session_jlm.asm')],check=True)
native_obj=out/'native_entry.obj'
subprocess.run([str(tools['jwasm']),'-coff','-c','-nologo','-I'+str(include),
                '-I'+str(root/'src/native'),'-Fo'+str(native_obj),
                '-Fl'+str(out/'native_entry.lst'),
                str(root/'src/native/native_entry.asm')],check=True)
video_objects=[]
for name in video_sources:
    video_obj=out/(pathlib.Path(name).stem+'.obj')
    subprocess.run([str(wcc),'-zq','-bt=nt','-mf','-3r','-ecc','-zl','-s','-ox','-ot','-w4','-we',
                    '-i='+str(watcom/'h'),'-fo='+str(video_obj),str(root/'src/vm'/name)],check=True)
    video_objects+=['file',str(video_obj)]
# Relative names: JWlink truncates long directive lines (the output name
# was cut once the device and OPL objects were added).
relative=lambda items:[pathlib.Path(i).name if i!='file' else i for i in items]
subprocess.run([str(tools['jwlink']),'format','win','nt','hx','dll','ru','native','file',obj.name,
                'file',native_obj.name,
                *relative(video_objects),*relative(opl_objects),'name',binary.name,
                'op','q,nodefaultlibs,MAP=session.map','export','_ddb.1'],check=True,cwd=out)
if {k:v for k,v in snapshot().items()}!={k:v for k,v in inputs.items() if k!='clang'}:
    raise SystemExit('CVSESSION inputs changed during compilation; outputs are not qualified')
(out/'manifest.json').write_text(json.dumps({'abi':1,'runtime_tested':False,'jemm_commit':meta['commit'],
    **inputs,'inputs_unchanged_during_build':True,
    'module':{'bytes':binary.stat().st_size,'sha256':sha(binary)}},indent=2)+'\n')
print(binary)
PY
