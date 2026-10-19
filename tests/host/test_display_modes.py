"""f1-23 host display tests; never starts QEMU or builds an image."""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('display_loader_model', ROOT / 'scripts/test/loader_model.py')
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)

class DisplayTests(unittest.TestCase):
    def mode(self, id=0x118, w=1024, h=768, bpp=32):
        return dict(mode=id, width=w, height=h, bpp=bpp, attributes=0x91, memory_model=6, planes=1,
                    banked_pitch=w*(bpp//8), linear_pitch=w*(bpp//8)+16, physical_base=0xE0000000,
                    masks=dict(red=[8,16], green=[8,8], blue=[8,0], reserved=[8,24]),
                    linear_masks=dict(red=[8,16], green=[8,8], blue=[8,0], reserved=[8,24]))

    def test_loader_inventory(self):
        c = dict(signature='VESA', version=0x300, total_memory_64k=256)
        modes = [self.mode(0x100+i) for i in range(40)]
        d = model.display_inventory(c, modes, current=0x127)
        self.assertEqual(len(d['modes']), 32)
        self.assertEqual(d['modes'][-1]['id'], 0x127)
        self.assertEqual(d['modes'][0]['pitch'], 4112)
        self.assertEqual(len(model.pack_display_inventory(d)), 596)
        c['version'] = 0x200
        self.assertEqual(model.display_inventory(c,modes)['modes'][0]['pitch'],4096)
        bad=[]
        for key,value in [('width',639),('width',1601),('height',479),('height',1201),('bpp',8),
                          ('attributes',0x11),('planes',2),('physical_base',0),('banked_pitch',1),('mode',0x4118)]:
            m = self.mode(); m[key]=value; bad.append(m)
        self.assertFalse(model.display_inventory(c,bad)['modes'])
        self.assertEqual(len(model.display_inventory(c,[modes[0]]*50)['modes']),1)
        m = self.mode(bpp=16); m['masks'] = dict(red=[5,11],green=[6,5],blue=[5,0],reserved=[0,0])
        self.assertEqual(model.display_inventory(c,[m])['modes'][0]['bpp'],16)
        m=self.mode(bpp=24); m['masks']['reserved']=[0,0]
        self.assertEqual(model.display_inventory(c,[m])['modes'][0]['bpp'],24)
        self.assertFalse(model.display_inventory(None,modes)['modes'])
        print('PASS loader display inventory: 16/24/32 bpp, VBE2/3 pitches, range/LFB/plane/capacity validation, deduplication, 32 entries/boot-mode retention')

    def test_loader_strings(self):
        mem=bytearray(1024*1024)
        mem[0xF1230:0xF1239]=b'S3\x01BIOS\0\0'
        mem[0xFFFDC:]=b'a'*36
        c=dict(signature='VESA',version=0x300,total_memory_64k=256,
               oem_ptr=0xF0001230,vendor_ptr=0xFFF000DC,product_ptr=0xFFFFFFFF)
        d=model.display_inventory(c,[],firmware_memory=mem)
        self.assertEqual(d['oem'],'S3?BIOS')
        self.assertEqual(d['vendor'],'a'*36)
        self.assertEqual(d['product'],'')
        c.update(oem='x'*100,vendor='S3',product='SuperSavage')
        self.assertEqual(len(model.display_inventory(c,[])['oem']),63)
        print('PASS loader controller strings: normalized far pointers, one-MiB bounds, printable ASCII, NUL/truncation')

    def build_run(self, names, include=(), cflags=()):
        parent=ROOT/'build/host'; parent.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='display-',dir=parent) as folder:
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',TMPDIR=folder)
            binary=Path(folder)/'test'
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-I',str(ROOT/'src/kernel/include'),
                            *cflags,
                            *sum((['-I',str(ROOT/i)] for i in include),[]),
                            *(str(ROOT/n) for n in names),'-o',str(binary)],check=True,env=env)
            result=subprocess.run([str(binary)],capture_output=True,text=True,env=env)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print(result.stdout.strip())

    def test_kernel_switch(self):
        self.build_run(['tests/host/display_test.c'])

    def test_checkpoint_queue(self):
        self.build_run(['tests/host/fullscreen_checkpoint_test.c'], cflags=[
            '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections'])

    def test_compositor_and_settings(self):
        self.build_run(['tests/host/desktop/display_modes_test.c','tests/host/desktop/fake.c',
                        *('apps/desktop/'+n+'.c' for n in ('client','protocol','input','compositor','theme','shell','autoplay','autoplay_content','settings','wallpaper','keymap','folder','../files/model','../files/transfer')),
                        *('apps/libdeskui/'+n+'.c' for n in ('draw','widgets','list','menu','dialog','textarea','icons','icon_map'))],include=['apps/desktop'])

    def test_sdk_header_locations_agree(self):
        self.assertEqual((ROOT/'src/sdk/include/ciuki/display.h').read_bytes(),
                         (ROOT/'sdk/sysroot-overlay/include/ciuki/display.h').read_bytes())

if __name__ == '__main__': unittest.main()
