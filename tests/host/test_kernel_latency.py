"""Replay production latency policy with deterministic time, never QEMU."""
import os
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from vm_creation_model import write_model

ROOT = Path(__file__).resolve().parents[2]


class KernelLatencyTests(unittest.TestCase):
    def test_bounded_cached_filesystem_walks(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='bounded-fs-', dir=out) as directory:
            temp = Path(directory)
            source = (ROOT / 'src/kernel/fs/fat.c').read_text()
            source = source.replace('uint32_t off=offset(v,c), x;', 'model_charge(1000); uint32_t off=offset(v,c), x;')
            (temp / 'bounded_fat.inc').write_text(source)
            source = (ROOT / 'src/kernel/fs/cache.c').read_text()
            source = source.replace('if (++probes>32)', 'model_charge(1000); if (++probes>32)')
            source = source.replace('static void scan_service(unsigned *work)\n{ if',
                                    'static void scan_service(unsigned *work)\n{ model_charge(1000); if')
            source = source.replace('blocks[at]->lba<b->lba', '(model_charge(250), blocks[at]->lba<b->lba)')
            source = source.replace('blocks[j]=blocks[j-1];', '{ model_charge(250); blocks[j]=blocks[j-1]; }')
            (temp / 'bounded_cache.inc').write_text(source)
            for mode in ('FAT', 'CACHE'):
                binary = temp / mode
                extra = [str(ROOT / 'src/kernel/fs/blkreq.c')]
                if mode == 'FAT': extra.append(str(ROOT / 'src/kernel/fs/cache.c'))
                subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-DBOUNDED_' + mode, '-pthread',
                                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                                '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                                '-I', str(ROOT / 'src/kernel/fs'), '-I', str(temp),
                                str(ROOT / 'tests/host/fs/bounded_work_test.c'),
                                str(ROOT / 'src/kernel/fs/fs_port.c'), *extra, '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_bounded_dos_metadata_names(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='bounded-names-', dir=out) as directory:
            temp = Path(directory)
            source = (ROOT / 'src/kernel/fs/fs_ro.c').read_text()
            source = source.replace('for (struct ro_page *p = v->pages; p; p = p->next) {',
                                    'for (struct ro_page *p = v->pages; p; p = p->next) { model_charge(1000);')
            source = source.replace('const uint8_t *x=(const uint8_t *)a->name,',
                                    'model_charge(64000); const uint8_t *x=(const uint8_t *)a->name,')
            source = source.replace('*at(v,j)=*at(v,j-1);', 'model_charge(2000); *at(v,j)=*at(v,j-1);')
            source = source.replace('path_equal(', 'model_equal(')
            (temp / 'bounded_names.inc').write_text(source)
            binary = temp / 'names'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread',
                            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                            '-I', str(ROOT / 'src/kernel/fs'), '-I', str(temp),
                            str(ROOT / 'tests/host/fs/bounded_dos_names_test.c'),
                            *[str(ROOT / ('src/kernel/fs/' + name + '.c')) for name in
                              ('fs_port', 'blkreq', 'cache', 'path')], '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_bounded_memory_walks(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='bounded-memory-', dir=out) as directory:
            temp = Path(directory)
            source = (ROOT / 'src/kernel/core/vmm.c').read_text().split('/* ---- user address spaces ---- */', 1)[1]
            source = source.replace('uint32_t old = pt[j];', 'charge(1000); uint32_t old = pt[j];')
            source = source.replace('if (pd[i] & PTE_P) {', 'charge(1000); if (pd[i] & PTE_P) {')
            (temp / 'bounded_aspace.inc').write_text(source)
            source = (ROOT / 'src/kernel/proc/uaddr.c').read_text()
            source = source.replace('for (uint32_t va = base; va < end; va += PAGE_SIZE) {',
                                    'for (uint32_t va = base; va < end; va += PAGE_SIZE) { charge(1000);')
            source = source.replace('while (j < 1024 && !pt[j]) j++;',
                                    'while (j < 1024 && !pt[j]) { charge(250); j++; }')
            source = source.replace('walk_step();', 'charge(1000); walk_step();')
            (temp / 'bounded_uaddr.inc').write_text(source)
            binary = temp / 'memory'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                            '-I', str(temp), str(ROOT / 'tests/host/bounded_memory_test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_pic_mask_write_sites(self):
        # Enumerate the owners, not just a call total: adding an IMR toggle
        # to a request path must fail even if another site is removed.
        allowed = {
            ('arch/pic_pit.c', 'pic_unmask'): ['pic_unmask(2)'],
            ('arch/pic_pit.c', 'pit_init'): ['pic_unmask(0)'],
            ('core/registry.c', 'chain_mask'): ['pic_mask(irq)'],
            ('core/registry.c', 'chain_resume'): ['pic_unmask(irq)'],
            ('core/registry.c', 'irq_chain_add'): ['pic_unmask(irq)'],
            ('core/serial.c', 'serial_writer_init'): ['pic_unmask(uart_irq)'],
            ('drivers/ata.c', 'failure'): ['pic_mask(c->irq)'],
            ('drivers/ata.c', 'ata_channel_idle'): ['pic_mask(c->irq)'],
            ('drivers/ata.c', 'ata_init'): ['pic_unmask(c->irq)'],
            ('drivers/i8042.c', 'quarantine'): ['pic_mask(1)', 'pic_mask(12)'],
            ('drivers/i8042.c', 'i8042_init'): ['pic_unmask(1)', 'pic_unmask(12)'],
            ('audio/backend.c', 'audio_backend_shutdown'): ['pic_mask(5)'],
            ('audio/backend.c', 'backend_dma_init'): ['pic_unmask(5)'],
            ('vm/biosvm.c', 'biosvm_init_for'): ['pic_unmask(1)', 'pic_unmask(12)'],
            ('probes/registry_probe.c', 'shared_fixture'): ['pic_mask((unsigned)irq)'],
        }
        actual = {}
        call = re.compile(r'\bpic_(?:mask|unmask)\s*\(')
        definition = re.compile(r'^\w[^;{}]*?\b(\w+)\([^;{}]*\)\s*\{', re.M)
        for path in sorted((ROOT / 'src/kernel').rglob('*.c')):
            # Ignore comments, while retaining all executable source text.
            source = re.sub(r'/\*.*?\*/|//[^\n]*', '', path.read_text(), flags=re.S)
            if not call.search(source):
                continue
            for match in definition.finditer(source):
                depth, end = 1, match.end()
                while depth:
                    depth += (source[end] == '{') - (source[end] == '}')
                    end += 1
                body = source[match.end():end - 1]
                calls = []
                for found in call.finditer(body):
                    closing, pos = 1, found.end()
                    while closing:
                        closing += (body[pos] == '(') - (body[pos] == ')')
                        pos += 1
                    calls.append(re.sub(r'\s+', '', body[found.start():pos]))
                if calls:
                    actual[(path.relative_to(ROOT / 'src/kernel').as_posix(), match[1])] = calls
        self.assertEqual(actual, allowed)
        print('PIC sites: initial handler activation, quarantine, chain deferral/rearm and H-page probe only PASS')

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
            harness = harness.replace('../../src/kernel/core/ledger.c', str(ROOT / 'src/kernel/core/ledger.c'))
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
            end = source.index('static void shared_tick_work(', begin)
            (temp / 'latency_pic.inc').write_text('static uint8_t mask_master=0xff, mask_slave=0xff;\n' + source[begin:end])
            binary = temp / 'audit'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                            '-I', str(temp), str(ROOT / 'tests/host/pic_mask_latency_test.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
    def test_dosvm_perf_irq_work(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='perf-irq-', dir=out) as directory:
            binary = Path(directory) / 'perf-irq'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-ffunction-sections', '-fdata-sections',
                            '-Wl,--gc-sections', '-fsanitize=address,undefined',
                            '-fsanitize-coverage=trace-pc-guard,trace-loads,trace-stores,trace-div',
                            '-I', str(ROOT / 'src/kernel/include'),
                            str(ROOT / 'tests/host/dosvm_perf_irq_test.c'), '-o', str(binary)], check=True)
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

    def test_contention_record_budget(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='contention-budget-', dir=out) as directory:
            binary = Path(directory) / 'budget'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread',
                            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                            str(ROOT / 'tests/host/contention_budget_test.c'),
                            *[str(ROOT / ('src/kernel/fs/' + unit + '.c')) for unit in ('fs_port', 'blkreq', 'vfs')],
                            str(ROOT / 'src/kernel/lib/fmt.c'), '-o', str(binary)], check=True)
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

    def test_desktop_accel_notice(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='accel-notice-', dir=out) as directory:
            temp = Path(directory)
            source = (ROOT / 'apps/desktop/desktop.c').read_text()
            begin = source.index('void accel_notice_text(')
            end = source.index('static void storage_poll(', begin)
            (temp / 'latency_accel_notice.inc').write_text(source[begin:end])
            binary = temp / 'notice'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(temp),
                            '-I', str(ROOT / 'src/kernel/include'),
                            str(ROOT / 'tests/host/desktop/accel_notice_test.c'), '-o', str(binary)], check=True)
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
            block = source[source.index('static struct task *rq_head'):source.index('void task_stack_init')]
            for name in ('schedule', 'sched_vm_tick_work', 'sched_tick',
                         'task_sleep_ms', 'task_start', 'sched_tasks', 'kpreempt_point'):
                match = re.search(r'^(?:static )?[^\n]+\b' + name + r'\([^\n]*\)\s*\{', source, re.M)
                self.assertIsNotNone(match, name)
                depth, end = 1, match.end()
                while depth:
                    depth += (source[end] == '{') - (source[end] == '}')
                    end += 1
                block += source[match.start():end] + '\n'
            (temp / 'latency_scheduler.inc').write_text('#include <ciuki/sync.h>\n' + block)
            timing = (ROOT / 'src/kernel/core/timing.c').read_text()
            begin = timing.index('static inline uint64_t rdtsc(void)')
            end = timing.index('\nvoid crit_begin(void)', begin)
            timing = (timing[:begin] + timing[end:]).replace('uint64_t g_tsc_per_ms;', 'extern uint64_t g_tsc_per_ms;')
            (temp / 'latency_timing.inc').write_text(timing)
            timers = (ROOT / 'src/kernel/core/sync.c').read_text()
            (temp / 'deadline_timers.inc').write_text(timers[timers.index('/* F0-46 timers:'):timers.index('/* End F0-46 timers. */')])
            pit = (ROOT / 'src/kernel/arch/pic_pit.c').read_text()
            (temp / 'pit_timer.inc').write_text(pit[pit.index('static void shared_tick_work('):pit.index('void pit_init(')])
            write_model(temp / 'vm_creation_model.inc')
            for unit, extra in (
                ('kernel_latency_test.c', []),
                ('scheduler_irqoff_test.c', []),
                ('scheduler_priorities_test.c', []),
                ('timer_audio_load_test.c', []),
                ('fs/wait_latency_test.c', []),
                ('vga_transport_latency_test.c', ['src/kernel/vm/vga_hardware.c']),
            ):
                binary = temp / Path(unit).stem
                subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                                '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                                '-I', str(temp), str(ROOT / 'tests/host' / unit),
                                *(str(ROOT / path) for path in extra),
                                str(ROOT / 'src/kernel/lib/fmt.c'), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

            binary = temp / 't23_audio_replay_test'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-fsanitize=address,undefined', '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L',
                            '-pthread', '-I', str(ROOT / 'src/kernel/include'),
                            '-I', str(temp), str(ROOT / 'tests/host/t23_audio_replay_test.c'),
                            str(ROOT / 'src/kernel/fs/fs_port.c'), str(ROOT / 'src/kernel/fs/blkreq.c'),
                            *(str(ROOT / 'src/kernel/fs' / unit) for unit in
                              ('vfs.c', 'fat.c', 'cache.c', 'path.c', 'fs_ro.c')),
                            str(ROOT / 'src/kernel/lib/fmt.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
            for policy in ('--vfs', '--old-vfs'):
                subprocess.run([str(binary), policy], check=True,
                               env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
            # F1-87 amendment 4 / f1-90: the trial's measured device work.
            # The trial image's aggregate device budget must reproduce the
            # overruns with either VFS policy (negative controls); the f1-90
            # attributed budget must give zero audio-refill device overruns
            # with either. C is fatal with CIUKI_F187_REQUIRE_C=1.
            strict = ['--require-c'] if os.environ.get('CIUKI_F187_REQUIRE_C') == '1' else []
            for policy in (['--old-vfs', '--old-device-budget'], ['--vfs', '--old-device-budget'],
                           ['--old-vfs'], ['--vfs', *strict]):
                subprocess.run([str(binary), *policy, '--trial-device-work'], check=True,
                               env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

            if os.environ.get('CIUKI_SCHED_BENCH') == '1':
                binary = temp / 'scheduler_benchmark'
                subprocess.run(['clang', '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror',
                                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                                '-I', str(ROOT / 'src/kernel/include'), '-I', str(temp),
                                str(ROOT / 'tests/host/scheduler_priorities_test.c'),
                                str(ROOT / 'src/kernel/lib/fmt.c'), '-o', str(binary)], check=True)
                for _ in range(5):
                    subprocess.run([str(binary), '--benchmark'], check=True)

    def test_console_rows(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='console-latency-', dir=out) as directory:
            binary = Path(directory) / 'console'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                            str(ROOT / 'tests/host/console_latency_test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
