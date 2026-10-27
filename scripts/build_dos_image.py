#!/usr/bin/env python3
"""Build the deterministic private FAT16 DOS VM payload (never a floppy)."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import json
from pathlib import Path
import re
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
LOCAL_PAYLOAD_DIRS = ("third_party/Doom", "third_party/WOLF3D", "third_party/Quake", "third_party/War2", "third_party/Duke3D")
LOCAL_PAYLOAD_NAMES = ("DOOM.EXE", "DOOM.WAD", "DOOM1.WAD", "DOOM2.WAD", "WOLF3D.EXE",
                       "QUAKE.EXE", "PAK0.PAK", "PAK1.PAK", "VSWAP.WL6", "VGAGRAPH.WL6", "GAMEMAPS.WL6", "AUDIOT.WL6",
                       "DUKE3D.EXE", "DUKE3D.GRP", "WAR2.EXE", "MAINDAT.WAR", "REZDAT.WAR", "SNDDAT.WAR", "STRDAT.WAR", "SFXDAT.SUD", "MUDDAT.CUD")
# Registered (commercial) game data: never published under any condition,
# not even as a pinned shareware member (owner decision 2026-10-12: only the
# shareware releases are bundled, scripts/build_games_disk.py).
REGISTERED_DATA = re.compile(r"PAK[1-9]\.PAK|(DOOM|DOOM2|DOOMU|TNT|PLUTONIA)\.WAD|[^/]*\.(WL6|SOD|SD[1-3]|N3D)")


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


def refuse_local_payloads(payloads, root=ROOT, free=frozenset()):
    """payloads: {path: sha256}. Raises when a local commercial file would be published.
    free: SHA-256 of the files pinned in the vendored shareware packages
    (scripts/build_games_disk.free_hashes); only such a file may equal a
    local file or carry a reserved name, never a registered data name."""
    local = local_payload_hashes(root)
    for path, sha in sorted(payloads.items()):
        name = path.replace("\\", "/").rsplit("/", 1)[-1].upper()
        if REGISTERED_DATA.fullmatch(name):
            raise SystemExit(f"refusing registered game data {path}: never published")
        if sha in free:
            continue
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


def write_fat16(out, boot, loader, files, label="CIUKIOSFULL", directories=(), sectors=SECTORS):
    """Deterministic FAT16 volume with explicitly approved nested 8.3 directories.
    Each subdirectory holds one cluster (126 entries, including children)."""
    if len(boot) != 512 or boot[510:] != b"\x55\xaa" or len(loader) > 4096:
        raise ValueError("DOS boot/loader bounds")
    if not DATA_LBA < sectors <= SECTORS: raise ValueError("private volume exceeds disk geometry")
    clusters = (sectors - DATA_LBA) // SPC
    if not 4085 <= clusters < 65525 or clusters + 2 > FAT_SECTORS * 256:
        raise ValueError("FAT16 cluster-count/capacity mismatch")
    if len(label) > 11:
        raise ValueError("volume label exceeds 11 bytes")
    fat = bytearray(FAT_SECTORS * 512)
    struct.pack_into("<HH", fat, 0, 0xFFF8, 0xFFFF)
    root = bytearray(ROOT_SECTORS * 512)
    next_cluster = 2
    names = ["SYSTEM"] + sorted({d.strip("/").upper() for d in directories} - {"SYSTEM"})
    for name in names:
        for part in name.split("/"):
            if not re.fullmatch(r"[A-Z0-9_\-]{1,8}(\.[A-Z0-9_\-]{1,3})?", part):
                raise ValueError("bad DOS payload directory")
        if "/" in name and name.rsplit("/", 1)[0] not in names:
            raise ValueError("unapproved DOS payload parent directory")
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
        stream.truncate(sectors * 512)
        stream.write(boot)
        stream.write(loader)
        first_cluster = {name: allocate(folders[name]) for name in names}
        root[:32] = entry(label.ljust(11), 8, 0, 0)
        root_index = 1
        index = {name: 2 for name in names}
        for name in names:
            parent, _, leaf = name.rpartition("/")
            record = entry(shortname(leaf), 0x10, first_cluster[name], 0)
            if parent:
                if index[parent] >= SPC * 512 // 32:
                    raise ValueError("DOS payload directory full: " + parent)
                at = index[parent] * 32
                folders[parent][at:at + 32] = record
                index[parent] += 1
            else:
                if root_index >= len(root) // 32:
                    raise ValueError("DOS root directory full")
                root[root_index * 32:(root_index + 1) * 32] = record
                root_index += 1
            folders[name][:32] = entry(".", 0x10, first_cluster[name], 0)
            folders[name][32:64] = entry("..", 0x10, first_cluster[parent] if parent else 0, 0)
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
            elif folder in folders:
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


def script_module(name):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, ROOT / ("scripts/" + name + ".py"))
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result)
    return result


def system_payloads(work, out):
    """Shared probes/tools only. The classic DOS kernel is retired from builds."""
    files = {}
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
    integration=script_module("dos_conformance")
    probe_files,probe_manifest=integration.probes(out.parent/"conformance")
    files.update(probe_files)
    probe_directories=("D48","D48TEST",*("D48TEST/"+r["tag"] for r in probe_manifest["cases"]))
    return files, probe_directories


def build(out=None):
    """Build the sole unified guest. Pristine FreeDOS is host-harness only."""
    out = Path(out or ROOT / "build/f0/dos/UNIFIED.IMG")
    out.parent.mkdir(parents=True, exist_ok=True)
    unified, games = script_module("unified_dos"), script_module("build_games_disk")
    with tempfile.TemporaryDirectory(prefix=".dos-", dir=out.parent) as temporary:
        files, directories = system_payloads(Path(temporary), out)
        game_files = {}
        for package in games.PACKAGES:
            _, members, _ = games.package_files(package)
            for path, data, _ in members:
                game_files[path] = data
        # All generated game presets and launchers, including Duke's CFG,
        # batch and SB16 setup helper, from the games disk's own list.
        for path, data, _ in games.generated_files():
            game_files[path] = data
        refuse_local_payloads({name: hashlib.sha256(data).hexdigest() for name, data in files.items()})
        # Every folder of the installed games (WOLF3D, DOOM, QUAKE/ID1, WAR2/DATA...),
        # parents before children, so a new bundled game needs no list edit here.
        game_dirs = {"/".join(path.strip("/").split("/")[:depth]).upper()
                     for path in game_files for depth in range(1, path.strip("/").count("/") + 1)}
        directories = (*directories, *sorted(game_dirs - set(directories), key=lambda d: (d.count("/"), d)))
        identity, licenses, metadata = unified.install(out, files, directories, game_files,
            source_output=out.parent.parent / "ciukidos-unified-source.zip")
    guest_files, _ = unified.fork_guest_files()
    files.update(guest_files)
    files["/D48/UNI.JSN"] = (json.dumps(identity,sort_keys=True,indent=2)+"\n").encode()
    manifest = {"schema_version": 1, "image_sha256": digest(out), "size": out.stat().st_size,
                "kernel": "unified", "identity": identity, "licenses": str(licenses),
                "unified_dos": metadata,
                "payloads": [{"path": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                             for name, data in sorted(files.items())]}
    out.with_suffix(".json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return out, manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path)
    build(parser.parse_args().out)
