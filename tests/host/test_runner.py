import fcntl
import copy
import hashlib
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

ROOT=Path(__file__).resolve().parents[2]
# Captured fsck.fat 4.2 diagnostics from f1-24, image 33a68c43…;
# only the per-boot export path is parameterized.
CRASH_ORPHAN_FSCK = ('fsck.fat 4.2 (2021-01-31)\n'
                     'Orphaned long file name part "F109CUT.BIN"\n'
                     '  Auto-deleting.\n'
                     'Dirty bit is set. Fs was not properly unmounted and some data may be corrupt.\n'
                     ' Automatically removing dirty bit.\n\n'
                     'Leaving filesystem unchanged.\n'
                     '{volume}: 69 files, 1465/130557 clusters\n')
sys.path.insert(0,str(ROOT/'scripts/test'))
from loader_model import selector, boot_options, F0_PROBES, F1_PROBES, F2_PROBES, SELECTOR_RE
import run as runner
import resources as res
from evidence import Parser,EvidenceError,ApplicationCapture,f2_metadata,F2_FIELDS


def linked_probe_names(rows, phase):
    """Read names at the production table bounds recorded by lld, without source slicing."""
    def address(symbol):
        return int(next(line.split()[0] for line in rows if symbol+' = .' in line),16)
    elf=(ROOT/'build/f0/VMM.ELF').read_bytes()
    phoff=struct.unpack_from('<I',elf,28)[0]
    phsize,phcount=struct.unpack_from('<HH',elf,42)
    segments=[struct.unpack_from('<8I',elf,phoff+i*phsize) for i in range(phcount)]
    def read(va,size):
        for kind,offset,base,physical,filesize,memory,flags,alignment in segments:
            if kind==1 and base<=va and va+size<=base+filesize:
                return elf[offset+va-base:offset+va-base+size]
        raise AssertionError('probe table address outside file-backed load segments')
    start,end=address(f'__f{phase}probes_start'),address(f'__f{phase}probes_end')
    if (end-start)%8:raise AssertionError('invalid target probe table extent')
    names=[]
    for va in range(start,end,8):
        pointer,callback=struct.unpack('<II',read(va,8))
        if not callback:raise AssertionError('probe registration has no implementation')
        names.append(read(pointer,24).split(b'\0',1)[0].decode('ascii'))
    return names


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
    def test_canonical_selector_safe_and_profile_platform(self):
        profile=json.loads((ROOT/'tests/profiles/qemu-e500.json').read_text())
        for suffix in (' safe=1',' platform=e500 safe=1'):
            case={**self.case,'selector':'f0:boot run={run_id}'+suffix}
            args,request=runner.qemu_args('fixture',profile,case,'12345678',self.image,self.firmware)
            self.assertEqual(request,'f0:boot run=12345678 platform=e500 safe=1')
            self.assertIn('name=opt/it.alcybercloud.ciukios/test,string='+request,args)
        case={**self.case,'selector':'f0:boot run={run_id} platform=e500'}
        self.assertEqual(runner.qemu_args('fixture',self.profile,case,'12345678',self.image,self.firmware)[1],
                         'f0:boot run=12345678 platform=e500')
    def test_desktop_profile_arguments_and_same_backing_image(self):
        for name,cpu,ram,vga in (('qemu-desktop-1998','pentium2',128,'cirrus'),
                                 ('qemu-desktop-2002','athlon',512,'std')):
            profile=runner.load(ROOT/'tests/profiles'/f'{name}.json')
            args,request=runner.qemu_args('fixture',profile,self.case,'12345678',self.image,self.firmware)
            with self.subTest(profile=name):
                for option,value in (('-machine','pc-i440fx-9.2'),('-cpu',cpu),('-accel','tcg'),
                                     ('-m',str(ram)),('-vga',vga),('-icount','shift=1,sleep=on')):
                    self.assertEqual(args[args.index(option)+1],value)
                self.assertEqual(profile['ide'],'PIIX');self.assertEqual(profile['input'],'i8042')
                self.assertEqual(profile['serial_base'],0x3f8)
                self.assertEqual(request,'f0:boot run=12345678')
                drive=args[args.index('-drive')+1]
                self.assertIn('file='+str(self.image)+',format=qcow2,if=ide,index=0,',drive)
                self.assertNotIn('-kernel',args);self.assertNotIn('-usb',args)
                self.assertNotIn('-device',args)

    def test_desktop_suite_bounds_and_native_path_predicates(self):
        core=runner.load(ROOT/'tests/suites/f0-core.json')['cases']
        inputs=runner.load(ROOT/'tests/suites/f1-input.json')['cases']
        safe=runner.load(ROOT/'tests/suites/f1-safe.json')['cases']
        for profile in ('qemu-desktop-1998','qemu-desktop-2002'):
            boots=[c for c in core if c['profile']==profile and c['probe']=='boot']
            self.assertEqual([c['attempt'] for c in boots],list(range(1,11)))
            self.assertEqual([c['boot_kind'] for c in boots],['cold']*5+['restart']*5)
            probes=[c['probe'] for c in core if c['profile']==profile and c['probe']!='boot']
            self.assertEqual(probes,[c['probe'] for c in core if c['profile']=='qemu-min128' and c['probe']!='boot'])
            selected=[c for c in inputs if c['profile']==profile and
                      not c['id'].startswith(('firmware_overrun','disallowed_io'))]
            self.assertEqual([c['probe'] for c in selected],['registry','input-fault','input','framebuffer'])
            for c in selected:
                original=next(t for t in inputs if t['profile']=='qemu-t23' and t['probe']==c['probe'])
                self.assertEqual(c['expected'],original['expected'])
                self.assertEqual(c.get('actions'),original.get('actions'))
                self.assertEqual(c['timeout'],120)
            selected_safe=[c for c in safe if c['profile']==profile]
            self.assertEqual(len(selected_safe),1)
            self.assertEqual(selected_safe[0]['expected'],safe[0]['expected'])
            self.assertEqual(selected_safe[0]['timeout'],90)
            for c in [*boots,*selected,*selected_safe]:
                request=selector(c['selector'].format(run_id='12345678'),'fw_cfg',True)
                self.assertIsNone(request['platform'])
                self.assertNotIn('device_exceptions',c)
                self.assertNotIn('patches',c)
            firmware=[c for c in inputs if c['profile']==profile and
                      c['id'].startswith(('firmware_overrun','disallowed_io'))]
            self.assertEqual(len(firmware),2)
            for c in firmware:
                self.assertEqual(c['probe'],'input-fault')
                self.assertEqual(c['timeout'],120)
                request=selector(c['selector'].format(run_id='12345678'),'fw_cfg',True)
                self.assertEqual(request['platform'],'e500')
    def test_duplicate_selector_keys_are_refused(self):
        for suffix in (' platform=e500 platform=e500',' safe=1 safe=1',' run=12345678'):
            case={**self.case,'selector':'f0:boot run={run_id}'+suffix}
            with self.subTest(suffix=suffix),self.assertRaises(ValueError):
                runner.qemu_args('fixture',self.profile,case,'12345678',self.image,self.firmware)
    def test_fixture_disk_connector_numbers_and_manifest(self):
        for count in range(4):
            fixtures=[{'generator':'mkfs.fat','fat_type':bits,'seed':1} for bits in (12,16,32)[:count]]
            # A nondisk fixture's ordinal must not leave a gap in IDE indices.
            fixtures.insert(0,{'label':'nondisk'})
            case={**self.case,'fixtures':fixtures}
            args,_=runner.qemu_args('fixture',self.profile,case,'12345678',self.image,self.firmware)
            drives=[args[i+1] for i,arg in enumerate(args) if arg=='-drive']
            self.assertEqual(len(drives),count+1)
            self.assertIn('if=ide,index=0,',drives[0])
            self.assertEqual(drives[1:],[f'file=fixture-{disk}.img,format=raw,if=ide,index={disk},cache=writeback' for disk in range(1,count+1)])
            with patch.object(FakeHost,'fat_fixture',return_value={'sha256':'a'*64}):
                manifest=runner.prepare_fixtures(FakeHost(self.cgroup),case,self.root)['manifest']
            self.assertNotIn('ide_index',manifest[0])
            self.assertEqual([item['ide_index'] for item in manifest[1:]],list(range(1,count+1)))
        case={**self.case,'fixtures':[{'generator':'mkfs.fat'}]*4}
        with self.assertRaisesRegex(res.Refusal,'too many IDE'):
            runner.qemu_args('fixture',self.profile,case,'12345678',self.image,self.firmware)
        with patch.object(FakeHost,'fat_fixture') as generate,self.assertRaisesRegex(res.Refusal,'too many IDE'):
            runner.prepare_fixtures(FakeHost(self.cgroup),case,self.root)
        generate.assert_not_called()

    def test_fat_read_suite_uses_kernel_disk_numbers(self):
        cases=runner.load(ROOT/'tests/suites/f1-fat32.json')['cases']
        for name,bits,disk in [('fat12-read',12,1),('fat16-read',16,1),('fat32-read',32,0)]:
            case=next(case for case in cases if case['id']==name)
            predicates=case['expected']['predicates']
            self.assertIn({'where':{'event':'DATA','case':'fixture','disk':1},'exact_count':1,
                           'fields':{'status':'present'}},predicates)
            self.assertIn({'where':{'event':'DATA','case':'mount','disk':disk},'exact_count':1,
                           'fields':{'type':{'eq':bits},'mode':'ro','writes':{'eq':0},'read_gate':{'eq':1}}},predicates)
    def test_legacy_loader_options_are_refused(self):
        for options in ({'safe':True},{}):
            case={**self.case,'loader_options':options}
            with self.assertRaisesRegex(res.Refusal,'loader_options'):
                runner.qemu_args('fixture',self.profile,case,'12345678',self.image,self.firmware)
    def test_not_run_is_retained_and_never_passes(self):
        result,directory=self.run_fake(records=[{'event':'NOT_RUN','reason':'prerequisite_failed','after':'bootinfo'}])
        self.assertEqual(result['outcome'],'not_run');self.assertIn('bootinfo',result['reason'])
        self.assertTrue((directory/'run.qcow2').exists());self.assertTrue(result['cleanup']['clean'])
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

    def test_blkdebug_read_builder_uses_guest_sectors_and_read_filter(self):
        case=runner.load(ROOT/'tests/suites/f1-storage.json')['cases'][-1]
        config=runner.blkdebug_config(case,self.root).read_text()
        self.assertIn('event = "none"',config)
        self.assertIn('iotype = "read"',config)
        self.assertIn('sector = "1048575"',config)
        self.assertIn('errno = "5"',config)
        self.assertIn('once = "on"',config)
        args,_=runner.qemu_args('fixture',self.profile,case,'12345678',self.root/'run.qcow2',self.firmware)
        drive=args[args.index('-drive')+1]
        graph=json.loads(drive.split(',format=',1)[0][10:].replace(',,',','))
        self.assertEqual(graph['driver'],'raw')
        self.assertEqual(graph['file']['driver'],'blkdebug')
        self.assertEqual(graph['file']['image']['driver'],'qcow2')
        self.assertIn('rerror=report',drive)
        for fault in ({'layer':'guest','event':'read_aio','sector':1,'errno':5},
                      {'layer':'host-block-backend','event':'read_aio','errno':5},
                      {'layer':'host-block-backend','event':'read_aio','sector':-1,'errno':5}):
            with self.subTest(fault=fault),self.assertRaises(res.Refusal):
                runner.blkdebug_config({**case,'fault':fault},self.root)

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

    def test_fd_table_checker_requires_arm_and_compares_the_guest_digest(self):
        import hashlib
        content=b'CiukiOS F2 durable\n';digest=hashlib.sha256(content).hexdigest()
        self.case.update(probe='fd-table',selector='f2:fd-table run={run_id}',
                         expected={'terminal':'END','predicates':[]})
        self.case['checks']={'offset':0,'size':len(self.image.read_bytes()),'listing_path':'::/tmp',
                             'listing_contains':['f2-durable.bin'],'files':[{'path':'::/tmp/f2-durable.bin','sha256':digest}]}
        self.case['digests']=[{'kind':'file','path':'/tmp/f2-durable.bin','offset':0,
                              'size':len(self.image.read_bytes()),'where':{'event':'DATA','case':'durable-file'}}]
        records=[{'event':'BEGIN'},{'event':'DATA','case':'durable-file','name_hex':'/tmp/f2-durable.bin'.encode().hex(),
                  'size':str(len(content)),'sha256':digest},
                 {'event':'ARM','action':'durable_shutdown'},{'event':'END','status':'PASS'}]
        order=[]
        @contextmanager
        def export(host,overlay,directory,offset,size):
            self.assertIsNotNone(host.process.poll());self.assertEqual(host.scope_pids(host.cgroup),[])
            order.append('export-readonly');yield self.image
        def check(host,args,directory):
            order.append(args[0]+(' -n' if '-n' in args else ''))
            return {'arguments':args,'returncode':0,'output':'f2-durable.bin','output_sha256':digest}
        def file_digest(host,image,directory,path):
            self.assertEqual(path,'/tmp/f2-durable.bin');return {'size':len(content),'sha256':digest}
        with patch.object(FakeHost,'export_readonly',export),patch.object(FakeHost,'checker',check), \
             patch.object(FakeHost,'file_digest',file_digest):
            result,_=self.run_fake(records=records)
            self.assertEqual(result['outcome'],'pass',result['reason'])
            self.assertLess(order.index('export-readonly'),order.index('fsck.fat -n'))
            self.assertIn('mtype',order);self.assertEqual(result['digests'][0]['measured']['sha256'],digest)
            self.assertTrue(result['durability_observations'][-1]['checker_passed'])
            result,_=self.run_fake(records=[r for r in records if r['event']!='ARM'])
            self.assertEqual(result['outcome'],'fail');self.assertIn('ARM',result['reason'])
            changed=[{**r,'sha256':'0'*64} if r.get('case')=='durable-file' else r for r in records]
            result,_=self.run_fake(records=changed)
            self.assertEqual(result['outcome'],'fail');self.assertIn('digest mismatch',result['reason'])
        self.case.pop('checks');self.case.pop('digests')
        result,_=self.run_fake(records=records)
        self.assertEqual(result['outcome'],'fail');self.assertIn('declarations',result['reason'])

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
        path='/Ciuki long fixture.txt'
        self.case['digests']=[{'kind':'file','source':'fixture','fixture':0,'path':path,
                              'where':{'event':'DATA','name_hex':path.encode().hex()}}]
        fixture_image=self.root/'fixture.img';fixture_image.write_bytes(b'fixture')
        fixture={'sha256':runner.sha(fixture_image),'path':str(fixture_image)}
        measured={'size':8,'sha256':'a'*64}
        records=[{'event':'BEGIN'},{'event':'DATA','tick':'10000','name_hex':path.encode().hex(),
                  'size':'8','sha256':'a'*64},{'event':'END','status':'PASS'}]
        with patch.object(FakeHost,'fat_fixture',return_value=fixture),patch.object(FakeHost,'file_digest',return_value=measured):
            result,_=self.run_fake(records)
            self.assertEqual(result['outcome'],'pass',result['reason'])
            self.assertFalse(any('/fixture,' in arg for arg in result['qemu']['arguments']))
            self.assertEqual(result['digests'][0]['measured'],measured)
            records[1]['sha256']='b'*64
            result,_=self.run_fake(records)
        self.assertEqual(result['outcome'],'fail');self.assertIn('guest digest mismatch',result['reason'])
        self.assertEqual(result['fixtures']['manifest'][0]['sha256'],fixture['sha256'])
        self.assertEqual(result['fixtures']['manifest'][0]['ide_index'],1)

    def test_chunked_utf8_names_join_real_file_digest_records(self):
        self.case['fixtures']=[{'generator':'mkfs.fat','fat_type':16,'seed':1}]
        self.case['expected']={'terminal':'END','predicates':[
            {'where':{'event':'DATA','case':'file'},'exact_count':1,'required_fields':['size','sha256']}]}
        path='/Ciuki '+('long '*9)+'caf\u00e9.txt'
        name=('D:'+path).encode()
        frames=[{'event':'DATA','case':'name','id':'7','field':'path','offset':str(i),
                 'bytes':str(len(name[i:i+40])),'hex':name[i:i+40].hex()} for i in range(0,len(name),40)]
        records=[{'event':'BEGIN'},*frames,{'event':'DATA','case':'file','id':'7','tick':'10000',
                 'size':'8','sha256':'a'*64},{'event':'END','status':'PASS'}]
        self.case['digests']=[{'kind':'file','source':'fixture','fixture':0,'drive':'D','path':path,
                              'where':{'event':'DATA','case':'file'}}]
        image=self.root/'fixture.img';image.write_bytes(b'fixture')
        fixture={'path':str(image),'sha256':runner.sha(image)}
        with patch.object(FakeHost,'fat_fixture',return_value=fixture), \
             patch.object(FakeHost,'file_digest',return_value={'size':8,'sha256':'a'*64}) as measured:
            result,_=self.run_fake(records)
            self.assertEqual(result['outcome'],'pass',result['reason'])
            self.assertEqual(measured.call_args.args[-1],path)
            for field,value in [('offset','1'),('bytes','1'),('hex','ff')]:
                broken=copy.deepcopy(records);broken[1][field]=value
                result,_=self.run_fake(broken)
                with self.subTest(field=field):
                    self.assertEqual(result['outcome'],'fail')
                    self.assertIn('invalid file name chunks',result['reason'])
            broken=copy.deepcopy(records);broken[-2]['size']='9'
            result,_=self.run_fake(broken)
            self.assertEqual(result['outcome'],'fail');self.assertIn('guest digest mismatch',result['reason'])
            for broken in (records[:1]+records[2:],records[:2]+[records[1]]+records[2:]):
                result,_=self.run_fake(broken)
                self.assertEqual(result['outcome'],'fail');self.assertIn('name chunks',result['reason'])

    def test_sector_digests_require_stopped_guest_and_exact_measurement(self):
        self.case['digests']=[{'kind':'sector','lba':0,'where':{'event':'DATA','lba':'0'}}]
        records=[{'event':'BEGIN'},{'event':'DATA','tick':'10000','lba':'0','sha256':'a'*64},
                 {'event':'END','status':'PASS'}]
        def measured(host,image,directory,lba,count,fmt):
            self.assertTrue((directory/'fake-stopped').exists())
            self.assertEqual((lba,count,fmt),(0,1,'qcow2'))
            return {'size':512,'sha256':'a'*64}
        with patch.object(FakeHost,'sector_digest',measured):
            result,_=self.run_fake(records)
            self.assertEqual(result['outcome'],'pass',result['reason'])
            records[1]['sha256']='b'*64
            result,_=self.run_fake(records)
            self.assertEqual(result['outcome'],'fail');self.assertIn('digest mismatch',result['reason'])
            records[1]['sha256']='a'*64;records.insert(2,dict(records[1]))
            result,_=self.run_fake(records)
            self.assertEqual(result['outcome'],'fail');self.assertIn('duplicate digest',result['reason'])

    def test_real_readonly_sector_export_file_digest_and_blkdebug(self):
        if not all(shutil.which(tool) for tool in ('qemu-img','qemu-io','mkfs.fat','mcopy','mtype')):
            self.skipTest('qemu image/FAT host tools unavailable')
        image=self.root/'digest.img'
        with image.open('wb') as stream:stream.truncate(4*1024**2)
        subprocess.run(['mkfs.fat','--invariant','-F','12',str(image)],check=True,capture_output=True)
        content=self.root/'binary.bin';content.write_bytes(b'\x00\xffCiuki\r\n'*64)
        subprocess.run(['mcopy','-i',str(image),str(content),'::/binary.bin'],check=True,capture_output=True)
        before=runner.sha(image);host=runner.Host()
        self.assertEqual(host.file_digest(image,self.root,'/binary.bin'),
                         {'size':content.stat().st_size,'sha256':runner.sha(content)})
        with image.open('rb') as stream:stream.seek(512);sector=stream.read(512)
        self.assertEqual(host.sector_digest(image,self.root,1,1,'raw')['sha256'],runner.hashlib.sha256(sector).hexdigest())
        overlay=self.root/'digest.qcow2'
        subprocess.run(['qemu-img','create','-f','qcow2','-b',str(image),'-F','raw',str(overlay)],check=True,capture_output=True)
        payload=self.root/'sector.bin';payload.write_bytes(b'X'*512)
        host.overlay_write(overlay,512,payload,512)
        self.assertEqual(host.sector_digest(overlay,self.root,1,1,'qcow2')['sha256'],runner.sha(payload))
        self.assertEqual(runner.sha(image),before)
        with self.assertRaisesRegex(res.Refusal,'outside image'):host.sector_digest(image,self.root,100000,1,'raw')
        self.assertFalse((self.root/'digest-file.bin').exists());self.assertFalse((self.root/'digest-sectors.raw').exists())
        case={**self.case,'fault':{'layer':'host-block-backend','event':'read_aio','sector':1,'errno':5}}
        runner.blkdebug_config(case,self.root)
        args,_=runner.qemu_args('fake',self.profile,case,'12345678',overlay,self.firmware)
        drive=args[args.index('-drive')+1]
        disk=drive.split(',format=',1)[0][5:].replace(',,',',')
        checked=subprocess.run(['qemu-io','-r','-f','raw','-c','read 0 512','-c','read 512 512',disk],capture_output=True,text=True)
        self.assertNotEqual(checked.returncode,0)
        self.assertIn('Input/output error',checked.stdout+checked.stderr)
        self.assertIn('read 512/512 bytes',checked.stdout+checked.stderr)
        suite=runner.load(ROOT/'tests/suites/f1-storage.json')
        self.assertEqual([c['id'] for c in suite['cases'] if c['id'].startswith('ata-fault')],
                         ['ata-fault','ata-fault-blkdebug'])

    def test_kernel_map_f1probes_within_rodata(self):
        path=ROOT/'build/f0/VMM.map'
        if not path.exists():self.skipTest('kernel map checked after the mandatory kernel build')
        rows=path.read_text().splitlines()
        # lld map lines include "symbol = ." as three separate tokens.
        def address(symbol):
            return int(next(line.split()[0] for line in rows if symbol+' = .' in line),16)
        start=address('__f1probes_start');end=address('__f1probes_end')
        self.assertLessEqual(address('__rodata_start'),start)
        self.assertLessEqual(start,end)
        self.assertLessEqual(end,address('__rodata_end'))
        self.assertEqual(linked_probe_names(rows,1),[
            'registry','input','input-fault','framebuffer','ata','ata-fault','partition',
            'fat-read','fat-write','cache','mount-crash','safe','bootlog'])
        registrations=[line.split()[-1] for line in rows if ' f1probe_' in line]
        self.assertEqual(registrations,[
            'f1probe_probe_registry','f1probe_probe_input','f1probe_probe_input_fault',
            'f1probe_probe_framebuffer','f1probe_probe_ata','f1probe_probe_ata_fault',
            'f1probe_probe_partition','f1probe_probe_fat_read','f1probe_probe_fat_write',
            'f1probe_probe_cache','f1probe_probe_mount_crash','f1probe_probe_safe','f1probe_probe_bootlog'])
        self.assertEqual(end-start,8*len(registrations))
        sections=[line for line in rows if ':(.f1probes)' in line]
        self.assertTrue(sections)
        for line in sections:
            columns=line.split();vma=int(columns[0],16);size=int(columns[2],16)
            self.assertGreaterEqual(vma,start);self.assertLessEqual(vma+size,end)

    def test_console_presenter_heap_region_arbitration(self):
        if not shutil.which('clang'):self.skipTest('host clang unavailable')
        harness=self.root/'console_arbitration.c'
        harness.write_text(r'''
#define main framebuffer_fixture_main
#include "tests/host/fbdev_test.c"
#undef main
#undef GUARD
#define CIUKI_CPU_H
#define P2V(p) ((void *)(uintptr_t)(p))
static unsigned host_if = 0x200, irq_sections;
static uint32_t irq_save(void) { unsigned old=host_if; host_if=0; irq_sections++; return old; }
static void irq_restore(uint32_t flags) { host_if=flags; }
const uint8_t font_cfn_regular[95+95*32] = { [95+('a'-32)*32+1] = 0x80 };
static void *console_map(uint32_t phys,uint32_t size,bool uncached)
{
    CHECK(phys==g_boot.fb_phys && size==g_boot.fb_pitch*g_boot.fb_height && uncached);
    return fake_lfb;
}
#define vmm_map_mmio console_map
#define pack console_pack
#include "src/kernel/core/console.c"
#undef pack
#undef vmm_map_mmio
int main(void)
{
    enum { PITCH=80, HEIGHT=35, SIZE=PITCH*HEIGHT, GUARD=32 };
    uint8_t *raw=malloc(SIZE+2*GUARD), *saved=malloc(SIZE);
    CHECK(raw && saved);
    memset(raw,0xA5,SIZE+2*GUARD);
    fake_lfb=raw+GUARD;
    boot_fixture(true);
    g_boot.fb_height=HEIGHT;g_boot.fb_pitch=PITCH;
    g_boot.vbe_mode_info[20]=HEIGHT;g_boot.vbe_mode_info[50]=PITCH;
    reservation.end=g_boot.fb_phys+SIZE;
    CHECK(fbdev_init()==0 && console_init_lfb());
    CHECK(console_rows==32 && !drawing);
    struct fb_rect owned={0,0,17,32}, below={0,32,17,3}, crossing={0,31,17,4};
    uint32_t source[17*35];
    for (unsigned i=0;i<17*35;i++) source[i]=0x123456;
    struct fb_surface surface={source,17,35,17*4,sizeof(source)};
    uint32_t flags=line_begin();
    CHECK(host_if==0 && drawing==CONSOLE_BUSY);
    memcpy(saved,fake_lfb,SIZE);
    CHECK(fbdev_fill(&owned,0xFFFFFF)==-EBUSY);
    CHECK(fbdev_present(&surface,&crossing)==-EBUSY);
    CHECK(!memcmp(saved,fake_lfb,SIZE));
    CHECK(fbdev_fill(&below,0xFFFFFF)==0);
    CHECK(!memcmp(saved,fake_lfb,32*PITCH));
    CHECK(fbdev_present(&surface,&below)==0);
    CHECK(!memcmp(saved,fake_lfb,32*PITCH));
    CHECK(drawing==CONSOLE_BUSY);
    line_end(flags);
    CHECK(host_if==0x200 && !drawing);
    CHECK(fbdev_fill(&owned,0x010203)==0 && !drawing);
    CHECK(fbdev_present(&surface,&owned)==0 && !drawing);
    memcpy(saved,fake_lfb,SIZE);
    /* Model the IRQ/panic preemption point after production present_begin. */
    CHECK(present_begin(&device,&owned) && drawing==PRESENT_BUSY);
    CHECK(!fbdev_console_begin());
    console_write("a\n",2);
    console_show_page(0,"a");
    CHECK(!memcmp(saved,fake_lfb,SIZE));
    CHECK(hist_count==1 && !strcmp(hist[0],"a") && drawing==PRESENT_BUSY);
    present_end(&device,&owned);
    console_show_page(0,"a");
    CHECK(memcmp(saved,fake_lfb,32*PITCH)!=0 && !drawing);
    CHECK(!memcmp(saved+32*PITCH,fake_lfb+32*PITCH,3*PITCH));
    /* Invalid and empty operations must neither leave a claim nor write. */
    struct fb_rect invalid={INT32_MAX,0,1,1}, empty={0,0,0,1};
    CHECK(fbdev_fill(&invalid,0)==-EINVAL && !drawing);
    CHECK(fbdev_present(&surface,&empty)==0 && !drawing);
    host_if=0;console_write("a",1);CHECK(host_if==0 && !drawing);
    CHECK(irq_sections>0 && locks==0);
    for (unsigned i=0;i<GUARD;i++) CHECK(raw[i]==0xA5 && raw[GUARD+SIZE+i]==0xA5);
    free(saved);free(raw);
    printf("console/presenter arbitration: %u failures\n",failures);
    return failures ? 1 : 0;
}
''')
        binary=self.root/'console_arbitration'
        checked=subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                                '-fsanitize=address,undefined','-I',str(ROOT),
                                '-I',str(ROOT/'src/kernel/include'),str(harness),'-o',str(binary)],
                               capture_output=True,text=True)
        self.assertEqual(checked.returncode,0,checked.stderr)
        checked=subprocess.run([str(binary)],capture_output=True,text=True,
                               env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
        self.assertEqual(checked.returncode,0,checked.stdout+checked.stderr)

    def test_missing_subcase_reaches_runner_as_not_run(self):
        self.case.update(probe='input-fault',selector='f1:input-fault run={run_id}',expected={'terminal':'END',
                             'not_run_subcases':[{'subcase':'firmware_overrun','reason':'missing_emitter'}]})
        result,_=self.run_fake(records=[{'event':'BEGIN'},{'event':'END','status':'PASS'}])
        self.assertEqual(result['outcome'],'not_run')
        self.assertEqual(result['not_run_subcases'],self.case['expected']['not_run_subcases'])

    def test_operator_confirmation_follows_probe_subcase_across_profiles(self):
        case={'id':'renamed-import','probe':'panic','profile':'future-profile',
              'evidence_sink':'screen','device_exceptions':{'serial':'none'}}
        self.assertTrue(runner.operator_confirmation_case(case))
        self.assertFalse(runner.failed_prerequisite(case,{'outcome':'fail','operator_confirmation':True}))
        self.assertTrue(runner.failed_prerequisite(case,{'outcome':'fail','operator_confirmation':False}))
        for altered in ({'probe':'boot'},{'evidence_sink':'serial'},{'device_exceptions':{}}):
            self.assertFalse(runner.operator_confirmation_case({**case,**altered}))

    def test_production_kernel_selector_names_and_phase_dispatch(self):
        if not shutil.which('clang'):self.skipTest('host clang unavailable')
        harness=self.root/'selector_harness.c'
        harness.write_text("""
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ciuki/process.h>
#include "selector.h"
static char evidence[4096];
static int calls0, calls1, calls2, failing;
static int f0(void) { calls0++; return failing; }
static int f1(void) { calls1++; return failing; }
static int f2(void) { calls2++; return failing; }
static void panic_probe(void) { calls0++; }
static void app_begin(const struct probe_selection *s) { (void)s; }
static void app_end(void) {}
void klog(const char *fmt,...) { (void)fmt; }
void rec_emit(const char *p,const char *event,const char *fmt,...) {
    unsigned n=(unsigned)strlen(evidence);
    n+=(unsigned)snprintf(evidence+n,sizeof(evidence)-n,"%s %s ",p,event);
    if (fmt) { va_list ap; va_start(ap,fmt); vsnprintf(evidence+n,sizeof(evidence)-n,fmt,ap); va_end(ap); }
    strcat(evidence,"\\n");
}
static const struct probe_def fixture_f0[] = {
    {"boot",f0},{"bootinfo",f0},{"allocator",f0},{"protection",f0},{"isolation",f0},
    {"preempt",f0},{"localfault",f0},{"syslife",f0},{"fpu",f0}
};
static const struct probe_def fixture_f1[] = {{"input",f1},{"framebuffer",f1}};
static const struct ciuki_f2_probe fixture_f2[] = {{"elf-load",f2},{"spawn-wait",f2}};
int validate(const char *s,unsigned len,unsigned flags) {
    struct probe_selection selection;
    evidence[0]=0;
    return probes_parse_selector(s,len,flags,&selection) ? (int)selection.phase+1 : 0;
}
int dispatch(const char *s,unsigned flags,unsigned count,int fail) {
    calls0=calls1=calls2=0;evidence[0]=0;failing=fail;
    struct probe_selection selection;
    if (probes_parse_selector(s,(unsigned)strlen(s),flags,&selection)) {
        const struct probe_tables tables = { fixture_f0, fixture_f1, 9, count, fixture_f2, 2 };
        const struct probe_hooks hooks = { app_begin, app_end, panic_probe };
        probes_dispatch(&selection,&tables,&hooks);
    }
    return calls0*100+calls1+calls2*10000;
}
const char *records(void) { return evidence; }
""")
        library=self.root/'selector.so'
        compiled=subprocess.run(['clang','-shared','-fPIC','-std=c17','-Wall','-Werror',
                                '-I',str(ROOT/'src/kernel/include'),'-I',str(ROOT/'src/kernel/probes'),
                                str(ROOT/'src/kernel/probes/selector.c'),str(harness),'-o',str(library)],
                                capture_output=True,text=True)
        self.assertEqual(compiled.returncode,0,compiled.stderr)
        native=ctypes.CDLL(str(library));native.validate.argtypes=[ctypes.c_char_p,ctypes.c_uint,ctypes.c_uint]
        native.dispatch.argtypes=[ctypes.c_char_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_int]
        native.records.restype=ctypes.c_char_p
        # Boot flags use the real shared definitions (safe=bit 0, QEMU=bit 5, forced=bit 7).
        trusted=1|32|128
        for phase,names in ((0,F0_PROBES),(1,F1_PROBES),(2,F2_PROBES)):
            for name in (*names, *(('all','core') if phase != 2 else ())):
                text=(f'f{phase}:'+name+' run=12ab34cd').encode()
                self.assertEqual(native.validate(text,len(text),0),phase+1)
                self.assertEqual(native.records(),b'')
                suffix=text+b' platform=e500 safe=1'
                self.assertEqual(native.validate(suffix,len(suffix),trusted),phase+1)
                self.assertEqual(native.records(),b'')
        for text in (b'f1:unknown run=12ab34cd',b'f1:input run=12ab34cd run=12345678',
                     b'f1:input run=12ab34cd safe=1 safe=1',b'f1:input run=12ab34cd '+b'x'*64,
                     b'f0:input run=12ab34cd',b'f1:boot run=12ab34cd',
                     b'f1:input run=12ab34cd\0',b'f1:input run=12ab34cd\n',b'f1:input run=12ab34cd\x7f'):
            self.assertEqual(native.validate(text,len(text),trusted),0)
            self.assertEqual(native.records(),b'')
        for suffix,required in ((b' safe=1',1),(b' platform=e500',128)):
            text=b'f1:input run=12ab34cd'+suffix
            for flags in (0,32,required):self.assertEqual(native.validate(text,len(text),flags),0)
            self.assertEqual(native.validate(text,len(text),32|required),2)
        for alias in ('all','core'):
            self.assertEqual(native.dispatch(f'f1:{alias} run=12ab34cd'.encode(),0,2,0),2)
            self.assertEqual(native.dispatch(f'f1:{alias} run=12ab34cd'.encode(),0,2,1),1)
            self.assertIn(b'framebuffer NOT_RUN reason=prerequisite_failed after=input',native.records())
        self.assertEqual(native.dispatch(b'f0:core run=12ab34cd',0,2,0),900)
        self.assertEqual(native.dispatch(b'f0:all run=12ab34cd',0,2,0),1000)
        self.assertEqual(native.dispatch(b'f0:all run=12ab34cd',0,2,1),100)
        self.assertIn(b'panic NOT_RUN',native.records())
        self.assertEqual(native.dispatch(b'f1:input run=12ab34cd',0,2,0),1)
        self.assertEqual(native.dispatch(b'f1:ata run=12ab34cd',0,2,0),0)
        self.assertIn(b'ata READY table=f1 installed=2',native.records())
        self.assertIn(b'ata ERROR status=not_run reason=missing_probe',native.records())
        for name in (*F1_PROBES,'all','core'):
            self.assertEqual(native.dispatch(f'f1:{name} run=12ab34cd'.encode(),0,0,0),0)
            self.assertIn(b'installed=0',native.records())
            self.assertIn(b'ERROR status=not_run reason=missing_probe',native.records())
        for name in ('all','core','input','boot','unknown'):
            text=f'f2:{name} run=12ab34cd'.encode()
            self.assertEqual(native.validate(text,len(text),trusted),0)
            self.assertEqual(native.dispatch(text,0,2,0),0)
        self.assertEqual(native.dispatch(b'f2:elf-load run=12ab34cd',0,2,0),10000)
        self.assertEqual(native.dispatch(b'f2:fd-table run=12ab34cd',0,2,0),0)
        self.assertIn(b'fd-table READY table=f2 installed=2',native.records())
        self.assertIn(b'fd-table ERROR status=not_run reason=missing_probe',native.records())

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


    def test_f2_missing_probe_and_result_fields(self):
        self.case.update(probe='fd-table',selector='f2:fd-table run={run_id}',expected={'terminal':'END'})
        result,_=self.run_fake(records=[{'event':'BEGIN'},{'event':'READY','table':'f2','installed':'5'},
                                     {'event':'ERROR','status':'not_run','reason':'missing_probe'}])
        self.assertEqual(result['outcome'],'not_run');self.assertTrue(result['cleanup']['clean'])
        self.assertEqual(result['abi_version'],'unknown');self.assertIn('sdk_manifest_sha256',result['missing_f2_fields'])

    def test_f2_application_flood_preserves_terminal_and_controller_log(self):
        self.case.update(probe='elf-load',selector='f2:elf-load run={run_id}',timeout=30,expected={'terminal':'END'})
        result,directory=self.run_fake(records=[{'event':'BEGIN'}],application_bytes=res.LOG_CAP+128,
                                      after_application=[{'event':'DATA','abi_version':'1'},{'event':'END','status':'PASS'}])
        self.assertEqual(result['outcome'],'pass',result['reason'])
        output=result['application_output'];self.assertTrue(output['capture_truncated'])
        self.assertEqual(output['total_bytes'],res.LOG_CAP+128)
        self.assertLessEqual(len(output['head_hex'])+len(output['tail_hex']),4*65536)
        self.assertLess((directory/'serial.log').stat().st_size,res.LOG_CAP)
        self.assertIn(b'event=END status=PASS',(directory/'serial.log').read_bytes())
        pattern=b'CIUKI_TEST event=END status=PASS\n'[:24]
        data=pattern*((res.LOG_CAP+128)//24)+pattern[:(res.LOG_CAP+128)%24]
        self.assertEqual(output['sha256'],hashlib.sha256(data).hexdigest())
        self.assertEqual(result['abi_version'],'1')

    def test_operator_screen_confirmation_is_fail_and_does_not_block(self):
        self.case.update(evidence_sink='screen',operator_confirmation=True,screen_capture_after=.2)
        result,directory=self.run_fake(records=[])
        self.assertEqual(result['outcome'],'fail');self.assertTrue(result['operator_confirmation'])
        self.assertTrue((directory/'screen.ppm').exists())
        self.assertFalse(runner.failed_prerequisite(self.case,result))
        result,_=self.run_fake(records=[{'event':'BEGIN'},{'event':'END','status':'FAIL'}])
        self.assertFalse(result['operator_confirmation']);self.assertTrue(runner.failed_prerequisite(self.case,result))

    def test_main_continues_after_operator_confirmation_but_stops_on_real_failure(self):
        lock=self.root/'common.lock'
        (self.root/'config').mkdir();(self.root/'config/toolchain.json').write_text(json.dumps({'qemu_machine':self.profile['machine']}))
        cases=[{**self.case,'id':name,'profile':'qemu-t23','operator_confirmation':name=='screen'} for name in ('screen','real-fail','unattempted')]
        calls=[]
        def boot(root,suite,case,*args,**kwargs):
            calls.append(case['id']);directory=self.root/'build/test-runs'/suite/case['id'];directory.mkdir(parents=True)
            return {'run_id':case['id'],'outcome':'fail','reason':'fixture','operator_confirmation':case['id']=='screen'},directory
        with patch.object(runner,'ROOT',self.root),patch.object(runner,'load_suite',return_value={'image':'full','cases':cases}), \
             patch.object(runner,'load',return_value=self.profile),patch.object(res,'common_lock',return_value=lock), \
             patch.object(runner.Host,'preflight',return_value=3*res.GIB),patch.object(runner,'run_case',side_effect=boot), \
             patch.object(runner.shutil,'which',return_value='fake'),patch.object(runner,'find_firmware',return_value=self.firmware), \
             patch('sys.stdout',new_callable=io.StringIO):
            self.assertEqual(runner.main(['fixture','--image',str(self.image)]),1)
        self.assertEqual(calls,['screen','real-fail'])
        summary=json.loads((self.root/'build/test-runs/fixture/summary.json').read_text())
        self.assertEqual([r['outcome'] for r in summary['cases']],['fail','fail','not_run'])


class F1SelectorTests(unittest.TestCase):
    def test_boot_cfg_whole_tokens(self):
        self.assertEqual(boot_options('safe=1 serial=0 mode=0x0118\r\n'),
                         {'safe':True,'serial':False,'mode':0x118})
        self.assertTrue(boot_options('safe=0',safe=True)['safe'])
        self.assertTrue(boot_options('safe=1 safe=0')['safe'])
        self.assertEqual(boot_options('  safe=0\nserial=1 '),
                         {'safe':False,'serial':True,'mode':None})
        for text in ('safe=1junk','safe=10','safe=2','serial=0junk','mode=0x0118junk',
                     'mode=0x4000','mode=0x00xz','unknown=1','safe=1\tserial=0',
                     'safe=1\0','x'*128,'sáfe=1'):
            with self.subTest(text=text),self.assertRaisesRegex(ValueError,'SELECT_ERROR'):
                boot_options(text)

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
    def test_not_run_is_a_standalone_terminal(self):
        parser=Parser('12345678','allocator')
        parser.feed(b'CIUKI_TEST v=1 run=12345678 seq=000010 probe=allocator event=NOT_RUN reason=prerequisite_failed after=bootinfo')
        self.assertEqual(parser.outcome,'not_run')
        with self.assertRaisesRegex(EvidenceError,'not_run'):parser.check({'terminal':'END'})
        with self.assertRaisesRegex(EvidenceError,'not_run'):parser.check({'terminal':'NOT_RUN'})
        with self.assertRaises(EvidenceError):
            parser.feed(b'CIUKI_TEST v=1 run=12345678 seq=000011 probe=allocator event=END status=PASS')
        for suffix in ('reason=other after=bootinfo','reason=prerequisite_failed','reason=prerequisite_failed after=unknown'):
            with self.subTest(suffix=suffix),self.assertRaises(EvidenceError):
                Parser('12345678','allocator').feed(('CIUKI_TEST v=1 run=12345678 seq=000010 probe=allocator event=NOT_RUN '+suffix).encode())
        parser=Parser('12345678','allocator')
        parser.feed(b'CIUKI_TEST v=1 run=12345678 seq=000001 probe=allocator event=BEGIN')
        with self.assertRaises(EvidenceError):
            parser.feed(b'CIUKI_TEST v=1 run=12345678 seq=000002 probe=allocator event=NOT_RUN reason=prerequisite_failed after=bootinfo')

    def test_suite_timer_predicates_reject_fabricated_progress(self):
        for path in sorted((ROOT/'tests/suites').glob('f0-*.json')):
            for case in json.loads(path.read_text())['cases']:
                for predicate in case['expected'].get('predicates',[]):
                    fields=predicate.get('fields',{})
                    if not {'ready_tick','final_tick','elapsed_pit_cycles'} <= fields.keys():continue
                    # Populate the actual suite predicate's other fields with
                    # valid values, so only its timing checks determine success.
                    data={**predicate['where']}
                    for field,rule in fields.items():
                        value=rule.get('eq',rule.get('ge',0)) if isinstance(rule,dict) else rule
                        if isinstance(value,str) and value.startswith('$'):value=0
                        data[field]=f'{value:08x}' if isinstance(rule,dict) and rule.get('encoding')=='hex' else str(value)
                    for ready,final,cycles,passes in ((0,0,11931820,False),(17,10026,10009*1193,False),
                                                    (17,10027,10010*1193+1,False),(17,10027,10010*1193,True)):
                        with self.subTest(suite=path.name,case=case['id'],ticks=(ready,final),cycles=cycles):
                            parser=Parser('12345678','boot')
                            data.update(ready_tick=str(ready),final_tick=str(final),elapsed_pit_cycles=str(cycles))
                            records=['event=BEGIN']
                            records.extend(f'event=DATA group=boot {k}={v}' for k,v in data.items() if k not in ('event','group'))
                            records.append('event=END status=PASS')
                            for seq,record in enumerate(records,1):
                                parser.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=boot {record}'.encode())
                            expected={'terminal':'END','predicates':[predicate]}
                            if passes:self.assertTrue(parser.check(expected))
                            else:
                                with self.assertRaises(EvidenceError):parser.check(expected)

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

class F1RecordTests(unittest.TestCase):
    """Fabricated successful records in the production probes' actual formats."""
    def records(self, probe, bodies, terminal='END', status='PASS'):
        parser=Parser('12345678',probe)
        records=['event=BEGIN',*[b if b.startswith('event=') else 'event=DATA '+b for b in bodies]]
        records.append('event='+terminal+' status='+status)
        for seq,record in enumerate(records,1):
            parser.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe={probe} {record}'.encode())
        return parser

    def fixtures(self):
        registry=[
            'case=ledger cycles=100 live_before=12 live_after=12 claims=100 releases=100 device_writes=0 mappings=0 buffers=0 ok=1',
            'case=resources buffers_allocated=100 buffers_freed=100 mappings=0 callbacks_pending=0 states=claimed,active,quiescing,released',
            'case=quarantine owner=registry-quarantine other_owner=registry-retry generation=101 idle=-14 release=-22 retry=-22 retained=1 device_writes=0',
            'case=shared_irq irq=11 boundary=production_shadow frame=synthetic physical_unmasks=0 device_writes=0 callbacks_a=2 callbacks_b=1003',
            'case=stuck_irq owner=fixture-b generation=1 passes=1003 unclaimed=1000 quarantined=1 eois=1003 removed_a=1 masked=1 ok=1',
            'group=registry cycles=100 claim_delta=0 mapping_delta=0 buffer_delta=0 conflict_writes=0 stale_writes=0 unknown_size_writes=0',
            'group=registry quarantine_retained=1 second_claim_refused=1 irq_owner_errors=0',
            'group=metadata subcase=lifecycle owner=registry-scratch generation=100 errors=0 gate=registry timing_domain=icount']
        faults=[]
        for name,error,elapsed,resends,quarantine in [('missing_ack',-110,200,0,1),('bounded_resend',0,2,2,0),('resend_exhausted',-71,2,2,1),('mixed_aux_key',0,0,0,0)]:
            faults.extend([f'case={name} owner=fixture generation=1 error={error} elapsed_ms={elapsed} resends={resends} quarantined={quarantine} pending=0 resets=0 ok=1',
                           f'case={name} timing=scripted_ms controller_reads=7 controller_writes=3 physical_claims=0 keys={2 if name=="mixed_aux_key" else 0} x={2 if name=="mixed_aux_key" else 0} y={-1 if name=="mixed_aux_key" else 0}'])
        faults.extend(['case=malformed_packet resync=3 x=2 y=-1 ok=1',
                       'case=queue_overflow overflow=256 drained=135 state_lost=1 resync_marked=1 fresh=1 ok=1'])
        for name in ('missing_ack','bounded_resend','resend_exhausted','mixed_aux_key','malformed_packet','queue_overflow'):
            faults.append(f'case={name} survivor_ticks=110 survivor_progress=200 survivor_ok=1')
        inputs={}
        for backend in ('native','firmware'):
            inputs[backend]=[
                f'case=counts backend={backend} generation=1 characters=100 key_transitions=200 button_transitions=20',
                'case=motion x=200 y=-100 digest=12345678 text_digest=12345678 events=420',
                'case=queue overflow=0 resync=0 duplicates=0 repeats=0 errors=0 stuck_keys=0 buttons=0 state_lost=0',
                f'group=input backend={backend} text_count=100 key_transitions=200 button_transitions=20 motion_x=200 motion_y=-100',
                'group=input loss=0 duplicates=0 stuck=0 owner_errors=0',
                f'group=metadata subcase=stimulus owner={"firmware-input" if backend=="firmware" else "i8042"} generation=1 errors=0 gate=input timing_domain=icount',
                f'case=lease backend={backend} generation=1 retained=1 active=1 quarantined=0']
            if backend=='firmware':inputs[backend].append('case=lease backend=firmware persistent=1 key_releases=1 disabled=0 scan_bytes=200 aux_bytes=360')
        framebuffer=[f'group=fixture bpp={bpp} pitch={pitch} digest=12345678 reference=12345678 guard_errors=0 errors=0'
                     for bpp,pitch in ((24,51),(24,58),(32,68),(32,75))]
        framebuffer.extend(['group=mode mode=0118 width=1024 height=768 bpp=32 pitch=4096 absent=0 owner=fbdev generation=1',
                            'group=masks red=8:16 green=8:8 blue=8:0 reserved=8:24',
                            'digest=12345678 guard_errors=0 errors=0 mode_calls=0 error=0 absent=0'])
        ata=['owner=ata0 generation=1 unit=0 model=QEMU identified=1',
             'capacity=1048576 sector_size=512 lba48=0 cache_state=1 flush=1 bios_calls=0 clock=pit',
             'lba=0 sha256='+'a'*64,'lba=2048 sha256='+'b'*64,
             'range_result=-22 zero_result=-22 overflow_result=-22 boundary_commands=0']
        ata_fault=[]
        for name in ('err','df','bsy_stuck','drq_stuck','missing','identify','flush'):
            missing=name=='missing'
            ata_fault.extend([f'case={name} owner=fake-ata0 generation=0 result={0 if missing else -5} issued={0 if missing else 1} status=41 error=04 quarantined={0 if missing else 1}',
                              f'case={name} elapsed_ms=0 deadline_ms={60000 if name=="flush" else 30000} next_result={0 if missing else -200} further_commands=0 accepted=1 clock=scaled_pit'])
        ata_fault.append('real_commands=0 bios_calls=0 survivor_samples=150 survivor_progress=300 survivor_alive=1')
        partition=['source=real owner=ata0 generation=1 result=0 count=1 walk_count=0 clock=pit',
                   'source=real partition=1 type=0c start=2048 length=1046528 logical=0']
        for name,error in [('primary_extended',0),('loop',-40),('overflow',-22),('overlap',-22),('protective_gpt',-95),('signature',-22),('out_of_range',-22)]:
            partition.extend([f'fixture={name} owner=fixture generation=0 sha256='+'c'*64,
                              f'fixture={name} result={error} expected={error} walk_count={2 if not error else 0} reads=3 outside=0 accepted={int(not error)} matched=1'])
        return {'registry':registry,'input-fault':faults,'input-qemu-t23':inputs['native'],
                'input-qemu-e500':inputs['firmware'],'framebuffer':framebuffer,
                'ata':ata+['event=ARM action=read_fault lba=1048575','case=tail lba=1048575 result=0'],
                'ata-fault':ata_fault,'partition':partition,
                **self.fat_fixtures(),
                'ata-fault-blkdebug':ata+[
                    'event=ARM action=read_fault lba=1048575',
                    'group=ata-fault-blkdebug lba=1048575 result=-5 issued=1 status=41 error=04 elapsed_ms=0 deadline_ms=30000',
                    'group=ata-fault-blkdebug next_result=-200 subsequent_commands=0 quarantine_retained=1 bios_calls=0',
                    'case=retained owner=ata0 generation=1 claims=3',
                    'group=ata-fault-blkdebug survivor_ticks=150 survivor_progress=300 survivor_alive=1 eio=1 deadline_met=1']}

    def fat_fixtures(self):
        # Formats captured from production probes through test_storage.c.
        meta='group=metadata subcase=complete owner=vfs generation=1 errors=0 gate=complete timing_domain=icount'
        result={}
        for bits in (12,16,32):
            drive='C' if bits==32 else 'D'
            result[f'fat{bits}-read']=[
                f'group=fat-read drive={drive} fat_type={bits} name_errors=0 alias_errors=0 size_errors=0 write_count=0 chain_bounded=1',
                f'case=list drive={drive} entries=5 sha256='+'a'*64,
                f'case=lfn drive={drive} orphan_observations=0 bad_checksum_observations=0 invalid_observations=0 handling=short_fallback',
                'case=invalid_name invalid=-22 unmappable=-84 writes=0',
                'case=fixture disk=1 status=present',
                f'case=mount drive={drive} disk={0 if bits==32 else 1} type={bits} mode=ro reasons=1 writes=0 read_gate=1',
                'case=entry id=1 drive=D size=4480 attr=32 lfn_slots=2',
                'case=file id=1 size=4480 sha256='+'b'*64,
                'case=name id=1 field=path offset=0 bytes=24 hex='+b'D:/Ciuki long fixture.txt'.hex(),
                'case=name id=1 field=alias offset=0 bytes=12 hex='+b'CIUKIL~1.TXT'.hex(),
                'case=boundary id=1 offset=511 bytes=2 eof_bytes=0 error=0',meta]
        cache=['case=writeback dirty_age_ms=5001 writes=1 persisted=1 result=0',
               'case=eviction blocks=2 issued=2 barriers=2 result=0',
               'case=coherence shared_position=4 independent_bytes=4 result=0',
               'case=unsupported_flush mode=ro upgrade=-30 reasons=3',
               'case=trace id=1 barrier=1 action=write lba=3',
               'case=trace id=2 barrier=1 action=persist lba=3',
               'case=trace id=3 barrier=1 action=flush',meta]
        for fault in ('write','flush'):
            cache.extend([f'case=error layer=block_driver fault={fault} delayed=-5 flush=-5 unmount=-5 mounted=1',
                          f'case=ata_error fault={fault} delayed=-5 flush=-5 unmount=-5 mounted=1 quarantined=1',
                          'case=ata_quarantine owner=synthetic_ata generation=0 further_commands=0 real_commands=0 next=-200'])
        for name in ('cache','cache-unsupported-flush','cache-delayed-error','cache-flush-error'):result[name]=cache
        files=[f'case=file id={i} size={size} sha256='+'c'*64 for i,size in enumerate((0,512,4194304))]
        write=['case=workload workload=zero_patch_v1 seed=12689417 root=F109',
               'case=directory entries=15 first=107 next=109 aliases=distinct',
               'case=workload flush_result=0 checker=host_required',
               'event=ARM action=cold_reboot overlay=reuse marker=F109/DONE.BIN',*files,meta]
        for i,size in enumerate((0,512,4194304)):
            for op in ('create_read','overwrite_read','truncate_rename_delete'):
                write.append(f'case=operation file={i} op={op} bytes={size} result=0')
        result['fat-write']=write
        result['fat-write-cold']=['case=cold_reboot flush_result=0 checker=host_required',*files,meta]
        for name,error,reason in [('bad-bpb',-22,0),('dirty',0,5),('error-flag',0,9),
                                  ('fat-divergence',0,17),('chain-corruption',0,33),('torn-sector',0,33)]:
            result['mount-'+name]=[
                'case=partition disk=1 error=0',
                f'case=mount drive=D error={error} reasons={reason} mode={"rw" if error else "ro"} lost=0 write_refusal={error or -30} writes=0',
                'case=coverage fixtures=1 cut_selected=0 cut_reboot=0 checker=host_required',meta]
        result['mount-crash-reboot']=['case=crash_reboot reasons=5 lost=0 scan_corrupt=0 checker=host_required writes=0',
                                      'case=crash_refusal drive=C write_refusal=-30 writes=0',
                                      'case=coverage fixtures=0 cut_selected=1 cut_reboot=1 checker=host_required',meta]
        result['mount-crash-cut']=['event=ARM action=crash_cut marker=F109CUT.ARM workload=replace_rename bytes=8192',
                                 'case=cut index=1 barrier=0 action=write lba=2050 result=0 durable=1',
                                 'case=cut index=2 barrier=0 action=write lba=2050 result=0 durable=1']
        before='group=bootlog case=before prequalification_writes=0 storage_calls=0 queued=106 limit=131072'
        log='case=file id=0 name_hex=2f53595354454d2f4c4f47532f424f4f542e4c4f47 size=106 sha256='+'d'*64
        result['bootlog']=[before,'case=capture source=klog connected=1 captured=200',log,
                           'case=qualification qualified_seq=2 first_log_write_seq=3 size=106 writes=1 flush_result=0',
                           'event=ARM action=cold_reboot overlay=reuse marker=F109LOG.OK',meta]
        result['bootlog-cold']=[before,log,'case=cold_reboot writes=0 result=0',
                                'case=qualification qualified_seq=0 first_log_write_seq=0 size=106 writes=0 flush_result=0',meta]
        result['bootlog-read-only']=[before,
            'group=bootlog case=readonly disk_log=unavailable result=-30 write_count=0 storage_calls=0',meta]
        return result

    def test_fat_cold_boot_and_crash_cut_predicates(self):
        fixtures=self.fat_fixtures()
        for case in runner.load(ROOT/'tests/suites/f1-fat32.json')['cases']:
            for index,boot in enumerate(case.get('boots',[])):
                key=('mount-crash-cut' if index==0 else 'mount-crash-reboot') if case['id']=='mount-crash-reboot' else case['id']+('-cold' if index else '')
                expected=boot.get('expected',case['expected'])
                parser=self.records(case['probe'],fixtures[key])
                if expected['terminal']=='ARM':parser.records.pop();parser.terminal=None
                with self.subTest(case=case['id'],boot=index):
                    self.assertTrue(parser.check(expected))
                    for predicate in expected.get('predicates',[]):
                        for field in predicate.get('fields',{}):
                            broken=copy.copy(parser);broken.records=copy.deepcopy(parser.records)
                            record=next(r for r in broken.records if all(r.get(k)==str(v) for k,v in predicate['where'].items()))
                            record[field]='invalid'
                            with self.subTest(field=field),self.assertRaises(EvidenceError):broken.check(expected)

    def test_crash_reboot_keeps_dirty_scan_and_write_refusal_strict(self):
        case=next(c for c in runner.load(ROOT/'tests/suites/f1-fat32.json')['cases'] if c['id']=='mount-crash-reboot')
        bodies=self.fat_fixtures()['mount-crash-reboot']
        for before,after in [('reasons=5','reasons=1'),('reasons=5','reasons=37'),
                             ('lost=0','lost=1'),('scan_corrupt=0','scan_corrupt=1'),
                             ('write_refusal=-30','write_refusal=0'),('writes=0','writes=1')]:
            with self.subTest(field=before):
                broken=[body.replace(before,after) for body in bodies]
                with self.assertRaises(EvidenceError):self.records('mount-crash',broken).check(case['expected'])

    def test_actual_f1_records_and_each_predicate_violation(self):
        fixtures=self.fixtures()
        for profile in ('qemu-desktop-1998','qemu-desktop-2002'):
            for probe in ('registry','input-fault','input','framebuffer'):
                fixtures[probe+'-'+profile]=fixtures['input-qemu-t23' if probe=='input' else probe]
        for name in ('f1-input','f1-storage','f1-fat32'):
            for case in runner.load(ROOT/'tests/suites'/f'{name}.json')['cases']:
                if name=='f1-input' and case['id'] not in fixtures:continue
                self.assertIn(case['id'],fixtures)
                bodies=fixtures[case['id']]
                with self.subTest(case=case['id']):
                    self.assertTrue(self.records(case['probe'],bodies).check(case['expected']))
                    for predicate in case['expected']['predicates']:
                        for field in predicate.get('fields',{}):
                            parser=self.records(case['probe'],bodies)
                            matches=[r for r in parser.records if all(r.get(k)==str(v) for k,v in predicate['where'].items())]
                            self.assertTrue(matches)
                            matches[0][field]='invalid'
                            with self.subTest(field=field),self.assertRaises(EvidenceError):parser.check(case['expected'])
                    parser=self.records(case['probe'],bodies)
                    parser.records=[r for r in parser.records if r.get('event')!='DATA']
                    with self.assertRaises(EvidenceError):parser.check(case['expected'])

    def test_framebuffer_absent_and_reference_mismatch(self):
        case=next(c for c in runner.load(ROOT/'tests/suites/f1-input.json')['cases'] if c['id']=='framebuffer-no-lfb')
        bodies=[b.replace('absent=0','absent=1').replace('generation=1','generation=0') for b in self.fixtures()['framebuffer']]
        self.assertTrue(self.records('framebuffer',bodies).check(case['expected']))
        bodies[0]=bodies[0].replace('reference=12345678','reference=87654321')
        with self.assertRaises(EvidenceError):self.records('framebuffer',bodies).check(case['expected'])

    def test_safe_record_sources_disabled_devices_and_order(self):
        for case in runner.load(ROOT/'tests/suites/f1-safe.json')['cases']:
            fw='fw-cfg' in case['id'] or 'boot-cfg' not in case['id']
            backend='firmware' if case['profile']=='qemu-e500' else 'native'
            absent=case['id']=='safe-no-lfb'
            bodies=[f'case=option safe_mode=1 fw_cfg={int(fw)} boot_cfg={int(not fw)} menu=0 menu_inferred=0 boot_flags=00000061',
                    'case=activation flag_sequence=1 framebuffer_sequence=2 input_sequence=3 ata_sequence=4 flag_before_activation=1 optional_activations=0',
                    f'case=required console=1 console_mode={"text" if absent else "lfb"} input=1 backend={backend} active_resources=12 disk_log=unavailable',
                    f'group=safe safe_mode=1 flag_before_activation=1 optional_activations=0 input_works=1 console_works=1 bios_retries=0 source={"fw-cfg" if fw else "boot-cfg"}',
                    f'group=metadata subcase=required owner={"firmware-input" if backend=="firmware" else "i8042"} generation=1 errors=0 gate=safe timing_domain=icount']
            bodies.extend(f'case=disabled device={dev} reason=safe_mode' for dev in ('audio','acceleration','network','dma','power','optional_firmware','ata'))
            if absent:bodies.append('case=disabled device=framebuffer reason=no_lfb console=text')
            parser=self.records('safe',bodies)
            parser.records.insert(-1,dict(event='READY',console='1',input='1',optional_activations='0'))
            with self.subTest(case=case['id']):
                self.assertTrue(parser.check(case['expected']))
                for field,value in [('optional_activations','1'),('input_sequence','1')]:
                    broken=copy.copy(parser)
                    broken.records=copy.deepcopy(parser.records)
                    next(r for r in broken.records if r.get('case')=='activation')[field]=value
                    with self.assertRaises(EvidenceError):broken.check(case['expected'])

    def test_missing_firmware_blkdebug_and_fat_evidence_never_pass(self):
        for case in runner.load(ROOT/'tests/suites/f1-input.json')['cases']:
            if not case['expected'].get('not_run_subcases'):continue
            parser=self.records('input-fault',self.fixtures()['input-fault'])
            with self.assertRaisesRegex(EvidenceError,'not_run'):parser.check(case['expected'])
            self.assertEqual(parser.not_run_subcases,case['expected']['not_run_subcases'])
        case=runner.load(ROOT/'tests/suites/f1-storage.json')['cases'][-1]
        parser=self.records('ata',self.fixtures()['ata'])
        with self.assertRaisesRegex(EvidenceError,'missing evidence'):parser.check(case['expected'])
        self.assertEqual(parser.not_run_subcases,[])
        # Installed-table ERROR is the actual format for every missing FAT/bootlog probe.
        for case in runner.load(ROOT/'tests/suites/f1-fat32.json')['cases']:
            parser=Parser('12345678',case['probe'])
            for seq,body in enumerate(('event=BEGIN','event=READY table=f1 installed=8','event=ERROR status=not_run reason=missing_probe'),1):
                parser.feed(f"CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe={case['probe']} {body}".encode())
            self.assertEqual(parser.outcome,'not_run')
            with self.assertRaisesRegex(EvidenceError,'not_run'):parser.check(case['expected'])

class F2EvidenceTests(unittest.TestCase):
    def test_f2_grammar_phase_separation_and_loader_registry(self):
        model=(ROOT/'src/boot/ciukldr/menu.inc').read_text()
        block=model.split('f2_probe_names db',1)[1].split('platform_suffix',1)[0]
        for probe in F2_PROBES:
            self.assertIn("'"+probe+"'",block)
            self.assertEqual(selector(f'f2:{probe} run=1234aBcD')['phase'],2)
            self.assertTrue(selector(f'f2:{probe} run=1234aBcD platform=e500 safe=1','fw_cfg',True)['safe'])
            for phase in (0,1):
                with self.assertRaises(ValueError):selector(f'f{phase}:{probe} run=12345678')
        self.assertNotIn("'all'",block);self.assertNotIn("'core'",block)
        for value in ('all','core','boot','input','unknown'):
            with self.assertRaises(ValueError):selector(f'f2:{value} run=12345678')
        for suffix in (' safe=1 platform=e500',' run=12345678',' safe=1 safe=1',' platform=e500 platform=e500','\n','\0',' '+'x'*64):
            with self.assertRaises(ValueError):selector('f2:elf-load run=12345678'+suffix,'fw_cfg',True)
        for source,valid in (('menu',True),('serial',True),('fw_cfg',False)):
            with self.assertRaises(ValueError):selector('f2:elf-load run=12345678 safe=1',source,valid)

    def test_application_frames_cannot_forge_controller_records(self):
        parser=Parser('12345678','app-gate')
        def feed(seq,extra):
            return parser.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=app-gate {extra}'.encode())
        feed(1,'event=BEGIN')
        data=b'CIUKI_TEST v=1 run=12345678 seq=000999 probe=app-gate event=END status=PASS\n'
        for index,off in enumerate(range(0,len(data),24),2):
            chunk=data[off:off+24]
            feed(index,f'event=DATA group=app pid=6 tid=7 stream=stdout offset={off} bytes={len(chunk)} data_hex={chunk.hex()}')
        self.assertIsNone(parser.terminal)
        self.assertEqual(len(parser.records),1)
        self.assertEqual(bytes(parser.application.head),data)
        with self.assertRaises(EvidenceError):parser.check({'terminal':'END'})
        feed(10,'event=END status=FAIL')
        with self.assertRaises(EvidenceError):parser.check({'terminal':'END'})

    def test_bounded_streaming_digest_head_tail_at_log_cap(self):
        cap=res.LOG_CAP;capture=ApplicationCapture(retention=32)
        digest=hashlib.sha256()
        for i in range(cap//8192+2):
            data=bytes([i%251])*8192;capture.feed(data);digest.update(data)
            self.assertLessEqual(len(capture.head)+len(capture.tail),cap+32)
        evidence=capture.result()
        self.assertGreater(evidence['total_bytes'],cap)
        self.assertTrue(evidence['capture_truncated'])
        self.assertEqual(evidence['sha256'],digest.hexdigest())
        self.assertEqual(bytes.fromhex(evidence['head_hex']),bytes(32))
        self.assertEqual(bytes.fromhex(evidence['tail_hex']),bytes([(cap//8192+1)%251])*32)

    def test_multipart_metadata_and_missing_fields(self):
        digest=hashlib.sha256(b'sdk').hexdigest()
        records=[dict(event='DATA',group='metadata',name='sdk_manifest_sha256',part=str(i+1),parts='2',encoding='sha256',hex=digest[i*32:(i+1)*32]) for i in range(2)]
        argv=['lua','-e','_U=true','all.lua']
        encoded=json.dumps(argv).encode().hex()
        records.append(dict(event='DATA',group='metadata',name='argv',part='1',parts='1',encoding='json',hex=encoded))
        result=f2_metadata(records)
        self.assertEqual(result['sdk_manifest_sha256'],digest);self.assertEqual(result['argv'],argv)
        self.assertEqual(result['abi_version'],'unknown');self.assertIn('abi_version',result['missing_fields'])
        self.assertEqual(result['excluded_modes'],{'complete':'excluded_by_contract','internal':'excluded_by_contract'})
        self.assertIn('sdk_manifest_sha256',f2_metadata(records[1:])['missing_fields'])
        with self.assertRaises(EvidenceError):f2_metadata(records+[records[0]])
        with self.assertRaises(EvidenceError):f2_metadata([dict(event='DATA',abi_version='1'),dict(event='DATA',abi_version='2')])
        with self.assertRaises(EvidenceError):f2_metadata([dict(event='DATA',sdk_manifest_sha256='abc')])

    def test_suite_matrix_deadlines_alias_and_prerequisites(self):
        deadlines={'elf-load':180,'spawn-wait':180,'fd-table':300,'mmap':180,'signals-fault':180,'threads-wait':300,'libc-smoke':180,'crash-isolation':300,'app-gate':900}
        probes=[]
        for name in runner.F2_SUITES:
            suite=runner.load(ROOT/'tests/suites'/f'{name}.json')
            self.assertEqual(suite['prerequisites'][:8],list(runner.REGRESSION_SUITES))
            for case in suite['cases']:
                probes.append(case['probe']);self.assertEqual(case['timeout'],deadlines[case['probe']])
                profile=runner.load(ROOT/'tests/profiles'/f"{case['profile']}.json")
                args,text=runner.qemu_args('fake',profile,case,'12345678',ROOT/'build/run.qcow2',ROOT/'build/bios.bin')
                self.assertEqual(runner.selector(text,'fw_cfg',True)['phase'],2)
                self.assertIn('shift=1,sleep=on',args)
                runner.Actions(case.get('actions',[]))
            for probe in {c['probe'] for c in suite['cases']}:
                self.assertEqual({c['profile'] for c in suite['cases'] if c['probe']==probe},({'qemu-t23','qemu-e500','qemu-min128','qemu-desktop-1998','qemu-desktop-2002'} if probe=='crash-isolation' else {'qemu-t23','qemu-e500','qemu-min128'}))
        self.assertEqual(set(probes),set(F2_PROBES))
        suite=runner.load_suite('f2-all');cases=suite['cases']
        first=next(i for i,c in enumerate(cases) if c['selector'].startswith('f2:'))
        expected=[c for name in runner.F2_SUITES for c in runner.load(ROOT/'tests/suites'/f'{name}.json')['cases']]
        self.assertEqual([c['id'] for c in cases[first:]],[c['id'] for c in expected])
        self.assertEqual(len(cases[:first]),sum(len(runner.load(ROOT/'tests/suites'/f'{n}.json')['cases']) for n in runner.REGRESSION_SUITES))
        for suite_name in ('f1-input','f1-storage','f1-fat32','f1-safe',*runner.F2_SUITES):
            cases=runner.load_suite(suite_name)['cases']
            confirmations=[c for c in cases if c.get('operator_confirmation')]
            self.assertEqual(len(confirmations),3)
            self.assertTrue(all(c['id'].startswith('uart-absent-') for c in confirmations))
            for c in confirmations:
                self.assertFalse(runner.failed_prerequisite(c,{'outcome':'fail','operator_confirmation':True}))
                self.assertTrue(runner.failed_prerequisite(c,{'outcome':'fail','operator_confirmation':False}))
                self.assertTrue(runner.failed_prerequisite({}, {'outcome':'fail','operator_confirmation':True}))

    def test_all_f2_predicates_fabricated_success_and_each_violation(self):
        # Exercise the actual JSON predicate declarations, splitting summaries
        # into bounded controller records, rather than bypassing Parser.
        def records_for(predicate):
            matches=[]
            for index in range(predicate.get('exact_count',predicate.get('count',1))):
                record=dict(predicate.get('where',{}))
                for key in predicate.get('required_fields',[]):record.setdefault(key,'1')
                if predicate.get('unique'):record[predicate['unique']]=str(index)
                for field,rule in predicate.get('fields',{}).items():
                    if not isinstance(rule,dict):record[field]=str(rule);continue
                    value=rule.get('eq',rule.get('ge',0))
                    if isinstance(value,str) and value.startswith('$'):value=0
                    record[field]=str(value)
                for field,rule in predicate.get('fields',{}).items():
                    if isinstance(rule,dict):
                        for op in ('eq','ge','le'):
                            value=rule.get(op)
                            if isinstance(value,str) and value.startswith('$'):record[field]=record[value[1:]]
                        if rule.get('encoding')=='hex':record[field]=f"{int(record[field]):0{rule.get('width',8)}x}"
                for relation in predicate.get('relations',[]):
                    record.setdefault(relation['right'],'1')
                    bound=int(record[relation['right']])
                    if 'subtract' in relation:
                        record.setdefault(relation['subtract'],'0');bound-=int(record[relation['subtract']])
                    bound=(bound+relation.get('add',0))*relation.get('multiply',1)
                    record[relation['left']]=str(max(int(record.get(relation['left'],bound)),bound) if relation['op']=='ge' else bound)
                if predicate.get('unique'):
                    key=predicate['unique']
                    record[key]=str(predicate.get('fields',{}).get(key,{}).get('ge',0)+index)
                matches.append(record)
            return matches
        def check(probe,predicate,records):
            parser=Parser('12345678',probe);seq=0
            def feed(record):
                nonlocal seq
                seq+=1;line='CIUKI_TEST '+ ' '.join(f'{k}={v}' for k,v in {'v':1,'run':'12345678','seq':f'{seq:06d}','probe':probe,**record}.items())
                self.assertLessEqual(len(line),240);parser.feed(line.encode())
            feed({'event':'BEGIN'})
            for record in records:
                if predicate.get('combine'):
                    where=predicate['where']
                    for key,value in record.items():
                        if key not in where:feed({**where,key:value})
                else:feed(record)
            feed({'event':'END','status':'PASS'})
            return parser.check({'terminal':'END','predicates':[predicate]})
        for name in runner.F2_SUITES:
            suite=runner.load(ROOT/'tests/suites'/f'{name}.json')
            for case in suite['cases']:
                for predicate in case['expected']['predicates']:
                    with self.subTest(case=case['id'],predicate=predicate['where']):
                        records=records_for(predicate)
                        self.assertTrue(check(case['probe'],predicate,records))
                        with self.assertRaises(EvidenceError):check(case['probe'],predicate,[])
                        for field,rule in predicate.get('fields',{}).items():
                            broken=copy.deepcopy(records)
                            value=rule.get('eq',rule.get('ge')) if isinstance(rule,dict) else rule
                            if isinstance(rule,dict):
                                old=int(broken[0][field],16 if rule.get('encoding')=='hex' else 10)
                                bad=rule['le']+1 if 'le' in rule else min(old,rule['ge'])-1 if isinstance(rule.get('ge'),int) else old-1 if 'ge' in rule else old+1
                                broken[0][field]=f'{bad:08x}' if rule.get('encoding')=='hex' else str(bad)
                            else:broken[0][field]='incorrect'
                            with self.subTest(field=field),self.assertRaises(EvidenceError):check(case['probe'],predicate,broken)

    def test_kernel_map_f2probes_within_rodata(self):
        path=ROOT/'build/f0/VMM.map'
        if not path.exists():self.skipTest('kernel map checked after the mandatory kernel build')
        rows=path.read_text().splitlines()
        def addr(name):return int(next(r.split()[0] for r in rows if name+' = .' in r),16)
        start,end=addr('__f2probes_start'),addr('__f2probes_end')
        self.assertLessEqual(addr('__rodata_start'),start);self.assertLessEqual(start,end)
        self.assertLessEqual(end,addr('__rodata_end'))
        # app-gate owns its registration in its own translation unit and is
        # the final F2 registration in the explicit builder source order.
        self.assertEqual(linked_probe_names(rows,2),[
            'elf-load','spawn-wait','mmap','threads-wait','crash-isolation','libc-smoke','fd-table','signals-fault','app-gate'])
        registrations=[line.split()[-1] for line in rows if ' f2_registration_' in line]
        self.assertEqual(registrations,[
            'f2_registration_probe_f2_elf_load','f2_registration_probe_f2_spawn_wait',
            'f2_registration_probe_f2_mmap','f2_registration_probe_f2_threads_wait',
            'f2_registration_probe_f2_crash_isolation','f2_registration_probe_f2_libc_smoke',
            'f2_registration_probe_f2_fd_table',
            'f2_registration_probe_f2_signals_fault','f2_registration_probe_f2_app_gate'])
        self.assertEqual(end-start,8*len(registrations))
        sections=[r for r in rows if ':(.f2probes)' in r];self.assertTrue(sections)
        for row in sections:
            fields=row.split();self.assertGreaterEqual(int(fields[0],16),start)
            self.assertLessEqual(int(fields[0],16)+int(fields[2],16),end)

    def test_production_kernel_application_framing_and_digest(self):
        source=(ROOT/'src/kernel/probes/probes.c').read_text()
        first=source.index('static char app_probe[24]');last=source.index('static const struct probe_def probes[]',first)
        scratch=ROOT/'build/runner-host-tests';scratch.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            directory=Path(directory)
            stubs=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/task.h>
#include <ciuki/process.h>
#include <ciuki/probe.h>
#include <ciuki/sync.h>
#include <ciuki/sha256.h>
static char evidence[8192];
static unsigned seq;
static struct process process = { .pid=7 };
static struct proc_thread thread = { .tid=9, .process=&process };
static struct task task = { .id=123 };
struct proc_thread *proc_thread_for(const struct task *t) { (void)t; return &thread; }
void kmutex_init(struct kmutex *m) { memset(m,0,sizeof(*m)); }
void kmutex_lock(struct kmutex *m) { (void)m; }
void kmutex_unlock(struct kmutex *m) { (void)m; }
static void klog(const char *fmt,...) {
    va_list ap; va_start(ap,fmt); vsnprintf(evidence,sizeof(evidence),fmt,ap); va_end(ap);
}
void rec_emit(const char *probe,const char *event,const char *fmt,...) {
    unsigned n=(unsigned)strlen(evidence);
    n+=(unsigned)snprintf(evidence+n,sizeof(evidence)-n,"CIUKI_TEST v=1 run=12345678 seq=%06u probe=%s event=%s",++seq,probe,event);
    if (fmt) {
        n+=(unsigned)snprintf(evidence+n,sizeof(evidence)-n," ");
        va_list ap; va_start(ap,fmt); vsnprintf(evidence+n,sizeof(evidence)-n,fmt,ap); va_end(ap);
    }
    strcat(evidence,"\n");
}
'''
            wrapper=r'''
void report(const char *msg,unsigned len,int active) {
    evidence[0]=0;seq=0;
    if (active) {
        strcpy(app_probe,"app-gate");sha256_init(&app_digest);app_bytes=0;
        rec_emit("app-gate","BEGIN",0);
    } else app_probe[0]=0;
    probe_user_report(&task,msg,len);
}
const char *records(void) { return evidence; }
'''
            harness=directory/'app.c';harness.write_text(stubs+source[first:last]+wrapper)
            library=directory/'app.so'
            compiled=subprocess.run(['clang','-shared','-fPIC','-std=c17','-Wall','-Wextra','-Werror','-I',str(ROOT/'src/kernel/include'),str(harness),str(ROOT/'src/kernel/lib/sha256.c'),'-o',str(library)],capture_output=True,text=True)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            native=ctypes.CDLL(str(library));native.report.argtypes=[ctypes.c_char_p,ctypes.c_uint,ctypes.c_int];native.records.restype=ctypes.c_char_p
            text=b'CIUKI_TEST v=1 event=END status=PASS '*6
            native.report(text,len(text),1)
            parser=Parser('12345678','app-gate')
            for line in native.records().splitlines():
                self.assertLessEqual(len(line),240);parser.feed(line)
            self.assertIsNone(parser.terminal)
            self.assertEqual(bytes(parser.application.head),text)
            self.assertEqual(parser.application.digest.hexdigest(),hashlib.sha256(text).hexdigest())
            self.assertEqual(parser.application.result()['identities'],[{'pid':'7','tid':'9','stream':'report'}])
            native.report(b'plain report',12,0)
            self.assertEqual(native.records(),b'[user 123 report] plain report')

    def test_f2_payload_manifest_comparison_rejects_mismatch(self):
        scratch=ROOT/'build/runner-host-tests';scratch.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            manifest=Path(directory)/'build-manifest.json'
            digest=hashlib.sha256(b'elf').hexdigest()
            manifest.write_text(json.dumps({'payloads':[{'path':'/bin/hello','sha256':digest}]}))
            for actual,ok in ((digest,True),('0'*64,False)):
                parser=Parser('12345678','elf-load')
                for seq,extra in enumerate(('event=BEGIN',f'event=DATA group=payload path=/bin/hello sha256={actual}','event=END status=PASS'),1):
                    parser.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=elf-load {extra}'.encode())
                result={'build_manifest':{'path':str(manifest)},'probe':'elf-load','outcome':'pass'}
                if ok:
                    runner.record_f2_result(result,parser)
                    self.assertTrue(result['payload_hash_comparisons'][0]['match'])
                    self.assertEqual(result['missing_payload_hashes'],[])
                else:
                    with self.assertRaisesRegex(EvidenceError,'guest-loaded'):runner.record_f2_result(result,parser)

    def test_app_result_requires_provenance_setup_wait_and_complete_output(self):
        scratch=ROOT/'build/runner-host-tests';scratch.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            directory=Path(directory);manifest=directory/'build-manifest.json';digest=hashlib.sha256(b'payload').hexdigest()
            paths=('/bin/lua','/system/tests/ciuki-f2.lua','/system/tests/lua-5.4.8-tests/all.lua')
            manifest.write_text(json.dumps({'sdk_manifest_sha256':digest,'payloads':[{'path':p,'sha256':digest} for p in paths],
                                           'lua':{'archives':{'source':{'sha256':digest},'tests':{'sha256':digest}}}}))
            metadata={field:'1' for field in F2_FIELDS}
            metadata.update(sdk_manifest_sha256=digest,newlib_source_sha256=digest,newlib_patch_hashes={'patch':digest},
                            application_source_sha256=digest,application_tests_sha256=digest,elf_hashes={'/bin/lua':digest},
                            argv=['lua','-e','_U=true','all.lua'],env={'LC_ALL':'C','TZ':'UTC0','HOME':'/home','TMPDIR':'/tmp'},
                            cwd='/system/tests/lua-5.4.8-tests',fd_setup={'inherited':[0,1,2],'stdin':'/dev/null','stdout':'bounded','stderr':'bounded'},
                            application_wait_status='0',declared_exclusions=['_U'],resource_ledgers={'baseline':1,'final':1})
            def parser_for(values,output=b'final OK !!!\n'):
                parser=Parser('12345678','app-gate');seq=0
                def feed(extra):
                    nonlocal seq
                    seq+=1;line=f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=app-gate '+extra
                    self.assertLessEqual(len(line),240);parser.feed(line.encode())
                feed('event=BEGIN')
                for name,value in values.items():
                    if isinstance(value,(dict,list)) or name=='cwd':
                        encoding='utf8' if name=='cwd' else 'json'
                        data=(value if encoding=='utf8' else json.dumps(value)).encode().hex()
                        parts=[data[i:i+48] for i in range(0,len(data),48)]
                        for i,part in enumerate(parts,1):feed(f'event=DATA group=metadata name={name} part={i} parts={len(parts)} encoding={encoding} hex={part}')
                    else:feed(f'event=DATA {name}={value}')
                for path in paths[1:]:feed(f'event=DATA group=payload path={path} sha256={digest}')
                feed(f'event=DATA group=app pid=7 tid=9 stream=stdout offset=0 bytes={len(output)} data_hex={output.hex()}')
                feed('event=END status=PASS');return parser
            def result():return {'probe':'app-gate','outcome':'pass','build_manifest':{'path':str(manifest)}}
            evidence=result();runner.record_f2_result(evidence,parser_for(metadata));self.assertEqual(evidence['missing_f2_fields'],[])
            for field,value in (('application_wait_status','9472'),('abi_version','2'),('argv',['lua','all.lua']),
                                ('env',{}),('cwd','/tmp'),('fd_setup',{'inherited':[0,1,2,3]})):
                changed={**metadata,field:value}
                with self.subTest(field=field),self.assertRaises(EvidenceError):runner.record_f2_result(result(),parser_for(changed))
            for output in (b'no final indication',b'assertion failed!'):
                with self.assertRaises(EvidenceError):runner.record_f2_result(result(),parser_for(metadata,output))
            changed=dict(metadata);changed.pop('max_committed_pages')
            with self.assertRaisesRegex(EvidenceError,'missing application'):runner.record_f2_result(result(),parser_for(changed))

    def test_application_capture_keeps_complete_output_until_cap_and_scans_after(self):
        capture=ApplicationCapture(retention=16,log_limit=128)
        capture.feed(b'x'*128)
        self.assertFalse(capture.result()['capture_truncated']);self.assertEqual(len(capture.head),128)
        capture.feed(b'assertion fai');capture.feed(b'led! final ');capture.feed(b'OK !!!')
        self.assertTrue(capture.result()['capture_truncated']);self.assertEqual(len(capture.head),16)
        self.assertEqual(capture.assertion_indications,1);self.assertTrue(capture.final_success_indication)
