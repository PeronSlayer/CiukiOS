"""Synthetic PE/INF fixtures only; never consume the owner's driver pack."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('pe_import_census', ROOT / 'scripts/pe_import_census.py')
census = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(census)


def pe_fixture(libraries=None, *, wide=False, fallback=False, delay=False, va_delay=False):
    """One raw-backed section with PE import descriptors built independently."""
    libraries = libraries if libraries is not None else {'NDIS.SYS': ['NdisMRegisterMiniport', 'NdisMSleep']}
    image = bytearray(0x4200)
    image[:2] = b'MZ'
    struct.pack_into('<I', image, 0x3c, 0x80)
    image[0x80:0x84] = b'PE\0\0'
    opt_size = 240 if wide else 224
    struct.pack_into('<HHIIIHH', image, 0x84, 0x8664 if wide else 0x14c,
                     1, 0, 0, 0, opt_size, 0x102)
    opt = 0x98
    struct.pack_into('<H', image, opt, 0x20b if wide else 0x10b)
    base = 0x400000
    struct.pack_into('<Q' if wide else '<I', image, opt + (24 if wide else 28), base)
    struct.pack_into('<II', image, opt + 32, 0x1000, 0x200)
    struct.pack_into('<HH', image, opt + 40, 5, 1)
    struct.pack_into('<HH', image, opt + 48, 5, 1)
    struct.pack_into('<II', image, opt + 56, 0x5000, 0x200)
    struct.pack_into('<H', image, opt + 68, 1)
    dd = opt + (112 if wide else 96)
    struct.pack_into('<I', image, dd - 4, 16)
    section = opt + opt_size
    image[section:section + 8] = b'.idata\0\0'
    struct.pack_into('<IIII', image, section + 8, 0x4000, 0x1000, 0x4000, 0x200)
    cursor = 0x1000

    def alloc(data):
        nonlocal cursor
        rva = cursor
        image[rva - 0x1000 + 0x200:rva - 0x1000 + 0x200 + len(data)] = data
        cursor += (len(data) + 7) & ~7
        return rva

    step = 32 if delay else 20
    table = alloc(bytes((len(libraries) + 1) * step))
    if libraries:
        struct.pack_into('<II', image, dd + (13 if delay else 1) * 8,
                         table, (len(libraries) + 1) * step)
    for i, (dll, names) in enumerate(libraries.items()):
        name_rva = alloc(dll.encode() + b'\0')
        vals = []
        for name in names:
            if isinstance(name, int):
                vals.append((1 << (63 if wide else 31)) | name)
            else:
                rva = alloc(b'\0\0' + name.encode() + b'\0')
                vals.append(rva + (base if va_delay else 0))
        thunk = alloc(struct.pack('<' + ('Q' if wide else 'I') * (len(vals) + 1), *vals, 0))
        at = table - 0x1000 + 0x200 + i * step
        if delay:
            delta = base if va_delay else 0
            struct.pack_into('<8I', image, at, 0 if va_delay else 1,
                             name_rva + delta, 0, thunk + delta,
                             0 if fallback else thunk + delta, 0, 0, 0)
        else:
            struct.pack_into('<5I', image, at, 0 if fallback else thunk, 0, 0, name_rva, thunk)
    return bytes(image)


def inf_fixture(binary='nic.sys', *, hardware=r'PCI\VEN_10EC&DEV_8139', decoration='NTx86.5.1', cls='Net'):
    return f'''[Version]
Signature="$Windows NT$"
Class={cls}
[Manufacturer]
%vendor%=Models,{decoration}
[Models.{decoration}]
%desc%=Install, {hardware}
[Install]
CopyFiles=@{binary}
[Install.Services]
AddService=NIC,2,Service
[Service]
ServiceBinary=%12%\\{binary}
[Strings]
vendor="Vendor"
desc="NIC"
'''


class PEParsingTests(unittest.TestCase):
    def test_named_ordinal_duplicate_imports_and_dll_case(self):
        parsed = census.parse_pe(pe_fixture({'ndis.sys': ['NdisMSleep', 17, 'NdisMSleep'],
                                             'HAL.dll': ['KeStallExecutionProcessor']}))
        self.assertEqual(parsed['imports'], {'NDIS.SYS': ['#17', 'NdisMSleep'],
                                            'HAL.DLL': ['KeStallExecutionProcessor']})
        self.assertEqual(parsed['machine'], '0x014c')
        self.assertEqual(parsed['os_version'], [5, 1])

    def test_original_thunk_fallback(self):
        self.assertEqual(census.parse_pe(pe_fixture(fallback=True))['imports'],
                         census.parse_pe(pe_fixture())['imports'])

    def test_pe32_plus_inventory(self):
        result = census.parse_pe(pe_fixture({'ntoskrnl.exe': ['KeGetCurrentIrql', 65535]}, wide=True))
        self.assertEqual(result['format'], 'PE32+')
        self.assertEqual(result['imports']['NTOSKRNL.EXE'], ['#65535', 'KeGetCurrentIrql'])

    def test_delay_imports_rva_and_legacy_va(self):
        for wide, va in ((False, False), (True, False), (False, True)):
            with self.subTest(wide=wide, va=va):
                result = census.parse_pe(pe_fixture(delay=True, wide=wide, va_delay=va, fallback=True))
                self.assertEqual(result['imports'], {})
                self.assertIn('NdisMSleep', result['delay_imports']['NDIS.SYS'])

    def test_absent_import_directory(self):
        self.assertEqual(census.parse_pe(pe_fixture({}))['imports'], {})

    def test_legacy_le_vxd(self):
        data = bytearray(80); data[:2] = b'MZ'
        struct.pack_into('<I', data, 0x3c, 64); data[64:66] = b'LE'
        self.assertEqual(census.parse_pe(data)['format'], 'LE')

    def test_truncated_headers_and_raw_section(self):
        data = pe_fixture()
        for length in (0, 2, 60, 128, 200, 350, 500, len(data) - 1):
            with self.subTest(length=length), self.assertRaises(census.PEError):
                census.parse_pe(data[:length])

    def test_directory_count_optional_header_bounds(self):
        data = bytearray(pe_fixture())
        struct.pack_into('<I', data, 0x98 + 92, 17)
        with self.assertRaisesRegex(census.PEError, 'directories'):
            census.parse_pe(data)

    def test_unbacked_and_overflowing_import_rvas(self):
        for rva in (0x5000, 0xfffffff0):
            data = bytearray(pe_fixture())
            struct.pack_into('<I', data, 0x98 + 96 + 8, rva)
            with self.subTest(rva=rva), self.assertRaises(census.PEError):
                census.parse_pe(data)

    def test_import_from_virtual_zero_fill_is_not_readable(self):
        data = bytearray(pe_fixture())
        struct.pack_into('<I', data, 0x98 + 224 + 16, 0x200)
        struct.pack_into('<I', data, 0x98 + 96 + 8, 0x2000)
        with self.assertRaisesRegex(census.PEError, 'backing'):
            census.parse_pe(data)

    def test_import_names_must_terminate_in_section(self):
        data = bytearray(pe_fixture())
        struct.pack_into('<I', data, 0x200 + 12, 0x4ffc)
        data[-4:] = b'abcd'
        with self.assertRaisesRegex(census.PEError, 'unterminated import name'):
            census.parse_pe(data)

    def test_descriptor_and_thunk_must_terminate(self):
        data = bytearray(pe_fixture())
        struct.pack_into('<I', data, 0x98 + 96 + 12, 20)
        with self.assertRaisesRegex(census.PEError, 'descriptor'):
            census.parse_pe(data)
        data = bytearray(pe_fixture())
        struct.pack_into('<II', data, 0x200, 0x4ffc, 0)
        struct.pack_into('<I', data, len(data) - 4, 0x80000001)
        with self.assertRaisesRegex(census.PEError, 'RVA'):
            census.parse_pe(data)

    def test_overlapping_virtual_sections(self):
        data = bytearray(pe_fixture())
        struct.pack_into('<H', data, 0x86, 2)
        section = 0x98 + 224
        data[section + 40:section + 80] = data[section:section + 40]
        with self.assertRaisesRegex(census.PEError, 'overlapping'):
            census.parse_pe(data)


class INFParsingTests(unittest.TestCase):
    def test_model_service_chain_and_strings(self):
        result = census.parse_inf(inf_fixture())
        self.assertEqual(len(result['references']), 1)
        ref = result['references'][0]
        self.assertEqual(ref['binary'], 'nic.sys')
        self.assertEqual(ref['os'], 'NT5.1+')
        self.assertEqual(ref['architecture'], 'x86')
        self.assertEqual(ref['hardware_ids'], [r'PCI\VEN_10EC&DEV_8139'])
        self.assertEqual(ref['description'], 'NIC')

    def test_mixed_os_inf_does_not_cross_assign_binaries(self):
        text = '''[Version]
Class=Net
[Manufacturer]
Vendor=Models,NTx86.5.1,NTamd64.6.0,Win9x
[Models.NTx86.5.1]
XP=XPInstall,PCI\\VEN_8086&DEV_100E
[Models.NTamd64.6.0]
Vista=VistaInstall,PCI\\VEN_8086&DEV_100E
[Models.Win9x]
Old=OldInstall,PCI\\VEN_8086&DEV_100E
[XPInstall]
CopyFiles=XPFiles
[XPFiles]
nic.sys,,,2
[VistaInstall]
CopyFiles=@nic64.sys
[OldInstall]
CopyFiles=@nic.vxd
[SourceDisksFiles]
helper.sys=1
'''
        refs = {r['binary']: r for r in census.parse_inf(text)['references']}
        self.assertEqual(refs['nic.sys']['os'], 'NT5.1+')
        self.assertEqual(refs['nic64.sys']['os'], 'NT6.0+')
        self.assertEqual(refs['nic.vxd']['os'], 'Win9x')
        self.assertEqual(refs['helper.sys']['os'], 'unknown')
        self.assertEqual(refs['helper.sys']['hardware_ids'], [])

    def test_legacy_install_decoration_and_chicago_not_nt_proof(self):
        text = '''[Version]
Class=Net
Signature="$Chicago$"
[Manufacturer]
Vendor=Models
[Models]
NIC=Install,PCI\\VEN_10EC&DEV_8139
[Install]
CopyFiles=@old.sys
[Install.NT]
CopyFiles=@new.sys
'''
        refs = {r['binary']: r for r in census.parse_inf(text)['references']}
        self.assertEqual(refs['old.sys']['os'], 'Win9x-or-NT')
        self.assertEqual(refs['new.sys']['os'], 'NT-unspecified')

    def test_quotes_continuation_comments_locales_and_case(self):
        text = inf_fixture(binary='NIC.SYS').replace('Class=Net', 'Class=%class%')
        text = text.replace('desc="NIC"', 'desc="NIC; with, punctuation"\nclass="NET"')
        text = text.replace('Models,NTx86.5.1', 'Models,\\\nNTx86.5.1 ; comment')
        text += '[Strings.0409]\nclass="Media"\n'
        result = census.parse_inf(text)
        self.assertEqual(result['class'], 'Net')
        self.assertEqual(result['references'][0]['description'], 'NIC; with, punctuation')
        self.assertEqual(census.fields('"a,b","x""y",,2'), ['a,b', 'x"y', '', '2'])

    def test_encodings(self):
        text = inf_fixture().replace('"NIC"', '"Café NIC"')
        for encoding in ('utf-16', 'utf-16-le', 'utf-16-be', 'utf-8-sig', 'cp1252'):
            with self.subTest(encoding=encoding):
                self.assertEqual(census.decode_inf(text.encode(encoding)), text)

    def test_include_needs_cycle_and_missing_section_are_disclosed(self):
        text = inf_fixture().replace('CopyFiles=@nic.sys', 'Include=external.inf\nNeeds=Missing,Other\nCopyFiles=@nic.sys')
        text += '[Other]\nNeeds=Install\n'
        warnings = census.parse_inf(text)['warnings']
        self.assertIn('external Include not resolved: external.inf', warnings)
        self.assertIn('missing section: missing', warnings)

    def test_architecture_and_os_decorations(self):
        self.assertEqual(census.target('NT.5.1')['os'], 'NT5.1+')
        self.assertEqual(census.target('NTx86')['os'], 'NT-unspecified')
        self.assertEqual(census.target('NTamd64.5.2')['architecture'], 'amd64')

    def test_x86_models_can_use_generic_nt_install(self):
        text = inf_fixture(decoration='NTx86').replace('[Install]', '[Install.NT]')
        text = text.replace('[Install.Services]', '[Install.NT.Services]')
        result = census.parse_inf(text)
        self.assertEqual(result['references'][0]['install_section'], 'install.nt')
        self.assertEqual(result['references'][0]['os'], 'NT-unspecified')
        self.assertEqual(result['references'][0]['architecture'], 'x86')
        self.assertEqual(result['references'][0]['manufacturer_decoration'], 'NTx86')

    def test_explicit_xp_models_prefer_compatible_nt_install_over_9x(self):
        text = inf_fixture().replace('Signature="$Windows NT$"', 'Signature="$Chicago$"')
        text = text.replace('CopyFiles=@nic.sys', 'CopyFiles=@old.sys')
        text = text.replace('[Install.Services]', '[Install.NT.Services]')
        text += '[Install.NT]\nCopyFiles=@nic.sys\n[Install.NT.5.0]\nCopyFiles=@win2k.sys\n[Install.NT.6.0]\nCopyFiles=@vista.sys\n'
        result = census.parse_inf(text)
        linked = [r for r in result['references'] if r['evidence'] == 'model-install']
        self.assertEqual({r['binary'] for r in linked}, {'win2k.sys'})
        self.assertEqual(linked[0]['install_section'], 'install.nt.5.0')
        self.assertEqual(linked[0]['os'], 'NT5.1+')
        self.assertEqual(next(r for r in result['references'] if r['binary'] == 'old.sys')['os'], 'unknown')


class CensusTests(unittest.TestCase):
    def test_complete_import_coverage_is_not_one_symbol_coverage(self):
        rows = [{'imports': {'NDIS.SYS': ['Common', 'A']}},
                {'imports': {'NDIS.SYS': ['Common', 'B']}}]
        result = census.coverage(rows)
        self.assertEqual(result['ranked'][0]['symbol'], 'Common')
        self.assertEqual(result['frequency_prefix_thresholds']['50']['symbols'], 2)
        self.assertEqual(result['frequency_prefix_thresholds']['80']['symbols'], 3)
        self.assertEqual(result['greedy_complete_sets'][0]['drivers'], 1)
        self.assertEqual(result['greedy_complete_sets'][-1]['symbols'], ['NDIS.SYS!A', 'NDIS.SYS!B', 'NDIS.SYS!Common'])

    def test_empty_cohort_and_empty_dll_surface(self):
        self.assertEqual(census.coverage([])['symbol_count'], 0)
        result = census.coverage([{'imports': {'NDIS.SYS': ['A']}}], {'HAL.DLL'})
        self.assertEqual(result['frequency_prefix_thresholds']['95']['symbols'], 0)

    def test_inventory_read_only_deterministic_and_content_deduplicated(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'pack'; root.mkdir()
            (root / 'nic.sys').write_bytes(pe_fixture())
            (root / 'nic.inf').write_text(inf_fixture())
            duplicate = root / 'duplicate'; duplicate.mkdir()
            (duplicate / 'nic.sys').write_bytes(pe_fixture())
            (duplicate / 'nic.inf').write_text(inf_fixture())
            (root / 'bad.sys').write_bytes(b'bad PE')
            (root / 'legacy.vxd').write_bytes(b'not PE')
            before = {p: p.read_bytes() for p in root.rglob('*') if p.is_file()}
            first = census.census(root)
            self.assertEqual(first, census.census(root))
            self.assertEqual(first['totals']['sys'], 3)
            self.assertEqual(first['totals']['vxd'], 1)
            self.assertEqual(first['totals']['binary_errors'], 2)
            self.assertEqual(first['totals']['net_nt5_candidate_paths'], 2)
            self.assertEqual(first['totals']['net_nt5_candidate_unique'], 1)
            self.assertEqual(first['sys_by_class_os'], [
                {'class': 'Net', 'os': 'NT5.1+', 'drivers': 2},
                {'class': 'Unknown', 'os': 'unknown', 'drivers': 1}])
            self.assertEqual(first['baseline'][1]['drivers'], ['duplicate/nic.sys', 'nic.sys'])
            self.assertEqual(before, {p: p.read_bytes() for p in root.rglob('*') if p.is_file()})
            output = Path(temp) / 'output.json'
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(census.main([str(root), '--output', str(output)]), 0)
            self.assertEqual(json.loads(output.read_text()), first)

    def test_same_basename_packages_missing_ambiguous_and_symlinks(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in ('a', 'b', 'missing', 'ambiguous/one', 'ambiguous/two'):
                (root / name).mkdir(parents=True)
            for name in ('a', 'b', 'ambiguous/one', 'ambiguous/two'):
                (root / name / 'nic.sys').write_bytes(pe_fixture())
            for name in ('a', 'missing', 'ambiguous'):
                (root / name / 'nic.inf').write_text(inf_fixture())
            (root / 'link.sys').symlink_to(root / 'a/nic.sys')
            (root / 'linked_dir').symlink_to(root / 'a', target_is_directory=True)
            result = census.census(root)
            self.assertEqual(result['totals']['sys'], 4)
            self.assertEqual(result['totals']['net_nt5_candidate_paths'], 1)
            self.assertEqual({r['reason'] for r in result['unresolved_references']},
                             {'ambiguous', 'missing-local-binary'})
            self.assertEqual(next(d for d in result['drivers'] if d['path'] == 'b/nic.sys')['classes'], ['Unknown'])

    def test_x64_nt4_vista_and_ndis6_not_miniport_candidates(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for label, deco, image in (
                ('x64', 'NTamd64.5.2', pe_fixture(wide=True)),
                ('nt4', 'NTx86.4.0', pe_fixture()),
                ('vista', 'NTx86.6.0', pe_fixture()),
                ('ndis6', 'NTx86.5.1', pe_fixture({'NDIS.SYS': ['NdisMRegisterMiniportDriver']}))):
                folder = root / label; folder.mkdir()
                (folder / 'nic.inf').write_text(inf_fixture(decoration=deco))
                (folder / 'nic.sys').write_bytes(image)
            result = census.census(root)
            self.assertEqual(result['totals']['net_nt5_candidate_paths'], 1)
            self.assertEqual(result['totals']['miniport_candidate_paths'], 0)

    def test_output_cannot_write_inside_input_or_symlink_to_it(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'pack'; root.mkdir()
            alias = Path(temp) / 'alias'; alias.symlink_to(root, target_is_directory=True)
            for target in (root / 'out.json', alias / 'out.json'):
                with self.subTest(target=target), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    census.main([str(root), '--output', str(target)])
            self.assertEqual(list(root.iterdir()), [])

    def test_limits_are_errors_not_unbounded_reads(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'large'; path.write_bytes(b'12345')
            with self.assertRaisesRegex(ValueError, 'exceeds'):
                census.read_limited(path, 4)

    def test_output_hardlink_does_not_modify_input(self):
        import os
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'pack'; root.mkdir()
            inf = root / 'nic.inf'; inf.write_text(inf_fixture())
            output = Path(temp) / 'output.json'
            os.link(inf, output)
            original = inf.read_bytes()
            with contextlib.redirect_stdout(io.StringIO()):
                census.main([str(root), '--output', str(output)])
            self.assertEqual(inf.read_bytes(), original)
            self.assertEqual(json.loads(output.read_text())['totals']['inf'], 1)

    def test_compressed_only_reference_is_disclosed_not_counted_as_sys(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'nic.inf').write_text(inf_fixture())
            (root / 'nic.sy_').write_bytes(b'compressed payload, deliberately not PE')
            result = census.census(root)
            self.assertEqual(result['totals']['sys'], 0)
            self.assertEqual(result['totals']['compressed_files_not_parsed'], 1)
            self.assertEqual(result['unresolved_references'][0]['compressed_candidates'], ['nic.sy_'])

    def test_model_rows_share_install_without_repeated_references(self):
        text = inf_fixture().replace('%desc%=Install, PCI\\VEN_10EC&DEV_8139',
                 '%desc%=Install, PCI\\VEN_10EC&DEV_8139\nOther=Install,PCI\\VEN_10EC&DEV_8169')
        result = census.parse_inf(text)
        self.assertEqual(len(result['models']), 2)
        self.assertEqual(len(result['references']), 1)
        self.assertEqual(result['references'][0]['hardware_ids'],
                         [r'PCI\VEN_10EC&DEV_8139', r'PCI\VEN_10EC&DEV_8169'])


if __name__ == '__main__':
    unittest.main()
