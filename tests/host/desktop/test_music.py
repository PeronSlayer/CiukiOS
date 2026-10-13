"""F4-07 production decoder/worker/UI tests; one native harness at a time."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

class MusicTests(unittest.TestCase):
    def harness(self, name, sources, *args):
        parent = ROOT/'build/host'; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='music-', dir=parent) as temp:
            binary = Path(temp)/name
            include = Path(temp)/'include/ciuki'; include.mkdir(parents=True)
            (include/'audio.h').write_bytes((ROOT/'sdk/sysroot-overlay/include/ciuki/audio.h').read_bytes())
            flags = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-D_POSIX_C_SOURCE=200809L',
                     '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer',
                     '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-pthread',
                     '-I', str(ROOT/'apps/accessories'), '-I', str(Path(temp)/'include'), '-I', str(ROOT/'src/kernel/include'), '-Wno-macro-redefined']
            icons = load('music_icons', ROOT/'tests/host/desktop/icon_assets.py')
            subprocess.run([*flags, *icons.defines(), *(str(ROOT/s) for s in sources), '-lm', '-o', str(binary)], check=True)
            extra = ([temp, str(ROOT/"misc/CiukiOS_SplashScreen.png"), *map(str,args)] if name == "viewer"
                     else [*map(str,args), *([temp] if name == "engine" else [])])
            subprocess.run([str(binary), *extra], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))

    def test_engine(self):
        self.harness('engine', ['apps/music/decoder.c', 'apps/music/vorbis.c', 'apps/music/playlist.c', 'apps/music/player.c',
                               'apps/music/runtime.c', 'tests/host/desktop/music_test.c'], ROOT/'tests/fixtures/music')

    def test_native_adapters(self):
        self.harness('native', ['tests/host/desktop/music_native_test.c', 'apps/desktop/protocol.c'])

    def test_ui_renders(self):
        acc = load('music_acc', ROOT/'tests/host/desktop/test_accessories.py')
        out = ROOT/'build/f4-07'; out.mkdir(parents=True, exist_ok=True)
        self.harness('view', ['apps/music/view.c', 'apps/accessories/acc.c', *acc.TOOLKIT,
                             'tests/host/desktop/music_view_test.c'], out)
        from PIL import Image
        for raw in out.glob('music-*.xrgb'):
            width, height = map(int, raw.with_suffix('.size').read_text().split())
            Image.frombytes('RGB', (width,height), raw.read_bytes(), 'raw', 'BGRX').save(raw.with_suffix('.png'))
            raw.unlink(); raw.with_suffix('.size').unlink()

    def test_viewer_renders(self):
        acc = load('music_viewer_acc', ROOT/'tests/host/desktop/test_accessories.py')
        out = ROOT/'build/f4-07'; out.mkdir(parents=True, exist_ok=True)
        self.harness('viewer', ['apps/accessories/viewer_model.c', 'apps/accessories/viewer_view.c',
                               'apps/accessories/viewer_stb.c', 'apps/accessories/acc.c', *acc.TOOLKIT,
                               'tests/host/desktop/viewer_test.c'], out)
        from PIL import Image
        for raw in out.glob('viewer-*.xrgb'):
            width, height = map(int, raw.with_suffix('.size').read_text().split())
            Image.frombytes('RGB', (width,height), raw.read_bytes(), 'raw', 'BGRX').save(raw.with_suffix('.png'))
            raw.unlink(); raw.with_suffix('.size').unlink()

    def test_provenance_and_registration(self):
        expected = {'third_party/minimp3/minimp3.h': '57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad',
                    'third_party/minimp3/LICENSE': '36ffd9dc085d529a7e60e1276d73ae5a030b020313e6c5408593a6ae2af39673',
                    'third_party/stb/stb_vorbis.c': '4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636'}
        for path, digest in expected.items(): self.assertEqual(hashlib.sha256((ROOT/path).read_bytes()).hexdigest(),digest)
        shell = (ROOT/'apps/desktop/shell.c').read_text()
        self.assertIn('{"music", "Music Player", "audio-x-generic", "programs"}',shell)
        self.assertIn('{"viewer", "Image Viewer", "image-x-generic", "programs"}',shell)
        self.assertIn('{"viewer", "Image Viewer", "image-x-generic", "accessories"}',shell)
        self.assertIn('if (d->pictures_exists) icon_add(d, "Pictures"',shell)
        image = (ROOT/'scripts/build_image.py').read_text()
        for token in ('"/Documents/Pictures"', '"/Documents/Music"', '"/Programs/music"', '"/system/licenses/minimp3.txt"', '"/system/licenses/stb_vorbis.txt"'):
            self.assertIn(token,image)

if __name__ == '__main__': unittest.main()
