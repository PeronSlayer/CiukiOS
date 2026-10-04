#!/usr/bin/env python3
"""Small host checks for VM time accounting and HDPMI VGA page transitions."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--suite', action='append', choices=('clock', 'vga-mapping'))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    cc = os.environ.get('CC', 'cc')
    flags = ['-std=c99', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-ffunction-sections', '-fdata-sections', '-I' + str(ROOT / 'src/vm')]
    suites = {
        'clock': ['test_session_clock.c', 'session_clock.c', 'guest_peripherals.c'],
        'vga-mapping': ['test_hdpmi_video_adapter.c', 'virtual_vga.c'],
    }
    results = {}
    for name, sources in suites.items():
        if args.suite and name not in args.suite:
            continue
        binary = output / name
        command = [cc, *flags]
        if name == 'vga-mapping':
            # The discarded x86 fault path uses the target's 32-bit addresses.
            command += ['-Wno-int-to-pointer-cast']
        command += [str(ROOT / 'src/vm' / source) for source in sources]
        command += ['-Wl,--gc-sections', '-o', str(binary)]
        subprocess.run(command, check=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True,
                                text=True, timeout=20, env={**os.environ,
                                'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                                'UBSAN_OPTIONS': 'halt_on_error=1'})
        results[name] = {'command': command, 'stdout': result.stdout,
                         'stderr': result.stderr, 'passed': True}
        print(name + ': passed', flush=True)
    (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
