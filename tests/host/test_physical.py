import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/test'))
from physical import import_evidence, import_sweep, split_boots, wire_record, FIELDS
from evidence import EvidenceError


class PhysicalImportTests(unittest.TestCase):
    def setUp(self):
        folder=ROOT/'build/runner-host-tests';folder.mkdir(exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=folder);self.capture=Path(self.temp.name)
        self.hash=hashlib.sha256(b'canonical').hexdigest()
        self.metadata={k:'unknown' for k in FIELDS}
        self.metadata.update(operator_confirmed=True,selector='f0:boot run=12345678',build_id='12ab34cd',write_sha256=self.hash,readback_sha256=self.hash,disk_log='unavailable')
        self.expected={'terminal':'END','predicates':[{'where':{'event':'DATA'},'fields':{'tick':{'ge':10000}}}]}
        (self.capture/'f0.log').write_text('CIUKI_TEST v=1 run=12345678 seq=000001 probe=boot event=BEGIN\nCIUKI_TEST v=1 run=12345678 seq=000002 probe=boot event=DATA tick=10000 build_id=12ab34cd\nCIUKI_TEST v=1 run=12345678 seq=000003 probe=boot event=END status=PASS\n')
    def tearDown(self):self.temp.cleanup()
    def acquire(self):
        (self.capture/'acquisition.json').write_text(json.dumps(self.metadata))
        return import_evidence(self.capture,self.hash,self.expected)
    def test_selector_matching_build_and_unknown_inventory(self):
        result=self.acquire();self.assertEqual(result['outcome'],'pass');self.assertEqual(result['disk_log'],'unavailable')
        self.assertEqual(result['physical']['bios_version'],'unknown');self.assertIn('f0.log',result['artifact_hashes'])
    def test_unconfirmed_wrong_hash_wrong_build_and_override_rejected(self):
        for key,value in [('operator_confirmed',False),('readback_sha256','0'*64),('build_id','87654321'),('selector','f0:boot run=12345678 platform=e500')]:
            old=self.metadata[key];self.metadata[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):self.acquire()
            self.metadata[key]=old
    def test_bounded_allowlist(self):
        (self.capture/'f0.log').write_bytes(b'a'*(128*1024+1))
        with self.assertRaisesRegex(EvidenceError,'128 KiB'):self.acquire()

    def sweep_capture(self):
        self.metadata.update(selector='all:sweep run=12345678', selector_source='cfg',
                             external_halt_seconds=5, resumed=False)
        banner='L:CPU signature=000006b1 family=00000006\r\nL:SELECT_SOURCE=cfg\r\nCiuki VMM F0 build 12ab34cd - CiukiOS\r\n'
        def record(probe, seq, event, extra=''):
            return f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe={probe} event={event}{extra}\r\n'
        raw = banner + record('boot',1,'BEGIN') + record('boot',2,'DATA',' tick=10000') + record('boot',3,'END',' status=PASS')
        raw += '\x00' + banner + record('fpu',1,'BEGIN') + record('fpu',2,'END',' status=PASS')
        raw += banner + record('panic',1,'BEGIN') + record('panic',2,'ARM',' expected_eip=c0100000 expected_cr2=ff800000 expected_error=00000000')
        raw += record('panic',3,'PANIC',' eip=c0100000 cr2=ff800000 error=00000000 storage_delta=0') + '\f'
        (self.capture/'f0.log').write_bytes(raw.encode())
        (self.capture/'acquisition.json').write_text(json.dumps(self.metadata))
        return [{'id':'physical-'+p,'probe':p,'expected':{'terminal':'PANIC' if p=='panic' else 'END','predicates':[]}}
                for p in ('boot','fpu','panic')]

    def test_three_boot_capture_one_panic_keeps_sequences_and_profile(self):
        cases=self.sweep_capture()
        result=import_sweep(self.capture,self.hash,cases)
        self.assertEqual([r['outcome'] for r in result],['pass']*3)
        self.assertEqual([r['profile'] for r in result],['physical']*3)
        self.assertEqual([r['boots'] for r in result],[[1],[2],[3]])
        self.assertEqual(result[2]['observed'][-1]['event'],'PANIC')
        self.assertEqual(result[1]['observed'][0]['seq'],'000001')

    def test_missing_panic_observation_and_per_case_confirmation_are_not_run(self):
        cases=self.sweep_capture();self.metadata.pop('external_halt_seconds')
        (self.capture/'acquisition.json').write_text(json.dumps(self.metadata))
        result=import_sweep(self.capture,self.hash,cases)
        self.assertEqual(result[-1]['outcome'],'not_run')
        cases=self.sweep_capture();cases[1]['operator_confirmation']=True
        result=import_sweep(self.capture,self.hash,cases)
        self.assertEqual(result[1]['outcome'],'not_run')
        self.assertIn('prerequisite failed',result[2]['reason'])

    def test_failed_probe_does_not_erase_later_observations(self):
        cases=self.sweep_capture()
        p=self.capture/'f0.log';p.write_bytes(p.read_bytes().replace(b'probe=fpu event=END status=PASS',b'probe=fpu event=END status=FAIL'))
        result=import_sweep(self.capture,self.hash,cases)
        self.assertEqual(result[1]['outcome'],'fail')
        self.assertEqual(result[2]['outcome'],'not_run')
        self.assertEqual(result[2]['observed'][-1]['event'],'PANIC')

    def test_wrong_run_build_and_duplicate_sequence_fail_closed(self):
        cases=self.sweep_capture();p=self.capture/'f0.log';original=p.read_bytes()
        for before,after in ((b'run=12345678',b'run=12345679'),
                             (b'build 12ab34cd',b'build 87654321'),
                             (b'seq=000002',b'seq=000001')):
            p.write_bytes(original.replace(before,after,1))
            with self.subTest(before=before),self.assertRaises(EvidenceError):import_sweep(self.capture,self.hash,cases)
        p.write_bytes(original+b'CIUKI_TEST v=1 run=12345678 seq=000004 probe=panic event=DATA tick=1\n')
        with self.assertRaises(EvidenceError):import_sweep(self.capture,self.hash,cases)

    def test_checker_requirements_are_not_satisfied_by_serial(self):
        cases=self.sweep_capture();cases[1]['checks']={'fsck':True}
        result=import_sweep(self.capture,self.hash,cases)
        self.assertEqual(result[1]['outcome'],'not_run')
        self.assertIn('independent',result[1]['reason'])

    def test_real_t23_boot_boundaries(self):
        source=ROOT/'build/f1-28/t23-capture.log'
        if not source.exists(): self.skipTest('private hardware capture is local only')
        boots=split_boots(source.read_bytes())
        self.assertGreaterEqual(len(boots),3)
        probes=set();runs=set();panic=0
        for boot in boots:
            seq=0
            for line in boot.splitlines(keepends=True):
                r=wire_record(line)
                if not r:continue
                self.assertGreater(int(r['seq']),seq);seq=int(r['seq'])
                probes.add(r['probe']);runs.add(r['run']);panic+=r['event']=='PANIC'
        self.assertIn('input',probes);self.assertIn('fpu',probes)
        self.assertGreaterEqual(len(runs),2);self.assertEqual(panic,1)

    def test_cli_import_runs_without_qemu_and_writes_one_summary(self):
        import run as runner
        (self.capture/'acquisition.json').write_text(json.dumps(self.metadata))
        image=self.capture/'image.img';image.write_bytes(b'canonical')
        suite={'image':'full','cases':[{'id':'physical-boot','probe':'boot','expected':self.expected}]}
        with patch.object(runner,'ROOT',self.capture),patch.object(runner,'load_suite',return_value=suite), \
             patch.object(runner.res,'common_lock',return_value=self.capture/'.qemu.lock'), \
             patch.object(runner.Host,'preflight'),patch.object(runner,'run_case') as launch, \
             patch.object(runner.shutil,'which',side_effect=AssertionError('QEMU discovery is forbidden during import')):
            self.assertEqual(runner.main(['physical-host','--image',str(image),'--physical-capture',str(self.capture)]),0)
            launch.assert_not_called()
        summary=json.loads((self.capture/'build/test-runs/physical-host/summary.json').read_text())
        self.assertEqual(summary['profile'],'physical');self.assertTrue(summary['sweep_complete'])
        self.assertEqual(summary['cases'][0]['case'],'physical-boot')
