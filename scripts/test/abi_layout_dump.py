#!/usr/bin/env python3
"""Extract ABI values from an i686 object, without running target code.

The public header supplies expressions only. Literal contract expectations live
in tests/host/abi_layout_test.c, independently of this discovery/dump step.
Output is a flat JSON object: sizeof.*, alignof.*, offsetof.* and constant.*.
Artifacts and compiler temporary files stay under build/host/abi-layout/.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import runpy
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "src/kernel/include/ciuki/abi.h"
OUT = ROOT / "build/host/abi-layout"


def expressions(header: str) -> dict[str, str]:
    """Discover the deliberately simple declarations in the public C header.

    A changed declaration syntax fails the independent oracle's missing/extra
    entry check rather than silently dropping a public record or constant.
    """
    result: dict[str, str] = {}
    for name, value in re.findall(r"^#define\s+(\w+)[ \t]+([^\n]+)", header, re.M):
        if "{" not in value:  # aggregate initializers are checked in C
            result[f"constant.{name}"] = name
    enum = re.search(r"enum ciuki_syscall\s*\{(.*?)\};", header, re.S)
    if not enum:
        raise ValueError("missing syscall enum")
    for name in re.findall(r"\b(CIUKI_SYS_\w+)\s*=", enum[1]):
        result[f"constant.{name}"] = name
    for name in re.findall(r"^typedef\s+[^;]+\s+(ciuki_\w+)\s*;", header, re.M):
        result[f"sizeof.{name}"] = f"sizeof({name})"
    for name, body in re.findall(r"struct\s+(ciuki_\w+)\s*\{(.*?)\};", header, re.S):
        result[f"sizeof.{name}"] = f"sizeof(struct {name})"
        result[f"alignof.{name}"] = f"_Alignof(struct {name})"
        for field, extent in re.findall(r"^\s+(?:struct\s+\w+|\w+)\s+(\w+)(?:\[(\d+)\])?;", body, re.M):
            result[f"offsetof.{name}.{field}"] = f"__builtin_offsetof(struct {name}, {field})"
            # Each saved general slot has a listed offset in the contract.
            if name == "ciuki_ucontext" and field == "gregs":
                for i in range(int(extent)):
                    result[f"offsetof.{name}.{field}[{i}]"] = f"__builtin_offsetof(struct {name}, {field}[{i}])"
    for name in ("char", "short", "int", "long", "long long", "void *",
                 "float", "double", "long double"):
        result[f"target.sizeof.{name}"] = f"sizeof({name})"
    result["target.alignof.long double"] = "_Alignof(long double)"
    result["target.char_signed"] = "((char)-1 < 0)"
    return dict(sorted(result.items()))


def dump() -> dict[str, int]:
    OUT.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(OUT))
    # Load the canonical flags, without invoking a kernel build or git queries.
    sys.dont_write_bytecode = True
    flags = runpy.run_path(str(ROOT / "scripts/build_kernel.py"))["CFLAGS"]
    pins = json.loads((ROOT / "config/toolchain.json").read_text())
    version = subprocess.run(["clang", "--version"], check=True, capture_output=True,
                             text=True, env=env).stdout
    major = re.search(r"clang version (\d+)", version)
    if not major or int(major[1]) != pins["clang_major"]:
        raise ValueError("clang major differs from config/toolchain.json")
    if f"--target={pins['target']}" not in flags or f"-march={pins['march']}" not in flags:
        raise ValueError("kernel flags differ from pinned target/CPU")
    values = expressions(HEADER.read_text())
    source = OUT / "layout.c"
    obj = OUT / "layout.o"
    raw = OUT / "layout.bin"
    source.write_text('#include <ciuki/abi.h>\n'
                      '__attribute__((used, section(".ciuki_abi_layout")))\n'
                      'const uint32_t ciuki_abi_layout[] = {\n' +
                      ''.join(f'    (uint32_t)({expr}), /* {key} */\n'
                              for key, expr in values.items()) + '};\n')
    subprocess.run(["clang", *flags, "-I", str(ROOT / "src/kernel/include"),
                    "-c", str(source), "-o", str(obj)], check=True, env=env)
    subprocess.run(["llvm-objcopy", "--dump-section", f".ciuki_abi_layout={raw}", str(obj)],
                   check=True, env=env)
    data = raw.read_bytes()
    if len(data) != 4 * len(values):
        raise ValueError("unexpected target layout table length")
    return dict(zip(values, struct.unpack(f"<{len(values)}I", data), strict=True))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="write JSON under this worktree's build/ instead of stdout")
    args = parser.parse_args()
    if args.output:
        path = args.output.resolve()
        if not path.is_relative_to((ROOT / "build").resolve()):
            parser.error("output must be under this worktree's build/")
    try:
        data = json.dumps(dump(), indent=2, sort_keys=True) + "\n"
        if args.output:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(data)
        else:
            sys.stdout.write(data)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"abi layout dump: FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
