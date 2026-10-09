#!/usr/bin/env python3
"""Decode the startup input ownership observations without hardware mutation."""
import struct

def parse_input_snapshot(data):
    if len(data) != 64 or data[:4] != b'CIPS':
        raise ValueError('expected exact64-byte CIPS input snapshot')
    version, size, stage = struct.unpack_from('<HHH', data, 4)
    if version != 0x0100 or size != 64 or stage > 2:
        raise ValueError('unsupported CIPS version, size or stage')
    vectors = {}
    for name, offset in [('irq1',24),('irq12',28),('keyboard_api',32),
                         ('mouse_api',36),('bios15',40)]:
        off, seg = struct.unpack_from('<HH', data, offset)
        vectors[name] = {'segment':seg, 'offset':off}
    head, tail, start, end = struct.unpack_from('<4H', data,14)
    return dict(format='CIPS',abi_version=version,stage=stage,
                stage_name=['input_drivers_ready','vm_manager_ready','desktop_painted'][stage],
                master_pic_mask=data[10],slave_pic_mask=data[11],
                irq1_masked=bool(data[10]&2),irq12_masked=bool(data[11]&16),
                controller_status=data[12],output_full=bool(data[12]&1),
                input_busy=bool(data[12]&2),output_is_aux=bool(data[12]&32),
                keyboard_flags=data[13],keyboard_ring=dict(head=head,tail=tail,start=start,end=end),
                bios_keyboard_flags=data[22],enhanced_keyboard_flags=data[23],
                bios_ticks=struct.unpack_from('<I',data,44)[0],vectors=vectors)
