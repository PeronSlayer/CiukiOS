"""Authored deterministic single-folder MSZIP cabinet writer (MS-CAB).

No external packer, timestamps, filesystem traversal or executable payloads.
The FDI reader, rather than this writer, verifies the extracted checksums.
"""
# SPDX-License-Identifier: MIT
import struct
import zlib


def checksum(data, value=0):
    end = len(data) & ~3
    for (word,) in struct.iter_unpack('<I', data[:end]):
        value ^= word
    # CAB's tail bytes are accumulated in reverse significance.
    return value ^ int.from_bytes(data[end:], 'big')


def mszip(files):
    """Mapping of safe relative ASCII names to bytes; fixed DOS 2000-01-01."""
    if not files or len(files) > 65535:
        raise ValueError('cabinet file count')
    entries, payload = bytearray(), bytearray()
    for name, data in sorted(files.items()):
        if (not name or name.startswith(('/', '\\')) or ':' in name or '\x00' in name or
                any(p in ('', '.', '..') for p in name.replace('\\', '/').split('/'))):
            raise ValueError('unsafe cabinet member')
        encoded = name.encode('ascii')
        if len(encoded) > 255 or len(payload) + len(data) > 16 * 1024 * 1024:
            raise ValueError('fixture cabinet bound')
        entries += struct.pack('<IIHHHH', len(data), len(payload), 0, 0x2821, 0, 0x20) + encoded + b'\0'
        payload += data
    blocks = bytearray()
    count = 0
    for at in range(0, len(payload), 32768):
        block = payload[at:at + 32768]
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        packed = b'CK' + compressor.compress(block) + compressor.flush()
        sizes = struct.pack('<HH', len(packed), len(block))
        blocks += struct.pack('<I', checksum(sizes, checksum(packed))) + sizes + packed
        count += 1
    offset = 36 + 8 + len(entries)
    header = struct.pack('<4sIIIII BBHHHHH', b'MSCF', 0, offset + len(blocks), 0, 44, 0,
                         3, 1, 1, len(files), 0, 0xC151, 0)
    return header + struct.pack('<IHH', offset, count, 1) + entries + blocks
