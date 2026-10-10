"""Call-3 desktop reports consume no application stream identities.

Use actual demo fault reports and kernel syscall/controller/observer/framing
code, then parse the emitted records with the unchanged runner retention cap.

Research/decision: OWASP requires treating cross-trust-zone logging data as
untrusted and validating it before logging:
https://cheatsheetseries.owasp.org/cheatsheets/Logging_Cheat_Sheet.html
Ciuki's f2-acceptance.md requires bounded call-3 input identified by the kernel
task and controller-derived desktop evidence; it does not require duplicating
these summaries into application streams. Consume active participant reports
(including validation failures) privately and retain the existing launch/step
diagnostics. Keep test-architecture.md's retention limits and the parser's
64-identity cap; other application output retains its existing hex framing.
"""
import hashlib
import os
from pathlib import Path
import resource
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'scripts/test'))
from evidence import Parser


class ReportRoutingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = ROOT/'build/host'
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        cls.env = dict(os.environ, TMPDIR=str(folder), ASAN_OPTIONS='detect_leaks=0')
        source = (ROOT/'src/kernel/probes/probes.c').read_text()
        first = source.index('static char app_probe[24]')
        last = source.index('static const struct probe_def probes[]', first)
        framing = folder/'framing.h'
        framing.write_text(source[first:last])
        cls.binary = folder/'report-routing'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-int-to-void-pointer-cast', '-fsanitize=address,undefined',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-I', str(ROOT/'src/kernel/include'),
                        f'-DCIUKI_APP_FRAMING_INCLUDE="{framing}"',
                        str(ROOT/'tests/host/proc/report_routing_test.c'),
                        str(ROOT/'src/kernel/core/syscall.c'),
                        str(ROOT/'tests/host/proc/signal_legacy.c'),
                        str(ROOT/'src/kernel/lib/sha256.c'), str(ROOT/'src/kernel/lib/fmt.c'),
                        str(ROOT/'src/kernel/probes/selector.c'), '-o', str(cls.binary)],
                       check=True, env=cls.env)
        include = folder/'include/ciuki'
        include.mkdir(parents=True)
        for name in ('channel.h', 'surface.h', 'spawn.h'):
            (include/name).write_bytes((ROOT/'sdk/sysroot-overlay/include/ciuki'/name).read_bytes())
        for name in ('raw.h', 'runtime.h'):
            (include/name).write_text('/* declarations supplied by host harness */\n')
        demo = folder/'demo'
        subprocess.run(['clang', '-std=c17', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-I', str(folder/'include'), '-I', str(ROOT/'apps/desktop'),
                        '-I', str(ROOT/'src/kernel/include'),
                        str(ROOT/'tests/host/desktop/demo_gate_test.c'),
                        str(ROOT/'apps/desktop/protocol.c'), str(ROOT/'apps/desktop/compositor.c'),
                        '-o', str(demo)], check=True, env=cls.env)
        result = subprocess.run([str(demo), '--fault-run', 'bad-pointer'],
                                capture_output=True, text=True, env=cls.env, timeout=10,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        if result.returncode != -11:
            raise AssertionError(result.stdout+result.stderr)
        reports = [line for line in result.stdout.splitlines() if line.startswith('case=native-demo ')]
        if len(reports) != 2:
            raise AssertionError(result.stdout)
        cls.victim_reports = folder/'victim.reports'
        cls.victim_reports.write_text('\n'.join(reports)+'\n')

    def parse(self, mode):
        result = subprocess.run([str(self.binary), mode, str(self.victim_reports)],
                                capture_output=True, env=self.env, timeout=30)
        parser = Parser('12345678', mode.removesuffix('-fallback'))
        for line in result.stdout.splitlines():
            self.assertLessEqual(len(line), 240)
            parser.feed(line)
        self.assertEqual(result.returncode, 0, result.stdout.decode()+result.stderr.decode())
        parser.check({'terminal': 'END'})
        return parser

    def test_100_distinct_victims_through_syscall_and_parser(self):
        parser = self.parse('crash-isolation')
        cycles = [r for r in parser.records if r.get('group') == 'routing']
        self.assertEqual([int(r['cycle']) for r in cycles], list(range(1, 101)))
        victims = {r['victim'] for r in cycles}
        self.assertEqual(len(victims), 100)
        self.assertTrue(all(r['stage'] == '3' for r in cycles))
        self.assertEqual(len(parser.application.identities), 3)
        self.assertFalse(victims & {pid for pid, _, _ in parser.application.identities})
        expected = (b'case=native-demo stage=1 turns=0 unauthorized=0 generation=0'
                    b'desktop stdoutinactive report')
        self.assertEqual(bytes(parser.application.head), expected)
        self.assertEqual(parser.application.digest.hexdigest(), hashlib.sha256(expected).hexdigest())
        launch = [r for r in parser.records if r.get('case') == 'launch']
        self.assertEqual(launch[0]['reason'], 'invalid_report:case')

    def test_libc_smoke_and_app_gate_keep_report_framing(self):
        for mode in ('libc-smoke', 'app-gate', 'app-gate-fallback'):
            with self.subTest(mode=mode):
                parser = self.parse(mode)
                self.assertEqual(len(parser.application.identities), 1)
                self.assertEqual({stream for _, _, stream in parser.application.identities}, {'report'})
                if mode == 'libc-smoke':
                    self.assertIn(b'case=destructor', parser.application.head)
                else:
                    self.assertEqual(bytes(parser.application.head), b'CIUKI_TEST v=1 event=END status=PASS')
