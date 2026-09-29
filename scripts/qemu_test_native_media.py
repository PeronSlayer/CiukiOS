#!/usr/bin/env python3
"""Removable media in Files (\\SYSTEM\\APPS\\FILES.APP) with real QEMU input.

For the floppy, a USB BIOS disk and a CD-ROM (read-only fixtures), Files'
Go menu opens the device through MEDIA.DRV. The test browses NESTED/DEEP,
previews README.TXT, copies PAYLOAD.BIN and pastes it into C:\\MEDIAn (an
import). The imported bytes are checked after QEMU shuts down; the source
images must be unchanged. Last, an ejected floppy reports an error. Files
logs its state on COM1 ("[FILES] ..."). QEMU evidence only.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_test_full_display_profile import VM                 # noqa: E402
from qemu_test_installed_hdd import FAT16                      # noqa: E402
from qemu_test_native_windows import WindowVM                  # noqa: E402

DEVICES = (('Floppy', 1, 'p'), ('USB BIOS disk', 2, 'd'), ('CD-ROM', 3, 'c'))


class MediaVM(WindowVM):
    def __init__(self, disk, output, fixtures):
        output.mkdir(parents=True, exist_ok=True)
        self.palette = 'platinum'
        self.control_latencies = []
        self.cursor_colors = ((36, 40, 48), (246, 246, 242))
        VM.__init__(self, disk, output, memory=128, qemu_args=[
            '-vga', 'std', '-drive', f'file={fixtures}/fat12.img,format=raw,if=floppy,index=0,readonly=on',
            '-drive', f'file={fixtures}/data.iso,format=raw,if=ide,index=2,media=cdrom,readonly=on',
            '-usb', '-drive', f'file={fixtures}/fat16.img,format=raw,if=none,id=usbmedia,readonly=on',
            '-device', 'usb-storage,drive=usbmedia'])


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    ap.add_argument('--fixtures', type=Path, default=Path('build/full/native-media-2026-09-26/driver-tests/fixtures'))
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    disk = out / 'test.img'
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    subprocess.run(['mmd', '-i', volume, '::MEDIA1', '::MEDIA2', '::MEDIA3'], check=True)
    sources = [args.fixtures / n for n in ('fat12.img', 'fat16.img', 'data.iso')]
    hashes = {str(p): digest(p) for p in sources}
    report = {'passed': False, 'physical_hardware_qualified': False, 'checks': []}
    vm = MediaVM(disk, out, args.fixtures.resolve())

    def keys(*names, marker=(), timeout=60):
        offset = vm.offset()
        for name in names:
            vm.key(name)
        for m in ([marker] if isinstance(marker, str) else marker):
            vm.wait(m, offset, timeout)

    def path(text):
        out_keys = []
        for ch in text:
            out_keys.append({'\\': 'backslash', ':': 'shift-semicolon'}.get(ch, ch.lower()))
        return out_keys
    try:
        vm.ready()
        keys('meta_l-e', marker='[FILES] list C:\\')
        for kind, device, letter in DEVICES:
            keys('alt-g', letter, marker=f'[FILES] list media:{device}:/ 2')
            keys('n', 'ret', marker=f'[FILES] list media:{device}:/NESTED 1')
            keys('d', 'ret', marker=f'[FILES] list media:{device}:/NESTED/DEEP 1')
            keys('r', 'ret', marker='[FILES] preview README.TXT')
            vm.shot(f'{device}-preview')
            keys('esc')
            keys('backspace', marker=f'[FILES] list media:{device}:/NESTED 1')
            keys('backspace', marker=f'[FILES] list media:{device}:/ 2')
            keys('p', 'ctrl-c', 'f4', *path(f'c:\\media{device}'), 'ret',
                 marker=f'[FILES] list C:\\MEDIA{device} 0')
            keys('ctrl-v', marker=['[FILES] job 1 item(s) copied.', f'[FILES] list C:\\MEDIA{device} 1'])
            vm.shot(f'{device}-imported')
            report['checks'].append({'device': kind, 'browse_preview_import': 'PASS',
                                     'destination': f'MEDIA{device}/PAYLOAD.BIN'})
            print(f'[native-media] PASS {kind}', flush=True)
        vm.hmp('eject -f floppy0')
        keys('alt-g', 'p', marker='[FILES] media error')
        vm.shot('missing-floppy')
        report['missing_floppy_error'] = True
    except Exception as error:
        report['error'] = repr(error)
        try:
            vm.shot('failure')
        except Exception:
            pass
    finally:
        vm.close()
    if 'error' not in report:
        fs = FAT16(disk)
        expected = (args.fixtures / 'PAYLOAD.BIN').read_bytes()
        for item in report['checks']:
            actual = fs.read(item['destination'])
            item['import_matches'] = actual == expected
        report['sources_unchanged'] = {str(p): digest(p) for p in sources} == hashes
        report['passed'] = all(i['import_matches'] for i in report['checks']) and report['sources_unchanged'] \
            and len(report['checks']) == 3
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'error': report.get('error')}, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
