"""Replay production latency policy with deterministic time, never QEMU."""
import os
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class KernelLatencyTests(unittest.TestCase):
    def test_critical_section_attribution(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='timing-', dir=out) as directory:
            temp = Path(directory)
            source = (ROOT / 'src/kernel/core/timing.c').read_text()
            begin = source.index('static inline uint64_t rdtsc(void)')
            end = source.index('\nvoid crit_begin(void)', begin)
            # Replace only the CPU instruction boundary; retain all policy.
            (temp / 'latency_timing.inc').write_text(source[:begin] + source[end:])
            binary = temp / 'timing'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                            '-I', str(temp), str(ROOT / 'tests/host/kernel_timing_test.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_diagnostics_are_separate_from_screen_logging(self):
        menu = (ROOT / 'src/boot/ciukldr/menu.inc').read_text()
        platform = (ROOT / 'src/boot/ciukldr/platform.inc').read_text()
        accept = platform.split('mov byte [selector_fw], 1', 1)[1]
        self.assertIn('call selector_accept\n    or byte [B(flags)+1], CBI_F_DIAG_VERBOSE >> 8', accept)
        self.assertNotIn('CBI_F_DIAG_VERBOSE', menu)
        screen = menu.split('.log:', 1)[1].split('.probe:', 1)[0]
        self.assertNotIn('CBI_F_DIAG_VERBOSE', screen)
        for path in ('usb/uhci.c', 'usb/usb_hid.c', 'vm/dosvm_perf.inc', 'drivers/savage2d.c'):
            self.assertIn('CBI_F_DIAG_VERBOSE', (ROOT / 'src/kernel' / path).read_text(), path)
        for suite in ('f0-smoke', 'f1-usb', 'f4-audio', 'f3-vm'):
            cases = json.loads((ROOT / 'tests/suites' / (suite + '.json')).read_text())['cases']
            for case in cases:
                # All probe cases use fw_cfg unless explicitly cfg-driven;
                # normal boot intentionally has no diagnostic selector.
                if case.get('kind') != 'normal-boot':
                    self.assertEqual(case.get('selector_source', 'fw_cfg'), 'fw_cfg', case['id'])

    def test_scheduler_and_sampler(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='latency-', dir=out) as directory:
            temp = Path(directory)
            source = (ROOT / 'src/kernel/core/task.c').read_text()
            block = source[source.index('static struct task *rq_head'):source.index('static void rq_push')]
            for name in ('rq_push', 'rq_pop', 'rq_remove', 'rq_any', 'schedule', 'sched_tick',
                         'task_sleep_ms', 'task_start', 'sched_tasks', 'kpreempt_point'):
                match = re.search(r'^(?:static )?[^\n]+\b' + name + r'\([^\n]*\)\s*\{', source, re.M)
                self.assertIsNotNone(match, name)
                depth, end = 1, match.end()
                while depth:
                    depth += (source[end] == '{') - (source[end] == '}')
                    end += 1
                block += source[match.start():end] + '\n'
            (temp / 'latency_scheduler.inc').write_text(block)
            binary = temp / 'latency'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                            '-I', str(temp), str(ROOT / 'tests/host/kernel_latency_test.c'),
                            str(ROOT / 'src/kernel/lib/fmt.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_console_rows(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='console-latency-', dir=out) as directory:
            binary = Path(directory) / 'console'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                            str(ROOT / 'tests/host/console_latency_test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
