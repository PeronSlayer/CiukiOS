"""Scripted QMP and independent screen checks, using fake time/transport only."""
import copy
import hashlib
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'scripts/test'))
from win32_session import InputPlayer, selector, validate_script, screen_check, probe_name


def key(down):
    return {'type':'key','data':{'down':down,'key':{'type':'qcode','data':'a'}}}


def script():
    return {'schema_version':1,'workload':'authored-fixture','scene':'default-play',
            'clock':'host-monotonic-ms','timeout_ms':600000,'segments':[
            {'name':'play','steps':[{'input_id':1,'at_ms':0,'events':[key(True)]},
                                  {'input_id':2,'at_ms':50,'events':[key(False)]},
                                  {'input_id':3,'at_ms':100,'events':[{'type':'rel','data':{'axis':'x','value':10}},
                                                         {'type':'rel','data':{'axis':'y','value':-20}}]}]}]}


class Qmp:
    def __init__(self):
        self.events = []
    def input_events(self, events):
        self.events.append(events)


class SessionTests(unittest.TestCase):
    def record(self, event, **fields):
        return {'event':event,'run':'1234abcd','probe':'win32-authored-fixture', **fields}

    def player(self):
        p = InputPlayer(script(), 'f5:win32-authored-fixture run=1234abcd', started_at_ms=1000, validated_fw_cfg=True)
        p.observe(self.record('BEGIN'), 1000)
        return p

    def test_manifest_alias_keeps_full_workload_identity(self):
        for workload, alias in [('starcraft-shareware', 'win32-sc'),
                                ('worms-armageddon-demo', 'win32-wa')]:
            scenario = script()
            scenario['workload'] = workload
            self.assertEqual(probe_name(workload), alias)
            player = InputPlayer(scenario, 'f5:'+alias+' run=1234abcd',
                                 started_at_ms=0, validated_fw_cfg=True)
            self.assertEqual(player.script['workload'], workload)
            with self.assertRaises(ValueError):
                InputPlayer(scenario, 'f5:win32-other run=1234abcd',
                            started_at_ms=0, validated_fw_cfg=True)

    def test_selector_requires_validated_fw_cfg_and_exact_request(self):
        request = 'f5:win32-authored-fixture run=1234abcd'
        self.assertEqual(selector(request, validated_fw_cfg=True)['phase'], 5)
        for kwargs in ({}, {'source':'menu','validated_fw_cfg':True}):
            with self.assertRaises(ValueError): selector(request, **kwargs)
        for bad in (request+' junk', request.replace('f5:','f4:'), request.replace('1234abcd','ABCDEF12'), 'f5:win32- run=1234abcd'):
            with self.assertRaises(ValueError): selector(bad, validated_fw_cfg=True)

    def test_arm_sync_exact_timing_and_receipts_are_host_time(self):
        p, q = self.player(), Qmp()
        self.assertIsNone(p.tick(1050, q))
        p.observe(self.record('ARM', segment='play', action='script'), 1100)
        first = p.tick(1100, q)
        self.assertEqual(first['scheduled_host_ms'], 1100)
        self.assertNotIn('latency_ms', first)
        self.assertIsNone(p.tick(1149, q))
        p.tick(1150, q)
        p.tick(1200, q)
        self.assertTrue(p.complete)
        self.assertEqual(q.events, [s['events'] for s in script()['segments'][0]['steps']])

    def test_delayed_poll_does_not_collapse_key_release(self):
        p, q = self.player(), Qmp()
        p.observe(self.record('ARM', segment='play', action='script'), 1100)
        p.tick(1300, q)
        self.assertIsNone(p.tick(1300, q))
        self.assertIsNone(p.tick(1349, q))
        p.tick(1350, q)
        self.assertEqual(p.observed[1]['sent_host_ms']-p.observed[0]['sent_host_ms'], 50)
        self.assertEqual(len(q.events), 2)

    def test_timeout_bad_arm_identity_and_backward_clock(self):
        for record in (self.record('ARM', segment='start', action='script'),
                       self.record('ARM', segment='play', action='wrong'),
                       self.record('ARM', segment='play', action='script', run='87654321')):
            p = self.player()
            with self.assertRaises(ValueError): p.observe(record, 1100)
        p = self.player()
        with self.assertRaises(ValueError): p.tick(999, Qmp())
        with self.assertRaises(TimeoutError): self.player().tick(601000, Qmp())
        pending = InputPlayer(script(), 'f5:win32-authored-fixture run=1234abcd', started_at_ms=0, validated_fw_cfg=True)
        with self.assertRaises(TimeoutError): pending.tick(600000, Qmp())
        p = self.player()
        p.observe(self.record('ARM', segment='play', action='script'), 1100)
        with self.assertRaises(ValueError): p.observe(self.record('ARM', segment='play', action='script'), 1101)

    def test_failed_qmp_batch_does_not_advance(self):
        p = self.player()
        p.observe(self.record('ARM', segment='play', action='script'), 1100)
        class Broken:
            def input_events(self, events): raise RuntimeError('QMP error')
        with self.assertRaises(RuntimeError): p.tick(1100, Broken())
        self.assertEqual(p.step, 0)
        self.assertFalse(p.observed)

    def test_script_rejects_unbalanced_or_unsafe_events(self):
        mutations = [
            lambda s: s['segments'][0]['steps'].pop(1),
            lambda s: s['segments'][0]['steps'][1].update(at_ms=0),
            lambda s: s['segments'][0]['steps'][2]['events'][0]['data'].update(value=32768),
            lambda s: s['segments'][0]['steps'][0]['events'][0]['data']['key'].update(data='a;quit'),
            lambda s: s.update(timeout_ms=600001),
            lambda s: s.update(clock='guest-icount'),
            lambda s: s['segments'][0]['steps'][1].update(input_id=1),
        ]
        for mutation in mutations:
            s = script(); mutation(s)
            with self.assertRaises(ValueError): validate_script(s)

    def test_exact_and_tolerant_region_oracles(self):
        frame = (4, 3, bytes(range(36)))
        region = [1,1,2,1]
        golden = bytes(range(15,21))
        pin = hashlib.sha256(golden).hexdigest()
        check = {'mode':'sha256','region':region,'sha256':pin}
        self.assertTrue(screen_check(frame, check)['match'])
        changed = bytearray(frame[2]); changed[15] += 2
        newer = (4,3,bytes(changed))
        self.assertFalse(screen_check(newer, check)['match'])
        tolerant = {**check,'mode':'tolerance','channel_delta':2,'max_bad_pixels':0}
        self.assertTrue(screen_check(newer, tolerant, frame)['match'])
        changed[15] += 1
        self.assertFalse(screen_check((4,3,bytes(changed)), tolerant, frame)['match'])
        tolerant['max_bad_pixels'] = 1
        self.assertTrue(screen_check((4,3,bytes(changed)), tolerant, frame)['match'])
        with self.assertRaises(ValueError): screen_check(frame, {**tolerant,'sha256':'0'*64}, frame)
        with self.assertRaises(ValueError): screen_check(frame, {**check,'region':[3,2,2,2]})
        with self.assertRaises(ValueError): screen_check(frame, {**tolerant,'max_bad_pixels':2}, frame)
        # Cursor/outside-region changes must not invalidate pinned menu cells.
        changed = bytearray(frame[2]); changed[0] ^= 255
        self.assertTrue(screen_check((4,3,bytes(changed)), check)['match'])


if __name__ == '__main__':
    unittest.main()
