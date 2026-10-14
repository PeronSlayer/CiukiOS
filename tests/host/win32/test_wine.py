"""Optional H2: fail on behavioural differences, skip only explicit host blockers."""
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright 2026 CiukiOS contributors.
from pathlib import Path
import signal
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT))
from tests.host.win32.adapter import build,wine_compare

class WineComparisonTests(unittest.TestCase):
    """Each self-authored probe must pass (exit 0) under Wine 11.18 and under
    the i386 adapter with the PE ntdll. Known Wine/Windows differences are
    accepted inside the probe only when it detects Wine, and are listed in
    docs/design/win32-pe-host-evidence.md."""
    @classmethod
    def setUpClass(cls):
        parent=ROOT/'build/host/win32';parent.mkdir(parents=True,exist_ok=True)
        cls.temp=tempfile.TemporaryDirectory(prefix='wine-',dir=parent);cls.addClassCleanup(cls.temp.cleanup)
        cls.out=Path(cls.temp.name);cls.binary=build.build(cls.out/'core')

    def compare(self,probe):
        result=wine_compare.compare(self.out/probe,self.binary,self.out/'core/i686-pc-windows-msvc',probe=probe)
        wine=result['wine'];adapter=result['adapter']
        if wine.get('unavailable'): self.skipTest('Wine unavailable: '+wine['unavailable'])
        if 'wineserver: bind: Operation not permitted' in wine.get('stderr',''):
            self.skipTest('sandbox denies Wine server socket; i386 adapter also requires an unrestricted host')
        self.assertFalse(wine.get('timeout'),wine)
        self.assertEqual(wine.get('exit'),0,wine)
        if adapter['exit']==-signal.SIGSYS: self.skipTest('Wine probe passes; adapter blocked by i386 syscall seccomp, comparison pending')
        self.assertTrue(result['agreement'],result)

    def test_self_authored_nt_equal_heap_and_strings(self):
        self.compare('oracle')

    def test_self_authored_seh_dispatch_unwind_and_faults(self):
        self.compare('seh_probe')

if __name__=='__main__': unittest.main()
