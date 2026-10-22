"""Synthetic f6-05 admission and real i386 saved-context recovery."""
import os
from pathlib import Path
import signal
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/ndis/containment'

class ContainmentTests(unittest.TestCase):
    def native(self, name, sources, sanitizers):
        OUT.mkdir(parents=True, exist_ok=True)
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize='+sanitizers, '-fno-sanitize-recover=all',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-I.', '-Isrc/kernel/include', *sources,
                        '-o', str(OUT/name)], cwd=ROOT, check=True)
        result = subprocess.run([OUT/name], capture_output=True, text=True, timeout=30,
                                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('PASS ', result.stdout)

    def test_admission_sanitizers(self):
        self.native('scan', ['src/kernel/ndis/scan.c', 'tests/host/ndis/scan_test.c'], 'address,undefined')

    def test_admission_undefined(self):
        self.native('scan-ubsan', ['src/kernel/ndis/scan.c', 'tests/host/ndis/scan_test.c'], 'undefined')

    def state(self, name, sanitizers):
        self.native(name, ['src/kernel/ndis/ntimage.c', 'src/kernel/ndis/callback.c',
                          'src/kernel/ndis/fence.c', 'tests/host/ndis/state_test.c'], sanitizers)

    def test_state_sanitizers(self):
        self.state('state', 'address,undefined')

    def test_state_undefined(self):
        self.state('state-ubsan', 'undefined')

    def boundary(self, name, sanitizers):
        self.native(name, ['src/kernel/ndis/ntimage.c', 'src/kernel/ndis/callback.c',
                          'src/kernel/ndis/execution.c', 'tests/host/ndis/boundary_test.c'], sanitizers)

    def test_boundary_sanitizers(self):
        self.boundary('boundary', 'address,undefined')

    def test_boundary_undefined(self):
        self.boundary('boundary-ubsan', 'undefined')

    def test_native_pci_transaction_sanitizers(self):
        self.native('pci', ['tests/host/ndis/pci_fence_test.c'], 'address,undefined')

    def test_native_pci_transaction_undefined(self):
        self.native('pci-ubsan', ['tests/host/ndis/pci_fence_test.c'], 'undefined')

    def test_i386_actual_faults_and_nonreturn(self):
        OUT.mkdir(parents=True, exist_ok=True)
        objects = []
        for source in ('src/kernel/arch/ndis_context.asm', 'tests/host/ndis/foreign.asm'):
            obj = OUT/(Path(source).stem+'.o'); objects.append(str(obj))
            subprocess.run(['nasm', '-f', 'elf32', source, '-o', str(obj)], cwd=ROOT, check=True)
        subprocess.run(['clang', '-m32', '-march=pentiumpro', '-std=c17', '-O2', '-g',
                        '-DCIUKI_NDIS_HOST_MODEL',
                        '-fno-pie', '-no-pie', '-mstack-alignment=4', '-mno-sse', '-mno-sse2',
                        '-mno-mmx', '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-I.', '-Isrc/kernel/include', 'src/kernel/ndis/execution.c',
                        'src/kernel/ndis/callback.c', 'src/kernel/ndis/ntimage.c',
                        'src/kernel/ndis/scan.c', 'tests/host/ndis/execution_test.c',
                        *objects, '-o', str(OUT/'execution')], cwd=ROOT, check=True)
        result = subprocess.run([OUT/'execution'], capture_output=True, text=True, timeout=15)
        if result.returncode == -signal.SIGSYS:
            self.skipTest('sandbox blocks i386 syscalls; binary compiled, execution not proved')
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('PASS i386 actual PF DE UD', result.stdout)

if __name__ == '__main__':
    unittest.main()
