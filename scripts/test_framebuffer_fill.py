#!/usr/bin/env python3
"""Execute actual JWasm-assembled CVSESSION fill and guest-span instructions."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EBX,
    UC_X86_REG_EDI, UC_X86_REG_ESI, UC_X86_REG_EDX,
)

ROOT = Path(__file__).resolve().parents[1]
FRAME = 0x400000
GUEST = 0x20000
CONTEXT = 0x30000
STACK = 0x78000
PAGE_MAP = 0x100000
ERROR_ADDRESS, ERROR_MAPPING, ERROR_ABI = 7, 8, 9


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def assemble(out):
    base = ROOT / 'build/external/jemm-monitor'
    selected = (base / (base / 'CURRENT').read_text().strip()).resolve()
    metadata = json.loads((selected / 'manifest.json').read_text())
    upstream = json.loads((ROOT / 'third_party/jemm/UPSTREAM.json').read_text())
    assert metadata['upstream'] == upstream
    tree = list((selected.parent / 'jwasm').iterdir())
    tree = [path for path in tree if path.is_dir()]
    assert len(tree) == 1
    assembler = tree[0] / upstream['toolchains']['jwasm']['binary']
    assert sha(assembler) == metadata['tools']['jwasm']['sha256']
    actual = (ROOT / 'src/vm/session_jlm.asm').read_text()
    snapshots = []
    for name in ('guest_span', 'unbind_framebuffer', 'framebuffer_page_copy'):
        match = re.search(r'^' + name + r' proc[^\n]*\n.*?^' + name + r' endp$',
                          actual, re.M | re.S)
        assert match, f'production {name} not found'
        snapshots.append(match.group(0))
    native = (ROOT / 'src/vm/session_gpu.inc').read_text()
    match = re.search(r'^gpu_savage_page_copy proc[^\n]*\n.*?^gpu_savage_page_copy endp$',
                      native, re.M | re.S)
    assert match, 'production native page-copy wrapper not found'
    snapshots.append(match.group(0))
    (out / 'guest_span_cpu.inc').write_text('\n'.join(snapshots) + '\n')
    binary = out / 'framebuffer-fill.bin'
    subprocess.run([str(assembler), '-bin', '-nologo', '-I' + str(ROOT / 'src/vm'),
                    '-I' + str(out), '-Fo' + str(binary),
                    '-Fl' + str(out / 'framebuffer-fill.lst'),
                    str(ROOT / 'scripts/fixtures/framebuffer_fill_cpu.asm')],
                   cwd=ROOT, check=True, capture_output=True, text=True)
    return binary


def execute(code, packet, *, count=32, offset=0, segment=0x2000,
            binding=FRAME, extent=65536, vm=0, unreadable=False,
            native_fill=1, sync_error=0, operation='fill', native_triangle=0,
            savage=1, mutate_during_submit=False, active=0,
            release_error=0, free_success=1, native_copy=1):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(0, 0x90000)
    uc.mem_map(PAGE_MAP, 4096)
    uc.mem_map(FRAME, 65536)
    uc.mem_write(0, code)
    fields = struct.unpack_from('<9I', code)
    assert fields[0] == 0x46464643
    routine, vmvar, fbvar, bytesvar, beginvar, accountvar, damagevar, stop = fields[1:]
    put32 = lambda at, value: uc.mem_write(at, struct.pack('<I', value))
    put32(vmvar, vm)
    put32(fbvar, binding)
    put32(bytesvar, extent)
    extended = struct.unpack_from('<28I', code)
    put32(extended[14], native_fill)
    put32(extended[15], sync_error)
    put32(extended[10], savage)
    put32(extended[11], native_triangle)
    if operation == 'triangle':
        routine = extended[9]
    if operation == 'unbind':
        routine = extended[18]
    if operation == 'page':
        routine = extended[23]
    put32(extended[24], native_copy)
    put32(extended[19], active)
    put32(extended[20], release_error)
    put32(extended[21], free_success)
    put32(extended[22], 16 if binding else 0)
    # Same permission checks as Jemm's real page-table self-map; RAM/UMB only.
    for page in range(256):
        put32(PAGE_MAP + page * 4, page * 4096 | 7)
    guest = (segment << 4) + offset
    if guest + len(packet) < 0x90000:
        uc.mem_write(guest, packet)
    if unreadable:
        put32(PAGE_MAP + (guest >> 12) * 4, 1)
    put32(CONTEXT, offset)       # Client_EDI
    put32(CONTEXT + 24, count)   # Client_ECX
    put32(CONTEXT + 60, segment) # Client_ES
    before = (bytes((i ^ (i >> 8)) & 255 for i in range(65536))
              if operation == 'page' else bytes([0xa5]) * 65536)
    uc.mem_write(FRAME, before)
    regs = {UC_X86_REG_EBX: 0x12345678, UC_X86_REG_EDI: 0x11223344,
            UC_X86_REG_ESI: 0x55667788, UC_X86_REG_EDX: 0x87654321}
    for reg, value in regs.items():
        uc.reg_write(reg, value)
    uc.reg_write(UC_X86_REG_EBP, CONTEXT)
    uc.reg_write(UC_X86_REG_ESP, STACK - 4)
    put32(STACK - 4, stop)

    def reject_mode_switch(machine, address, size, _):
        opcode = bytes(machine.mem_read(address, min(size, 3)))
        assert not opcode.startswith(b'\x0f\x22'), 'guest-style CR0 switch'
        if mutate_during_submit and address == extended[17]:
            machine.mem_write(guest, bytes([0xff]) * len(packet))
        if mutate_during_submit and address == extended[27]:
            machine.mem_write(guest, bytes([0xff]) * len(packet))

    uc.hook_add(UC_HOOK_CODE, reject_mode_switch)
    uc.emu_start(routine, stop, count=100000)
    assert uc.reg_read(UC_X86_REG_ESP) == STACK
    assert uc.reg_read(UC_X86_REG_EBP) == CONTEXT
    for reg, value in regs.items():
        assert uc.reg_read(reg) == value
    counters = [struct.unpack('<I', bytes(uc.mem_read(var, 4)))[0]
                for var in (beginvar, accountvar, damagevar)]
    if operation == 'triangle':
        calls = struct.unpack('<I', bytes(uc.mem_read(extended[12], 4)))[0]
        return (uc.reg_read(UC_X86_REG_EAX),
                bytes(uc.mem_read(extended[13], 64)), [calls, counters[2]])
    if operation == 'unbind':
        state = [struct.unpack('<I', bytes(uc.mem_read(var, 4)))[0]
                 for var in (fbvar, bytesvar, extended[22])]
        return uc.reg_read(UC_X86_REG_EAX), bytes(uc.mem_read(FRAME, 65536)), state
    if operation == 'page':
        calls = struct.unpack('<I', bytes(uc.mem_read(extended[25], 4)))[0]
        native_args = bytes(uc.mem_read(extended[26], 20))
        return (uc.reg_read(UC_X86_REG_EAX), bytes(uc.mem_read(FRAME, 65536)),
                counters + [calls], native_args)
    return uc.reg_read(UC_X86_REG_EAX), bytes(uc.mem_read(FRAME, 65536)), counters


def make_packet(offset=3, pixels=7, colour=0x89abcdef, pixelbytes=4, **bad):
    fields = [0x46465643, 0x00200100, offset, pixels, colour, pixelbytes, 0, 0]
    for key, value in bad.items():
        fields[int(key)] = value
    return struct.pack('<8I', *fields)


def page_packet(source=3, destination=4101, rowbytes=13, rows=3, pitch=32,
                reserved0=0, reserved1=0, magic=0x43465643, abi=0x00200100):
    return struct.pack('<IIIIHHIII', magic, abi, source, destination,
                       rowbytes, rows, pitch, reserved0, reserved1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'build/tests/framebuffer-fill-cpu')
    args = parser.parse_args()
    out = args.output.resolve()
    assert ROOT / 'build' in out.parents
    out.mkdir(parents=True, exist_ok=True)
    binary = assemble(out)
    code = binary.read_bytes()
    checks = []

    for bpp in (1, 2, 3, 4):
        colour = 0x89abcdef
        pixels = 7
        offset = 3
        status, actual, calls = execute(code, make_packet(offset, pixels, colour, bpp))
        expected = bytearray([0xa5] * 65536)
        expected[offset:offset + pixels * bpp] = colour.to_bytes(4, 'little')[:bpp] * pixels
        assert status == 0 and actual == bytes(expected) and calls == [1, 1, 1]
        checks.append(f'exact {bpp}-byte pixels preserve neighbouring VRAM')

    for bpp in (1, 2, 3, 4):
        pixels = 16384 // bpp
        offset = 65536 - pixels * bpp
        status, actual, calls = execute(code, make_packet(offset, pixels, 0x12345678, bpp))
        assert status == 0 and calls == [1, 1, 1]
        assert actual[offset:] == (b'\x78\x56\x34\x12'[:bpp] * pixels)
        assert actual[:offset] == bytes([0xa5]) * offset
        checks.append(f'maximum {bpp}-byte fill and exact framebuffer end')

    failures = [
        ('unbound', make_packet(), {'binding': 0}, 11),
        ('non-system VM', make_packet(), {'vm': 1}, 1),
        ('short CX', make_packet(), {'count': 31}, ERROR_ADDRESS),
        ('guest offset wraps', make_packet(), {'offset': 65520}, ERROR_ADDRESS),
        ('low guest address', make_packet(), {'segment': 0x0500}, ERROR_ADDRESS),
        ('VGA aperture guest', make_packet(), {'segment': 0xa000}, ERROR_ADDRESS),
        ('unreadable guest page', make_packet(), {'unreadable': True}, ERROR_ADDRESS),
        ('bad magic', make_packet(**{'0': 0}), {}, ERROR_ABI),
        ('bad version', make_packet(**{'1': 0x00200101}), {}, ERROR_ABI),
        ('bad packet size', make_packet(**{'1': 0x001f0100}), {}, ERROR_ABI),
        ('reserved0', make_packet(**{'6': 1}), {}, ERROR_ABI),
        ('reserved1', make_packet(**{'7': 1}), {}, ERROR_ABI),
        ('zero pixels', make_packet(pixels=0), {}, ERROR_ADDRESS),
        ('zero pixel size', make_packet(pixelbytes=0), {}, ERROR_ADDRESS),
        ('unsupported pixel size', make_packet(pixelbytes=5), {}, ERROR_ADDRESS),
        ('oversized work', make_packet(pixels=4097), {}, ERROR_ADDRESS),
        ('multiplication wraps', make_packet(pixels=0x40000000), {}, ERROR_ADDRESS),
        ('offset addition wraps', make_packet(offset=0xfffffffc), {}, ERROR_ADDRESS),
        ('past framebuffer', make_packet(offset=65535), {}, ERROR_ADDRESS),
        ('mapped address wraps', make_packet(), {'binding': 0xfffffff0}, ERROR_ADDRESS),
    ]
    for name, packet, options, expected_status in failures:
        status, actual, calls = execute(code, packet, **options)
        assert status == expected_status, (name, status, expected_status)
        assert actual == bytes([0xa5]) * 65536 and calls == [0, 0, 0], name
        checks.append(f'{name}: error before all writes')
    for name, options, status_wanted in (
        ('native fill completes without CPU stores', {'native_fill': 0}, 0),
        ('native engine failure refuses CPU fallback', {'native_fill': 2}, ERROR_MAPPING),
        ('reentered native trampoline refuses CPU fallback', {'native_fill': 0xffffffff}, ERROR_MAPPING),
        ('engine sync failure refuses CPU stores', {'sync_error': 1}, ERROR_MAPPING),
    ):
        status, actual, calls = execute(code, make_packet(), **options)
        assert status == status_wanted, (name, status)
        assert actual == bytes([0xa5]) * 65536
        assert calls == ([0, 0, 1] if status_wanted == 0 else [0, 0, 0])
        checks.append(name)
    for name, options, status_wanted, state_wanted in (
        ('inactive empty UNBIND is idempotent', {'binding': 0, 'extent': 0}, 0, [0, 0, 0]),
        ('active empty UNBIND refuses cleanup', {'binding': 0, 'extent': 0, 'active': 1}, 3, [0, 0, 0]),
        ('idle owned UNBIND releases mapping', {}, 0, [0, 0, 0]),
        ('engine cleanup failure retains mapping', {'release_error': ERROR_MAPPING}, 5, [FRAME, 65536, 16]),
        ('page free failure retains mapping', {'free_success': 0}, 5, [FRAME, 65536, 16]),
        ('active owned UNBIND retains mapping', {'active': 1}, 3, [FRAME, 65536, 16]),
    ):
        status, actual, state = execute(code, bytes(32), operation='unbind', **options)
        assert status == status_wanted and state == state_wanted, (name, status, state)
        assert actual == bytes([0xa5]) * 65536
        checks.append(name)
    triangle = (struct.pack('<4I', 0x33545643, 64, 0, 0) +
                b''.join(struct.pack('<fffI', x, y, .5, colour)
                         for x, y, colour in ((20., 20., 0xffff0000),
                                              (180., 20., 0xff00ff00),
                                              (100., 100., 0xff0000ff))))
    for mutate in (False, True):
        status, snapshot, calls = execute(code, triangle, count=64,
                                         operation='triangle',
                                         mutate_during_submit=mutate)
        assert status == 0 and snapshot == triangle and calls == [1, 1]
        checks.append('native triangle snapshots guest packet' +
                      (' before guest source mutation' if mutate else ' exactly'))
    triangle_failures = [
        ('unsupported hardware', triangle, {'savage': 0}, 1, 0),
        ('triangle non-system VM', triangle, {'vm': 1}, 1, 0),
        ('triangle unbound', triangle, {'binding': 0}, 11, 0),
        ('triangle short CX', triangle, {'count': 63}, ERROR_ADDRESS, 0),
        ('triangle guest wrap', triangle, {'offset': 65504}, ERROR_ADDRESS, 0),
        ('triangle unreadable guest', triangle, {'unreadable': True}, ERROR_ADDRESS, 0),
        ('triangle bad magic', bytes(4) + triangle[4:], {}, ERROR_ABI, 0),
        ('triangle bad size', triangle[:4] + struct.pack('<I', 63) + triangle[8:], {}, ERROR_ABI, 0),
        ('triangle bad flags', triangle[:8] + struct.pack('<I', 1) + triangle[12:], {}, ERROR_ABI, 0),
        ('triangle reserved', triangle[:12] + struct.pack('<I', 1) + triangle[16:], {}, ERROR_ABI, 0),
        ('native triangle rejects unsupported geometry', triangle, {'native_triangle': 1}, 1, 1),
        ('native triangle reports engine failure', triangle, {'native_triangle': 2}, ERROR_MAPPING, 1),
        ('native triangle trampoline reentry', triangle, {'native_triangle': 0xffffffff}, ERROR_MAPPING, 1),
    ]
    for name, packet, options, expected_status, expected_calls in triangle_failures:
        options.setdefault('count', 64)
        status, snapshot, calls = execute(code, packet, operation='triangle', **options)
        assert status == expected_status, (name, status, expected_status)
        assert calls == [expected_calls, 0]
        if expected_calls == 0:
            assert snapshot == bytes(64)
        checks.append(name)

    page_before = bytes((i ^ (i >> 8)) & 255 for i in range(65536))
    expected_page = bytearray(page_before)
    for row in range(3):
        expected_page[4101 + row*32:4114 + row*32] = page_before[3 + row*32:16 + row*32]
    for savage, mutate in ((0, False), (1, False), (1, True)):
        status, actual, calls, arguments = execute(
            code, page_packet(), operation='page', savage=savage,
            mutate_during_submit=mutate)
        assert status == 0 and actual == bytes(expected_page)
        assert calls == [1, 1, 1, savage]
        assert arguments == (struct.pack('<5I', 3, 4101, 13, 3, 32)
                             if savage else bytes(20))
        checks.append('exact disjoint pitched CPU page copy' +
                      (' after immutable native submission snapshot' if mutate else
                       ' after safe native fallback' if savage else ' without a native backend'))
    for name, options, expected_status, expected_calls in (
        ('native page copy completes without CPU VRAM stores', {'native_copy': 0}, 0, [0, 0, 1, 1]),
        ('native page copy failure forbids CPU fallback', {'native_copy': 2}, ERROR_MAPPING, [0, 0, 0, 1]),
        ('native page-copy trampoline reentry forbids CPU fallback', {'native_copy': 0xffffffff}, ERROR_MAPPING, [0, 0, 0, 1]),
        ('native page-copy fallback synchronizes before CPU stores', {'sync_error': 1}, ERROR_MAPPING, [0, 0, 0, 1]),
    ):
        status, actual, calls, arguments = execute(
            code, page_packet(source=0, destination=4096, rowbytes=16, pitch=64),
            operation='page', **options)
        assert status == expected_status and calls == expected_calls, (name, status, calls)
        assert actual == page_before and arguments == struct.pack('<5I', 0, 4096, 16, 3, 64)
        checks.append(name)
    page_failures = (
        ('page copy non-system VM', page_packet(), {'vm': 1}, 1),
        ('page copy unbound', page_packet(), {'binding': 0}, 11),
        ('page copy short CX', page_packet(), {'count': 31}, ERROR_ADDRESS),
        ('page copy unreadable guest', page_packet(), {'unreadable': True}, ERROR_ADDRESS),
        ('page copy bad magic', page_packet(magic=0), {}, ERROR_ABI),
        ('page copy wrong ABI', page_packet(abi=0x00200101), {}, ERROR_ABI),
        ('page copy reserved field', page_packet(reserved1=1), {}, ERROR_ABI),
        ('page copy zero row size', page_packet(rowbytes=0), {}, ERROR_ADDRESS),
        ('page copy zero rows', page_packet(rows=0), {}, ERROR_ADDRESS),
        ('page copy excessive row count', page_packet(rows=65), {}, ERROR_ADDRESS),
        ('page copy excessive payload', page_packet(rows=64, rowbytes=257, pitch=512), {}, ERROR_ADDRESS),
        ('page copy short pitch', page_packet(pitch=12), {}, ERROR_ADDRESS),
        ('page copy pitched extent overflow', page_packet(rows=64, pitch=0xffffffff), {}, ERROR_ADDRESS),
        ('page copy source outside mapping', page_packet(source=65536), {}, ERROR_ADDRESS),
        ('page copy destination outside mapping', page_packet(destination=65536), {}, ERROR_ADDRESS),
        ('page copy bounding spans overlap', page_packet(destination=16), {}, ERROR_ADDRESS),
        ('page copy mapped address wraps', page_packet(), {'binding': 0xfffffff0}, ERROR_ADDRESS),
    )
    for name, packet, options, expected_status in page_failures:
        status, actual, calls, arguments = execute(code, packet, operation='page', **options)
        assert status == expected_status, (name, status, expected_status)
        assert actual == page_before and calls == [0, 0, 0, 0] and arguments == bytes(20), name
        checks.append(name + ': rejected before CPU or GPU writes')
    record = {'status': 'PASS', 'checks': checks, 'count': len(checks),
              'scope': 'Actual production JWasm instructions, Unicorn 32-bit CPU; no hardware engine claim',
              'artifact_sha256': sha(binary),
              'production_fill_sha256': sha(ROOT / 'src/vm/session_framebuffer_fill.inc'),
              'production_triangle_sha256': sha(ROOT / 'src/vm/session_framebuffer_triangle.inc'),
              'production_guest_span_snapshot_sha256': sha(out / 'guest_span_cpu.inc')}
    (out / 'report.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'PASS: {len(checks)} protected framebuffer fill/triangle/page-copy transport checks')


if __name__ == '__main__':
    main()
