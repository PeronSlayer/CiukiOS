import fcntl
import json
import os
from pathlib import Path
import signal
import select
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

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
        scratch=ROOT/'build/runner-host-tests';scratch.mkdir(exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=scratch);self.root=Path(self.temp.name)
        self.image=self.root/'image.raw';self.image.write_bytes(b'canonical immutable image')
        self.firmware=self.root/'bios.bin';self.firmware.write_bytes(b'firmware fixture')
        self.cgroup=self.root/'cgroup';self.cgroup.mkdir();(self.cgroup/'memory.events').write_text('oom 0\noom_kill 0\n')
        self.img=self.root/'fake-img';self.img.write_text('#!/usr/bin/env python3\nimport pathlib,sys\nassert sys.argv[1:4]==["create","-f","qcow2"]\nassert "-b" in sys.argv and "-F" in sys.argv\npathlib.Path(sys.argv[-1]).write_bytes(b"fixture overlay")\n');self.img.chmod(0o755)
        self.script=self.root/'scenario.json'
        self.env=patch.dict(os.environ,{'CIUKI_FAKE_SCRIPT':str(self.script)});self.env.start()
        self.profile=dict(name='qemu-t23',machine='pc-i440fx-9.2',cpu='pentium3',accelerator='tcg',ram_mib=512,vga='std',audio='AC97')
        self.case=dict(id='boot',probe='boot',timeout=2,expected=dict(terminal='END',predicates=[dict(where={'event':'DATA'},fields={'tick':{'ge':10000}})]))
    def tearDown(self):self.env.stop();self.temp.cleanup()
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
