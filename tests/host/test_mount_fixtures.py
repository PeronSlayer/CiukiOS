"""Real FAT host tools and scripted emulator boundaries; no QEMU guest."""
import copy
from contextlib import contextmanager
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import test_runner as existing
import fat_fixtures
from mount_fixtures import place_marker
import run as runner
import resources as res

ROOT = existing.ROOT
EMPTY = {'size':0, 'sha256':hashlib.sha256(b'').hexdigest()}


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
                'boot_gates':[[[{'event':'BEGIN'}], [cuts[0]], [{'event':'DATA',**cuts[1]}], [{'event':'DATA',**cuts[2]}]],[]]}

    def run_sequence(self, scenario, bad_checker=False, bad_digest=False, bad_file=False):
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
                if bad_checker:output+='Cross-linked clusters\n'
            return {'arguments':args,'returncode':1 if '-n' in args else 0,'output':output,'output_sha256':hashlib.sha256(output.encode()).hexdigest()}
        def digest(host,overlay,directory,lba,count,fmt):
            self.assertIsNotNone(host.process.poll());order.append('digest')
            return {'size':512,'sha256':('b' if bad_digest and order.count('digest')==2 else 'a')*64}
        with patch.object(runner,'place_marker',marker),patch.object(existing.FakeHost,'export_readonly',export), \
             patch.object(existing.FakeHost,'checker',check),patch.object(existing.FakeHost,'sector_digest',digest), \
             patch.object(existing.FakeHost,'file_digest',return_value={'size':1,'sha256':'b'*64} if bad_file else EMPTY):
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
        self.assertTrue(all(b['cleanup']['clean'] for b in boots))
        self.assertFalse(Path(boots[0]['overlay']).exists())
        self.assertEqual(set(p.name for p in directory.iterdir()),{'serial.log','result.json'})

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
                if option=='arm':gates[1]=[]
                if option=='index':gates[2][0]['index']='2'
                if option=='overshoot':gates[3].append({**gates[3][0],'index':'3'})
                if option=='durable':gates[2][0]['durable']='0'
                if option=='suspension':scenario.pop('boot_gates');scenario['boot_records'][0]=[{'event':'BEGIN'},*gates[1],*gates[2],*gates[3]]
                result,_,_=self.run_sequence(scenario)
                self.assertEqual(result['outcome'],'fail')
                self.assertEqual(result['unattempted_boots'],1)


if __name__=='__main__':unittest.main()
