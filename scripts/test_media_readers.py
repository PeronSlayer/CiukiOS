#!/usr/bin/env python3
"""Execute the production 16-bit MEDIA.COM against independent disk fixtures.

This verifies parser/BIOS-register/error behavior; QEMU tests separately verify
real emulated device I/O. No claim about physical USB host-controller support.
"""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_INSN
from unicorn.x86_const import *


def command(*args):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def fat_layout(data, bits):
    reserved = struct.unpack_from("<H", data, 14)[0]
    fatsize = struct.unpack_from("<H", data, 22)[0] or struct.unpack_from("<I", data, 36)[0]
    roots = struct.unpack_from("<H", data, 17)[0]
    root_sector = reserved + data[16]*fatsize
    first_data = root_sector + (roots*32+511)//512
    root_offset = (first_data if bits == 32 else root_sector)*512
    entry = data.index(b"PAYLOAD BIN", root_offset, root_offset+512)
    cluster = struct.unpack_from("<H", data, entry+26)[0]
    if bits == 32:
        cluster |= struct.unpack_from("<H", data, entry+20)[0] << 16
    return reserved, fatsize, first_data, entry, cluster


def fat_value(data, bits, reserved, cluster):
    offset = reserved*512 + (cluster+cluster//2 if bits == 12 else cluster*(bits//8))
    if bits == 12:
        n = struct.unpack_from("<H", data, offset)[0]
        return (n >> 4 if cluster & 1 else n) & 0xfff
    return struct.unpack_from("<H" if bits == 16 else "<I", data, offset)[0]


def set_fat_value(data, bits, reserved, fatsize, cluster, value):
    for table in range(data[16]):
        offset = (reserved+table*fatsize)*512 + (cluster+cluster//2 if bits == 12 else cluster*(bits//8))
        if bits == 12:
            previous = struct.unpack_from("<H", data, offset)[0]
            value16 = (previous & 15) | value << 4 if cluster & 1 else (previous & 0xf000) | value
            struct.pack_into("<H", data, offset, value16)
        else:
            struct.pack_into("<H" if bits == 16 else "<I", data, offset, value)


def fragment(image, bits, binary):
    data = bytearray(image.read_bytes())
    reserved, fatsize, first_data, entry, cluster = fat_layout(data, bits)
    old = []
    while cluster < {12: 0xff8, 16: 0xfff8, 32: 0xffffff8}[bits]:
        old.append(cluster)
        cluster = fat_value(data, bits, reserved, cluster)
    # Cluster341's FAT12 entry straddles bytes511/512 of the table.
    new = [341, 511, 342, 700] + list(range(900, 900+len(old)-4))
    assert len(old) == len(new)
    for old_cluster in old:
        set_fat_value(data, bits, reserved, fatsize, old_cluster, 0)
    for i, new_cluster in enumerate(new):
        assert fat_value(data, bits, reserved, new_cluster) == 0
        start = (first_data+new_cluster-2)*512
        chunk = binary[i*512:(i+1)*512]
        data[start:start+512] = chunk.ljust(512, b"\x00")
        set_fat_value(data, bits, reserved, fatsize, new_cluster,
                      new[i+1] if i+1 < len(new) else {12: 0xfff, 16: 0xffff, 32: 0xfffffff}[bits])
    struct.pack_into("<H", data, entry+26, new[0])
    if bits == 32:
        struct.pack_into("<H", data, entry+20, 0)
    image.write_bytes(data)
    # Independent mtools implementation proves our fragmented fixture still
    # carries the original payload and the expected filesystem structure.
    assert command("mtype", "-i", str(image), "::PAYLOAD.BIN") == binary


def fixtures(directory):
    directory.mkdir(parents=True, exist_ok=True)
    payload = b"CiukiOS external-media regression: exact bytes, fragmented FAT chains.\r\n" * 130
    binary = bytes(range(256)) * 35 + b"END\x00\xff"
    (directory / "README.TXT").write_bytes(payload)
    (directory / "PAYLOAD.BIN").write_bytes(binary)
    result = {}
    for bits, size in [(12, 1474560), (16, 16777216), (32, 67108864)]:
        image = directory / f"fat{bits}.img"
        with image.open("wb") as stream:
            stream.truncate(size)
        command("mkfs.fat", "-F", str(bits), "-s", "1", str(image))
        command("mmd", "-i", str(image), "::NESTED")
        command("mmd", "-i", str(image), "::NESTED/DEEP")
        command("mcopy", "-i", str(image), str(directory / "README.TXT"), "::NESTED/DEEP/README.TXT")
        command("mcopy", "-i", str(image), str(directory / "PAYLOAD.BIN"), "::PAYLOAD.BIN")
        fragment(image, bits, binary)
        assert command("mtype", "-i", str(image), "::NESTED/DEEP/README.TXT") == payload
        result[f"fat{bits}"] = image
    iso_root = directory / "iso-root"
    (iso_root / "NESTED" / "DEEP").mkdir(parents=True, exist_ok=True)
    (iso_root / "NESTED" / "DEEP" / "README.TXT").write_bytes(payload)
    (iso_root / "PAYLOAD.BIN").write_bytes(binary)
    iso = directory / "data.iso"
    command("xorriso", "-as", "mkisofs", "-quiet", "-iso-level", "1", "-o", str(iso), str(iso_root))
    result["iso"] = iso
    return result, payload, binary


class Machine:
    def __init__(self, program, tail, devices, fail_lba=None, clobber=True, segment=0x2000, dma_boundary=False, keys=""):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, 0x200000)
        self.segment = segment
        self.dma_boundary = dma_boundary
        self.start = self.segment * 16
        self.uc.mem_write(self.start + 0x100, program)
        self.uc.mem_write(self.start + 0x80, bytes([len(tail)]) + tail.encode("ascii"))
        for reg in [UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS]:
            self.uc.reg_write(reg, self.segment)
        self.uc.reg_write(UC_X86_REG_SP, 0xfffe)
        self.uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
        self.devices, self.fail_lba, self.clobber = devices, fail_lba, clobber
        self.output = bytearray()
        self.keys = list(keys.encode("ascii"))
        self.files = {}
        self.handles = {}
        self.reads = []
        self.exit = None
        self.uc.hook_add(UC_HOOK_INTR, self.interrupt)
        self.in_hook = self.uc.hook_add(UC_HOOK_INSN, lambda *_: 0xff, None, 1, 0, UC_X86_INS_IN)
        self.uc.hook_add(UC_HOOK_INSN, lambda *_: None, None, 1, 0, UC_X86_INS_OUT)

    def string(self, address):
        data = bytearray()
        while True:
            c = self.uc.mem_read(address + len(data), 1)[0]
            if not c:
                return data.decode("ascii")
            data.append(c)

    def carry(self, value):
        flags = self.uc.reg_read(UC_X86_REG_EFLAGS)
        self.uc.reg_write(UC_X86_REG_EFLAGS, (flags & ~1) | int(value))

    def interrupt(self, uc, number, _):
        ax, bx, cx, dx, si = [uc.reg_read(r) & 65535 for r in
                              [UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_SI]]
        ds, es = uc.reg_read(UC_X86_REG_DS)*16, uc.reg_read(UC_X86_REG_ES)*16
        ah = ax >> 8
        self.carry(False)
        if number == 0x21:
            if ah == 0x02:
                self.output.append(dx & 255)
            elif ah == 0x4a:
                pass
            elif ah == 0x4c:
                self.exit = ax & 255
                uc.emu_stop()
            elif ah == 0x3d:
                path = self.string(ds+dx)
                if path not in self.files:
                    self.carry(True)
                    uc.reg_write(UC_X86_REG_AX, 2)
                else:
                    handle = len(self.handles)+5
                    self.handles[handle] = path
                    uc.reg_write(UC_X86_REG_AX, handle)
            elif ah == 0x3c:
                path = self.string(ds+dx)
                if path in self.files:
                    self.carry(True)
                    uc.reg_write(UC_X86_REG_AX, 80)
                else:
                    self.files[path] = bytearray()
                    handle = len(self.handles)+5
                    self.handles[handle] = path
                    uc.reg_write(UC_X86_REG_AX, handle)
            elif ah == 0x40:
                self.files[self.handles[bx]].extend(uc.mem_read(ds+dx, cx))
                uc.reg_write(UC_X86_REG_AX, cx)
            elif ah == 0x3e:
                del self.handles[bx]
            elif ah == 0x41:
                self.files.pop(self.string(ds+dx), None)
            else:
                raise AssertionError(f"unexpected DOS function {ax:04x}")
            return
        if number == 0x16:
            assert self.keys, "Unexpected keyboard wait"
            uc.reg_write(UC_X86_REG_AX, self.keys.pop(0))
            return
        if number != 0x13:
            raise AssertionError(f"unexpected interrupt {number:02x}")
        device = self.devices.get(dx & 255)
        if not device:
            self.carry(True)
            uc.reg_write(UC_X86_REG_AX, 0x0100)
            return
        data, block_size, extended = device
        if ah == 0x41:
            self.carry(not extended)
            uc.reg_write(UC_X86_REG_BX, 0xaa55)
            uc.reg_write(UC_X86_REG_CX, 1)
        elif ah == 0x48:
            params = bytearray(74)
            struct.pack_into("<H", params, 0, 30)
            struct.pack_into("<QH", params, 16, len(data)//block_size, block_size)
            uc.mem_write(ds+si, bytes(params))
        elif ah == 0x08:
            uc.reg_write(UC_X86_REG_CX, (79 << 8) | 18)
            uc.reg_write(UC_X86_REG_DX, 1 << 8 | 1)
        elif ah in [0x42, 0x02]:
            if ah == 0x42:
                dap = bytes(uc.mem_read(ds+si, 16))
                size, _, count, offset, segment, lba = struct.unpack("<BBHHHQ", dap)
                assert size == 16 and count == 1, "DAP not reinitialized"
                destination = segment*16+offset
                # Real BIOSes modify the count after partial/failed I/O.
                uc.mem_write(ds+si+2, b"\x00\x00")
            else:
                cylinder = (cx >> 8) | ((cx & 0xc0) << 2)
                lba = (cylinder*2 + (dx>>8))*18+(cx&63)-1
                destination = es+bx
            self.reads.append(lba)
            if (lba == self.fail_lba or (lba+1)*block_size > len(data) or
                    (self.dma_boundary and (destination & 0xffff)+block_size > 65536)):
                self.carry(True)
                uc.reg_write(UC_X86_REG_AX, 0x2000)
            else:
                uc.mem_write(destination, bytes(data[lba*block_size:(lba+1)*block_size]))
                uc.reg_write(UC_X86_REG_AX, 0)
        elif ah != 0:
            raise AssertionError(f"unexpected BIOS function {ax:04x}")
        if self.clobber:
            # AH=08 legitimately returns ES:DI. Adversarial upper halves and
            # other segments must never affect the C caller or its stack.
            uc.reg_write(UC_X86_REG_ES, 0xf000)
            uc.reg_write(UC_X86_REG_DS, 0xa000)
            uc.reg_write(UC_X86_REG_FS, 0xb000)
            uc.reg_write(UC_X86_REG_GS, 0xc000)
            for reg in [UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP]:
                uc.reg_write(reg, uc.reg_read(reg) | 0xa5a50000)

    def run(self):
        self.uc.emu_start(self.start+0x100, 0x200000, count=20000000)
        assert self.exit is not None, "MEDIA hung or exceeded instruction budget"
        return self


def test_atapi_deadline(program, elf, initial_tick):
    symbols = {}
    for line in command("ia16-elf-nm", str(elf)).decode().splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    vm = Machine(program, "", {})
    vm.uc.hook_del(vm.in_hook)
    polls = 0

    def busy(uc, port, size, _):
        nonlocal polls
        assert port == 0x1f7 and size == 1
        polls += 1
        uc.mem_write(0x46c, struct.pack("<H", (initial_tick + polls//16) & 65535))
        return 0x80

    vm.uc.hook_add(UC_HOOK_INSN, busy, None, 1, 0, UC_X86_INS_IN)
    vm.uc.mem_write(0x46c, struct.pack("<H", initial_tick))
    vm.uc.mem_write(vm.start+symbols["atapi_base"], struct.pack("<H", 0x1f0))
    vm.uc.reg_write(UC_X86_REG_SP, 0xfffa)
    vm.uc.mem_write(vm.start+0xfffa, struct.pack("<HH", 0x100, 1))
    vm.uc.emu_start(vm.start+symbols["atapi_wait"], vm.start+0x100, count=1000000)
    assert vm.uc.reg_read(UC_X86_REG_IP) == 0x100 and vm.uc.reg_read(UC_X86_REG_AX) == 0
    assert 91*16 <= polls < 100*16, polls


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--program", type=Path, default=Path("build/full/media-work/media.com"))
    parser.add_argument("--output", type=Path, default=Path("build/full/media-work/tests"))
    parser.add_argument("--elf", type=Path, default=Path("build/full/media-work/media.elf"))
    args = parser.parse_args()
    paths, payload, binary = fixtures(args.output / "fixtures")
    program = args.program.read_bytes()
    results = []
    for kind, path in paths.items():
        is_cd = kind == "iso"
        drive = 0xe0 if is_cd else 0 if kind == "fat12" else 0x81
        device = "CD" if is_cd else "FLOPPY" if drive == 0 else "USB:81"
        data = path.read_bytes()
        devices = {drive: (data, 2048 if is_cd else 512, drive != 0)}
        for action in ["DIR /NESTED/DEEP", "TYPE /NESTED/DEEP/README.TXT", "COPY /PAYLOAD.BIN C:\\READBACK.BIN"]:
            vm = Machine(program, device + " " + action, devices).run()
            (args.output / (kind+"-"+action.split()[0].lower()+".log")).write_bytes(vm.output)
            assert vm.exit == 0, vm.output.decode(errors="replace")
            if action.startswith("DIR"):
                assert b"README.TXT" in vm.output and str(len(payload)).encode() in vm.output
            elif action.startswith("TYPE"):
                assert payload in vm.output
            else:
                assert vm.files["C:\\READBACK.BIN"] == binary
            results.append({"filesystem": kind, "action": action, "reads": len(vm.reads), "status": "PASS"})
        missing = Machine(program, device + " TYPE /MISSING.TXT", devices).run()
        assert missing.exit == 1 and b"not found" in missing.output
        results.append({"filesystem": kind, "action": "missing file", "status": "PASS"})
    bad = bytearray(paths["fat16"].read_bytes())
    bad[13] = 3
    vm = Machine(program, "USB:81 DIR", {0x81: (bad, 512, True)}).run()
    assert vm.exit == 1
    results.append({"action": "reject invalid FAT geometry", "status": "PASS"})
    vm = Machine(program, "FLOPPY DIR", {0: (paths["fat12"].read_bytes(), 512, False)}, fail_lba=0).run()
    assert vm.exit == 1 and len(vm.reads) == 3
    results.append({"action": "bounded BIOS failure", "status": "PASS"})
    for bits in (12, 16, 32):
        data = bytearray(paths[f"fat{bits}"].read_bytes())
        reserved, fatsize, _, _, cluster = fat_layout(data, bits)
        device, letter = ("FLOPPY", 0) if bits == 12 else ("USB:81", 0x81)
        for failure, value in [("cyclic chain", cluster), ("truncated chain", {12: 0xfff, 16: 0xffff, 32: 0xfffffff}[bits])]:
            broken = bytearray(data)
            set_fat_value(broken, bits, reserved, fatsize, cluster, value)
            vm = Machine(program, device + " COPY /PAYLOAD.BIN C:\\PARTIAL.BIN", {letter: (broken, 512, letter != 0)}).run()
            assert vm.exit == 1 and "C:\\PARTIAL.BIN" not in vm.files, vm.output
            results.append({"filesystem": f"fat{bits}", "action": failure + ": remove partial copy", "status": "PASS"})
    data = paths["fat16"].read_bytes()
    mbr = bytearray(63*512)
    mbr[510:512] = b"\x55\xaa"
    mbr[450] = 6
    struct.pack_into("<II", mbr, 454, 63, len(data)//512)
    vm = Machine(program, "USB:81 COPY /PAYLOAD.BIN C:\\PART.BIN", {0x81: (bytes(mbr)+data, 512, True)}).run()
    assert vm.exit == 0 and vm.files["C:\\PART.BIN"] == binary
    results.append({"action": "MBR partition offset", "status": "PASS"})
    vm = Machine(program, "USB:81 COPY /PAYLOAD.BIN C:\\EXISTS.BIN", {0x81: (data, 512, True)})
    vm.files["C:\\EXISTS.BIN"] = bytearray(b"KEEP EXISTING DATA")
    vm.run()
    assert vm.exit == 1 and vm.files["C:\\EXISTS.BIN"] == b"KEEP EXISTING DATA"
    results.append({"action": "refuse destination overwrite", "status": "PASS"})
    vm = Machine(program, "FLOPPY", {0: (paths["fat12"].read_bytes(), 512, False)},
                 keys="cd nested\rcd deep\rdir\rtype README.TXT\rcd ..\rcd ..\rdir\rexit\r").run()
    assert vm.exit == 0 and payload in vm.output and b"Media /nested/deep>" in vm.output
    assert vm.output.count(b"Media />") == 3, vm.output[-800:]
    results.append({"action": "interactive browser nested navigation and read", "status": "PASS"})
    vm = Machine(program, "FLOPPY", {}, keys="\x1b").run()
    assert vm.exit == 1 and b"Press any key to return to CiukiOS." in vm.output and not vm.keys
    results.append({"action": "desktop media error remains until dismissed", "status": "PASS"})
    # Several PSP placements exercise physical DMA boundaries independently
    # of a segment-local buffer offset. BIOS rejects crossing transfers.
    for segment in (0x2000, 0x3c00, 0x3d00, 0x3e00, 0x3f00):
        for drive, device, fixture, size in [(0, "FLOPPY", "fat12", 512), (0xe0, "CD", "iso", 2048)]:
            vm = Machine(program, device + " COPY /PAYLOAD.BIN C:\\DMA.BIN",
                         {drive: (paths[fixture].read_bytes(), size, drive != 0)},
                         segment=segment, dma_boundary=True).run()
            assert vm.exit == 0 and vm.files["C:\\DMA.BIN"] == binary, vm.output
        results.append({"action": f"DMA-safe transfers at PSP {segment:04X}", "status": "PASS"})
    report = {"program_sha256": hashlib.sha256(program).hexdigest(), "checks": results}
    for tick in (0, 0xfff0):
        test_atapi_deadline(program, args.elf, tick)
        results.append({"action": f"busy ATAPI deadline, initial tick {tick:04X}", "status": "PASS"})
    (args.output / "result.json").write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
