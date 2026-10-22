"""f5-17B lead note (2026-10-20): every suite selector must exist in all three tables.

tests/suites/*.json name a guest probe per case. The probe is selectable only
when it appears in the kernel selector (src/kernel/probes/selector.c,
`f<phase>_names`), in the boot-loader menu (src/boot/ciukldr/menu.inc,
`f<phase>_probe_names`) and in the host reference model
(scripts/test/loader_model.py). A case whose name one table lacks cannot be
selected; the runner would boot QEMU and the kernel would reject the request.

KNOWN_UNREGISTERED lists, with owners, the cases that are authored ahead of
their probe. The list is exact: a name missing from a table that is not listed
fails, and an entry that is registered everywhere (or is missing from a
different set of tables) also fails until this list is updated. Nothing here
registers a probe; the owners add the name to the three tables.
"""
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'scripts/test'))
import probe_tables  # noqa: E402
import run as runner  # noqa: E402

ALL = ('kernel', 'menu', 'loader_model')
# (phase, probe name) -> owner and the exact set of tables that lack the name.
# f5-15C: the ten title aliases are registered in the kernel selector and the
# loader model; the loader table entry is the patch build/codex/f5-15c-menu-inc.patch,
# applied by the lead after merging main (f0-54 frees the fixed extent's bytes).
TITLE_ALIASES = ('win32-diablo-shareware', 'win32-sc', 'win32-aoe-trial', 'win32-half-life-uplink',
                 'win32-ut-demo-348', 'win32-wa', 'win32-heroes3-demo', 'win32-rct-demo',
                 'win32-jazz2-shareware', 'win32-ta-demo')
MENU_OWNER = 'lead: merge main (f0-54) then apply build/codex/f5-15c-menu-inc.patch'
KNOWN_UNREGISTERED = {}  # f5-15C title probes registered in the loader table on 2026-10-21 (after f0-54)


def suite_cases():
    """(suite file, case id, selector) for every case that selects a probe by name."""
    rows = []
    for path in sorted((ROOT/'tests/suites').glob('*.json')):
        suite = json.loads(path.read_text())
        for case in runner.expand_cases(suite):
            selector = case.get('selector')
            if selector is not None:
                rows.append((path.name, case['id'], selector))
    return rows


class SelectorTableTests(unittest.TestCase):
    def test_table_parsers_read_every_phase(self):
        kernel, menu = probe_tables.kernel_names(), probe_tables.menu_names()
        for phase in probe_tables.PHASES:
            with self.subTest(phase=phase):
                self.assertTrue(kernel[phase], 'selector.c table not parsed')
                self.assertTrue(menu[phase], 'menu.inc table not parsed')
        self.assertLessEqual({'boot', 'runner', 'all', 'core'}, kernel[0])
        self.assertLessEqual({'sweep', 'boot', 'runner', 'all', 'core'}, menu[0])
        self.assertLessEqual({'f5-core', 'w32-objects', 'w32-dialogs'}, kernel[5] & menu[5])
        self.assertEqual(kernel[6], {'net-arp', 'browser'})
        self.assertTrue(probe_tables.loader_model_accepts(5, 'w32-objects'))
        self.assertFalse(probe_tables.loader_model_accepts(5, 'no-such-probe'))

    def test_menu_lists_keep_labelled_entries_until_their_terminator(self):
        # f3-55 put `dos_conformance_name db` before `f3_title_name db` in the
        # F3 list; a labelled entry must not end the list early.
        menu = probe_tables.menu_names()
        self.assertLessEqual({'sweep', 'vm-fullscreen', 'dos-conformance', 'title'}, menu[3])
        self.assertLessEqual({'threads-wait', 'crash-isolation', 'app-gate'}, menu[2])
        self.assertEqual(probe_tables.registration(3, 'dos-conformance'),
                         {'kernel': True, 'menu': True, 'loader_model': True})
        self.assertEqual(probe_tables.registration(3, 'title'),
                         {'kernel': True, 'menu': True, 'loader_model': True})
        import tempfile
        with tempfile.TemporaryDirectory(prefix='probe-tables-', dir=ROOT/'build/host') as folder:
            menu_inc = Path(folder)/probe_tables.MENU_INC
            menu_inc.parent.mkdir(parents=True)
            menu_inc.write_text('f3_probe_names:\n    db \'a\',0\n; note\n\n'
                                'other_label db \'b\',0\nlast_name db \'c\',0,0\n'
                                'after db \'not-a-probe\',0\n'
                                'f4_probe_names db \'d\',0\ncode_label:\n    db \'e\',0,0\n')
            tables = probe_tables.menu_names(folder)
            self.assertEqual(tables[3], {'a', 'b', 'c'})
            self.assertEqual(tables[4], {'d'})

    def test_selector_splitting(self):
        self.assertEqual(probe_tables.split_selector('f5:w32-memory run={run_id}'), (5, 'w32-memory'))
        self.assertEqual(probe_tables.split_selector('f1:safe run={run_id} safe=1'), (1, 'safe'))
        self.assertEqual(probe_tables.split_selector('f2:crash-isolation'), (2, 'crash-isolation'))
        self.assertIsNone(probe_tables.split_selector('probe=sweep'))
        self.assertIsNone(probe_tables.split_selector(None))

    def test_every_suite_selector_is_in_all_three_tables(self):
        rows = suite_cases()
        self.assertGreater(len(rows), 250)
        problems, observed = [], {}
        for suite, case, selector in rows:
            parts = probe_tables.split_selector(selector)
            if parts is None:
                problems.append('%s %s: selector %r has no f<phase>:<probe> form' % (suite, case, selector))
                continue
            absent = tuple(probe_tables.missing(*parts))
            if absent:
                observed.setdefault(parts, set()).add(absent)
                if parts not in KNOWN_UNREGISTERED:
                    problems.append('%s %s: f%d:%s is missing from %s' % (suite, case, parts[0], parts[1], ', '.join(absent)))
        for parts, entry in sorted(KNOWN_UNREGISTERED.items()):
            if parts not in observed:
                problems.append('f%d:%s is listed as unregistered (owner %s) but every table knows it or no suite uses it: '
                                'remove the entry' % (parts[0], parts[1], entry['owner']))
            elif observed[parts] != {entry['missing']}:
                problems.append('f%d:%s: tables lacking the name are now %s, the list says %s'
                                % (parts[0], parts[1], sorted(observed[parts]), entry['missing']))
        self.assertEqual(problems, [], '\n' + '\n'.join(problems))

    def test_known_unregistered_entries_name_an_owner_and_a_reason(self):
        self.assertLessEqual({entry['owner'] for entry in KNOWN_UNREGISTERED.values()}, {MENU_OWNER})
        for parts, entry in KNOWN_UNREGISTERED.items():
            with self.subTest(probe=parts):
                self.assertTrue(entry['owner'] and entry['note'])
                self.assertTrue(set(entry['missing']) <= set(ALL))
                self.assertRegex(parts[1], r'^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$')

    def test_merged_core_and_restart_observers_are_registered(self):
        # Core and restart observers are registered in the merged state.
        for name in ('w32-fixtures', 'w32-kernel32', 'w32-restart'):
            with self.subTest(probe=name):
                self.assertEqual(probe_tables.registration(5, name),
                                 {'kernel': True, 'menu': True, 'loader_model': True})

    def test_win32_title_probe_names_against_the_kernel_buffer(self):
        # struct probe_selection.probe[24]: at most 23 bytes of probe name.
        manifest = json.loads((ROOT/'tests/win32/workloads.json').read_text())
        aliases = [row['probe'] for row in manifest['workloads']]
        self.assertEqual(len(aliases), len(set(aliases)))
        for alias in aliases:
            self.assertLessEqual(len(alias.encode('ascii')), probe_tables.PROBE_LIMIT)
        self.assertIn('win32-sc', aliases)
        self.assertIn('win32-wa', aliases)


if __name__ == '__main__':
    unittest.main()
