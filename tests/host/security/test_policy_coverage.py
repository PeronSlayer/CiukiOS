"""H13 (sec-k08): coverage and bounded-cost audit of the security programme.

Census: every published native syscall selector, call-101 operation, call-99
service and sub-action, channel service/operation (channel.c max_op and the
servers' private protocols), device command (call 69) and VFS access/policy
kind is derived from the sources and must have exactly one mapping in
policy_census.py (policy class plus the test that covers it). A selector
without a mapping, a stale mapping or a mapping whose test marker no longer
exists fails. The dispatchers' default branches are checked to deny.

Adversarial harness: production code of each frontend (native syscalls and
call 99/101 on the FAT/VFS/policy/OWNER stack, the call-101 channels with
LOGON/ACCOUNT routes, the DOS VM, the Win32 personality, the libreg/registry
broker) runs Standard-user and Guest attempts on SYSTEM state, another
user's profile/hive/crash logs/processes/tokens/windows and the sealed
boot/service set; every attempt is refused and repeated attempts leak no fd,
subject, pin, page or VM. The existing harnesses' teardown checks then
require zero of each.

Bounded cost: the native harness prints "H13 cost" figures (warm descriptor
checks without storage lookups, metadata at 32 profiles/256 roots, cold
lookup cost, identity limits); docs/validation/sec-k08.md records them.
"""
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/security/policy-coverage'
SAN = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
WARN = ['-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror']


def census_module():
    spec = importlib.util.spec_from_file_location('policy_census', Path(__file__).with_name('policy_census.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(cmd, env, timeout=300):
    result = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=timeout)
    return result


class PolicyCensusTests(unittest.TestCase):
    """Source-derived census; no compilation."""

    def test_every_selector_has_one_mapping(self):
        c = census_module()
        derived = c.derive(ROOT)
        problems = c.check(ROOT, derived)
        self.assertEqual(problems, [], '\n'.join(problems))
        total = sum(len(v) for v in derived.values())
        print(f'H13 census: {total} selectors in {len(derived)} families, each with one policy/test mapping')

    def test_dispatchers_deny_unknown_selectors(self):
        c = census_module()
        problems = c.check_defaults(ROOT)
        self.assertEqual(problems, [], '\n'.join(problems))

    def test_a_new_selector_without_mapping_fails(self):
        """The census fails closed: an added selector, a removed one or a lost test marker is reported."""
        c = census_module()
        derived = c.derive(ROOT)
        derived['syscall'] = derived['syscall'] + ['CIUKI_SYS_NEW_CALL']
        derived['devctl'] = [d for d in derived['devctl'] if d != 'MEDIA_FORMAT']
        problems = c.check(ROOT, derived)
        self.assertIn('syscall: CIUKI_SYS_NEW_CALL has no policy/test mapping', problems)
        self.assertIn('devctl: stale mapping MEDIA_FORMAT (no longer in the sources)', problems)
        saved = c.MAP['call99']['CIUKI_RECOVERY_SETTING']
        c.MAP['call99']['CIUKI_RECOVERY_SETTING'] = (saved[0], 'tests/host/security/system_services_test.c::no such marker')
        self.assertTrue(any('does not contain "no such marker"' in p for p in c.check(ROOT, c.derive(ROOT))))


class PolicyAdversarialTests(unittest.TestCase):
    """Production frontends; Standard user and Guest attempts are refused."""

    def setUp(self):
        OUT.mkdir(parents=True, exist_ok=True)
        self.env = dict(os.environ, TMPDIR=str(OUT))

    def test_native_frontend_and_bounded_cost(self):
        """Native syscalls, call 99 and call 101 on the FAT/VFS/policy/OWNER stack (the H16 fixture)."""
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/fs/fixtures.py')], check=True, env=self.env)
        owner = [f'src/kernel/fs/{n}.c' for n in ('owner', 'owner_wire', 'owner_store', 'owner_checkpoint', 'owner_roots',
                                                   'owner_vfs', 'security_policy', 'security_path', 'sysvc', 'sysvc_drivers',
                                                   'sysvc_jobs')]
        sources = ['tests/host/security/system_services_test.c', 'tests/host/proc/signal_legacy.c',
                   'tests/host/proc/media_legacy.c', 'tests/host/proc/desktop_legacy.c',
                   'src/kernel/fs/fs_port.c', 'src/kernel/fs/blkreq.c', 'src/kernel/fs/cache.c',
                   'src/kernel/fs/path.c', 'src/kernel/fs/fat.c', 'src/kernel/fs/vfs.c',
                   'src/kernel/fs/fs_ro.c', 'src/kernel/fs/iso9660.c', 'tests/host/fs/fake.c',
                   'src/kernel/probes/f2_probes_files.c', 'src/kernel/lib/sha256.c',
                   'src/kernel/net/ipx_config.c', 'src/kernel/ndis/package.c', 'src/kernel/lib/tzrule.c', *owner]
        program = OUT / 'native'
        subprocess.run(['clang', *WARN, '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', *SAN,
                        '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-DCIUKI_POLICY_COVERAGE', '-pthread',
                        '-I', str(ROOT / 'src/kernel/include'), '-I', str(ROOT / 'src/kernel/fs'),
                        *[str(ROOT / s) for s in sources], '-o', str(program)], check=True, env=self.env)
        r = run([str(program), str(ROOT / 'build/host/fs/fat32.img'), str(ROOT / 'build/host/fs/fat16.img')], self.env)
        lines = [l for l in r.stdout.splitlines() if l.startswith('H13')]
        print('\n'.join(lines))
        self.assertEqual(r.returncode, 0, r.stdout[-6000:] + r.stderr[-6000:])
        self.assertIn('H13 native:', r.stdout)
        self.assertIn('H16 sec-sysvc SYSTEM services', r.stdout)   # the fixture's zero-leak teardown ran
        warm = [l for l in lines if l.startswith('H13 cost warm-fd')]
        self.assertEqual(len(warm), 2)
        for line in warm:
            self.assertRegex(line, r'disk reads 0, OWNER reads 0,')
        for name in ('H13 cost metadata:', 'H13 cost cold-lookup:', 'H13 cost identity:'):
            self.assertTrue(any(l.startswith(name) for l in lines), name)

    def test_channels_and_registry_broker(self):
        """Call-101 channels (kernel max_op, connect roles) and the production registry broker (H07 adapter fixture)."""
        flags = [*WARN, '-ffunction-sections', '-fdata-sections', *SAN]
        include = ['-I' + str(ROOT / 'src/kernel/include'), '-I' + str(ROOT / 'sdk/win32/include')]
        objects = []
        for name in ('apps/registry/broker.c', 'apps/registry/kernel_adapter.c', 'apps/registry/server.c',
                     'apps/w32srv/registry.c', 'apps/w32srv/storage.c', 'apps/w32srv/defaults.c', 'apps/w32srv/directory.c',
                     'sdk/libreg/registry.c', 'sdk/libreg/storage.c', 'sdk/libreg/transport.c'):
            obj = OUT / (name.replace('/', '_') + '.o')
            subprocess.run(['clang', *flags, *include, '-c', str(ROOT / name), '-o', str(obj)], check=True, env=self.env)
            objects.append(str(obj))
        program = OUT / 'channels'
        subprocess.run(['clang', *flags, *include, '-Wl,--gc-sections', '-DCIUKI_POLICY_COVERAGE',
                        str(ROOT / 'tests/host/security/registry_adapter_test.c'),
                        str(ROOT / 'tests/host/proc/signal_legacy.c'), str(ROOT / 'src/kernel/lib/sha256.c'),
                        str(ROOT / 'src/kernel/lib/tzrule.c'), str(ROOT / 'src/kernel/lib/fmt.c'),
                        str(ROOT / 'src/kernel/probes/selector.c'), *objects, '-o', str(program)], check=True, env=self.env)
        r = run([str(program)], self.env, 120)
        print('\n'.join(l for l in r.stdout.splitlines() if l.startswith('H13')))
        self.assertEqual(r.returncode, 0, r.stdout[-6000:] + r.stderr[-6000:])
        self.assertIn('H13 channels:', r.stdout)
        self.assertIn('H07 production adapter:', r.stdout)   # zero fd/subject/page teardown

    def test_dos_vm_frontend(self):
        """The DOS VM frontend: production DFV2 redirector over the shared policy (the H06 fixture)."""
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/fs/fixtures.py')], check=True, env=self.env,
                       capture_output=True)
        owner = [f'src/kernel/fs/{n}.c' for n in ('owner', 'owner_wire', 'owner_store', 'owner_checkpoint', 'owner_roots',
                                                   'security_policy')]
        dos = [f'src/kernel/vm/{n}.c' for n in ('dos_identity', 'dos_unified', 'dos_unified_ifs', 'dos_file',
                                                 'dos_file_volume', 'dos_save', 'dos_save_store')]
        sources = ['tests/host/security/dos_identity_test.c', 'tests/host/proc/signal_legacy.c',
                   'tests/host/proc/media_legacy.c', 'tests/host/proc/desktop_legacy.c',
                   'src/kernel/fs/fs_port.c', 'src/kernel/fs/blkreq.c', 'src/kernel/fs/cache.c',
                   'src/kernel/fs/path.c', 'src/kernel/fs/fat.c', 'src/kernel/fs/vfs.c',
                   'src/kernel/fs/fs_ro.c', 'src/kernel/fs/iso9660.c', 'src/kernel/fs/partition.c',
                   'src/kernel/drivers/blkpart.c', 'tests/host/fs/fake.c',
                   'src/kernel/probes/f2_probes_files.c', 'src/kernel/lib/sha256.c',
                   'src/kernel/fs/owner_vfs.c', 'src/kernel/fs/security_path.c', *owner, *dos]
        program = OUT / 'dos'
        subprocess.run(['clang', *WARN, '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', *SAN,
                        '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-DCIUKI_POLICY_COVERAGE', '-pthread',
                        '-I', str(ROOT / 'src/kernel/include'), '-I', str(ROOT / 'src/kernel/fs'),
                        *[str(ROOT / s) for s in sources], '-o', str(program)], check=True, env=self.env, cwd=ROOT)
        r = run([str(program), str(ROOT / 'build/host/fs/fat32.img'), str(ROOT / 'build/host/fs/fat16.img')], self.env)
        print('\n'.join(l for l in r.stdout.splitlines() if l.startswith('H13')))
        self.assertEqual(r.returncode, 0, r.stdout[-6000:] + r.stderr[-6000:])
        self.assertIn('H13 dos:', r.stdout)
        self.assertIn('H06 final:', r.stdout)

    def test_win32_frontend(self):
        """The Win32 personality: production objects, tokens, messages, APCs (the H07 kernel isolation fixture)."""
        sources = ['tests/host/win32/kernel_core_test.c', 'src/kernel/fs/path.c',
                   'tests/host/proc/signal_legacy.c', 'tests/host/proc/desktop_legacy.c']
        program = OUT / 'win32'
        subprocess.run(['clang', *WARN, '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', *SAN,
                        '-DCIUKI_POLICY_COVERAGE', '-I' + str(ROOT / 'src/kernel/include'),
                        *[str(ROOT / s) for s in sources], '-o', str(program)], check=True, env=self.env)
        r = run([str(program), '--security'], self.env, 120)
        print('\n'.join(l for l in r.stdout.splitlines() if l.startswith('H13')))
        self.assertEqual(r.returncode, 0, r.stdout[-6000:] + r.stderr[-6000:])
        self.assertIn('H13 win32:', r.stdout)
        self.assertIn('H07 kernel:', r.stdout)

    def test_account_service_frontend(self):
        """The ACCOUNT service frontend: production service over the H03 boundary fakes."""
        sources = ['tests/host/security/accounts_test.c', 'tests/host/security/fake.c', 'tests/host/security/round2_test.c',
                   *[f'apps/accounts/{n}.c' for n in ('crypto', 'codec', 'owner', 'bootstrap', 'protocol', 'store', 'service',
                                                      'migration', 'persist', 'results', 'auth', 'enrollment', 'manifest',
                                                      'checkpoint', 'logon')],
                   'apps/desktop/users.c', 'src/kernel/lib/sha256.c']
        program = OUT / 'accounts'
        subprocess.run(['clang', '-std=c17', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-I', str(ROOT / 'apps/accounts'), '-I', str(ROOT / 'apps/desktop'), '-I', str(ROOT / 'src/kernel/include'),
                        *[str(ROOT / s) for s in sources], '-pthread', '-o', str(program)], check=True, env=self.env)
        r = run([str(program), 'h13'], self.env, 300)
        print('\n'.join(l for l in r.stdout.splitlines() if l.startswith('H13')))
        self.assertEqual(r.returncode, 0, r.stdout[-6000:] + r.stderr[-6000:])
        self.assertIn('H13 accounts:', r.stdout)

    def test_registry_checker_and_kdf_figures(self):
        """S2-4 hive sizes (production w32srv registry) and the self-test KDF's equivalence with acct_derive."""
        sources = ['tests/host/security/policy_cost_test.c', 'apps/w32srv/registry.c', 'apps/w32srv/storage.c',
                   'apps/w32srv/defaults.c', 'apps/w32srv/directory.c', 'apps/accounts/crypto.c',
                   'src/kernel/lib/pbkdf2.c', 'src/kernel/lib/sha256.c']
        program = OUT / 'cost'
        subprocess.run(['clang', *WARN, *SAN, '-D_POSIX_C_SOURCE=200809L', '-I', str(ROOT / 'sdk/win32/include'),
                        '-I', str(ROOT / 'src/kernel/include'), '-I', str(ROOT / 'apps/accounts'), '-I', str(ROOT / 'apps/desktop'),
                        *[str(ROOT / s) for s in sources], '-o', str(program)], check=True, env=self.env)
        r = run([str(program)], self.env, 120)
        print('\n'.join(l for l in r.stdout.splitlines() if l.startswith('H13') and 'hive file' not in l))
        self.assertEqual(r.returncode, 0, r.stdout[-4000:] + r.stderr[-4000:])
        sizes = re.search(r'machine hive (\d+) bytes, default user template (\d+) bytes, new profile hive (\d+) bytes; '
                          r'hive text bound W32R_TEXT (\d+) bytes; Registry Checker archive bound (\d+) bytes, (\d+) files', r.stdout)
        self.assertTrue(sizes, r.stdout)
        machine, template, profile, hive_bound, archive_bytes, archive_files = map(int, sizes.groups())
        # The measured reason in docs/validation/sec-k08.md: 32 typical profile hives fit the bytes,
        # not the 32 file entries next to the machine set; one hive at its bound alone exceeds the bytes.
        self.assertLess(32 * profile + machine, archive_bytes)
        self.assertGreaterEqual(hive_bound, archive_bytes)
        self.assertEqual(archive_files, 32)
        self.assertIn('H13 kdf:', r.stdout)


if __name__ == '__main__':
    unittest.main()
