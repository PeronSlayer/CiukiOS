"""Host-only hardware page and FAT32 cut/recovery model; no QEMU."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
class HardwareCheckTests(unittest.TestCase):
    def test_usb_enumeration_budget(self):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='hwcheck-usb-', dir=folder) as temp:
            binary = Path(temp) / 'check'
            sources = ['tests/host/hardware_check_usb_test.c', 'src/kernel/core/hardware_check_usb.c',
                       'src/kernel/fs/fs_port.c', 'src/kernel/lib/fmt.c']
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                            '-I', str(ROOT / 'src/kernel/include'), *[str(ROOT / s) for s in sources],
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0'})

    def test_page_and_crash_volume(self):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='hwcheck-', dir=folder) as temp:
            binary = Path(temp) / 'check'
            sources = ['tests/host/hardware_check_test.c', 'src/kernel/core/hardware_check_model.c',
                       'src/kernel/core/hardware_check_storage.c', 'src/kernel/lib/fmt.c',
                       *(f'src/kernel/fs/{s}.c' for s in ('fs_port', 'cache', 'fat', 'vfs', 'path', 'fs_ro', 'iso9660'))]
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                            '-I', str(ROOT / 'src/kernel/include'), *[str(ROOT / s) for s in sources],
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0'})
    def test_boot_wiring(self):
        menu=(ROOT/'src/boot/ciukldr/menu.inc').read_text()
        self.assertIn('H Hardware check',menu)
        self.assertIn('je .hardware',menu)
        probes=(ROOT/'src/kernel/probes/probes.c').read_text()
        self.assertLess(probes.index('hardware_check_run();'),probes.index('supervisor_bootstrap();'))
        init=(ROOT/'src/kernel/core/init.c').read_text()
        self.assertIn('fbdev_accel_schedule && !(g_boot.flags & CBI_F_HW_CHECK)',init)

class SavageSweepTests(unittest.TestCase):
    def test_register_matrix(self):
        folder=ROOT/'build/host'; folder.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='savage-sweep-',dir=folder) as temp:
            binary=Path(temp)/'sweep'
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                '-Wno-unused-parameter','-DFS_HOST','-D_POSIX_C_SOURCE=200809L','-pthread',
                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined',
                '-I',str(ROOT/'src/kernel/include'),str(ROOT/'tests/host/savage_sweep_test.c'),
                str(ROOT/'src/kernel/drivers/savage2d.c'),str(ROOT/'src/kernel/lib/fmt.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})

if __name__=='__main__': unittest.main()
