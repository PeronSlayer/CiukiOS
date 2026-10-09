#!/usr/bin/env python3
"""Wrap VMFORK's PSP-relative image in an MZ with its explicit stack extent.

The complete COM payload includes the 512-byte private stack and ends at
stack_top. CS/SS=-10h relative to the image select its PSP, preserving the
same origin100h code in both formats. No relocation or runtime stub is needed.
"""
import argparse
from pathlib import Path
import struct


def wrap(payload):
    stack_top = len(payload) + 0x100
    if not payload or stack_top > 0xFFFE:
        raise ValueError('VMFORK payload and private stack must fit its PSP segment')
    size = len(payload) + 32
    header = struct.pack('<14H', 0x5A4D, size % 512, (size + 511) // 512,
                         0, 2, 1, 1, 0xFFF0, stack_top, 0, 0x100,
                         0xFFF0, 0x1C, 0)
    return header.ljust(32, b'\0') + payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('com', type=Path)
    parser.add_argument('exe', type=Path)
    args = parser.parse_args()
    args.exe.write_bytes(wrap(args.com.read_bytes()))


if __name__ == '__main__':
    main()
