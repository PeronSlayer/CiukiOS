"""Draft record/framing/verdict host checks; all telemetry here is synthetic."""
import copy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'scripts/test'))
from win32_evidence import Parser, EvidenceError, LEDGERS

IMAGE = 'a'*64
PAYLOAD = 'b'*64
HOST = {k:True for k in ('installed_files','menu_screen','play_screen','game_progress',
    'audio_reference','desktop_screen','input_acknowledged','focus_screens','uninstalled_files')}
HOST['input_ids'] = [1]


def canned():
    records = [{'event':'BEGIN','image_sha256':IMAGE},
               {'event':'DATA','case':'identity','payload_sha256':PAYLOAD,'scene':'default-play'}]
    def data(case, **fields): records.append({'event':'DATA','case':case, **fields})
    def begin(name, ms): data('segment', segment=name, op='begin', guest_ms=ms, icount=ms*100)
    def end(name, ms): data('segment', segment=name, op='end', guest_ms=ms, icount=ms*100)
    begin('install',0)
    data('install', segment='install', default_choices=1, system_writes=0, menu_entry=1, uninstall_registered=1)
    end('install',100)
    begin('start',100)
    for kind in sorted(LEDGERS): data('ledger',segment='start',when='pre',kind=kind,count=0,guest_ms=100)
    data('process',segment='start',pid=7,op='start',guest_ms=100)
    for stage in ('map','imports','tls','entry'): data('loader',segment='start',pid=7,stage=stage,status='PASS')
    end('start',200)
    begin('play',200)
    records.append({'event':'ARM','segment':'play','action':'script'})
    data('mode',segment='play',pid=7,width=640,height=480,bpp=8)
    data('progress',segment='play',pid=7,checkpoint='scene-start',counter=1)
    data('input',segment='play',pid=7,input_id=1,latency_us=10000,latency_frames=1)
    data('progress',segment='play',pid=7,checkpoint='scene-end',counter=2)
    data('frames',segment='play',pid=7,duration_ms=180000,game_frames=5400,presents=10800)
    data('fps',segment='play',pid=7,avg_milli=30000,median_milli=30000,p05_milli=26000,p95_us=38000,samples=5400)
    data('audio',segment='play',pid=7,qualified=1,underruns=0,consumed_frames=7938000,non_silent_frames=7000000)
    data('commit',segment='play',pid=7,peak_bytes=123456)
    end('play',180200)
    begin('focus',180200)
    data('focus',segment='focus',pid=7,round_trips=10,corrupt=0)
    end('focus',190200)
    begin('exit',190200)
    data('process',segment='exit',pid=7,op='exit',guest_ms=190250,code=0)
    for kind in sorted(LEDGERS): data('ledger',segment='exit',when='post',kind=kind,count=0,guest_ms=190300)
    end('exit',190300)
    begin('uninstall',190300)
    data('uninstall',segment='uninstall',key_removed=1,menu_removed=1)
    end('uninstall',190400)
    records.append({'event':'END','status':'PASS'})
    return records


def wire(record, seq, probe='win32-diablo-shareware'):
    fields = {'v':1,'run':'1234abcd','seq':f'{seq:06}','probe':probe, **record}
    return ('CIUKI_TEST '+' '.join(f'{k}={v}' for k,v in fields.items())+'\r\n').encode()


def parser(records=None, probe='win32-diablo-shareware'):
    p = Parser('1234abcd', probe, IMAGE, PAYLOAD, 'default-play')
    for seq, record in enumerate(records if records is not None else canned(), 1):
        p.feed(wire(record, seq, probe))
    return p


class EvidenceTests(unittest.TestCase):
    def verdict(self, p, host=None, **kwargs):
        return p.verdict(HOST if host is None else host, focus=True, uninstall=True, **kwargs)

    def test_complete_segmented_evidence_passes_only_with_independent_checks(self):
        p = parser()
        self.assertEqual(self.verdict(p)['outcome'], 'pass')
        self.assertEqual(p.segments['play']['end']-p.segments['play']['begin'], 180000)
        result = self.verdict(p, {})
        self.assertEqual(result['outcome'], 'not_run')
        self.assertIn('menu_screen', result['missing'])
        self.assertEqual(self.verdict(p, {**HOST,'play_screen':False})['outcome'], 'fail')
        self.assertEqual(self.verdict(p, {**HOST,'input_ids':[2]})['outcome'], 'fail')

    def test_records_fit_existing_240_byte_envelope_for_longest_title(self):
        for row in canned():
            self.assertLessEqual(len(wire(row,1,'win32-worms-armageddon-demo').rstrip(b'\r\n')), 240)
        self.assertEqual(self.verdict(parser(probe='win32-worms-armageddon-demo'))['outcome'], 'pass')

    def test_hardware_policy_is_pending_and_explicit_policy_checks_metrics(self):
        p = parser()
        self.assertIn('lead_hardware_policy', self.verdict(p,hardware=True)['missing'])
        policy = {'play_ms':180000,'min_fps_milli':25000,'max_underruns':0,'max_latency_us':33333,'max_latency_frames':1}
        self.assertEqual(self.verdict(p,hardware=True,policy=policy)['outcome'], 'pass')
        self.assertEqual(self.verdict(p,hardware=True,policy={**policy,'play_ms':600000})['outcome'], 'fail')
        self.assertEqual(self.verdict(p,hardware=True,policy={**policy,'max_latency_us':9999})['outcome'], 'fail')
        self.assertEqual(self.verdict(p,timeout=True)['reason'], 'host_timeout')

    def test_missing_metrics_or_terminal_cannot_pass(self):
        for case in ('fps','frames','mode','audio','input','progress','commit','loader','ledger','install','uninstall','focus'):
            rows = [r for r in canned() if r.get('case') != case]
            with self.subTest(case=case):
                self.assertEqual(self.verdict(parser(rows))['outcome'], 'not_run')
        self.assertEqual(self.verdict(parser(canned()[:-1]))['outcome'], 'not_run')

    def test_duplicate_summary_and_input_ids_are_failures(self):
        for case in ('frames','fps','audio','input'):
            rows = canned()
            at = next(i for i,r in enumerate(rows) if r.get('case') == case)
            rows.insert(at, copy.deepcopy(rows[at]))
            with self.subTest(case=case):
                self.assertEqual(self.verdict(parser(rows))['outcome'], 'fail')

    def test_repeated_presents_silent_audio_and_bad_ledgers_fail(self):
        for case, fields in (
                ('frames', {'game_frames':0}), ('fps', {'avg_milli':60000}),
                ('audio', {'non_silent_frames':0}), ('focus', {'round_trips':9}),
                ('focus', {'corrupt':1}), ('install', {'system_writes':1}),
                ('uninstall', {'key_removed':0}), ('progress', {'counter':1}),
                ('ledger', {'count':1,'when':'post'})):
            rows = canned()
            target = next(r for r in reversed(rows) if r.get('case') == case)
            target.update(fields)
            with self.subTest(case=case,fields=fields):
                self.assertEqual(self.verdict(parser(rows))['outcome'], 'fail')
        rows = canned()
        next(r for r in rows if r.get('case') == 'audio')['qualified'] = 0
        self.assertIn('qualified_audio_backend', self.verdict(parser(rows))['missing'])

    def test_fault_unhandled_stub_and_not_run(self):
        for case, fields in (('crash',{'code':'access_violation'}), ('stub',{'name':'UnsupportedAPI','handled':0})):
            rows = canned()
            at = next(i for i,r in enumerate(rows) if r.get('case') == 'mode')
            rows.insert(at, {'event':'DATA','case':case,'segment':'play','pid':7, **fields})
            self.assertEqual(self.verdict(parser(rows))['outcome'], 'fail')
        rows = [{'event':'NOT_RUN','reason':'fixtures_absent'}]
        self.assertEqual(self.verdict(parser(rows))['outcome'], 'not_run')
        rows = canned()[:2]+[{'event':'ERROR','reason':'missing_probe'}]
        self.assertEqual(self.verdict(parser(rows))['outcome'], 'fail')

    def test_strict_framing_identity_sequence_and_schema(self):
        malformed = [
            b'bad CIUKI_TEST v=1', b'CIUKI_TEST '+b'a'*241,
            wire({'event':'BEGIN','image_sha256':IMAGE},1).replace(b'run=1234abcd',b'run=87654321'),
            wire({'event':'BEGIN','image_sha256':'c'*64},1),
            wire({'event':'BEGIN','image_sha256':IMAGE},1).replace(b'v=1',b'v=1 v=1'),
            wire({'event':'BEGIN','image_sha256':IMAGE},1).replace(b'000001',b'00000x'),
            wire({'event':'BEGIN','image_sha256':IMAGE,'extra':'x'},1),
            wire({'event':'BEGIN','image_sha256':IMAGE},1).replace(b'v=1',b'v=\xff'),
        ]
        for raw in malformed:
            with self.subTest(raw=raw):
                with self.assertRaises(EvidenceError):
                    Parser('1234abcd','win32-diablo-shareware',IMAGE,PAYLOAD,'default-play').feed(raw)
        p = parser(canned()[:2])
        with self.assertRaises(EvidenceError): p.feed(wire(canned()[1],3))
        p = parser(canned()[:1])
        with self.assertRaises(EvidenceError): p.feed(wire(canned()[1],1))
        p = parser()
        with self.assertRaises(EvidenceError): p.feed(wire({'event':'END','status':'PASS'},999))
        p = Parser('1234abcd','win32-diablo-shareware',IMAGE,PAYLOAD,'default-play')
        self.assertIsNone(p.feed(b'ordinary serial banner\n'))

    def test_segment_order_clock_pid_loader_and_numeric_validation(self):
        mutations = [
            lambda rows: rows[2].update(op='end'),
            lambda rows: next(r for r in rows if r.get('case') == 'segment' and r.get('segment') == 'start').update(guest_ms=99),
            lambda rows: next(r for r in rows if r.get('case') == 'mode').update(pid=99),
            lambda rows: next(r for r in rows if r.get('case') == 'mode').update(bpp=7),
            lambda rows: next(r for r in rows if r.get('case') == 'loader').update(stage='entry'),
            lambda rows: next(r for r in rows if r.get('case') == 'frames').update(presents=1),
            lambda rows: next(r for r in rows if r.get('case') == 'audio').update(underruns=-1),
            lambda rows: next(r for r in rows if r.get('case') == 'fps').update(p05_milli=30001),
            lambda rows: next(r for r in rows if r.get('case') == 'input').update(latency_us='18446744073709551616'),
        ]
        for mutation in mutations:
            rows = canned(); mutation(rows)
            with self.subTest(mutation=mutation), self.assertRaises(EvidenceError): parser(rows)


if __name__ == '__main__':
    unittest.main()
