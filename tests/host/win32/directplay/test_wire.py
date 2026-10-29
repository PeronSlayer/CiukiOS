"""Independent format replay; does not claim DirectPlay 8 runtime support.

Vectors transcribed from Microsoft Open Specifications, not Wine headers:
MS-DPDX 2.2.4 and 2.2.7, MC-DPL4CS transport and Wine's observed DP4 frame.
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
import struct
import unittest
import uuid

DXDIAG = uuid.UUID('61ef80da-691b-4247-9add-1c7bed2bc13e')


def enum_query(packet):
    if len(packet) < 5 or packet[:2] != b'\0\2' or packet[4] not in (1, 2):
        raise ValueError('invalid EnumQuery header')
    if packet[4] == 1:
        if len(packet) != 21:
            raise ValueError('invalid DXDiag GUID/payload')
        guid = uuid.UUID(bytes_le=packet[5:21])
        if guid != DXDIAG:
            raise ValueError('different application')
    elif len(packet) != 5:
        raise ValueError('unexpected DXDiag application payload')
    return struct.unpack_from('<H', packet, 2)[0]


class IndependentReplay(unittest.TestCase):
    def test_ms_dpdx_enumeration_golden_datagram_and_mutations(self):
        # LeadByte, CommandByte, EnumPayload, QueryType, DXDiag ApplicationGUID.
        packet = bytes.fromhex('00 02 34 12 01 da80ef61 1b69 4742 9add 1c7bed2bc13e')
        self.assertEqual(enum_query(packet), 0x1234)
        self.assertEqual(enum_query(bytes.fromhex('00 02 78 56 02')), 0x5678)
        for end in range(len(packet)):
            with self.assertRaises(ValueError):
                enum_query(packet[:end])
        for offset, byte in ((0, 1), (1, 3), (4, 0), (5, 0)):
            changed = bytearray(packet)
            changed[offset] = byte
            with self.assertRaises(ValueError):
                enum_query(bytes(changed))
        with self.assertRaises(ValueError):
            enum_query(packet + b'\0')

    def test_dp4_and_dxdiag_are_different_protocols(self):
        # Wine DP4 TCP frame: length/token, IPv4 sockaddr, play envelope.
        frame = bytes.fromhex('1c00b0fa 0200 08fc 0a000002 0000000000000000 706c6179 0200 0b00')
        self.assertEqual(len(frame), 28)
        mixed, family = struct.unpack_from('<IH', frame)
        self.assertEqual((mixed & 0xfffff, mixed & 0xfff00000, family), (28, 0xfab00000, 2))
        self.assertEqual(struct.unpack_from('>H', frame, 6)[0], 2300)
        self.assertEqual(frame[20:24], b'play')
        self.assertEqual(struct.unpack_from('<HH', frame, 24), (2, 11))
        with self.assertRaises(ValueError):
            enum_query(frame)
