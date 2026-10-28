"""G21 startup programs: production RPC, policy persistence and dispatcher."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import argparse
SANITIZER = "address,undefined"

ROOT = Path(__file__).resolve().parents[3]

class StartupTests(unittest.TestCase):
    def test_startup(self):
        work = ROOT / 'build/host/startup'
        work.mkdir(parents=True, exist_ok=True)
        (work/'include').mkdir(exist_ok=True)
        if not (work/'include/ciuki').exists():
            (work/'include/ciuki').symlink_to(ROOT/'sdk/sysroot-overlay/include/ciuki', target_is_directory=True)
        flags = ['clang', '-std=c17', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize='+SANITIZER, '-fno-sanitize-recover=undefined',
                 '-I'+str(work/'include'), '-I'+str(ROOT/'sdk/win32/include'), '-I'+str(ROOT/'src/kernel/include'),
                 '-I'+str(ROOT/'apps/desktop'), '-idirafter', str(ROOT/'sdk/sysroot-overlay/include')]
        io = work/'posix-io.o'
        subprocess.run([*flags, '-c', ROOT/'apps/w32srv/posix_io.c', '-o', io], check=True)
        # sec-user-05: libreg's reg_load/reg_commit reference the broker
        # transport since sec-k06; a host build has no remote roots.
        stub = work/'remote.c'
        stub.write_text('#include <errno.h>\n#include <stdint.h>\nstruct reg_remote;struct rt;\n'
                        'int reg_remote_load(struct reg_remote *r,struct rt **t){(void)r;(void)t;errno=EOPNOTSUPP;return -1;}\n'
                        'int reg_remote_commit(struct reg_remote *r,struct rt *t,uint64_t g){(void)r;(void)t;(void)g;errno=EOPNOTSUPP;return -1;}\n'
                        'void reg_remote_close(struct reg_remote *r){(void)r;}\n')
        remote = work/'remote.o'
        subprocess.run([*flags, '-c', stub, '-o', remote], check=True)
        sources = ['apps/desktop/'+n+'.c' for n in ('startup', 'win32_registry', 'win32_launch', 'protocol')]
        sources += ['apps/w32srv/'+n+'.c' for n in ('registry', 'storage', 'directory', 'defaults')]
        sources += ['sdk/libreg/'+n+'.c' for n in ('registry', 'storage', 'hardware')]
        sources += ['tests/host/desktop/startup_test.c']
        binary = work/'startup-test'
        subprocess.run([*flags, '-include', ROOT/'tests/host/desktop/replay_base.h',
                        *(ROOT/s for s in sources), io, remote, '-pthread', '-o', binary], check=True)
        with tempfile.TemporaryDirectory(dir=work) as directory:
            subprocess.run([binary, directory], check=True, timeout=30)

if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--sanitizer', default=SANITIZER)
    args, rest=parser.parse_known_args()
    SANITIZER=args.sanitizer
    unittest.main(argv=[__file__, *rest])
