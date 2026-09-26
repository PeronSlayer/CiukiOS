#!/usr/bin/env python3
"""Exact CPU check of the production DOS cell blitter; not a frame-rate test."""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UC_HOOK_INTR
from unicorn.x86_const import *


def section(path, start, end):
    text = Path(path).read_text()
    return text[text.index(start):text.index(end)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fields = {
        'ui_width': 2, 'ui_height': 2, 'ui_composing': 1, 'ui_vbe': 1,
        'ui_fill_color': 1, 'ui_fill_width': 2, 'ui_fill_height': 2,
        'vc_bytes': 1, 'vc_pitch': 2, 'ui_front_base': 4, 'vc_colors': 1024,
        'ui_assets_seg': 2, 'ui_comp_top': 2, 'ui_comp_bottom': 2,
        'ug_text_color': 1, 'ui_font_ptr': 2, 'ui_advance': 1,
        'ui_comp_left': 2, 'ui_comp_right': 2, 'ui_comp_seg': 2,
        'ui_comp_stride': 2, 'ui_comp_glyph_x': 2,
        'dw_clip_width': 2, 'dw_clip_height': 2, 'dw_clip_advance': 2,
        'dw_clip_skip': 1,
    }
    source = 'bits 16\norg 0x100\n%include "src/com/dos_window_abi.inc"\nUI_FONT_BYTES equ 3135\nUI_ASSET_FONT equ 0\n'
    source += 'stop: hlt\nug_rect: int 0xFE\nug_glyph: int 0xFE\n'
    source += 'vc_span: int 0xFE\nvc_put_index: int 0xFE\n'
    source += ''.join(f'{name}: times {size} db 0\n' for name, size in fields.items())
    source += section('src/com/shell_gui_draw.inc', 'ui_rect:', '; Raised two-step')
    source += section('src/com/shell_gui_compositor.inc', 'ui_comp_rect:', 'ui_comp_present:')
    source += section('src/com/shell_dos_window.inc', 'dw_opaque_cell:', 'ut_dos_window db')
    # Equivalent original cell path, using the actual unchanged generic
    # background/glyph instructions rather than a simulated baseline.
    source += '''
old_cell:
    mov ah,al
    push si
    shr al,4
    and al,7
    mov cx,8
    mov si,16
    call ui_rect
    pop si
    mov al,ah
    and al,15
    mov [ug_text_color],al
    call ui_glyph
    ret
'''
    names = ['stop', 'dw_opaque_cell', 'old_cell', *fields]
    source += 'db "DWCELLS1"\n' + ''.join(f'dw {name}\ndb "{name}",0\n' for name in names) + 'dw 0\n'
    asm = args.output / 'fixture.asm'
    binary_path = args.output / 'fixture.bin'
    asm.write_text(source)
    subprocess.run(['nasm', '-f', 'bin', str(asm), '-o', str(binary_path)], check=True)
    binary = binary_path.read_bytes()
    pos = binary.index(b'DWCELLS1') + 8
    symbols = {}
    while address := struct.unpack_from('<H', binary, pos)[0]:
        end = binary.index(0, pos+2)
        symbols[binary[pos+2:end].decode()] = address
        pos = end+1
    module_fields = ['DW_CELL_ENTRY', 'DW_CELL_BAND_SEG', 'DW_CELL_STRIDE',
                     'DW_CELL_LEFT', 'DW_CELL_RIGHT', 'DW_CELL_TOP',
                     'DW_CELL_BOTTOM', 'DW_CELL_PALETTE', 'DW_CELL_BYTES']
    module_source = '''bits 16
org 0
%include "src/com/dos_window_abi.inc"
times DW_CELL_ENTRY db 0
dw dw_cell_render,0x3000
times DW_HEADER_BYTES-($-$$) db 0
%include "src/com/dos_window_cell.inc"
db "DWEXT1"
'''
    module_source += ''.join(f'dw {name}\ndb "{name}",0\n' for name in module_fields)+'dw 0\n'
    (args.output/'module.asm').write_text(module_source)
    subprocess.run(['nasm', '-f', 'bin', str(args.output/'module.asm'), '-o', str(args.output/'module.bin')], check=True)
    module = (args.output/'module.bin').read_bytes()
    assert len(module) < 0x800, 'Fixture font must not overlap module code'
    pos = module.index(b'DWEXT1')+6
    module_offsets = {}
    while address := struct.unpack_from('<H', module, pos)[0]:
        end = module.index(0, pos+2)
        module_offsets[module[pos+2:end].decode()] = address
        pos = end+1
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, 0x100000)
    uc.mem_write(0x10100, binary)
    uc.mem_write(0x30000, module)
    uc.hook_add(UC_HOOK_INTR, lambda *_: (_ for _ in ()).throw(AssertionError('Interrupt during cell paint')))
    counter = [0]
    uc.hook_add(UC_HOOK_CODE, lambda *_: counter.__setitem__(0, counter[0]+1))

    def put(name, value, size=2):
        uc.mem_write(0x10000+symbols[name], value.to_bytes(size, 'little'))

    def put_module(name, value, size=2):
        uc.mem_write(0x30000+module_offsets[name], value.to_bytes(size, 'little'))

    def call(name, attr):
        initial = dict(EAX=0xABCD0000 | attr, EBX=0x12340064,
                       ECX=0x98762345, EDX=0x1357004D, ESI=0x24680800,
                       EDI=0x56781234, EBP=0xDCBAFEED, ES=0x2222,
                       FS=0x3000, GS=0x4444)
        for reg, value in initial.items():
            uc.reg_write(globals()['UC_X86_REG_'+reg], value)
        uc.reg_write(UC_X86_REG_CS, 0x1000)
        uc.reg_write(UC_X86_REG_DS, 0x1000)
        uc.reg_write(UC_X86_REG_SS, 0x7000)  # timer's private stack, SS != DS
        uc.reg_write(UC_X86_REG_SP, 0xF000)
        uc.mem_write(0x7F000, struct.pack('<H', symbols['stop']))
        uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
        counter[0] = 0
        uc.emu_start(0x10000+symbols[name], 0x10000+symbols['stop'], count=100000)
        assert uc.reg_read(UC_X86_REG_IP) == symbols['stop'], 'Instruction limit reached'
        if name == 'dw_opaque_cell':
            for reg, value in initial.items():
                assert uc.reg_read(globals()['UC_X86_REG_'+reg]) == value, reg
            assert uc.reg_read(UC_X86_REG_EFLAGS) & 0x200
        return counter[0]

    put('ui_width', 800)
    put('ui_height', 600)
    put('ui_composing', 1, 1)
    put('ui_vbe', 1, 1)
    put('ui_comp_seg', 0x5000)
    put_module('DW_CELL_BAND_SEG', 0x5000)
    put_module('DW_CELL_PALETTE', symbols['vc_colors'])
    font = bytes([0xAA, 0x55, 0x81, 0x7E, 0x18, 0xFF, 0, 0x66]*2)
    uc.mem_write(0x30800, b''.join(bytes((0, value)) for value in font))
    formats = [(8, 1, [i for i in range(16)]),
               (15, 2, [(i*2113)&0x7FFF for i in range(16)]),
               (16, 2, [(i*4369)&0xFFFF for i in range(16)]),
               (24, 3, [(i*0x123457)&0xFFFFFF for i in range(16)]),
               (32, 4, [0xCF000000|((i*0x543217)&0xFFFFFF) for i in range(16)])]
    cases = [(96, 72, 24, 28), (103, 81, 3, 6), (98, 76, 7, 12),
             (109, 77, 8, 16), (100, 94, 8, 3), (100, 77, 0, 16)]
    report = {'scope': 'Exact CPU instructions and operation counts, not wall-clock FPS or GPU acceleration',
              'source_sha256': hashlib.sha256(Path('src/com/shell_dos_window.inc').read_bytes()).hexdigest(),
              'module_source_sha256': hashlib.sha256(Path('src/com/dos_window_cell.inc').read_bytes()).hexdigest(),
              'far_module_cs_differs_from_host_ds_ss': True,
              'cases': [], 'formats': []}
    for depth, size, palette in formats:
        put('vc_bytes', size, 1)
        put_module('DW_CELL_BYTES', size, 1)
        uc.mem_write(0x10000+symbols['vc_colors'], b''.join(struct.pack('<I', p) for p in palette))
        old_total = new_total = 0
        for left, top, width, height in cases:
            put('ui_comp_left', left)
            put('ui_comp_right', left+width)
            put('ui_comp_top', top)
            put('ui_comp_bottom', top+height)
            put('ui_comp_stride', max(width*size, 1))
            for field, value in [('LEFT',left),('RIGHT',left+width),('TOP',top),
                                 ('BOTTOM',top+height),('STRIDE',max(width*size,1))]:
                put_module('DW_CELL_'+field, value)
            for attr in [0x07, 0x1F, 0x4E, 0x87, 0xFF]:
                expected = bytearray(b'\xA5'*65536)
                for y in range(max(top, 77), min(top+height, 93)):
                    for x in range(max(left, 100), min(left+width, 108)):
                        index = attr & 15 if font[y-77] & (0x80 >> (x-100)) else (attr>>4)&7
                        offset = ((y-top)*width+x-left)*size
                        expected[offset:offset+size] = palette[index].to_bytes(4, 'little')[:size]
                counts = {}
                for target in ('old_cell', 'dw_opaque_cell'):
                    uc.mem_write(0x50000, b'\xA5'*65536)
                    counts[target] = call(target, attr)
                    actual = bytes(uc.mem_read(0x50000, 65536))
                    assert actual == expected, (depth, target, left, top, width, height, attr,
                                                 next((i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b), None))
                old_total += counts['old_cell']
                new_total += counts['dw_opaque_cell']
                report['cases'].append(dict(depth=depth, band=[left,top,width,height], attr=attr, counts=counts))
        report['formats'].append(dict(depth=depth, cases=len(cases)*5,
            old_instructions=old_total, new_instructions=new_total,
            instruction_reduction=1-new_total/old_total))
    report['passed'] = True
    (args.output/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report['formats'], indent=2))


if __name__ == '__main__':
    main()
