#!/usr/bin/env python3
"""Boot a private CiukiOS disk and execute a real child under the Jemm monitor.

This gate does not qualify window presentation or original-game compatibility. It uses real
keyboard events and DOS execution, never injected guest memory or stopped CPUs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time
import re
import struct

from qemu_test_installed_hdd import FAT16, InstalledVM


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class MonitorVM(InstalledVM):
    def text(self, command):
        special = {' ': 'spc', '.': 'dot', '\\': 'backslash', '-': 'minus',
                   '/': 'slash', ':': 'shift-semicolon', '=': 'equal',
                   '!': 'shift-1'}
        for char in command:
            self.key('shift-' + char.lower() if char.isupper()
                     else special.get(char, char))
        self.key('ret')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--jemm', type=Path, required=True)
    parser.add_argument('--jload', type=Path, required=True)
    parser.add_argument('--module', type=Path)
    parser.add_argument('--framebuffer', action='store_true')
    parser.add_argument('--dpmi-probe', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.framebuffer and not args.module:
        parser.error('--framebuffer requires --module')
    if args.dpmi_probe and not args.module:
        parser.error('--dpmi-probe requires --module')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    disk = output / 'disk.img'
    before = sha(args.image)
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    probe = output / 'VMSESS.COM'
    binaries = [probe]
    subprocess.run(['nasm', '-f', 'bin', 'src/probes/vm/session.asm', '-o', str(probe)], check=True)
    for path, target in ((args.kernel, 'SYSTEM/CIUKIDOS.SYS'),
                         (args.jemm, 'JEMM386.EXE'), (args.jload, 'JLOAD.EXE'),
                         (probe, 'VMSESS.COM')):
        subprocess.run(['mcopy', '-o', '-i', volume, str(path), '::' + target], check=True)
    if args.module:
        probes = [('VMPARENT.COM', 'video_parent.asm'),
                  ('VMGUEST.COM', 'video_guest.asm'),
                  ('VMNEG.COM', 'video_negative.asm')]
        if args.framebuffer:
            probes.append(('VMFRAME.COM', 'framebuffer.asm'))
        for name, source in probes:
            binary = output / name
            binaries.append(binary)
            subprocess.run(['nasm', '-f', 'bin', 'src/probes/vm/' + source,
                            *(['-DVM_DPMI_PROBE=1'] if args.dpmi_probe and source == 'video_parent.asm' else []),
                            '-o', str(binary)], check=True)
            subprocess.run(['mcopy', '-o', '-i', volume, str(binary), '::' + name], check=True)
        subprocess.run(['mcopy', '-o', '-i', volume, str(args.module),
                        '::CVSESS.DLL'], check=True)
        if args.dpmi_probe:
            subprocess.run(['mcopy', '-o', '-i', volume, str(args.dpmi_probe),
                            '::DPMIVGA.EXE'], check=True)
    record = dict(passed=False, source_image_sha256=before,
                  kernel_sha256=sha(args.kernel), jemm_sha256=sha(args.jemm),
                  jload_sha256=sha(args.jload), probe_sha256=sha(probe),
                  cpu='pentium3', memory_mib=128, graphics_virtualized=False,
                  dpmi_virtualized=False, events=[])
    record['prepared_image_sha256'] = sha(disk)
    record['fixture_sha256'] = {binary.name: sha(binary) for binary in binaries}
    vm = MonitorVM(disk, output, memory=128)
    record['qemu_command'] = vm.process.args
    started = time.monotonic()

    def command(line, *expected):
        print('[vm-session] ' + line, flush=True)
        body = vm.result(line, *expected, timeout=40)
        record['events'].append(dict(command=line, output=body,
                                     elapsed=time.monotonic() - started))
        return body

    def observe(marker, name, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            dump = output / (name + '.bin')
            vm.hmp(f'pmemsave 0 0x100000 "{dump}"')
            data = dump.read_bytes()
            matches = [m.start() for m in re.finditer(re.escape(marker), data)]
            if len(matches) == 1:
                return matches[0], data
            time.sleep(.1)
        raise AssertionError('Guest did not reach observation point: ' + name)

    def memory(address, count, name):
        path = output / (name + '.bin')
        vm.hmp(f'pmemsave {address:#x} {count} "{path}"')
        return path.read_bytes()

    try:
        vm.wait('[DESKTOP] READY', timeout=90)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>')
        command('run \\VMSESS.COM', '[VMSESSION] No V86 monitor')
        command('run \\JEMM386.EXE LOAD NOEMS X=A000-FFFF NODYN MAX=32M MIN=32M NOVME')
        command('run \\VMSESS.COM', '[VMSESSION] PE=1 VCPI=present',
                '[VMSESSION] DOS create/write/read/close/delete and B800 PASS')
        command('comdemo', 'COM demo via INT21h')
        command('mzdemo', 'MZ demo via INT21h')
        command('dir \\SYSTEM', 'CIUKIDOS.SYS', 'SHELL.COM')
        vm.shot('monitored-console')
        if args.module:
            command('run \\JLOAD.EXE \\CVSESS.DLL')
            offset = vm.offset()
            vm.text('run \\VMPARENT.COM')
            at, data = observe(b'VMPH\x01\0\0\0', 'before-virtual-video')
            cr3, = struct.unpack_from('<I', data, at + 8 + 32)
            assert cr3 & 4095 == 0 and cr3 >= 0x100000
            pde, = struct.unpack('<I', memory(cr3, 4, 'low-page-directory'))
            assert pde & 1 and not pde & 128, 'Expected normal low page table'
            pt = (pde & 0xfffff000) + 0xa0 * 4
            saved_ptes = memory(pt, 128, 'vga-ptes-before')
            physical_before = memory(0xb8000, 0x8000, 'physical-video-before')
            vm.key('spc')
            at, data = observe(b'VMGS\x01\0', 'running-guest', timeout=45 if args.dpmi_probe else 15)
            record['guest_observation_address'] = at
            active_ptes = memory(pt, 128, 'vga-ptes-active')
            assert all((a & 0xfffff000) != (b & 0xfffff000) for a, b in
                       zip(struct.unpack('<32I', saved_ptes), struct.unpack('<32I', active_ptes))), 'Not every VGA page was shadowed'
            vm.shot('guest-running-host-video')
            assert physical_before == memory(0xb8000, 0x8000, 'physical-video-during'), 'Guest changed physical text surface'
            vm.key('spc')
            observe(b'VMPH\x02\0\0\0', 'after-virtual-video')
            assert saved_ptes == memory(pt, 128, 'vga-ptes-restored'), 'VGA page table was not restored exactly'
            record['exact_vga_pte_restore'] = True
            vm.key('spc')
            vm.wait('[VMVIDEO] Original DOS child, VGA shadow/readback and cleanup PASS', offset, 30)
            vm.wait('CiukiOS SHELL C:\\APPS>', offset, 30)
            record['video_shadow_passed'] = True
            record['module_sha256'] = sha(args.module)
            if args.dpmi_probe:
                record['dpmi_shadow_and_io_probe_passed'] = True
                record['dpmi_probe_sha256'] = sha(args.dpmi_probe)
                record['hdpmi_sha256'] = hashlib.sha256(FAT16(disk).read('SBEMU/HDPMI32I.EXE')).hexdigest()
            command('type \\VMGUEST.TXT', 'Ordinary DOS child: BIOS video, ports, VRAM and file output.')
            command('run \\VMNEG.COM', '[VMNEG] Lifecycle, rejected I/O and reset PASS')
            record['negative_session_lifecycle_passed'] = True
            if args.framebuffer:
                offset = vm.offset()
                vm.text('run \\VMFRAME.COM')
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    dump = output / 'framebuffer-guest.bin'
                    vm.hmp(f'pmemsave 0 0x100000 "{dump}"')
                    data = dump.read_bytes()
                    matches = [m.start() for m in re.finditer(b'VMFB\x01\x00\x00\x00', data)]
                    if len(matches) == 1:
                        break
                    time.sleep(.1)
                else:
                    raise AssertionError('Protected framebuffer fixture did not reach observation point')
                physical_base, = struct.unpack_from('<I', data, matches[0] + 8)
                physical = output / 'physical-lfb.bin'
                vm.hmp(f'pmemsave {physical_base:#x} 4096 "{physical}"')
                assert physical.read_bytes() == bytes.fromhex('11223344') * 1024
                vm.shot('protected-framebuffer-copy', (800, 600))
                vm.key('spc')
                vm.wait('[VMFRAME] Protected LFB copy, bounds and unbind PASS', offset, 30)
                vm.wait('CiukiOS SHELL C:\\APPS>', offset, 30)
                record['protected_framebuffer_copy_passed'] = True
                record['physical_framebuffer_base'] = hex(physical_base)
            command('run \\JLOAD.EXE /u \\CVSESS.DLL')
        command('run \\JEMM386.EXE UNLOAD', 'unloaded')
        command('run \\VMSESS.COM', '[VMSESSION] No V86 monitor')
        command('comdemo', 'COM demo via INT21h')
        offset = vm.offset()
        vm.text('exit')
        vm.wait('[DESKTOP] READY', offset, 60)
        vm.shot('desktop-restored')
        record['passed'] = True
        print('PASS actual V86 DOS child, COM/MZ, storage, unload and desktop return', flush=True)
    except Exception as error:
        record['error'] = repr(error)
        vm.shot('failure')
        record['registers'] = vm.hmp('info registers').decode(errors='replace')
        raise
    finally:
        vm.close()
        record['source_image_unchanged'] = sha(args.image) == before
        (output / 'report.json').write_text(json.dumps(record, indent=2) + '\n')
        assert record['source_image_unchanged']


if __name__ == '__main__':
    main()
