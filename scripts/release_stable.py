#!/usr/bin/env python3
"""Package an owner-approved canonical image and Setup CD as a stable release, or an alpha/beta pre-release (--channel).

No builds or guest execution. --dry-run performs the audits without network or
release output. See docs/release-process.md for the approval and source contract.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import shlex
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ET
import zipfile

import build_cd
import build_dos_image
import build_games_disk


ROOT = Path(__file__).resolve().parent.parent
REPO = os.environ.get("CIUKIOS_GITHUB_REPO", "PeronSlayer/CiukiOS")
SHA256 = re.compile(r"[0-9a-f]{64}")
COMMIT = re.compile(r"[0-9a-f]{40}")
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
CHANNELS = ("stable", "alpha", "beta")
RELEASE_TAG = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+-(?:stable|alpha|beta)\.[1-9][0-9]*")
STABLE_TAG = RELEASE_TAG
PUBLIC_FORBIDDEN = re.compile(r"\b(?:T23|E500|ThinkPad|Armada)\b|qemu-t23", re.IGNORECASE)
OVF = "http://schemas.dmtf.org/ovf/envelope/1"
RASD = "http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_ResourceAllocationSettingData"
VSSD = "http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_VirtualSystemSettingData"
DISK_FORMAT = "http://www.vmware.com/interfaces/specifications/vmdk.html#streamOptimized"
PRIVATE_ROOTS = ("legacy/local", "third_party/mswin98rskt", "third_party/drivers")
MAX_AUDIT_DEPTH = 6


class Refusal(ValueError):
    """An input cannot be published safely."""


def require(condition: object, message: str) -> None:
    if not condition:
        raise Refusal(message)


def command(*args: str, root: Path = ROOT) -> str:
    result = subprocess.run([str(a) for a in args], cwd=root, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    require(result.returncode == 0,
            f"{shlex.join(str(a) for a in args)} failed:\n{result.stderr.strip()}")
    return result.stdout.strip()


def digest(path: Path, algorithm: str = "sha256") -> str:
    result = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def git_bytes(root: Path, commit: str, path: str) -> bytes:
    result = subprocess.run(["git", "show", f"{commit}:{path}"], cwd=root,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    require(result.returncode == 0, f"missing {path} at source commit {commit}")
    return result.stdout


def git_json(root: Path, commit: str, path: str) -> dict:
    return json.loads(git_bytes(root, commit, path))


def safe_name(name: str) -> str:
    require(isinstance(name, str) and name and not any(ord(c) < 32 for c in name),
            "empty or control-character payload path")
    name = name.replace("\\", "/")
    require(not name.startswith("//") and ":" not in name, f"unsafe path: {name}")
    parts = name.lstrip("/").rstrip("/").split("/")
    require(all(p not in ("", ".", "..") for p in parts), f"unsafe path: {name}")
    lower = "/" + "/".join(parts).casefold() + "/"
    require(not any("/" + p + "/" in lower for p in PRIVATE_ROOTS)
            and "/mswin98rskt/" not in lower, f"private payload path: {name}")
    return "/".join(parts)


def records(rows: list[dict]) -> dict[str, dict]:
    require(isinstance(rows, list) and rows, "missing payload records")
    result = {}
    for row in rows:
        name = safe_name(row["path"]).casefold()
        require(name not in result, f"duplicate payload path: {name}")
        require(isinstance(row.get("sha256"), str) and SHA256.fullmatch(row["sha256"])
                and type(row.get("size")) is int and row["size"] >= 0,
                f"invalid payload record: {name}")
        result[name] = row
    return result


def verify_file(path: Path, row: dict) -> None:
    require(path.is_file() and not path.is_symlink(), f"missing or linked payload: {path.name}")
    require(path.stat().st_size == row["size"] and digest(path) == row["sha256"],
            f"payload hash/size mismatch: {path.name}")


def fat_volume(image: Path) -> str | None:
    with image.open("rb") as stream:
        boot = stream.read(512)
        if len(boot) != 512 or boot[510:] != b"\x55\xaa":
            return None
        if boot[0] not in (0xEB, 0xE9) or struct.unpack_from("<H", boot, 11)[0] != 512:
            if boot[450] not in (1, 4, 6, 0x0B, 0x0C, 0x0E):
                return None
            offset = struct.unpack_from("<I", boot, 454)[0] * 512
            require(offset >= 512, "invalid FAT partition offset")
            stream.seek(offset)
            boot = stream.read(512)
        else:
            offset = 0
    require(len(boot) == 512 and struct.unpack_from("<H", boot, 11)[0] == 512
            and boot[13] in (1, 2, 4, 8, 16, 32, 64, 128), "invalid FAT BPB")
    return str(image) + (f"@@{offset}" if offset else "")


class PayloadAudit:
    """Read live payloads, including nested FAT/ZIP/tar containers, fail closed.

    Hash private files lazily by matching size. Short generic files are excluded
    from the content guard, as in build_dos_image; reserved names still apply.
    Scratch storage lives on disk, never in the host's tmpfs.
    """

    def __init__(self, root: Path, private_roots: list[str], free: set[str]):
        self.root = root
        self.free = free
        self.private: dict[int, list[Path]] = {}
        self.private_digests: dict[int, set[str]] = {}
        for relative in set(PRIVATE_ROOTS + build_dos_image.LOCAL_PAYLOAD_DIRS + tuple(private_roots)):
            require(not Path(relative).is_absolute() and ".." not in Path(relative).parts,
                    "private roots must be repository-relative")
            folder = root / relative
            for path in folder.rglob("*") if folder.is_dir() else ():
                if path.is_file() and not path.is_symlink() and path.stat().st_size >= 64:
                    self.private.setdefault(path.stat().st_size, []).append(path)

    def file(self, path: Path, name: str, scratch: Path, depth: int = 0) -> None:
        require(depth <= MAX_AUDIT_DEPTH, "payload nesting limit exceeded")
        name = safe_name(name)
        base = PurePosixPath(name).name.upper()
        sha = digest(path)
        require(not build_dos_image.REGISTERED_DATA.fullmatch(base),
                f"commercial game data: {name}")
        require(base not in build_dos_image.LOCAL_PAYLOAD_NAMES or sha in self.free,
                f"unpinned game payload: {name}")
        size = path.stat().st_size
        if size in self.private:
            if size not in self.private_digests:
                self.private_digests[size] = {digest(p) for p in self.private[size]}
            require(sha not in self.private_digests[size] or sha in self.free,
                    f"private payload content: {name}")
        if fat_volume(path) is not None:
            self.fat(path, scratch, depth=depth + 1)
        elif zipfile.is_zipfile(path):
            with zipfile.ZipFile(path) as archive:
                names = set()
                for entry in archive.infolist():
                    child = safe_name(entry.filename).casefold()
                    require(child not in names, "duplicate archive member")
                    names.add(child)
                    require(not stat.S_ISLNK(entry.external_attr >> 16), "archive symlink refused")
                    if not entry.is_dir():
                        with archive.open(entry) as source:
                            self.member(source, entry.filename, scratch, depth + 1)
        elif name.lower().endswith((".tar", ".tar.gz", ".tgz", ".tar.xz", ".txz", ".tar.bz2")):
            with tarfile.open(path, "r|*") as archive:
                names = set()
                for entry in archive:
                    # Upstream tarballs commonly prefix names with './'.
                    child = entry.name.removeprefix("./")
                    if child in ("", ".") and entry.isdir():
                        continue
                    safe_name(child)
                    require(child not in names, "duplicate tar member")
                    names.add(child)
                    require(entry.isfile() or entry.isdir() or entry.issym() or entry.islnk(),
                            "special tar member refused")
                    if entry.issym() or entry.islnk():
                        target = posixpath.normpath(posixpath.join(
                            str(PurePosixPath(child).parent) if entry.issym() else "", entry.linkname))
                        require(not target.startswith(("/", "../")) and target != "..",
                                "unsafe source tar link")
                        safe_name(target)
                    elif entry.isfile():
                        with archive.extractfile(entry) as source:
                            self.member(source, child, scratch, depth + 1)

    def member(self, source, name: str, scratch: Path, depth: int) -> None:
        with tempfile.TemporaryDirectory(dir=scratch, prefix="member-") as temporary:
            path = Path(temporary) / "payload"
            with path.open("wb") as dest:
                shutil.copyfileobj(source, dest, 1024 * 1024)
            self.file(path, name, scratch, depth)

    def fat(self, image: Path, scratch: Path, expected: list[dict] | None = None,
            depth: int = 0) -> dict[str, dict]:
        require(depth <= MAX_AUDIT_DEPTH, "FAT nesting limit exceeded")
        volume = fat_volume(image)
        require(volume is not None, "payload is not a FAT image")
        listing = command("mdir", "-b", "-s", "-i", volume, "::/", root=self.root)
        found = {}
        for line in listing.splitlines():
            require(line.startswith("::/"), "unrecognised mtools listing")
            name = safe_name(line[3:])
            if line.endswith("/"):
                continue
            key = name.casefold()
            require(key not in found, "duplicate FAT file")
            with tempfile.TemporaryDirectory(dir=scratch, prefix="fat-") as temporary:
                path = Path(temporary) / "payload"
                command("mcopy", "-i", volume, "::/" + name, str(path), root=self.root)
                found[key] = {"path": "/" + name, "size": path.stat().st_size, "sha256": digest(path)}
                self.file(path, name, scratch, depth)
        if expected is not None:
            wanted = records(expected)
            require(found.keys() == wanted.keys(), "FAT files differ from manifest (missing/unlisted files)")
            for name, row in found.items():
                require(all(row[k] == wanted[name][k] for k in ("size", "sha256")),
                        f"FAT payload differs from manifest: {name}")
        return found


def source_files(root: Path, commit: str, includes: list[str]) -> dict[str, tuple[str, str]]:
    result = subprocess.run(["git", "ls-tree", "-rz", commit, "--", *includes], cwd=root,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    require(result.returncode == 0, "cannot read corresponding-source Git tree")
    files = {}
    for row in result.stdout.split(b"\0"):
        if not row:
            continue
        meta, name = row.split(b"\t", 1)
        mode, kind, oid = meta.decode().split()
        require(kind == "blob" and mode in ("100644", "100755"), "unsupported source Git entry")
        files["ciukios/" + name.decode()] = (mode, oid)
    require(files, "empty corresponding-source Git tree")
    return files


def verify_source_zip(path: Path, root: Path, commit: str, config: dict) -> None:
    wanted = source_files(root, commit, config["source_tree"]["include"])
    with zipfile.ZipFile(path) as archive:
        require(set(archive.namelist()) == wanted.keys() and len(archive.infolist()) == len(wanted),
                "source ZIP entries differ from built commit")
        for entry in archive.infolist():
            mode, oid = wanted[entry.filename]
            require(entry.external_attr >> 16 == int(mode, 8), "source ZIP mode differs from Git")
            blob = hashlib.sha1(f"blob {entry.file_size}\0".encode())
            with archive.open(entry) as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    blob.update(block)
            require(blob.hexdigest() == oid, f"source ZIP differs from built commit: {entry.filename}")
        # The release policy specifically requires the whole Wine recipe, pins and patches.
        for prefix in ("ciukios/scripts/win32/", "ciukios/third_party/wine/patches/"):
            require(any(n.startswith(prefix) for n in wanted), f"missing corresponding source: {prefix}")
        for name in ("config/wine-pin.json", "config/wine-upper.json", "third_party/wine/SOURCE-OFFER.txt"):
            require("ciukios/" + name in wanted, f"missing corresponding source: {name}")


def clean(value: object) -> bool:
    return value is False or (type(value) is int and value == 0)


def inputs(root: Path, version: str, approved_sha: str, channel: str = "stable") -> dict:
    require(channel in CHANNELS, "channel must be stable, alpha or beta")
    require(VERSION.fullmatch(version), "version must be MAJOR.MINOR.PATCH")
    require(SHA256.fullmatch(approved_sha), "approval must name a full lowercase SHA-256")
    policy = json.loads((root / "config/release-policy.json").read_text())["stable"]
    require(policy.get("requires_owner_approval") is True and policy.get("owner_approved") is True
            and isinstance(policy.get("approval_record"), str) and policy["approval_record"].strip(),
            "explicit owner approval record required")
    require(policy.get("approved_image_sha256") == approved_sha, "policy approval SHA-256 mismatch")
    directory = root / "build/f0"
    image, iso = directory / "ciukios.img", directory / "ciukios-setup.iso"
    disk_path, cd_path = directory / "build-manifest.json", directory / "cd-manifest.json"
    disk, cd = json.loads(disk_path.read_text()), json.loads(cd_path.read_text())
    commit = disk.get("git_revision", "")
    require(isinstance(commit, str) and COMMIT.fullmatch(commit), "invalid source commit")
    require(policy.get("source_commit") == commit, "policy source commit mismatch")
    build = cd["build"]
    require(build.get("git_revision") == commit and build.get("source_revision") == commit
            and build.get("source_matches_build") is True, "disk/CD source commit mismatch")
    require(clean(disk.get("git_dirty")) and clean(build.get("git_dirty")), "dirty or unknown build source")
    require(disk.get("schema_version") == 1 and cd.get("schema_version") == 1, "unsupported manifest schema")
    require(disk.get("image_sha256") == approved_sha and build.get("disk_image_sha256") == approved_sha,
            "manifest image approval mismatch")
    require(disk.get("version") == build.get("version"), "disk/CD version mismatch")
    identity = git_json(root, commit, "config/version.json")
    require(identity["version"] == version and disk["version"] == f"{version} {identity['stage']}",
            "release version differs from built source")
    if channel != "stable":
        require(identity["stage"] == channel, f"source stage {identity['stage']!r} is not the {channel} channel")
    verify_file(image, {"sha256": approved_sha, "size": image.stat().st_size})
    verify_file(iso, {"sha256": cd["iso"]["sha256"], "size": cd["iso"]["bytes"]})
    config = git_json(root, commit, "config/cd-components.json")
    changelog = git_bytes(root, commit, "CHANGELOG.md").decode()
    section = changelog_section(changelog, version, None if channel == "stable" else channel)
    channel_notes = ""
    if channel in ("alpha", "beta"):
        channel_notes = git_bytes(root, commit, f"docs/release-notes-1.0-{channel}.md").decode()
        require(channel_notes.strip(), f"empty {channel} release notes")
    return {"root": root, "image": image, "iso": iso, "disk": disk, "cd": cd,
            "disk_manifest": disk_path, "cd_manifest": cd_path, "commit": commit,
            "version": version, "sha256": approved_sha, "config": config,
            "policy": policy, "changelog": section, "channel": channel,
            "channel_notes": channel_notes}


def changelog_section(changelog: str, version: str, stage: str | None = None) -> str:
    # One version can carry an alpha and a beta section (SemVer pre-releases of the same version).
    sections = re.split(r"(?m)^(?=## )", changelog)
    tail = r"\]?\s+" + re.escape(stage) + r"\b" if stage else r"(?=[\]\s(]|$)"
    for section in sections:
        heading = section.splitlines()[0] if section else ""
        if re.match(r"## \[?" + re.escape(version) + tail, heading):
            return section.strip()
    raise Refusal(f"CHANGELOG has no section for {version}{' ' + stage if stage else ''}")


def shareware_hashes(root: Path, commit: str) -> set[str]:
    """Only the built revision's pins may exempt common shareware file names."""
    hashes = set()
    for package in build_games_disk.PACKAGES:
        record = git_json(root, commit, f"{build_games_disk.GAMES}/{package}/UPSTREAM.json")
        hashes.update(row["sha256"] for row in record["install"]["files"])
        hashes.update(row["sha256"] for row in record["license_files"])
    require(all(isinstance(sha, str) and SHA256.fullmatch(sha) for sha in hashes),
            "invalid shareware member pin")
    return hashes


def audit_inputs(info: dict, scratch: Path) -> tuple[Path, list[Path], list[Path]]:
    root, cd, disk, config = (info[k] for k in ("root", "cd", "disk", "config"))
    free = shareware_hashes(root, info["commit"])
    audit = PayloadAudit(root, info["policy"].get("private_roots", []), free)
    audit.fat(info["image"], scratch, disk["payloads"])
    tree = scratch / "iso"
    command("xorriso", "-osirrox", "on", "-indev", str(info["iso"]), "-extract", "/", str(tree), root=root)
    expected = records(cd["iso_tree"])
    found = {}
    for path in tree.rglob("*"):
        name = safe_name(path.relative_to(tree).as_posix())
        require(not path.is_symlink(), "ISO symlink refused")
        if path.is_file():
            require(name.casefold() not in found, "duplicate ISO path")
            found[name.casefold()] = path
    require(found.keys() == expected.keys(), "ISO files differ from manifest (missing/unlisted files)")
    for name, path in found.items():
        verify_file(path, expected[name])
        audit.file(path, name, scratch)
    boot = found[safe_name(cd["boot_image"]["iso_path"]).casefold()]
    audit.fat(boot, scratch, cd["boot_image"]["files"])
    # Enforce the component catalogue recorded at the built commit for both media.
    try:
        packages = [p["name"] for p in disk.get("games", {}).get("packages", [])]
        _, _, disk_required = build_cd.eligibility(config, [r["path"] for r in disk["payloads"]], packages)
        _, present, cd_required = build_cd.eligibility(config, [r["path"] for r in cd["boot_image"]["files"]], packages)
    except SystemExit as error:
        raise Refusal(str(error)) from error
    require(set(cd["components"]["present"]) == set(present), "CD component catalogue differs from source")
    sources = []
    for name in sorted(set(disk_required + cd_required)):
        spec = config["archives"][name]
        row = cd["archives"][name]
        path = found[("sources/" + name).casefold()]
        verify_file(path, row)
        if spec["kind"] == "source-tree":
            verify_source_zip(path, root, info["commit"], config)
        elif spec["kind"] == "pinned":
            pin = git_json(root, info["commit"], spec["pin"])
            for key in spec["key"].split("."):
                pin = pin[key]
            require(digest(path) == pin, f"source archive pin mismatch: {name}")
        elif spec["kind"] == "produced":
            require(digest(path) == build_cd.manifest_value(disk, spec["manifest_key"]),
                    f"produced source hash mismatch: {name}")
        else:
            raise Refusal(f"unknown source archive kind: {name}")
        sources.append(path)
    require(any(p.name.upper() == "WINE100.TXZ" for p in sources), "missing pinned Wine corresponding source")
    notices = [p for name, p in found.items() if name.startswith("licenses/")]
    require(notices and "licenses/index.txt" in found and "licenses/sources.txt" in found,
            "missing CD license texts/index")
    # build_cd names repo notices sequentially. Verify their bytes, not a label.
    notice_hashes = {digest(p) for p in notices}
    for component in config["components"]:
        if component["id"] in present:
            for name in component.get("licence_files", []):
                require(hashlib.sha256(git_bytes(root, info["commit"], name)).hexdigest() in notice_hashes,
                        f"missing component notice: {name}")
    # Every installed license must also accompany the separate disk packages.
    for row in disk["payloads"]:
        if row["path"].casefold().startswith("/system/licenses/"):
            require(row["sha256"] in notice_hashes, f"missing installed notice: {row['path']}")
    return tree, sources, notices


def next_number(version: str, tags: list[str], channel: str = "stable") -> int:
    pattern = re.compile(r"v" + re.escape(version) + "-" + channel + r"\.([1-9][0-9]*)")
    numbers = {int(m[1]) for tag in tags if (m := pattern.fullmatch(tag))}
    return max(numbers, default=0) + 1


def release_tags(root: Path, version: str, remote: bool, channel: str = "stable") -> list[str]:
    tags = command("git", "tag", "--list", f"v{version}-{channel}.*", root=root).splitlines()
    if remote:
        refs = json.loads(command("gh", "api", f"repos/{REPO}/git/matching-refs/tags/v{version}-{channel}.", root=root))
        tags += [r["ref"].removeprefix("refs/tags/") for r in refs]
    return tags


def artifacts(info: dict) -> list[str]:
    stem = "ciukios-" + info["version"]
    return [stem + suffix for suffix in ("-disk.zip", "-setup.iso", "-qemu.zip", "-vm.ova",
                                        "-vmware.zip", "-virtualbox.zip", "-sources.zip", "-notices.zip")] + [
        "build-manifest.json", "cd-manifest.json", "RELEASE-NOTES.md", "SHA256SUMS"]


def conversion_commands(image: Path, directory: Path) -> list[list[str]]:
    return [["qemu-img", "convert", "-f", "raw", "-O", fmt, *(["-o", options] if options else []),
             str(image), str(directory / name)]
            for fmt, options, name in (("qcow2", "compat=1.1", "ciukios.qcow2"),
                                       ("vdi", "", "ciukios.vdi"),
                                       ("vmdk", "subformat=streamOptimized,adapter_type=ide", "ciukios-stream.vmdk"),
                                       ("vmdk", "subformat=monolithicSparse,adapter_type=ide", "ciukios.vmdk"))]


def convert_disks(image: Path, directory: Path, root: Path) -> None:
    for args in conversion_commands(image, directory):
        command(*args, root=root)
        target = Path(args[-1])
        fmt = args[args.index("-O") + 1]
        record = json.loads(command("qemu-img", "info", "--output=json", "-f", fmt, str(target), root=root))
        require(record["format"] == fmt and record["virtual-size"] == image.stat().st_size
                and not record.get("backing-filename"), "converted disk format/size/backing mismatch")
        if fmt == "vmdk":
            expected = "streamOptimized" if "stream" in target.name else "monolithicSparse"
            require(record["format-specific"]["data"]["create-type"] == expected, "wrong VMDK subformat")
        command("qemu-img", "compare", "-f", "raw", "-F", fmt, str(image), str(target), root=root)


def qemu_launchers() -> tuple[str, str]:
    options = ('-machine pc-i440fx-9.2 -cpu pentium3 -m 512 -smp 1 -vga std '
               '-drive file=ciukios.qcow2,format=qcow2,if=ide,index=0 '
               '-device AC97 -nic user,model=e1000 -rtc base=localtime -boot c')
    shell = ('#!/bin/sh\nset -eu\ncd -- "$(dirname -- "$0")"\n'
             'QEMU=${QEMU:-qemu-system-x86_64}\naccel=tcg\n'
             'available=$("$QEMU" -accel help)\n'
             'case "$(uname -s):$(uname -m)" in\n'
             '  Linux:x86_64|Linux:i?86)\n'
             '    if [ -r /dev/kvm ] && [ -w /dev/kvm ] && '
             'echo "$available" | grep -qw kvm; then accel=kvm; fi ;;\n'
             '  Darwin:x86_64)\n'
             '    if echo "$available" | grep -qw hvf; then accel=hvf; fi ;;\n'
             'esac\n'
             f'"$QEMU" -accel "$accel" {options} "$@" && exit 0\n'
             '[ "$accel" = tcg ] && exit 1\n'
             'echo "Acceleration unavailable; retrying with TCG." >&2\n'
             f'exec "$QEMU" -accel tcg {options} "$@"\n')
    cmd = ('@echo off\nsetlocal\ncd /d "%~dp0"\n'
           'if not defined QEMU set "QEMU=qemu-system-x86_64.exe"\n'
           'set "ACCEL=tcg"\n'
           '"%QEMU%" -accel help 2>nul | findstr /x /c:"whpx" >nul\n'
           'if not errorlevel 1 set "ACCEL=whpx"\n'
           f'"%QEMU%" -accel %ACCEL% {options} %*\n'
           'if not errorlevel 1 exit /b 0\n'
           'if "%ACCEL%"=="tcg" exit /b 1\n'
           'echo Acceleration unavailable; retrying with TCG.\n'
           f'"%QEMU%" -accel tcg {options} %*\nexit /b %errorlevel%\n')
    return shell, cmd.replace("\n", "\r\n")


def ovf_descriptor(disk: Path, capacity: int, version: str) -> bytes:
    for prefix, uri in (("ovf", OVF), ("rasd", RASD), ("vssd", VSSD)):
        ET.register_namespace(prefix, uri)
    def child(parent, tag, text=None, **attrs):
        node = ET.SubElement(parent, f"{{{OVF}}}{tag}", {f"{{{OVF}}}{k}": str(v) for k, v in attrs.items()})
        node.text = text
        return node
    envelope = ET.Element(f"{{{OVF}}}Envelope")
    refs = child(envelope, "References")
    child(refs, "File", id="file1", href=disk.name, size=disk.stat().st_size)
    disks = child(envelope, "DiskSection")
    child(disks, "Info", "Virtual disk")
    child(disks, "Disk", diskId="disk1", fileRef="file1", capacity=capacity,
          capacityAllocationUnits="byte", format=DISK_FORMAT)
    networks = child(envelope, "NetworkSection")
    child(networks, "Info", "Logical network (select NAT on import)")
    child(child(networks, "Network", name="NAT"), "Description", "NAT")
    system = child(envelope, "VirtualSystem", id="CiukiOS")
    child(system, "Info", "CiukiOS " + version)
    child(system, "Name", "CiukiOS " + version)
    os_section = child(system, "OperatingSystemSection", id=1)
    child(os_section, "Info", "Other operating system")
    hardware = child(system, "VirtualHardwareSection")
    child(hardware, "Info", "512 MiB, one CPU, IDE PIIX4, ES1371 (VirtualBox AC97), E1000, VGA")
    spec = child(hardware, "System")
    for name, text in (("ElementName", "Virtual Hardware Family"), ("InstanceID", "0"),
                       ("VirtualSystemIdentifier", "CiukiOS"), ("VirtualSystemType", "vmx-07")):
        ET.SubElement(spec, f"{{{VSSD}}}{name}").text = text
    def item(**values):
        node = child(hardware, "Item")
        for name, text in sorted(values.items()):
            ET.SubElement(node, f"{{{RASD}}}{name}").text = str(text)
    item(ElementName="1 virtual CPU", InstanceID=1, ResourceType=3, VirtualQuantity=1)
    item(AllocationUnits="byte * 2^20", ElementName="512 MiB RAM", InstanceID=2, ResourceType=4, VirtualQuantity=512)
    item(Address=0, ElementName="IDE controller", InstanceID=3, ResourceSubType="PIIX4", ResourceType=5)
    item(AddressOnParent=0, ElementName="System disk", HostResource="ovf:/disk/disk1", InstanceID=4, Parent=3, ResourceType=17)
    item(AutomaticAllocation="true", Connection="NAT", ElementName="E1000 network", InstanceID=5, ResourceSubType="E1000", ResourceType=10)
    item(AutomaticAllocation="true", ElementName="ES1371 audio (VirtualBox AC97)", InstanceID=6, ResourceSubType="ensoniq.es1371", ResourceType=35)
    # VirtualBox maps a simple OVF sound request to AC97 (ApplianceImplImport).
    # The VMware VMX below explicitly chooses ES1371.
    item(ElementName="VGA display", InstanceID=7, ResourceType=24)
    ET.indent(envelope)
    return ET.tostring(envelope, encoding="utf-8", xml_declaration=True)


def vmx() -> str:
    return ('config.version = "8"\nvirtualHW.version = "7"\n'
            'displayName = "CiukiOS"\nguestOS = "other"\nfirmware = "bios"\n'
            'memsize = "512"\nnumvcpus = "1"\n'
            'ide0:0.present = "TRUE"\nide0:0.fileName = "ciukios.vmdk"\n'
            'ethernet0.present = "TRUE"\nethernet0.virtualDev = "e1000"\n'
            'ethernet0.connectionType = "nat"\nethernet0.addressType = "generated"\n'
            'sound.present = "TRUE"\nsound.virtualDev = "es1371"\n'
            'svga.present = "TRUE"\nrtc.diffFromUTC = "0"\n')


VBOX_CREATE = [
    'VBoxManage createvm --name CiukiOS --ostype Other --register',
    'VBoxManage modifyvm CiukiOS --memory 512 --vram 16 --cpus 1 --ioapic off --firmware bios '
    '--graphicscontroller vboxvga --nic1 nat --nictype1 Am79C973 --audiocontroller ac97 --audiocodec ad1980 '
    '--rtcuseutc off --boot1 disk --boot2 none --boot3 none --boot4 none',
    'VBoxManage storagectl CiukiOS --name IDE --add ide --controller PIIX4',
    'VBoxManage storageattach CiukiOS --storagectl IDE --port 0 --device 0 --type hdd --medium ciukios.vdi',
]


def vbox_scripts() -> tuple[str, str]:
    return ("#!/bin/sh\n# Creates the CiukiOS VM in VirtualBox from ciukios.vdi next to this script.\n"
            'set -e\ncd "$(dirname "$0")"\n' + "\n".join(VBOX_CREATE) + "\n",
            "@echo off\r\ncd /d \"%~dp0\"\r\nif defined VBOX_MSI_INSTALL_PATH (set VBM=\"%VBOX_MSI_INSTALL_PATH%VBoxManage.exe\") "
            "else set VBM=VBoxManage\r\n"
            + "".join(line.replace("VBoxManage", "%VBM%", 1) + "\r\n" for line in VBOX_CREATE))


VBOX_README = """CiukiOS for VirtualBox (VDI disk)

Fastest: run create-vm.sh (Linux, macOS) or create-vm.cmd (Windows) in this
folder. It needs VBoxManage and creates a VM named CiukiOS using ciukios.vdi.
Then enable audio for your host in Settings > Audio (the host driver name
differs between VirtualBox versions) and start the VM.

By hand, create a VM of type Other/Other, then set:
  System: 512 MB, one CPU, I/O APIC off, EFI off
  Display: VBoxVGA graphics controller, 16 MB video memory
  Storage: IDE controller PIIX4, ciukios.vdi attached as the primary master
  Audio: Intel AC97 controller (codec AD1980)
  Network: NAT, adapter type PCnet-FAST III (Am79C973)
  System clock: local time (do not set hardware clock to UTC)
Do not enable 3D acceleration. Alternatively import ciukios-<ver>-vm.ova.
"""


def zip_bundle(output: Path, files: list[tuple[Path, str]], texts: dict[str, str] | None = None) -> None:
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True) as archive:
        for path, name in files:
            archive.write(path, name)
        for name, text in (texts or {}).items():
            entry = zipfile.ZipInfo(name)
            entry.create_system = 3
            entry.external_attr = (0o100755 if name.endswith(".sh") else 0o100644) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, text)
    with zipfile.ZipFile(output) as archive:
        require(archive.testzip() is None, f"ZIP integrity failed: {output.name}")


def make_ova(directory: Path, output: Path, capacity: int, version: str) -> None:
    disk = directory / "ciukios-stream.vmdk"
    descriptor = directory / "ciukios.ovf"
    descriptor.write_bytes(ovf_descriptor(disk, capacity, version))
    manifest = directory / "ciukios.mf"
    manifest.write_text("".join(f"SHA1({p.name})= {digest(p, 'sha1')}\n" for p in (descriptor, disk)), encoding="ascii")
    with tarfile.open(output, "w", format=tarfile.USTAR_FORMAT) as archive:
        for path in (descriptor, manifest, disk):
            archive.add(path, arcname=path.name, recursive=False)


DISK_README = """CiukiOS raw disk image

Extract ciukios.img. Writing it erases the entire selected destination disk.
Use Rufus in DD image mode, or balenaEtcher (select image, whole disk, flash).
On Linux, after unmounting the destination partitions:
  sudo dd if=ciukios.img of=/dev/sdX bs=4M conv=fsync status=progress
Replace /dev/sdX with the intended whole disk, never a partition or host disk.

Targets: late-1990s and early-2000s desktops and laptops (1998-2004 BIOS PCs)
with an i686 CMOV CPU, E820, VBE 2.0+ linear framebuffer, PS/2 input, legacy parallel ATA
and FAT32. Current reference memory is at least 128 MiB; VM packages use 512.
Pentium MMX/K6, UEFI-only and AHCI-only systems are outside this baseline.
See docs/design/hardware-baseline.md at the release source commit for scope
and the qualification status of each hardware class. Owner approval applies to the named disk hash;
it does not establish qualification of every machine or emulator.
"""

VM_README = """CiukiOS virtual machines

QEMU: install QEMU with the pc-i440fx-9.2 machine and pentium3 CPU model;
extract the ZIP and run ./run-qemu.sh or run-qemu.cmd. QEMU may be set to
the executable path. The launchers prefer KVM (Linux), WHPX (Windows), or
HVF (Intel macOS) and retry with TCG when acceleration cannot start.
Apple Silicon uses x86 TCG. AC97, E1000 NAT, local RTC, IDE, standard VGA,
512 MiB and one CPU follow the reference virtual profile. No emulator is bundled.

VirtualBox (OVA route): import the OVA, select NAT, PIIX4 IDE, ICH AC97 and standard VGA
(VBoxVGA), disable EFI, and retain 512 MiB and one CPU. OVF importers may
map device hints differently; inspect the imported settings before starting.
VirtualBox: extract the VirtualBox ZIP (VDI disk) and run create-vm.sh or
create-vm.cmd, or follow its README. IDE PIIX4, ICH AC97, PCnet-FAST III,
VBoxVGA, 512 MiB, I/O APIC off. The OVA is an alternative import route.
VMware: extract the VMware ZIP and open ciukios.vmx. It explicitly selects
IDE, ES1371 sound, E1000 NAT and VMware SVGA. The runnable VMDK is
monolithicSparse; the OVA's streamOptimized VMDK is an import transport.

These packages have host conversion/integrity checks. Windows/macOS launch
and VirtualBox/VMware runtime qualification must be recorded separately.
"""


def notes(info: dict) -> str:
    return (f"# {info['title']}\n\nSource commit: `{info['commit']}`\n\n"
            f"Approved disk SHA-256: `{info['sha256']}`\n\n"
            f"Owner approval: {info['policy']['approval_record']}\n\n"
            + ("This is a pre-release (" + info["channel"] + " channel): expect rough edges; see the known issues below.\n\n"
             if info.get("channel", "stable") != "stable" else "")
            + (info["channel_notes"].rstrip() + "\n\n" if info.get("channel_notes") else "")
            + "## Try this release\n\n"
            "Verify SHA256SUMS before use. Extract the disk ZIP for Rufus (DD mode), "
            "balenaEtcher or dd; writing it erases the destination disk. "
            "The Setup ISO is an El Torito hard-disk-emulation CD, not a hybrid USB image; "
            "burn it to a CD or attach it as an IDE CD-ROM. Use the raw disk for USB writing.\n\n"
            + VM_README + "\nCorresponding sources and component notices accompany this release "
            "in the sources and notices ZIPs and on the Setup CD.\n\n"
            "## Hardware scope\n\n" + DISK_README + "\n## Changelog\n\n" + info["changelog"] + "\n")


def package(info: dict, directory: Path, scratch: Path, sources: list[Path], notices: list[Path]) -> None:
    version, root = info["version"], info["root"]
    stem = "ciukios-" + version
    convert_disks(info["image"], scratch, root)
    zip_bundle(directory / (stem + "-disk.zip"), [(info["image"], "ciukios.img")], {"README.txt": DISK_README})
    shutil.copyfile(info["iso"], directory / (stem + "-setup.iso"))
    shell, cmd = qemu_launchers()
    zip_bundle(directory / (stem + "-qemu.zip"), [(scratch / "ciukios.qcow2", "ciukios.qcow2")],
               {"run-qemu.sh": shell, "run-qemu.cmd": cmd, "README.txt": VM_README})
    make_ova(scratch, directory / (stem + "-vm.ova"), info["image"].stat().st_size, version)
    zip_bundle(directory / (stem + "-vmware.zip"), [(scratch / "ciukios.vmdk", "ciukios.vmdk")],
               {"ciukios.vmx": vmx(), "README.txt": VM_README})
    shell_vbox, cmd_vbox = vbox_scripts()
    zip_bundle(directory / (stem + "-virtualbox.zip"), [(scratch / "ciukios.vdi", "ciukios.vdi")],
               {"create-vm.sh": shell_vbox, "create-vm.cmd": cmd_vbox, "README.txt": VBOX_README})
    zip_bundle(directory / (stem + "-sources.zip"), [(p, "SOURCES/" + p.name) for p in sources])
    zip_bundle(directory / (stem + "-notices.zip"), [(p, "LICENSES/" + p.name) for p in notices])
    shutil.copyfile(info["disk_manifest"], directory / "build-manifest.json")
    shutil.copyfile(info["cd_manifest"], directory / "cd-manifest.json")
    require(json.loads((directory / "build-manifest.json").read_text()) == info["disk"]
            and json.loads((directory / "cd-manifest.json").read_text()) == info["cd"],
            "input manifests changed during packaging")
    (directory / "RELEASE-NOTES.md").write_text(notes(info), encoding="utf-8")
    # Detect input changes during packaging before finalising or publishing.
    require(digest(info["image"]) == info["sha256"] and digest(info["iso"]) == info["cd"]["iso"]["sha256"],
            "release input changed during packaging")
    for text in (DISK_README, VM_README, (directory / "RELEASE-NOTES.md").read_text(encoding="utf-8")):
        found = PUBLIC_FORBIDDEN.search(text)
        require(not found, f"public text names a specific PC model: {found and found[0]}")
    sums = "".join(f"{digest(p)}  {p.name}\n" for p in sorted(directory.iterdir()))
    (directory / "SHA256SUMS").write_text(sums, encoding="ascii")


def publication_command(info: dict, directory: Path) -> list[str]:
    return ["gh", "release", "create", info["tag"],
            *(str(directory / name) for name in artifacts(info)),
            "--target", info["commit"], "--title", info["title"],
            "--notes-file", str(directory / "RELEASE-NOTES.md"), "-R", REPO,
            *(["--prerelease"] if info.get("channel", "stable") != "stable" else [])]


def release_title(version: str, channel: str, number: int) -> str:
    if channel == "stable":
        return f"CiukiOS {version} \u2014 stable {number}"
    return f"CiukiOS {version} {channel}" + (f" {number}" if number > 1 else "")


@contextmanager
def release_lock(root: Path):
    directory = root / "build/codex"
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / "stable-release.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise Refusal("another stable release is running") from error
        yield


def release(root: Path, version: str, approved_sha: str, dry_run: bool, channel: str = "stable") -> Path:
    with release_lock(root):
        info = inputs(root, version, approved_sha, channel)
        number = next_number(version, release_tags(root, version, not dry_run, channel), channel)
        info.update(tag=f"v{version}-{channel}.{number}", title=release_title(version, channel, number))
        releases = root / "build/releases"
        destination = releases / info["tag"]
        require(not destination.exists(), "release destination already exists; inspect it before retrying")
        old = list(releases.iterdir()) if releases.exists() else []
        require(all(p.is_dir() and not p.is_symlink() and RELEASE_TAG.fullmatch(p.name) for p in old),
                "build/releases contains unrelated outputs; relocate them before release")
        scratch_parent = root / "build/codex"
        with tempfile.TemporaryDirectory(dir=scratch_parent, prefix="stable-audit-") as temporary:
            scratch = Path(temporary)
            _, sources, notices = audit_inputs(info, scratch)
            print(json.dumps({"tag": info["tag"], "title": info["title"], "source_commit": info["commit"],
                              "approved_image_sha256": approved_sha, "artifacts": artifacts(info),
                              "sources": [p.name for p in sources], "notices": [p.name for p in notices],
                              "remove_previous": [p.name for p in old]}, indent=2))
            for args in conversion_commands(info["image"], scratch):
                print(shlex.join(args))
            print(shlex.join(publication_command(info, destination)))
            if dry_run:
                print("[stable-release] dry run: audits passed; no network or release output")
                return destination
            releases.mkdir(parents=True, exist_ok=True)
            staging = scratch / "release"
            staging.mkdir()
            package(info, staging, scratch, sources, notices)
            staging.rename(destination)
        # Preparation succeeded; retain just this release, even if upload fails.
        for previous in old:
            shutil.rmtree(previous)
        command(*publication_command(info, destination), root=root)
        print(f"[stable-release] published {info['tag']}: {destination}")
        return destination


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--approved-image-sha256", required=True)
    parser.add_argument("--channel", choices=CHANNELS, default="stable",
                        help="stable (default), or alpha/beta: tag v<version>-<channel>.<n>, GitHub prerelease")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    try:
        release(ROOT, args.version, args.approved_image_sha256, args.dry_run, args.channel)
    except (Refusal, OSError, KeyError, TypeError, ValueError, zipfile.BadZipFile, tarfile.TarError) as error:
        raise SystemExit(f"[stable-release] refused: {error}") from error


if __name__ == "__main__":
    main()
