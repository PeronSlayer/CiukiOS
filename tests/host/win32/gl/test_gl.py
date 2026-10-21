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
from unittest.mock import patch
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
  units += [ROOT/'sdk/win32/services/bytes.c',ROOT/'tests/host/win32/gdi32/support.c',HERE/'engine.c',HERE/'amendment.c',HERE/'review.c',HERE/'transport.c',HERE/'fixture.c']
  units += list((ROOT/'sdk/win32/opengl32').glob('*.c'))+[ROOT/'sdk/libgl/software/renderer.c']
  cls.binary=out/('engine' if sanitize else 'benchmark')
  flags=['-std=c17','-D_GNU_SOURCE','-DW32_HOST_MODEL','-O2','-g','-ffp-contract=off','-Wall','-Wextra','-Werror','-Wno-unused-function','-fno-vectorize','-fno-slp-vectorize','-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'sdk/libgl/vendor/include')]
  flags+=['-I'+str(ROOT/'src/kernel/include')]
  if sanitize:flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
  fixture=out/('fixture-sanitize.o' if sanitize else 'fixture.o')
  subprocess.run(['clang',*flags,'-include',str(HERE/'aliases.h'),'-Dentry=gl_fixture_entry',
    '-DwglUseFontBitmapsA=gl_fixture_font','-c',str(ROOT/'src/probes/win32_gl/exercise.c'),'-o',str(fixture)],check=True)
  units+=[fixture]
  units += [ROOT/'src/probes/win32_gl/scenes.c']
  subprocess.run(['clang',*flags,*map(str,units),str(lib),'-lm','-pthread','-o',str(cls.binary)],check=True)
  cls.present=out/('present' if sanitize else 'present-benchmark')
  subprocess.run(['clang',*flags,'-include',str(ROOT/'tests/host/desktop/replay_base.h'),str(HERE/'present_test.c'),str(ROOT/'apps/desktop/gl_present.c'),str(ROOT/'apps/desktop/protocol.c'),str(ROOT/'apps/libdeskui/v3.c'),'-o',str(cls.present)],check=True)
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
  self.assertEqual(selector.count('"w32-gl"'),1);self.assertEqual(loader.count("'w32-gl'"),1);self.assertEqual(model.count("'w32-gl'"),1)
  case=next(c for c in json.loads((ROOT/'tests/suites/f5-core.json').read_text())['cases'] if c['id']=='w32-gl-qemu-t23')
  self.assertIn('unrun',case['integration_pending'])
  self.assertIn('0x100e0000',case['integration_pending'])
  self.assertEqual(case['selector'],'f5:w32-gl run={run_id}')
  goldens=json.loads((HERE/'goldens.json').read_text())
  scenes=[p for p in case['expected']['predicates'] if p['where']['case']=='scene']
  self.assertEqual({str(p['where']['scene']):p['fields']['checksum']['eq'] for p in scenes},goldens)
 def test_present_completion(self):
  p=subprocess.run([str(self.present)],capture_output=True,text=True,timeout=60)
  print(p.stdout,end='');self.assertEqual(p.returncode,0,p.stdout+p.stderr)
 def test_guest_content_and_completion_predicates(self):
  with patch.object(sys,'path',[str(ROOT/'scripts/test'),*sys.path]):
   from evidence import evaluate_predicates,EvidenceError
  case=next(c for c in json.loads((ROOT/'tests/suites/f5-core.json').read_text())['cases'] if c['id']=='w32-gl-qemu-t23')
  records=[]
  for p in case['expected']['predicates']:
   records.append({k:str(v) for k,v in {**p['where'],**{k:r.get('eq',r.get('ge',1)) for k,r in p['fields'].items()}}.items()})
  self.assertEqual(evaluate_predicates(records,case['expected']),[])
  for field,value in (('checksum','1'),('frames','6')):
   bad=[dict(r) for r in records];bad[0][field]=value
   with self.assertRaises(EvidenceError):evaluate_predicates(bad,case['expected'])
  bad=[dict(r) for r in records];bad[-2]['display_width']='0'
  with self.assertRaises(EvidenceError):evaluate_predicates(bad,case['expected'])
 @classmethod
 def build_compositor(cls,sanitize=True):
  from tests.host.desktop.test_desktop import SHELL_SOURCES
  sources=[ROOT/'apps/desktop'/n for n in ('protocol.c','client.c','input.c','compositor.c','theme.c','gl_present.c')]
  sources+=SHELL_SOURCES+[ROOT/'apps/libdeskui'/n for n in ('icons.c','icon_map.c','v3.c')]
  sources+=[ROOT/'tests/host/desktop'/n for n in ('fake.c','v3_test.c')]
  binary=cls.out/('compositor' if sanitize else 'compositor-benchmark')
  subprocess.run(['clang','-std=c17','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror',
    *(['-fsanitize=address,undefined','-fno-sanitize-recover=all'] if sanitize else []),'-include',str(ROOT/'tests/host/desktop/replay_base.h'),
    '-I'+str(ROOT/'apps/desktop'),'-I'+str(ROOT/'src/kernel/include'),*map(str,sources),'-o',str(binary)],check=True)
  return binary
 def test_production_compositor_completion(self):
  binary=self.build_compositor()
  p=subprocess.run([str(binary),'--gl-present-only'],capture_output=True,text=True,timeout=60)
  print(p.stdout,end='');self.assertEqual(p.returncode,0,p.stdout+p.stderr)
 def test_scenes(self):
  p=subprocess.run([str(self.binary)],capture_output=True,text=True,timeout=60)
  print(p.stdout,end='');self.assertEqual(p.returncode,0,p.stdout+p.stderr)
  actual={m[1]:int(m[2]) for m in re.finditer(r'GL_SCENE scene=(\d+) checksum=(\d+)',p.stdout)}
  self.assertEqual(actual,json.loads((HERE/'goldens.json').read_text()))
  amendment={m[1]:int(m[2]) for m in re.finditer(r'GL_AMENDMENT name=(\S+) checksum=(\d+)',p.stdout)}
  self.assertEqual(amendment,json.loads((HERE/'amendment-goldens.json').read_text()))

if __name__=='__main__':
 if '--benchmark' in sys.argv:
  GLTests.build(sanitize=False)
  subprocess.run([str(GLTests.present)],check=True)
  subprocess.run([str(GLTests.build_compositor(sanitize=False)),'--gl-present-only'],check=True)
  p=subprocess.run([str(GLTests.binary),'--benchmark'],capture_output=True,text=True);print(p.stdout,end='');assert p.returncode==0,p.stderr
  actual={m[1]:int(m[2]) for m in re.finditer(r'GL_SCENE scene=(\d+) checksum=(\d+)',p.stdout)}
  assert actual==json.loads((HERE/'goldens.json').read_text()),actual
  amendment={m[1]:int(m[2]) for m in re.finditer(r'GL_AMENDMENT name=(\S+) checksum=(\d+)',p.stdout)}
  assert amendment==json.loads((HERE/'amendment-goldens.json').read_text()),amendment
 else:unittest.main()
