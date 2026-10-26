"""H01: production credentials, sessions, spawn and channels on fake hardware."""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]


class CredentialTests(unittest.TestCase):
    def test_sdk_credentials(self):
        out = ROOT / 'build/host/security/sdk'
        (out / 'ciuki').mkdir(parents=True, exist_ok=True)
        for name in ('proc.h', 'security.h'):
            (out / 'ciuki' / name).write_bytes((ROOT / 'sdk/sysroot-overlay/include/ciuki' / name).read_bytes())
        (out / 'ciuki/runtime.h').write_text('''#ifndef SDK_TEST_RUNTIME_H
#define SDK_TEST_RUNTIME_H
#include <ciuki/abi.h>
#include <errno.h>
struct sdk_reent { int _errno; };
struct sdk_reent *__getreent(void);
uint32_t sdk_pointer(void *);
uint32_t ciuki_syscall(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
#undef errno
#define errno (__getreent()->_errno)
#define CU_PTR(p) sdk_pointer((void *)(p))
#define CU_CALL(n,b,c,d,s,i,p) ciuki_syscall(CIUKI_SYS_##n,b,c,d,s,i,p)
static inline int ciuki_error(uint32_t r) { return r >= (uint32_t)-4095 ? -(int32_t)r : 0; }
#endif
''')
        program = out / 'credentials'
        env = dict(os.environ, TMPDIR=str(out))
        subprocess.run([
            'clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-I', str(out), '-idirafter', str(ROOT / 'sdk/sysroot-overlay/include'),
            '-I', str(ROOT / 'src/kernel/include'),
            str(ROOT / 'tests/host/security/sdk_credentials_test.c'), '-o', str(program),
        ], check=True, env=env)
        result = subprocess.run([str(program)], capture_output=True, text=True, env=env, timeout=15)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H01 SDK:', result.stdout)

    def test_production_credentials(self):
        out = ROOT / 'build/host/security'
        out.mkdir(parents=True, exist_ok=True)
        program = out / 'credentials'
        env = dict(os.environ, TMPDIR=str(out))
        subprocess.run([
            'clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-I', str(ROOT / 'src/kernel/include'),
            str(ROOT / 'tests/host/security/credentials_test.c'),
            str(ROOT / 'tests/host/proc/signal_legacy.c'),
            str(ROOT / 'src/kernel/lib/sha256.c'),
            str(ROOT / 'src/kernel/lib/fmt.c'),
            str(ROOT / 'src/kernel/probes/selector.c'), '-o', str(program),
        ], check=True, env=env)
        result = subprocess.run([str(program)], capture_output=True, text=True,
                                env=env, timeout=90)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H01 credentials: PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
