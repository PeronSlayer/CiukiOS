#!/usr/bin/env python3
"""Exercise native Files/Tasks through real QEMU PS/2 input on an image copy.

RAM is read only to locate rendered controls and assert state. All guest actions
come from mouse/keyboard events. Final data checks use an independent FAT parser.
This is a focused feature test, not a claim about physical E500/T23 hardware.
"""
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import time
from pathlib import Path

from qemu_test_installed_hdd import FAT16
from qemu_test_native_windows import WindowVM
from qemu_test_ui_regressions import ui_hit_binding


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Utilities:
    def __init__(self, vm, shell, listing):
        self.vm = vm
        self.lines = listing.read_text().splitlines()
        self.offsets = {}
        dump = vm.output / 'initial-memory.bin'
        vm.hmp(f'pmemsave 0 1048576 "{dump}"')
        memory = dump.read_bytes()
        signature = shell[:256]
        candidates = []
        pos = memory.find(signature)
        while pos >= 0:
            candidates.append(pos)
            pos = memory.find(signature, pos + 1)
        assert len(candidates) == 1, f'loaded SHELL signature is not unique: {candidates}'
        self.base = candidates[0]  # COM file byte zero; PSP precedes this by 100h.
        self.ram = b''
        self.events = []
        self.refresh()

    def refresh(self):
        path = self.vm.output / 'shell-observed.bin'
        self.vm.hmp(f'pmemsave {self.base} 65536 "{path}"')
        self.ram = path.read_bytes()
        assert self.vm.process.poll() is None, 'QEMU exited unexpectedly'
        return self

    def offset(self, label):
        if label not in self.offsets:
            declaration = re.compile(r'\b' + re.escape(label) + r'(?=:|\s+(?:d[bwdq]|times)\b)')
            for line_number, line in enumerate(self.lines):
                if not declaration.search(line):
                    continue
                for following in self.lines[line_number:]:
                    address = re.match(r'\s*\d+\s+([0-9A-F]{8})\s', following)
                    if address:
                        self.offsets[label] = int(address.group(1), 16)
                        break
                break
            assert label in self.offsets, f'Shell listing has no {label}'
        return self.offsets[label]

    def b(self, label, index=0):
        return self.ram[self.offset(label) + index]

    def w(self, label, index=0):
        return struct.unpack_from('<H', self.ram, self.offset(label) + index * 2)[0]

    def z(self, label):
        return self.z_at(self.offset(label))

    def z_at(self, offset):
        return self.ram[offset:offset + 256].split(b'\0', 1)[0].decode('cp437')

    def status(self):
        return self.z_at(self.w('fm_status') - 0x100)

    def until(self, predicate, description, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.refresh()
            if predicate():
                return
            time.sleep(.08)
        raise AssertionError(f'{description}; active={self.b("ui_active_window")}, '
                             f'prompt={self.b("fm_prompt")}, status={self.status()!r}')

    def shot(self, name):
        self.vm.shot(name, (800, 600))
        self.refresh()
        self.events.append({'name': name, 'active_window': self.b('ui_active_window'),
                            'path': self.z('fm_path'), 'status': self.status()})
        print(f'[native-utilities] {name}: {self.status()}', flush=True)

    def click(self, action, owner=None):
        self.refresh()
        count = self.w('ui_hit_count')
        assert 0 < count <= 192, f'invalid hit count {count}'
        hits = self.hit_records()
        # Use only a point where reverse hit testing really resolves to this
        # rendered control; background windows may have occluded hit regions.
        for n in range(count - 1, -1, -1):
            x, y, right, bottom, encoded = hits[n]
            # Owner zero is the desktop; native window N is encoded as N+1.
            if encoded & 255 != action or (owner is not None and encoded >> 8 != owner + 1):
                continue
            for px, py in (((x + right) // 2, (y + bottom) // 2),
                           (x + 3, y + 3), (right - 4, bottom - 4)):
                if not (0 <= px < 776 and 0 <= py < 584):
                    continue
                front = next((i for i in range(count - 1, -1, -1)
                              if hits[i][0] <= px < hits[i][2]
                              and hits[i][1] <= py < hits[i][3]), None)
                if front == n:
                    self.vm.completed_control_click(px, py)
                    self.refresh()
                    return
        raise AssertionError(f'no exposed control action={action}, owner={owner}, hits={hits}')

    def hit_records(self):
        count=self.w('ui_hit_count')
        assert 0 <= count <= 192, f'invalid hit count {count}'
        binding=ui_hit_binding(self.lines,self.base,self.w)
        if binding['kind']=='inline':
            data=self.ram[binding['offset']:binding['offset']+count*10]
        else:
            path=self.vm.output/'hit-observed.bin'
            self.vm.hmp(f'pmemsave {binding["address"]} {max(count*10,1)} "{path}"')
            data=path.read_bytes()
        return [struct.unpack_from('<5H',data,i*10) for i in range(count)]

    def prompt(self, action):
        self.click(action, 8)
        self.until(lambda: self.b('fm_prompt') == action, f'prompt {action}')

    def replace_text(self, value):
        self.refresh()
        count = self.b('fm_edit_len')
        self.vm.key('home')
        for _ in range(count):
            self.vm.key('delete')
        self.vm.text(value)

    def location(self, path, success=True):
        before = self.z('fm_path')
        self.prompt(108)
        self.replace_text(path)
        if success:
            self.until(lambda: self.b('fm_prompt') == 0 and self.z('fm_path') == path,
                       f'navigate to {path}')
        else:
            self.until(lambda: 'not found' in self.status().lower(), 'missing folder reported')
            assert self.z('fm_path') == before, 'failed navigation changed current location'
            self.vm.key('esc')
            self.until(lambda: self.b('fm_prompt') == 0, 'close invalid-location prompt')

    def rows(self):
        return [self.z_at(self.offset('fm_records') + i * 18)
                for i in range(self.b('fm_count'))]

    def select(self, name):
        # Fixture directories have at most six entries. Selection is still a
        # real click on the visible list, never a mutation of fm_selection.
        self.refresh()
        names = self.rows()
        assert name in names, f'{name} missing from visible native Files rows {names}'
        row = names.index(name)
        self.click(120 + row, 8)
        self.until(lambda: self.b('fm_selection') == row, f'select {name}')

    def operation(self, action, text, status='Done.'):
        self.prompt(action)
        self.replace_text(text)
        self.until(lambda: self.b('fm_busy') == 0 and self.b('fm_prompt') == 0
                   and self.status() == status, f'operation {action}: {text}', timeout=30)

    def cancel_prompt(self):
        self.vm.key('esc')
        self.until(lambda: self.b('fm_prompt') == 0, 'cancel prompt')


def exercise_launches(ui, vm, report):
    ui.location('C:\\QARUN')
    stacks = []
    for attempt in range(3):
        ui.select('COMDEMO.COM')
        offset = vm.offset()
        vm.key('ret')
        vm.wait('COM demo via INT21h', offset, 30)
        vm.ready(offset)
        ui.until(lambda: ui.b('ui_active_window') == 8, 'Files resumes after COM child')
        ui.shot(f'07-comdemo-return-{attempt + 1}')
        # Observe a suspended idle CPU, without altering guest registers.
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            registers = vm.hmp('info registers').decode(errors='replace')
            if 'HLT=1' in registers:
                sp = re.search(r'ESP=([0-9A-Fa-f]+)', registers)
                ss = re.search(r'SS =([0-9A-Fa-f]+)', registers)
                assert sp and ss, registers
                stacks.append((int(ss.group(1), 16), int(sp.group(1), 16)))
                break
            time.sleep(.03)
        else:
            raise AssertionError('Could not observe a parked desktop stack')
    assert len(set(stacks)) == 1, f'Files launch leaked stack across returns: {stacks}'
    report['repeated_com_launch_stacks'] = stacks



def exercise_tasks(ui, vm, report):
    ui.click(1)  # Programs shortcut restores the real library.
    ui.click(22, 0)  # System page.
    ui.click(33, 0)  # Tasks tile, selected by its actual hit region.
    vm.key('ret')
    ui.until(lambda: ui.b('ui_active_window') == 9, 'Tasks opens from System library')
    ui.click(18, 9)
    ui.until(lambda: ui.b('ui_window_flags', 9) == 0, 'Tasks closes before shortcut')
    vm.key('ctrl-shift-esc')
    ui.until(lambda: ui.b('ui_active_window') == 9, 'Ctrl+Shift+Esc opens Tasks')
    report['task_manager_shortcut'] = 'Ctrl+Shift+Esc'
    assert ui.b('ui_tm_valid') == 1, 'Task Manager rejected the real MCB chain'
    assert ui.b('ui_tm_process_count') >= 1, 'Task Manager does not list shell allocation'
    assert 0 < ui.w('ui_tm_largest') <= ui.w('ui_tm_free'), 'invalid memory figures'
    ui.shot('08-tasks-windows')
    ui.click(166, 9)
    ui.until(lambda: ui.b('ui_tm_tab') == 1, 'process tab')
    psp = ui.w('ui_tm_psp')
    owners = [ui.w('ui_tm_processes', i * 2) for i in range(ui.b('ui_tm_process_count'))]
    assert psp in owners, 'current shell PSP is missing from process display'
    ui.shot('09-tasks-processes')
    ui.click(167, 9)
    ui.until(lambda: ui.b('ui_tm_tab') == 2, 'memory tab')
    ui.shot('10-tasks-memory')
    ui.click(165, 9)
    ui.refresh()
    windows = list(ui.ram[ui.offset('ui_tm_windows'):ui.offset('ui_tm_windows') + ui.w('ui_tm_window_count')])
    assert 8 in windows and windows.index(8) < 5, f'Files is absent from Tasks: {windows}'
    ui.click(170 + windows.index(8), 9)
    ui.click(161, 9)
    ui.until(lambda: ui.b('ui_active_window') == 8, 'Tasks switches to Files')
    ui.click(17, 8)
    ui.until(lambda: ui.b('ui_window_flags', 8) == 2, 'Files minimizes')
    ui.click(209)
    ui.until(lambda: ui.b('ui_active_window') == 9, 'Tasks restores from taskbar')
    ui.shot('11-tasks-minimized-file-window')
    ui.click(170 + windows.index(8), 9)
    ui.click(162, 9)
    ui.until(lambda: ui.b('ui_window_flags', 8) == 0, 'Tasks closes Files window')
    ui.shot('12-tasks-close-window')
    report['task_manager'] = {'shell_psp': psp, 'process_owners': owners,
                              'used_paragraphs': ui.w('ui_tm_used'),
                              'free_paragraphs': ui.w('ui_tm_free'),
                              'largest_paragraphs': ui.w('ui_tm_largest')}


def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'hdd.img'
    assert not disk.exists(), f'preserve prior artifacts: choose a new output folder ({disk})'
    source = args.image.resolve()
    source_hash = sha(source.read_bytes())
    shutil.copyfile(source, disk)
    fs = FAT16(disk)
    volume = f'{disk}@@{fs.start}'
    if args.shell:
        subprocess.run(['mcopy', '-o', '-i', volume, str(args.shell.resolve()),
                        '::SYSTEM/SHELL.COM'], check=True)
        fs = FAT16(disk)
    if args.kernel:
        subprocess.run(['mcopy', '-o', '-i', volume, str(args.kernel.resolve()),
                        '::SYSTEM/CIUKIDOS.SYS'], check=True)
        fs = FAT16(disk)
    shell = fs.read('SYSTEM/SHELL.COM')
    listing = out / 'tested-shell.lst'
    shutil.copyfile(args.shell_listing, listing)
    report = {'scope': 'QEMU KVM Pentium III 128MiB, actual PS/2 GUI input; no hardware claim',
              'source_image': str(source), 'source_sha256': source_hash,
              'shell_sha256': sha(shell), 'kernel_sha256': sha(fs.read('SYSTEM/CIUKIDOS.SYS')),
              'shell_override': str(args.shell.resolve()) if args.shell else None,
              'kernel_override': str(args.kernel.resolve()) if args.kernel else None,
              'listing': str(listing), 'listing_sha256': sha(listing.read_bytes()), 'fat_start': fs.start,
              'case': args.case, 'result': 'RUNNING'}
    fixture = out / 'fixtures'
    fixture.mkdir()
    files = {'SOURCE.BIN': bytes((i * 53 + 17) & 255 for i in range(12325)),
             'EXIST.BIN': b'Existing destination must survive.\r\n' * 19,
             'DELETE.BIN': b'Delete only after explicit confirmation.\r\n'}
    subprocess.run(['mmd', '-i', volume, '::QAUTIL'], check=True)
    for name, contents in files.items():
        path = fixture / name
        path.write_bytes(contents)
        subprocess.run(['mcopy', '-o', '-i', volume, str(path), f'::QAUTIL/{name}'], check=True)
    subprocess.run(['mmd', '-i', volume, '::QARUN'], check=True)
    comdemo = fixture / 'COMDEMO.COM'
    comdemo.write_bytes(fs.read('APPS/COMDEMO.COM'))
    subprocess.run(['mcopy', '-o', '-i', volume, str(comdemo), '::QARUN/COMDEMO.COM'], check=True)
    display = fixture / 'DISPLAY.CFG'
    display.write_bytes(b'0800')
    subprocess.run(['mcopy', '-o', '-i', volume, str(display), '::SYSTEM/VIDEO/DISPLAY.CFG'], check=True)
    vm = WindowVM(disk, out, 'std', memory=128, palette='platinum')
    vm.auto_enter_dos = False
    ui = None
    try:
        vm.ready()
        ui = Utilities(vm, shell, listing)
        ui.shot('01-desktop')
        ui.click(2)  # Actual Files desktop shortcut.
        ui.until(lambda: ui.b('ui_active_window') == 8, 'Files opens')
        if args.case == 'all':
            ui.location('C:\\QAUTIL')
            ui.operation(103, 'DEST')
            ui.shot('02-created-folder')
            # Inspect actual on-disk metadata at the first mutation. A later GUI
            # refresh alone cannot establish correct '.'/'..' or child placement.
            vm.hmp('stop')
            try:
                created = FAT16(disk)
                directory = created.entry('QAUTIL/DEST')
                contents = created.entries(directory[1])
                report['mkdir_checkpoint'] = {'entry': directory, 'entries': contents}
                (out / 'mkdir-checkpoint.json').write_text(json.dumps(report['mkdir_checkpoint'], indent=2))
                assert contents.get('.', (0, 0))[1] == directory[1], 'mkdir did not initialize self entry'
                assert contents.get('..', (0, 0))[1] == created.entry('QAUTIL')[1], 'mkdir parent entry is wrong'
            finally:
                vm.hmp('cont')

            ui.select('SOURCE.BIN')
            ui.operation(105, 'C:\\QAUTIL\\COPIED.BIN', 'File copied.')
            ui.select('COPIED.BIN')
            ui.operation(104, 'RENAMED.BIN')
            ui.select('RENAMED.BIN')
            ui.operation(106, 'C:\\QAUTIL\\DEST\\MOVED.BIN')
            ui.shot('03-copy-rename-move')
            vm.hmp('stop')
            try:
                moved = FAT16(disk)
                report['move_checkpoint'] = {'entries': moved.entries(moved.entry('QAUTIL/DEST')[1]),
                                              'sha256': sha(moved.read('QAUTIL/DEST/MOVED.BIN'))}
                (out / 'move-checkpoint.json').write_text(json.dumps(report['move_checkpoint'], indent=2))
                assert moved.read('QAUTIL/DEST/MOVED.BIN') == files['SOURCE.BIN'], 'moved bytes differ before delete'
            finally:
                vm.hmp('cont')

            ui.select('SOURCE.BIN')
            ui.prompt(105)
            ui.replace_text('C:\\QAUTIL\\EXIST.BIN')
            ui.until(lambda: 'already exists' in ui.status(), 'existing destination rejected')
            ui.shot('04-existing-target-preserved')
            ui.cancel_prompt()
            ui.prompt(105)
            ui.replace_text('D:\\SOURCE.BIN')
            ui.until(lambda: 'within one DOS drive' in ui.status(), 'cross-drive operation rejected')
            ui.cancel_prompt()
            ui.location('C:\\MISSING', success=False)
            ui.shot('05-location-preserved')

            ui.select('DEST')
            ui.prompt(107)
            assert ui.w('ui_focus') == 114, 'deletion does not default to Cancel'
            vm.key('ret')
            ui.until(lambda: ui.b('fm_prompt') == 0, 'Enter cancels deletion')
            ui.prompt(107)
            vm.key('tab')
            ui.until(lambda: ui.w('ui_focus') == 113, 'explicit delete focus')
            vm.key('ret')
            ui.until(lambda: 'Access denied' in ui.status(), 'nonempty directory deletion rejected')
            ui.shot('06-nonempty-folder-protected')
            ui.cancel_prompt()

            ui.select('DELETE.BIN')
            ui.prompt(107)
            vm.key('ret')
            ui.until(lambda: ui.b('fm_prompt') == 0, 'file deletion cancelled')
            assert 'DELETE.BIN' in ui.rows(), 'cancel removed file'
            ui.prompt(107)
            vm.key('tab')
            vm.key('ret')
            ui.until(lambda: ui.b('fm_prompt') == 0 and 'DELETE.BIN' not in ui.rows(),
                     'confirmed file deletion')
            ui.operation(103, 'TEMP')
            ui.select('TEMP')
            ui.operation(104, 'TEMP2')
            temp_cluster = FAT16(disk).entry('QAUTIL/TEMP2')[1]
            report['removed_empty_directory_cluster'] = temp_cluster
            ui.select('TEMP2')
            ui.prompt(107)
            vm.key('tab')
            vm.key('ret')
            ui.until(lambda: ui.b('fm_prompt') == 0 and 'TEMP2' not in ui.rows(),
                     'confirmed empty directory deletion')
            ui.shot('07-delete-results')

        if args.case in ('all', 'launches'):
            exercise_launches(ui, vm, report)
        if args.case in ('all', 'tasks'):
            exercise_tasks(ui, vm, report)
        report['events'] = ui.events
    except BaseException as exc:
        report['result'] = 'FAIL'
        report['error'] = repr(exc)
        if ui is not None:
            report['events'] = ui.events
        try:
            vm.shot('failure')
            vm.hmp(f'pmemsave 0 1048576 "{out}/failure-memory.bin"')
        except Exception:
            pass
        raise
    finally:
        vm.close()
        report['qemu_exit_code'] = vm.process.returncode
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    if args.case != 'all':
        report['result'] = 'PASS'
        assert sha(source.read_bytes()) == source_hash, 'source image was modified'
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f'[native-utilities] PASS real {args.case} feature checks', flush=True)
        return

    try:
        final = FAT16(disk)
        assert final.read('QAUTIL/SOURCE.BIN') == files['SOURCE.BIN'], 'source bytes changed'
        assert final.read('QAUTIL/EXIST.BIN') == files['EXIST.BIN'], 'existing target was overwritten'
        assert final.read('QAUTIL/DEST/MOVED.BIN') == files['SOURCE.BIN'], 'copy/move bytes differ'
        present = final.entries(final.entry('QAUTIL')[1])
        for removed in ('COPIED.BIN', 'RENAMED.BIN', 'DELETE.BIN', 'TEMP', 'TEMP2'):
            assert removed not in present, f'{removed} still exists after operation'
        assert struct.unpack_from('<H', final.data, final.fat + temp_cluster * 2)[0] == 0, \
            'empty-directory removal leaked its data cluster'
        fat_bytes = struct.unpack_from('<H', final.data, final.start + 22)[0] * final.bps
        assert final.data[final.fat:final.fat + fat_bytes] == \
            final.data[final.fat + fat_bytes:final.fat + 2 * fat_bytes], 'FAT mirrors differ'
        assert sha(source.read_bytes()) == source_hash, 'source image was modified'
        report['fat_checks'] = {'source_sha256': sha(final.read('QAUTIL/SOURCE.BIN')),
                                'moved_sha256': sha(final.read('QAUTIL/DEST/MOVED.BIN')),
                                'existing_target_sha256': sha(final.read('QAUTIL/EXIST.BIN')),
                                'remaining_entries': sorted(present), 'source_image_unchanged': True}
    except BaseException as exc:
        report['result'] = 'FAIL'
        report['error'] = repr(exc)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        raise
    report['result'] = 'PASS'
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('[native-utilities] PASS real Files operations, FAT bytes and Tasks controls', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=('all', 'tasks', 'launches'), default='all')
    parser.add_argument('--image', type=Path, required=True,
                        help='Prepared candidate; copied before fixture changes.')
    parser.add_argument('--shell-listing', type=Path, required=True,
                        help='NASM listing matching SYSTEM/SHELL.COM in that image.')
    parser.add_argument('--shell', type=Path,
                        help='Optional candidate SHELL.COM, injected only into the image copy.')
    parser.add_argument('--kernel', type=Path,
                        help='Optional matching-profile CIUKIDOS.SYS for the isolated copy.')
    parser.add_argument('--output', type=Path, required=True)
    run(parser.parse_args())


if __name__ == '__main__':
    main()
