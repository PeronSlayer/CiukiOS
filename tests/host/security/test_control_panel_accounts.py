"""fx-uaccounts: Control Panel -> User Accounts as a real session process.

The production Control Panel (apps/control/main.c compiled whole: native loop,
cp_job worker, completion channel and the accounts glue; app.c's User
Accounts page) runs as /Programs/control, a session child of the desktop, on
the H10/H12 kernel host harness with the production LOGON coordinator and
ACCOUNT server (logon_service_test.c, -DCIUKI_H12_ACCOUNTS
-DCIUKI_CONTROL_PANEL, control_accounts_test.inc). Its ACCOUNTS link and
QUERY_SELF cross the kernel model's syscall boundary like every harness
process. Replaced only: the desktop window turn, the clock, the completion
pair's transport (host socketpair) and poll()'s 120 s bound
(control_panel_test.c). Administrator, Standard user and Guest each READ and
change through the page; the page must adopt every worker result on the loop
turn after its completion token (the 2026-10-28 "Reading the accounts..."
hang: idle() adopted results only on one-second pages). ASan/UBSan with leak
detection throughout.
"""
# SPDX-License-Identifier: MIT
import importlib.util
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/security/control_panel_accounts'
WARN = ['-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections']
SANITIZE = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
INCLUDE = ['-I', str(ROOT / 'apps/accounts'), '-I', str(ROOT / 'apps/logon'), '-I', str(ROOT / 'sdk/private'),
           '-I', str(ROOT / 'apps/desktop'), '-I', str(ROOT / 'apps/control'), '-I', str(ROOT / 'src/kernel/include')]
ACCOUNT = ('crypto', 'codec', 'owner', 'bootstrap', 'protocol', 'store', 'service', 'migration', 'persist', 'results',
           'auth', 'enrollment', 'manifest', 'checkpoint', 'logon', 'kernel_adapter', 'server', 'firstboot')
USER = ('sdk/private/accounts_link.c', 'tests/host/security/fake.c', 'apps/desktop/users.c',
        'apps/logon/logon_proto.c', 'apps/logon/links.c', 'apps/logon/coordinator.c', 'apps/control/accounts_client.c')
# The Control Panel process, as tests/host/desktop/test_control_jobs.py builds it.
PANEL = ['tests/host/security/control_panel_test.c', 'apps/control/app.c', 'apps/desktop/power_policy.c',
         'apps/systools/devctl.c', 'apps/systools/profiles.c', 'apps/control/driver_pack_ui.c', 'apps/control/display.c',
         'apps/control/fonts.c', 'sdk/libfont/font.c', 'apps/libdeskui/text_ttf.c', 'apps/desktop/protocol.c',
         *(f'apps/desktop/{n}.c' for n in ('settings', 'settings_io', 'sounds', 'wallpaper', 'image', 'compositor', 'theme')),
         *(f'apps/libdeskui/{n}.c' for n in ('draw', 'widgets', 'list', 'menu', 'dialog', 'controls', 'commdlg', 'textarea',
                                             'icons', 'icon_map', 'iconx', 'fileicon'))]


def _module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


class ControlPanelAccountsTests(unittest.TestCase):
    program = None

    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        cls.env = dict(os.environ, TMPDIR=str(OUT))
        objects = []
        for source in [f'apps/accounts/{n}.c' for n in ACCOUNT] + list(USER):
            obj = OUT / (source.replace('/', '_') + '.o')
            subprocess.run(['clang', *WARN, '-D_POSIX_C_SOURCE=200809L', *SANITIZE, *INCLUDE, '-c', str(ROOT / source),
                            '-o', str(obj)], check=True, env=cls.env)
            objects.append(str(obj))
        icons = _module('control_panel_icons', ROOT / 'tests/host/desktop/icon_assets.py')
        ft = _module('control_panel_ft', ROOT / 'sdk/freetype/build.py')
        library = ft.host()
        include = OUT / 'include'
        include.mkdir(exist_ok=True)
        if not (include / 'ciuki').exists():
            (include / 'ciuki').symlink_to(ROOT / 'sdk/sysroot-overlay/include/ciuki', target_is_directory=True)
        for source in PANEL:
            obj = OUT / (source.replace('/', '_') + '.o')
            subprocess.run(['clang', *WARN, '-D_POSIX_C_SOURCE=200809L', *SANITIZE,
                            f'-DDOSBOX_SETTINGS_DIR="{OUT / "settings"}"', *icons.defines(),
                            '-I', str(include), '-I', str(ROOT / 'src/kernel/include'), '-I', str(ROOT / 'sdk/win32/include'),
                            '-idirafter', str(ROOT / 'sdk/sysroot-overlay/include'), *ft.includes(),
                            '-c', str(ROOT / source), '-o', str(obj)], check=True, env=cls.env)
            objects.append(str(obj))
        cls.program = OUT / 'control_panel_accounts'
        subprocess.run(['clang', *WARN, '-DCIUKI_H12_ACCOUNTS', '-DCIUKI_CONTROL_PANEL', '-Wl,--gc-sections', *SANITIZE,
                        *INCLUDE, str(ROOT / 'tests/host/security/logon_service_test.c'),
                        str(ROOT / 'tests/host/security/secure_input_queue.c'),
                        str(ROOT / 'tests/host/proc/signal_legacy.c'), str(ROOT / 'src/kernel/lib/sha256.c'),
                        str(ROOT / 'src/kernel/lib/tzrule.c'), str(ROOT / 'src/kernel/lib/fmt.c'),
                        str(ROOT / 'src/kernel/probes/selector.c'), *objects, str(library), '-pthread', '-lm',
                        '-o', str(cls.program)], check=True, env=cls.env)
        names = [line.split()[-1] for line in
                 subprocess.check_output(['nm', '-g', '--defined-only', str(cls.program)], text=True).splitlines()
                 if line.split()]
        assert len(names) == len(set(names)), 'duplicate exported symbol'

    def test_user_accounts_page_adopts_service_results(self):
        result = subprocess.run([str(self.program), 'control-panel'], capture_output=True, text=True, env=self.env,
                                timeout=900)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H10 logon service control-panel: PASS', result.stdout)
        for marker in ('H12 Control Panel administrator:', 'H12 Control Panel Standard user:', 'H12 Control Panel Guest:'):
            self.assertIn(marker, result.stdout)


if __name__ == '__main__':
    unittest.main()
