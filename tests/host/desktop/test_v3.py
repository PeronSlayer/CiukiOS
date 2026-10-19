"""Protocol v3 production harness; deterministic frames are host evidence only."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import unittest
from .test_desktop import ROOT, SHELL_SOURCES, load
from .frame_review import capture


class DesktopV3Tests(unittest.TestCase):
    def test_protocol_lifecycle_input_shell_and_frames(self):
        out = ROOT / 'build/codex/f5-09-frames'
        out.mkdir(parents=True, exist_ok=True)
        work = ROOT / 'build/host/desktop-v3'
        work.mkdir(parents=True, exist_ok=True)
        portrait = work / 'portrait.xrgb'
        converter = load('v3_portrait', ROOT / 'apps/desktop/convert_portrait.py')
        portrait.write_bytes(converter.convert(ROOT / 'assets/brand/ciuki-logo.png'))
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
        binary = work / 'v3-test'
        sources = [ROOT / 'apps/desktop' / n for n in ('protocol.c', 'client.c', 'input.c', 'compositor.c', 'theme.c')]
        sources += SHELL_SOURCES
        sources += [ROOT / 'apps/libdeskui' / n for n in ('icons.c', 'icon_map.c', 'v3.c')]
        sources += [ROOT / 'tests/host/desktop' / n for n in ('fake.c', 'v3_test.c')]
        subprocess.run(['cc', '-std=c17', '-D_POSIX_C_SOURCE=200809L', '-include',
                        str(ROOT / 'tests/host/desktop/replay_base.h'), '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(ROOT / 'apps/desktop'), '-I', str(ROOT / 'src/kernel/include'),
                        *(str(s) for s in sources), '-o', str(binary)], check=True, env=env)
        result = subprocess.run([str(binary), str(portrait), str(out)], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (ROOT / 'build/codex/f5-09-v3.log').write_text(result.stdout + result.stderr)
        record = {}
        for theme in ('ciuki-blue', 'platinum'):
            for case in ('overlapped', 'popup', 'tool', 'topmost', 'alt-tab', 'alt-tab-committed', 'alt-enter', 'ctrl-alt-del', 'programs-folder', 'crash', 'crash-details', 'restart',*[f'refuse-{i}' for i in range(1,8)]):
                name = f'{theme}-{case}'
                data = (out / (name + '.xrgb')).read_bytes()
                self.assertEqual(len(data), 800 * 600 * 4)
                record[name] = hashlib.sha256(data).hexdigest()
                capture('f5-09-' + name, data, 800, 600)
        goldens = ROOT / 'tests/host/desktop/v3-goldens.json'
        self.assertEqual(record, json.loads(goldens.read_text()))

    def test_add_remove_page(self):
        out = ROOT / 'build/codex/f5-09-frames'
        out.mkdir(parents=True, exist_ok=True)
        work = ROOT / 'build/host/desktop-v3'
        work.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
        icons = load('v3_control_icons', ROOT / 'tests/host/desktop/icon_assets.py')
        ft = load('v3_control_ft', ROOT / 'sdk/freetype/build.py')
        library = ft.host()
        sources = ['tests/host/desktop/v3_control_test.c', 'apps/control/app.c', 'apps/control/display.c',
                   'apps/control/fonts.c', 'sdk/libfont/font.c', 'apps/libdeskui/text_ttf.c', 'apps/desktop/protocol.c',
                   *(f'apps/desktop/{n}.c' for n in ('settings', 'sounds', 'wallpaper', 'image', 'compositor', 'theme')),
                   *(f'apps/libdeskui/{n}.c' for n in ('draw', 'widgets', 'list', 'menu', 'dialog', 'textarea', 'icons', 'icon_map'))]
        binary = work / 'v3-control-test'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-D_POSIX_C_SOURCE=200809L',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', *icons.defines(),
                        '-I', str(ROOT / 'apps/control'), '-I', str(ROOT / 'apps/libdeskui'),
                        '-I', str(ROOT / 'src/kernel/include'), '-idirafter', str(ROOT / 'sdk/sysroot-overlay/include'),
                        *ft.includes(), *(str(ROOT / s) for s in sources), str(library), '-pthread', '-lm', '-o', str(binary)],
                       check=True, env=env)
        result = subprocess.run([str(binary), str(out)], capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (ROOT / 'build/codex/f5-09-control.log').write_text(result.stdout + result.stderr)
        record = {}
        for theme in ('ciuki-blue', 'platinum'):
            name = theme + '-add-remove'
            data = (out / (name + '.xrgb')).read_bytes()
            self.assertEqual(len(data), 560 * 420 * 4)
            record[name] = hashlib.sha256(data).hexdigest()
            capture('f5-09-' + name, data, 560, 420)
        self.assertEqual(record, json.loads((ROOT / 'tests/host/desktop/v3-control-goldens.json').read_text()))
