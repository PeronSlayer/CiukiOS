"""G31 production scalar printing with filesystem spool evidence."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import os
from pathlib import Path
import subprocess
import sys
import unittest
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT))
from sdk.freetype import build as freetype
from scripts.win32 import gdi32
class PrintingTests(unittest.TestCase):
    def test_shared_queue_and_dc_lifecycle(self):
        out=ROOT/'build/host/win32/printing';out.mkdir(parents=True,exist_ok=True)
        env=dict(os.environ,TMPDIR=str(out))
        lib=freetype.build(out/'freetype',flags=['-O2','-fPIC'],env=env)
        sources=[ROOT/'sdk/win32/gdi32'/n for n in gdi32.SOURCES if n not in ('compiler.c','compiler.S')]
        sources += [ROOT/'sdk/win32/gdi32/native_font.c',ROOT/'sdk/win32/services/bytes.c',ROOT/'sdk/libfont/font.c',ROOT/'apps/libprint/print.c',ROOT/'tests/host/win32/gdi32/printing_test.c']
        flags=['-std=c17','-D_GNU_SOURCE','-DW32_HOST_MODEL','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'sdk/win32/include'),'-idirafter',str(ROOT/'sdk/sysroot-overlay/include'),'-I'+str(ROOT/'src/kernel/include'),*freetype.includes()]
        program=out/'printing-test'
        subprocess.run(['clang',*flags,*map(str,sources),str(lib),'-pthread','-o',str(program)],check=True,env=env)
        root=out/('queue-'+str(os.getpid()))
        subprocess.run([str(program),str(root)],check=True,env=env)
if __name__=='__main__':unittest.main()
