"""DOS Box close under ordinary C: load (QEMU, image 56e58374: "Closing..." stayed)."""
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]


class CloseContentionTests(unittest.TestCase):
    def test_close_under_load(self):
        """The transient session's close path (logical C: flush through the overlay
        provider, overlay commit, close and delete) under the storage writer's
        write-back and boot-log appends, the print spooler's probes and desktop
        settings saves, over the production cache, FAT, VFS and dos_file_volume."""
        work = ROOT / 'build/host'
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='dos-close-', dir=work) as temporary:
            binary = Path(temporary) / 'close-contention'
            sources = ['src/kernel/fs/' + name + '.c' for name in
                       ('fs_port', 'blkreq', 'cache', 'path', 'fat', 'vfs', 'fs_ro', 'partition')]
            sources += ['src/kernel/drivers/blkpart.c', 'src/kernel/lib/sha256.c', 'src/kernel/lib/fmt.c',
                        'src/kernel/vm/dos_file_volume.c', 'tests/host/dos/teardown_contention_test.c']
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-D_POSIX_C_SOURCE=200809L',
                            '-DFS_HOST', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-pthread',
                            '-I' + str(ROOT / 'src/kernel/include'), '-I' + str(ROOT / 'src/kernel/fs'),
                            *(str(ROOT / s) for s in sources), '-o', str(binary)], check=True)
            ran = subprocess.run([str(binary), '12', '50'], capture_output=True, text=True, timeout=300,
                                 env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
            print(ran.stdout.strip())
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('PASS DOS Box close under C: load', ran.stdout)


if __name__ == '__main__':
    unittest.main()
