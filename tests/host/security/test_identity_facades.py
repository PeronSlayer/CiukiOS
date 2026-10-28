"""H11 (sec-user-05): application-facing identity on production code.

Kernel token objects behind the ADVAPI32 token/SID/name/privilege facade,
the Win32 startup environment, console backing and shell folders, the
native getpwnam/getpwuid adapters over the ACCOUNTS wire format, and the
per-user mixer preference store. Boundaries only are fixtures; sanitizers
and leak detection stay on.
  nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_identity_facades.py
"""
# SPDX-License-Identifier: GPL-2.0-only
import os
from pathlib import Path
import subprocess
import unittest
ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/security/identity'
SAN = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
WARN = ['-Wall', '-Wextra', '-Werror']


def run(program, *args, timeout=120):
    env = dict(os.environ, TMPDIR=str(OUT))
    result = subprocess.run([str(program), *map(str, args)], capture_output=True, text=True, env=env, timeout=timeout)
    print(result.stdout, end='')
    return result


class IdentityFacadeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)

    def compile(self, sources, output, flags):
        subprocess.run(['clang', '-std=c17', '-O1', '-g', *WARN, *SAN, *flags, *map(str, sources), '-o', str(output)],
                       check=True, env=dict(os.environ, TMPDIR=str(OUT)))

    def test_win32_token_facade_over_kernel_tokens(self):
        win32 = ['-DW32_HOST_MODEL', '-I' + str(ROOT / 'sdk/win32/include'), '-I' + str(ROOT / 'src/kernel/include')]
        objects = []
        for name in ('sdk/win32/advapi32/identity.c', 'sdk/win32/advapi32/text.c', 'sdk/win32/services/bytes.c'):
            obj = OUT / (Path(name).stem + '-facade.o')
            subprocess.run(['clang', '-std=c17', '-O1', '-g', *WARN, *SAN, *win32, '-c', str(ROOT / name), '-o', str(obj)],
                           check=True)
            objects.append(obj)
        program = OUT / 'kernel-identity'
        self.compile([ROOT / 'tests/host/win32/kernel_core_test.c', ROOT / 'src/kernel/fs/path.c',
                      ROOT / 'tests/host/proc/signal_legacy.c', ROOT / 'tests/host/proc/desktop_legacy.c', *objects],
                     program, ['-DCIUKI_H11_IDENTITY', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                               '-I' + str(ROOT / 'src/kernel/include')])
        result = run(program, '--identity')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H11 kernel tokens:', result.stdout)

    def test_win32_environment_console_and_folders(self):
        program = OUT / 'win32-identity'
        sources = ['tests/host/security/identity_win32_test.c', 'sdk/win32/services/start.c', 'sdk/win32/kernel32/console.c',
                   'sdk/win32/shell/folders.c', 'sdk/win32/services/bytes.c']
        self.compile([ROOT / s for s in sources], program,
                     ['-DW32_HOST_MODEL', '-D_GNU_SOURCE', '-I' + str(ROOT / 'sdk/win32/include'), '-I' + str(ROOT / 'src/kernel/include')])
        result = run(program)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H11 Win32 facades:', result.stdout)

    def test_native_passwd_adapters(self):
        # The native SDK's newlib <pwd.h> layout (pw_comment), not the host's.
        include = OUT / 'newlib-pwd'
        include.mkdir(exist_ok=True)
        (include / 'pwd.h').write_text(
            '#ifndef H11_PWD_H\n#define H11_PWD_H\n#include <sys/types.h>\n#include <stddef.h>\n'
            'struct passwd { char *pw_name; char *pw_passwd; uid_t pw_uid; gid_t pw_gid; char *pw_comment;\n'
            '  char *pw_gecos; char *pw_dir; char *pw_shell; };\n'
            'struct passwd *getpwuid(uid_t);\nstruct passwd *getpwnam(const char *);\n'
            'int getpwnam_r(const char *, struct passwd *, char *, size_t, struct passwd **);\n'
            'int getpwuid_r(uid_t, struct passwd *, char *, size_t, struct passwd **);\n#endif\n')
        # The SDK's <ciuki/runtime.h> passes i386 addresses in 32-bit record
        # fields; on the 64-bit host the fixture maps them through a table.
        (include / 'ciuki').mkdir(exist_ok=True)
        (include / 'ciuki' / 'runtime.h').write_text(
            '#ifndef H11_RUNTIME_H\n#define H11_RUNTIME_H\n#include <ciuki/abi.h>\n#include <stdint.h>\n'
            'uint32_t h11_pointer(const void *);\n#define CU_PTR(p) h11_pointer(p)\n#endif\n')
        # The native SDK's <unistd.h> and <ciuki/security.h>/<ciuki/channel.h>:
        # the host's unistd.h collides with the CiukiOS ABI's sysconf names.
        (include / 'unistd.h').write_text('int close(int);\n')
        (include / 'ciuki').mkdir(exist_ok=True)
        for name in ('security.h', 'channel.h'):
            (include / 'ciuki' / name).write_bytes((ROOT / 'sdk/sysroot-overlay/include/ciuki' / name).read_bytes())
        flags = ['-I' + str(include), '-I' + str(ROOT / 'src/kernel/include'), '-idirafter', str(ROOT / 'sdk/sysroot-overlay/include')]
        objects = []
        for name, extra in (('sdk/libciuki/pwd.c', []), ('sdk/libciuki/account_link.c', ['-Dclose=fixture_close']),
                            ('tests/host/security/identity_native_test.c', ['-Dclose=fixture_close'])):
            obj = OUT / (Path(name).stem + '-native.o')
            subprocess.run(['clang', '-std=c17', '-D_GNU_SOURCE', '-O1', '-g', *WARN, *SAN, *flags, *extra, '-c', str(ROOT / name),
                            '-o', str(obj)], check=True)
            objects.append(obj)
        program = OUT / 'native-identity'
        subprocess.run(['clang', *SAN, *map(str, objects), '-o', str(program)], check=True)
        result = run(program)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H11 native passwd:', result.stdout)

    def test_per_user_mixer_preferences(self):
        import shutil
        work = OUT / 'mixer'
        shutil.rmtree(work, ignore_errors=True)
        (work / 'include/ciuki').mkdir(parents=True)
        # As tests/host/desktop/test_system_sounds.py: the SDK headers the store uses.
        for header in ('audio.h', 'display.h', 'font.h', 'registry.h', 'devices.h'):
            (work / 'include/ciuki' / header).write_bytes((ROOT / 'sdk/sysroot-overlay/include/ciuki' / header).read_bytes())
        program = OUT / 'mixer-preferences'
        sources = ['apps/desktop/master_storage.c', 'tests/host/security/identity_preferences_test.c']
        self.compile([ROOT / s for s in sources], program,
                     ['-D_POSIX_C_SOURCE=200809L', '-Wno-macro-redefined', '-I' + str(work / 'include'),
                      '-I' + str(ROOT / 'src/kernel/include'), '-pthread'])
        (work / 'run').mkdir()
        result = run(program, work / 'run')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H11 mixer preferences:', result.stdout)

    def test_xp_shell_folder_merge(self):
        # sec-user-05: merged desktop over the production Files model and the
        # session's per-user shell folders (Start menu/StartUp/progman merges
        # are in tests/host/desktop/test_startup.py and win32/shell).
        import shutil
        work = OUT / 'shell-folders'
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir()
        program = OUT / 'shell-folders-test'
        self.compile([ROOT / 'tests/host/security/identity_shell_folders_test.c', ROOT / 'apps/files/model.c'], program,
                     ['-I' + str(ROOT / 'src/kernel/include'), '-pthread'])
        result = run(program, work)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H11 shell folders:', result.stdout)

if __name__ == '__main__':
    unittest.main()
