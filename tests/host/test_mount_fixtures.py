"""Real FAT host tools and scripted emulator boundaries; no QEMU guest."""
import copy
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import test_runner as existing
import fat_fixtures
from mount_fixtures import place_marker, classify_crash_checker, check_crash_files
import run as runner
import resources as res

ROOT = existing.ROOT
EMPTY = {'size':0, 'sha256':hashlib.sha256(b'').hexdigest()}


class CrashCheckerTests(unittest.TestCase):
    def setUp(self):
        self.case=next(c for c in runner.load(ROOT/'tests/suites/f1-fat32.json')['cases'] if c['id']=='mount-crash-reboot')
        self.output=existing.CRASH_ORPHAN_FSCK.format(volume='/ciuki/check-volume.raw')

    def classify(self, output, returncode=1):
        outcomes=[]
        for boot in self.case['boots']:
            checks=boot['checks']
            self.assertEqual(checks['fsck_exit_codes'],[1])
            self.assertEqual(checks['empty_files'],['::/F109CUT.ARM'])
            outcomes.append(classify_crash_checker({'output':output,'returncode':returncode},
                checks['interrupted_files'],checks['interrupted_patterns']))
        self.assertEqual(outcomes[0],outcomes[1])
        return outcomes[0]

    def test_captured_orphan_is_declared_for_both_boots(self):
        self.assertEqual(self.classify(self.output),
                         {'dirty':True,'files':[{'path':'::/F109CUT.BIN','state':'orphan-lfn'}]})

    def test_empty_workload_remains_declared(self):
        output=self.output.replace('Orphaned long file name part "F109CUT.BIN"\n  Auto-deleting.\n','')
        self.assertEqual(self.classify(output)['files'],[{'path':'::/F109CUT.BIN','state':'empty-file'}])

    def test_orphan_attribution_and_every_other_diagnostic_are_strict(self):
        pair='Orphaned long file name part "F109CUT.BIN"\n  Auto-deleting.\n'
        for output in (self.output.replace('F109CUT.BIN','OTHER.BIN'),
                       self.output.replace('F109CUT.BIN','F109CUT'),
                       self.output.replace('  Auto-deleting.\n',''),
                       self.output.replace(pair,pair+pair),
                       self.output+'  Auto-deleting.\n',
                       self.output+'Cross-linked clusters\n',
                       self.output+'Reclaimed 1 unused cluster (4096 bytes).\n',
                       self.output+'FATs differ but appear to be intact.\n'):
            with self.subTest(output=output),self.assertRaises(runner.EvidenceError):self.classify(output)

    def test_dirty_bit_and_nonmutating_checker_are_required(self):
        for line in ('Dirty bit is set. Fs was not properly unmounted and some data may be corrupt.\n',
                     ' Automatically removing dirty bit.\n','Leaving filesystem unchanged.\n'):
            for output in (self.output.replace(line,''),self.output+line):
                with self.subTest(line=line,output=output),self.assertRaises(runner.EvidenceError):self.classify(output)
        for returncode in (0,2):
            with self.subTest(returncode=returncode),self.assertRaises(runner.EvidenceError):
                self.classify(self.output,returncode)

    def test_listing_errors_and_directory_names_never_prove_absence(self):
        classification=self.classify(self.output)
        for returncode,output in ((1,''),(0,'mdir: I/O error\n'),
                                  (0,'::/f109cut.bin\n'),(0,'::/F109CUT.BIN/\n')):
            with self.subTest(returncode=returncode,output=output), \
                 patch.object(runner.Host,'checker',return_value={'returncode':returncode,'output':output}), \
                 self.assertRaises(runner.EvidenceError):
                check_crash_files(runner.Host(),'volume',ROOT/'build',classification,[])


class CorruptionTests(unittest.TestCase):
    def setUp(self):
        if not all(shutil.which(tool) for tool in ('mkfs.fat', 'mcopy', 'mdir', 'fsck.fat')):
            self.skipTest('FAT host tools unavailable')
        scratch=ROOT/'build/runner-host-tests';scratch.mkdir(parents=True,exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=scratch);self.root=Path(self.temp.name)

    def tearDown(self):self.temp.cleanup()

    def defect(self, name):
        for kind in (16, 32):
            with self.subTest(kind=kind):
                d=self.root/str(kind);d.mkdir()
                fixture={'fat_type':kind, 'seed':1, 'corruption':name}
                manifest=runner.Host().fat_fixture(fixture,d,0)
                image=Path(manifest['path']);checksum=runner.sha(image)
                self.assertEqual(checksum,manifest['sha256'])
                self.assertTrue(manifest['checker_classification']['classified'])
                self.assertEqual(manifest['corruption_checker']['returncode'],0 if name=='error-flag' else 1)
                self.assertEqual(manifest['checker_classification']['detects_intended_defect'],name!='error-flag')
                corruption=manifest['corruption'];g=corruption['geometry'];width=kind//8
                with image.open('r+b') as stream:
                    for item in corruption['patches']:
                        offset=item['sector']*512+item['offset']
                        stream.seek(offset);self.assertEqual(stream.read(item['bytes']),bytes.fromhex(item['after_hex']))
                        stream.seek(offset);stream.write(bytes.fromhex(item['before_hex']))
                # Undo precisely the declared byte ranges: the clean seed is
                # identical and fsck sees no unrelated damage.
                self.assertEqual(runner.sha(image),manifest['seed_sha256'])
                self.assertEqual(subprocess.run(['fsck.fat','-n',str(image)],capture_output=True).returncode,0)
                self.assertEqual(corruption['patched_sectors'],sorted({p['sector'] for p in corruption['patches']}))
                if name in ('dirty','error-flag'):
                    mask=(0x8000 if name=='dirty' else 0x4000) if kind==16 else (0x08000000 if name=='dirty' else 0x04000000)
                    self.assertEqual(len(corruption['patches']),2)
                    for item in corruption['patches']:
                        self.assertEqual(item['offset'],width)
                        self.assertEqual(int.from_bytes(bytes.fromhex(item['before_hex']),'little') ^
                                         int.from_bytes(bytes.fromhex(item['after_hex']),'little'),mask)
                if name=='torn-sector':
                    self.assertEqual([(p['sector'],p['offset'],p['bytes']) for p in corruption['patches']],[(g['root'],256,256)])
                if name=='chain-corruption':
                    self.assertEqual(corruption['patches'][0]['after_hex'],corruption['patches'][1]['after_hex'])
                regenerated=runner.Host().fat_fixture(fixture,d,0)
                self.assertEqual(regenerated['sha256'],checksum)
                self.assertEqual(regenerated['corruption']['patches'],corruption['patches'])
                bad=copy.deepcopy(regenerated['corruption_checker']);bad['output']+='Unexpected cross-link\n'
                with self.assertRaises(res.Refusal):fat_fixtures.validate_checker(name,bad)

    def test_bad_bpb(self):self.defect('bad-bpb')
    def test_dirty(self):self.defect('dirty')
    def test_error_flag_and_checker_limitation(self):self.defect('error-flag')
    def test_mirrored_divergence(self):self.defect('fat-divergence')
    def test_chain_loop(self):self.defect('chain-corruption')
    def test_torn_directory_half_sector(self):self.defect('torn-sector')

    def test_fat32_reserved_nibble_preserved_and_active_fat_refused(self):
        manifest=runner.Host().fat_fixture({'fat_type':32,'seed':1},self.root,0)
        image=Path(manifest['path'])
        with image.open('r+b') as stream:
            g=fat_fixtures.geometry(stream.read(512))
            for copy in range(2):
                stream.seek((g['reserved']+copy*g['fat_sectors'])*512+4)
                stream.write((0xafffffff).to_bytes(4,'little'))
        changes=fat_fixtures.corrupt(image,'dirty')
        for item in changes['patches']:self.assertEqual(int.from_bytes(bytes.fromhex(item['after_hex']),'little')>>28,10)
        with image.open('r+b') as stream:
            stream.seek(40);stream.write(b'\x80\x00')
        with self.assertRaisesRegex(res.Refusal,'mirroring'):fat_fixtures.corrupt(image,'fat-divergence')

    def test_real_overlay_marker_mtools_and_immutable_backing(self):
        if not all(shutil.which(tool) for tool in ('qemu-img','qemu-io')):self.skipTest('QEMU image tools unavailable')
        manifest=runner.Host().fat_fixture({'fat_type':32,'seed':1},self.root,0)
        volume=Path(manifest['path']);disk=self.root/'disk.raw'
        # Format the partitioned disk independently: no whole-image copies.
        with disk.open('wb') as output:output.truncate(volume.stat().st_size+512)
        subprocess.run(['mkfs.fat','--invariant','--offset=1','-F','32','-i','00000001',str(disk)],check=True,capture_output=True)
        subprocess.run(['mcopy','-i',str(disk)+'@@512',str(self.root/'fixture-0.txt'),'::/Ciuki long fixture.txt'],check=True,capture_output=True)
        mbr=bytearray(512);mbr[510:]=b'\x55\xaa';struct.pack_into('<II',mbr,454,1,volume.stat().st_size//512)
        with disk.open('r+b') as stream:stream.write(mbr)
        baseline=runner.sha(disk);overlay=self.root/'marker.qcow2'
        subprocess.run(['qemu-img','create','-f','qcow2','-b',str(disk),'-F','raw',str(overlay)],check=True,capture_output=True)
        item=place_marker(runner.Host(),overlay,self.root,disk.stat().st_size,'F109CUT.ARM')
        self.assertEqual(runner.sha(disk),baseline);self.assertEqual(item['size'],0)
        sector=runner.Host().overlay_read(overlay,item['sector']*512,512)
        entry=sector[item['offset']:item['offset']+32]
        self.assertEqual(entry[:11],b'F109CUT ARM');self.assertEqual(entry[20:22]+entry[26:32],bytes(8))
        # Expose only the changed sector to mtools on the disposable fixture,
        # avoiding FUSE permissions and any whole-overlay export.
        with volume.open('r+b') as stream:stream.seek((item['sector']-1)*512);stream.write(sector)
        listed=subprocess.run(['mdir','-i',str(volume),'::/F109CUT.ARM'],check=True,capture_output=True,text=True)
        self.assertIn('F109CUT',listed.stdout)
        self.assertEqual(runner.Host().file_digest(volume,self.root,'/F109CUT.ARM'),EMPTY)
        self.assertEqual(runner.Host().file_digest(volume,self.root,'/Ciuki long fixture.txt'),
                         {'size':4608,'sha256':runner.sha(self.root/'fixture-0.txt')})
        self.assertEqual(subprocess.run(['fsck.fat','-n',str(volume)],capture_output=True).returncode,0)
        with self.assertRaisesRegex(res.Refusal,'already exists'):
            place_marker(runner.Host(),overlay,self.root,disk.stat().st_size,'F109CUT.ARM')
        self.assertEqual(runner.sha(disk),baseline)

    def test_actual_blkdebug_node_breaks_only_once_per_guest_write(self):
        if not all(shutil.which(tool) for tool in ('qemu-img','qemu-io')):self.skipTest('QEMU image tools unavailable')
        disk=self.root/'tiny.raw';disk.write_bytes(bytes(4096));overlay=self.root/'tiny.qcow2'
        subprocess.run(['qemu-img','create','-f','qcow2','-b',str(disk),'-F','raw',str(overlay)],check=True,capture_output=True)
        args=['qemu-io','--image-opts', 'driver=blkdebug,image.driver=qcow2,image.file.driver=file,image.file.filename='+str(overlay)]
        commands=['break pwritev ciuki-write','aio_write -P 90 0 512','wait_break ciuki-write',
                  'break pwritev ciuki-write','resume ciuki-write','aio_flush',
                  'aio_write -P 91 512 512','wait_break ciuki-write','resume ciuki-write','aio_flush']
        for command in commands:args+=['-c',command]
        result=subprocess.run(args,capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual(result.stdout.count("Suspended request 'ciuki-write'"),2)
        self.assertEqual(runner.Host().overlay_read(overlay,0,1024),b'Z'*512+b'['*512)

    def test_real_orphan_checker_and_mtools_preserve_unrepaired_volume(self):
        image=self.root/'check-volume.raw'
        with image.open('wb') as stream:stream.truncate(64*1024**2)
        subprocess.run(['mkfs.fat','--invariant','-F','32',str(image)],check=True,capture_output=True)
        empty=self.root/'empty';empty.touch()
        subprocess.run(['mcopy','-i',str(image),str(empty),'::/F109CUT.ARM'],check=True,capture_output=True)
        with image.open('r+b') as stream:
            g=fat_fixtures.geometry(stream.read(512))
            root=g['data']+(g['root_cluster']-2)*g['spc']
            stream.seek(root*512);sector=bytearray(stream.read(512))
            slot=next(i for i in range(0,480,32) if not sector[i])
            # The short owner is still zero: persist only one valid LFN slot,
            # with ordinal 0x41, attribute 0x0f and no owned cluster.
            alias=b'F109CUT BIN';checksum=0
            for value in alias:checksum=(((checksum&1)<<7)+(checksum>>1)+value)&255
            entry=bytearray(32);entry[0]=0x41;entry[11]=15;entry[13]=checksum
            for i,offset in enumerate((1,3,5,7,9,14,16,18,20,22,24,28,30)):
                name='F109CUT.BIN'
                struct.pack_into('<H',entry,offset,ord(name[i]) if i<len(name) else 0 if i==len(name) else 0xffff)
            sector[slot:slot+32]=entry
            self.assertEqual(sector[slot+32:slot+64],bytes(32))
            stream.seek(root*512);stream.write(sector)
            for copy in range(2):
                offset=(g['reserved']+copy*g['fat_sectors'])*512+4
                stream.seek(offset);flags=int.from_bytes(stream.read(4),'little')
                stream.seek(offset);stream.write((flags&~0x08000000).to_bytes(4,'little'))
        baseline=runner.sha(image);host=runner.Host()
        checked=host.checker(['fsck.fat','-n',str(image)],self.root)
        self.assertIn('Orphaned long file name part "F109CUT.BIN"\n  Auto-deleting.\n',checked['output'])
        case=next(c for c in runner.load(ROOT/'tests/suites/f1-fat32.json')['cases'] if c['id']=='mount-crash-reboot')
        checks=case['boots'][0]['checks']
        classification=classify_crash_checker(checked,checks['interrupted_files'],checks['interrupted_patterns'])
        reports=[];check_crash_files(host,image,self.root,classification,reports)
        self.assertTrue(reports[-1]['absent'])
        self.assertEqual(host.file_digest(image,self.root,'/F109CUT.ARM'),EMPTY)
        self.assertEqual(runner.sha(image),baseline)
        self.assertEqual(host.checker(['fsck.fat','-n',str(image)],self.root)['output'],checked['output'])


class CrashRunnerTests(unittest.TestCase):
    setUp=existing.RunnerTests.setUp
    tearDown=existing.RunnerTests.tearDown
    run_fake=existing.RunnerTests.run_fake

    def scenario(self):
        self.case=copy.deepcopy(next(c for c in runner.load(ROOT/'tests/suites/f1-fat32.json')['cases'] if c['id']=='mount-crash-reboot'))
        self.case['timeout']=3
        self.image.write_bytes(bytes(4096))
        for boot in self.case['boots']:
            boot['checks'].update(offset=0,size=4096)
        records=existing.F1RecordTests().fat_fixtures()
        def body(text):return dict(word.split('=',1) for word in text.split())
        cuts=[body(r) for r in records['mount-crash-cut']]
        for record in cuts[1:]:record['lba']='1'
        reboot=[{'event':'BEGIN'},*[{'event':'DATA',**body(r)} for r in records['mount-crash-reboot']],{'event':'END','status':'PASS'}]
        return {'boot_records':[[],reboot],
                'boot_gates':[[[{'event':'BEGIN'},
                                {'event':'DATA','group':'storage','mode':'ro','writes':'0'},
                                {'event':'DATA','case':'corrupt_fixtures','status':'not_run','reason':'absent'},
                                cuts[0]], [{'event':'DATA',**cuts[1]}], [{'event':'DATA',**cuts[2]}]],[]]}

    def run_sequence(self, scenario, bad_checker=False, bad_digest=False, bad_file=False,
                     checker_output=None, listed_files=None, file_sizes=None):
        order=[]
        def marker(host,overlay,directory,size,name):
            self.assertIsNone(host.process);order.append('marker');return {'file':name,'sector':1,'size':0}
        @contextmanager
        def export(host,overlay,directory,offset,size):
            self.assertIsNotNone(host.process.poll());self.assertEqual(host.scope_pids(host.cgroup),[])
            order.append('export');yield self.image
        def check(host,args,directory):
            order.append(args[0])
            output='F109CUT ARM'
            if '-n' in args:
                output='fsck.fat 4.2 (2021-01-31)\nDirty bit is set. Fs was not properly unmounted and some data may be corrupt.\n Automatically removing dirty bit.\nLeaving filesystem unchanged.\n'+str(directory/'check-volume.raw')+': 2 files, 1/100000 clusters\n'
                if checker_output is not None:
                    selected=checker_output[order.count('fsck.fat')-1] if isinstance(checker_output,list) else checker_output
                    output=selected.format(volume=directory/'check-volume.raw')
                if bad_checker:output+='Cross-linked clusters\n'
            elif '-b' in args:
                selected=checker_output[order.count('fsck.fat')-1] if isinstance(checker_output,list) else checker_output
                files=['::/F109CUT.ARM']
                if selected is None or 'Orphaned long file name part' not in selected:files.append('::/F109CUT.BIN')
                output='\n'.join(files if listed_files is None else listed_files)+'\n'
            return {'arguments':args,'returncode':1 if '-n' in args else 0,'output':output,'output_sha256':hashlib.sha256(output.encode()).hexdigest()}
        def digest(host,overlay,directory,lba,count,fmt):
            self.assertIsNotNone(host.process.poll());order.append('digest')
            return {'size':512,'sha256':('b' if bad_digest and order.count('digest')==2 else 'a')*64}
        def file_digest(host,volume,directory,path):
            size=(file_sizes or {}).get(path,1 if bad_file else 0)
            return {'size':size,'sha256':'b'*64} if size else EMPTY
        with patch.object(runner,'place_marker',marker),patch.object(existing.FakeHost,'export_readonly',export), \
             patch.object(existing.FakeHost,'checker',check),patch.object(existing.FakeHost,'sector_digest',digest), \
             patch.object(existing.FakeHost,'file_digest',file_digest):
            result,directory=self.run_fake(**scenario)
        return result,directory,order

    def test_marker_gated_cut_checker_reboot_and_comparison(self):
        result,directory,order=self.run_sequence(self.scenario())
        self.assertEqual(result['outcome'],'pass',result['reason'])
        self.assertEqual(order[0],'marker');self.assertEqual(order.count('export'),2)
        self.assertEqual(result['crash_comparison']['index'],2)
        self.assertEqual(result['crash_comparison']['durable_sectors'],[1])
        boots=result['reboot_sequence'];self.assertEqual(len({b['overlay'] for b in boots}),1)
        self.assertEqual(boots[0]['marker_manifest'][0]['file'],'F109CUT.ARM')
        self.assertEqual(boots[1]['marker_manifest'],[])
        self.assertTrue(boots[0]['cut_point']['next_write_suspended'])
        observations=boots[0]['cut_point']['gate_observations']
        self.assertEqual([o['completed_index'] for o in observations],[0,1,2])
        self.assertTrue(all(o['armed'] for o in observations))
        self.assertTrue(all(b['cleanup']['clean'] for b in boots))
        self.assertFalse(Path(boots[0]['overlay']).exists())
        self.assertEqual(set(p.name for p in directory.iterdir()),{'serial.log','result.json'})

    def test_captured_orphan_reaches_boot_two_and_preserves_evidence(self):
        result,_,order=self.run_sequence(self.scenario(),checker_output=existing.CRASH_ORPHAN_FSCK)
        self.assertEqual(result['outcome'],'pass',result['reason'])
        self.assertEqual(result['unattempted_boots'],0)
        self.assertEqual(order.count('export'),2)
        boots=result['reboot_sequence'];self.assertEqual(len(boots),2)
        self.assertEqual(boots[0]['crash_overlay'],boots[1]['crash_overlay'])
        for boot in boots:
            report=next(r for r in boot['checkers'] if r['kind']=='fsck.fat')
            self.assertEqual(report['returncode'],1)
            self.assertEqual(report['interrupted_outcomes'],
                             {'dirty':True,'files':[{'path':'::/F109CUT.BIN','state':'orphan-lfn'}]})
            self.assertTrue(any(r.get('absent') for r in boot['checkers'] if r.get('path')=='::/F109CUT.BIN'))
        self.assertTrue(result['crash_comparison']['checkers_match'])

    def test_checker_and_file_state_must_agree_before_reboot(self):
        for output,files,sizes in ((existing.CRASH_ORPHAN_FSCK,['::/F109CUT.ARM','::/F109CUT.BIN'],None),
                                   (None,['::/F109CUT.ARM'],None),
                                   (None,None,{'/F109CUT.BIN':1}),
                                   (existing.CRASH_ORPHAN_FSCK.replace('F109CUT.BIN','OTHER.BIN'),None,None),
                                   (existing.CRASH_ORPHAN_FSCK.replace('  Auto-deleting.\n',''),None,None),
                                   (existing.CRASH_ORPHAN_FSCK+'Cross-linked clusters\n',None,None)):
            with self.subTest(output=output,files=files,sizes=sizes):
                self.script.with_suffix('.count').unlink(missing_ok=True)
                result,_,_=self.run_sequence(self.scenario(),checker_output=output,listed_files=files,file_sizes=sizes)
                self.assertEqual(result['outcome'],'fail')
                self.assertEqual(result['unattempted_boots'],1)
                self.assertTrue(Path(result['reboot_sequence'][0]['overlay']).exists())

    def test_changed_orphan_outcome_between_boots_fails(self):
        empty=existing.CRASH_ORPHAN_FSCK.replace('Orphaned long file name part "F109CUT.BIN"\n  Auto-deleting.\n','')
        result,_,_=self.run_sequence(self.scenario(),checker_output=[existing.CRASH_ORPHAN_FSCK,empty])
        self.assertEqual(result['outcome'],'fail')
        self.assertEqual(result['unattempted_boots'],0)
        self.assertIn('crash checker outcomes differ between boots',result['reason'])

    def test_breakpoint_follows_setup_and_arm_and_targets_drive(self):
        result,directory,_=self.run_sequence(self.scenario())
        self.assertEqual(result['outcome'],'pass',result['reason'])
        boot=json.loads((Path(result['reboot_sequence'][0]['overlay']).parent/'result.json').read_text())
        drive=boot['qemu']['arguments'][boot['qemu']['arguments'].index('-drive')+1]
        self.assertIn('id=ciuki-cut-drive',drive)
        self.assertEqual(boot['observed_blockstats_armed']['ide0']['wr_operations'],5)
        self.assertEqual(boot['initial_blockstats']['ide0']['wr_operations'],0)

    def test_arm_receipt_lag_preserves_exact_prefix_or_rejects_overshoot(self):
        for lag in (1,2,3):
            with self.subTest(lag=lag):
                self.script.with_suffix('.count').unlink(missing_ok=True)
                scenario=self.scenario();scenario['gate_arm_lag_writes']=lag
                last=scenario['boot_gates'][0][-1][0]
                scenario['boot_gates'][0].append([{**last,'index':'3'}])
                result,_,_=self.run_sequence(scenario)
                if lag<=2:
                    self.assertEqual(result['outcome'],'pass',result['reason'])
                    self.assertEqual(result['crash_comparison']['index'],2)
                else:
                    self.assertEqual(result['outcome'],'fail')
                    self.assertIn('declared cut index already passed',result['reason'])
                    self.assertIsNone(result['cut_point'])

    def test_timeout_identifies_pending_cut_and_arm(self):
        for phase in ('arm','index','suspension','qmp'):
            with self.subTest(phase=phase):
                self.script.with_suffix('.count').unlink(missing_ok=True)
                scenario=self.scenario();self.case['timeout']=.5
                gates=scenario['boot_gates'][0]
                if phase=='arm':gates[0]=gates[0][:-1]
                elif phase=='index':scenario['boot_gates'][0]=gates[:1]
                elif phase=='suspension':
                    scenario.pop('boot_gates');scenario['boot_records'][0]=[r for batch in gates for r in batch]
                else:scenario['gate_qmp_timeout']=True
                result,_,_=self.run_sequence(scenario)
                self.assertEqual(result['outcome'],'fail')
                self.assertTrue(result['timeout']['occurred'])
                pending=result['timeout']['pending_stimulus']
                self.assertEqual(pending['action']['type'],'cut')
                self.assertEqual(pending['action']['after'],{'event':'ARM','action':'crash_cut'})
                self.assertEqual(pending['declared_cut_index'],2)
                expected=('waiting_for_arm' if phase=='arm' else 'arming_gate' if phase=='qmp' else
                          'waiting_for_cut_index' if phase=='index' else 'waiting_for_write_suspension')
                self.assertEqual(pending['phase'],expected)
                self.assertIn('pending declared stimulus',result['reason'])
                self.assertIn(expected,result['reason'])
                self.assertEqual(result['stimulus']['pending'],pending)
                self.assertEqual(result['stimulus']['observed'],[])
                self.assertTrue(result['cleanup']['clean'])

    def test_cross_link_and_file_outcome_stop_before_reboot(self):
        for option in ('bad_checker','bad_file'):
            with self.subTest(option=option):
                self.script.with_suffix('.count').unlink(missing_ok=True)
                result,_,_=self.run_sequence(self.scenario(),**{option:True})
                self.assertEqual(result['outcome'],'fail')
                self.assertEqual(result['unattempted_boots'],1)
                self.assertTrue(Path(result['reboot_sequence'][0]['overlay']).exists())

    def test_changed_directory_and_missing_refusal_never_pass(self):
        for option in ('digest','refusal'):
            with self.subTest(option=option):
                self.script.with_suffix('.count').unlink(missing_ok=True)
                scenario=self.scenario()
                if option=='refusal':scenario['boot_records'][1]=[r for r in scenario['boot_records'][1] if r.get('case')!='crash_refusal']
                result,_,_=self.run_sequence(scenario,bad_digest=option=='digest')
                self.assertEqual(result['outcome'],'fail')
                self.assertTrue(Path(result['reboot_sequence'][0]['overlay']).exists())

    def test_missing_arm_skipped_index_overshoot_and_volatile_write_fail(self):
        for option in ('arm','index','overshoot','durable','suspension'):
            with self.subTest(option=option):
                self.script.with_suffix('.count').unlink(missing_ok=True)
                scenario=self.scenario();gates=scenario['boot_gates'][0]
                if option=='arm':gates[0]=gates[0][:-1];self.case['timeout']=.5
                if option=='index':gates[1][0]['index']='2'
                if option=='overshoot':gates[2].append({**gates[2][0],'index':'3'})
                if option=='durable':gates[1][0]['durable']='0'
                if option=='suspension':
                    scenario.pop('boot_gates');scenario['boot_records'][0]=[r for batch in gates for r in batch]
                    self.case['timeout']=.5
                result,_,_=self.run_sequence(scenario)
                self.assertEqual(result['outcome'],'fail')
                self.assertEqual(result['unattempted_boots'],1)


if __name__=='__main__':unittest.main()
