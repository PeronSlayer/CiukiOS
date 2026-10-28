"""H16 (sec-sysvc): SYSTEM services for administrator operations and logs.

Links the production call-99 adapter (proc/syscalls_sysvc.c), the services
(fs/sysvc.c, fs/sysvc_drivers.c), the NDIS package format, the native
syscalls/fd table/POSIX namespace, the legacy VFS tables and the managed-volume
policy/OWNER code on the real FAT/VFS (fake block device, in-memory OWNER
blobs, fake NDIS device observations). Administrator, standard user and Guest
for every service; the direct routes stay refused (K-Q4).
"""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/security'
OWNER = [f'src/kernel/fs/{name}.c' for name in (
    'owner', 'owner_wire', 'owner_store', 'owner_checkpoint', 'owner_roots', 'owner_vfs',
    'security_policy', 'security_path', 'sysvc', 'sysvc_drivers',
    'sysvc_jobs')]  # sec-sysvc2: SYSTEM jobs and sealed boot settings


class SystemServicesTests(unittest.TestCase):
    def test_system_services(self):
        OUT.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, TMPDIR=str(OUT))
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/fs/fixtures.py')], check=True, env=env)
        sources = ['tests/host/security/system_services_test.c', 'tests/host/proc/signal_legacy.c',
                   'tests/host/proc/media_legacy.c', 'tests/host/proc/desktop_legacy.c',
                   'src/kernel/fs/fs_port.c', 'src/kernel/fs/blkreq.c', 'src/kernel/fs/cache.c',
                   'src/kernel/fs/path.c', 'src/kernel/fs/fat.c', 'src/kernel/fs/vfs.c',
                   'src/kernel/fs/fs_ro.c', 'src/kernel/fs/iso9660.c', 'tests/host/fs/fake.c',
                   'src/kernel/probes/f2_probes_files.c', 'src/kernel/lib/sha256.c',
                   'src/kernel/ndis/package.c', 'src/kernel/lib/tzrule.c', *OWNER]
        program = OUT / 'system_services'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread',
                        '-I', str(ROOT / 'src/kernel/include'), '-I', str(ROOT / 'src/kernel/fs'),
                        *[str(ROOT / s) for s in sources], '-o', str(program)], check=True, env=env)
        result = subprocess.run([str(program), str(ROOT / 'build/host/fs/fat32.img'),
                                 str(ROOT / 'build/host/fs/fat16.img')],
                                env=env, capture_output=True, text=True, timeout=300)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H16 sec-sysvc SYSTEM services', result.stdout)


if __name__ == '__main__':
    unittest.main()
