"""F2-41 retained registry marshalling and Control Panel host fixtures."""
# SPDX-License-Identifier: MIT
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

class DeviceTests(unittest.TestCase):
    def scratch(self):
        parent = ROOT / 'build/host'
        parent.mkdir(parents=True, exist_ok=True)
        return tempfile.TemporaryDirectory(prefix='devices-', dir=parent)

    def test_registry_query(self):
        with self.scratch() as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS='detect_leaks=0')
            binary = folder / 'devices'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=undefined', '-I', str(ROOT / 'src/kernel/include'),
                            str(ROOT / 'tests/host/devices_test.c'), '-Wl,--gc-sections', '-o', str(binary)], check=True, env=env)
            r = subprocess.run([binary], check=True, env=env, capture_output=True, text=True)
            self.assertIn('PASS devices:', r.stdout)
            print(r.stdout.strip())

    def test_applets_and_renders(self):
        with self.scratch() as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS='detect_leaks=0')
            binary = folder / 'control'
            icons = load('devices_icons', ROOT / 'tests/host/desktop/icon_assets.py')
            sources = ['tests/host/desktop/device_manager_test.c', 'apps/control/app.c', 'apps/control/display.c', 'apps/control/fonts.c', 'sdk/libfont/font.c', 'apps/libdeskui/text_ttf.c', 'apps/desktop/protocol.c',
                       *(f'apps/desktop/{n}.c' for n in ('settings', 'sounds', 'wallpaper', 'image', 'compositor', 'theme')),
                       *(f'apps/libdeskui/{n}.c' for n in ('draw', 'widgets', 'list', 'menu', 'dialog', 'textarea', 'icons', 'icon_map'))]
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-D_POSIX_C_SOURCE=200809L', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                            *icons.defines(), '-I', str(ROOT / 'src/kernel/include'), '-I', str(ROOT / 'apps/control'),
                            '-idirafter', str(ROOT / 'sdk/sysroot-overlay/include'), '-pthread', *(str(ROOT / s) for s in sources), '-lm', '-o', str(binary)], check=True, env=env)
            portrait = icons.portrait()
            frames = folder / 'frames'; frames.mkdir()
            r = subprocess.run([binary, portrait, frames, ROOT / 'assets/fonts/liberation'], env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('PASS device manager:', r.stdout)
            image = load('devices_png', ROOT / 'tests/host/desktop/test_desktop.py')
            target = ROOT / 'build/f2-41'; target.mkdir(parents=True, exist_ok=True)
            captures = sorted(frames.glob('*.xrgb'))
            self.assertEqual(len(captures), 21)  # f2-51 AutoPlay page
            for path in captures:
                width, height = map(int, path.with_suffix('.size').read_text().split())
                image.write_png(target / (path.stem + '.png'), width, height, image.xrgb_to_rgb(path.read_bytes()))
            print(r.stdout.strip())

if __name__ == '__main__':
    unittest.main()
