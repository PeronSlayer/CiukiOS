"""Runner USB topology, installed-machine refusal, and unchanged QMP transport."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import unittest
import ctypes
import tempfile
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/test'))
import run as runner
import resources as res
from evidence import EvidenceError


class USBRunnerTests(unittest.TestCase):
    def setUp(self):
        self.profile=runner.load(ROOT/'tests/profiles/qemu-desktop-2002.json')
        self.case={'probe':'boot','expected':{'terminal':'END'}}

    def args(self,profile=None,case=None):
        return runner.qemu_args('fixture',profile or self.profile,case or self.case,
                                '12345678',ROOT/'build/run.qcow2',ROOT/'build/bios.bin')

    def test_controller_bus_arguments_and_overrides(self):
        for controller,model in (('uhci','piix3-usb-uhci'),('ohci','pci-ohci')):
            usb=[{'controller':controller,'devices':['kbd','mouse','tablet']}]
            for profile,case in ((self.profile,{**self.case,'usb':usb}),
                                 ({**self.profile,'usb':usb},self.case)):
                with self.subTest(controller=controller,profile='usb' in profile):
                    args,request=self.args(profile,case)
                    devices=[args[i+1] for i,arg in enumerate(args) if arg=='-device']
                    self.assertEqual(devices,[model+',id=usb0',*[f'usb-{d},bus=usb0.0' for d in usb[0]['devices']]])
                    self.assertEqual(request,'f0:boot run=12345678')
                    self.assertEqual(args[args.index('-machine')+1],self.profile['machine'])
        profile={**self.profile,'usb':[{'controller':'uhci','devices':['kbd']}]}
        case={**self.case,'usb':[{'controller':'ohci','devices':['mouse']},
                                {'controller':'uhci','devices':['kbd']}]}
        args,_=self.args(profile,case)
        self.assertIn('pci-ohci,id=usb0',args);self.assertIn('usb-mouse,bus=usb0.0',args)
        self.assertIn('piix3-usb-uhci,id=usb1',args);self.assertIn('usb-kbd,bus=usb1.0',args)
        args,_=self.args(profile,{**self.case,'usb':[]})
        self.assertNotIn('-device',args)

    def test_invalid_declarations_refuse_before_capability_query(self):
        malformed=[None,{},'uhci',[None],[{}],[{'controller':'xhci','devices':['kbd']}],
                   [{'controller':'uhci','devices':'kbd'}],
                   [{'controller':'ohci','devices':['joystick']}],
                   [{'controller':'ohci','devices':[{}]}],
                   [{'controller':'uhci','devices':[],'extra':True}]]
        with patch.object(runner.subprocess,'run') as query:
            for value in malformed:
                for profile,case in (({**self.profile,'usb':value},{**self.case,'usb':[]}),
                                     (self.profile,{**self.case,'usb':value})):
                    with self.subTest(usb=value),self.assertRaises(res.Refusal):self.args(profile,case)
            for value in (None,0,1,'false',[],{}):
                for profile,case in (({**self.profile,'i8042':value},{**self.case,'i8042':True}),
                                     (self.profile,{**self.case,'i8042':value})):
                    with self.subTest(i8042=value),self.assertRaisesRegex(res.Refusal,'boolean'):self.args(profile,case)
            query.assert_not_called()

    def test_missing_counter_source_refuses_before_capability_query_and_launch(self):
        from test_runner import RunnerTests, FakeHost
        fixture=RunnerTests();fixture.setUp()
        try:
            fixture.profile['usb']=[{'controller':'uhci','devices':['mouse']}]
            base={**fixture.case,'i8042':False,'probe':'crash-isolation',
                  'selector':'f2:crash-isolation run={run_id} server=desktop',
                  'actions':runner.desktop_stimulus()}
            bad=[{**base,'probe':'boot','selector':'f0:boot run={run_id}'},
                 {**base,'selector':'f2:crash-isolation run={run_id} server=standin'},
                 {**base,'selector':'f2:crash-isolation run={run_id}'},
                 {**base,'selector':'f2:crash-isolation run={run_id} safe=1 server=desktop'},
                 {**base,'device_exceptions':{'vga':'none'}}]
            with patch.object(runner.subprocess,'run') as query, patch.object(FakeHost,'launch') as launch:
                for fixture.case in bad:
                    with self.subTest(case=fixture.case),self.assertRaisesRegex(res.Refusal,'no counter record source'):
                        fixture.run_fake()
                query.assert_not_called();launch.assert_not_called()
            self.assertFalse(any((fixture.root/'build/test-runs').rglob('run.qcow2')))
            # Case overrides and non-pointer stimuli do not need counters.
            with patch.object(runner,'require_i8042_off'):
                for case in ({**bad[0],'usb':[]},
                             {**bad[0],'usb':[{'controller':'ohci','devices':['kbd','tablet']}]},
                             {**bad[0],'actions':[]},
                             {**bad[0],'actions':[{'type':'input','after':{'event':'ARM'},'batches':[
                                 {'events':[{'type':'key','data':{'down':True,'key':{'type':'qcode','data':'a'}}}],
                                  'pause_ms':10}]}]}):
                    self.args(profile=fixture.profile,case=case)
        finally:fixture.tearDown()

    def test_baseline_after_first_key_batches_allows_first_pointer_batch(self):
        from test_runner import RunnerTests
        fixture=RunnerTests();fixture.setUp()
        try:
            fixture.case.update(probe='crash-isolation',selector='f2:crash-isolation run={run_id} server=desktop',
                                actions=runner.desktop_stimulus())
            fixture.profile['usb']=[{'controller':'ohci','devices':['mouse']}]
            fixture.case['expected']['predicates'][0]['where']['tick']='10000'
            result,_=fixture.run_fake(records=[dict(event='BEGIN'),dict(event='ARM',action='post_fault_input')],
                input_baseline_after=2,consume_input=True,finish_after_input=6,
                after_input=[dict(event='DATA',tick='10000'),dict(event='END',status='PASS')])
            self.assertEqual(result['outcome'],'pass',result['reason'])
            self.assertEqual(result['input_pacing']['batches'],6)
            self.assertEqual(len(result['input_pacing']['waits_ms']),4)
        finally:fixture.tearDown()

    def test_i8042_off_requires_installed_machine_boolean_property(self):
        help_result=subprocess.CompletedProcess([],0,'  i8042=<bool> - PS/2 controller\n','')
        with patch.object(runner.subprocess,'run',return_value=help_result) as query:
            for profile,case in ((self.profile,{**self.case,'i8042':False}),
                                 ({**self.profile,'i8042':False},self.case)):
                args,_=self.args(profile,case)
                self.assertEqual(args[args.index('-machine')+1],self.profile['machine']+',i8042=off')
            query.assert_called_with(['fixture','-machine',self.profile['machine']+',help'],
                                     capture_output=True,text=True,timeout=5,check=False)
            query.reset_mock()
            args,_=self.args({**self.profile,'i8042':False},{**self.case,'i8042':True})
            self.assertEqual(args[args.index('-machine')+1],self.profile['machine'])
            query.assert_not_called()
        for output in ('usb=<bool>','x-i8042=<bool>','i8042=<str>'):
            with patch.object(runner.subprocess,'run',return_value=subprocess.CompletedProcess([],0,output,'')), \
                 self.assertRaisesRegex(res.Refusal,'does not advertise i8042=off'):
                self.args(case={**self.case,'i8042':False})
        with patch.object(runner.subprocess,'run',return_value=subprocess.CompletedProcess([],1,help_result.stdout,'')), \
             self.assertRaises(res.Refusal):self.args(case={**self.case,'i8042':False})
        for error in (FileNotFoundError('missing'),subprocess.TimeoutExpired('fixture',5)):
            with patch.object(runner.subprocess,'run',side_effect=error), \
                 self.assertRaisesRegex(res.Refusal,'cannot check QEMU'):
                self.args(case={**self.case,'i8042':False})

    def test_suite_reuses_desktop_shell_contract_and_prerequisites(self):
        suite=runner.load(ROOT/'tests/suites/f1-usb.json')
        self.assertEqual(suite['prerequisites'],['f0-smoke'])
        self.assertEqual([c['id'] for c in suite['cases']],['usb-input-uhci','usb-input-ohci'])
        baseline=next(c for c in runner.load(ROOT/'tests/suites/f2-desktop.json')['cases']
                      if c['id']=='desktop-shell-qemu-t23')
        with patch.object(runner,'require_i8042_off'):
            for controller,case in zip(('uhci','ohci'),suite['cases']):
                self.assertEqual(case['profile'],'qemu-desktop-2002')
                self.assertIs(case['i8042'],False)
                self.assertEqual(case['usb'],[{'controller':controller,'devices':['kbd','mouse']}])
                self.assertEqual(case['actions'],baseline['actions'])
                self.assertEqual(case['timeout'],baseline['timeout'])
                self.assertTrue(case['desktop_screen'])
                expected=copy.deepcopy(case['expected']);patterns=expected.pop('serial_patterns')
                self.assertEqual(expected,baseline['expected']);self.assertEqual(len(patterns),5)
                runner.Actions(case['actions'])
                _,request=self.args(case=case)
                self.assertEqual(request,'f2:crash-isolation run=12345678 server=desktop')
        smoke=runner.load(ROOT/'tests/suites/f0-smoke.json')['cases']
        expanded=runner.load_suite('f1-usb')['cases']
        self.assertEqual([c['id'] for c in expanded],[c['id'] for c in smoke+suite['cases']])

    def test_normal_boot_accepts_input_configuration_without_test_request(self):
        from test_normal_boot import normal_case
        case=normal_case(usb=[{'controller':'ohci','devices':['kbd','mouse']}],i8042=False)
        with patch.object(runner,'require_i8042_off'):
            args,request=self.args(case=case)
        self.assertIsNone(request);self.assertNotIn('-fw_cfg',args)
        self.assertIn(self.profile['machine']+',i8042=off',args)
        self.assertIn('usb-kbd,bus=usb0.0',args);self.assertIn('usb-mouse,bus=usb0.0',args)

    def usb_lines(self,controller):
        return [f'[usb] controller={controller} ports=2 pci=00:04.0 mode=poll reason=ready_polling',
                '[usb] device addr=1 vid:pid=0627:0001 class=00 speed=full',
                '[usb] device addr=2 vid:pid=0627:0002 class=00 speed=full',
                '[usb] hid keyboard attached addr=1 interface=0 wheel=0',
                '[usb] hid mouse attached addr=2 interface=0 wheel=1']

    def test_live_usb_evidence_serial_requirements_and_paced_qmp_input(self):
        from test_runner import RunnerTests
        fixture=RunnerTests();fixture.setUp()
        fixture.case.update(probe='crash-isolation',selector='f2:crash-isolation run={run_id} server=desktop')
        try:
            fixture.case['expected']['predicates'][0]['where']['tick']='10000'
            for controller in ('uhci','ohci'):
                declaration=next(c for c in runner.load(ROOT/'tests/suites/f1-usb.json')['cases']
                                 if c['id']=='usb-input-'+controller)
                for i8042 in (False,True):
                    fixture.case.update(usb=declaration['usb'],i8042=i8042,actions=runner.desktop_stimulus())
                    fixture.case['expected']['serial_patterns']=declaration['expected']['serial_patterns']
                    records=[*self.usb_lines(controller),dict(event='BEGIN'),
                             dict(event='DATA',case='gate',motion='0',buttons='0'),
                             dict(event='ARM',action='post_fault_input')]
                    with patch.object(runner.QMP,'command',autospec=True,side_effect=runner.QMP.command) as requests:
                        result,directory=fixture.run_fake(records=records,finish_after_input=6,consume_input=True,
                            after_input=[dict(event='DATA',tick='10000'),dict(event='END',status='PASS')])
                    sent=[call.args[2] for call in requests.call_args_list if call.args[1]=='input-send-event']
                    self.assertEqual(sent,[{'events':b['events']} for b in runner.Actions.usb_batches(fixture.case['actions'][0]['batches'])])
                    self.assertEqual(result['outcome'],'pass',result['reason'])
                    self.assertEqual(result['qemu']['devices']['usb'],declaration['usb'])
                    self.assertIs(result['qemu']['devices']['i8042'],i8042)
                    self.assertEqual(len(result['serial_matches']),5)
                    self.assertEqual(result['image']['sha256'],runner.sha(fixture.image))
                    self.assertEqual(result['stimulus']['observed'][0]['events'],fixture.case['actions'][0]['batches'][0]['events'])
                    self.assertFalse((directory/'run.qcow2').exists())
                    self.assertEqual(result['input_pacing']['batches'],6)
                    self.assertEqual(len(result['input_pacing']['waits_ms']),4)
                    self.assertTrue(all(ms>=150 for ms in result['input_pacing']['waits_ms']))
                for omitted in range(5):
                    missing=[line for i,line in enumerate(self.usb_lines(controller)) if i!=omitted]
                    fixture.case['actions']=[]
                    result,_=fixture.run_fake(records=[*missing,dict(event='BEGIN'),
                        dict(event='DATA',tick='10000'),dict(event='END',status='PASS')])
                    self.assertEqual(result['outcome'],'fail')
                    self.assertIn('missing expected serial pattern',result['reason'])
        finally:fixture.tearDown()

    def test_shell_end_waits_for_final_consumed_release(self):
        """Run the probe predicate and reproduce the old END in the runner model."""
        from test_runner import RunnerTests
        with tempfile.TemporaryDirectory() as scratch:
            source=Path(scratch)/'shell.c';library=Path(scratch)/'shell.so'
            source.write_text('#include <ciuki/shell_probe.h>\n'
                              'int pending(unsigned s,unsigned k,unsigned b) {return shell_probe_pending(s,k,b);}\n')
            subprocess.run(['clang','-shared','-fPIC','-Wall','-Wextra','-Werror',
                            '-I',str(ROOT/'src/kernel/include'),str(source),'-o',str(library)],check=True)
            pending=ctypes.CDLL(str(library)).pending
            pending.argtypes=[ctypes.c_uint]*3;pending.restype=ctypes.c_int
            for controller in ('uhci','ohci'):
                declaration=next(c for c in runner.load(ROOT/'tests/suites/f1-usb.json')['cases']
                                 if c['id']=='usb-input-'+controller)
                schedule=runner.Actions.usb_batches(declaration['actions'][0]['batches'])
                buttons=keys=0;early=None
                for index,batch in enumerate(schedule,1):
                    buttons+=sum(e['type']=='btn' for e in batch['events'])
                    keys+=sum(e['type']=='key' for e in batch['events'])
                    if keys==4 and early is None:
                        early=index
                        self.assertEqual(buttons,10)
                    if index<len(schedule):self.assertTrue(pending(6,keys,buttons))
                self.assertEqual(buttons,12);self.assertFalse(pending(6,keys,buttons))
                for terminal_at,expected in ((early,'fail'),(len(schedule),'pass')):
                    fixture=RunnerTests();fixture.setUp()
                    try:
                        fixture.case.update(probe='crash-isolation',
                            selector='f2:crash-isolation run={run_id} server=desktop',
                            usb=declaration['usb'],actions=declaration['actions'],timeout=declaration['timeout'])
                        fixture.case['expected']['predicates'][0]['where']['tick']='10000'
                        records=[dict(event='BEGIN'),dict(event='DATA',case='gate',motion='0',buttons='0'),
                                 dict(event='ARM',action='post_fault_input')]
                        result,_=fixture.run_fake(records=records,consume_input=True,input_ack_delay=.01,
                            finish_after_input=terminal_at,
                            after_input=[dict(event='DATA',tick='10000'),dict(event='END',status='PASS')])
                        self.assertEqual(result['outcome'],expected,result['reason'])
                        if expected=='fail':self.assertIn('terminal evidence before declared stimuli completed',result['reason'])
                        else:self.assertEqual(result['input_pacing']['batches'],len(schedule))
                    finally:fixture.tearDown()

    def test_usb_profile_splits_signed_axes_and_preserves_totals_across_repeats(self):
        from test_runner import RunnerTests
        fixture=RunnerTests();fixture.setUp()
        fixture.case.update(probe='crash-isolation',selector='f2:crash-isolation run={run_id} server=desktop')
        try:
            fixture.case['expected']['predicates'][0]['where']['tick']='10000'
            fixture.profile['usb']=[{'controller':'ohci','devices':['mouse']}]
            fixture.case['timeout']=5
            fixture.case['actions']=[{'type':'input','after':{'event':'ARM'},'repeat':2,'batches':[
                {'events':[{'type':'rel','data':{'axis':'x','value':-300}},
                           {'type':'rel','data':{'axis':'y','value':128}}],'pause_ms':10},
                {'events':[{'type':'btn','data':{'button':'left','down':True}}],'pause_ms':10},
                {'events':[{'type':'btn','data':{'button':'left','down':False}}],'pause_ms':10}]}]
            result,directory=fixture.run_fake(records=[dict(event='BEGIN'),
                dict(event='DATA',case='gate',motion='7',buttons='4'),dict(event='ARM')],
                consume_input=True,input_motion=7,input_buttons=4,finish_after_input=10,
                after_input=[dict(event='DATA',tick='10000'),dict(event='END',status='PASS')])
            self.assertEqual(result['outcome'],'pass',result['reason'])
            events=[e for b in result['stimulus']['observed'] for e in b['events'] if e['type']=='rel']
            self.assertTrue(all(abs(e['data']['value'])<=127 for e in events))
            for axis,total in [('x',-600),('y',256)]:
                self.assertEqual(sum(e['data']['value'] for e in events if e['data']['axis']==axis),total)
            self.assertEqual(result['input_pacing']['batches'],10)
            self.assertEqual(len(result['input_pacing']['waits_ms']),10)
            self.assertEqual(json.loads((directory/'result.json').read_text())['input_pacing'],result['input_pacing'])
        finally:fixture.tearDown()

    def test_ps2_and_non_mouse_usb_keep_original_requests_and_timing(self):
        from test_runner import RunnerTests
        fixture=RunnerTests();fixture.setUp()
        try:
            fixture.case['actions']=runner.desktop_stimulus()
            for usb in ([],[{'controller':'uhci','devices':['kbd','tablet']}],
                        [{'controller':'uhci','devices':['mouse']}]):
                fixture.profile['usb']=usb
                if usb and usb[0]['devices']==['mouse']:fixture.case['usb']=[]
                result,_=fixture.run_fake(records=[dict(event='BEGIN'),dict(event='ARM',action='post_fault_input')],
                    finish_after_input=5,after_input=[dict(event='DATA',tick='10000'),dict(event='END',status='PASS')])
                self.assertEqual(result['outcome'],'pass',result['reason'])
                self.assertNotIn('input_pacing',result)
                self.assertEqual([b['events'] for b in result['stimulus']['observed']],
                                 [b['events'] for b in fixture.case['actions'][0]['batches']])
                sent=result['stimulus']['observed']
                self.assertTrue(all(b['host_monotonic']-a['host_monotonic']>=.05 for a,b in zip(sent,sent[1:])))
        finally:fixture.tearDown()

    def test_usb_consumption_timeout_names_stalled_batch_and_missing_baseline(self):
        from test_runner import RunnerTests
        fixture=RunnerTests();fixture.setUp()
        fixture.case.update(probe='crash-isolation',selector='f2:crash-isolation run={run_id} server=desktop')
        try:
            fixture.profile['usb']=[{'controller':'uhci','devices':['mouse']}]
            fixture.case['actions']=runner.desktop_stimulus()
            for baseline in (True,False):
                records=[dict(event='BEGIN'),dict(event='ARM',action='post_fault_input')]
                if baseline:records.append(dict(event='DATA',case='gate',motion='0',buttons='0'))
                with patch.object(runner.Actions,'USB_CONSUMPTION_TIMEOUT',.2):
                    result,_=fixture.run_fake(records=records,consume_input=True,stall_input=3)
                self.assertEqual(result['outcome'],'fail')
                self.assertIn('USB input consumption stalled action=0 batch=2 repeat=0 step=0',result['reason'])
                self.assertEqual(result['input_pacing']['batches'],3 if baseline else 2)
                self.assertEqual(result['input_pacing']['waits_ms'],[])
                self.assertTrue(result['cleanup']['clean'])
        finally:fixture.tearDown()

    def test_raw_input_counters_pace_the_final_batch(self):
        from test_runner import RunnerTests
        fixture=RunnerTests();fixture.setUp()
        fixture.case.update(probe='crash-isolation',selector='f2:crash-isolation run={run_id} server=desktop')
        try:
            fixture.profile['usb']=[{'controller':'uhci','devices':['mouse']}]
            fixture.case['actions']=[{'type':'input','after':{'event':'ARM'},'batches':[
                {'events':[{'type':'rel','data':{'axis':'x','value':127}},
                           {'type':'rel','data':{'axis':'y','value':-127}}],'pause_ms':10}]}]
            result,_=fixture.run_fake(records=[dict(event='BEGIN'),'[gate] motion=0 buttons=0',dict(event='ARM')],
                consume_input=True,raw_input_counters=True,finish_after_input=1,
                after_input=[dict(event='DATA',tick='10000'),dict(event='END',status='PASS')])
            self.assertEqual(result['outcome'],'pass',result['reason'])
            self.assertEqual(result['input_pacing']['batches'],1)
            self.assertEqual(len(result['input_pacing']['waits_ms']),1)
        finally:fixture.tearDown()

    def test_barrier_needs_both_axes_and_fresh_counters_ignores_application_bytes(self):
        from types import SimpleNamespace
        parser=SimpleNamespace(records=[dict(event='ARM',seq='000001')])
        actions=runner.Actions([{'type':'input','after':{'event':'ARM'},'batches':[
            {'events':[{'type':'rel','data':{'axis':'x','value':127}},
                       {'type':'rel','data':{'axis':'y','value':-127}}],'pause_ms':10}]}],
            {'usb':[{'controller':'uhci','devices':['mouse']}]})
        actions.observe(b'[gate] motion=9 buttons=2\n',None)
        actions.step(parser,SimpleNamespace(input_events=lambda events:None),1)
        self.assertFalse(actions.complete)
        self.assertFalse(actions.wait_consumption(parser,1.1))
        actions.observe(b'',dict(event='DATA',group='app',case='gate',motion='11',buttons='2'))
        self.assertFalse(actions.wait_consumption(parser,1.2))
        for line in (b'prefix [input] motion=11 buttons=2\n',b'[gate] motion=11 buttons=2 motion=11\n'):
            actions.observe(line,None)
            self.assertFalse(actions.wait_consumption(parser,1.3))
        actions.observe(b'[input] motion=10 buttons=2\n',None)
        self.assertFalse(actions.wait_consumption(parser,1.4))
        actions.observe(b'[input] motion=11 buttons=2\n',None)
        self.assertTrue(actions.wait_consumption(parser,1.5))
        self.assertTrue(actions.complete)
        self.assertEqual(actions.input_pacing['waits_ms'],[500])

    def test_final_batch_stall_refuses_after_action_index_advanced(self):
        from types import SimpleNamespace
        parser=SimpleNamespace(records=[dict(event='ARM',seq='000001')])
        actions=runner.Actions([{'type':'input','after':{'event':'ARM'},'batches':[
            {'events':[{'type':'btn','data':{'button':'left','down':True}}],'pause_ms':10}]}],
            {'usb':[{'controller':'uhci','devices':['mouse']}]})
        actions.observe(b'[gate] motion=0 buttons=0\n',None)
        actions.step(parser,SimpleNamespace(input_events=lambda events:None),1)
        self.assertEqual(actions.index,1)
        self.assertFalse(actions.complete)
        with self.assertRaisesRegex(res.Refusal,'stalled action=0 batch=0 repeat=0 step=0'):
            actions.step(parser,None,11)

    def test_serial_schema_and_anchoring(self):
        for patterns in (None,'x',[None],['unanchored'],['^['],['^\u00e9']):
            with self.subTest(patterns=patterns),self.assertRaises(res.Refusal):
                self.args(case={**self.case,'expected':{'serial_patterns':patterns}})
        from test_runner import RunnerTests
        fixture=RunnerTests();fixture.setUp()
        try:
            path=fixture.root/'serial.log'
            expected={'serial_patterns':['^\\[usb\\] hid keyboard attached.*$']}
            for data in (b'[usb] hid keyboard attached addr=1\n',b'[usb] hid keyboard attached addr=1\r\n'):
                path.write_bytes(data);self.assertEqual(len(runner.check_serial_patterns(path,expected)),1)
            path.write_bytes(b'prefix [usb] hid keyboard attached addr=1\n')
            with self.assertRaises(EvidenceError):runner.check_serial_patterns(path,expected)
            malformed=fixture.root/'suite.json'
            malformed.write_text(json.dumps({'schema_version':1,'cases':[{'usb':'uhci'}]}))
            with self.assertRaises(res.Refusal):runner.load(malformed)
        finally:fixture.tearDown()


if __name__=='__main__':unittest.main()
