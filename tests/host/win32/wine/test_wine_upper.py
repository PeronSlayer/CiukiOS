"""F5-14B continuation: reused Wine upper DLL record, decisions, T1 and H0 binding.

SPDX-License-Identifier: LGPL-2.1-or-later
Offline: the record checks need no build; build/audit/loader checks run only
when the local outputs exist and match the record. No PE code is executed.
"""
import copy
import json
from pathlib import Path
import re
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
        audits = {d['audit'] for d in RECORD['decisions']}
        self.assertEqual(audits, {'f5-14b', 'f5-20b', 'f5-20c'})
        for entry in RECORD['decisions']:
            if entry['audit'] == 'f5-14b':
                self.assertTrue(entry['f5_20c'].startswith('closed: '), entry)
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
        # f5-20C: the re-audit leaves no missing eager import of a reused DLL.
        for dll, row in RECORD['imports'].items():
            if dll not in wu.NOT_REUSED:
                self.assertEqual(row['eager_missing'], 0, dll)
        self.assertEqual({(o, d['name']) for d in RECORD['decisions'] if d['audit'] == 'f5-20b'
                          for o in [d['owner']]},
                         {(o, n) for o, names in wu.F5_20B_OUTSIDE.items() for n in names})
        self.assertEqual(len(wu.F5_20B_OUTSIDE['user32.dll']), 87)
        self.assertEqual(len(wu.F5_20B_OUTSIDE['gdi32.dll']), 4)

    def test_negative_mutations_are_rejected(self):
        changed = copy.deepcopy(RECORD)
        changed['decisions'] = [d for d in changed['decisions'] if d['audit'] != 'f5-14b'][:-1] + \
            [d for d in changed['decisions'] if d['audit'] == 'f5-14b']
        changed['decisions'].pop()
        with self.assertRaisesRegex(ValueError, 'decision'):
            wu.validate(changed)
        changed = copy.deepcopy(RECORD)
        changed['decisions'] = [d for d in changed['decisions'] if d['audit'] != 'f5-20c']
        with self.assertRaisesRegex(ValueError, 'decision'):
            wu.validate(changed)
        changed = copy.deepcopy(RECORD)
        first = next(d for d in changed['decisions'] if d['audit'] == 'f5-14b')
        first['f5_20c'] = 'open: missing-export kernel32.dll!X'
        with self.assertRaisesRegex(ValueError, 'not closed'):
            wu.validate(changed)
        changed = copy.deepcopy(RECORD)
        changed['decisions'] = [d for d in changed['decisions']
                                if not (d['audit'] == 'f5-20b' and d['name'] == 'CreateWindowExW')]
        with self.assertRaisesRegex(ValueError, 'outside-table'):
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

    def test_icon_trim_filter_is_recorded_and_deterministic(self):
        spec = json.loads((ROOT / 'third_party/wine/patches/0006-shell32-icon-trim.json').read_text())
        self.assertEqual(spec['keep'], {'sizes': [16, 32, 48], 'max_bit_count': 8, 'png': False})
        self.assertEqual(RECORD['recipe']['filters']['0006-shell32-icon-trim.json'], spec['totals'])
        for name, entry in spec['files'].items():
            self.assertGreater(entry['images'][1], 0, name)
            self.assertLessEqual(entry['images'][1], entry['images'][0], name)
        # A synthetic icon: PNG, 32 bpp and 64 px images go; 4/8 bpp 16/32/48 stay.
        import struct
        def image(width, bits):
            return struct.pack('<IiiHHIIiiII', 40, width, 2 * width, 1, bits, 0, 0, 0, 0, 0, 0) + b'\0' * 8
        images = [image(16, 4), wu.PNG + b'\0' * 40, image(32, 32), image(48, 8), image(64, 8)]
        offset, directory = 6 + 16 * len(images), b''
        for data in images:
            directory += struct.pack('<BBBBHHII', 0, 0, 0, 0, 1, 0, len(data), offset)
            offset += len(data)
        icon = struct.pack('<HHH', 0, 1, len(images)) + directory + b''.join(images)
        out, counts = wu.trim_icon(icon, spec['keep'])
        self.assertEqual(counts, [5, 2])
        self.assertEqual(out[6 + 32:], images[0] + images[3])
        with self.assertRaisesRegex(ValueError, 'without a kept image'):
            wu.trim_icon(struct.pack('<HHH', 0, 1, 1) + struct.pack('<BBBBHHII', 0, 0, 0, 0, 1, 32, 48, 22)
                         + images[2], spec['keep'])

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
        # f5-14B amendment 2: the trimmed set fits the 14,680,064-byte budget,
        # and SHELL32 is under the loader's 4,096-entry resource walk budget.
        self.assertTrue(fit['size_of_image_within_budget'])
        self.assertEqual(fit['installer_budget_bytes'], 14680064)
        self.assertLess(RECORD['dlls']['shell32.dll']['resource_entries'], 4096)

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


class CoreAmendmentTests(unittest.TestCase):
    """f5-20C pieces outside the Wine build: absent-DLL W21 stubs, census
    stubs, USER32/GDI32 w-stubs gates, critical-section debug blocks."""

    def test_absent_dll_stubs_cover_every_decided_entry(self):
        decided = {(d['provider'], d['name']) for d in RECORD['decisions']
                   if d['audit'] == 'f5-14b' and d['action'] == 'w21-stub-dll'}
        table = {(dll, name) for dll, names in upper_dlls.ABSENT_DELAYED.items() for name in names}
        self.assertEqual(decided, table)
        for dll, names in upper_dlls.ABSENT_DELAYED.items():
            for name, failure in names.items():
                self.assertIn(failure, upper_dlls.STUB_FAILURES, (dll, name))
        store_path = CORE / 'builtin-store.json'
        if not store_path.is_file():
            self.skipTest('core builtin store not built')
        store = json.loads(store_path.read_text())
        upper_dlls.verify_absent_stubs(CORE, store)
        self.assertEqual(sorted(d['name'] for d in store['dlls'] if d.get('origin') == upper_dlls.STUB_ORIGIN),
                         sorted(upper_dlls.ABSENT_DELAYED))

    def test_census_stub_amendments_and_wide_gates(self):
        self.assertEqual({dll: len(n) for dll, n in upper_dlls.census_stub_amendments().items()},
                         {'imm32.dll': 3, 'mpr.dll': 2, 'winspool.drv': 12})
        from scripts.win32 import stub_signatures
        for dll, names in upper_dlls.census_stub_amendments().items():
            for name in names:
                sig = stub_signatures.signature(dll, name)
                self.assertIsNotNone(sig and sig['convention'], (dll, name))
        for stem in ('user32', 'gdi32'):
            coverage = CORE / (stem + '-coverage.json')
            image = CORE / (stem + '.dll')
            if not coverage.is_file():
                self.skipTest('core not built')
            from scripts.win32 import census
            exports = {n for e in census.PE(image.read_bytes()).exports() for n in e['names']}
            gated = set(json.loads(coverage.read_text())['wide_gate'])
            self.assertTrue(gated and gated <= exports, stem)
            self.assertTrue(all(n.endswith('W') for n in gated), stem)
        self.assertIn('SendMessageW', json.loads((CORE / 'user32-coverage.json').read_text())['wide_gate'])
        self.assertIn('TextOutW', json.loads((CORE / 'gdi32-coverage.json').read_text())['wide_gate'])

    def test_critical_section_debug_blocks(self):
        out = ROOT / 'build/host/win32/wine-tests'
        out.mkdir(parents=True, exist_ok=True)
        program = out / 'critical'
        sources = [Path(__file__).with_name('critical_debug_test.c')] + [
            ROOT / 'sdk/win32/ntdll' / (n + '.c') for n in ('rtl', 'heap', 'critical')]
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DW32_HOST_MODEL',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I' + str(ROOT / 'sdk/win32/include'), '-I' + str(ROOT / 'src/kernel/include'),
                        *map(str, sources), '-o', str(program)], check=True, timeout=120)
        result = subprocess.run([program], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS critical section debug blocks', result.stdout)


class StagingTests(unittest.TestCase):
    """Qualified reused PEs (MSVCRT) join the image's builtin store, fail closed."""

    def test_qualified_reused_pe_staging(self):
        qualified = sorted(n for n, r in RECORD['dlls'].items() if r['qualified'])
        self.assertTrue(set(qualified) <= set(wu.QUALIFIED))
        for name in qualified:
            self.assertTrue(wu.qualified_by_evidence(RECORD, name), name)
        store = {'schema': 1, 'dlls': [], 'msvcrt_policy': 'base'}
        with tempfile.TemporaryDirectory(dir=ROOT / 'build/host/win32') as directory:
            folder, pe = Path(directory), Path(directory) / 'pe'
            pe.mkdir()
            if not qualified:
                self.assertEqual(upper_dlls.stage_reused(folder, store, pe), (store, None))
                return
            with self.assertRaisesRegex(ValueError, 'missing or changed'):
                upper_dlls.stage_reused(folder, store, pe)
            if not local_build_matches():
                self.skipTest('local PEs absent')
            for name in qualified:
                (pe / name).write_bytes((BUILD / 'pe' / name).read_bytes())
            staged, out = upper_dlls.stage_reused(folder, store, pe)
            self.assertEqual([d['name'] for d in staged['dlls']], qualified)
            self.assertIn('staged as the builtin MSVCRT', staged['msvcrt_policy'])
            self.assertEqual(json.loads((out / 'builtin-store.json').read_text()), staged)
            raw = (out / 'BUILTIN.BIN').read_bytes()
            self.assertEqual(int.from_bytes(raw[4:8], 'little'), len(qualified))
            clash = {'schema': 1, 'dlls': [{'name': qualified[0], 'path': qualified[0]}]}
            with self.assertRaisesRegex(ValueError, 'shadows a builtin'):
                upper_dlls.stage_reused(folder, clash, pe)


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
        residual = [{k: v for k, v in d.items() if k != 'audit'} for d in RECORD['decisions']
                    if d['audit'] == 'f5-20c' and d['rule'] not in ('core-census-w21', 'core-api-f5-20c')]
        self.assertEqual(result['decisions'], residual)
        self.assertEqual(result['imports'], RECORD['imports'])
        self.assertEqual(result['w21_bound'], RECORD['w21_bound'])

    def test_production_loader_h0_binding_of_each_reused_dll(self):
        store_path = CORE / 'builtin-store.json'
        if not local_build_matches() or not store_path.is_file():
            self.skipTest('local PEs or core outputs absent')
        reused = [m + '.dll' for m in wu.MODULES if m + '.dll' not in wu.NOT_REUSED]
        rows = store_rows(json.loads(store_path.read_text()), reused)
        with tempfile.TemporaryDirectory(dir=ROOT / 'build/host/win32') as directory:
            directory = Path(directory)
            (directory / 'upper_store.h').write_text(store_header('upper_store', rows) +
                'static const char *const upper_names[] = {' +
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
        evidence = {}
        for line in lines:
            # f5-20C: every reused DLL binds; its closure attaches and detaches.
            self.assertEqual(line[2], 'OK', line)
            attach, detach = int(line[3].split('=')[1]), int(line[4].split('=')[1])
            self.assertGreater(attach, 0, line)
            self.assertEqual(attach, detach, line)
            evidence[line[1]] = {'bound': True, 'dllmain_attach_observed': attach,
                                 'dllmain_detach_observed': detach}
        wu.dump(BUILD / 'h0-load.json', {'pe_sha256': {n: RECORD['dlls'][n]['sha256'] for n in reused},
                                         'core_pe_sha256': core_hashes(), 'dlls': evidence})


def core_hashes():
    return {p.name.lower(): wu.sha(p) for p in sorted(CORE.iterdir())
            if p.suffix.lower() in ('.dll', '.drv', '.acm') and p.is_file()}


def store_rows(store, reused, ntdll=None):
    """Builtin rows: the core store (minus replaced placeholders) plus the reused PEs."""
    rows = []
    for d in store['dlls']:
        name = d['name'].lower()
        if name in reused:
            continue
        if name == 'ntdll.dll' and ntdll:
            raw = Path(ntdll).read_bytes()
            pe = int.from_bytes(raw[60:64], 'little')
            rows.append((d['name'], str(ntdll), wu.sha(ntdll), 0x70000000,
                         int.from_bytes(raw[pe + 80:pe + 84], 'little'), [0, 0, 0, 0]))
            continue
        rows.append((d['name'], str(CORE / d['path']), d['sha256'], d['base'], d['size'], d['version']))
    rows += [(n, str(BUILD / 'pe' / n), RECORD['dlls'][n]['sha256'], RECORD['dlls'][n]['base'],
              RECORD['dlls'][n]['size_of_image'], [0, 0, 0, 0]) for n in reused]
    return rows


def store_header(symbol, rows):
    return ('static struct w32_builtin ' + symbol + '[] = {\n' + ''.join(
        '{' + ','.join([json.dumps(n), json.dumps(p), json.dumps(s), hex(b), str(z),
                        '{' + ','.join(map(str, v)) + '}']) + '},\n'
        for n, p, s, b, z, v in rows) + '};\n')


def build_h1_ntdll(out):
    """The adapter's PE ntdll (production loader/ntdll sources) with the H1
    transport instead of services/ciuki_stub.c and ntdll/server.c."""
    core = ROOT / 'sdk/win32'
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    sources = [s for s in sorted((core / 'ldr').glob('*.c')) + sorted((core / 'ntdll').glob('*.c'))
               if s.name != 'server.c']
    sources += [core / 'ntdll/context.S', core / 'ntdll/runtime.S', Path(__file__).with_name('h1_transport.c')]
    flags = ['--target=i686-pc-windows-msvc', '-march=pentiumpro', '-ffreestanding', '-fno-builtin',
             '-fno-stack-protector', '-fno-omit-frame-pointer', '-mno-sse', '-mno-sse2', '-mno-mmx',
             '-fno-ident', '-O1', '-Wall', '-Wextra', '-Werror', '-fno-pic', '-fno-pie',
             '-I' + str(core / 'include'), '-I' + str(ROOT / 'src/kernel/include')]
    objects = []
    for i, source in enumerate(sources):
        obj = out / f'{i:02d}-{source.stem}.o'
        subprocess.run(['clang', *flags, '-c', str(source), '-o', str(obj)], check=True, timeout=120)
        objects.append(str(obj))
    subprocess.run(['llvm-rc', '/fo', str(out / 'version.res'), str(core / 'ntdll/version.rc')], check=True)
    image = out / 'ntdll.dll'
    subprocess.run(['lld-link', '/machine:x86', '/dll', '/noentry', '/nodefaultlib', '/safeseh:no',
                    '/base:0x70000000', '/subsystem:windows,4.0', '/osversion:4.0', '/timestamp:0',
                    '/incremental:no', '/def:' + str(core / 'ntdll/ntdll.def'), '/out:' + str(image),
                    *objects, str(out / 'version.res')], check=True, timeout=120)
    return image


def build_h1(directory, reused):
    """The H1 adapter ELF and its store header (PE ntdll with the H1 transport)."""
    directory = Path(directory)
    main = directory / 'upper_h1.c'
    main.write_text(Path(__file__).with_name('upper_h1.c').read_text())
    ntdll = build_h1_ntdll(directory / 'ntdll')
    rows = store_rows(json.loads((CORE / 'builtin-store.json').read_text()), reused, ntdll)
    (directory / 'upper_h1_store.h').write_text(store_header('h1_store', rows))
    return adapter.build(directory / 'core', main, windows=False), ntdll


H1_PATTERN = re.compile(r'^DLLMAIN (\S+) reason=(\d+) return=(\d+) gate=(\d+)$')


def h1_run(program, name):
    """One reused DLL in its own i386 process: load, DllMains, free."""
    try:
        result = subprocess.run([program, name], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return {'outcome': 'timeout'}
    out = result.stdout
    if result.returncode == 77 and 'SANDBOX-UNAVAILABLE' in out:
        return {'outcome': 'sandbox-unavailable'}
    calls = [m.groups() for m in map(H1_PATTERN.match, out.splitlines()) if m]
    entered = [line.split()[1] for line in out.splitlines() if line.endswith(' enter')]
    record = {'exit': result.returncode,
              'attach': [[n, int(r)] for n, reason, r, _ in calls if reason == '1'],
              'detach': [[n, int(r)] for n, reason, r, _ in calls if reason == '0'],
              'gate_calls': sum(int(g) for *_, g in calls)}
    line = next((l for l in out.splitlines() if l.startswith('RESULT ')), '')
    done = next((l for l in out.splitlines() if l.startswith('DONE ')), '')
    terminate = next((l for l in out.splitlines() if l.startswith('TERMINATE ')), '')
    if terminate:
        record['outcome'] = 'terminated'
        record['status'] = terminate.split()[1]
        record['in_dllmain'] = entered[-1] if len(entered) > len(calls) else None
    elif ' LOADED ' in line and result.returncode == 0:
        record['outcome'] = 'loaded'
    else:
        record['outcome'] = 'failed'
        record['result'] = line
    if done:
        record['resources'] = done
    return record


SUMMARY = re.compile(r'^\w+:(\w+):\S* (\d+) tests executed \((\d+) marked as todo, (\d+) as flaky, '
                     r'(\d+) failures?\), (\d+) skipped\.')


def conformance_run(program, exe, unit):
    """One upstream conformance unit in its own i386 process on the adapter."""
    try:
        result = subprocess.run([program, 'exec', exe, unit], capture_output=True, text=True,
                                errors='replace', timeout=120)
        out, code = result.stdout, result.returncode
    except subprocess.TimeoutExpired as exc:
        out, code = (exc.stdout or b'').decode('latin-1') if isinstance(exc.stdout, bytes) else (exc.stdout or ''), None
    lines = out.splitlines()
    record = {'failed_checks_reported': sum('Test failed' in l for l in lines)}
    summary = next((m for m in map(SUMMARY.match, lines) if m), None)
    if summary:
        record.update(dict(zip(('executed', 'todo', 'flaky', 'failures', 'skipped'),
                               map(int, summary.groups()[1:]))))
    crash = next((re.search(r'unhandled exception ([0-9a-f]+) at ([0-9A-Fa-f]+)', l) for l in lines
                  if 'unhandled exception' in l), None)
    terminate = next((l.split()[1] for l in lines if l.startswith('TERMINATE ')), None)
    stats = next((l for l in lines if l.startswith('STATS ')), '')
    record['kernel_enosys'] = dict(re.findall(r'enosys:(0x[0-9a-f]+)=(\d+)', stats))
    if code is None:
        record['outcome'] = 'timeout'
    elif any(l.startswith('REFUSE ') for l in lines):
        record['outcome'] = 'refused'
    elif summary and terminate == str(record['failures']):
        record['outcome'] = 'completed'
    elif crash:
        record['outcome'] = 'crashed'
        record['exception'] = crash.group(1)
    elif summary:
        record['outcome'] = 'completed-then-faulted'
    else:
        record['outcome'] = 'exited'
        record['exit_code'] = terminate
    return record


class LoadTests(unittest.TestCase):
    """H1: actual i386 execution of every DllMain on the Linux adapter."""

    def test_h1_dllmain_attach_detach_of_each_reused_dll(self):
        store_path = CORE / 'builtin-store.json'
        if not local_build_matches() or not store_path.is_file():
            self.skipTest('local PEs or core outputs absent')
        reused = [m + '.dll' for m in wu.MODULES if m + '.dll' not in wu.NOT_REUSED]
        with tempfile.TemporaryDirectory(dir=ROOT / 'build/host/win32') as directory:
            program, ntdll = build_h1(directory, reused)
            ntdll_sha = wu.sha(ntdll)
            results = {name: h1_run(program, name) for name in reused}
        if all(r['outcome'] == 'sandbox-unavailable' for r in results.values()):
            self.skipTest('seccomp filter refused: PE code cannot be confined on this host')
        wu.dump(BUILD / 'h1-load.json', {'pe_sha256': {n: RECORD['dlls'][n]['sha256'] for n in reused},
                                         'core_pe_sha256': core_hashes(),
                                         'adapter_ntdll_sha256': ntdll_sha, 'dlls': results})
        for name, r in results.items():
            with self.subTest(dll=name):
                self.assertEqual(r['outcome'], 'loaded', r)
                self.assertTrue(r['attach'] and all(v == 1 for _, v in r['attach']), r)
                self.assertEqual(sorted(n for n, _ in r['attach']), sorted(n for n, _ in r['detach']), r)
                # Every image view is released; the two remaining host
                # allocations are KERNEL32's process-lifetime command-line
                # buffers (GetCommandLineA/W, cached as on Windows).
                self.assertIn(' mappings=0', r['resources'])
                self.assertRegex(r['resources'], r' allocations=[02] ')
                self.assertIn(name, [n for n, _ in r['attach']])

    def test_h1_kernel32_debugbreak_raises_breakpoint(self):
        """Lead decision (f5-20C): DebugBreak is an int 3 delivered as
        EXCEPTION_BREAKPOINT through the SEH dispatch; a handler continues."""
        store_path = CORE / 'builtin-store.json'
        if not local_build_matches() or not store_path.is_file():
            self.skipTest('local PEs or core outputs absent')
        reused = [m + '.dll' for m in wu.MODULES if m + '.dll' not in wu.NOT_REUSED]
        with tempfile.TemporaryDirectory(dir=ROOT / 'build/host/win32') as directory:
            program, _ = build_h1(directory, reused)
            result = subprocess.run([program, 'debugbreak'], capture_output=True, text=True, timeout=60)
        if result.returncode == 77:
            self.skipTest('seccomp filter refused')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS DebugBreak EXCEPTION_BREAKPOINT at 0x702', result.stdout)

    def test_h1_upstream_conformance_units(self):
        """The census-selected Wine 10.0 conformance units (wine_upper.CONFORMANCE)
        over the CiukiOS core exports. The adapter is not the CiukiOS kernel:
        file, object, registry and window services fail with ENOSYS, so the
        results are recorded per unit (completed, failures, crashes), not
        asserted to pass; the guest run (w32-upper-dlls-qemu-t23) is the gate."""
        programs_path = BUILD / 'tests-pe/programs.json'
        store_path = CORE / 'builtin-store.json'
        if not local_build_matches() or not store_path.is_file() or not programs_path.is_file():
            self.skipTest('local PEs, core or conformance programs absent '
                          '(scripts/win32/wine_upper.py conformance-build)')
        programs = json.loads(programs_path.read_text())
        reused = [m + '.dll' for m in wu.MODULES if m + '.dll' not in wu.NOT_REUSED]
        with tempfile.TemporaryDirectory(dir=ROOT / 'build/host/win32') as directory:
            program, ntdll = build_h1(directory, reused)
            ntdll_sha = wu.sha(ntdll)
            results = {}
            for module, info in sorted(programs.items()):
                exe = BUILD / 'tests-pe' / info['program']
                self.assertEqual(wu.sha(exe), info['sha256'])
                for unit in info['units']:
                    results[f'{module}:{unit}'] = conformance_run(program, str(exe), unit)
        if all(r['outcome'] == 'refused' for r in results.values()):
            self.fail('no conformance program could bind: ' + repr(results))
        wu.dump(BUILD / 'conformance.json', {'pe_sha256': {n: RECORD['dlls'][n]['sha256'] for n in reused},
                                             'core_pe_sha256': core_hashes(), 'programs': programs,
                                             'adapter_ntdll_sha256': ntdll_sha, 'units': results})
        self.assertEqual(set(results), {f'{m}:{u}' for m, units in wu.CONFORMANCE.items() for u in units})
        recorded = RECORD.get('conformance', {}).get('units')
        if recorded and RECORD['conformance'].get('core_pe_sha256') == core_hashes():
            self.assertEqual({k: v['outcome'] for k, v in results.items()},
                             {k: v['outcome'] for k, v in recorded.items()})


if __name__ == '__main__':
    unittest.main()
