#!/usr/bin/env python3
"""Actual IN AL and XMS calls before/after pinned Jemm LOAD in private HDD copies.

The optional old binary must reproduce both the A20 input register corruption
and the XMS AH08 failure. Only the new binary may pass the qualified profile.
No guest memory writes, CPU pauses, or host-side model calls are used.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from qemu_test_vm_session import MonitorVM, FAT16


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def qualify(image, kernel, jemm, probe, output):
    output.mkdir(parents=True, exist_ok=False)
    disk = output / 'disk.img'
    shutil.copyfile(image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    for path, target in ((kernel, 'SYSTEM/CIUKIDOS.SYS'),
                         (jemm, 'JEMM386.EXE'), (probe, 'XMSJEMM.COM')):
        subprocess.run(['mcopy', '-o', '-i', volume, str(path), '::' + target], check=True)
    record = dict(passed=False, source_image_sha256=sha(image),
                  prepared_image_sha256=sha(disk), kernel_sha256=sha(kernel),
                  jemm_sha256=sha(jemm), probe_sha256=sha(probe),
                  cpu='pentium3', memory_mib=128, events=[])
    vm = MonitorVM(disk, output, memory=128)
    try:
        vm.wait('[DESKTOP] READY', timeout=90)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>')
        for command in (r'run \XMSJEMM.COM',
                        r'run \JEMM386.EXE LOAD NOEMS X=A000-FFFF NODYN MAX=32M MIN=32M NOVME',
                        r'run \XMSJEMM.COM'):
            text = vm.result(command, timeout=40)
            print(command + '\n' + text, flush=True)
            record['events'].append(dict(command=command, output=text))
        first, load, last = [event['output'] for event in record['events']]
        assert '[XMSJEMM] PASS' in first and '[XMSJEMM] FAIL' not in first
        assert 'Jemm386 loaded' in load
        record['baseline_passed'] = True
        record['a20_high24_corruption'] = '[XMSJEMM] FAIL IN AL,92h changed EAX high24' in last
        record['legacy_query_failure'] = '[XMSJEMM] FAIL AH08 free memory' in last
        record['passed'] = ('[XMSJEMM] PASS' in last and '[XMSJEMM] FAIL' not in last
                            and '[XMSJEMM] AH08/AH88 query PASS' in last
                            and '[XMSJEMM] Allocate/lock/unlock/free/accounting PASS' in last)
        vm.shot('xms-result')
    except Exception as error:
        record['error'] = repr(error)
    finally:
        vm.close()
        record['source_image_unchanged'] = sha(image) == record['source_image_sha256']
        if not record['source_image_unchanged']:
            record['passed'] = False
        (output / 'report.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--jemm', required=True, type=Path)
    parser.add_argument('--old-jemm', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    probe = output / 'XMSJEMM.COM'
    subprocess.run(['nasm', '-f', 'bin', 'src/probes/vm/xms_jemm.asm', '-o', str(probe)], check=True)
    summary = {}
    if args.old_jemm:
        old = qualify(args.image, args.kernel, args.old_jemm, probe, output / 'old')
        summary['old_regression_observed'] = (not old['passed'] and old.get('baseline_passed', False)
            and old.get('a20_high24_corruption', False) and old.get('legacy_query_failure', False)
            and old['source_image_unchanged'])
    new = qualify(args.image, args.kernel, args.jemm, probe, output / 'new')
    summary['new_passed'] = new['passed']
    summary['passed'] = new['passed'] and summary.get('old_regression_observed', True)
    (output / 'report.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
