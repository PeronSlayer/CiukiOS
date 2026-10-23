#!/usr/bin/env python3
"""f1-109: compile the exact runtime + production OSL, no guest or image."""
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import os
import runpy
import subprocess
import hashlib
import argparse
import re

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/acpi'

def run(args):
    subprocess.run([str(a) for a in args], check=True, env={**os.environ, 'TMPDIR': str(OUT)})

def main():
    global OUT
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitizer', choices=('address,undefined', 'undefined'), default='address,undefined')
    args = parser.parse_args()
    if args.sanitizer == 'undefined': OUT = OUT.parent / 'acpi-ubsan'
    OUT.mkdir(parents=True, exist_ok=True)
    recipe = runpy.run_path(str(ROOT / 'src/kernel/acpi/build.py'))
    sources = recipe['prepare'](OUT)
    common = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
              '-Wno-unused-parameter', '-fno-builtin', '-fno-strict-aliasing',
              '-fsanitize=' + args.sanitizer, '-fno-sanitize-recover=all']
    objects = []
    headers = b''.join(p.read_bytes() for p in sorted((OUT / 'include').rglob('*.h')))
    headers += b''.join(p.read_bytes() for p in sorted((ROOT / 'src/kernel/acpi').glob('*.h')))
    headers += b''.join((ROOT / 'src/kernel/include/ciuki' / n).read_bytes() for n in
                        ('abi.h', 'firmware.h', 'firmware_abi.h', 'registry.h', 'sync.h'))
    for p in [*sources, *(ROOT / 'src/kernel/acpi' / n for n in ('osl.c', 'tables.c', 'inventory.c', 'regions.c', 'sources.c', 'thermal.c', 'ec.c', 'events.c'))]:
        obj = OUT / (p.stem + '.o')
        cmd = [*common, '-DCIUKI_ACPI_HOST', *recipe['flags'](OUT), '-c', p, '-o', obj]
        digest = hashlib.sha256(headers + p.read_bytes() + repr(cmd).encode()).hexdigest()
        stamp = obj.with_suffix('.sha256')
        if not obj.exists() or not stamp.exists() or stamp.read_text() != digest:
            run(cmd)
            stamp.write_text(digest)
        objects.append(obj)
    run([*common, '-U__linux__', '-U_LINUX', '-DCIUKI_ACPICA', '-DCIUKI_ACPI_HOST', '-D_DEFAULT_SOURCE',
         '-I', OUT / 'include', '-I', ROOT / 'src/kernel/acpi', '-I', ROOT / 'src/kernel/include',
         '-c', ROOT / 'tests/host/acpi/test.c', '-o', OUT / 'test.o'])
    run([*common, *objects, OUT / 'test.o', '-Wl,-Map=' + str(OUT / 'host.map'), '-o', OUT / 'test'])
    run([*common, '-U__linux__', '-U_LINUX', '-DCIUKI_ACPICA', '-DCIUKI_ACPI_HOST',
         '-I', OUT / 'include', '-I', ROOT / 'src/kernel/acpi', '-I', ROOT / 'src/kernel/include',
         '-c', ROOT / 'tests/host/acpi/events_test.c', '-o', OUT / 'events_test.o'])
    run([*common, *objects, OUT / 'events_test.o', '-o', OUT / 'events_test'])
    run([*common, '-I', ROOT / 'src/kernel/include', ROOT / 'tests/host/acpi/abi.c', '-o', OUT / 'abi'])
    runpy.run_path(str(ROOT / 'tests/host/acpi/fixtures.py'))['generate'](OUT)
    cases = [[OUT / 'abi']]
    cases += [[OUT / 'events_test', OUT, case] for case in ('events', 'no-crt', 'passive', 'hot', 'unit', 'unknown', 'bix',
        'ec-events', 'ec-timeout', 'route', 'enable-timeout', 'rollback', 'init-failure', 'pts-failure', 'storm', 'thermal-change', 'global-lock',
        'gpe-loop', 'shared-power', 'critical-battery', 'lid-open', 'battery-limit', 'eisa-button', 'cid-button', 'already-enabled',
        'provider-busy', 't23-legacy', 'tzp-poll', 'tzp-zero', 'tzp-ec')]
    cases += [[OUT / 'test', OUT, case] for case in ('inventory', 'ssdt', 'checksum', 'root64', 'dsdt64', 'length', 'aml', 'loop', 'sleep', 'write', 'region', 'osl', 'broker', 'pnp', 'page')]
    cases += [[OUT / 'test', OUT, case] for case in ('crs-bad', 'mem64', 'large', 'module-loop', 'rsdt', 'rsdp-checksum', 'pnp-bad', 'no-acpi', 'eisa', 'ec', 'module-write', 'start-failure')]
    cases += [[OUT / 'test', OUT, case] for case in ('serial', 'serial-disabled', 'serial-cid', 'serial-level', 'serial-low', 'serial-variable')]
    cases += [[OUT / 'test', OUT, case] for case in ('preempted', 'preempted-wall', 'starved', 't23-load', 't23-load-over')]
    failures = blocked = 0
    fault_count = 96
    # The successful fixture measures how many allocation sites are reached;
    # fault each of them in a fresh process, including discovery/evaluation.
    index = 0
    base_count = len(cases)
    while index < len(cases):
        cmd = cases[index]
        index += 1
        r = subprocess.run([str(a) for a in cmd], capture_output=True, text=True,
                           env={**os.environ, 'TMPDIR': str(OUT)})
        if r.stdout: print(r.stdout, end='')
        match = re.search(r'allocations=(\d+)', r.stdout)
        if match and cmd[0] == OUT / 'test': fault_count = max(fault_count, int(match[1]))
        if r.returncode:
            if 'LeakSanitizer has encountered a fatal error' in r.stderr:
                blocked += 1
                print('BLOCKED LeakSanitizer sandbox inspection:', ' '.join(map(str, cmd)))
            else:
                failures += 1
                print('FAIL', r.returncode, ' '.join(map(str, cmd)), r.stderr)
        if index == base_count:
            cases += [[OUT / 'test', OUT, 'allocation', n] for n in range(fault_count)]
    print(f'acpi: {len(cases)} cases, {failures} failures, {blocked} LeakSanitizer blocks; sanitizer={args.sanitizer}')
    if failures or blocked: raise SystemExit(1)

if __name__ == '__main__':
    main()
