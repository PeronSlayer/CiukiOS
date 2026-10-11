"""F3 selector/suite rejection controls and deterministic DOS image layout.

Fixtures below are parser inputs, not guest or QEMU evidence.
"""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import run as runner
from evidence import EvidenceError


class DosVMTests(unittest.TestCase):
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
                         {'qemu-t23','qemu-e500','qemu-min128'})
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
                        handles_before=2,handles_after=2,devices_before=3,devices_after=3,restored=1)),
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
