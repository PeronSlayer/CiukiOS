"""H02: production OWNER, retained subjects, policy and FAT/native resolver."""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/security'
OWNER = [f'src/kernel/fs/{name}.c' for name in (
    'owner', 'owner_wire', 'owner_store', 'owner_checkpoint', 'owner_roots', 'security_policy')]


class OwnerPolicyTests(unittest.TestCase):
    def compile_run(self, name, sources, args=(), flags=()):
        OUT.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, TMPDIR=str(OUT))
        command = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                   '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread',
                   '-I', str(ROOT / 'src/kernel/include'), '-I', str(ROOT / 'src/kernel/fs')]
        subprocess.run(command + list(flags) + [str(ROOT / s) for s in sources] + ['-o', str(OUT / name)],
                       check=True, env=env)
        result = subprocess.run([str(OUT / name), *map(str, args)], env=env,
                                capture_output=True, text=True, timeout=180)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H02', result.stdout)

    def test_owner_transactions(self):
        self.compile_run('owner_policy', [
            'tests/host/security/owner_policy_test.c', 'tests/host/proc/signal_legacy.c',
            'src/kernel/fs/fs_port.c', 'src/kernel/fs/path.c', 'src/kernel/lib/sha256.c',
            'src/kernel/lib/fmt.c', 'src/kernel/lib/tzrule.c', 'src/kernel/probes/selector.c',
            'src/kernel/fs/owner_service.c',
            *OWNER])

    def test_resolved_names(self):
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/fs/fixtures.py')], check=True)
        self.compile_run('owner_names', [
            'tests/host/security/owner_names_test.c', 'tests/host/proc/signal_legacy.c',
            'tests/host/proc/media_legacy.c', 'tests/host/proc/desktop_legacy.c',
            'src/kernel/fs/fs_port.c', 'src/kernel/fs/blkreq.c', 'src/kernel/fs/cache.c',
            'src/kernel/fs/path.c', 'src/kernel/fs/fat.c', 'src/kernel/fs/vfs.c',
            'src/kernel/fs/fs_ro.c', 'src/kernel/fs/iso9660.c', 'tests/host/fs/fake.c',
            'src/kernel/probes/f2_probes_files.c', 'src/kernel/lib/sha256.c',
            'src/kernel/fs/owner_vfs.c', 'src/kernel/fs/security_path.c', *OWNER],
            [ROOT / 'build/host/fs/fat32.img', ROOT / 'build/host/fs/fat16.img'])

    def test_owner_vfs_backend(self):
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/fs/fixtures.py')], check=True)
        self.compile_run('owner_vfs', [
            'tests/host/security/owner_policy_test.c', 'tests/host/proc/signal_legacy.c',
            'src/kernel/fs/fs_port.c', 'src/kernel/fs/blkreq.c', 'src/kernel/fs/cache.c',
            'src/kernel/fs/path.c', 'src/kernel/fs/fat.c', 'src/kernel/fs/vfs.c',
            'src/kernel/fs/fs_ro.c', 'src/kernel/fs/iso9660.c', 'tests/host/fs/fake.c',
            'src/kernel/lib/sha256.c', 'src/kernel/lib/fmt.c', 'src/kernel/lib/tzrule.c',
            'src/kernel/probes/selector.c', 'src/kernel/fs/owner_vfs.c',
            'src/kernel/fs/owner_service.c', *OWNER],
            [ROOT / 'build/host/fs/fat32.img'], ['-DOWNER_REAL_VFS_TEST', '-Wno-unused-function'])


if __name__ == '__main__':
    unittest.main()
