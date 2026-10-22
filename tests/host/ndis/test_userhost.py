"""ND14 hostile models and actual production mapping loops; no vendor inputs."""
import os
from pathlib import Path
import signal
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/ndis/userhost'

class UserHostTests(unittest.TestCase):
    def native(self, name, sources, sanitizer):
        OUT.mkdir(parents=True, exist_ok=True)
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-DCIUKI_NDIS_HOST_MODEL', '-fsanitize='+sanitizer,
                        '-fno-sanitize-recover=all', '-ffunction-sections', '-fdata-sections',
                        '-Wl,--gc-sections', '-I.', '-Isrc/kernel/include', '-I'+str(OUT),
                        *sources, '-o', str(OUT/name)], cwd=ROOT, check=True)
        result = subprocess.run([OUT/name], cwd=ROOT, text=True, capture_output=True, timeout=30,
                                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('PASS ', result.stdout)

    def host(self, sanitizer):
        self.native('state-'+sanitizer.replace(',', '-'),
                    ['src/kernel/ndis/userhost.c', 'src/kernel/ndis/ntimage.c',
                     'tests/host/ndis/userhost_test.c'], sanitizer)

    def ring(self, sanitizer):
        self.native('ring-'+sanitizer.replace(',', '-'),
                    ['src/kernel/ndis/packet_ring.c', 'tests/host/ndis/packet_ring_test.c'], sanitizer)

    def memory(self, sanitizer):
        OUT.mkdir(parents=True, exist_ok=True)
        # Extract production loops, following the existing bounded-memory
        # fixture pattern. Fake only the physical/CPU/registry backend.
        source = (ROOT/'src/kernel/core/vmm.c').read_text()
        alias = source.split('bool vmm_cache_alias_ok(', 1)[1].split('\nvoid vmm_init(', 1)[0]
        address = source.split('/* ---- user address spaces ---- */', 1)[1]
        (OUT/'vmm_user.inc').write_text('bool vmm_cache_alias_ok('+alias+'\n'+address)
        self.native('memory-'+sanitizer.replace(',', '-'), ['tests/host/ndis/user_memory_test.c'], sanitizer)

    def test_host_undefined(self): self.host('undefined')
    def test_host_sanitizers(self): self.host('address,undefined')
    def test_ring_undefined(self): self.ring('undefined')
    def test_ring_sanitizers(self): self.ring('address,undefined')
    def test_memory_undefined(self): self.memory('undefined')
    def test_memory_sanitizers(self): self.memory('address,undefined')

    def registry(self, sanitizer):
        self.native('registry-'+sanitizer.replace(',', '-'),
                    ['tests/host/kernel_sync_test.c', 'tests/host/entropy/unavailable_stub.c'], sanitizer)

    def test_registry_undefined(self): self.registry('undefined')
    def test_registry_sanitizers(self): self.registry('address,undefined')

    def ledger(self, sanitizer):
        self.native('ledger-'+sanitizer.replace(',', '-'), ['tests/host/ledger_test.c'], sanitizer)

    def test_ledger_undefined(self): self.ledger('undefined')
    def test_ledger_sanitizers(self): self.ledger('address,undefined')

    def test_i386_user_process(self):
        OUT.mkdir(parents=True, exist_ok=True)
        subprocess.run(['clang', '-m32', '-march=pentiumpro', '-std=c17', '-O1', '-g',
                        '-DCIUKI_NDIS_HOST_MODEL', '-fno-pie', '-no-pie',
                        '-mno-sse', '-mno-sse2', '-mno-mmx', '-Wall', '-Wextra', '-Werror',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-I.', '-Isrc/kernel/include', 'src/kernel/ndis/userhost.c',
                        'src/kernel/ndis/ntimage.c', 'tests/host/ndis/user_process_test.c',
                        '-o', str(OUT/'process-i386')], cwd=ROOT, check=True)
        result = subprocess.run([OUT/'process-i386'], cwd=ROOT, text=True,
                                capture_output=True, timeout=15)
        if result.returncode == -signal.SIGSYS:
            self.skipTest('sandbox blocks i386 syscalls; compiled, no ring-3 runtime evidence')
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('PASS ', result.stdout)

if __name__ == '__main__': unittest.main()
