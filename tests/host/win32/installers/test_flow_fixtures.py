"""Pinned engines and authored installer fixtures; never QEMU or image builds.

H1 skips are limited to the existing adapter's demonstrated confinement
preflight. A fault after PE startup is a failure. Parser unit vectors are
explicitly labelled as such and are never installer runtime evidence.
"""
# SPDX-License-Identifier: MIT
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib

from scripts.win32 import cabinet, census, installer_fixture as fixture
from . import host_services
ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
OUT = ROOT / 'build/host/win32/installers'


def decode_cabinet(raw):
    """Independent bounded MS-CAB reader, using stdlib DEFLATE, not the writer."""
    if len(raw) < 44 or raw[:4] != b'MSCF':
        raise ValueError('header')
    length, table = struct.unpack_from('<I4xI', raw, 8)
    minor, major, folders, files, flags = struct.unpack_from('<BBHHH', raw, 24)
    if (length != len(raw) or (major, minor) != (1, 3) or folders != 1 or flags or table != 44):
        raise ValueError('unsupported header')
    offset, count, compression = struct.unpack_from('<IHH', raw, 36)
    if compression != 1 or offset > len(raw):
        raise ValueError('folder')
    names = []
    for _ in range(files):
        if table + 16 > offset:
            raise ValueError('file table')
        size, at, folder = struct.unpack_from('<IIH', raw, table)
        end = raw.index(0, table + 16, offset)
        name = raw[table + 16:end].decode('ascii')
        if folder or name in {n for n, _, _ in names}:
            raise ValueError('file')
        names.append((name, at, size)); table = end + 1
    if table != offset:
        raise ValueError('table trailing bytes')
    data = bytearray()
    for _ in range(count):
        crc, packed, unpacked = struct.unpack_from('<IHH', raw, offset)
        block = raw[offset + 8:offset + 8 + packed]
        check = block + struct.pack('<HH', packed, unpacked)
        # Compute the CAB XOR checksum independently (including tail order).
        def xor_bytes(b):
            result = 0
            while len(b) >= 4:
                result ^= int.from_bytes(b[:4], 'little'); b = b[4:]
            return result ^ int.from_bytes(b, 'big')
        if len(block) != packed or block[:2] != b'CK' or crc != (xor_bytes(block) ^ xor_bytes(check[-4:])):
            raise ValueError('CFDATA checksum')
        decoder = zlib.decompressobj(-15, zdict=bytes(data[-32768:])) if data else zlib.decompressobj(-15)
        decoded = decoder.decompress(block[2:]) + decoder.flush()
        if len(decoded) != unpacked or not decoder.eof or decoder.unused_data:
            raise ValueError('MSZIP block')
        data += decoded; offset += 8 + packed
    if offset != len(raw):
        raise ValueError('CFDATA trailing bytes')
    result = {}
    for name, at, size in names:
        if at > len(data) or size > len(data) - at:
            raise ValueError('file range')
        result[name] = bytes(data[at:at + size])
    return result


class CabinetWriterTests(unittest.TestCase):
    def test_temporary_reservation_cleanup_ownership(self):
        OUT.mkdir(parents=True, exist_ok=True)
        for name in ('selfextract', 'infchain'):
            with self.subTest(fixture=name):
                binary = OUT / (name + '-temporary-tree-ubsan')
                source = ROOT / 'tests/win32/fixtures/installers' / (name + '.c')
                subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-DW32_HOST_MODEL', '-DFIXTURE_SOURCE="' + str(source) + '"',
                    '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    '-I' + str(ROOT / 'sdk/win32/include'), '-I' + str(ROOT / 'src/kernel/include'),
                    HERE / 'temporary_tree_test.c', '-o', binary], check=True)
                result = subprocess.run([binary], capture_output=True, text=True, check=True)
                print(result.stdout, end='')

    def test_shared_gate_installer_routing_ubsan(self):
        OUT.mkdir(parents=True, exist_ok=True)
        binary = OUT / 'shared-gate-ubsan'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-include', ROOT / 'tests/host/desktop/replay_base.h',
            '-fsanitize=undefined', '-fno-sanitize-recover=all',
            '-I' + str(ROOT / 'apps/desktop'), '-I' + str(ROOT / 'sdk/win32/include'),
            '-I' + str(ROOT / 'src/kernel/include'), '-idirafter', ROOT / 'sdk/sysroot-overlay/include',
            ROOT / 'tests/host/desktop/w32_fixture_gate_test.c', '-o', binary], check=True)
        result = subprocess.run([binary], capture_output=True, text=True, check=True)
        print(result.stdout, end='')

    def test_native_installer_coordinator_checkpoint_page(self):
        OUT.mkdir(parents=True, exist_ok=True)
        binary = OUT / 'controller-page-ubsan'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-D_GNU_SOURCE', '-include', ROOT / 'tests/host/desktop/replay_base.h',
            '-fsanitize=undefined', '-fno-sanitize-recover=all',
            '-I' + str(ROOT / 'apps/desktop'), '-I' + str(ROOT / 'sdk/win32/include'),
            '-I' + str(ROOT / 'src/kernel/include'),
            '-idirafter', ROOT / 'sdk/sysroot-overlay/include',
            HERE / 'controller_page_test.c', '-o', binary], check=True)
        result = subprocess.run([binary], capture_output=True, text=True, check=True)
        print(result.stdout, end='')

    def test_inf_caller_queue_callback_and_context_lifetime(self):
        OUT.mkdir(parents=True, exist_ok=True)
        binary = OUT / 'inf-callback-ubsan'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-DW32_HOST_MODEL', '-fsanitize=undefined', '-fno-sanitize-recover=all',
            '-I' + str(ROOT / 'sdk/win32/include'), '-I' + str(ROOT / 'src/kernel/include'),
            HERE / 'inf_callback_test.c', '-o', binary], check=True)
        result = subprocess.run([binary], capture_output=True, text=True, check=True)
        print(result.stdout, end='')

    def test_native_registry_observer_on_production_directory(self):
        OUT.mkdir(parents=True, exist_ok=True)
        binary = OUT / 'registry-observer'
        sources = [ROOT / 'apps/desktop/installer_registry.c', HERE / 'registry_observer_test.c', HERE / 'host.c',
                   ROOT / 'sdk/win32/kernel32/text.c', ROOT / 'sdk/win32/services/bytes.c']
        sources += [ROOT / 'sdk/win32/advapi32' / n for n in ('registry.c', 'text.c')]
        sources += [ROOT / 'apps/w32srv' / n for n in ('registry.c', 'storage.c', 'defaults.c', 'directory.c', 'posix_io.c')]
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DW32_HOST_MODEL',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-fsanitize=undefined',
            '-fno-sanitize-recover=all', '-I' + str(ROOT / 'sdk/win32/include'),
            '-I' + str(ROOT / 'src/kernel/include'), *sources, '-o', binary], check=True)
        with tempfile.TemporaryDirectory(dir=OUT) as directory:
            result = subprocess.run([binary, directory], capture_output=True, text=True, check=True)
        print(result.stdout, end='')

    def test_9x_profile_and_wide_delayed_move_refusal(self):
        OUT.mkdir(parents=True, exist_ok=True)
        for name, sources in (
                ('profile-9x', [ROOT / 'tests/host/win32/profile_test.c', ROOT / 'sdk/win32/kernel32/profile.c']),
                ('security-9x', [HERE / 'security_test.c', ROOT / 'sdk/win32/advapi32/security.c']),
                ('load-flags', [HERE / 'load_flags_test.c', ROOT / 'sdk/win32/kernel32/modules.c']),
                ('wide-9x', [HERE / 'delay_move_test.c', ROOT / 'sdk/win32/kernel32/wide.c'])):
            binary = OUT / name
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-DW32_HOST_MODEL', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-fsanitize=undefined', '-fno-sanitize-recover=all',
                '-I' + str(ROOT / 'sdk/win32/include'), '-I' + str(ROOT / 'src/kernel/include'),
                *sources, '-o', binary], check=True)
            result = subprocess.run([binary], capture_output=True, text=True, check=True)
            print(result.stdout, end='')

    def test_multiblock_exact_bytes_checksums_and_corruption(self):
        files = {'one.txt': b'Ciuki\r\n', 'two.bin': bytes(range(256)) * 300}
        raw = cabinet.mszip(files)
        self.assertEqual(raw, cabinet.mszip(dict(reversed(list(files.items())))))
        self.assertEqual(decode_cabinet(raw), files)
        for at in (len(raw) - 1, struct.unpack_from('<I', raw, 36)[0]):
            changed = bytearray(raw); changed[at] ^= 1
            with self.assertRaises(ValueError):
                decode_cabinet(changed)
        for name in ('../escape', 'C:\\escape', '/escape', 'folder//file', 'a\0b'):
            with self.assertRaises(ValueError):
                cabinet.mszip({name: b'authored'})


class FlowFixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        cls.fixtures = OUT / 'fixtures'
        try:
            cls.record = fixture.build(cls.fixtures)
        except FileNotFoundError as error:
            raise unittest.SkipTest(str(error)) from error

    def test_fixture_and_nsis_uninstaller_subsystem_versions(self):
        # Amendment 11: inspect the producer output without the i386 preflight.
        # NSIS 2.51 Main.c delegates with CopyFile, preserving this PE header.
        _, archives = fixture.build_nsis.archives()
        with zipfile.ZipFile(archives['binary']) as archive:
            stub = archive.read('nsis-2.51/Stubs/zlib')
        images = {name: (self.fixtures / name).read_bytes()
                  for name in (fixture.NAME, 'child.exe', 'infchain.exe', 'selfextract.exe')}
        images['NSIS 2.51 release zlib stub'] = stub
        images['WriteUninstaller output (delegation source)'] = fixture.uninstaller_bytes(images[fixture.NAME])
        for name, raw in images.items():
            with self.subTest(image=name):
                pe, = struct.unpack_from('<I', raw, 0x3c)
                self.assertEqual(raw[pe:pe + 4], b'PE\0\0')
                self.assertEqual(struct.unpack_from('<H', raw, pe + 4)[0], 0x14c)
                optional = pe + 24
                self.assertEqual(struct.unpack_from('<H', raw, optional)[0], 0x10b)
                self.assertEqual(struct.unpack_from('<HH', raw, optional + 48), (4, 0))
                self.assertEqual(struct.unpack_from('<H', raw, optional + 68)[0], 2)
                print(f'PASS {name}: i386 PE32 GUI subsystem version 4.0')

    def test_inf_manifest_retains_installed_child_and_completion(self):
        # CopyFiles installs the executable; RunPostSetupCommands runs it in
        # Program Files. Only the extracted temporary tree belongs to cleanup.
        expected = json.loads((fixture.SOURCE / 'expected.json').read_text())
        files = expected['states']['infchain.exe']['files']
        directory = 'C/Program Files/Ciuki INF Fixture/'
        self.assertEqual(files[directory + 'child.exe'], fixture.sha(self.fixtures / 'child.exe'))
        self.assertEqual(files[directory + 'child.done'], hashlib.sha256(b'child completed\r\n').hexdigest())

    def test_compiled_fixture_and_cabinet_independent_hash_manifest(self):
        self.assertEqual(self.record['identical_runs'], 2)
        expected = json.loads((fixture.SOURCE / 'expected.json').read_text())
        self.assertEqual(self.record['files'], expected['artifacts'])
        self.assertEqual(self.record['identities'], expected['identities'])
        header = (ROOT / 'src/probes/win32_installers/identities.h').read_text()
        for name, prefix in (('child.exe', 'CHILD'), ('uninstall.exe', 'UNINSTALL')):
            self.assertIn(f'INSTALLER_{prefix}_SIZE {expected["identities"][name]["size"]}u', header)
            self.assertIn(f'INSTALLER_{prefix}_FNV 0x{expected["identities"][name]["fnv1a32"]:08x}u', header)
        store = ROOT / 'build/apps/win32'
        if (store / 'fixtures.json').is_file():
            staged = json.loads((store / 'fixtures.json').read_text())['files']
            for name, digest in expected['artifacts'].items():
                self.assertEqual(staged[name], digest)
                self.assertEqual(fixture.sha(store / 'fixtures' / name), digest)
        files = decode_cabinet((self.fixtures / 'fixture.cab').read_bytes())
        self.assertEqual(set(files), {'child.exe', 'payload.txt', 'chain.inf'})
        for name, data in files.items():
            source = fixture.SOURCE / name if name == 'payload.txt' else self.fixtures / name
            self.assertEqual(hashlib.sha256(data).hexdigest(), fixture.sha(source))
        image = census.inspect((self.fixtures / fixture.NAME).read_bytes())
        self.assertEqual(image['machine'], 'i386')

    def test_authored_stubs_import_real_core_exports(self):
        from scripts.win32 import upper_dlls
        core = ROOT / 'build/apps/win32'
        if not (core / 'builtin-store.json').is_file():
            self.skipTest('core outputs absent: make CAP= apps')
        providers = upper_dlls.read_images(core)
        providers.update(upper_dlls.read_images(ROOT / 'build/win32/upper/pe'))
        index = {name: upper_dlls.export_index(info) for name, (_, info) in providers.items()}
        # The production loader forwards precisely this platform-owned set.
        forwards = re.findall(r'X\((\w+)\)', (ROOT / 'sdk/win32/include/w32/shell_exports.h').read_text())
        for name in forwards:
            self.assertIn(name, index['ciukishell.dll'])
            index['shell32.dll'][name] = index['ciukishell.dll'][name]
        for name in (fixture.NAME, 'child.exe', 'selfextract.exe', 'infchain.exe'):
            for row in census.inspect((self.fixtures / name).read_bytes())['imports']:
                for symbol in row['symbols']:
                    found = upper_dlls.resolve(index, row['dll'].lower(), symbol)
                    self.assertFalse(found['error'], f'{name}: {row["dll"]}!{symbol}: {found}')

    def test_observer_negatives_and_suite_parser_unit_vectors(self):
        binary = OUT / 'observer'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all', HERE / 'observer_test.c', '-o', binary], check=True)
        result = subprocess.run([binary], capture_output=True, text=True, check=True)
        print(result.stdout, end='')
        # This exercises the production publisher and runner grammar with
        # unit vectors; it makes no claim that an installer was executed.
        sys.path.insert(0, str(ROOT / 'scripts/test'))
        import evidence
        case = next(c for c in json.loads((ROOT / 'tests/suites/f5-core.json').read_text())['cases']
                    if c['probe'] == 'w32-installer-nsis')
        rows = [dict(event='DATA', **dict(t.split('=', 1) for t in fields.split()))
                for fields in re.findall(r'^RECORD w32-installer-nsis DATA (.*)$', result.stdout, re.M)]
        self.assertEqual(len(rows), 4)
        evidence.evaluate_predicates(rows, case['expected'])
        for i in range(4):
            changed = [dict(r) for r in rows]; changed[i]['manifest'] = '0'
            with self.assertRaises(evidence.EvidenceError):
                evidence.evaluate_predicates(changed, case['expected'])

    def h1(self):
        from tests.host.win32.wine import test_wine_upper as upper
        if not (ROOT / 'build/apps/win32/builtin-store.json').is_file():
            self.skipTest('core outputs absent: make CAP= apps')
        if not upper.local_build_matches():
            self.skipTest('reused PEs do not match the qualified f5-20D record; make CAP= upper')
        directory = OUT / 'h1'; directory.mkdir(exist_ok=True)
        if not getattr(self.__class__, 'h1_program', None):
            reused = [n for n, r in upper.RECORD['dlls'].items() if r['qualified']]
            extras = {'extra_sources': [HERE / 'h1_services.c'],
                      'extra_flags': ['-DH1_INSTALLERS', '-I' + str(HERE)]}
            program, _ = upper.build_h1(directory, reused, **extras)
            (directory / 'upper_table.h').write_text(upper.upper_fixture.table_header(
                [('cabinet.dll', upper.RECORD['dlls']['cabinet.dll']['base'])]))
            arrays = {'cab_source': (self.fixtures / 'fixture.cab').read_bytes(),
                      'cab_payload': (fixture.SOURCE / 'payload.txt').read_bytes(),
                      'cab_child': (self.fixtures / 'child.exe').read_bytes(),
                      'cab_inf': (self.fixtures / 'chain.inf').read_bytes()}
            (directory / 'cab_bytes.h').write_text(''.join('static unsigned char ' + n + '[]={' +
                ','.join(map(str, data)) + '};\n' for n, data in arrays.items()))
            (directory / 'upper_fixture_src.h').write_text('#include "' + str(directory / 'cab_bytes.h') +
                '"\n#include "' + str(HERE / 'fdi_memory.h') + '"\n')
            program = upper.adapter.build(directory / 'core', directory / 'upper_h1.c', windows=False, **extras)
            self.__class__.h1_program = program
        program = self.__class__.h1_program
        upper.require_h1_sandbox(self, program)
        return program

    def test_reused_fdi_exact_extraction_and_bad_checksum(self):
        program = self.h1()
        library, _, _ = host_services.build_bridge(OUT / 'services')
        with tempfile.TemporaryDirectory(dir=OUT) as root:
            service = host_services.Services(root, library, program)
            try:
                code, output = service.start_adapter(['installer-fixture'], 'FDI memory fixture').finish(120)
                (OUT / 'fdi-h1.log').write_text(output)
                self.assertEqual(code, 0, output[-12000:])
                self.assertIn('PASS FDI MSZIP exact extracted bytes', output)
                self.assertIn('kernel_enosys=0', output)
                service.assert_quiescent()
            finally:
                service.close()

    def test_reused_inf_chain_and_selfextract_child_wait(self):
        program = self.h1()
        from .flow_driver import FixtureDriver
        library, _, _ = host_services.build_bridge(OUT / 'services')
        for name in ('infchain.exe', 'selfextract.exe'):
            with self.subTest(image=name):
                with tempfile.TemporaryDirectory(dir=OUT) as root:
                    driver = FixtureDriver(root, library, program, self.fixtures)
                    try:
                        output = driver.chain(name)
                        (OUT / (name + '-h1.log')).write_text(output)
                    finally:
                        (OUT / (name + '-h1.log')).write_text(''.join(driver.outputs))
                        driver.close()

    def test_four_predicate_probe_dry_run(self):
        program = self.h1()
        from .flow_driver import FixtureDriver
        library, _, _ = host_services.build_bridge(OUT / 'services')
        with tempfile.TemporaryDirectory(dir=OUT) as root:
            driver = FixtureDriver(root, library, program, self.fixtures)
            try:
                records = driver.nsis()
                (OUT / 'nsis-probe-dry-run.log').write_text(records)
            finally:
                (OUT / 'nsis-h1.log').write_text(''.join(driver.outputs))
                driver.close()
        sys.path.insert(0, str(ROOT / 'scripts/test'))
        import evidence
        case = next(c for c in json.loads((ROOT / 'tests/suites/f5-core.json').read_text())['cases']
                    if c['probe'] == 'w32-installer-nsis')
        rows = [dict(event='DATA', **dict(t.split('=', 1) for t in fields.split()))
                for fields in re.findall(r'^RECORD w32-installer-nsis DATA (.*)$', records, re.M)]
        evidence.evaluate_predicates(rows, case['expected'])

    def test_nsis_cancellation_preserves_existing_and_late_rollback(self):
        self.run_driver_case(lambda driver: driver.cancellation_cases())

    def test_original_uninstaller_copies_launches_and_exits(self):
        self.run_driver_case(lambda driver: driver.original_uninstall())

    def test_explicit_uninstaller_copy_preserves_unowned_parent(self):
        # Include empty parents: a stray DIRNUL would succeed there instead
        # of merely failing because an unrelated file prevents rmdir.
        for directory in ('C:\\Existing unrelated directory', 'C:\\WINDOWS\\TEMP\\~nsu.tmp'):
            for populated in (False, True):
                with self.subTest(directory=directory, populated=populated):
                    self.run_driver_case(lambda driver: driver.unrelated_uninstall(directory, populated))

    def test_actual_runonce_interruption_and_recovery(self):
        for boundary in ('journal_persisted', 'wininit_tick', 'wininit_retired', 'runonce_consumed', 'runonce_executed'):
            with self.subTest(boundary=boundary):
                self.run_driver_case(lambda driver: driver.interrupted_restart(self.service_library, boundary))

    def run_driver_case(self, action):
        program = self.h1()
        from .flow_driver import FixtureDriver
        self.service_library, _, _ = host_services.build_bridge(OUT / 'services')
        with tempfile.TemporaryDirectory(dir=OUT) as root:
            driver = FixtureDriver(root, self.service_library, program, self.fixtures)
            try: action(driver)
            finally:
                (OUT / (self._testMethodName + '.log')).write_text(''.join(driver.outputs))
                driver.close()

    def test_corrupt_cabinet_cleans_inf_and_selfextract_temporary_trees(self):
        def check(driver, name, code):
            raw = bytearray((self.fixtures / 'fixture.cab').read_bytes()); raw[-1] ^= 1
            driver.paths.path(driver.base + 'fixture.cab').write_bytes(raw)
            driver.execute(name, expected=code)
            driver.service.assert_quiescent()
        for name, code in (('infchain.exe', 4), ('selfextract.exe', 1)):
            with self.subTest(image=name): self.run_driver_case(lambda driver: check(driver, name, code))

    def test_selfextract_timeout_reaps_child_before_removing_tree(self):
        def check(driver):
            driver.service.wait_timeout_once = True
            driver.execute('selfextract.exe', expected=1)
            driver.service.assert_quiescent()
            if len(driver.service.children) < 2: raise AssertionError('no real child to terminate')
        self.run_driver_case(check)


if __name__ == '__main__':
    unittest.main()
