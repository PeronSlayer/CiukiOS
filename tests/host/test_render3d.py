"""F7 universal driver service and shared-owner transport. Sequential, no image.

UBSan default, matching display family suites. CIUKI_DISPLAY_ASAN=1 adds ASan
with default leak detection. No sanitizer option is disabled by this runner.
"""
from pathlib import Path
import ast
import os
import re
import subprocess
import runpy
import struct
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/host/render3d'
VIDEO = ROOT / 'src/kernel/drivers/video'

class RenderTests(unittest.TestCase):
    def test_submission_cost_model(self):
        # Explicit assumptions, not a host timing extrapolation to silicon.
        # 1 GHz Pentium III, 120 cycles/UC word, 250/status observation,
        # 100 MiB/s readback, 256 triangles/batch. The 3000/4600 lists
        # match the existing single/two-pass Quake-like GL replay workloads.
        mhz = 1000
        self.assertEqual(256*28,7168)
        self.assertLess(28804,65536)
        for triangles in (1024,3000,4600):
            batches=(triangles+255)//256
            # Draw: 17 state + 17 restore + bitmap restore + one draw event.
            small_words,events=17*2+17*2+5,36
            # Final color-to-linear copy: 8 words + 18 restore packets/events.
            present_words=8+17*2+5+19*2
            words=triangles*28+batches*(small_words+events*2)+present_words
            observations=(triangles*28+7)//8+batches*events*2+19*2
            submit_us=(words*120+observations*250)/mhz
            for w,h in ((640,480),(800,600),(1024,768)):
                # Every geometry job reads color/D16; present reads both
                # the color target and its linear copy. No asynchronous DMA.
                read_bytes=(batches*6+8)*w*h
                read_ms=read_bytes/(100*1024*1024)*1000
                print(f'PIII MODEL {w}x{h}: {triangles} triangles/{batches} batches, '
                      f'{words} UC words/frame, submission={submit_us/1000:.3f} ms, '
                      f'readback={read_bytes} bytes/{read_ms:.3f} ms at assumed 100 MiB/s; '
                      'excludes initial uploads/clear, FIFO stalls, scalar fallback, raster latency and scanout')

    def build_run(self, name, sources, args=(), flags=()):
        OUT.mkdir(parents=True, exist_ok=True)
        sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
        command = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize='+sanitizer, '-fno-sanitize-recover=undefined',
                 '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                 '-I', str(ROOT/'src/kernel/include'), '-I', str(ROOT/'tests/host'), '-I', str(OUT)]
        binary = OUT/name
        subprocess.run(command+list(flags)+[str(ROOT/p) for p in sources]+['-lm','-o',str(binary)],check=True)
        subprocess.run([str(binary),*args],check=True)

    def test_service(self):
        self.build_run('test', ['tests/host/render3d_test.c',
            'src/kernel/drivers/video/render3d.c', 'src/kernel/drivers/video/render3d_scalar.c',
            'src/kernel/drivers/video/s3savage/savage3d.c'])

    def test_worker(self):
        self.build_run('worker', ['tests/host/render3d_worker_test.c',
            'src/kernel/drivers/video/render3d.c','src/kernel/drivers/video/render3d_scalar.c',
            'src/kernel/drivers/video/render3d_transport.c'])
        subprocess.run([str(OUT/'worker'),'budget'],check=True)

    def test_worker_scheduling(self):
        self.build_run('scheduling', ['tests/host/render3d_scheduling_test.c',
            'sdk/win32/gdi32/render3d.c','sdk/win32/services/bytes.c',
            'src/kernel/drivers/video/render3d.c','src/kernel/drivers/video/render3d_scalar.c',
            'src/kernel/drivers/video/render3d_transport.c'],
            flags=['-DW32_HOST_MODEL','-I',str(ROOT/'sdk/win32/include')])

    def test_abi_uv_shape(self):
        # Same-byte-size reshape: independent row/component/stride expectations
        # must fail while the enclosing size and every offset still agree.
        expressions=runpy.run_path(str(ROOT/'scripts/test/abi_layout_dump.py'))['expressions']
        header=(ROOT/'src/kernel/include/ciuki/abi.h').read_text()
        keys=['sizeof.ciuki_r3d_draw','offsetof.ciuki_r3d_draw.uv1',
              'offsetof.ciuki_r3d_draw.vertices','rows.ciuki_r3d_draw.uv1',
              'components.ciuki_r3d_draw.uv1','strideof.ciuki_r3d_draw.uv1']
        expected=(28804,1124,7268,768,2,8)
        flags=runpy.run_path(str(ROOT/'scripts/build_kernel.py'))['CFLAGS']
        OUT.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=OUT) as tmp:
            folder=Path(tmp)
            for shape in ('[768][2]','[384][4]'):
                changed=header.replace('uv1[768][2]','uv1'+shape)
                found=expressions(changed)
                self.assertTrue(set(keys)<=found.keys())
                (folder/'abi.h').write_text(changed)
                (folder/'shape.c').write_text('#include "abi.h"\n'
                    '__attribute__((used,section(".shape"))) const uint32_t shape[] = {'+
                    ','.join(found[k] for k in keys)+'};\n')
                subprocess.run(['clang',*flags,'-I',str(ROOT/'src/kernel/include'),
                    '-c',str(folder/'shape.c'),'-o',str(folder/'shape.o')],check=True)
                subprocess.run(['llvm-objcopy','--dump-section',f'.shape={folder}/shape.bin',str(folder/'shape.o')],check=True)
                actual=struct.unpack('<6I',(folder/'shape.bin').read_bytes())
                self.assertEqual(actual[:3],expected[:3])
                if shape=='[768][2]': self.assertEqual(actual,expected)
                else:
                    self.assertEqual(actual[3:],(384,4,16))
                    self.assertNotEqual(actual[3:],expected[3:])
        print('PASS i686 UV oracle: 768 rows/2 components/8-byte stride; same-size 384x4 mutation rejected')

    def test_encoder_capabilities(self):
        self.build_run('encoder', ['tests/host/render3d_encoder_test.c'])

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
            'sdk/libgl/software/renderer.c',
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

    def test_fullsize(self):
        OUT.mkdir(parents=True, exist_ok=True)
        source=(ROOT/'tests/host/savage2d_test.c').read_text()
        (OUT/'savage2d_fixture.inc').write_text(source.replace('\nint main(void)\n','\nint frozen_2d_main(void)\n'))
        self.build_run('fullsize',['tests/host/savage3d_backend_test.c','src/kernel/lib/fmt.c',
            'sdk/libgl/software/renderer.c',
            'src/kernel/drivers/video/render3d.c','src/kernel/drivers/video/render3d_scalar.c',
            'src/kernel/drivers/video/render3d_native.c',
            'src/kernel/drivers/video/s3savage/savage3d.c'],args=['fullsize'])

    def test_optional_formats(self):
        self.run_shader_case("formats")

    def test_review4(self):
        self.run_shader_case("review4")

    def test_sampled_shaders(self):
        self.run_shader_case("shader")

    def run_shader_case(self, case):
        OUT.mkdir(parents=True, exist_ok=True)
        source=(ROOT/'tests/host/savage2d_test.c').read_text()
        (OUT/'savage2d_fixture.inc').write_text(source.replace('\nint main(void)\n','\nint frozen_2d_main(void)\n'))
        self.build_run('shader',['tests/host/savage3d_backend_test.c','src/kernel/lib/fmt.c',
            'sdk/libgl/software/renderer.c',
            'src/kernel/drivers/video/render3d.c','src/kernel/drivers/video/render3d_scalar.c',
            'src/kernel/drivers/video/render3d_native.c',
            'src/kernel/drivers/video/s3savage/savage3d.c'],args=[case])

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
        new=['src/kernel/drivers/video/'+n for n in ('render3d.c','render3d_scalar.c','render3d_native.c','render3d_transport.c',
            's3savage/savage3d.c','s3savage/savage3d_owner.c')]
        new += ['src/kernel/core/'+n for n in ('render3d_transport.c','display_present.c','display.c')]
        new += ['src/kernel/proc/win32/'+n for n in ('syscall.c','records.c')]
        objects=[]
        for rel in sorted(set(changed+new)):
            if not rel.startswith('src/kernel/') or not rel.endswith('.c'): continue
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
        for name in ('render3d.c','render3d_scalar.c','render3d_native.c','render3d_transport.c','s3savage_savage3d.c','s3savage_savage3d_owner.c'):
            obj=OUT/('drivers_video_'+name+'.o')
            dis=subprocess.check_output(['objdump','-d',str(obj)],text=True)
            self.assertNotRegex(dis,r'\b(?:xmm\d|mm[0-7]|fld|fst|fadd|fmul|fdiv)\b')
        print(f'i686: {len(objects)} production objects, {len(definitions)} unique external definitions; stack limit and renderer FP/SIMD checks PASS')

if __name__=='__main__': unittest.main()
