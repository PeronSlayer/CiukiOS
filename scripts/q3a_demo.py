#!/usr/bin/env python3
"""Quake III Arena demo installer, vendored whole in third_party/games/q3a-demo-installer.

The Windows installer (MindVision VISE, not unpackable on the host) is installed
unmodified at C:\\Games\\Quake III Arena Demo Setup\\Q3ADemo.exe with a
Start > Programs > Games shortcut (CiukiOS .lnk v3, apps/desktop/shell_link.h).
The image build never runs it. Owner request 2026-10-25."""
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "q3a-demo-installer"
FOLDER = ROOT / "third_party/games" / PACKAGE
INSTALLER_PATH = "/Games/Quake III Arena Demo Setup/Q3ADemo.exe"
SHORTCUT_PATH = "/WINDOWS/Start Menu/Programs/Games/Install Quake III Arena Demo.lnk"
DIRECTORIES = ("/Games", "/Games/Quake III Arena Demo Setup", "/WINDOWS/Start Menu/Programs/Games")
LABEL = "Install Quake III Arena Demo"
TARGET = "C:\\Games\\Quake III Arena Demo Setup\\Q3ADemo.exe"
WORKDIR = "C:\\Games\\Quake III Arena Demo Setup"


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def upstream(root=ROOT):
    return json.loads((Path(root) / "third_party/games" / PACKAGE / "UPSTREAM.json").read_text())


def encode_link(label, target, arguments="", directory="", icon="", show=1, icon_index=0, hotkey=0):
    """Bytes equal to desk_link_encode for the same record."""
    fields = [f.encode("utf-8") for f in (label, target, arguments, directory, icon)]
    out = bytearray(b"CIUKLNK3" + struct.pack("<Ii", show, icon_index))
    out += b"".join(struct.pack("<I", len(f)) for f in fields)
    out += struct.pack("<I", hotkey)
    assert len(out) == 40
    return bytes(out) + b"".join(fields)


def verify(root=ROOT):
    """The pins against the vendored files; raises SystemExit on any difference."""
    root = Path(root)
    record = upstream(root)
    folder = root / "third_party/games" / PACKAGE
    archive = folder / record["archive"]
    if not archive.is_file() or archive.stat().st_size != record["size"] or sha256(archive) != record["sha256"]:
        raise SystemExit("Q3ADemo.exe differs from its pin in UPSTREAM.json")
    for item in record["license_files"]:
        if sha256(folder / item["name"]) != item["sha256"]:
            raise SystemExit(f"{item['name']} differs from its pin in UPSTREAM.json")
    return record


def payloads(out_dir, root=ROOT):
    """(sources, directories, metadata, free hashes). out_dir receives the generated shortcut."""
    root = Path(root)
    record = verify(root)
    folder = root / "third_party/games" / PACKAGE
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    link = out_dir / "Install Quake III Arena Demo.lnk"
    link.write_bytes(encode_link(LABEL, TARGET, directory=WORKDIR))
    sources = {INSTALLER_PATH: folder / record["archive"], SHORTCUT_PATH: link,
               f"/system/licenses/{PACKAGE}-UPSTREAM.json": folder / "UPSTREAM.json"}
    for item in record["license_files"]:
        sources[f"/system/licenses/{PACKAGE}-{item['name']}"] = folder / item["name"]
    metadata = {"q3a_demo": {"package": PACKAGE, "installer": INSTALLER_PATH, "sha256": record["sha256"],
                             "size": record["size"], "shortcut": SHORTCUT_PATH}}
    return sources, set(DIRECTORIES), metadata, {record["sha256"]}
