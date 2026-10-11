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
