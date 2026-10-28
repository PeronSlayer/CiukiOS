"""sec-runtime1: production clients, CSC1 fragmentation and service handlers.

The host seam supplies only kernel identity/channels and disk storage; it does
not replace account/registry body decoders, policies, handlers or replies.
The native-only branches are compiled, including desktop's SYSTEM route.
ASan/UBSan and default LeakSanitizer remain enabled.
"""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/security/runtime-protocol'
ACCOUNT = ('crypto', 'codec', 'owner', 'bootstrap', 'protocol', 'store', 'service', 'migration',
           'persist', 'results', 'auth', 'enrollment', 'manifest', 'checkpoint', 'logon')


class RuntimeProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        shim = OUT / 'include'
        (shim / 'ciuki').mkdir(parents=True, exist_ok=True)
        (shim / 'ciuki/runtime.h').write_text(
            '#include <ciuki/abi.h>\nuint32_t runtime_pointer(const void *);\n#define CU_PTR(p) runtime_pointer(p)\n')
        (shim / 'unistd.h').write_text('#include <sys/types.h>\nint close(int);\npid_t getpid(void);\nchar *getcwd(char *,size_t);\n')
        for name in ('security.h', 'channel.h'):
            (shim / 'ciuki' / name).write_bytes((ROOT / 'sdk/sysroot-overlay/include/ciuki' / name).read_bytes())
        # newlib passwd layout, as in the existing H11 identity fixture.
        (shim / 'pwd.h').write_text(
            '#include <sys/types.h>\n#include <stddef.h>\n'
            'struct passwd { char *pw_name,*pw_passwd; uid_t pw_uid; gid_t pw_gid;\n'
            'char *pw_comment,*pw_gecos,*pw_dir,*pw_shell; };\nstruct passwd *getpwuid(uid_t);\n')
        cls.flags = ['-std=c17', '-D_GNU_SOURCE', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                     '-Wno-misleading-indentation', '-Wno-macro-redefined', '-include', 'signal.h',
                     '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                     '-I'+str(shim), '-I'+str(ROOT/'src/kernel/include'), '-I'+str(ROOT/'sdk/win32/include'),
                     '-I'+str(ROOT/'apps/accounts'), '-idirafter', str(ROOT/'sdk/sysroot-overlay/include')]
        cls.env = dict(os.environ, TMPDIR=str(OUT))
        sources = [f'apps/accounts/{s}.c' for s in ACCOUNT] + [
            'tests/host/security/fake.c', 'tests/host/security/runtime_protocol_test.c',
            'apps/desktop/users.c', 'apps/desktop/win32_native.c', 'apps/desktop/win32_registry.c',
            'apps/desktop/protocol.c', 'apps/desktop/win32_service.c',
            'apps/control/accounts_client.c', 'apps/logon/logon_proto.c', 'apps/registry/broker.c', 'apps/registry/server.c', 'apps/registry/accounts.c',
            'apps/w32srv/registry.c', 'apps/w32srv/storage.c', 'apps/w32srv/defaults.c', 'apps/w32srv/directory.c',
            'apps/w32srv/security_io.c', 'sdk/libreg/transport.c', 'sdk/libreg/registry.c',
            'sdk/libciuki/account_link.c', 'sdk/libciuki/pwd.c', 'src/kernel/lib/sha256.c']
        objects = []
        for source in sources:
            extra = []
            if source in ('sdk/libreg/transport.c', 'apps/w32srv/security_io.c', 'apps/desktop/win32_native.c'):
                extra += ['-D__CIUKIOS__', '-U__linux__']
            if source == 'apps/desktop/win32_native.c':
                extra += ['-include', 'signal.h']
            if source in ('sdk/libreg/transport.c', 'apps/desktop/win32_native.c', 'sdk/libciuki/account_link.c'):
                extra += ['-Dclose=runtime_close']
            if source == 'apps/registry/accounts.c':
                extra += ['-Dfopen=runtime_fopen']
            if source == 'apps/w32srv/security_io.c':
                extra += ['-Dmkdir=runtime_mkdir']
            obj = OUT / (source.replace('/', '_')+'.o')
            subprocess.run(['clang', *cls.flags, *extra, '-c', str(ROOT/source), '-o', str(obj)], check=True, env=cls.env)
            objects.append(str(obj))
        cls.program = OUT / 'peers'
        subprocess.run(['clang', *cls.flags, '-Wl,--gc-sections', *objects, '-pthread', '-o', str(cls.program)], check=True, env=cls.env)
        # Catch duplicate exports in input objects as well as in the executable.
        names = []
        for obj in objects:
            for line in subprocess.check_output(['nm', '-g', '--defined-only', obj], text=True).splitlines():
                fields = line.split()
                if len(fields) == 3 and fields[1] not in ('W', 'V', 'C'):
                    names.append(fields[2])
        assert len(names) == len(set(names)), 'duplicate external symbol in conformance inputs'

    def test_real_peers(self):
        for role in (1, 2, 3):
            with self.subTest(role=role):
                result = subprocess.run([str(self.program), str(role)], capture_output=True, text=True, env=self.env, timeout=120)
                print(result.stdout, end='')
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertIn('sec-runtime1 real peers: PASS', result.stdout)
