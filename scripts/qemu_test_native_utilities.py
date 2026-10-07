#!/usr/bin/env python3
"""Utilities: observe SHELL.COM's state in guest RAM and click its controls.

RAM is read only (through the listing) to locate rendered controls and assert
state; all guest actions are real mouse/keyboard events. Used by the desktop
gates. Files, Tasks and Notepad are separate modules since 2026-09-29; their
gate is scripts/qemu_test_desktop_apps.py.
"""
import re
import struct
import time
from pathlib import Path

from qemu_test_ui_regressions import ui_hit_binding


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

    def until(self, predicate, description, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.refresh()
            if predicate():
                return
            time.sleep(.08)
        raise AssertionError(f'{description}; active={self.b("ui_active_window")}')

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
            width, height = self.w('ui_width'), self.w('ui_height')
            for px, py in (((x + right) // 2, (y + bottom) // 2),
                           (x + 3, y + 3), (right - 4, bottom - 4)):
                # Leave the same 24x16 safety margin used by the old 800x600
                # test, while allowing the current 1024x768 profile.
                if not (0 <= px < width - 24 and 0 <= py < height - 16):
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


if __name__ == '__main__':
    raise SystemExit('Files and Tasks are desktop modules now: run scripts/qemu_test_desktop_apps.py')
