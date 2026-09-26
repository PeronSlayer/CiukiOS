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
ap = argparse.ArgumentParser(description='Build pinned JEMM386 and JLOAD; no runtime installation.')
ap.add_argument('--output', type=Path, default=root/'build/external/jemm-monitor')
ap.add_argument('--jobs', type=int, default=2)
ap.add_argument('--offline', action='store_true', help='Require the verified tool archives in OUTPUT/downloads')
ap.add_argument('--ciukios-device-query', action='store_true',
                help='Opt in to CiukiOS device queries and the A20 IN AL register-preservation fix')
args = ap.parse_args(sys.argv[2:])
if not 1 <= args.jobs <= 8:
    ap.error('--jobs must be between 1 and 8')
out = args.output.resolve()
if out == root or out == Path('/') or root/'build' not in out.parents:
    ap.error('--output must be a subdirectory of this repository\'s ignored build directory')
for tool in ('make', 'gcc', 'ar', 'curl') + (('patch',) if args.ciukios_device_query else ()):
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
adaptation = None
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
    adaptation = {'name':'ciukios-device-query', 'patch_sha256':digest(patch),
                  'helper_sha256':digest(helper), 'helper_license':'MIT',
                  'a20_input_patch_sha256':digest(a20_patch),
                  'line_endings':'LF in the three patched translation units',
                  'scope':'Failed EMMXXXX0/EMMQXXX0 open uses registered device IOCTL/close; A20 port60h/92h IN AL preserves EAX high24; no kernel changes'}
for directory in (jemm/'Include', jemm/'src', jemm/'Tools/JLOAD'):
    for path in list(directory.iterdir()):
        if path.is_file() and path.suffix.upper() == '.INC':
            alias = path.with_name(path.name.lower())
            if alias != path and not alias.exists():
                alias.symlink_to(path.name)

# The upstream explicit EXE target needs its output directory pre-created.
(jemm/'build/JEMM386').mkdir(parents=True)
run(['make','-f','Linux.mak','DEBUG=0','build/JEMM386/JEMM386.EXE'], jemm)
(jemm/'Tools/JLOAD/RELEASE').mkdir()
run(['make','-f','Linux.mak','DEBUG=0','RELEASE/JLOAD32.bin'], jemm/'Tools/JLOAD')
# The unmodified JLOAD.ASM incbins JLoad32.bin, while Linux.mak creates
# JLOAD32.bin. Match its case through an alias, just like the include files.
(jemm/'Tools/JLOAD/RELEASE/JLoad32.bin').symlink_to('JLOAD32.bin')
run(['make','-f','Linux.mak','DEBUG=0'], jemm/'Tools/JLOAD')
payloads = {'JEMM386.EXE': jemm/'build/JEMM386/JEMM386.EXE',
            'JLOAD.EXE': jemm/'Tools/JLOAD/RELEASE/JLOAD.EXE'}
for name, path in payloads.items():
    if path.read_bytes()[:2] != b'MZ':
        raise SystemExit(f'Invalid DOS EXE output: {path}')
manifest = {'upstream': meta, 'source_bytes_modified': bool(adaptation),
            'adaptation': adaptation,
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
if adaptation:
    shutil.copyfile(patch,result_dir/patch.name)
    shutil.copyfile(a20_patch,result_dir/a20_patch.name)
    shutil.copyfile(helper,result_dir/helper.name)
    (result_dir/'CIUKIOS-MODIFICATIONS.TXT').write_text(
        'Modified CiukiOS experimental Jemm/JLOAD build, 2026-09-26.\n'
        'Upstream Jemm Artistic and JLOAD MIT notices remain in their included files.\n'
        'The separate device-query helper is MIT licensed; its complete notice is in the include.\n'
        'Apply both included patches and copy the helper into upstream Include/ to reproduce.\n'
        'Failed DOS opens of the genuine registered EMM device use its registered header.\n'
        'The A20 input handler preserves guest EAX bits8-31 for byte reads of ports60h/92h.\n'
        'The latter is an independently authored correction to the upstream Artistic-licensed source.\n'
        'Compilation does not prove device execution, unload or full DOS virtualization.\n')
(result_dir/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
# Publish only after both upstream programs have compiled and validated.
# Each build has immutable outputs; CURRENT contains the selected directory.
(out/'CURRENT').write_text(str(result_dir.relative_to(out))+'\n')
print(json.dumps({'outputs':str(result_dir),'runtime_tested':False,'binaries':manifest['outputs']},indent=2))
PY
