#!/usr/bin/env python3
"""Canonical SDK application inventory; each producer uses the same ELF checker."""
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def validate_payloads():
    desktop = load("apps_desktop", ROOT / "apps/desktop/build_desktop.py").validate_payloads()
    tcc = load("apps_tcc", ROOT / "apps/tcc/build_tcc.py").validate_payloads()
    lua = load("apps_lua", ROOT / "apps/lua/build_lua.py")
    sdk = lua.validate_sdk()
    checker = lua.checker()
    files = {}
    for name in ("desktop", "demo", "dosbox", "files", "tasks", "ciuknote", "ciukide", "paint"):
        files["/Programs/" + name] = {"source": "build/apps/desktop/" + name,
                               "sha256": desktop["files"][name], "elf": desktop["elf"][name]}
    for name, source in (("lua", "build/apps/lua/lua"), ("hello", "build/tools/ciuki-sdk/tests/hello.elf"),
                         ("libc_smoke", "build/tools/ciuki-sdk/tests/libc_smoke.elf")):
        path = ROOT / source
        files["/Programs/" + name] = {"source": source, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                               "elf": checker.inspect(path)}
    files["/Programs/tcc"] = {"source": "build/apps/tcc/tcc", "sha256": tcc["files"]["tcc"], "elf": tcc["elf"]}
    result = {"schema_version": 1, "sdk_manifest_sha256": hashlib.sha256((ROOT / "build/tools/ciuki-sdk/manifest.json").read_bytes()).hexdigest(),
              "applications": files, "newlib": sdk["newlib"], "guest_qualification": "not_run"}
    return result

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--jobs", "-j", type=int, default=2, choices=(1, 2))
    parser.add_argument("--source-archive", type=Path, default=ROOT / "build/downloads/newlib/lua-5.4.8.tar.gz")
    parser.add_argument("--tests-archive", type=Path, default=ROOT / "build/downloads/newlib/lua-5.4.8-tests.tar.gz")
    args = parser.parse_args()
    subprocess.run([sys.executable, "-B", str(ROOT / "apps/lua/build_lua.py"), "--jobs", str(args.jobs),
                    "--source-archive", str(args.source_archive), "--tests-archive", str(args.tests_archive)], check=True)
    load("apps_desktop_build", ROOT / "apps/desktop/build_desktop.py").build()
    load("apps_tcc_build", ROOT / "apps/tcc/build_tcc.py").build()
    (ROOT / "build/apps/manifest.json").write_text(json.dumps(validate_payloads(), indent=2, sort_keys=True) + "\n")

if __name__ == "__main__":
    main()
