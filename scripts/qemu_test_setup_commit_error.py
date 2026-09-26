#!/usr/bin/env python3
"""Inject a real backend flush error after Setup publishes its bootable MBR.

The guest runs the ISO's unchanged installer and ATA driver. blkdebug fails
the third target flush: invalidation, payload, then final MBR. The host inspects
the resulting disk after the guest displays its error page.
"""
import argparse
import json
from pathlib import Path

from qemu_test_setup_graphical import SetupVM, fill_disk


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--iso', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--expect-vulnerable', action='store_true')
    ap.add_argument('--fault', choices=('commit-flush', 'post-flush-read'), default='commit-flush')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'target.img'
    if disk.exists():
        ap.error('use a fresh output directory')
    fill_disk(disk, 128)
    config = out / 'flush-error.conf'
    config.write_text('''[set-state]
event = "flush_to_disk"
state = "1"
new_state = "2"

[set-state]
event = "flush_to_disk"
state = "2"
new_state = "3"

[inject-error]
event = "flush_to_disk"
state = "3"
errno = "5"
once = "on"
immediately = "on"
''')
    if args.fault == 'post-flush-read':
        config.write_text(config.read_text().replace(
            '[inject-error]\nevent = "flush_to_disk"',
            '[inject-error]\nevent = "read_aio"\nsector = "1000"').replace(
            'once = "on"', 'once = "off"'))
    report = {'completed': False, 'fault': args.fault}
    vm = SetupVM([(0, f'blkdebug:{config}:{disk}')], args.iso.resolve(), out)
    try:
        vm.boot_menu(1)
        vm.wait('[BOOT-SESSION] SETUP', timeout=90)
        vm.page(0)
        for page in (1, 2, 3):
            vm.key('ret')
            vm.page(page)
        vm.key('spc')
        vm.key('ret')
        vm.page(4)
        vm.page(6, timeout=1800)
        vm.shot('commit-error', (800, 600))
        serial = vm.serial.read_text(errors='replace')
        assert '[SETUP-HDD-INSTALL] COPY-DONE' in serial, 'error occurred before commit'
        if args.fault == 'commit-flush':
            assert '[SETUP-HDD-INSTALL] FAIL S=F' in serial, 'expected cache-flush error'
        else:
            assert '[SETUP-VERIFY] BIOS READ AFTER FLUSH' in serial
            assert '[SETUP-HDD-INSTALL] FAIL S=R' in serial, 'expected final BIOS read error'
        assert '[SETUP-GUI] PAGE 05' not in serial, 'installer announced success'
        report['guest_error_page'] = True
    finally:
        vm.close()
        with disk.open('rb') as stream:
            mbr = stream.read(512)
        report['boot_signature'] = mbr[510:].hex()
        report['active_partition'] = mbr[446] == 0x80
        report['mbr_zero'] = mbr == bytes(512)
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    if args.expect_vulnerable:
        assert report['boot_signature'] == '55aa' and report['active_partition'], report
        report['reproduced'] = 'failed install left a bootable MBR'
    elif args.fault == 'commit-flush':
        assert report['mbr_zero'], report
        assert '[SETUP-COMMIT] ROLLBACK VERIFIED' in serial
    else:
        # The copy contains the source boot code/partition table with its
        # signature withheld. A failed readback must never publish 55AA.
        assert report['boot_signature'] == '0000', report
    report['completed'] = True
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
