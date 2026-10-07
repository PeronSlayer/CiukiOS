#!/usr/bin/env python3
"""Gate for the desktop applications and shortcuts (Files, Notepad, Tasks).

Boots a copy of the image with a small C:\\QA tree and drives everything with
QEMU keyboard/mouse events. Parts:
  keys     Win+R (Run), Ctrl+Esc (Programs), Win+E (Files), Alt+F4 (close),
           Ctrl+Shift+Esc and Ctrl+Alt+Del (Tasks; no reboot), Win+D.
  files    Explorer operations: new folder + rename in place, copy/paste,
           cut/paste (move), delete with confirmation, new text document from
           the context menu (right click), properties, open in Notepad.
  notepad  typing, Save, Replace All, Undo (toggles), Time/Date (F5), Find,
           Go To, Open (Ctrl+O) of a .LOG file (time stamp appended), Save,
           Alt+F4.
  tasks    all four tabs; End Task closes Files.
The modules log state on COM1 ("[FILES] ...", "[CIUKNOTE] ...", "[TASKS] ...",
"[DESKTOP] WINDOW nn OPEN/CLOSE"). After shutdown the disk image is checked
with an independent FAT16 parser. QEMU evidence only.
"""
import argparse
import datetime
import json
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

    def serial(self, offset):
        return subprocess.check_output(['scripts/serial_log_normalize.py', '--offset', str(offset),
                                        str(self.vm.serial)]).decode('cp437', 'replace')

    def act(self, name, keys, expect, timeout=30):
        """Send keys (a list, or a callable), then wait for every marker."""
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
        print(f'[desktop-apps] PASS {name}', flush=True)
        return self.serial(offset)


def fixtures(disk, out):
    volume = f'{disk}@@{FAT16(disk).start}'
    files = {'A.TXT': b'alpha file\r\n', 'B.TXT': b'bravo file\r\n',
             'LOG.TXT': b'.LOG\r\nfirst entry\r\n', 'SUB/C.TXT': b'charlie\r\n'}
    subprocess.run(['mmd', '-i', volume, '::QA', '::QA/SUB', '::QA2', '::QA2/TARGET'], check=True)
    extra = out / 'E.TXT'
    extra.write_bytes(b'echo file\r\n')
    subprocess.run(['mcopy', '-o', '-i', volume, str(extra), '::QA2/E.TXT'], check=True)
    for name, data in files.items():
        src = out / name.replace('/', '_')
        src.write_bytes(data)
        subprocess.run(['mcopy', '-o', '-i', volume, str(src), '::QA/' + name], check=True)
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    disk = out / 'disk.img'
    shutil.copyfile(args.image, disk)
    source = fixtures(disk, out)
    report = {'passed': False, 'physical_hardware_qualified': False}
    dates = [datetime.datetime.now(datetime.timezone.utc).date()]
    vm = WindowVM(disk, out, 'std', memory=128, palette='platinum')
    g = Gate(vm)
    try:
        vm.ready()
        # Dismiss the boot About window so Tasks sees only this test's apps.
        vm.key('esc')
        # ---- keys ----
        g.act('Win+R opens Run', ['meta_l-r'], '[DESKTOP] WINDOW 01 OPEN')
        g.act('Esc closes Run', ['esc'], '[DESKTOP] WINDOW 01 CLOSE')
        g.act('Ctrl+Esc opens Programs', ['ctrl-esc'], '[DESKTOP] WINDOW 00 OPEN')
        g.act('Win+E opens Files', ['meta_l-e'], ['[DESKTOP] WINDOW 08 OPEN', '[FILES] list C:\\'])
        g.act('Alt+F4 closes Files', ['alt-f4'], '[DESKTOP] WINDOW 08 CLOSE')
        g.act('Ctrl+Shift+Esc opens Tasks', ['ctrl-shift-esc'], ['[DESKTOP] WINDOW 09 OPEN', '[TASKS] tab Applications'])
        g.act('Alt+F4 closes Tasks', ['alt-f4'], '[DESKTOP] WINDOW 09 CLOSE')
        g.act('Ctrl+Alt+Del opens Tasks, no reboot', ['ctrl-alt-delete'], '[DESKTOP] WINDOW 09 OPEN')
        g.act('Alt+F4 closes Tasks again', ['alt-f4'], '[DESKTOP] WINDOW 09 CLOSE')
        vm.shot('keys')
        # ---- files ----
        g.act('Run: explorer c:\\qa', ['meta_l-r', 'type:explorer c:\\qa', 'ret'], '[FILES] list C:\\QA 4')
        g.act('New folder, rename in place', ['ctrl-shift-n'], '[FILES] created New Folder')
        g.act('Rename to DOCS', ['type:DOCS', 'ret'], ['[FILES] renamed DOCS', '[FILES] list C:\\QA 5'])
        vm.shot('files-renamed')
        g.act('Copy A.TXT', ['type:a', 'ctrl-c'], [], 5)
        g.act('Open DOCS', ['type:d', 'ret'], '[FILES] list C:\\QA\\DOCS 0')
        g.act('Paste copies A.TXT', ['ctrl-v'], ['[FILES] job 1 item(s) copied.', '[FILES] list C:\\QA\\DOCS 1'], 60)
        g.act('Backspace: up', ['backspace'], '[FILES] list C:\\QA 5')
        g.act('Cut B.TXT', ['type:b', 'ctrl-x'], [], 5)
        g.act('Open DOCS again', ['type:d', 'ret'], '[FILES] list C:\\QA\\DOCS 1')
        g.act('Paste moves B.TXT', ['ctrl-v'], ['[FILES] job 1 item(s) moved.', '[FILES] list C:\\QA\\DOCS 2'], 60)
        g.act('Alt+Left: back', ['alt-left'], '[FILES] list C:\\QA 4')
        g.act('Delete SUB asks', ['type:s', 'delete'], "[FILES] confirm Are you sure you want to send 'SUB' to the Recycle Bin?")
        g.act('Yes moves SUB to the Recycle Bin', ['ret'], ['[FILES] recycled 1', '[FILES] list C:\\QA 3'], 60)
        vm.shot('files-after-ops')

        def background_menu():
            x = vm.hmp('info registers')  # keep the monitor warm
            del x
            px, py = 900, 520
            vm.position(px, py)
            vm.hmp('mouse_button 2')
            time.sleep(0.2)
            vm.hmp('mouse_button 0')
        g.act('Right click: background menu', background_menu, '[FILES] menu background')
        vm.shot('files-context-menu')
        g.act('New Text Document from the menu', ['type:t'], '[FILES] created New Text Document.txt')
        g.act('Rename to NOTES.TXT', ['type:NOTES.TXT', 'ret'], '[FILES] renamed NOTES.TXT')
        g.act('Properties of DOCS', ['type:d', 'alt-ret'], '[FILES] properties Size: ')
        vm.shot('files-properties')
        g.act('Close Properties', ['esc'], [], 5)
        # ---- notepad ----
        g.act('Enter opens NOTES.TXT in Notepad', ['type:n', 'ret'],
              ['[FILES] open C:\\QA\\NOTES.TXT', '[DESKTOP] WINDOW 12 OPEN', '[CIUKNOTE] opened C:\\QA\\NOTES.TXT'])
        g.act('Type and save', ['type:Hello from Notepad', 'ret', 'type:second line', 'ctrl-s'],
              '[CIUKNOTE] saved C:\\QA\\NOTES.TXT')
        g.act('Replace dialog', ['ctrl-h'], '[CIUKNOTE] dialog Replace')
        g.act('Replace All', ['type:Notepad', 'tab', 'type:CiukiOS', 'alt-a'], '[CIUKNOTE] replaced 1')
        g.act('Undo', ['ctrl-z'], '[CIUKNOTE] undo')
        g.act('Undo again (toggles back)', ['ctrl-z'], '[CIUKNOTE] undo')
        g.act('Find dialog', ['ctrl-f'], '[CIUKNOTE] dialog Find')
        g.act('Find closes on Cancel', ['type:second', 'ret', 'esc'], [], 5)
        g.act('Go To dialog', ['ctrl-g'], '[CIUKNOTE] dialog Go To Line')
        g.act('Go To line 1', ['ret'], [], 5)
        g.act('Time/Date and save', ['ctrl-end', 'ret', 'f5', 'ctrl-s'], '[CIUKNOTE] saved C:\\QA\\NOTES.TXT')
        vm.shot('notepad-saved')
        g.act('Open dialog', ['ctrl-o'], '[CIUKNOTE] dialog Open')
        g.act('Open LOG.TXT (.LOG stamps it)', ['type:log.txt', 'ret'], '[CIUKNOTE] opened C:\\QA\\LOG.TXT')
        g.act('Save the stamped log', ['ctrl-s'], '[CIUKNOTE] saved C:\\QA\\LOG.TXT')
        vm.shot('notepad-log')
        g.act('Alt+F4 closes Notepad', ['alt-f4'], '[DESKTOP] WINDOW 12 CLOSE')
        # ---- tasks ----
        g.act('Tasks', ['ctrl-shift-esc'], '[TASKS] tab Applications')
        g.act('Processes tab', ['right'], '[TASKS] tab Processes')
        g.act('Virtual Machines tab', ['right'], '[TASKS] tab Virtual Machines 4')
        g.act('Performance tab', ['right'], '[TASKS] tab Performance')
        vm.shot('tasks-performance')
        g.act('Back to Applications', ['right'], '[TASKS] tab Applications')
        g.act('End Task closes Files', ['down', 'delete'], ['[TASKS] end Files', '[DESKTOP] WINDOW 08 CLOSE'])
        # ---- drag and drop, folder tree ----
        text = g.act('Files on C:\\QA2', ['meta_l-r', 'type:explorer c:\\qa2', 'ret'],
                     ['[FILES] list C:\\QA2 2', '[FILES] geometry'])
        geometry = [int(v) for v in text.split('[FILES] geometry ')[-1].split()[:4]]
        list_x, row_y, tree_x, tree_y = geometry

        def drag():
            # Rows: TARGET (folder first), then E.TXT.
            vm.position(list_x + 60, row_y + 18 + 9)
            vm.hmp('mouse_button 1')
            time.sleep(0.3)
            for _ in range(6):
                vm.hmp('mouse_move 0 -2')
                time.sleep(0.1)
            vm.position(list_x + 60, row_y + 9)
            time.sleep(0.3)
            vm.hmp('mouse_button 0')
        g.act('Drag E.TXT onto TARGET moves it', drag,
              ['[FILES] drop C:\\QA2\\TARGET', '[FILES] job 1 item(s) moved.'], 60)
        vm.shot('files-dropped')

        def tree_click():
            # The tree: the disk, then C:'s folders (it scrolls to keep QA2
            # in view); the first row shown is another folder than QA2.
            vm.position(tree_x + 10, tree_y + 9)
            vm.hmp('mouse_button 1')
            time.sleep(0.2)
            vm.hmp('mouse_button 0')
        tree = g.act('Folders tree: another folder', tree_click, '[FILES] list C:\\')
        assert '[FILES] list C:\\QA2 ' not in tree, 'the tree click stayed in QA2'
        g.act('Win+D shows the desktop', ['meta_l-d'], '[DESKTOP] SHOW DESKTOP')
        vm.shot('final')
        report['steps'] = g.steps
    except Exception as error:
        report['error'] = repr(error)
        try:
            vm.shot('failure')
            report['serial_tail'] = g.serial(0)[-3000:]
        except Exception as diagnostic:
            report['diagnostic_error'] = repr(diagnostic)
    finally:
        vm.close()
    dates.append(datetime.datetime.now(datetime.timezone.utc).date())
    if 'error' not in report:
        fs = FAT16(disk)
        qa = fs.entries(fs.entry('QA')[1])
        docs = fs.entries(fs.entry('QA/DOCS')[1])
        notes = fs.read('QA/NOTES.TXT')
        log = fs.read('QA/LOG.TXT')
        checks = {
            'copy_kept_source': 'A.TXT' in qa and fs.read('QA/A.TXT') == source['A.TXT'],
            'copy_in_docs': fs.read('QA/DOCS/A.TXT') == source['A.TXT'],
            'move_removed_source': 'B.TXT' not in qa,
            'move_in_docs': fs.read('QA/DOCS/B.TXT') == source['B.TXT'],
            'folder_deleted': 'SUB' not in qa,
            'folder_in_bin': any(fs.read('RECYCLED/' + n) == source['SUB/C.TXT']
                                 for n in [f'DC1/C.TXT']),
            'drag_moved': fs.read('QA2/TARGET/E.TXT') == b'echo file\r\n'
                          and 'E.TXT' not in fs.entries(fs.entry('QA2')[1]),
            'notes_text': notes.startswith(b'Hello from CiukiOS\r\nsecond line\r\n') and b'/' in notes[33:],
            'log_stamped': log.startswith(source['LOG.TXT']) and len(log) > len(source['LOG.TXT']) + 8,
            # Time/Date comes from the RTC (QEMU: UTC) through INT 21h 2Ah/2Ch.
            'date_is_today': any(f'{d.month}/{d.day}/{d.year}'.encode() in notes for d in dates),
        }
        report['checks'] = checks
        report['qa'] = sorted(qa)
        report['notes'] = notes.decode('cp437', 'replace')
        report['log'] = log.decode('cp437', 'replace')
        report['passed'] = all(checks.values())
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'error': report.get('error'),
                      'checks': report.get('checks')}, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
