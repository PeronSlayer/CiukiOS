"""IS5 bootstrap host checks; the commercial CD is optional and read-only."""
# SPDX-License-Identifier: LGPL-2.1-or-later
from pathlib import Path
import errno
import os
import re
import shutil
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
CD = Path('/run/media/peronslayer/Halflife')
OUT = ROOT / 'build/host/win32/is5boot'


class BootstrapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        cls.binary = OUT / 'units'
        command = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-fno-sanitize-recover=all', ROOT / 'sdk/win32/is5boot/archive.c',
                   ROOT / 'sdk/win32/is5boot/handoff.c', HERE / 'is5boot_test.c']
        subprocess.run([*command, '-fsanitize=undefined', '-o', cls.binary], check=True)
        cls.sanitized = OUT / 'units-asan'
        subprocess.run([*command, '-fsanitize=address,undefined', '-o', cls.sanitized], check=True)

    def test_address_and_leak_sanitizers(self):
        result = subprocess.run([self.sanitized], capture_output=True, text=True)
        if 'LeakSanitizer has encountered a fatal error' in result.stderr:
            self.skipTest('LeakSanitizer cannot inspect threads in this sandbox; leak detection was not disabled')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_synthetic(self):
        subprocess.run([self.binary], check=True)

    @unittest.skipUnless(CD.is_dir(), 'original Half-Life CD is not mounted')
    def test_local_archive_sizes(self):
        archive = next(p for p in CD.iterdir() if p.name.upper() == '_INST32I.EX_')
        subprocess.run([self.binary, archive], check=True)

    @unittest.skipUnless(CD.is_dir(), 'original Half-Life CD is not mounted')
    def test_local_error_resources(self):
        module = next(p for p in CD.iterdir() if p.name.upper() == '_SETUP.DLL')
        subprocess.run([self.binary, '--errors', module], check=True)

    def test_helper_pe_build(self):
        from scripts.win32 import is5boot, census
        image = is5boot.build(OUT)
        pe = census.PE(image.read_bytes())
        self.assertEqual(pe.base, 0x400000)
        raw = image.read_bytes()
        optional = struct.unpack_from('<I', raw, 60)[0] + 24
        self.assertEqual(struct.unpack_from('<II', raw, optional + 72), (1048576, 65536))

    def test_production_helper_on_service_model(self):
        binary = OUT / 'runtime'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-DW32_HOST_MODEL', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        '-I' + str(ROOT / 'sdk/win32/include'), '-I' + str(ROOT / 'src/kernel/include'),
                        ROOT / 'sdk/win32/is5boot/archive.c', ROOT / 'sdk/win32/is5boot/handoff.c',
                        HERE / 'is5boot_runtime_test.c', '-o', binary], check=True)
        subprocess.run([binary], check=True)


def records(process):
    """An H1 child's own records, read in memory only (never persisted)."""
    if process.output is not None:
        return process.output
    return os.pread(process.log.fileno(), 1024 * 1024, 0).decode(errors='replace')


def diagnostics(service):
    """CiukiOS's own records for a failure message. Window titles and file
    names are the engine's text: each record is cut before such a field."""
    ours = []
    for process in list(service.children):
        log = records(process)
        ours.append('process %d: %d bytes, %d lines' % (process.process.pid, len(log), len(log.splitlines())))
        ours += [line.split(' title=')[0].split(' path=')[0][:160] for line in log.splitlines()
                 if line.startswith(('[', 'GAME', 'H1', 'adapter', 'FAULT', 'fault', 'REPORT', 'REFUSE'))]
    return '\n'.join(dict.fromkeys(ours))


@unittest.skipUnless(CD.is_dir(), 'original Half-Life CD is not mounted')
class LocalEngineTests(unittest.TestCase):
    def harness(self, options=None, volume=None):
        """The H1 installer services with a read-only D: view of the mounted
        CD and the kernel's IS5 admission. `options` are H1 game-exec harness
        settings (h1_game.c h1_game_options); `volume` overrides the fixture
        C: free/total bytes reported by GetDiskFreeSpace."""
        from scripts.win32 import is5boot, installer_fixture
        from tests.host.win32.wine import test_wine_upper as upper
        from . import host_services, test_flow_fixtures
        # Optional read-only build inputs; never mutate another worktree.
        saved = upper.CORE, upper.BUILD
        upper.CORE = Path(os.environ.get('CIUKI_IS5_CORE', upper.CORE))
        upper.BUILD = Path(os.environ.get('CIUKI_IS5_UPPER', upper.BUILD))
        self.addCleanup(setattr, upper, 'CORE', saved[0])
        self.addCleanup(setattr, upper, 'BUILD', saved[1])
        if not (upper.CORE / 'builtin-store.json').is_file() or not upper.local_build_matches():
            self.skipTest('H1 core outputs / qualified reused PEs absent')
        # Run the normal H1 confinement preflight before reading commercial
        # payload or compiling fixtures. A post-startup PE fault is a failure.
        preflight = os.environ.get('CIUKI_IS5_H1_PREFLIGHT')
        if preflight:
            upper.require_h1_sandbox(self, Path(preflight))
        fixtures = OUT / 'fixtures'
        installer_fixture.build(fixtures)
        program = test_flow_fixtures.h1_program(fixtures, self)
        library, _, _ = host_services.build_bridge(OUT / 'services')
        helper = is5boot.build(OUT / 'pe')

        class Session(host_services.Session):
            def path(self, name, writing=False):
                full = self.fullpath(name)
                if full[0] != 'D':
                    return super().path(name, writing)
                if writing:
                    raise OSError(errno.EROFS, 'original CD is read-only')
                p = CD
                for part in full[3:].split('\\'):
                    if not part:
                        continue
                    matches = [x for x in p.iterdir() if x.name.casefold() == part.casefold()]
                    if not matches:
                        raise FileNotFoundError(errno.ENOENT, 'CD file absent')
                    p = matches[0]
                return p

            def handle_path(self, h, writing=False):
                # A handle opened on the read-only CD view maps back to D:.
                current = Path(os.readlink(f'/proc/self/fd/{h.value}'))
                try:
                    relative = current.relative_to(CD)
                except ValueError:
                    return super().handle_path(h, writing)
                if writing:
                    raise OSError(errno.EROFS, 'original CD is read-only')
                return self.path('D:\\' + '\\'.join(relative.parts))

            def file(self, a, raw):
                super().file(a, raw)
                # GetDiskFreeSpace (action 19) of C: on a volume large enough
                # for the title; the files really land on the host disk.
                if volume and a[4] == 19 and not a[17] and self.fullpath(self.string(raw, a[6]) or '')[0] == 'C':
                    struct.pack_into('<4I', raw, a[8] + 24, volume[0] & 0xffffffff, volume[0] >> 32,
                                     volume[1] & 0xffffffff, volume[1] >> 32)

            def dispatch(self, kind, raw, cap):
                # w32srv's own directory records identify dialog windows:
                # WINDOW_ALLOC (17) of the dialog class atom WC_DIALOG
                # (#32770, 0x8002); the reply key is the HWND. MessageBox
                # and DialogBox both build #32770 windows.
                status, out = super().dispatch(kind, raw, cap)
                if (kind == 2 and not status and len(raw) >= 40 and len(out) >= 40 and
                        struct.unpack_from('<I', raw, 4)[0] == 17 and struct.unpack_from('<I', raw, 16)[0] == 0x8002 and
                        not struct.unpack_from('<I', out, 4)[0]):
                    with self.service.lock:
                        self.service.dialogs.add((self.pid, struct.unpack_from('<I', out, 8)[0]))
                return status, out

        class Services(host_services.Services):
            def session(self, cwd='C:\\WINDOWS\\TEMP'):
                with self.lock:
                    self.next_pid += 1
                    result = Session(self, self.next_pid, cwd)
                    self.sessions.append(result)
                    return result

            def start(self, image, command, cwd='C:\\WINDOWS\\TEMP', flags=0):
                source = Session(self, 0, cwd).path(image)
                with source.open('rb') as f:
                    dos = f.read(64)
                    ne = b''
                    if len(dos) == 64 and dos[:2] == b'MZ':
                        f.seek(struct.unpack_from('<I', dos, 60)[0]); ne = f.read(64)
                # H1 replaces the kernel admission only. The native helper,
                # decoder and original engine execute unchanged as PE32.
                if len(ne) == 64 and ne[:2] == b'NE' and ne[54] == 2 and not struct.unpack_from('<H', ne, 12)[0] & 0x8000:
                    siblings = {p.name.upper() for p in source.parent.iterdir()}
                    bundle = 'SETUP.INS' in siblings and siblings.intersection({'_INST32I.EX_', '_INST32A.EX_', '_INST32M.EX_', '_INST32P.EX_'})
                    cleanup = False
                    if source.name.upper() == '_ISDEL.EXE':
                        ini = Session(self, 0, cwd).path('C:\\WINDOWS\\_delis43.ini')
                        if ini.is_file() and ini.stat().st_size <= 4096:
                            data = ini.read_bytes()
                            cleanup = data.startswith(b'[file') and all(key in data for key in
                                (b'\nmain=', b'\nexeostype=1\r\n', b'\nalt0=', b'\nalt1='))
                    if bundle or cleanup:
                        text = command.lstrip()
                        if text.startswith('"'):
                            end = text.find('"', 1); tail = text[end+1:].lstrip() if end >= 0 else ''
                        else:
                            tail = text.split(None, 1)[1] if len(text.split(None, 1)) > 1 else ''
                        command = '"' + image + '" ' + tail
                        image = 'C:\\System\\Win32\\is5boot.exe'
                return self.start_adapter(['game-exec', image, command, *([options] if options else [])], image, cwd, flags)

        scratch = tempfile.TemporaryDirectory(dir=OUT)
        self.addCleanup(scratch.cleanup)
        service = Services(Path(scratch.name) / 'root', library, program)
        service.dialogs = set()
        # Close before the TemporaryDirectory is removed: no commercial
        # expanded engine/support or installed bytes remain on disk.
        self.addCleanup(service.close)
        target = service.root / 'C/System/Win32/is5boot.exe'
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(helper, target)
        return service

    def test_original_setup_to_first_dialog(self):
        service = self.harness()
        child = service.start('D:\\SETUP.EXE', '"D:\\SETUP.EXE"', 'D:\\')
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            extracted = list((service.root / 'C/WINDOWS/TEMP').glob('_ISTMP*.DIR/*'))
            dialogs = False
            for process in list(service.children):
                log = records(process)
                # The dialog's wire window (DESK_V3_WIN32_HWND) and a GDI
                # frame (DESK_DAMAGE) of that window generation.
                hwnds = {h for pid, h in set(service.dialogs) if pid == process.process.pid}
                wires = {(w, g) for w, g, h in re.findall(
                    r'^GAME desktop op=78 window=(\d+) generation=(\d+) bytes=\d+ hwnd=(\d+)', log, re.M)
                    if int(h) in hwnds}
                shown = set(re.findall(r'^GAME frame n=\d+ kind=gdi window=(\d+) generation=(\d+) ', log, re.M))
                if wires & shown:
                    dialogs = True
            if extracted and dialogs:
                print('Half-Life H1: native bootstrap, _ISTMP support extraction (%d files) and the first dialog frame'
                      % len(extracted))
                return
            if child.process.poll() is not None:
                self.fail('Half-Life helper exited before support extraction and a dialog frame; exit=%s\n%s'
                          % (child.process.returncode, diagnostics(service)))
            time.sleep(0.05)
        self.fail('Half-Life engine did not reach support extraction and first dialog within 90 seconds; extracted=%d\n%s'
                  % (len(extracted), diagnostics(service)))

    @unittest.skipUnless(os.environ.get('CIUKI_IS5_INSTALL'),
                         'opt-in: the whole Half-Life install into a scratch C: (about 400 MB, minutes)')
    def test_original_setup_complete_install(self):
        """The whole original install in H1, to the engine's exit.

        Defaults on every page (Enter after 5 s without window or write
        activity), except the third prompt: H1 has no sound device (winmm's
        int 0x80 is refused by the confinement), so Sierra's system test
        reports the missing sound card and its "continue?" box defaults to
        No; the answer there is the &Yes mnemonic. C: reports 4 GiB free
        (the fixture volume's 128 MiB is too small for the title)."""
        service = self.harness('defaults=5000,80 answers=..y', (4 << 30, 8 << 30))
        child = service.start('D:\\SETUP.EXE', '"D:\\SETUP.EXE"', 'D:\\')
        try:
            code, _ = child.finish(int(os.environ.get('CIUKI_IS5_INSTALL_SECONDS', '1800')))
        except subprocess.TimeoutExpired:
            self.fail('Half-Life install did not finish\n' + diagnostics(service))
        installed = [p for p in (service.root / 'C').rglob('*') if p.is_file() and 'WINDOWS' not in p.parts
                     and 'System' not in p.parts]
        size = sum(p.stat().st_size for p in installed)
        engine = [process for process in service.children if process is not child][:1]
        log = records(engine[0]) if engine else ''
        self.assertEqual(code, 0, diagnostics(service))
        self.assertNotIn('REFUSE ', log, diagnostics(service))
        self.assertNotIn('REPORT action=1', log, diagnostics(service))
        self.assertGreater(size, 300 << 20, 'installed %d files, %d bytes' % (len(installed), size))
        print('Half-Life H1: install complete, engine exit 0, %d files, %d bytes' % (len(installed), size))
        # fx-exeicons: the Start menu shortcuts the installer created show the
        # programs' own icons (read in place under the scratch C:).
        import importlib.util
        spec = importlib.util.spec_from_file_location('file_icon_tool', ROOT / 'tests/host/desktop/file_icon_tool.py')
        tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tool)
        binary = tool.build(OUT / 'icons')
        menu = service.root / 'C/WINDOWS/Start Menu/Programs'
        links = sorted(menu.rglob('*.lnk'))
        self.assertTrue(links, 'no Start menu shortcuts')
        own = 0
        for link in links:
            r = tool.resolve(binary, service.root / 'C', '/' + link.relative_to(service.root / 'C').as_posix())
            own += r['own'] == '1' and r.get('loaded') == '1'
            print('Half-Life shortcut %s: own=%s loaded=%s icon=%s,%s' % (link.name, r['own'], r.get('loaded'), r.get('file'), r.get('index')))
        game = [link for link in links if link.stem.lower() == 'half-life']
        self.assertTrue(game and own, 'the Half-Life shortcut has no own icon')


if __name__ == '__main__':
    unittest.main()
