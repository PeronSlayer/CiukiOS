"""Pure host checks for the lead's QMP input plan; never starts QEMU."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts/test'))
spec = importlib.util.spec_from_file_location('netsurf_probe', ROOT / 'scripts/test/netsurf_probe.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class ProbeTests(unittest.TestCase):
    def test_urls_without_qemu(self):
        for url in ('http://example.com', 'https://example.com'):
            output = io.StringIO()
            with patch.object(probe.subprocess, 'Popen', side_effect=AssertionError('QEMU prohibited')), \
                 patch.object(probe.subprocess, 'run', side_effect=AssertionError('QEMU prohibited')), \
                 contextlib.redirect_stdout(output):
                probe.main(['--dry-run', '--url', url])
            plan = json.loads(output.getvalue())
            self.assertEqual(plan['url'], url)
            self.assertEqual(plan['screenshots'], 20)
            self.assertEqual(plan['nic'], 'i82559er')
            self.assertEqual(plan['typing'][url.index(':')], ['shift', 'semicolon'])
            self.assertEqual(plan['route'][-1], 'ret')

    def test_invalid_urls(self):
        for url in ('file:///secret', 'https://user:password@example.com/',
                    'http:///missing-host', 'http://example.com/\n', 'https://example.com/' + 'x' * 4096):
            with self.subTest(url=url[:40]), self.assertRaises(ValueError):
                probe.url_keys(url)
