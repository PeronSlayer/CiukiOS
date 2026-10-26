"""Win32 production GL/GDI model, ABI and stored render checksums."""
# SPDX-License-Identifier: MIT
import importlib.util
import os
import json
import re
import sys
from pathlib import Path
import subprocess
import tempfile
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
 def build(cls,sanitize,profile=True,gamma_hooks=True):
  out=ROOT/'build/host/win32/gl';out.mkdir(parents=True,exist_ok=True);cls.out=out
  spec=importlib.util.spec_from_file_location('glbuild',ROOT/'sdk/libgl/build.py');gl=importlib.util.module_from_spec(spec);spec.loader.exec_module(gl)
  lib=gl.host(sanitize=sanitize)
  units=[ROOT/'sdk/win32/gdi32'/n for n in gdi32.SOURCES if n not in ('compiler.c','compiler.S')]
  units += [ROOT/'sdk/win32/services/bytes.c',ROOT/'tests/host/win32/gdi32/support.c',HERE/'engine.c',HERE/'amendment.c',HERE/'review.c',HERE/'amendment2.c',HERE/'transport.c',HERE/'fixture.c',HERE/'quake3.c',HERE/'raster.c',HERE/'raster_reference.c']
  units += list((ROOT/'sdk/win32/opengl32').glob('*.c'))+[ROOT/'sdk/libgl/software/renderer.c']
  fixture_source=(ROOT/'tests/host/savage3d_backend_test.c').read_text()
  (out/'savage3d_backend_fixture.inc').write_text(fixture_source.replace('int main(int argc,char **argv)','int frozen_3d_main(int argc,char **argv)'))
  fixture_source=(ROOT/'tests/host/savage2d_test.c').read_text()
  (out/'savage2d_fixture.inc').write_text(fixture_source.replace('\nint main(void)\n','\nint frozen_2d_main(void)\n'))
  units += [HERE/'hardware.c',HERE/'kernel_engine.c',HERE/'kernel_replay.c']
  units += [ROOT/'src/kernel/drivers/video'/n for n in ('render3d.c','render3d_scalar.c','render3d_native.c','render3d_transport.c','s3savage/savage3d.c')]
  units += [ROOT/'src/kernel/lib/fmt.c']
  cls.binary=out/(('engine' if sanitize else 'benchmark')+('' if gamma_hooks else '-no-gamma'))
  flags=['-std=c17','-D_GNU_SOURCE','-DW32_HOST_MODEL','-O2','-g','-ffp-contract=off','-Wall','-Wextra','-Werror','-Wno-unused-function','-fno-vectorize','-fno-slp-vectorize','-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'sdk/libgl/vendor/include')]
  flags+=['-I'+str(ROOT/'src/kernel/include'),'-I'+str(ROOT/'tests/host'),'-I'+str(out)]
  if sanitize:flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
  if profile:flags+=['-DSW_PROFILE']
  if not gamma_hooks:flags+=['-DGL_TEST_NO_GAMMA']
  fixture=out/('fixture-sanitize.o' if sanitize else 'fixture.o')
  subprocess.run(['clang',*flags,'-include',str(HERE/'aliases.h'),'-Dentry=gl_fixture_entry',
    '-DwglUseFontBitmapsA=gl_fixture_font','-c',str(ROOT/'src/probes/win32_gl/exercise.c'),'-o',str(fixture)],check=True)
  units+=[fixture]
  units += [ROOT/'src/probes/win32_gl/scenes.c']
  subprocess.run(['clang',*flags,*map(str,units),str(lib),'-lm','-pthread','-o',str(cls.binary)],check=True)
  # The differential oracle includes the authored scalar backend, with every
  # external definition renamed. Audit the linked symbol table for collisions.
  nm=subprocess.check_output(['nm','--defined-only',str(cls.binary)],text=True)
  external=[m[1] for m in re.finditer(r'^[0-9a-fA-F]+ [A-Z] (\S+)$',nm,re.M)]
  assert len(external)==len(set(external)), 'duplicate external GL symbol'
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
  self.assertTrue({'wglChoosePixelFormat','wglDescribePixelFormat','wglGetPixelFormat','wglSetPixelFormat','wglSwapBuffers'}<=names)
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
 def test_quake3_replay(self):
  p=subprocess.run([str(self.binary),'--quake3'],capture_output=True,text=True,timeout=120)
  print(p.stdout,end='');self.assertEqual(p.returncode,0,p.stdout+p.stderr)
  check_quake3(p.stdout,sanitize=True)
 def test_kernel_transport(self):
  p=subprocess.run([str(self.binary),'--kernel-transport'],capture_output=True,text=True,timeout=120)
  print(p.stdout,end='');self.assertEqual(p.returncode,0,p.stdout+p.stderr)
 def test_hardware_adapter(self):
  p=subprocess.run([str(self.binary),'--hardware'],capture_output=True,text=True,timeout=60)
  print(p.stdout,end='');self.assertEqual(p.returncode,0,p.stdout+p.stderr)
 def test_render3d_minimum(self):
  binary=self.out/'minimum'
  subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
    '-fsanitize=address,undefined','-fno-sanitize-recover=all',
    '-I'+str(ROOT/'src/kernel/include'),str(HERE/'minimum_test.c'),
    str(ROOT/'sdk/win32/opengl32/minimum.c'),str(ROOT/'sdk/libgl/software/renderer.c'),
    str(ROOT/'src/kernel/drivers/video/render3d_scalar.c'),'-o',str(binary)],check=True)
  p=subprocess.run([str(binary)],capture_output=True,text=True,timeout=60)
  print(p.stdout,end='');self.assertEqual(p.returncode,0,p.stdout+p.stderr)
 def test_hardware_quake3_replay(self):
  p=subprocess.run([str(self.binary),'--hardware-quake3'],capture_output=True,text=True,timeout=300)
  print(p.stdout,end='');self.assertEqual(p.returncode,0,p.stdout+p.stderr)
  check_quake3(p.stdout,sanitize=True)
  row=re.search(r'GL_HW_MODEL submissions=(\d+) vertices=(\d+)',p.stdout)
  self.assertIsNotNone(row);self.assertGreater(int(row[2]),10000)
  self.assertLess(int(row[1])*3,int(row[2]))
  self.assertEqual(len(re.findall(r'^GL_HW_REPLAY .* frames=4 ',p.stdout,re.M)),4)
  rows=re.findall(r'^GL_SAVAGE_H1 scope=(\S+) width=(\d+) height=(\d+) frame=(\d+) gpu_draws=(\d+) gpu_fragments=(\d+) cpu_draws=(\d+) cpu_fragments=(\d+) eligible_world=(\d+)/(\d+) eligible_entities=(\d+)/(\d+).* switches=(\d+) ',p.stdout,re.M)
  self.assertEqual(len(rows),12)
  for scope,w,h,frame,gpu,fragments,cpu,cpu_fragments,world,total_world,entities,total_entities,switches in rows:
   self.assertIn((w,h),(('640','480'),('1024','768')))
   self.assertGreaterEqual(int(world)*100,int(total_world)*95)
   self.assertGreaterEqual(int(entities)*100,int(total_entities)*95)
   self.assertEqual(int(switches),0)
   self.assertEqual(int(gpu),2202 if scope=='qualified-prefix' else 0)
   self.assertEqual(int(cpu),0 if scope=='qualified-prefix' else 3000)
   self.assertGreater(int(fragments if scope=='qualified-prefix' else cpu_fragments),0)
 def test_hardware_quake3_default_format_replay(self):
  p=subprocess.run([str(self.binary),'--hardware-quake3-default'],capture_output=True,text=True,timeout=120)
  print(p.stdout,end='');self.assertEqual(p.returncode,0,p.stdout+p.stderr)
  check_quake3(p.stdout,sanitize=True,default_format=True)
 def test_quake3_default_format_replay(self):
  p=subprocess.run([str(self.binary),'--quake3-default'],capture_output=True,text=True,timeout=120)
  print(p.stdout,end='');self.assertEqual(p.returncode,0,p.stdout+p.stderr)
  check_quake3(p.stdout,sanitize=True,default_format=True)
 def test_optional_gamma_seam(self):
  original=self.binary
  try:
   self.build(sanitize=True,gamma_hooks=False)
   p=subprocess.run([str(self.binary),'--amendment2','gamma'],capture_output=True,text=True,timeout=60)
   print(p.stdout,end='');self.assertEqual(p.returncode,0,p.stdout+p.stderr)
  finally:
   type(self).binary=original
 def test_i686_hot_math_has_no_wide_division(self):
  from scripts.win32 import opengl32
  with tempfile.TemporaryDirectory(dir=self.out) as folder:
   source=Path(folder)/'hot.c';ir=source.with_suffix('.ll')
   source.write_text('#include "'+str(ROOT/'sdk/libgl/software/renderer.c')+'"\n'
     'int audit_repeat(int64_t uv) { return coordinate(uv, GL_REPEAT); }\n'
     'uint64_t audit_reciprocal(unsigned q) { return reciprocal48(q); }\n')
   subprocess.run(['clang',*opengl32.FLAGS,'-S','-emit-llvm',str(source),'-o',str(ir)],check=True)
   bodies=dict(re.findall(r'^define [^\n]*@([\w.]+)\([^\n]*\).*?\{(.*?)^}',ir.read_text(),re.M|re.S))
   pending=['audit_repeat','audit_reciprocal'];seen=set()
   while pending:
    name=pending.pop()
    if name in seen:continue
    seen.add(name);body=bodies[name]
    self.assertNotRegex(body,r'\b(?:sdiv|udiv|srem|urem) i64\b',name)
    pending += [n for n in re.findall(r'\bcall\b[^\n]*@([\w.]+)\(',body) if n in bodies]

def check_quake3(output,sanitize=False,default_format=False):
 expected=json.loads((HERE/'quake3-goldens.json').read_text())
 rows=re.findall(r'GL_Q3 width=(\d+) height=(\d+) passes=(\d+) triangles=(\d+) frames=(\d+) checksum=(\d+) ns_frame=(\d+) ns_pixel=([\d.]+)',output)
 assert len(rows)==4,output
 attachments=re.findall(r'GL_Q3 .* depth=(\d+) stencil=(\d+)',output)
 assert attachments==([('24','8')]*4 if default_format else [('16','0')]*4),attachments
 for w,h,passes,triangles,frames,checksum,ns_frame,ns_pixel in rows:
  key=f'{w}x{h}/{passes}'
  assert int(checksum)==expected[key],(key,checksum)
  assert int(triangles)==(3000 if passes=='1' else 4600)
  assert int(frames)>0 and int(ns_frame)>0
  # Deliberately broad wall-time guard for loaded/sanitized CI; differential
  # traversal also has a same-process relative early-depth regression bound.
  assert float(ns_pixel)<(15000 if sanitize else 3000),(key,ns_pixel)
 models=[dict(re.findall(r'(\w+)=([\d.]+)',line)) for line in output.splitlines() if line.startswith('GL_Q3_MODEL ')]
 if models:
  assert len(models)==4,models
  for row in models:
   # Deterministic work bounds catch restoration of per-pixel divisions on
   # the replay even when host speed or scheduler load changes. Sliver and
   # trilinear division fallbacks are checked separately by the LOD oracle.
   assert int(row['wide_div'])==0,row
   assert int(row['perspective_div'])*2<int(row['covered']),row

if __name__=='__main__':
 if any(arg in sys.argv for arg in ('--quake3-benchmark','--quake3-profile','--quake3-default-benchmark','--quake3-default-profile')):
  profile='--quake3-profile' in sys.argv or '--quake3-default-profile' in sys.argv
  default_format='--quake3-default-benchmark' in sys.argv or '--quake3-default-profile' in sys.argv
  GLTests.build(sanitize=False,profile=profile)
  p=subprocess.run([str(GLTests.binary),'--quake3-default' if default_format else '--quake3'],capture_output=True,text=True,timeout=120)
  print(p.stdout,end='');assert p.returncode==0,p.stderr
  check_quake3(p.stdout,default_format=default_format)
 elif '--benchmark' in sys.argv:
  GLTests.build(sanitize=False)
  subprocess.run([str(GLTests.present)],check=True)
  subprocess.run([str(GLTests.build_compositor(sanitize=False)),'--gl-present-only'],check=True)
  p=subprocess.run([str(GLTests.binary),'--benchmark'],capture_output=True,text=True);print(p.stdout,end='');assert p.returncode==0,p.stderr
  actual={m[1]:int(m[2]) for m in re.finditer(r'GL_SCENE scene=(\d+) checksum=(\d+)',p.stdout)}
  assert actual==json.loads((HERE/'goldens.json').read_text()),actual
  amendment={m[1]:int(m[2]) for m in re.finditer(r'GL_AMENDMENT name=(\S+) checksum=(\d+)',p.stdout)}
  assert amendment==json.loads((HERE/'amendment-goldens.json').read_text()),amendment
 else:unittest.main()
