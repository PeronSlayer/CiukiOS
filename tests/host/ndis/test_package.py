"""f6-07 synthetic local pack import and production target compilation."""
import os
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/ndis/package'

class PackageTests(unittest.TestCase):
    def test_installed_kernel_gate_undefined(self):
        OUT.mkdir(parents=True, exist_ok=True)
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all', '-I.', '-Isrc/kernel/include',
                        'src/kernel/ndis/package_kernel.c', 'src/kernel/ndis/package.c',
                        'src/kernel/lib/sha256.c', 'tests/host/ndis/package_gate_test.c',
                        '-o', str(OUT/'gate')], cwd=ROOT, check=True)
        r = subprocess.run([OUT/'gate'], cwd=ROOT, text=True, capture_output=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout+r.stderr)
        self.assertIn('PASS installed NDIS gate:', r.stdout)
        print(r.stdout.strip())

    def test_native_control_sources_compile_with_installed_sdk(self):
        sdk = Path(os.environ.get('CIUKI_SDK_ROOT', ROOT/'build/tools/ciuki-sdk'))
        if not (sdk/'manifest.json').exists():
            self.skipTest('set CIUKI_SDK_ROOT to an installed read-only SDK')
        manifest = json.loads((sdk/'manifest.json').read_text())
        clang = manifest['tools']['clang']['path']
        resource = subprocess.check_output([clang, '-print-resource-dir'], text=True).strip()
        out = OUT/'target'; out.mkdir(parents=True, exist_ok=True)
        for source in ('apps/control/main.c', 'apps/control/app.c', 'apps/control/driver_pack_ui.c',
                       'apps/control/driver_pack.c', 'apps/control/driver_pack_io.c',
                       'src/kernel/ndis/package.c', 'src/kernel/lib/sha256.c'):
            subprocess.run([clang, *manifest['flags'], '--sysroot='+str(sdk/'sysroot'),
                            '-nostdinc', '-isystem', str(sdk/'sysroot/include'),
                            '-isystem', resource+'/include', '-DDP_NATIVE_VFS',
                            # as apps/desktop/build_desktop.py: kernel headers after the SDK's (G30b system_settings.h)
                            '-I', str(ROOT/'apps/desktop'), '-I', str(ROOT/'sdk/win32/include'),
                            '-idirafter', str(ROOT/'src/kernel/include'),
                            '-c', source, '-o', str(out/(Path(source).stem+'.o'))],
                           cwd=ROOT, check=True)

    def packages(self, sanitizer):
        OUT.mkdir(parents=True, exist_ok=True)
        sources = ['apps/control/driver_pack.c', 'apps/control/driver_pack_io.c',
                   'src/kernel/ndis/package.c', 'src/kernel/lib/sha256.c',
                   'tests/host/ndis/package_test.c']
        command = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize='+sanitizer, '-fno-sanitize-recover=all',
                   '-I.', '-Isrc/kernel/include', *sources, '-o', str(OUT/'packages')]
        subprocess.run(command, cwd=ROOT, check=True)
        with tempfile.TemporaryDirectory(dir=OUT) as temp:
            r = subprocess.run([OUT/'packages', temp], cwd=ROOT, text=True,
                               capture_output=True, timeout=60,
                               env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))
            self.assertEqual(r.returncode, 0, r.stdout+r.stderr)
            self.assertIn('PASS driver packs:', r.stdout)
            print(r.stdout.strip())

    def test_packages_under_address_undefined_and_leak_sanitizers(self):
        self.packages('address,undefined')

    def test_packages_under_undefined_sanitizer(self):
        self.packages('undefined')
