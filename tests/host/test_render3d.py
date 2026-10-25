"""F7 universal driver service and shared-owner transport. Sequential, no image.

UBSan default, matching display family suites. CIUKI_DISPLAY_ASAN=1 adds ASan
with default leak detection. No sanitizer option is disabled by this runner.
"""
from pathlib import Path
import ast
import os
import re
import subprocess
import unittest
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/host/render3d'
VIDEO = ROOT / 'src/kernel/drivers/video'

class RenderTests(unittest.TestCase):
    def build_run(self, name, sources, args=()):
        OUT.mkdir(parents=True, exist_ok=True)
        sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
        flags = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize='+sanitizer, '-fno-sanitize-recover=undefined',
                 '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                 '-I', str(ROOT/'src/kernel/include'), '-I', str(ROOT/'tests/host'), '-I', str(OUT)]
        binary = OUT/name
        subprocess.run(flags+[str(ROOT/p) for p in sources]+['-lm','-o',str(binary)],check=True)
        subprocess.run([str(binary),*args],check=True)

    def test_service(self):
        self.build_run('test', ['tests/host/render3d_test.c',
            'src/kernel/drivers/video/render3d.c', 'src/kernel/drivers/video/render3d_scalar.c',
            'src/kernel/drivers/video/s3savage/savage3d.c'])

    def test_scalar_recovery(self):
        self.build_run('scalar_recovery', ['tests/host/render3d_scalar_test.c'])

    def test_shared_transport(self):
        OUT.mkdir(parents=True, exist_ok=True)
        source=(ROOT/'tests/host/savage2d_test.c').read_text()
        signature='\nint main(void)\n'
        self.assertEqual(source.count(signature),1)
        (OUT/'savage2d_fixture.inc').write_text(source.replace(signature,'\nint frozen_2d_main(void)\n'))
        self.build_run('transport',['tests/host/savage3d_transport_test.c','src/kernel/lib/fmt.c'])

    def test_backend(self):
        OUT.mkdir(parents=True, exist_ok=True)
        source=(ROOT/'tests/host/savage2d_test.c').read_text()
        (OUT/'savage2d_fixture.inc').write_text(source.replace('\nint main(void)\n','\nint frozen_2d_main(void)\n'))
        self.build_run('backend',['tests/host/savage3d_backend_test.c','src/kernel/lib/fmt.c',
            'src/kernel/drivers/video/render3d.c','src/kernel/drivers/video/render3d_scalar.c',
            'src/kernel/drivers/video/render3d_native.c',
            'src/kernel/drivers/video/s3savage/savage3d.c'])

    def test_owner(self):
        self.build_run('owner',['tests/host/savage3d_owner_test.c','src/kernel/lib/fmt.c',
            'src/kernel/drivers/video/attempt.c','src/kernel/drivers/video/render3d.c',
            'src/kernel/drivers/video/render3d_scalar.c','src/kernel/drivers/video/s3savage/savage3d.c',
            'src/kernel/drivers/video/s3savage/savage3d_owner.c'], args=['0'])
        for variant in range(1,15):
            subprocess.run([str(OUT/'owner'),str(variant)],check=True)

    def test_registry(self):
        expected={'tdfx','rage128','radeon2d','mach64','matrox','i740','i810','intel2d',
                  's3savage','s3virge','sis','unichrome','riva128','nv2d','trident','vmwaresvga'}
        found=set()
        for folder in expected:
            descriptors=[p for p in (VIDEO/folder).glob('*.c') if 'render_family =' in p.read_text()]
            self.assertEqual(len(descriptors),1,folder)
            source=descriptors[0].read_text()
            self.assertIn('R3D_UNQUALIFIED' if folder=='s3savage' else 'R3D_NOT_IMPLEMENTED',source)
            self.assertRegex(source,r'render3d=&render_family|S3_RENDER_FAMILY \(&render_family\)')
            found.add(folder)
        self.assertEqual(found,expected)
        for folder in ('cirrus','chipshiqv','neomagic','s3trio64'):
            for p in (VIDEO/folder).glob('*.c'):
                self.assertNotIn('render_family =',p.read_text())
        # Registry nominations never become application hardware capabilities.
        self.assertNotIn('potential_', (VIDEO/'render3d.c').read_text())

    def test_target_objects(self):
        # Compile changed translation units with the real i686 kernel flags,
        # including the stack-frame limit; no linking/build/image invocation.
        tree=ast.parse((ROOT/'scripts/build_kernel.py').read_text())
        env={}
        for node in tree.body:
            if isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id in ('KERNEL_FRAME_LIMIT','CFLAGS') for t in node.targets):
                exec(compile(ast.Module(body=[node],type_ignores=[]),'<flags>','exec'),env)
        OUT.mkdir(parents=True,exist_ok=True)
        changed=subprocess.check_output(['git','diff','--name-only'],cwd=ROOT,text=True).splitlines()
        new=['src/kernel/drivers/video/'+n for n in ('render3d.c','render3d_scalar.c','render3d_native.c',
            's3savage/savage3d.c','s3savage/savage3d_owner.c')]
        objects=[]
        for rel in sorted(set(changed+new)):
            if not rel.startswith('src/kernel/drivers/') or not rel.endswith('.c'): continue
            obj=OUT/(rel.removeprefix('src/kernel/').replace('/','_')+'.o')
            subprocess.run(['clang',*env['CFLAGS'],'-I',str(ROOT/'src/kernel/include'),
                            '-c',str(ROOT/rel),'-o',str(obj)],check=True)
            objects.append(obj)
        # No FPU/SIMD in new renderer objects, and no duplicated public symbols.
        definitions={}
        for obj in objects:
            output=subprocess.check_output(['nm','--defined-only','--extern-only',str(obj)],text=True)
            for line in output.splitlines():
                parts=line.split()
                if len(parts)!=3 or parts[1].upper()=='W': continue
                self.assertNotIn(parts[2],definitions,(parts[2],obj,definitions.get(parts[2])))
                definitions[parts[2]]=obj
        for name in ('render3d.c','render3d_scalar.c','render3d_native.c','s3savage_savage3d.c','s3savage_savage3d_owner.c'):
            obj=OUT/('drivers_video_'+name+'.o')
            dis=subprocess.check_output(['objdump','-d',str(obj)],text=True)
            self.assertNotRegex(dis,r'\b(?:xmm\d|mm[0-7]|fld|fst|fadd|fmul|fdiv)\b')
        print(f'i686: {len(objects)} production objects, {len(definitions)} unique external definitions; stack limit and renderer FP/SIMD checks PASS')

if __name__=='__main__': unittest.main()
