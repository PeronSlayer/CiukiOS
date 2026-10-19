"""F4-07 production decoder/worker/UI tests; one native harness at a time."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import os
import shutil
from pathlib import Path
import subprocess
import tempfile
import unittest
import importlib.util as _review_util  # optional PNG review copies (frame_review.py, any import mode)
_review_spec = _review_util.spec_from_file_location("frame_review", __file__.rsplit("/", 1)[0] + "/frame_review.py")
_review = _review_util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)
capture = _review.capture
ROOT = Path(__file__).resolve().parents[3]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

class MusicTests(unittest.TestCase):
    def test_slow_removable_volume(self):
        parent = ROOT/'build/host'; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='music-slow-', dir=parent) as temp:
            folder = Path(temp); volume = folder/'mnt/e'; volume.mkdir(parents=True)
            # Repeat our checked-in CC0 tone, stripping its leading ID3 tag
            # and Xing count so this exercises a long stream of unknown length.
            mp3 = (ROOT/'tests/fixtures/music/tone.mp3').read_bytes()
            if mp3[:3] == b'ID3':
                size = 10 + sum(mp3[6+i] << (21-7*i) for i in range(4))
                mp3 = mp3[size:]
            mp3 = mp3.replace(b'Xing', b'Junk').replace(b'Info', b'Junk')
            (volume/'A long name with spaces - original synthetic tone.mp3').write_bytes(mp3*1024)
            (volume/'late-error.mp3').write_bytes(mp3*1024)
            (volume/'cover-art.mp3').write_bytes(b'ID3\x04\x00\x00\x00\x40\x00\x00' + bytes(1<<20) + mp3)
            nested = volume/('Long removable folder ' + 'x'*160); nested.mkdir()
            shutil.copyfile(ROOT/'tests/fixtures/music/tone.mp3', nested/'Original tone.mp3')
            shutil.copyfile(ROOT/'tests/fixtures/music/pcm-16-2-96000.wav', volume/'tone.wav')
            shutil.copyfile(ROOT/'tests/fixtures/music/tone.ogg', volume/'tone.ogg')
            for name in ('denied.mp3', 'read-error.mp3', 'invalid.mp3', 'read-error.wav', 'read-error.ogg'):
                (volume/name).write_bytes(b'not audio')
            binary = folder/'slow'
            sources = ['decoder.c', 'vorbis.c', 'playlist.c', 'player.c', 'runtime.c']
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-D_POSIX_C_SOURCE=200809L', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                '-fno-omit-frame-pointer', '-pthread', '-Wl,--wrap=fopen',
                *(str(ROOT/'apps/music'/s) for s in sources),
                str(ROOT/'tests/host/desktop/music_slow_test.c'), '-lm', '-o', str(binary)], check=True)
            result = subprocess.run([str(binary), temp], text=True, capture_output=True,
                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())
            self.assertIn('[music] open path=', result.stderr)
            self.assertTrue(all(len(line) <= 240 for line in result.stderr.splitlines()))
            self.assertIn('[music] path part=0 value=', result.stderr)
            for fmt in ('mp3', 'wav', 'ogg'):
                self.assertRegex(result.stderr, rf'\[music\] decode format={fmt} rate=\d+ channels=2 first_pcm_ms=\d+')
            self.assertIn('[music] error stage=open errno=', result.stderr)
            self.assertIn('[music] error stage=decode errno=', result.stderr)
            self.assertIn('[music] error stage=playlist-open errno=', result.stderr)
            self.assertRegex(result.stderr, r'\[music\] open path=.*denied.mp3 result=-\d+ ms=\d+')

    @unittest.skipUnless(shutil.which('ffmpeg'), 'ffmpeg needed for ten-second encoded VFS fixtures')
    def test_decoded_reservoir_storage_stalls(self):
        parent = ROOT/'build/host'; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='music-reservoir-', dir=parent) as temp:
            folder = Path(temp)
            # Independent deterministic stereo noise keeps the 10 s compressed
            # fixtures larger than read-ahead, exercising mid-song VFS reads.
            wav = folder/'stall.wav'
            subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                'anoisesrc=d=10:r=48000:a=0.2:seed=34', '-f', 'lavfi', '-i',
                'anoisesrc=d=10:r=48000:a=0.2:seed=35', '-filter_complex',
                '[0:a][1:a]amerge=inputs=2[a]', '-map', '[a]', '-c:a', 'pcm_s16le', str(wav)], check=True)
            for ext, options in [('mp3', ['-c:a', 'libmp3lame', '-b:a', '320k']),
                                 ('ogg', ['-c:a', 'libvorbis', '-q:a', '10'])]:
                subprocess.run(['ffmpeg', '-v', 'error', '-i', str(wav), *options, str(folder/f'stall.{ext}')], check=True)
            binary = folder/'reservoir'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-D_POSIX_C_SOURCE=200809L', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                '-fno-omit-frame-pointer', '-pthread', '-Wl,--wrap=fopen', '-I', str(ROOT/'apps/music'),
                *(str(ROOT/'apps/music'/s) for s in ['decoder.c', 'vorbis.c', 'playlist.c', 'player.c', 'runtime.c']),
                str(ROOT/'tests/host/desktop/music_reservoir_test.c'), '-lm', '-o', str(binary)], check=True)
            result = subprocess.run([str(binary), *(str(folder/f'stall.{ext}') for ext in ['wav', 'mp3', 'ogg']), 'starve'],
                text=True, capture_output=True, timeout=75, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())
            records = [line for line in result.stderr.splitlines() if line.startswith('[music] starve ')]
            self.assertGreaterEqual(len(records), 2)
            self.assertLessEqual(len(records), 3)
            levels = [line for line in result.stderr.splitlines() if line.startswith('[music] level ')]
            self.assertGreaterEqual(len(levels), 4)
            for record in levels:
                self.assertRegex(record, r'ms=\d+ reservoir=\d+/\d+/\d+ reader_fps=\d+ decode_us_frame=\d+\.\d{3} '
                                 r'writes_ps=\d+ bytes=\d+ queued=\d+/\d+ starvations=\d+ ui_mps=\d+ repaint_mps=\d+$')
            print('Real-codec telemetry:', levels[0])
            self.assertIn('[music] close starvations=1', result.stderr)
            self.assertTrue(all(len(line) <= 240 for line in result.stderr.splitlines()))

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

    def test_pump_publication_and_debounce(self):
        acc = load('music_policy_acc', ROOT/'tests/host/desktop/test_accessories.py')
        self.harness('policy', ['apps/music/decoder.c', 'apps/music/vorbis.c', 'apps/music/playlist.c',
                               'apps/music/player.c', 'apps/music/view.c', 'apps/accessories/acc.c',
                               *acc.TOOLKIT, 'tests/host/desktop/music_policy_test.c'])

    def test_contention_model(self):
        model = load('music_contention', ROOT/'tests/host/desktop/music_contention_model.py')
        for scale in (10, 30):
            before = model.run(legacy=True, cost_us=0.021393, scale=scale)
            after = model.run(legacy=False, cost_us=0.021393, scale=scale)
            self.assertGreater(before['starvations_per_minute'], 900)
            self.assertEqual(before['reservoir_min'], 0)
            self.assertLess(before['position_seconds'], 30)
            self.assertEqual(after['starvations_per_minute'], 0)
            self.assertEqual(after['device_gaps_per_minute'], 0)
            self.assertLess(abs(after['position_seconds'] / after['wall_seconds'] - 1), .01)
            self.assertLessEqual(after['repaints_per_second'], 2)

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
            capture(raw.stem, raw.read_bytes(), width, height)
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
            capture(raw.stem, raw.read_bytes(), width, height)
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
