"""Small original AML fixtures, encoded using ACPI §20 PkgLength grammar."""
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import struct

def pkg(op, body):
    for count in range(1, 5):
        length = len(body) + count
        if length < (64 if count == 1 else 1 << (4 + 8 * (count - 1))):
            enc = bytes([length]) if count == 1 else bytes([((count - 1) << 6) | (length & 15)]) + (length >> 4).to_bytes(count - 1, 'little')
            return op + enc + body
    raise ValueError('fixture too long')

def integer(n):
    if n < 2: return bytes([n])
    if n < 256: return b'\x0a' + bytes([n])
    return b'\x0c' + struct.pack('<I', n)

def string(s): return b'\x0d' + s.encode() + b'\0'
def name(n, value): return b'\x08' + n.encode() + value
def method(n, body): return pkg(b'\x14', n.encode() + b'\x00' + body)
def device(n, body): return pkg(b'\x5b\x82', n.encode() + body)
def scope(n, body): return pkg(b'\x10', n + body)
def buffer(data): return pkg(b'\x11', integer(len(data)) + data)
def package(items): return pkg(b'\x12', bytes([len(items)]) + b''.join(items))

def sdt(sig, body):
    data = bytearray(struct.pack('<4sIBB6s8sI4sI', sig.encode(), 36 + len(body), 2, 0, b'CIUKI ', b'FWTEST  ', 1, b'CIUK', 1) + body)
    data[9] = -sum(data) & 255
    return data

def generate(out):
    out = Path(out)
    crs = b'\x4b\x60\x00\x01\x22\x02\x00\x79\x00'
    base = device('PCI0', name('_HID', string('PNP0A03')) + name('_CID', package([string('PNP0A08'), string('ACME0001')])) +
                  device('KBD0', method('_HID', b'\xa4' + string('PNP0303')) + name('_CRS', buffer(crs))))
    base += device('LID0', name('_HID', string('PNP0C0D')) + method('_LID', b'\xa4\x01') +
                   method('_INI', pkg(b'\xa2', b'\x01\xa3')))  # Must never execute _INI.
    base += device('BAT0', name('_HID', string('PNP0C0A')) + name('_BST', integer(0)))
    base += device('EC00', name('_HID', string('PNP0C09')))
    base = scope(b'\\_SB_', base)
    base += scope(b'\\_TZ_', pkg(b'\x5b\x85', b'THRM' + name('_TMP', integer(3000))))
    ssdt = sdt('SSDT', scope(b'\\_SB_', device('ACAD', name('_HID', string('ACPI0003')) + name('_PSR', integer(1)))))
    cases = ('inventory', 'ssdt', 'checksum', 'root64', 'dsdt64', 'length', 'aml', 'loop', 'sleep', 'write', 'region', 'osl', 'broker', 'pnp', 'page', 'allocation',
             'crs-bad', 'mem64', 'large', 'module-loop', 'rsdt', 'rsdp-checksum', 'pnp-bad', 'no-acpi', 'eisa', 'ec', 'module-write', 'start-failure',
             'serial', 'serial-disabled', 'serial-cid', 'serial-level', 'serial-low', 'serial-variable')
    for case in cases:
        aml = base
        if case.startswith('serial'):
            resources=b'\x4b\xf8\x03\x08\x23\x10\x00' + bytes([0 if case=='serial-level' else 9 if case=='serial-low' else 1]) + b'\x79\x00'
            if case=='serial-variable': resources=b'\x47\x01\xf8\x03\xf8\x04\x08\x08\x23\x10\x00\x01\x79\x00'
            hid=name('_HID',string('ACME0001' if case=='serial-cid' else 'PNP0501'))
            if case=='serial-cid': hid+=name('_CID',package([string('PNP0500'),string('PNP0501')]))
            sta=name('_STA',integer(0 if case=='serial-disabled' else 15))
            aml=scope(b'\\_SB_',device('COM0',hid+sta+name('_CRS',buffer(resources))))
        if case in ('crs-bad', 'mem64', 'eisa'):
            resources = b'\x86\xff\xff' if case == 'crs-bad' else b'\x8a\x2b\x00' + bytes([0, 0x0d, 0]) + struct.pack('<QQQQQ', 0, 0x100000000, 0x100000fff, 0, 0x1000) + b'\x79\x00'
            hid = integer(0x0303d041) if case == 'eisa' else string('ACME0001')
            aml = scope(b'\\_SB_', device('TST0', name('_HID', hid) + (name('_CRS', buffer(resources)) if case != 'eisa' else b'')))
        if case == 'large':
            aml = scope(b'\\_SB_', b''.join(device(f'D{i:03d}', name('_HID', string('ACME0001'))) for i in range(70)))
        if case == 'module-loop': aml = pkg(b'\xa2', b'\x01\xa3')
        if case in ('loop', 'sleep', 'write', 'region'):
            region = b'\x5b\x80' + b'IO00\x01' + integer(0x80) + integer(1)
            field = pkg(b'\x5b\x81', b'IO00\x01' + b'REG0\x08')
            body = pkg(b'\xa2', b'\x01\xa3') if case == 'loop' else b'\x5b\x22' + integer(10000) if case == 'sleep' else b'\x70\x01REG0' if case == 'write' else b'\xa4REG0'
            aml = scope(b'\\_SB_', region + field + device('BAD0', method('_HID', body)))
        if case in ('ec', 'module-write'):
            region = b'\x5b\x80IO00' + bytes([3 if case == 'ec' else 1]) + integer(0x80) + integer(1)
            field = pkg(b'\x5b\x81', b'IO00\x01REG0\x08')
            aml = scope(b'\\_SB_', region + field + (device('BAD0', method('_HID', b'\xa4REG0')) if case == 'ec' else b'\x70\x01REG0'))
        if case == 'aml': aml = b'\x5b\x82\x4f'  # Truncated device/PkgLength.
        dsdt = sdt('DSDT', aml)
        if case == 'checksum': dsdt[-1] ^= 1
        if case == 'length': dsdt[4:8] = struct.pack('<I', 0x40001)
        image = bytearray(0x100000)
        image[0x10000:0x10000 + len(dsdt)] = dsdt
        image[0x12000:0x12000 + len(ssdt)] = ssdt
        fadt = sdt('FACP', bytes(240))
        struct.pack_into('<I', fadt, 40, 0x10000)
        if case == 'dsdt64': struct.pack_into('<Q', fadt, 140, 0x100010000)
        fadt[9] = 0; fadt[9] = -sum(fadt) & 255
        image[0x14000:0x14000 + len(fadt)] = fadt
        root = sdt('XSDT', struct.pack('<QQ', 0x100014000 if case == 'root64' else 0x14000, 0x12000))
        if case == 'rsdt': root = sdt('RSDT', struct.pack('<II', 0x14000, 0x12000))
        image[0x16000:0x16000 + len(root)] = root
        rsdp = bytearray(b'RSD PTR ' + bytes(1) + b'CIUKI ' + bytes([2]) + bytes(4) + struct.pack('<IQ', 36, 0x16000) + bytes(4))
        rsdp[8] = -sum(rsdp[:20]) & 255; rsdp[32] = -sum(rsdp) & 255
        if case == 'rsdt':
            rsdp[15] = 0; struct.pack_into('<I', rsdp, 16, 0x16000)
            rsdp[8] = 0; rsdp[8] = -sum(rsdp[:20]) & 255
        if case == 'rsdp-checksum': rsdp[8] ^= 1
        image[0xe0000:0xe0000 + len(rsdp)] = rsdp
        if case in ('pnp', 'pnp-bad'):
            pnp = bytearray(b'$PnP\x10\x21' + bytes(27)); pnp[8] = -sum(pnp) & 255
            if case == 'pnp-bad': pnp[8] ^= 1
            image[0xf0000:0xf0000 + len(pnp)] = pnp
        (out / (case + '.bin')).write_bytes(image)
