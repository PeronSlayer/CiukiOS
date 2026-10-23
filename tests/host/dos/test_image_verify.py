"""fx-4b: production background verifier and stamp over production FAT/VFS."""
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import hashlib
import os
import random
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]


class ImageVerifyTests(unittest.TestCase):
    def test_background_verify_and_stamp(self):
        work = ROOT / 'build/host/fx-4b'
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            folder = Path(temporary)
            env = dict(os.environ, TMPDIR=temporary, MTOOLS_SKIP_CHECK='1')
            image = folder / 'UNIFIED.IMG'
            image.write_bytes(random.Random(4).randbytes(1024 * 1024 + 1024))
            sidecar = folder / 'UNIFIED.SHA'
            sidecar.write_text(hashlib.sha256(image.read_bytes()).hexdigest() + '\n')
            outer = folder / 'outer.img'
            with outer.open('wb') as stream:
                stream.truncate(16 * 1024 * 1024)
            subprocess.run(['mkfs.fat', '--invariant', '-F', '16', '-s', '4', outer], check=True, env=env, capture_output=True)
            for name in ('System', 'System/DOS'):
                subprocess.run(['mmd', '-i', outer, '::/' + name], check=True, env=env)
            for item in (image, sidecar):
                subprocess.run(['mcopy', '-i', outer, item, '::/System/DOS/' + item.name], check=True, env=env)
            sources = ['src/kernel/fs/' + name + '.c' for name in ('fs_port', 'blkreq', 'cache', 'path', 'fat', 'vfs', 'fs_ro')]
            sources += ['tests/host/fs/fake.c', 'src/kernel/lib/fmt.c', 'src/kernel/lib/sha256.c',
                        'tests/host/dos/image_verify_test.c']
            binary = folder / 'image-verify-test'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-D_POSIX_C_SOURCE=200809L',
                            '-DFS_HOST', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                            '-I' + str(ROOT / 'src/kernel/include'), '-I' + str(ROOT / 'src/kernel/fs'),
                            *(str(ROOT / s) for s in sources), '-pthread', '-o', binary], check=True, env=env)
            ran = subprocess.run([binary, outer], env=env)
            self.assertEqual(ran.returncode, 0)
            subprocess.run(['fsck.fat', '-n', outer], check=True, env=env)
            stamp = folder / 'stamp'
            subprocess.run(['mcopy', '-o', '-i', outer, '::/System/DOS/UNIFIED.VFY', stamp], check=True, env=env)
            data = stamp.read_bytes()
            self.assertEqual(len(data), 128)
            self.assertEqual(data[:8], b'CIUVFY01')
            self.assertEqual(hashlib.sha256(data[:96]).digest(), data[96:])


if __name__ == '__main__':
    unittest.main()
