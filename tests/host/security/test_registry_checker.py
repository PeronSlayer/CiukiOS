"""H07 sec-sysvc2: Registry Checker in the production SYSTEM registry broker.

Links the production broker (apps/registry/broker.c, checker.c), the
Registry Checker engine and client (apps/sysrecovery/regcheck.c with
RC_REMOTE_TEST: root "" is a broker request) and libreg on a real fixture
directory; the kernel binding (peer identities, start status, clock) is a
fake. Administrator, standard user, Guest, interim session and SYSTEM.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class RegistryCheckerTests(unittest.TestCase):
    def test_registry_checker_broker(self):
        out = ROOT / 'build/host/security/registry-checker'
        out.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, TMPDIR=str(out))
        sources = ['tests/host/security/registry_checker_test.c', 'apps/registry/broker.c', 'apps/registry/checker.c',
                   'apps/sysrecovery/regcheck.c', 'apps/w32srv/registry.c', 'apps/w32srv/storage.c',
                   'apps/w32srv/defaults.c', 'apps/w32srv/directory.c',
                   'sdk/libreg/registry.c', 'sdk/libreg/storage.c', 'sdk/libreg/transport.c']
        program = out / 'checker'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-macro-redefined',
                        '-DCIUKI_REG_TRANSPORT_FAKE', '-DRC_REMOTE_TEST', '-Wno-misleading-indentation',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I' + str(ROOT / 'sdk/win32/include'), '-I' + str(ROOT / 'src/kernel/include'),
                        *[str(ROOT / s) for s in sources], '-o', str(program)], check=True, env=env)
        with tempfile.TemporaryDirectory(prefix='drive-', dir=out) as drive:
            result = subprocess.run([str(program), drive], capture_output=True, text=True, env=env, timeout=120)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H07 sec-sysvc2 Registry Checker broker: PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
