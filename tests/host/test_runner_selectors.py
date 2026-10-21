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
KNOWN_UNREGISTERED = {
    (5, 'w32-restart'): {'owner': 'f5-07 continuation', 'missing': ALL, 'note': 'authored, unrun restart case; guest observer pending'},
    # Found by this check: authored cases whose probe none of the tables knows.
    (5, 'w32-installer-nsis'): {'owner': 'f5-15A2', 'missing': ALL, 'note': 'case written by f5-15A; probe arrives with the NSIS flow (continuation f5-15A2)'},
    # f5-17B title probes: win32-<workload id>, selected by fw_cfg only. The
    # kernel registers none yet; win32_runner reports not_run before any boot.
    (5, 'win32-diablo-shareware'): {'owner': 'f5-17 continuation', 'missing': ALL, 'note': 'authored acceptance case; no guest probe'},
    (5, 'win32-sc'): {'owner': 'f5-17 continuation', 'missing': ALL, 'note': 'authored acceptance case; no guest probe'},
    (5, 'win32-aoe-trial'): {'owner': 'f5-17 continuation', 'missing': ALL, 'note': 'authored acceptance case; no guest probe'},
    (5, 'win32-half-life-uplink'): {'owner': 'f5-17 continuation', 'missing': ALL, 'note': 'authored acceptance case; no guest probe'},
    (5, 'win32-ut-demo-348'): {'owner': 'f5-17 continuation', 'missing': ALL, 'note': 'authored acceptance case; no guest probe'},
    (5, 'win32-wa'): {'owner': 'f5-17 continuation', 'missing': ALL, 'note': 'authored acceptance case; no guest probe'},
    (5, 'win32-heroes3-demo'): {'owner': 'f5-17 continuation', 'missing': ALL, 'note': 'authored acceptance case; no guest probe'},
    (5, 'win32-rct-demo'): {'owner': 'f5-17 continuation', 'missing': ALL, 'note': 'authored acceptance case; no guest probe'},
    (5, 'win32-jazz2-shareware'): {'owner': 'f5-17 continuation', 'missing': ALL, 'note': 'authored acceptance case; no guest probe'},
    (5, 'win32-ta-demo'): {'owner': 'f5-17 continuation', 'missing': ALL, 'note': 'authored acceptance case; no guest probe'},
}


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
        self.assertEqual(kernel[6], {'net-arp'})
        self.assertTrue(probe_tables.loader_model_accepts(5, 'w32-objects'))
        self.assertFalse(probe_tables.loader_model_accepts(5, 'no-such-probe'))

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
        self.assertEqual({entry['owner'] for entry in KNOWN_UNREGISTERED.values()},
                         {'f5-15A2', 'f5-07 continuation', 'f5-17 continuation'})
        for parts, entry in KNOWN_UNREGISTERED.items():
            with self.subTest(probe=parts):
                self.assertTrue(entry['owner'] and entry['note'])
                self.assertTrue(set(entry['missing']) <= set(ALL))
                self.assertRegex(parts[1], r'^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$')

    def test_lead_note_probes_are_reported_for_the_lead(self):
        # Restart remains owned by the f5-07 continuation; the model matches production.
        for name in ('w32-restart',):
            with self.subTest(probe=name):
                self.assertEqual(probe_tables.registration(5, name),
                                 {'kernel': False, 'menu': False, 'loader_model': False})

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
