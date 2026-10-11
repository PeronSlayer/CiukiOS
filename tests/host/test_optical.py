"""Production optical parser and canonical-runner optical host evidence."""
import unittest
import subprocess
import tempfile
import shutil
import hashlib
import sys
import os
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/test'))
import iso_fixtures
import run as runner

class Optical(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('xorriso') or not shutil.which('clang'): raise unittest.SkipTest('optical host tools absent')
        out=ROOT/'build/host/optical'
        out.mkdir(parents=True,exist_ok=True)
        cls.binary=out/'test_optical'
        sources=['tests/host/fs/test_optical.c','src/kernel/fs/fs_port.c','src/kernel/fs/cache.c',
                 'src/kernel/fs/path.c','src/kernel/fs/fat.c','src/kernel/fs/vfs.c',
                 'src/kernel/fs/fs_ro.c','src/kernel/fs/iso9660.c','src/kernel/lib/sha256.c']
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                        '-DFS_HOST','-D_POSIX_C_SOURCE=200809L','-pthread','-Isrc/kernel/include','-Isrc/kernel/fs',
                        *sources,'-o',str(cls.binary)],cwd=ROOT,check=True,capture_output=True)
        cls.temp=tempfile.TemporaryDirectory(dir=out,prefix='fixtures-')
        cls.directory=Path(cls.temp.name)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def check_image(self,manifest,refused=False):
        before=iso_fixtures.sha(manifest['path'])
        result=subprocess.run([str(self.binary),manifest['path']],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
        self.assertEqual(result.returncode,0,result.stdout+'\n'+result.stderr)
        self.assertEqual(before,iso_fixtures.sha(manifest['path']))
        lines=result.stdout.splitlines()
        if refused:
            self.assertTrue(any(s.startswith('REFUSED\t') for s in lines),result.stdout)
        else:
            self.assertFalse(any(s.startswith('REFUSED') for s in lines),result.stdout)
            actual={}
            for line in lines:
                fields=line.split('\t')
                if fields[0]=='D': actual[fields[1]]={'kind':'directory'}
                if fields[0]=='F': actual[fields[1]]={'kind':'file','size':int(fields[2]),'sha256':fields[3]}
            expected={e['path']:{k:v for k,v in e.items() if k in ('kind','size','sha256')} for e in manifest['entries']}
            self.assertEqual(expected,actual)
            self.assertTrue(any(s.startswith('COUNTERS\twrites=0\tflushes=0') for s in lines))
        return result.stdout

    def test_levels_joliet_determinism(self):
        for level in (1,2,3):
            for joliet in (False,True):
                with self.subTest(level=level,joliet=joliet):
                    spec={'level':level,'joliet':joliet}
                    a=iso_fixtures.generate(spec,self.directory,level*10+joliet)
                    b=iso_fixtures.generate(spec,self.directory,level*10+joliet+100)
                    self.assertEqual(a['sha256'],b['sha256'])
                    self.check_image(a)

    def test_malformed_descriptor(self):
        a=iso_fixtures.generate({'malformed':True},self.directory,300)
        self.check_image(a,True)
        for corruption in ('endian','terminator','joliet'):
            a=iso_fixtures.generate({'joliet':True},self.directory,{'endian':301,'terminator':302,'joliet':303}[corruption])
            p=Path(a['path']); data=bytearray(p.read_bytes())
            if corruption=='endian':data[16*2048+84]^=1
            if corruption=='terminator':data[18*2048]=0
            if corruption=='joliet':data[17*2048+84]^=1
            p.write_bytes(data); self.check_image(a,True)

    def test_multi_extent_and_bounds(self):
        a=iso_fixtures.generate({'level':3},self.directory,400)
        p=Path(a['path']); data=bytearray(p.read_bytes())
        root=int.from_bytes(data[16*2048+158:16*2048+162],'little')*2048
        def records(base):
            offset=0
            while offset<2048 and data[base+offset]:
                length=data[base+offset]; yield base+offset
                offset+=length
        directory=next(r for r in records(root) if data[r+33:r+33+data[r+32]]==b'DATA')
        base=int.from_bytes(data[directory+2:directory+6],'little')*2048
        target=next(r for r in records(base) if data[r+33:r+33+data[r+32]]==b'ODD.BIN;1')
        length=data[target]; original=bytes(data[target:target+length]); extent=int.from_bytes(original[2:6],'little')
        # Insert a second contiguous record, preserving later siblings.
        following=bytes(data[target+length:base+2048-length])
        data[target+2*length:base+2048]=following
        data[target+length:target+2*length]=original
        def both(at,x):data[at:at+8]=x.to_bytes(4,'little')+x.to_bytes(4,'big')
        both(target+10,2048); data[target+25]|=128
        both(target+length+2,extent+1); both(target+length+10,4099-2048)
        p.write_bytes(data); self.check_image(a)
        both(target+length+2,len(data)//2048+20); p.write_bytes(data); self.check_image(a,True)

    def test_runner_slots_actions_and_manifest_refusal(self):
        with self.assertRaises(runner.res.Refusal):runner.fixture_disks({'fixtures':[{'generator':'mkfs.fat'}]*3+[{'generator':'xorriso'}]})
        fixture={'entries':[{'path':'/x','path_hex':'2f78','kind':'file','size':1,'sha256':'a'*64}]}
        records=[dict(case='entry',cycle=str(c),id='1',path_hex='2f78',kind='file') for c in (1,2)]
        records += [dict(case='file',cycle=str(c),id='1',size='1',sha256='a'*64) for c in (1,2)]
        iso_fixtures.verify_records(fixture,records)
        with self.assertRaises(ValueError):iso_fixtures.verify_records(fixture,records[:1])
        class Parser:terminal=False
        Parser.records=[{'event':'ARM','action':'media_reinsert','seq':'12'}]
        class Qmp:
            calls=[]
            def command(self,name,args):self.calls.append((name,args));return {}
        q=Qmp(); a=runner.Actions([{'type':'media','after':{'event':'ARM','action':'media_reinsert'},'operation':'change','device':'ciuki-optical','target':'fixture-2.iso'}])
        a.step(Parser,q,1); self.assertTrue(a.complete); self.assertEqual(q.calls[0][0],'blockdev-change-medium')
        self.assertEqual(runner.selector('f4:optical run=12345678','fw_cfg',True)['phase'],4)

if __name__=='__main__':unittest.main()
