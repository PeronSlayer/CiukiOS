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
        with tempfile.TemporaryDirectory(dir=out) as temp:
            result=subprocess.run([program,temp],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
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
