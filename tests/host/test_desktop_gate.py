"""Desktop gate's independent pixels/stimuli and production report seam.

f2-16 research/validation: NULL mmap placement is implementation-selected
(https://pubs.opengroup.org/onlinepubs/9799919799/functions/mmap.html); Ciuki's
first-fit arena remains the authority, including preceding TLS/surface maps.
Positive signed-decimal PIDs also satisfy the unsigned report parser
(https://pubs.opengroup.org/onlinepubs/9799919799/functions/fprintf.html).
Capture production text and protocol packets without rewriting their fields.
"""
import ctypes
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import copy
import resource
from unittest.mock import patch
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/test'))
import run as runner
from evidence import EvidenceError


class DesktopGateTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT/'build/host';scratch.mkdir(parents=True,exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.folder = Path(self.temp.name)

    def tearDown(self):self.temp.cleanup()

    def test_selector_server_and_policy(self):
        harness = self.folder/'selector.c'
        harness.write_text('''
#include <ciuki/kernel.h>
void rec_emit(const char *p,const char *e,const char *f,...) {(void)p;(void)e;(void)f;}
void klog(const char *f,...) {(void)f;}
''')
        library = self.folder/'selector.so'
        subprocess.run(['clang','-shared','-fPIC','-std=c17','-Wall','-Wextra','-Werror',
                        '-I',str(ROOT/'src/kernel/include'),str(ROOT/'src/kernel/probes/selector.c'),
                        str(harness),'-o',str(library)],check=True,
                       env=dict(os.environ,TMPDIR=str(self.folder)))
        native = ctypes.CDLL(str(library))
        native.probes_crash_server.argtypes = [ctypes.c_char_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_bool,ctypes.c_bool]
        base = 'f2:crash-isolation run=12345678'
        def choose(s,flags=32,lfb=True,payload=True):
            b=s.encode();return native.probes_crash_server(b,len(b),flags,lfb,payload)
        self.assertEqual(choose(base),1)
        self.assertEqual(choose(base,payload=False),0)
        self.assertEqual(choose(base+' server=standin'),0)
        self.assertEqual(choose(base+' server=desktop',flags=0),-22)
        self.assertEqual(choose(base+' server=desktop',payload=False),1)
        self.assertEqual(choose(base+' server=desktop',lfb=False),0)
        self.assertEqual(choose(base+' safe=1 server=desktop',flags=33),0)
        self.assertEqual(choose(base+' platform=e500 server=desktop',flags=160),1)
        for suffix in (' server=bogus',' server=desktop server=standin',' server=desktop safe=1',
                       ' safe=1 server=standin platform=e500',' server=desktop\n'):
            self.assertEqual(choose(base+suffix,flags=161),-22)
        for phase,probe in ((0,'boot'),(1,'input'),(2,'libc-smoke')):
            self.assertEqual(choose(f'f{phase}:{probe} run=12345678 server=desktop'),-22)
        # All three optional keys exceed the existing 64-byte loader buffer.
        self.assertEqual(choose(base+' platform=e500 safe=1 server=standin',flags=161),-22)
        for kind in ('desktop','standin'):
            self.assertEqual(runner.selector(base+' server='+kind,'fw_cfg',True)['server'],kind)
            with self.assertRaises(ValueError):runner.selector(base+' server='+kind)
            with self.assertRaises(ValueError):runner.selector(base+' server='+kind,'fw_cfg')
            profile=runner.load(ROOT/'tests/profiles/qemu-e500.json')
            case=dict(probe='crash-isolation',selector=base+' server='+kind)
            _,request=runner.qemu_args('fake',profile,case,'12345678',self.folder/'overlay',self.folder/'bios')
            self.assertEqual(request,base+' platform=e500 server='+kind)
        for text in (base+' server=other',base+' server=desktop safe=1',
                     base+' server=standin server=desktop','f1:input run=12345678 server=desktop',
                     base+' platform=e500 safe=1 server=standin'):
            with self.assertRaises(ValueError):runner.selector(text,'fw_cfg',True)

    def test_loader_assembly_suffixes_and_t1_bound(self):
        source=(ROOT/'src/boot/ciukldr/menu.inc').read_text()
        for token in ("desktop_suffix db ' server=desktop'","standin_suffix db ' server=standin'",
                      'cmp bx, f2_crash_name','cmp byte [selector_fw], 1','cmp cx, 64'):
            self.assertIn(token,source)
        output=self.folder/'ciukldr.bin'
        subprocess.run(['nasm','-f','bin',str(ROOT/'src/boot/ciukldr.asm'),'-o',str(output)],
                       check=True,cwd=ROOT,env=dict(os.environ,TMPDIR=str(self.folder)))
        spec=importlib.util.spec_from_file_location('gate_t1_image',ROOT/'scripts/build_image.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        prepared=module.prepare_loader(output.read_bytes())
        self.assertEqual(len(prepared)%512,0);self.assertLessEqual(len(prepared),1023*512)

    def test_stimulus_order_pacing_and_arm(self):
        action = runner.desktop_stimulus()
        self.assertEqual(action[0]['after'],{'event':'ARM','action':'post_fault_input'})
        batches = action[0]['batches']
        self.assertEqual([e['type'] for b in batches for e in b['events']],['key','key','rel','rel','btn','btn'])
        self.assertEqual([batches[i]['events'][0]['data']['down'] for i in (0,1,3,4)],[True,False,True,False])
        self.assertTrue(all(b['pause_ms']==50 for b in batches))
        actions = runner.Actions(action)
        class Parser:records=[]
        class QMP:
            events=[]
            def input_events(self,events):self.events.append(events)
        parser = Parser();qmp = QMP()
        actions.step(parser,qmp,0);self.assertFalse(qmp.events)
        parser.records = [dict(event='ARM',action='post_fault_input',seq='000010')]
        for t in (0,.06,.12,.18,.24):actions.step(parser,qmp,t)
        self.assertTrue(actions.complete);self.assertEqual(qmp.events,[b['events'] for b in batches])
        self.assertTrue(all(o['sync_seq']=='000010' for o in actions.observed))

    def screen(self,width,height):
        spec = importlib.util.spec_from_file_location('gate_portrait',ROOT/'apps/desktop/convert_portrait.py')
        c = importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
        portrait = c.convert(ROOT/'assets/brand/ciuki-logo.png')
        pixels = bytearray(bytes((0x37,0x55,0x64))*width*height)
        x0=(width-256)//2;y0=max(32,32+(height-32-256-36)//2)
        for y in range(256):
            for x in range(256):
                p=(y*256+x)*4;off=((y0+y)*width+x0+x)*3
                pixels[off:off+3]=portrait[p:p+3][::-1]
        return pixels

    def write_screen(self,name,width,height,pixels):
        path=self.folder/name
        path.write_bytes(f'P6\n# QMP synthetic fixture\n{width} {height}\n255\n'.encode()+pixels)
        return path

    def test_external_portrait_cursor_change_and_failures(self):
        for width,height in ((640,480),(800,600),(1024,768)):
            original=self.screen(width,height);before=bytearray(original);after=bytearray(original)
            # Initial cursor: no whole-frame/clock-only digest can pass this check.
            for y in range(height//2,height//2+16):
                off=(y*width+width//2)*3;before[off:off+24]=b'\xff'*24
            b=self.write_screen('before.ppm',width,height,before)
            a=self.write_screen('after.ppm',width,height,after)
            observed=runner.observe_desktop_screen(b,a)
            self.assertEqual(observed['ppm_sha256'],hashlib.sha256(a.read_bytes()).hexdigest())
            self.assertEqual(observed['portrait_pixels_checked'],6144)
            self.assertGreaterEqual(observed['cursor_changed_pixels'],16)
            with self.assertRaisesRegex(EvidenceError,'cursor'):runner.observe_desktop_screen(b,b)
            clock_only=bytearray(before);clock_only[:30]=b'\xff'*30
            a=self.write_screen('clock.ppm',width,height,clock_only)
            with self.assertRaisesRegex(EvidenceError,'cursor'):runner.observe_desktop_screen(b,a)
            x,y,_,_=observed['portrait_region'];after[(y*width+x)*3]^=1
            a=self.write_screen('corrupt.ppm',width,height,after)
            with self.assertRaisesRegex(EvidenceError,'portrait'):runner.observe_desktop_screen(b,a)
        for data in (b'P3\n640 480\n255\n',b'P6\n640 480\n256\n',b'P6\n640 480\n255\nshort',b'P6\n# missing newline'):
            path=self.folder/'malformed.ppm';path.write_bytes(data)
            with self.assertRaises(EvidenceError):runner.read_ppm(path)


    def test_native_demo_arguments_summaries_pacing_and_faults(self):
        include=self.folder/'include/ciuki';include.mkdir(parents=True)
        for name in ('channel.h','surface.h','spawn.h'):
            (include/name).write_bytes((ROOT/'sdk/sysroot-overlay/include/ciuki'/name).read_bytes())
        for name in ('raw.h','runtime.h'):(include/name).write_text('/* host transport declaration supplied by harness */\n')
        binary=self.folder/'demo-test'
        env=dict(os.environ,TMPDIR=str(self.folder))
        command=['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
            '-I',str(self.folder/'include'),
            '-I',str(ROOT/'apps/desktop'),'-I',str(ROOT/'src/kernel/include'),
            str(ROOT/'tests/host/desktop/demo_gate_test.c'),str(ROOT/'apps/desktop/protocol.c'),
            str(ROOT/'apps/desktop/compositor.c'),'-o',str(binary)]
        subprocess.run(command,check=True,env=env)
        def execute(*args):
            return subprocess.run([str(binary),*args],capture_output=True,text=True,env=env,
                preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        result=execute();self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('100 acknowledged turns',result.stdout)
        result=execute('--summary');self.assertEqual(result.returncode,0)
        self.assertEqual(result.stdout.strip(),'case=native-demo stage=2 turns=100 unauthorized=0 generation=7')
        for kind in ('bad-pointer','closed-peer','forged-fd','grant-fd','handler-fault'):
            result=execute('--fault-run',kind)
            self.assertEqual(result.returncode,-13 if kind=='closed-peer' else -11,(kind,result.stdout,result.stderr))
            self.assertIn('stage=1 turns=0 unauthorized=0',result.stdout)
            self.assertIn('stage=3 turns=0 unauthorized=0',result.stdout)
        for args in (('--channel-fd=2',),('--channel-fd=64',),('--channel-fd=',),('--channel-fd=4x',),
                     ('--fault=bogus',),('--test=bogus',),('--unknown',)):
            result=execute('--arguments',*args);self.assertEqual(result.returncode,2,(args,result.stderr))

    def interaction_records(self):
        before=dict(presents='10',input_events='0',pixel_digest='12345678',server='desktop')
        after=dict(presents='11',input_events='7',pixel_digest='87654321',changed='1',keys='2',motion='1',buttons='2',server='desktop')
        return [dict(event='ARM',action='post_fault_input',**before),
                dict(event='DATA',case='interaction',stage='before',**before),
                dict(event='DATA',case='interaction',stage='after',**after)]

    def test_native_desktop_gate_loop(self):
        include=self.folder/'desktop-include/ciuki';include.mkdir(parents=True)
        for name in ('channel.h','surface.h','spawn.h'):
            (include/name).write_bytes((ROOT/'sdk/sysroot-overlay/include/ciuki'/name).read_bytes())
        for name in ('raw.h','runtime.h'):(include/name).write_text('/* declarations supplied by harness */\n')
        binary=self.folder/'desktop-gate-test'
        env=dict(os.environ,TMPDIR=str(self.folder))
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
            '-I',str(self.folder/'desktop-include'),'-I',str(ROOT/'apps/desktop'),
            '-I',str(ROOT/'src/kernel/include'),str(ROOT/'tests/host/desktop/desktop_gate_test.c'),
            *(str(ROOT/'apps/desktop'/n) for n in ('client.c','protocol.c','input.c','compositor.c')),
            '-o',str(binary)],check=True,env=env)
        result=subprocess.run([str(binary)],capture_output=True,text=True,env=env,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('five fault kinds',result.stdout)

    def test_native_handshake_reports_through_kernel_hook(self):
        include=self.folder/'handshake-include/ciuki';include.mkdir(parents=True)
        for name in ('channel.h','surface.h','spawn.h'):
            (include/name).write_bytes((ROOT/'sdk/sysroot-overlay/include/ciuki'/name).read_bytes())
        for name in ('raw.h','runtime.h'):
            (include/name).write_text('/* declarations supplied by host harness */\n')
        env=dict(os.environ,TMPDIR=str(self.folder),ASAN_OPTIONS='detect_leaks=0')
        common=['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                '-I',str(self.folder/'handshake-include'),'-I',str(ROOT/'apps/desktop'),
                '-I',str(ROOT/'src/kernel/include')]
        desktop=self.folder/'desktop';demo=self.folder/'demo';kernel=self.folder/'kernel'
        subprocess.run([*common,str(ROOT/'tests/host/desktop/demo_gate_test.c'),
                        *(str(ROOT/'apps/desktop'/n) for n in ('protocol.c','compositor.c')),
                        '-o',str(demo)],check=True,env=env)
        subprocess.run([*common,str(ROOT/'tests/host/desktop/desktop_gate_test.c'),
                        *(str(ROOT/'apps/desktop'/n) for n in ('client.c','protocol.c','input.c','compositor.c')),
                        '-o',str(desktop)],check=True,env=env)
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                        '-I',str(ROOT/'src/kernel/include'),
                        str(ROOT/'tests/host/proc/desktop_test.c'),str(ROOT/'tests/host/proc/signal_legacy.c'),
                        str(ROOT/'src/kernel/lib/sha256.c'),str(ROOT/'src/kernel/lib/fmt.c'),
                        str(ROOT/'src/kernel/probes/selector.c'),'-o',str(kernel)],check=True,env=env)
        def execute(program,*args):
            result=subprocess.run([str(program),*map(str,args)],capture_output=True,text=True,env=env,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            return result.stdout
        client=self.folder/'client.messages';server=self.folder/'server.messages'
        execute(demo,'--hello',client)
        desktop_output=execute(desktop,'--handshake',client,server)
        demo_output=execute(demo,'--handshake',server)
        desk=[line for line in desktop_output.splitlines() if line.startswith('case=native-desktop ')]
        peer=[line for line in demo_output.splitlines() if line.startswith('case=native-demo ')]
        self.assertEqual(len(desk),2,desktop_output);self.assertEqual(len(peer),2,demo_output)
        self.assertIn('generation=0 survivor=42 victim=0 cycle=0',desk[0])
        self.assertEqual(peer[0],'case=native-demo stage=2 turns=0 unauthorized=0 generation=0')
        self.assertEqual(peer[1],'case=native-demo stage=2 turns=0 unauthorized=0 generation=1')
        reports=self.folder/'handshake.reports'
        reports.write_text('\n'.join((desk[0],peer[0],desk[1],peer[1]))+'\n')
        failure=subprocess.run([str(desktop),'--input-error'],capture_output=True,text=True,env=env,timeout=10)
        self.assertEqual(failure.returncode,1,failure.stdout+failure.stderr)
        setup=self.folder/'setup.report'; setup.write_text(failure.stdout)
        output=execute(kernel,'--native-handshake',reports,setup)
        self.assertIn('CONFIGURE and snapshot PASS',output)
        self.assertIn('case=launch server=desktop pid=41 survivor=42 control=20065000 stage=2',output)
        self.assertIn('reason=ok',output)
        for check in ('survivor_parent','survivor_group','control_range'):
            self.assertIn('reason=invalid_report:'+check,output)
        self.assertIn('case=step server=desktop command=5 generation=2 reached=0 live=1 survivor=1',output)
        self.assertIn('reason=control_write',output)
        self.assertIn('reason=setup:input:5',output)
        for program in (desktop,demo):
            failure=subprocess.run([str(program),'--report-error'],capture_output=True,text=True,env=env,timeout=10)
            self.assertEqual(failure.returncode,126,failure.stdout+failure.stderr)
            self.assertIn('gate report failed length=',failure.stderr)
            self.assertIn('error=14',failure.stderr)

    def test_guest_interaction_binding_and_failures(self):
        rows=self.interaction_records();runner.desktop_interaction(rows)
        for key,value in (('presents','10'),('input_events','0'),('pixel_digest','12345678'),('keys','1'),('motion','0'),('buttons','1')):
            bad=copy.deepcopy(rows);bad[-1][key]=value
            with self.assertRaises(EvidenceError):runner.desktop_interaction(bad)
        bad=copy.deepcopy(rows);bad[1]['presents']='9'
        with self.assertRaises(EvidenceError):runner.desktop_interaction(bad)
        with self.assertRaises(EvidenceError):runner.desktop_interaction(rows+rows[-1:])

    def test_all_suite_cases_and_predicates(self):
        from evidence import Parser
        suite=runner.load(ROOT/'tests/suites/f2-desktop.json')
        normal=[c for c in suite['cases'] if '-normal-' in c['id']]
        self.assertEqual({c['profile'] for c in normal},{'qemu-t23','qemu-e500','qemu-min128','qemu-desktop-1998','qemu-desktop-2002'})
        self.assertEqual(len(suite['cases']),11)
        for case in suite['cases']:
            native=case in normal;server='desktop' if native else 'standin'
            profile=runner.load(ROOT/'tests/profiles'/f"{case['profile']}.json")
            _,request=runner.qemu_args('fake',profile,case,'12345678',self.folder/'overlay',self.folder/'bios')
            selected=runner.selector(request,'fw_cfg',True)
            self.assertEqual(selected.get('server','standin'),server)
            self.assertEqual(case['timeout'],300)
            self.assertEqual(case.get('desktop_screen',False),native)
            self.assertEqual(case.get('actions',[]),runner.desktop_stimulus() if native else [])
            rows=[dict(event='BEGIN'),dict(event='END',status='PASS')]
            for predicate in case['expected']['predicates']:
                for i in range(predicate['exact_count']):
                    row={k:str(v) for k,v in predicate['where'].items()}
                    row.update({k:'1' for k in predicate.get('required_fields',[])})
                    for field,rule in predicate.get('fields',{}).items():
                        value=rule.get('eq',rule.get('ge',1)) if isinstance(rule,dict) else rule
                        row[field]=str(11 if isinstance(value,str) and value.startswith('$') else value)
                        if isinstance(value,str) and value.startswith('$'):row[value[1:]]='11'
                    if predicate.get('unique'):row[predicate['unique']]=str(i+1)
                    rows.append(row)
            parser=Parser('12345678','crash-isolation');parser.records=rows;parser.terminal=rows[1];parser.outcome='pass'
            parser.check(case['expected'])
            bad=next(r for r in rows if r.get('case')=='progress');bad['replies']='99'
            with self.assertRaises(EvidenceError):parser.check(case['expected'])

    def test_live_runner_capture_before_teardown_and_missing_interaction(self):
        from test_runner import RunnerTests
        fixture=RunnerTests();fixture.setUp()
        try:
            width,height=640,480
            before=self.screen(width,height);after=bytearray(before)
            for y in range(height//2,height//2+16):
                off=(y*width+width//2)*3;before[off:off+24]=b'\xff'*24
            b=self.write_screen('runner-before.ppm',width,height,before)
            a=self.write_screen('runner-after.ppm',width,height,after)
            fixture.case.update(probe='crash-isolation',selector='f2:crash-isolation run={run_id} server=desktop',
                desktop_screen=True,timeout=4,actions=runner.desktop_stimulus(),expected={'terminal':'END'})
            rows=self.interaction_records()
            # The live observation is received well before terminal evidence.
            result,directory=fixture.run_fake(records=[dict(event='BEGIN'),rows[0]],finish_after_input=5,
                after_input=rows[1:],delayed_terminal=[dict(event='END',status='PASS')],desktop_screens={'before':str(b),'after':str(a)})
            self.assertEqual(result['outcome'],'pass',result.get('reason'))
            self.assertEqual(result['desktop_screen']['ppm_sha256'],runner.sha(a))
            self.assertEqual(result['desktop_interaction']['after']['buttons'],'2')
            self.assertFalse(list(directory.glob('*.ppm')))
            result,_=fixture.run_fake(records=[dict(event='BEGIN'),dict(event='END',status='PASS')])
            self.assertEqual(result['outcome'],'fail')
        finally:fixture.tearDown()


if __name__ == '__main__':unittest.main()
