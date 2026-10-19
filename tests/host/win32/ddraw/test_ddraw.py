"""Original COM core, real DIB/GDI and independent pinned transfer results."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import json
import os
from pathlib import Path
import subprocess
import unittest
from scripts.win32 import ddraw, gdi32, census
ROOT=Path(__file__).resolve().parents[4]
HERE=Path(__file__).resolve().parent

def pattern(x,y,seed,bpp):
    value=(x*37+y*61+seed*19)^(x*y*13+seed*71)
    return value&255 if bpp==8 else value*149&65535 if bpp==16 else value*0x010307&0xffffff

def oracle(bpp,variant):
    width,height=13,9
    pitch=(width*(bpp//8)+3)&~3
    result=bytearray([0xa5])*(pitch*height)
    # Independent integer arrays; never call production pixel or ROP helpers.
    source=[[pattern(x,y,1,bpp) for x in range(width)] for y in range(height)]
    dest=[[pattern(x,y,3,bpp) for x in range(width)] for y in range(height)]
    before=[row[:] for row in dest]
    src=(1,1,12,8);dr=(0,0,13,9)
    if variant==16:source=before;src=(0,0,10,7);dr=(2,1,12,8)
    if variant==20:dr=(-3,-2,16,11)
    if variant==26:source=before;src=(2,1,12,8)
    for y in range(height):
        for x in range(width):
            if not(dr[0]<=x<dr[2] and dr[1]<=y<dr[3]):continue
            if variant==24 and not(2<=x<8 and 1<=y<6 or 10<=x<13 and 3<=y<9):continue
            sx=(x-dr[0])*(src[2]-src[0])//(dr[2]-dr[0])
            sy=(y-dr[1])*(src[3]-src[1])//(dr[3]-dr[1])
            if variant in (21,23,26):sx=src[2]-src[0]-1-sx
            if variant in (22,23,26):sy=src[3]-src[1]-1-sy
            s=source[src[1]+sy][src[0]+sx];d=before[y][x]
            high=120 if bpp==8 else 30000 if bpp==16 else 0x800000
            if variant==17 and s==pattern(3,2,1,bpp):continue
            if variant==18 and d>high:continue
            if variant==19 and (s<=high or d>high):continue
            if variant==25:value=0x3b4c5d
            elif variant<16:
                # Boolean truth table evaluated one bit at a time, rather
                # than the DIB engine's bit-plane minterm implementation.
                value=sum(((variant>>(((s>>bit)&1)*2+((d>>bit)&1)))&1)<<bit for bit in range(bpp))
            else:value=s
            mask=(1<<min(bpp,24))-1
            dest[y][x]=value&mask
    for y,row in enumerate(dest):
        for x,value in enumerate(row):
            at=y*pitch+x*(bpp//8)
            # XRGB8888 retains the original high-byte canary on every write.
            result[at:at+min(3,bpp//8)]=value.to_bytes(4,'little')[:min(3,bpp//8)]
    return bytes(result)

def game_oracle(bpp,fullscreen):
    width,height=(320,200) if fullscreen else (800,600)
    depth=bpp if fullscreen else 32
    pitch=width*(depth//8)
    result=bytearray(pitch*height)
    for y in range(1,10):
        for x in range(2,15):
            if not fullscreen and not (2<=x<8 and 1<=y<6 or 10<=x<13 and 3<=y<9):continue
            value=pattern(x-2,y-1,1,bpp)
            if value==pattern(3,2,1,bpp):continue
            if not fullscreen:
                if bpp==8:value=value*0x010101
                elif bpp==16:
                    r,g,b=(value>>11)&31,(value>>5)&63,value&31
                    value=((r*8+r//4)<<16)|((g*4+g//16)<<8)|(b*8+b//4)
            offset=y*pitch+x*(depth//8)
            result[offset:offset+depth//8]=value.to_bytes(depth//8,'little')
    at=5*pitch+4*(depth//8)
    result[at:at+depth//8]=((1<<min(depth,24))-1).to_bytes(depth//8,'little')
    return bytes(result)

class DirectDrawTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out=ROOT/'build/host/win32/ddraw';cls.out.mkdir(parents=True,exist_ok=True)
        cls.binary=cls.out/'engine'
        units=[ROOT/'sdk/win32/ddraw'/s for s in ddraw.SOURCES if s!='native.c']
        units += [ROOT/'sdk/win32/gdi32'/s for s in gdi32.SOURCES if s not in ('compiler.c','compiler.S')]
        units += [ROOT/'sdk/win32/services/bytes.c',HERE/'support.c',HERE/'shim.c',HERE/'replay.c',ROOT/'apps/desktop/protocol.c',HERE/'engine.c']
        cls.env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',TMPDIR=str(cls.out))
        cls.units=units
        cls.flags=['-std=c17','-D_GNU_SOURCE','-DW32_HOST_MODEL','-O2','-g','-fno-builtin','-fno-vectorize','-fno-slp-vectorize','-Wall','-Wextra','-Werror','-I'+str(ROOT/'sdk/win32/include'),'-idirafter',str(ROOT/'sdk/sysroot-overlay/include'),'-I'+str(ROOT/'src/kernel/include')]
        subprocess.run(['clang',*cls.flags,'-fsanitize=address,undefined','-fno-sanitize-recover=all',*map(str,units),'-pthread','-o',str(cls.binary)],check=True,env=cls.env)
    def execute(self,mode,*args):
        result=subprocess.run([str(self.binary),mode,*map(str,args)],capture_output=True,text=True,env=self.env,timeout=60)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS',result.stdout)
        return result.stdout
    def test_generation_layout_dispatch_refcounts_and_cap_sizes(self):self.execute('abi')
    def test_locks_dc_flip_loss_and_ten_regrants_without_leaks(self):
        session=json.loads((HERE/'session.json').read_text())
        recording=self.out/'session.bin';recording.write_bytes(b''.join(bytes.fromhex(m) for m in session['messages']))
        self.execute('state',recording)
    def test_palette_partial_updates_present_without_flip(self):self.execute('palettes')
    def test_windowed_primary_clipping_and_gamma(self):self.execute('windowed')
    def test_modes_private_data_client_storage_and_bltfast(self):self.execute('operations')
    def test_reentrant_user_publication_release_and_pacing_worker_lifetime(self):self.execute('callback-lifetime')
    def test_concurrent_surface_and_interface_lifetime(self):self.execute('concurrency')
    def test_windowed_and_555_shims(self):self.execute('shims')
    def test_memory_exhaustion_recovery_and_allocation_failures(self):self.execute('memory')
    def test_independent_pixels_and_pinned_digests(self):
        folder=self.out/'frames';folder.mkdir(exist_ok=True)
        self.execute('frames',folder)
        pin=json.loads((HERE/'digests.json').read_text())
        for bpp in (8,16,32):
            for variant in range(27):
                name=f'{bpp}-{variant:02d}'
                actual=(folder/(name+'.bin')).read_bytes()
                expected=oracle(bpp,variant)
                self.assertEqual(actual,expected,name)
                self.assertEqual(hashlib.sha256(actual).hexdigest(),pin['frames'][name],name)
    def test_authored_fixture_game_scenes_windowed_fullscreen_all_depths(self):
        folder=self.out/'games';folder.mkdir(exist_ok=True)
        self.execute('games',folder)
        pins=json.loads((HERE/'digests.json').read_text())['games']
        for bpp in (8,16,32):
            for fullscreen in (False,True):
                name=f"game-{'full' if fullscreen else 'window'}-{bpp}"
                path=folder/(name+'.bin');actual=path.read_bytes()
                self.assertEqual(actual,game_oracle(bpp,fullscreen),name)
                self.assertEqual(hashlib.sha256(actual).hexdigest(),pins[name],name)
                path.unlink()

    def test_scalar_flip_cost_and_exercise_driver(self):
        binary=self.out/'benchmark'
        subprocess.run(['clang',*self.flags,'-mno-sse','-mno-sse2','-mno-mmx',*map(str,self.units),'-pthread','-o',str(binary)],check=True,env=self.env)
        result=subprocess.run([str(binary),'benchmark'],capture_output=True,text=True,env=self.env,check=True)
        self.assertIn('PASS benchmark',result.stdout)
        (ROOT/'build/codex/f5-11-cost.txt').write_text(result.stdout)
        exercise=ddraw.build_exercise(self.out/'exercise')
        self.assertEqual(census.inspect(exercise.read_bytes())['machine'],'i386')

    def test_pe_scalar_instruction_audit_and_provisional_census(self):
        record=ddraw.build(self.out/'builtin')
        parsed=census.inspect((self.out/'builtin/ddraw.dll').read_bytes())
        names={n for export in parsed['exports'] for n in export['names']}
        self.assertTrue(ddraw.PUBLIC<=names)
        self.assertEqual(json.loads((self.out/'builtin/ddraw-audit.json').read_text())['fpu_simd_instructions'],[])
        self.assertEqual(record['name'],'ddraw.dll')
        coverage=json.loads((self.out/'builtin/ddraw-coverage.json').read_text())
        self.assertTrue(coverage['exports'])
        self.assertTrue(all(e['status']=='implemented' for e in coverage['exports']))
