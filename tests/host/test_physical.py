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

FIXTURES = ROOT/'tests/host/fixtures/physical'


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

    def test_real_sweep_fixture_imports_every_f2_all_case(self):
        import run as runner
        capture = FIXTURES/'66666666'
        metadata = json.loads((capture/'acquisition.json').read_text())
        cases = runner.load_suite('f2-all')['cases']
        results = import_sweep(capture,metadata['write_sha256'],cases)
        self.assertEqual(len(results),len(cases))
        self.assertEqual([r['case'] for r in results],[c['id'] for c in cases])
        self.assertEqual(len(results[0]['boot_evidence']),35)
        self.assertTrue(all(b['records_lost'] == 0 for b in results[0]['boot_evidence']))
        self.assertEqual(results[0]['sweep_summary'][0]['passed'],'27')
        self.assertEqual(results[0]['sweep_summary'][0]['failed'],'6')
        self.assertEqual(results[0]['sweep_summary'][0]['not_run'],'2')
        self.assertTrue(all(r['profile'] == 'physical' and r['sweep_complete'] for r in results))
        fd = next(r for r in results if r['case'] == 'fd-table-qemu-t23')
        self.assertEqual(fd['evidence_reason'],'second_volume_absent')
        self.assertIn('second_volume_absent',fd['not_run_reasons'])
        self.assertTrue(fd['observed']); self.assertFalse(fd['prerequisite']['satisfied'])
        self.assertEqual(next(r for r in results if r['case'] == 'mount-bad-bpb')['evidence_reason'],'fixtures_absent')
        self.assertEqual(next(r for r in results if r['case'] == 'uart-absent-qemu-t23')['evidence_reason'],'operator_absent')
        panic = next(r for r in results if r['case'] == 'panic')
        self.assertEqual(panic['boots'],[10]); self.assertEqual(panic['evidence_outcome'],'pass')
        self.assertIn('reset_before_completion',panic['not_run_reasons'])
        fat = next(r for r in results if r['case'] == 'fat-write')
        self.assertEqual(fat['boots'],[21,22])
        self.assertEqual(next(r for r in results if r['case'] == 'mount-crash-reboot')['boots'],[23,24])
        self.assertEqual(next(r for r in results if r['case'] == 'bootlog')['boots'],[25,26])
        self.assertEqual(next(r for r in results if r['case'] == 'app-gate-qemu-t23')['evidence_outcome'],'pass')

    def test_startup_fallbacks_are_per_boot_and_do_not_split_twice(self):
        cases = self.sweep_capture(); path = self.capture/'f0.log'; raw = path.read_bytes()
        cpu = b'L:CPU signature=000006b1 family=00000006\r\n'
        # No surviving CPU banner on boot 2; kernel banner is its boundary.
        raw = raw.replace(b'\x00'+cpu,b'\x00SELECT_READY\r\n',1)
        self.assertEqual(len(split_boots(raw)),3)
        path.write_bytes(raw)
        self.assertEqual([r['outcome'] for r in import_sweep(self.capture,self.hash,cases)],['pass']*3)
        raw = raw.replace(b'\x00SELECT_READY\r\n',b'')
        path.write_bytes(raw)
        self.assertEqual(len(split_boots(raw)),3)
        self.assertEqual([r['boots'] for r in import_sweep(self.capture,self.hash,cases)],[[1],[2],[3]])

    def test_byte_loss_is_bounded_evidence_and_later_boots_survive(self):
        cases = self.sweep_capture(); path = self.capture/'f0.log'; original = path.read_bytes()
        record = b'CIUKI_TEST v=1 run=12345678 seq=000002 probe=boot event=DATA tick=10000\r\n'
        for replacement in (b'',record.replace(b'event=DATA',b'event=DA TA'),
                            record.replace(b' seq=000002',b' seq=00002')):
            with self.subTest(replacement=replacement):
                path.write_bytes(original.replace(record,replacement))
                results = import_sweep(self.capture,self.hash,cases)
                self.assertEqual(results[0]['outcome'],'not_run')
                self.assertEqual(results[0]['reason'],'records_lost=1')
                self.assertEqual(results[0]['boot_evidence'][0]['records_lost'],1)
                self.assertEqual(results[0]['boot_evidence'][0]['sequence_gaps'],[{'first':2,'last':2}])
                self.assertEqual(results[1]['evidence_outcome'],'pass')
                self.assertEqual(results[2]['observed'][-1]['event'],'PANIC')
                if replacement:
                    self.assertEqual(results[0]['boot_evidence'][0]['damaged_records'][0]['raw_hex'],replacement.hex())

    def test_historical_real_fixture_requires_verified_hash_override(self):
        import run as runner
        capture = FIXTURES/'44444444'; metadata = json.loads((capture/'acquisition.json').read_text())
        cases = runner.load_suite('f2-all')['cases']
        with self.assertRaisesRegex(EvidenceError,'write/readback'):
            import_sweep(capture,self.hash,cases)
        with self.assertRaisesRegex(EvidenceError,'write/readback'):
            import_sweep(capture,self.hash,cases,image_sha256='0'*64)
        results = import_sweep(capture,self.hash,cases,image_sha256=metadata['write_sha256'])
        self.assertEqual(results[0]['image_sha256'],metadata['write_sha256'])
        self.assertEqual(results[0]['image_identity'],{'canonical_sha256':self.hash,
            'override_sha256':metadata['write_sha256'],'historical_image':True})
        self.assertFalse(results[0]['sweep_complete'])
        fd = next(r for r in results if r['case'] == 'fd-table-qemu-t23')
        self.assertEqual(fd['evidence_outcome'],'fail')
        self.assertEqual(fd['evidence_reason'],'unexpected kernel panic: double_fault')
        self.assertEqual(fd['observed'][-1]['probe'],'panic')

    def test_missing_begin_is_loss_evidence_and_sequence_reset_is_not_a_reboot(self):
        cases = self.sweep_capture(); path = self.capture/'f0.log'; raw = path.read_bytes()
        path.write_bytes(raw.replace(b'CIUKI_TEST v=1 run=12345678 seq=000001 probe=boot event=BEGIN\r\n',b''))
        results = import_sweep(self.capture,self.hash,cases)
        self.assertEqual(results[0]['reason'],'records_lost=1')
        self.assertIn('boot',results[0]['boot_evidence'][0]['parser_errors'])
        self.assertEqual(results[1]['evidence_outcome'],'pass')
        path.write_bytes(raw.replace(b'CIUKI_TEST v=1 run=12345678 seq=000002 probe=boot event=DATA tick=10000',
            b'CIUKI_TEST v=1 run=12345678 seq=000001 probe=sweep event=BEGIN phase=3 step=1'))
        with self.assertRaisesRegex(EvidenceError,'sequence must increase'):
            import_sweep(self.capture,self.hash,cases)

    def test_completion_without_probe_is_not_run_and_bad_identity_still_refuses(self):
        self.sweep_capture(); path = self.capture/'f0.log'
        raw = (b'L:SELECT_SOURCE=cfg\nCiuki VMM F0 build 12ab34cd - CiukiOS\n'
            b'CIUKI_TEST v=1 run=12345678 seq=000001 probe=sweep event=BEGIN phase=3 step=10\n'
            b'CIUKI_TEST v=1 run=12345678 seq=000002 probe=panic event=SWEEP step=9 result=not_run reason=reset_before_completion\n')
        case = {'id':'panic','probe':'panic','expected':{'terminal':'PANIC'}}
        path.write_bytes(raw)
        result = import_sweep(self.capture,self.hash,[case])[0]
        self.assertEqual(result['evidence_reason'],'reset_before_completion')
        path.write_bytes(raw.replace(b'run=12345678 seq=000002',b'run=12345679 seq=000002').replace(b'reason=reset_before_completion',b'reason=reset before_completion'))
        with self.assertRaisesRegex(EvidenceError,'unapproved run'):
            import_sweep(self.capture,self.hash,[case])

    def test_fat_read_fixture_absence_retains_original_records(self):
        cases = self.sweep_capture()
        self.metadata.update(selector='all:sweep run=12345678')
        (self.capture/'acquisition.json').write_text(json.dumps(self.metadata))
        path = self.capture/'f0.log'
        path.write_bytes(b'L:SELECT_SOURCE=cfg\nCiuki VMM F0 build 12ab34cd - CiukiOS\n'
            b'CIUKI_TEST v=1 run=12345678 seq=000001 probe=sweep event=BEGIN phase=3 step=17\n'
            b'CIUKI_TEST v=1 run=12345678 seq=000002 probe=fat-read event=BEGIN\n'
            b'CIUKI_TEST v=1 run=12345678 seq=000003 probe=fat-read event=ERROR status=not_run reason=fixtures_absent\n'
            b'CIUKI_TEST v=1 run=12345678 seq=000004 probe=fat-read event=END status=NOT_RUN\n')
        result = import_sweep(self.capture,self.hash,[{'id':'fat-read','probe':'fat-read','expected':{'terminal':'END'}}])[0]
        self.assertEqual(result['outcome'],'not_run'); self.assertEqual(result['reason'],'fixtures_absent')
        self.assertEqual(result['observed'][-1]['status'],'NOT_RUN')

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
