#!/usr/bin/env python3
"""Run production F3 selector/COW tests even if an earlier host suite stops.

Reuse the kernel runner's exact F3 preparation/compile commands. No sanitizer
or leak override. SPDX-License-Identifier: MIT
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
script = (ROOT / "scripts/test/host_kernel_tests.sh").read_text()
start = script.index("# F3 lifecycle, BIOS/disk preflight and record channel.")
end = script.index('\n"$out/dosvm_test"\n', start)
prefix = 'set -euo pipefail\nroot="$PWD"\nout="$root/build/host"\nmkdir -p "$out"\n'
prefix += 'export TMPDIR="$out" PYTHONDONTWRITEBYTECODE=1\n'
subprocess.run(["bash"], input=prefix + script[start:end] +
               '\nCIUKI_VM_KERNEL_CHOICE=1 "$out/dosvm_test"\n',
               text=True, cwd=ROOT, check=True)
