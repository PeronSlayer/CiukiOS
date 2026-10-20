"""f5-12B host evidence: MCI traces, AVI playback frames, DrawDib, MCIWnd,
ACM enumeration/PCM conversion and PE inventories. Production C only; the
harnesses model kernel32 files, time, USER windows and the audio device."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import ctypes as C
import hashlib
import os
from pathlib import Path
import re
import subprocess
import unittest
from . import fixtures as F

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
OUT = ROOT / 'build/host/win32/media'
AUDIO = ROOT / 'sdk/win32/audio'
CODECS = ROOT / 'sdk/win32/codecs'
SAN = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-sanitize=function']
FLAGS = ['-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DW32_HOST_MODEL',
         '-I' + str(ROOT / 'sdk/win32/include'), '-I' + str(ROOT / 'src/kernel/include'),
         '-I' + str(CODECS)]
MCI_SOURCES = [AUDIO / n for n in ('mix.c', 'wave.c', 'midi.c', 'mci.c', 'mcistr.c', 'mcitime.c',
                                   'mciwave.c', 'mciseq.c')] + \
    [ROOT / 'sdk/win32/services/bytes.c', CODECS / 'adpcm.c', CODECS / 'acm.c', CODECS / 'pcm.c', HERE / 'host_files.c',
     HERE / 'mci_host.c']
UPDATE = os.environ.get('CIUKI_UPDATE_GOLDEN') == '1'


def env():
    # ASan, UBSan and LeakSanitizer stay enabled; the harnesses also check
    # their own allocation balance.
    return dict(os.environ, ASAN_OPTIONS='detect_leaks=1', TMPDIR=str(OUT))


def compile_program(name, sources, extra=()):
    exe = OUT / name
    subprocess.run(['clang', *FLAGS, *SAN, *extra, *map(str, sources), '-o', str(exe)],
                   check=True, env=env())
    return exe


def golden(test, name, text):
    path = HERE / 'traces' / name
    if UPDATE:
        path.parent.mkdir(exist_ok=True)
        path.write_text(text)
    test.assertTrue(path.exists(), f'missing pinned trace {path}')
    test.assertEqual(text, path.read_text(), f'trace {name} differs from the pinned one')


class Media(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        cls.fixtures = F.write_all(OUT / 'fixtures')

    def run_harness(self, exe, trace_name):
        trace = OUT / (trace_name + '.trace')
        p = subprocess.run([str(exe), str(self.fixtures), str(trace)], capture_output=True,
                           text=True, timeout=300, env=env())
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return p.stdout, trace.read_text()

    def test_mci_waveaudio_sequencer_traces(self):
        exe = compile_program('mci', [HERE / 'mci_test.c', *MCI_SOURCES])
        stdout, trace = self.run_harness(exe, 'mci')
        self.assertIn('PASS MCI', stdout)
        golden(self, 'mci.txt', trace)
        for name in ('ima.pcm frames=0..6060', 'ima.pcm frames=1000..2000', 'msadpcm.pcm frames=0..3000'):
            self.assertIn('ORACLE ' + name + ' match=1', trace)
        # Independent tempo-map oracle for "play s from 0": every MIDI message
        # fires at its tick's microsecond time, at the mixer rate.
        section = trace.split('-- play all')[1].split('NOTIFY')[0]
        frames = [int(m) for m in re.findall(r'MIDI frame=(\d+) (?:msg|sysex)', section)]
        ticks, us, end = F.song_oracle()
        rate = int(re.search(r'TRANSPORT open rate=(\d+)', trace)[1])
        self.assertEqual(len(frames), len(ticks))
        for got, want in zip(frames, us):
            self.assertLessEqual(abs((got - frames[0]) - want * rate // 1000000), 1)
        self.assertEqual(end, 1500000)
        self.assertIn('LIVE allocations=0', trace)

    def avi_program(self):
        import sys
        sys.path.insert(0, str(ROOT))
        from sdk.freetype import build as freetype
        from scripts.win32 import gdi32
        gdi = ROOT / 'build/host/win32/gdi32'
        gdi.mkdir(parents=True, exist_ok=True)
        fontlib = freetype.build(gdi / 'freetype', flags=['-O2', '-fPIC'], env=env())
        # Two groups: the GDI engine with its host test support (GNU libc
        # macros), and the media code over the CiukiOS ABI headers.
        media = [HERE / 'avi_test.c', *MCI_SOURCES, AUDIO / 'mciavi.c',
                 *[CODECS / n for n in ('avi.c', 'video.c', 'vcm.c', 'drawdib.c', 'mciwnd.c', 'acmenum.c')]]
        engine = [HERE / 'avi_gdi.c',
                  *[ROOT / 'sdk/win32/gdi32' / n for n in gdi32.SOURCES if n not in ('compiler.c', 'compiler.S')],
                  ROOT / 'sdk/win32/gdi32/native_font.c', ROOT / 'sdk/libfont/font.c']
        objects = []
        for group, extra in ((media, ['-DMCI_TEST_AVI']),
                             (engine, ['-D_GNU_SOURCE', '-idirafter', str(ROOT / 'sdk/sysroot-overlay/include'),
                                       *freetype.includes()])):
            for source in group:
                obj = OUT / ('avi-' + source.parent.name + '-' + source.stem + '.o')
                subprocess.run(['clang', *FLAGS, *SAN, *extra, '-c', str(source), '-o', str(obj)],
                               check=True, env=env())
                objects.append(str(obj))
        exe = OUT / 'avi'
        subprocess.run(['clang', *SAN, *objects, str(fontlib), '-pthread', '-o', str(exe)], check=True, env=env())
        return exe

    def test_avi_playback_drawdib_mciwnd_frames(self):
        stdout, trace = self.run_harness(self.avi_program(), 'avi')
        self.assertIn('PASS AVI', stdout)
        golden(self, 'avi.txt', trace)
        for name in ('clip-pcm.pcm', 'clip-ima.pcm'):
            self.assertRegex(trace, r'ORACLE ' + re.escape(name) + r' samples=\d+ match=1')
        # Independent pixel oracle: authored MS Video 1 fills, grayscale
        # palette, bottom-up DIB drawn top-down into 32-bit XRGB surfaces.
        def expected(frame, width, height, scale=1):
            _, pixels = F.video_frame(frame)
            out = bytearray()
            for y in range(height):
                for x in range(width):
                    sy, sx = y // scale, x // scale
                    c = pixels[(16 - 1 - sy) * 32 + sx]
                    out += bytes((c, c, c, 0))
            return bytes(out)
        cases = {'pcm-last': (5, 32, 16, 1), 'pcm-seek2': (2, 32, 16, 1), 'pcm-step4': (4, 32, 16, 1),
                 'app-2x-frame1': (1, 64, 32, 2), 'app-2x-frame3': (3, 64, 32, 2),
                 'ima-last': (5, 32, 16, 1), 'drawdib-frame4': (4, 32, 16, 1),
                 'drawdib-update': (4, 32, 16, 1), 'mciwnd-last': (5, 32, 16, 1)}
        for name, (frame, width, height, scale) in cases.items():
            with self.subTest(frame=name):
                raw = (self.fixtures / (name + '.frame')).read_bytes()
                w, h = int.from_bytes(raw[:4], 'little'), int.from_bytes(raw[4:8], 'little')
                self.assertEqual((w, h), (width, height))
                self.assertEqual(raw[8:], expected(frame, width, height, scale))
        self.assertIn('LIVE heap=0 audio=0', trace)

    def codec_library(self):
        sources = [CODECS / n for n in ('adpcm.c', 'pcm.c', 'acm.c', 'acmenum.c', 'video.c', 'vcm.c', 'avi.c')]
        sources.append(ROOT / 'tests/host/win32/codecs/host.c')
        lib = OUT / 'codecs-media.so'
        subprocess.run(['clang', *FLAGS, '-shared', '-fPIC', *map(str, sources), '-o', str(lib)], check=True, env=env())
        return C.CDLL(str(lib))

    def test_acm_discovery_and_pcm_converter(self):
        lib = self.codec_library()
        U, P, H = C.c_uint32, C.c_void_p, C.c_size_t
        # ACMDRIVERDETAILSA: 40-byte header (i386); this host build keeps
        # HICON pointer-sized, so the text follows at offset 40 + pad.
        details = C.create_string_buffer(1800 + 16)
        C.memmove(details, (920 + 4).to_bytes(4, 'little'), 4)
        lib.acmDriverDetailsA.argtypes = [H, P, U]
        for driver, support, tags in ((1, 2, 1), (2, 1, 2), (17, 1, 2)):
            C.memmove(details, (2000).to_bytes(4, 'little'), 4)
            self.assertEqual(lib.acmDriverDetailsA(driver, details, 0), 0)
            raw = details.raw
            self.assertEqual(raw[4:8], b'audc')
            self.assertEqual(int.from_bytes(raw[24:28], 'little'), support)
            self.assertEqual(int.from_bytes(raw[28:32], 'little'), tags)
        self.assertEqual(lib.acmDriverDetailsA(3, details, 0), 5)
        self.assertEqual(lib.acmDriverDetailsA(1, details, 1), 10)

        class Tag(C.Structure):
            _fields_ = [('size', U), ('index', U), ('tag', U), ('format_size', U), ('support', U),
                        ('standard', U), ('name', C.c_char * 96)]

        class Fmt(C.Structure):
            _fields_ = [('size', U), ('index', U), ('tag', U), ('support', U), ('format', P),
                        ('format_size', U), ('name', C.c_char * 256)]
        lib.acmFormatTagDetailsA.argtypes = [H, C.POINTER(Tag), U]
        t = Tag(size=72, index=1)
        self.assertEqual(lib.acmFormatTagDetailsA(2, C.byref(t), 0), 0)
        self.assertEqual((t.tag, t.format_size, t.standard, t.name), (2, 50, 8, b'Microsoft ADPCM'))
        t = Tag(size=72, index=2)
        self.assertEqual(lib.acmFormatTagDetailsA(2, C.byref(t), 0), 512)
        t = Tag(size=72, tag=0)
        self.assertEqual(lib.acmFormatTagDetailsA(0, C.byref(t), 2), 0)
        self.assertEqual((t.tag, t.format_size), (2, 50))
        t = Tag(size=72, tag=17)
        self.assertEqual(lib.acmFormatTagDetailsA(0, C.byref(t), 1), 0)
        self.assertEqual(t.name, b'IMA ADPCM')
        seen = []
        TagCB = C.CFUNCTYPE(C.c_int, H, C.POINTER(Tag), H, U)
        cb = TagCB(lambda d, p, u, f: seen.append((d, p.contents.tag, f)) or 1)
        lib.acmFormatTagEnumA.argtypes = [H, C.POINTER(Tag), TagCB, H, U]
        self.assertEqual(lib.acmFormatTagEnumA(0, C.byref(Tag(size=72)), cb, 0, 0), 0)
        self.assertEqual(seen, [(17, 17, 1), (2, 2, 1), (1, 1, 2)])
        # Every enumerated IMA/MS ADPCM standard format is decodable by the
        # stream API (query open) and describes itself consistently.
        lib.acmFormatEnumA.argtypes = [H, C.POINTER(Fmt), C.CFUNCTYPE(C.c_int, H, C.POINTER(Fmt), H, U), H, U]
        lib.acmStreamOpen.argtypes = [C.POINTER(H), H, P, P, P, H, H, U]
        lib.acmFormatSuggest.argtypes = [H, P, P, U, U]
        FmtCB = C.CFUNCTYPE(C.c_int, H, C.POINTER(Fmt), H, U)
        for tag, count in ((17, 8), (2, 8), (1, 16)):
            found = []
            wave = C.create_string_buffer(64)
            C.memmove(wave, tag.to_bytes(2, 'little'), 2)
            def collect(d, p, u, f, found=found):
                found.append((bytes(C.string_at(p.contents.format, 50)), p.contents.name))
                return 1
            fcb = FmtCB(collect)
            f = Fmt(size=152, format=C.cast(wave, P), format_size=64)
            self.assertEqual(lib.acmFormatEnumA(0, C.byref(f), fcb, 0, 0x10000), 0)
            self.assertEqual(len(found), count)
            for fmt, name in found:
                src = C.create_string_buffer(fmt, 64)
                dst = C.create_string_buffer(64)
                self.assertEqual(lib.acmFormatSuggest(0, src, dst, 18, 0), 0, name)
                self.assertEqual(lib.acmStreamOpen(None, 0, src, dst, None, 0, 0, 1), 0, name)
                self.assertRegex(name.decode(), r'^\d+\.\d{3} kHz, (4|8|16) Bit, (Mono|Stereo)$')
        # CONVERT from IMA 22.05 kHz mono reaches only PCM16 at that layout.
        ima = bytes(F.wav_adpcm(17, 1, 22050, 256, 1)[0][20:40])
        src = C.create_string_buffer(ima, 64)
        found = []
        fcb = FmtCB(lambda d, p, u, f2: found.append(bytes(C.string_at(p.contents.format, 16))) or 1)
        f = Fmt(size=152, format=C.cast(src, P), format_size=64)
        self.assertEqual(lib.acmFormatEnumA(0, C.byref(f), fcb, 0, 0x100000), 0)
        self.assertEqual([x[:4] + x[12:16] for x in found], [bytes((1, 0, 1, 0)) + bytes((2, 0, 16, 0))])

        # PCM converter: 8-bit stereo 11025 -> 16-bit mono 22050, oracle:
        # out[n] = mean(src[floor(n * 11025 / 22050)]), 8-bit as (v-128)*256.
        import struct
        def wave(ch, rate, bits):
            align = ch * bits // 8
            return struct.pack('<HHIIHHH', 1, ch, rate, rate * align, align, bits, 0)
        src_fmt, dst_fmt = wave(2, 11025, 8), wave(1, 22050, 16)
        data = bytes((i * 7 + 3) & 255 for i in range(2 * 301))
        frames = len(data) // 2
        def oracle(n_out):
            out = []
            for n in range(n_out):
                k = n * 11025 // 22050
                l, r = (data[2 * k] - 128) * 256, (data[2 * k + 1] - 128) * 256
                out.append(int((l + r) / 2))
            return struct.pack('<%dh' % n_out, *out)
        h = H()
        self.assertEqual(lib.acmStreamOpen(C.byref(h), 0, src_fmt, dst_fmt, None, 0, 0, 0), 0)
        size = U()
        lib.acmStreamSize.argtypes = [H, U, C.POINTER(U), U]
        self.assertEqual(lib.acmStreamSize(h, len(data), C.byref(size), 0), 0)
        self.assertEqual(size.value, frames * 2 * 2)

        class Header(C.Structure):
            _pack_ = 1
            _fields_ = [('size', U), ('status', U), ('user', H), ('src', P), ('src_len', U), ('src_used', U),
                        ('src_user', H), ('dst', P), ('dst_len', U), ('dst_used', U), ('dst_user', H),
                        ('reserved', U * 10)]
        for proc in ('acmStreamPrepareHeader', 'acmStreamConvert', 'acmStreamUnprepareHeader'):
            getattr(lib, proc).argtypes = [H, C.POINTER(Header), U]
        produced = b''
        for part in (data[:2 * 100], data[2 * 100:]):  # split: phase continuity
            sbuf = C.create_string_buffer(part, len(part))
            dbuf = C.create_string_buffer(4096)
            hd = Header(size=C.sizeof(Header), src=C.addressof(sbuf), src_len=len(part), dst=C.addressof(dbuf),
                        dst_len=4096)
            self.assertEqual(lib.acmStreamPrepareHeader(h, C.byref(hd), 0), 0)
            self.assertEqual(lib.acmStreamConvert(h, C.byref(hd), 0), 0)
            self.assertEqual(hd.src_used, len(part))
            produced += dbuf.raw[:hd.dst_used]
            self.assertEqual(lib.acmStreamUnprepareHeader(h, C.byref(hd), 0), 0)
        self.assertEqual(produced, oracle(frames * 2))
        self.assertEqual(lib.acmStreamClose(h, 0), 0)
        # acmFormatSuggest honours requested PCM fields for a PCM source.
        dst = C.create_string_buffer(struct.pack('<HHIIHHH', 1, 2, 44100, 0, 0, 16, 0), 18)
        self.assertEqual(lib.acmFormatSuggest(0, src_fmt, dst, 18, 0x20000 | 0x40000 | 0x80000), 0)
        self.assertEqual(struct.unpack('<HHIIHH', dst.raw[:16]), (1, 2, 44100, 176400, 4, 16))

    def test_avi_sample_time_conversion(self):
        lib = self.codec_library()
        P = C.c_void_p
        data = (self.fixtures / 'clip-pcm.avi').read_bytes()
        buf = C.create_string_buffer(data, len(data))
        lib.test_file.argtypes = [P, C.c_uint32]
        lib.test_file(buf, len(data))
        f, v = P(), P()
        lib.AVIFileOpenA.argtypes = [C.POINTER(P), C.c_char_p, C.c_uint32, P]
        lib.AVIFileGetStream.argtypes = [P, C.POINTER(P), C.c_uint32, C.c_int32]
        for name in ('AVIStreamSampleToTime', 'AVIStreamTimeToSample'):
            getattr(lib, name).argtypes = [P, C.c_int32]
            getattr(lib, name).restype = C.c_int32
        lib.AVIFileRelease.argtypes = lib.AVIStreamRelease.argtypes = [P]
        self.assertEqual(lib.AVIFileOpenA(C.byref(f), b'clip.avi', 0, None), 0)
        self.assertEqual(lib.AVIFileGetStream(f, C.byref(v), int.from_bytes(b'vids', 'little'), 0), 0)
        # 15 frames per second (dwScale 1, dwRate 15), six frames.
        for sample, ms in ((0, 0), (3, 200), (6, 400), (99, 400), (-5, 0)):
            self.assertEqual(lib.AVIStreamSampleToTime(v, sample), ms)
        for ms, sample in ((0, 0), (250, 3), (266, 3), (267, 4), (5000, 6)):
            self.assertEqual(lib.AVIStreamTimeToSample(v, ms), sample)
        self.assertEqual(lib.AVIStreamTimeToSample(v, -1), -1)
        self.assertEqual(lib.AVIStreamSampleToTime(None, 1), -1)
        lib.AVIStreamRelease(v)
        lib.AVIFileRelease(f)

    def test_avi_fuzz(self):
        exe = self.avi_program()
        p = subprocess.run([str(exe), str(self.fixtures), 'unused', '3000'], capture_output=True, text=True,
                           timeout=900, env=env())
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('PASS AVI fuzz', p.stdout)

    def test_pe_store_exports_imports_coverage_and_i686(self):
        import json
        import sys
        sys.path.insert(0, str(ROOT))
        from scripts.win32 import builtins, census, codecs, stubs
        out = OUT / 'store'
        manifest = builtins.build(out)
        stubs.verify_store(manifest)
        store = json.loads(manifest.read_text())
        exports = {d['name']: {n for e in census.inspect((out / d['path']).read_bytes())['exports'] for n in e['names']}
                   for d in store['dlls']}
        self.assertEqual(exports['mciavi32.dll'], {'CiukiMciDriver'})
        self.assertTrue({'mciSendStringA', 'mciSendStringW', 'mciSendCommandA', 'mciSendCommandW', 'mciGetDeviceIDA',
                         'mciGetDeviceIDW', 'mciGetErrorStringA', 'mciGetErrorStringW', 'mciGetCreatorTask',
                         'mciSetYieldProc', 'mciGetYieldProc', 'mciExecute', 'midiStreamOpen'} <= exports['winmm.dll'])
        for dll in ('winmm.dll', 'mciavi32.dll', 'msvfw32.dll', 'msacm32.dll', 'avifil32.dll'):
            for imported in census.inspect((out / dll).read_bytes())['imports']:
                for symbol in imported.get('symbols', []):
                    self.assertIn(symbol, exports[imported['dll'].lower()], dll)
        # Codec images keep importing kernel32 (and msvfw32); GDI, USER and
        # MCI entry points are bound by name at first use: those names must
        # be exported by the store and present in the image.
        image = (out / 'msvfw32.dll').read_bytes()
        for dll, names in codecs.BOUND.items():
            for name in names:
                self.assertIn(name, exports[dll])
                self.assertIn(name.encode() + b'\0', image)
        inventory = {d['name']: d['coverage'] for d in json.loads((out / 'codecs-inventory.json').read_text())['dlls']}
        implemented = {'msvfw32.dll': ('ICInfo', 'MCIWndCreateA', 'MCIWndCreateW', 'MCIWndCreate',
                                       'MCIWndRegisterClass', 'DrawDibOpen', 'DrawDibBegin', 'DrawDibDraw',
                                       'DrawDibEnd', 'DrawDibClose', 'ICDecompress', 'ICSendMessage', 'ICOpen'),
                       'msacm32.dll': ('acmDriverDetailsA', 'acmDriverDetailsW', 'acmFormatTagEnumA',
                                       'acmFormatEnumA', 'acmFormatDetailsA', 'acmFormatTagDetailsA',
                                       'acmStreamOpen', 'acmStreamConvert', 'acmDriverEnum'),
                       'avifil32.dll': ('AVIStreamSampleToTime', 'AVIStreamTimeToSample', 'AVIStreamGetFrame',
                                        'AVIFileOpenA', 'AVIStreamRead')}
        for dll, names in implemented.items():
            for name in names:
                self.assertEqual(inventory[dll][name], 'implemented', dll + ' ' + name)
        census_winmm = json.loads((out / 'winmm.dll-coverage.json').read_text())
        self.assertEqual({k for k, v in census_winmm.items() if v != 'implemented'}, {'#2'})
        required = stubs.imports((ROOT / 'docs/design/research/win32-layer-2026-10-14.md').read_text())
        for name in required['msvfw32.dll']:
            self.assertEqual(inventory['msvfw32.dll'][name], 'implemented')
        for dll in ('mciavi32.dll', 'winmm.dll', 'msvfw32.dll', 'msacm32.dll', 'avifil32.dll'):
            text = subprocess.run(['llvm-objdump', '-d', str(out / dll)], capture_output=True, text=True,
                                  check=True).stdout
            self.assertNotRegex(text, r'\b(?:xmm\d|mm[0-7])\b', dll)

    def test_midi_and_mci_string_fuzz(self):
        exe = compile_program('mci-fuzz', [HERE / 'mci_fuzz.c', *MCI_SOURCES])
        p = subprocess.run([str(exe), str(self.fixtures), '20000'], capture_output=True, text=True,
                           timeout=600, env=env())
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('PASS MCI fuzz', p.stdout)


if __name__ == '__main__':
    unittest.main()
