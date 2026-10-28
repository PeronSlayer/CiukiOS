"""SVGA3D backend (f7-31): UBSan by default, CIUKI_DISPLAY_ASAN=1 adds ASan/LSan.

No sanitizer or leak-detection option is disabled, matching test_render3d.py.
Host-only evidence: a strict host model, never QEMU or VirtualBox.
"""
from pathlib import Path
import os
import re
import runpy
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'build/host/vmwaresvga3d'
DRIVER='src/kernel/drivers/video/vmwaresvga/'
SOURCES=['tests/host/vmwaresvga3d_model.c',DRIVER+'vmwaresvga3d.c',DRIVER+'engine.c',
 'src/kernel/drivers/video/render3d.c','src/kernel/drivers/video/render3d_scalar.c']
PINNED=ROOT/'build/downloads/f7-sources'
def sanitizer():
    return 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
def build(name,sources):
    OUT.mkdir(parents=True,exist_ok=True)
    binary=OUT/name
    subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize='+sanitizer(),
        '-fno-sanitize-recover=all','-I',str(ROOT/'src/kernel/include'),*[str(ROOT/s) for s in sources],
        '-lm','-o',str(binary)],check=True)
    return binary
class Svga3DTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary=build('backend',['tests/host/vmwaresvga3d_backend_test.c',*SOURCES])
    def run_mode(self,*args):
        result=subprocess.run([str(self.binary),*args],capture_output=True,text=True,timeout=1200)
        print(result.stdout,end='')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS SVGA3D',result.stdout)
    def test_differential(self): self.run_mode('differential')
    def test_qualification(self): self.run_mode('qualification')
    def test_budget(self): self.run_mode('budget')
    def test_fuzz(self): self.run_mode('fuzz')
    def test_unsupported_features(self): self.run_mode('unsupported')
    def test_service(self): self.run_mode('service')
    def test_eviction(self): self.run_mode('eviction')
    def test_hosts(self): self.run_mode('hosts')
    def test_ieee(self): self.run_mode('ieee')
    def test_layouts(self): self.run_mode('layouts')
    def test_faults(self):
        for variant in range(5): self.run_mode('fault',str(variant))
    def test_owner(self):
        binary=build('owner',['tests/host/vmwaresvga3d_owner_test.c',*SOURCES,DRIVER+'vmwaresvga3d_owner.c',
            'src/kernel/drivers/video/attempt.c','src/kernel/lib/fmt.c'])
        for variant in range(21):
            result=subprocess.run([str(binary),str(variant)],capture_output=True,text=True,timeout=1200)
            print(result.stdout,end='')
            self.assertEqual(result.returncode,0,f'variant {variant}: '+result.stdout+result.stderr)
    def test_target(self):
        flags=runpy.run_path(str(ROOT/'scripts/build_kernel.py'))['CFLAGS']
        builder=(ROOT/'scripts/build_kernel.py').read_text()
        symbols=set()
        for name in ('engine.c','driver.c','io.c','vmwaresvga3d.c','vmwaresvga3d_owner.c'):
            self.assertIn('"vmwaresvga/'+name+'"',builder)
            obj=OUT/('target-'+name+'.o')
            OUT.mkdir(parents=True,exist_ok=True)
            subprocess.run(['clang',*flags,'-I',str(ROOT/'src/kernel/include'),'-c',str(ROOT/DRIVER/name),'-o',str(obj)],check=True)
            for line in subprocess.check_output(['nm','--defined-only','--extern-only',str(obj)],text=True).splitlines():
                symbol=line.split()[-1]; self.assertNotIn(symbol,symbols); symbols.add(symbol)
            if name.startswith('vmwaresvga3d'):
                dis=subprocess.check_output(['objdump','-d',str(obj)],text=True)
                self.assertNotRegex(dis,r'\b(?:xmm\d|mm[0-7]|fld|fst|fadd|fmul|fdiv)\b')
        print(f'PASS SVGA3D i686: five objects, {len(symbols)} unique symbols, frame limit, no FPU/SIMD in 3D objects')
    def test_protocol_citations(self):
        # Every numeric protocol value names its pinned header line; check
        # both files when the pinned downloads are present on this host.
        text=(ROOT/DRIVER/'vmwaresvga3d_protocol.h').read_text()
        rows=re.findall(r'#define (VMSVGA3D_\w+) (0x[0-9a-f]+|\d+)u\s+/\* mesa:(\S+):(\d+)(?: linux:(\S+):(\d+))? (\w+)',text)
        self.assertGreater(len(rows),90)
        roots={'mesa':PINNED/'mesa-21.3.9-svga/include','linux':PINNED/'linux-6.1-vmwgfx'}
        if not all(p.is_dir() for p in roots.values()):
            self.skipTest('pinned sources not downloaded on this host (see docs/design/f7-family-vmware-svga3d.md)')
        checked=0
        for name,value,mfile,mline,lfile,lline,upstream in rows:
            for root,f,line in (('mesa',mfile,mline),('linux',lfile,lline)):
                if not f: continue
                source=(roots[root]/f).read_text(errors='replace').splitlines()[int(line)-1]
                self.assertIn(upstream,source,f'{name}: {root}:{f}:{line}')
                number=re.search(r'=\s*(0x[0-9A-Fa-f]+|\d+)|<<\s*(\d+)|\b(0x[0-9A-Fa-f]+|\d+)\s*$|define\s+\w+\s+(\(?(?:0x[0-9A-Fa-f]+|\d+))',source)
                literal=int(value,0)
                if number and number.group(1) and 'MAKE_HWVERSION' not in source:
                    self.assertEqual(int(number.group(1),0),literal,f'{name}: {root}:{f}:{line}')
                checked+=1
        print(f'PASS SVGA3D protocol: {len(rows)} constants, {checked} pinned line citations')
    def test_precision_copy(self):
        # Verbatim f7-11 envelope until the f7-33 consolidation (f7-20 section E).
        savage=(ROOT/'src/kernel/drivers/video/s3savage/savage3d_precision.h').read_text().splitlines()[6:]
        copy=(ROOT/DRIVER/'vmwaresvga3d_precision.h').read_text().splitlines()[8:]
        self.assertEqual(savage,copy)
if __name__=='__main__':unittest.main()
