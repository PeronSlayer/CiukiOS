"""Production scalar GDI, independent reference pixels, pinned frames and ABI."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from sdk.freetype import build as freetype
from scripts.win32 import gdi32, census

HERE = Path(__file__).with_name('gdi32')
PIN = HERE/'frames/manifest.json'
FONT = ROOT/'assets/fonts/liberation/LiberationSans-Regular.ttf'

def digest(data):
    return hashlib.sha256(data).hexdigest()

def authored(x, y):
    return (((x*37+y*19)&255)<<16)|(((x*13+y*43)&255)<<8)|((x*61+y*7)&255)

def expand565(v):
    r, g, b = (v>>11)&31, (v>>5)&63, v&31
    return (((r<<3)|(r>>2))<<16)|(((g<<2)|(g>>4))<<8)|((b<<3)|(b>>2))

def encode565(rgb):
    return ((rgb>>8)&0xf800)|((rgb>>5)&0x7e0)|((rgb>>3)&31)

def frame_oracle(name, m):
    width, height, bpp, pitch = (m[k] for k in ('width', 'height', 'bpp', 'pitch'))
    result = bytearray([0xa5])*m['size']
    def put(x, y, c):
        at = y*pitch+x*(bpp//8)
        if bpp == 16:
            result[at:at+2] = encode565(c).to_bytes(2, 'little')
        else:
            result[at:at+3] = c.to_bytes(3, 'little')
    if name.startswith(('rop-', 'rop16-')):
        rop = int(name.split('-')[1])
        for y in range(height):
            for x in range(width):
                p, s, d = authored((x+3)%8+2, (y-2)%8+5), authored(x,y), authored(x+7,y+9)
                if bpp==16:
                    p,s,d=map(encode565,(p,s,d))
                out = 0
                for bit in range(16 if bpp==16 else 24):
                    index = (((p>>bit)&1)<<2)|(((s>>bit)&1)<<1)|((d>>bit)&1)
                    out |= ((rop>>index)&1)<<bit
                if bpp==16:
                    at=y*pitch+x*2
                    result[at:at+2]=out.to_bytes(2,'little')
                else:
                    put(x, y, out)
    elif name.startswith(('format-', 'format555-')):
        depth = int(name.split('-')[1])
        for y in range(height):
            for x in range(width):
                if depth <= 8:
                    index = (x*7+y*11)%(1<<depth)
                    c = (0xffffff if index else 0) if depth == 1 else ((index*53&255)<<16)|((index*97&255)<<8)|(index*193&255)
                else:
                    c = authored(x, y)
                    if depth == 16:
                        if name.startswith('format555-'):
                            r,g,b=(c>>19)&31,(c>>11)&31,(c>>3)&31
                            c=(((r<<3)|(r>>2))<<16)|(((g<<3)|(g>>2))<<8)|((b<<3)|(b>>2))
                        else:
                            c = expand565(encode565(c))
                put(x, y, c)
    elif name.startswith('stretch-'):
        mode = int(name.split('-')[1])
        for y in range(height):
            for x in range(width):
                x0, y0 = x*17//width, y*11//height
                x1 = ((x+1)*17+width-1)//width if mode == 4 or (mode != 3 and 17>width) else x0+1
                y1 = ((y+1)*11+height-1)//height if mode == 4 or (mode != 3 and 11>height) else y0+1
                c = 0xffffff if mode == 1 else 0
                totals, total = [0,0,0], 0
                for yy in range(y0,y1):
                    for xx in range(x0,x1):
                        sample = authored(xx, yy)
                        if mode == 1:
                            c &= sample
                        elif mode == 2:
                            c |= sample
                        elif mode == 3:
                            c = sample
                        else:
                            weight = (min((x+1)*17,(xx+1)*width)-max(x*17,xx*width))*(min((y+1)*11,(yy+1)*height)-max(y*11,yy*height))
                            for k in range(3):
                                totals[k] += ((sample>>(k*8))&255)*weight
                            total += weight
                if mode == 4:
                    c = sum(((totals[k]+total//2)//total)<<(k*8) for k in range(3))
                put(x,y,c)
    elif name.startswith('region-'):
        mode = int(name.split('-')[1])
        for y in range(height):
            for x in range(width):
                # Doubled pixel centres; independent rational ellipse test.
                a = 1 <= x < 22 and 1 <= y < 21 and ((2*x+1-23)**2*65536//(21**2)+(2*y+1-22)**2*65536//(20**2) <= 65536)
                b = 10 <= x < 30 and 3 <= y < 17
                inside = (a and b, a or b, a != b, a and not b, a)[mode-1]
                if inside:
                    put(x,y,0xffffff)
    elif name.startswith('palette-'):
        colors=[0x010203,0x010203,0xff0000,0x040506]
        if name=='palette-animate':
            colors[0]=0xc86432
        for y in range(height):
            for x in range(width):
                put(x,y,colors[(y*width+x)%4])
    else:
        return None
    return bytes(result)

class Gdi32Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = ROOT/'build/host/win32/gdi32'
        cls.out.mkdir(parents=True, exist_ok=True)
        (ROOT/'build/codex').mkdir(parents=True,exist_ok=True)
        cls.env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', TMPDIR=str(cls.out))
        cls.fontlib = freetype.build(cls.out/'freetype', flags=['-O2', '-fPIC'], env=cls.env)
        sources = [ROOT/'sdk/win32/gdi32'/n for n in gdi32.SOURCES if n not in ('compiler.c','compiler.S')]
        sources += [ROOT/'sdk/win32/gdi32/native_font.c', ROOT/'sdk/win32/services/bytes.c', ROOT/'sdk/libfont/font.c', HERE/'support.c', HERE/'engine_test.c']
        cls.sources = sources
        cls.program = cls.out/'gdi-tests'
        cls.flags = ['-std=c17','-D_GNU_SOURCE','-DW32_HOST_MODEL','-O2','-g','-Wall','-Wextra','-Werror',
                     '-I'+str(ROOT/'sdk/win32/include'),'-idirafter',str(ROOT/'sdk/sysroot-overlay/include'),'-I'+str(ROOT/'src/kernel/include'),*freetype.includes()]
        subprocess.run(['clang', *cls.flags, '-fsanitize=address,undefined', '-fno-sanitize-recover=all', *map(str,sources), str(cls.fontlib), '-pthread', '-o', str(cls.program)], check=True, env=cls.env)

    def execute(self, mode, *args):
        p = subprocess.run([str(self.program),mode,*map(str,args)], capture_output=True,text=True,env=self.env,timeout=180)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertIn('PASS',p.stdout)
        return p.stdout

    def test_object_lifetime_delete_dc_saved_states_process_exit_and_oom(self): self.execute('lifetime')
    def test_independent_region_boolean_grid_serialization_winding_and_ellipse(self): self.execute('regions')
    def test_dc_mapping_state_visible_regions_clipping_and_pixel_apis(self): self.execute('state')
    def test_dib_section_direct_writes_scan_bands_sections_masks_and_padding(self): self.execute('bitmaps')
    def test_logical_palette_static_colors_reserved_animation_and_notifications(self): self.execute('palettes')
    def test_shapes_strokes_and_flood_fill(self): self.execute('drawing')
    def test_all_256_rops_independent_bit_oracle_overlap_and_allocation_failure(self): self.execute('rops')
    def test_concurrent_object_and_dc_lifetime(self): self.execute('concurrency')
    def test_reflections_clip_phase_mask_transparency_mono_conversion_and_pattern_origin(self): self.execute('transfers')
    def test_transactional_region_dc_oom_and_fixed_point_xform(self): self.execute('faults')
    def test_user32_borrowed_surface_defaults_child_origin_and_device_clipping(self): self.execute('child')
    def test_font_provider_metadata_extents_and_saved_stock_lifetime(self): self.execute('font-boundary')
    def test_freetype_cp1252_extent_alignment_dx_and_drawtext_layout(self): self.execute('text', FONT)

    def test_pinned_frames_and_independent_pixel_oracles(self):
        folder = self.out/'frames'
        folder.mkdir(exist_ok=True)
        stdout = self.execute('frames', folder, FONT)
        manifest = {}
        for line in stdout.splitlines():
            if not line.startswith('FRAME '):
                continue
            _, name, *values = line.split()
            m = dict(zip(('width','height','bpp','pitch','size'),map(int,values)))
            raw = (folder/(name+'.bin')).read_bytes()
            self.assertEqual(len(raw),m['size'])
            m['sha256'] = digest(raw)
            expected = frame_oracle(name,m)
            if expected is not None:
                self.assertEqual(raw,expected,name)
            manifest[name] = m
        record = {'schema':1,'provenance':'CiukiOS production engine host frames; independent Boolean/format/stretch/region oracles; not Windows captures',
                  'font_sha256':digest(FONT.read_bytes()),'frames':manifest}
        (folder/'manifest.json').write_text(json.dumps(record,indent=2,sort_keys=True)+'\n')
        # Updating pins is explicit and still checks independent pixel oracles.
        if os.environ.get('CIUKI_GDI_PIN_FRAMES') == '1':
            import shutil
            PIN.write_text(json.dumps(record,indent=2,sort_keys=True)+'\n')
            for name in manifest:
                shutil.copyfile(folder/(name+'.bin'), PIN.parent/(name+'.bin'))
        self.assertEqual(record,json.loads(PIN.read_text()))
        for name,m in manifest.items():
            self.assertEqual(digest((PIN.parent/(name+'.bin')).read_bytes()),m['sha256'])
        self.assertEqual(len(manifest),569)

    def test_pe_census_stdcall_surface_boundary_and_i686_no_fpu_simd(self):
        subprocess.run(['clang',*gdi32.FLAGS,'-I'+str(ROOT/'sdk/win32/include'),'-fsyntax-only',str(HERE/'abi.c')],check=True)
        image_record = gdi32.build(self.out/'builtin')
        raw = (self.out/'builtin/gdi32.dll').read_bytes()
        self.assertEqual(digest(raw),image_record['sha256'])
        info = census.inspect(raw)
        exports = {n for row in info['exports'] for n in row['names']}
        coverage = json.loads((self.out/'builtin/gdi32-coverage.json').read_text())['exports']
        self.assertEqual(len(coverage),93)
        self.assertTrue({row['name'] for row in coverage} <= exports)
        self.assertTrue({row['status'] for row in coverage} <= {'implemented','W21 nonreturning stub'})
        self.assertTrue({'w32_gdi_surface_dc','w32_gdi_create_font','TextOutA','StretchDIBits','CreateDIBSection'} <= exports)
        self.assertEqual(json.loads((self.out/'builtin/gdi32-audit.json').read_text())['fpu_simd_instructions'],[])
        definition = (self.out/'builtin/gdi32.def').read_text()
        self.assertIn('StretchDIBits=_StretchDIBits@52',definition)
        self.assertIn('CreateDIBSection=_CreateDIBSection@24',definition)
        self.assertIn('w32_gdi_surface_dc=_w32_gdi_surface_dc@20',definition)

    def test_scalar_o2_frame_costs(self):
        program = self.out/'gdi-benchmark'
        # The benchmark executes drawing only, while the linked native font
        # backend retains its separately pinned scalar FreeType recipe.
        subprocess.run(['clang', *self.flags,'-mno-sse','-mno-sse2','-mno-mmx','-fno-vectorize','-fno-slp-vectorize',
                        *map(str,self.sources),str(self.fontlib),'-pthread','-o',str(program)],check=True,env=self.env)
        p=subprocess.run([str(program),'benchmark'],capture_output=True,text=True,check=True,env=self.env)
        (ROOT/'build/codex/f5-10b-costs.txt').write_text(p.stdout)
        self.assertIn('COST StretchDIBits-8-to-32',p.stdout)
        self.assertIn('COST BitBlt-SRCCOPY-32',p.stdout)
        print(p.stdout,end='')

if __name__ == '__main__':
    unittest.main()
