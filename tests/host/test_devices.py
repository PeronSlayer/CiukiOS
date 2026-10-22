"""F2-41 retained registry marshalling and Control Panel host fixtures."""
# SPDX-License-Identifier: MIT
import importlib.util
import os
import shutil
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
                            str(ROOT / 'tests/host/devices_test.c'), str(ROOT / 'src/kernel/lib/fmt.c'),
                            '-Wl,--gc-sections', '-o', str(binary)], check=True, env=env)
            r = subprocess.run([binary], check=True, env=env, capture_output=True, text=True)
            self.assertIn('PASS devices:', r.stdout)
            print(r.stdout.strip())

    def test_native_readonly_log_io(self):
        with self.scratch() as temp:
            folder=Path(temp)
            fixture=folder / 'boot.log'
            data=(b'Ciuki boot log: device ready\n' * 13000)[:320*1024]
            fixture.write_bytes(data)
            fixture.chmod(0o444)
            binary=folder / 'log-reader'
            shutil.copytree(ROOT / 'sdk/sysroot-overlay/include/ciuki',folder / 'include/ciuki')
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                            '-D_POSIX_C_SOURCE=200809L','-ffunction-sections','-fdata-sections',
                            '-fsanitize=address,undefined','-fno-sanitize-recover=undefined',
                            '-I',str(folder / 'include'),'-I',str(ROOT / 'src/kernel/include'),
                            '-idirafter',str(ROOT / 'sdk/sysroot-overlay/include'),
                            str(ROOT / 'tests/host/desktop/control_log_io_test.c'),
                            '-Wl,--gc-sections','-o',str(binary)],check=True)
            result=subprocess.run([binary,fixture],capture_output=True,text=True,
                                  env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual(fixture.read_bytes(),data)
            print(result.stdout.strip())

    def test_applets_and_renders(self):
        with self.scratch() as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS='detect_leaks=0')
            binary = folder / 'control'
            icons = load('devices_icons', ROOT / 'tests/host/desktop/icon_assets.py')
            sources = ['tests/host/desktop/device_manager_test.c', 'apps/control/app.c', 'apps/control/display.c', 'apps/control/fonts.c', 'sdk/libfont/font.c', 'apps/libdeskui/text_ttf.c', 'apps/desktop/protocol.c',
                       *(f'apps/desktop/{n}.c' for n in ('settings', 'sounds', 'wallpaper', 'image', 'compositor', 'theme')),
                       *(f'apps/libdeskui/{n}.c' for n in ('draw', 'widgets', 'list', 'menu', 'dialog', 'textarea', 'icons', 'icon_map'))]
            ft = load('devices_freetype', ROOT / 'sdk/freetype/build.py'); library = ft.host()  # f2-63: libfont needs FreeType
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-D_POSIX_C_SOURCE=200809L', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                            *icons.defines(), '-I', str(ROOT / 'src/kernel/include'), '-I', str(ROOT / 'apps/control'),
                            '-idirafter', str(ROOT / 'sdk/sysroot-overlay/include'), *ft.includes(), '-pthread', *(str(ROOT / s) for s in sources), str(library), '-lm', '-o', str(binary)], check=True, env=env)
            portrait = icons.portrait()
            frames = folder / 'frames'; frames.mkdir()
            r = subprocess.run([binary, portrait, frames, ROOT / 'assets/fonts/liberation'], env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('PASS device manager:', r.stdout)
            image = load('devices_png', ROOT / 'tests/host/desktop/test_desktop.py')
            target = ROOT / 'build/host/f2-75'; target.mkdir(parents=True, exist_ok=True)
            captures = sorted(frames.glob('*.xrgb'))
            self.assertEqual(len(captures), 30)  # f1-109 firmware inventory dialog
            for path in captures:
                width, height = map(int, path.with_suffix('.size').read_text().split())
                image.control_frame(self,path.stem,path.read_bytes(),width,height)
                image.write_png(target / (path.stem + '.png'), width, height, image.xrgb_to_rgb(path.read_bytes()))
            print(r.stdout.strip())

    def test_network_worker(self):
        with self.scratch() as temp:
            binary=Path(temp)/'network-worker'
            env=dict(os.environ,TMPDIR=temp,ASAN_OPTIONS='detect_leaks=0')
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-sanitize-recover=undefined',
                '-I',str(ROOT/'src/kernel/include'),'-I',str(ROOT/'apps/control'),
                '-idirafter',str(ROOT/'sdk/sysroot-overlay/include'),
                str(ROOT/'tests/host/desktop/network_worker_test.c'),'-pthread','-o',str(binary)],check=True,env=env)
            subprocess.run([binary],check=True,env=env)

    def test_network_settings(self):
        import hashlib
        import json
        with self.scratch() as temp:
            folder=Path(temp)
            env=dict(os.environ, TMPDIR=temp, ASAN_OPTIONS='detect_leaks=0')
            binary=folder / 'network-settings'
            icons=load('network_settings_icons', ROOT / 'tests/host/desktop/icon_assets.py')
            sources=['tests/host/desktop/network_settings_test.c', 'apps/control/app.c', 'apps/control/display.c', 'apps/control/fonts.c', 'sdk/libfont/font.c', 'apps/libdeskui/text_ttf.c', 'apps/desktop/protocol.c',
                *(f'apps/desktop/{n}.c' for n in ('settings','sounds','wallpaper','image','compositor','theme')),
                *(f'apps/libdeskui/{n}.c' for n in ('draw','widgets','list','menu','dialog','textarea','icons','icon_map'))]
            ft=load('network_settings_freetype', ROOT / 'sdk/freetype/build.py'); library=ft.host()
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-D_POSIX_C_SOURCE=200809L',
                '-fsanitize=address,undefined','-fno-sanitize-recover=undefined',*icons.defines(),
                '-I',str(ROOT / 'src/kernel/include'),'-I',str(ROOT / 'apps/control'),
                '-idirafter',str(ROOT / 'sdk/sysroot-overlay/include'),*ft.includes(),'-pthread',
                *(str(ROOT / p) for p in sources),str(library),'-lm','-o',str(binary)],check=True,env=env)
            frames=folder / 'frames';frames.mkdir()
            subprocess.run([binary,frames],check=True,env=env)
            actual={p.stem:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(frames.glob('*.xrgb'))}
            self.assertEqual(len(actual),8)
            pins=ROOT / 'tests/host/desktop/network-settings-goldens.json'
            self.assertEqual(actual,json.loads(pins.read_text()))
            capture=load('network_settings_png',ROOT / 'tests/host/desktop/test_desktop.py')
            review=ROOT / 'build/host/f2-100';review.mkdir(exist_ok=True,parents=True)
            for p in frames.glob('*.xrgb'):
                capture.write_png(review/(p.stem+'.png'),560,420,capture.xrgb_to_rgb(p.read_bytes()))

if __name__ == '__main__':
    unittest.main()
