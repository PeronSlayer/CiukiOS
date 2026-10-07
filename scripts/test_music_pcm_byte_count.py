#!/usr/bin/env python3
"""Small regression fixture for QEMU music-capture PCM byte metrics."""
import unittest
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_test_music_player import count_nonzero_pcm_bytes


class PCMByteCountTests(unittest.TestCase):
    def test_all_zero_payload_counts_zero(self):
        self.assertEqual(count_nonzero_pcm_bytes(b'\x00' * 8), 0)

    def test_counts_each_nonzero_byte(self):
        self.assertEqual(count_nonzero_pcm_bytes(b'\x00\x01\x00\x80\xff'), 3)

    def test_empty_payload_counts_zero(self):
        self.assertEqual(count_nonzero_pcm_bytes(b''), 0)


if __name__ == '__main__':
    unittest.main()
