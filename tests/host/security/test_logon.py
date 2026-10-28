"""H10: authenticated logon and session UX.

sec-user-01/02 cases: the account service's private LOGON relay/correlation
core and invariants (accounts_test.c / round2_test.c through test_accounts).
sec-user-03 cases: the production LOGON coordinator (apps/logon) with the
production account service and its kernel adapter on the kernel host harness
(k01 sessions/launch/secure console, k07 backend, channels, the merged input
queue), driven by LOGON_UI and session-desktop requests through the
production CSC1 client; and the LOGON_UI model/view against a scripted LOGON
with frame goldens (record: CIUKI_RECORD_LOGON_UI=1). ASan/UBSan with leak
detection throughout.
"""
import json
import os
from pathlib import Path
import subprocess
import unittest
try:  # imported as a package by discover -t . and as a script directory otherwise
    from .test_accounts import AccountTests as _Accounts
except ImportError:
    from test_accounts import AccountTests as _Accounts

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/security/logon'
WARN = ['-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections']
SANITIZE = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
INCLUDE = ['-I', str(ROOT / 'apps/accounts'), '-I', str(ROOT / 'apps/logon'), '-I', str(ROOT / 'sdk/private'),
           '-I', str(ROOT / 'apps/desktop'), '-I', str(ROOT / 'src/kernel/include'),
           '-idirafter', str(ROOT / 'sdk/sysroot-overlay/include')]
ACCOUNT = ('crypto', 'codec', 'owner', 'bootstrap', 'protocol', 'store', 'service', 'migration', 'persist', 'results',
           'auth', 'enrollment', 'manifest', 'checkpoint', 'logon', 'kernel_adapter', 'server', 'firstboot')
USER = ('sdk/private/accounts_link.c', 'tests/host/security/fake.c', 'apps/desktop/users.c',
        'apps/logon/logon_proto.c', 'apps/logon/links.c', 'apps/logon/coordinator.c',
        'apps/logon/ui.c', 'apps/libdeskui/draw.c', 'apps/libdeskui/keymap.c', 'apps/desktop/theme.c')
GOLDENS = ROOT / 'tests/host/security/logon-ui-goldens.json'


class LogonTests(unittest.TestCase):
    setUpClass = classmethod(_Accounts.setUpClass.__func__)
    run_case = _Accounts.run_case

    def test_full_logon_relay(self):
        self.run_case("round2-logon")

    def test_autologon_route(self):
        self.run_case("round2-autologon")

    def test_enrollment(self):
        self.run_case("round2-enrollment")

    def test_committed_enrollment_outcome(self):
        self.run_case("round3-enrollment-outcome")

    def test_logon_respawn_attempts(self):
        self.run_case("round3-logon-restart")

    def test_distinct_retained_handles(self):
        self.run_case("round3-distinct-handles")

    def test_faulted_queued_authentication(self):
        self.run_case("round3-faulted-queue")

    def test_blank_retirement_and_reboot(self):
        self.run_case("retirement")

    def test_consumed_attempt_and_generations(self):
        self.run_case("protocol")


# --- sec-user-03 block: the LOGON service and LOGON_UI --------------------
def _build_service():
    OUT.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(OUT))
    objects = []
    for source in [f'apps/accounts/{n}.c' for n in ACCOUNT] + list(USER):
        obj = OUT / (source.replace('/', '_') + '.o')
        subprocess.run(['clang', *WARN, '-D_POSIX_C_SOURCE=200809L', *SANITIZE, *INCLUDE, '-c', str(ROOT / source),
                        '-o', str(obj)], check=True, env=env)
        objects.append(str(obj))
    program = OUT / 'logon_service'
    subprocess.run(['clang', *WARN, '-DCIUKI_RUNTIME_UI', '-Wl,--gc-sections', *SANITIZE, *INCLUDE,
                    str(ROOT / 'tests/host/security/logon_service_test.c'),
                    str(ROOT / 'tests/host/security/secure_input_queue.c'),
                    str(ROOT / 'tests/host/proc/signal_legacy.c'), str(ROOT / 'src/kernel/lib/sha256.c'),
                    str(ROOT / 'src/kernel/lib/tzrule.c'), str(ROOT / 'src/kernel/lib/fmt.c'),
                    str(ROOT / 'src/kernel/probes/selector.c'), *objects, '-pthread', '-o', str(program)],
                   check=True, env=env)
    names = [line.split()[-1] for line in subprocess.check_output(['nm', '-g', '--defined-only', str(program)], text=True).splitlines()
             if line.split()]
    assert len(names) == len(set(names)), 'duplicate exported symbol'
    return program, env


class LogonServiceTests(unittest.TestCase):
    """H10 sec-user-03: production LOGON + ACCOUNT + k01/k07 kernel."""
    program = None

    @classmethod
    def setUpClass(cls):
        cls.program, cls.env = _build_service()

    def run_scenario(self, name, markers):
        result = subprocess.run([str(self.program), name], capture_output=True, text=True, env=self.env, timeout=300)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f'H10 logon service {name}: PASS', result.stdout)
        for marker in markers:
            self.assertIn(marker, result.stdout)

    def test_real_welcome_tile(self):
        self.run_scenario('welcome-tile', ('runtime Welcome tile: blank authenticated, protected prompts PASS',))

    def test_ready_image(self):
        self.run_scenario('ready', ('H10 ready boot:', 'H10 ready session:', 'H10 Welcome:', 'H10 logon-screen power:'))

    def test_setup_install(self):
        self.run_scenario('setup', ('H10 lock:', 'H10 LOGON_UI restart:', 'H10 shutdown:'))

    def test_safe_mode_recovery_account(self):
        self.run_scenario('safe', ('H10 Safe Mode:',))

    def test_safe_mode_ready_image(self):
        self.run_scenario('ready-safe', ('H10 Safe Mode ready image:',))

    def test_start_failure_and_logon_restart(self):
        self.run_scenario('restart', ('H10 start failure:', 'H10 LOGON restart:'))


class LogonUiTests(unittest.TestCase):
    """H10 sec-user-03: LOGON_UI model/view, request bodies, masking, frames."""

    def test_logon_ui(self):
        out = ROOT / 'build/host/security/logon_ui'
        frames = out / 'frames'
        frames.mkdir(parents=True, exist_ok=True)
        for old in frames.glob('*'):
            old.unlink()
        env = dict(os.environ, TMPDIR=str(out))
        program = out / 'logon_ui'
        sources = ['tests/host/security/logon_ui_test.c', 'apps/logon/ui.c', 'apps/logon/logon_proto.c',
                   'apps/libdeskui/draw.c', 'apps/libdeskui/keymap.c', 'apps/desktop/theme.c']
        subprocess.run(['clang', *WARN, '-D_POSIX_C_SOURCE=200809L', *SANITIZE, '-Wl,--gc-sections',
                        '-idirafter', str(ROOT / 'sdk/sysroot-overlay/include'), '-I', str(ROOT / 'src/kernel/include'),
                        '-I', str(ROOT / 'apps/logon'), *(str(ROOT / s) for s in sources), '-o', str(program)],
                       check=True, env=env)
        result = subprocess.run([str(program), str(frames)], capture_output=True, text=True, env=env, timeout=120)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H10 logon UI: PASS', result.stdout)
        actual = dict(line.split() for line in (frames / 'goldens.txt').read_text().splitlines() if line.strip())
        if os.environ.get('CIUKI_RECORD_LOGON_UI') == '1':
            GOLDENS.write_text(json.dumps(actual, indent=2, sort_keys=True) + '\n')
        self.assertEqual(actual, json.loads(GOLDENS.read_text()), 'LOGON_UI frame mismatch (review frames in ' + str(frames) + ')')
# --- end sec-user-03 block ----------------------------------------------------

# Do not expose the imported TestCase to unittest discovery.
del _Accounts
