"""Production shell dispatcher and desktop restart driver; no emulator."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[4]
class ShellTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out=ROOT/'build/host/win32/shell';cls.out.mkdir(parents=True,exist_ok=True)
        cls.binary=cls.out/'shell'
        sources=['apps/w32srv/'+n+'.c' for n in ('registry','storage','directory','defaults','protocol','posix_io','progman','restart','shell_fs')]
        sources+=['apps/desktop/protocol.c','apps/desktop/win32_restart.c','tests/host/win32/shell/shell_test.c']
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-D_POSIX_C_SOURCE=200809L','-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),*[str(ROOT/s) for s in sources],'-o',str(cls.binary)],check=True)
    def run_case(self,mode):
        with tempfile.TemporaryDirectory(dir=self.out) as path:
            p=subprocess.run([self.binary,mode,path],capture_output=True,text=True,timeout=20,
                env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1'))
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertIn('PASS',p.stdout)
        (self.out/(mode+'.log')).write_text(p.stdout+p.stderr)
    def test_command_transcripts_and_conversation_ownership(self):self.run_case('dde')
    def test_wininit_order_faults_bounds_and_interrupted_replay(self):self.run_case('wininit')
    def test_runonce_order_commit_failure_and_deletion(self):self.run_case('runonce')
    def test_end_session_accept_veto_force_timeout_and_desktop(self):self.run_case('session')
