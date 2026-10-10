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

    @unittest.skipUnless((FIXTURES/'66666666'/'f0.log').exists(), 'real T23 capture fixture is local only (legacy/local, never published)')
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
        for name, key in (('video-fallback', 'vga'), ('uart-absent-qemu-t23', 'serial')):
            result = next(r for r in results if r['case'] == name)
            self.assertEqual(result['outcome'], 'not_run')
            self.assertEqual(result['evidence_outcome'], 'not_run')
            self.assertEqual(result['reason'], f'device_exception_not_applicable:{key}=none')
        panic = next(r for r in results if r['case'] == 'panic')
        self.assertEqual(panic['boots'],[10]); self.assertEqual(panic['evidence_outcome'],'pass')
        self.assertIn('reset_before_completion',panic['not_run_reasons'])
        fat = next(r for r in results if r['case'] == 'fat-write')
        self.assertEqual(fat['boots'],[21,22])
        self.assertEqual(next(r for r in results if r['case'] == 'mount-crash-reboot')['boots'],[23,24])
        self.assertEqual(next(r for r in results if r['case'] == 'bootlog')['boots'],[25,26])
        self.assertEqual(next(r for r in results if r['case'] == 'app-gate-qemu-t23')['evidence_outcome'],'pass')

    @unittest.skipUnless((FIXTURES/'77777777'/'f0.log').exists(), 'real T23 capture fixture is local only (never published)')
    def test_real_damaged_provenance_fixture_imports_all_boots(self):
        import run as runner
        capture = FIXTURES/'77777777'
        metadata = json.loads((capture/'acquisition.json').read_text())
        cases = runner.load_suite('f2-all')['cases']
        results = import_sweep(capture, self.hash, cases, image_sha256=metadata['write_sha256'])
        self.assertEqual(len(results), len(cases))
        evidence = results[0]['boot_evidence']
        self.assertEqual(len(evidence), 35)
        self.assertTrue(all(b['records_lost'] == 0 for b in evidence))
        damaged = [b for b in evidence if b['provenance'] == 'damaged']
        self.assertEqual([b['boot'] for b in damaged], [14, 20])
        self.assertEqual([b['provenance_raw_line'] for b in damaged],
                         ['L:ELECT_SOURCE=cfg', 'L:SELECT_SOURCE=fg'])
        for boot in damaged:
            self.assertEqual(boot['provenance_inferred_from'], 'sweep continuity')
            self.assertEqual(bytes.fromhex(boot['provenance_raw_hex']).decode(), boot['provenance_raw_line'])
        self.assertEqual({k: results[0]['sweep_summary'][0][k] for k in ('passed', 'failed', 'not_run')},
                         {'passed': '29', 'failed': '4', 'not_run': '2'})
        self.assertEqual(next(r for r in results if r['case'] == 'app-gate-qemu-t23')['evidence_outcome'], 'pass')

    def cursor_capture(self, source=b'L:SELECT_SOURCE=fg', step=1, first_step=0, probe='fpu', arm=False):
        self.sweep_capture()
        banner = b'L:CPU signature=000006b1\n%s\nCiuki VMM F0 build 12ab34cd - CiukiOS\n'
        def record(p, seq, event, extra=''):
            return f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe={p} event={event}{extra}\n'.encode()
        first_probe = probe if arm else 'boot'
        first = banner % b'L:SELECT_SOURCE=cfg'
        first += record('sweep', 1, 'BEGIN', f' phase=3 step={first_step}')
        first += record(first_probe, 2, 'BEGIN')
        action = 'crash_cut' if probe == 'mount-crash' else 'cold_reboot'
        first += record(first_probe, 3, 'ARM' if arm else 'END', f' action={action}' if arm else ' status=PASS')
        second = banner % source
        second += record('sweep', 1, 'BEGIN', f' phase=3 step={step}')
        second += record(probe, 2, 'BEGIN') + record(probe, 3, 'END', ' status=PASS')
        (self.capture/'f0.log').write_bytes(first + second)
        return [{'id': probe, 'probe': probe, 'expected': {'terminal': 'END', 'predicates': []}}], first, second

    def test_damaged_provenance_requires_adjacent_same_run_cursor(self):
        cases, first, second = self.cursor_capture()
        result = import_sweep(self.capture, self.hash, cases)[0]
        self.assertEqual(result['outcome'], 'pass')
        self.assertEqual(result['boot_evidence'][1]['provenance_inferred_from'], 'sweep continuity')
        for raw in (second, first + second.replace(b'step=1', b'step=3'),
                    first.replace(b'phase=3', b'phase=2') + second):
            with self.subTest(raw=raw):
                (self.capture/'f0.log').write_bytes(raw)
                result = import_sweep(self.capture, self.hash, cases)[0]
                self.assertEqual(result['outcome'], 'not_run')
                self.assertEqual(result['evidence_reason'], 'provenance_damaged')
                self.assertNotIn('provenance_inferred_from', result['boot_evidence'][-1])
        self.metadata['selectors'] = ['all:sweep run=12345678', 'all:sweep run=87654321']
        (self.capture/'acquisition.json').write_text(json.dumps(self.metadata))
        (self.capture/'f0.log').write_bytes(first.replace(b'run=12345678', b'run=87654321') + second)
        self.assertEqual(import_sweep(self.capture, self.hash, cases)[0]['reason'], 'provenance_damaged')

    def test_intact_alternate_provenance_refuses_even_with_continuity(self):
        for source in (b'menu', b'fw_cfg', b'none'):
            cases, _, _ = self.cursor_capture(b'L:SELECT_SOURCE=' + source)
            with self.subTest(source=source), self.assertRaisesRegex(EvidenceError, 'non-BOOT.CFG'):
                import_sweep(self.capture, self.hash, cases)

    def test_damaged_non_ascii_source_retains_original_bytes(self):
        cases, _, _ = self.cursor_capture(b'L:SELECT_SOURCE=\xffg')
        result = import_sweep(self.capture, self.hash, cases)[0]
        self.assertEqual(result['outcome'], 'pass')
        self.assertEqual(bytes.fromhex(result['boot_evidence'][1]['provenance_raw_hex']), b'L:SELECT_SOURCE=\xffg')

    def test_missing_source_with_damaged_record_retains_loss_refusal(self):
        cases, first, second = self.cursor_capture(b'')
        second = second.replace(b'probe=fpu event=END', b'probe=fpu event=EN D')
        (self.capture/'f0.log').write_bytes(first + second)
        result = import_sweep(self.capture, self.hash, cases)[0]
        self.assertEqual(result['reason'], 'records_lost=1')
        self.assertEqual(result['boot_evidence'][1]['provenance_inferred_from'], 'sweep continuity')

    def test_missing_provenance_requires_damage_and_continuity(self):
        cases, first, second = self.cursor_capture(b'')
        with self.assertRaisesRegex(EvidenceError, 'lacks BOOT.CFG'):
            import_sweep(self.capture, self.hash, cases)
        # Loss of the first record is evidence of damage at the boot start.
        second = second.replace(b'seq=000001 probe=sweep', b'seq=000002 probe=sweep')
        second = second.replace(b'seq=000002 probe=fpu', b'seq=000003 probe=fpu')
        second = second.replace(b'seq=000003 probe=fpu event=END', b'seq=000004 probe=fpu event=END')
        (self.capture/'f0.log').write_bytes(first + second)
        result = import_sweep(self.capture, self.hash, cases)[0]
        self.assertEqual(result['reason'], 'records_lost=1')
        self.assertEqual(result['boot_evidence'][1]['provenance_inferred_from'], 'sweep continuity')
        self.assertIsNone(result['boot_evidence'][1]['provenance_raw_line'])
        (self.capture/'f0.log').write_bytes(second)
        self.assertEqual(import_sweep(self.capture, self.hash, cases)[0]['reason'], 'provenance_damaged')

    def test_repeated_cursor_is_only_allowed_for_armed_two_boot_probe(self):
        for probe, arm, inferred in (('fat-write', True, True), ('bootlog', True, True), ('mount-crash', True, True),
                                     ('fpu', True, False), ('fat-write', False, False)):
            cases, _, _ = self.cursor_capture(step=0, probe=probe, arm=arm)
            if arm and probe in ('fat-write', 'bootlog', 'mount-crash'):
                cases[0]['boots'] = [{'expected': {'terminal': 'ARM'}}, {'expected': {'terminal': 'END'}}]
            result = import_sweep(self.capture, self.hash, cases)[0]
            self.assertEqual('provenance_inferred_from' in result['boot_evidence'][1], inferred)
            if not inferred: self.assertEqual(result['reason'], 'provenance_damaged')

    def test_device_exception_metadata_controls_predicates_and_prerequisites(self):
        cases = self.sweep_capture()
        cases[0]['device_exceptions'] = {'vga': 'none'}
        cases[0]['expected']['predicates'] = [{'where': {'event': 'DATA'}, 'fields': {'tick': {'eq': 1}}}]
        for devices, outcome in (({}, 'not_run'), ({'vga': 'std'}, 'not_run'), ({'vga': 'none'}, 'fail')):
            self.metadata['devices'] = devices
            (self.capture/'acquisition.json').write_text(json.dumps(self.metadata))
            result = import_sweep(self.capture, self.hash, cases)
            self.assertEqual(result[0]['outcome'], outcome)
            if outcome == 'not_run':
                self.assertEqual(result[0]['reason'], 'device_exception_not_applicable:vga=none')
                self.assertEqual(result[0]['device_exceptions_not_applicable'], [result[0]['reason']])
                self.assertEqual(result[1]['outcome'], 'pass')
            else: self.assertEqual(result[1]['outcome'], 'not_run')
        cases[0]['expected']['predicates'] = []
        (self.capture/'acquisition.json').write_text(json.dumps(self.metadata))
        self.assertEqual(import_sweep(self.capture, self.hash, cases)[0]['outcome'], 'pass')

    def test_qemu_block_variants_require_explicit_physical_configuration(self):
        for key, value in (('disk_cache', 'writethrough'), ('fault', {'layer': 'host-block-backend', 'errno': 5})):
            cases = self.sweep_capture(); cases[1][key] = value
            result = import_sweep(self.capture, self.hash, cases)[1]
            self.assertEqual(result['outcome'], 'not_run')
            self.assertTrue(result['reason'].startswith('device_exception_not_applicable:' + key + '='))
            self.metadata['machine_variants'] = {key: value}
            (self.capture/'acquisition.json').write_text(json.dumps(self.metadata))
            self.assertEqual(import_sweep(self.capture, self.hash, cases)[1]['outcome'], 'pass')

    def test_qemu_only_safe_selector_flag_is_not_applicable_on_hardware(self):
        # Lead amendment to f1-35: ` safe=1` is accepted by the kernel only under QEMU.
        cases = self.sweep_capture(); cases[0]['selector'] = 'f0:boot run={run_id} safe=1'
        result = import_sweep(self.capture, self.hash, cases)
        self.assertEqual(result[0]['outcome'], 'not_run')
        self.assertEqual(result[0]['reason'], 'selector_flag_not_applicable:safe=1')
        self.assertEqual(result[1]['outcome'], 'pass')
        self.metadata['machine_variants'] = {'safe': 1}
        (self.capture/'acquisition.json').write_text(json.dumps(self.metadata))
        self.assertEqual(import_sweep(self.capture, self.hash, cases)[0]['outcome'], 'pass')

    def test_qemu_timing_domain_literal_means_hardware_on_physical_boots(self):
        # Lead amendment to f1-35: records say icount under QEMU and hardware otherwise.
        cases = self.sweep_capture()
        raw = (self.capture/'f0.log').read_bytes().replace(b' tick=10000', b' tick=10000 timing_domain=hardware')
        (self.capture/'f0.log').write_bytes(raw)
        for literal, outcome in (('icount', 'pass'), ('hardware', 'pass'), ('tcg', 'fail')):
            cases[0]['expected']['predicates'] = [{'where': {'event': 'DATA'}, 'fields': {'timing_domain': literal}}]
            self.assertEqual(import_sweep(self.capture, self.hash, cases)[0]['outcome'], outcome, literal)

    def test_operator_input_records_replace_exact_stimulus_totals(self):
        # Lead amendment to f1-34: criteria=operator records are judged by thresholds.
        cases = self.sweep_capture()
        banner = 'L:CPU signature=000006b1 family=00000006\r\nL:SELECT_SOURCE=cfg\r\nCiuki VMM F0 build 12ab34cd - CiukiOS\r\n'
        def record(seq, event, extra=''):
            return f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=input event={event}{extra}\r\n'
        def capture(criteria):
            raw = banner + record(1, 'BEGIN') + record(2, 'DATA', f' case=counts criteria={criteria} characters=113')
            raw += record(3, 'DATA', ' group=input text_count=113 key_transitions=200 button_transitions=74 motion_x=3667 motion_y=-4760')
            raw += record(4, 'DATA', ' group=input loss=0 duplicates=0 stuck=0 owner_errors=0') + record(5, 'END', ' status=PASS')
            (self.capture/'f0.log').write_bytes(raw.encode())
        exact = {'terminal': 'END', 'predicates': [{'where': {'event': 'DATA', 'group': 'input'}, 'combine': True, 'fields': {
            'text_count': {'eq': 100}, 'key_transitions': {'eq': 200}, 'button_transitions': {'eq': 20},
            'motion_x': {'eq': 200}, 'motion_y': {'eq': -100}}}]}
        cases = [{'id': 'input-qemu-t23', 'probe': 'input', 'expected': exact}]
        capture('operator')
        self.assertEqual(import_sweep(self.capture, self.hash, cases)[0]['outcome'], 'pass')
        capture('exact')
        self.assertEqual(import_sweep(self.capture, self.hash, cases)[0]['outcome'], 'fail')

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

    @unittest.skipUnless((FIXTURES/'66666666'/'f0.log').exists(), 'real T23 capture fixture is local only (legacy/local, never published)')
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

    def operator_capture(self, probe, fail=False):
        self.sweep_capture()
        def record(p, seq, event, extra=''):
            return f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe={p} event={event}{extra}\r\n'
        banner='L:CPU signature=000006b1\r\nL:SELECT_SOURCE=cfg\r\nCiuki VMM F0 build 12ab34cd - CiukiOS\r\n'
        subcase='stimulus' if probe=='input' else 'interaction'
        server='' if probe=='input' else ' server=desktop'
        raw=banner+record(probe,1,'BEGIN')
        raw+=record(probe,2,'DATA',' case=setup result=0' if probe=='input' else ' case=cycles server=desktop cycles=100 desktop_restarts=0')
        raw+=record(probe,3,'DATA',f'{server} subcase={subcase} status=not_run reason=operator_absent')
        raw+=record(probe,4,'DATA',' case=lease active=1 quarantined=0' if probe=='input' else ' case=ledger objects_equal=1 processes_equal=1')
        raw+=record(probe,5,'END',f'{server} status='+('FAIL reason=independent_failure' if fail else 'NOT_RUN reason=operator_absent'))
        raw+=record(probe,6,'SWEEP',' step=0 result='+('fail' if fail else 'not_run reason=operator_absent'))
        raw+=banner+record('boot',1,'BEGIN')+record('boot',2,'END',' status=PASS')
        raw+=record('sweep',3,'SWEEP_END',f' passed=1 failed={int(fail)} not_run={int(not fail)}')
        (self.capture/'f0.log').write_bytes(raw.encode())
        return [{'id':p,'probe':p,'expected':{'terminal':'END','predicates':[]}} for p in (probe,'boot')]

    def test_operator_absence_maps_to_confirmation_without_blocking_later_cases(self):
        for probe in ('input','crash-isolation'):
            cases=self.operator_capture(probe)
            for confirmed in (False,True):
                self.metadata['case_confirmations']={probe:confirmed}
                (self.capture/'acquisition.json').write_text(json.dumps(self.metadata))
                results=import_sweep(self.capture,self.hash,cases)
                self.assertEqual([r['outcome'] for r in results],['not_run','pass'])
                self.assertEqual(results[0]['reason'],'operator_absent')
                self.assertTrue(results[0]['operator_confirmation_required'])
                self.assertEqual(results[0]['operator_confirmation'],confirmed)
                self.assertEqual(results[0]['not_run_subcases'],[{'subcase':'stimulus' if probe=='input' else 'interaction','reason':'operator_absent'}])
                self.assertEqual(results[0]['observed'][1].get('case'),'setup' if probe=='input' else 'cycles')
                self.assertEqual(results[0]['sweep_summary'][0]['not_run'],'1')

    def test_operator_absence_does_not_hide_independent_failure(self):
        for probe in ('input','crash-isolation'):
            cases=self.operator_capture(probe,fail=True)
            results=import_sweep(self.capture,self.hash,cases)
            self.assertEqual(results[0]['outcome'],'fail')
            self.assertTrue(results[0]['not_run_subcases'])
            self.assertEqual(results[1]['outcome'],'not_run')
            self.assertIn('prerequisite failed',results[1]['reason'])

    def test_operator_records_reject_missing_wrong_duplicate_and_pass_subcases(self):
        cases=self.operator_capture('input');path=self.capture/'f0.log';raw=path.read_bytes()
        for before,after in ((b'subcase=stimulus',b'subcase=interaction'),
                             (b'status=NOT_RUN reason=operator_absent',b'status=PASS'),
                             (b'event=DATA subcase=stimulus status=not_run reason=operator_absent',b'event=DATA case=motion x=0'),
                             (b'L:SELECT_SOURCE=cfg',b'L:SELECT_SOURCE=menu')):
            path.write_bytes(raw.replace(before,after))
            with self.subTest(before=before),self.assertRaises(EvidenceError):
                import_sweep(self.capture,self.hash,cases)
        duplicate=b'CIUKI_TEST v=1 run=12345678 seq=000004 probe=input event=DATA subcase=stimulus status=not_run reason=operator_absent\r\n'
        path.write_bytes(raw.replace(b' seq=000004 probe=input',b' seq=000005 probe=input').replace(b' seq=000005 probe=input event=END',b' seq=000006 probe=input event=END').replace(b' seq=000006 probe=input event=SWEEP',b' seq=000007 probe=input event=SWEEP').replace(b'CIUKI_TEST v=1 run=12345678 seq=000005 probe=input event=DATA case=lease',duplicate+b'CIUKI_TEST v=1 run=12345678 seq=000005 probe=input event=DATA case=lease'))
        with self.assertRaisesRegex(EvidenceError,'operator_absent subcase'):
            import_sweep(self.capture,self.hash,cases)

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
