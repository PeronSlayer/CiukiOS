#!/usr/bin/env python3
"""Host compile/link check of ACPICA and OSL with canonical i686 flags.

Produces relocatable objects only: no kernel/image build or target execution.
"""
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import os
import runpy
import subprocess

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/acpi-i686'

def main():
    OUT.mkdir(parents=True, exist_ok=True)
    build = runpy.run_path(str(ROOT / 'scripts/build_kernel.py'))
    recipe = runpy.run_path(str(ROOT / 'src/kernel/acpi/build.py'))
    sources = recipe['prepare'](OUT)
    env = {**os.environ, 'TMPDIR': str(OUT)}
    objects = []
    for p in [*sources, *sorted((ROOT / 'src/kernel/acpi').glob('*.c'))]:
        obj = OUT / (p.stem + '.o')
        subprocess.run(['clang', *build['CFLAGS'], *recipe['flags'](OUT), '-c', str(p), '-o', str(obj)], env=env, check=True)
        objects.append(obj)
    combined = OUT / 'acpica-osl.o'
    subprocess.run(['ld.lld', '-m', 'elf_i386', '-r', *(str(p) for p in objects), '-o', str(combined)], check=True, env=env)
    for rel in ('tests/host/acpi/abi.c', 'src/kernel/core/devices.c', 'src/kernel/core/init.c'):
        subprocess.run(['clang', *build['CFLAGS'], '-I', str(ROOT / 'src/kernel/include'), '-c', str(ROOT / rel),
                        '-o', str(OUT / (Path(rel).stem + '-integration.o'))], env=env, check=True)
    subprocess.run(['clang', *build['CFLAGS'], '-I', str(ROOT / 'sdk/sysroot-overlay/include'),
                    '-I', str(ROOT / 'src/kernel/include'), '-c', str(ROOT / 'tests/host/acpi/sdk.c'),
                    '-o', str(OUT / 'sdk-client.o')], env=env, check=True)
    subprocess.run(['size', str(combined)], check=True, env=env)
    result = subprocess.run(['nm', '-u', str(combined)], check=True, text=True, capture_output=True)
    (OUT / 'undefined.txt').write_text(result.stdout)
    print(result.stdout, end='')
    if any(' Acpi' in line for line in result.stdout.splitlines()):
        raise SystemExit('unresolved ACPICA symbol')
    print('PASS i686: canonical flags, 4-KiB frame bound, relocatable link, v1/v2 ABI and integration units')

if __name__ == '__main__': main()
