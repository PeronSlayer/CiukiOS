#!/usr/bin/env python3
"""Build and inspect desktop/demo using the existing static Ciuki SDK."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/apps/desktop"
SDK = ROOT / "build/tools/ciuki-sdk"
CFLAGS = ["-O2", "-Wall", "-Wextra", "-Werror"]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def inputs():
    files = [p for directory in (ROOT / "apps/desktop", ROOT / "apps/demo")
             for p in directory.rglob("*") if p.is_file() and "__pycache__" not in p.parts]
    files.append(ROOT / "assets/brand/ciuki-logo.png")
    return {"sdk_manifest_sha256": digest(SDK / "manifest.json"),
            "sources": {str(p.relative_to(ROOT)): digest(p) for p in sorted(files)},
            "cflags": CFLAGS}


def current_build(expected):
    try:
        m = json.loads((OUT / "manifest.json").read_text())
        return (m["inputs"] == expected and set(m["files"]) ==
                {"desktop", "demo", "ciuki-portrait.xrgb"} and
                all(digest(OUT / n) == h for n, h in m["files"].items()))
    except (OSError, ValueError, KeyError):
        return False


def validate_payloads():
    lua = module("desktop_sdk_validator", ROOT / "apps/lua/build_lua.py")
    lua.validate_sdk()
    if not current_build(inputs()):
        raise ValueError("missing/stale desktop payloads; run make desktop")
    m = json.loads((OUT / "manifest.json").read_text())
    converter = module("desktop_portrait", ROOT / "apps/desktop/convert_portrait.py")
    if (m["files"]["ciuki-portrait.xrgb"] != converter.ARRAY_SHA256 or
            digest(ROOT / "assets/brand/ciuki-logo.png") != converter.SOURCE_SHA256 or
            m["portrait"] != portrait_record(converter)):
        raise ValueError("approved Ciuki portrait provenance mismatch")
    inspect = lua.checker().inspect
    for name in ("desktop", "demo"):
        if inspect(OUT / name) != m["elf"][name]:
            raise ValueError(f"desktop ELF evidence: {name}")
    return m


def portrait_record(c):
    return {"source": "assets/brand/ciuki-logo.png", "source_sha256": c.SOURCE_SHA256,
            "array_sha256": c.ARRAY_SHA256, "width": c.SIZE, "height": c.SIZE,
            "format": "XRGB8888 little-endian B,G,R,0", "bytes": c.SIZE*c.SIZE*4,
            "conversion": "floor(x*1254/256), floor(y*1254/256); integer alpha over #375564"}


def build():
    lua = module("desktop_sdk_build", ROOT / "apps/lua/build_lua.py")
    lua.validate_sdk()
    c = module("desktop_portrait", ROOT / "apps/desktop/convert_portrait.py")
    data = c.convert(ROOT / "assets/brand/ciuki-logo.png")
    if hashlib.sha256(data).hexdigest() != c.ARRAY_SHA256:
        raise ValueError("converted portrait differs from recorded SHA-256")
    expected = inputs()
    if current_build(expected):
        validate_payloads()
        print("[desktop] inputs and output hashes unchanged")
        return
    OUT.mkdir(parents=True, exist_ok=True)
    temp = OUT / "temp"
    temp.mkdir(exist_ok=True)
    env = os.environ.copy()
    env["TMPDIR"] = str(temp)
    start = time.monotonic()
    sources = {"desktop": ["desktop.c", "client.c", "protocol.c", "input.c", "compositor.c", "assets.c"],
               "demo": ["../demo/demo.c", "protocol.c", "compositor.c"]}
    evidence, commands = {}, {}
    for name, files in sources.items():
        commands[name] = [str(SDK / "bin/ciuki-cc"), *CFLAGS, "-I", str(ROOT / "apps/desktop"),
                          *(str(ROOT / "apps/desktop" / p) for p in files), "-o", str(OUT / name)]
        subprocess.run(commands[name], check=True, cwd=ROOT, env=env)
        evidence[name] = lua.checker().inspect(OUT / name)
    # Audit compositor code separately: integer drawing must never emit x87.
    sdk = json.loads((SDK / "manifest.json").read_text())
    obj = temp / "compositor.o"
    subprocess.run([str(SDK / "bin/ciuki-cc"), *CFLAGS, "-c", str(ROOT / "apps/desktop/compositor.c"),
                    "-o", str(obj)], check=True, env=env)
    audit_integer(obj, sdk)
    obj.unlink()
    (OUT / "ciuki-portrait.xrgb").write_bytes(data)
    m = {"schema_version": 1, "application": "Ciuki desktop", "protocol_version": 1,
         "inputs": expected, "commands": commands, "elf": evidence,
         "files": {name: digest(OUT / name) for name in (*sources, "ciuki-portrait.xrgb")},
         "portrait": portrait_record(c), "integer_compositor_audit": "PASS",
         "build_seconds": round(time.monotonic()-start, 3), "guest_qualification": "not_run"}
    (OUT / "manifest.json").write_text(json.dumps(m, indent=2, sort_keys=True)+"\n")
    print(f"[desktop] built/inspected desktop and demo in {m['build_seconds']} seconds; guest not_run")


def audit_integer(obj, sdk):
    import re
    audit = module("desktop_opcode_audit", ROOT / "scripts/build_kernel.py")
    dis = subprocess.check_output([sdk["tools"]["llvm-objdump"]["path"], "-d", "--no-show-raw-insn", str(obj)], text=True)
    for line in dis.splitlines():
        ins = re.match(r"\s*[0-9a-f]+:\s+([a-z][a-z0-9]*)\s*(.*)", line)
        if ins and (audit.is_fpu_or_simd(ins[1]) or re.search(r"%(?:xmm|ymm|zmm|mm)[0-9]+", ins[2])):
            raise ValueError(f"noninteger compositor instruction: {line}")


if __name__ == "__main__":
    try:
        build()
    except (OSError, ValueError) as error:
        raise SystemExit(str(error))
