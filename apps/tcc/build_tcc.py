#!/usr/bin/env python3
"""Offline, pinned TinyCC port and installable target SDK. One job at a time."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/apps/tcc"
SDK = ROOT / "build/tools/ciuki-sdk"
VENDOR = ROOT / "third_party/tcc"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def inputs():
    pin = json.loads((VENDOR / "UPSTREAM.json").read_text())
    actual = {str(p.relative_to(VENDOR / "upstream")): digest(p)
              for p in sorted((VENDOR / "upstream").rglob("*")) if p.is_file()}
    snapshot = hashlib.sha256("".join(f"{h}  {p}\n" for p, h in actual.items()).encode()).hexdigest()
    patches = {p.name: digest(p) for p in sorted((VENDOR / "patches").glob("*.patch"))}
    if actual != pin["files"] or snapshot != pin["snapshot_sha256"] or patches != pin["patches"]:
        raise ValueError("TinyCC source/patch SHA-256 mismatch; no download or fallback")
    hostcc = shutil.which("clang")
    authored = [ROOT / "docs/developer-guide.md", ROOT / "docs/developer-api.md",
                ROOT / "scripts/gen_api_reference.py", VENDOR / "README.md", VENDOR / "COPYING",
                VENDOR / "upstream-files.json", VENDOR / "GPL-2.txt", *sorted((ROOT / "sdk/samples").rglob("*")),
                *sorted((ROOT / "apps/tcc/include").glob("*.h")), ROOT / "apps/tcc/capture.c"]
    return {"upstream": pin, "sdk": digest(SDK / "manifest.json"),
            "desktop": digest(ROOT / "build/apps/desktop/manifest.json"),
            "hostcc": {"path": hostcc, "sha256": digest(hostcc)},
            "recipe": digest(__file__), "authored": {str(p.relative_to(ROOT)): digest(p) for p in authored if p.is_file()}}


def current(expected):
    try:
        m = json.loads((OUT / "manifest.json").read_text())
        return m["inputs"] == expected and all(digest(OUT / n) == h for n, h in m["files"].items())
    except (OSError, ValueError, KeyError):
        return False


def validate_payloads():
    module("tcc_sdk_validator", ROOT / "apps/lua/build_lua.py").validate_sdk()
    if not current(inputs()):
        raise ValueError("missing/stale TinyCC payloads; run make apps")
    m = json.loads((OUT / "manifest.json").read_text())
    actual = module("tcc_elf_check", ROOT / "sdk/tests/check_sdk.py").inspect(OUT / "tcc")
    if actual != m["elf"]:
        raise ValueError("TinyCC ELF evidence mismatch")
    return m


def image_payloads():
    m = validate_payloads()
    paths = {"/Programs/tcc": OUT / "tcc"}
    for name in m["files"]:
        if name.startswith("sdk/"):
            paths["/System/" + name] = OUT / name
        elif name.startswith("developer/"):
            paths["/Documents/Developer/" + name.removeprefix("developer/")] = OUT / name
    dirs = {str(Path(n).parent) for n in paths}
    # The image writer requires each ancestor to exist explicitly.
    for n in list(dirs):
        dirs.update(str(p) for p in Path(n).parents if str(p) != "/")
    return paths, dirs, {"manifest_sha256": digest(OUT / "manifest.json"),
                         "release": m["inputs"]["upstream"]["release"], "elf": m["elf"],
                         "snapshot_sha256": m["inputs"]["upstream"]["snapshot_sha256"]}


def build():
    module("tcc_sdk_validator_build", ROOT / "apps/lua/build_lua.py").validate_sdk()
    expected = inputs()  # Validate immutable source before touching output.
    if current(expected):
        validate_payloads()
        print("[tcc] input/output hashes unchanged")
        return
    OUT.mkdir(parents=True, exist_ok=True)
    work = OUT / "work"
    if work.exists():
        shutil.rmtree(work)
    shutil.copytree(VENDOR / "upstream", work)
    env = os.environ.copy()
    env["TMPDIR"] = str(work)
    for k in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH"):
        env.pop(k, None)
    def run(command):
        subprocess.run([str(x) for x in command], cwd=work, env=env, check=True)
    for patch in sorted((VENDOR / "patches").glob("*.patch")):
        run(["patch", "--batch", "--fuzz=0", "-p1", "-i", patch])
    abi = (ROOT / "src/kernel/include/ciuki/abi.h").read_text()
    base = re.search(r"^#define CIUKI_IMAGE_BASE (\S+)", abi, re.M)[1]
    (work / "config.h").write_text('\n'.join([
        '#define TCC_VERSION "0.9.27-ciuki"', '#define CIUKI_TCC 1',
        '#define TCC_TARGET_I386 1', '#define CONFIG_TCC_STATIC 1',
        '#define CONFIG_TCC_ASM 1', '#define CONFIG_TCCDIR "/System/sdk"',
        '#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/tcc-include:{B}/include"',
        '#define CONFIG_TCC_LIBPATHS "{B}/lib"', '#define CONFIG_TCC_CRTPREFIX "{B}/lib"',
        '#define TCC_LIBTCC1 "lib/libtcc1.a"', f'#define CIUKI_TCC_IMAGE_BASE {base}',
        '#define CONFIG_TCC_ELFINTERP "unsupported"', '']) )
    cc = SDK / "bin/ciuki-cc"
    # Upstream release predates current Clang's strict declaration diagnostics.
    flags = ["-O2", "-w", "-Wno-error", "-Wno-implicit-function-declaration", "-Wno-int-conversion",
             "-Wno-incompatible-pointer-types", "-Wno-deprecated-non-prototype"]
    run([cc, *flags, work / "tcc.c", ROOT / "apps/tcc/capture.c", "-Wl,--wrap=_write_r", "-o", OUT / "tcc"])
    run([expected["hostcc"]["path"], *flags, work / "tcc.c", "-o", OUT / "tcc-host", "-lm"])
    target = OUT / "sdk"
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(SDK / "sysroot", target)
    inc = target / "include"
    shutil.copytree(ROOT / "apps/libdeskui", inc / "deskui", ignore=shutil.ignore_patterns("*.c"))
    (inc / "desktop").mkdir(exist_ok=True)
    for name in ("protocol.h", "theme.h", "appearance.h", "font_codes.h"):
        shutil.copy2(ROOT / "apps/desktop" / name, inc / "desktop" / name)
    (inc / "deskui.h").write_text('/* SPDX-License-Identifier: MIT */\n#include "deskui/deskui.h"\n')
    shutil.copy2(ROOT / "build/apps/desktop/libdeskui.a", target / "lib/libdeskui.a")
    shutil.copytree(work / "include", target / "tcc-include")
    shutil.copy2(ROOT / "apps/tcc/include/stddef.h", target / "tcc-include/stddef.h")
    runtime = []
    for name in ("libtcc1.c", "alloca86.S", "alloca86-bt.S"):
        obj = work / (name + ".o")
        run([OUT / "tcc-host", "-B" + str(target), "-c", work / "lib" / name, "-o", obj])
        runtime.append(obj)
    sdk = json.loads((SDK / "manifest.json").read_text())
    run([sdk["tools"]["llvm-ar"]["path"], "rcsD", target / "lib/libtcc1.a", *runtime])
    notice = (VENDOR / "README.md").read_text() + "\n" + (VENDOR / "COPYING").read_text()
    notice += "\nUpstream libtcc1 runtime notice and additional linking permission:\n" + (VENDOR / "upstream/lib/libtcc1.c").read_text().split("*/", 1)[0] + "*/\n"
    notice += "\nGNU GPL version 2 (libtcc1, with the exception above):\n" + (VENDOR / "GPL-2.txt").read_text()
    (target / "TCC.TXT").write_text(notice)
    shutil.copytree(VENDOR, target / "tcc-source/third_party/tcc")
    (target / "tcc-source/apps/tcc").mkdir(parents=True)
    shutil.copy2(Path(__file__), target / "tcc-source/apps/tcc/build_tcc.py")
    shutil.copy2(ROOT / "apps/tcc/capture.c", target / "tcc-source/apps/tcc/capture.c")
    shutil.copytree(ROOT / "apps/tcc/include", target / "tcc-source/apps/tcc/include")
    shutil.copy2(work / "config.h", target / "tcc-source/config.h")
    shutil.copytree(SDK / "licenses", target / "licenses")
    shutil.copy2(ROOT / "sdk/LICENSE", target / "licenses/SDK-MIT.txt")
    elf = module("tcc_build_check", ROOT / "sdk/tests/check_sdk.py").inspect(OUT / "tcc")
    module("tcc_runtime_check", ROOT / "sdk/tests/check_sdk.py").audit_code(target / "lib/libtcc1.a", sdk)
    reference = module("tcc_api_reference", ROOT / "scripts/gen_api_reference.py")
    if reference.generate() != (ROOT / "docs/developer-api.md").read_text():
        raise ValueError("stale developer API reference; regenerate public headers")
    developer = OUT / "developer"
    if developer.exists():
        shutil.rmtree(developer)
    developer.mkdir()
    shutil.copy2(ROOT / "docs/developer-guide.md", developer / "Guide.txt")
    (developer / "API").mkdir()
    for name, header in reference.headers().items():
        (developer / "API" / (name.replace("/", "-") + ".txt")).write_text(reference.header_reference(name, header))
    shutil.copytree(ROOT / "sdk/samples", developer / "Samples")
    files = {str(p.relative_to(OUT)): digest(p) for p in sorted(target.rglob("*")) if p.is_file()}
    files.update({str(p.relative_to(OUT)): digest(p) for p in sorted(developer.rglob("*")) if p.is_file()})
    files.update({n: digest(OUT / n) for n in ("tcc", "tcc-host")})
    (OUT / "manifest.json").write_text(json.dumps({"schema_version": 1, "inputs": expected,
        "files": files, "elf": elf, "guest_qualification": "not_run"}, indent=2, sort_keys=True) + "\n")
    shutil.rmtree(work)
    print("[tcc] target compiler, host cross compiler and target SDK built; guest not_run")


if __name__ == "__main__":
    build()
