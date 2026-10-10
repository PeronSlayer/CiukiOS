#!/usr/bin/env python3
"""Build and statically verify the canonical Ciuki F0 MBR/FAT32 disk."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import importlib.util
import errno
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
SECTOR = 512
PART_LBA = 2048
IMAGE_BYTES = 512 * 1024 * 1024
HEADER = struct.Struct("<4sHHIHH")
DEFAULT_CONFIG = b"safe=0 serial=1\n"
sys.dont_write_bytecode = True
SDK = ROOT / "build/tools/ciuki-sdk"
LUA = ROOT / "build/apps/lua"
DESKTOP = ROOT / "build/apps/desktop"
TEST_PATH = "/system/tests/lua-5.4.8-tests"


def run(*args):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def desktop_payloads():
    """Validate desktop/demo ELFs and the exact converted portrait independently."""
    spec = importlib.util.spec_from_file_location("build_desktop", ROOT / "apps/desktop/build_desktop.py")
    desktop_build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(desktop_build)
    desktop = desktop_build.validate_payloads()
    sources = {"/bin/desktop": DESKTOP / "desktop", "/bin/demo": DESKTOP / "demo",
               "/system/assets/ciuki-portrait.xrgb": DESKTOP / "ciuki-portrait.xrgb"}
    metadata = {"manifest_sha256": sha256(DESKTOP / "manifest.json"),
                "protocol_version": desktop["protocol_version"],
                "elf": desktop["elf"], "portrait": desktop["portrait"]}
    return sources, {"/bin", "/system", "/system/assets"}, metadata


def application_payloads():
    """Validate SDK/Lua/desktop provenance before creating an image."""
    spec = importlib.util.spec_from_file_location("build_lua", ROOT / "apps/lua/build_lua.py")
    lua_build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lua_build)
    sdk = lua_build.validate_sdk()
    manifest = json.loads((LUA / "manifest.json").read_text())
    expected = {"archives": json.loads((ROOT / "config/sdk-pins.json").read_text())["lua"],
                "sdk_manifest_sha256": sha256(SDK / "manifest.json"),
                "app_sources": lua_build.inventory(ROOT / "apps/lua"),
                "cflags": lua_build.CFLAGS,
                "configuration": "generic; C89/Linux/POSIX/readline/dlopen off; int64/double"}
    require(lua_build.current_build(expected), "missing/stale Lua payloads; run make lua")
    inspect = lua_build.checker().inspect
    sources = {"/bin/lua": LUA / "lua",
               "/bin/hello": SDK / "tests/hello.elf",
               "/bin/libc_smoke": SDK / "tests/libc_smoke.elf",
               "/system/tests/ciuki-f2.lua": LUA / "ciuki-f2.lua",
               f"{TEST_PATH}/ciuki-f2.lua": LUA / "ciuki-f2.lua",
               "/system/licenses/lua-5.4.8.txt": LUA / "LUA-LICENSE.txt",
               "/system/licenses/COPYING.NEWLIB": SDK / "licenses/COPYING.NEWLIB",
               "/system/licenses/SDK-MIT.txt": SDK / "licenses/SDK-MIT.txt"}
    for name in ("lua", "luac"):
        require(inspect(LUA / name) == manifest["elf"][name], f"Lua ELF evidence: {name}")
    for name in ("hello", "libc_smoke"):
        actual = inspect(SDK / f"tests/{name}.elf")
        recorded = sdk["test_evidence"][name]
        require(all(actual[k] == recorded[k] for k in actual), f"SDK ELF evidence: {name}")
    for name in manifest["test_files"]:
        sources[f"{TEST_PATH}/{name}"] = LUA / "lua-5.4.8-tests" / name
    desktop_sources, desktop_directories, desktop_metadata = desktop_payloads()
    sources.update(desktop_sources)
    directories = {"/bin", "/tmp", "/home", "/system", "/system/tests",
                   "/system/licenses", "/system/assets", TEST_PATH}
    directories.update(f"{TEST_PATH}/{name}" for name in manifest["test_directories"])
    directories.update(desktop_directories)
    metadata = {"sdk_manifest_sha256": expected["sdk_manifest_sha256"],
                "lua": {"version": manifest["version"],
                        "manifest_sha256": sha256(LUA / "manifest.json"),
                        "archives": expected["archives"], "elf": manifest["elf"],
                        "supplement_sha256": manifest["files"]["ciuki-f2.lua"],
                        "upstream_mode": manifest["upstream_mode"], "patches": manifest["patches"]}}
    metadata["desktop"] = desktop_metadata
    # The gate's relative supplement argv uses TEST_PATH. Retain the f2-07
    # absolute path as an alias; both copies are separately verified in T1.
    # Runtime provenance cannot be recovered from a host-only build manifest.
    # Supply its application fields and immutable inventory in a small sidecar.
    provenance = LUA / "app-gate.meta"
    provenance.write_text(application_provenance(sources, manifest, sdk), encoding="ascii")
    sources["/system/tests/app-gate.meta"] = provenance
    return sources, directories, metadata


def application_provenance(sources, manifest, sdk):
    """Bounded wire format read/validated by f2_probes_app.c, never guest code.

    Lua's official procedure (https://www.lua.org/tests/) requires _U as the
    only supplied switch. The pinned all.lua derives the four omissions below;
    we retain every archive member and hash its actual image bytes in the guest.
    """
    fields = {
        "sdk_manifest_sha256": ("sha256", manifest["inputs"]["sdk_manifest_sha256"]),
        "newlib_source_sha256": ("sha256", sdk["newlib"]["sha256"]),
        "newlib_patch_hashes": ("json", sdk["newlib"]["patches"]),
        "application_source_sha256": ("sha256", manifest["inputs"]["archives"]["source"]["sha256"]),
        "application_tests_sha256": ("sha256", manifest["inputs"]["archives"]["tests"]["sha256"]),
        "declared_exclusions": ("json", {"_U": True, "_soft": True, "_port": True,
                                          "_nomsg": True, "T": None,
                                          "complete": "excluded_by_contract",
                                          "internal": "excluded_by_contract"}),
    }
    lines = []
    for name, (encoding, value) in fields.items():
        encoded = value if encoding == "sha256" else json.dumps(value, sort_keys=True, separators=(",", ":")).encode("ascii").hex()
        parts = (len(encoded) + 31) // 32
        for part in range(parts):
            lines.append(f"M {name} {part + 1} {parts} {encoding} {encoded[part*32:(part+1)*32]}\n")
    for path, source in sorted(sources.items()):
        if path == "/bin/lua" or path == "/system/tests/ciuki-f2.lua" or path.startswith(TEST_PATH + "/"):
            require(len(path) <= 80, "app-gate payload path exceeds evidence bound")
            lines.append(f"P {path} {sha256(source)}\n")
    return "".join(lines)


def payload_records(sources):
    return [{"path": name, "sha256": sha256(source), "size": source.stat().st_size}
            for name, source in sorted(sources.items())]


def populate_payloads(volume, sources, directories):
    for directory in sorted(directories, key=lambda p: (p.count("/"), p)):
        run("mmd", "-i", volume, "::" + directory)
    for name, source in sorted(sources.items()):
        run("mcopy", "-i", volume, source, "::" + name)


def check_payloads(image, payloads, directories):
    """Read every file from the final disk, rather than trusting the staging volume."""
    volume = f"{image}@@{PART_LBA * SECTOR}"
    for directory in sorted(directories):
        subprocess.run(["mdir", "-i", volume, "::" + directory], cwd=ROOT,
                       check=True, stdout=subprocess.DEVNULL)
    for payload in payloads:
        # mcopy's '-' destination writes raw file bytes to stdout without scratch copies.
        data = subprocess.run(["mcopy", "-i", volume, "::" + payload["path"], "-"],
                              cwd=ROOT, check=True, capture_output=True).stdout
        require(len(data) == payload["size"], f"payload size: {payload['path']}")
        require(hashlib.sha256(data).hexdigest() == payload["sha256"],
                f"payload SHA-256: {payload['path']}")
    print(f"T1 PASS: {len(payloads)} payloads read back with matching sizes/SHA-256; required directories")


def prepare_loader(raw):
    loader = bytearray(raw)
    require(16 < len(loader) <= 1023 * SECTOR and len(loader) % SECTOR == 0,
            "loader size")
    magic, version, _, _, entry, length = HEADER.unpack_from(loader)
    require(magic == b"CLDR" and version == 1 and length == 16, "loader header")
    require(16 <= entry < len(loader), "loader entry")
    struct.pack_into("<H", loader, 6, len(loader) // SECTOR)
    struct.pack_into("<I", loader, 8, zlib.crc32(loader[16:]))
    return loader


def check_image(image, volume):
    with image.open("rb") as stream:
        mbr = stream.read(SECTOR)
        require(len(mbr) == SECTOR and mbr[510:] == b"\x55\xaa", "MBR signature")
        parts = [mbr[446+i*16:462+i*16] for i in range(4)]
        require(parts[0][0] == 0x80 and parts[0][4] == 0x0C, "active FAT32 LBA")
        start, count = struct.unpack_from("<II", parts[0], 8)
        require(start == PART_LBA and count == IMAGE_BYTES // SECTOR - start, "FAT32 BPB")
        require(all(p == bytes(16) for p in parts[1:]), "single partition")
        magic, version, sectors, crc, entry, length = HEADER.unpack(stream.read(16))
        require(magic == b"CLDR" and version == 1 and length == 16, "loader header")
        require(1 <= sectors <= 1023 and 16 <= entry < sectors * SECTOR, "loader extent")
        payload = stream.read(sectors * SECTOR - length)
        require(len(payload) == sectors * SECTOR - length, "FAT32 BPB")
        require(zlib.crc32(payload) == crc, "loader CRC32")
        gap = stream.read((PART_LBA - 1 - sectors) * SECTOR)
        require(gap == bytes(len(gap)), "reserved disk gap")
        bpb = stream.read(SECTOR)
    bps, spc, reserved, fats = struct.unpack_from("<HBHB", bpb, 11)
    require((bps, spc, reserved, fats) == (512, 8, 32, 2), "FAT32 geometry")
    require(struct.unpack_from("<H", bpb, 17)[0] == 0, "FAT32 BPB")
    require(struct.unpack_from("<H", bpb, 22)[0] == 0, "FAT32 BPB")
    total = struct.unpack_from("<I", bpb, 32)[0]
    fat_size = struct.unpack_from("<I", bpb, 36)[0]
    require(total == count, "partition/BPB size")
    require((total - reserved - fats * fat_size) // spc >= 65525, "FAT32 cluster count")
    require(struct.unpack_from("<I", bpb, 44)[0] == 2, "root cluster")
    require(struct.unpack_from("<HH", bpb, 48) == (1, 6), "FSInfo/backup locations")
    require(bpb[510:] == b"\x55\xaa", "FAT32 BPB")
    with volume.open("rb") as stream:
        stream.seek(SECTOR)
        fsinfo = stream.read(SECTOR)
        require(struct.unpack_from("<I", fsinfo)[0] == 0x41615252, "FAT32 BPB")
        require(struct.unpack_from("<I", fsinfo, 484)[0] == 0x61417272, "FAT32 BPB")
        require(fsinfo[508:] == b"\0\0\x55\xaa", "FAT32 BPB")
        stream.seek(6 * SECTOR)
        require(stream.read(SECTOR) == bpb, "backup BPB")
        require(stream.read(SECTOR) == fsinfo, "backup FSInfo")
    run("fsck.fat", "-n", volume)
    print("T1 PASS: MBR, FAT32 geometry, fsck, loader size and CRC32")


def build(kernel, out, boot_cfg):
    for tool in ("nasm", "mkfs.fat", "mmd", "mdir", "mcopy", "fsck.fat"):
        if not shutil.which(tool):
            raise SystemExit(f"missing host tool: {tool}")
    if not kernel.is_file():
        raise SystemExit(f"kernel not found: {kernel}")
    options = boot_cfg.read_bytes() if boot_cfg else DEFAULT_CONFIG
    if len(options) > 127 or b"\0" in options or any(c > 127 for c in options):
        raise SystemExit("BOOT.CFG must be <=127 ASCII bytes without NUL")
    sources, directories, metadata = application_payloads()
    out.parent.mkdir(parents=True, exist_ok=True)
    # Large scratch files stay on disk beside the output, never in /tmp.
    with tempfile.TemporaryDirectory(prefix=".image-", dir=out.parent) as directory:
        scratch = Path(directory)
        mbr_path = scratch / "mbr.bin"
        loader_path = scratch / "ciukldr.bin"
        volume = scratch / "system.fat"
        cfg = scratch / "BOOT.CFG"
        cfg.write_bytes(options)
        run("nasm", "-f", "bin", ROOT / "src/boot/mbr.asm", "-o", mbr_path)
        run("nasm", "-f", "bin", ROOT / "src/boot/ciukldr.asm", "-o", loader_path)
        loader = prepare_loader(loader_path.read_bytes())
        loader_path.write_bytes(loader)
        mbr = bytearray(mbr_path.read_bytes())
        require(len(mbr) == 512 and mbr[510:] == b"\x55\xaa", "MBR signature")
        struct.pack_into("<I", mbr, 440, 0x4B554943)  # Stable "CIUK" disk signature.
        # CHS values are conventional saturated markers; boot uses LBA.
        struct.pack_into("<B3sB3sII", mbr, 446, 0x80, b"\x20\x21\0", 0x0C,
                         b"\xfe\xff\xff", PART_LBA,
                         IMAGE_BYTES // SECTOR - PART_LBA)
        partition_bytes = IMAGE_BYTES - PART_LBA * SECTOR
        with volume.open("wb") as stream:
            stream.truncate(partition_bytes)
        run("mkfs.fat", "--invariant", "-a", "-F", "32", "-S", "512", "-s", "8",
            "-f", "2", "-R", "32", "-b", "6", "-h", str(PART_LBA),
            "-n", "CIUKIOS", volume)
        sources.update({"/system/VMM.ELF": kernel, "/system/BOOT.CFG": cfg})
        payloads = payload_records(sources)
        populate_payloads(volume, sources, directories)
        # mtools updates the primary allocation hint but not its backup.
        with volume.open("r+b") as stream:
            stream.seek(SECTOR)
            fsinfo = stream.read(SECTOR)
            stream.seek(7 * SECTOR)
            stream.write(fsinfo)
        # Copy allocated extents, retaining sparsity of the FAT volume.
        candidate = scratch / "ciukios.img"
        with candidate.open("wb") as dest, volume.open("rb") as source:
            dest.truncate(IMAGE_BYTES)
            dest.write(mbr)
            dest.write(loader)
            cursor = 0
            while cursor < partition_bytes:
                try:
                    data = os.lseek(source.fileno(), cursor, os.SEEK_DATA)
                except OSError as exc:
                    if exc.errno == errno.ENXIO:  # ENXIO: no further allocated extent.
                        break
                    raise
                hole = min(os.lseek(source.fileno(), data, os.SEEK_HOLE), partition_bytes)
                source.seek(data)
                dest.seek(PART_LBA * SECTOR + data)
                while data < hole:
                    block = source.read(min(1024 * 1024, hole - data))
                    if not block:
                        raise RuntimeError("short volume read")
                    dest.write(block)
                    data += len(block)
                cursor = hole
        check_image(candidate, volume)
        check_payloads(candidate, payloads, directories)
        os.replace(candidate, out)
    image_sha = sha256(out)
    write_manifest(out, kernel, image_sha, payloads, metadata)
    print(f"SHA256 {image_sha}  {out}")


def git_identity():
    """Commit and dirty state of the sources that produce the image."""
    try:
        rev = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=all",
                                "--", "src", "scripts", "sdk", "apps", "config", "tests", "Makefile"],
                               capture_output=True, text=True, check=True).stdout.strip()
        return rev, bool(dirty)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown", "unknown"


def write_manifest(image, kernel, image_sha, payloads, metadata):
    """build-manifest.json beside the image: read by scripts/test/run.py."""
    rev, dirty = git_identity()
    clock_record = json.loads((kernel.parent / "build-clock.json").read_text())
    require(clock_record["kernel_sha256"] == sha256(kernel), "kernel build-clock hash")
    manifest = {
        "schema_version": 1,
        "git_revision": rev,
        "git_dirty": dirty,
        "image": image.name,
        "image_sha256": image_sha,
        "kernel": str(kernel),
        "kernel_sha256": sha256(kernel),
        "build_epoch": clock_record["utc_epoch"],
        "payloads": payloads,
        **metadata,
    }
    (image.parent / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=ROOT / "build/f0/ciukios.img")
    parser.add_argument("--boot-cfg", type=Path)
    args = parser.parse_args()
    build(args.kernel.resolve(), args.out.resolve(),
          args.boot_cfg.resolve() if args.boot_cfg else None)


if __name__ == "__main__":
    main()
