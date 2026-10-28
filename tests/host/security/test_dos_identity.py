"""H06 (sec-k05): DOS VMs inherit the creator's subject; D:/E: host views,
per-user private C: data and the sector APIs (user-security-contract.md 8.4).

- identity/host views/stores: production k01 credentials and sessions, k02
  policy/owner labels, native namespace/VFS/FAT, dos_identity.c and the live
  DFV2 IFS backend over synthetic K2043 structures (dos_identity_test.c);
- VM lifecycle and BIOS INT 13h: the production dosvm/client/disk/BIOS code
  of the RK DOS VM harness (tests/host/dosvm_test.c, CIUKI_VM_IDENTITY);
- per-user title stores through the production manager (persistent saves);
- INT 25h/26h and INT 21h/7305h: the real CiukiDOS fork on libx86emu over the
  production IFS (H57 guest test absolute-sectors; heavy, needs the pinned
  Open Watcom/NASM tools and libx86emu, so it is opt-in: CIUKI_H06_FORK=1,
  and it always runs inside tests/host/dos/run_unified_file.sh).
Run one command at a time at nice 19. No QEMU or image.
SPDX-License-Identifier: GPL-2.0-only
"""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/security'
OWNER = [f'src/kernel/fs/{name}.c' for name in (
    'owner', 'owner_wire', 'owner_store', 'owner_checkpoint', 'owner_roots', 'security_policy')]
DOS = [f'src/kernel/vm/{name}.c' for name in (
    'dos_identity', 'dos_unified', 'dos_unified_ifs', 'dos_file', 'dos_file_volume', 'dos_save', 'dos_save_store')]


def run(command, env=None, timeout=900):
    result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=timeout)
    print(result.stdout, end='')
    return result


class DosIdentityTests(unittest.TestCase):
    def test_identity_views_and_stores(self):
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/fs/fixtures.py')], check=True, cwd=ROOT,
                       capture_output=True)
        OUT.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, TMPDIR=str(OUT))
        program = OUT / 'dos_identity'
        command = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                   '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread',
                   '-I', str(ROOT / 'src/kernel/include'), '-I', str(ROOT / 'src/kernel/fs'),
                   *(str(ROOT / s) for s in (
                       'tests/host/security/dos_identity_test.c', 'tests/host/proc/signal_legacy.c',
                       'tests/host/proc/media_legacy.c', 'tests/host/proc/desktop_legacy.c',
                       'src/kernel/fs/fs_port.c', 'src/kernel/fs/blkreq.c', 'src/kernel/fs/cache.c',
                       'src/kernel/fs/path.c', 'src/kernel/fs/fat.c', 'src/kernel/fs/vfs.c',
                       'src/kernel/fs/fs_ro.c', 'src/kernel/fs/iso9660.c', 'src/kernel/fs/partition.c',
                       'src/kernel/drivers/blkpart.c', 'tests/host/fs/fake.c',
                       'src/kernel/probes/f2_probes_files.c', 'src/kernel/lib/sha256.c',
                       'src/kernel/fs/owner_vfs.c', 'src/kernel/fs/security_path.c', *OWNER, *DOS)),
                   '-o', str(program)]
        subprocess.run(command, check=True, cwd=ROOT, env=env)
        result = run([str(program), str(ROOT / 'build/host/fs/fat32.img'), str(ROOT / 'build/host/fs/fat16.img')],
                     env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for line in ('H06 identity: PASS', 'H06 D: view: PASS', 'H06 E: view: PASS', 'H06 gate: PASS',
                     'H06 stores: PASS', 'H06 final:'):
            self.assertIn(line, result.stdout)

    def test_title_stores(self):
        result = run(['bash', 'tests/host/dos/run_persistent_saves.sh'])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS sec-k05 profile stores', result.stdout)

    def test_vm_lifecycle_and_int13(self):
        out = ROOT / 'build/host'
        env = dict(os.environ, TMPDIR=str(out), PYTHONDONTWRITEBYTECODE='1')
        subprocess.run(['python3', str(ROOT / 'tests/host/vm/prepare_lifecycle.py'), str(ROOT), str(out)],
                       check=True, cwd=ROOT, env=env, capture_output=True)
        objects = []
        for unit in ('vm_source', 'opl3', 'mixer'):
            obj = out / f'h06_audio_{unit}.o'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-include', str(ROOT / 'tests/host/vm_audio_shim.h'),
                            '-I', str(ROOT / 'src/kernel/include'), '-c', str(ROOT / f'src/kernel/audio/{unit}.c'),
                            '-o', str(obj)], check=True, cwd=ROOT, env=env)
            objects.append(str(obj))
        pool = subprocess.run(['python3', str(ROOT / 'tests/host/vm/pool_build.py'), str(ROOT), str(out / 'vm')],
                              check=True, cwd=ROOT, env=env, capture_output=True, text=True).stdout.strip()
        program = out / 'h06_dosvm_test'
        guest = []
        if Path('/usr/include/x86emu.h').exists():
            guest = ['-DCIUKI_DOSVM_GUEST_CPU', str(ROOT / 'tests/host/dosvm_guest_cpu.c'), '-lx86emu']
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-pointer-to-int-cast',
                        '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-I', str(ROOT / 'src/kernel/include'), '-I', str(out),
                        str(ROOT / 'tests/host/dosvm_test.c'), str(ROOT / 'tests/host/dosvm_uaddr_model.c'),
                        str(ROOT / 'src/kernel/net/ipx_config.c'),
                        str(ROOT / 'src/kernel/net/bridge.c'), str(ROOT / 'src/kernel/vm/dosvm_ipx.c'),
                        str(ROOT / 'src/kernel/vm/dosvm_ne2000.c'), str(ROOT / 'src/kernel/lib/fmt.c'),
                        str(ROOT / 'src/kernel/lib/sha256.c'), str(ROOT / 'src/kernel/vm/virtual_vga.c'),
                        str(ROOT / 'src/kernel/vm/virtual_vga_bios.c'), str(ROOT / 'src/kernel/vm/vga_x86.c'),
                        str(ROOT / 'src/kernel/vm/vga_presenter.c'), *objects, *guest, pool, '-pthread',
                        '-Wl,-u,pool_fixture_start', '-o', str(program)], check=True, cwd=ROOT, env=env)
        result = run([str(program)], env=dict(env, CIUKI_VM_IDENTITY='1'))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H06 VM identity: PASS', result.stdout)
        self.assertIn('H06 VM identity checks=', result.stdout)

    @unittest.skipUnless(os.environ.get('CIUKI_H06_FORK') == '1',
                         'fork cells run in tests/host/dos/run_unified_file.sh (set CIUKI_H06_FORK=1 here)')
    def test_absolute_sectors_on_the_fork(self):
        result = run(['bash', 'tests/host/dos/run_unified_file.sh'], timeout=5400)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
