"""Production shell dispatcher and desktop restart driver; no emulator."""
import json
import re
import sys
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[4]
def verify_restart_predicates(text):
    """Feed actual shared-C publisher output to the unchanged suite parser."""
    sys.path.insert(0,str(ROOT/'scripts/test'))
    import evidence
    case=next(c for c in json.loads((ROOT/'tests/suites/f5-core.json').read_text())['cases']
              if c['id']=='w32-restart-qemu-t23')
    records=re.findall(r'^case=\S+ errors=\d+ checked=\d+$',text,re.M)[:6]
    assert len(records)==6, text
    body=[('BEGIN','')]+[('DATA',r) for r in records]+[('END','status=PASS')]
    lines=[f'CIUKI_TEST v=1 run=12345678 seq={i:06d} probe=w32-restart event={event}'+(' '+fields if fields else '')
           for i,(event,fields) in enumerate(body,1)]
    def parse(rows):
        parser=evidence.Parser('12345678','w32-restart')
        for line in rows:
            assert len(line)<=240
            parser.feed(line.encode())
        parser.check(case['expected'])
    parse(lines)
    negatives=0
    for record in records:
        marker=record.split()[0]+' '
        for good,bad in (('checked=1','checked=0'),('errors=0','errors=1')):
            changed=[line.replace(good,bad) if marker in line else line for line in lines]
            try:
                parse(changed)
            except evidence.EvidenceError:
                negatives+=1
            else:
                raise AssertionError('suite accepted missing/failed restart evidence: '+marker)
    assert negatives==12
    (ROOT/'build/codex/f5-07d-predicate-dry-run.log').write_text('\n'.join(lines)+'\n')

class ShellTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out=ROOT/'build/host/win32/shell';cls.out.mkdir(parents=True,exist_ok=True)
        cls.binary=cls.out/'shell'
        sources=['apps/w32srv/'+n+'.c' for n in ('registry','storage','directory','defaults','protocol','posix_io','progman','restart','shell_fs')]
        sources+=['src/probes/win32_restart/exercise.c','apps/desktop/protocol.c','apps/desktop/win32_restart.c','tests/host/win32/shell/shell_test.c']
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-D_POSIX_C_SOURCE=200809L','-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),*[str(ROOT/s) for s in sources],'-o',str(cls.binary)],check=True)
    def run_case(self,mode):
        with tempfile.TemporaryDirectory(dir=self.out) as path:
            p=subprocess.run([self.binary,mode,path],capture_output=True,text=True,timeout=20,
                env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1'))
        (self.out/(mode+'.log')).write_text(p.stdout+p.stderr)
        if mode=='fixture':verify_restart_predicates(p.stdout)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertIn('PASS',p.stdout)
    def test_command_transcripts_and_conversation_ownership(self):self.run_case('dde')
    def test_wininit_order_faults_bounds_and_interrupted_replay(self):self.run_case('wininit')
    def test_runonce_order_commit_failure_and_deletion(self):self.run_case('runonce')
    def test_end_session_accept_veto_force_timeout_and_desktop(self):self.run_case('session')

    def test_restart_guest_fixture_and_predicate_dry_run(self):self.run_case('fixture')
