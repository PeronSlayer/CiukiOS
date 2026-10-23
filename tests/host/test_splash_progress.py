"""f0-55 boot splash progress bar (docs/boot.md); no QEMU.

The production console (src/kernel/core/console.c) renders the pinned Ciuki
photograph, the tagline in CiukiOS Sans and the progress bar on a host
framebuffer: golden frames at 0 %, 40 %, 100 % and indeterminate, the phase
mapping, the claim budget and the display-grant handover
(tests/host/splash_progress_test.c). The source checks keep the phase
reports next to the init records they follow.
f0-55b: the harness also checks the first frame (centred sweep at tick 0,
or the level of phases reported before the paint), the worker's 40 ms turns
with no phase and no klog, the worker's class above init, and the serial
phase records; the source checks pin the paint before the stack-guard
entropy, the scheduler and the accelerator probe.
"""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
# 1024x768, 32 bpp (x8r8g8b8 little endian), pinned photograph and font.
GOLDEN = {
    'splash-indeterminate': 'c32b9db7f75650e7df201f45e2d3cb46afb7533f9a8cb5c6b99b93be5bb5a6f3',
    'splash-0': '1aeb6dab4c295bbdac6c8bc989abb37c847094157be1ab9008c33e9f44b6ead4',
    'splash-40': 'a9e626cb83fe505f9359771ece392fbf2307402c8772d8f737a3526e8fc4b801',
    'splash-100': '743614799351196b2e0c11ac48a629c9cf8f4784b2d885d9d7584f27edbf8396',
    # f1-97: the handover frame with the accelerator line; f2-92: "2D engine:
    # OK (drawing: software)" until the engine has drawn, then "2D
    # acceleration: YES" (frame unchanged), and "2D engine: NO (<reason>)".
    'splash-accel-ok': '3dd2728624df35d85bbf25276b5d5f3a2ad38feac930db75d047fcbe885dfbf0',
    'splash-accel-yes': 'a30b62c9d9e280b5df187d6cb8cd250b707e7ccbc3e0110b5f358233f5aec613',
    'splash-accel-no': 'b39c94f9eaab5cfeecfbaa07dd6948cd8da0f8db7dcf3893ca0bba6dd4be9dbe',
}


class SplashProgressTests(unittest.TestCase):
    def test_golden_frames_phases_claims_and_handover(self):
        if not shutil.which('clang'):
            self.skipTest('host clang unavailable')
        base = ROOT / 'build/host'
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='splash-progress-', dir=base) as folder:
            folder = Path(folder)
            spec = importlib.util.spec_from_file_location('convert_splash', ROOT / 'scripts/convert_splash.py')
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            splash = module.build(folder / 'splash.rgb565')
            binary = folder / 'splash_progress'
            built = subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                    '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                                    f'-DCIUKI_SPLASH_BIN="{splash}"',
                                    f'-DCIUKI_TEST_SANS_CFN="{ROOT / "assets/fonts/native/CIUKIOS.CFN"}"',
                                    str(ROOT / 'tests/host/splash_progress_test.c'), '-o', str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([str(binary), str(folder)], capture_output=True, text=True,
                                 env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PASS', run.stdout)
            for name, digest in GOLDEN.items():
                frame = (folder / f'{name}.raw').read_bytes()
                self.assertEqual(len(frame), 1024 * 768 * 4, name)
                self.assertEqual(hashlib.sha256(frame).hexdigest(), digest, name)

    def test_phase_reports_follow_the_init_records(self):
        init = (ROOT / 'src/kernel/core/init.c').read_text()
        body = init.split('void drivers_init(void)', 1)[1]
        order = [('klog("[init] flag safe_mode=', 'SPLASH_PHASE_DRIVERS'),
                 ('.device = "framebuffer"', 'SPLASH_PHASE_FRAMEBUFFER'),
                 ('.device = "input"', 'SPLASH_PHASE_INPUT'),
                 ('.device = "ata"', 'SPLASH_PHASE_ATA'),
                 ('klog("[init] storage done; files bootstrap");', 'SPLASH_PHASE_STORAGE'),
                 ('klog("[init] files result=%d; audio init", native_files);', 'SPLASH_PHASE_FILES'),
                 ('klog("[init] audio init done");', 'SPLASH_PHASE_AUDIO'),
                 ('if (usb_init) usb_init();', 'SPLASH_PHASE_DEVICES')]
        position = 0
        for record, phase in order:
            at = body.index(record, position)
            report = body.index(f'splash_report({phase});', at)
            # The report directly follows its record, before the next step.
            self.assertNotIn('splash_report(', body[at:report], phase)
            self.assertLess(len(body[at:report]), 400, phase)
            position = report
        self.assertLess(body.index('splash_report(SPLASH_PHASE_ATA);'), body.index('storage_init();'))
        add = init.split('static void activation_add(struct activation_entry entry)', 1)[1].split('\n}\n', 1)[0]
        self.assertIn('int phase = splash_storage_phase(entry.kind);', add)
        self.assertIn('#pragma weak console_splash_phase', init)
        supervisor = (ROOT / 'src/kernel/proc/supervisor.c').read_text()
        spawn = supervisor.split('int supervisor_spawn(', 1)[1]
        self.assertLess(spawn.index('err = elf_load(&file, &image, p->memory);'),
                        spawn.index('console_splash_phase(SPLASH_PHASE_DESKTOP);'))
        self.assertLess(spawn.index('console_splash_phase(SPLASH_PHASE_DESKTOP);'),
                        spawn.index('err = grants_install(p, grants);'))
        bootstrap = supervisor.split('void supervisor_bootstrap(void)', 1)[1]
        self.assertLess(bootstrap.index('klog("[supervisor] bootstrap: spawning /Programs/desktop");'),
                        bootstrap.index('console_splash_phase(SPLASH_PHASE_SUPERVISOR);'))
        self.assertLess(bootstrap.index('console_splash_phase(SPLASH_PHASE_SUPERVISOR);'),
                        bootstrap.index('supervisor_spawn("/Programs/desktop"'))
        # The display grant is the handover: the bar completes before the
        # owner's pixels, and nothing clears the splash.
        grants = (ROOT / 'src/kernel/proc/grants.c').read_text()
        install = grants.split('int grants_install(', 1)[1].split('\n}\n', 1)[0]
        self.assertIn('console_display_owned(true);', install)
        console = (ROOT / 'src/kernel/core/console.c').read_text()
        owned = console.split('void console_display_owned(bool owned)', 1)[1].split('\n}\n', 1)[0]
        self.assertLess(owned.index('if (owned) splash_handover();'), owned.index('display_owned = owned;'))
        for function in ('void console_wake(void)', 'void console_reclaim(void)',
                         'void console_framebuffer_changed(', 'void console_init_text(void)'):
            self.assertIn('splash_stop();', console.split(function, 1)[1].split('\n}\n', 1)[0], function)
        font = (ROOT / 'src/kernel/arch/font.asm').read_text()
        self.assertIn('incbin "assets/fonts/native/CIUKIOS.CFN", 64, 95 + 95 * 32', font)

    def test_splash_paint_precedes_init_and_the_accelerator_probe(self):
        # f0-55b: kmain paints the splash as soon as the LFB is mapped, before
        # the stack-guard entropy (registry_init -> stackprot_init ->
        # entropy_boot), the scheduler (console worker) and the PIT
        # calibration; drivers_init schedules the accelerator probe only
        # after its last phase report, so the probe never delays the paint.
        main = (ROOT / 'src/kernel/core/main.c').read_text()
        body = main.split('void kmain(', 1)[1]
        paint = body.index('if (!text && !console_init_lfb())')
        for later in ('registry_init();', 'sched_init();', 'timing_calibrate();', 'sched_start();'):
            self.assertLess(paint, body.index(later), later)
        registry = (ROOT / 'src/kernel/core/registry.c').read_text()
        self.assertIn('stackprot_init();', registry)
        task = (ROOT / 'src/kernel/core/task.c').read_text()
        self.assertIn('console_worker_init();', task.split('void sched_init(void)', 1)[1].split('\n}\n', 1)[0])
        init = (ROOT / 'src/kernel/core/init.c').read_text()
        drivers = init.split('void drivers_init(void)', 1)[1]
        self.assertLess(drivers.index('splash_report(SPLASH_PHASE_DEVICES);'),
                        drivers.index('fbdev_accel_schedule(&storage_get()->vfs);'))
        probes = (ROOT / 'src/kernel/probes/probes.c').read_text()
        self.assertIn('struct task *t = task_create_kernel("probes", probes_main, 0, P_INTERACTIVE);', main)
        normal = probes.split('if (!(g_boot.flags & CBI_F_TEST_REQUEST)) {', 1)[1]
        self.assertLess(normal.index('drivers_init();'), normal.index('supervisor_bootstrap();'))
        # The worker: raised above the boot task at creation and on each turn
        # while the splash is up; woken by its own sleep timer, not by klog.
        console = (ROOT / 'src/kernel/core/console.c').read_text()
        self.assertIn('#define SPLASH_WORKER_LEVEL (SCHED_FOREGROUND + 2u)', console)
        worker_init = console.split('void console_worker_init(void)', 1)[1].split('\n}\n', 1)[0]
        self.assertLess(worker_init.index('splash_worker_class();'), worker_init.index('task_start(t);'))
        service = console.split('static uint32_t splash_service(void)', 1)[1].split('\n}\n', 1)[0]
        self.assertTrue(service.split('{', 1)[1].lstrip().startswith('splash_worker_class();'))
        worker = console.split('static void console_worker(void *arg)', 1)[1].split('\n}\n', 1)[0]
        self.assertIn('task_sleep_ms(paint.active ? 1 : splash_ms);', worker)
        write = console.split('void console_write(const char *s, size_t n)', 1)[1].split('\n}\n', 1)[0]
        self.assertNotIn('task_start', write)
        self.assertNotIn('splash', write)
        # The phase record: one format, gated by the serial-log flag.
        record = console.split('static void splash_record(uint32_t level)\n{', 1)[1].split('\n}\n', 1)[0]
        self.assertIn('if (!(g_boot.flags & CBI_F_SERIAL_LOG)) return;', record)
        self.assertIn('klog("[console] splash phase=%u permille=%u", phase, level);', record)
        self.assertIn('splash_bar.records < SPLASH_PHASES', record)
        self.assertEqual(console.count('[console] splash phase='), 2)   # the record and its comment

    def test_accelerator_summary_record_and_indicator_sources(self):
        # f1-97: one protected record, the splash line drawn by the console
        # worker (never by the decision's caller), the desktop notice.
        savage = (ROOT / 'src/kernel/drivers/video/s3savage/savage2d.c').read_text()
        self.assertEqual(savage.count('klog("[accel] summary accelerated=%u driver=savage reason=%s used=%u"'), 1)
        decline = savage.split('static const struct accel_ops *decline(const char *reason)\n{', 1)[1]
        self.assertTrue(decline.lstrip().startswith('summary(false, reason);'))
        ready = savage.split('klog("[accel] savage ready', 1)[1].split('return &engine.ops;', 1)[0]
        self.assertIn('summary(true, 0);', ready)
        console = (ROOT / 'src/kernel/core/console.c').read_text()
        accel = console.split('void console_splash_accel(bool accelerated, bool used, const char *reason)', 1)[1].split('\n}\n', 1)[0]
        self.assertNotIn('splash_accel_update', accel)
        self.assertNotIn('fbdev_console_begin', accel)
        service = console.split('static uint32_t splash_service(void)', 1)[1].split('\n}\n', 1)[0]
        self.assertIn('splash_accel_update(true);', service)
        fbdev = (ROOT / 'src/kernel/drivers/fbdev.c').read_text()
        # Part A moved disabled-mode policy/summary into the generic registry.
        self.assertIn('if (display_accel_disabled(&reason)) { display_driver_software(reason); return; }', fbdev)
        registry = (ROOT / 'src/kernel/drivers/video/registry.c').read_text()
        self.assertIn('if (g_boot.flags & CBI_F_SAFE_MODE) { if (reason) *reason = "safe_mode"; return true; }', registry)
        desktop = (ROOT / 'apps/desktop/desktop.c').read_text()
        self.assertIn('(accel_noticed=accel_notice(d))', desktop)
        self.assertIn('d->sh.notification.expires=d->now_ns+10000000000ull;', desktop)
        # f2-92: the engine-use record of the 2 s performance window.
        self.assertIn('"[desktop] accel fills=%llu copies=%llu writes=%llu fallback=%llu cache_rows=%llu', desktop)
        sample = desktop.split('static void performance_sample(struct desktop *d, uint64_t now)\n{', 1)[1].split('\n}\n', 1)[0]
        self.assertIn('accel_record(d,now);', sample)
        self.assertLess(sample.index('desk_stats_sample(d);'), sample.index('accel_record(d,now);'))
        self.assertLess(sample.index('accel_record(d,now);'), sample.index('if (verbose_boot)'))
        self.assertIn('klog("[accel] savage write_test result=%s phase=%s', savage)
        self.assertIn('klog("[accel] savage quarantined reason=%s phase=draw', savage)


if __name__ == '__main__':
    unittest.main()
