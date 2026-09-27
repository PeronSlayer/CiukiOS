#!/usr/bin/env python3
"""Measure real Jemm host/guest ticks across a V86 CLI busy loop.

--expect-starved is a counterexample gate; passing that mode proves the old
invariant false, not that the runtime is correct.  No host memory injection,
CPU pause, HLT or yielding guest calls make the measured ticks progress.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from qemu_test_vm_session import MonitorVM, FAT16


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'kernel', 'jemm', 'jload', 'module', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--expect-starved', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    disk = output / 'disk.img'
    source_hash = sha(args.image)
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    probe = output / 'V86CLI.COM'
    subprocess.run(['nasm', '-f', 'bin',
                    *([] if args.expect_starved else ['-DVM_CLI_SEMANTICS=1']),
                    'src/probes/vm/v86_cli.asm',
                    '-o', str(probe)], check=True)
    payloads = ((args.kernel, 'SYSTEM/CIUKIDOS.SYS'),
                (args.jemm, 'JEMM386.EXE'), (args.jload, 'JLOAD.EXE'),
                (args.module, 'CVSESS.DLL'), (probe, 'V86CLI.COM'))
    for source, target in payloads:
        subprocess.run(['mcopy', '-o', '-i', volume, str(source),
                        '::' + target], check=True)
    record = dict(passed=False, mode=('counterexample' if args.expect_starved
                                     else 'scheduler-invariant'),
                  source_image_sha256=source_hash,
                  fixtures={target: sha(source) for source, target in payloads},
                  events=[])
    vm = MonitorVM(disk, output, memory=128)
    record['qemu_command'] = vm.process.args

    def command(text, *expected):
        if text == r'run \V86CLI.COM' and not args.expect_starved:
            offset = vm.offset()
            vm.text(text)
            vm.wait('[V86CLI] BIOS KEYBOARD READY', offset, 30)
            vm.key('x')
            vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
            raw = subprocess.check_output(['scripts/serial_log_normalize.py',
                                            '--offset', str(offset), str(vm.serial)])
            body = raw.partition(b'\r\n')[2].decode('cp437')
            for marker in expected:
                assert marker in body, (text, marker, body)
        else:
            body = vm.result(text, *expected, timeout=60)
        record['events'].append(dict(command=text, output=body))
        print(text + '\n' + body, flush=True)
        return body

    try:
        vm.wait('[DESKTOP] READY', timeout=90)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>')
        command(r'run \JEMM386.EXE LOAD NOEMS X=A000-FFFF NODYN MAX=32M MIN=32M NOVME',
                'Jemm386 loaded')
        command(r'run \JLOAD.EXE \CVSESS.DLL', 'loaded successfully')
        body = command(r'run \V86CLI.COM', '[V86CLI] COUNTERS')
        if not args.expect_starved:
            assert '[V86CLI] FLAGS/INT/IRET/stack/STI/PIC/EOI/HLT PASS' in body
            negotiated = re.search(r'PROFILE NEGOTIATED ALLOW/DECLINE/RELEASE PASS flags=([0-9A-F]{8})', body)
            assert negotiated, 'virtual-IF profile was not negotiated at run time'
            record['negotiated_profile_flags'] = negotiated.group(1)
        match = re.search(r'\[V86CLI\] COUNTERS ((?:[0-9A-F]{8} ){12})', body)
        assert match, 'missing complete guest observations'
        values = [int(value, 16) for value in match.group(1).split()]
        pairs = list(zip(values[::2], values[1::2]))
        deltas = [[(pairs[second][field] - pairs[first][field]) & 0xffffffff
                   for field in range(2)] for first, second in ((0, 1), (2, 3), (4, 5))]
        record['samples_host_guest_ticks'] = pairs
        record['deltas_before_during_after_cli'] = deltas
        assert deltas[0][0] >= 3 and deltas[0][1] >= 3, 'no baseline timer service'
        assert deltas[2][0] >= 3 and deltas[2][1] >= 3, 'no resumed timer service'
        assert deltas[1][1] == 0, 'guest IRQ0 leaked through virtual CLI'
        if args.expect_starved:
            assert deltas[1][0] == 0, 'expected counterexample did not reproduce'
            record['host_starvation_reproduced'] = True
        else:
            assert deltas[1][0] >= 3, 'host scheduler starved under guest CLI'
        command(r'run \JLOAD.EXE -u \CVSESS.DLL', 'unloaded successfully')
        command(r'run \JEMM386.EXE UNLOAD', 'Jemm unloaded')
        command('comdemo', 'COM demo via INT21h')
        record['passed'] = True
    except Exception as error:
        record['error'] = repr(error)
        try:
            record['registers_on_failure'] = vm.hmp('info registers').decode('utf-8', errors='replace')
            record['pic_on_failure'] = vm.hmp('info pic').decode('utf-8', errors='replace')
        except Exception as diagnostic_error:
            record['diagnostic_error'] = repr(diagnostic_error)
    finally:
        vm.close()
        record['source_image_unchanged'] = sha(args.image) == source_hash
        record['passed'] &= record['source_image_unchanged']
        (output / 'report.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
