"""F5-14B continuation: reused Wine upper DLL record, decisions, T1 and H0 binding.

SPDX-License-Identifier: LGPL-2.1-or-later
Offline: the record checks need no build; build/audit/loader checks run only
when the local outputs exist and match the record. No PE code is executed.
"""
import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from scripts.win32 import upper_dlls, wine_upper as wu
from tests.host.win32.adapter import build as adapter

ROOT = Path(__file__).resolve().parents[4]
BUILD = ROOT / 'build/host/win32/wine-upper'
CORE = ROOT / 'build/apps/win32'
RECORD = json.loads(wu.RECORD.read_text())


def local_build_matches():
    pe = BUILD / 'pe'
    return all((pe / n).is_file() and wu.sha(pe / n) == r['sha256'] for n, r in RECORD['dlls'].items())


class RecordTests(unittest.TestCase):
    def test_record_is_self_consistent_and_rendered(self):
        self.assertTrue(wu.validate(RECORD))
        self.assertIn(wu.render(RECORD), wu.DOCUMENT.read_text())
        self.assertEqual(RECORD['identical_runs'], 2)
        self.assertEqual(RECORD['modules'], list(wu.MODULES))
        self.assertEqual(set(RECORD['not_reused']), set(wu.NOT_REUSED))

    def test_every_missing_export_has_one_closed_decision(self):
        owners = {d['owner'] for d in RECORD['decisions']}
        self.assertNotIn('kernelbase.dll', owners)
        self.assertNotIn('win32u.dll', owners)
        for entry in RECORD['decisions']:
            self.assertIn(entry['rule'], RECORD['rules'])
            if entry['name'].startswith(('wine_', '__wine_')):
                self.assertEqual(entry['action'], 'wine-patch-remove')
            if entry['action'].startswith('core-'):
                self.assertNotIn(entry['owner'], {m + '.dll' for m in wu.MODULES} - set(wu.NOT_REUSED))
            if entry['importer'] in wu.NOT_REUSED:
                self.assertEqual(entry['action'], 'not-reused')
        eager_absent = [d for d in RECORD['decisions'] if d['kind'] == 'import'
                        and d['rule'] in ('absent-delayed-dll', 'absent-forwarder-dll')]
        self.assertEqual(eager_absent, [])

    def test_negative_mutations_are_rejected(self):
        changed = copy.deepcopy(RECORD)
        changed['decisions'].pop()
        with self.assertRaisesRegex(ValueError, 'decision'):
            wu.validate(changed)
        changed = copy.deepcopy(RECORD)
        changed['decisions'][0]['rule'] = 'invented'
        with self.assertRaisesRegex(ValueError, 'unknown decision'):
            wu.validate(changed)
        changed = copy.deepcopy(RECORD)
        changed['decisions'].append(dict(changed['decisions'][0]))
        with self.assertRaisesRegex(ValueError, 'duplicate decision'):
            wu.validate(changed)
        changed = copy.deepcopy(RECORD)
        first = next(iter(changed['recipe']['patches']))
        changed['recipe']['patches'][first] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'patch hash'):
            wu.validate(changed)
        changed = copy.deepcopy(RECORD)
        changed['dlls']['shlwapi.dll']['base'] = changed['dlls']['shell32.dll']['base']
        with self.assertRaisesRegex(ValueError, 'overlapping'):
            wu.validate(changed)
        changed = copy.deepcopy(RECORD)
        changed['dlls']['cabinet.dll']['qualified'] = True
        with self.assertRaisesRegex(ValueError, 'qualified'):
            wu.validate(changed)

    def test_bases_lie_above_the_authored_builtins(self):
        low, high = RECORD['recipe']['base_region']
        self.assertEqual((low, high), (wu.UPPER_BASE, wu.UPPER_LIMIT))
        store = CORE / 'builtin-store.json'
        if not store.is_file():
            self.skipTest('core builtin store not built')
        end = max(d['base'] + d['size'] for d in json.loads(store.read_text())['dlls'])
        self.assertLess(end, low)

    def test_msvcrt_decision_and_fit_figures(self):
        m = RECORD['msvcrt']
        self.assertEqual(m['wine']['surface_exported'], m['surface_count'])
        self.assertEqual(m['newlib']['covered'] + len(m['newlib']['missing']), m['surface_count'])
        self.assertGreater(m['newlib']['linked_bytes_over_empty']['text'], 0)
        fit = RECORD['fit']
        reused = {n: r for n, r in RECORD['dlls'].items() if n not in RECORD['not_reused']}
        self.assertEqual(fit['size_of_image'], sum(r['size_of_image'] for r in reused.values()))
        self.assertEqual(fit['shared_seed_pages'], sum(r['memory']['shared_seed_pages'] for r in reused.values()))
        self.assertEqual(fit['size_of_image_within_budget'], fit['size_of_image'] <= fit['installer_budget_bytes'])

    def test_t1_refuses_reused_wine_records(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            name, entry = 'cabinet.dll', RECORD['dlls']['cabinet.dll']
            (folder / name).write_bytes(b'not the recorded PE')
            store = {'dlls': [{'name': name, 'path': name, 'sha256': entry['sha256'], 'base': entry['base'],
                               'license': entry['license'], 'wine_reused': True}]}
            with self.assertRaisesRegex(ValueError, 'mismatched reused Wine PE'):
                upper_dlls.reject_unqualified_wine(folder, store)
            if (BUILD / 'pe' / name).is_file() and wu.sha(BUILD / 'pe' / name) == entry['sha256']:
                (folder / name).write_bytes((BUILD / 'pe' / name).read_bytes())
                with self.assertRaisesRegex(ValueError, 'unqualified reused Wine PE'):
                    upper_dlls.reject_unqualified_wine(folder, store)
            (folder / name).unlink()
            store['dlls'][0]['name'] = store['dlls'][0]['path'] = 'invented.dll'
            (folder / 'invented.dll').write_bytes(b'x')
            with self.assertRaisesRegex(ValueError, 'unlisted reused Wine PE'):
                upper_dlls.reject_unqualified_wine(folder, store)


class LocalBuildTests(unittest.TestCase):
    def setUp(self):
        if not (BUILD / 'build.json').is_file():
            self.skipTest('no local upper build (scripts/win32/build-wine-dlls.sh --upper-build)')

    def test_local_build_reproduces_record(self):
        build = json.loads((BUILD / 'build.json').read_text())
        for name, entry in RECORD['dlls'].items():
            for key in ('sha256', 'file_bytes', 'base', 'size_of_image', 'memory'):
                self.assertEqual(build['dlls'][name][key], entry[key], name + ' ' + key)
        self.assertEqual(build['patches'], RECORD['recipe']['patches'])

    def test_local_audit_reproduces_decisions(self):
        if not local_build_matches() or not (CORE / 'builtin-store.json').is_file():
            self.skipTest('local PEs or core outputs absent')
        core = {p.name.lower(): wu.sha(p) for p in sorted(CORE.iterdir())
                if p.suffix.lower() in ('.dll', '.drv', '.acm') and p.is_file()}
        if core != RECORD['core_pe_sha256']:
            self.skipTest('CiukiOS core PEs changed since the record; rerun audit/record')
        result = wu.audit(BUILD, CORE)
        self.assertEqual(result['decisions'], RECORD['decisions'])
        self.assertEqual(result['imports'], RECORD['imports'])

    def test_production_loader_h0_binding_of_each_reused_dll(self):
        store_path = CORE / 'builtin-store.json'
        if not local_build_matches() or not store_path.is_file():
            self.skipTest('local PEs or core outputs absent')
        reused = [m + '.dll' for m in wu.MODULES if m + '.dll' not in wu.NOT_REUSED]
        core = [d for d in json.loads(store_path.read_text())['dlls'] if d['name'].lower() not in reused]
        rows = [(d['name'], str(CORE / d['path']), d['sha256'], d['base'], d['size'], d['version']) for d in core]
        rows += [(n, str(BUILD / 'pe' / n), RECORD['dlls'][n]['sha256'], RECORD['dlls'][n]['base'],
                  RECORD['dlls'][n]['size_of_image'], [0, 0, 0, 0]) for n in reused]
        with tempfile.TemporaryDirectory(dir=ROOT / 'build/host/win32') as directory:
            directory = Path(directory)
            (directory / 'upper_store.h').write_text(
                'static struct w32_builtin upper_store[] = {\n' + ''.join(
                    '{' + ','.join([json.dumps(n), json.dumps(p), json.dumps(s), hex(b), str(z),
                                    '{' + ','.join(map(str, v)) + '}']) + '},\n'
                    for n, p, s, b, z, v in rows) + '};\nstatic const char *const upper_names[] = {' +
                ', '.join(json.dumps(n) for n in reused) + '};\n')
            main = directory / 'main.c'
            main.write_text(Path(__file__).with_name('upper_adapter.c').read_text())
            program = adapter.build_model(directory / 'model', main)
            result = subprocess.run([program], capture_output=True, text=True, timeout=120)
            (BUILD / 'h0-load.txt').write_text(result.stdout)
        self.assertEqual(result.returncode, 0, result.stdout[-2000:] + result.stderr[-2000:])
        self.assertIn('PASS upper H0 binding observation; resources released', result.stdout)
        lines = [line.split() for line in result.stdout.splitlines() if line.startswith('RESULT ')]
        self.assertEqual([line[1] for line in lines], reused)
        missing = {(d['provider'], d['name']) for d in RECORD['decisions']}
        absent = {d['provider'] for d in RECORD['decisions']}
        for line in lines:
            name = line[1].lower()
            if line[2] == 'OK':
                self.assertEqual(RECORD['imports'][name]['eager_missing'], 0, line)
                continue
            self.assertEqual(line[2], 'FAIL', line)
            code, module, symbol = int(line[3]), Path(line[5].replace('\\', '/')).name.lower(), line[6]
            if line[4] == 'resource-count-limit':
                # The PE parser's resource walk budget (sdk/win32/ldr/pe.c).
                self.assertEqual(code, 193, line)
                self.assertGreater(RECORD['dlls'][module]['resource_entries'], 4096, line)
            elif code == 127:
                self.assertIn((module, symbol), missing, line)
            else:
                self.assertEqual(code, 126, line)
                self.assertIn(module, absent, line)


if __name__ == '__main__':
    unittest.main()
