#!/usr/bin/env python3
"""Build and test the freestanding VGA model, virtual BIOS and presenter.

This is device/firmware-model evidence only, not a DOS VM claim. It runs the
C assertions under ASan/UBSan, proves every module compiles and links as a
freestanding OpenWatcom object, and compares the BIOS mode register tables and
default DAC palettes byte-for-byte with QEMU's installed SeaVGABIOS ROM.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MODULES = ['virtual_vga.c', 'virtual_vga_bios.c', 'vga_presenter.c', 'vga_x86.c']
ROM = Path('/usr/share/qemu/vgabios-stdvga.bin')


def seavgabios_modes(rom):
    """Parse SeaVGABIOS stdvga_modes records: u16 mode, vgamode_s, dac pointer,
    dac size, sequencer/misc/CRTC/attribute/GC pointers (32-bit ROM offsets)."""
    found = {}
    for at in range(0, len(rom) - 44, 2):
        mode, = struct.unpack_from('<H', rom, at)
        if mode not in (0x00, 0x01, 0x02, 0x03, 0x0d, 0x0e, 0x10, 0x11, 0x12, 0x13):
            continue
        segment, = struct.unpack_from('<H', rom, at + 12)
        dac, dac_size = struct.unpack_from('<IH', rom, at + 16)
        seq, misc, crtc, attr, gc = struct.unpack_from('<IBxxxIII', rom, at + 24)
        if segment not in (0xa000, 0xb800) or not all(0 < p < len(rom) for p in (dac, seq, crtc, attr, gc)):
            continue
        if dac_size not in (192, 768):
            continue
        found.setdefault(mode, dict(offset=at, seq=rom[seq:seq + 4].hex(), misc=misc,
                                    crtc=rom[crtc:crtc + 25].hex(), attr=rom[attr:attr + 20].hex(),
                                    gc=rom[gc:gc + 9].hex(), palette=rom[dac:dac + dac_size].hex()))
    return found


def rom_cross_check(binary, report):
    if not ROM.exists():
        report['firmware_cross_check'] = 'SKIPPED: ' + str(ROM) + ' is not installed'
        return
    rom = ROM.read_bytes()
    tables = json.loads(subprocess.run([str(binary), '--dump-tables'], check=True,
                                       capture_output=True, text=True).stdout)
    firmware = seavgabios_modes(rom)
    palettes = tables['palettes']
    compared = []
    for entry in tables['modes']:
        mode = entry['mode']
        rom_entry = firmware.get(mode)
        assert rom_entry, f'mode {mode:#x} not found in {ROM}'
        for field in ('seq', 'crtc', 'attr', 'gc'):
            assert entry[field] == rom_entry[field], (hex(mode), field, entry[field], rom_entry[field])
        assert entry['misc'] == rom_entry['misc'], (hex(mode), 'misc', entry['misc'], rom_entry['misc'])
        which = {192: None, 768: '3'}[len(rom_entry['palette']) // 2]
        if which is None:
            which = '1' if rom_entry['palette'] == palettes['1'] else '2'
        assert rom_entry['palette'] == palettes[which], (hex(mode), 'palette')
        compared.append(dict(mode=hex(mode), rom_record_offset=hex(rom_entry['offset']), palette=int(which)))
    report['firmware_cross_check'] = dict(
        rom=str(ROM), rom_sha256=hashlib.sha256(rom).hexdigest(), modes=compared,
        compared_fields=['SR1-SR4', 'misc', 'CR00-CR18', 'AR00-AR13', 'GR00-GR08', 'default DAC'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tests/virtual-vga')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    sources = [ROOT / 'src/vm' / name for name in MODULES]
    harness = ROOT / 'src/vm/test_virtual_vga.c'
    binary = output / 'test-virtual-vga'
    command = [os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
               '-pedantic', '-O1', '-g', '-fno-omit-frame-pointer',
               '-fsanitize=address,undefined', *map(str, sources), str(harness), '-o', str(binary)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(binary)], text=True, capture_output=True)
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    result.check_returncode()
    watcom = Path(os.environ.get('WATCOM', '/opt/watcom'))
    compiler = watcom / 'binl64/wcc386'
    if not compiler.exists():
        compiler = watcom / 'binl/wcc386'
    if not compiler.exists():
        raise SystemExit('OpenWatcom wcc386 is required to verify the target objects')
    target_commands = []
    objects = []
    for source in sources:
        obj = output / (source.stem + '.obj')
        target_command = [str(compiler), '-zq', '-bt=dos', '-mf', '-3r', '-ecc', '-zl',
                          '-s', '-ox', '-ot', '-w4', '-we', '-i=' + str(watcom / 'h'),
                          '-fo=' + str(obj), str(source)]
        subprocess.run(target_command, check=True)
        target_commands.append(target_command)
        objects.append(obj)
    # Resolve the objects by themselves with all runtime/default libraries
    # disabled: any compiler helper import (64-bit arithmetic, memcpy, stack
    # checks) fails this link. OMF EXTDEF also lists same-object references,
    # so scanning for EXTDEF records alone would be wrong.
    link_command = [str(compiler.with_name('wlink')), 'option',
                    'quiet,nodefaultlibs,start=_cvga_init', 'format', 'raw', 'bin',
                    'disable', '1014', 'name', str(output / 'virtual_vga.bin')]
    for obj in objects:
        link_command += ['file', str(obj)]
    subprocess.run(link_command, check=True)
    report = {
        'result': 'PASS', 'scope': 'VGA device model, virtual BIOS and presenter only',
        'dos_guest_execution': False, 'v86_or_dpmi_memory_trapping': False,
        'host_compiler_command': command, 'target_compiler_commands': target_commands,
        'freestanding_link_command': link_command,
        'assertions': result.stdout.strip(),
        'target_objects_have_no_external_imports': True,
        'physical_hardware_qualified': False,
    }
    rom_cross_check(binary, report)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('firmware cross-check:', json.dumps(report['firmware_cross_check'])[:400])


if __name__ == '__main__':
    main()
