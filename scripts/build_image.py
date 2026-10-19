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
FAT_RESERVED = 160
BLACKBOX_DESCRIPTOR = 8
BLACKBOX_FIRST = 32
BLACKBOX_SECTORS = 128
IMAGE_BYTES = 512 * 1024 * 1024
HEADER = struct.Struct("<4sHHIHH")
DEFAULT_CONFIG = b"safe=0 serial=1\n"
sys.dont_write_bytecode = True
SDK = ROOT / "build/tools/ciuki-sdk"
LUA = ROOT / "build/apps/lua"
DESKTOP = ROOT / "build/apps/desktop"
TEST_PATH = "/system/tests/lua-5.4.8-tests"
WALLPAPERS = ("Ciuk1.png", "Ciuk2.png", "Ciuk3.png")
WALLPAPER_SIZES = ((800, 600), (1024, 768), (1280, 800), (1280, 1024), (1400, 1050))


def wallpaper_cache_payloads(out=None):
    """Pre-render the default with the production C renderer, never a PIL substitute."""
    out = Path(out) if out is not None else DESKTOP / "wallpaper-cache"
    source = ROOT / "assets/wallpapers/ciuki/Ciuk2.png"
    # Ciuki interprets FAT timestamps as UTC; mcopy -m with TZ=UTC below
    # preserves this exact source timestamp, truncated to two seconds.
    mtime = int(source.stat().st_mtime) & ~1
    units = ("render_wallpaper.c", "wallpaper_io.c", "wallpaper.c", "image.c", "settings.c", "theme.c")
    inputs = {"sources": {name: sha256(ROOT / "apps/desktop" / name) for name in (*units, "wallpaper_io.h", "wallpaper.h", "settings.h", "theme.h", "ui_fonts.h", "desktop.h", "shell.h", "protocol.h")},
              "stb_sha256": sha256(ROOT / "third_party/stb/stb_image.h"),
              "source_sha256": sha256(source), "source_size": source.stat().st_size, "source_mtime": mtime}
    manifest = out / "manifest.json"
    try:
        record = json.loads(manifest.read_text())
        valid = record["inputs"] == inputs and len(record["files"]) == len(WALLPAPER_SIZES) and all(
            (out / name).stat().st_size == frame["size"] and sha256(out / name) == frame["sha256"]
            for name, frame in record["files"].items())
    except (OSError, ValueError, KeyError):
        valid = False
    if not valid:
        out.mkdir(parents=True, exist_ok=True)
        # Regenerate this derived staging directory, including obsolete keys.
        for path in out.glob("*.raw"):
            path.unlink()
        with tempfile.TemporaryDirectory(prefix="render-", dir=out) as temporary:
            tool = Path(temporary) / "render-wallpaper"
            subprocess.run(["nice", "-n", "19", "clang", "-std=c17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                            *(str(ROOT / "apps/desktop" / name) for name in units), "-o", str(tool)], check=True)
            result = subprocess.run(["nice", "-n", "19", str(tool), str(source), str(out), str(mtime)], check=True,
                                    capture_output=True, text=True)
        names = result.stdout.splitlines()
        require(len(names) == len(WALLPAPER_SIZES), "default wallpaper cache resolutions")
        record = {"inputs": inputs, "format": "XRGB8888 little-endian B,G,R,0", "mode": "fill", "colour": "375564",
                  "source_path": "/system/wallpapers/Ciuk2.png", "files": {}}
        for name, (width, height) in zip(names, WALLPAPER_SIZES):
            require(Path(name).name == name and name.endswith(f"-{width}x{height}-fill.raw"), "wallpaper cache key")
            path = out / name
            require(path.stat().st_size == width * height * 4, "wallpaper cache frame size")
            record["files"][name] = {"sha256": sha256(path), "size": path.stat().st_size, "width": width, "height": height}
        manifest.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    return {f"/System/cache/wallpaper/{name}": out / name for name in record["files"]}, record


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


def read_version():
    """{'number', 'stage', 'display'} from config/version.json, the single
    source of the CiukiOS version (validated by scripts/ciuki_version.py)."""
    spec = importlib.util.spec_from_file_location("ciuki_version", ROOT / "scripts/ciuki_version.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.load()


def desktop_payloads():
    """Validate desktop/demo ELFs and the exact converted portrait independently."""
    spec = importlib.util.spec_from_file_location("build_desktop", ROOT / "apps/desktop/build_desktop.py")
    desktop_build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(desktop_build)
    desktop = desktop_build.validate_payloads()
    sources = {"/Programs/desktop": DESKTOP / "desktop", "/Programs/demo": DESKTOP / "demo", "/Programs/dosbox": DESKTOP / "dosbox",
               "/Programs/gfxdemo": DESKTOP / "gfxdemo",
               "/Programs/files": DESKTOP / "files", "/Programs/tasks": DESKTOP / "tasks", "/Programs/ciuknote": DESKTOP / "ciuknote",
               "/Programs/regedit": DESKTOP / "regedit",
               "/Programs/control": DESKTOP / "control", "/Programs/ciukide": DESKTOP / "ciukide",
               "/Programs/calc": DESKTOP / "calc", "/Programs/charmap": DESKTOP / "charmap", "/Programs/clock": DESKTOP / "clock",
               "/Programs/help": DESKTOP / "help", "/Programs/mixer": DESKTOP / "mixer", "/Programs/viewer": DESKTOP / "viewer",
               "/Programs/terminal": DESKTOP / "terminal",
               "/Programs/paint": DESKTOP / "paint", "/Programs/music": DESKTOP / "music",
               "/system/assets/ciuki-portrait.xrgb": DESKTOP / "ciuki-portrait.xrgb",
               "/system/licenses/stb_image.txt": ROOT / "third_party/stb/LICENSE"}
    sources.update({f"/system/fonts/{p.name}": p for p in (ROOT / "assets/fonts/native").glob("*.CFN")})
    sources["/system/fonts/LICENSES.TXT"] = ROOT / "assets/fonts/LICENSES.TXT"
    font_sources, fonts = font_payloads()
    sources.update(font_sources)
    for name in ("FreeType-FTL.txt", "FreeType-LICENSE.txt", "FreeType-NOTICE.txt"):
        sources[f"/system/licenses/{name}"] = SDK / "licenses" / name
    for name in ("TinyGL-LICENSE.txt", "TinyGL-NOTICE.txt"):
        sources[f"/system/licenses/{name}"] = SDK / "licenses" / name
    sources["/system/licenses/minimp3.txt"] = ROOT / "third_party/minimp3/LICENSE"
    sources["/system/licenses/stb_vorbis.txt"] = ROOT / "third_party/stb/LICENSE"
    sources["/system/licenses/stb_image_write.txt"] = ROOT / "third_party/stb/LICENSE"
    # Tango 0.8.90 system icon store and its Public Domain notice/authors/credits.
    sources.update({f"/system/icons/{name}": DESKTOP / name for name in desktop_build.ICON_FILES})
    hwids = ROOT / "assets/hwids"
    sources["/System/hwids/pci-ids.json"] = hwids / "pci-ids.json"
    sources["/System/hwids/usb-ids.json"] = hwids / "usb-ids.json"
    metadata = {"manifest_sha256": sha256(DESKTOP / "manifest.json"),
                "protocol_version": desktop["protocol_version"],
                "elf": desktop["elf"], "portrait": desktop["portrait"], "icons": desktop["icons"], "fonts": fonts}
    # The owner's three adaptive Ciuki scenes (assets/wallpapers/ciuki, 1448x1086 PNG; Ciuk2 is the
    # default wallpaper of apps/desktop/settings.c). /system/settings holds desktop.cfg (written at run
    # time); the owner may add PNG/JPEG/BMP files to /system/wallpapers. Never Microsoft's
    # third_party/win_bg bitmaps: see docs/desktop.md.
    sources.update({f"/system/wallpapers/{name}": ROOT / "assets/wallpapers/ciuki" / name for name in WALLPAPERS})
    cache_sources, cache = wallpaper_cache_payloads()
    sources.update(cache_sources)
    metadata["wallpaper_cache"] = cache
    sound_sources, sounds = sound_payloads()
    sources.update(sound_sources)
    metadata["sounds"] = sounds
    return sources, {"/Documents", "/Documents/Music", "/Documents/Pictures", "/system/sounds", "/Programs", "/System", "/System/hwids", "/System/cache", "/System/cache/wallpaper", "/system/assets", "/system/icons", "/system/licenses", "/system/fonts", "/system/settings", "/system/wallpapers"}, metadata


def font_payloads():
    """Pinned, unchanged Liberation files, independently of SDK/image builds."""
    folder = ROOT / "assets/fonts/liberation"
    record = json.loads((folder / "UPSTREAM.json").read_text())
    expected = {f"Liberation{family}-{style}.ttf" for family in ("Sans", "Serif", "Mono")
                for style in ("Regular", "Bold", "Italic", "BoldItalic")}
    require(set(record["files"]) == expected, "Liberation inventory must contain twelve faces")
    require({p.name for p in folder.glob("*.ttf")} == expected, "unexpected Liberation files")
    sources = {}
    for name, digest in record["files"].items():
        require(sha256(folder / name) == digest, f"changed Liberation font: {name}")
        sources[f"/System/fonts/{name}"] = folder / name
    require(sha256(folder / "LICENSE") == record["license_sha256"], "changed Liberation license")
    sources["/System/fonts/LIBERATION-LICENSE.txt"] = folder / "LICENSE"
    sources["/System/fonts/LIBERATION-UPSTREAM.json"] = folder / "UPSTREAM.json"
    sources["/System/fonts/LIBERATION-AUTHORS.txt"] = folder / "AUTHORS"
    require(sha256(folder / "AUTHORS") == record["authors_sha256"], "changed Liberation authors")
    return sources, {"version": record["version"], "record_sha256": sha256(folder / "UPSTREAM.json"),
                     "files": {name: sha256(source) for name, source in sorted(sources.items())},
                     "bytes": sum(p.stat().st_size for p in sources.values())}


def sound_payloads():
    """Validate and map the bounded provisional sound pack; no image build."""
    spec = importlib.util.spec_from_file_location("build_sounds", ROOT / "scripts/build_sounds.py")
    sound_build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sound_build)
    sounds = sound_build.validate()
    sources = {f"/system/sounds/{name}":
               sound_build.OUTPUT / name for name in sounds["files"]}
    return sources, sounds


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
    sources = {"/Programs/lua": LUA / "lua",
               "/Programs/hello": SDK / "tests/hello.elf",
               "/Programs/libc_smoke": SDK / "tests/libc_smoke.elf",
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
    tcc_spec = importlib.util.spec_from_file_location("image_tcc", ROOT / "apps/tcc/build_tcc.py")
    tcc_build = importlib.util.module_from_spec(tcc_spec)
    tcc_spec.loader.exec_module(tcc_build)
    tcc_sources, tcc_directories, tcc_metadata = tcc_build.image_payloads()
    sources.update(tcc_sources)
    w32_spec = importlib.util.spec_from_file_location("image_w32srv", ROOT / "apps/w32srv/build_w32srv.py")
    w32_build = importlib.util.module_from_spec(w32_spec)
    w32_spec.loader.exec_module(w32_build)
    w32_sources, w32_directories, w32_metadata = w32_build.image_payloads()
    sources.update(w32_sources)
    directories = {"/Programs", "/tmp", "/Documents", "/Documents/Desktop", "/Documents/Trash", "/System", "/system/tests",
                   "/system/licenses", "/system/assets", TEST_PATH}
    directories.update(f"{TEST_PATH}/{name}" for name in manifest["test_directories"])
    directories.update(desktop_directories)
    directories.update(tcc_directories)
    directories.update(w32_directories)
    metadata = {"sdk_manifest_sha256": expected["sdk_manifest_sha256"],
                "lua": {"version": manifest["version"],
                        "manifest_sha256": sha256(LUA / "manifest.json"),
                        "archives": expected["archives"], "elf": manifest["elf"],
                        "supplement_sha256": manifest["files"]["ciuki-f2.lua"],
                        "upstream_mode": manifest["upstream_mode"], "patches": manifest["patches"]}}
    metadata["desktop"] = desktop_metadata
    metadata["tcc"] = tcc_metadata
    metadata["w32srv"] = w32_metadata
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
        if path == "/Programs/lua" or path == "/system/tests/ciuki-f2.lua" or path.startswith(TEST_PATH + "/"):
            require(len(path) <= 80, "app-gate payload path exceeds evidence bound")
            lines.append(f"P {path} {sha256(source)}\n")
    return "".join(lines)


def games_payloads(dos_image, out=None):
    """Owner decisions 2026-10-12/14: Wolfenstein 3-D, DOOM and Quake shareware are
    pre-installed. The games disk (the DOS system plus \\WOLF3D, \\DOOM and \\QUAKE,
    from the pinned archives in third_party/games) is the base of every
    /Programs/dosbox window VM (dosvm_disk.c, C:\\GAMES\\CIUKIGAM.IMG); the
    packages' licence documents and provenance go to /system/licenses.
    Returns sources, directories, metadata and the SHA-256 set of pinned
    shareware files the local-payload guard accepts. out: the disk's path
    (default build/f0/games/CIUKIGAM.IMG; host tests write elsewhere)."""
    spec = importlib.util.spec_from_file_location("build_games_disk", ROOT / "scripts/build_games_disk.py")
    games = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(games)
    image, manifest = games.build(out, dos_image)
    sources = {"/Games/CIUKIGAM.IMG": image}
    for record in manifest["licenses"]:
        sources[f"/system/licenses/{record['package']}-{record['name']}"] = ROOT / record["path"]
    for package in manifest["packages"]:
        sources[f"/system/licenses/{package['name']}-UPSTREAM.json"] = ROOT / games.GAMES / package["name"] / "UPSTREAM.json"
    metadata = {k: v for k, v in manifest.items() if k not in ("checkers", "geometry")}
    return sources, {"/Games", "/system/licenses"}, metadata, games.free_hashes()


def payload_records(sources):
    return [{"path": name, "sha256": sha256(source), "size": source.stat().st_size}
            for name, source in sorted(sources.items())]


def populate_payloads(volume, sources, directories):
    for directory in sorted(directories, key=lambda p: (p.count("/"), p)):
        run("mmd", "-i", volume, "::" + directory)
    for name, source in sorted(sources.items()):
        if name.startswith("/system/wallpapers/"):
            subprocess.run(["mcopy", "-m", "-i", str(volume), str(source), "::" + name], cwd=ROOT,
                           env=dict(os.environ, TZ="UTC"), check=True)
        else:
            run("mcopy", "-i", volume, source, "::" + name)


def check_payloads(image, payloads, directories):
    """Read every file from the final disk, rather than trusting the staging volume."""
    volume = f"{image}@@{PART_LBA * SECTOR}"
    for directory in sorted(directories):
        subprocess.run(["mdir", "-i", volume, "::" + directory], cwd=ROOT,
                       check=True, stdout=subprocess.DEVNULL)
    # A case-insensitive lookup alone would accept all-uppercase 8.3 names.
    # Check enumeration too: Files must display the owner's exact long names.
    display_names = {name for name in ("Programs", "Documents", "System", "Games")
                     if "/" + name in directories}
    if display_names:
        listing = subprocess.run(["mdir", "-b", "-i", volume, "::/"], cwd=ROOT,
                                 check=True, capture_output=True, text=True).stdout
        names = {line.rstrip("/").rsplit("/", 1)[-1] for line in listing.splitlines()}
        for name in sorted(display_names):
            require(name in names, f"directory display name: {name}")
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


def initialize_blackbox(volume):
    """Reserve raw crash sectors outside allocation and BPB metadata."""
    descriptor = bytearray(SECTOR)
    struct.pack_into("<4sIII", descriptor, 0, b"CBB1", 1, BLACKBOX_FIRST, BLACKBOX_SECTORS)
    struct.pack_into("<I", descriptor, 508, zlib.crc32(descriptor[:508]))
    with volume.open("r+b") as stream:
        bpb = stream.read(SECTOR)
        require(struct.unpack_from("<H", bpb, 14)[0] == FAT_RESERVED, "blackbox reserved geometry")
        require(struct.unpack_from("<HH", bpb, 48) == (1, 6), "blackbox BPB overlap")
        stream.seek(BLACKBOX_DESCRIPTOR * SECTOR)
        stream.write(descriptor)
        stream.seek(BLACKBOX_FIRST * SECTOR)
        stream.write(bytes(BLACKBOX_SECTORS * SECTOR))


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
    require((bps, spc, reserved, fats) == (512, 8, FAT_RESERVED, 2), "FAT32 geometry")
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
        stream.seek(BLACKBOX_DESCRIPTOR * SECTOR)
        descriptor = stream.read(SECTOR)
        require(struct.unpack_from("<4sIII", descriptor) == (b"CBB1", 1, BLACKBOX_FIRST, BLACKBOX_SECTORS), "blackbox descriptor")
        require(struct.unpack_from("<I", descriptor, 508)[0] == zlib.crc32(descriptor[:508]), "blackbox descriptor checksum")
        stream.seek(BLACKBOX_FIRST * SECTOR)
        require(stream.read(BLACKBOX_SECTORS * SECTOR) == bytes(BLACKBOX_SECTORS * SECTOR), "blackbox empty ring")
        stream.seek(6 * SECTOR)
        require(stream.read(SECTOR) == bpb, "backup BPB")
        require(stream.read(SECTOR) == fsinfo, "backup FSInfo")
    run("fsck.fat", "-n", volume)
    print("T1 PASS: MBR, FAT32 geometry, fsck, loader size and CRC32")


def registry_payloads(directory):
    """Seed the persistent identity root using libreg's UTF-8 text dialect."""
    revision, _ = git_identity()
    # JSON quoting shares the registry string escapes for this ASCII metadata.
    # ensure_ascii=False keeps the UTF-8 storage contract explicit.
    quote = lambda value: json.dumps(value, ensure_ascii=False)
    text = ("; ciuki-registry generation=1\n"
            "; System identity seeded by the image builder\n"
            "[System\\Version]\n"
            f"name = {quote('CiukiOS')}\n"
            f"version = {quote(read_version()['display'])}\n"
            f"build = {quote(revision)}\n"
            "[System\\Identity]\n"
            f"Tagline = {quote('Classic computing, one woof at a time.')}\n")
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "System.reg"
    path.write_text(text, encoding="utf-8")
    return {"/System/Registry/System.reg": path}, {"/System", "/System/Registry"}


def build(kernel, out, boot_cfg):
    for tool in ("nasm", "mkfs.fat", "mmd", "mdir", "mcopy", "fsck.fat"):
        if not shutil.which(tool):
            raise SystemExit(f"missing host tool: {tool}")
    if not kernel.is_file():
        raise SystemExit(f"kernel not found: {kernel}")
    # Refuse a kernel compiled with another version before the long steps; the
    # record is absent only for kernels built before the version existed.
    built = json.loads((kernel.parent / "build-clock.json").read_text()).get("version")
    wanted = read_version()["display"]
    require(built in (None, wanted),
            f"kernel built as '{built}', config/version.json says '{wanted}'; run make kernel")
    options = boot_cfg.read_bytes() if boot_cfg else DEFAULT_CONFIG
    if len(options) > 127 or b"\0" in options or any(c > 127 for c in options):
        raise SystemExit("BOOT.CFG must be <=127 ASCII bytes without NUL")
    sources, directories, metadata = application_payloads()
    registry_sources, registry_directories = registry_payloads(out.parent / "registry")
    sources.update(registry_sources)
    directories.update(registry_directories)
    dos_spec = importlib.util.spec_from_file_location("build_dos_image", ROOT / "scripts/build_dos_image.py")
    dos_build = importlib.util.module_from_spec(dos_spec)
    dos_spec.loader.exec_module(dos_build)
    dos_image, dos_manifest = dos_build.build()
    sources["/system/dos/DOSBOOT.IMG"] = dos_image
    directories.add("/system/dos")
    metadata["dos"] = dos_manifest
    games_sources, games_directories, metadata["games"], free = games_payloads(dos_image)
    sources.update(games_sources)
    directories.update(games_directories)
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
        # The menu title carries the version; tests assemble the loader
        # without the define and get 'unknown'.
        run("nasm", "-f", "bin", f"-dCIUKI_VERSION='{wanted}'",
            ROOT / "src/boot/ciukldr.asm", "-o", loader_path)
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
            "-f", "2", "-R", str(FAT_RESERVED), "-b", "6", "-h", str(PART_LBA),
            "-n", "CIUKIOS", volume)
        initialize_blackbox(volume)
        sources.update({"/system/VMM.ELF": kernel, "/system/BOOT.CFG": cfg})
        payloads = payload_records(sources)
        # f3-06: owner-local game files never enter the canonical image; only
        # files pinned in the vendored shareware packages may equal one.
        dos_build.refuse_local_payloads({p["path"]: p["sha256"] for p in payloads}, free=free)
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
        # The version the kernel was compiled with (build() has checked it
        # against config/version.json).
        "version": clock_record.get("version", read_version()["display"]),
        "git_revision": rev,
        "git_dirty": dirty,
        "image": image.name,
        "image_sha256": image_sha,
        "blackbox": {"descriptor_sector": BLACKBOX_DESCRIPTOR, "first_sector": BLACKBOX_FIRST, "sectors": BLACKBOX_SECTORS, "reserved_sectors": FAT_RESERVED},
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
