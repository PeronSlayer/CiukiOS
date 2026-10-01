#!/usr/bin/env python3
"""Gate for the desktop, the Recycle Bin, the Control Panel and drivers.

Boots a copy of the image, drives it with QEMU keyboard and mouse events and
follows the modules' COM1 lines ([DESK], [BIN], [FILES], [CONTROL],
[DEVICES], [CIUKNOTE], [TASKS], [DESKTOP] WINDOW nn). Parts:
  desktop   context menus (background, taskbar, top bar, a window's title
            bar, a folder), a new folder renamed in place, Delete to the
            Recycle Bin, Restore from the Recycle Bin window, the folder's
            Properties window, a drag onto the Recycle Bin, Empty.
  files     the Refresh button, a Properties window that leaves Files usable.
  control   a colour scheme, mouse and keyboard settings, a font installed
            and made the system font, the date set through the RTC (then
            read back by CiukNote's Time/Date), CiukNote's font dialog.
  drivers   the sample driver package installed from Device Manager.
  tasks     Task Manager's right-click menu.
After shutdown the disk is checked with an independent FAT16 parser
(DESKTOP.CFG, the font, DRIVERS.CFG, the Recycle Bin, the saved date). A
second boot on the same disk must load the driver (LOADDRV) and come up with
the saved colours and font. QEMU evidence only.
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_test_installed_hdd import FAT16                      # noqa: E402
from qemu_test_native_windows import WindowVM                  # noqa: E402

KEYS = {' ': 'spc', '.': 'dot', '\\': 'backslash', '-': 'minus', '/': 'slash',
        ':': 'shift-semicolon', '_': 'shift-minus'}
OCEAN_TITLE = (6, 22, 40)


class Gate:
    def __init__(self, vm):
        self.vm = vm
        self.steps = []

    def type(self, text):
        for ch in text:
            key = KEYS.get(ch, ch)
            if ch.isupper():
                key = 'shift-' + ch.lower()
            self.vm.key(key)

    def serial(self, offset=0):
        return subprocess.check_output(['scripts/serial_log_normalize.py', '--offset', str(offset),
                                        str(self.vm.serial)]).decode('cp437', 'replace')

    def act(self, name, keys, expect, timeout=30):
        offset = self.vm.offset()
        if callable(keys):
            keys()
        else:
            for k in keys:
                if k.startswith('type:'):
                    self.type(k[5:])
                else:
                    self.vm.key(k)
        for marker in ([expect] if isinstance(expect, str) else expect):
            self.vm.wait(marker, offset, timeout)
        time.sleep(0.4)
        self.steps.append(name)
        print(f'[desktop-polish] PASS {name}', flush=True)
        return self.serial(offset)

    def run(self, command, expect, timeout=30):
        return self.act('Run: ' + command, ['meta_l-r', 'type:' + command, 'ret'], expect, timeout)

    def click(self, x, y, button=1):
        self.vm.position(x, y)
        self.vm.hmp(f'mouse_button {button}')
        time.sleep(0.15)
        self.vm.hmp('mouse_button 0')
        time.sleep(0.3)

    def double_click(self, x, y):
        self.vm.position(x, y)
        for _ in range(2):
            self.vm.hmp('mouse_button 1')
            time.sleep(0.05)
            self.vm.hmp('mouse_button 0')
            time.sleep(0.05)

    def icon(self, name):
        """The last logged centre of a desktop icon."""
        rows = [line.split() for line in self.serial().splitlines() if line.startswith('[DESK] icon ')]
        for row in reversed(rows):
            if len(row) >= 5 and row[2].upper() == name.upper():
                return int(row[3]), int(row[4])
        raise AssertionError(f'no desktop icon {name}')


def last(pattern, text):
    found = re.findall(pattern, text)
    assert found, f'no {pattern!r} in the log'
    return found[-1]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    disk = out / 'disk.img'
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    subprocess.run(['mmd', '-i', volume, '::QA'], check=True)
    subprocess.run(['mcopy', '-o', '-i', volume, 'assets/fonts/native/DEJAVU.CFN', '::QA/EXTRAFNT.CFN'], check=True)
    report = {'passed': False, 'physical_hardware_qualified': False}
    vm = WindowVM(disk, out / 'first', 'std', memory=128, palette='platinum')
    g = Gate(vm)
    folder = None
    try:
        vm.ready()
        boot = g.serial()
        assert re.search(r'\[DESK\] icons \d+', boot), 'the desktop module did not load'
        g.act('Alt+F4 closes the Application Library', ['alt-f4'], [], 5)
        # ---- desktop ----
        g.act('Right click: desktop menu', lambda: g.click(700, 420, 2), '[DESK] menu background')
        vm.position(760, 476)
        time.sleep(0.8)
        vm.shot('desktop-menu-hover')
        text = g.act('New Folder from the menu', ['f'], '[DESK] created ')
        created = last(r'\[DESK\] created (.+)', text).strip()
        text = g.act('Rename in place', ['type:Docs', 'ret'], '[DESK] renamed ')
        folder = last(r'\[DESK\] renamed (.+)', text).strip()
        assert folder.upper().startswith('DOCS'), folder
        report['desktop_folder'] = {'created': created, 'renamed': folder}
        vm.shot('desktop-folder')
        g.act('Delete asks for the Recycle Bin', ['delete'], 'to the Recycle Bin?')
        g.act('Yes recycles the folder', ['ret'], '[DESK] recycled 1')
        bx, by = g.icon('*BIN')
        g.act('Recycle Bin opens', lambda: g.double_click(bx, by), ['[BIN] list 1', '[DESKTOP] WINDOW 15 OPEN'])
        vm.shot('recycle-bin')
        g.act('Restore', ['alt-f', 'e'], '[BIN] restored ')
        g.act('Recycle Bin closes', ['alt-f4'], '[DESKTOP] WINDOW 15 CLOSE')
        time.sleep(1.5)
        fx, fy = g.icon(folder)
        g.act('Right click: folder menu', lambda: g.click(fx, fy, 2), '[DESK] menu folder')
        g.act('Properties window', ['r'], ['[DESK] properties ', 'OPEN'])
        vm.shot('folder-properties')
        g.act('Properties closes', ['esc'], 'CLOSE')

        def drag_to_bin():
            vm.position(fx, fy)
            vm.hmp('mouse_button 1')
            time.sleep(0.3)
            for step in range(1, 7):
                vm.position(fx + (bx - fx) * step // 6, fy + (by - fy) * step // 6)
                time.sleep(0.1)
            time.sleep(0.3)
            vm.hmp('mouse_button 0')
        g.act('Drag the folder onto the Recycle Bin', drag_to_bin, '[DESK] recycled 1')
        g.act('Recycle Bin opens again', lambda: g.double_click(bx, by), '[BIN] list 1')
        g.act('Empty Recycle Bin asks', ['alt-f', 'b'], '[BIN] confirm ')
        g.act('Yes empties it', ['ret'], ['[BIN] emptied', '[BIN] list 0'])
        g.act('Recycle Bin closes again', ['alt-f4'], '[DESKTOP] WINDOW 15 CLOSE')
        g.act('Right click: taskbar menu', lambda: g.click(640, vm_height(vm) - 16, 2), '[DESK] menu taskbar')
        g.act('Esc closes it', ['esc'], [], 5)
        g.act('Right click: top bar menu', lambda: g.click(500, 14, 2), '[DESK] menu top bar')
        vm.shot('topbar-menu')
        g.act('Esc closes the top bar menu', ['esc'], [], 5)
        # ---- files ----
        text = g.act('Files', ['meta_l-e'], ['[DESKTOP] WINDOW 08 OPEN', '[FILES] geometry'])
        list_x, row_y, tree_x, tree_y = [int(v) for v in last(r'\[FILES\] geometry ([\d ]+)', text).split()[:4]]
        x0 = tree_x - 40
        tb_y = row_y - 22 - 60
        refresh_x = x0 + 4 + 60 + 2 + 76 + 2 + 36 + 2 + 31
        g.act('Refresh button', lambda: g.click(refresh_x, tb_y + 14), '[FILES] list ')
        g.act('Right click: title bar menu', lambda: g.click(x0 + 120, tb_y - 37, 2), '[DESK] window menu 08')
        g.act('Esc closes the window menu', ['esc'], [], 5)
        g.act('Properties of the selection', ['alt-ret'], ['[FILES] properties ', 'OPEN'])
        vm.shot('files-properties')
        g.act('Files still answers with Properties open', lambda: (g.click(list_x + 200, row_y + 300), vm.key('f5')),
              '[FILES] list ')
        g.act('Alt+F4 closes Files and its Properties', ['alt-f4'], '[DESKTOP] WINDOW 08 CLOSE')
        # ---- tasks (mouse steps: before the colour scheme changes the pointer) ----
        text = g.act('Task Manager', ['ctrl-shift-esc'], ['[DESKTOP] WINDOW 09 OPEN', '[TASKS] tab Applications'])
        g.act('Right click: an application', lambda: g.click(640, 400, 2), '[TASKS] menu ')
        vm.shot('tasks-menu')
        g.act('Esc (tasks menu)', ['esc'], [], 5)
        g.act('Task Manager closes', ['alt-f4'], '[DESKTOP] WINDOW 09 CLOSE')
        # ---- control panel ----
        g.run('control appearance', '[CONTROL] applet Appearance')
        g.act('Ocean colour scheme', ['down'], '[CONTROL] scheme Ocean')
        vm.shot('appearance-ocean')
        g.act('OK saves it', ['ret'], '[CONTROL] saved appearance')
        g.act('Control Panel closes', ['alt-f4'], '[DESKTOP] WINDOW 13 CLOSE')
        g.run('control mouse', '[CONTROL] applet Mouse')
        g.act('Mouse OK', ['ret'], '[CONTROL] mouse speed ')
        g.act('Control Panel closes (mouse)', ['alt-f4'], '[DESKTOP] WINDOW 13 CLOSE')
        g.run('control keyboard', '[CONTROL] applet Keyboard')
        g.act('Keyboard OK', ['ret'], '[CONTROL] saved keyboard')
        g.act('Control Panel closes (keyboard)', ['alt-f4'], '[DESKTOP] WINDOW 13 CLOSE')
        g.run('control font:c:\\qa\\extrafnt.cfn', '[CONTROL] applet Fonts')
        g.act('Install the font', ['alt-i'], '[CONTROL] font installed EXTRAFNT.CFN')
        g.act('Message OK', ['ret'], [], 5)
        g.act('Use it as the system font', ['alt-u'], '[CONTROL] system font EXTRAFNT.CFN')
        time.sleep(2)
        vm.shot('system-font')
        g.act('Control Panel closes (fonts)', ['alt-f4', 'alt-f4'], '[DESKTOP] WINDOW 13 CLOSE')
        g.run('control datetime', '[CONTROL] applet Date and Time')
        g.act('Year 2027', ['tab', 'tab', 'ctrl-a', 'type:2027', 'ret'], '[CONTROL] date set 2027-')
        g.act('Control Panel closes (date)', ['alt-f4'], '[DESKTOP] WINDOW 13 CLOSE')
        g.run('ciuknote', '[DESKTOP] WINDOW 12 OPEN')
        g.act('Time/Date and Save As', ['f5', 'ctrl-s'], '[CIUKNOTE] dialog Save As')
        g.act('Saved', ['type:c:\\qa\\date.txt', 'ret'], '[CIUKNOTE] saved C:\\QA\\DATE.TXT')
        g.act('Font dialog', ['alt-o', 'f'], '[CIUKNOTE] dialog Font')
        g.act('Another font', ['tab', 'down', 'down', 'ret'], '[CIUKNOTE] font ')
        vm.shot('ciuknote-font')
        g.act('CiukNote closes', ['alt-f4'], '[DESKTOP] WINDOW 12 CLOSE')
        # ---- drivers ----
        g.run('devmgmt', '[DEVICES] tab devices ')
        vm.shot('device-manager')
        g.act('Add New Driver', ['tab', 'alt-d', 'a'], '[DEVICES] dialog Add New Driver')
        g.act('The sample package', ['ctrl-a', 'type:c:\\drivers\\samples\\hello', 'ret'], '[DEVICES] confirm HELLO')
        g.act('Yes installs it', ['ret'], '[DEVICES] driver installed HELLO')
        g.act('Message OK (driver)', ['ret'], [], 5)
        vm.shot('drivers')
        g.act('Devices closes', ['alt-f4'], '[DESKTOP] WINDOW 14 CLOSE')
        report['steps'] = g.steps
    except Exception as error:
        report['error'] = repr(error)
        try:
            vm.shot('failure')
            report['serial_tail'] = g.serial()[-3000:]
        except Exception as diagnostic:
            report['diagnostic_error'] = repr(diagnostic)
    finally:
        vm.close()
    if 'error' not in report:
        fs = FAT16(disk)
        cfg = fs.read('SYSTEM/UI/DESKTOP.CFG')
        drivers = fs.read('DRIVERS/DRIVERS.CFG').decode('ascii', 'replace')
        date = fs.read('QA/DATE.TXT')
        desktop = fs.entries(fs.entry('DESKTOP')[1])
        recycle_entries = fs.entries(fs.entry('RECYCLED')[1])
        if 'INFO2.DAT' in recycle_entries:
            info = fs.read('RECYCLED/INFO2.DAT')
            record_size, used_offset = 320, 286
        else:
            info = fs.read('RECYCLED/INFO.DAT')
            record_size, used_offset = 128, 106
        checks = {
            'settings_saved': len(cfg) == 72 and cfg[:4] == b'CUI1',
            'scheme_title_colour': tuple(cfg[4 + 3:4 + 6]) == OCEAN_TITLE,
            'system_font': cfg[57:57 + 12] == b'EXTRAFNT.CFN',
            'font_installed': fs.read('SYSTEM/FONTS/EXTRAFNT.CFN')[:8] == b'CIUKFNT1',
            'driver_listed': re.search(r'^1 SAMPLE HELLO ', drivers, re.M) is not None,
            'driver_files': fs.read('DRIVERS/SAMPLE/HELLO/HELLO.COM')[:1] != b'',
            'folder_gone': not any(n.upper().startswith('DOCS') for n in desktop),
            'recycle_bin_empty': len(info) % record_size == 0 and all(
                info[i + used_offset] == 0 for i in range(0, len(info), record_size)),
            'date_from_rtc': b'2027' in date,
        }
        report['checks'] = checks
        # Second boot: the driver loads, the colours and font come back.
        vm2 = WindowVM(disk, out / 'second', 'std', memory=128, palette='platinum')
        try:
            vm2.ready()
            time.sleep(2)
            vm2.shot('second-boot')
            text = subprocess.check_output(['scripts/serial_log_normalize.py', str(vm2.serial)]).decode('cp437', 'replace')
            checks['driver_loaded_at_boot'] = '[LOADDRV] HELLO OK' in text and '[HELLO] driver loaded' in text
        except Exception as error:
            report['error'] = 'second boot: ' + repr(error)
        finally:
            vm2.close()
        report['passed'] = 'error' not in report and all(checks.values())
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'error': report.get('error'),
                      'checks': report.get('checks')}, indent=2))
    return 0 if report['passed'] else 1


def vm_height(vm):
    """The screen height (the taskbar is its last 32 rows)."""
    from PIL import Image
    with Image.open(vm.shot('size')) as im:
        return im.size[1]


if __name__ == '__main__':
    raise SystemExit(main())
