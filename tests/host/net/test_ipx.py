"""Real shared packet allocator, endpoint protocol and DOS bridge/API."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]


class IpxTests(unittest.TestCase):
    def test_existing_dos_and_bridge(self):
        out = ROOT / 'build/host/net-ipx'
        out.mkdir(parents=True, exist_ok=True)
        pool = subprocess.check_output(['python3', str(ROOT / 'tests/host/vm/pool_build.py'),
                                        str(ROOT), str(out)], text=True).strip()
        for name in ('bridge', 'ipx'):
            with self.subTest(fixture=name):
                binary = out / ('legacy-' + name)
                subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-pthread',
                    '-I', str(ROOT / 'src/kernel/include'),
                    str(ROOT / ('tests/host/vm/' + name + '_test.c')),
                    str(ROOT / 'src/kernel/net/bridge.c'),
                    str(ROOT / 'src/kernel/vm/dosvm_ipx.c'), pool, '-Wl,-u,pool_fixture_start',
                    '-o', str(binary)], check=True)
                subprocess.run(['stdbuf', '-oL', str(binary)], check=True, timeout=30)

    def test_ipx_spx(self):
        out = ROOT / 'build/host/net-ipx'
        out.mkdir(parents=True, exist_ok=True)
        pool = subprocess.check_output(['python3', str(ROOT / 'tests/host/vm/pool_build.py'),
                                        str(ROOT), str(out)], text=True).strip()
        binary = out / 'ipx-test'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-pthread',
                        '-I', str(ROOT / 'src/kernel/include'),
                        *[str(ROOT / n) for n in ('tests/host/net/ipx_test.c',
                           'src/kernel/net/ipx.c', 'src/kernel/net/bridge.c',
                           'src/kernel/vm/dosvm_ipx.c')], pool, '-Wl,-u,pool_fixture_start',
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
