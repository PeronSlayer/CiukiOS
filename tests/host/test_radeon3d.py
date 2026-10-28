"""R100/R200 independent MMIO model. Sequential host tests, default leaks intact."""
from pathlib import Path
import importlib.util
import os
import subprocess
import sys
import re
import unittest
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'build/host/radeon3d'
FAMILY = 'src/kernel/drivers/video/radeon2d/'
class Radeon3DTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
    def build(self, name, sources):
        flags=['-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
            '-Wno-unused-variable','-fsanitize='+('address,undefined' if os.getenv('CIUKI_DISPLAY_ASAN') else 'undefined'),
            '-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
            '-I'+str(ROOT/'src/kernel/include')]
        binary=OUT/name
        subprocess.run(['clang',*flags,*[str(ROOT/s) for s in sources],'-lm','-o',str(binary)],check=True)
        return binary
    def backend(self):
        return self.build('backend',['tests/host/radeon3d_backend_test.c',FAMILY+'radeon3d.c',FAMILY+'engine.c',FAMILY+'cursor.c',
            'src/kernel/drivers/video/render3d.c','src/kernel/drivers/video/render3d_scalar.c','src/kernel/drivers/video/render3d_native.c'])
    def test_differential(self):
        subprocess.run([str(self.backend())],check=True)
    def test_fullsize(self):
        subprocess.run([str(self.backend()),'fullsize'],check=True)
    def test_owner(self):
        binary=self.build('owner',['tests/host/radeon3d_owner_test.c','src/kernel/lib/fmt.c',
            'src/kernel/drivers/video/attempt.c','src/kernel/drivers/video/render3d.c',
            'src/kernel/drivers/video/render3d_scalar.c',FAMILY+'radeon3d.c',FAMILY+'radeon3d_owner.c'])
        for variant in range(15):
            subprocess.run([str(binary),str(variant)],check=True)
    def test_firmware(self):
        spec=importlib.util.spec_from_file_location('image',ROOT/'scripts/build_image.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        payloads=module.firmware_payloads()
        self.assertEqual(len(payloads),4)
        for name in ('R100_cp.bin','R200_cp.bin'):
            self.assertEqual(payloads['/System/firmware/'+name].stat().st_size,2048)
        self.assertIn('SPDX-License-Identifier: MIT',payloads['/System/firmware/radeon-WHENCE.txt'].read_text())
    def test_h1(self):
        # Same copied Win32 GL transport and private deterministic H1 fixture.
        # The fixture calls the Radeon MMIO decoder, never the scalar engine.
        sys.path.insert(0,str(ROOT))
        spec=importlib.util.spec_from_file_location('radeon_gl',ROOT/'tests/host/win32/gl/test_gl.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        GLTests,check_quake3=module.GLTests,module.check_quake3
        GLTests.build(sanitize=True,kernel_family='radeon')
        result=subprocess.run([str(GLTests.binary),'--hardware-quake3'],capture_output=True,text=True,timeout=2400)
        (OUT/'h1.log').write_text(result.stdout+result.stderr)
        print(result.stdout,end='')
        self.assertEqual(result.returncode,0,result.stderr)
        check_quake3(result.stdout,sanitize=True)
        rows=re.findall(r'^GL_RADEON_H1 .*gpu_draws=(\d+) gpu_fragments=(\d+) cpu_draws=(\d+) cpu_fragments=(\d+).* switches=(\d+) ',result.stdout,re.M)
        self.assertEqual(len(rows),12)
        for gpu,fragments,cpu,cpu_fragments,switches in rows:
            self.assertIn(int(gpu),(3000,4600))
            self.assertGreater(int(fragments),0)
            self.assertEqual((int(cpu),int(cpu_fragments),int(switches)),(0,0,0))
    def test_target(self):
        spec=importlib.util.spec_from_file_location('flags',ROOT/'scripts/build_kernel.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        objects=[]
        for name in ('engine.c','cursor.c','driver.c','radeon3d.c','radeon3d_owner.c'):
            obj=OUT/(name+'.o');objects.append(obj)
            subprocess.run(['clang',*module.CFLAGS,'-I',str(ROOT/'src/kernel/include'),'-c',str(ROOT/FAMILY/name),'-o',str(obj)],check=True)
            dis=subprocess.check_output(['objdump','-d',str(obj)],text=True)
            self.assertNotRegex(dis,r'\b(?:xmm\d|mm[0-7]|fld|fst|fadd|fmul|fdiv)\b')
        symbols=subprocess.check_output(['nm','-g','--defined-only',*map(str,objects)],text=True)
        names=[line.split()[-1] for line in symbols.splitlines() if len(line.split())==3]
        self.assertEqual(len(names),len(set(names)))
        print(f'i686 target: {len(objects)} objects; frame <=4096; no FP/SIMD; {len(names)} unique external symbols PASS')
if __name__=='__main__': unittest.main()
