#!/usr/bin/env python3
"""Build the deterministic private FAT16 DOS VM payload (never a floppy)."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SECTORS, SPC, RESERVED, FAT_SECTORS, ROOT_SECTORS = 262144, 8, 73, 128, 32
DATA_LBA = RESERVED + 2 * FAT_SECTORS + ROOT_SECTORS
DATE = ((2026 - 1980) << 9) | (10 << 5) | 11
GEOMETRY = {"FAT_SPT": 63, "FAT_HEADS": 16, "FAT_RESERVED_SECTORS": RESERVED,
            "FAT_SECTORS_PER_CLUSTER": SPC, "FAT_SECTORS_PER_FAT": FAT_SECTORS,
            "FAT_ROOT_DIR_SECTORS": ROOT_SECTORS, "FAT_TYPE": 16,
            "FAT_TOTAL_SECTORS": SECTORS, "FAT_LBA_OFFSET": 0,
            "DOS_DEFAULT_DRIVE_INDEX": 2}
PERSONALITY = {"CIUKIDOS_EXTERNAL_XMS": 1, "STAGE1_SELFTEST_AUTORUN": 0,
               "STAGE1_RUNTIME_PROBE": 0, "STAGE1_DEBUG_COMMANDS": 0,
               "TRACE_CHILD_INT21": 0, "TRACE_WIN_INT2F": 0,
               "TRACE_WIN_MEMORY": 0, "TRACE_WIN_XMS": 0,
               "STAGE1_BOOT_EXTERNAL_SHELL": 1, "STAGE2_AUTORUN": 0,
               "HARDWARE_VALIDATION_SCREEN": 0, "ENABLE_PS2_MOUSE_INIT": 0,
               "WOLF_RUNTIME_DIAG": 0}


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def assemble(source, output, definitions=None):
    command = ["nasm", "-f", "bin", str(ROOT / source), "-o", str(output)]
    command += [f"-D{name}={value}" for name, value in (definitions or {}).items()]
    subprocess.run(command, cwd=ROOT, check=True)
    return output.read_bytes()


def entry(name, attr, cluster, size):
    # All fields, including directory and file timestamps, are deterministic.
    record = bytearray(32)
    record[:11] = name.encode("ascii").ljust(11, b" ")
    record[11] = attr
    struct.pack_into("<HHH", record, 14, 0, DATE, DATE)
    struct.pack_into("<HHHI", record, 22, 0, DATE, cluster, size)
    return record


def shortname(name):
    parts = name.upper().split(".")
    if len(parts) > 2 or not 1 <= len(parts[0]) <= 8 or (len(parts) == 2 and not 1 <= len(parts[1]) <= 3):
        raise ValueError("DOS payload requires 8.3 names")
    return parts[0].ljust(8) + (parts[1] if len(parts) == 2 else "").ljust(3)


def write_fat16(out, boot, loader, files):
    if len(boot) != 512 or boot[510:] != b"\x55\xaa" or len(loader) > 4096:
        raise ValueError("DOS boot/loader bounds")
    clusters = (SECTORS - DATA_LBA) // SPC
    if not 4085 <= clusters < 65525 or clusters + 2 > FAT_SECTORS * 256:
        raise ValueError("FAT16 cluster-count/capacity mismatch")
    fat = bytearray(FAT_SECTORS * 512)
    struct.pack_into("<HH", fat, 0, 0xFFF8, 0xFFFF)
    root = bytearray(ROOT_SECTORS * 512)
    system = bytearray(SPC * 512)
    next_cluster = 2

    def allocate(data):
        nonlocal next_cluster
        count = (len(data) + SPC * 512 - 1) // (SPC * 512)
        if not count:
            return 0
        first = next_cluster
        if next_cluster + count > clusters + 2:
            raise ValueError("DOS payload volume full")
        for i in range(count):
            cluster = next_cluster
            struct.pack_into("<H", fat, cluster * 2, 0xFFFF if i + 1 == count else cluster + 1)
            stream.seek((DATA_LBA + (cluster - 2) * SPC) * 512)
            stream.write(data[i * SPC * 512:(i + 1) * SPC * 512])
            next_cluster += 1
        return first

    with out.open("w+b") as stream:
        stream.truncate(SECTORS * 512)
        stream.write(boot)
        stream.write(loader)
        directory_cluster = allocate(system)
        root[:32] = entry("CIUKIOSFULL", 8, 0, 0)
        root[32:64] = entry(shortname("SYSTEM"), 0x10, directory_cluster, 0)
        system[:32] = entry(".", 0x10, directory_cluster, 0)
        system[32:64] = entry("..", 0x10, 0, 0)
        root_index, system_index = 2, 2
        for path, data in sorted(files.items()):
            cluster = allocate(data)
            parent, name = path.rsplit("/", 1)
            record = entry(shortname(name), 0x20, cluster, len(data))
            if parent == "/SYSTEM":
                system[system_index * 32:(system_index + 1) * 32] = record
                system_index += 1
            elif not parent:
                root[root_index * 32:(root_index + 1) * 32] = record
                root_index += 1
            else:
                raise ValueError("unapproved DOS payload directory")
        stream.seek(DATA_LBA * 512)
        stream.write(system)
        for lba in (RESERVED, RESERVED + FAT_SECTORS):
            stream.seek(lba * 512)
            stream.write(fat)
        stream.seek((RESERVED + 2 * FAT_SECTORS) * 512)
        stream.write(root)


def build(out=None):
    out = Path(out or ROOT / "build/f0/dos/DOSBOOT.IMG")
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".dos-", dir=out.parent) as temporary:
        work = Path(temporary)
        boot = assemble("src/boot/full_boot.asm", work / "boot.bin", {"FAT_TOTAL_SECTORS": SECTORS})
        loader = assemble("src/boot/full_stage1_loader.asm", work / "loader.bin", GEOMETRY)
        kernel = assemble("src/runtime/ciukidos.asm", work / "CIUKIDOS.SYS", {**GEOMETRY, **PERSONALITY})
        subprocess.run(["python3", "scripts/ciukidos_image.py", "--kernel", str(work / "CIUKIDOS.SYS"), "--json"],
                       cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        command = assemble("src/com/shell.asm", work / "COMMAND.COM", {"COMMAND_COMPAT": 1})
        shell = assemble("src/com/dosvm_shell.asm", work / "SHELL.COM")
        if len(command) > 0xFF00 or len(shell) > 0xFF00:
            raise ValueError("DOS shell exceeds COM extent")
        files = {"/SYSTEM/CIUKIDOS.SYS": kernel, "/SYSTEM/SHELL.COM": shell,
                 "/COMMAND.COM": command, "/AUTOEXEC.BAT": b"\\DOSREADY.COM\r\n"}
        for source, name in (("dosready", "DOSREADY.COM"), ("memory", "MEMORY.COM"),
                             ("command_status", "CMDSTAT.COM"), ("exit37", "EXIT37.COM"),
                             ("vmpriv", "VMPRIV.COM"), ("vmport", "VMPORT.COM"),
                             ("vmdevices", "VMDEV.COM")):
            files["/" + name] = assemble(f"src/probes/doscompat/{source}.asm", work / name)
        files["/VMMEMORY.COM"] = assemble("src/probes/dos/vm_memory.asm", work / "VMMEMORY.COM")  # f3-03 memory conformance payload
        candidate = work / "DOSBOOT.IMG"
        write_fat16(candidate, boot, loader, files)
        subprocess.run(["fsck.fat", "-n", str(candidate)], check=True)
        for name, data in files.items():
            actual = subprocess.run(["mcopy", "-i", str(candidate), "::" + name, "-"],
                                    check=True, capture_output=True).stdout
            if actual != data:
                raise ValueError(f"DOS payload read-back mismatch: {name}")
        candidate.replace(out)
    manifest = {"schema_version": 1, "image_sha256": digest(out), "size": SECTORS * 512,
                "geometry": GEOMETRY, "personality": PERSONALITY,
                "shell": "src/com/shell.asm COMMAND_COMPAT=1; dosvm_shell.asm AUTOEXEC/exit wrapper",
                "payloads": [{"path": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                             for name, data in sorted(files.items())]}
    out.with_suffix(".json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"T1 DOS PASS: FAT16, CIUKIDOS header, {len(files)} payloads read back; SHA256 {manifest['image_sha256']}")
    return out, manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/f0/dos/DOSBOOT.IMG")
    build(parser.parse_args().out)
