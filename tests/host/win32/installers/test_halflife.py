"""fx-hlrun: Half-Life (Sierra 1998 CD) after its InstallShield 5 install, on the H1 host.

Local only. The original CD is read in place at /run/media/peronslayer/Halflife
(read-only D:, test_is5boot.cd_services; a CD-ROM drive with its label);
the install lands in a local service root under build/ ($CIUKI_HL_ROOT,
default build/host/win32/halflife/root), which is never committed and is
deleted when the work ends (`CIUKI_HL_CLEAN=1` removes it after the run).
Without an installed tree the tests skip unless CIUKI_IS5_INSTALL=1 asks for
the install (about 400 MB, three minutes); the registry of that fresh
install is kept beside it and every game run starts from it.

hl.exe then runs unmodified in the H1 adapter's game mode (h1_game.c): the
production PE DLLs of this tree, Win32 threads as Linux threads, the H1
file/registry services, the stand-in desktop and the disc's CD drive (its
TOC read from the host drive). Frames are recorded as checksums; PNG copies
of sampled frames go to build/hlrun-frames for the lead, never committed.
The CD key is read from build/host/win32/halflife/cdkey (mode 600) and typed
without key records. No commercial bytes or text are logged: game output is
reduced to CiukiOS's own adapter records.
  nice -n 19 python3 -B -m unittest tests.host.win32.installers.test_halflife
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import unittest

from . import test_is5boot

ROOT = Path(__file__).resolve().parents[4]
CD = test_is5boot.CD
OUT = ROOT / 'build/host/win32/halflife'
INSTALLED = Path(os.environ.get('CIUKI_HL_ROOT', OUT / 'root'))
VOLUME = (4 << 30, 8 << 30)
# The game asks for its CD key at the first start (the install stores none;
# Cancel quits). The owner's key is read from this local file (one line,
# mode 600) or from $CIUKI_HL_CDKEY; it is typed by the harness without key
# records and never printed or stored outside the scratch registry.
CDKEY = OUT / 'cdkey'
REGISTRY = OUT / 'hives-installed'  # the registry of the fresh install (no CD key stored)


def cd_key_file():
    """A path holding the CD key for the adapter, or None (caller removes a temporary copy)."""
    if os.environ.get('CIUKI_HL_CDKEY'):
        path = OUT / 'run' / '.cdkey'
        path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as f:
            f.write(os.environ['CIUKI_HL_CDKEY'].strip() + '\n')
        return path, True
    if CDKEY.is_file():
        return CDKEY, False
    return None, False


def cd_toc():
    """The mounted disc's table of contents as the adapter option cdtoc=
    (first,last,lead-out LBA, then LBA:control of every track), read from
    the host drive behind the mount (Linux CDROMREADTOCHDR 0x5305 and
    CDROMREADTOCENTRY 0x5306 in LBA format), or None for an image without a
    drive (an ISO file has no audio tracks). Half-Life's disc check compares
    the audio track lengths through MCI cdaudio."""
    import fcntl
    import struct
    try:
        source = subprocess.run(['findmnt', '-no', 'SOURCE', str(CD)], capture_output=True, text=True,
                                timeout=10).stdout.strip()
        if not source.startswith('/dev/sr'):
            return None
        fd = os.open(source, os.O_RDONLY | os.O_NONBLOCK)
    except (OSError, subprocess.SubprocessError):
        return None
    try:
        header = bytearray(2)
        fcntl.ioctl(fd, 0x5305, header)
        first, last = header
        entries = []
        for track in list(range(first, last + 1)) + [0xaa]:
            entry = bytearray(12)
            entry[0], entry[2] = track, 1  # CDROM_LBA
            fcntl.ioctl(fd, 0x5306, entry)
            entries.append((struct.unpack_from('<i', entry, 4)[0], entry[1] >> 4))
    except OSError:
        return None
    finally:
        os.close(fd)
    return 'cdtoc=%d,%d,%d,%s' % (first, last, entries[-1][0], ','.join('%d:%d' % e for e in entries[:-1]))


SHOTS = ROOT / 'build/hlrun-frames'  # local-only screenshots for the lead and the owner


def ppm_to_png(source, target):
    """A binary PPM (P6, 8-bit) written by the adapter, as an RGB PNG (stdlib only)."""
    import struct
    import zlib
    data = source.read_bytes()
    fields, at = [], 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        end = at
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[at:end])
        at = end
    at += 1
    if fields[0] != b'P6' or fields[3] != b'255':
        raise ValueError('not an 8-bit P6 PPM')
    width, height = int(fields[1]), int(fields[2])
    pixels = data[at:at + width * height * 3]
    raw = b''.join(b'\0' + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
    target.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                       chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
    return width, height


def screenshots(name, run=None):
    """The run's frames (RUN/q3-frames, default build/host/win32/halflife/run)
    as PNG files in build/hlrun-frames (never committed):
    NAME-window-N.png is each open window's final picture, NAME-gdi-N.png and
    NAME-frame-N.png the sampled window and full-screen frames."""
    SHOTS.mkdir(parents=True, exist_ok=True)
    for old in SHOTS.glob(name + '-*.png'):
        old.unlink()
    kinds = {'w': 'window', 'g': 'gdi', 'f': 'frame', 'e': 'step'}
    saved = []
    ppms = sorted(((run or OUT / 'run') / 'q3-frames').glob('*.ppm'), key=lambda p: (p.stat().st_mtime_ns, p.name))
    # gdiframes=1 names carry the adapter's pid (gNNNNN-PID); processes are
    # numbered in the order of their first frame.
    processes = {}
    for ppm in ppms:
        number, _, pid = ppm.stem[1:].partition('-')
        process = processes.setdefault(pid, len(processes) + 1) if pid else 0
        target = SHOTS / ('%s-%s%s-%d.png' % (name, 'p%d-' % process if process else '',
                                              kinds.get(ppm.name[0], ppm.name[0]), int(number)))
        size = ppm_to_png(ppm, target)
        saved.append((target, size))
    return saved


def game_executable(root):
    """The installed hl.exe (outside WINDOWS), or None."""
    drive = root / 'C'
    if not drive.is_dir():
        return None
    for path in sorted(drive.rglob('*')):
        if path.name.lower() == 'hl.exe' and 'WINDOWS' not in path.parts and path.is_file():
            return path
    return None


def guest(root, path):
    return 'C:\\' + '\\'.join(path.relative_to(root / 'C').parts)


def ours(output):
    """CiukiOS's own adapter records (no program text)."""
    keep = ('GAME', 'REPORT', 'REFUSE', 'STATS', 'H1', 'FAULT', 'fault', 'adapter', 'KERNEL ENOSYS', 'MISSING',
            'missing', 'W32', 'TERMINATE', 'STACK', 'DLLMAIN', 'LOAD')
    lines = [line.split(' title=')[0][:200] for line in output.splitlines() if line.startswith(keep)]
    return '\n'.join(lines)


def ppm_colours(path, box=None):
    """Distinct colours of a binary PPM written by the adapter (optionally
    inside box = (left, top, right, bottom))."""
    data = path.read_bytes()
    fields, at = [], 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        end = at
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[at:end])
        at = end
    at += 1
    width, height = int(fields[1]), int(fields[2])
    left, top, right, bottom = box or (0, 0, width, height)
    seen = set()
    for y in range(top, bottom):
        row = at + y * width * 3
        for x in range(left, right):
            seen.add(data[row + x * 3:row + x * 3 + 3])
    return len(seen)


@unittest.skipUnless(CD.is_dir(), 'original Half-Life CD is not mounted')
class HalfLifeTests(unittest.TestCase):
    def services(self, options=None):
        return test_is5boot.cd_services(self, INSTALLED, options, VOLUME, fonts=True)

    def installed(self):
        """The installed hl.exe. Without an installed tree, CIUKI_IS5_INSTALL=1
        runs the original SETUP.EXE from the CD to completion (the dialogs'
        defaults; the third answer accepts the licence) and keeps the
        registry of the fresh install as REGISTRY: the game asks for its CD
        key until a run stores it, and every keyed run starts from it."""
        exe = game_executable(INSTALLED)
        if exe and REGISTRY.is_dir():
            return exe
        if exe:
            self.skipTest('installed tree without its fresh registry %s: remove %s and install again' %
                          (REGISTRY, INSTALLED))
        if not os.environ.get('CIUKI_IS5_INSTALL'):
            self.skipTest('no installed tree; CIUKI_IS5_INSTALL=1 installs from the CD into ' + str(INSTALLED))
        INSTALLED.parent.mkdir(parents=True, exist_ok=True)
        service = self.services('defaults=5000,80 answers=..y gdiframes=1')
        run = OUT / 'run-install'
        shutil.rmtree(run / 'q3-frames', ignore_errors=True)
        (run / 'q3-frames').mkdir(parents=True)
        work = Path.cwd()
        os.chdir(run)  # each press writes the open windows to ./q3-frames (gdiframes=1)
        try:
            began = time.monotonic()
            child = service.start('D:\\SETUP.EXE', '"D:\\SETUP.EXE"', 'D:\\')
            try:
                code, _ = child.finish(int(os.environ.get('CIUKI_IS5_INSTALL_SECONDS', '1800')))
            except subprocess.TimeoutExpired:
                self.fail('Half-Life install did not finish\n' + test_is5boot.diagnostics(service))
        finally:
            os.chdir(work)
        self.assertEqual(code, 0, test_is5boot.diagnostics(service))
        service.close()
        exe = game_executable(INSTALLED)
        self.assertIsNotNone(exe, 'install finished without hl.exe')
        shutil.rmtree(REGISTRY, ignore_errors=True)
        shutil.copytree(INSTALLED / 'hives', REGISTRY)
        shots = screenshots('00-install', run)
        print('\nHalf-Life H1: install finished in %.1f s; %d screenshots in %s' %
              (time.monotonic() - began, len(shots), SHOTS))
        return exe

    def fresh_registry(self):
        """The registry as the install left it (no CD key stored)."""
        shutil.rmtree(INSTALLED / 'hives', ignore_errors=True)
        shutil.copytree(REGISTRY, INSTALLED / 'hives')

    def run_game(self, arguments, timeout, options=None, cd_key=False, answers='*'):
        """hl.exe in game mode from the fresh registry. With cd_key the idle
        dialogs get ANSWERS in turn ('*' types the owner's key and Enter in
        the CD-key prompt, a letter is a menu mnemonic); skips when no key is
        available. The fresh registry is put back afterwards, so the key is
        kept nowhere."""
        exe = self.installed()
        self.fresh_registry()
        temporary = None
        if cd_key:
            key, owned = cd_key_file()
            if not key:
                self.skipTest('the game asks for its CD key: put the owner\'s key in ' + str(CDKEY) +
                              ' (mode 600) or $CIUKI_HL_CDKEY')
            temporary = key if owned else None
            options = ' '.join(x for x in (options, 'defaults=4000,%d answers=%s secret=%s' %
                                           (len(answers), answers, key)) if x)
        try:
            return self._run_game(exe, arguments, timeout, options)
        finally:
            if temporary:
                temporary.unlink(missing_ok=True)
            self.fresh_registry()

    def _run_game(self, exe, arguments, timeout, options, image=None, run=None):
        toc = cd_toc()
        if toc:
            options = ' '.join(x for x in (options, toc) if x)
        service = self.services(options)
        service.request_timeout = timeout
        run = run or OUT / 'run'
        frames = run / 'q3-frames'
        shutil.rmtree(frames, ignore_errors=True)
        frames.mkdir(parents=True)
        work = Path.cwd()
        os.chdir(run)  # the adapter writes sampled PPM frames into ./q3-frames
        try:
            began = time.monotonic()
            image = image or guest(INSTALLED, exe)
            child = service.start_adapter(['game-exec', image, '"' + image + '" ' + arguments,
                                           *([options] if options else [])],
                                          image, cwd=image.rsplit('\\', 1)[0] + '\\')
            try:
                code, output = child.finish(timeout)
            except subprocess.TimeoutExpired:
                code, output = None, test_is5boot.records(child)
            seconds = time.monotonic() - began
        finally:
            os.chdir(work)
        (OUT / 'adapter.log').write_text(ours(output))
        return code, output, seconds

    def assert_clean(self, output):
        self.assertNotIn('REFUSE ', output, ours(output)[-4000:])
        self.assertNotIn('REPORT action=1', output, ours(output)[-4000:])
        self.assertNotIn('TERMINATE', output, ours(output)[-4000:])

    def test_cd_menu(self):
        """The disc's own menu (AUTORUN.EXE, MFC): its WM_ERASEBKGND handler
        paints the whole background through BeginPaint, which works only when
        the window is erased as it is shown, before WM_PAINT validates it;
        the menu items are drawn over that background. Static: no input."""
        code, output, seconds = self._run_game(None, '', 90, 'watchdog=15000 gdiframes=1', image='D:\\AUTORUN.EXE',
                                               run=OUT / 'run-menu')
        self.assert_clean(output)
        shots = screenshots('00-cd-menu', OUT / 'run-menu')
        final = [ppm for ppm in (OUT / 'run-menu' / 'q3-frames').glob('w*.ppm')]
        self.assertTrue(final, ours(output)[-3000:])
        # The background art above the menu items: hundreds of colours, not a black field.
        self.assertGreater(max(ppm_colours(ppm, (0, 0, 250, 160)) for ppm in final), 200)
        print('\nHalf-Life H1: CD menu after %.1f s; screenshots %s' % (seconds, ', '.join(str(p) for p, _ in shots)))

    def test_cd_key_prompt(self):
        """hl.exe (MFC launcher) starts and asks for the CD key in its own
        dialog (Italian resources: the static text, the edit field and the
        owner-drawn buttons of dialog 194, drawn from gfx/shell/btns_main.bmp
        in valve/pak0.pak, a 2.9 MB read), rendered with the system fonts.
        The run ends by the harness watchdog; Escape (Cancel) would quit."""
        code, output, seconds = self.run_game('-console', 120, 'watchdog=15000 gdiframes=1 apicount=1')
        self.assert_clean(output)
        windows = re.findall(r'^GAME desktop op=2 window=(\d+) generation=\d+ bytes=\d+ style=\d+ owner=\d+ size=(\d+)x(\d+)',
                             output, re.M)
        self.assertIn(('318', '174'), [(w, h) for _, w, h in windows], ours(output)[-4000:])
        calls = dict(re.findall(r'^GAME api name=(\S+) calls=(\d+)', output, re.M))
        for api in ('USER32!SetWindowsHookExA', 'USER32!BeginPaint', 'GDI32!BitBlt', 'GDI32!GetDIBits',
                    'GDI32!SetDIBitsToDevice', 'USER32!DrawTextExA'):
            self.assertIn(api, calls)
        shots = screenshots('01-cd-key')
        self.assertTrue(any(size == (318, 174) for _, size in shots), shots)
        # The owner-drawn OK and Cancel buttons (dialog units 43,86 and 112,86, 50x14) carry their text.
        final = [ppm for ppm in (OUT / 'run' / 'q3-frames').glob('w*.ppm')]
        self.assertTrue(any(ppm_colours(ppm, (64, 140, 139, 162)) > 2 and ppm_colours(ppm, (168, 140, 243, 162)) > 2
                            for ppm in final), shots)
        print('\nHalf-Life H1: CD-key dialog after %.1f s; screenshots %s' %
              (seconds, ', '.join(str(path) for path, _ in shots)))

    def test_play_training(self):
        """The CD key typed from the local key file, the launcher's main menu,
        then its Hazard Course item (mnemonic d): the disc check (the CD
        drive's TOC and audio track lengths through MCI cdaudio, on the
        launcher's CD-audio thread), the software renderer (sw.dll) and the
        t0a0 map rendered full screen through DirectDraw (640x480x16 lease).
        The harness reads the TOC from the host drive behind the mount; an
        ISO image has no audio tracks and the game asks for its disc."""
        if not cd_toc():
            self.skipTest('the disc check needs the CD drive itself (audio tracks): mount the disc, not an ISO')
        code, output, seconds = self.run_game('-console', 240, 'watchdog=150000 gdiframes=1', cd_key=True,
                                              answers='*d')
        self.assert_clean(output)
        self.assertIn('GAME secret typed', output, ours(output)[-4000:])
        frames = [int(n) for n in re.findall(r'^GAME frame n=(\d+) kind=lease size=640x480x16', output, re.M)]
        self.assertTrue(frames and max(frames) >= 256, ours(output)[-4000:])
        shots = screenshots('02-play')
        rendered = sorted((OUT / 'run' / 'q3-frames').glob('f*.ppm'), key=lambda p: int(p.stem[1:].split('-')[0]))
        # The map (textured walls), not the console's text on the background.
        self.assertGreater(ppm_colours(rendered[-1]), 300, rendered[-1])
        print('\nHalf-Life H1: training map, %d frames in %.1f s; screenshots %s' %
              (max(frames), seconds, ', '.join(str(path) for path, _ in shots)))

    def test_play_training_opengl(self):
        """The same path with the OpenGL renderer (hl.exe -vt gl -vid_mode 3:
        hw.dll on CiukiOS opengl32, the software rasterizer on this host) at
        640x480x32."""
        if not cd_toc():
            self.skipTest('the disc check needs the CD drive itself (audio tracks): mount the disc, not an ISO')
        code, output, seconds = self.run_game('-console -vt gl -vid_mode 3', 240, 'watchdog=180000 gdiframes=1',
                                              cd_key=True, answers='*d')
        self.assert_clean(output)
        frames = [int(n) for n in re.findall(r'^GAME frame n=(\d+) kind=lease size=640x480x32', output, re.M)]
        self.assertTrue(frames and max(frames) >= 256, ours(output)[-4000:])
        shots = screenshots('03-play-gl')
        rendered = sorted((OUT / 'run' / 'q3-frames').glob('f*.ppm'), key=lambda p: int(p.stem[1:].split('-')[0]))
        self.assertGreater(ppm_colours(rendered[-1]), 300, rendered[-1])
        print('\nHalf-Life H1: training map (OpenGL), %d frames in %.1f s; screenshots %s' %
              (max(frames), seconds, ', '.join(str(path) for path, _ in shots)))

    def test_game_starts(self):
        """Manual exploration: CIUKI_HL_ARGS, CIUKI_HL_SECONDS, CIUKI_HL_OPTIONS,
        CIUKI_HL_ANSWERS (with CIUKI_HL_WITH_KEY)."""
        if not os.environ.get('CIUKI_HL_EXPLORE'):
            self.skipTest('set CIUKI_HL_EXPLORE=1 for an exploratory run')
        code, output, seconds = self.run_game(os.environ.get('CIUKI_HL_ARGS', '-console'),
                                              int(os.environ.get('CIUKI_HL_SECONDS', '120')),
                                              os.environ.get('CIUKI_HL_OPTIONS'),
                                              cd_key=bool(os.environ.get('CIUKI_HL_WITH_KEY')),
                                              answers=os.environ.get('CIUKI_HL_ANSWERS', '*'))
        screenshots('explore')
        print('\nHalf-Life H1 run: exit=%s %.1f s\n%s' % (code, seconds, ours(output)[-6000:]))


def tearDownModule():
    if os.environ.get('CIUKI_HL_CLEAN'):
        shutil.rmtree(INSTALLED, ignore_errors=True)
        shutil.rmtree(OUT / 'run', ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
