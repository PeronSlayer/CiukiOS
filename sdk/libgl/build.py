"""Offline TinyGL recipe shared by the SDK and sequential host tests."""
# SPDX-License-Identifier: MIT
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SOURCES = ("api", "list", "vertex", "init", "matrix", "texture", "misc", "clear",
           "light", "clip", "select", "get", "zbuffer", "zline", "ztriangle",
           "zmath", "image_util", "msghandling", "arrays", "specbuf", "memory",
           "ztext", "zraster", "accum", "zpostprocess")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def recipe():
    upstream = json.loads((HERE / "upstream.json").read_text())
    port = json.loads((HERE / "port.json").read_text())
    expected = {**upstream["upstream_files"], **port["altered_files"]}
    actual = {str(p.relative_to(HERE / "vendor")): digest(p)
              for p in sorted((HERE / "vendor").rglob("*")) if p.is_file()}
    if actual != expected:
        raise ValueError("changed/unrecorded TinyGL source; update reviewed port hashes")
    if digest(ROOT / "sdk/libgl/LICENSE-TinyGL.txt") != expected["LICENSE"]:
        raise ValueError("TinyGL licence differs from retained upstream notice")
    return {"upstream": upstream, "port": port, "sources": SOURCES,
            "adapter": {str(p.relative_to(HERE)): digest(p) for p in
                        (HERE / "surface.c", HERE / "include/ciuki/gl.h", HERE / "build.py", *sorted((HERE / "software").glob("*.[ch]")))}}


def includes():
    return ["-I", str(HERE / "include"), "-I", str(HERE / "vendor/include")]


def install_headers(destination):
    destination = Path(destination)
    shutil.copytree(HERE / "vendor/include/GL", destination / "GL", dirs_exist_ok=True)
    shutil.copytree(HERE / "include/ciuki", destination / "ciuki", dirs_exist_ok=True)


def notices(destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "sdk/libgl/LICENSE-TinyGL.txt", destination / "TinyGL-LICENSE.txt")
    (destination / "TinyGL-NOTICE.txt").write_text(
        "CiukiOS uses TinyGL by Fabrice Bellard, Gek (DMHSW), and C-Chads.\n"
        "Software OpenGL subset; see TinyGL-LICENSE.txt.\n")


def build(folder, cc="clang", ar="llvm-ar", flags=(), env=None):
    record = recipe()
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ if env is None else env)
    for key in ("CPATH", "C_INCLUDE_PATH", "LIBRARY_PATH"):
        environment.pop(key, None)
    objects = []
    for source in [*(HERE / "vendor/src" / (n + ".c") for n in SOURCES), HERE / "surface.c", HERE / "software/renderer.c"]:
        obj = folder / (source.stem + ".o")
        subprocess.run([str(cc), *flags, *includes(), "-fno-strict-aliasing", "-ffp-contract=off",
                        "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-unused-function",
                        "-Wno-sign-compare", "-Wno-implicit-fallthrough",
                        f"-ffile-prefix-map={ROOT}=.", f"-fdebug-prefix-map={ROOT}=.",
                        f"-ffile-prefix-map={folder}=./libgl-build", f"-fdebug-prefix-map={folder}=./libgl-build",
                        "-fdebug-compilation-dir=.", "-c", str(source), "-o", str(obj)],
                       check=True, env=environment)
        objects.append(str(obj))
    library = folder / "libgl.a"
    library.unlink(missing_ok=True)
    subprocess.run([str(ar), "rcsD", str(library), *objects], check=True, env=environment)
    (folder / "recipe.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    return library


def host(sanitize=False):
    folder = ROOT / "build/host" / ("libgl-sanitize" if sanitize else "libgl")
    expected = {**recipe(), "sanitize": sanitize,
                "compiler": subprocess.check_output(["clang", "--version"], text=True)}
    # JSON roundtrip normalizes the source tuple before comparison.
    expected = json.loads(json.dumps(expected))
    stamp = folder / "host.json"
    library = folder / "libgl.a"
    if stamp.exists() and library.exists():
        cached = json.loads(stamp.read_text())
        if cached.get("inputs") == expected and cached.get("sha256") == digest(library):
            return library
    flags = ["-std=c17", "-O2", "-fPIC", "-ffunction-sections", "-fdata-sections"]
    if sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
    build(folder, flags=flags)
    stamp.write_text(json.dumps({"inputs": expected, "sha256": digest(library)}, sort_keys=True) + "\n")
    return library
