#!/usr/bin/env bash
# Build upstream Jemm/JLOAD in isolation. This does not install a DOS VM.
set -euo pipefail
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 - "$root_dir" "$@" <<'PY'
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

root = Path(sys.argv[1])
ap = argparse.ArgumentParser(description='Build pinned JEMM386, JEMMEX and JLOAD; no runtime installation.')
ap.add_argument('--output', type=Path, default=root/'build/external/jemm-monitor')
ap.add_argument('--jobs', type=int, default=2)
ap.add_argument('--offline', action='store_true', help='Require the verified tool archives in OUTPUT/downloads')
ap.add_argument('--ciukios-device-query', action='store_true',
                help='Opt in to CiukiOS device queries and the A20 IN AL register-preservation fix')
ap.add_argument('--ciukios-vm-scheduler', action='store_true',
                help='Opt in to the exact-owned physical IRQ scheduler callback used by CVSESSION')
ap.add_argument('--ciukios-v86-interrupts', action='store_true',
                help='Accepted for compatibility: the scheduler build always compiles the '
                     'V86 interrupt profile, which a session must negotiate at run time')
args = ap.parse_args(sys.argv[2:])
if args.ciukios_v86_interrupts and not args.ciukios_vm_scheduler:
    ap.error('--ciukios-v86-interrupts requires --ciukios-vm-scheduler')
# One Jemm binary: the profile is compiled with the scheduler and stays idle
# until a CVSESSION owner requests it through Host_Scheduler_Profile.
args.ciukios_v86_interrupts = args.ciukios_vm_scheduler
if not 1 <= args.jobs <= 8:
    ap.error('--jobs must be between 1 and 8')
out = args.output.resolve()
if out == root or out == Path('/') or root/'build' not in out.parents:
    ap.error('--output must be a subdirectory of this repository\'s ignored build directory')
for tool in ('make', 'gcc', 'ar', 'curl') + (('patch',) if (args.ciukios_device_query or args.ciukios_vm_scheduler) else ()):
    if not shutil.which(tool):
        raise SystemExit(f'Missing host build tool: {tool}')
meta = json.loads((root/'third_party/jemm/UPSTREAM.json').read_text())
out.mkdir(parents=True, exist_ok=True)
cache = out/'downloads'
cache.mkdir(exist_ok=True)

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def checked(path, expected):
    if digest(path) != expected:
        raise SystemExit(f'SHA-256 mismatch (left untouched): {path}')
    return path

# Validate all inputs before compiling or replacing a previous output.
sources = {'jemm': checked(root/'third_party/jemm'/meta['archive'], meta['sha256'])}
for name, spec in meta['toolchains'].items():
    archive = cache/f'{name}-{spec["commit"]}.tar.gz'
    if not archive.exists():
        if args.offline:
            raise SystemExit(f'Offline archive missing: {archive}')
        fd, temporary = tempfile.mkstemp(prefix=f'.{name}-', suffix='.download', dir=cache)
        os.close(fd)
        temporary = Path(temporary)
        try:
            subprocess.run(['curl','--fail','--location','--retry','2','--connect-timeout','20',
                            '--output',str(temporary),spec['download']], check=True)
            checked(temporary, spec['sha256'])
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    sources[name] = checked(archive, spec['sha256'])

work = Path(tempfile.mkdtemp(prefix='work-', dir=out))
print(f'[jemm] Isolated build: {work}', flush=True)
trees = {}
for name, archive in sources.items():
    target = work/name
    target.mkdir()
    with tarfile.open(archive) as tf:
        tf.extractall(target, filter='data')
    entries = list(target.iterdir())
    if len(entries) != 1 or not entries[0].is_dir():
        raise SystemExit(f'Unexpected archive layout: {archive}')
    trees[name] = entries[0]

env = dict(os.environ, LC_ALL='C', TZ='UTC', SOURCE_DATE_EPOCH=str(meta['source_date_epoch']))
log_path = work/'build.log'

def run(command, directory):
    with log_path.open('a') as log:
        log.write(f'\n$ {" ".join(map(str, command))}\n')
        log.flush()
        result = subprocess.run(command, cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print('\n'.join(log_path.read_text(errors='replace').splitlines()[-35:]), file=sys.stderr)
        raise SystemExit(f'Build failed ({result.returncode}); complete log: {log_path}')

tools = {}
for name, spec in meta['toolchains'].items():
    run(['make','-f','GccUnix.mak','DEBUG=0',f'-j{args.jobs}'], trees[name])
    tools[name] = trees[name]/spec['binary']
    if not os.access(tools[name], os.X_OK):
        raise SystemExit(f'Expected compiler/linker output missing: {tools[name]}')
env['PATH'] = os.pathsep.join([str(tools[name].parent) for name in tools] + [env.get('PATH','')])

# Upstream explicitly requires these case aliases on case-sensitive hosts.
# Without the explicit adapter option, upstream source bytes are unchanged.
jemm = trees['jemm']
adaptations = []
included_adaptation_files = []
if args.ciukios_device_query:
    patch = root/'patches/jemm-ciukios-device-query.patch'
    a20_patch = root/'patches/jemm-preserve-a20-input-registers.patch'
    helper = root/'src/vm/jemm_device_query.inc'
    # Upstream archives preserve CRLF; the source-controlled patch uses LF.
    # Normalize only the three explicitly modified translation units.
    for relative in ('src/INIT16.ASM','Tools/JLOAD/JLOAD.ASM','src/XMS.ASM'):
        source_file = jemm/relative
        source_file.write_text(source_file.read_text())
    run(['patch', '--batch', '--forward', '-p1', '-i', str(patch)], jemm)
    run(['patch', '--batch', '--forward', '-p1', '-i', str(a20_patch)], jemm)
    shutil.copyfile(helper, jemm/'Include/ciuki_device_query.inc')
    adaptations.append({'name':'ciukios-device-query', 'patch_sha256':digest(patch),
                       'helper_sha256':digest(helper), 'helper_license':'MIT',
                       'a20_input_patch_sha256':digest(a20_patch),
                       'line_endings':'LF in the three patched translation units',
                       'scope':'Failed EMMXXXX0/EMMQXXX0 open uses registered device IOCTL/close; A20 port60h/92h IN AL preserves EAX high24; no kernel changes'})
    included_adaptation_files += [patch, a20_patch, helper]
if args.ciukios_vm_scheduler:
    scheduler_patch = root/'patches/jemm-ciukios-vm-scheduler.patch'
    # Normalize only the seven scheduler/profile integration units. Some may already
    # be LF when combined with the independently selected device adaptation.
    for relative in ('Include/JLM.INC','src/JEMM32.INC','src/JEMM32.ASM',
                     'Tools/JLOAD/VMM.ASM','src/EMU.ASM','src/VCPI.ASM','src/EXTERN32.INC'):
        source_file = jemm/relative
        source_file.write_text(source_file.read_text())
    run(['patch', '--batch', '--forward', '-p1', '-i', str(scheduler_patch)], jemm)
    # Ctrl+Alt+Del reaches the V86 keyboard intercept (the desktop's Task
    # Manager) instead of Jemm's own soft reboot.
    cad_patch = root/'patches/jemm-ciukios-ctrl-alt-del.patch'
    run(['patch', '--batch', '--forward', '-p1', '-i', str(cad_patch)], jemm)
    umb_patch = root/'patches/jemm-ciukios-umb-release.patch'
    (jemm/'src/UMB.ASM').write_text((jemm/'src/UMB.ASM').read_text())
    run(['patch', '--batch', '--forward', '-p1', '-i', str(umb_patch)], jemm)
    # An idle V86 context (HLT) gives the CPU to another ready VM at once.
    hlt_patch = root/'patches/jemm-ciukios-hlt-yield.patch'
    run(['patch', '--batch', '--forward', '-p1', '-i', str(hlt_patch)], jemm)
    adaptations.append({'name':'ciukios-ctrl-alt-del', 'patch_sha256':digest(cad_patch),
                        'scope':'INT 15h AX=4F53h is reflected to V86 like any key; the BIOS resets when no intercept takes it'})
    included_adaptation_files.append(cad_patch)
    adaptations.append({'name':'ciukios-vm-scheduler',
                        'patch_sha256':digest(scheduler_patch),
                        'line_endings':'LF in the seven patched scheduler/profile integration units; CVIRQ.INC is a new LF include',
                        'scope':'One exact-owned IRQ0 callback before V86 IRQ reflection; no additional VM context'})
    included_adaptation_files.append(scheduler_patch)
    adaptations.append({'name':'ciukios-umb-release',
                        'patch_sha256':digest(umb_patch),
                        'scope':'Atomic XMS UMB claim and exact release from CVSESSION VM ownership; Jemm UMB table remains authoritative'})
    included_adaptation_files.append(umb_patch)
    adaptations.append({'name':'ciukios-hlt-yield',
                        'patch_sha256':digest(hlt_patch),
                        'scope':'Profile function 11: the owner may switch V86 contexts at a V86 HLT instead of halting the CPU'})
    included_adaptation_files.append(hlt_patch)
if args.ciukios_v86_interrupts:
    adaptations.append({'name':'ciukios-v86-interrupts',
                        'negotiated':'Host_Scheduler_Profile service, version 1.0',
                        'scope':'Software IF/FLAGS/INT/PIC profile, active only while the exact host scheduler callback is installed and its owner has requested it; not a complete device VM'})
for directory in (jemm/'Include', jemm/'src', jemm/'Tools/JLOAD'):
    for path in list(directory.iterdir()):
        if path.is_file() and path.suffix.upper() == '.INC':
            alias = path.with_name(path.name.lower())
            if alias != path and not alias.exists():
                alias.symlink_to(path.name)

# The upstream explicit EXE target needs its output directory pre-created.
(jemm/'build/JEMM386').mkdir(parents=True)
(jemm/'build/JEMMEX').mkdir(parents=True)
profile_options = (['AOPT=-c -nologo -IInclude -DCIUKIOS_V86_INTERRUPTS=1']
                   if args.ciukios_v86_interrupts else [])
run(['make','-f','Linux.mak','DEBUG=0',*profile_options,'build/JEMM386/JEMM386.EXE'], jemm)
run(['make','-f','Linux.mak','DEBUG=0',*profile_options,'build/JEMMEX/JEMMEX.EXE'], jemm)
(jemm/'Tools/JLOAD/RELEASE').mkdir()
run(['make','-f','Linux.mak','DEBUG=0','RELEASE/JLOAD32.bin'], jemm/'Tools/JLOAD')
# The unmodified JLOAD.ASM incbins JLoad32.bin, while Linux.mak creates
# JLOAD32.bin. Match its case through an alias, just like the include files.
(jemm/'Tools/JLOAD/RELEASE/JLoad32.bin').symlink_to('JLOAD32.bin')
run(['make','-f','Linux.mak','DEBUG=0'], jemm/'Tools/JLOAD')
payloads = {'JEMM386.EXE': jemm/'build/JEMM386/JEMM386.EXE',
            'JEMMEX.EXE': jemm/'build/JEMMEX/JEMMEX.EXE',
            'JLOAD.EXE': jemm/'Tools/JLOAD/RELEASE/JLOAD.EXE'}
for name, path in payloads.items():
    if path.read_bytes()[:2] != b'MZ':
        raise SystemExit(f'Invalid DOS EXE output: {path}')
manifest = {'upstream': meta, 'source_bytes_modified': bool(adaptations),
            'adaptation': adaptations or None,
            'scope': 'Compilation only; no CiukiOS installation, guest execution or device virtualization proved.',
            'runtime_tested': False, 'build_log': str(log_path.relative_to(root)),
            'build_script_sha256': digest(root/'scripts/build_jemm_monitor.sh'),
            'tools': {name: {'sha256':digest(path), 'source_commit':meta['toolchains'][name]['commit']}
                      for name,path in tools.items()},
            'host_gcc': subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],
            'outputs': {name: {'bytes':path.stat().st_size,'sha256':digest(path)} for name,path in payloads.items()}}
result_dir = work/'output'
result_dir.mkdir()
for name, path in payloads.items():
    shutil.copyfile(path, result_dir/name)
for name, path in {'ARTISTIC.TXT':jemm/'Artistic.txt', 'JLOAD-LICENSE.TXT':jemm/'Tools/JLOAD/license.txt',
                   'README-UPSTREAM.TXT':jemm/'Readme.txt', 'JLOAD.TXT':jemm/'Tools/JLOAD/JLOAD.txt'}.items():
    if path.is_file():
        shutil.copyfile(path,result_dir/name)
shutil.copyfile(sources['jemm'],result_dir/meta['archive'])
if adaptations:
    for adaptation_file in included_adaptation_files:
        shutil.copyfile(adaptation_file,result_dir/adaptation_file.name)
    (result_dir/'CIUKIOS-MODIFICATIONS.TXT').write_text(
        'Modified CiukiOS experimental Jemm/JLOAD build, 2026-09-26.\n'
        'Upstream Jemm Artistic and JLOAD MIT notices remain in their included files.\n'
        'Apply the selected included patches (and copy any included helper) to reproduce.\n'
        'The optional device-query helper is MIT licensed; its complete notice is in the include.\n'
        'The optional scheduler extension owns one exact IRQ0 callback before V86 reflection.\n'
        'Its negotiated V86 interrupt profile keeps one virtual PIC; function 8 lets the owner accept\n'
        'a device IRQ for delivery to a protected-mode client with the same masking and priority rules.\n'
        'It services the one foreground DOS context and does not create an independent VM.\n'
        'Compilation does not prove device execution, unload or full DOS virtualization.\n')
(result_dir/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
# Publish only after both upstream programs have compiled and validated.
# Each build has immutable outputs; CURRENT contains the selected directory.
(out/'CURRENT').write_text(str(result_dir.relative_to(out))+'\n')
print(json.dumps({'outputs':str(result_dir),'runtime_tested':False,'binaries':manifest['outputs']},indent=2))
PY
