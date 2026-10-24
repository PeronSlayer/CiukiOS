"""G23 appearance behavior, independent of screenshot digests."""
from pathlib import Path
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[3]
class AppearanceTests(unittest.TestCase):
    def test_appearance(self):
        out=ROOT/'build/host/appearance';out.mkdir(parents=True,exist_ok=True)
        binary=out/'appearance'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I'+str(ROOT/'apps/desktop'),
                        *[str(ROOT/p) for p in ('tests/host/desktop/appearance_test.c','apps/desktop/settings.c','apps/desktop/theme.c')],
                        '-o',str(binary)],check=True)
        subprocess.run([binary],check=True)
if __name__=='__main__': unittest.main()
