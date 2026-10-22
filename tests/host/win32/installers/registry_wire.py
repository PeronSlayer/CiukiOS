"""Copied w32srv request helpers for independent installer manifests."""
# SPDX-License-Identifier: MIT
import struct
OPEN, CREATE, CLOSE, SET, QUERY, DELETE_KEY, DELETE_VALUE = range(1, 8)
HKLM = 0x80000002


def request(op, key, path='', name='', data=b'', type_=0):
    p, n = path.encode() + b'\0', name.encode() + b'\0'
    return struct.pack('<10I', 1, op, key, 0, type_, 0x2001f, 0, len(p), len(n), len(data)) + p + n + data
