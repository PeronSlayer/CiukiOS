#!/usr/bin/env python3
"""Verify the default photo wallpaper on the full CD-only boot path.

The guest has no hard disk: QEMU boots the read-only full-CD ISO, with KVM,
Pentium III, std VGA, and 512 MiB. The harness checks the partition's shipped
shell/assets and samples the actual desktop screenshot against Ciuk1.
Run after a full CD build; this script does not build or alter source images.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import uuid

import numpy as np
from PIL import Image

from qemu_test_full_display_profile import VM
from qemu_test_installed_hdd import FAT16
from qemu_test_photo_wallpaper import check_assets, sha256


ROOT = Path(__file__).resolve().parents[1]


class CDVM(VM):
    """Full-display monitor helpers with an optical-only boot configuration."""

    def __init__(self, iso, output):
        self.output = output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.serial = self.output / 'serial.log'
        self.sock = Path(tempfile.gettempdir()) / f'ciukios-cd-wallpaper-{uuid.uuid4().hex}.sock'
        self.err = (self.output / 'qemu.stderr.log').open('w')
        qemu = os.environ.get('QEMU_BIN', 'qemu-system-i386')
        args = [
            qemu,
            '-accel', 'kvm',
            '-machine', 'pc,vmport=off,i8042=on,pcspk-audiodev=snd0',
            '-cpu', 'pentium3', '-m', '512',
            '-device', 'virtio-rng-pci,disable-modern=on,disable-legacy=off',
            '-cdrom', str(iso.resolve()), '-boot', 'd', '-snapshot',
            '-netdev', 'user,id=ciuknet0',
            '-device', 'ne2k_pci,netdev=ciuknet0,mac=52:54:00:12:34:56',
            '-vga', 'std',
            '-audiodev', 'none,id=snd0',
            '-device', 'AC97,audiodev=snd0',
            '-device', 'sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=snd0',
            '-device', 'adlib,audiodev=snd0',
            '-display', 'none', '-serial', f'file:{self.serial}',
            '-monitor', f'unix:{self.sock},server,nowait', '-no-reboot', '-no-shutdown',
        ]
        self.process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=self.err)
        deadline = time.monotonic() + 15
        while not self.sock.exists():
            assert self.process.poll() is None, (self.output / 'qemu.stderr.log').read_text()
            assert time.monotonic() < deadline, 'QEMU monitor socket did not appear'
            time.sleep(.05)

    def ready(self, offset=0, timeout=120):
        self.wait('[DESKTOP] READY', offset, timeout)


def assemble_shell(output):
    shell = output / 'fresh-SHELL.COM'
    listing = output / 'fresh-SHELL.lst'
    subprocess.run(['nasm', '-f', 'bin', '-l', str(listing), '-o', str(shell),
                    'src/com/shell.asm'], cwd=ROOT, check=True)
    return shell, listing


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def c_div2(value):
    """Match the renderer's signed C division when centering cropped images."""
    return value // 2 if value >= 0 else -((-value) // 2)


def marker_photo_check(frame, source):
    height, width = frame.shape[:2]
    src_h, src_w = source.shape[:2]
    work_h = height - 61
    assert (width, height) in ((640, 480), (800, 600), (1024, 768), (1280, 800)), \
        f'unexpected native desktop geometry {(width, height)}'
    # Independent Fill geometry: cover the desktop work area, then center-crop.
    by_width = width * src_h >= work_h * src_w
    if by_width:
        draw_w = width
        draw_h = (src_h * width + src_w - 1) // src_w
    else:
        draw_h = work_h
        draw_w = (src_w * work_h + src_h - 1) // src_h
    left = c_div2(width - draw_w)
    top = 29 + c_div2(work_h - draw_h)
    reports = []
    # Keep samples away from the initial pointer and the left-side desktop icons.
    points = [(int(width * fx), 29 + int(work_h * fy))
              for fy in (.13, .28, .43, .58, .73, .86)
              for fx in (.34, .46, .58, .70, .82, .92)]
    for x, y in points:
        sx = min(src_w - 1, max(0, (x - left) * src_w // draw_w))
        sy = min(src_h - 1, max(0, (y - top) * src_h // draw_h))
        patch = source[max(0, sy - 1):min(src_h, sy + 2),
                       max(0, sx - 1):min(src_w, sx + 2)].astype(np.int16)
        observed = frame[y, x].astype(np.int16)
        delta = np.max(np.abs(patch - observed), axis=2)
        reports.append({'screen': [x, y], 'source': [sx, sy],
                        'max_channel_delta': int(delta.min())})
    deltas = [point['max_channel_delta'] for point in reports]
    return {
        'frame': [width, height], 'style': 'Fill',
        'photo_rect': [0, 29, width, work_h], 'draw_rect': [left, top, draw_w, draw_h],
        'sample_count': len(reports), 'median_delta': float(np.median(deltas)),
        'max_delta': max(deltas), 'samples': reports,
    }


def save_failure(vm, output, report):
    if vm is None:
        return
    try:
        vm.shot('failure')
    except Exception as exc:
        report['failure_screenshot_error'] = str(exc)
    for name, command in (('registers.txt', 'info registers'),
                          ('pic.txt', 'info pic'), ('pci.txt', 'info pci')):
        try:
            (output / name).write_bytes(vm.hmp(command))
        except Exception as exc:
            report[f'{name}_error'] = str(exc)
    ram = output / 'failure-ram-1m.bin'
    try:
        vm.hmp(f'pmemsave 0 0x100000 "{ram}"')
        if ram.is_file() and ram.stat().st_size != 0x100000:
            report['failure_ram_bytes'] = ram.stat().st_size
    except Exception as exc:
        report['failure_ram_error'] = str(exc)
    if vm.serial.exists():
        (output / 'failure-serial.log').write_bytes(vm.serial.read_bytes())
        normalized = subprocess.run(['scripts/serial_log_normalize.py', str(vm.serial)],
                                    cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        (output / 'failure-serial-normalized.log').write_bytes(normalized.stdout)
    vm.err.flush()
    report['failure_qemu_stderr'] = 'qemu.stderr.log'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, default=Path('build/full/ciukios-full-cd.iso'))
    parser.add_argument('--partition', type=Path,
                        default=Path('build/full/ciukios-full-cd-partition.img'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=90,
                        help='seconds allowed for WALLP.APP to report Ciuk1 ready')
    args = parser.parse_args()
    iso, partition, output = args.iso.resolve(), args.partition.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report_path = output / 'report.json'
    assert not report_path.exists(), 'preserve existing evidence; choose a fresh output directory'
    report = {
        'status': 'running', 'scope': 'CD-only QEMU boot; emulator evidence, no physical-hardware claim',
        'physical_hardware_qualified': False, 'qemu_memory_mib': 512,
        'boot_drive': 'read-only CD ISO; no hard-disk device',
        'iso': str(iso), 'iso_sha256': file_sha256(iso),
        'partition_image': str(partition), 'partition_sha256': file_sha256(partition),
        'checks': [],
    }
    def save():
        report_path.write_text(json.dumps(report, indent=2) + '\n')

    vm = None
    try:
        fs = FAT16(partition)
        shell, listing = assemble_shell(output)
        shipped_shell = fs.read('SYSTEM/SHELL.COM')
        assert shell.read_bytes() == shipped_shell, \
            'fresh NASM SHELL.COM does not match the CD FAT16 partition copy'
        records, photos = check_assets(fs)
        photo = photos['Ciuk1']
        catalog = fs.read('SYSTEM/UI/WALLS.DAT')
        selected = fs.read('SYSTEM/UI/WALL.CFG')
        wallp = fs.read('SYSTEM/APPS/WALLP.APP')
        assert len(wallp) >= 22 and wallp[:4] == b'CAPP', \
            f'CD WALLP.APP is absent, truncated, or has an invalid CAPP header: {len(wallp)} bytes'
        wallp_paras = int.from_bytes(wallp[6:8], 'little')
        wallp_entry = int.from_bytes(wallp[12:14], 'little')
        assert 32 <= wallp_paras <= 4096 and len(wallp) <= wallp_paras * 16 - 0x100, \
            f'CD WALLP.APP size/header mismatch: {len(wallp)} bytes, {wallp_paras} paragraphs'
        assert 0x116 <= wallp_entry < 0x100 + len(wallp), \
            f'CD WALLP.APP has an invalid entry offset 0x{wallp_entry:04X}'
        assert selected[:1] == bytes([photo['index']]), \
            f'fresh CD profile does not default to Ciuk1: {selected!r}'
        report.update({
            'shell_sha256': sha256(shipped_shell),
            'fresh_shell_sha256': sha256(shell.read_bytes()),
            'shell_listing_sha256': sha256(listing.read_bytes()),
            'wall_catalog_sha256': sha256(catalog),
            'wallpaper_config': selected.hex(),
            'catalog_entries': len(records),
            'wallp_app': {'bytes': len(wallp), 'sha256': sha256(wallp),
                          'header': wallp[:22].hex(), 'paragraphs': wallp_paras,
                          'entry': wallp_entry},
            'Ciuk1': {'index': photo['index'], 'filename': photo['filename'],
                      'cwp_sha256': photo['sha256'], 'source_sha256': photo['source_sha256']},
        })
        report['checks'].append('fresh shell and WALLP CAPP image plus Ciuk1 CWP/catalog/default match CD partition')
        save()

        vm = CDVM(iso, output)
        report['qemu_command'] = vm.process.args
        report['qemu_version'] = subprocess.check_output(
            [os.environ.get('QEMU_BIN', 'qemu-system-i386'), '--version'], text=True).splitlines()[0]
        save()
        boot_started = time.monotonic()
        vm.ready(timeout=120)
        report['desktop_ready_seconds'] = time.monotonic() - boot_started
        vm.wait('[DESK] icons', 0, 30)
        vm.wait('[DESK] wallpaper module loaded', 0, 30)
        vm.wait('[ABOUT] open', 0, 30)
        report['checks'].append('desktop ready, desktop module loaded, and startup About opened')
        save()

        # The startup About window receives Escape through the normal keyboard
        # path, as users do; no click or guest-memory manipulation is involved.
        close_offset = vm.offset()
        vm.key('esc')
        vm.wait('[DESKTOP] WINDOW 02 CLOSE', close_offset, 20)
        report['about_dismissed_by_escape'] = True
        wallpaper_marker = f'[WALLP] ready {photo["filename"]}'
        vm.wait(wallpaper_marker, 0, args.timeout)
        report['wallpaper_ready_seconds_since_qemu_start'] = time.monotonic() - boot_started
        vm.wait('[DESKTOP] PAINT', close_offset, 20)

        # Park the emulated pointer in the upper-left corner, away from all
        # sample locations, then capture the real composited desktop.
        for _ in range(12):
            vm.hmp('mouse_move -127 -127 0')
        time.sleep(.5)
        shot = vm.shot('desktop-Ciuk1')
        with Image.open(shot) as image:
            frame = np.asarray(image.convert('RGB'), dtype=np.uint8)
        metrics = marker_photo_check(frame, photo['rgb'])
        assert metrics['sample_count'] >= 30 and metrics['max_delta'] <= 48 and \
            metrics['median_delta'] <= 24, metrics
        report['rendered_Ciuk1'] = metrics
        report['checks'].append('actual desktop screenshot matches centered Fill samples from Ciuk1 RGB')
        report['desktop_screenshot'] = shot.name
        report['status'] = 'passed'
        save()
        print('[cd-wallpaper] PASS', json.dumps(report['checks']), flush=True)
    except Exception as exc:
        report['status'] = 'failed'
        report['error'] = f'{type(exc).__name__}: {exc}'
        save()
        save_failure(vm, output, report)
        save()
        raise
    finally:
        if vm is not None:
            try:
                vm.close()
            except Exception:
                try:
                    vm.process.terminate()
                    vm.process.wait(timeout=10)
                except Exception:
                    vm.process.kill()
                    vm.process.wait(timeout=10)
                vm.err.close()
                vm.sock.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
