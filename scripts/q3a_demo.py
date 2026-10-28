#!/usr/bin/env python3
"""Quake III Arena demo installer, vendored whole in third_party/games/q3a-demo-installer.

The Windows installer (MindVision VISE, not unpackable on the host) is installed unmodified, with its licence
agreement and README, in the protected installer store /Programs/Installers/Quake III Arena Demo
(C:\\Programs\\Installers\\Quake III Arena Demo\\Q3ADemo.exe): administrators and SYSTEM write it, everyone reads
and executes it (user-security-contract.md 8.3, A1-5; sec-user-02). The visible desktop folder
/Documents/Desktop/Quake III Arena Demo stays as a protected presentation anchor holding only shortcuts to the
exact store files with the store as working directory; it carries no executable, so nothing in shared /Documents
can be replaced, planted beside the installer or used as its search path. The installer creates its own Start Menu
shortcuts, which the Start menu merges. There is no built-in Start-menu record. The image build never runs the
installer. Owner decision 2026-10-25."""
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "q3a-demo-installer"
FOLDER = ROOT / "third_party/games" / PACKAGE
STORE_ROOT = "/Programs/Installers"
STORE_FOLDER = STORE_ROOT + "/Quake III Arena Demo"
DESKTOP_FOLDER = "/Documents/Desktop/Quake III Arena Demo"
INSTALLER_PATH = STORE_FOLDER + "/Q3ADemo.exe"
DIRECTORIES = ("/Programs", STORE_ROOT, STORE_FOLDER, "/Documents", "/Documents/Desktop", DESKTOP_FOLDER)
STORE_WINDOWS = "C:\\Programs\\Installers\\Quake III Arena Demo"
TARGET = STORE_WINDOWS + "\\Q3ADemo.exe"
# Shortcut name -> (label, store file). Labels are the owner's English UI copy.
LINKS = {
    "Install Quake III Arena Demo.lnk": ("Install Quake III Arena Demo", "Q3ADemo.exe"),
    "Licence Agreement.lnk": ("Quake III Arena Demo Licence Agreement", "Q3A_EULA.txt"),
    "Read Me.lnk": ("Quake III Arena Demo Read Me", "README.txt"),
}


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


def shell_link(label, target, directory, show=1):
    """CiukiOS .lnk v3 (apps/desktop/shell_link.h desk_link_encode): magic, show,
    icon index, five u32 lengths, hotkey; then label, target, arguments,
    working directory and icon, UTF-8 without NULs."""
    fields = [label.encode(), target.encode(), b"", directory.encode(), b""]
    for value in fields:
        if b"\0" in value or any(c < 32 or c == 127 for c in value):
            raise ValueError("shortcut field")
    header = b"CIUKLNK3" + struct.pack("<Ii5II", show, 0, *map(len, fields), 0)
    data = header + b"".join(fields)
    if len(data) > 4096:
        raise ValueError("shortcut too long")
    return data


def payloads(out_dir=None, root=ROOT):
    """(sources, directories, metadata, free hashes). The three shortcuts are
    generated under out_dir (default: build/q3a-demo)."""
    root = Path(root)
    record = verify(root)
    folder = root / "third_party/games" / PACKAGE
    out = Path(out_dir) if out_dir is not None else ROOT / "build/q3a-demo"
    out.mkdir(parents=True, exist_ok=True)
    sources = {INSTALLER_PATH: folder / record["archive"],
               f"{STORE_FOLDER}/Q3A_EULA.txt": folder / "Q3A_EULA.txt",
               f"{STORE_FOLDER}/README.txt": folder / "README-linux-demo.txt",
               f"/system/licenses/{PACKAGE}-UPSTREAM.json": folder / "UPSTREAM.json"}
    links = {}
    for name, (label, file) in LINKS.items():
        path = out / name
        path.write_bytes(shell_link(label, f"{STORE_WINDOWS}\\{file}", STORE_WINDOWS))
        sources[f"{DESKTOP_FOLDER}/{name}"] = path
        links[name] = {"target": f"{STORE_WINDOWS}\\{file}", "directory": STORE_WINDOWS, "sha256": sha256(path)}
    for item in record["license_files"]:
        sources[f"/system/licenses/{PACKAGE}-{item['name']}"] = folder / item["name"]
    metadata = {"q3a_demo": {"package": PACKAGE, "installer": INSTALLER_PATH, "sha256": record["sha256"],
                             "size": record["size"], "store": STORE_FOLDER, "folder": DESKTOP_FOLDER,
                             "links": links}}
    return sources, set(DIRECTORIES), metadata, {record["sha256"]}
