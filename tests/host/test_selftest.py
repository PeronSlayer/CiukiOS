"""fx-selftest host tests: the shared model, the production check table on a
bare and a T23-profile fake machine, the report parser, the loader entry and
the kernel wiring (docs/design/self-test.md). No QEMU, image or disk."""
import contextlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import selftest_report as report  # noqa: E402

CLANG = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']
ENV = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}
RESERVED = ('v', 'run', 'seq', 'probe', 'event', 'check', 'status', 'ms', 'part')


def build_and_run(folder, name, sources, includes=(), link_flags=()):
    binary = folder / name
    subprocess.run([*CLANG, '-I', str(ROOT / 'src/kernel/include'), *(f'-I{ROOT / i}' for i in includes),
                    *(str(ROOT / s) for s in sources), *link_flags, '-o', str(binary)], check=True)
    return subprocess.run([str(binary), str(folder)], check=True, env=ENV,
                          capture_output=True, text=True).stdout


def check_table():
    """(phase, id, limit_ms) rows of core/selftest_checks.c."""
    source = (ROOT / 'src/kernel/core/selftest_checks.c').read_text()
    rows = []
    for phase in ('selftest_phase1', 'selftest_phase2'):
        body = source.split(f'const struct selftest_check {phase}[] = {{', 1)[1].split('};', 1)[0]
        rows += [(phase, m[1], int(m[2])) for m in re.finditer(r'\{ "([^"]+)", (\d+), \w+ \}', body)]
    return rows


class SelftestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base = ROOT / 'build/host'
        base.mkdir(parents=True, exist_ok=True)
        cls.scratch = tempfile.TemporaryDirectory(prefix='selftest-', dir=base)
        cls.dir = Path(cls.scratch.name)
        cls.model_out = build_and_run(cls.dir, 'model', ['tests/host/selftest_model_test.c',
                                      'src/kernel/core/selftest_model.c', 'src/kernel/lib/fmt.c'])
        cls.checks_out = build_and_run(cls.dir, 'checks', [
            'tests/host/selftest_checks_test.c', 'src/kernel/core/selftest_checks.c',
            'src/kernel/vm/dosvm_selftest.c', 'src/kernel/core/selftest_model.c', 'src/kernel/lib/fmt.c',
            'src/kernel/lib/pbkdf2.c', 'src/kernel/lib/sha256.c'],   # sec-k08: security.kdf
            includes=('src/kernel/vm',), link_flags=('-Wl,--wrap=pbkdf2_hmac_sha256',))

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def cli(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            status = report.main([str(a) for a in args])
        return status, out.getvalue()

    # ---- producers -------------------------------------------------------
    def test_model_sequencing_bounds_and_framing(self):
        self.assertIn('selftest model: 17 records, 0 failures', self.model_out)

    def test_check_table_skips_cleanly_and_passes_the_t23_profile(self):
        # sec-user-06: security.kdf passes on any CPU; registry_backup and lock_latency need the desktop.
        self.assertIn('bare: 3 passed, 1 failed, 30 skipped', self.checks_out)
        self.assertIn('t23: 33 passed, 0 failed, 1 skipped', self.checks_out)
        self.assertIn('t23 without 3D: 31 passed, 1 failed, 2 skipped', self.checks_out)
        self.assertIn('selftest checks: 0 failures', self.checks_out)
        self.assertIn('KDF timing: threshold, WARN, partial sampling and deadline enforced', self.checks_out)

    # ---- parser ----------------------------------------------------------
    def test_t23_log_and_serial_agree(self):
        log = report.load(self.dir / 'T23.LOG')
        serial = report.load(self.dir / 't23-serial.txt')
        for r in (log, serial):
            self.assertTrue(r['complete'])
            self.assertEqual(r['problems'], [])
            self.assertEqual(r['counts'], {'passed': 33, 'failed': 0, 'skipped': 1, 'checks': 34})
            self.assertEqual((r['run'], r['build'], r['os']), ('0badf00d', '12345678', '1.0.1_alpha'))
        self.assertEqual(report.compare(log, serial), [])
        self.assertEqual(log['details'], serial['details'])
        self.assertEqual([c['id'] for c in log['checks']], [row[1] for row in check_table()])
        status, text = self.cli(self.dir / 'T23.LOG', self.dir / 't23-serial.txt')
        self.assertEqual(status, 0)
        self.assertIn('Result: 33 passed, 0 failed, 1 skipped of 34 checks in 30.0 s (complete)', text)
        self.assertIn('SKIP optical.disc (no_disc)', text)

    def test_bare_machine_report(self):
        log = report.load(self.dir / 'BARE.LOG')
        self.assertTrue(log['complete'])
        self.assertEqual(log['counts'], {'passed': 3, 'failed': 1, 'skipped': 30, 'checks': 34})
        self.assertTrue(all('reason' in c['figures'] for c in log['checks'] if c['status'] != 'pass'))
        self.assertEqual(report.compare(log, report.load(self.dir / 'bare-serial.txt')), [])
        status, text = self.cli(self.dir / 'BARE.LOG')
        self.assertEqual(status, 1)
        self.assertIn('FAIL desktop.start (89800 ms): ', text)

    def test_model_sample_json_and_continuations(self):
        status, text = self.cli('--json', self.dir / 'SELFTEST.LOG', self.dir / 'serial.txt')
        self.assertEqual(status, 1)
        data = json.loads(text)
        self.assertEqual(data['differences'], [])
        log, serial = data['reports']
        long_check = next(c for c in serial['checks'] if c['id'] == 'eta.long')
        self.assertEqual(long_check['figures']['truncated'], '1')
        self.assertEqual(long_check, next(c for c in log['checks'] if c['id'] == 'eta.long'))
        slow = next(c for c in log['checks'] if c['id'] == 'epsilon.slow')
        self.assertEqual((slow['status'], slow['ms'], slow['figures']['deadline_exceeded']), ('fail', 80, '1'))

    def test_incomplete_runs(self):
        lines = (self.dir / 'T23.LOG').read_text().splitlines()
        head = lines[:12]
        cases = {
            'running': (head + ['RUNNING dos.window'], 'incomplete: stopped while running dos.window'),
            'stalled': (head + ['STALLED dos.window ms=120000'], 'incomplete: stalled in dos.window'),
            'truncated': (lines[:-3] + ['LOG TRUNCATED'], 'incomplete: log truncated'),
            'no summary': (head, 'incomplete: no summary'),
        }
        for name, (text, state) in cases.items():
            with self.subTest(name=name):
                path = self.dir / f'cut-{name.replace(" ", "_")}.LOG'
                path.write_text('\n'.join(text) + '\n')
                parsed = report.load(path)
                self.assertFalse(parsed['complete'])
                status, out = self.cli(path)
                self.assertEqual(status, 2)
                self.assertIn(state, out)
        serial = (self.dir / 't23-serial.txt').read_bytes()
        cut = self.dir / 'cut-serial.txt'
        cut.write_bytes(serial[:serial.index(b'event=END')].rsplit(b'\n', 1)[0] + b'\n')
        self.assertFalse(report.load(cut)['complete'])
        stalled = self.dir / 'stalled-serial.txt'
        stalled.write_bytes(serial.split(b'\n')[0] + b'\nCIUKI_TEST v=1 run=0badf00d seq=000002 probe=selftest '
                            b'event=DATA check=dos.window status=stalled ms=120000\n')
        parsed = report.load(stalled)
        self.assertEqual(parsed['stalled'], [{'id': 'dos.window', 'ms': 120000}])

    def test_malformed_input_is_rejected_or_flagged(self):
        good = (self.dir / 'T23.LOG').read_text()
        rejected = {
            'header': good.replace('CiukiOS self-test format=1', 'Self-test format=1', 1),
            'format': good.replace('format=1', 'format=2', 1),
            'token': good.replace('PASS serial.uart ms=0 ', 'PASS serial.uart ms=0 Bad=1 ', 1),
            'duplicate': good.replace('PASS serial.uart ms=0 ', 'PASS serial.uart ms=0 fifo=1 ', 1),
            'line': good.replace('Details:', 'Detail:', 1),
        }
        for name, text in rejected.items():
            with self.subTest(name=name):
                with self.assertRaises(report.ReportError):
                    report.parse_log(text)
        flagged = {
            'summary': good.replace('SUMMARY passed=33', 'SUMMARY passed=32', 1),
            'skip reason': good.replace(' reason=no_disc', '', 1),
            'twice': good.replace('PASS memory.start', 'PASS serial.uart', 1),
        }
        for name, text in flagged.items():
            with self.subTest(name=name):
                parsed = report.parse_log(text)
                self.assertTrue(parsed['problems'])
                self.assertEqual(report.exit_status([parsed], []), 2)
        prefix = b'CIUKI_TEST v=1 run=0badf00d seq=%06d probe=selftest event='
        begin = prefix % 1 + b'BEGIN format=1 build=12345678 os=x\n'
        bad_serial = {
            'long': begin + prefix % 2 + b'DATA check=a.b status=pass ms=1 x=' + b'1' * 200 + b'\n',
            'duplicate': begin + prefix % 2 + b'DATA check=a.b status=pass ms=1 x=1 x=2\n',
            'orphan part': begin + prefix % 2 + b'DATA check=a.b part=2 x=1\n',
            'no status': begin + prefix % 2 + b'DATA check=a.b x=1\n',
            'no begin': prefix % 2 + b'DATA check=a.b status=pass ms=1\n',
        }
        for name, data in bad_serial.items():
            with self.subTest(serial=name):
                with self.assertRaises(report.ReportError):
                    report.parse_serial(data)

    def test_serial_capture_with_two_runs(self):
        first = (self.dir / 'serial.txt').read_bytes()
        second = (self.dir / 't23-serial.txt').read_bytes()
        both = self.dir / 'two-runs.txt'
        both.write_bytes(first + b'L:CPU reboot\r\n' + second)
        self.assertEqual(report.load(both)['run'], '0badf00d')
        self.assertEqual(report.load(both, '1a2b3c4d')['counts']['checks'], 7)
        status, _ = self.cli('--run', '1a2b3c4d', both)
        self.assertEqual(status, 1)

    # ---- table, entry and wiring (static) ---------------------------------
    def test_check_table_ids_bounds_and_documentation(self):
        rows = check_table()
        ids = [row[1] for row in rows]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertTrue(all(report.CHECK_ID.fullmatch(i) for i in ids))
        for phase in ('selftest_phase1', 'selftest_phase2'):
            self.assertLessEqual(sum(row[0] == phase for row in rows), 32)  # PHASE_MAX
        self.assertLessEqual(sum(row[2] for row in rows), 600000)  # worst case ten minutes
        design = (ROOT / 'docs/design/self-test.md').read_text()
        for phase, check_id, limit in rows:
            self.assertRegex(design, rf'\| `{re.escape(check_id)}` \| {limit // 1000} s \|')
        for name in ('core/selftest_checks.c', 'vm/dosvm_selftest.c'):
            source = (ROOT / 'src/kernel' / name).read_text()
            for call in re.finditer(r'selftest_(?:figures|detail)\([^"]*"([^"]*)"', source):
                keys = re.findall(r'(?:^| )([a-z][a-z0-9_]*)=', call[1])
                self.assertFalse(set(keys) & set(RESERVED), (name, call[1]))
            for call in re.finditer(r'selftest_token\([^,]+, [^,]+, "([a-z0-9_]+)"', source):
                self.assertNotIn(call[1], RESERVED, name)

    def test_entry_and_kernel_wiring(self):
        boot_info = (ROOT / 'src/kernel/include/ciuki/boot_info.h').read_text()
        self.assertIn('#define CBI_F_SELF_TEST        (1u << 23)', boot_info)
        self.assertIn('CBI_F_SELF_TEST', (ROOT / 'src/boot/boot_info.inc').read_text())
        startup = (ROOT / 'src/boot/ciukldr/startup.inc').read_text()
        self.assertIn('dd CBI_F_HW_CHECK | CBI_F_VERBOSE, CBI_F_SELF_TEST', startup)
        config = (ROOT / 'src/boot/ciukldr/menu.inc').read_text().split('boot_menu:', 1)[0]
        self.assertNotIn('selftest', config.lower())  # menu only: no BOOT.CFG token
        probes = (ROOT / 'src/kernel/probes/probes.c').read_text()
        normal = probes.split('if (!(g_boot.flags & CBI_F_TEST_REQUEST)) {', 1)[1].split('struct probe_selection', 1)[0]
        order = ['drivers_init();', 'selftest_before_desktop();', 'supervisor_bootstrap();',
                 'power_discover();', 'selftest_after_desktop();']
        self.assertEqual([normal.index(o) for o in order], sorted(normal.index(o) for o in order))
        self.assertLess(probes.index('hardware_check_run();'), probes.index('selftest_before_desktop();'))
        init = (ROOT / 'src/kernel/core/init.c').read_text()
        self.assertIn('!(g_boot.flags & CBI_F_SELF_TEST)', init)
        self.assertIn('fbdev_accel_schedule(&s->vfs);', (ROOT / 'src/kernel/probes/selftest.c').read_text())
        display = (ROOT / 'src/kernel/core/display.c').read_text()
        self.assertIn('(CBI_F_HW_CHECK | CBI_F_SELF_TEST)) || grants_display_pid()', display)
        savage = (ROOT / 'src/kernel/drivers/video/s3savage/savage3d_owner.c').read_text()
        self.assertIn('return (g_boot.flags&CBI_F_SELF_TEST)!=0;', savage)
        desktop = (ROOT / 'apps/desktop/desktop.c').read_text()
        self.assertIn('devices.boot_flags&(1u<<23)', desktop)
        self.assertIn('v->id!=0x7F000u', desktop)
        self.assertIn('#define SELFTEST_DEVICE_ID 0x7F000u',
                      (ROOT / 'src/kernel/include/ciuki/selftest.h').read_text())
        self.assertIn('"/FSTEST.COM"', (ROOT / 'scripts/build_dos_image.py').read_text())

    def test_fstest_payload(self):
        binary = self.dir / 'FSTEST.COM'
        subprocess.run(['nasm', '-f', 'bin', str(ROOT / 'src/probes/dos/fs_test.asm'), '-o', str(binary)], check=True)
        data = binary.read_bytes()
        self.assertLess(len(data), 512)
        self.assertEqual(data[:3], b'\xb8\x13\x00')             # mov ax, 0013h
        self.assertIn(b'\xb8\x02\x4f\xbb\x01\x01\xcd\x10', data)  # mov ax, 4F02h; mov bx, 0101h; int 10h
        self.assertIn(b'FSTEST done\r\n$', data)
        self.assertNotIn(b'\xcd\x13', data)                     # no INT 13h disk access
        self.assertNotIn(b'\xb4\x3c', data)                     # no INT 21h AH=3Ch create
        self.assertNotIn(b'\xb4\x40', data)                     # no INT 21h AH=40h write


if __name__ == '__main__':
    unittest.main()
