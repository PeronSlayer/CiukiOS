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
               "CIUKIDOS_FIRMWARE_MOUSE": 1, "WOLF_RUNTIME_DIAG": 0}


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


# f3-04 pinned DOS/4GW client: Open Watcom 2.0 wcl386 (the 0.8 recipe of
# scripts/build_pmirqsb_dos4gw.sh) and the extender shipped with it, found
# by the stub through the CIUKIDOS PATH (C:\SYSTEM). Same output name in a
# fresh directory gives byte-identical builds (the module name is embedded).
WATCOM = Path("/opt/watcom")
DOS4GW_SHA256 = "b8265123ac8a189637448618409ef3ecd2e9f3e1a47062c685a02240f688dec1"
DOS4GW_STATUS = ("DOS/4GW 1.97 runtime from Open Watcom 2.0 binw/dos4gw.exe, distributed with "
                 "Open Watcom for applications it builds (Tenberry); shipped by the 0.8 images; "
                 "exact 1.97 redistribution text not located (open question, f3-acceptance.md f3-04)")


# f3-06: owner-local commercial DOS games (ignored by git). They reach a DOS
# VM only through the runner's separate fixture disk (scripts/test/
# dos_fixtures.py), never this payload, the canonical image or a release.
LOCAL_PAYLOAD_DIRS = ("third_party/Doom", "third_party/WOLF3D")
LOCAL_PAYLOAD_NAMES = ("DOOM.EXE", "DOOM.WAD", "DOOM1.WAD", "DOOM2.WAD", "WOLF3D.EXE",
                       "VSWAP.WL6", "VGAGRAPH.WL6", "GAMEMAPS.WL6", "AUDIOT.WL6")


def local_payload_hashes(root=ROOT):
    """SHA-256 of every file below the declared local game directories present on this host."""
    hashes = {}
    for folder in LOCAL_PAYLOAD_DIRS:
        base = Path(root) / folder
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            # Tiny files (batch stubs, empty files) could collide with
            # unrelated payloads by content; the names below still apply.
            if path.is_file() and not path.is_symlink() and path.stat().st_size >= 64:
                hashes[digest(path)] = str(path.relative_to(root))
    return hashes


def refuse_local_payloads(payloads, root=ROOT):
    """payloads: {path: sha256}. Raises when a local commercial file would be published."""
    local = local_payload_hashes(root)
    for path, sha in sorted(payloads.items()):
        name = path.replace("\\", "/").rsplit("/", 1)[-1].upper()
        if sha in local or name in LOCAL_PAYLOAD_NAMES:
            raise SystemExit(f"refusing local commercial payload {path} "
                             f"({local.get(sha, 'reserved name')}): fixtures only (f3-06)")


def watcom_build(source, name, work):
    wcl = WATCOM / "binl64/wcl386"
    extender = WATCOM / "binw/dos4gw.exe"
    if not wcl.is_file() or not extender.is_file():
        raise SystemExit(f"missing Open Watcom (wcl386, binw/dos4gw.exe) under {WATCOM}: required for {name}")
    folder = work / ("watcom-" + name.split(".")[0].lower())
    folder.mkdir()
    env = {"WATCOM": str(WATCOM), "INCLUDE": str(WATCOM / "h"),
           "PATH": f"{WATCOM / 'binl64'}:{WATCOM / 'binl'}:/usr/bin:/bin"}
    stem = name.split(".")[0]
    subprocess.run([str(wcl), "-zq", "-bt=dos", "-l=dos4g", f"-fo={stem}.o", f"-fe={name}", str(ROOT / source)],
                   cwd=folder, env=env, check=True, stdout=subprocess.DEVNULL)
    data = (folder / name).read_bytes()
    runtime = extender.read_bytes()
    if hashlib.sha256(runtime).hexdigest() != DOS4GW_SHA256:
        raise SystemExit("DOS4GW.EXE differs from the pinned Open Watcom 2.0 runtime")
    return data, runtime


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


def write_fat16(out, boot, loader, files, label="CIUKIOSFULL", directories=()):
    """Deterministic private FAT16 volume: root, /SYSTEM and (f3-06) further
    one-level directories, each one cluster (126 entries). Paths are
    "/NAME.EXT" or "/DIR/NAME.EXT" with 8.3 names."""
    if len(boot) != 512 or boot[510:] != b"\x55\xaa" or len(loader) > 4096:
        raise ValueError("DOS boot/loader bounds")
    clusters = (SECTORS - DATA_LBA) // SPC
    if not 4085 <= clusters < 65525 or clusters + 2 > FAT_SECTORS * 256:
        raise ValueError("FAT16 cluster-count/capacity mismatch")
    if len(label) > 11:
        raise ValueError("volume label exceeds 11 bytes")
    fat = bytearray(FAT_SECTORS * 512)
    struct.pack_into("<HH", fat, 0, 0xFFF8, 0xFFFF)
    root = bytearray(ROOT_SECTORS * 512)
    next_cluster = 2
    names = ["SYSTEM"] + sorted({d.strip("/").upper() for d in directories} - {"SYSTEM"})
    folders = {name: bytearray(SPC * 512) for name in names}

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
        first_cluster = {name: allocate(folders[name]) for name in names}
        root[:32] = entry(label.ljust(11), 8, 0, 0)
        root_index = 1
        index = {}
        for name in names:
            root[root_index * 32:(root_index + 1) * 32] = entry(shortname(name), 0x10, first_cluster[name], 0)
            root_index += 1
            folders[name][:32] = entry(".", 0x10, first_cluster[name], 0)
            folders[name][32:64] = entry("..", 0x10, 0, 0)
            index[name] = 2
        for path, data in sorted(files.items()):
            cluster = allocate(data)
            parent, name = path.rsplit("/", 1)
            record = entry(shortname(name), 0x20, cluster, len(data))
            folder = parent.strip("/").upper()
            if not parent:
                if root_index >= len(root) // 32:
                    raise ValueError("DOS root directory full")
                root[root_index * 32:(root_index + 1) * 32] = record
                root_index += 1
            elif folder in folders and "/" not in folder:
                if index[folder] >= SPC * 512 // 32:
                    raise ValueError("DOS payload directory full: " + folder)
                folders[folder][index[folder] * 32:(index[folder] + 1) * 32] = record
                index[folder] += 1
            else:
                raise ValueError("unapproved DOS payload directory")
        for name in names:
            stream.seek((DATA_LBA + (first_cluster[name] - 2) * SPC) * 512)
            stream.write(folders[name])
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
        files["/VMVIDEO.COM"] = assemble("src/probes/dos/vm_video.asm", work / "VMVIDEO.COM")  # f3-02 virtual VGA pattern payload
        files["/VMAUDIO.COM"] = assemble("src/probes/dos/vm_audio.asm", work / "VMAUDIO.COM")  # f3-05 SB16/OPL3/8237 state payload
        files["/VMPLAY.COM"] = assemble("src/probes/dos/vm_play.asm", work / "VMPLAY.COM")  # f4-05 SB16/OPL3 output pattern payload
        # f3-04 DPMI clients: 16/32-bit conformance, nested children, fault, DOS/4GW.
        for width in (16, 32):
            files[f"/DPMI{width}.COM"] = assemble("src/probes/dos/dpmi_client.asm", work / f"DPMI{width}.COM",
                                                  {"BITS32": int(width == 32)})
            files[f"/DPMICH{width}.COM"] = assemble("src/probes/dos/dpmi_child.asm", work / f"DPMICH{width}.COM",
                                                    {"BITS32": int(width == 32)})
        files["/DPMIFLT.COM"] = assemble("src/probes/dos/dpmi_fault.asm", work / "DPMIFLT.COM")
        # f3-06 workloads: EXEC-and-report launcher and the starvation-gate CLI loop.
        files["/RUNREC.COM"] = assemble("src/probes/dos/run_record.asm", work / "RUNREC.COM")
        files["/CLILOOP.COM"] = assemble("src/probes/dos/cli_loop.asm", work / "CLILOOP.COM")
        files["/DPMI4GW.EXE"], files["/SYSTEM/DOS4GW.EXE"] = watcom_build("src/probes/vm/dpmi_dos4gw.c", "DPMI4GW.EXE", work)
        refuse_local_payloads({name: hashlib.sha256(data).hexdigest() for name, data in files.items()})
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
                             for name, data in sorted(files.items())],
                "dos4gw": {"client": "/DPMI4GW.EXE", "extender": "/SYSTEM/DOS4GW.EXE",
                           "extender_sha256": DOS4GW_SHA256, "redistribution": DOS4GW_STATUS,
                           "recipe": "wcl386 -zq -bt=dos -l=dos4g (Open Watcom 2.0)"}}
    out.with_suffix(".json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"T1 DOS PASS: FAT16, CIUKIDOS header, {len(files)} payloads read back; SHA256 {manifest['image_sha256']}")
    return out, manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/f0/dos/DOSBOOT.IMG")
    build(parser.parse_args().out)
