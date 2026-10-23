#!/usr/bin/env python3
"""Build and inspect the Setup placeholder (/Programs/setup on the Setup CD
boot image only; directive f9-01 item 7). Rebuilt when an input changes."""
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/apps/setup"
SDK = ROOT / "build/tools/ciuki-sdk"
SOURCES = ("apps/setup/setup.c", "apps/setup/setup_info.c")
HEADERS = ("apps/setup/setup_info.h",)
CFLAGS = ["-O2", "-Wall", "-Wextra", "-Werror"]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def inputs():
    return {"sources": {name: digest(ROOT / name) for name in (*SOURCES, *HEADERS, "apps/setup/build_setup.py")},
            "sdk_manifest_sha256": digest(SDK / "manifest.json")}


def build(validate_sdk=True):
    """Returns (ELF path, manifest record). validate_sdk=False only for
    evidence builds against an SDK built from other sources (recorded)."""
    lua = module("setup_sdk", ROOT / "apps/lua/build_lua.py")
    if validate_sdk:
        lua.validate_sdk()
    expected = {**inputs(), "sdk_validated": validate_sdk}
    manifest_path = OUT / "manifest.json"
    elf = OUT / "setup"
    try:
        record = json.loads(manifest_path.read_text())
        if record["inputs"] == expected and digest(elf) == record["sha256"]:
            return elf, record
    except (OSError, ValueError, KeyError):
        pass
    OUT.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(OUT))
    command = [str(SDK / "bin/ciuki-cc"), *CFLAGS, "-I", str(ROOT / "apps/setup"),
               *(str(ROOT / name) for name in SOURCES), "-o", str(elf)]
    subprocess.run(["nice", "-n", "19", *command], check=True, cwd=ROOT, env=env)
    record = {"schema_version": 1, "program": "/Programs/setup", "kind": "placeholder (f9-01)",
              "inputs": expected, "command": command, "elf": lua.checker().inspect(elf),
              "sha256": digest(elf), "size": elf.stat().st_size, "guest_qualification": "not_run"}
    manifest_path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    return elf, record


if __name__ == "__main__":
    path, result = build(validate_sdk="--unvalidated-sdk" not in sys.argv[1:])
    print(f"[setup] {path.relative_to(ROOT)} {result['sha256']}")
