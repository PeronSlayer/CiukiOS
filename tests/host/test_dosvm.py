"""F3 selector/suite rejection controls and deterministic DOS image layout.

Fixtures below are parser inputs, not guest or QEMU evidence.
"""
import copy
import importlib.util
import json
import re
from pathlib import Path
import struct
import shutil
import tempfile
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import run as runner
from evidence import EvidenceError


class DosVMTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('nasm'), 'requires nasm')
    def test_generated_dos_idle_calls(self):
        """Assemble the canonical DOS personality, never the floppy profile.

        Check actual bytes and relative CALL destinations, so a source-only
        idle routine excluded from the payload cannot satisfy this test.
        """
        spec = importlib.util.spec_from_file_location('idle_dos_builder', ROOT / 'scripts/build_dos_image.py')
        builder = importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)
        scratch = ROOT / 'build/host'; scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='dos-idle-', dir=scratch) as folder:
            kernel = builder.assemble('src/runtime/ciukidos.asm', Path(folder) / 'CIUKIDOS.SYS',
                                      {**builder.GEOMETRY, **builder.PERSONALITY})
            command = builder.assemble('src/com/shell.asm', Path(folder) / 'COMMAND.COM', {'COMMAND_COMPAT': 1})
        idle_prefix = bytes.fromhex('9c 50 fb cd28 b88016 cd2f b401 cd16')
        idle = kernel.index(idle_prefix)
        self.assertEqual(kernel.count(idle_prefix), 1)
        # Conditional key recheck skips STI/HLT; AX/FLAGS are restored.
        self.assertEqual(kernel[idle + len(idle_prefix):idle + len(idle_prefix) + 7],
                         bytes.fromhex('7502 fb f4 58 9d c3'))
        reader = kernel.index(bytes.fromhex('b401 cd16 7505 e8'))
        call = reader + 6
        self.assertEqual(call + 3 + struct.unpack_from('<h', kernel, call + 1)[0], idle)
        # 01h, 07h/08h and 0Ah all call the emitted reader.
        calls = [i for i, b in enumerate(kernel[:-2]) if b == 0xe8 and
                 (i + 3 + struct.unpack_from('<h', kernel, i + 1)[0]) & 0xffff == reader]
        self.assertGreaterEqual(len(calls), 3)
        prompt = command.index(bytes.fromhex('cd28 b88016 cd2f b401 cd16'))
        self.assertEqual(command[prompt + 11:prompt + 15], bytes.fromhex('7502 fb f4'))

    def test_selector_and_prerequisites(self):
        self.assertEqual(runner.selector('f3:vm-boot run=Ab12cD34')['phase'], 3)
        # The platform override is the e500 twin's selector, as for F1/F2 (lead,
        # 2026-10-11: the qemu-e500 profile appends it to every selector).
        self.assertEqual(runner.selector('f3:vm-boot run=12345678 platform=e500', 'fw_cfg', True)['platform'], 'e500')
        for request in ('f3:all run=12345678', 'f3:boot run=12345678',
                        'f3:vm-boot run=12345678 safe=1'):
            with self.subTest(request=request), self.assertRaises(ValueError):
                runner.selector(request, 'fw_cfg', True)
        suite = runner.load(ROOT / 'tests/suites/f3-vm.json')
        self.assertEqual({c['profile'] for c in suite['cases']},
                         {'qemu-t23','qemu-e500','qemu-min128','qemu-desktop-2002','qemu-desktop-1998'})
        # f3-02: the desktop-twin profiles run only the composited window row.
        self.assertEqual({c['probe'] for c in suite['cases'] if c['profile'].startswith('qemu-desktop')}, {'vm-window'})
        self.assertTrue(set((*runner.REGRESSION_SUITES,*runner.F2_SUITES)) <= set(suite['prerequisites']))
        resolved = runner.load_suite('f3-vm')
        self.assertEqual(sum(c['probe']=='vm-boot' for c in resolved['cases']),3)
        self.assertTrue(any(c['probe']=='app-gate' for c in resolved['cases']))

    def parser(self, records):
        p = runner.Parser('12345678','vm-boot')
        for seq, (event,fields) in enumerate(records,1):
            row = ' '.join(f'{key}={value}' for key,value in fields.items())
            suffix = ' '+row if row else ''
            p.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=vm-boot event={event}{suffix}\n'.encode())
        return p

    def test_suite_needs_every_cycle_and_cleanup(self):
        expected = runner.load(ROOT / 'tests/suites/f3-vm.json')['cases'][0]['expected']
        records = [('BEGIN',{})]
        records += [('DATA',dict(group='admission',free=90000000,reserved=8388608,resident=1200000,
                        outstanding=7188608,request=8388608,headroom=33554432,admitted=1)) for _ in range(202)]
        records += [('DATA',dict(group='vm_cleanup',vm=1,generation=n,data_loss=0,error=0)) for n in range(1,203)]
        records += [('DATA',dict(case='boot',ready=1,prompt=1,major=5,minor=0,free_paras=20000,drive=2)),
                    ('DATA',dict(case='cycles',clean=100,faults=100,privileged=50,ports=50)),
                    ('DATA',dict(case='isolation',ivt=1,disk=1,heartbeat=1,native_dispatches=1000)),
                    ('DATA',dict(case='baselines',pages_before=20000,pages_after=20000,
                        handles_before=2,handles_after=2,devices_before=4,devices_after=4,restored=1)),
                    ('DATA',dict(case='cleanup',vms=0,tasks=0,pages=0,handles=0,devices=0,restored=1)),
                    ('END',dict(status='PASS',error=0))]
        self.assertTrue(self.parser(records).check(expected))
        for case,field,value in [('boot','prompt',0),('cycles','clean',99),('cycles','faults',99),
                                 ('isolation','heartbeat',0),('baselines','pages_after',19999),
                                 ('cleanup','handles',1)]:
            broken=copy.deepcopy(records)
            next(fields for event,fields in broken if fields.get('case')==case)[field]=value
            with self.subTest(case=case,field=field), self.assertRaises(EvidenceError):
                self.parser(broken).check(expected)
        with self.assertRaises(EvidenceError): self.parser(records[:1]+records[2:]).check(expected)
        broken=copy.deepcopy(records); broken[1][1]['admitted']=0
        with self.assertRaises(EvidenceError): self.parser(broken).check(expected)

    def video_records(self, golden):
        records = [('BEGIN', {})]
        records.append(('DATA', dict(group='admission', free=90000000, reserved=0, resident=0, outstanding=0,
                                     request=8388608, headroom=33554432, admitted=1)))
        records.append(('DATA', dict(case='boot', ready=1, prompt=1, free_paras=30000)))
        records.append(('DATA', dict(case='prompt', mode=3, width=640, height=400, digest='ab' * 32)))
        geometry = [(3, 640, 400, 1, 1, 3), (3, 640, 400, 1, 1, 0), (19, 320, 200, 2, 0, 1),
                    (19, 320, 240, 2, 0, 0), (18, 640, 480, 1, 0, 0)]
        for stage, (mode, width, height, scale, text, flags) in enumerate(geometry, 1):
            records.append(('DATA', dict(case='frame', stage=stage, mode=mode, width=width, height=height,
                                         scale=scale, text=text, flags=flags, digest=golden[stage - 1])))
        records += [('DATA', dict(case='return', stage=6, prompt=1, records=6, dropped=0)),
                    ('DATA', dict(group='vm_stop', vm=1, generation=4, reason=0, vector=0, eip='00000000',
                                  address='00000000', error=0, code='0000000000000000')),
                    ('DATA', dict(group='vm_cleanup', vm=1, generation=4, data_loss=0, error=0)),
                    ('DATA', dict(case='cleanup', frames=5, vms=0, tasks=0, pages=0, handles=0, devices=0, restored=1)),
                    ('END', dict(status='PASS', error=0))]
        return records

    def video_parser(self, records):
        p = runner.Parser('12345678', 'vm-video')
        for seq, (event, fields) in enumerate(records, 1):
            row = ' '.join(f'{key}={value}' for key, value in fields.items())
            p.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=vm-video event={event}{" " + row if row else ""}\n'.encode())
        return p

    def test_video_goldens_match_the_host_model(self):
        """f3-02: suite digests are exactly the host harness's pinned goldens."""
        source = (ROOT / 'tests/host/dosvm_video_test.inc').read_text()
        block = source[source.index('static const char *const video_golden[5] = {'):]
        golden = re.findall(r'"([0-9a-f]{64})"', block[:block.index('};')])
        self.assertEqual(len(golden), 5)
        self.assertEqual(len(set(golden)), 5)
        self.assertEqual(runner.selector('f3:vm-video run=12345678', 'fw_cfg', True)['phase'], 3)
        cases = [c for c in runner.load(ROOT / 'tests/suites/f3-vm.json')['cases'] if c['probe'] == 'vm-video']
        self.assertEqual({c['profile'] for c in cases}, {'qemu-t23', 'qemu-e500', 'qemu-min128'})
        for case in cases:
            digests = {p['where']['stage']: p['fields']['digest'] for p in case['expected']['predicates']
                       if p['where'].get('case') == 'frame' and 'stage' in p['where']}
            self.assertEqual([digests[s] for s in range(1, 6)], golden)
            self.assertTrue(self.video_parser(self.video_records(golden)).check(case['expected']))
        expected = cases[0]['expected']
        for index, field, value in [(4, 'digest', '0' * 64), (4, 'flags', 1), (6, 'flags', 0), (7, 'scale', 1), (8, 'mode', 3)]:
            broken = copy.deepcopy(self.video_records(golden))
            broken[index][1][field] = value
            with self.subTest(index=index, field=field), self.assertRaises(EvidenceError):
                self.video_parser(broken).check(expected)
        records = self.video_records(golden)
        with self.assertRaises(EvidenceError):
            self.video_parser(records[:6] + records[7:]).check(expected)        # a stage missing
        broken = copy.deepcopy(records); broken[-2][1]['frames'] = 4
        with self.assertRaises(EvidenceError): self.video_parser(broken).check(expected)

    def window_records(self, digest):
        return [('BEGIN', {}),
                ('DATA', dict(group='admission', free=90000000, reserved=0, resident=0, outstanding=0,
                              request=33554432, headroom=33554432, admitted=1)),
                ('DATA', dict(case='window', vm=1, generation=3, client=21, desktop=20, ready=1, prompt=1)),
                ('ARM', dict(action='window_input', vm=1)),
                ('DATA', dict(case='frame', stage=1, typed='window', mode=3, width=640, height=400, scale=1, text=1, flags=3, digest=digest)),
                ('ARM', dict(action='window_screen', digest=digest)),
                ('DATA', dict(group='vm_stop', vm=1, generation=3, reason=0, vector=0, eip='00000000',
                              address='00000000', error=0, code='0000000000000000')),
                ('DATA', dict(case='closed', reason=0, dead=1, window_released=1, client_live=0)),
                ('DATA', dict(group='vm_cleanup', vm=1, generation=3, data_loss=0, error=0)),
                ('DATA', dict(case='cleanup', vms=0, tasks=0, pages=0, handles=0, restored=1, objects=1,
                              surfaces=0, grants=0, channels=0)),
                ('END', dict(status='PASS', error=0))]

    def test_window_row_and_composited_region(self):
        """f3-02 desktop row: records, typed stimulus and the screendump region."""
        cases = [c for c in runner.load(ROOT / 'tests/suites/f3-vm.json')['cases'] if c['probe'] == 'vm-window']
        self.assertEqual({c['profile'] for c in cases}, {'qemu-desktop-2002', 'qemu-desktop-1998'})
        self.assertEqual(runner.selector('f3:vm-window run=12345678', 'fw_cfg', True)['phase'], 3)
        golden = cases[0]['window_screen']['digest']
        for case in cases:
            runner.Actions(case['actions'])            # accepted pacing/synchronisation
            keys = [e['data']['key']['data'] for a in case['actions'] for b in a['batches'] for e in b['events']
                    if e['type'] == 'key' and e['data']['down']]
            self.assertEqual(keys, ['backslash', 'shift', 'v', 'm', 'v', 'i', 'd', 'e', 'o', 'dot',
                                    'shift', 'c', 'o', 'm', 'ret'])
            p = runner.Parser('12345678', 'vm-window')
            for seq, (event, fields) in enumerate(self.window_records(golden), 1):
                row = ' '.join(f'{k}={v}' for k, v in fields.items())
                p.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=vm-window event={event}{" " + row if row else ""}\n'.encode())
            self.assertTrue(p.check(case['expected']))
            kernel = copy.deepcopy(self.window_records(golden)); kernel[4][1]['typed'] = 'kernel'
            p = runner.Parser('12345678', 'vm-window')
            for seq, (event, fields) in enumerate(kernel, 1):
                row = ' '.join(f'{k}={v}' for k, v in fields.items())
                p.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=vm-window event={event}{" " + row if row else ""}\n'.encode())
            with self.assertRaises(EvidenceError):    # QEMU requires the desktop input path
                p.check(case['expected'])
        # A 1024x768 screendump whose window region holds the frame: build the
        # region from XRGB rows so the check is independent of the runner code.
        import hashlib, tempfile
        width, height, (x0, y0) = 1024, 768, cases[0]['window_screen']['origin']
        screen = bytearray(width * height * 3)
        frame = hashlib.sha256()
        for y in range(480):
            xrgb = bytearray()
            for x in range(640):
                r, g, b = (x * 3 + y) & 255, (y * 5) & 255, (x ^ y) & 255
                off = ((y0 + y) * width + x0 + x) * 3
                screen[off:off + 3] = bytes((r, g, b))
                xrgb += bytes((b, g, r, 0))
            frame.update(xrgb)
        digest = frame.hexdigest()
        spec = dict(cases[0]['window_screen'], digest=digest)
        with tempfile.TemporaryDirectory(dir=ROOT / 'build/host') as folder:
            path = Path(folder) / 'window.ppm'
            path.write_bytes(b'P6\n1024 768\n255\n' + bytes(screen))
            observed = runner.observe_window_screen(path, spec, {'digest': digest})
            self.assertEqual(observed['digest'], digest)
            self.assertEqual(observed['region'], [x0, y0, 640, 480])
            with self.assertRaises(EvidenceError):
                runner.observe_window_screen(path, spec, {'digest': '0' * 64})
            with self.assertRaises(EvidenceError):
                runner.observe_window_screen(path, dict(spec, digest='1' * 64), {'digest': digest})
            with self.assertRaises(EvidenceError):
                runner.observe_window_screen(path, dict(spec, origin=[500, 400]), {'digest': digest})
            screen[(y0 * width + x0) * 3] ^= 1
            path.write_bytes(b'P6\n1024 768\n255\n' + bytes(screen))
            with self.assertRaises(EvidenceError):
                runner.observe_window_screen(path, spec, {'digest': digest})

    def test_dos_payload_includes_video_fixture(self):
        builder_source = (ROOT / 'scripts/build_dos_image.py').read_text()
        self.assertIn('"/VMVIDEO.COM"', builder_source)
        payload = (ROOT / 'src/probes/dos/vm_video.asm').read_text()
        # The payload's stage line is the kernel latch format "VMVIDEO s f".
        self.assertIn("line db 'VMVIDEO 0 0', 10, 0", payload)
        self.assertIn('VMVIDEO ', (ROOT / 'src/kernel/vm/dosvm_bios.c').read_text())
    def test_audio_state_suite_needs_every_case(self):
        # f3-05: record shapes as emitted by probes/f3_probes_audio_state.c.
        self.assertEqual(runner.selector('f3:vm-audio-state run=12345678')['phase'], 3)
        suite = runner.load(ROOT / 'tests/suites/f3-vm.json')
        cases = [c for c in suite['cases'] if c['probe'] == 'vm-audio-state']
        self.assertEqual({c['profile'] for c in cases}, {'qemu-t23', 'qemu-e500', 'qemu-min128'})
        expected = cases[0]['expected']
        rows = [
            dict(case='detect', ok=1, reset='aa', version='0405', ident='aa', test='5a', speaker='ff00',
                 base='220', irq=7, dma=1, hdma=5, error=0),
            dict(case='mixer', ok=1, irq_bits='04', dma_bits='22', status='20', master='a8', compat='a5', default='c0'),
            dict(case='dma8_single', ok=1, irqs=1, status='21', tc='02', blocks=1, fresh=1, frames=256, rate=8000,
                 cause=1, host_ms=33),
            dict(case='double_buffer', ok=1, irqs=32, blocks=32, fresh=32, services=32, boundaries=32, late=0,
                 stale=0, bad_spacing=0, isr_bad=0, max_service=48),
            dict(case='pause', ok=1, paused=1, units=0, boundaries=0, count=67, irqs=2),
            dict(case='continue', ok=1, count_a=67, count_b=67, irqs=5, active=0, blocks=5, segments=6),
            dict(case='dma16_stereo', ok=1, irqs=8, blocks=8, fresh=8, cause=2, format=7, rate=12000,
                 bad_spacing=0, isr_bad=0),
            dict(case='if_clear', ok=1, host_ms=101, boundaries=10, expected=10, edges=1, pending=1, delivered=0,
                 causes=1, guest_irqs=1, lag=240),
            dict(case='if_restore', ok=1, irqs_before=1, status='21', irqs=3),
            dict(case='opl', ok=1, s1='00', s2='c0', t1='c0', t2='e0', s3='a0', s4='00', events=30, match=30,
                 monotonic=1, generation=1, window=1),
            dict(case='opl_state', ok=1, new=0, pairs='3f', rhythm='3f', r1c0='f1', rc0='31', t1_us=20440, t2_us=81760),
            dict(case='dma_fault', ok=1, faults=1, error=1, boundaries=0, irqs=0, reset='aa', alive=1),
            dict(case='exit', ok=1, state=4, reason=0),
            dict(case='locality', ok=1, witness=1, frames=0, events=0, boundaries=0, causes=0, regs=0),
            dict(case='cleanup', restored=1, vms=0, tasks=0, pages=0, handles=0, devices=0),
        ]
        def parse(rows, status='PASS'):
            p = runner.Parser('12345678', 'vm-audio-state')
            records = [('BEGIN', {'version': 1})] + [('DATA', r) for r in rows] + [('END', {'status': status, 'error': 0})]
            for seq, (event, fields) in enumerate(records, 1):
                row = ' '.join(f'{k}={v}' for k, v in fields.items())
                p.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=vm-audio-state event={event} {row}\n'.encode())
            return p
        self.assertTrue(parse(rows).check(expected))
        for case, field, value in [('double_buffer', 'fresh', 31), ('double_buffer', 'stale', 1),
                                   ('if_clear', 'pending', 0), ('if_clear', 'delivered', 1), ('if_clear', 'edges', 2),
                                   ('continue', 'count_b', 66), ('opl', 'match', 29), ('opl_state', 't1_us', 20481),
                                   ('dma_fault', 'alive', 0), ('locality', 'events', 1), ('detect', 'version', '0201'),
                                   ('dma16_stereo', 'format', 3), ('cleanup', 'pages', 6)]:
            broken = copy.deepcopy(rows)
            next(r for r in broken if r['case'] == case)[field] = value
            with self.subTest(case=case, field=field), self.assertRaises(EvidenceError):
                parse(broken).check(expected)
        with self.assertRaises(EvidenceError):
            parse([r for r in rows if r['case'] != 'opl']).check(expected)
        with self.assertRaises(EvidenceError):
            parse(rows, 'FAIL').check(expected)

    def test_dos_fat16_writer_and_fixed_dates(self):
        spec=importlib.util.spec_from_file_location('dos_builder',ROOT/'scripts/build_dos_image.py')
        builder=importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)
        self.assertEqual(builder.DATA_LBA,361)
        self.assertEqual((builder.SECTORS-builder.DATA_LBA)//builder.SPC,32722)
        self.assertEqual(builder.shortname('DOSREADY.COM'),'DOSREADYCOM')
        record=builder.entry(builder.shortname('COMMAND.COM'),0x20,7,1234)
        self.assertEqual(struct.unpack_from('<HI',record,26),(7,1234))
        self.assertEqual(struct.unpack_from('<H',record,24)[0],builder.DATE)
        self.assertEqual(builder.PERSONALITY['CIUKIDOS_EXTERNAL_XMS'],1)
        self.assertEqual(builder.PERSONALITY['STAGE1_BOOT_EXTERNAL_SHELL'],1)
        # The VMM's INT 33h must survive the DOS boot (f3-01 host review finding).
        self.assertEqual(builder.PERSONALITY['CIUKIDOS_FIRMWARE_MOUSE'],1)
        self.assertEqual(builder.PERSONALITY['ENABLE_PS2_MOUSE_INIT'],0)


if __name__ == '__main__': unittest.main()
