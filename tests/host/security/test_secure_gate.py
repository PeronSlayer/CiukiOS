"""H09: trusted gate, physical secure attention and session teardown (sec-k07).

Links the production k01 coordinator (security.c), the k07 backend
(secure_console.c), grants/presentation, SESSION_LAUNCH (spawn.c), channels
and, as a second unit, the merged native input queue (drivers/i8042.c).
The files program qualifies the real session console description on the
production fd table/POSIX namespace. A source census checks that only the
kernel input queue can raise secure attention or switch the input domain.
"""
import os
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/security'
FLAGS = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all']


class SecureGateTests(unittest.TestCase):
    def test_production_gate(self):
        OUT.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, TMPDIR=str(OUT))
        program = OUT / 'secure_gate'
        subprocess.run([*FLAGS, '-I', str(ROOT / 'src/kernel/include'),
                        str(ROOT / 'tests/host/security/secure_gate_test.c'),
                        str(ROOT / 'tests/host/security/secure_input_queue.c'),
                        str(ROOT / 'tests/host/proc/signal_legacy.c'),
                        str(ROOT / 'src/kernel/lib/sha256.c'), str(ROOT / 'src/kernel/lib/tzrule.c'),
                        str(ROOT / 'src/kernel/lib/fmt.c'), str(ROOT / 'src/kernel/probes/selector.c'),
                        '-o', str(program)], check=True, env=env)
        result = subprocess.run([str(program)], capture_output=True, text=True, env=env, timeout=120)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for marker in ('H09 interim:', 'H09 boot gate:', 'H09 launch rollback:', 'H09 activation/attention:',
                       'H09 sequence 2:', 'H09 physical producers:', 'H09 fences:', 'H09 sequence 3:',
                       'H09 logoff:', 'H09 sequence 1 + logoff:', 'H09 desktop crash:', 'H09 secure gate: PASS'):
            self.assertIn(marker, result.stdout)

    def test_session_console_files(self):
        OUT.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, TMPDIR=str(OUT))
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/fs/fixtures.py')], check=True, env=env)
        program = OUT / 'secure_console_files'
        sources = ['tests/host/security/secure_console_files_test.c', 'tests/host/proc/signal_legacy.c',
                   'tests/host/proc/media_legacy.c', 'tests/host/proc/desktop_legacy.c',
                   'src/kernel/fs/fs_port.c', 'src/kernel/fs/blkreq.c', 'src/kernel/fs/cache.c',
                   'src/kernel/fs/path.c', 'src/kernel/fs/fat.c', 'src/kernel/fs/vfs.c',
                   'src/kernel/fs/fs_ro.c', 'src/kernel/fs/iso9660.c', 'tests/host/fs/fake.c',
                   'src/kernel/probes/f2_probes_files.c', 'src/kernel/lib/sha256.c',
                   *[f'src/kernel/fs/{n}.c' for n in ('owner', 'owner_wire', 'owner_store', 'owner_checkpoint',
                                                       'owner_roots', 'owner_vfs', 'security_policy',
                                                       'security_path')]]
        subprocess.run([*FLAGS, '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread',
                        '-I', str(ROOT / 'src/kernel/include'), '-I', str(ROOT / 'src/kernel/fs'),
                        *[str(ROOT / s) for s in sources], '-o', str(program)], check=True, env=env)
        result = subprocess.run([str(program), str(ROOT / 'build/host/fs/fat32.img')],
                                capture_output=True, text=True, env=env, timeout=120)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H09 session console: PASS', result.stdout)

    def test_attention_census(self):
        """Only the kernel input queue raises attention or switches the domain."""
        callers = {
            'security_attention_physical(': {'src/kernel/drivers/i8042.c', 'src/kernel/proc/secure_console.c',
                                              'src/kernel/include/ciuki/security.h'},
            'input_secure_domain(': {'src/kernel/drivers/i8042.c', 'src/kernel/proc/grants.c',
                                      'src/kernel/include/ciuki/input.h'},
            'grants_console_activate(': {'src/kernel/proc/grants.c', 'src/kernel/proc/secure_console.c',
                                          'src/kernel/include/ciuki/desktop.h'},
            # sec-user-06: the self-test's attention entry lives in the input
            # driver, refuses outside a T boot, and has one caller.
            'input_selftest_attention(': {'src/kernel/drivers/i8042.c', 'src/kernel/core/selftest_checks.c',
                                           'src/kernel/include/ciuki/input.h'},
        }
        for symbol, allowed in callers.items():
            found = set()
            for path in (ROOT / 'src').rglob('*.[ch]'):
                if symbol in path.read_text(errors='replace'):
                    found.add(str(path.relative_to(ROOT)))
            self.assertEqual(found, allowed, symbol)
        # No syscall table entry or SDK wrapper exposes the internal entries.
        for path in (ROOT / 'sdk').rglob('*.[ch]'):
            text = path.read_text(errors='replace')
            self.assertNotIn('input_secure_domain', text, path)
            self.assertNotIn('input_selftest_attention', text, path)   # sec-user-06
            self.assertNotIn('security_console_attention', text, path)
        i8042 = (ROOT / 'src/kernel/drivers/i8042.c').read_text()
        self.assertRegex(i8042, r'secure_attention\(c, code, value\)\) return; /\* sec-k07 \*/\n    enqueue\(c, INPUT_KEY')
        self.assertIn('!secure_trusted && dosvm_fullscreen_input(type,code,value)', i8042)
        security = (ROOT / 'src/kernel/proc/security.c').read_text()
        self.assertIn('if (&secure_console_ops) security_grants=&secure_console_ops;', security)
        self.assertTrue(re.search(r'secure_session_retained\(id\)\) draining = true;', security))


if __name__ == '__main__':
    unittest.main()
