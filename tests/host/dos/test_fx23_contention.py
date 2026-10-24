"""fx-23: Files and the storage writer during the background DOS image check."""
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import hashlib
import os
import random
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]


class ContentionTests(unittest.TestCase):
    def test_files_and_writer_during_check(self):
        work = ROOT / 'build/host/fx-23'
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            folder = Path(temporary)
            env = dict(os.environ, TMPDIR=temporary, MTOOLS_SKIP_CHECK='1')
            image = folder / 'UNIFIED.IMG'
            image.write_bytes(random.Random(23).randbytes(6 * 1024 * 1024))
            sidecar = folder / 'UNIFIED.SHA'
            sidecar.write_text(hashlib.sha256(image.read_bytes()).hexdigest() + '\n')
            outer = folder / 'outer.img'
            with outer.open('wb') as stream:
                stream.truncate(24 * 1024 * 1024)
            subprocess.run(['mkfs.fat', '--invariant', '-F', '16', '-s', '4', outer], check=True, env=env, capture_output=True)
            for name in ('System', 'System/DOS', 'Files'):
                subprocess.run(['mmd', '-i', outer, '::/' + name], check=True, env=env)
            for item in (image, sidecar):
                subprocess.run(['mcopy', '-i', outer, item, '::/System/DOS/' + item.name], check=True, env=env)
            data = folder / 'data'
            data.mkdir()
            rng = random.Random(5)
            for i in range(40):
                (data / f'F{i:02}.DAT').write_bytes(rng.randbytes(16384))
            subprocess.run(['mcopy', '-i', outer, *sorted(data.iterdir()), '::/Files/'], check=True, env=env)
            sources = ['src/kernel/fs/' + name + '.c' for name in ('fs_port', 'blkreq', 'cache', 'path', 'fat', 'vfs', 'fs_ro')]
            sources += ['tests/host/fs/fake.c', 'src/kernel/lib/fmt.c', 'src/kernel/lib/sha256.c',
                        'tests/host/dos/contention_test.c']
            binary = folder / 'contention-test'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-D_POSIX_C_SOURCE=200809L',
                            '-DFS_HOST', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                            '-I' + str(ROOT / 'src/kernel/include'), '-I' + str(ROOT / 'src/kernel/fs'),
                            *(str(ROOT / s) for s in sources), '-pthread', '-o', binary], check=True, env=env)
            ran = subprocess.run([binary, outer], env=env, capture_output=True, text=True, timeout=300)
            print(ran.stdout.strip())
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('PASS fx-23 contention', ran.stdout)


if __name__ == '__main__':
    unittest.main()
