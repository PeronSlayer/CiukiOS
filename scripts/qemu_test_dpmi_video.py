#!/usr/bin/env python3
"""Actual packaged-HDPMI CPU-port trap probe on a private bootable image."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

from qemu_test_full_display_profile import VM
from qemu_test_installed_hdd import FAT16


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--probe', type=Path, default=Path('build/tests/dpmi-video/DPMIVGA.EXE'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'test.img'
    if disk.exists():
        raise SystemExit('Choose a fresh output directory to preserve previous evidence')
    source = args.image.resolve()
    source_hash = digest(source.read_bytes())
    shutil.copyfile(source, disk)
    fs = FAT16(disk)
    volume = f'{disk}@@{fs.start}'
    for file, target in ((args.kernel, 'SYSTEM/CIUKIDOS.SYS'), (args.probe, 'APPS/DPMIVGA.EXE')):
        subprocess.run(['mcopy', '-o', '-i', volume, str(file.resolve()), '::' + target], check=True)
    fs = FAT16(disk)
    report = {'result': 'RUNNING', 'scope': 'actual 32-bit CPU port exceptions under packaged HDPMI; no Jemm',
              'source_image_sha256': source_hash, 'prepared_image_sha256': digest(disk.read_bytes()),
              'kernel_sha256': digest(fs.read('SYSTEM/CIUKIDOS.SYS')),
              'hdpmi32i_sha256': digest(fs.read('SBEMU/HDPMI32I.EXE')),
              'probe_sha256': digest(fs.read('APPS/DPMIVGA.EXE')),
              'original_games_qualified': False, 'physical_hardware_qualified': False,
              'memory_mib': 128, 'commands': []}
    vm = VM(disk, out, memory=128)
    report['qemu_command'] = vm.process.args
    try:
        vm.wait('CiukiOS SHELL C:\\APPS>', timeout=90)
        command = 'run \\SBEMU\\HDPMI32I.EXE -r'
        report['commands'].append(command)
        vm.command(command, 'HDPMI32 now resident', timeout=60)
        off = vm.offset()
        command = 'run DPMIVGA.EXE'
        report['commands'].append(command)
        vm.text(command)
        vm.wait('[DPMIVGA] PASS ports isolated', off, 45)
        vm.wait('CiukiOS SHELL C:\\APPS>', off, 30)
        serial = subprocess.check_output(['scripts/serial_log_normalize.py', str(vm.serial)]).decode(errors='replace')
        assert '[DPMIVGA] FAIL' not in serial and '[DPMIVGA] INSTALL FAIL' not in serial, serial[-4000:]
        vm.shot('pass')
        report['commands'].append('run \\SBEMU\\HDPMI32I.EXE -u')
        vm.command(report['commands'][-1], 'HDPMI32 uninstalled', timeout=30)
        off = vm.offset()
        vm.text('exit')
        vm.wait('[DESKTOP] READY', off, 60)
        vm.key('f3')
        time.sleep(.3)
        vm.shot('desktop-return')
        report['result'] = 'PASS'
        report['desktop_return'] = True
    except Exception as exc:
        report['result'] = 'FAIL'
        report['error'] = str(exc)
        try:
            vm.shot('failure')
        except Exception:
            pass
        raise
    finally:
        vm.close()
        report['source_image_unchanged'] = digest(source.read_bytes()) == source_hash
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
