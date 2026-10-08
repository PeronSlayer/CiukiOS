#!/usr/bin/env python3
"""Verify pristine AUTO boot, native startup DMA and keyboard return in QEMU.

The only guest changes are those the shipping OS makes during its own boot
and normal keyboard input. VGA EDID and audio devices are configured before
boot. Run this gate in a capped scope, sequentially with other QEMU/build jobs.
This is emulator evidence; it does not qualify the physical T23 speakers/GPU.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

import numpy as np
from PIL import Image

from analyze_audio_wav import pcm_payload
from inspect_boot_hardware import parse_log
from qemu_test_full_display_profile import VM
from qemu_test_installed_hdd import FAT16, listing_address
from qemu_test_photo_wallpaper import (check_assets, desktop_icon_boxes,
                                       fit_bar_samples, fit_samples)


ROOT = Path(__file__).resolve().parents[1]
SHELL_PATH = 'SYSTEM/SHELL.COM'
SFX_PATH = 'SYSTEM/DRIVERS/SFX.DRV'
AUDIO_PATH = 'SYSTEM/AUDIO.LOG'
DISPLAY_LOG_PATH = 'SYSTEM/VIDEO/DISPLAY.LOG'
WP_FIT = 1


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def file_sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def optional_file(fs, path):
    try:
        return fs.read(path)
    except KeyError:
        return b''


def read_live(volume, path):
    result = subprocess.run(['mtype', '-i', volume, '::' + path],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=5)
    return result.stdout if result.returncode == 0 else b''


def native_audio_records(raw):
    records = []
    for line in raw.decode('ascii', 'replace').splitlines():
        if not line.startswith('[AUDIO] native '):
            continue
        fields = dict(re.findall(r'([a-z]+)=([^\s]+)', line))
        record = {'line': line, 'event': fields.get('event')}
        for key, value in fields.items():
            if key == 'event':
                continue
            if key in ('pci', 'codec'):
                record[key] = value
            else:
                try:
                    record[key] = int(value, 16)
                except ValueError:
                    record[key] = value
        records.append(record)
    return records


def new_log_bytes(raw, baseline):
    return raw[len(baseline):] if raw.startswith(baseline) else raw


def wait_native_audio(volume, baseline, timeout=20):
    deadline = time.monotonic() + timeout
    latest = b''
    while time.monotonic() < deadline:
        latest = read_live(volume, AUDIO_PATH)
        records = native_audio_records(new_log_bytes(latest, baseline))
        if any(record['event'] in ('E', 'F', 'X') for record in records):
            return latest, records
        if any(record['event'] == 'I' and record.get('result') != 0 for record in records):
            return latest, records
        time.sleep(.3)
    raise AssertionError('native startup did not produce EOF/failure within the bounded log wait; '
                         f'last records={native_audio_records(new_log_bytes(latest, baseline))}')


def verify_native_audio(records, boot_bytes):
    by_event = {event: [record for record in records if record['event'] == event]
                for event in ('I', 'S', 'E', 'F', 'X')}
    assert len(by_event['I']) == len(by_event['S']) == len(by_event['E']) == 1, records
    assert not by_event['F'] and not by_event['X'], records
    init, start, end = (by_event[event][0] for event in ('I', 'S', 'E'))
    assert init.get('stage') == 7 and init.get('result') == 0, init
    assert init.get('pci') == '8086:2415', init   # actual QEMU AC97 controller
    assert init.get('codec') not in (None, '0000:0000', 'FFFF:FFFF'), init
    assert init.get('power', 0) & 0x000E == 0x000E, init
    assert all(init.get(key, 0xFFFF) & 0x8000 == 0 for key in ('master', 'hp', 'pcm')), init
    assert init.get('ring', 0) != 0, init
    assert start.get('result') == 1 and start.get('total') == boot_bytes, start
    assert 0 < start.get('loaded', 0) <= 31 * 2048, start
    assert end.get('result') == 0 and end.get('total') == boot_bytes, end
    assert end.get('loaded') == end.get('played') == boot_bytes > 65536, end
    assert end.get('sr', 0) & 5 == 5 and not end.get('sr', 0) & 0x10, end
    assert 0 <= end.get('civ', -1) <= 31, end
    return {'controller': init['pci'], 'codec': init['codec'],
            'boot_pcm_bytes': boot_bytes, 'dma_fetched_bytes': end['played'],
            'initial_primed_bytes': start['loaded'], 'completion_sr': end['sr'],
            'completion_civ': end['civ'], 'queue_resumptions': end.get('restarts', 0),
            'speaker_output_measured': False}


def audio_idle_snapshot(vm, output, name, shell_image, shell_listing, sfx_image, sfx_listing):
    """Read the actual shell-owned SFX allocation while its guest CPU is paused."""
    path = output / (name + '-ram-1m.bin')
    vm.hmp('stop')
    try:
        vm.hmp(f'pmemsave 0 0x100000 "{path}"')
        ram = path.read_bytes()
        assert len(ram) == 0x100000, 'incomplete conventional RAM capture'
        candidates = []
        position = 0
        while (position := ram.find(shell_image[:64], position)) >= 0:
            candidates.append(position)
            position += 1
        assert len(candidates) == 1, f'ambiguous active shell candidates: {candidates}'
        shell_at = candidates[0]
        entry_at = shell_at + listing_address(shell_listing, 'ui_sfx_entry')
        driver_segment = int.from_bytes(ram[entry_at + 2:entry_at + 4], 'little')
        driver_at = driver_segment * 16
        assert driver_segment and ram[driver_at:driver_at + 32] == sfx_image[:32], \
            'the shell-owned native SFX image does not match the shipped driver'
        fields = {}
        for symbol, size in (('sfx_available', 1), ('sfx_playing', 1), ('sfx_boot_active', 1),
                             ('sfx_music_owned', 1), ('sfx_boot_count', 2), ('sfx_boot_file', 2),
                             ('sfx_buffer', 2)):
            address = driver_at + listing_address(sfx_listing, symbol)
            fields[symbol] = int.from_bytes(ram[address:address + size], 'little')
        for symbol, size in (('ui_sfx_status', 2), ('ui_sfx_pending', 1)):
            address = shell_at + listing_address(shell_listing, symbol)
            fields[symbol] = int.from_bytes(ram[address:address + size], 'little')
        return {'ram_capture': path.name, 'shell_physical': shell_at,
                'sfx_segment': driver_segment, 'sfx_header_matches_shipped': True, 'fields': fields}
    finally:
        vm.hmp('cont')


def video_layout_snapshot(vm, output, shell_image, shell_listing, expected_height):
    """Read one-page scanline state from the live shell before entering DOS."""
    path = output / 'vbe06-layout-ram-1m.bin'
    vm.hmp('stop')
    try:
        vm.hmp(f'pmemsave 0 0x100000 "{path}"')
        ram = path.read_bytes()
        assert len(ram) == 0x100000, 'incomplete conventional RAM capture'
        candidates = []
        position = 0
        while (position := ram.find(shell_image[:64], position)) >= 0:
            candidates.append(position)
            position += 1
        assert len(candidates) == 1, f'ambiguous active shell candidates: {candidates}'
        shell_at = candidates[0]
        scan_addr = shell_at + listing_address(shell_listing, 'vc_scan_lines')
        page_addr = shell_at + listing_address(shell_listing, 'ui_page_enabled')
        scan_lines = int.from_bytes(ram[scan_addr:scan_addr + 2], 'little')
        page_enabled = ram[page_addr]
        snapshot = {'ram_capture': path.name, 'shell_physical': shell_at,
                    'vc_scan_lines': scan_lines, 'ui_page_enabled': page_enabled}
        assert scan_lines == expected_height and page_enabled == 0, snapshot
        return snapshot
    finally:
        vm.hmp('cont')


def verify_audio_idle(snapshot):
    fields = snapshot['fields']
    assert fields['sfx_available'] == 1 and fields['sfx_buffer'] != 0, snapshot
    assert all(fields[key] == 0 for key in ('sfx_playing', 'sfx_boot_active', 'sfx_music_owned',
                                           'sfx_boot_count', 'ui_sfx_pending')), snapshot
    # Shell status retains the last event's success/playing result; successful
    # idle polls deliberately do not update it. Actual driver ownership is
    # checked above, rather than treating this presentation field as DMA state.
    assert fields['ui_sfx_status'] in (0, 1), snapshot
    assert fields['sfx_boot_file'] == 0xFFFF, snapshot


def verify_video(video, expected, edid):
    assert (video['geometry']['width'], video['geometry']['height']) == expected, video
    assert video['renderer']['ui_vbe'] == 1 and video['renderer']['bpp_header'] == 32, video
    assert video['mode_info']['bpp'] == 32 and video['checks']['full_color_direct_mode'], video
    for check in ('vbe_mode_readback_matches', 'vbe_lfb_readback_matches_header',
                  'modeinfo_geometry_matches_log', 'rgb_masks_valid', 'header_bpp_matches_modeinfo'):
        assert video['checks'][check], (check, video)
    assert (video['transport_flags']['raw'] & 3 or video['session']['row_calls'] > 0), \
        'no validated native renderer path or completed protected row transfer recorded'
    if edid != 'off':
        assert video['checks']['edid_preferred_timing_valid'] and video['checks']['edid_preferred_geometry_matches_log'], video


def wait_wallpaper(vm, filename, offset=0, timeout=90):
    """READY alone precedes asynchronous photo loading; require its final paint."""
    marker = f'[WALLP] ready {filename}'
    deadline = time.monotonic() + timeout
    latest = ''
    while time.monotonic() < deadline:
        if vm.serial.exists():
            latest = subprocess.check_output([
                str(ROOT / 'scripts/serial_log_normalize.py'), '--offset', str(offset),
                str(vm.serial)]).decode('cp437', 'replace')
            ready_at = latest.rfind(marker)
            if ready_at >= 0 and latest.find('[DESKTOP] PAINT', ready_at + len(marker)) >= 0:
                return {'ready_marker': marker, 'completed_paint_after_ready': True,
                        'serial_offset': offset}
        assert vm.process.poll() is None, 'QEMU stopped while loading the wallpaper'
        time.sleep(.2)
    raise AssertionError(f'No completed desktop paint after {marker!r}; serial tail={latest[-1600:]!r}')


def photo_samples(frame, photo, icons):
    """36 independent RGB samples plus containment bars for the Fit layout."""
    height, width = frame.shape[:2]
    source = photo['rgb']
    source_h, source_w = source.shape[:2]
    view_h = height - 61
    if width * source_h <= view_h * source_w:
        draw_w = width
        draw_h = source_h * width // source_w
    else:
        draw_h = view_h
        draw_w = source_w * view_h // source_h
    left, top = (width - draw_w) // 2, 29 + (view_h - draw_h) // 2
    assert draw_w <= width and draw_h <= view_h and (draw_w < width or draw_h < view_h), \
        (draw_w, draw_h, width, view_h)
    samples = []
    for x, y in fit_samples(left, top, draw_w, draw_h, icons):
        sx = min(source_w - 1, (x - left) * source_w // draw_w)
        sy = min(source_h - 1, (y - top) * source_h // draw_h)
        expected, observed = source[sy, sx], frame[y, x]
        delta = int(np.max(np.abs(expected.astype(np.int16) - observed.astype(np.int16))))
        samples.append({'screen': [x, y], 'source': [sx, sy],
                        'expected_rgb': expected.tolist(), 'observed_rgb': observed.tolist(),
                        'max_channel_delta': delta})
    bar_points = fit_bar_samples(width, height, icons, (left, top, draw_w, draw_h))
    bar_pixels = [frame[y, x].astype(np.int16) for x, y in bar_points]
    bar_delta = max((int(np.max(np.abs(pixel - bar_pixels[0]))) for pixel in bar_pixels), default=0)
    deltas = [sample['max_channel_delta'] for sample in samples]
    return {'photo': 'Ciuk1', 'filename': photo['filename'], 'cwp_sha256': photo['sha256'],
            'source_png_sha256': photo['source_sha256'], 'frame': [width, height],
            'style': 'Fit', 'fit_rectangle': [left, top, draw_w, draw_h],
            'bar_samples': [list(point) for point in bar_points], 'bar_max_delta': bar_delta,
            'sample_count': len(samples),
            'median_delta': float(np.median(deltas)), 'max_delta': max(deltas), 'samples': samples}


def verify_photo(vm, output, name, expected, photo):
    path = vm.shot(name, expected)
    with Image.open(path) as image:
        image.save(output / (name + '.png'))
        frame = np.asarray(image.convert('RGB'), dtype=np.uint8)
    icons = desktop_icon_boxes(vm, frame.shape[1], frame.shape[0])
    evidence = photo_samples(frame, photo, icons)
    # This gate requires 32-bit RGB transport, so no palette quantization or
    # nearby-source tolerance is needed. Each sampled owner pixel must match.
    assert evidence['sample_count'] == 36 and evidence['max_delta'] == 0, evidence
    assert evidence['bar_max_delta'] <= 4, evidence
    return evidence


def waveform_analysis(path, output, filename):
    analysis = subprocess.run([sys.executable, str(ROOT / 'scripts/analyze_audio_wav.py'),
                               str(path), '--label', 'native-startup'],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    (output / filename).write_text(analysis.stdout + analysis.stderr)
    assert analysis.returncode == 0, analysis.stdout + analysis.stderr
    pcm, depth = pcm_payload(path.read_bytes())
    return {'pcm_bytes': len(pcm), 'bits_per_sample': depth,
            'pcm_sha256': sha256(pcm), 'analysis': analysis.stdout.strip(),
            'physical_speakers_tested': False}


def diagnostic_capture(vm, output, report):
    try:
        vm.hmp('stop')
    except Exception as exc:
        report['failure_pause_error'] = repr(exc)
    for filename, command in (('failure-registers.txt', 'info registers'),
                              ('failure-pic.txt', 'info pic'), ('failure-irq.txt', 'info irq')):
        try:
            (output / filename).write_bytes(vm.hmp(command))
        except Exception as exc:
            report[filename + '_error'] = repr(exc)
    try:
        Image.open(vm.shot('failure')).save(output / 'failure.png')
    except Exception as exc:
        report['failure_screenshot_error'] = repr(exc)
    try:
        vm.hmp(f'pmemsave 0 0x100000 "{output / "failure-ram-1m.bin"}"')
    except Exception as exc:
        report['failure_ram_error'] = repr(exc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--edid', choices=('off', '1024x768', '1280x800'), default='off')
    parser.add_argument('--audio', choices=('ac97', 'none'), default='ac97')
    parser.add_argument('--memory', type=int, default=128)
    parser.add_argument('--exercise-native-client', action='store_true',
                        help='exercise streamed HELP and the native 3D client unsupported path')
    parser.add_argument('--vbe06-pitch-divisor8', action='store_true',
                        help='install a test-only INT 10h TSR that divides successful 4F06 BX by 8')
    args = parser.parse_args()
    if args.vbe06_pitch_divisor8 and args.edid != '1280x800':
        parser.error('--vbe06-pitch-divisor8 requires --edid 1280x800')
    source, output = args.image.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    disk = output / 'disk.img'
    report_path = output / 'report.json'
    report = {'status': 'running', 'physical_hardware_qualified': False,
              'scope': ('Pristine installed image; preboot VGA/audio devices; real keyboard and read-only RAM evidence'
                        if not args.vbe06_pitch_divisor8 else
                        'Pristine source-image preflight, then a private HDD copy with test-only VBE 4F06 TSR override; read-only RAM evidence'),
              'image': str(source), 'image_sha256': file_sha256(source), 'edid': args.edid,
              'audio_profile': args.audio, 'memory_mib': args.memory,
              'guest_binary_overrides': bool(args.vbe06_pitch_divisor8),
              'vbe06_pitch_divisor8': bool(args.vbe06_pitch_divisor8),
              'checks': []}
    expected = (1024, 768) if args.edid == 'off' else tuple(map(int, args.edid.split('x')))
    report['expected_geometry'] = list(expected)
    vm = None
    failure = None
    stage = 'pristine source and binary preflight'
    audio_baseline = b''
    volume = None
    try:
        fs = FAT16(source)
        offset = fs.start
        shell_image, sfx_image = fs.read(SHELL_PATH), fs.read(SFX_PATH)
        driver_cfg = fs.read('DRIVERS/DRIVERS.CFG')
        boot_pcm = fs.read('SYSTEM/BOOT.PCM')
        display_cfg = fs.read('SYSTEM/VIDEO/DISPLAY.CFG')
        assert display_cfg == b'AUTO', f'canonical default must be AUTO, got {display_cfg!r}'
        assert optional_file(fs, 'SYSTEM/BOOT.SND') != b'0', 'canonical startup sound is disabled'
        audio_baseline = optional_file(fs, AUDIO_PATH)
        wall_catalog, photos = check_assets(fs)
        photo = photos['Ciuk1']
        wall_cfg = fs.read('SYSTEM/UI/WALL.CFG')
        assert wall_cfg == bytes((photo['index'], WP_FIT)), \
            f'canonical wallpaper must be Ciuk1/Fit, got {wall_cfg.hex()}'
        report['owner_wallpapers'] = {
            title: {key: value for key, value in details.items() if key != 'rgb'}
            for title, details in photos.items()}
        report['wall_catalog_entries'] = len(wall_catalog)
        report['wallpaper_config'] = wall_cfg.hex()
        del photos
        report['source_files'] = {path: {'bytes': len(data), 'sha256': sha256(data)} for path, data in
                                  ((SHELL_PATH, shell_image), (SFX_PATH, sfx_image),
                                   ('SYSTEM/BOOT.PCM', boot_pcm), ('SYSTEM/VIDEO/DISPLAY.CFG', display_cfg))}
        report['source_audio_log_bytes'] = len(audio_baseline)
        assert sfx_image[3:11] == b'CSFX0001' and len(sfx_image) == int.from_bytes(sfx_image[12:14], 'little'), \
            'invalid or truncated installed native SFX image'
        assert len(sfx_image) <= 16384 and int.from_bytes(sfx_image[14:16], 'little') == 1
        for name, source_asm, defines, installed in (
                ('SHELL', 'src/com/shell.asm', [], shell_image),
                ('SFX', 'src/com/ac97init.asm', ['-DUI_SFX_DRIVER=1'], sfx_image)):
            target = output / ('fresh-' + name + '.bin')
            listing = output / (name + '.lst')
            subprocess.run(['nasm', '-f', 'bin', *defines, '-l', str(listing),
                            '-o', str(target), source_asm], cwd=ROOT, check=True)
            assert target.read_bytes() == installed, f'shipped {name} differs from fresh source assembly'
        shell_listing = (output / 'SHELL.lst').read_text().splitlines()
        sfx_listing = (output / 'SFX.lst').read_text().splitlines()
        fault_tsr = None
        if args.vbe06_pitch_divisor8:
            fault_tsr = output / 'VBE06TSR.COM'
            subprocess.run(['nasm', '-f', 'bin', '-l', str(output / 'VBE06TSR.lst'),
                            '-o', str(fault_tsr), 'scripts/fixtures/vbe06_fault.asm'],
                           cwd=ROOT, check=True)
        del fs
        shutil.copyfile(source, disk)
        assert file_sha256(disk) == report['image_sha256'], 'private test image changed before boot'
        report['volume_offset_bytes'] = offset
        volume = f'{disk}@@{offset}'
        if args.vbe06_pitch_divisor8:
            lines = driver_cfg.decode('ascii').splitlines()
            assert not any('VBE06TSR' in line for line in lines), 'fault driver already exists in source image'
            fault_line = r'1 VIDEO VBE06TSR \DRIVERS\VIDEO\VBE06TSR.COM'
            s3_index = next((i for i, line in enumerate(lines) if 'S3VBEFIX' in line), None)
            lines.insert(s3_index + 1 if s3_index is not None else len(lines), fault_line)
            cfg_path = output / 'DRIVERS.CFG'
            cfg_path.write_bytes(('\r\n'.join(lines) + '\r\n').encode('ascii'))
            subprocess.run(['mcopy', '-o', '-i', volume, str(fault_tsr),
                            '::DRIVERS/VIDEO/VBE06TSR.COM'], check=True)
            subprocess.run(['mcopy', '-o', '-i', volume, str(cfg_path),
                            '::DRIVERS/DRIVERS.CFG'], check=True)
            report['vbe06_fault_tsr'] = {
                'path': 'DRIVERS/VIDEO/VBE06TSR.COM',
                'bytes': fault_tsr.stat().st_size,
                'sha256': file_sha256(fault_tsr),
                'private_driver_config_line': fault_line,
            }
            report['checks'].append('test-only VBE 4F06 fault TSR staged in private HDD after S3VBEFIX')
        report['checks'].append('pristine source image hash, AUTO default, and exact freshly assembled SHELL/SFX verified before private-image test override')
        report_path.write_text(json.dumps(report, indent=2) + '\n')

        stage = 'AUTO desktop boot'
        vga = ('VGA,vgamem_mb=32,edid=off' if args.edid == 'off'
               else f'VGA,vgamem_mb=32,edid=on,xres={expected[0]},yres={expected[1]}')
        audio = (['-audiodev', f'wav,id=bootaudio,path={output / "audio.wav"}',
                  '-device', 'AC97,audiodev=bootaudio'] if args.audio == 'ac97'
                 else ['-audiodev', 'none,id=bootaudio'])
        vm = VM(disk, output, qemu_args=['-vga', 'none', '-device', vga, *audio], memory=args.memory)
        vm.auto_enter_dos = False
        report['qemu_command'] = vm.process.args
        started = time.monotonic()
        vm.wait('[DESKTOP] READY', timeout=120)
        report['desktop_ready_seconds'] = time.monotonic() - started
        Image.open(vm.shot('desktop-ready', expected)).save(output / 'desktop-ready.png')
        # The shipping startup About window is dismissed through normal input.
        vm.wait('[ABOUT] open', timeout=30)
        offset_before_escape = vm.offset()
        vm.key('esc')
        vm.wait('[DESKTOP] WINDOW 02 CLOSE', offset_before_escape, 30)
        report['checks'].append('AUTO desktop READY at expected geometry; About closed with Escape')

        stage = 'initial Ciuk1 asynchronous load and RGB presentation'
        report['wallpaper_initial_ready'] = wait_wallpaper(vm, photo['filename'])
        report['wallpaper_initial'] = verify_photo(vm, output, 'Ciuk1-initial', expected, photo)
        report['checks'].append('owner Ciuk1 ready and painted; all 36 initial RGB samples match exactly')
        if args.vbe06_pitch_divisor8:
            driver_log = read_live(volume, 'DRIVERS/LOADDRV.LOG')
            assert b'VBE06TSR OK' in driver_log, driver_log
            report['vbe06_fault_driver_log'] = driver_log.decode('ascii', 'replace')
            report['checks'].append('fault TSR installed by the normal startup driver loader before VBE mode selection')

        stage = 'bounded native startup completion'
        time.sleep(5)
        report['video_before_dos'] = parse_log(read_live(volume, DISPLAY_LOG_PATH))
        verify_video(report['video_before_dos'], expected, args.edid)
        report['checks'].append('CVB1 proves 32-bit direct color, exact 4F03 readback and native renderer-path use')
        if args.audio == 'ac97':
            raw_audio, records = wait_native_audio(volume, audio_baseline)
            (output / 'AUDIO-before-dos.LOG').write_bytes(raw_audio)
            report['native_audio_records'] = records
            report['native_startup'] = verify_native_audio(records, len(boot_pcm))
            report['audio_idle_before_dos'] = audio_idle_snapshot(
                vm, output, 'before-dos', shell_image, shell_listing, sfx_image, sfx_listing)
            verify_audio_idle(report['audio_idle_before_dos'])
            # Validate a host capture while the VM still exists, so waveform
            # failure receives live registers/IRQ/RAM and a screenshot.
            stage = 'live captured startup waveform'
            shutil.copyfile(output / 'audio.wav', output / 'audio-live.wav')
            report['audio_capture_live'] = waveform_analysis(
                output / 'audio-live.wav', output, 'audio-analysis-live.txt')
            report['checks'].append('native ICH startup fetched the complete >64 KiB track and released startup ownership')
        else:
            report['native_audio_assertions_skipped'] = True
            report['audio_skip_reason'] = 'No audio device configured; native/legacy fallback is not qualified by this case'

        stage = 'real keyboard DOS entry and desktop return'
        if args.vbe06_pitch_divisor8:
            layout = video_layout_snapshot(vm, output, shell_image, shell_listing, expected[1])
            report['vbe06_one_page_layout'] = layout
            report['checks'].append('malformed VBE 4F06 query leaves one-page scanline layout and page flipping disabled')
        keyboard_offset = vm.offset()
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>', keyboard_offset, 60)
        if args.vbe06_pitch_divisor8:
            vm.command(r'run \DRIVERS\VIDEO\VBE06TSR.COM Q', timeout=30)
            fault_record = read_live(volume, 'SYSTEM/VIDEO/VBE06.LOG')
            assert len(fault_record) == 10 and fault_record[:4] == b'V06Q', fault_record
            hits = int.from_bytes(fault_record[4:6], 'little')
            original_stride = int.from_bytes(fault_record[6:8], 'little')
            damaged_stride = int.from_bytes(fault_record[8:10], 'little')
            assert hits > 0 and damaged_stride == original_stride // 8, fault_record.hex()
            video = report['video_before_dos']
            expected_bytes = expected[0] * 4
            assert video['renderer']['active_pitch'] >= expected_bytes, video
            assert video['mode_info']['modeinfo_pitch_field'] == video['renderer']['active_pitch'], video
            assert video['renderer']['active_pitch'] > damaged_stride, (video, fault_record.hex())
            assert video['renderer']['vclfb_header'] == 2, video
            report['vbe06_fault_observation'] = {
                'successful_get_calls': hits, 'original_bx_bytes_per_scanline': original_stride,
                'divided_reply_bx': damaged_stride,
                'validated_descriptor_pitch': video['renderer']['active_pitch'],
                'mode_info_descriptor_pitch': video['mode_info']['modeinfo_pitch_field'],
                'record_hex': fault_record.hex(),
            }
            report['checks'].append('AUTO retained the valid 1280x800x32 descriptor pitch after the TSR divided 4F06 BX by 8')
        vm.command('echo INPUT READY', 'INPUT READY\r\nCiukiOS SHELL', timeout=30)
        if args.exercise_native_client:
            stage = 'streamed help and native 3D client in the system shell'
            vm.command('help', 'CiukiOS SHELL C:\\APPS>', timeout=30)
            vm.command('sav3d', 'Native S3 triangle unsupported on this active display.', timeout=30)
            raw_3d = read_live(volume, 'SYSTEM/VIDEO/GPU3D.LOG')
            assert len(raw_3d) == 400 and raw_3d[:4] == b'CG3D', (len(raw_3d), raw_3d[:4])
            assert int.from_bytes(raw_3d[8:12], 'little') == 1, 'QEMU incorrectly reported native S3 3D'
            (output / 'GPU3D.LOG').write_bytes(raw_3d)
            report['native_client_result'] = 'unsupported on QEMU, logged without a GPU success claim'
            report['checks'].append('streamed HELP and system-shell SAV3D returned; unsupported native 3D was logged')
        return_offset = vm.offset()
        vm.text('exit')
        vm.wait('[DESKTOP] READY', return_offset, 90)
        Image.open(vm.shot('desktop-after-dos', expected)).save(output / 'desktop-after-dos.png')
        stage = 'Ciuk1 asynchronous reload and RGB presentation after DOS'
        report['wallpaper_after_dos_ready'] = wait_wallpaper(vm, photo['filename'], return_offset)
        report['wallpaper_after_dos'] = verify_photo(vm, output, 'Ciuk1-after-dos', expected, photo)
        report['checks'].append('owner Ciuk1 reloaded after DOS; all 36 returned-desktop RGB samples match exactly')
        if args.audio == 'ac97':
            report['audio_idle_after_dos'] = audio_idle_snapshot(
                vm, output, 'after-dos', shell_image, shell_listing, sfx_image, sfx_listing)
            verify_audio_idle(report['audio_idle_after_dos'])
        report['video_after_dos'] = parse_log(read_live(volume, DISPLAY_LOG_PATH))
        verify_video(report['video_after_dos'], expected, args.edid)
        report['checks'].append('F4, echo INPUT READY, and exit returned to the same native desktop')
    except Exception as exc:
        failure = exc
        report['status'] = 'failed'
        report['failed_stage'] = stage
        report['error'] = repr(exc)
        if vm is not None:
            diagnostic_capture(vm, output, report)
    finally:
        if vm is not None:
            try:
                vm.close()
            except Exception as exc:
                report['close_error'] = repr(exc)
                failure = failure or exc
        if disk.exists() and volume is not None:
            # Read after QEMU has closed/flushed the private writable image.
            captures = [(AUDIO_PATH, 'AUDIO.LOG'), (DISPLAY_LOG_PATH, 'DISPLAY.LOG'),
                        ('DRIVERS/LOADDRV.LOG', 'LOADDRV.LOG')]
            if args.vbe06_pitch_divisor8:
                captures.append(('SYSTEM/VIDEO/VBE06.LOG', 'VBE06.LOG'))
            for path, destination in captures:
                try:
                    (output / destination).write_bytes(read_live(volume, path))
                except Exception as exc:
                    report[destination + '_read_error'] = repr(exc)
            report['private_image_final_sha256'] = file_sha256(disk)
        try:
            report['source_image_unchanged'] = file_sha256(source) == report['image_sha256']
            assert report['source_image_unchanged'], 'source image changed during the isolated test'
        except Exception as exc:
            failure = failure or exc
            report['source_preservation_error'] = repr(exc)
        report_path.write_text(json.dumps(report, indent=2) + '\n')

    if failure is None:
        try:
            stage = 'actual VBE readback and transport log'
            video = parse_log((output / 'DISPLAY.LOG').read_bytes())
            report['video'] = video
            verify_video(video, expected, args.edid)
            if args.audio == 'ac97':
                stage = 'captured startup waveform'
                report['audio_capture'] = waveform_analysis(output / 'audio.wav', output, 'audio-analysis.txt')
                report['checks'].append('QEMU PCM WAV contains a changing nonconstant startup signal')
        except Exception as exc:
            failure = exc
            report['failed_stage'] = stage
            report['error'] = repr(exc)
    report['status'] = 'passed' if failure is None else 'failed'
    report['passed'] = failure is None
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    if failure is not None:
        raise failure
    pass_kind = ('private-image VBE 4F06 fault boot' if args.vbe06_pitch_divisor8
                 else 'pristine AUTO boot')
    print(f'PASS {pass_kind} {expected[0]}x{expected[1]}x32, EDID={args.edid}, '
          f'audio={args.audio}, keyboard/desktop return; image={report["image_sha256"]}')


if __name__ == '__main__':
    main()
