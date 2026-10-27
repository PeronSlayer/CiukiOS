#!/usr/bin/env python3
"""f3-06 local DOS workload fixture disks (DOOM, Wolf3D).

The owner's commercial game files live in ignored local directories
(third_party/Doom, third_party/WOLF3D). They are never copied into the
canonical image or a release. For a case that declares one, this generator
builds, inside the run directory only:

1. a private DOS VM base image with the exact DOSBOOT.IMG geometry
   (scripts/build_dos_image.write_fat16): the DOS system payloads read back
   from build/f0/dos/UNIFIED.IMG (hashes checked against its manifest) plus
   the game directory as /<TARGET> and generated test files;
2. a FAT16 superfloppy (mkfs.fat) holding that image as \\CIUKIGAM.IMG in one
   contiguous, sparse chain; the kernel mounts it as a further drive and the
   workload probe boots the VM from it (dosvm_config.base).

Absent directory or required file -> FixturesAbsent (the runner records
not_run reason=fixtures_absent without launching QEMU). Every file is hashed;
the manifest records redistribution=local-only. fsck.fat and mtools read
back both volumes independently. FAT layout: Microsoft FAT specification,
sections 3-6 (https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf).
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from evidence import EvidenceNotRun  # noqa: E402
import fat_fixtures  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BASE_NAME = 'CIUKIGAM.IMG'
OUTER_BYTES = 160 * 1024 * 1024
OUTER_SPC = 64                     # 32 KiB clusters: the 128 MiB base is 4096 clusters
OUTER_LABEL = 'CIUKIGAMES'
OUTER_ID = '43494b47'
REDISTRIBUTION = 'local-only'

# DOOM 1.9 reads its defaults from -config <file> (linuxdoom-1.10 m_misc.c
# M_LoadDefaults); names and SB values follow the DOS i_sound.c
# reconstruction (snd_sfxdevice/snd_musicdevice 3 = Sound Blaster, music
# through the OPL; port 544 = 220h, IRQ 7, DMA 1 as CIUKIDOS's BLASTER).
DOOM_CONFIG = ''.join(f'{key} {value}\r\n' for key, value in (
    ('mouse_sensitivity', 5), ('sfx_volume', 8), ('music_volume', 8), ('show_messages', 1),
    ('use_mouse', 1), ('use_joystick', 0), ('screenblocks', 10), ('detaillevel', 0),
    ('snd_channels', 8), ('snd_musicdevice', 3), ('snd_sfxdevice', 3), ('snd_sbport', 544),
    ('snd_sbirq', 7), ('snd_sbdma', 1), ('snd_mport', -1), ('usegamma', 0))).encode('ascii')

WORKLOADS = {
    'doom': {'source': 'third_party/Doom', 'target': '/DOOM',
             'required': (('DOOM.EXE',), ('DOOM.WAD', 'DOOM1.WAD')),
             'roles': {'DOOM.EXE': 'executable', 'DOOM.WAD': 'data', 'DOOM1.WAD': 'data'},
             'generated': {'CIUKI.CFG': DOOM_CONFIG}},
    'wolf3d': {'source': 'third_party/WOLF3D', 'target': '/WOLF3D',
               'required': (('WOLF3D.EXE',), ('VSWAP.WL6',), ('VGAGRAPH.WL6',), ('GAMEMAPS.WL6',)),
               'roles': {'WOLF3D.EXE': 'executable'},
               'generated': {}},
}


# Plain 8.3 names in the subset the controller's record grammar carries
# (letters, digits, '_' and '-'); others are skipped and listed.
DOS_NAME = re.compile(r"[A-Z0-9_\-]{1,8}(\.[A-Z0-9_\-]{1,3})?")


class FixturesAbsent(EvidenceNotRun):
    """The declared local directory or a required file is missing."""


def _build_dos():
    spec = importlib.util.spec_from_file_location('build_dos_image', ROOT / 'scripts/build_dos_image.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def run(args, **kw):
    p = subprocess.run([str(a) for a in args], capture_output=True, timeout=120,
                       env=dict(os.environ, LC_ALL='C', TZ='UTC', MTOOLS_SKIP_CHECK='1'), **kw)
    if p.returncode:
        raise ValueError('DOS fixture command failed: ' + ' '.join(map(str, args)) + '\n' +
                         p.stderr.decode(errors='replace')[-2000:])
    return p


def stream_sha(args):
    """SHA-256 of a command's stdout, read in blocks (the base image is 128 MiB)."""
    h = hashlib.sha256()
    with subprocess.Popen([str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          env=dict(os.environ, LC_ALL='C', TZ='UTC', MTOOLS_SKIP_CHECK='1')) as process:
        for block in iter(lambda: process.stdout.read(1024 * 1024), b''):
            h.update(block)
    if process.returncode:
        raise ValueError('DOS fixture command failed: ' + ' '.join(map(str, args)))
    return h.hexdigest()


def check_output(directory, root=ROOT):
    """Fixtures are written only below build/test-runs (runner) or build/host
    (host tests); never beside the canonical image or a release."""
    directory = Path(directory).resolve()
    root = Path(root).resolve()
    for forbidden in ('build/f0', 'build/full', 'build/releases'):
        if directory.is_relative_to(root / forbidden):
            raise ValueError('refusing to place local commercial payloads under ' + forbidden)
    if not any(directory.is_relative_to(root / allowed) for allowed in ('build/test-runs', 'build/host')):
        raise ValueError('DOS fixture output must be under build/test-runs or build/host')
    return directory


def local_files(workload, root=ROOT):
    """Top-level regular files with valid 8.3 names; the rest is reported as skipped."""
    spec = WORKLOADS[workload]
    source = (Path(root) / spec['source'])
    if not source.is_dir():
        raise FixturesAbsent('fixtures_absent: ' + spec['source'])
    dos = _build_dos()
    chosen, skipped = {}, []
    for path in sorted(source.iterdir(), key=lambda p: p.name.upper()):
        name = path.name.upper()
        try:
            if not path.is_file() or path.is_symlink() or not DOS_NAME.fullmatch(name):
                raise ValueError
            dos.shortname(name)
        except ValueError:
            skipped.append(path.name)
            continue
        if name in chosen or name in spec['generated']:
            skipped.append(path.name)
            continue
        chosen[name] = path
    for alternatives in spec['required']:
        if not any(name in chosen for name in alternatives):
            raise FixturesAbsent('fixtures_absent: ' + '|'.join(alternatives))
    return chosen, skipped


def extender(data):
    """A DOS/4GW extender bound into the executable (DOOM 1.9: Professional 1.95)."""
    at = data.find(b'DOS/4GW Professional Protected Mode Run-time')
    if at < 0:
        return 'none'
    version = re.search(rb'\x00([0-9]\.[0-9]{2}[a-z]?)\x00', data[at:at + 160])
    return 'dos4gw-professional-' + (version.group(1).decode() if version else 'unknown') + '-bound'


def dos_system(dos_image, expected_sha256=None):
    """Boot sector, loader area and every payload of the built DOS VM image,
    each read back with mtools and checked against the DOS manifest."""
    dos_image = Path(dos_image)
    manifest_path = dos_image.with_suffix('.json')
    if not dos_image.is_file() or not manifest_path.is_file():
        raise ValueError('DOS VM payload missing: build/f0/dos/UNIFIED.IMG (make build-full)')
    manifest = json.loads(manifest_path.read_text())
    actual = sha(dos_image)
    if actual != manifest.get('image_sha256') or (expected_sha256 and actual != expected_sha256):
        raise ValueError('UNIFIED.IMG differs from its manifest or the canonical image')
    with dos_image.open('rb') as stream:
        boot = stream.read(512)
        loader = stream.read(4096)
    files = {}
    for payload in manifest['payloads']:
        data = run(['mcopy', '-n', '-i', mtools_image(dos_image), '::' + payload['path'], '-']).stdout
        if len(data) != payload['size'] or hashlib.sha256(data).hexdigest() != payload['sha256']:
            raise ValueError('DOS payload read-back mismatch: ' + payload['path'])
        files[payload['path']] = data
    return boot, loader, files, actual


def mtools_image(path):
    """Address the FAT partition of a unified guest, or a synthetic host FAT."""
    with Path(path).open('rb') as stream:
        header=stream.read(512)
    # A FAT boot sector at LBA 0 is a superfloppy; otherwise the first MBR
    # entry addresses the volume when its type is a FAT type (Microsoft
    # partition types 01h/04h/06h/0Bh/0Ch/0Eh; the unified image uses 0Eh, fx-4).
    vbr=header[:1] in (b'\xeb',b'\xe9') and struct.unpack_from('<H',header,11)[0]==512
    if len(header)==512 and not vbr and header[510:512]==b'\x55\xaa' and header[450] in (0x01,0x04,0x06,0x0B,0x0C,0x0E):
        return str(path)+'@@'+str(struct.unpack_from('<I',header,454)[0]*512)
    return str(path)


def write_guest(path, boot, loader, files, directories):
    """Production fixtures retain the unified fork; legacy kernels fail closed.
    A superfloppy branch exists only for synthetic host parser fixtures."""
    if '/SYSTEM/CIUKIDOS.SYS' in files:
        raise ValueError('classic CiukiDOS is retired; rebuild the unified DOS payload')
    dos=_build_dos()
    if '/CIUKIDF.SYS' not in files:
        dos.write_fat16(path,boot,loader,files,directories=directories)
        return run(['fsck.fat','-n',path]).stdout.decode(errors='replace')
    identity=json.loads(files['/D48/UNI.JSN'])
    if identity.get('profile',{}).get('kernel')!='unified':
        raise ValueError('fixture requires the unified fork')
    spec=importlib.util.spec_from_file_location('fixture_unified',ROOT/'scripts/dos_conformance.py')
    conformance=importlib.util.module_from_spec(spec); spec.loader.exec_module(conformance)
    conformance.build_reference(path,files,directories,guest=(files,identity))
    # The outer fixture exposes the fixed VMM geometry; the unused tail is zero.
    with Path(path).open('r+b') as stream: stream.truncate(dos.SECTORS*512)
    return 'unified partition: fsck and payload read-back passed by build_reference'


def embed(outer, inner):
    """Write inner as \\CIUKIGAM.IMG: one contiguous chain from cluster 2 of
    the freshly made FAT16 volume, copying only allocated extents (sparse)."""
    size = inner.stat().st_size
    with outer.open('r+b') as stream:
        g = fat_fixtures.geometry(stream.read(512))
        if g['kind'] != 16 or g['spc'] != OUTER_SPC:
            raise ValueError('unexpected outer FAT geometry')
        cluster_bytes = g['spc'] * 512
        count = (size + cluster_bytes - 1) // cluster_bytes
        if count > g['clusters']:
            raise ValueError('outer fixture volume too small')
        for copy in range(g['fats']):
            base = (g['reserved'] + copy * g['fat_sectors']) * 512
            chain = bytearray()
            for i in range(count):
                chain += (0xFFFF if i + 1 == count else 3 + i).to_bytes(2, 'little')
            stream.seek(base + 4)
            stream.write(chain)
        stream.seek(g['root'] * 512)
        root = bytearray(stream.read(32 * 16))
        slot = next(i for i in range(16) if root[i * 32] in (0, 0xE5))
        record = _build_dos().entry('CIUKIGAMIMG', 0x21, 2, size)   # read-only, archive
        stream.seek(g['root'] * 512 + slot * 32)
        stream.write(record)
        data_offset = g['data'] * 512
        # Positional reads only: SEEK_DATA/SEEK_HOLE move the descriptor, which
        # a buffered reader would not notice.
        fd = os.open(inner, os.O_RDONLY)
        try:
            cursor = 0
            while cursor < size:
                try:
                    start = os.lseek(fd, cursor, os.SEEK_DATA)
                except OSError:
                    break
                end = min(os.lseek(fd, start, os.SEEK_HOLE), size)
                stream.seek(data_offset + start)
                while start < end:
                    block = os.pread(fd, min(1024 * 1024, end - start), start)
                    if not block:
                        raise ValueError('short base image read')
                    stream.write(block)
                    start += len(block)
                cursor = end
        finally:
            os.close(fd)
    return g


def generate(fixture, directory, index=0, root=ROOT, expected_dos_sha256=None, dos_image=None):
    workload = fixture.get('workload')
    spec = WORKLOADS.get(workload)
    if (not spec or fixture.get('generator') != 'dos-local' or fixture.get('source') != spec['source'] or
            fixture.get('target') != spec['target'] or fixture.get('redistribution') != REDISTRIBUTION):
        raise ValueError('undeclared DOS workload fixture')
    chosen, skipped = local_files(workload, root)
    directory = check_output(directory, root)
    directory.mkdir(parents=True, exist_ok=True)
    dos = _build_dos()
    boot, loader, files, dos_sha = dos_system(dos_image or Path(root) / 'build/f0/dos/UNIFIED.IMG', expected_dos_sha256)
    target = spec['target']
    workload_files = []
    for name, path in chosen.items():
        data = path.read_bytes()
        files[f'{target}/{name}'] = data
        workload_files.append({'path': f'{target}/{name}', 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                               'role': spec['roles'].get(name, 'data'), 'redistribution': REDISTRIBUTION,
                               **({'extender': extender(data)} if spec['roles'].get(name) == 'executable' else {})})
    for name, data in spec['generated'].items():
        files[f'{target}/{name}'] = data
        workload_files.append({'path': f'{target}/{name}', 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                               'role': 'config', 'redistribution': 'free'})
    dos.refuse_local_payloads({k: hashlib.sha256(v).hexdigest() for k, v in files.items()
                               if not k.startswith(target + '/')}, root)
    inner = directory / f'fixture-{index}.vm'
    outer = directory / f'fixture-{index}.img'
    for path in (inner, outer):
        if path.exists():
            raise ValueError('DOS fixture already exists: ' + path.name)
    try:
        # The boot sector's BPB names CIUKIOSFULL: keep the root label equal (fsck.fat compares them).
        directories=sorted({p.rsplit('/',1)[0] for p in files if p.count('/')>1}|{target})
        inner_check = write_guest(inner,boot,loader,files,directories)
        for item in workload_files:
            data = run(['mcopy', '-n', '-i', mtools_image(inner), '::' + item['path'], '-']).stdout
            if hashlib.sha256(data).hexdigest() != item['sha256']:
                raise ValueError('base image read-back mismatch: ' + item['path'])
        base_sha = sha(inner)
        with outer.open('wb') as stream:
            stream.truncate(OUTER_BYTES)
        run(['mkfs.fat', '--invariant', '-F', '16', '-S', '512', '-s', str(OUTER_SPC), '-R', '4', '-f', '2',
             '-r', '512', '-i', OUTER_ID, '-n', OUTER_LABEL, outer])
        geometry = embed(outer, inner)
        outer_check = run(['fsck.fat', '-n', outer]).stdout.decode(errors='replace')
        listing = run(['mdir', '-i', outer, '::/']).stdout.decode(errors='replace')
        if stream_sha(['mcopy', '-n', '-i', outer, '::/' + BASE_NAME, '-']) != base_sha:
            raise ValueError('outer fixture read-back differs from the base image')
    finally:
        inner.unlink(missing_ok=True)
    return {'generator': 'dos-local', 'workload': workload, 'source': spec['source'], 'target': target,
            'redistribution': REDISTRIBUTION, 'path': str(outer), 'size': outer.stat().st_size,
            'sha256': sha(outer), 'base_name': BASE_NAME, 'base_sha256': base_sha, 'dos_image_sha256': dos_sha,
            'files': workload_files, 'skipped': skipped, 'geometry': geometry,
            'checkers': {'base_fsck': inner_check, 'fixture_fsck': outer_check, 'listing': listing}}


def verify_records(fixture, records):
    """The guest-side hashes (DATA group=workload_file path= size= sha256=),
    measured by the probe on the VM's own disk, must equal the generator's
    for every file of the workload directory (executable with its bound
    extender, data, generated config), and name no other file."""
    seen = {}
    for r in records:
        if r.get('event') == 'DATA' and r.get('group') == 'workload_file':
            if r.get('path') in seen:
                raise ValueError('duplicate workload file record: ' + str(r.get('path')))
            seen[r.get('path')] = r
    for item in fixture['files']:
        r = seen.get(item['path'])
        if not r or r.get('sha256') != item['sha256'] or r.get('size') != str(item['size']):
            raise ValueError('guest-measured workload file differs from the fixture: ' + item['path'])
    unknown = set(seen) - {item['path'] for item in fixture['files']}
    if unknown:
        raise ValueError('workload file not in the fixture: ' + ', '.join(sorted(unknown)))
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('workload', choices=sorted(WORKLOADS))
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    spec = WORKLOADS[args.workload]
    fixture = {'generator': 'dos-local', 'workload': args.workload, 'source': spec['source'],
               'target': spec['target'], 'redistribution': REDISTRIBUTION}
    try:
        manifest = generate(fixture, args.directory)
    except FixturesAbsent as e:
        print(json.dumps({'status': 'not_run', 'reason': 'fixtures_absent', 'detail': str(e)}))
        return 0
    print(json.dumps({k: v for k, v in manifest.items() if k != 'checkers'}, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
