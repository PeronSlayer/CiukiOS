#!/usr/bin/env python3
"""Quake III Arena demo installer, vendored whole in third_party/games/q3a-demo-installer.

The Windows installer (MindVision VISE, not unpackable on the host) is installed
unmodified at C:\\Games\\Quake III Arena Demo Setup\\Q3ADemo.exe. The Programs > Games
entry "Install Quake III Arena Demo" is a built-in menu record (apps/desktop/shell.c
programs[], name q3demo), not a Start Menu folder tree: the image carries no
/WINDOWS/Start Menu/Programs folder, which belongs to the installers that create it.
The image build never runs the installer. Owner request 2026-10-25."""
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "q3a-demo-installer"
FOLDER = ROOT / "third_party/games" / PACKAGE
INSTALLER_PATH = "/Games/Quake III Arena Demo Setup/Q3ADemo.exe"
DIRECTORIES = ("/Games", "/Games/Quake III Arena Demo Setup")
LABEL = "Install Quake III Arena Demo"
TARGET = "C:\\Games\\Quake III Arena Demo Setup\\Q3ADemo.exe"


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def upstream(root=ROOT):
    return json.loads((Path(root) / "third_party/games" / PACKAGE / "UPSTREAM.json").read_text())


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


def payloads(out_dir=None, root=ROOT):
    """(sources, directories, metadata, free hashes). out_dir is unused: nothing is generated."""
    root = Path(root)
    record = verify(root)
    folder = root / "third_party/games" / PACKAGE
    sources = {INSTALLER_PATH: folder / record["archive"],
               f"/system/licenses/{PACKAGE}-UPSTREAM.json": folder / "UPSTREAM.json"}
    for item in record["license_files"]:
        sources[f"/system/licenses/{PACKAGE}-{item['name']}"] = folder / item["name"]
    metadata = {"q3a_demo": {"package": PACKAGE, "installer": INSTALLER_PATH, "sha256": record["sha256"],
                             "size": record["size"], "menu": "Programs > Games"}}
    return sources, set(DIRECTORIES), metadata, {record["sha256"]}
