"""Synthetic upper edge, no owner pack input or runtime qualification."""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/ndis/miniport'

class MiniportTests(unittest.TestCase):
    def run_model(self, sanitizers):
        OUT.mkdir(parents=True, exist_ok=True)
        binary = OUT / ('model-' + sanitizers.replace(',', '-'))
        sources = ['userhost', 'ntimage', 'nt_runtime', 'nt_services', 'exports',
                   'miniport', 'ndis_services', 'channel', 'packet_ring']
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-DCIUKI_NDIS_HOST_MODEL', '-fsanitize=' + sanitizers,
                        '-fno-sanitize-recover=all', '-ffunction-sections', '-fdata-sections',
                        '-Wl,--gc-sections', '-I.', '-Isrc/kernel/include',
                        *['src/kernel/ndis/' + s + '.c' for s in sources],
                        'tests/host/ndis/miniport_test.c', '-o', str(binary)], cwd=ROOT, check=True)
        result = subprocess.run([binary], cwd=ROOT, capture_output=True, text=True,
                                timeout=30, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS synthetic NDIS', result.stdout)

    def test_undefined(self): self.run_model('undefined')
    def test_sanitizers(self): self.run_model('address,undefined')

if __name__ == '__main__': unittest.main()
