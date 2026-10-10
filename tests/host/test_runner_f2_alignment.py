"""F2-13: replay supplied guest records, keeping failures as failures.

References/decision: POSIX nanosleep requires EINTR and a bounded remainder
(https://pubs.opengroup.org/onlinepubs/9799919799/functions/nanosleep.html).
Hashlib is the independent byte/digest oracle, including captured native
streams (https://docs.python.org/3/library/hashlib.html). No guest PASS or
application success string substitutes for measured fields or wait status.
The interim controllers do not implement every acceptance-table workload;
these tests identify missing measurements rather than fabricate coverage.
"""
import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import run as runner
from evidence import Parser as RecordParser, EvidenceError

FIXTURES = ROOT / 'tests/host/fixtures/f2'
SUITES = ('f2-process', 'f2-runtime', 'f2-app')


def cases():
    return [c for name in SUITES for c in runner.load(ROOT / f'tests/suites/{name}.json')['cases']]


def fixture_records(probe):
    return [dict(token.split('=', 1) for token in line.split()[1:])
            for line in (FIXTURES / f'{probe}.records').read_text().splitlines()]


def replay(probe, records):
    parser = runner.Parser(records[0]['run'], probe)
    for r in records:
        parser.feed(('CIUKI_TEST ' + ' '.join(f'{k}={v}' for k, v in r.items())).encode())
    return parser


def data_projection(probe, records, predicate):
    # Test one DATA predicate independently of a failed boot's terminal.
    # This is not guest qualification: complete replays below retain FAIL.
    parser = RecordParser(records[0]['run'], probe)
    parser.records = copy.deepcopy(records)
    parser.terminal = {'event': 'END', 'status': 'PASS'}
    return parser.check({'terminal': 'END', 'predicates': [predicate]})


def captured_records(probe, data):
    records = []
    def add(**fields):
        records.append(dict(v='1',run='12345678',seq=f'{len(records)+1:06d}',probe=probe,**fields))
    add(event='BEGIN')
    if probe == 'app-gate':
        add(event='DATA',case='lua-basic',exit='0',final_ok='1',assertion_failures='0')
    add(event='DATA',group='capture',pid='2',stream='stdout',bytes=str(len(data)),truncated=str(int(len(data)>2048)))
    add(event='DATA',group='capture_digest',pid='2',stream='stdout',sha256=hashlib.sha256(data).hexdigest())
    windows = [(0,data)] if len(data)<=2048 else [(0,data[:1024]),(len(data)-1024,data[-1024:])]
    for offset, window in windows:
        for i in range(0,len(window),24):
            chunk=window[i:i+24]
            add(event='DATA',group='app',pid='2',tid='0',stream='stdout',offset=str(offset+i),bytes=str(len(chunk)),data_hex=chunk.hex())
    add(event='END',status='PASS')
    return records


class F2AlignmentTests(unittest.TestCase):
    def test_source_derived_supplement_output_is_not_controller_evidence(self):
        if not any('stdout_cases' in c['expected'] or 'application_reports' in c['expected']
                   for c in cases() if c['probe'] == 'app-gate'):
            self.skipTest('f2-15 app-gate uses controller-side predicates and provenance')
        source = (ROOT/'apps/lua/ciuki-f2.lua').read_text()
        output = []
        h=2166136261
        for i in range(65536): h=((h^(255-i%251 if 8192<=i<12288 else i%251))*16777619)&0xffffffff
        for name in ('file-roundtrip','allocation','time-utc','console'):
            body=source.split(f'case("{name}", function()',1)[1].split('\nend)',1)[0]
            text=re.search(r'return (?:string.format\()?"([^"]+)"',body)[1].replace('%08x',f'{h:08x}')
            output.append(f'case={name} ok=1 {text}\n')
        data=''.join(output).encode()
        # A marker-looking application line stays in the escaped stream.
        data+=b'CIUKI_TEST v=1 run=12345678 seq=999999 probe=app-gate event=END status=PASS\n'
        for case in (c for c in cases() if c['probe']=='app-gate'):
            parser=replay('app-gate',captured_records('app-gate',data))
            self.assertTrue(parser.check(case['expected']))
            self.assertEqual(sum(r['event']=='END' for r in parser.records),1)
            self.assertFalse(any(r.get('case')=='allocation' for r in parser.records))
            for bad in (data.replace(b'case=allocation ok=1',b'case=allocation ok=0'),
                        data+output[0].encode(),data.replace(b'bytes=65536',b'bytes=65535')):
                with self.assertRaises(EvidenceError):
                    replay('app-gate',captured_records('app-gate',bad)).check(case['expected'])

    def test_truncated_capture_geometry_and_digest_verification_limit(self):
        self.assertIn('#define SUPERVISOR_CAPTURE_BYTES 1024u',(ROOT/'src/kernel/include/ciuki/supervisor.h').read_text())
        records=captured_records('app-gate',b'x'*3500+b'\nfinal OK !!!\n')
        parser=replay('app-gate',records)
        self.assertTrue(parser.check({'terminal':'END'}))
        self.assertEqual(len(next(iter(parser.captures.values()))['data']),2048)
        result={'probe':'app-gate','outcome':'fail','build_manifest':{'path':'unknown'}}
        runner.record_f2_result(result,parser)
        self.assertFalse(result['captured_streams'][0]['full_digest_verified'])
        changed=copy.deepcopy(records)
        next(r for r in changed if r.get('group')=='app' and int(r['offset'])>1024)['offset']='1025'
        with self.assertRaisesRegex(EvidenceError,'offset/range'):replay('app-gate',changed)
        changed=copy.deepcopy(records)
        next(r for r in changed if r.get('group')=='capture')['truncated']='0'
        with self.assertRaisesRegex(EvidenceError,'retention declaration'):replay('app-gate',changed)

    def test_real_records_for_every_profile_and_predicate(self):
        added = {'fd-table': {'seek', 'growth', 'fd-slots', 'fd-reuse', 'directory-digest'},
                 'spawn-wait': {'fault'}, 'mmap': {'fault'}, 'threads-wait': {'thread'}}
        for case in cases():
            probe = case['probe']
            with self.subTest(case=case['id']):
                if probe == 'app-gate' and not (FIXTURES / 'app-gate.records').exists():
                    source = (ROOT / 'apps/lua/ciuki-f2.lua').read_text()
                    if 'stdout_cases' in case['expected']:
                        self.assertEqual(set(re.findall(r'\ncase\("([^"]+)"', source)),
                                         {d['case'] for d in case['expected']['stdout_cases']})
                    missing = replay(probe, [dict(v='1', run='12345678', seq='000001', probe=probe, event='BEGIN'),
                                            dict(v='1', run='12345678', seq='000002', probe=probe, event='READY', table='f2', installed='8'),
                                            dict(v='1', run='12345678', seq='000003', probe=probe, event='ERROR', status='not_run', reason='missing_probe')])
                    with self.assertRaises(EvidenceError): missing.check(case['expected'])
                    continue
                records = fixture_records(probe)
                for predicate in case['expected']['predicates']:
                    where = predicate['where']
                    matches = [r for r in records if all(r.get(k) == str(v) for k, v in where.items())]
                    new = where.get('operation', where.get('case')) in added.get(probe, set())
                    missing_fixture = probe == 'fd-table' and (where.get('operation') in ('exdev', 'readonly') or where['event'] == 'ARM')
                    signal_failure = probe == 'signals-fault' and (
                        where.get('index') == 7 or
                        where == {'event':'DATA','case':'fault-repair','part':'context'} or
                        where.get('part') == 'status' and where.get('case') in ('fault-repair', 'nanosleep-eintr') or
                        where.get('part') == 'handlers' and where.get('case') in ('fault-repair', 'channel-eintr') or
                        where.get('part') == 'sleep' and where.get('case') == 'nanosleep-eintr' or
                        where.get('syscall') in ('nanosleep', 'channel_recv'))
                    with self.subTest(where=where):
                        if new or missing_fixture or signal_failure:
                            with self.assertRaises(EvidenceError): data_projection(probe, records, predicate)
                            continue
                        self.assertTrue(matches)
                        self.assertTrue(data_projection(probe, records, predicate))
                        # Every observed field is independently necessary; a
                        # later good record must not hide an earlier bad one.
                        for field in predicate['fields']:
                            changed = copy.deepcopy(records)
                            next(r for r in changed if all(r.get(k) == str(v) for k, v in where.items()))[field] = 'invalid'
                            with self.subTest(field=field), self.assertRaises(EvidenceError):
                                data_projection(probe, changed, predicate)

    def test_complete_replays_keep_actual_failures_and_missing_new_records(self):
        for case in cases():
            probe = case['probe']
            if probe == 'app-gate': continue
            records = fixture_records(probe)
            with self.subTest(case=case['id']):
                if probe == 'fd-table':
                    # This supplied capture has an ERROR for its absent second
                    # volume, followed by FAIL. Do not edit it into a pass.
                    self.assertEqual(records[-1]['status'], 'FAIL')
                    with self.assertRaisesRegex(EvidenceError, 'missing probe requires READY'):
                        replay(probe, records)
                else:
                    parser = replay(probe, records)
                    if probe == 'elf-load': self.assertTrue(parser.check(case['expected']))
                    else:
                        with self.assertRaises(EvidenceError): parser.check(case['expected'])
                    if probe == 'signals-fault':
                        self.assertEqual(parser.terminal['status'], 'FAIL')
                        with self.assertRaisesRegex(EvidenceError, 'kernel reported FAIL'):
                            parser.check(case['expected'])

    def test_old_native_libc_frames_and_independent_stream_digests(self):
        case = next(c for c in cases() if c['probe'] == 'libc-smoke')
        expected = copy.deepcopy(case['expected'])
        # Snapshot predates the three newly added clock measurements.
        expected['application_reports'] = expected['application_reports'][:3]
        records = fixture_records('libc-smoke')
        parser = replay('libc-smoke', records)
        self.assertFalse(any(c['verified'] for c in parser.captures.values()))
        self.assertTrue(parser.check(expected))
        self.assertTrue(all(c['verified'] for c in parser.captures.values()))
        self.assertEqual(len(parser.captures), 2)
        self.assertTrue(all(c['end'] == c['size'] for c in parser.captures.values()))
        self.assertFalse(any(r.get('case') == 'atexit' for r in parser.records))
        for stream in ('stdout', 'stderr'):
            changed = copy.deepcopy(records)
            next(r for r in changed if r.get('group') == 'capture_digest' and r['stream'] == stream)['sha256'] = '0' * 64
            with self.subTest(stream=stream), self.assertRaisesRegex(EvidenceError, 'size/digest'):
                replay('libc-smoke', changed).check({'terminal':'END'})
        changed = copy.deepcopy(records)
        next(r for r in changed if r.get('group') == 'app' and r.get('stream') == 'stdout')['offset'] = '1'
        with self.assertRaisesRegex(EvidenceError, 'offset/range'): replay('libc-smoke', changed)
        changed = copy.deepcopy(records)
        next(r for r in changed if r.get('group') == 'capture' and r['stream'] == 'stdout')['bytes'] = '37'
        with self.assertRaisesRegex(EvidenceError, 'size/digest'):
            replay('libc-smoke', changed).check({'terminal':'END'})

    def test_ledgers_compare_real_baselines_not_presence(self):
        case = next(c for c in cases() if c['probe'] == 'spawn-wait')
        expected = {'terminal': 'END', 'comparisons': case['expected']['comparisons']}
        records = fixture_records('spawn-wait')
        self.assertTrue(replay('spawn-wait', records).check(expected))
        for case_name, field in (('memory_ledger', 'pages_free'), ('object_ledger', 'handles')):
            changed = copy.deepcopy(records)
            target = next(r for r in changed if r.get('case') == case_name and r.get('stage') == 'final')
            target[field] = str(int(target[field]) + 1)
            with self.subTest(field=field), self.assertRaisesRegex(EvidenceError, 'comparison failed'):
                replay('spawn-wait', changed).check(expected)
        changed = records + [next(r for r in records if r.get('case') == 'memory_ledger')]
        parser = replay('spawn-wait', records)
        parser.records = changed
        with self.assertRaisesRegex(EvidenceError, 'duplicate comparison'): parser.check(expected)

    def test_clock_seed_qualified_rtc_and_build_fallback(self):
        records = [r for r in fixture_records('fd-table') if r['event'] in ('BEGIN', 'DATA')]
        records.append(dict(v='1',run=records[0]['run'],seq='000100',probe='fd-table',event='END',status='PASS'))
        case = next(c for c in cases() if c['probe'] == 'fd-table')
        expected = {'terminal': 'END', 'clock_seed': True,
                    'predicates': [p for p in case['expected']['predicates'] if p['where'].get('case', '').startswith('clock-')]}
        self.assertTrue(replay('fd-table', records).check(expected))
        for source in (0, 1):
            changed = copy.deepcopy(records)
            seed = next(r for r in changed if r.get('case') == 'clock-source')
            uname = next(r for r in changed if r.get('case') == 'clock-uname')
            offset = next(r for r in changed if r.get('case') == 'clock-offset')
            seed.update(realtime_source=str(source), source='rtc' if source else 'build', sample='10' if source else '0')
            seed.update(qualified='1',valid=str(source))
            uname.update(realtime_source=str(source),clock_source=str(source))
            offset.update(expected_ms=str(int(seed['utc'])*1000-int(seed['sample'])), observed_ms=str(int(seed['utc'])*1000-int(seed['sample'])))
            self.assertTrue(replay('fd-table', changed).check(expected))
            if source == 0:
                seed['valid']='1'
                with self.assertRaisesRegex(EvidenceError,'seed/provider'): replay('fd-table', changed).check(expected)
                seed['valid']='0'
            seed['source'] = 'build' if source else 'rtc'
            with self.assertRaisesRegex(EvidenceError, 'seed/provider'): replay('fd-table', changed).check(expected)

    def test_embedded_elf_digest_uses_build_artifact_and_detects_mismatch(self):
        case = next(c for c in cases() if c['probe'] == 'elf-load')
        scratch = ROOT / 'build/runner-host-tests'; scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as d:
            root = Path(d); artifact = root / 'build/f0/proc-payload.elf'; artifact.parent.mkdir(parents=True)
            artifact.write_bytes(b'ELF fixture bytes')
            record = dict(event='DATA',case='image',file_bytes=str(artifact.stat().st_size),sha256=runner.sha(artifact))
            with patch.object(runner, 'ROOT', root):
                result = {'observed':[record],'digests':[]}
                runner.check_digests(None,None,root,case['digests'],result)
                self.assertEqual(len(result['digests']),1)
                record['sha256'] = '0'*64
                with self.assertRaisesRegex(EvidenceError,'ELF digest'): runner.check_digests(None,None,root,case['digests'],result)
                artifact.unlink()
                with self.assertRaisesRegex(EvidenceError,'comparison source'): runner.check_digests(None,None,root,case['digests'],result)

    def test_fallback_epoch_is_compared_to_canonical_image_manifest(self):
        scratch=ROOT/'build/runner-host-tests';scratch.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            manifest=Path(directory)/'manifest.json';manifest.write_text(json.dumps({'build_epoch':123}))
            def parser(epoch):
                records=[dict(v='1',run='12345678',seq='000001',probe='fd-table',event='BEGIN'),
                         dict(v='1',run='12345678',seq='000002',probe='fd-table',event='DATA',case='clock-source',
                              source='build',realtime_source='0',qualified='0',valid='0',utc=str(epoch),sample='0'),
                         dict(v='1',run='12345678',seq='000003',probe='fd-table',event='END',status='PASS')]
                return replay('fd-table',records)
            def result():return {'probe':'fd-table','outcome':'pass','build_manifest':{'path':str(manifest)}}
            evidence=result();runner.record_f2_result(evidence,parser(123))
            self.assertTrue(evidence['clock_seed_comparison']['match'])
            with self.assertRaisesRegex(EvidenceError,'clock seed differs'):runner.record_f2_result(result(),parser(124))
            manifest.write_text('{}')
            with self.assertRaisesRegex(EvidenceError,'clock seed differs'):runner.record_f2_result(result(),parser(123))

    def test_fixture_disk_checker_paths_and_hashes_follow_production(self):
        source = (ROOT / 'src/kernel/probes/f2_probes_files.c').read_text()
        digest = hashlib.sha256(b'CiukiOS F2 durable\n').hexdigest()
        for case in cases():
            if case['probe'] != 'fd-table': continue
            self.assertEqual(case['fixtures'], [{'fat_type':16,'seed':1,'generator':'mkfs.fat'}])
            self.assertIn('path[] = "/tmp/f2-durable.bin"',source)
            self.assertEqual(case['checks']['files'],[{'path':'::/tmp/f2-durable.bin','sha256':digest}])
            self.assertEqual(case['digests'][0]['path'],'/tmp/f2-durable.bin')
            self.assertEqual(case['checks']['fsck_exit_codes'],[0])

    def test_new_record_functions_compile_and_supply_real_shape(self):
        # Compile the production functions, not a Python imitation. Boundary
        # values exercise formatting and counters independently of the suite.
        scratch = ROOT / 'build/runner-host-tests'; scratch.mkdir(parents=True, exist_ok=True)
        files = (ROOT / 'src/kernel/probes/f2_probes_files.c').read_text()
        process = (ROOT / 'src/kernel/probes/f2_probes_process.c').read_text()
        libc = (ROOT / 'sdk/tests/libc_smoke.c').read_text()
        def block(source, start, end): return source.split(start,1)[1].split(end,1)[0]
        functions = 'static void record_slots' + block(files,'static void record_slots','static int kernel_io')
        functions += 'struct payload_result {' + block(process,'struct payload_result {','static int image_read')
        functions += 'static void record_fault' + block(process,'static void record_fault','static bool run_payload')
        clock = 'static void clock_report' + block(libc,'static void clock_report','static uint32_t signal_start')
        harness = '''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void _exit(int) __attribute__((noreturn));
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/sha256.h>
void rec_emit(const char *probe, const char *event, const char *fmt, ...) {
  printf("%s %s ",probe,event); va_list ap; va_start(ap,fmt); vprintf(fmt,ap); va_end(ap); putchar('\\n');
}
#define CU_PTR(p) ((uintptr_t)(p))
static int ciuki_error(int value) { return value; }
static int ciuki_raw_probe_report(uintptr_t p, int n, int a, int b, int c, int d) {
  (void)a;(void)b;(void)c;(void)d; fwrite((void *)p,1,n,stdout); putchar('\\n'); return 0;
}
''' + functions + clock + '''
int main(void) {
  struct proc_fd slots[CIUKI_OPEN_MAX]={0};
  struct process p = {.fds=slots}; struct proc_object object = {0};
  for(unsigned i=0;i<CIUKI_OPEN_MAX;i++) p.fds[i].object=&object;
  record_slots(&p); p.fds[73].object=0; record_slots(&p);
  record_seek(4294967305LL,0,17,17);
  struct sha256_ctx names;sha256_init(&names);
  sha256_update(&names,".\\0..\\0Moved\\0",11);record_directory_digest(&names);
  char bytes[64]={0}; record_growth(bytes,sizeof(bytes)); bytes[63]=1;
  if(record_growth(bytes,sizeof(bytes))!=1) return 1;
  p.pid=3;p.fault_vector=14;p.fault_error=7;p.fault_address=0x00400000;p.fault_eip=0x004000ab;
  record_fault("mmap",&p);
  struct payload_result r={0};for(unsigned i=0;i<8;i++)r.tids[i]=100+i;
  record_thread_ids("threads-wait",&r);
  clock_report("clock-monotonic","reads",10000,"decreases",0,"sleep_ns",20000000);
  clock_report("clock-cpu","busy_ns",1,"sleep_cpu_ns",2000000,"requested_sleep_ns",100000000);
  clock_report("sleep-interrupt","result",-1,"errno",4,"remainder_ns",20000000);
  return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=scratch) as d:
            d = Path(d); src=d/'records.c'; binary=d/'records';src.write_text(harness)
            compiled=subprocess.run(['clang','-std=c17','-Wall','-Wextra','-Werror','-DFS_HOST','-D_POSIX_C_SOURCE=200809L','-I',str(ROOT/'src/kernel/include'),str(src),str(ROOT/'src/kernel/lib/sha256.c'),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            out=subprocess.run([str(binary)],check=True,capture_output=True,text=True).stdout.splitlines()
        self.assertIn('fd-table DATA operation=fd-slots slots=128 limit=128',out)
        self.assertIn('fd-table DATA operation=fd-slots slots=127 limit=128',out)
        self.assertIn('fd-table DATA operation=growth size=64 zero_errors=0',out)
        self.assertIn('fd-table DATA operation=growth size=64 zero_errors=1',out)
        emitted=[dict(t.split('=',1) for t in line.split()[2:]) for line in out if line.startswith(('fd-table','mmap','threads-wait'))]
        for case in cases():
            probe=case['probe']
            for predicate in case['expected']['predicates']:
                where=predicate['where']
                if where.get('operation') in ('fd-slots','growth','seek','directory-digest') or where.get('case') in ('fault','thread') and probe in ('mmap','threads-wait'):
                    records=[dict(v='1',run='12345678',event='DATA',**r) for r in emitted if all(r.get(k)==str(v) for k,v in where.items() if k!='event')]
                    # One fault is emitted by this host fixture; two separate
                    # processes are required by the actual mmap suite.
                    pred=copy.deepcopy(predicate)
                    if where.get('case')=='fault':pred['exact_count']=1
                    if where.get('operation') in ('fd-slots','growth','seek','directory-digest'):records=records[:1]
                    self.assertTrue(data_projection(probe,records,pred))
        parser=replay('libc-smoke',fixture_records('libc-smoke'))
        parser.report_bytes.extend(''.join(line for line in out if line.startswith('case=')).encode())
        libc_case=next(c for c in cases() if c['probe']=='libc-smoke')
        self.assertTrue(parser.check(libc_case['expected']))
        parser.report_bytes=parser.report_bytes.replace(b'remainder_ns=20000000',b'remainder_ns=20000001')
        with self.assertRaisesRegex(EvidenceError,'report predicate'):parser.check(libc_case['expected'])
