"""Desktop gate's independent QMP pixels/stimuli and production selector seam."""
import ctypes
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/test'))
import run as runner
from evidence import EvidenceError


class DesktopGateTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT/'build/host';scratch.mkdir(parents=True,exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.folder = Path(self.temp.name)

    def tearDown(self):self.temp.cleanup()

    def test_selector_server_and_policy(self):
        harness = self.folder/'selector.c'
        harness.write_text('''
#include <ciuki/kernel.h>
void rec_emit(const char *p,const char *e,const char *f,...) {(void)p;(void)e;(void)f;}
void klog(const char *f,...) {(void)f;}
''')
        library = self.folder/'selector.so'
        subprocess.run(['clang','-shared','-fPIC','-std=c17','-Wall','-Wextra','-Werror',
                        '-I',str(ROOT/'src/kernel/include'),str(ROOT/'src/kernel/probes/selector.c'),
                        str(harness),'-o',str(library)],check=True,
                       env=dict(os.environ,TMPDIR=str(self.folder)))
        native = ctypes.CDLL(str(library))
        native.probes_crash_server.argtypes = [ctypes.c_char_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_bool,ctypes.c_bool]
        base = 'f2:crash-isolation run=12345678'
        def choose(s,flags=0,lfb=True,payload=True):
            b=s.encode();return native.probes_crash_server(b,len(b),flags,lfb,payload)
        self.assertEqual(choose(base),1)
        self.assertEqual(choose(base,payload=False),0)
        self.assertEqual(choose(base+' server=standin'),0)
        self.assertEqual(choose(base+' server=desktop',payload=False),1)
        self.assertEqual(choose(base+' server=desktop',lfb=False),0)
        self.assertEqual(choose(base+' safe=1 server=desktop',flags=33),0)
        self.assertEqual(choose(base+' platform=e500 server=desktop',flags=160),1)
        for suffix in (' server=bogus',' server=desktop server=standin',' server=desktop safe=1',
                       ' safe=1 server=standin platform=e500',' server=desktop\n'):
            self.assertEqual(choose(base+suffix,flags=161),-22)
        for phase,probe in ((0,'boot'),(1,'input'),(2,'libc-smoke')):
            self.assertEqual(choose(f'f{phase}:{probe} run=12345678 server=desktop'),-22)
        # All three optional keys exceed the existing 64-byte loader buffer.
        self.assertEqual(choose(base+' platform=e500 safe=1 server=standin',flags=161),-22)
        for kind in ('desktop','standin'):
            self.assertEqual(runner.selector(base+' server='+kind)['server'],kind)
            profile=runner.load(ROOT/'tests/profiles/qemu-e500.json')
            case=dict(probe='crash-isolation',selector=base+' server='+kind)
            _,request=runner.qemu_args('fake',profile,case,'12345678',self.folder/'overlay',self.folder/'bios')
            self.assertEqual(request,base+' platform=e500 server='+kind)
        for text in (base+' server=other',base+' server=desktop safe=1',
                     base+' server=standin server=desktop','f1:input run=12345678 server=desktop',
                     base+' platform=e500 safe=1 server=standin'):
            with self.assertRaises(ValueError):runner.selector(text,'fw_cfg',True)

    def test_stimulus_order_pacing_and_arm(self):
        action = runner.desktop_stimulus()
        self.assertEqual(action[0]['after'],{'event':'ARM','action':'post_fault_input'})
        batches = action[0]['batches']
        self.assertEqual([e['type'] for b in batches for e in b['events']],['key','key','rel','rel','btn','btn'])
        self.assertEqual([batches[i]['events'][0]['data']['down'] for i in (0,1,3,4)],[True,False,True,False])
        self.assertTrue(all(b['pause_ms']==50 for b in batches))
        actions = runner.Actions(action)
        class Parser:records=[]
        class QMP:
            events=[]
            def input_events(self,events):self.events.append(events)
        parser = Parser();qmp = QMP()
        actions.step(parser,qmp,0);self.assertFalse(qmp.events)
        parser.records = [dict(event='ARM',action='post_fault_input',seq='000010')]
        for t in (0,.06,.12,.18,.24):actions.step(parser,qmp,t)
        self.assertTrue(actions.complete);self.assertEqual(qmp.events,[b['events'] for b in batches])
        self.assertTrue(all(o['sync_seq']=='000010' for o in actions.observed))

    def screen(self,width,height):
        spec = importlib.util.spec_from_file_location('gate_portrait',ROOT/'apps/desktop/convert_portrait.py')
        c = importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
        portrait = c.convert(ROOT/'assets/brand/ciuki-logo.png')
        pixels = bytearray(bytes((0x37,0x55,0x64))*width*height)
        x0=(width-256)//2;y0=max(32,32+(height-32-256-36)//2)
        for y in range(256):
            for x in range(256):
                p=(y*256+x)*4;off=((y0+y)*width+x0+x)*3
                pixels[off:off+3]=portrait[p:p+3][::-1]
        return pixels

    def write_screen(self,name,width,height,pixels):
        path=self.folder/name
        path.write_bytes(f'P6\n# QMP synthetic fixture\n{width} {height}\n255\n'.encode()+pixels)
        return path

    def test_external_portrait_cursor_change_and_failures(self):
        for width,height in ((640,480),(800,600),(1024,768)):
            original=self.screen(width,height);before=bytearray(original);after=bytearray(original)
            # Initial cursor: no whole-frame/clock-only digest can pass this check.
            for y in range(height//2,height//2+16):
                off=(y*width+width//2)*3;before[off:off+24]=b'\xff'*24
            b=self.write_screen('before.ppm',width,height,before)
            a=self.write_screen('after.ppm',width,height,after)
            observed=runner.observe_desktop_screen(b,a)
            self.assertEqual(observed['ppm_sha256'],hashlib.sha256(a.read_bytes()).hexdigest())
            self.assertEqual(observed['portrait_pixels_checked'],6144)
            self.assertGreaterEqual(observed['cursor_changed_pixels'],16)
            with self.assertRaisesRegex(EvidenceError,'cursor'):runner.observe_desktop_screen(b,b)
            clock_only=bytearray(before);clock_only[:30]=b'\xff'*30
            a=self.write_screen('clock.ppm',width,height,clock_only)
            with self.assertRaisesRegex(EvidenceError,'cursor'):runner.observe_desktop_screen(b,a)
            x,y,_,_=observed['portrait_region'];after[(y*width+x)*3]^=1
            a=self.write_screen('corrupt.ppm',width,height,after)
            with self.assertRaisesRegex(EvidenceError,'portrait'):runner.observe_desktop_screen(b,a)
        for data in (b'P3\n640 480\n255\n',b'P6\n640 480\n256\n',b'P6\n640 480\n255\nshort',b'P6\n# missing newline'):
            path=self.folder/'malformed.ppm';path.write_bytes(data)
            with self.assertRaises(EvidenceError):runner.read_ppm(path)


if __name__ == '__main__':unittest.main()
