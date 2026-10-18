"""Normal-boot runner cases: a QEMU boot without the fw_cfg test request.

Host evidence only (no QEMU): command line, case validation, serial
milestones and negative lines, the screendump heuristic on synthetic and
production-compositor frames, the f0-smoke declaration and physical import.
"""
import copy
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import run as runner
import resources as res
from evidence import EvidenceError
from loader_model import BootMenu, selector
from test_runner import FakeHost

GOOD_SERIAL = [
    'L:CPU signature=00000673 family=00000006 model=00000007 stepping=00000003\r',
    'SELECT_READY\r',
    'CiukiOS 0.9.0 pre-alpha: N Normal / S Safe / L Log / H Hardware / P Probe (3 s)\r',
    'L:SELECT_SOURCE=menu\r',
    'L:VBE_SKIP\r',
    'Ciuki VMM F0 build f5242070d0b4 - CiukiOS',
    'CiukiOS 0.9.0 pre-alpha',
    '[init] flag safe_mode=0 boot_flags=0000016e activation_seq=1',
    '[records] dropped premature evidence record',
    '[supervisor] bootstrap: spawning /Programs/desktop',
    '[spawn] open err=0',
    '[supervisor] desktop pid=3 pgid=3',
]
NEGATIVE_LINES = {
    'Ciuki VMM panic: halting.': 'panic',
    'Ciuki VMM double fault: halting.': 'panic',
    'CIUKI_TEST v=1 run=00000000 seq=000001 probe=panic event=PANIC vector=none reason=x': 'panic',
    'CIUKI_TEST v=1 run=12345678 seq=000001 probe=boot event=BEGIN': 'test_record',
    'L:SELECT_SOURCE=fw_cfg': 'test_request_source',
    'L:SELECT_SOURCE=cfg': 'test_request_source',
    'CIUKI: this PC needs VBE 2.0 or later for a linear framebuffer': 'loader_refusal',
    'L:SELECT_ERROR': 'loader_refusal',
    'L:SAFE=1': 'safe_mode',
    '[supervisor] desktop unavailable: -2; console ready': 'desktop_unavailable',
    '[supervisor] desktop stopped; console ready pid=3 status=139 deaths=1': 'desktop_stopped',
}


def normal_case(**changes):
    case = {'id': 'normal-boot-fixture', 'kind': 'normal-boot', 'probe': 'normal-boot', 'profile': 'qemu-t23',
            'timeout': 3, 'expected': {'evidence_kind': 'normal-boot', 'settle_seconds': 0.3,
                                       'screen': {'geometry': [1024, 768]}}}
    case.update(changes)
    return case


def rgb(colour):
    return bytes(((colour >> 16) & 255, (colour >> 8) & 255, colour & 255))


def frame(width=1024, height=768, background=runner.DESK_BACKGROUND, top=runner.DESK_TOP_BAR,
          taskbar=runner.DESK_TASKBAR, button=runner.DESK_START_BUTTON[0], quantize=False):
    """Synthetic shell desktop laid out like apps/desktop/compositor.c."""
    def colour(c):
        if quantize:  # 16-bpp 565 framebuffer read back as 8-bit RGB
            r, g, b = (c >> 16) & 255, (c >> 8) & 255, c & 255
            c = (r & 0xf8) << 16 | (g & 0xfc) << 8 | (b & 0xf8)
        return rgb(c)
    pixels = bytearray(colour(background) * width * height)
    def fill(x, y, w, h, c):
        for row in range(y, y + h):
            pixels[(row * width + x) * 3:(row * width + x + w) * 3] = colour(c) * w
    fill(0, 0, width, 32, top)
    fill(*runner.DESK_START_BOX, button)
    fill(*runner.desktop_portrait_region(), 0x9a7a5a)
    fill((width - 256) // 2, 200, 256, 256, 0x9a7a5a)  # portrait stand-in
    fill(0, height - 32, width, 32, taskbar)
    fill(width - 64, 5, 48, 7, 0xf5f8fb)    # taskbar clock
    return b'P6\n%d %d\n255\n' % (width, height) + bytes(pixels)


def console_frame(width=1024, height=768, background=0x10243c):
    pixels = bytearray(rgb(background) * width * height)
    for row in range(0, height, 16):  # console text rows
        for y in range(row + 4, row + 11):
            pixels[(y * width) * 3:(y * width + 600) * 3] = rgb(0xe8e8e8) * 600
    return b'P6\n%d %d\n255\n' % (width, height) + bytes(pixels)


def feed(lines):
    parser = runner.NormalBootParser('00000000')
    for line in lines:
        parser.feed(line.encode() + b'\n')
    return parser


class NormalBootUnitTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / 'build/runner-host-tests'; scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch); self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def profile(self, name):
        return runner.load(ROOT / 'tests/profiles' / f'{name}.json')

    def test_command_line_has_no_fw_cfg_and_same_profile_arguments(self):
        overlay, firmware = self.root / 'run.qcow2', self.root / 'bios.bin'
        for name in ('qemu-t23', 'qemu-desktop-2002'):
            with self.subTest(profile=name):
                profile = self.profile(name)
                args, request = runner.qemu_args('qemu', profile, normal_case(profile=name), '12345678', overlay, firmware)
                self.assertIsNone(request)
                self.assertNotIn('-fw_cfg', args)
                self.assertFalse(any('opt/it.alcybercloud.ciukios' in a for a in args))
                for option in ('-S', '-no-shutdown', '-no-reboot'):
                    self.assertIn(option, args)
                self.assertEqual(args[args.index('-serial') + 1], 'file:serial.fifo')
                self.assertEqual(args[args.index('-icount') + 1], 'shift=1,sleep=on')
                # Identical to the probe boot on the same profile, minus the request.
                probe = {'id': 'boot', 'probe': 'boot', 'timeout': 30, 'expected': {'terminal': 'END'}}
                probe_args, probe_request = runner.qemu_args('qemu', profile, probe, '12345678', overlay, firmware)
                index = probe_args.index('-fw_cfg')
                self.assertEqual(probe_args[index + 1], 'name=opt/it.alcybercloud.ciukios/test,string=' + probe_request)
                self.assertEqual(args, probe_args[:index] + probe_args[index + 2:])
        with self.assertRaisesRegex(res.Refusal, 'platform override'):
            runner.qemu_args('qemu', self.profile('qemu-e500'), normal_case(profile='qemu-e500'), '12345678', overlay, firmware)

    def test_case_validation_refuses_probe_features_and_keeps_probe_cases(self):
        self.assertEqual(runner.case_kind(normal_case()), 'normal-boot')
        self.assertEqual(runner.case_kind({'probe': 'boot', 'selector': 'f0:boot run={run_id}'}), 'probe')
        bad = [{'selector': 'f0:boot run={run_id}'}, {'selector_source': 'cfg'}, {'actions': []}, {'fixtures': []},
               {'patches': []}, {'fault': {}}, {'boots': []}, {'device_exceptions': {'vga': 'none'}},
               {'desktop_screen': True}, {'window_screen': {}}, {'checks': {}}, {'evidence_sink': 'screen'},
               {'boot_kind': 'restart'}, {'kind': 'other'}, {'probe': 'boot'}, {'timeout': 0}, {'timeout': '300'},
               {'expected': {'terminal': 'END', 'evidence_kind': 'normal-boot', 'settle_seconds': 8}},
               {'expected': {'settle_seconds': 8}},
               {'expected': {'evidence_kind': 'normal-boot', 'settle_seconds': 0}},
               {'expected': {'evidence_kind': 'normal-boot', 'settle_seconds': 61}},
               {'expected': {'evidence_kind': 'normal-boot', 'settle_seconds': True}},
               {'expected': {'evidence_kind': 'normal-boot', 'settle_seconds': 8, 'screen': {'geometry': [1024]}}},
               {'expected': {'evidence_kind': 'normal-boot', 'settle_seconds': 8, 'screen': {'digest': 'x'}}}]
        for change in bad:
            with self.subTest(change=change), self.assertRaises(res.Refusal):
                runner.case_kind(normal_case(**change))
        # A probe case cannot name the normal-boot probe without the kind,
        # and the frozen selector grammar never accepts it.
        with self.assertRaises(res.Refusal):
            runner.case_kind({'probe': 'normal-boot', 'expected': {}})
        with self.assertRaises(ValueError):
            selector('f0:normal-boot run=12345678', 'fw_cfg', True)

    def test_milestones_are_ordered_whole_line_prefixes(self):
        parser = feed(GOOD_SERIAL)
        self.assertTrue(parser.check())
        self.assertIsNone(parser.failure)
        summary = parser.summary()
        self.assertEqual(list(summary['milestones']), [name for name, _ in runner.NORMAL_BOOT_MILESTONES])
        self.assertEqual((summary['desktop_pid'], summary['desktop_pgid']), (3, 3))
        self.assertEqual(summary['kernel_build_id'], 'f5242070d0b4')
        self.assertEqual(summary['ciuki_test_records'], 0)
        self.assertEqual(summary['milestones']['loader_source']['text'], 'L:SELECT_SOURCE=menu')
        # Not at the start of the line: never a milestone.
        shifted = [('x' + l if l.startswith('[supervisor] desktop pid') else l) for l in GOOD_SERIAL]
        with self.assertRaisesRegex(EvidenceError, 'missing normal-boot milestone: desktop_pid'):
            feed(shifted).check()
        for missing in ('L:CPU', 'L:SELECT_SOURCE', 'Ciuki VMM F0', '[supervisor] bootstrap'):
            with self.subTest(missing=missing), self.assertRaisesRegex(EvidenceError, 'missing normal-boot milestone'):
                feed([l for l in GOOD_SERIAL if not l.startswith(missing)]).check()
        reordered = GOOD_SERIAL[:9] + [GOOD_SERIAL[11], GOOD_SERIAL[10], GOOD_SERIAL[9]]
        with self.assertRaisesRegex(EvidenceError, 'out of order: desktop_pid'):
            feed(reordered).check()
        for suffix in ('X', ' pgid=', ' pgid=3x'):
            with self.subTest(suffix=suffix):
                lines = GOOD_SERIAL[:-1] + ['[supervisor] desktop pid=3' + suffix]
                with self.assertRaisesRegex(EvidenceError, 'desktop_pid'):
                    feed(lines).check()
        repeated = feed(GOOD_SERIAL + [GOOD_SERIAL[0]])
        self.assertEqual(repeated.failure['name'], 'repeated_milestone')
        with self.assertRaisesRegex(EvidenceError, 'repeated_milestone'):
            repeated.check()

    def test_negative_lines_fail_and_mid_line_words_do_not(self):
        for line, kind in NEGATIVE_LINES.items():
            with self.subTest(line=line):
                parser = feed(GOOD_SERIAL + [line, 'trailing diagnostic'])
                self.assertEqual(parser.failure['name'], kind)
                self.assertEqual(parser.failure['serial_line'], len(GOOD_SERIAL) + 1)
                with self.assertRaisesRegex(EvidenceError, 'normal boot: ' + kind):
                    parser.check()
        benign = ['[init] no panic recorded', 'note: [supervisor] desktop stopped (quoted)',
                  'echo CIUKI_TEST is a record prefix', 'x L:SELECT_SOURCE=fw_cfg']
        parser = feed(GOOD_SERIAL + benign)
        self.assertIsNone(parser.failure)
        self.assertTrue(parser.check())
        self.assertEqual(feed(GOOD_SERIAL + ['CIUKI_TEST v=1 run=00000000 seq=000001 probe=panic event=PANIC'])
                         .summary()['ciuki_test_records'], 1)

    def test_screen_heuristic_on_synthetic_frames(self):
        path = self.root / 'screen.ppm'
        def observe(data, spec={'geometry': [1024, 768]}):
            path.write_bytes(data)
            return runner.observe_normal_boot_screen(path, spec)
        good = observe(frame())
        self.assertEqual(good['failures'], [])
        self.assertEqual(good['ppm_sha256'], runner.sha(path))
        self.assertEqual((good['width'], good['height']), (1024, 768))
        self.assertEqual(good['regions']['taskbar']['box'], [0, 736, 1024, 32])
        self.assertGreater(good['regions']['taskbar']['fraction'], 0.9)
        self.assertEqual(good['regions']['console_absent']['fraction'], 0)
        self.assertEqual(observe(frame(button=runner.DESK_START_BUTTON[1]))['failures'], [])   # start menu open
        self.assertEqual(observe(frame(quantize=True))['failures'], [])     # 16-bpp framebuffer
        self.assertEqual(observe(frame(800, 600), {})['failures'], [])      # geometry relative
        self.assertEqual(observe(console_frame())['failures'],
                         ['top_bar', 'console_absent', 'taskbar', 'start_button', 'background'])
        self.assertEqual(observe(console_frame(background=0x800000))['failures'][:2], ['top_bar', 'console_absent'])
        self.assertEqual(observe(frame(taskbar=0x375564, button=0x375564))['failures'], ['taskbar', 'start_button'])
        self.assertEqual(observe(frame(top=0x10243c))['failures'], ['top_bar', 'console_absent'])
        self.assertIn('geometry 800x600', observe(frame(800, 600))['failures'][0])
        path.write_bytes(b'P6\n1 1\n255\n\x00\x00\x00')
        with self.assertRaisesRegex(EvidenceError, 'geometry outside'):
            runner.observe_normal_boot_screen(path, {})

    @unittest.skipUnless(shutil.which('clang'), 'clang unavailable')
    def test_screen_heuristic_on_production_compositor_frames(self):
        folder = ROOT / 'build/host'; folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='normal-boot-screen-', dir=folder) as temp:
            temp = Path(temp); env = dict(os.environ, TMPDIR=str(temp), ASAN_OPTIONS='detect_leaks=0')
            binary = temp / 'capture'
            sources = [ROOT / 'apps/desktop' / n for n in ('client.c', 'protocol.c', 'input.c', 'compositor.c', 'theme.c',
                                                           'shell.c', 'autoplay.c', 'autoplay_content.c', 'settings.c', 'wallpaper.c', 'keymap.c', 'folder.c', '../files/model.c', '../files/transfer.c')]
            sources += [ROOT / 'apps/libdeskui/draw.c']
            sources += [ROOT / 'tests/host/desktop' / n for n in ('fake.c', 'compositor_capture.c')]
            subprocess.run(['clang', '-std=c17', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'apps/desktop'),
                            '-I', str(ROOT / 'src/kernel/include'), *map(str, sources), '-o', str(binary)],
                           check=True, env=env)
            sys.path.insert(0, str(ROOT / 'apps/desktop'))
            try:
                import convert_portrait
            finally:
                sys.path.remove(str(ROOT / 'apps/desktop'))
            portrait = temp / 'portrait.xrgb'
            portrait.write_bytes(convert_portrait.convert(ROOT / 'assets/brand/ciuki-logo.png'))
            observed = {}
            for mode in ('shell', 'plain', 'desktop'):
                xrgb = temp / (mode + '.xrgb')
                subprocess.run([str(binary), str(portrait), str(xrgb), mode], check=True, env=env)
                data = xrgb.read_bytes()
                pixels = bytearray(640 * 480 * 3)
                pixels[0::3], pixels[1::3], pixels[2::3] = data[2::4], data[1::4], data[0::4]
                ppm = temp / (mode + '.ppm'); ppm.write_bytes(b'P6\n640 480\n255\n' + bytes(pixels))
                observed[mode] = runner.observe_normal_boot_screen(ppm, {'geometry': [640, 480]})
                if mode == 'desktop':
                    # Quantize the production gradient as fbdev's pack_565
                    # does, then re-expand for the screenshot reader.
                    quantized = bytes(round((v >> ((3, 2, 3)[i % 3])) * 255 / (255 >> ((3, 2, 3)[i % 3])))
                                      for i, v in enumerate(pixels))
                    ppm.write_bytes(b'P6\n640 480\n255\n' + quantized)
                    observed['desktop-565'] = runner.observe_normal_boot_screen(ppm, {'geometry': [640, 480]})
        # The shell desktop (taskbar, open start menu) passes; the plain
        # compositor without the shell has a top bar but no taskbar.
        self.assertEqual(observed['shell']['failures'], [])
        self.assertEqual(observed['desktop']['failures'], [])
        self.assertEqual(observed['desktop-565']['failures'], [])
        self.assertEqual(observed['plain']['failures'], ['taskbar', 'start_button'])

    def test_heuristic_colours_match_desktop_and_console_sources(self):
        header = (ROOT / 'apps/desktop/desktop.h').read_text()
        compositor = (ROOT / 'apps/desktop/compositor.c').read_text()
        console = (ROOT / 'src/kernel/core/console.c').read_text()
        output = (ROOT / 'src/kernel/core/output.c').read_text()
        geometry_header = (ROOT / 'apps/desktop/start-orb.h').read_text()
        self.assertIn('#include "start-orb.h"', header)
        self.assertIn('#define DESK_BAR %d' % runner.DESK_BAR_ROWS, geometry_header)
        # DESK_BACKGROUND remains the saved-settings sentinel; screen colours
        # come from the selected default theme, independently of table order.
        theme_header = (ROOT / 'apps/desktop/theme.h').read_text()
        self.assertIn('#define DESK_THEME_DEFAULT "Ciuki Blue"', theme_header)
        theme = (ROOT / 'apps/desktop/theme.c').read_text()
        default = theme[theme.index('.name = "Ciuki Blue"'):]
        default = default[:default.index('\n    },')]
        self.assertIn('.desktop = 0x%06xu,' % runner.DESK_BACKGROUND, default)
        self.assertIn('.topbar = 0x%06xu,' % runner.DESK_TOP_BAR, default)
        self.assertIn('.taskbar = 0x%06xu,' % runner.DESK_TASKBAR, default)
        self.assertIn('.topbar_accent = 0x%06xu,' % runner.DESK_START_BUTTON[0], default)
        self.assertIn('desk_paint_bar(&p,0,0,(int)d->width,DESK_BAR,t->topbar,d->lfb_bytes_per_pixel==2);', compositor)
        self.assertIn('desk_paint_bar(&p,bar.x,bar.y,bar.w,bar.h,t->taskbar,d->lfb_bytes_per_pixel==2);', compositor)
        # The Ciuki button: its box (desktop.h, compositor.c) and its face colours (shell.c draw_start).
        for name, value in zip(('X', 'Y'), runner.DESK_START_BOX[:2]):
            self.assertIn('#define DESK_START_%s %d\n' % (name, value), geometry_header)
        self.assertIn('return (struct desk_box){DESK_START_X,DESK_START_Y,DESK_START_W,DESK_START_H};', compositor)
        shell = (ROOT / 'apps/desktop/shell.c').read_text()
        self.assertIn('uint32_t base = t->topbar_accent;', shell)
        for name, value in zip(('W', 'H'), runner.DESK_START_BOX[2:]):
            self.assertIn('#define DESK_START_%s %d\n' % (name, value), geometry_header)
        self.assertIn('{0,(int32_t)d->height-DESK_BAR,(int32_t)d->width,DESK_BAR}', compositor)
        self.assertIn('fg = 0x%06X, bg = 0x%06X;' % (runner.CONSOLE_COLOURS[1], runner.CONSOLE_COLOURS[0]), console)
        self.assertIn('console_set_color(0xFFFFFF, 0x%06X);' % runner.CONSOLE_COLOURS[2], output)

    def test_loader_model_times_out_to_a_normal_boot_without_request(self):
        menu = BootMenu(tick=0)
        self.assertEqual(menu.output, ['SELECT_READY'])
        menu.poll(BootMenu.MENU_TICKS)
        self.assertIsNone(menu.request)
        self.assertFalse(menu.safe)
        self.assertTrue(runner.NORMAL_BOOT_MILESTONES[1][1].match(menu.output[-1].encode()))
        default = re.search(r'^DEFAULT_CONFIG = (b".*")$', (ROOT / 'scripts/build_image.py').read_text(), re.M)[1]
        self.assertNotIn('probe=', default)

    def test_f0_smoke_declares_normal_boot_after_probe_boots(self):
        suite = runner.load(ROOT / 'tests/suites/f0-smoke.json')
        ids = [c['id'] for c in suite['cases']]
        self.assertEqual(ids[-2:], ['normal-boot-qemu-t23', 'normal-boot-qemu-desktop-2002'])
        for case in suite['cases'][:-2]:
            self.assertEqual(runner.case_kind(case), 'probe')
            self.assertEqual(selector(case['selector'].format(run_id='12345678'), 'fw_cfg', True)['probe'], case['probe'])
            args, request = runner.qemu_args('qemu', self.profile(case['profile']), case, '12345678',
                                             self.root / 'run.qcow2', self.root / 'bios.bin')
            self.assertIn('name=opt/it.alcybercloud.ciukios/test,string=' + request, args)
        for case, profile in zip(suite['cases'][-2:], ('qemu-t23', 'qemu-desktop-2002')):
            self.assertEqual(runner.case_kind(case), 'normal-boot')
            self.assertEqual(case['profile'], profile)
            self.assertEqual(case['timeout'], 300)
            self.assertNotIn('selector', case)
            self.assertEqual(case['expected'], {'evidence_kind': 'normal-boot', 'settle_seconds': 8,
                                                'screen': {'geometry': [1024, 768]}})
            self.assertNotIn('platform', self.profile(profile))
        resolved = runner.load_suite('f0-smoke')['cases']
        self.assertEqual([c['id'] for c in resolved], ids)
        self.assertTrue(all(c['_smoke'] for c in resolved))
        for name in ('f1-input', 'f2-process', 'f3-vm'):
            with self.subTest(suite=name):
                cases = runner.load_suite(name)['cases']
                self.assertEqual([c['id'] for c in cases[:len(ids)]], ids)

    def test_main_dispatches_f0_smoke_normal_boot_cases(self):
        suite = runner.load_suite('f0-smoke')
        (self.root / 'tests/profiles').mkdir(parents=True); (self.root / 'config').mkdir()
        for name in {c['profile'] for c in suite['cases']}:
            shutil.copy2(ROOT / 'tests/profiles' / f'{name}.json', self.root / 'tests/profiles')
        shutil.copy2(ROOT / 'config/toolchain.json', self.root / 'config')
        image = self.root / 'image.raw'; image.write_bytes(b'canonical')
        calls = []
        def boot(root, suite_name, case, profile, *args, **kwargs):
            calls.append((case['id'], profile['name']))
            directory = self.root / 'build/test-runs' / suite_name / str(len(calls)); directory.mkdir(parents=True)
            return {'run_id': case['id'], 'outcome': 'pass', 'reason': 'fixture'}, directory
        with patch.object(runner, 'ROOT', self.root), patch.object(runner, 'load_suite', return_value=suite), \
             patch.object(res, 'common_lock', return_value=self.root / 'common.lock'), \
             patch.object(runner.Host, 'preflight', return_value=3 * res.GIB), \
             patch.object(runner, 'run_case', side_effect=boot), \
             patch.object(runner.shutil, 'which', return_value='fake'), \
             patch.object(runner, 'find_firmware', return_value=image), patch('sys.stdout', new_callable=io.StringIO):
            self.assertEqual(runner.main(['f0-smoke', '--image', str(image)]), 0)
        self.assertEqual(calls, [(c['id'], c['profile']) for c in suite['cases']])


class NormalBootRunnerTests(unittest.TestCase):
    """The production _run_boot path through the scripted QEMU boundary."""
    def setUp(self):
        scratch = ROOT / 'build/runner-host-tests'; scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch); self.root = Path(self.temp.name)
        self.image = self.root / 'image.raw'; self.image.write_bytes(b'canonical immutable image')
        self.firmware = self.root / 'bios.bin'; self.firmware.write_bytes(b'firmware fixture')
        self.cgroup = self.root / 'cgroup'; self.cgroup.mkdir()
        (self.cgroup / 'memory.events').write_text('oom 0\noom_kill 0\n')
        self.img = self.root / 'fake-img'
        self.img.write_text('#!/usr/bin/env python3\nimport pathlib,sys\nassert sys.argv[1:4]==["create","-f","qcow2"]\n'
                            'pathlib.Path(sys.argv[-1]).write_bytes(b"fixture overlay")\n')
        self.img.chmod(0o755)
        self.script = self.root / 'scenario.json'
        self.screen = self.root / 'desktop.ppm'; self.screen.write_bytes(frame())
        self.identity = patch.object(runner, 'git_identity', return_value={'revision': 'fixture', 'dirty': False})
        self.identity.start()
        self.env = patch.dict(os.environ, {'CIUKI_FAKE_SCRIPT': str(self.script)}); self.env.start()
        self.profile = runner.load(ROOT / 'tests/profiles/qemu-t23.json')

    def tearDown(self):
        self.env.stop(); self.identity.stop(); self.temp.cleanup()

    def run_normal(self, records=GOOD_SERIAL, case=None, **scenario):
        scenario = {'normal_boot': True, 'records': records, 'screen': str(self.screen), **scenario}
        self.script.write_text(json.dumps(scenario))
        host = FakeHost(self.cgroup)
        with res.ExclusiveLock(self.root / 'shared.lock'):
            result, directory = runner.run_case(self.root, 'fixture', case or normal_case(), self.profile, self.image,
                                                str(ROOT / 'tests/host/fake_qemu.py'), self.firmware,
                                                host=host, qemu_img=str(self.img))
        self.assertEqual(host.scope_pids(self.cgroup), [])
        if getattr(host, 'tx', None) is not None:
            os.close(host.tx); os.close(host.rx)
        self.assertEqual(json.loads((directory / 'result.json').read_text())['outcome'], result['outcome'])
        return result, directory

    def test_pass_keeps_screen_artifact_and_declares_evidence_kind(self):
        result, directory = self.run_normal()
        self.assertEqual(result['outcome'], 'pass', result['reason'])
        self.assertEqual(result['evidence_kind'], 'normal-boot')
        self.assertIsNone(result['selector'])
        self.assertNotIn('-fw_cfg', result['qemu']['arguments'])
        self.assertIn('MemoryMax=1500M', result['qemu']['scope_arguments'])
        self.assertEqual(set(p.name for p in directory.iterdir()), {'result.json', 'serial.log', 'screen.ppm'})
        self.assertEqual(set(result['artifacts']), {'serial.log', 'screen.ppm'})
        self.assertEqual(result['retention']['passing_files'], ['result.json', 'serial.log', 'screen.ppm'])
        digest = runner.sha(directory / 'screen.ppm')
        self.assertEqual(result['screen']['ppm_sha256'], digest)
        self.assertEqual(result['artifacts']['screen.ppm']['sha256'], digest)
        self.assertEqual(result['screen']['failures'], [])
        boot = result['normal_boot']
        self.assertIsNone(boot['fw_cfg_request']); self.assertFalse(boot['ciuki_test_records_required'])
        self.assertEqual(boot['ciuki_test_records'], 0)
        self.assertEqual((boot['desktop_pid'], boot['desktop_pgid']), (3, 3))
        self.assertEqual(boot['settle_seconds'], 0.3)
        self.assertGreaterEqual(boot['screendump_after_seconds'] - boot['desktop_seen_after_seconds'], 0.29)
        self.assertEqual([r['name'] for r in result['observed']], [n for n, _ in runner.NORMAL_BOOT_MILESTONES])
        self.assertIn(b'[supervisor] desktop pid=3 pgid=3\n', (directory / 'serial.log').read_bytes())
        self.assertEqual(result['image']['sha256_after'], result['image']['sha256'])
        self.assertTrue(result['cleanup']['clean'])

    def test_negative_lines_fail_with_diagnostics(self):
        panic = GOOD_SERIAL[:10] + ['Ciuki VMM panic: halting.',
                                    'CIUKI_TEST v=1 run=00000000 seq=000001 probe=panic event=PANIC vector=none reason=x']
        result, directory = self.run_normal(panic)
        self.assertEqual(result['outcome'], 'fail')
        self.assertRegex(result['reason'], r'^normal boot: panic at serial line 11: Ciuki VMM panic')
        # The 1 s grace kept the PANIC record that follows the banner.
        self.assertIn(b'probe=panic event=PANIC', (directory / 'serial.log').read_bytes())
        self.assertTrue((directory / 'screen-fail.ppm').is_file())
        self.assertNotIn('screen', result)
        stopped = '[supervisor] desktop stopped; console ready pid=3 status=139 deaths=1'
        result, _ = self.run_normal(delayed_records=[stopped], delay=.1,
                                    case=normal_case(expected={'evidence_kind': 'normal-boot', 'settle_seconds': 1.5}))
        self.assertEqual(result['outcome'], 'fail')
        self.assertIn('normal boot: desktop_stopped', result['reason'])
        self.assertNotIn('screen', result)
        result, _ = self.run_normal(GOOD_SERIAL + ['CIUKI_TEST v=1 run=12345678 seq=000001 probe=boot event=BEGIN'])
        self.assertIn('normal boot: test_record', result['reason'])
        self.assertEqual(result['normal_boot']['ciuki_test_records'], 1)

    def test_missing_milestones_hang_and_console_screen_fail(self):
        result, _ = self.run_normal([l for l in GOOD_SERIAL if not l.startswith('[supervisor] bootstrap')])
        self.assertEqual(result['outcome'], 'fail')
        self.assertEqual(result['reason'], 'missing normal-boot milestone: desktop_spawn')
        # Today's hang: the spawn line and nothing after it.
        result, directory = self.run_normal(GOOD_SERIAL[:10], case=normal_case(timeout=2))
        self.assertEqual(result['outcome'], 'fail')
        self.assertTrue(result['timeout']['occurred'])
        self.assertTrue((directory / 'screen-fail.ppm').is_file())
        self.assertTrue((directory / 'run.qcow2').is_file())
        self.assertNotIn('desktop_pid', result['normal_boot']['milestones'])
        self.screen.write_bytes(console_frame())
        result, directory = self.run_normal()
        self.assertEqual(result['outcome'], 'fail')
        self.assertIn('normal-boot screen heuristic failed: top_bar, console_absent, taskbar', result['reason'])
        self.assertEqual(result['screen']['ppm_sha256'], runner.sha(directory / 'screen.ppm'))
        self.assertIn('screen.ppm', result['artifacts'])


class NormalBootPhysicalImportTests(unittest.TestCase):
    def test_sweep_import_reports_normal_boot_not_run_without_blocking(self):
        import test_physical
        (ROOT / 'build/runner-host-tests').mkdir(parents=True, exist_ok=True)
        helper = test_physical.PhysicalImportTests('test_three_boot_capture_one_panic_keeps_sequences_and_profile')
        helper.setUp()
        try:
            cases = [normal_case()] + helper.sweep_capture()
            results = test_physical.import_sweep(helper.capture, helper.hash, cases)
        finally:
            helper.tearDown()
        self.assertEqual([r['case'] for r in results], [c['id'] for c in cases])
        self.assertEqual(results[0]['outcome'], 'not_run')
        self.assertEqual(results[0]['reason'], 'normal_boot_qemu_only')
        self.assertEqual(results[0]['prerequisite'], {'satisfied': True, 'failed_case': None})
        self.assertEqual([r['outcome'] for r in results[1:]], ['pass'] * 3)


if __name__ == '__main__':
    unittest.main()
