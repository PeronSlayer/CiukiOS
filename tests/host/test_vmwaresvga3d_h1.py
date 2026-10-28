"""Existing H1 Quake III replay through real GL/wire/service and the SVGA3D
host model (f7-31), with both host semantics: VMware (D3D9 combiners) and
VirtualBox's legacy OpenGL backend (combiners ignored, TEXTURE_OPS 0).

UBSan default. CIUKI_DISPLAY_ASAN=1 adds ASan/LSan without altering options.
Run separately from other host suites.
"""
from pathlib import Path
import importlib.util
import os
import re
import subprocess
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
class SvgaH1Tests(unittest.TestCase):
    def replay(self,family):
        spec=importlib.util.spec_from_file_location('svga_h1_gl_'+family.replace('-','_'),ROOT/'tests/host/win32/gl/test_gl.py')
        gl=importlib.util.module_from_spec(spec); spec.loader.exec_module(gl)
        sanitizer='address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
        gl.GLTests.build(sanitize=True,family=family,sanitizer=sanitizer)
        result=subprocess.run([str(gl.GLTests.binary),'--hardware-quake3'],capture_output=True,text=True,timeout=1800)
        print(result.stdout,end='')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        rows=re.findall(r'^GL_VMWARESVGA_H1 host=(\S+) scope=(\S+) width=(\d+) height=(\d+) frame=(\d+) gpu_draws=(\d+) '
                        r'gpu_fragments=(\d+) cpu_draws=(\d+) cpu_fragments=(\d+)',result.stdout,re.M)
        self.assertEqual(len(rows),18)
        return rows
    def test_vmware_semantics(self):
        for host,scope,w,h,frame,gpu,fragments,cpu,unused in self.replay('vmwaresvga'):
            self.assertEqual(host,'vmware'); self.assertIn((w,h),(('640','480'),('1024','768')))
            self.assertEqual(int(gpu),{'prefix':2202,'complete':3000,'complete-multipass':4600}[scope])
            self.assertGreater(int(fragments),0); self.assertEqual(int(cpu),0)
    def test_virtualbox_gl_semantics(self):
        for host,scope,w,h,frame,gpu,fragments,cpu,unused in self.replay('vmwaresvga-vbox'):
            self.assertEqual(host,'vbox-gl')
            if scope=='prefix':
                self.assertEqual(int(gpu),2202); self.assertGreater(int(fragments),0); self.assertEqual(int(cpu),0)
            else:
                self.assertEqual(int(gpu),0); self.assertEqual(int(cpu),4600 if scope=='complete-multipass' else 3000)
if __name__=='__main__':unittest.main()
