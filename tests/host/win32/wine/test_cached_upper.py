"""f5-20D: the reused Wine DLL build as a cached step of make build-full.

Every scenario runs on copies under build/host/win32 (the SDK output, the
cached PEs' audit files, the record); the real record and cache are never
written. No PE code is executed here and no download happens.
SPDX-License-Identifier: LGPL-2.1-or-later
"""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

from scripts.win32 import wine_upper as wu

ROOT = Path(__file__).resolve().parents[4]
CORE = ROOT / 'build/apps/win32'
CACHE = wu.CACHE


def quiet(*_):
    pass


class MakeStepTests(unittest.TestCase):
    """The Makefile wiring (make -n runs nothing)."""

    def plan(self, target):
        return subprocess.run(['make', '-n', 'CAP=', target], cwd=ROOT, check=True,
                              capture_output=True, text=True).stdout

    def test_build_full_runs_kernel_apps_upper_image_in_order(self):
        plan = self.plan('build-full')
        marks = [plan.index(m) for m in ('build_kernel.py', 'build_apps.py',
                                         'wine_upper.py cached-build', 'build_image.py')]
        self.assertEqual(marks, sorted(marks), plan)
        self.assertEqual(len(set(marks)), 4)

    def test_apps_does_not_depend_on_the_upper_build(self):
        self.assertNotIn('wine_upper', self.plan('apps'))
        self.assertIn('wine_upper.py cached-build', self.plan('upper'))

    def test_clean_keeps_the_cache(self):
        makefile = (ROOT / 'Makefile').read_text()
        clean = makefile[makefile.index('\nclean:'):]
        clean = clean[:clean.index('\n\n')]
        self.assertNotIn('build/win32', clean.split('@echo')[0])
        self.assertIn('build/win32/upper', clean)
        self.assertEqual(wu.CACHE, ROOT / 'build/win32/upper')


class CachedBuildTests(unittest.TestCase):
    def setUp(self):
        if not (CACHE / 'cache.json').is_file() or not wu.pes_match(CACHE) or not (CORE / 'builtin-store.json').is_file():
            self.skipTest('cached upper PEs or core output absent (make apps; make upper)')
        base = ROOT / 'build/host/win32'
        base.mkdir(parents=True, exist_ok=True)
        self.work = Path(tempfile.mkdtemp(dir=base))
        self.addCleanup(shutil.rmtree, self.work, True)
        self.core = self.work / 'core'
        shutil.copytree(CORE, self.core)
        self.out = self.work / 'upper'
        self.out.mkdir()
        (self.out / 'pe').symlink_to(CACHE / 'pe')
        for name in ('cache.json', 'decisions.json', 'import-audit.json', 'build.json'):
            if (CACHE / name).is_file():
                shutil.copyfile(CACHE / name, self.out / name)
        self.record = self.work / 'wine-upper.json'
        shutil.copyfile(wu.RECORD, self.record)
        self.document = self.work / 'audit.md'
        shutil.copyfile(wu.DOCUMENT, self.document)
        for target, value in (('RECORD', self.record), ('DOCUMENT', self.document)):
            patcher = mock.patch.object(wu, target, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.log = []
        # Normalise against the real core (a stale cache would audit or refresh once).
        with mock.patch.object(wu, 'build', side_effect=AssertionError('unexpected rebuild')):
            wu.cached_build(self.out, self.core, log=self.log.append)
        self.log.clear()
        self.reference = self.record.read_bytes()

    def run_cached(self, **kw):
        with mock.patch.object(wu, 'build', side_effect=AssertionError('unexpected rebuild')):
            return wu.cached_build(self.out, self.core, log=self.log.append, **kw)

    def test_warm_run_reuses_everything(self):
        actions = self.run_cached()
        self.assertEqual(len(actions), 1)
        self.assertIn('reused', actions[0])
        self.assertEqual(self.record.read_bytes(), self.reference)
        self.assertEqual(wu.check(self.out, self.core, log=quiet), 0)

    def test_moved_core_hash_is_refreshed_not_an_error(self):
        # A core rebuild that moved PE bytes but no export: the record's hash
        # follows, the audit is not repeated, nothing fails.
        victim = self.core / 'lz32.dll'
        victim.write_bytes(victim.read_bytes() + b'\0' * 16)
        store = json.loads((self.core / 'builtin-store.json').read_text())
        for entry in store['dlls']:
            if entry['name'] == 'lz32.dll':
                entry['sha256'] = wu.sha(victim)
        (self.core / 'builtin-store.json').write_text(json.dumps(store, sort_keys=True, indent=2) + '\n')
        actions = self.run_cached()
        text = '\n'.join(actions)
        self.assertIn('core PE hashes moved without an export change', text)
        self.assertIn('core-DLL hashes refreshed', text)
        self.assertEqual(json.loads(self.record.read_text())['core_pe_sha256']['lz32.dll'], wu.sha(victim))
        # Only that hash moved.
        before, after = json.loads(self.reference), json.loads(self.record.read_text())
        after['core_pe_sha256']['lz32.dll'] = before['core_pe_sha256']['lz32.dll']
        self.assertEqual(before, after)
        self.log.clear()
        self.assertEqual(len(self.run_cached()), 1)  # warm again
        self.assertEqual(wu.check(self.out, self.core, log=quiet), 0)

    def test_check_refreshes_hashes_of_the_built_core(self):
        victim = self.core / 'lz32.dll'
        victim.write_bytes(victim.read_bytes() + b'\0' * 16)
        self.assertEqual(wu.check(self.out, self.core, log=self.log.append), 1)
        self.assertEqual(json.loads(self.record.read_text())['core_pe_sha256']['lz32.dll'], wu.sha(victim))
        self.assertEqual(wu.check(self.out, self.core, log=quiet), 0)

    def test_changed_export_without_a_new_manifest_is_reaudited(self):
        victim = self.core / 'kernel32.dll'
        raw = victim.read_bytes()
        self.assertIn(b'GetLocaleInfoA\0', raw)
        victim.write_bytes(raw.replace(b'GetLocaleInfoA\0', b'XetLocaleInfoA\0'))
        # Leave builtin-store.json untouched: check must inspect actual PEs.
        with self.assertRaisesRegex(ValueError, 'now missing GetLocaleInfoA'):
            wu.check(self.out, self.core, log=quiet)
        self.assertEqual(self.record.read_bytes(), self.reference)

    def test_changed_w21_decision_without_a_new_manifest_is_reaudited(self):
        coverage = self.core / 'kernel32-coverage.json'
        data = json.loads(coverage.read_text())
        row = next(row for row in data['exports'] if row['name'] == 'GetLocaleInfoA')
        row['status'] = 'W21 test mutation'
        coverage.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'decision changed|core W21 stub'):
            self.run_cached()
        self.assertEqual(self.record.read_bytes(), self.reference)

    def test_check_refuses_changed_or_missing_cached_pes(self):
        wrong = self.work / 'wrong-pe'
        shutil.copytree(CACHE / 'pe', wrong)
        (wrong / 'msvcrt.dll').unlink()
        (self.out / 'pe').unlink()
        (self.out / 'pe').symlink_to(wrong)
        with self.assertRaisesRegex(ValueError, 'cached upper PEs missing or changed'):
            wu.check(self.out, self.core, log=quiet)
        self.assertEqual(self.record.read_bytes(), self.reference)

    def test_semantic_differences_fail_and_leave_the_record_alone(self):
        # A decision row gone from the record, as after a core change that
        # needs review.
        data = json.loads(self.record.read_text())
        removed = data['decisions'].pop()
        self.record.write_text(json.dumps(data, sort_keys=True, indent=2) + '\n')
        before = self.record.read_bytes()
        with self.assertRaisesRegex(ValueError, r'(?s)differs from config/wine-upper\.json.*decision added.*refresh-audit') as caught:
            self.run_cached()
        self.assertIn(removed['symbol'], str(caught.exception))
        self.assertEqual(self.record.read_bytes(), before)
        # check validates the record and its rendered tables first.
        with self.assertRaisesRegex(ValueError, 'decision added|generated upper DLL tables differ'):
            wu.check(self.out, self.core, log=quiet)
        # refresh-audit is the reviewed way back.
        wu.refresh_audit(self.out, self.core, log=quiet)
        self.assertEqual(json.loads(self.record.read_text())['decisions'], json.loads(self.reference)['decisions'])

    def test_a_missing_export_in_the_audit_fails(self):
        path = self.out / 'decisions.json'
        decisions = json.loads(path.read_text())
        row = decisions['imports']['comctl32.dll']['imports']['kernel32.dll']
        row['missing'] = sorted(row['missing'] + ['NoSuchCiukiExport'])
        path.write_text(json.dumps(decisions, sort_keys=True, indent=2) + '\n')
        with self.assertRaisesRegex(ValueError, 'comctl32.dll.*kernel32.dll now missing NoSuchCiukiExport'):
            self.run_cached()

    def test_rebuild_reasons_are_logged_and_the_build_never_downloads(self):
        cache = json.loads((self.out / 'cache.json').read_text())
        build_json = json.loads((self.out / 'build.json').read_text())
        # A new archive pin: nothing in the cache matches, the recipe runs.
        cache['archive_sha256'] = build_json['archive_sha256'] = '0' * 64
        (self.out / 'cache.json').write_text(json.dumps(cache))
        (self.out / 'build.json').write_text(json.dumps(build_json))
        with mock.patch.object(wu, 'build', side_effect=RuntimeError('recipe ran')) as recipe:
            with self.assertRaisesRegex(RuntimeError, 'recipe ran'):
                wu.cached_build(self.out, self.core, log=self.log.append)
        recipe.assert_called_once()
        self.assertTrue(any('Wine archive hash changed: rebuilding' in line for line in self.log), self.log)
        # No cache and no archive: refused, with the reason, never downloaded.
        (self.out / 'cache.json').unlink()
        (self.out / 'build.json').unlink()
        with self.assertRaisesRegex(ValueError, r'(?s)no cached upper build.*archive is absent.*never downloads'):
            wu.cached_build(self.out, self.core, archive=self.work / 'absent.tar.xz', log=quiet)

    def test_patch_set_change_is_a_rebuild_reason(self):
        cache = json.loads((self.out / 'cache.json').read_text())
        build_json = json.loads((self.out / 'build.json').read_text())
        cache['recipe_sha256'] = '1' * 64
        build_json['patches'] = dict(build_json['patches'], **{'0001-in-process-rpc-only.patch': '2' * 64})
        (self.out / 'cache.json').write_text(json.dumps(cache))
        (self.out / 'build.json').write_text(json.dumps(build_json))
        with mock.patch.object(wu, 'build', side_effect=RuntimeError('recipe ran')):
            with self.assertRaisesRegex(RuntimeError, 'recipe ran'):
                wu.cached_build(self.out, self.core, log=self.log.append)
        self.assertTrue(any('patch set or recipe changed: rebuilding' in line for line in self.log), self.log)

    def test_changed_base_recipe_cannot_adopt_an_older_build(self):
        with mock.patch.object(wu, 'UPPER_BASE', wu.UPPER_BASE + 0x10000), \
             mock.patch.object(wu, 'build', side_effect=RuntimeError('recipe ran')) as recipe:
            with self.assertRaisesRegex(RuntimeError, 'recipe ran'):
                wu.cached_build(self.out, self.core, log=self.log.append)
        recipe.assert_called_once()
        self.assertTrue(any('patch set or recipe changed: rebuilding' in line for line in self.log), self.log)

    def test_equal_recipe_output_is_adopted_without_a_rebuild(self):
        # cache.json lost (a fresh build/ copy) but the recipe output is intact.
        (self.out / 'cache.json').unlink()
        actions = self.run_cached()
        self.assertTrue(any('already present is adopted' in a for a in actions), actions)
        self.assertTrue((self.out / 'cache.json').is_file())

    def test_a_pe_that_does_not_reproduce_the_record_is_refused(self):
        # Rebuilt bytes that differ from the record (changed toolchain).
        wrong = self.work / 'wrong-pe'
        shutil.copytree(CACHE / 'pe', wrong)
        victim = wrong / 'cabinet.dll'
        victim.write_bytes(victim.read_bytes() + b'\0')
        (self.out / 'pe').unlink()
        (self.out / 'pe').symlink_to(wrong)
        for name in ('cache.json', 'build.json'):
            (self.out / name).unlink()

        def fake_build(output, archive=None):
            return {}
        with mock.patch.object(wu, 'build', side_effect=fake_build):
            with self.assertRaisesRegex(ValueError, 'does not reproduce config/wine-upper.json: cabinet.dll'):
                wu.cached_build(self.out, self.core, log=quiet)


if __name__ == '__main__':
    unittest.main()
