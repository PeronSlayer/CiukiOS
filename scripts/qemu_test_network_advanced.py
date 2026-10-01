#!/usr/bin/env python3
"""QEMU gate for the graphical network profile, adapter and DHCP workflow."""
import argparse
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

from qemu_test_full_display_profile import VM
from qemu_test_native_windows import WindowVM

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)


def click(vm, x, y):
    vm.position(x, y)
    vm.hmp('mouse_button 1')
    time.sleep(.18)
    vm.hmp('mouse_button 0')
    time.sleep(.7)


def type_into(vm, x, y, value):
    click(vm, x, y)
    vm.key('ctrl-a')
    for c in value:
        vm.key({'-': 'minus', '.': 'dot'}.get(c, c))


def profile(disk):
    return subprocess.check_output(['mtype', '-i', str(disk), '::NET/MTCP.CFG'],
                                   text=True, errors='replace')


def static_run(image, out, checks):
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'disk.img'
    shutil.copyfile(image, disk)
    vm = WindowVM(disk, out, 'std', memory=128, palette='platinum')
    try:
        vm.ready()
        click(vm, 850, 16)
        vm.shot('network-before')

        before = vm.offset()
        click(vm, 616, 582)
        vm.wait('[DEVICES] tab devices', before, 15)
        vm.shot('adapter-in-device-manager')
        vm.key('alt-f4')
        time.sleep(.4)

        before = vm.offset()
        click(vm, 766, 582)
        vm.wait('[DEVICES] tab drivers', before, 15)
        vm.shot('network-drivers')
        vm.key('alt-f4')
        time.sleep(.4)

        type_into(vm, 605, 281, '255.0.255.0')
        before = vm.offset()
        click(vm, 792, 659)
        vm.wait('The subnet mask must have contiguous network bits.', before, 10)
        vm.shot('invalid-mask-rejected')
        vm.key('ret')
        time.sleep(.3)
        type_into(vm, 605, 281, '255.255.255.0')

        type_into(vm, 850, 362, '9999')
        before = vm.offset()
        click(vm, 792, 659)
        vm.wait('The Ethernet MTU must be from 576 to 1500.', before, 10)
        vm.shot('invalid-mtu-rejected')
        vm.key('ret')
        time.sleep(.3)

        type_into(vm, 615, 362, 'ciukilab')
        type_into(vm, 850, 362, '1400')
        before = vm.offset()
        click(vm, 792, 659)
        vm.wait('[CONTROL] network saved', before, 15)
        vm.shot('advanced-profile-saved')
    finally:
        vm.close()
    text = profile(disk).lower()
    for line in ('hostname ciukilab', 'hostname_assigned ciukilab', 'mtu 1400',
                 'ipaddr 10.0.2.15', 'netmask 255.255.255.0'):
        assert line in text, f'{line} missing from saved profile'
    assert 'mtu 9999' not in text, 'invalid MTU was saved'
    checks['static_and_adapter'] = 'passed'


def dhcp_run(image, out, checks):
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'disk.img'
    shutil.copyfile(image, disk)
    vm = WindowVM.__new__(WindowVM)
    vm.palette = 'platinum'
    vm.cursor_colors = ((36, 40, 48), (246, 246, 242))
    vm.control_latencies = []
    VM.__init__(vm, disk, out, qemu_args=[
        '-vga', 'std', '-netdev', 'user,id=ciuknet0',
        '-device', 'ne2k_pci,netdev=ciuknet0,mac=52:54:00:12:34:56',
    ], memory=128)
    try:
        vm.ready()
        before = vm.offset()
        click(vm, 850, 16)
        vm.wait('[CONTROL] network adapter Hardware: Realtek RTL8029 Ethernet', before, 15)
        vm.shot('adapter-active')
        before = vm.offset()
        click(vm, 868, 582)
        vm.wait('[DESKTOP] RUN NETCFG DHCP', before, 20)
        vm.wait('NETSTART: ICMP resident active', before, 45)
        vm.wait('NETCFG: DHCP lease saved', before, 100)
        vm.wait('[DESKTOP] READY', before, 40)
        time.sleep(1)
        before = vm.offset()
        click(vm, 850, 16)
        vm.wait('[CONTROL] applet Network', before, 15)
        time.sleep(5)
        vm.shot('lease-after-reopen')
        before = vm.offset()
        click(vm, 766, 582)
        vm.wait('[DEVICES] tab drivers', before, 15)
        before = vm.offset()
        click(vm, 722, 697)
        vm.wait('[DEVICES] driver disabled NE2000', before, 15)
        vm.shot('adapter-driver-disabled')
    finally:
        vm.close()
    text = profile(disk).lower()
    assert 'ipaddr 10.0.2.15' in text and 'lease_time ' in text, 'DHCP lease not saved'
    drivers = subprocess.check_output(['mtype', '-i', str(disk), '::DRIVERS/DRIVERS.CFG'],
                                      text=True, errors='replace')
    assert '0 NET NE2000 ' in drivers, 'disabled network driver was not saved'
    checks['dhcp_and_reopen'] = 'passed'
    reboot = out / 'reboot'
    reboot.mkdir(exist_ok=True)
    vm = VM(disk, reboot, qemu_args=[
        '-vga', 'std', '-netdev', 'user,id=ciuknet0',
        '-device', 'ne2k_pci,netdev=ciuknet0,mac=52:54:00:12:34:56',
    ], memory=128)
    try:
        vm.wait('[DESKTOP] READY', 0, 90)
    finally:
        vm.close()
    serial = subprocess.check_output([
        'scripts/serial_log_normalize.py', str(reboot / 'serial.log')],
        text=True, errors='replace')
    assert '[LOADDRV] NE2000 OK' not in serial, 'disabled driver loaded on reboot'
    checks['adapter_driver_disabled'] = 'saved and absent after reboot'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = {'host': 'QEMU', 'passed': False, 'checks': {}}
    try:
        static_run(a.image.resolve(), out / 'static', report['checks'])
        dhcp_run(a.image.resolve(), out / 'dhcp', report['checks'])
        report['passed'] = True
        print(f'[network-advanced] PASS {out}', flush=True)
    except Exception as e:
        report['error'] = repr(e)
        raise
    finally:
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
