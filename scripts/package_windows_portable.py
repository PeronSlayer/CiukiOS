#!/usr/bin/env python3
"""Package the full CiukiOS image with a pinned portable Windows QEMU."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import urllib.request
import zipfile


ROOT = Path(__file__).resolve().parent.parent
QEMU_DATE = "20260811"
QEMU_VERSION = "11.1.0"
INSTALLER_NAME = f"qemu-w64-setup-{QEMU_DATE}.exe"
INSTALLER_URL = f"https://qemu.weilnetz.de/w64/2026/{INSTALLER_NAME}"
INSTALLER_SHA512 = (
    "5bcf9eed634e8575a37b74f445af41a2fe4106da512d0c30c368301d4c105037fdfab40a5287367a28a957624cddebbc8c07e16c88ab6634f554cdf3d16bf543"
)
QEMU_EXE_SHA256 = "0b98713bcdb4bc2467142d7f05b8d987cc52e1edbf0a1413e6faad5d581ccff4"
QEMU_DIR = ROOT / "build" / "external" / f"qemu-win64-{QEMU_DATE}"
DOWNLOAD_DIR = ROOT / "build" / "downloads"
RELEASE_DIR = ROOT / "build" / "releases"
NAME = "CiukiOS-0.8.3-Windows-portable"
EXCLUDE_GUEST_DIRS = (
    "::APPS/DOOM",
    "::APPS/DOOMVAN",
    "::APPS/WOLF3D",
    "::APPS/DOSNAV",
    "::DOOMDATA",
)
EXCLUDE_GUEST_FILES = (
    "::DESKTOP/TestGames/DOOM.COM",
    "::DESKTOP/TestGames/DOOMVAN.COM",
    "::DESKTOP/TestGames/WOLF3D.COM",
)


def digest(path: Path, algorithm: str = "sha256") -> str:
    h = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def installer() -> Path:
    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    path = DOWNLOAD_DIR / INSTALLER_NAME
    if not path.exists():
        print(f"[windows-portable] downloading {INSTALLER_URL}", flush=True)
        with urllib.request.urlopen(INSTALLER_URL, timeout=120) as response:
            with tempfile.NamedTemporaryFile(dir=DOWNLOAD_DIR, delete=False) as tmp:
                tmp_path = Path(tmp.name)
                try:
                    shutil.copyfileobj(response, tmp)
                except BaseException:
                    tmp_path.unlink(missing_ok=True)
                    raise
        tmp_path.replace(path)
    if digest(path, "sha512") != INSTALLER_SHA512:
        raise SystemExit(f"[windows-portable] installer SHA-512 mismatch: {path}")
    return path


def qemu_tree() -> Path:
    source = installer()
    exe = QEMU_DIR / "qemu-system-i386w.exe"
    if exe.is_file() and (QEMU_DIR / "share" / "bios.bin").is_file() \
            and (QEMU_DIR / "COPYING").is_file() \
            and (QEMU_DIR / "SDL2.dll").is_file() \
            and (QEMU_DIR / "VERSION").is_file() \
            and (QEMU_DIR / "VERSION").read_text().strip() == QEMU_VERSION:
        return QEMU_DIR
    if shutil.which("7z") is None:
        raise SystemExit("[windows-portable] 7z is required to extract Windows QEMU")
    QEMU_DIR.mkdir(parents=True, exist_ok=True)
    print("[windows-portable] extracting pinned Windows QEMU", flush=True)
    subprocess.run(["7z", "x", "-bd", "-y", "-mmt=2", f"-o{QEMU_DIR}", str(source)],
                   check=True, stdout=subprocess.DEVNULL)
    if not exe.is_file() or not (QEMU_DIR / "share" / "bios.bin").is_file():
        raise SystemExit("[windows-portable] extracted QEMU is incomplete")
    if (QEMU_DIR / "VERSION").read_text().strip() != QEMU_VERSION:
        raise SystemExit("[windows-portable] extracted QEMU version mismatch")
    return QEMU_DIR


def guest_copy(image: Path, output: Path) -> None:
    if shutil.which("mdeltree") is None or shutil.which("mdel") is None:
        raise SystemExit("[windows-portable] mtools (mdeltree and mdel) is required")
    shutil.copyfile(image, output)
    for guest_path in EXCLUDE_GUEST_DIRS:
        exists = subprocess.run(["mdir", "-i", str(output), guest_path],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if exists.returncode == 0:
            subprocess.run(["mdeltree", "-i", str(output), guest_path], check=True,
                           stdout=subprocess.DEVNULL)
    for guest_path in EXCLUDE_GUEST_FILES:
        exists = subprocess.run(["mdir", "-i", str(output), guest_path],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if exists.returncode == 0:
            subprocess.run(["mdel", "-i", str(output), guest_path], check=True,
                           stdout=subprocess.DEVNULL)
    scrub_free_fat16_clusters(output)


def scrub_free_fat16_clusters(image: Path) -> None:
    """Erase deleted payload bytes left in FAT16 free clusters."""
    with image.open("r+b", buffering=0) as stream:
        boot = stream.read(512)
        bytes_per_sector = struct.unpack_from("<H", boot, 11)[0]
        sectors_per_cluster = boot[13]
        reserved = struct.unpack_from("<H", boot, 14)[0]
        fat_count = boot[16]
        root_entries = struct.unpack_from("<H", boot, 17)[0]
        total = struct.unpack_from("<H", boot, 19)[0] or struct.unpack_from("<I", boot, 32)[0]
        sectors_per_fat = struct.unpack_from("<H", boot, 22)[0]
        root_sectors = (root_entries * 32 + bytes_per_sector - 1) // bytes_per_sector
        data_start = reserved + fat_count * sectors_per_fat + root_sectors
        clusters = (total - data_start) // sectors_per_cluster
        if (boot[510:512] != b"\x55\xaa" or bytes_per_sector != 512
                or sectors_per_cluster == 0 or fat_count != 2
                or sectors_per_fat == 0 or not 4085 <= clusters < 65525
                or total * bytes_per_sector != image.stat().st_size):
            raise SystemExit("[windows-portable] expected a complete FAT16 image")
        stream.seek(reserved * bytes_per_sector)
        fat = stream.read(sectors_per_fat * bytes_per_sector)
        if len(fat) < (clusters + 2) * 2:
            raise SystemExit("[windows-portable] invalid FAT16 size")
        cluster_bytes = sectors_per_cluster * bytes_per_sector
        zeros = bytes(min(1024 * 1024, cluster_bytes * 256))
        free_clusters = 0
        for cluster in range(2, clusters + 2):
            if struct.unpack_from("<H", fat, cluster * 2)[0] != 0:
                continue
            stream.seek((data_start * bytes_per_sector) + (cluster - 2) * cluster_bytes)
            remaining = cluster_bytes
            while remaining:
                part = zeros[:remaining]
                stream.write(part)
                remaining -= len(part)
            free_clusters += 1
    print(f"[windows-portable] scrubbed {free_clusters} free FAT16 clusters")


def qemu_files(tree: Path) -> list[Path]:
    files = [tree / "qemu-system-i386w.exe", tree / "qemu-system-i386.exe",
             tree / "COPYING", tree / "COPYING.LIB", tree / "README.rst",
             tree / "VERSION"]
    files.extend(tree.glob("*.dll"))
    files.extend(path for directory in (tree / "share", tree / "lib")
                 for path in directory.rglob("*") if path.is_file())
    for path in files:
        if not path.is_file():
            raise SystemExit(f"[windows-portable] missing QEMU payload: {path}")
    return sorted(set(files))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path,
                        default=ROOT / "build/full/ciukios-full.img")
    args = parser.parse_args()
    image = args.image.resolve()
    if not image.is_file() or image.stat().st_size < 1024 * 1024:
        raise SystemExit(f"[windows-portable] full image missing or too small: {image}")
    qemu = qemu_tree()
    if digest(qemu / "qemu-system-i386w.exe") != QEMU_EXE_SHA256:
        raise SystemExit("[windows-portable] extracted QEMU executable SHA-256 mismatch")
    source_commit = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    source_dirty = bool(subprocess.check_output(
        ["git", "status", "--porcelain", "--untracked-files=normal"],
        cwd=ROOT, text=True).strip())
    release = RELEASE_DIR / f"{NAME}.zip"
    RELEASE_DIR.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(dir=RELEASE_DIR, prefix="windows-portable-") as work:
        clean_image = Path(work) / "CiukiOS.img"
        guest_copy(image, clean_image)
        manifest = {
            "ciukios_version": "0.8.3",
            "ciukios_image_sha256": digest(clean_image),
            "source_commit": source_commit,
            "source_dirty": source_dirty,
            "qemu_version": QEMU_VERSION,
            "qemu_installer_url": INSTALLER_URL,
            "qemu_installer_sha512": INSTALLER_SHA512,
            "qemu_executable_sha256": digest(qemu / "qemu-system-i386w.exe"),
            "removed_private_guest_paths": list(EXCLUDE_GUEST_DIRS + EXCLUDE_GUEST_FILES),
            "windows_runtime_tested": False,
        }
        tmp_zip = Path(work) / release.name
        with zipfile.ZipFile(tmp_zip, "w", compression=zipfile.ZIP_DEFLATED,
                             compresslevel=1, allowZip64=True) as archive:
            def add(source: Path, destination: str) -> None:
                archive.write(source, f"{NAME}/{destination}")

            add(clean_image, "CiukiOS.img")
            for cmd_name in ("Start-CiukiOS.cmd", "Start-CiukiOS-VGA.cmd", "Start-CiukiOS-VirtIO.cmd"):
                cmd_content = (ROOT / "scripts/windows_portable" / cmd_name).read_bytes()
                archive.writestr(f"{NAME}/{cmd_name}",
                                 cmd_content.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
            add(ROOT / "scripts/windows_portable/README.txt", "README.txt")
            add(ROOT / "LICENSE", "LICENSE.txt")
            add(ROOT / "assets/icons/README.md", "ICON-NOTICES.txt")
            add(ROOT / "assets/drivers/README.md", "DRIVER-NOTICES.txt")
            for path in qemu_files(qemu):
                add(path, "qemu/" + path.relative_to(qemu).as_posix())
            archive.writestr(f"{NAME}/MANIFEST.json",
                             json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        with zipfile.ZipFile(tmp_zip) as archive:
            bad = archive.testzip()
            if bad:
                raise SystemExit(f"[windows-portable] ZIP integrity failed: {bad}")
        os.replace(tmp_zip, release)
    print(f"[windows-portable] ready: {release} ({release.stat().st_size:,} bytes)")
    print(f"[windows-portable] SHA-256: {digest(release)}")


if __name__ == "__main__":
    main()
