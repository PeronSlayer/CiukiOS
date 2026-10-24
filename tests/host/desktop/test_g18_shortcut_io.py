"""G18 worker-side shortcut persistence and classic ICO parser, no VM."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]
class ShortcutIOTests(unittest.TestCase):
    def test_classic_icons_and_durable_repair(self):
        work = ROOT/'build/host/g18-shortcut'
        work.mkdir(parents=True, exist_ok=True)
        binary = work/'shortcut-io'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-D_POSIX_C_SOURCE=200809L','-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all','-I'+str(ROOT/'apps/desktop'),
                        '-include',str(ROOT/'tests/host/desktop/replay_base.h'),
                        '-I'+str(ROOT/'src/kernel/include'),
                        str(ROOT/'tests/host/desktop/g18_shortcut_io_test.c'),
                        str(ROOT/'apps/desktop/settings_io.c'),str(ROOT/'apps/desktop/protocol.c'),
                        '-pthread','-o',str(binary)],check=True)
        with tempfile.TemporaryDirectory(dir=work) as directory:
            subprocess.run([binary,directory],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1'))
