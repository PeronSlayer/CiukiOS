"""G26 pure wire/storage preferences oracle; sequential, no image or guest."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[3]

class UserTools(unittest.TestCase):
    def test_terminal_commands(self):
        out=ROOT/'build/host/g26';out.mkdir(parents=True,exist_ok=True)
        binary=out/'terminal-commands'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'src/kernel/include'),
            '-idirafter',str(ROOT/'sdk/sysroot-overlay/include'),
            str(ROOT/'tests/host/net/terminal_commands_test.c'),'-o',str(binary)],check=True)
        result=subprocess.run([binary],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1'))
        print(result.stdout,end='')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_wire_storage_and_preferences(self):
        out=ROOT/'build/host/g26';out.mkdir(parents=True,exist_ok=True)
        binary=out/'user-tools'
        subprocess.run(['clang','-std=c17','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            str(ROOT/'tests/host/net/user_tools_test.c'),str(ROOT/'sdk/netsurf/port/download_store.c'),'-o',str(binary)],check=True)
        with tempfile.TemporaryDirectory(dir=out) as directory:
            result=subprocess.run([binary,directory],capture_output=True,text=True,
                env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1'))
            print(result.stdout,end='')
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__=='__main__':unittest.main()
