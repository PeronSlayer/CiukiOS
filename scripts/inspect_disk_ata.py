#!/usr/bin/env python3
"""Decode the bounded CDS1 boot-device binding and native I/O status record."""
import argparse
import json
from pathlib import Path
import struct

FIELDS = (
    'ready state error command_port control_port device_head irq '
    'control_shadow_known control_shadow read_count write_count last_status '
    'last_error last_lba capacity_sectors logical_heads sectors_per_track cylinders'
).split()
STATES = {0: 'unbound', 1: 'prepared', 2: 'bound', 3: 'quarantined'}
ERRORS = {
    0: 'success', 1: 'unbound', 2: 'invalid_packet', 3: 'unsupported',
    4: 'busy', 5: 'no_device', 6: 'ata_error', 7: 'timeout',
    8: 'quarantined', 9: 'out_of_range', 10: 'invalid_argument',
}


def parse_disk_status(data):
    if len(data) != 80 or data[:4] != b'CDS1':
        raise ValueError('CDS1 requires its magic and exactly 80 bytes')
    version, size = struct.unpack_from('<HH', data, 4)
    if version != 1 or size != 80:
        raise ValueError('unsupported CDS1 version or declared size')
    status = dict(zip(FIELDS, struct.unpack_from('<18I', data, 8)))
    if status['state'] not in STATES or status['ready'] not in (0, 1):
        raise ValueError('invalid CDS1 state')
    if status['ready'] and status['state'] != 2:
        raise ValueError('CDS1 reports ready without a bound device')
    if status['control_shadow_known'] not in (0, 1):
        raise ValueError('invalid CDS1 control-shadow flag')
    return dict(format='CDS1', version=version, size_bytes=size,
                native_ready=bool(status['ready'] and status['state'] == 2),
                state_name=STATES[status['state']],
                error_name=ERRORS.get(status['error'], 'unknown'), **status)


def parse_disk_binding(data):
    if len(data) != 80 or data[:8] != b'CDB1\x01\x00\x50\x00':
        raise ValueError('CDB1 requires version1 and exactly80 bytes')
    packet = data[16:]
    if packet[:6] != b'ADP1\x40\x00' or data[12] not in (0, 1):
        raise ValueError('invalid CDB1 packet or discovery flag')
    extension, result = struct.unpack_from('<HH', data, 8)
    off, seg = struct.unpack_from('<HH', packet, 34)
    dpte = packet[40:56]
    return dict(format='CDB1', version=1, size_bytes=80,
                bios_extensions=extension, binding_phase=result >> 8,
                binding_error=result & 255, discovery_selected=bool(data[12]),
                bios_drive=packet[6], edd_size=struct.unpack_from('<H',packet,8)[0],
                edd_flags=struct.unpack_from('<H',packet,10)[0],
                configuration_pointer=dict(segment=seg, offset=off,
                                           physical=seg * 16 + off),
                dpte=dict(raw=dpte.hex(), present=any(dpte),
                          checksum_valid=bool(any(dpte) and sum(dpte) % 256 == 0),
                          command_port=struct.unpack_from('<H',dpte,0)[0],
                          control_port=struct.unpack_from('<H',dpte,2)[0],
                          device_head=dpte[4], irq=dpte[6],
                          options=struct.unpack_from('<H',dpte,10)[0],
                          revision=dpte[14]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    args = parser.parse_args()
    data = args.snapshot.read_bytes()
    decode = parse_disk_binding if data[:4] == b'CDB1' else parse_disk_status
    print(json.dumps(decode(data), indent=2))


if __name__ == '__main__':
    main()
