"""F1-63 production three-volume namespace and desktop master persistence."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import iso_fixtures


class VolumeResolutionTests(unittest.TestCase):
    def test_system_volume_with_cd_and_failed_usb(self):
        out = ROOT / 'build/host/fs'
        out.mkdir(parents=True, exist_ok=True)
        subprocess.run([sys.executable, str(ROOT / 'tests/host/fs/fixtures.py')], check=True)
        with tempfile.TemporaryDirectory(prefix='volume-resolution-', dir=out) as scratch:
            work = Path(scratch)
            inc = work / 'include'
            inc.mkdir()
            (inc / 'ciuki').symlink_to(ROOT / 'sdk/sysroot-overlay/include/ciuki', target_is_directory=True)
            iso = iso_fixtures.generate({'level': 3, 'joliet': True}, work)
            env = dict(os.environ, TMPDIR=scratch, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1')
            flags = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                     '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                     '-ffunction-sections', '-fdata-sections', '-pthread']
            master = work / 'master.o'
            subprocess.run([*flags, '-D__CIUKIOS__', '-I'+str(inc),
                            '-I'+str(ROOT / 'src/kernel/include'), '-c',
                            str(ROOT / 'apps/desktop/master_storage.c'), '-o', str(master)], check=True, env=env)
            sources = ['src/kernel/fs/'+n+'.c' for n in
                       ('fs_port', 'cache', 'partition', 'path', 'fat', 'vfs', 'fs_ro', 'iso9660', 'ntfs_fs', 'ntfs_ro', 'mount')]
            sources += ['src/kernel/core/bootlog.c', 'src/kernel/drivers/blkpart.c',
                        'src/kernel/probes/fat_probes.c', 'src/kernel/lib/sha256.c', 'src/kernel/lib/fmt.c',
                        'tests/host/fs/fake.c', 'tests/host/fs/scan.c', 'tests/host/fs/volume_resolution_test.c']
            binary = work / 'volume-test'
            subprocess.run([*flags, '-Wno-unused-function', '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L',
                            '-I'+str(ROOT / 'src/kernel/include'), '-I'+str(ROOT / 'src/kernel/fs'),
                            *(str(ROOT / s) for s in sources), str(master), '-Wl,--gc-sections',
                            *('-Wl,--wrap='+n for n in ('open', 'close', 'stat', 'mkdir', 'unlink', 'rename', 'fsync', 'fopen', 'fileno')),
                            '-o', str(binary)], check=True, env=env)
            subprocess.run([str(binary), *(str(out / ('fat'+str(n)+'.img')) for n in (32, 12, 16)), iso['path']],
                           check=True, env=env)


if __name__ == '__main__':
    unittest.main()
