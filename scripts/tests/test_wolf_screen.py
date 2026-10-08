#!/usr/bin/env python3
"""Prove the Wolf screen recognizer rejects titles/blanks as gameplay."""
from pathlib import Path
import struct
import sys
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from qemu_files_game_launch import address_fields
from wolf_screen import WolfPictures, hud_score, pattern_score


def synthetic_assets():
    # Complete8-bit tree exercises root254, LSB-first packed input and every
    # literal without embedding any game's commercial pixels/dictionary.
    nodes = []

    def tree(values):
        if len(values) == 1:
            return values[0]
        left = tree(values[:len(values)//2])
        right = tree(values[len(values)//2:])
        nodes.append((left, right))
        return 256 + len(nodes) - 1

    assert tree(list(range(256))) == 510
    dictionary = b''.join(struct.pack('<HH', *pair) for pair in nodes)
    reverse = bytes(int(f'{value:08b}'[::-1], 2) for value in range(256))

    def packed(raw):
        return struct.pack('<I', len(raw)) + raw.translate(reverse)

    table = bytearray(132 * 4)
    struct.pack_into('<HH', table, (86 - 3) * 4, 320, 40)
    struct.pack_into('<HH', table, (87 - 3) * 4, 320, 200)
    rng = np.random.default_rng(713)
    hud = rng.integers(0, 16, (40, 320), dtype=np.uint8)
    title = rng.integers(0, 16, (200, 320), dtype=np.uint8)

    def planar(picture):
        h, w = picture.shape
        return picture.reshape(h, w//4, 4).transpose(2, 0, 1).tobytes()

    graph = packed(bytes(table)) + packed(planar(hud)) + packed(planar(title))
    starts = [0xffffff] * 89
    starts[0] = 0
    starts[86] = len(packed(bytes(table)))
    starts[87] = starts[86] + len(packed(planar(hud)))
    starts[88] = len(graph)
    header = b''.join(value.to_bytes(3, 'little') for value in starts)
    return WolfPictures(header, dictionary, graph), hud, title


class WolfScreenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pictures, cls.hud, cls.title = synthetic_assets()
        cls.palette = np.random.default_rng(91).integers(0, 256, (256, 3), dtype=np.uint8)

    def gameplay(self):
        frame = np.zeros((200, 320, 3), dtype=np.uint8)
        frame[160:] = self.palette[self.hud]
        return frame

    def test_huffman_and_planar_decode(self):
        np.testing.assert_array_equal(self.pictures.picture(86), self.hud)
        np.testing.assert_array_equal(self.pictures.picture(87), self.title)

    def test_actual_hud_pattern_and_documented_dynamic_mask(self):
        frame = self.gameplay()
        self.assertTrue(hud_score(frame, self.pictures)['matched'])
        frame[164:196, 136:160] = 0  # Dynamic face.
        frame[176:192, 168:192] = 255  # Health number.
        self.assertTrue(hud_score(frame, self.pictures)['matched'])
        frame[160:163, :] = 0  # Static statusbar detail must survive.
        self.assertFalse(hud_score(frame, self.pictures)['matched'])

    def test_title_blank_colourful_noise_and_shift_are_not_hud(self):
        frames = [np.zeros((200, 320, 3), np.uint8), self.palette[self.title],
                  np.random.default_rng(23).integers(0, 256, (200, 320, 3), dtype=np.uint8),
                  np.roll(self.gameplay(), 1, axis=1)]
        for frame in frames:
            with self.subTest(frame=hash(frame.tobytes())):
                self.assertFalse(hud_score(frame, self.pictures)['matched'])

    def test_palette_change_keeps_indexed_pattern(self):
        frame = 255 - self.palette[self.title]
        self.assertTrue(pattern_score(frame, self.title, 0, 0)['matched'])

    def test_field_scanner_checks_real_structure_and_buffer(self):
        memory = bytearray(65536)
        text = b'C:\\DESKTOP\\TestGames'
        memory[4000:4000+len(text)] = text
        struct.pack_into('<6H', memory, 2000, 4000, 260, len(text), len(text), 1, 0)
        fields = address_fields(memory)
        self.assertEqual(len(fields), 1)
        self.assertEqual(fields[0]['text'], text.decode())
        self.assertEqual(fields[0]['selected'], 1)
        struct.pack_into('<H', memory, 2004, len(text) + 1)
        self.assertEqual(address_fields(memory), [])

    def test_bound_field_accepts_partial_edit_without_relaxing_discovery(self):
        memory = bytearray(65536)
        memory[4000:4002] = b'c\0'
        struct.pack_into('<6H', memory, 2000, 4000, 260, 1, 1, 0, 0)
        self.assertEqual(address_fields(memory), [])
        self.assertEqual(address_fields(memory, 2000)[0]['text'], 'c')
        memory[4001:4003] = b':\0'
        struct.pack_into('<6H', memory, 2000, 4000, 260, 2, 2, 0, 0)
        self.assertEqual(address_fields(memory), [])
        self.assertEqual(address_fields(memory, 2000)[0]['text'], 'c:')
        struct.pack_into('<H', memory, 2004, 3)  # Length/buffer mismatch still rejected.
        self.assertEqual(address_fields(memory, 2000), [])


if __name__ == '__main__':
    unittest.main()
