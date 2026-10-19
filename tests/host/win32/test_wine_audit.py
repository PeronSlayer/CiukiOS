"""Offline pinned Wine audit consistency and independently recorded PE facts.

SPDX-License-Identifier: GPL-2.0-only
No builds, Wine execution, guest execution or network access in this suite.
"""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('wine_audit', ROOT / 'scripts/win32/wine_audit.py')
audit_tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit_tool)
PIN = json.loads(audit_tool.PIN.read_text())
AUDIT = json.loads(audit_tool.AUDIT.read_text())


class SpecParserTests(unittest.TestCase):
    def test_i386_conventions_arch_flags_and_explicit_ordinals(self):
        entries = audit_tool.parse_spec('''
7 stdcall -noname OrdinalOnly(long)
@ cdecl -arch=win64 WrongArchitecture()
@ thiscall -arch=i386 Method(ptr)
@ stdcall -arch=win32 RightArchitecture()
@ extern -private __wine_private
@ stub Unimplemented
''')
        self.assertEqual(set(entries), {'OrdinalOnly', 'Method', 'RightArchitecture', '__wine_private', 'Unimplemented'})
        self.assertEqual(entries['OrdinalOnly']['ordinal'], 7)
        self.assertIn('-noname', entries['OrdinalOnly']['flags'])
        self.assertIsNone(entries['Method']['ordinal'])
        self.assertEqual(entries['Method']['kind'], 'thiscall')
        self.assertEqual(entries['Unimplemented']['kind'], 'stub')
        self.assertIn('-private', entries['__wine_private']['flags'])

    def test_forward_import_and_local_alias_are_distinct(self):
        entries = audit_tool.parse_spec('''
@ stdcall Forwarded(ptr) kernelbase.Target
@ stdcall -import Imported(ptr)
@ cdecl LocalAlias() local_implementation
@ stdcall OrdinalForward() other.#42 # retained ordinal target
''')
        self.assertEqual(entries['Forwarded']['forward'], 'kernelbase.Target')
        self.assertTrue(entries['Imported']['import'])
        self.assertIsNone(entries['LocalAlias']['forward'])
        self.assertEqual(entries['OrdinalForward']['forward'], 'other.#42')

    def test_duplicate_arch_neutral_name_fails(self):
        with self.assertRaisesRegex(ValueError, 'duplicate i386 export'):
            audit_tool.parse_spec('@ cdecl Name()\n@ stdcall Name()\n')

    def test_invalid_spec_fails(self):
        with self.assertRaisesRegex(ValueError, 'unsupported spec syntax'):
            audit_tool.parse_spec('invented syntax')

    def test_observer_excludes_comments_and_strings_from_direct_names(self):
        specs = {'kernel32': {'CreateFileW': {'flags': []}}, 'user32': {'GetWindowLongW': {'flags': []}}}
        text = '/* CreateFileW */\n"CreateFileW"; // CreateFileW\nGetWindowLongPtrW(hwnd, 0);\n'
        references, literals, _, _, _ = audit_tool.observe(text, 'example.c', specs)
        self.assertEqual(set(references), {'user32!GetWindowLongW'})
        self.assertEqual(references['user32!GetWindowLongW']['line'], 3)
        self.assertEqual(literals[0]['name'], 'CreateFileW')

    def test_url_literal_does_not_swallow_following_lookup(self):
        specs = {'kernel32': {'CreateFileW': {'flags': []}}}
        _, literals, _, _, _ = audit_tool.observe('"http://example.test"; "CreateFileW";', 'x.c', specs)
        self.assertEqual([item['name'] for item in literals], ['CreateFileW'])

    def test_default_ucrt_and_explicit_crt_and_nodefaultlibs(self):
        parse = audit_tool.parse_makefile
        self.assertEqual(parse('MODULE = version.dll\nIMPORTS = kernelbase\n')['defaults'],
                         ['winecrt0', 'ucrtbase', 'kernel32', 'ntdll'])
        self.assertEqual(parse('MODULE = advapi32.dll\nIMPORTS = msvcrt\n')['defaults'],
                         ['winecrt0', 'msvcrt', 'kernel32', 'ntdll'])
        self.assertEqual(parse('MODULE = msvcrt.dll\n')['defaults'], ['winecrt0', 'kernel32', 'ntdll'])
        self.assertEqual(parse('MODULE = kernelbase.dll\nEXTRADLLFLAGS = -nodefaultlibs\n')['defaults'], [])
        self.assertEqual(parse('STATICLIB = libwinecrt0.a\n')['defaults'], [])

    def test_delays_variables_and_cycles_are_retained(self):
        parsed = audit_tool.parse_makefile('MODULE = x.dll\nIMPORTS = $(ZLIB_PE_LIBS) uuid\nDELAYIMPORTS = y \\\n z\n')
        self.assertEqual(parsed['variables'], ['ZLIB_PE_LIBS'])
        self.assertEqual(parsed['delay'], ['y', 'z'])
        graph = {'a': {'direct': ['b', 'uuid'], 'delay': ['missing'], 'defaults': []},
                 'b': {'direct': ['a'], 'delay': [], 'defaults': []}}
        self.assertEqual(audit_tool.closure(graph, 'a'), (['a', 'b'], ['missing']))


class PinnedAuditTests(unittest.TestCase):
    def test_all_named_core_and_modern_exports_exist_in_hash_pinned_specs(self):
        specs = audit_tool.validate(AUDIT, PIN)
        self.assertEqual(set(AUDIT['roots']), set(audit_tool.ROOTS))
        self.assertEqual(AUDIT['core_union_counts'], {'ntdll': 242, 'kernel32': 374, 'user32': 301, 'gdi32': 135})
        self.assertEqual(specs['ole32']['CoCreateInstance']['forward'], 'combase.CoCreateInstance')
        self.assertEqual(AUDIT['graph']['version']['direct'], ['kernelbase'])
        self.assertEqual(AUDIT['graph']['win32u']['unixlib'], 'win32u.so')

    def test_generated_markdown_is_consistent(self):
        document = (ROOT / 'docs/design/win32-wine-dependency-audit.md').read_text()
        self.assertIn(audit_tool.render_tables(AUDIT), document)

    def test_source_candidate_status_is_not_an_abi_gate(self):
        self.assertIn('core-freeze-blocked', AUDIT['status'])
        self.assertIsNone(AUDIT['title_census']['core_exports'])
        self.assertEqual(PIN['status'], 'archive-verified')
        self.assertGreater(len(AUDIT['graph']), 600)
        self.assertEqual(sum(row['pe_build'] == 'built-experiment' for row in AUDIT['roots'].values()), 1)
        self.assertTrue(all(not row['unresolved_modules'] for row in AUDIT['roots'].values()))

    def test_notices_and_protocol_snapshot_have_pinned_hashes(self):
        for relative in ('LICENSE', 'COPYING.LIB', 'AUTHORS', 'libs/musl/COPYRIGHT', 'libs/zlib/zlib.h', 'include/wine/server_protocol.h'):
            data = (audit_tool.CATALOG / relative).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), PIN['source_files'][relative]['sha256'])
        protocol = (audit_tool.CATALOG / 'include/wine/server_protocol.h').read_text()
        for requests in AUDIT['server_backend_survey']['files'].values():
            for request in requests:
                self.assertRegex(protocol, r'\bREQ_' + re.escape(request) + r'\b')

    def test_unbuilt_source_direct_server_observation_is_explicit(self):
        self.assertTrue(all(not row['wineserver_requests_direct'] for row in AUDIT['roots'].values()))
        self.assertTrue(all(not row['unix_calls_direct'] for row in AUDIT['roots'].values()))
        self.assertIn('NOT per-root reachable', AUDIT['server_backend_survey']['evidence'])

    def test_unknown_export_negative_mutation_is_rejected(self):
        changed = copy.deepcopy(AUDIT)
        row = changed['roots']['version']
        row['core_exports']['ntdll'] = sorted(row['core_exports']['ntdll'] + ['CiukiInventedExport'])
        row['core_counts']['ntdll'] += 1
        with self.assertRaisesRegex(ValueError, 'export absent from pinned spec'):
            audit_tool.validate(changed, PIN)

    def test_count_and_union_negative_mutations_are_rejected(self):
        changed = copy.deepcopy(AUDIT)
        changed['roots']['version']['core_counts']['ntdll'] += 1
        with self.assertRaisesRegex(ValueError, 'bad counts'):
            audit_tool.validate(changed, PIN)
        changed = copy.deepcopy(AUDIT)
        changed['core_union_counts']['ntdll'] += 1
        with self.assertRaisesRegex(ValueError, 'union count drift'):
            audit_tool.validate(changed, PIN)

    def test_pin_mismatch_and_dependency_graph_drift_are_rejected(self):
        changed = copy.deepcopy(AUDIT)
        changed['commit'] = '0' * 40
        with self.assertRaisesRegex(ValueError, 'release mismatch'):
            audit_tool.validate(changed, PIN)
        changed = copy.deepcopy(AUDIT)
        changed['graph']['version']['direct'] = []
        with self.assertRaisesRegex(ValueError, 'Makefile graph drift'):
            audit_tool.validate(changed, PIN)

    def test_corrupted_spec_hash_is_rejected(self):
        changed = copy.deepcopy(PIN)
        changed['source_files']['dlls/ntdll/ntdll.spec']['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'spec snapshot hash mismatch'):
            audit_tool.validate(AUDIT, changed)

    def test_actual_version_imports_and_forwards_match_independent_readobj_record(self):
        pe = AUDIT['build_experiment']['pe']
        expected = {
            'KERNEL32.DLL': ['IsBadStringPtrA', 'LZClose', 'LZCopy', 'LZOpenFileA', 'MoveFileA', 'OpenFile', '_lclose'],
            'KERNELBASE.DLL': ['DeleteFileA', 'DisableThreadLibraryCalls', 'GetFileAttributesA',
                'GetFileVersionInfoA', 'GetFileVersionInfoExA', 'GetFileVersionInfoExW', 'GetFileVersionInfoSizeA',
                'GetFileVersionInfoSizeExA', 'GetFileVersionInfoSizeExW', 'GetFileVersionInfoSizeW',
                'GetFileVersionInfoW', 'GetTempFileNameA', 'HeapAlloc', 'HeapFree', 'MultiByteToWideChar',
                'VerFindFileA', 'VerFindFileW', 'VerQueryValueA', 'VerQueryValueW', 'WideCharToMultiByte', 'lstrcmpiA'],
            'NTDLL.DLL': ['__wine_dbg_header', '__wine_dbg_output', '__wine_dbg_strdup'],
            'UCRTBASE.DLL': ['__stdio_common_vsprintf', 'strcpy', 'strlen', 'strrchr'],
        }
        self.assertEqual(pe['imports'], {dll: sorted(names) for dll, names in expected.items()})
        self.assertEqual(pe['delay_imports'], {})
        self.assertEqual(pe['forwards'], {'VerLanguageNameA': 'kernel32.VerLanguageNameA', 'VerLanguageNameW': 'kernel32.VerLanguageNameW'})
        self.assertEqual(len(pe['exports']), 16)
        self.assertEqual(pe['image_size'], 32768)
        self.assertGreater(pe['resource_bytes'], 0)
        self.assertEqual(pe['sha256'], AUDIT['build_experiment']['artifacts']['version.dll'])

    def test_memory_proxy_totals_are_labelled_and_consistent(self):
        proxy = AUDIT['memory_proxy']
        self.assertEqual(proxy['release'], '11.18')
        self.assertIn('NOT Wine 10.0', proxy['evidence'])
        self.assertEqual(proxy['sum_image_bytes'], sum(row.get('image_size', 0) for row in proxy['modules'].values()))
        self.assertEqual(proxy['sum_file_bytes'], sum(row.get('file_bytes', 0) for row in proxy['modules'].values()))
        self.assertEqual(sum(AUDIT['p4_p5_only_source_union_counts'].values()), 784)

    def test_actual_pe_missing_export_and_invented_import_are_rejected(self):
        changed = copy.deepcopy(AUDIT)
        del changed['build_experiment']['pe']['exports']['VerInstallFileA']
        with self.assertRaisesRegex(ValueError, 'PE exports/forwards differ'):
            audit_tool.validate(changed, PIN)
        changed = copy.deepcopy(AUDIT)
        changed['build_experiment']['pe']['imports']['KERNEL32.DLL'].append('CiukiInventedImport')
        with self.assertRaisesRegex(ValueError, 'PE import absent from pinned spec'):
            audit_tool.validate(changed, PIN)

    def test_available_source_reproduces_census_without_building(self):
        source = ROOT / 'build/external/wine-10.0'
        if not source.is_dir():
            self.skipTest('full pinned source cache is optional; offline spec checks still run')
        audit_tool.verify_source(source, PIN)
        collected = audit_tool.collect(source, PIN)
        for key in ('core_union', 'modern_reference_candidates', 'server_backend_survey', 'helper_survey', 'graph', 'spec_paths'):
            self.assertEqual(collected[key], AUDIT[key])
        for dll, row in collected['roots'].items():
            old = dict(AUDIT['roots'][dll]); old['pe_build'] = 'not-built'
            self.assertEqual(row, old)

    def test_available_pe_matches_saved_evidence_and_llvm_import_reader(self):
        binary = ROOT / 'build/host/win32/wine-10.0/version.dll'
        if not binary.is_file():
            self.skipTest('PE output is optional; offline build evidence checks still run')
        evidence = audit_tool.pe_evidence(binary)
        self.assertEqual(evidence, AUDIT['build_experiment']['pe'])
        text = subprocess.check_output(['llvm-readobj', '--coff-imports', str(binary)], text=True)
        found = {}
        for block in re.findall(r'Import \{(.*?)\n\}', text, re.S):
            dll = re.search(r'Name: (\S+)', block)[1].upper()
            found[dll] = sorted(re.findall(r'Symbol: (\S+) \(', block))
        self.assertEqual(found, evidence['imports'])

    def test_pe_reader_rejects_non_pe_without_guest_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'invalid.dll'; path.write_bytes(b'ELF, not a DLL')
            with self.assertRaises(ValueError):
                audit_tool.pe_evidence(path)


if __name__ == '__main__':
    unittest.main()
