"""F5 probe names: the kernel selector, the loader table and the runner model agree.

A name missing from the loader table is refused with L:SELECT_ERROR before the
kernel ever sees it, and a name missing from the selector never starts (the
f5-17B lead note, 2026-10-20). The three sources are read as text.
SPDX-License-Identifier: LGPL-2.1-or-later
"""
import json
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import loader_model

# All core observers, including the separate NSIS flow, are registered.
UNREGISTERED = {}
# f5-15C installers tier: the title aliases are in the kernel selector and the
# loader model; the loader table line is build/codex/f5-15c-menu-inc.patch,
# applied by the lead after the f0-54 merge (the fixed extent is full here).
# Empty this tuple when the patch is applied: the test then requires equality.
MENU_PENDING = ()  # the ten f5-15C title probes joined the loader table on 2026-10-21 (after f0-54)
F5_SUITES = ('f5-core.json', 'f5-installers.json', 'f5-titles.json')


def selector_names():
    text = (ROOT / 'src/kernel/probes/selector.c').read_text()
    body = re.search(r'f5_names\[\]\s*=\s*\{([^}]*)\}', text).group(1)
    return re.findall(r'"([^"]+)"', body)


def loader_names():
    text = (ROOT / 'src/boot/ciukldr/menu.inc').read_text()
    row = re.search(r'^f5_probe_names db (.*)$', text, re.M).group(1)
    items = [part.strip() for part in row.split(',')]
    names = []
    for item in items:
        if item.startswith("'"):
            names.append(item.strip("'"))
        else:
            assert item == '0', item
    return names, items


class ProbeRegistryTests(unittest.TestCase):
    def test_kernel_and_loader_tables_are_the_same_list(self):
        loader, items = loader_names()
        self.assertEqual([n for n in selector_names() if n not in MENU_PENDING], loader)
        self.assertFalse(set(MENU_PENDING) & set(loader), 'patch applied: empty MENU_PENDING')
        self.assertEqual(items[-2:], ['0', '0'], 'the table ends with an empty name')
        self.assertEqual(len(set(loader)), len(loader))

    def test_runner_model_matches_the_production_tables(self):
        self.assertEqual(list(loader_model.F5_PROBES), selector_names())

    def test_suite_selectors_are_registered_or_explicitly_pending_with_owners(self):
        suite = json.loads((ROOT / 'tests/suites/f5-core.json').read_text())
        pending = set()
        for case in suite['cases']:
            name = case['selector'].split()[0].removeprefix('f5:')
            self.assertEqual(name, case['probe'], case['id'])
            if name not in selector_names():
                pending.add(name)
                self.assertNotIn(name, loader_names()[0])
                self.assertTrue(case.get('integration_pending'), case['id'])
                if name in UNREGISTERED:
                    self.assertIn(UNREGISTERED[name], case['integration_pending'])
                with self.assertRaises(ValueError):
                    loader_model.selector(case['selector'].format(run_id='12345678'), 'fw_cfg', True)
            else:
                self.assertIn(name, loader_names()[0])
                loader_model.selector(case['selector'].format(run_id='12345678'), 'fw_cfg', True)
        self.assertEqual(set(UNREGISTERED), pending)

    def test_every_registered_probe_has_its_registration_and_a_suite_case(self):
        probes = set()
        for name in F5_SUITES:
            probes |= {case['probe'] for case in json.loads((ROOT / 'tests/suites' / name).read_text())['cases']}
        sources = ''.join(p.read_text() for p in (ROOT / 'src/kernel/probes').glob('*.c'))
        for name in selector_names():
            if name == 'f5-core':
                continue
            self.assertIn(name, probes, name)
            self.assertIn(f'CIUKI_F5_PROBE("{name}"', sources, name)

    def test_upper_dlls_case_selector_is_accepted_by_the_model(self):
        case = next(c for c in json.loads((ROOT / 'tests/suites/f5-core.json').read_text())['cases']
                    if c['probe'] == 'w32-upper-dlls')
        picked = loader_model.selector(case['selector'].format(run_id='12345678'), 'fw_cfg', True)
        self.assertEqual((picked['phase'], picked['probe']), (5, 'w32-upper-dlls'))
        self.assertIn('w32-upper-dlls', selector_names())
        self.assertIn('w32-upper-dlls', loader_names()[0])

class ProbeWriteGateTests(unittest.TestCase):
    @staticmethod
    def starters(text):
        """Inspect function bodies, including nested error/cleanup blocks."""
        starts = []
        for match in re.finditer(r'\b(?:static\s+)?int\s+(\w+)\([^;{}]*\)\s*\{', text):
            at, depth = match.end(), 1
            while at < len(text) and depth:
                depth += (text[at] == '{') - (text[at] == '}')
                at += 1
            body = text[match.end():at-1]
            # Every desktop launcher can start w32srv through a gate command.
            spawn = re.search(r'\bsupervisor_spawn_desktop_probe\s*\(', body)
            if not spawn:
                spawn = re.search(r'\bsupervisor_spawn\s*\(\s*"/(?:Programs/desktop|System/Win32/w32srv)"', body)
            if spawn:
                gate = re.search(r'storage_enable_write\s*\(\s*storage_get\s*\(\s*\)\s*,\s*2\s*\)', body)
                starts.append((match[1], bool(gate and gate.start() < spawn.start())))
        return starts

    def test_all_desktop_probe_launchers_open_c_write_gate_before_spawn(self):
        observed = []
        for source in (ROOT/'src/kernel/probes').glob('*.c'):
            observed.extend(self.starters(source.read_text()))
        self.assertGreaterEqual(len(observed), 3)
        self.assertEqual([name for name, gated in observed if not gated], [])

    def test_missing_or_late_gate_is_rejected(self):
        source = (ROOT/'src/kernel/probes/f2_probes_desktop.c').read_text()
        self.assertTrue(all(gated for _, gated in self.starters(source)))
        removed = source.replace('storage_enable_write(storage_get(),2)', '0')
        self.assertTrue(any(not gated for _, gated in self.starters(removed)))
        launch = 'supervisor_spawn("/System/Win32/w32srv", argv, 0, false, &p);'
        gate = 'storage_enable_write(storage_get(),2);'
        self.assertEqual(self.starters('int probe(void) {'+launch+'}'), [('probe', False)])
        self.assertEqual(self.starters('int probe(void) {'+launch+gate+'}'), [('probe', False)])
        self.assertEqual(self.starters('int probe(void) {'+gate+launch+'}'), [('probe', True)])

    def test_installers_tier_probe_opens_the_gate_before_the_desktop(self):
        """f5-15C: probe_title lives in an .inc included by the desktop probe TU."""
        source = (ROOT/'src/kernel/probes/installer_title_probe.inc').read_text()
        self.assertIn(('probe_title', True), self.starters(source))
        removed = source.replace('storage_enable_write(storage_get(),2)', '0')
        self.assertIn(('probe_title', False), self.starters(removed))

if __name__ == '__main__':
    unittest.main()
