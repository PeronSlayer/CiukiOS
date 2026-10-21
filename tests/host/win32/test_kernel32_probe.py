"""Kernel32 result grammar and guest PE; this dry-run never claims a guest pass."""
import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'scripts/test'))
from evidence import evaluate_predicates, EvidenceError
from scripts.win32 import kernel32_fixture, census


class Kernel32ProbeTests(unittest.TestCase):
    def test_result_protocol_predicate_dry_run_and_rejects_incomplete_or_failed_results(self):
        out = ROOT/'build/host/win32/kernel32-probe'
        out.mkdir(parents=True, exist_ok=True)
        binary = out/'protocol'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(ROOT/'tests/host/win32/kernel32_probe_test.c'), '-o', str(binary)], check=True)
        result = subprocess.run([binary], text=True, capture_output=True)
        cases = json.loads((ROOT/'tests/suites/f5-core.json').read_text())['cases']
        case = next(c for c in cases if c['probe'] == 'w32-kernel32')
        # Check the publisher's DATA grammar directly; this is synthetic
        # protocol evidence, separate from the registered guest observer.
        records = []
        for line in result.stdout.splitlines():
            probe, event, payload = line.split(' ', 2)
            self.assertEqual(probe, 'w32-kernel32')
            records.append(dict(event=event, **dict(token.split('=', 1) for token in payload.split())))
        self.assertEqual(evaluate_predicates(records, case['expected']), [])
        for failed in ('files', 'memory', 'threads', 'profile', 'teardown'):
            negative = [dict(r, errors='1') if r['case'] == failed else dict(r) for r in records]
            with self.assertRaises(EvidenceError):
                evaluate_predicates(negative, case['expected'])
            with self.assertRaises(EvidenceError):
                evaluate_predicates([r for r in records if r['case'] != failed], case['expected'])
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_guest_fixture_is_pe32_stdcall_and_reproducible(self):
        out = ROOT/'build/host/win32/kernel32-probe/pe'
        image = kernel32_fixture.build(out)
        first = image.read_bytes()
        self.assertEqual(kernel32_fixture.build(out).read_bytes(), first)
        info = census.inspect(first)
        self.assertTrue(info['imports'])
        self.assertEqual({row['dll'].lower() for row in info['imports']}, {'kernel32.dll'})
        names = {n for row in info['imports'] for n in row['symbols']}
        self.assertLessEqual({'VirtualAlloc', 'VirtualProtect', 'VirtualQuery', 'CreateThread',
                             'WaitForSingleObject', 'GetExitCodeThread', 'WritePrivateProfileStringA'}, names)
