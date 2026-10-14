"""Replay production latency policy with deterministic time, never QEMU."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class KernelLatencyTests(unittest.TestCase):
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
