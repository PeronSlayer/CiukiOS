#!/usr/bin/env python3
"""Build and test the actual freestanding VGA model, not a DOS VM claim."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tests/virtual-vga')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    model = ROOT / 'src/vm/virtual_vga.c'
    harness = ROOT / 'src/vm/test_virtual_vga.c'
    binary = output / 'test-virtual-vga'
    command = [os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
               '-pedantic', '-O1', '-g', '-fno-omit-frame-pointer',
               '-fsanitize=address,undefined', str(model), str(harness), '-o', str(binary)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(binary)], text=True, capture_output=True)
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    result.check_returncode()
    watcom = Path(os.environ.get('WATCOM', '/opt/watcom'))
    compiler = watcom / 'binl64/wcc386'
    if not compiler.exists():
        compiler = watcom / 'binl/wcc386'
    if not compiler.exists():
        raise SystemExit('OpenWatcom wcc386 is required to verify the target object')
    target_command = [str(compiler), '-zq', '-bt=dos', '-mf', '-3r', '-ecc', '-zl',
                      '-s', '-w4', '-we', '-i=' + str(watcom / 'h'),
                      '-fo=' + str(output / 'virtual_vga.obj'), str(model)]
    subprocess.run(target_command, check=True)
    # Resolve the object by itself with all runtime/default libraries disabled.
    # OMF EXTDEF also lists same-object forward references, so simply rejecting
    # EXTDEF records would incorrectly reject valid freestanding objects.
    link_command = [str(compiler.with_name('wlink')), 'option',
                    'quiet,nodefaultlibs,start=_cvga_init', 'format', 'raw', 'bin',
                    'disable', '1014', 'name', str(output / 'virtual_vga.bin'),
                    'file', str(output / 'virtual_vga.obj')]
    subprocess.run(link_command, check=True)
    report = {
        'result': 'PASS', 'scope': 'standalone VGA device model only',
        'dos_guest_execution': False, 'v86_or_dpmi_memory_trapping': False,
        'host_compiler_command': command, 'target_compiler_command': target_command,
        'freestanding_link_command': link_command,
        'assertions': result.stdout.strip(),
        'target_object_has_no_external_imports': True,
        'physical_hardware_qualified': False,
    }
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
