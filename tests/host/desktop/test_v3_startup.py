"""Production PE fixture readiness guard, with a service boundary oracle."""
import os
from pathlib import Path
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[3]
class StartupTests(unittest.TestCase):
    def test_no_pe_before_service_registration(self):
        out=ROOT/'build/host/desktop-startup';out.mkdir(parents=True,exist_ok=True)
        binary=out/'startup'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-include',str(ROOT/'tests/host/desktop/replay_base.h'),
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'apps/desktop'),
                        '-I'+str(ROOT/'src/kernel/include'),'-idirafter',str(ROOT/'sdk/sysroot-overlay/include'),
                        str(ROOT/'apps/desktop/v3_fixture.c'),str(ROOT/'apps/desktop/protocol.c'),
                        str(ROOT/'tests/host/desktop/v3_startup_test.c'),'-o',str(binary)],check=True)
        # Keep leak detection enabled; a restricted sandbox must report its
        # LeakSanitizer failure instead of changing this test's policy.
        p=subprocess.run([binary],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1'))
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
