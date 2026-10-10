#!/usr/bin/env python3
"""Build the pinned, unmodified Lua 5.4.8 sources offline with the Ciuki SDK."""
# SPDX-License-Identifier: MIT
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/apps/lua"
SDK = ROOT / "build/tools/ciuki-sdk"
SOURCE_DIR = "lua-5.4.8"
TEST_DIR = "lua-5.4.8-tests"
CFLAGS = "-O2 -Wall -Wextra -DLUA_COMPAT_5_3"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def inventory(directory):
    return {p.relative_to(directory).as_posix(): digest(p)
            for p in sorted(directory.rglob("*")) if p.is_file()}


def extract(archive_path, destination, top):
    """Only the expected regular files/directories, with no path or link escape."""
    with tarfile.open(archive_path) as archive:
        names = set()
        for member in archive.getmembers():
            path = Path(member.name)
            if (path.is_absolute() or ".." in path.parts or not path.parts
                    or path.parts[0] != top or member.name in names
                    or not (member.isfile() or member.isdir())):
                raise ValueError(f"unexpected archive member: {member.name}")
            names.add(member.name)
        archive.extractall(destination, filter="data")
    return destination / top


def checker():
    spec = importlib.util.spec_from_file_location("check_sdk", ROOT / "sdk/tests/check_sdk.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate_sdk():
    manifest = json.loads((SDK / "manifest.json").read_text())
    pins = json.loads((ROOT / "config/sdk-pins.json").read_text())
    if manifest["newlib"] != pins["newlib"]:
        raise ValueError("changed newlib pins; rebuild the SDK")
    for field in ("files", "license_files", "sdk_sources"):
        base = ROOT if field == "sdk_sources" else SDK
        for name, expected in manifest[field].items():
            if digest(base / name) != expected:
                raise ValueError(f"changed SDK input: {name}; rebuild the SDK")
    if manifest["abi_sha256"] != digest(ROOT / "src/kernel/include/ciuki/abi.h"):
        raise ValueError("changed ABI header; rebuild the SDK")
    for name, tool in manifest["tools"].items():
        if any(tool[field] != pins["tools"][name][field] for field in ("version", "sha256")):
            raise ValueError(f"changed SDK tool pin: {name}; rebuild the SDK")
        if digest(tool["path"]) != tool["sha256"]:
            raise ValueError(f"changed pinned SDK tool: {name}")
    return manifest


def inputs(source_archive, tests_archive):
    pins = json.loads((ROOT / "config/sdk-pins.json").read_text())["lua"]
    archives = {}
    for name, path in (("source", source_archive), ("tests", tests_archive)):
        if not path.is_file() or digest(path) != pins[name]["sha256"]:
            raise ValueError(f"missing {name} archive or SHA-256 mismatch; no download/fallback")
        archives[name] = pins[name]
    validate_sdk()
    return {"archives": archives, "sdk_manifest_sha256": digest(SDK / "manifest.json"),
            "app_sources": inventory(ROOT / "apps/lua"), "cflags": CFLAGS,
            "configuration": "generic; C89/Linux/POSIX/readline/dlopen off; int64/double"}


def current_build(expected):
    try:
        manifest = json.loads((OUT / "manifest.json").read_text())
        return (manifest["inputs"] == expected
                and all(digest(OUT / name) == sha for name, sha in manifest["files"].items())
                and all(digest(OUT / SOURCE_DIR / name) == sha
                        for name, sha in manifest["source_files"].items())
                and inventory(OUT / TEST_DIR) == manifest["test_files"]
                and sorted(p.relative_to(OUT / TEST_DIR).as_posix()
                           for p in (OUT / TEST_DIR).rglob("*") if p.is_dir())
                == manifest["test_directories"])
    except (OSError, ValueError, KeyError):
        return False


def build(source_archive, tests_archive, jobs):
    expected = inputs(source_archive, tests_archive)
    if current_build(expected):
        print("[lua] inputs and output hashes unchanged")
        return
    # Archive/SDK validation happens before replacing a previous build.
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    started = time.monotonic()
    temp = OUT / "temp"
    temp.mkdir()
    env = os.environ.copy()
    env["TMPDIR"] = str(temp)
    for name in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH",
                 "MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES"):
        env.pop(name, None)
    src = extract(source_archive, OUT, SOURCE_DIR)
    tests = extract(tests_archive, OUT, TEST_DIR)
    original_sources = inventory(src)
    sdk = validate_sdk()
    # Verify the actual target configuration, without modifying an upstream file.
    probe = OUT / "configuration.c"
    probe.write_text('''#include "lua.h"
#if defined(LUA_USE_C89) || defined(LUA_USE_LINUX) || defined(LUA_USE_POSIX) || defined(LUA_USE_READLINE) || defined(LUA_USE_DLOPEN) || defined(LUA_DL_DLL) || defined(NDEBUG)
#error unsupported Lua configuration
#endif
#if LUA_32BITS || !defined(LUA_COMPAT_5_3)
#error unsupported Lua numeric/compatibility configuration
#endif
_Static_assert(sizeof(lua_Integer) == 8, "64-bit Lua integers");
_Static_assert(sizeof(lua_Number) == 8 && LUA_FLOAT_TYPE == LUA_FLOAT_DOUBLE, "double Lua numbers");
''')
    subprocess.run([str(SDK / "bin/ciuki-cc"), *CFLAGS.split(), "-I", str(src / "src"),
                    "-c", str(probe), "-o", str(OUT / "configuration.o")], check=True, env=env)
    command = ["make", "-C", str(src / "src"), f"-j{jobs}", "generic",
               f"CC={SDK / 'bin/ciuki-cc'}", f"CFLAGS={CFLAGS}",
               f"AR={sdk['tools']['llvm-ar']['path']} rcsD",
               f"RANLIB={sdk['tools']['llvm-ranlib']['path']}",
               "SYSCFLAGS=", "SYSLDFLAGS=", "SYSLIBS=", "MYCFLAGS=", "MYLDFLAGS=", "MYLIBS=",
               "MYOBJS=", "CMCFLAGS=", "LDFLAGS=", "LIBS=-lm"]
    with (OUT / "build.log").open("w") as log:
        try:
            subprocess.run(command, check=True, env=env, stdout=log, stderr=subprocess.STDOUT)
        except subprocess.CalledProcessError:
            print((OUT / "build.log").read_text(), file=sys.stderr)
            raise
    for name, sha in original_sources.items():
        if digest(src / name) != sha:
            raise ValueError(f"upstream source modified: {name}")
    evidence = {}
    inspect = checker().inspect
    for name in ("lua", "luac"):
        shutil.copy2(src / "src" / name, OUT / name)
        evidence[name] = inspect(OUT / name)
    # The exact release notice ships on the volume separately from the test notice.
    shutil.copy2(src / "src/lua.h", OUT / "LUA-LICENSE.txt")
    shutil.copy2(ROOT / "apps/lua/ciuki-f2.lua", OUT / "ciuki-f2.lua")
    files = {name: digest(OUT / name) for name in ("lua", "luac", "LUA-LICENSE.txt", "ciuki-f2.lua")}
    manifest = {"schema_version": 1, "application": "Lua", "version": "5.4.8",
                "inputs": expected, "command": command, "source_files": original_sources,
                "test_files": inventory(tests), "test_directories": sorted(
                    p.relative_to(tests).as_posix() for p in tests.rglob("*") if p.is_dir()),
                "files": files, "elf": evidence, "patches": [],
                "build_seconds": round(time.monotonic() - started, 3),
                "guest_qualification": "not_run",
                "upstream_mode": {"argv": ["lua", "-e", "_U=true", "all.lua"],
                                  "exclusions": {"_soft": True, "_port": True, "_nomsg": True,
                                                 "T": None},
                                  "complete": "excluded_by_contract",
                                  "internal": "excluded_by_contract"}}
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"[lua] built/inspected lua and luac in {manifest['build_seconds']} seconds; guest not_run")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    downloads = ROOT / "build/downloads/newlib"
    parser.add_argument("--source-archive", type=Path, default=downloads / "lua-5.4.8.tar.gz")
    parser.add_argument("--tests-archive", type=Path, default=downloads / "lua-5.4.8-tests.tar.gz")
    parser.add_argument("--jobs", type=int, choices=(1, 2), default=2)
    args = parser.parse_args()
    try:
        build(args.source_archive.resolve(), args.tests_archive.resolve(), args.jobs)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
