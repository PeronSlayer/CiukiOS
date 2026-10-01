#!/usr/bin/env python3
"""Boot the installed driver catalog against QEMU PCI NICs and fault fixtures."""
import argparse
import json
import re
import shutil
import subprocess
import time
from pathlib import Path


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def boot(image, directory, device=None, timeout=110, prepare=None):
    directory.mkdir(parents=True, exist_ok=True)
    disk = directory / 'disk.img'
    shutil.copyfile(image, disk)
    if prepare:
        prepare(disk, directory)
    serial = directory / 'serial.log'
    args = ['qemu-system-i386', '-accel', 'kvm', '-machine', 'pc,vmport=off,i8042=on',
            '-cpu', 'pentium3', '-m', '128', '-drive', f'file={disk},format=raw,if=ide',
            '-boot', 'c', '-display', 'none', '-serial', f'file:{serial}',
            '-monitor', 'none', '-no-reboot', '-no-shutdown']
    if device:
        args += ['-netdev', 'user,id=n0', '-device', f'{device},netdev=n0']
    else:
        args += ['-nic', 'none']
    with (directory / 'qemu.err').open('w') as err:
        vm = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=err)
        deadline = time.monotonic() + timeout
        try:
            while time.monotonic() < deadline:
                data = serial.read_bytes() if serial.exists() else b''
                if b'[DESKTOP] READY' in data:
                    break
                if vm.poll() is not None:
                    raise AssertionError(f'QEMU exited before desktop: {vm.returncode}')
                time.sleep(.25)
            else:
                raise AssertionError('desktop did not become ready')
            time.sleep(1)
        finally:
            vm.kill()
            vm.wait()
    text = serial.read_bytes().decode('cp437', 'replace')
    log = run('mtype', '-i', str(disk), '::DRIVERS/LOADDRV.LOG',
              capture_output=True, text=True).stdout
    (directory / 'loader.log').write_text(log)
    return text, log


def fault_fixtures(disk, directory):
    source = Path('scripts/driver_pack_fixtures')
    run('mmd', '-i', str(disk), '::DRIVERS/TEST')
    run('mmd', '-i', str(disk), '::DRIVERS/TEST/FIX')
    for name in ('hang', 'badhook', 'vecprobe'):
        target = directory / (name.upper() + '.COM')
        run('nasm', '-f', 'bin', str(source / (name + '.asm')), '-o', str(target))
        run('mcopy', '-o', '-i', str(disk), str(target),
            '::DRIVERS/TEST/FIX/' + target.name)
    cfg = run('mtype', '-i', str(disk), '::DRIVERS/DRIVERS.CFG',
              capture_output=True).stdout
    suffix = ('1 TEST PROBE1 \\DRIVERS\\TEST\\FIX\\VECPROBE.COM\r\n'
              '1 TEST BADHOOK \\DRIVERS\\TEST\\FIX\\BADHOOK.COM\r\n'
              '1 TEST PROBE2 \\DRIVERS\\TEST\\FIX\\VECPROBE.COM\r\n'
              '1 TEST HANG \\DRIVERS\\TEST\\FIX\\HANG.COM\r\n'
              '1 TEST PROBE3 \\DRIVERS\\TEST\\FIX\\VECPROBE.COM\r\n')
    path = directory / 'DRIVERS.CFG'
    path.write_bytes(cfg.rstrip(b'\r\n') + b'\r\n' + suffix.encode('ascii'))
    run('mcopy', '-o', '-i', str(disk), str(path), '::DRIVERS/DRIVERS.CFG')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'checks': {}, 'physical_hardware_qualified': False}
    active = ('NE2000', 'PCNET', 'RTL8139')
    try:
        for device, expected in [(None, None), ('ne2k_pci', 'NE2000'),
                                 ('pcnet', 'PCNET'), ('rtl8139', 'RTL8139'),
                                 ('e1000', None)]:
            label = device or 'no-nic'
            serial, log = boot(a.image, a.output / label, device)
            assert '[DESKTOP] READY' in serial
            for name in active:
                result = 'OK' if name == expected else 'SKIP'
                assert f'{name} {result}' in log, (label, name, result, log)
            assert 'E1000 OK' not in log and 'E1000 ERROR' not in log, (label, log)
            if expected:
                line = next(line for line in log.splitlines()
                            if line.startswith(expected + ' OK'))
                assert 'INT 0x' in line and ' MAC ' in line, (label, log)
            report['checks'][label] = 'passed'
            print(f'[driver-pack] {label}: {expected or "no driver"} verified', flush=True)
        serial, log = boot(a.image, a.output / 'fault-fixtures', timeout=155,
                           prepare=fault_fixtures)
        assert 'BADHOOK ERROR' in log and 'HANG ERROR' in log, log
        assert 'timed out' in log, log
        assert all(f'PROBE{i} OK' in log for i in (1, 2, 3)), log
        vectors = re.findall(r'^\[VECPROBE\].*$', serial, re.M)
        assert len(vectors) == 3 and vectors[0] == vectors[1] == vectors[2], vectors
        report['checks']['fault-fixtures'] = 'passed'
        print('[driver-pack] failure and watchdog restoration verified', flush=True)
        report['passed'] = True
    except Exception as error:
        report['error'] = repr(error)
        raise
    finally:
        (a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
