"""F3-20 evidence rejection controls. No QEMU or hardware is executed."""
import copy
import tempfile
import unittest
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import run as runner
from evidence import EvidenceError

class FullscreenEvidenceTests(unittest.TestCase):
    def test_tcg_launch_margin(self):
        cases=runner.load(ROOT/'tests/suites/f3-vm.json')['cases']
        for name in ('vm-fullscreen-qemu-t23','vm-fullscreen-cirrus-qemu-t23'):
            self.assertEqual(next(c for c in cases if c['id']==name)['timeout'],600)
        source=(ROOT/'src/kernel/probes/f3_probes_video.c').read_text().split('static int probe_f3_vm_fullscreen(void)',1)[1]
        self.assertIn('until=deadline_after_ms(300000)',source)
        self.assertIn('"case=launch ms=%llu",g_ticks-launch_started',source)

    def test_exact_screen_equality(self):
        parent=ROOT/'build/host'; parent.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as folder:
            before,after=(Path(folder)/s for s in ('before.ppm','after.ppm'))
            # read_ppm admits only desktop-sized dumps (256..2048 x 300..2048).
            width,height=256,300; size=width*height*3
            pixels=bytes(i%251 for i in range(size)); header=b'P6\n%d %d\n255\n'%(width,height)
            before.write_bytes(header+pixels); after.write_bytes(header+pixels)
            result=runner.observe_fullscreen_screen(before,after)
            self.assertTrue(result['equal']); self.assertEqual((result['width'],result['height']),(width,height))
            with self.assertRaises(EvidenceError): runner.observe_fullscreen_demo(before,after)
            after.write_bytes(header+pixels[:-1]+b'\xff')
            self.assertTrue(runner.observe_fullscreen_demo(before,after)['changed'])
            with self.assertRaises(EvidenceError): runner.observe_fullscreen_screen(before,after)
            after.write_bytes(b'P6\n%d %d\n255\n'%(width+4,height)+pixels+bytes(4*height*3))
            with self.assertRaises(EvidenceError): runner.observe_fullscreen_screen(before,after)
            with self.assertRaises(EvidenceError): runner.observe_fullscreen_demo(before,after)
            after.write_bytes(b'P6\n4 3\n255\n'+bytes(range(36)))
            with self.assertRaises(EvidenceError): runner.observe_fullscreen_screen(before,after)
            before.write_bytes(header+bytes(size)); after.write_bytes(before.read_bytes())
            with self.assertRaises(EvidenceError): runner.observe_fullscreen_screen(before,after)

    def test_case_requires_demo_restore_and_cleanup(self):
        case=next(c for c in runner.load(ROOT/'tests/suites/f3-vm.json')['cases'] if c['id']=='vm-fullscreen-qemu-t23')
        self.assertEqual(runner.selector(case['selector'].format(run_id='12345678'))['probe'],'vm-fullscreen')
        runner.Actions(case['actions'])
        self.assertTrue(case['fullscreen_screen'])
        rows=[dict(case='demo',active=1,ms=10000,mode=19),dict(case='restore',active=0,display=1,width=1024,height=768),
              dict(case='cleanup',restored=1,vms=0,tasks=0,pages=0,handles=0)]
        def parse(rows):
            parser=runner.Parser('12345678','vm-fullscreen')
            records=[('BEGIN',dict(version=1))]+[('DATA',r) for r in rows]+[('END',dict(status='PASS',error=0))]
            for seq,(event,fields) in enumerate(records,1):
                data=' '.join(f'{k}={v}' for k,v in fields.items())
                parser.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=vm-fullscreen event={event} {data}\n'.encode())
            return parser
        self.assertTrue(parse(rows).check(case['expected']))
        for index,key,value in [(0,'active',0),(0,'ms',9999),(1,'display',0),(2,'handles',1)]:
            broken=copy.deepcopy(rows); broken[index][key]=value
            with self.assertRaises(EvidenceError): parse(broken).check(case['expected'])
        with self.assertRaises(EvidenceError): parse(rows[1:]).check(case['expected'])

if __name__=='__main__': unittest.main()
