"""Safe-removal suite declarations and synchronized USB detach, without a guest."""
import sys
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/test'))
import run as runner

class MediaRemovalTests(unittest.TestCase):
    def test_media_cases(self):
        suite=runner.load_suite('f4-media')
        cases=[c for c in suite['cases'] if c['probe']=='media-remove']
        self.assertEqual(len(cases),1)
        case=cases[0]
        self.assertEqual(runner.selector(case['selector'].format(run_id='12345678'),'fw_cfg',True)['probe'],'media-remove')
        self.assertEqual(runner.fixture_usb_disks(case),[0])
        self.assertEqual({p['where']['case'] for p in case['expected']['predicates']},{'busy','safe_remove','detach'})
        class Parser:
            records=[]
        class Qmp:
            calls=[]
            def command(self,name,args): self.calls.append((name,args)); return {}
        q=Qmp(); a=runner.Actions(case['actions'])
        a.step(Parser,q,0); self.assertFalse(q.calls)
        Parser.records=[{'event':'ARM','action':'usb_detach','seq':'3'}]
        a.step(Parser,q,1); self.assertEqual(q.calls,[('device_del',{'id':'ciuki-usb-storage'})]); self.assertTrue(a.complete)
        for c in suite['cases']:
            if c['probe']=='optical' and 'malformed' not in c['id']:
                self.assertTrue(any(p.get('where',{}).get('case')=='safe_remove' for p in c['expected']['predicates']))
        for device,op in [('ciuki-optical','detach'),('ciuki-usb-storage','eject'),('other','detach')]:
            with self.assertRaises(runner.res.Refusal):
                runner.Actions([{'type':'media','after':{'event':'ARM'},'device':device,'operation':op}])

if __name__=='__main__': unittest.main()
