"""Runner USB topology, installed-machine refusal, and unchanged QMP transport."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import unittest
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

    def test_live_usb_evidence_serial_requirements_and_unchanged_qmp_input(self):
        from test_runner import RunnerTests
        fixture=RunnerTests();fixture.setUp()
        try:
            for controller in ('uhci','ohci'):
                declaration=next(c for c in runner.load(ROOT/'tests/suites/f1-usb.json')['cases']
                                 if c['id']=='usb-input-'+controller)
                for i8042 in (False,True):
                    fixture.case.update(usb=declaration['usb'],i8042=i8042,actions=runner.desktop_stimulus())
                    fixture.case['expected']['serial_patterns']=declaration['expected']['serial_patterns']
                    records=[*self.usb_lines(controller),dict(event='BEGIN'),
                             dict(event='ARM',action='post_fault_input')]
                    with patch.object(runner.QMP,'command',autospec=True,side_effect=runner.QMP.command) as requests:
                        result,directory=fixture.run_fake(records=records,finish_after_input=5,
                            after_input=[dict(event='DATA',tick='10000'),dict(event='END',status='PASS')])
                    sent=[call.args[2] for call in requests.call_args_list if call.args[1]=='input-send-event']
                    self.assertEqual(sent,[{'events':b['events']} for b in fixture.case['actions'][0]['batches']])
                    self.assertEqual(result['outcome'],'pass',result['reason'])
                    self.assertEqual(result['qemu']['devices']['usb'],declaration['usb'])
                    self.assertIs(result['qemu']['devices']['i8042'],i8042)
                    self.assertEqual(len(result['serial_matches']),5)
                    self.assertEqual(result['image']['sha256'],runner.sha(fixture.image))
                    self.assertEqual(result['stimulus']['observed'][0]['events'],fixture.case['actions'][0]['batches'][0]['events'])
                    self.assertFalse((directory/'run.qcow2').exists())
                for omitted in range(5):
                    missing=[line for i,line in enumerate(self.usb_lines(controller)) if i!=omitted]
                    fixture.case['actions']=[]
                    result,_=fixture.run_fake(records=[*missing,dict(event='BEGIN'),
                        dict(event='DATA',tick='10000'),dict(event='END',status='PASS')])
                    self.assertEqual(result['outcome'],'fail')
                    self.assertIn('missing expected serial pattern',result['reason'])
        finally:fixture.tearDown()

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
