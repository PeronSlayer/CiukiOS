"""f0-55 boot splash progress bar (docs/boot.md); no QEMU.

The production console (src/kernel/core/console.c) renders the pinned Ciuki
photograph, the tagline in CiukiOS Sans and the progress bar on a host
framebuffer: golden frames at 0 %, 40 %, 100 % and indeterminate, the phase
mapping, the claim budget and the display-grant handover
(tests/host/splash_progress_test.c). The source checks keep the phase
reports next to the init records they follow.
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


if __name__ == '__main__':
    unittest.main()
