#!/usr/bin/env python3
"""Build and statically verify the CiukiOS Setup CD (directive f9-01).

Inputs are the canonical disk image and its build-manifest.json (the same
payload set, read back from the disk image and checked against the manifest
hashes), the Setup placeholder ELF and config/cd-components.json. Outputs,
in --out-dir: ciukios-setup-boot.img (El Torito hard-disk-emulation image:
MBR, ciukldr, FAT32 with the payload, lead decision O1-A), ciukios-setup.iso
(ISO 9660 level 1, volume CIUKIOS_SETUP, \\BOOT.CAT, \\LICENSES, \\SOURCES,
\\SETUPINF.TXT, \\CDMANIFE.JSN) and cd-manifest.json.

Sources: El Torito 1.0 (2.0, 2.1, 2.2, 4.1, figures 2, 3, 7); SeaBIOS 1.16.3
src/cdrom.c (cdrom_boot: emulated geometry from partition entry 1's end CHS;
cdemu_read: image + lba/4 with a 512-byte slice; writes are write-protected);
ECMA-119 (primary volume descriptor, path tables, level 1 names); Microsoft
FAT specification 3.5 (FAT32 needs >= 65,525 clusters); GPLv2 3(a).
"""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import datetime
import errno
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
SECTOR = 512
CD_BLOCK = 2048
PART_LBA = 2048
FAT_RESERVED = 160
CLUSTER_SECTORS = 8
MIN_CLUSTERS = 65525                 # fatgen103 3.5; ciukldr/disk.inc requires FAT32
HEADS, SPT = 255, 63
CYLINDER = HEADS * SPT               # 16,065 sectors
IMAGE_GRANULE = 4 * CYLINDER         # whole 255x63 cylinders and whole CD blocks
HEADROOM_PERCENT = 5                 # directive f9-01 item 1 [O1-A]
CD_LIMIT_SECTORS = 333000            # 74-minute CD-R: 681,984,000 bytes (plan 1)
SETUP_SIGNATURE = 0x4B554943         # reserved Setup CD disk signature (lead decision O4)
SETUP_TOKEN = b"setup=1"
# Programs of the Setup runtime. /Programs/hwreport is the rescue page's
# Hardware Report (f9-07 amendment 1); it comes from the disk image's payload
# list, next to /Programs/setup. Without it the rescue page can only fall back
# to Setup's own report, so the build refuses a CD that lacks it.
SETUP_PROGRAMS = ("/Programs/setup", "/Programs/hwreport")
VOLUME_ID = "CIUKIOS_SETUP"
VOLUME_LABEL = "CIUKISETUP"
BOOT_IMAGE = "CIUKBOOT.IMG"
CATALOG = "BOOT.CAT"
MANIFEST_COPY = "CDMANIFE.JSN"
SAMPLE = "SETUPINF.TXT"
COMPONENTS = ROOT / "config/cd-components.json"
LOADER_HEADER = struct.Struct("<4sHHIHH")
TOOLS = ("mkfs.fat", "fsck.fat", "mmd", "mcopy", "mdir", "mattrib", "xorriso", "git")

# cd-setup-plan.md section 8, schema version 1 (the sample; \SETUP.INF itself is absent).
ANSWER_SAMPLE = """; CiukiOS Setup answer file sample (cd-setup-plan.md section 8, schema 2).
; Copy it as SETUP.INF to the root of a FAT USB stick for an unattended installation.
; Passwords are never stored in an answer file: Setup stops before writing to the disk
; (status missing_answer, key UserPassword) until a secure way to supply them exists.
; Without SETUP.INF, Setup asks for your password and the CiukiAdministrator recovery password.
[Setup]
Version=2
AcceptLicences=yes            ; required: yes
EraseDisk=yes                 ; required whenever the destination holds any partition or filesystem; explicit consent to erase
Disk=ata0                     ; ata0 (the only installable disk in the Beta) | model="<IDENTIFY model>" serial="<serial>" | auto
Reinstall=no                  ; yes allows erasing a disk whose SETUP.STATE says installed with the same release
Components=dos,windows,netsurf,games,devtools,soundfont,sources   ; system is implicit; names from the catalogue
UserName=<1..31 letters, digits, - or _; the first administrator account; not CiukiUser>
ComputerName=<1..15 chars, LAN rules>
Keyboard=us                   ; a layout id of the keyboard page
TimeZone=+01:00               ; offset; Clock=local|utc (RTC convention; default local)
Network=dhcp                  ; dhcp | static:<ip>/<prefix>,<gateway>,<dns> | none
IPX=yes
Restart=eject                 ; eject (default) | wait (wait for removal, no timeout) | force (restart after 60 s even with the CD inserted)
"""


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def environment(epoch):
    return dict(os.environ, TZ="UTC", LC_ALL="C", SOURCE_DATE_EPOCH=str(epoch))


def run(args, epoch, **kwargs):
    return subprocess.run([str(a) for a in args], cwd=ROOT, check=True, env=environment(epoch), **kwargs)


def chs(lba):
    """Packed CHS bytes (head, sector | cylinder high bits, cylinder low) for 255x63."""
    cylinder, rest = divmod(lba, CYLINDER)
    head, sector = divmod(rest, SPT)
    require(cylinder <= 1023, "CHS cylinder overflow")
    return bytes((head, (sector + 1) | ((cylinder >> 8) & 3) << 6, cylinder & 0xFF))


def emulated_geometry(entry):
    """SeaBIOS 1.16.3 disk.c getLCHS for the emulated drive (cdrom.c cdrom_boot:
    CDEmu.chs = partition entry 1 end CHS); AH=08h reports this geometry."""
    head, sptcyl, cyllow = entry[5], entry[6], entry[7]
    return {"cylinders": ((sptcyl << 2) & 0x300) + cyllow + 1, "heads": head + 1, "sectors": sptcyl & 0x3F}


def setup_mbr(code, part_count):
    require(len(code) == 440, "MBR code size")
    mbr = bytearray(SECTOR)
    mbr[:440] = code
    struct.pack_into("<I", mbr, 440, SETUP_SIGNATURE)
    last = PART_LBA + part_count - 1
    struct.pack_into("<B3sB3sII", mbr, 446, 0x80, chs(PART_LBA), 0x0C, chs(last), PART_LBA, part_count)
    mbr[510:] = b"\x55\xaa"
    return bytes(mbr)


def loader_extent(raw):
    magic, version, sectors, crc, entry, length = LOADER_HEADER.unpack_from(raw)
    require(magic == b"CLDR" and version == 1 and length == 16, "loader header")
    require(1 <= sectors <= 1023 and 16 <= entry < sectors * SECTOR, "loader extent")
    loader = raw[:sectors * SECTOR]
    require(len(loader) == sectors * SECTOR and zlib.crc32(loader[16:]) == crc, "loader CRC32")
    return loader


# ---------------------------------------------------------------- eligibility

def assign(path, components):
    """The longest matching entry wins: '/dir/' and 'name*' are prefixes,
    any other entry is one exact path."""
    best, length = None, -1
    for component in components:
        for entry in component["paths"]:
            prefix = entry[:-1] if entry.endswith("*") else entry
            match = path.startswith(prefix) if entry.endswith(("/", "*")) else path == entry
            if match and len(prefix) > length:
                best, length = component, len(prefix)
    return best


def eligibility(config, paths, games_packages):
    """Component rows for every CD path; fails closed (directive f9-01 item 3)."""
    components = config["components"]
    ids = [c["id"] for c in components]
    require(len(ids) == len(set(ids)), "duplicate component id")
    for c in components:
        require(c.get("class") in ("free", "copyleft", "bundled-shareware"), f"component {c['id']}: licence class")
        require(isinstance(c.get("paths"), list) and isinstance(c.get("archives"), list), f"component {c['id']}: fields")
        for archive in c["archives"]:
            require(archive in config["archives"], f"component {c['id']}: unknown archive {archive}")
        if c["class"] == "copyleft":
            require(c["archives"], f"copyleft component {c['id']} lists no source archive")
        if c["class"] == "bundled-shareware":
            require(str(c.get("conditions", "")).strip(), f"bundled-shareware component {c['id']} lacks its conditions text")
    rows = {c.get("package"): c for c in components if c.get("package")}
    for package in games_packages:
        require(package in rows, f"games package {package} has no component row")
        require(rows[package]["class"] == "bundled-shareware", f"games package {package}: class")
    assignment = {}
    for path in sorted(paths):
        component = assign(path, components)
        require(component is not None, f"payload path without a component: {path}")
        assignment[path] = component["id"]
    present = sorted(set(assignment.values()))
    required = sorted({a for c in components if c["id"] in present for a in c["archives"]})
    return assignment, present, required


# ---------------------------------------------------------------- archives

def pinned_hash(record):
    value = json.loads((ROOT / record["pin"]).read_text())
    for key in record["key"].split("."):
        value = value[key]
    require(isinstance(value, str) and len(value) == 64, f"pin {record['pin']}:{record['key']}")
    return value


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True).stdout


def source_tree_zip(config, output, epoch):
    """Deterministic zip of the tracked files of the listed paths, as they are
    in the working tree that built the binaries (sorted, fixed dates/modes)."""
    include = config["source_tree"]["include"]
    tracked = git("ls-files", "-z", "-s", "--", *include).split(b"\0")
    date = datetime.datetime.fromtimestamp(max(epoch, 315532800), datetime.timezone.utc).timetuple()[:6]
    entries = []
    for line in tracked:
        if not line:
            continue
        meta, name = line.split(b"\t", 1)
        mode = meta.split()[0]
        path = name.decode()
        require(mode in (b"100644", b"100755"), f"unsupported tracked mode {mode.decode()} for {path}")
        entries.append((path, mode == b"100755"))
    require(entries, "empty source tree")
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path, executable in sorted(entries):
            info = zipfile.ZipInfo("ciukios/" + path, date_time=date)
            info.create_system = 3
            info.external_attr = (0o100755 if executable else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, (ROOT / path).read_bytes(), compresslevel=9)
    return len(entries)


def manifest_value(manifest, key):
    value = manifest or {}
    for part in key.split("."):
        value = value.get(part) if isinstance(value, dict) else None
    return value


def build_archives(config, required, directory, epoch, image_dir=None, manifest=None):
    """Produce every required archive in directory (the ISO's \\SOURCES)."""
    directory.mkdir(parents=True, exist_ok=True)
    records = {}
    for name in sorted(required):
        spec = config["archives"][name]
        target = directory / name
        if spec["kind"] == "source-tree":
            files = source_tree_zip(config, target, epoch)
            records[name] = {"kind": "source-tree", "files": files}
        elif spec["kind"] == "pinned":
            source = ROOT / spec["file"]
            require(source.is_file(), f"source archive {name} missing: {spec['file']}")
            expected = pinned_hash(spec)
            require(sha256_file(source) == expected, f"source archive {name}: SHA-256 differs from {spec['pin']}")
            shutil.copyfile(source, target)
            records[name] = {"kind": "pinned", "source": spec["file"], "pin": f"{spec['pin']}:{spec['key']}"}
        else:   # produced by the image build beside the image, hash in its manifest
            source = Path(image_dir) / spec["beside_image"] if image_dir else None
            require(source is not None and source.is_file(),
                    f"source archive {name} is required but not produced ({spec['producer']}: {spec['beside_image']} absent)")
            expected = manifest_value(manifest, spec["manifest_key"])
            require(expected == sha256_file(source), f"source archive {name}: SHA-256 differs from build-manifest.json {spec['manifest_key']}")
            shutil.copyfile(source, target)
            records[name] = {"kind": "produced", "source": spec["beside_image"], "pin": f"build-manifest.json:{spec['manifest_key']}"}
        os.utime(target, (epoch, epoch))
        records[name].update({"sha256": sha256_file(target), "size": target.stat().st_size, "describes": spec["describes"]})
    return records


def licence_tree(config, present, payload_files, directory, archives, assignment, epoch, identity):
    """\\LICENSES: every payload file under /system/licenses/ and each present
    component's licence files, as L0001.TXT... with INDEX.TXT and SOURCES.TXT."""
    directory.mkdir(parents=True, exist_ok=True)
    sources = {f"payload:{p}": payload_files[p] for p in payload_files if p.startswith("/system/licenses/")}
    for component in config["components"]:
        if component["id"] in present:
            for name in component.get("licence_files", []):
                require((ROOT / name).is_file(), f"licence file missing: {name}")
                sources[f"repo:{name}"] = ROOT / name
    index = ["CiukiOS Setup CD licence texts. Short names map to their origin on the",
             "installed system (payload:) or in the CiukiOS source tree (repo:).", ""]
    for number, label in enumerate(sorted(sources), 1):
        short = f"L{number:04d}.TXT"
        shutil.copyfile(sources[label], directory / short)
        os.utime(directory / short, (epoch, epoch))
        index.append(f"{short}  {sha256_file(directory / short)}  {(directory / short).stat().st_size:>9}  {label}")
    (directory / "INDEX.TXT").write_text("\n".join(index) + "\n")
    lines = ["CiukiOS Setup CD: corresponding source (GNU GPL version 2, section 3(a); GNU LGPL",
             "version 2.1, sections 4 and 6). Every copyleft binary on this CD is accompanied by",
             "its complete corresponding source in \\SOURCES on the same medium. Copyright,",
             "warranty and licence notices are preserved in the archives and in \\LICENSES.",
             f"Release {identity['version']}, revision {identity['git_revision']}"
             f"{' (uncommitted changes included)' if identity['git_dirty'] else ''}.", ""]
    for name, record in sorted(archives.items()):
        owners = sorted(c["title"] for c in config["components"] if c["id"] in present and name in c["archives"])
        lines += [f"\\SOURCES\\{name}  SHA-256 {record['sha256']}  {record['size']} bytes",
                  f"  {record['describes']}", "  Corresponds to:"] + [f"    - {o}" for o in owners] + [""]
    for component in config["components"]:
        if component["id"] in present and component.get("conditions"):
            lines += [f"{component['title']}: {component['conditions']}"]
    (directory / "SOURCES.TXT").write_text("\n".join(lines) + "\n")
    for name in ("INDEX.TXT", "SOURCES.TXT"):
        os.utime(directory / name, (epoch, epoch))
    return len(sources)


# ---------------------------------------------------------------- disk image input

def read_disk_image(image, manifest):
    require(sha256_file(image) == manifest["image_sha256"], "disk image SHA-256 differs from build-manifest.json")
    with image.open("rb") as stream:
        mbr = stream.read(SECTOR)
        head = stream.read(1023 * SECTOR)
    require(mbr[510:] == b"\x55\xaa", "disk image MBR signature")
    start = struct.unpack_from("<I", mbr, 454)[0]
    require(mbr[446 + 4] == 0x0C and start == PART_LBA, "disk image partition 1")
    return mbr[:440], loader_extent(head), start


def mattrib_tree(volume, epoch):
    """Directories and read-only files of a FAT volume (mattrib -/)."""
    listing = run(["mattrib", "-/", "-i", volume, "::/"], epoch, capture_output=True, text=True).stdout
    entries = {}
    for line in listing.splitlines():
        if "::" not in line:
            continue
        flags, name = line.split("::", 1)
        entries[name] = set(flags.split())
    return entries


def stage_payloads(image, start, payloads, stage, epoch):
    volume = f"{image}@@{start * SECTOR}"
    files = {}
    for record in payloads:
        path = record["path"]
        target = stage / path.lstrip("/")
        target.parent.mkdir(parents=True, exist_ok=True)
        run(["mcopy", "-n", "-m", "-i", volume, "::" + path, target], epoch, stdout=subprocess.DEVNULL)
        require(target.stat().st_size == record["size"] and sha256_file(target) == record["sha256"],
                f"disk image payload differs from build-manifest.json: {path}")
        files[path] = target
    return files


# ---------------------------------------------------------------- boot image

def copy_extents(source, offset, dest, target, size):
    """Copy the allocated extents of source[offset:offset+size] to dest at
    target, retaining sparsity (as build_image.py copies its volume)."""
    cursor = 0
    while cursor < size:
        try:
            data = os.lseek(source.fileno(), offset + cursor, os.SEEK_DATA) - offset
        except OSError as exc:
            if exc.errno == errno.ENXIO:
                break
            raise
        if data >= size:
            break
        hole = min(os.lseek(source.fileno(), offset + data, os.SEEK_HOLE) - offset, size)
        source.seek(offset + data)
        dest.seek(target + data)
        while data < hole:
            block = source.read(min(1024 * 1024, hole - data))
            require(block, "short extent read")
            dest.write(block)
            data += len(block)
        cursor = hole


def plan_geometry(files, directories):
    """Partition sectors: payload clusters plus directory clusters, 5 % headroom,
    at least 65,525 clusters, then whole 255x63 cylinders and CD blocks."""
    data = sum(-(-Path(source).stat().st_size // (CLUSTER_SECTORS * SECTOR)) for source in files.values())
    children = {d: 0 for d in ("/", *directories)}
    for name in (*files, *directories):
        parent = name.rsplit("/", 1)[0] or "/"
        children[parent] = children.get(parent, 0) + 2 + -(-len(name.rsplit("/", 1)[1]) // 13)
    folders = sum(max(1, -(-(count + 2) * 32 // (CLUSTER_SECTORS * SECTOR))) for count in children.values())
    clusters = max(MIN_CLUSTERS, -(-(data + folders) * (100 + HEADROOM_PERCENT) // 100))
    fat = -(-(clusters + 2) * 4 // SECTOR)
    image = PART_LBA + FAT_RESERVED + 2 * fat + clusters * CLUSTER_SECTORS
    image = -(-image // IMAGE_GRANULE) * IMAGE_GRANULE
    return image - PART_LBA, {"payload_clusters": data, "directory_clusters": folders, "planned_clusters": clusters}


def fat_attributes(flags, directory=False):
    return (16 if directory else 0) | sum(bit for flag, bit in (("R", 1), ("H", 2), ("S", 4), ("A", 32)) if flag in flags)


def boot_manifest_records(files, directories, tree, assignment):
    folded = {p.lower(): flags for p, flags in tree.items()}
    records = [{"path": p, "sha256": sha256_file(source), "size": Path(source).stat().st_size,
                "component": assignment[p], "attributes": fat_attributes(folded.get(p.lower(), {"A"}))}
               for p, source in sorted(files.items())]
    folders = []
    for path in directories:
        components = {assignment[p] for p in files if p.lower().startswith(path.lower() + "/")}
        folders.append({"path": path, "attributes": fat_attributes(folded.get(path.lower(), set()), True),
                        "component": next(iter(components)) if len(components) == 1 else "system"})
    return records, folders


def populate(volume, files, directories, readonly, epoch, attributes=None):
    for directory in sorted(directories, key=lambda p: (p.count("/"), p)):
        run(["mmd", "-i", volume, "::" + directory], epoch)
    groups = {}
    for path in files:
        groups.setdefault(path.rsplit("/", 1)[0] or "/", []).append(path)
    for parent in sorted(groups, key=lambda p: (p.count("/"), p)):
        names = sorted(groups[parent])
        for path in names:
            require(Path(files[path]).name == path.rsplit("/", 1)[1], f"staged name differs: {path}")
        run(["mcopy", "-m", "-i", volume, *(files[p] for p in names), "::" + parent + ("" if parent.endswith("/") else "/")], epoch)
    if attributes is None:
        attributes = {p: 33 for p in readonly}
    for path, attr in sorted(attributes.items(), key=lambda row: (-row[0].count("/"), row[0])):
        options = [("+" if attr & bit else "-") + flag for flag, bit in (("r",1),("h",2),("s",4),("a",32))]
        run(["mattrib", "-i", volume, *options, "::" + path], epoch)


def build_boot_image(work, files, directories, readonly, mbr_code, loader, epoch, initialize_blackbox, attributes=None):
    part_count, plan = plan_geometry(files, directories)
    volume = work / "setup.fat"
    with volume.open("wb") as stream:
        stream.truncate(part_count * SECTOR)
    run(["mkfs.fat", "--invariant", "-a", "-F", "32", "-S", "512", "-s", str(CLUSTER_SECTORS), "-f", "2",
         "-R", str(FAT_RESERVED), "-b", "6", "-h", str(PART_LBA), "-n", VOLUME_LABEL, volume], epoch,
        stdout=subprocess.DEVNULL)
    initialize_blackbox(volume)
    populate(volume, files, directories, readonly, epoch, attributes)
    with volume.open("r+b") as stream:   # mtools updates the primary FSInfo only
        stream.seek(SECTOR)
        fsinfo = stream.read(SECTOR)
        stream.seek(7 * SECTOR)
        stream.write(fsinfo)
    image = work / "boot.img"
    mbr = setup_mbr(mbr_code, part_count)
    with image.open("wb") as dest, volume.open("rb") as source:
        dest.truncate((PART_LBA + part_count) * SECTOR)
        dest.write(mbr)
        dest.write(loader)
        copy_extents(source, 0, dest, PART_LBA * SECTOR, part_count * SECTOR)
    volume.unlink()
    return image, part_count, plan


def boot_image_facts(image):
    """Every field the loader pins (ciukldr/disk.inc) and the MBR, read back."""
    with image.open("rb") as stream:
        mbr = stream.read(SECTOR)
        stream.seek(PART_LBA * SECTOR)
        bpb = stream.read(SECTOR)
        fsinfo = stream.read(SECTOR)
        stream.seek((PART_LBA + 6) * SECTOR)
        backup = stream.read(2 * SECTOR)
        stream.seek((PART_LBA + 8) * SECTOR)
        descriptor = stream.read(SECTOR)
        stream.seek((PART_LBA + 32) * SECTOR)
        ring = stream.read(128 * SECTOR)
    entry = mbr[446:462]
    bps, spc, reserved, fats = struct.unpack_from("<HBHB", bpb, 11)
    total, fat_size = struct.unpack_from("<II", bpb, 32)
    clusters = (total - reserved - fats * fat_size) // spc
    return {
        "mbr": mbr, "signature": struct.unpack_from("<I", mbr, 440)[0], "status": entry[0], "type": entry[4],
        "start_chs": entry[1:4].hex(), "end_chs": entry[5:8].hex(), "start": struct.unpack_from("<I", entry, 8)[0],
        "count": struct.unpack_from("<I", entry, 12)[0], "other_entries_zero": mbr[462:510] == bytes(48),
        "bytes_per_sector": bps, "sectors_per_cluster": spc, "reserved": reserved, "fats": fats,
        "root_entries": struct.unpack_from("<H", bpb, 17)[0], "total16": struct.unpack_from("<H", bpb, 19)[0],
        "fat16_size": struct.unpack_from("<H", bpb, 22)[0], "total_sectors": total, "fat_sectors": fat_size,
        "ext_flags": struct.unpack_from("<H", bpb, 40)[0], "root_cluster": struct.unpack_from("<I", bpb, 44)[0],
        "fsinfo_sector": struct.unpack_from("<H", bpb, 48)[0], "backup_sector": struct.unpack_from("<H", bpb, 50)[0],
        "clusters": clusters, "bpb_signature": bpb[510:] == b"\x55\xaa",
        "fsinfo_ok": struct.unpack_from("<I", fsinfo)[0] == 0x41615252 and struct.unpack_from("<I", fsinfo, 484)[0] == 0x61417272,
        "backup_equal": backup == bpb + fsinfo, "data_start": reserved + fats * fat_size,
        "blackbox_descriptor": struct.unpack_from("<4sIII", descriptor), "blackbox_crc_ok":
            struct.unpack_from("<I", descriptor, 508)[0] == zlib.crc32(descriptor[:508]),
        "blackbox_ring_empty": ring == bytes(len(ring)), "geometry": emulated_geometry(entry),
    }


def check_boot_image(image, part_count, mbr_code, loader, files, epoch):
    facts = boot_image_facts(image)
    image_sectors = PART_LBA + part_count
    require(image.stat().st_size == image_sectors * SECTOR and image_sectors % IMAGE_GRANULE == 0, "boot image size")
    require(facts["mbr"][:440] == mbr_code and facts["signature"] == SETUP_SIGNATURE, "boot image MBR code/signature")
    require(facts["status"] == 0x80 and facts["type"] == 0x0C and facts["other_entries_zero"], "boot image partition table")
    require(facts["start"] == PART_LBA and facts["count"] == part_count, "boot image partition extent")
    require(facts["start_chs"] == chs(PART_LBA).hex() and facts["end_chs"] == chs(image_sectors - 1).hex(), "boot image CHS")
    geometry = facts["geometry"]
    require(geometry["heads"] == HEADS and geometry["sectors"] == SPT and
            geometry["cylinders"] * CYLINDER == image_sectors, "emulated geometry covers exactly the image")
    with image.open("rb") as stream:
        stream.seek(SECTOR)
        require(stream.read(len(loader)) == loader, "boot image loader bytes")
        gap = stream.read((PART_LBA - 1) * SECTOR - len(loader))
        require(gap == bytes(len(gap)), "boot image loader gap")
    require((facts["bytes_per_sector"], facts["sectors_per_cluster"], facts["reserved"], facts["fats"]) ==
            (SECTOR, CLUSTER_SECTORS, FAT_RESERVED, 2), "FAT32 geometry (disk.inc)")
    require(facts["root_entries"] == 0 and facts["total16"] == 0 and facts["fat16_size"] == 0, "FAT32 BPB")
    require(facts["total_sectors"] == part_count and facts["ext_flags"] == 0, "BPB total/mirroring")
    require(MIN_CLUSTERS <= facts["clusters"] <= 0x0FFFFFF5 and facts["fat_sectors"] * 128 >= facts["clusters"] + 2,
            "FAT32 cluster count (disk.inc)")
    require(facts["root_cluster"] == 2 and (facts["fsinfo_sector"], facts["backup_sector"]) == (1, 6), "FAT32 BPB fields")
    require(facts["bpb_signature"] and facts["fsinfo_ok"] and facts["backup_equal"], "BPB/FSInfo/backup")
    require(facts["blackbox_descriptor"] == (b"CBB1", 1, 32, 128) and facts["blackbox_crc_ok"] and facts["blackbox_ring_empty"],
            "blackbox descriptor and empty ring")
    volume = f"{image}@@{PART_LBA * SECTOR}"
    partition = image.parent / "partition.check"
    with image.open("rb") as source, partition.open("wb") as dest:   # fsck.fat needs the bare volume
        dest.truncate(part_count * SECTOR)
        copy_extents(source, PART_LBA * SECTOR, dest, 0, part_count * SECTOR)
    try:
        fsck = subprocess.run(["fsck.fat", "-n", partition], capture_output=True, text=True)
    finally:
        partition.unlink()
    require(fsck.returncode == 0, "fsck.fat -n: " + (fsck.stdout + fsck.stderr)[-2000:])
    for path, source in sorted(files.items()):
        data = run(["mcopy", "-i", volume, "::" + path, "-"], epoch, capture_output=True).stdout
        require(len(data) == Path(source).stat().st_size and sha256_bytes(data) == sha256_file(source),
                f"boot image read-back: {path}")
    # Record the checker's verdict without the scratch path (determinism).
    return facts, [line.replace(str(partition), "partition") for line in fsck.stdout.strip().splitlines()[:2]]


# ---------------------------------------------------------------- ISO

def iso_names_ok(name, directory):
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")
    if directory:
        return 1 <= len(name) <= 8 and set(name) <= allowed
    base, _, ext = name.partition(".")
    return 1 <= len(base) <= 8 and len(ext) <= 3 and set(base) <= allowed and set(ext) <= allowed


def build_iso(tree, iso, epoch):
    date = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc).strftime("%Y%m%d%H%M%S00")
    args = ["xorriso", "-as", "mkisofs", "-iso-level", "1", "--norock", "-V", VOLUME_ID, "-A", "CIUKIOS_SETUP",
            "-publisher", "CIUKIOS", "-sysid", "", "-p", "", "--modification-date=" + date,
            "--set_all_file_dates", date, "-uid", "0", "-gid", "0", "-b", BOOT_IMAGE, "-c", CATALOG,
            "-hard-disk-boot", "-o", iso, tree]
    run(args, epoch, capture_output=True)
    return [str(a) if not isinstance(a, Path) else a.name for a in args]


def read_block(stream, block, count=1):
    stream.seek(block * CD_BLOCK)
    data = stream.read(count * CD_BLOCK)
    require(len(data) == count * CD_BLOCK, f"ISO short read at block {block}")
    return data


def both32(record, offset, what):
    little, big = struct.unpack_from("<I", record, offset)[0], struct.unpack_from(">I", record, offset + 4)[0]
    require(little == big, f"ISO both-endian field: {what}")
    return little


def iso_tree(stream, extent, size, prefix, files, directories):
    data = read_block(stream, extent, -(-size // CD_BLOCK))[:size]
    at = 0
    while at < size:
        length = data[at]
        if not length:
            at = (at // CD_BLOCK + 1) * CD_BLOCK
            continue
        record = data[at:at + length]
        name_length = record[32]
        name = record[33:33 + name_length]
        child_extent, child_size = both32(record, 2, "extent"), both32(record, 10, "size")
        flags = record[25]
        at += length
        if name in (b"\0", b"\1"):
            continue
        text = name.decode("ascii")
        if flags & 2:
            require(iso_names_ok(text, True), f"ISO level 1 directory name: {text}")
            directories.append(prefix + text)
            iso_tree(stream, child_extent, child_size, prefix + text + "/", files, directories)
        else:
            require(text.endswith(";1") and iso_names_ok(text[:-2], False), f"ISO level 1 file name: {text}")
            files[prefix + text[:-2]] = (child_extent, child_size)


def check_iso(iso, boot_image, part_count, expected_files, epoch, work):
    """Independent parse (ECMA-119 and El Torito 1.0) and xorriso extraction."""
    facts = {}
    with iso.open("rb") as stream:
        pvd = read_block(stream, 16)
        require(pvd[0] == 1 and pvd[1:6] == b"CD001" and pvd[6] == 1, "primary volume descriptor")
        require(pvd[40:72].decode().rstrip() == VOLUME_ID, "volume identifier")
        space = both32(pvd, 80, "volume space size")
        require(struct.unpack_from("<H", pvd, 128)[0] == CD_BLOCK, "logical block size")
        path_size = both32(pvd, 132, "path table size")
        l_table = struct.unpack_from("<I", pvd, 140)[0]
        root = pvd[156:190]
        boot = read_block(stream, 17)
        require(boot[0] == 0 and boot[1:6] == b"CD001" and boot[6] == 1, "boot record volume descriptor")
        require(boot[7:39] == b"EL TORITO SPECIFICATION".ljust(32, b"\0") and boot[39:71] == bytes(32), "boot system identifier")
        catalog = struct.unpack_from("<I", boot, 0x47)[0]
        terminator = 18
        while read_block(stream, terminator)[0] != 255:
            terminator += 1
            require(terminator < 64, "volume descriptor set terminator")
        table = read_block(stream, l_table, -(-path_size // CD_BLOCK))[:path_size]
        at, names = 0, []
        while at < path_size:
            length = table[at]
            name = table[at + 8:at + 8 + length]
            names.append(name)
            at += 8 + length + (length & 1)
        require(names[0] == b"\0" and all(iso_names_ok(n.decode(), True) for n in names[1:]), "path table level 1 names")
        files, directories = {}, []
        iso_tree(stream, both32(root, 2, "root extent"), both32(root, 10, "root size"), "", files, directories)
        cat = read_block(stream, catalog)
        validation, initial = cat[:32], cat[32:64]
        checksum = sum(struct.unpack("<16H", validation)) & 0xFFFF
        facts["catalog"] = {
            "block": catalog, "validation_entry_hex": validation.hex(), "initial_entry_hex": initial.hex(),
            "header": validation[0], "platform": validation[1], "id_string": validation[4:28].rstrip(b"\0 ").decode("latin-1"),
            "checksum": struct.unpack_from("<H", validation, 0x1C)[0], "word_sum": checksum,
            "key": validation[0x1E:0x20].hex(), "boot_indicator": initial[0], "media_type": initial[1],
            "load_segment": struct.unpack_from("<H", initial, 2)[0], "system_type": initial[4], "unused5": initial[5],
            "sector_count": struct.unpack_from("<H", initial, 6)[0], "load_rba": struct.unpack_from("<I", initial, 8)[0],
            "unused_tail_zero": initial[12:32] == bytes(20)}
        c = facts["catalog"]
        require(c["header"] == 1 and c["platform"] == 0 and validation[2:4] == b"\0\0" and c["word_sum"] == 0 and c["key"] == "55aa",
                "validation entry (El Torito 2.1)")
        require(c["boot_indicator"] == 0x88 and c["media_type"] == 4 and c["load_segment"] == 0 and c["system_type"] == 0x0C
                and c["unused5"] == 0 and c["sector_count"] == 1 and c["unused_tail_zero"], "initial entry (El Torito 2.2)")
        require(CATALOG in files and files[CATALOG][0] == catalog, "BOOT.CAT extent equals the catalogue pointer")
        require(BOOT_IMAGE in files, "boot image in the ISO tree")
        extent, size = files[BOOT_IMAGE]
        require(extent == c["load_rba"] and size == (PART_LBA + part_count) * SECTOR, "boot image extent and data length")
        digest = hashlib.sha256()
        stream.seek(extent * CD_BLOCK)
        remaining = size
        while remaining:
            block = stream.read(min(remaining, 1024 * 1024))
            require(block, "short boot image extent")
            digest.update(block)
            remaining -= len(block)
        require(digest.hexdigest() == sha256_file(boot_image), "boot image bytes in the ISO")
    facts.update({"volume_space_blocks": space, "path_table_bytes": path_size, "terminator_block": terminator,
                  "boot_record_block": 17, "image_extent_block": extent, "image_bytes": size,
                  "directories": sorted(directories)})
    require(set(files) == set(expected_files) | {CATALOG}, "ISO file set: " + ", ".join(sorted(set(files) ^ (set(expected_files) | {CATALOG}))))
    extracted = work / "extract"
    extracted.mkdir()
    try:
        run(["xorriso", "-osirrox", "on", "-indev", iso, "-extract", "/", extracted], epoch, capture_output=True)
        found = {p.relative_to(extracted).as_posix(): p for p in extracted.rglob("*") if p.is_file()}
        require(set(found) == set(expected_files) | {CATALOG}, "xorriso extraction file set")
        for name, source in expected_files.items():
            require(sha256_file(found[name]) == sha256_file(source), f"xorriso extraction differs: {name}")
        facts["catalog_file_sha256"] = sha256_file(found[CATALOG])
    finally:
        for p in [extracted, *extracted.rglob("*")]:
            if p.is_dir():
                p.chmod(0o700)
        shutil.rmtree(extracted)
    return facts


# ---------------------------------------------------------------- build

def git_head():
    try:
        return git("rev-parse", "HEAD").decode().strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"


def content_digest(boot_files, iso_files):
    lines = []
    for scope, files in (("boot", boot_files), ("iso", iso_files)):
        for path in sorted(files):
            lines.append(f"{scope}:{path} {Path(files[path]).stat().st_size} {sha256_file(files[path])}\n")
    return sha256_bytes("".join(lines).encode()), len(lines)


def check_setup_programs(files):
    """files: staged CD boot-image path -> source file. Every Setup runtime
    program must be present (FAT names are case-insensitive) and an ELF."""
    by_lower = {path.lower(): source for path, source in files.items()}
    for program in SETUP_PROGRAMS:
        source = by_lower.get(program.lower())
        require(source is not None, f"Setup CD payload lacks {program}")
        with Path(source).open("rb") as stream:
            require(stream.read(4) == b"\x7fELF", f"Setup CD payload {program} is not an ELF program")


def build(image, out_dir, setup_elf, config_path=COMPONENTS, limit_sectors=CD_LIMIT_SECTORS,
          allow_source_mismatch=False, keep_work=False):
    for tool in TOOLS:
        require(shutil.which(tool), f"missing host tool: {tool}")
    image, out_dir = Path(image).resolve(), Path(out_dir).resolve()
    manifest_path = image.parent / "build-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    clock = json.loads((image.parent / "build-clock.json").read_text())
    epoch = int(manifest["build_epoch"])
    clock_state = "consistent" if clock["utc_epoch"] == epoch and clock["kernel_sha256"] == manifest.get("kernel_full_sha256", manifest["kernel_sha256"]) \
        else "kernel-rebuilt-after-image"
    require(clock_state == "consistent" or (allow_source_mismatch and out_dir != image.parent),
            "build-clock.json describes another kernel than build-manifest.json; run the image build first")
    config = json.loads(Path(config_path).read_text())
    bi = module("cd_build_image", ROOT / "scripts/build_image.py")
    head = git_head()
    source_matches = head == manifest["git_revision"]
    require(source_matches or allow_source_mismatch,
            f"CIUKIOS.ZIP must hold the built revision {manifest['git_revision']}; the tree is at {head}")
    payloads = manifest["payloads"]
    by_path = {p["path"]: p for p in payloads}
    require("/system/BOOT.CFG" in by_path and "/system/VMM.ELF" in by_path, "disk payload lacks BOOT.CFG/VMM.ELF")
    out_dir.mkdir(parents=True, exist_ok=True)
    work = out_dir / ".cd-work"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir()
    try:
        mbr_code, loader, start = read_disk_image(image, manifest)
        sibling = image.parent / "ciukldr.bin"
        sibling_state = "equal" if sibling.is_file() and sibling.read_bytes() == loader else \
            "absent" if not sibling.is_file() else "different"
        require(sibling_state == "equal" or out_dir != image.parent,
                f"{sibling} is {sibling_state}; build_image.py writes the loader of this build there")
        tree = mattrib_tree(f"{image}@@{start * SECTOR}", epoch)
        stage = work / "boot"
        # sec-user-02 (user-security-contract.md 5.2): the ready image's account
        # seed and bootstrap.state never reach the Setup medium; Setup stages
        # its own origin-2 seed on the destination. No other account state may
        # be on the CD either.
        provisioning = manifest.get("provisioning") or {}
        require(provisioning.get("kind") == "ready", "build-manifest.json lacks the ready-image provisioning record")
        ready_only = {provisioning["seed"].casefold(), provisioning["state"].casefold()}
        files = stage_payloads(image, start, [p for p in payloads if p["path"] != "/system/BOOT.CFG"
                                              and p["path"].casefold() not in ready_only], stage, epoch)
        # sec-k06 keeps the machine registry hives in the SYSTEM broker's tree
        # /System/Security/Registry/Machine: machine configuration that Setup
        # installs, like the default hives of a Windows Setup medium, not
        # account state.
        machine_registry = "/system/security/registry/machine/"
        require(not [p for p in files if p.casefold().startswith("/system/security/")
                     and not p.casefold().startswith(machine_registry)],
                "the Setup CD must not carry account state below /System/Security")
        # CD-only files: the placeholder Setup program and the Setup boot options.
        disk_cfg = (stage / "system/BOOT.CFG.disk")
        run(["mcopy", "-n", "-i", f"{image}@@{start * SECTOR}", "::/system/BOOT.CFG", disk_cfg], epoch)
        options = disk_cfg.read_bytes()
        disk_cfg.unlink()
        require(sha256_bytes(options) == by_path["/system/BOOT.CFG"]["sha256"], "disk BOOT.CFG")
        require(SETUP_TOKEN not in options.split(), "the disk image BOOT.CFG must not carry setup=1")
        setup_options = options.rstrip(b"\r\n ") + b" " + SETUP_TOKEN + b"\n"
        require(len(setup_options) <= 127 and all(32 <= c < 127 or c in (10, 13) for c in setup_options), "Setup BOOT.CFG extent")
        # fx-12: the CD installs the Setup program without DWARF, like the disk's programs.
        setup_installed = Path(setup_elf)
        if bi.is_native_elf(setup_elf):
            setup_installed = bi.strip_elf(setup_elf, work / "setup-installed" / "setup", "payload /Programs/setup")
        extra = {"/system/BOOT.CFG": setup_options, "/Programs/setup": setup_installed.read_bytes()}
        for path, data in extra.items():
            target = stage / path.lstrip("/")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            os.utime(target, (epoch, epoch))
            files[path] = target
        check_setup_programs(files)
        cd_paths = sorted({*files, "/system/RELEASE.JSON"})
        games = [p["name"] for p in manifest.get("games", {}).get("packages", [])]
        assignment, present, required = eligibility(config, cd_paths, games)
        require(not any("DMFAQ66" in p.upper() for p in cd_paths), "DMFAQ66 files must stay excluded")
        # ISO tree: sources, licences, answer-file sample.
        iso_tree_dir = work / "iso"
        archives = build_archives(config, required, iso_tree_dir / "SOURCES", epoch, image.parent, manifest)
        identity = {"version": manifest["version"], "version_full": manifest.get("version_full", "unknown"),
                    "version_display": manifest.get("version_display", manifest["version"]), "git_revision": manifest["git_revision"], "git_dirty": manifest["git_dirty"]}
        licence_count = licence_tree(config, present, {p: files[p] for p in files}, iso_tree_dir / "LICENSES",
                                     archives, assignment, epoch, identity)
        (iso_tree_dir / SAMPLE).write_text(ANSWER_SAMPLE)
        iso_files = {p.relative_to(iso_tree_dir).as_posix(): p for p in iso_tree_dir.rglob("*") if p.is_file()}
        digest, listed = content_digest(files, iso_files)
        release = {"schema_version": 1, "version": manifest["version"], "version_full": manifest.get("version_full", "unknown"),
                    "version_display": manifest.get("version_display", manifest["version"]), "git_revision": manifest["git_revision"],
                   "git_dirty": manifest["git_dirty"], "disk_image": manifest["image"], "disk_image_sha256": manifest["image_sha256"],
                   "kernel_sha256": manifest["kernel_sha256"], "build_epoch": epoch, "boot_options": setup_options.decode(),
                   "payload_files": len(files) + 1, "payload_bytes": sum(Path(s).stat().st_size for s in files.values()),
                   "content_digest": digest,
                   "content_digest_scope": "SHA-256 of sorted '<scope>:<path> <size> <sha256>' lines for every boot image file "
                                           "except /system/RELEASE.JSON and every ISO file except CIUKBOOT.IMG, BOOT.CAT and CDMANIFE.JSN"}
        release_path = stage / "system/RELEASE.JSON"
        release_path.write_text(json.dumps(release, indent=2, sort_keys=True) + "\n")
        os.utime(release_path, (epoch, epoch))
        files["/system/RELEASE.JSON"] = release_path
        # mattrib prints FAT names (one case); the manifest keeps the build's.
        file_names = {p.lower(): p for p in by_path}
        directories = sorted(name for name in tree if name != "/" and name.lower() not in file_names)
        # FAT names are case-insensitive: the manifest's /System/... and
        # /system/... share the disk image's one directory (mattrib names).
        folded = {d.lower() for d in directories}
        for path in files:
            parent = path.rsplit("/", 1)[0]
            while parent:
                require(parent.lower() in folded, f"directory missing in the disk image tree: {parent}")
                parent = parent.rsplit("/", 1)[0]
        readonly = sorted(file_names[p.lower()] for p, flags in tree.items()
                          if "R" in flags and p.lower() in file_names and file_names[p.lower()] in files)
        boot_records, directory_records = boot_manifest_records(files, directories, tree, assignment)
        attributes = {r["path"]: r["attributes"] for r in (*boot_records, *directory_records)}
        boot_image, part_count, plan = build_boot_image(work, files, directories, readonly, mbr_code, loader, epoch,
                                                        bi.initialize_blackbox, attributes)
        facts, fsck = check_boot_image(boot_image, part_count, mbr_code, loader, files, epoch)
        boot_sha = sha256_file(boot_image)
        shutil.move(boot_image, iso_tree_dir / BOOT_IMAGE)
        for p in [iso_tree_dir, *iso_tree_dir.rglob("*")]:
            os.utime(p, (epoch, epoch))
        # Manifest copy on the ISO: everything except the ISO's own identity.
        iso_records = [{"path": p, "sha256": sha256_file(s), "size": s.stat().st_size, "attributes": 32} for p, s in sorted(iso_files.items())]
        cd = {
            "schema_version": 1,
            "build": {"version": manifest["version"], "version_full": manifest.get("version_full", "unknown"),
                    "version_display": manifest.get("version_display", manifest["version"]), "git_revision": manifest["git_revision"], "git_dirty": manifest["git_dirty"],
                      "source_revision": head, "source_matches_build": source_matches, "build_epoch": epoch,
                      "disk_image": manifest["image"], "disk_image_sha256": manifest["image_sha256"],
                      "kernel_sha256": manifest["kernel_sha256"],
                      "kernel_full_sha256": manifest.get("kernel_full_sha256", manifest["kernel_sha256"]),
                      "loader_sha256": sha256_bytes(loader),
                      "sibling_ciukldr_bin": sibling_state, "build_clock": clock_state, "setup_program_sha256": sha256_file(setup_installed),
                      "setup_program_full_sha256": sha256_file(setup_elf),
                      "tools": {"xorriso": subprocess.run(["xorriso", "-version"], capture_output=True, text=True).stdout.splitlines()[0],
                                "mtools": subprocess.run(["mtools", "--version"], capture_output=True, text=True).stdout.splitlines()[0]}},
            "boot_image": {"file": "ciukios-setup-boot.img", "iso_path": BOOT_IMAGE, "sha256": boot_sha,
                           "image_sectors": PART_LBA + part_count, "bytes": (PART_LBA + part_count) * SECTOR,
                           "iso_blocks": (PART_LBA + part_count) // 4, "partition_start": PART_LBA, "partition_sectors": part_count,
                           "signature": f"0x{SETUP_SIGNATURE:08X}", "start_chs": facts["start_chs"], "end_chs": facts["end_chs"],
                           "emulated_geometry": facts["geometry"], "bpb": {k: facts[k] for k in (
                               "bytes_per_sector", "sectors_per_cluster", "reserved", "fats", "fat_sectors", "total_sectors",
                               "root_cluster", "fsinfo_sector", "backup_sector", "ext_flags")},
                           "clusters": facts["clusters"], "data_start_sector": facts["data_start"],
                           "data_region_cd_aligned": (PART_LBA + facts["data_start"]) % 4 == 0, "plan": plan,
                           "blackbox": {"descriptor_sector": 8, "first_sector": 32, "sectors": 128, "ring_empty": facts["blackbox_ring_empty"]},
                           "fsck": fsck, "boot_options": setup_options.decode(),
                           "boot_cfg_template": options.decode("ascii"), "boot_cfg_sha256": sha256_bytes(options),
                           "files": boot_records, "directories": directory_records},
            "iso_tree": iso_records,
            "archives": archives,
            "components": {"present": present, "rows": [c for c in config["components"] if c["id"] in present],
                           "config_sha256": sha256_file(config_path)},
            "licences": licence_count,
            "content_digest": digest,
            "content_digest_lines": listed,
        }
        (iso_tree_dir / MANIFEST_COPY).write_text(json.dumps(cd, indent=2, sort_keys=True) + "\n")
        os.utime(iso_tree_dir / MANIFEST_COPY, (epoch, epoch))
        iso = work / "setup.iso"
        cd["iso_command"] = build_iso(iso_tree_dir, iso, epoch)
        expected = {p.relative_to(iso_tree_dir).as_posix(): p for p in iso_tree_dir.rglob("*") if p.is_file()}
        iso_facts = check_iso(iso, iso_tree_dir / BOOT_IMAGE, part_count, expected, epoch, work)
        sectors = -(-iso.stat().st_size // CD_BLOCK)
        cd["iso"] = {"file": "ciukios-setup.iso", "sha256": sha256_file(iso), "bytes": iso.stat().st_size, "sectors": sectors,
                     "limit_sectors": limit_sectors, "limit_bytes": limit_sectors * CD_BLOCK,
                     "headroom_sectors": limit_sectors - sectors, "headroom_bytes": (limit_sectors - sectors) * CD_BLOCK,
                     "volume_id": VOLUME_ID, **iso_facts}
        cd["iso_tree"].append({"path": BOOT_IMAGE, "sha256": boot_sha, "size": (PART_LBA + part_count) * SECTOR})
        cd["iso_tree"].append({"path": MANIFEST_COPY, "sha256": sha256_file(iso_tree_dir / MANIFEST_COPY),
                               "size": (iso_tree_dir / MANIFEST_COPY).stat().st_size})
        cd["iso_tree"].append({"path": CATALOG, "sha256": iso_facts["catalog_file_sha256"], "size": CD_BLOCK})
        cd["iso_tree"].sort(key=lambda r: r["path"])
        require(sectors <= limit_sectors, f"ISO is {sectors} sectors, over the {limit_sectors}-sector CD-R limit")
        shutil.move(iso, out_dir / "ciukios-setup.iso")
        shutil.move(iso_tree_dir / BOOT_IMAGE, out_dir / "ciukios-setup-boot.img")
        if out_dir != image.parent:
            (out_dir / "ciukldr.bin").write_bytes(loader)
        (out_dir / "cd-manifest.json").write_text(json.dumps(cd, indent=2, sort_keys=True) + "\n")
        if out_dir == image.parent:
            manifest["cd"] = {"iso_sha256": cd["iso"]["sha256"], "boot_image_sha256": boot_sha, "iso_sectors": sectors,
                              "manifest": "cd-manifest.json", "content_digest": digest}
            manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
        print(f"[cd] boot image {PART_LBA + part_count} sectors, {facts['clusters']} clusters, SHA256 {boot_sha}")
        print(f"[cd] ISO {sectors} sectors ({cd['iso']['bytes']} bytes, headroom {limit_sectors - sectors} sectors) SHA256 {cd['iso']['sha256']}")
        return cd
    finally:
        if not keep_work and work.exists():
            for p in [work, *work.rglob("*")]:
                if p.is_dir():
                    p.chmod(0o700)
            shutil.rmtree(work)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image", type=Path, default=ROOT / "build/f0/ciukios.img")
    parser.add_argument("--out-dir", type=Path, help="default: the disk image's directory")
    parser.add_argument("--setup-elf", type=Path, help="default: build apps/setup with the validated SDK")
    parser.add_argument("--allow-source-mismatch", action="store_true",
                        help="evidence builds from another revision's payload (recorded in cd-manifest.json)")
    args = parser.parse_args()
    setup = args.setup_elf
    if setup is None:
        setup, _ = module("cd_setup_program", ROOT / "apps/setup/build_setup.py").build()
    build(args.image, args.out_dir or args.image.resolve().parent, setup, allow_source_mismatch=args.allow_source_mismatch)


if __name__ == "__main__":
    main()
