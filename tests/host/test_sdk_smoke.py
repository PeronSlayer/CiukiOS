"""Execute the smoke assertions against native pinned newlib and SDK wrappers."""
import importlib.util
from pathlib import Path
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[2]


class LibcSmokeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (ROOT/'build/tools/ciuki-sdk/manifest.json').is_file():
            raise unittest.SkipTest('SDK absent; build_sdk is a mandatory prerequisite')
        spec=importlib.util.spec_from_file_location('libc_host',ROOT/'sdk/tests/build_libc_host.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        cls.program=module.build()

    def execute(self,mode=None):
        result=subprocess.run([str(self.program),*([mode] if mode else [])],capture_output=True,text=True,timeout=30)
        lines=[line for line in result.stdout.splitlines() if line.startswith('case=')]
        self.assertEqual(result.stderr,'')
        self.assertTrue(all(len(line)<=240 and all(32<=ord(c)<=126 for c in line) for line in lines))
        return result,lines

    def test_smoke_newlib_runtime_and_exit_callbacks(self):
        result,lines=self.execute()
        self.assertEqual(result.returncode,0,result.stdout)
        self.assertIn('CiukiOS libc smoke: 4294967297 1.25',result.stdout)
        self.assertIn('CiukiOS stderr smoke',result.stdout)
        # f2-13 added three clock reports (clock-monotonic, clock-cpu, sleep-interrupt).
        clocks=[line.split()[0] for line in lines if line.split()[0] in ('case=clock-monotonic','case=clock-cpu','case=sleep-interrupt')]
        self.assertEqual(sorted(clocks),['case=clock-cpu','case=clock-monotonic','case=sleep-interrupt'])
        summaries=[line for line in lines if line.split()[0] in ('case=libc-smoke','case=atexit','case=destructor')]
        self.assertEqual(len(lines),6);self.assertEqual(len(summaries),3)
        for line,name,order in zip(summaries,['libc-smoke','atexit','destructor'],[2,3,4]):
            fields=dict(item.split('=',1) for item in line.split())
            self.assertEqual(fields['case'],name);self.assertEqual(fields['failures'],'0')
            self.assertEqual(fields['order'],str(order));self.assertGreater(int(fields['checks']),10000)

    def test_each_failure_has_its_own_bounded_named_call_3_record(self):
        result,lines=self.execute('fail-uname')
        self.assertEqual(result.returncode,1,result.stdout)
        failed=[line for line in lines if ' expected=1 observed=0 ' in line]
        self.assertEqual(len(failed),1,result.stdout)
        self.assertRegex(failed[0],r'^case=\w*uname\w* expected=1 observed=0 line=\d+ errno=38$')
        self.assertTrue(all('failures=1' in line for line in lines if 'checks=' in line))

    def test_original_readonly_mount_and_missing_uname_reproduce_three_named_failures(self):
        result,lines=self.execute('original')
        self.assertEqual(result.returncode,1,result.stdout)
        failed=[dict(item.split('=',1) for item in line.split()) for line in lines if ' expected=1 observed=0 ' in line]
        self.assertEqual(len(failed),3,result.stdout)
        self.assertEqual([(f['case'],f['errno']) for f in failed[:2]],[('f__NULL','30'),('fd__0','30')])
        self.assertIn('uname',failed[2]['case']);self.assertEqual(failed[2]['errno'],'38')
        self.assertTrue(all('failures=3' in line for line in lines if 'checks=' in line))
