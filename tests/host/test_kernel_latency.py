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
    def test_pmm_scan_reserve_and_rollback_chunks(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='pmm-audit-', dir=out) as directory:
            temp = Path(directory)
            # Instrument only the bitmap memory boundary, retaining every
            # production loop and mutation. Charge a conservative model
            # microsecond per access; no host wall-time assertion.
            source = (ROOT / 'src/kernel/core/pmm.c').read_text()
            source = source.replace('{ return bitmap[', '{ audit_mem(); return bitmap[')
            source = source.replace('{ bitmap[', '{ audit_mem(); bitmap[')
            (temp / 'pmm_audit.inc').write_text(source)
            harness = (ROOT / 'tests/host/pmm_contig_test.c').read_text()
            harness = harness.replace('static uint32_t test_irq_save(void) { return 0; }', '''
static unsigned irq_depth, accesses, maximum;
static void audit_mem(void) { if (irq_depth) accesses++; }
static uint32_t test_irq_save(void) { if (!irq_depth++) accesses=0; return irq_depth==1 ? 0x200 : 0; }''')
            harness = harness.replace('static void test_irq_restore(uint32_t f) { (void)f; }', '''
static void test_irq_restore(uint32_t f) {
    if (f & 0x200) { CHECK(irq_depth==1 && accesses<=64); if(accesses>maximum) maximum=accesses; }
    irq_depth--;
}''')
            harness = harness.replace('#include "../../src/kernel/core/pmm.c"', '#include "pmm_audit.inc"')
            harness = harness.replace('return failures ? 1 : 0;', '''
    CHECK(maximum==64 && !irq_depth);
    printf("PMM IRQ audit: max=%uus model (64 bitmap accesses), fragmentation/reserve/rollback PASS\\n",maximum);
    return failures ? 1 : 0;''')
            (temp / 'audit.c').write_text(harness)
            binary = temp / 'audit'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                            '-I', str(temp), str(temp / 'audit.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_pic_mask_transactions_and_ten_second_timer(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='irq-audit-', dir=out) as directory:
            temp = Path(directory)
            source = (ROOT / 'src/kernel/core/timing.c').read_text()
            begin = source.index('static inline uint64_t rdtsc(void)')
            end = source.index('\nvoid crit_begin(void)', begin)
            (temp / 'latency_timing.inc').write_text(source[:begin] + source[end:])
            source = (ROOT / 'src/kernel/arch/pic_pit.c').read_text()
            begin = source.index('void pic_unmask(')
            end = source.index('static void timer_irq(', begin)
            (temp / 'latency_pic.inc').write_text('static uint8_t mask_master=0xff, mask_slave=0xff;\n' + source[begin:end])
            binary = temp / 'audit'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                            '-I', str(temp), str(ROOT / 'tests/host/pic_mask_latency_test.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

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

    def test_i8042_pacing_bound(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='pacing-', dir=out) as directory:
            temp = Path(directory)
            source = (ROOT / 'src/kernel/drivers/i8042.c').read_text()
            begin = source.index('static uint8_t physical_read(')
            end = source.index('static void physical_write(', begin)
            (temp / 'latency_i8042_pacing.inc').write_text(source[begin:end])
            binary = temp / 'pacing'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(temp),
                            str(ROOT / 'tests/host/i8042_pacing_test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_desktop_storage_reason(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='storage-notice-', dir=out) as directory:
            temp = Path(directory)
            source = (ROOT / 'apps/desktop/desktop.c').read_text()
            begin = source.index('static void storage_poll(')
            end = source.index('static int run_command(', begin)
            (temp / 'latency_storage_notice.inc').write_text(source[begin:end])
            binary = temp / 'notice'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(temp),
                            '-I', str(ROOT / 'src/kernel/include'),
                            str(ROOT / 'tests/host/desktop/storage_notice_test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_diagnostics_require_explicit_boot_selection(self):
        menu = (ROOT / 'src/boot/ciukldr/menu.inc').read_text()
        platform = (ROOT / 'src/boot/ciukldr/platform.inc').read_text()
        accept = platform.split('mov byte [selector_fw], 1', 1)[1]
        self.assertIn('call selector_accept\n    or byte [B(flags)+1], CBI_F_DIAG_VERBOSE >> 8', accept)
        screen = menu.split('.log:', 1)[1].split('.probe:', 1)[0]
        self.assertIn('or byte [B(flags)+1], CBI_F_DIAG_VERBOSE >> 8', screen)
        self.assertEqual(menu.count('CBI_F_DIAG_VERBOSE'), 1)
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
