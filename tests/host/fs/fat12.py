#!/usr/bin/env python3
"""Host FAT12 geometry/interoperability tests; no OS image build or QEMU."""
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/fat12'
# KB75131: KiB, heads, sectors/track, sectors/cluster, root entries, media.
FORMATS = [
    (160, 1, 8, 1, 64, 0xfe), (180, 1, 9, 1, 64, 0xfc),
    (320, 2, 8, 2, 112, 0xff), (360, 2, 9, 2, 112, 0xfd),
    (720, 2, 9, 2, 112, 0xf9), (1200, 2, 15, 1, 224, 0xf9),
    (1440, 2, 18, 1, 224, 0xf0), (2880, 2, 36, 2, 240, 0xf0),
]


def run(*args, binary=False):
    print('+ ' + shlex.join(map(str, args)), flush=True)
    result = subprocess.run(list(map(str, args)), cwd=ROOT, env=ENV,
                            capture_output=True)
    if not binary:
        print(result.stdout.decode(), end='', flush=True)
    if result.stderr:
        print(result.stderr.decode(), end='', flush=True)
    result.check_returncode()
    return result.stdout


def pattern(size):
    return bytes((i * 29 + (i >> 8)) & 255 for i in range(size))


def fixture(folder, fmt, spc=None):
    kib, heads, spt, cluster, root, media = fmt
    cluster = spc or cluster
    image = folder / 'volume.img'
    with image.open('wb') as f:
        f.truncate(kib * 1024)
    run('mkfs.fat', '--invariant', '-a', '-F', '12', '-S', '512', '-s', cluster,
        '-R', '1', '-r', root, '-f', '2', '-g', f'{heads}/{spt}',
        '-M', hex(media), image)
    with image.open('rb') as f:
        b = f.read(512)
    u16 = lambda n: struct.unpack_from('<H', b, n)[0]
    assert (u16(11), b[13], u16(14), b[16], u16(17), u16(19), b[21],
            u16(24), u16(26)) == (512, cluster, 1, 2, root, kib * 2, media, spt, heads)
    clusters = (kib * 2 - 1 - 2 * u16(22) - root // 16) // cluster
    assert 0 < clusters < 4085
    seed = folder / 'seed.bin'
    seed.write_bytes(pattern(4099))
    run('mcopy', '-i', image, seed, '::/SEED.BIN')
    run('fsck.fat', '-n', image)
    run('mdir', '-i', image, '::/')
    return image


def verify(image, mode, fmt):
    run('fsck.fat', '-n', image)
    run('mdir', '-i', image, '::/')
    if mode != 'split':
        assert run('mtype', '-i', image, '::/SEED.BIN', binary=True) == pattern(4099)
    if mode == 'rw':
        assert run('mtype', '-i', image, '::/RENAMED.BIN', binary=True) == pattern(1500)
        listing = run('mdir', '-b', '-i', image, '::/GROW/').decode().splitlines()
        assert len(listing) == fmt[3] * 16 + 3, listing
        names = run('mdir', '-b', '-i', image, '::/').decode()
        assert 'NEW.BIN' not in names and 'DELETE.BIN' not in names and 'RENAMED.BIN' in names
    if mode == 'split':
        assert 'SPLIT.BIN' not in run('mdir', '-b', '-i', image, '::/').decode()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='native-', dir=OUT) as scratch:
        folder = Path(scratch)
        ENV['MTOOLSRC'] = str(folder / 'mtoolsrc')
        (folder / 'mtoolsrc').write_text('default_codepage=437\n')
        exe = folder / 'test_fat12'
        sources = ['src/kernel/fs/' + s + '.c' for s in
                   ('fs_port', 'blkreq', 'cache', 'partition', 'path', 'fat', 'vfs',
                    'fs_ro', 'iso9660', 'ntfs_fs', 'ntfs_ro', 'mount')]
        sources += ['src/kernel/core/bootlog.c', 'src/kernel/drivers/blkpart.c',
                    'src/kernel/probes/fat_probes.c', 'src/kernel/lib/sha256.c',
                    'src/kernel/lib/fmt.c', 'tests/host/fs/fake.c',
                    'tests/host/fs/scan.c', 'tests/host/fs/test_fat12.c']
        run('clang', '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-std=c17',
            '-Wall', '-Wextra', '-Werror', '-g', '-O1', '-fsanitize=address,undefined',
            '-fno-omit-frame-pointer', '-pthread', '-Isrc/kernel/include',
            '-Isrc/kernel/fs', *sources, '-o', exe)
        count = 0
        for fmt in FORMATS:
            for mode in ('rw', 'root'):
                image = fixture(folder, fmt)
                run(exe, mode, image)
                verify(image, mode, fmt)
                count += 1
        for mode in ('full', 'corrupt', 'protected', 'late-protected', 'changed-protected', 'remove'):
            image = fixture(folder, FORMATS[4])
            run(exe, mode, image)
            verify(image, mode, FORMATS[4])
            count += 1
        for spc in (2, 4):
            fmt = (1680, 2, 21, spc, 16, 0xf0)
            image = fixture(folder, fmt)
            original = image.read_bytes()
            run(exe, 'dmf', image)
            assert image.read_bytes() == original
            verify(image, 'dmf', fmt)
            count += 1
        image = fixture(folder, FORMATS[4])
        run('mdel', '-i', image, '::/SEED.BIN')
        (folder / 'split.bin').write_bytes(pattern(690 * 1024))
        run('mcopy', '-i', image, folder / 'split.bin', '::/SPLIT.BIN')
        run('fsck.fat', '-n', image)
        run(exe, 'split', image)
        verify(image, 'split', FORMATS[4])
        count += 1
    print(f'PASS native FAT12: {count} cases; 8 standard geometries, DMF 1K/2K, '
          'fsck/mtools before and after; ASan/UBSan; temporary artifacts removed')


ENV = dict(os.environ, LC_ALL='C.UTF-8', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
if __name__ == '__main__':
    main()
