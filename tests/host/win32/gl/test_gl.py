"""Win32 production GL/GDI model, ABI and stored render checksums."""
# SPDX-License-Identifier: MIT
import importlib.util
import os
import json
import re
import sys
from pathlib import Path
import subprocess
import unittest
from scripts.win32 import gdi32
ROOT=Path(__file__).resolve().parents[4]
HERE=Path(__file__).resolve().parent
class GLTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.build(sanitize=True)
 @classmethod
 def build(cls,sanitize):
  out=ROOT/'build/host/win32/gl';out.mkdir(parents=True,exist_ok=True);cls.out=out
  spec=importlib.util.spec_from_file_location('glbuild',ROOT/'sdk/libgl/build.py');gl=importlib.util.module_from_spec(spec);spec.loader.exec_module(gl)
  lib=gl.host(sanitize=sanitize)
  units=[ROOT/'sdk/win32/gdi32'/n for n in gdi32.SOURCES if n not in ('compiler.c','compiler.S')]
  units += [ROOT/'sdk/win32/services/bytes.c',ROOT/'tests/host/win32/gdi32/support.c',HERE/'engine.c']
  units += list((ROOT/'sdk/win32/opengl32').glob('*.c'))+[ROOT/'sdk/libgl/software/renderer.c']
  cls.binary=out/('engine' if sanitize else 'benchmark')
  flags=['-std=c17','-D_GNU_SOURCE','-DW32_HOST_MODEL','-O2','-g','-ffp-contract=off','-Wall','-Wextra','-Werror','-Wno-unused-function','-fno-vectorize','-fno-slp-vectorize','-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'sdk/libgl/vendor/include')]
  if sanitize:flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
  units += [ROOT/'src/probes/win32_gl/scenes.c']
  subprocess.run(['clang',*flags,*map(str,units),str(lib),'-lm','-pthread','-o',str(cls.binary)],check=True)
  cls.present=out/('present' if sanitize else 'present-benchmark')
  subprocess.run(['clang',*flags,'-include',str(ROOT/'tests/host/desktop/replay_base.h'),'-I'+str(ROOT/'src/kernel/include'),str(HERE/'present_test.c'),str(ROOT/'apps/desktop/gl_present.c'),str(ROOT/'apps/desktop/protocol.c'),'-o',str(cls.present)],check=True)
 def test_export_surface_and_probe_registration(self):
  from scripts.win32 import census,opengl32
  opengl32.build(self.out/'builtin')
  rows=json.loads((ROOT/'sdk/win32/opengl32/abi.json').read_text())['functions']+json.loads((ROOT/'sdk/win32/opengl32/extensions.json').read_text())
  self.assertEqual(len(rows[:336]),336)
  info=census.inspect((self.out/'builtin/opengl32.dll').read_bytes())
  names={name for row in info['exports'] for name in row['names']}
  self.assertFalse({row['name'] for row in rows}-names)
  self.assertTrue({'wglCreateContext','wglDeleteContext','wglMakeCurrent','wglGetCurrentContext','wglGetCurrentDC','wglGetProcAddress','wglShareLists','wglUseFontBitmapsA','wglUseFontBitmapsW'}<=names)
  selector=(ROOT/'src/kernel/probes/selector.c').read_text()
  loader=(ROOT/'src/boot/ciukldr/menu.inc').read_text()
  model=(ROOT/'scripts/test/loader_model.py').read_text()
  self.assertIn('"w32-gl"',selector);self.assertIn("'w32-gl'",loader);self.assertIn("'w32-gl'",model)
  case=next(c for c in json.loads((ROOT/'tests/suites/f5-core.json').read_text())['cases'] if c['id']=='w32-gl-qemu-t23')
  self.assertIn('unrun',case['integration_pending'])
  self.assertEqual(case['selector'],'f5:w32-gl run={run_id}')
 def test_present_completion(self):
  p=subprocess.run([str(self.present)],capture_output=True,text=True,timeout=60)
  print(p.stdout,end='');self.assertEqual(p.returncode,0,p.stdout+p.stderr)
 def test_scenes(self):
  p=subprocess.run([str(self.binary)],capture_output=True,text=True,timeout=60)
  print(p.stdout,end='');self.assertEqual(p.returncode,0,p.stdout+p.stderr)
  actual={m[1]:int(m[2]) for m in re.finditer(r'GL_SCENE scene=(\d+) checksum=(\d+)',p.stdout)}
  self.assertEqual(actual,json.loads((HERE/'goldens.json').read_text()))

if __name__=='__main__':
 if '--benchmark' in sys.argv:
  GLTests.build(sanitize=False)
  subprocess.run([str(GLTests.present)],check=True)
  p=subprocess.run([str(GLTests.binary),'--benchmark'],check=True,capture_output=True,text=True);print(p.stdout,end='')
  actual={m[1]:int(m[2]) for m in re.finditer(r'GL_SCENE scene=(\d+) checksum=(\d+)',p.stdout)}
  assert actual==json.loads((HERE/'goldens.json').read_text()),actual
 else:unittest.main()
