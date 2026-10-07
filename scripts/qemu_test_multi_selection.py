#!/usr/bin/env python3
"""Drive real marquee/Ctrl selection and verify multi-file operations on FAT."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import time

from PIL import Image
from qemu_test_desktop_apps import Gate
from qemu_test_installed_hdd import FAT16
from qemu_test_native_windows import WindowVM


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    before = digest(args.image)
    disk = out / 'disk.img'
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    subprocess.run(['mmd', '-i', volume, '::QABOX', '::QACOPY', '::QADESK',
                    '::NESTSRC', '::NESTCOPY', '::NESTMOVE',
                    '::NESTSRC/TREE', '::NESTSRC/TREE/LEVEL1',
                    '::NESTSRC/TREE/LEVEL1/LEVEL2',
                    '::NESTSRC/TREE/LEVEL1/LEVEL2/LEVEL3'], check=True)
    subprocess.run(['mdel', '-i', volume, '::SYSTEM/UI/ICONPOS.DAT'], capture_output=True)
    fixtures = {'A.TXT': b'alpha\r\n', 'B.TXT': b'bravo\r\n', 'C.TXT': b'charlie\r\n',
                'BOXA.TXT': b'desktop alpha\r\n', 'BOXB.TXT': b'desktop bravo\r\n'}
    for name, contents in fixtures.items():
        source = out / name
        source.write_bytes(contents)
        target = '::DESKTOP/' if name.startswith('BOX') else '::QABOX/'
        subprocess.run(['mcopy', '-o', '-i', volume, str(source), target + name], check=True)
    nested = {
        'TREE/ROOT.TXT': b'nested root payload\r\n',
        'TREE/LEVEL1/ONE.BIN': bytes(range(256)) * 3,
        'TREE/LEVEL1/LEVEL2/TWO.DAT': b'level two\x00payload\xff\r\n',
        'TREE/LEVEL1/LEVEL2/LEVEL3/DEEPEST.TXT': b'four levels down\r\n',
        'SIBLING.BIN': b'sibling payload\x00with binary\xff\r\n',
    }
    nested_hashes = {name: hashlib.sha256(data).hexdigest() for name, data in nested.items()}
    for name, contents in nested.items():
        source = out / ('nested-' + name.replace('/', '_'))
        source.write_bytes(contents)
        subprocess.run(['mcopy', '-o', '-i', volume, str(source),
                        '::NESTSRC/' + name], check=True)
    report = {'passed': False, 'physical_tested': False}
    vm = None

    def marquee(name, x0, y0, x1, y1):
        vm.position(x0, y0)
        vm.hmp('mouse_button 1')
        time.sleep(.25)
        for step in range(1, 7):
            vm.position(x0 + (x1-x0)*step//6, y0 + (y1-y0)*step//6)
            time.sleep(.15)
        time.sleep(.5)
        image = Image.open(vm.shot(name + '-held')).convert('RGB')
        # A border point in blank space must use the selection colour.
        left, top = min(x0, x1), min(y0, y1)
        samples = [image.getpixel((x, top)) for x in range(left, max(x0, x1)+1)]
        assert sum(max(c)-min(c) > 30 for c in samples) > 8, 'marquee border was not drawn'
        vm.hmp('mouse_button 0')
        time.sleep(.6)
        vm.shot(name + '-released')

    try:
        vm = WindowVM(disk, out, 'std', memory=512, palette='platinum')
        vm.ready()
        gate = Gate(vm)
        vm.key('meta_l-d')
        time.sleep(.7)
        text = gate.serial(0)
        icons = {name: (int(x), int(y)) for name, x, y in
                 re.findall(r'\[DESK\] icon (\S+) (\d+) (\d+)', text)}
        with Image.open(vm.shot('right-default')) as screen:
            width = screen.width
        assert icons['BOXA.TXT'][0] > width // 2 and icons['BOXB.TXT'][0] > width // 2, icons
        report['default_icons'] = icons
        ax, ay = icons['BOXA.TXT']; bx, by = icons['BOXB.TXT']
        marquee('desktop-marquee', min(ax, bx)-70, min(ay, by)-24,
                max(ax, bx)+38, max(ay, by)+60)
        vm.key('ctrl-c')
        gate.act('Paste desktop selection', ['f3', 'type:files c:\\qadesk', 'ret', 'ctrl-v'],
                 '[FILES] job 2 item(s) copied.', 60)
        vm.key('alt-f4')
        text = gate.act('Files fixture', ['f3', 'type:files c:\\qabox', 'ret'],
                        ['[FILES] list C:\\QABOX 3', '[FILES] geometry'])
        lx, ry, _, _ = map(int, re.findall(r'\[FILES\] geometry ([\d ]+)', text)[-1].split()[:4])
        marquee('files-marquee', lx+140, ry+82, lx+12, ry+3)
        # Remove only C with Ctrl; A and B remain selected.
        vm.position(lx+45, ry+2*18+9)
        vm.hmp('sendkey ctrl 1500')
        vm.hmp('mouse_button 1'); time.sleep(.15)
        vm.hmp('mouse_button 0'); time.sleep(.3)
        time.sleep(1.2)
        gate.act('Copy two selected files', ['ctrl-c', 'alt-d', 'ctrl-a',
                                           'type:c:\\qacopy', 'ret', 'ctrl-v'],
                 '[FILES] job 2 item(s) copied.', 60)
        vm.shot('copied-two')

        def files_at(label, path, count):
            text = gate.act(label, ['alt-f4', 'f3', 'type:files ' + path, 'ret'],
                            [f'[FILES] list {path.upper()} {count}', '[FILES] geometry'], 60)
            geometry = [int(v) for v in re.findall(r'\[FILES\] geometry ([\d ]+)', text)[-1].split()[:4]]
            return geometry

        def select_two(label, path):
            lx, ry, _, _ = files_at(label, path, 2)
            marquee(label.lower().replace(' ', '-') + '-selection',
                    lx+140, ry+45, lx+12, ry+3)

        select_two('Open nested source', 'c:\\nestsrc')
        gate.act('Copy nested tree and sibling', ['ctrl-c', 'alt-d', 'ctrl-a',
                 'type:c:\\nestcopy', 'ret', 'ctrl-v'],
                 '[FILES] job 9 item(s) copied.', 90)
        select_two('Open nested copy', 'c:\\nestcopy')
        gate.act('Move copied tree and sibling', ['ctrl-x', 'alt-d', 'ctrl-a',
                 'type:c:\\nestmove', 'ret', 'ctrl-v'],
                 '[FILES] job 2 item(s) moved.', 90)
        select_two('Open nested move target', 'c:\\nestmove')
        gate.act('Confirm recursive recycle', ['delete'],
                 '[FILES] confirm Are you sure you want to send these 2 items', 30)
        gate.act('Recycle nested tree and sibling', ['ret'],
                 ['[FILES] recycled 2', '[FILES] list C:\\NESTMOVE 0'], 90)
        serial = gate.serial(0)
        for marker in ('[FILES] error', '[FILES] job failed', '[FILES] media error'):
            assert marker not in serial, f'unexpected Files error marker: {marker}'
        report['nested_operation_log_checked'] = True
        report['passed'] = True
        report['steps'] = gate.steps
    except Exception as error:
        report['error'] = repr(error)
        if vm:
            try: vm.shot('failure')
            except Exception: pass
    finally:
        if vm: vm.close()
        filesystem = FAT16(disk)
        if report['passed']:
            for name in ('A.TXT', 'B.TXT'):
                assert filesystem.read('QACOPY/' + name) == fixtures[name]
            try:
                filesystem.entry('QACOPY/C.TXT')
            except KeyError:
                pass
            else:
                raise AssertionError('Ctrl toggle failed: C.TXT was also copied')
            for name in ('BOXA.TXT', 'BOXB.TXT'):
                assert filesystem.read('QADESK/' + name) == fixtures[name]
            for name, contents in nested.items():
                assert hashlib.sha256(filesystem.read('NESTSRC/' + name)).hexdigest() == nested_hashes[name], name
            def children(path):
                return set(filesystem.entries(filesystem.entry(path)[1])) - {'.', '..'}
            assert children('NESTSRC') == {'TREE', 'SIBLING.BIN'}
            assert not children('NESTCOPY')
            assert not children('NESTMOVE')
            recycled = filesystem.entries(filesystem.entry('RECYCLED')[1])
            tree_roots = []
            for name, (attr, _, _) in recycled.items():
                if not (attr & 0x10):
                    continue
                try:
                    if hashlib.sha256(filesystem.read('RECYCLED/' + name + '/LEVEL1/ONE.BIN')).hexdigest() == nested_hashes['TREE/LEVEL1/ONE.BIN']:
                        tree_roots.append(name)
                except KeyError:
                    continue
            assert len(tree_roots) == 1, f'recycled nested tree not found: {tree_roots}'
            recycled_tree = tree_roots[0]
            for name, contents in nested.items():
                if name.startswith('TREE/'):
                    relative = name[len('TREE/'):]
                    assert hashlib.sha256(filesystem.read('RECYCLED/' + recycled_tree + '/' + relative)).hexdigest() == nested_hashes[name], name
            recycled_siblings = []
            for name, (attr, _, _) in recycled.items():
                if attr & 0x10:
                    continue
                try:
                    if hashlib.sha256(filesystem.read('RECYCLED/' + name)).hexdigest() == nested_hashes['SIBLING.BIN']:
                        recycled_siblings.append(name)
                except KeyError:
                    continue
            assert len(recycled_siblings) == 1, 'recycled sibling payload not found exactly once'
            report['fat_verified'] = True
            report['nested_files'] = nested_hashes
        report['source_unchanged'] = digest(args.image) == before
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] and report['source_unchanged'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
