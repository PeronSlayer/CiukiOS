from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[4]
class DiagnosticTests(unittest.TestCase):
    def test_complete_protected_record_sinks(self):
        out=ROOT/'build/host/win32/diagnostics';out.mkdir(parents=True,exist_ok=True)
        program=out/'sinks'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined',
            '-I'+str(ROOT/'src/kernel/include'),str(ROOT/'tests/host/win32/diagnostics/sinks_test.c'),
            str(ROOT/'src/kernel/lib/fmt.c'),'-o',str(program)],check=True)
        r=subprocess.run([program],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
    def test_eight_text_crash_files_no_dump(self):
        out=ROOT/'build/host/win32/diagnostics';out.mkdir(parents=True,exist_ok=True)
        program=out/'crash-files'
        subprocess.run(['clang','-std=c17','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror',
            '-include',str(ROOT/'tests/host/desktop/replay_base.h'),
            '-fsanitize=address,undefined','-I'+str(ROOT/'src/kernel/include'),
            str(ROOT/'tests/host/win32/diagnostics/crash_file_test.c'),str(ROOT/'apps/desktop/win32_crash.c'),'-o',str(program)],check=True)
        with tempfile.TemporaryDirectory(dir=out) as temp, tempfile.TemporaryDirectory(dir=out) as native:
            result=subprocess.run([program,temp,native],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('PASS native crash text rotation',result.stdout)
            files=sorted(Path(native).iterdir());self.assertEqual(len(files),8)
            texts=[f.read_text() for f in files]
            self.assertEqual(sorted(int(t.splitlines()[0].split('=')[1]) for t in texts),list(range(3,11)))
            newest=max(texts,key=lambda t:int(t.splitlines()[0].split('=')[1]))
            self.assertIn('Executable: crasher',newest);self.assertIn('PID: 109 TID: 9',newest)
            self.assertIn('Page fault: write of unmapped address 00000010',newest)
            self.assertIn('Exception: 0E, error code 000006',newest);self.assertIn('EIP: 00401234',newest)
            self.assertIn('ESP: BFFFFFF0',newest);self.assertEqual(newest.split('Stack:')[1].split(),['DEADBEEF','01020304']+['????????']*6)
            for f in files:self.assertLessEqual(f.stat().st_size,2048)
            files=sorted(Path(temp).iterdir());self.assertEqual(len(files),8)
            sequences=[]
            for file in files:
                self.assertEqual(file.suffix,'.TXT');self.assertLessEqual(file.stat().st_size,16384)
                text=file.read_text();sequences.append(int(text.splitlines()[0].split('=')[1]))
                self.assertEqual(text.split('Stack:')[1].split(),['12345678']+['????????']*6+['87654321'])
            self.assertEqual(sorted(sequences),list(range(5,13)))
    def test_kernel_faults_records_and_io(self):
        out=ROOT/'build/host/win32/diagnostics';out.mkdir(parents=True,exist_ok=True)
        program=out/'diagnostics'
        sources=['tests/host/win32/diagnostics/kernel_test.c','src/kernel/fs/path.c','tests/host/proc/signal_legacy.c','tests/host/proc/desktop_legacy.c']
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'src/kernel/include'),
            *[str(ROOT/s) for s in sources],'-o',str(program)],check=True)
        r=subprocess.run([program],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertIn('PASS AV/divide',r.stdout)
        (ROOT/'build/codex/f5-16-diagnostic-cases.log').write_text(r.stdout)
