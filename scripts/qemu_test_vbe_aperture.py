#!/usr/bin/env python3
"""Gate full-HDD VBE scanout paths and visible damage below 64 KiB rows.

Runs three isolated QEMU HDD boots: standard and Cirrus VGA with CVSESSION's
protected LFB row/fill transport, and standard
VGA with STARTUP.CFG=NOVM using the real-mode LFB path. Emulator results do
not qualify physical hardware.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import time
from pathlib import Path

import numpy as np
from PIL import Image

from qemu_test_dos_window import Session
from qemu_test_installed_hdd import FAT16
from qemu_test_native_windows import WindowVM

ROOT = Path(__file__).resolve().parents[1]
PROFILE = b'1024'
CASES = (
    dict(name='std-vm', vga='std', startup=b'LIVE', lfb=2, bound=1, lfb_ok=1),
    # Cirrus firmware also supplies a genuine linear mode. The new transport
    # must prefer that verified mode over the previous banked aperture alias.
    dict(name='cirrus-vm', vga='cirrus', startup=b'LIVE', lfb=2, bound=1, lfb_ok=1),
    dict(name='std-novm-lfb', vga='std', startup=b'NOVM', lfb=1, bound=0),
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def assemble_shell(output):
    shell = output / 'assembled-SHELL.COM'
    listing = output / 'SHELL.lst'
    subprocess.run([
        'nasm', '-f', 'bin', '-l', str(listing), '-o', str(shell),
        'src/com/shell.asm',
    ], cwd=ROOT, check=True)
    return shell, listing


def inject_profiles(disk, case):
    fs = FAT16(disk)
    volume = f'{disk}@@{fs.start}'
    cfg = disk.parent / 'DISPLAY.CFG'
    startup = disk.parent / 'STARTUP.CFG'
    cfg.write_bytes(PROFILE)
    startup.write_bytes(case['startup'])
    subprocess.run(['mcopy', '-o', '-i', volume, str(cfg),
                    '::SYSTEM/VIDEO/DISPLAY.CFG'], check=True)
    subprocess.run(['mcopy', '-o', '-i', volume, str(startup),
                    '::SYSTEM/STARTUP.CFG'], check=True)
    check = FAT16(disk)
    assert check.read('SYSTEM/VIDEO/DISPLAY.CFG') == PROFILE
    assert check.read('SYSTEM/STARTUP.CFG') == case['startup']
    return check.read('SYSTEM/SHELL.COM')


def save_frame(vm, name):
    ppm = vm.shot(name, (1024, 768))
    png = ppm.with_suffix('.png')
    with Image.open(ppm) as image:
        rgb = image.convert('RGB')
        rgb.save(png)
        return np.asarray(rgb).copy(), png


def close_rgb(actual, expected, tolerance=10):
    return np.all(np.abs(actual.astype(np.int16) -
                         np.asarray(expected, dtype=np.int16)) <= tolerance,
                  axis=-1)


def drag_run(vm, ui, target_y):
    # There is no DOS child in this gate. Locate the exposed title directly
    # instead of depending on the DOS-session harness's state aliases.
    ui.until(lambda: ui.b('ui_composing') == 0, 'desktop paint did not settle')
    hits = ui.hit_records()
    for index in range(len(hits) - 1, -1, -1):
        x, y, right, bottom, action = hits[index]
        if action != (62 | (2 << 8)):
            continue
        px, py = (x + right) // 2, (y + bottom) // 2
        front = next(i for i in range(len(hits) - 1, -1, -1)
                     if hits[i][0] <= px < hits[i][2]
                     and hits[i][1] <= py < hits[i][3])
        if front == index:
            break
    else:
        raise AssertionError('Run title has no exposed drag control')
    current_y = ui.geometry(1)[1]
    vm.position(px, py)
    vm.hmp('mouse_button 1')
    time.sleep(.2)
    vm.move(0, (target_y - current_y + 1) // 2)
    time.sleep(.3)
    vm.hmp('mouse_button 0')
    time.sleep(.3)


def verify_moved_frame(vm, frame, before, ui):
    x, y = ui.geometry(1)
    width = ui.w('ui_window_width', 1)
    height = ui.w('ui_window_height', 1)
    # The real mouse handler keeps the complete Run window and taskbar on
    # screen. At 1024x768 this is 768 - 226 - 37 = 505; its content starts
    # below scanline 512 while preserving a visible taskbar.
    max_y = ui.w('ui_height') - height - 37
    assert y == max_y == 505, f'Run window did not reach legal lower bound: y={y}, max={max_y}'
    assert y + 40 > 512, f'Run window client did not enter lower frame region: y={y}'

    paper = (247, 247, 243)
    title = (52, 72, 120)
    top = frame[y, x:x + width]
    left = frame[y:y + height, x]
    title_pixels = frame[y + 3:y + 28, x + 4:x + width - 4]
    top_count = int(close_rgb(top, paper).sum())
    left_count = int(close_rgb(left, paper).sum())
    title_count = int(close_rgb(title_pixels, title).sum())
    assert top_count >= width - 12, f'actual top bevel missing/shifted: {top_count}/{width}'
    assert left_count >= height - 40, f'actual left bevel missing/shifted: {left_count}/{height}'
    assert title_count > 1000, f'active title color missing at moved location: {title_count}'

    lower = frame[512:]
    changed_lower = int(np.count_nonzero(np.any(lower != before[512:], axis=2)))
    assert changed_lower > 1000, f'window move changed too few lower-frame pixels: {changed_lower}'

    taskbar_y = frame.shape[0] - 32
    taskbar_paper = int(close_rgb(frame[taskbar_y], paper).sum())
    assert taskbar_paper >= frame.shape[1] - 8, (
        f'taskbar top rule is not visible at the bottom: {taskbar_paper}')
    return dict(origin=[x, y], dimensions=[width, height],
                content_top=y + 40, content_crosses_scanline=512,
                bevel=[x, y, width],
                bevel_pixels={'top_paper': top_count, 'left_paper': left_count,
                              'active_title': title_count},
                changed_pixels_below_512=changed_lower,
                taskbar_rule_paper_pixels=taskbar_paper)


def run_case(source_image, shell, listing, root_output, case):
    case_dir = root_output / case['name']
    case_dir.mkdir(parents=True)
    disk = case_dir / 'private.img'
    shutil.copyfile(source_image, disk)
    shipped_shell = inject_profiles(disk, case)
    assert shipped_shell == shell.read_bytes(), (
        f'{case["name"]}: assembled SHELL.COM does not match the shipped FAT16 copy')

    expected = {'vc_active': 1, 'ui_vbe': 1, 'ui_width': 1024,
                'ui_height': 768, 'vc_lfb': case['lfb'],
                'vc_session_bound': case['bound']}
    if 'lfb_ok' in case:
        expected['vc_lfb_ok'] = case['lfb_ok']
    record = dict(name=case['name'], vga=case['vga'], startup=case['startup'].decode(),
                  display_profile=PROFILE.decode(), shell_sha256=hashlib.sha256(shipped_shell).hexdigest(),
                  expected=expected)
    vm = WindowVM(disk, case_dir, case['vga'], memory=128, palette='platinum')
    ui = None
    try:
        record['qemu_args'] = vm.process.args
        vm.ready()
        ui = Session(vm, shipped_shell, listing,
                     dict(events=[], cases=[]))
        ui.until(lambda: ui.b('vc_active') == 1 and ui.b('ui_vbe') == 1,
                 'VBE desktop did not become active', 30)
        observed = {
            'vc_active': ui.b('vc_active'),
            'ui_vbe': ui.b('ui_vbe'),
            'ui_width': ui.w('ui_width'),
            'ui_height': ui.w('ui_height'),
            'vc_lfb': ui.b('vc_lfb'),
            'vc_session_bound': ui.b('vc_session_bound'),
            'vc_lfb_ok': ui.b('vc_lfb_ok'),
        }
        assert all(observed.get(key) == value for key, value in record['expected'].items()), (
            f'{case["name"]}: framebuffer path mismatch', observed, record['expected'])
        desktop, desktop_png = save_frame(vm, 'desktop')
        record['desktop_png'] = desktop_png.name
        record['desktop_size'] = list(desktop.shape[1::-1])

        vm.key('f3')
        ui.until(lambda: ui.b('ui_active_window') == 1,
                 'F3 did not open the native Run window')
        time.sleep(.4)
        before, before_png = save_frame(vm, 'run-before')
        # Session.drag takes desired screen-pixel deltas and sends half-sized
        # PS/2 packets because this desktop maps each packet to two pixels.
        x, y = ui.geometry(1)
        target_y = ui.w('ui_height') - ui.w('ui_window_height', 1) - 37
        drag_run(vm, ui, target_y)
        ui.until(lambda: ui.w('ui_window_y', 1) == target_y,
                 'Run window did not reach the lower legal screen position')
        time.sleep(.35)
        moved, moved_png = save_frame(vm, 'run-moved')
        record['run_before_png'] = before_png.name
        record['run_moved_png'] = moved_png.name
        record['lower_frame'] = verify_moved_frame(vm, moved, before, ui)
        record['observed'] = observed
        record['status'] = 'pass'
        return record
    except Exception as exc:
        record.update(status='failed', error=repr(exc))
        try:
            save_frame(vm, 'failure')
        except Exception as capture_error:
            record['failure_capture_error'] = repr(capture_error)
        raise
    finally:
        vm.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, default=Path('build/tests/vbe-aperture'))
    args = parser.parse_args()
    source_image = args.image.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report_path = output / 'results.json'
    assert not report_path.exists(), 'preserve existing evidence; choose a fresh output directory'

    original_sha = sha(source_image)
    shell, listing = assemble_shell(output)
    source_fs = FAT16(source_image)
    shipped_shell = source_fs.read('SYSTEM/SHELL.COM')
    assert shell.read_bytes() == shipped_shell, (
        'assembled SHELL.COM does not match the source HDD FAT16 copy')
    report = dict(
        scope='QEMU full-HDD renderer gate; emulator evidence only',
        physical_hardware_qualified=False,
        source_image_sha256=original_sha,
        shell_sha256=hashlib.sha256(shipped_shell).hexdigest(),
        shell_listing=listing.name,
        shell_listing_sha256=sha(listing),
        guest_memory_writes=False,
        cases=[],
    )
    try:
        for case in CASES:
            try:
                result = run_case(source_image, shell, listing, output, case)
            except Exception as exc:
                report['cases'].append(dict(name=case['name'], status='failed',
                                            error=repr(exc)))
                raise
            report['cases'].append(result)
            report_path.write_text(json.dumps(report, indent=2) + '\n')
            print(f"[vbe-aperture] PASS {case['name']}", flush=True)
        report['status'] = 'pass'
    except Exception as exc:
        report.update(status='failed', error=repr(exc))
        raise
    finally:
        report['source_image_unchanged'] = sha(source_image) == original_sha
        report_path.write_text(json.dumps(report, indent=2) + '\n')
    assert report.get('status') == 'pass', report.get('error')


if __name__ == '__main__':
    main()
