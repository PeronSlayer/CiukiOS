"""F5-08 production conversion, lease revocation and -O2 host costs; no VM."""
from pathlib import Path
import os
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[2]
class PresenterTests(unittest.TestCase):
    def build(self,source,flags=()):
        out=ROOT/'build/host/presenter';out.mkdir(parents=True,exist_ok=True)
        program=out/(Path(source).stem+('-bench' if '-O2' in flags else ''))
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',TMPDIR=str(out))
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                        '-I',str(ROOT/'src/kernel/include'),*flags,str(ROOT/source),
                        str(ROOT/'src/kernel/vm/vga_presenter.c'),str(ROOT/'src/kernel/vm/virtual_vga.c'),
                        '-o',str(program)],check=True,env=env)
        return program,env
    def test_conversion(self):
        program,env=self.build('tests/host/present_surface_test.c',['-fsanitize=address,undefined'])
        result=subprocess.run([program],check=True,capture_output=True,text=True,env=env)
        print(result.stdout,end='')
    def test_revocation(self):
        program,env=self.build('tests/host/display_present_test.c',['-fsanitize=address,undefined','-pthread'])
        result=subprocess.run([program],capture_output=True,text=True,env=env)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        print(result.stdout,end='')
    def test_mode_transactions(self):
        program,env=self.build('tests/host/display_present_modes_test.c',[
            '-fsanitize=address,undefined',str(ROOT/'src/kernel/core/display_present.c')])
        result=subprocess.run([program],capture_output=True,text=True,env=env)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        print(result.stdout,end='')
    def test_cost(self):
        program,env=self.build('tests/host/present_surface_test.c',['-O2','-mno-sse','-mno-sse2','-mno-mmx','-fno-vectorize','-fno-slp-vectorize'])
        result=subprocess.run([program,'--bench'],check=True,capture_output=True,text=True,env=env)
        (ROOT/'build/codex').mkdir(parents=True,exist_ok=True)
        (ROOT/'build/codex/f5-08-costs.txt').write_text(result.stdout)
        print(result.stdout,end='')
    def test_shared_header(self):
        for name in ('present.h','present_abi.h'):
            self.assertEqual((ROOT/'src/kernel/include/ciuki'/name).read_bytes(),
                             (ROOT/'sdk/sysroot-overlay/include/ciuki'/name).read_bytes())
if __name__=='__main__':unittest.main()
