"""IS5 bootstrap host checks; the commercial CD is optional and read-only."""
# SPDX-License-Identifier: LGPL-2.1-or-later
from pathlib import Path
import errno
import os
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


@unittest.skipUnless(CD.is_dir(), 'original Half-Life CD is not mounted')
class LocalEngineTests(unittest.TestCase):
    def test_original_setup_to_first_dialog(self):
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
                return self.start_adapter(['game-exec', image, command], image, cwd, flags)

        with tempfile.TemporaryDirectory(dir=OUT) as scratch:
            service = Services(Path(scratch) / 'root', library, program)
            target = service.root / 'C/System/Win32/is5boot.exe'
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(helper, target)
            try:
                child = service.start('D:\\SETUP.EXE', '"D:\\SETUP.EXE"', 'D:\\')
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    extracted = list((service.root / 'C/WINDOWS/TEMP').glob('_ISTMP*.DIR/*'))
                    dialogs = False
                    for process in list(service.children):
                        # Read diagnostics in memory only; never persist text
                        # emitted by the commercial engine.
                        log = os.pread(process.log.fileno(), 1024*1024, 0).decode(errors='replace')
                        if ' modal=1' in log and 'GAME frame n=' in log and 'kind=window' in log:
                            dialogs = True
                    if extracted and dialogs:
                        print('Half-Life H1: native bootstrap, _ISTMP support extraction and first modal dialog frame')
                        break
                    if child.process.poll() is not None:
                        self.fail('Half-Life helper exited before support extraction and a dialog frame; exit=' + str(child.process.returncode))
                    time.sleep(0.05)
                else:
                    self.fail('Half-Life engine did not reach support extraction and first dialog within 90 seconds')
            finally:
                # Close before TemporaryDirectory exits: no commercial
                # expanded engine/support bytes remain on disk.
                service.close()


if __name__ == '__main__':
    unittest.main()
