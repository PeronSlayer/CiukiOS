import fcntl
import ctypes
import json
import io
import os
from pathlib import Path
import signal
import select
import socket
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
from contextlib import contextmanager
from loader_model import selector, F1_PROBES, SELECTOR_RE

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/test'))
import run as runner
import resources as res
from evidence import Parser,EvidenceError


class FakeHost(runner.Host):
    """Uses real child processes/QMP/FIFO, with no systemd or QEMU invocation."""
    def __init__(self,cgroup,bad_limits=False):self.cgroup=cgroup;self.process=None;self.bad_limits=bad_limits
    def preflight(self,root):res.check_budget(root);return 3*res.GIB
    def launch(self,args,cwd):
        assert args[:3]==['systemd-run','--user','--scope']
        assert 'MemoryMax=1500M' in args and 'MemorySwapMax=0' in args
        actual=args[args.index('--')+1:]
        for name in ('commands.fifo','responses.fifo'):os.mkfifo(cwd/name)
        self.tx=os.open(cwd/'commands.fifo',os.O_RDWR|os.O_NONBLOCK)
        self.rx=os.open(cwd/'responses.fifo',os.O_RDWR|os.O_NONBLOCK)
        self.process=subprocess.Popen(actual,cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
        return self.process
    def connect_qmp(self,path,log):
        host=self
        class Transport:
            def sendall(self,data):os.write(host.tx,data)
            def recv(self,size):
                if not select.select([host.rx],[],[],.3)[0]:raise socket.timeout('fixture QMP timeout')
                return os.read(host.rx,size)
            def close(self):
                os.close(host.tx);os.close(host.rx);host.tx=host.rx=None
        return runner.QMP(path,log,Transport())
    def verify(self,unit):
        if self.bad_limits:raise res.Refusal('effective limits mismatch (fixture)')
        return {'MemoryMax':str(res.MEMORY_MAX),'MemorySwapMax':'0','memory.max':str(res.MEMORY_MAX),'memory.swap.max':'0'},self.cgroup
    def unowned_qemu_count(self):return 0
    def scope_pids(self,path):
        if self.process is None:return []
        found=[]
        for p in Path('/proc').glob('[0-9]*/stat'):
            try:
                words=p.read_text().rsplit(')',1)[1].split()
                if words[0]!='Z' and int(words[2])==self.process.pid:found.append(int(p.parent.name))
            except (OSError,ValueError,IndexError):continue
        return found
    def kill_scope(self,unit,sig):
        try:os.killpg(self.process.pid,getattr(signal,sig))
        except ProcessLookupError:pass


class RunnerTests(unittest.TestCase):
    def setUp(self):
        scratch=ROOT/'build/runner-host-tests';scratch.mkdir(parents=True,exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=scratch);self.root=Path(self.temp.name)
        self.image=self.root/'image.raw';self.image.write_bytes(b'canonical immutable image')
        self.firmware=self.root/'bios.bin';self.firmware.write_bytes(b'firmware fixture')
        self.cgroup=self.root/'cgroup';self.cgroup.mkdir();(self.cgroup/'memory.events').write_text('oom 0\noom_kill 0\n')
        self.img=self.root/'fake-img';self.img.write_text('#!/usr/bin/env python3\nimport pathlib,sys\nassert sys.argv[1:4]==["create","-f","qcow2"]\nassert "-b" in sys.argv and "-F" in sys.argv\npathlib.Path(sys.argv[-1]).write_bytes(b"fixture overlay")\n');self.img.chmod(0o755)
        self.script=self.root/'scenario.json'
        self.identity=patch.object(runner,'git_identity',return_value={'revision':'fixture','dirty':False});self.identity.start()
        self.env=patch.dict(os.environ,{'CIUKI_FAKE_SCRIPT':str(self.script)});self.env.start()
        self.profile=dict(name='qemu-t23',machine='pc-i440fx-9.2',cpu='pentium3',accelerator='tcg',ram_mib=512,vga='std',audio='AC97')
        self.case=dict(id='boot',probe='boot',timeout=2,expected=dict(terminal='END',predicates=[dict(where={'event':'DATA'},fields={'tick':{'ge':10000}})]))
    def tearDown(self):self.env.stop();self.identity.stop();self.temp.cleanup()
    def run_fake(self,records=None,**scenario):
        if records is None:records=[{'event':'BEGIN'},{'event':'DATA','tick':'10000'},{'event':'END','status':'PASS'}]
        self.script.write_text(json.dumps(dict(records=records,**scenario)))
        host=FakeHost(self.cgroup)
        with res.ExclusiveLock(self.root/'shared.lock'):
            result,directory=runner.run_case(self.root,'fixture',self.case,self.profile,self.image,str(ROOT/'tests/host/fake_qemu.py'),self.firmware,host=host,qemu_img=str(self.img))
        self.assertEqual(host.scope_pids(self.cgroup),[])
        if getattr(host,'tx',None) is not None:os.close(host.tx);os.close(host.rx)
        return result,directory
    def test_pass_hash_limits_and_overlay_retention(self):
        result,directory=self.run_fake()
        self.assertEqual(result['outcome'],'pass',result['reason'])
        self.assertEqual(result['image']['sha256'],runner.sha(self.image));self.assertEqual(result['image']['sha256_after'],result['image']['sha256'])
        self.assertEqual(set(p.name for p in directory.iterdir()),{'result.json','serial.log'})
        self.assertEqual(result['host']['effective_limits']['memory.swap.max'],'0')
        self.assertTrue(result['cleanup']['clean']);self.assertTrue(result['cleanup']['qmp_quit'])
    def test_missing_and_malformed_markers_retain_overlay(self):
        for records in ([{'event':'BEGIN'},{'event':'END','status':'PASS'}],['CIUKI_TEST v=1 v=1 run={run_id} seq=000001 probe=boot event=BEGIN']):
            with self.subTest(records=records):
                result,directory=self.run_fake(records=records)
                self.assertEqual(result['outcome'],'fail');self.assertTrue((directory/'run.qcow2').exists());self.assertTrue(result['cleanup']['clean'])
    def test_missing_terminal_and_timeout(self):
        self.case['timeout']=.35
        result,_=self.run_fake(records=[{'event':'BEGIN'}])
        self.assertEqual(result['outcome'],'fail');self.assertTrue(result['timeout']['occurred'])
    def test_child_termination_escalates_to_kill(self):
        self.case['timeout']=.35
        result,_=self.run_fake(records=[],child=True,ignore_quit=True)
        self.assertEqual(result['outcome'],'fail');self.assertTrue(result['cleanup']['killed_scope']);self.assertEqual(result['cleanup']['remaining_children'],[])
    def test_lock_contention_in_another_process(self):
        lock=self.root/'shared.lock'
        with res.ExclusiveLock(lock):
            code='import sys;sys.path.insert(0,sys.argv[1]);from resources import ExclusiveLock,Refusal\ntry:\n with ExclusiveLock(sys.argv[2]):sys.exit(1)\nexcept Refusal:sys.exit(0)'
            result=subprocess.run([sys.executable,'-c',code,str(ROOT/'scripts/test'),str(lock)],capture_output=True,timeout=3)
            self.assertEqual(result.returncode,0,result.stderr)
    def test_real_git_common_directory_shared_inode(self):
        main=self.root/'main';common=main/'.git';(common/'objects').mkdir(parents=True);(common/'refs').mkdir()
        (common/'HEAD').write_text('ref: refs/heads/main\n');(common/'config').write_text('[core]\nrepositoryformatversion = 0\nbare = false\n')
        paths=[]
        for name in ('a','b'):
            wt=self.root/name;wt.mkdir();meta=common/'worktrees'/name;meta.mkdir(parents=True)
            (wt/'.git').write_text('gitdir: '+str(meta)+'\n');(meta/'commondir').write_text('../..\n');(meta/'gitdir').write_text(str(wt/'.git')+'\n');(meta/'HEAD').write_text('ref: refs/heads/main\n')
            paths.append(res.common_lock(wt))
        self.assertEqual(paths,[main/'build/test-runs/.qemu.lock']*2)
        with res.ExclusiveLock(paths[0]):
            with self.assertRaises(res.Refusal):
                with res.ExclusiveLock(paths[1]):pass
            self.assertEqual(paths[0].stat().st_ino,paths[1].stat().st_ino)
    def test_insufficient_memory_existing_qemu_heavy_build(self):
        with patch.object(res,'qemu_count',return_value=0),patch.object(res,'heavy_builds',return_value=[]),patch.object(res,'available_memory',return_value=res.GIB):
            with self.assertRaisesRegex(res.Refusal,'MemAvailable'):res.preflight(self.root,0)
        with patch.object(res,'qemu_count',return_value=1):
            with self.assertRaisesRegex(res.Refusal,'another qemu-system'):res.preflight(self.root,0)
        with patch.object(res,'qemu_count',return_value=0),patch.object(res,'heavy_builds',return_value=[123]):
            with self.assertRaisesRegex(res.Refusal,'heavy build'):res.preflight(self.root,0)
    def test_sparse_disk_budget_refusal(self):
        runs=self.root/'build/test-runs';runs.mkdir(parents=True)
        with (runs/'sparse').open('wb') as stream:stream.truncate(2*res.GIB+1)
        self.assertLess((runs/'sparse').stat().st_blocks*512,res.GIB)
        with self.assertRaisesRegex(res.Refusal,'disk budget'):self.run_fake()
        self.assertFalse((runs/'fixture').exists())
    def test_retention_last_five(self):
        runs=self.root/'runs';runs.mkdir()
        for i in range(7):
            d=runs/str(i);d.mkdir();os.utime(d,(100+i,100+i))
        res.prune(runs);self.assertEqual(sorted(p.name for p in runs.iterdir()),['2','3','4','5','6'])
    def test_serial_cap(self):
        result,directory=self.run_fake(records=[],flood=True)
        self.assertEqual(result['outcome'],'fail');self.assertIn('cap',result['reason'])
        self.assertLessEqual((directory/'serial.log').stat().st_size,res.LOG_CAP)
    def test_unverified_scope_never_executes_qemu(self):
        self.script.write_text(json.dumps({'records':[]}));self.case['timeout']=.25
        host=FakeHost(self.cgroup,bad_limits=True)
        result,directory=runner.run_case(self.root,'fixture',self.case,self.profile,self.image,str(ROOT/'tests/host/fake_qemu.py'),self.firmware,host=host,qemu_img=str(self.img))
        self.assertEqual(result['outcome'],'fail');self.assertFalse((directory/'gate').exists());self.assertEqual(result['observed'],[])
        if getattr(host,'tx',None) is not None:os.close(host.tx);os.close(host.rx)
    def test_unexpected_reset_fails(self):
        result,_=self.run_fake(reset=True);self.assertEqual(result['outcome'],'fail');self.assertIn('reset',result['reason'])
    def test_warm_restart_uses_qmp_reset(self):
        self.case['boot_kind']='restart'
        result,_=self.run_fake();self.assertEqual(result['outcome'],'pass',result['reason']);self.assertIn('restart_prerequisite',result)
    def test_panic_observation_storage_and_no_pass(self):
        self.case.update(probe='panic',timeout=7,expected={'terminal':'PANIC','predicates':[]})
        records=[{'event':'BEGIN'},{'event':'ARM','expected_error':'00000000','expected_eip':'c0101234','expected_cr2':'dead0000'},{'event':'PANIC','vector':'14','error':'00000000','eip':'c0101234','cr2':'dead0000'}]
        result,_=self.run_fake(records=records)
        self.assertEqual(result['outcome'],'pass',result['reason']);self.assertGreaterEqual(result['panic_observation_seconds'],5)
        result,_=self.run_fake(records=records,writes=512)
        self.assertEqual(result['outcome'],'fail');self.assertIn('block writes',result['reason'])

    def test_f1_missing_probe_is_not_run_after_ready(self):
        self.case.update(probe='input',selector='f1:input run={run_id}',expected={'terminal':'END'})
        result,_=self.run_fake(records=[{'event':'BEGIN'},{'event':'READY','installed':'0'},
                                        {'event':'ERROR','status':'not_run','reason':'missing_probe'}])
        self.assertEqual(result['outcome'],'not_run',result['reason']);self.assertTrue(result['cleanup']['clean'])

    def test_f1_input_waits_for_ready_records_batches_and_checks_guest(self):
        self.case.update(probe='input',selector='f1:input run={run_id}',expected={'terminal':'END','predicates':[
            {'where':{'event':'DATA'},'fields':{'key_transitions':{'eq':2}}}]},actions=[{'type':'input',
            'after':{'event':'READY'},'batches':[
                {'events':[{'type':'key','data':{'down':True,'key':{'type':'qcode','data':'a'}}}], 'pause_ms':10},
                {'events':[{'type':'key','data':{'down':False,'key':{'type':'qcode','data':'a'}}},
                           {'type':'rel','data':{'axis':'x','value':2}},
                           {'type':'btn','data':{'down':True,'button':'left'}}], 'pause_ms':50}]}])
        result,_=self.run_fake(records=[{'event':'BEGIN'},{'event':'READY'}],finish_after_input=2,
                              after_input=[{'event':'DATA','key_transitions':'2'},{'event':'END','status':'PASS'}])
        self.assertEqual(result['outcome'],'pass',result['reason'])
        observed=result['stimulus']['observed'];self.assertEqual(len(observed),2)
        self.assertEqual(observed[0]['sync_seq'],'000002')
        self.assertGreaterEqual(observed[1]['host_monotonic']-observed[0]['host_monotonic'],.01)
        self.case['actions'][0]['after']['event']='ARM'
        result,directory=self.run_fake(records=[{'event':'BEGIN'},{'event':'READY'}])
        self.assertEqual(result['outcome'],'fail')
        requests=[json.loads(line) for line in (directory/'fake-qmp.jsonl').read_text().splitlines()]
        self.assertFalse(any(r['execute']=='input-send-event' for r in requests))

    def test_blkdebug_arguments_cache_safety_and_fault_layer(self):
        self.case['fault']={'layer':'host-block-backend','event':'flush_to_disk','errno':5}
        result,_=self.run_fake();self.assertEqual(result['outcome'],'pass',result['reason'])
        drive=result['qemu']['arguments'][result['qemu']['arguments'].index('-drive')+1]
        self.assertIn('blkdebug',drive);self.assertIn('cache=writeback',drive)
        self.assertEqual(result['fault']['layer'],'host-block-backend')
        self.case['disk_cache']='unsafe'
        with self.assertRaisesRegex(res.Refusal,'cache'):self.run_fake()

    def test_overlay_boot_cfg_patch_manifest_round_trip(self):
        patch_decl=[{'file':'SYSTEM/BOOT.CFG','offset':0,'before_hex':b'safe=0'.hex(),'after_hex':b'safe=1'.hex()}]
        self.case['patches']=patch_decl;contents=bytearray(b'safe=0 serial=1\n')
        def read(host,overlay,offset,length):return bytes(contents[offset-4096:offset-4096+length])
        def write(host,overlay,offset,payload,length):contents[offset-4096:offset-4096+length]=payload.read_bytes()
        with patch.object(runner,'boot_cfg_extent',return_value=(4096,len(contents))), \
             patch.object(FakeHost,'overlay_read',read),patch.object(FakeHost,'overlay_write',write):
            result,_=self.run_fake()
        self.assertEqual(result['outcome'],'pass',result['reason'])
        manifest=json.loads(json.dumps(result['patch_manifest']))[0]
        self.assertEqual(manifest['offset'],4096);self.assertEqual(bytes.fromhex(manifest['after_hex']),b'safe=1')
        import hashlib
        self.assertEqual(manifest['sha256_before'],hashlib.sha256(b'safe=0').hexdigest())
        self.assertEqual(manifest['sha256_after'],hashlib.sha256(b'safe=1').hexdigest())
        self.assertEqual(self.image.read_bytes(),b'canonical immutable image')

    def test_declared_cold_reboots_reuse_one_overlay(self):
        self.case['boots']=[{'boot_kind':'cold'},{'boot_kind':'cold'}]
        result,directory=self.run_fake()
        self.assertEqual(result['outcome'],'pass',result['reason'])
        boots=result['reboot_sequence'];self.assertEqual(len(boots),2)
        self.assertEqual(boots[0]['overlay'],boots[1]['overlay'])
        self.assertNotEqual(boots[0]['run_id'],boots[1]['run_id'])
        self.assertTrue(all(b['cleanup']['clean'] for b in boots))
        self.assertFalse(Path(boots[0]['overlay']).exists())

    def test_readonly_export_after_stop_before_fsck_and_mtools(self):
        self.case['checks']={'offset':0,'size':len(self.image.read_bytes()),'listing_contains':['EMPTY']}
        order=[]
        @contextmanager
        def export(host,overlay,directory,offset,size):
            self.assertIsNotNone(host.process.poll());self.assertEqual(host.scope_pids(host.cgroup),[])
            self.assertTrue((directory/'fake-stopped').exists());order.append('export-readonly')
            yield self.image
            order.append('export-closed')
        def check(host,args,directory):
            order.append(args[0]+(' -n' if '-n' in args else ''))
            return {'arguments':args,'returncode':0,'output':'EMPTY','output_sha256':'fixture'}
        with patch.object(FakeHost,'export_readonly',export),patch.object(FakeHost,'checker',check):
            result,_=self.run_fake()
        self.assertEqual(result['outcome'],'pass',result['reason'])
        self.assertLess(order.index('export-readonly'),order.index('fsck.fat -n'))
        self.assertEqual(order[-1],'export-closed');self.assertEqual(len(result['checkers']),4)

    def test_cut_is_guest_termination_and_reboot_uses_same_overlay(self):
        self.case['boots']=[{'actions':[{'type':'cut','after':{'event':'ARM','cut':'directory-publication'},'mode':'guest-termination'}],
                             'expected':{'terminal':'ARM','predicates':[]}},
                            {'actions':[],'expected':{'terminal':'END','predicates':[]}}]
        result,_=self.run_fake(boot_records=[[{'event':'BEGIN'},{'event':'ARM','cut':'directory-publication'}],
                                                  [{'event':'BEGIN'},{'event':'END','status':'PASS'}]])
        self.assertEqual(result['outcome'],'pass',result['reason'])
        self.assertEqual(result['reboot_sequence'][0]['cut_point']['mode'],'guest-termination')
        self.assertEqual(len({b['overlay'] for b in result['reboot_sequence']}),1)

    def test_real_qcow2_patch_and_fat32_extent(self):
        if not all(shutil.which(tool) for tool in ('qemu-img','qemu-io','mkfs.fat','mcopy','mmd')):
            self.skipTest('qemu image/FAT host tools unavailable')
        image=self.root/'fat.raw'
        with image.open('wb') as stream:stream.truncate(64*1024**2+512)
        subprocess.run(['mkfs.fat','--invariant','--offset=1','-F','32',str(image)],check=True,capture_output=True)
        volume=str(image)+'@@512'
        subprocess.run(['mmd','-i',volume,'::/SYSTEM'],check=True,capture_output=True)
        cfg=self.root/'BOOT.CFG';cfg.write_bytes(b'safe=0 serial=1\n')
        subprocess.run(['mcopy','-i',volume,str(cfg),'::/SYSTEM/BOOT.CFG'],check=True,capture_output=True)
        mbr=bytearray(512);mbr[510:]=b'\x55\xaa'
        struct.pack_into('<II',mbr,454,1,64*1024**2//512)
        with image.open('r+b') as stream:stream.write(mbr)
        offset,size=runner.boot_cfg_extent(image)
        self.assertEqual(size,len(cfg.read_bytes()))
        with image.open('rb') as stream:stream.seek(offset);self.assertEqual(stream.read(size),cfg.read_bytes())
        overlay=self.root/'real.qcow2'
        subprocess.run(['qemu-img','create','-f','qcow2','-b',str(image),'-F','raw',str(overlay)],check=True,capture_output=True)
        before=runner.sha(image)
        manifest=runner.patch_overlay(runner.Host(),image,overlay,self.root,[{'file':'SYSTEM/BOOT.CFG','offset':0,
                  'before_hex':b'safe=0'.hex(),'after_hex':b'safe=1'.hex()}])
        self.assertEqual(manifest[0]['offset'],offset)
        self.assertEqual(runner.Host().overlay_read(overlay,offset,6),b'safe=1')
        self.assertEqual(runner.sha(image),before)

    def test_independent_fat_fixture_hashes_and_guest_mismatch(self):
        self.case['fixtures']=[{'generator':'mkfs.fat','fat_type':32,'seed':1}]
        fixture={'files':[{'id':'fixture-0-lfn','path':'/Ciuki long fixture.txt','size':8,'sha256':'a'*64}],
                 'sha256':'b'*64,'path':'fixture-0.img'}
        with patch.object(FakeHost,'fat_fixture',return_value=fixture):
            result,_=self.run_fake()
        self.assertEqual(result['outcome'],'fail');self.assertIn('missing evidence',result['reason'])
        self.assertEqual(result['fixtures']['manifest'][0]['sha256'],'b'*64)
        self.assertIn('name=opt/it.alcybercloud.ciukios/fixture,file=fixtures.json',result['qemu']['arguments'])


    def test_production_kernel_selector_names_and_empty_dispatch(self):
        if not shutil.which('clang'):self.skipTest('host clang unavailable')
        source=(ROOT/'src/kernel/probes/probes.c').read_text()
        first=source.index('static bool parse_selector(');last=source.index('\nstatic __attribute__',first)
        harness=self.root/'selector.c'
        harness.write_text('#include <stdbool.h>\n#include <string.h>\n'
            '#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))\n'
            'struct probe_def { const char *name; int (*fn)(void); };\n'
            'static int emitted;\nstatic void rec_set_run(const char *run) {}\n'
            'static void rec_emit(const char *p,const char *event,const char *fmt,...) { emitted++; }\n'
            "static bool is_hex(char c) { return (c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F'); }\n"
            +source[first:last]+'\nint validate(const char *s,unsigned len,char *probe) { char run[9]; emitted=0; return parse_selector(s,len,probe,24,run); }\n'
            'int records(void) { return emitted; }\n')
        library=self.root/'selector.so'
        subprocess.run(['clang','-shared','-fPIC','-std=c17','-Wall','-Werror',str(harness),'-o',str(library)],check=True,capture_output=True)
        native=ctypes.CDLL(str(library));native.validate.argtypes=[ctypes.c_char_p,ctypes.c_uint,ctypes.c_void_p]
        for name in (*F1_PROBES,'all','core'):
            text=('f1:'+name+' run=12ab34cd').encode();probe=ctypes.create_string_buffer(24)
            self.assertEqual(native.validate(text,len(text),probe),1)
            self.assertEqual(probe.value,b'f1:'+name.encode());self.assertEqual(native.records(),3)
        for text in (b'f1:unknown run=12ab34cd',b'f1:input run=12ab34cd run=12345678',
                     b'f1:input run=12ab34cd safe=1 safe=1',b'f1:input run=12ab34cd '+b'x'*64,
                     b'f0:input run=12ab34cd',b'f1:boot run=12ab34cd'):
            self.assertEqual(native.validate(text,len(text),ctypes.create_string_buffer(24)),0)
            self.assertEqual(native.records(),0)
        text=b'f0:all run=12ab34cd';probe=ctypes.create_string_buffer(24)
        self.assertEqual(native.validate(text,len(text),probe),1);self.assertEqual(probe.value,b'all')
        self.assertEqual(native.records(),0)


    def test_main_holds_shared_lock_for_host_prerequisite_and_boot(self):
        lock=self.root/'common.lock';directory=self.root/'build/test-runs/f1-input/12345678'
        (self.root/'config').mkdir();(self.root/'config/toolchain.json').write_text(json.dumps({'qemu_machine':self.profile['machine']}))
        case={**self.case,'id':'registry','probe':'registry','profile':'qemu-t23','selector':'f1:registry run={run_id}'}
        profile={**self.profile,'icount':'shift=1,sleep=on'}
        def locked():
            with self.assertRaises(res.Refusal):
                with res.ExclusiveLock(lock):pass
        def checked(args,**kwargs):
            locked();self.assertIn('unittest',args)
            return subprocess.CompletedProcess(args,0,b'host fixture passed',b'')
        def boot(*args,**kwargs):
            locked();directory.mkdir(parents=True)
            return {'run_id':'12345678','outcome':'not_run','reason':'missing F1 probe'},directory
        def load(path):
            return {'schema_version':1,'image':'full','cases':[case]} if path.name=='f1-input.json' else profile
        with patch.object(runner,'ROOT',self.root),patch.object(runner,'load',side_effect=load), \
             patch.object(res,'common_lock',return_value=lock),patch.object(runner.Host,'preflight',return_value=3*res.GIB), \
             patch.object(runner.subprocess,'run',side_effect=checked),patch.object(runner,'run_case',side_effect=boot), \
             patch.object(runner.shutil,'which',return_value='fake'),patch.object(runner,'find_firmware',return_value=self.firmware), \
             patch('sys.stdout',new_callable=io.StringIO):
            code=runner.main(['f1-input','--image',str(self.image)])
        self.assertEqual(code,1)
        summary=json.loads((directory.parent/'summary.json').read_text())
        self.assertEqual(summary['cases'][0]['outcome'],'not_run')
        with res.ExclusiveLock(lock):pass

    def test_checker_failure_retains_overlay_and_cannot_pass(self):
        self.case['checks']={'offset':0,'size':len(self.image.read_bytes())}
        @contextmanager
        def export(host,overlay,directory,offset,size):
            self.assertIsNotNone(host.process.poll());yield self.image
        def check(host,args,directory):
            return {'arguments':args,'returncode':1 if '-n' in args else 0,
                    'output':'fsck.fat 4.2\nUnexpected cross-link','output_sha256':'fixture'}
        with patch.object(FakeHost,'export_readonly',export),patch.object(FakeHost,'checker',check):
            result,directory=self.run_fake()
        self.assertEqual(result['outcome'],'fail');self.assertIn('fsck',result['reason'])
        self.assertTrue((directory/'run.qcow2').exists())


class F1SelectorTests(unittest.TestCase):
    def test_f1_names_aliases_trust_duplicates_oversize(self):
        for name in (*F1_PROBES,'all','core'):
            with self.subTest(name=name):
                self.assertEqual(selector('f1:'+name+' run=12ab34cd')['probe'],name)
                self.assertTrue(selector('f1:'+name+' run=12ab34cd platform=e500 safe=1','fw_cfg',True)['safe'])
        for text in ('f1:unknown run=12ab34cd','f1:input run=12ab34cd run=12345678',
                     'f1:input run=12ab34cd safe=1 safe=1','f1:input run=12ab34cd safe=1 platform=e500',
                     'f1:input run=12ab34cd '+'x'*64,'f1:boot run=12ab34cd','f0:input run=12ab34cd'):
            with self.subTest(text=text),self.assertRaises(ValueError):selector(text,'fw_cfg',True)
        with self.assertRaises(ValueError):selector('f1:input run=12ab34cd safe=1')

    def test_alias_order_and_suite_deadlines(self):
        templates=[{'id':p,'probe':p,'profile':'qemu-t23','timeout':120,'expected':{'terminal':'END'}} for p in F1_PROBES]
        cases=runner.expand_cases({'cases':[{'probe':'all','probe_cases':templates}]})
        self.assertEqual([c['probe'] for c in cases],list(F1_PROBES))
        self.assertTrue(all(selector(c['selector'].format(run_id='12345678'))['probe']==c['probe'] for c in cases))
        for name,deadline in [('input',120),('storage',180),('fat32',300),('safe',90)]:
            suite=runner.load(ROOT/'tests/suites'/('f1-'+name+'.json'))
            self.assertEqual(suite['prerequisites'][0],'f0-smoke')
            self.assertTrue(all(c['timeout']==deadline for c in suite['cases']))
            for c in suite['cases']:
                profile=runner.load(ROOT/'tests/profiles'/(c['profile']+'.json'))
                args,text=runner.qemu_args('fake',profile,c,'12345678',ROOT/'build/run.qcow2',ROOT/'build/bios.bin')
                self.assertEqual(selector(text,'fw_cfg',True)['probe'],c['probe'])
                self.assertIn('shift=1,sleep=on',args)


class ParserTests(unittest.TestCase):
    def test_strict_grammar_sequence_terminal_and_length(self):
        for bad in [b'CIUKI_TEST v=1 run=12345678 seq=000001 probe=boot event=BEGIN v=1',b'CIUKI_TEST v=1 run=87654321 seq=000001 probe=boot event=BEGIN',b'CIUKI_TEST v=1 run=12345678 seq=1 probe=boot event=BEGIN',b'CIUKI_TEST v=1 run=12345678 seq=000001 probe=boot event=BEGIN x='+b'a'*200,b' CIUKI_TEST v=1 run=12345678 seq=000001 probe=boot event=BEGIN',b'CIUKI_TEST v=1 run=12345678 seq=000001 probe=boot event=BEGIN x=\xff']:
            with self.subTest(bad=bad[:80]),self.assertRaises(EvidenceError):Parser('12345678','boot').feed(bad)
        p=Parser('12345678','boot');p.feed(b'CIUKI_TEST v=1 run=12345678 seq=000001 probe=boot event=BEGIN')
        with self.assertRaises(EvidenceError):p.feed(b'CIUKI_TEST v=1 run=12345678 seq=000001 probe=boot event=DATA')
        p.feed(b'CIUKI_TEST v=1 run=12345678 seq=000002 probe=boot event=END status=PASS')
        with self.assertRaises(EvidenceError):p.feed(b'CIUKI_TEST v=1 run=12345678 seq=000003 probe=boot event=DATA tick=1')
    def test_numeric_fields_are_not_repaired(self):
        p=Parser('12345678','boot')
        for seq,event,data in [(1,'BEGIN',''),(2,'DATA',' tick=0x10000'),(3,'END',' status=PASS')]:
            p.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=boot event={event}{data}'.encode())
        with self.assertRaises(EvidenceError):p.check({'terminal':'END','predicates':[{'where':{'event':'DATA'},'fields':{'tick':{'ge':1}}}]})
    def test_qmp_log_cap_and_scope_limit_refusal(self):
        with patch.object(res,'scope_properties',return_value={'MemoryMax':'infinity','MemorySwapMax':'0'}):
            with self.assertRaisesRegex(res.Refusal,'effective'):res.verify_scope('fixture.scope')

class GroupedEvidenceTests(unittest.TestCase):
    def test_long_summary_can_span_bounded_records(self):
        parser=Parser('12345678','boot')
        records=['event=BEGIN','event=DATA group=boot ready_tick=1','event=DATA group=boot final_tick=10001','event=END status=PASS']
        for i,record in enumerate(records,1):parser.feed(f'CIUKI_TEST v=1 run=12345678 seq={i:06d} probe=boot {record}'.encode())
        self.assertTrue(parser.check({'terminal':'END','predicates':[{'where':{'event':'DATA','group':'boot'},'combine':True,'fields':{'ready_tick':{'ge':1},'final_tick':{'ge':'$ready_tick'}}}]}))
    def test_conflicting_summary_cannot_hide_failure(self):
        parser=Parser('12345678','boot')
        for i,record in enumerate(['event=BEGIN','event=DATA group=boot errors=1','event=DATA group=boot errors=0','event=END status=PASS'],1):
            parser.feed(f'CIUKI_TEST v=1 run=12345678 seq={i:06d} probe=boot {record}'.encode())
        with self.assertRaises(EvidenceError):parser.check({'terminal':'END','predicates':[{'where':{'event':'DATA','group':'boot'},'combine':True,'fields':{'errors':{'eq':0}}}]})
