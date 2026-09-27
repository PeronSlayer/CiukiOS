#!/usr/bin/env python3
"""Qualify the owned Jemm/official-HX-HDP324 foreground-session lifecycle."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time

from qemu_test_installed_hdd import FAT16, InstalledVM


MAGIC = b'CVSC'
STATE_BOUND = 1
STATE_SESSION = 2
STATE_JEMM = 4
STATE_HOST = 8
STATE_CLIENT = 0x10
STATE_CALLBACK = 0x20
STATE_LIVE = (STATE_BOUND | STATE_SESSION | STATE_JEMM | STATE_HOST |
              STATE_CLIENT | STATE_CALLBACK)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class LifetimeVM(InstalledVM):
    def text(self, command):
        special = {' ': 'spc', '.': 'dot', '\\': 'backslash', '-': 'minus',
                   '/': 'slash', ':': 'shift-semicolon', '=': 'equal'}
        for char in command:
            self.key('shift-' + char.lower() if char.isupper()
                     else special.get(char, char))
        self.key('ret')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, required=True)
    ap.add_argument('--kernel', type=Path,
                    help='Optional candidate CIUKIDOS.SYS; otherwise keep the image kernel')
    ap.add_argument('--jemm', type=Path, required=True)
    ap.add_argument('--jload', type=Path, required=True)
    ap.add_argument('--module', type=Path, required=True)
    ap.add_argument('--hdpmi', type=Path, required=True)
    ap.add_argument('--life', type=Path, required=True)
    ap.add_argument('--fault', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    disk = output/'disk.img'
    source_hash = sha(args.image)
    shutil.copyfile(args.image, disk)
    source_fat = FAT16(disk)
    doom = source_fat.read('APPS/DOOMVAN/PCDMCORE.EXE')
    assert len(doom) > 500000 and doom[:2] == b'MZ'
    doom_wad = source_fat.read('APPS/DOOMVAN/DOOM.WAD')
    assert len(doom_wad) > 1000000
    # The kernel's direct EXEC path accepts root files but does not resolve a
    # nested absolute path from this parent.  Stage a byte-identical executable
    # at the root and its IWAD in the parent's APPS working directory, so the
    # test still executes the shipped original extender without changing it.
    staged_doom = output/'DPMIDOOM.EXE'
    staged_wad = output/'DPMIDOOM.WAD'
    staged_doom.write_bytes(doom)
    staged_wad.write_bytes(doom_wad)
    volume = f'{disk}@@{source_fat.start}'
    parent = output/'DPMIPAR.COM'
    short_demo = output/'CVTEST.LMP'
    subprocess.run(['nasm', '-f', 'bin', 'src/probes/vm/dpmi_lifetime_parent.asm',
                    '-o', str(parent)], check=True)
    subprocess.run(['nasm', '-f', 'bin', 'src/probes/vm/dpmi_short_demo.asm',
                    '-o', str(short_demo)], check=True)
    payloads = ((args.jemm, 'JEMM386.EXE'), (args.jload, 'JLOAD.EXE'),
                (args.module, 'CVSESS.DLL'), (args.hdpmi, 'HDPMI32.EXE'),
                (args.life, 'DPMILIF.EXE'), (args.fault, 'DPMIFLT.EXE'),
                (staged_doom, 'DPMIDOOM.EXE'), (staged_wad, 'APPS/DOOM.WAD'),
                (short_demo, 'APPS/CVTEST.LMP'), (parent, 'DPMIPAR.COM'))
    if args.kernel:
        payloads = ((args.kernel, 'SYSTEM/CIUKIDOS.SYS'),) + payloads
    for source, target in payloads:
        subprocess.run(['mcopy', '-o', '-i', volume, str(source), '::'+target],
                       check=True)

    report = {'passed': False, 'source_image_sha256': source_hash,
              'original_client': {'path': 'APPS/DOOMVAN/PCDMCORE.EXE',
                                  'staged_path': 'DPMIDOOM.EXE',
                                  'bytes': len(doom),
                                  'sha256': hashlib.sha256(doom).hexdigest()},
              'fixtures': {Path(source).name: sha(Path(source))
                           for source, _ in payloads},
              'events': []}
    vm = LifetimeVM(disk, output, memory=128)
    report['qemu_command'] = vm.process.args

    def command(text, *expected, timeout=60):
        body = vm.result(text, *expected, timeout=timeout)
        report['events'].append({'command': text, 'output': body})
        return body

    def memory(address, count, name):
        path = output/(name+'.bin')
        vm.hmp(f'pmemsave {address:#x} {count:#x} "{path}"')
        return path.read_bytes()

    def first_meg(name):
        return memory(0, 0x100000, name)

    def descriptor(data=None):
        if data is None:
            data = first_meg('descriptor-scan')
        matches = []
        start = 0
        while True:
            at = data.find(MAGIC, start)
            if at < 0:
                break
            if at + 256 <= len(data):
                version, size = struct.unpack_from('<HH', data, at+4)
                state, generation = struct.unpack_from('<II', data, at+12)[0], \
                                    struct.unpack_from('<I', data, at+8)[0]
                if version == 0x100 and size == 256 and generation and state:
                    matches.append(at)
            start = at + 1
        assert len(matches) == 1, [hex(value) for value in matches]
        at = matches[0]
        return at, data[at:at+256]

    def desc_at(address, name):
        return memory(address, 256, name)

    def dword(data, offset):
        return struct.unpack_from('<I', data, offset)[0]

    def shared_video_snapshot(address, low, name):
        packet = low[address+256:address+256+296]
        if len(packet) < 12 or packet[:4] != b'CVSH':
            return {'packet': packet.hex(), 'error': 'VIDEO_SHARE packet missing'}
        pages = dword(packet, 4)
        physical = dword(packet, 8)
        header = memory(physical, 4096, name+'-shared-header')
        return {'pages': pages, 'first_physical': hex(physical),
                'magic': header[:4].decode('ascii', errors='replace'),
                'version': hex(dword(header, 4)), 'bytes': dword(header, 8),
                'generation': dword(header, 12), 'fatal': dword(header, 28),
                'pm_faults': dword(header, 32),
                'pm_instructions': dword(header, 36),
                'pm_elements': dword(header, 40),
                'pm_unsupported': dword(header, 44),
                'pm_port_reads': dword(header, 48),
                'pm_port_writes': dword(header, 52),
                'pm_attached': dword(header, 56)}

    def page_table(cr3, name):
        assert cr3 >= 0x100000 and cr3 & 0xfff == 0, hex(cr3)
        pde, = struct.unpack('<I', memory(cr3, 4, name+'-pde'))
        assert pde & 1 and not pde & 0x80, hex(pde)
        base = pde & 0xfffff000
        return base, memory(base + 0xa0*4, 32*4, name+'-vga-ptes')

    def virtual_memory(cr3, address, count, name):
        result = bytearray()
        while count:
            pde = dword(memory(cr3 + ((address >> 22) & 0x3ff) * 4,
                               4, name+'-pde'), 0)
            assert pde & 1 and not pde & 0x80, hex(pde)
            pte = dword(memory((pde & 0xfffff000) +
                               ((address >> 12) & 0x3ff) * 4,
                               4, name+'-pte'), 0)
            assert pte & 1, hex(pte)
            take = min(count, 0x1000 - (address & 0xfff))
            result += memory((pte & 0xfffff000) + (address & 0xfff),
                             take, name+'-data')
            address += take
            count -= take
        return bytes(result)

    def wait_phase(address, phase, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            low = first_meg(f'phase-{phase}')
            marker = low.find(b'CVLP')
            if marker >= 0 and low[marker+4] == phase:
                return desc_at(address, f'descriptor-phase-{phase}')
            time.sleep(.1)
        raise AssertionError(f'parent did not reach phase {phase}')

    def wait_low_phase(phase, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            low = first_meg(f'pre-phase-{phase}')
            marker = low.find(b'CVLP')
            if marker >= 0 and low[marker+4] == phase:
                return low
            time.sleep(.1)
        raise AssertionError(f'parent did not reach pre-session phase {phase}')

    try:
        vm.wait('[DESKTOP] READY', timeout=90)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>')
        command('run \\JEMM386.EXE LOAD NOEMS X=A000-FFFF NODYN MAX=32M MIN=32M NOVME')
        command('run \\JLOAD.EXE \\CVSESS.DLL')
        vm.text('run \\DPMIPAR.COM')
        low = wait_low_phase(0x10)
        packets = [match.start() for match in re.finditer(b'CVMS', low)
                   if match.start()+64 <= len(low) and
                   struct.unpack_from('<HH', low, match.start()+4) == (0x100, 64)]
        assert len(packets) == 1, [hex(value) for value in packets]
        owner_cr3 = dword(low[packets[0]:packets[0]+64], 32)
        owner_pt, original_ptes = page_table(owner_cr3, 'owner-before')
        physical_vga = memory(0xa0000, 0x20000, 'physical-vga-before')

        cursor = vm.offset()
        vm.key('spc')
        low = wait_low_phase(1, 10)
        address, begun = descriptor()
        assert dword(begun, 12) == (STATE_BOUND | STATE_SESSION | STATE_JEMM), \
               f'host appeared before the post-BEGIN creation boundary: {dword(begun, 12):#x}'
        report['post_begin_pre_host'] = {'descriptor_physical': hex(address),
                                         'state': hex(dword(begun, 12))}
        vm.key('spc')
        vm.wait('[DPMILIFE] BIOS WAIT ENTER', cursor, 90)
        address, live = descriptor()
        assert dword(live, 12) == STATE_LIVE
        assert dword(live, 40) != 0
        assert dword(live, 44) == 1 and dword(live, 48) == 0
        assert dword(live, 28) >= 3, 'physical HDPMI ticks did not survive CLI'
        assert dword(live, 244) == 0x3da, 'actual VGA status callback was not observed'
        expected_ptes = live[96:224]
        active_owner = memory(owner_pt + 0xa0*4, 128, 'owner-active-ptes')
        assert active_owner == expected_ptes
        hdpmi_cr3 = dword(live, 248)
        assert hdpmi_cr3 and hdpmi_cr3 != owner_cr3
        _, hdpmi_ptes = page_table(hdpmi_cr3, 'hdpmi-active')
        assert hdpmi_ptes == expected_ptes, 'separate HDPMI page table lost shadow'
        v86_before = dword(live, 24)
        time.sleep(.35)
        during = desc_at(address, 'bios-wait-service')
        assert dword(during, 24) > v86_before, 'blocked BIOS wait starved host service'
        low = first_meg('first-client-shared')
        video = shared_video_snapshot(address, low, 'first-client-video')
        assert video['pm_attached'] == 1
        assert video['pm_faults'] and video['pm_instructions']
        assert video['pm_port_reads'] >= 3 and video['pm_port_writes'] >= 1
        report['first_client'] = {'descriptor_physical': hex(address),
                                  'jemm_cr3': hex(owner_cr3),
                                  'hdpmi_cr3': hex(hdpmi_cr3),
                                  'dpmi_ticks_after_cli': dword(during, 28),
                                  'v86_ticks_during_wait': dword(during, 24)-v86_before,
                                  'protected_video': video}
        cursor = vm.offset()
        vm.key('spc')
        vm.wait('[DPMILIFE] BIOS WAIT ENTER', cursor, 90)
        second = desc_at(address, 'second-client-live')
        assert dword(second, 12) == STATE_LIVE
        assert dword(second, 44) == 2 and dword(second, 48) == 1
        v86_before = dword(second, 24)
        time.sleep(.35)
        assert dword(desc_at(address, 'second-bios-wait'), 24) > v86_before
        cursor = vm.offset()
        vm.key('spc')
        vm.wait('[DPMIFAULT] UD2 ENTER', cursor, 90)
        faulted = wait_phase(address, 5, 60)
        assert dword(faulted, 40) == 0
        assert dword(faulted, 44) == 3 and dword(faulted, 48) == 3
        assert dword(faulted, 52) == 3 and dword(faulted, 56) == 3
        assert dword(faulted, 60) == 1
        before_original = shared_video_snapshot(
            address, first_meg('before-original-low'), 'before-original-video')
        video_physical = int(before_original['first_physical'], 16)
        cursor = vm.offset()
        vm.key('spc')
        deadline = time.monotonic() + 90
        attempt = 0
        while True:
            original = desc_at(address, f'original-live-{attempt}')
            header = memory(video_physical, 4096,
                            f'original-video-{attempt}')
            live_work = (dword(original, 12) == STATE_LIVE and
                         dword(original, 44) == 4 and
                         dword(original, 48) == 3 and
                         dword(original, 52) == 4 and
                         dword(original, 56) == 3 and
                         dword(header, 56) == 1 and
                         dword(header, 36) - before_original['pm_instructions'] >= 10000 and
                         dword(header, 48) - before_original['pm_port_reads'] >= 50 and
                         dword(header, 52) - before_original['pm_port_writes'] >= 500)
            if live_work:
                break
            assert time.monotonic() < deadline, \
                   'original extender did not reach bounded real VGA work'
            attempt += 1
            time.sleep(.5)
        original_cr3 = dword(original, 248)
        assert original_cr3 and original_cr3 != owner_cr3
        _, original_client_ptes = page_table(original_cr3,
                                              'original-client-active')
        assert original_client_ptes == expected_ptes
        assert memory(owner_pt + 0xa0*4, 128,
                      'owner-original-active-ptes') == expected_ptes
        report['original_client_runtime'] = {
            'state': hex(dword(original, 12)),
            'jemm_cr3': hex(owner_cr3),
            'hdpmi_cr3': hex(original_cr3),
            'pm_instructions': dword(header, 36) - before_original['pm_instructions'],
            'pm_port_reads': dword(header, 48) - before_original['pm_port_reads'],
            'pm_port_writes': dword(header, 52) - before_original['pm_port_writes'],
            'dpmi_ticks': dword(original, 28),
        }
        vm.shot('original-extender-running')
        vm.wait('[DPMILIFE] PASS repeated/fault/original client, owned removal, full unwind',
                cursor, 300)
        final = desc_at(address, 'descriptor-final')
        assert dword(final, 12) == 0 and dword(final, 40) == 0
        assert dword(final, 44) == dword(final, 48) == 5
        assert dword(final, 52) == dword(final, 56) == 5
        assert dword(final, 60) == 1
        assert dword(final, 64) > 0
        assert dword(final, 72) == 0x20000
        assert dword(final, 76) >= 4096 + 256
        assert dword(final, 80) and dword(final, 84)
        assert memory(owner_pt + 0xa0*4, 128, 'owner-restored-ptes') == original_ptes
        assert memory(0xa0000, 0x20000, 'physical-vga-after') == physical_vga
        report['final'] = {'installs': 5, 'removes': 5, 'entries': 5,
                           'exits': 5, 'fault_exits': 1,
                           'state': hex(dword(final, 12)),
                           'dpmi_handle': hex(dword(final, 40)),
                           'pte_checks': dword(final, 64),
                           'pte_repairs': dword(final, 68),
                           'v86_ticks': dword(final, 24),
                           'dpmi_ticks': dword(final, 28),
                           'scheduler_bytes': dword(final, 76),
                           'adapter_bytes': dword(final, 80),
                           'callback_bytes': dword(final, 84),
                           'jemm_ptes_restored': True,
                           'physical_vga_restored': True}
        report['passed'] = True
        print('PASS official HDPMI 3.24 original clients, physical scheduling and exact ownership unwind')
    except Exception as error:
        report['error'] = repr(error)
        report['registers'] = vm.hmp('info registers').decode(errors='replace')
        try:
            low = first_meg('failure-low')
            address, failed = descriptor(low)
            report['failure_descriptor'] = {
                'physical': hex(address), 'state': hex(dword(failed, 12)),
                'last_error': dword(failed, 20), 'failures': dword(failed, 92)}
            last_eip = dword(failed, 236)
            if last_eip >= 16:
                client_cr3 = dword(failed, 248)
                report['failure_io_code'] = {
                    'eip': hex(last_eip),
                    'cr3': hex(client_cr3),
                    'bytes': virtual_memory(client_cr3, last_eip - 16, 64,
                                            'failure-io-code').hex()}
            report['failure_video'] = shared_video_snapshot(
                address, low, 'failure-video')
        except Exception as diagnostic_error:       # pragma: no cover
            report['failure_diagnostic_error'] = repr(diagnostic_error)
        vm.shot('failure')
        raise
    finally:
        vm.close()
        report['source_image_unchanged'] = sha(args.image) == source_hash
        (output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
        assert report['source_image_unchanged']


if __name__ == '__main__':
    main()
