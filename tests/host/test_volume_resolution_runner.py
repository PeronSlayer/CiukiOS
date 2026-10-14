"""F1-63 fixture topology and required on-disk/serial evidence, without QEMU."""
import copy
from contextlib import contextmanager
from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/test'))
import run as runner
from evidence import EvidenceError


class VolumeResolutionRunnerTests(unittest.TestCase):
    def cases(self):
        return [c for c in runner.load(ROOT/'tests/suites/f4-media.json')['cases']
                if c['id'].startswith('volume-resolution-')]

    def test_cases_use_real_usb_and_optical_frontends(self):
        runner.load_suite('f4-media')
        cases=self.cases();self.assertEqual(len(cases),2)
        for case in cases:
            runner.Actions(case['actions'])
            self.assertEqual(runner.fixture_disks(case),[])
            self.assertEqual(runner.fixture_usb_disks(case),[1])
            profile=runner.load(ROOT/'tests/profiles/qemu-t23.json')
            args,_=runner.qemu_args('qemu-system-i386',profile,case,'12345678',Path('system.qcow2'),Path('bios.bin'))
            self.assertIn('usb-storage,bus=ciuki-storage-usb.0,drive=ciuki-usb',args)
            self.assertIn('ide-cd,drive=ciuki-optical,bus=ide.1,unit=1',args)
            drives=[args[i+1] for i,a in enumerate(args) if a=='-drive']
            self.assertTrue(any('fixture-1.img' in s and 'if=none' in s for s in drives))
            self.assertFalse(any('fixture-1.img' in s and 'if=ide' in s for s in drives))
            self.assertTrue(case['expected']['serial_patterns'])
            usb_result='-22' if '-failed-' in case['id'] else '0'
            self.assertIn('result='+usb_result+'\\r?$',case['expected']['serial_patterns'][1])
            self.assertEqual(case['checks']['files'][0]['contains'],['[System\\Audio]\n"Master" = 70676\n'])
            keys=[e['data'] for b in case['actions'][0]['batches'] for e in b['events'] if e['type']=='key']
            held=set()
            for k in keys:
                code=k['key']['data']
                if k['down']:self.assertNotIn(code,held);held.add(code)
                else:self.assertIn(code,held);held.remove(code)
            self.assertFalse(held)

    def test_serial_requires_both_media_and_the_correct_mount_outcome(self):
        out=ROOT/'build/host';out.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out) as directory:
            path=Path(directory)/'serial.log'
            for case in self.cases():
                usb_result='-22' if '-failed-' in case['id'] else '0'
                records=['[storage] optical slot=3 drive=D mode=ro generation=1 result=3 reason=mounted',
                         '[usb-storage] mount addr=1 result='+usb_result,
                         '[launch-stage] name=dosbox pid=42 stage=vm-create window=CiukiDOS']
                path.write_text('\n'.join(records)+'\n')
                self.assertEqual(len(runner.check_serial_patterns(path,case['expected'])),3)
                for i in range(len(records)):
                    path.write_text('\n'.join(records[:i]+records[i+1:])+'\n')
                    with self.assertRaises(EvidenceError):runner.check_serial_patterns(path,case['expected'])

    def test_fixture_manifests_keep_transport_and_ide_numbers_separate(self):
        class Host:
            def fat_fixture(self,fixture,directory,index):
                return {'path':str(directory/('fixture-'+str(index)+'.img')),'sha256':'0'*64}
        for case in self.cases():
            # Exercise actual FAT generation and the real ISO generator in
            # the filesystem regression; keep this test at the runner boundary.
            from unittest.mock import patch
            with patch.object(runner.iso_fixtures,'generate',return_value={'path':'fixture-0.iso','sha256':'1'*64}):
                fixtures=runner.prepare_fixtures(Host(),case,Path('scratch'))['manifest']
            self.assertEqual(fixtures[0]['ide_index'],3)
            self.assertEqual(fixtures[1]['transport'],'usb')
            self.assertNotIn('ide_index',fixtures[1])

    def test_transport_refusals(self):
        for fixture in ({'generator':'xorriso','transport':'usb'},
                        {'generator':'mkfs.fat','transport':'scsi'}):
            with self.assertRaises(runner.res.Refusal):runner.fixture_disks({'fixtures':[fixture]})
        with self.assertRaises(runner.res.Refusal):
            runner.fixture_disks({'fixtures':[{'generator':'mkfs.fat','transport':'usb'}]*2})
        self.assertEqual(runner.fixture_disks({'fixtures':[{'generator':'mkfs.fat'},
                          {'generator':'xorriso'},{'generator':'mkfs.fat','transport':'usb'}]}),[(0,1)])

    def test_registry_evidence_rejects_unsaved_and_wrong_volume_content(self):
        class Host:
            text='[System\\Audio]\n"Master" = 70676\n'
            @contextmanager
            def export_readonly(self,*args):yield 'volume'
            def checker(self,args,directory):
                command=args[0]
                output=self.text if command=='mtype' else 'System' if command=='mdir' else 'fsck.fat 4.2 (2021-01-31)\nvolume: 10 files, 20/100 clusters\n'
                return {'returncode':0,'output':output,'output_sha256':'0'*64}
        host=Host();checks=self.cases()[0]['checks'];out=ROOT/'build/host';out.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out) as directory:
            result={'image':{'size':536870912},'checkers':[],'durability_observations':[]}
            runner.check_overlay(host,Path('overlay'),Path(directory),checks,result)
            for text in ('[System\\Audio]\n"Master" = 5140\n','[Hardware\\Audio]\n"Master" = 70676\n',
                         '[System\\Audio]\n"Master" = 5140\n[System\\Other]\n"Master" = 70676\n'):
                host.text=text
                with self.assertRaises(EvidenceError):runner.check_overlay(host,Path('overlay'),Path(directory),checks,result)
            wrong=copy.deepcopy(checks);wrong['files'][0]['contains']=[]
            with self.assertRaises(runner.res.Refusal):runner.check_overlay(host,Path('overlay'),Path(directory),wrong,result)


if __name__=='__main__':
    unittest.main()
