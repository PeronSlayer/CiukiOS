"""F5 core publisher dry-runs against unchanged suite predicates, not PE execution."""
import json
from pathlib import Path
import subprocess
import sys
import unittest
from scripts.win32 import core_observer_fixtures, census

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'scripts/test'))
from evidence import evaluate_predicates, EvidenceError


class CoreObserverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = ROOT/'build/host/win32/core-observer'
        cls.out.mkdir(parents=True, exist_ok=True)
        cls.binary = cls.out/'protocol'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(ROOT/'tests/host/win32/core_observer_protocol.c'), '-o', str(cls.binary)], check=True)

    def dry_run(self, name):
        index = core_observer_fixtures.NAMES.index(name)+1
        result = subprocess.run([self.binary, str(index)], capture_output=True, text=True)
        records = []
        for line in result.stdout.splitlines():
            probe, event, payload = line.split(' ', 2)
            self.assertEqual(probe, 'w32-'+name)
            records.append(dict(event=event, **dict(token.split('=', 1) for token in payload.split())))
        case = next(c for c in json.loads((ROOT/'tests/suites/f5-core.json').read_text())['cases']
                    if c['probe'] == 'w32-'+name)
        self.assertEqual(evaluate_predicates(records, case['expected']), [])
        # Every authored field must reject a counterexample, independently of
        # the C schema. Deleting any required record must also fail.
        for predicate in case['expected']['predicates']:
            for field, constraint in predicate['fields'].items():
                wrong = (constraint.get('ge', 0)-1 if 'ge' in constraint
                         else constraint.get('eq', constraint.get('le'))+1)
                negative = [dict(r, **{field: str(wrong)}) if r['case'] == predicate['where']['case']
                            else dict(r) for r in records]
                with self.assertRaises(EvidenceError):
                    evaluate_predicates(negative, case['expected'])
            with self.assertRaises(EvidenceError):
                evaluate_predicates([r for r in records if r['case'] != predicate['where']['case']], case['expected'])
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_fullscreen(self): self.dry_run('fullscreen')
    def test_registry(self): self.dry_run('registry')
    def test_audio(self): self.dry_run('audio')
    def test_gdi32(self): self.dry_run('gdi32')
    def test_crash(self): self.dry_run('crash')
    def test_user32(self): self.dry_run('user32')
    def test_codecs(self): self.dry_run('codecs')
    def test_ddraw(self): self.dry_run('ddraw')
    def test_input(self): self.dry_run('input')

    def test_guest_pes_are_reproducible_and_have_real_imports(self):
        images = core_observer_fixtures.build(self.out/'pe')
        first = {p.name: p.read_bytes() for p in images}
        second = core_observer_fixtures.build(self.out/'pe')
        self.assertEqual(first, {p.name: p.read_bytes() for p in second})
        for name, raw in first.items():
            with self.subTest(image=name):
                info = census.inspect(raw)
                imports = {n for row in info['imports'] for n in row['symbols']}
                self.assertLessEqual({'VirtualAlloc', 'Sleep', 'ExitProcess'}, imports)
                self.assertFalse(any(n.startswith(('Adapter', 'test_')) for n in imports))

    def test_kernel_units_compile_with_canonical_flags(self):
        from scripts.build_kernel import CFLAGS
        out = self.out/'kernel'
        out.mkdir(exist_ok=True)
        for name in ('f2_probes_desktop', 'f5_probes_observers', 'selector'):
            subprocess.run(['clang', *CFLAGS, '-I'+str(ROOT/'src/kernel/include'),
                            '-fstack-usage', '-c', str(ROOT/f'src/kernel/probes/{name}.c'),
                            '-o', str(out/(name+'.o'))], check=True)


if __name__ == '__main__':
    unittest.main()
