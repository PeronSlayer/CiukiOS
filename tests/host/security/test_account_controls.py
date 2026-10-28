"""H12 (sec-user-04): User Accounts, saver/resume lock, administrator log-off
of a locked session and G32 upgrade enrollment.

- The Control Panel's account client (apps/control/accounts_client.c) against
  the production account service's decoder and digest (account_client_test.c).
- The production LOGON coordinator + ACCOUNT service + k01/k07 kernel host
  harness of H10 (logon_service_test.c, built with -DCIUKI_H12_ACCOUNTS and
  account_controls_test.inc): administrator/Standard/Guest account controls
  with apply-time service checks, saver and Stand by locks through LOGON, a
  password-less owner, the PASSWORD notice, an administrator's log-off of a
  locked session and the G32 upgrade enrollment end to end.
ASan/UBSan with leak detection throughout. The desktop's ordering (gate
before the saver or Stand by returns) is RWM (window_model_test.c); the page
model is the desktop suite's control_test.c.
"""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/security/account_controls'
WARN = ['-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections']
SANITIZE = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
INCLUDE = ['-I', str(ROOT / 'apps/accounts'), '-I', str(ROOT / 'apps/logon'), '-I', str(ROOT / 'sdk/private'),
           '-I', str(ROOT / 'apps/desktop'), '-I', str(ROOT / 'apps/control'), '-I', str(ROOT / 'src/kernel/include')]
ACCOUNT = ('crypto', 'codec', 'owner', 'bootstrap', 'protocol', 'store', 'service', 'migration', 'persist', 'results',
           'auth', 'enrollment', 'manifest', 'checkpoint', 'logon', 'kernel_adapter', 'server', 'firstboot')
USER = ('sdk/private/accounts_link.c', 'tests/host/security/fake.c', 'apps/desktop/users.c',
        'apps/logon/logon_proto.c', 'apps/logon/links.c', 'apps/logon/coordinator.c', 'apps/control/accounts_client.c')


def _objects(env, sources, defines=()):
    objects = []
    for source in sources:
        obj = OUT / (source.replace('/', '_') + '.o')
        subprocess.run(['clang', *WARN, '-D_POSIX_C_SOURCE=200809L', *defines, *SANITIZE, *INCLUDE, '-c', str(ROOT / source),
                        '-o', str(obj)], check=True, env=env)
        objects.append(str(obj))
    return objects


class AccountClientTests(unittest.TestCase):
    def test_client_codec_matches_service(self):
        OUT.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, TMPDIR=str(OUT))
        program = OUT / 'account_client'
        sources = ['tests/host/security/account_client_test.c', 'apps/control/accounts_client.c', 'apps/accounts/protocol.c',
                   'apps/accounts/codec.c', 'apps/accounts/crypto.c', 'apps/accounts/auth.c', 'src/kernel/lib/sha256.c']
        subprocess.run(['clang', *WARN, '-D_POSIX_C_SOURCE=200809L', *SANITIZE, '-Wl,--gc-sections', *INCLUDE,
                        *(str(ROOT / s) for s in sources), '-o', str(program)], check=True, env=env)
        result = subprocess.run([str(program)], capture_output=True, text=True, env=env, timeout=120)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H12 account client codec:', result.stdout)


class AccountServiceTests(unittest.TestCase):
    """Production LOGON + ACCOUNT + kernel host harness (H10's), H12 scenarios."""
    program = None

    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        cls.env = dict(os.environ, TMPDIR=str(OUT))
        objects = _objects(cls.env, [f'apps/accounts/{n}.c' for n in ACCOUNT] + list(USER))
        cls.program = OUT / 'account_controls'
        subprocess.run(['clang', *WARN, '-DCIUKI_H12_ACCOUNTS', '-Wl,--gc-sections', *SANITIZE, *INCLUDE,
                        str(ROOT / 'tests/host/security/logon_service_test.c'),
                        str(ROOT / 'tests/host/security/secure_input_queue.c'),
                        str(ROOT / 'tests/host/proc/signal_legacy.c'), str(ROOT / 'src/kernel/lib/sha256.c'),
                        str(ROOT / 'src/kernel/lib/tzrule.c'), str(ROOT / 'src/kernel/lib/fmt.c'),
                        str(ROOT / 'src/kernel/probes/selector.c'), *objects, '-pthread', '-o', str(cls.program)],
                       check=True, env=cls.env)
        names = [line.split()[-1] for line in
                 subprocess.check_output(['nm', '-g', '--defined-only', str(cls.program)], text=True).splitlines() if line.split()]
        assert len(names) == len(set(names)), 'duplicate exported symbol'

    def run_scenario(self, name, markers):
        result = subprocess.run([str(self.program), name], capture_output=True, text=True, env=self.env, timeout=600)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f'H10 logon service {name}: PASS', result.stdout)
        for marker in markers:
            self.assertIn(marker, result.stdout)

    def test_user_accounts_roles(self):
        self.run_scenario('accounts', ('H12 administrator:', 'H12 Standard user:', 'H12 Guest:'))

    def test_last_administrator(self):
        self.run_scenario('last-admin', ('H12 last administrator:',))

    def test_saver_and_resume_lock(self):
        self.run_scenario('saver', ('H12 saver:', 'H12 resume:'))

    def test_blank_owner_and_password_notice(self):
        self.run_scenario('saver-blank', ('H12 blank owner:', 'H12 password set:'))

    def test_administrator_logs_off_locked_session(self):
        self.run_scenario('override', ('H12 administrator log-off:',))

    def test_g32_upgrade_enrollment(self):
        self.run_scenario('upgrade', ('H12 G32 upgrade:',))


if __name__ == '__main__':
    unittest.main()
