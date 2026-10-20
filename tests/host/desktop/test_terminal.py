"""Sequential f2-46 host harness using production libdeskui and pinned Lua."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import importlib.util as _review_util  # optional PNG review copies (frame_review.py, any import mode)
_review_spec = _review_util.spec_from_file_location("frame_review", __file__.rsplit("/", 1)[0] + "/frame_review.py")
_review = _review_util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)
capture = _review.capture
ROOT = Path(__file__).resolve().parents[3]

class TerminalTests(unittest.TestCase):
    def test_application(self):
        parent = ROOT / 'build/host'; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='terminal-', dir=parent) as temp:
            temp = Path(temp)
            spec = importlib.util.spec_from_file_location('terminal_lua', ROOT / 'apps/lua/build_lua.py')
            lua = importlib.util.module_from_spec(spec); spec.loader.exec_module(lua)
            archive = ROOT / 'build/downloads/newlib/lua-5.4.8.tar.gz'
            pin = json.loads((ROOT / 'config/sdk-pins.json').read_text())['lua']['source']['sha256']
            self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), pin)
            source = lua.extract(archive, temp, 'lua-5.4.8') / 'src'
            upstream = sorted(p for p in source.glob('*.c') if p.name not in ('lua.c', 'luac.c'))
            toolkit = ['draw', 'widgets', 'list', 'menu', 'dialog', 'textarea', 'icons', 'icon_map']
            sources = [*(ROOT / f'apps/libdeskui/{s}.c' for s in toolkit),
                       *(ROOT / f'apps/terminal/{s}.c' for s in ('app', 'commands', 'lua')),
                       ROOT / 'apps/tasks/model.c', ROOT / 'apps/files/model.c',
                       ROOT / 'apps/desktop/compositor.c', ROOT / 'apps/desktop/theme.c',
                       ROOT / 'tests/host/desktop/fake_vfs.c', ROOT / 'tests/host/desktop/terminal_test.c', *upstream]
            binary = temp / 'terminal'
            env = dict(os.environ, TMPDIR=str(temp), ASAN_OPTIONS='detect_leaks=0', TZ='UTC0')
            flags = ['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                     '-D_POSIX_C_SOURCE=200809L', '-DLUA_COMPAT_5_3', '-fsanitize=address,undefined',
                     '-fno-sanitize-recover=undefined', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                     '-I', str(source), '-I', str(ROOT / 'src/kernel/include')]
            subprocess.run([*flags, *(str(s) for s in sources), '-lm', '-o', str(binary)], check=True, env=env)
            frame = temp / "terminal.xrgb"
            env["CIUKI_TERMINAL_FRAME"] = str(frame)
            env["CIUKI_TERMINAL_SELECTION"] = str(temp / "selection")
            result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('terminal: PASS', result.stdout)
            print(result.stdout.strip())
            capture("terminal", frame.read_bytes(), 80 * 7 + 24, 25 * 16 + 8)
            selections = {}
            for selected in sorted(temp.glob("selection-*.xrgb")):
                pixels = selected.read_bytes()
                selections[selected.stem] = hashlib.sha256(pixels).hexdigest()
                capture("terminal-" + selected.stem, pixels, 80 * 7 + 24, 25 * 16 + 8)
            review = ROOT / "build/host/f2-68"
            review.mkdir(parents=True, exist_ok=True)
            (review / "terminal-selection-goldens.json").write_text(json.dumps(selections, indent=2, sort_keys=True) + "\n")
            pins = ROOT / "tests/host/desktop/terminal-selection-goldens.json"
            self.assertTrue(pins.exists(), json.dumps(selections, indent=2))
            self.assertEqual(selections, json.loads(pins.read_text()))

    def test_registration(self):
        for path in ('apps/build_apps.py', 'scripts/build_image.py'):
            self.assertIn('terminal', (ROOT / path).read_text())
        shell = (ROOT / 'apps/desktop/shell.c').read_text()
        self.assertIn('{"terminal", "Terminal", "utilities-terminal", "programs", NULL}', shell)
        self.assertIn('"terminal"', (ROOT / 'tests/host/desktop/shell_menu.py').read_text())
        self.assertIn('{"program:terminal", "utilities-terminal"}', (ROOT / 'apps/libdeskui/icon_map.c').read_text())
        self.assertIn('liblua.a', (ROOT / 'apps/desktop/build_desktop.py').read_text())

if __name__ == '__main__':
    unittest.main()
