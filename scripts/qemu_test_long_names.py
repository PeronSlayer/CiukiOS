#!/usr/bin/env python3
"""QEMU gate for desktop and Files VFAT names, with independent disk checks."""
import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fat16_lfn import VFAT
from qemu_test_desktop_polish import Gate
from qemu_test_native_windows import WindowVM


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    disk = out / 'disk.img'
    shutil.copyfile(args.image, disk)
    seed = out / 'A.TXT'
    seed.write_bytes(b'long-name copy probe\r\n')
    moved = out / 'B.TXT'
    moved.write_bytes(b'long-name move probe\r\n')
    subprocess.run(['mmd', '-i', str(disk), '::QA'], check=True)
    subprocess.run(['mcopy', '-o', '-i', str(disk), str(seed), '::QA/A.TXT'], check=True)
    subprocess.run(['mcopy', '-o', '-i', str(disk), str(moved), '::QA/B.TXT'], check=True)
    report = {'passed': False, 'physical_hardware_qualified': False, 'steps': []}
    vm = WindowVM(disk, out, 'std', memory=128, palette='platinum')
    g = Gate(vm)
    try:
        vm.ready()
        g.act('Close the Application Library', ['alt-f4'], [], 5)
        g.act('Desktop context menu', lambda: g.click(700, 420, 2), '[DESK] menu background')
        g.act('Desktop New Folder', ['f'], '[DESK] created New Folder')
        g.act('Desktop long folder name', ['type:My Documents 2026', 'ret'],
              '[DESK] renamed My Documents 2026')
        vm.shot('desktop-long-folder')
        g.act('Files on QA', ['meta_l-r', 'type:explorer c:\\qa', 'ret'], '[FILES] list C:\\QA')
        g.act('Files New Folder', ['ctrl-shift-n'], '[FILES] created New Folder')
        g.act('Files long folder name', ['type:Work Projects 2026', 'ret'],
              '[FILES] renamed Work Projects 2026')
        g.act('Copy source file', ['type:a', 'ctrl-c'], [], 5)
        g.act('Open long folder', ['type:w', 'ret'], '[FILES] list C:\\QA\\Work Projects 2026')
        g.act('Copy into long folder', ['ctrl-v'], '[FILES] job 1 item(s) copied.', 60)
        g.act('Back to QA', ['backspace'], '[FILES] list C:\\QA')
        g.act('Cut second source file', ['type:b', 'ctrl-x'], [], 5)
        g.act('Open long folder again', ['type:w', 'ret'], '[FILES] list C:\\QA\\Work Projects 2026')
        g.act('Move into long folder', ['ctrl-v'], '[FILES] job 1 item(s) moved.', 60)
        g.act('Back to QA again', ['backspace'], '[FILES] list C:\\QA')
        def background_menu():
            g.click(900, 520, 2)
        g.act('Files context menu', background_menu, '[FILES] menu background')
        g.act('Files New Text Document', ['t'], '[FILES] created New Text Document.txt')
        g.act('Files long text name', ['type:Project Notes 2026.txt', 'ret'],
              '[FILES] renamed Project Notes 2026.txt')
        g.act('Copy long text in place', ['type:p', 'ctrl-c', 'ctrl-v'],
              '[FILES] job 1 item(s) copied.', 60)
        vm.shot('files-long-names')
        g.act('Recycle long folder', ['type:w', 'delete'],
              "[FILES] confirm Are you sure you want to send 'Work Projects 2026'")
        g.act('Confirm recycle', ['ret'], '[FILES] recycled 1', 60)
        g.act('Close Files', ['alt-f4'], '[DESKTOP] WINDOW 08 CLOSE')
        bx, by = g.icon('*BIN')
        g.act('Open Recycle Bin', lambda: g.double_click(bx, by), '[BIN] list 1')
        g.act('Restore long folder', ['alt-f', 'e'], '[BIN] restored C:\\QA\\Work Projects 2026')
        g.act('Close Recycle Bin', ['alt-f4'], '[DESKTOP] WINDOW 15 CLOSE')
        report['steps'] = g.steps
    except Exception as error:
        report['error'] = repr(error)
        try:
            vm.shot('failure')
            report['serial_tail'] = g.serial()[-4000:]
        except Exception as diagnostic:
            report['diagnostic_error'] = repr(diagnostic)
    finally:
        vm.close()
    if 'error' not in report:
        fs = VFAT(disk)
        checks = {
            'desktop_long_folder': fs.entry('DESKTOP/My Documents 2026').is_dir,
            'files_long_folder': fs.entry('QA/Work Projects 2026').is_dir,
            'long_text_document': fs.entry('QA/Project Notes 2026.txt').size == 0,
            'copy_kept_long_name': fs.entry('QA/Project Notes 2026 - Copy.txt').size == 0,
            'copy_preserved_bytes': fs.read('QA/Work Projects 2026/A.TXT') == seed.read_bytes(),
            'move_preserved_bytes': fs.read('QA/Work Projects 2026/B.TXT') == moved.read_bytes(),
            'move_removed_source': not any(e.name.upper() == 'B.TXT' for e in fs.listing('QA')),
            'valid_vfat': not fs.check(),
            'short_alias': fs.entry('QA/Work Projects 2026').short != 'Work Projects 2026',
        }
        check = subprocess.run(['fsck.fat', '-n', str(disk)], capture_output=True, text=True)
        checks['fsck_clean'] = check.returncode == 0
        report['fsck'] = check.stdout[-1000:] + check.stderr[-1000:]
        report['checks'] = checks
        report['passed'] = all(checks.values())
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report.get(k) for k in ('passed', 'error', 'checks')}, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
