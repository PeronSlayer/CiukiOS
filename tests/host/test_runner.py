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
sys.path.insert(0,str(ROOT/'scripts/test'))
from loader_model import selector, boot_options, F0_PROBES, F1_PROBES, F2_PROBES, SELECTOR_RE
import run as runner
import resources as res
from evidence import Parser,EvidenceError,ApplicationCapture,f2_metadata,F2_FIELDS


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
    def test_duplicate_selector_keys_are_refused(self):
        for suffix in (' platform=e500 platform=e500',' safe=1 safe=1',' run=12345678'):
            case={**self.case,'selector':'f0:boot run={run_id}'+suffix}
            with self.subTest(suffix=suffix),self.assertRaises(ValueError):
                runner.qemu_args('fixture',self.profile,case,'12345678',self.image,self.firmware)
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
        sections=[line for line in rows if ':(.f1probes)' in line]
        self.assertTrue(sections)
        for line in sections:
            columns=line.split();vma=int(columns[0],16);size=int(columns[2],16)
            self.assertGreaterEqual(vma,start);self.assertLessEqual(vma+size,end)

    def test_production_kernel_selector_names_and_phase_dispatch(self):
        if not shutil.which('clang'):self.skipTest('host clang unavailable')
        source=(ROOT/'src/kernel/probes/probes.c').read_text()
        first=source.index('static const struct probe_def probes[]');last=source.index('\nstatic __attribute__',first)
        harness=self.root/'selector.c'
        stubs="""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include <ciuki/boot_info.h>
#include <ciuki/probe.h>
#include <ciuki/process.h>
#include <ciuki/sha256.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define CIUKI_BUILD_ID "host"
static struct ciuki_boot_info g_boot;
static uint64_t g_tsc_per_ms;
static jmp_buf done;
static char evidence[4096];
static int calls0, calls1, failing;
static int f0(void) { calls0++; return failing; }
static int f1(void) { calls1++; return failing; }
#define probe_boot f0
#define probe_bootinfo f0
#define probe_allocator f0
#define probe_protection f0
#define probe_isolation f0
#define probe_preempt f0
#define probe_localfault f0
#define probe_syslife f0
#define probe_fpu f0
static void probe_panic(void) { calls0++; }
void rec_set_run(const char *s) { (void)s; }
void rec_emit(const char *p,const char *event,const char *fmt,...) {
    unsigned n=(unsigned)strlen(evidence);
    n+=(unsigned)snprintf(evidence+n,sizeof(evidence)-n,"%s %s ",p,event);
    if (fmt) { va_list ap; va_start(ap,fmt); vsnprintf(evidence+n,sizeof(evidence)-n,fmt,ap); va_end(ap); }
    strcat(evidence,"\\n");
}
static void timing_calibrate(void) {}
static void kwork_init(void) {}
void task_sleep_ms(unsigned n) { (void)n; longjmp(done,1); }
static void klog(const char *fmt,...) { (void)fmt; }
static __attribute__((noreturn)) void show_evidence_forever(void) { longjmp(done,1); }
static const struct probe_def fixture_table[] = {{"input",f1},{"framebuffer",f1}};
static const struct probe_def *table_end;
#define __f1probes_start fixture_table
#define __f1probes_end table_end
static int calls2;
static int f2(void) { calls2++; return failing; }
static const struct ciuki_f2_probe fixture_f2[] = {{"elf-load",f2},{"spawn-wait",f2}};
#define __f2probes_start fixture_f2
#define __f2probes_end (fixture_f2+2)
static char app_probe[24];
static struct sha256_ctx app_digest;
static struct kmutex app_lock;
static volatile unsigned app_bytes;
void sha256_init(struct sha256_ctx *p) { (void)p; }
void kmutex_init(struct kmutex *p) { (void)p; }
"""
        wrappers="""
int validate(const char *s,unsigned len,unsigned flags) {
    struct probe_selection selection;
    g_boot.flags=flags;
    evidence[0]=0;
    if (!parse_selector(s,len,&selection)) return 0;
    return (int)selection.phase+1;
}
int dispatch(const char *s,unsigned flags,unsigned count,int fail) {
    memset(&g_boot,0,sizeof(g_boot));
    g_boot.flags=flags|CBI_F_TEST_REQUEST;
    g_boot.test_request_len=(uint16_t)strlen(s);
    memcpy(g_boot.test_request,s,g_boot.test_request_len);
    table_end=fixture_table+count;calls0=calls1=calls2=0;evidence[0]=0;failing=fail;
    if (!setjmp(done)) probes_main(0);
    return calls0*100+calls1+calls2*10000;
}
const char *records(void) { return evidence; }
"""
        # Compile the production parser and probes_main; only hardware/probe bodies are mocked.
        harness.write_text(stubs+source[first:last]+'\n'+source[source.index('void probes_main(void *arg)'):]+wrappers)
        library=self.root/'selector.so'
        compiled=subprocess.run(['clang','-shared','-fPIC','-std=c17','-Wall','-Werror','-I',str(ROOT/'src/kernel/include'),
                                str(harness),'-o',str(library)],capture_output=True,text=True)
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
                self.assertEqual(selector(text,'fw_cfg',True)['phase'],2)
                self.assertIn('shift=1,sleep=on',args)
                runner.Actions(case.get('actions',[]))
            for probe in {c['probe'] for c in suite['cases']}:
                self.assertEqual({c['profile'] for c in suite['cases'] if c['probe']==probe},{'qemu-t23','qemu-e500','qemu-min128'})
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
                    record.setdefault(relation['right'],'1');record[relation['left']]=record[relation['right']]
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
                                bad=rule['le']+1 if 'le' in rule else old-1 if 'ge' in rule else old+1
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
