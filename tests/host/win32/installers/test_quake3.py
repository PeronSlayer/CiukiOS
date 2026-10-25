"""f7: the original Quake III Arena demo (1.11) to its first rendered menu frames on the host.

quake3.exe runs unmodified on the confined i386 H1 adapter in its game mode
(`game-exec`, tests/host/win32/installers/h1_game.c): the production PE
DLLs of this tree (build/apps/win32: kernel32, user32, gdi32, opengl32 with
the software renderer, dinput, dsound, wsock32, winmm, ole32 and the reused
Wine DLLs) load it, the H1 file and registry services serve C:\\Q3Ademo, and
h1_game.c stands in for the desktop (window surfaces, focus, the full-screen
lease), the thread message queue and the display query. No QEMU, no image.

The game files are the id Software demo payload and are never committed:
the test looks for the installed tree (quake3.exe, demoq3/pak0.pk3) in
$CIUKI_Q3A_DEMO, build/downloads/win32/q3a-demo-installed/Q3Ademo or
build/lead-scratch/q3-installed/Q3Ademo, and skips without it. Run:
  nice -n 19 python3 -B -m unittest tests.host.win32.installers.test_quake3
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time
import unittest

from scripts.win32 import installer_fixture as fixture
from . import host_services, test_flow_fixtures

ROOT = Path(__file__).resolve().parents[4]
OUT = test_flow_fixtures.OUT / 'quake3'
EXE_SHA256 = '2c0dd4d06e3282e0abc41ebc8e84e2d9868a17182ac175b232a9bb604a8269a3'
PAK_SIZE = 46853694
CANDIDATES = ('build/downloads/win32/q3a-demo-installed/Q3Ademo', 'build/lead-scratch/q3-installed/Q3Ademo')


def game_tree():
    """The installed demo tree, or None (never fetched or copied into tracked files)."""
    paths = ([Path(os.environ['CIUKI_Q3A_DEMO'])] if os.environ.get('CIUKI_Q3A_DEMO') else []) + \
        [ROOT / c for c in CANDIDATES]
    for path in paths:
        exe, pak = path / 'quake3.exe', path / 'demoq3/pak0.pk3'
        if exe.is_file() and pak.is_file() and pak.stat().st_size == PAK_SIZE and \
                hashlib.sha256(exe.read_bytes()).hexdigest() == EXE_SHA256:
            return path
    return None


def tga_pixels(data):
    """Uncompressed 24/32-bit TGA (Quake III screenshot): width, height, RGB rows bottom-up."""
    idlen, cmap, kind = data[0], data[1], data[2]
    width, height, bpp = struct.unpack_from('<HHB', data, 12)
    if cmap or kind != 2 or bpp not in (24, 32):
        raise ValueError('not an uncompressed true-colour TGA')
    step = bpp // 8
    pixels = data[18 + idlen:]
    return width, height, [(pixels[i + 2], pixels[i + 1], pixels[i]) for i in range(0, width * height * step, step)]


class Quake3DemoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.game = game_tree()
        if cls.game is None:
            raise unittest.SkipTest('Quake III Arena demo tree absent (install it from Q3ADemo.exe; see '
                                    'docs/validation/f7-quake3-demo-2026-10-25.md)')
        OUT.mkdir(parents=True, exist_ok=True)
        try:
            fixture.build(test_flow_fixtures.OUT / 'fixtures')
        except FileNotFoundError as error:
            raise unittest.SkipTest(str(error)) from error

    def run_game(self, arguments, timeout):
        program = test_flow_fixtures.h1_program(test_flow_fixtures.OUT / 'fixtures', self)
        library, _, _ = host_services.build_bridge(test_flow_fixtures.OUT / 'services')
        root = OUT / 'root'
        shutil.rmtree(root, ignore_errors=True)
        service = host_services.Services(root, library, program)
        service.request_timeout = timeout
        target = root / 'C/Q3Ademo/demoq3'
        target.mkdir(parents=True)
        for name in ('quake3.exe', 'demoq3/pak0.pk3'):
            done = subprocess.run(['cp', '--reflink=auto', '--', str(self.game / name), str(root / 'C/Q3Ademo' / name)])
            self.assertEqual(done.returncode, 0, name)
        frames = OUT / 'q3-frames'
        shutil.rmtree(frames, ignore_errors=True)
        work = Path.cwd()
        os.chdir(OUT)  # the adapter writes PPM frames into ./q3-frames when it exists
        try:
            began = time.monotonic()
            child = service.start_adapter(['game-exec', 'C:\\Q3Ademo\\quake3.exe',
                                           '"C:\\Q3Ademo\\quake3.exe" ' + arguments], 'quake3', cwd='C:\\Q3Ademo')
            code, output = child.finish(timeout)
            seconds = time.monotonic() - began
        finally:
            os.chdir(work)
        (OUT / 'adapter.log').write_text(output)
        log = (target / 'qconsole.log').read_text(errors='replace') if (target / 'qconsole.log').is_file() else ''
        (OUT / 'qconsole.log').write_text(log)
        return code, output, log, seconds, root

    @staticmethod
    def gl_profile(output):
        """The adapter's GL call profile: (frames, total calls, [(name, calls)] descending)."""
        total = re.search(r'^GAME gl total=(\d+) names=\d+ frames=(\d+)', output, re.M)
        calls = [(m[1], int(m[2])) for m in re.finditer(r'^GAME gl name=(\S+) calls=(\d+)', output, re.M)]
        return (int(total[2]), int(total[1]), calls) if total else (0, 0, calls)

    @staticmethod
    def profile_text(output, top=12):
        frames, total, calls = Quake3DemoTests.gl_profile(output)
        if not frames:
            return 'no GL profile'
        return f'{total / frames:.0f} GL calls/frame: ' + ', '.join(
            f'{name} {count / frames:.1f}' for name, count in calls[:top])

    def test_menu_frames_through_opengl32_and_the_lease(self):
        # Quake III's "wait N" counts Cbuf_Execute calls and Com_Frame makes
        # two per frame (qcommon/common.c), so 200 waits are about 100 frames.
        # Startup commands other than +set skip the id logo cinematic
        # (Com_Init: "cinematic idlogo.RoQ" only without them), so the
        # screenshot is the main menu at any frame rate.
        code, output, log, seconds, root = self.run_game('+set logfile 2 +wait 200 +screenshot +wait 5 +quit', 300)
        self.assertEqual(code, 0, output[-4000:] + log[-4000:])
        self.assertNotIn('REPORT action=1', output)
        # Display settings, pixel format, context and extensions (win_glimp.c GLW_* messages).
        for line in ('...calling LoadLibrary( \'C:\\WINDOWS\\SYSTEM\\opengl32.dll\' ): succeeded',
                     '...calling CDS: ok', '...created window@0,0 (640x480)', '...hardware acceleration found',
                     '...creating GL context: succeeded', '...making context current: succeeded',
                     '...using GL_ARB_multitexture', '...using GL_EXT_compiled_vertex_array',
                     'PIXELFORMAT: color(24-bits) Z(16-bit) stencil(0-bits)', 'MODE: 3, 640 x 480 fullscreen',
                     'GL_RENDERER: CiukiOS', '----- finished R_Init -----', 'Loading vm file vm/ui.qvm.'):
            self.assertIn(line, log)
        self.assertNotIn('Low Desktop Color Depth', output)
        # Frames reach the display lease (W32_DISPLAY_FRAME) at the mode the game set.
        frames = [int(n) for n in re.findall(r'^GAME frame n=(\d+) kind=lease size=640x480x32 ', output, re.M)]
        summary = re.search(r'^GAME summary frames=(\d+) .* refused_frames=(\d+) first_to_last_ms=(\d+)', output, re.M)
        self.assertTrue(frames and summary, output[-3000:])
        count, refused, span = (int(summary[i]) for i in (1, 2, 3))
        self.assertGreaterEqual(count, 80)
        self.assertEqual(refused, 0)
        # The GL call profile (export-table thunks of h1_game.c): the menu is
        # drawn with textured quads through the vertex arrays and
        # glDrawElements; the extension entry points fetched with
        # wglGetProcAddress are counted too.
        frames, total, calls = self.gl_profile(output)
        self.assertGreaterEqual(frames, count)
        named = dict(calls)
        for name in ('glDrawElements', 'glBindTexture', 'glTexCoordPointer', 'glVertexPointer', 'glColorPointer',
                     'glLockArraysEXT', 'glUnlockArraysEXT', 'glClear', 'glReadPixels', 'glTexImage2D'):
            self.assertIn(name, named, calls[:30])
        self.assertGreater(named['glDrawElements'], count)
        self.assertEqual(named['glReadPixels'], 1)  # the screenshot
        self.assertGreater(total, 20 * count)
        # The menu screenshot (glReadPixels through opengl32): the red "SINGLE
        # PLAYER" item and the black background of the main menu.
        shots = sorted((root / 'C/Q3Ademo/demoq3/screenshots').glob('shot*.tga'))
        self.assertTrue(shots, log[-2000:])
        width, height, pixels = tga_pixels(shots[0].read_bytes())
        self.assertEqual((width, height), (640, 480))
        red = sum(1 for r, g, b in pixels if r > 150 and g < 90 and b < 90)
        black = sum(1 for r, g, b in pixels if r < 16 and g < 16 and b < 16)
        self.assertGreater(red, 2000)
        self.assertGreater(black, width * height // 3)
        rate = count * 1000 / span if span else 0
        print(f'\nQuake III menu on the H1 host: {count} frames in {span} ms ({rate:.1f} frames/s), '
              f'run {seconds:.1f} s, screenshot {shots[0].name}: red {red} black {black}; '
              + self.profile_text(output))

    def test_timedemo_demo001_headless(self):
        """The arena demo as a timedemo (every demo frame rendered once, as fast
        as the renderer goes), to the console's "N frames, S seconds: R fps"
        line; "nextdemo quit" ends the program when the demo completes
        (cl_main.c CL_DemoCompleted, CL_NextDemo). Minutes to an hour on the
        software renderer: opt in with CIUKI_Q3A_TIMEDEMO=1 (a mode number,
        e.g. 0 for 320x240, selects r_mode; the default 3 is 640x480)."""
        mode = os.environ.get('CIUKI_Q3A_TIMEDEMO', '')
        if not mode:
            self.skipTest('set CIUKI_Q3A_TIMEDEMO=1 (or an r_mode number) for the timedemo evidence run')
        mode = mode if mode.isdigit() and mode != '1' else '3'
        code, output, log, seconds, root = self.run_game(
            f'+set logfile 2 +set r_mode {mode} +set nextdemo quit +set timedemo 1 +demo demo001', 3500)
        self.assertEqual(code, 0, output[-4000:] + log[-4000:])
        self.assertNotIn('REPORT action=1', output)
        result = re.search(r'^(\d+) frames, ([\d.]+) seconds: ([\d.]+) fps', log, re.M)
        self.assertTrue(result, log[-3000:])
        summary = re.search(r'^GAME summary frames=(\d+) .* refused_frames=(\d+) first_to_last_ms=(\d+) .* lease=(\S+)',
                            output, re.M)
        self.assertTrue(summary, output[-3000:])
        self.assertEqual(int(summary[2]), 0)
        for line in ('Loading vm file vm/cgame.qvm.', 'CL_InitCGame:'):
            self.assertIn(line, log)
        frames, total, calls = self.gl_profile(output)
        mode_line = re.search(r'^MODE: .*$', log, re.M)
        print(f'\nQuake III timedemo demo001 on the H1 host ({mode_line[0] if mode_line else "?"}): '
              f'{result[1]} frames, {result[2]} s: {result[3]} fps (console); presented {summary[1]} frames '
              f'in {summary[3]} ms, run {seconds:.0f} s; ' + self.profile_text(output, 24))
        (OUT / 'timedemo-profile.txt').write_text(
            f'{result[0]}\n' + '\n'.join(f'{name} {count} {count / frames:.2f}' for name, count in calls) + '\n')


if __name__ == '__main__':
    unittest.main()
