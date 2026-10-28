"""H15 (sec-own): system-volume OWNER binding and first-boot publication.

The production kernel OWNER (fs/owner*.c, fs/owner_system.c) on the kernel host
harness's credentials, roles, service channels and real FAT32/VFS, and the
production ACCOUNT service (server, kernel adapter, first-boot adapter, kernel
OWNER adapter) end to end, on a FAT32 volume carrying scripts/build_image.py's
exact ready provisioning (seed, UNPROVISIONED state, volume identity).
ASan/UBSan and default leak detection are retained; fsck.fat checks the volume.
"""
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/security/sec-own'
SERVICE = ("crypto", "codec", "owner", "bootstrap", "protocol", "store", "service", "migration",
           "persist", "results", "auth", "enrollment", "manifest", "checkpoint", "logon")
OWNER = [f'src/kernel/fs/{name}.c' for name in (
    'owner', 'owner_wire', 'owner_store', 'owner_checkpoint', 'owner_roots', 'security_policy',
    'owner_vfs', 'owner_service', 'owner_system')]
SANITIZE = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
WARN = ['-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections']
ENV_MTOOLS = {'MTOOLS_SKIP_CHECK': '1', 'MTOOLS_NO_VFAT': '0', 'LC_ALL': 'C.UTF-8'}


def builder():
    spec = importlib.util.spec_from_file_location('sec_own_build_image', ROOT / 'scripts/build_image.py')
    module = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(module)
    return module


class SystemOwnerTests(unittest.TestCase):
    def volume(self):
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/fs/fixtures.py')], check=True, capture_output=True)
        OUT.mkdir(parents=True, exist_ok=True)
        image = OUT / 'system.img'
        shutil.copyfile(ROOT / 'build/host/fs/fat32.img', image)
        build = builder()
        seed = build.ready_seed()
        identity = build.volume_identity(image)
        self.assertEqual(identity, 2048 << 32 | struct.unpack_from('<I', image.read_bytes()[:512], 67)[0])
        import hashlib
        state = build.bootstrap_state(1, identity, hashlib.sha256(seed).digest())
        (OUT / 'provision.seed').write_bytes(seed)
        (OUT / 'bootstrap.state').write_bytes(state)
        (OUT / 'G1K1LEGACY.COW').write_bytes(bytes((i * 13) & 255 for i in range(4096)))
        env = dict(os.environ, **ENV_MTOOLS, MTOOLSRC=str(ROOT / 'build/host/fs/mtoolsrc'))
        for directory in ('::/System', '::/System/Security', '::/WINDOWS', '::/WINDOWS/Profiles', '::/System/DOS',
                          '::/System/DOS/SAVES', '::/Games', '::/Documents'):
            subprocess.run(['mmd', '-i', str(image), directory], check=True, env=env, capture_output=True)
        for name, target in (('provision.seed', '::/System/Security/provision.seed'),
                             ('bootstrap.state', '::/System/Security/bootstrap.state'),
                             ('G1K1LEGACY.COW', '::/System/DOS/SAVES/G1K1LEGACY.COW')):
            subprocess.run(['mcopy', '-i', str(image), str(OUT / name), target], check=True, env=env, capture_output=True)
        return image

    def test_system_owner(self):
        image = self.volume()
        env = dict(os.environ, TMPDIR=str(OUT))
        include = ['-I', str(ROOT / 'apps/accounts'), '-I', str(ROOT / 'sdk/private'),
                   '-I', str(ROOT / 'apps/desktop'), '-I', str(ROOT / 'src/kernel/include')]
        objects = []
        user = [f'apps/accounts/{name}.c' for name in SERVICE]
        user += ['apps/accounts/kernel_adapter.c', 'apps/accounts/server.c', 'apps/accounts/owner_kernel.c',
                 'apps/accounts/firstboot.c', 'sdk/private/accounts_link.c', 'apps/desktop/users.c']
        for source in user:
            obj = OUT / (source.replace('/', '_') + '.o')
            subprocess.run(['clang', *WARN, '-D_POSIX_C_SOURCE=200809L', *SANITIZE, *include,
                            '-c', str(ROOT / source), '-o', str(obj)], check=True, env=env)
            objects.append(str(obj))
        kernel = ['tests/host/security/system_owner_test.c', 'tests/host/proc/signal_legacy.c',
                  'src/kernel/fs/fs_port.c', 'src/kernel/fs/blkreq.c', 'src/kernel/fs/cache.c',
                  'src/kernel/fs/path.c', 'src/kernel/fs/fat.c', 'src/kernel/fs/vfs.c',
                  'src/kernel/fs/fs_ro.c', 'src/kernel/fs/iso9660.c', 'tests/host/fs/fake.c',
                  'src/kernel/lib/sha256.c', 'src/kernel/lib/fmt.c', 'src/kernel/lib/tzrule.c',
                  'src/kernel/probes/selector.c', *OWNER]
        program = OUT / 'system_owner'
        subprocess.run(['clang', *WARN, '-Wl,--gc-sections', *SANITIZE, '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L',
                        '-pthread', '-Wno-unused-function', *include, '-I', str(ROOT / 'src/kernel/fs'),
                        *[str(ROOT / s) for s in kernel], *objects, '-o', str(program)], check=True, env=env)
        cut = OUT / 'cut.img'
        shutil.copyfile(image, cut)
        result = subprocess.run([str(program), str(image), str(cut)], capture_output=True, text=True, env=env, timeout=300)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H15 sec-own system OWNER integration: PASS', result.stdout)
        for volume in (image, cut):
            fsck = subprocess.run(['fsck.fat', '-n', str(volume)], capture_output=True, text=True)
            self.assertEqual(fsck.returncode, 0, fsck.stdout + fsck.stderr)
        listing = subprocess.run(['mdir', '-i', str(image), '-b', '::/WINDOWS/Profiles'], capture_output=True, text=True,
                                 env=dict(os.environ, **ENV_MTOOLS, MTOOLSRC=str(ROOT / 'build/host/fs/mtoolsrc')))
        for root in ('u000001f4', 'u000001f5', 'u000003e8'):
            self.assertIn(root, listing.stdout)


if __name__ == '__main__':
    unittest.main()
