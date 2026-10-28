"""Offline native OpenGL recipe shared by the SDK and sequential host tests.

Since f7-14 the native library is the GL engine of sdk/win32/opengl32 (the
same sources the Win32 personality builds) over the retained TinyGL
front-end, the scalar software backend and the native render3d provider.
TinyGL's public names are renamed inside the library; the engine owns the
public gl* entry points of sdk/libgl/include/GL/gl.h.
"""
# SPDX-License-Identifier: MIT
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
ENGINE_DIR = ROOT / "sdk/win32/opengl32"
SOURCES = ("api", "list", "vertex", "init", "matrix", "texture", "misc", "clear",
           "light", "clip", "select", "get", "zbuffer", "zline", "ztriangle",
           "zmath", "image_util", "msghandling", "arrays", "specbuf", "memory",
           "ztext", "zraster", "accum", "zpostprocess")
# Shared engine units (sdk/win32/opengl32); the Win32-only units (context,
# present, runtime, api, wgl_formats, minimum) stay in the DLL build.
ENGINE = ("engine", "state", "texture", "pixels", "validate", "geometry", "backend",
          "census", "common")
NATIVE = ("context", "provider")
RENAME = HERE / "native/tinygl_rename.h"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def engine_files():
    files = [ENGINE_DIR / (n + ".c") for n in ENGINE]
    files += [ENGINE_DIR / n for n in ("internal.h", "backend.h", "admission.h", "encode.h",
                                      "minimum.h", "ids.h", "families.inc", "family_validation.inc",
                                      "vectors.inc", "begin.inc", "provider.inc")]
    return files


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
    adapter = [HERE / "build.py", *sorted((HERE / "software").glob("*.[ch]")),
               *sorted((HERE / "native").glob("*.[ch]")), *sorted((HERE / "include").rglob("*.h"))]
    return {"upstream": upstream, "port": port, "sources": SOURCES, "engine": ENGINE,
            "adapter": {str(p.relative_to(HERE)): digest(p) for p in adapter},
            "engine_files": {str(p.relative_to(ROOT)): digest(p) for p in engine_files()}}


def includes():
    """Public headers of the native library (GL/gl.h, ciuki/gl.h)."""
    return ["-I", str(HERE / "include")]


def install_headers(destination):
    destination = Path(destination)
    shutil.copytree(HERE / "include/GL", destination / "GL", dirs_exist_ok=True)
    shutil.copytree(HERE / "include/ciuki", destination / "ciuki", dirs_exist_ok=True)


def notices(destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "sdk/libgl/LICENSE-TinyGL.txt", destination / "TinyGL-LICENSE.txt")
    (destination / "TinyGL-NOTICE.txt").write_text(
        "CiukiOS uses TinyGL by Fabrice Bellard, Gek (DMHSW), and C-Chads.\n"
        "Software OpenGL subset; see TinyGL-LICENSE.txt.\n")
    # f7-14: libgl.a links the shared GL engine of sdk/win32/opengl32, whose
    # files carry SPDX LGPL-2.1-or-later; keep that licence beside the library.
    shutil.copy2(ROOT / "sdk/win32/COPYING.LIB", destination / "opengl-engine-LGPL-2.1.txt")


def _compile(cc, flags, source, obj, folder, environment, extra=(), include=()):
    # The library's own headers precede the caller's sysroot, so an installed
    # SDK's previous GL headers can never shadow the ones being built.
    subprocess.run([str(cc), *include, *flags, *extra, "-fno-strict-aliasing", "-ffp-contract=off",
                    f"-ffile-prefix-map={ROOT}=.", f"-fdebug-prefix-map={ROOT}=.",
                    f"-ffile-prefix-map={folder}=./libgl-build", f"-fdebug-prefix-map={folder}=./libgl-build",
                    "-fdebug-compilation-dir=.", "-c", str(source), "-o", str(obj)],
                   check=True, env=environment)


VENDOR_WARNINGS = ["-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-unused-function",
                   "-Wno-sign-compare", "-Wno-implicit-fallthrough"]


def build(folder, cc="clang", ar="llvm-ar", flags=(), env=None, model=()):
    """The native library. model adds host-model flags (never in the SDK)."""
    record = recipe()
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ if env is None else env)
    for key in ("CPATH", "C_INCLUDE_PATH", "LIBRARY_PATH"):
        environment.pop(key, None)
    objects = []
    native = ["-DCIUKI_GL_NATIVE", *model, "-include", str(RENAME)]
    for source in (HERE / "vendor/src" / (n + ".c") for n in SOURCES):
        obj = folder / ("tinygl_" + source.stem + ".o")
        _compile(cc, flags, source, obj, folder, environment, [*native, *VENDOR_WARNINGS],
                 [*includes(), "-I", str(HERE / "vendor/include")])
        objects.append(str(obj))
    for source in [HERE / "software/renderer.c", *(ENGINE_DIR / (n + ".c") for n in ENGINE),
                   *(HERE / "native" / (n + ".c") for n in NATIVE)]:
        obj = folder / (("engine_" if source.parent == ENGINE_DIR else "") + source.stem + ".o")
        _compile(cc, flags, source, obj, folder, environment, [*native, "-Wno-unused-function"], includes())
        objects.append(str(obj))
    obj = folder / "api.o"
    _compile(cc, flags, HERE / "native/api.c", obj, folder, environment, ["-DCIUKI_GL_NATIVE"], includes())
    objects.append(str(obj))
    library = folder / "libgl.a"
    library.unlink(missing_ok=True)
    subprocess.run([str(ar), "rcsD", str(library), *objects], check=True, env=environment)
    (folder / "recipe.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    return library


def build_tinygl(folder, cc="clang", ar="llvm-ar", flags=(), env=None):
    """TinyGL plus the scalar backend only, un-renamed: the Win32 host model
    links it under the opengl32 sources (tests/host/win32/gl)."""
    recipe()
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ if env is None else env)
    objects = []
    for source in [*(HERE / "vendor/src" / (n + ".c") for n in SOURCES), HERE / "software/renderer.c"]:
        obj = folder / (source.stem + ".o")
        _compile(cc, flags, source, obj, folder, environment,
                 ["-I", str(HERE / "vendor/include"), *VENDOR_WARNINGS])
        objects.append(str(obj))
    library = folder / "libtinygl.a"
    library.unlink(missing_ok=True)
    subprocess.run([str(ar), "rcsD", str(library), *objects], check=True, env=environment)
    return library


def _cached(folder, name, inputs, make):
    expected = json.loads(json.dumps(inputs))
    stamp = folder / "host.json"
    library = folder / name
    if stamp.exists() and library.exists():
        cached = json.loads(stamp.read_text())
        if cached.get("inputs") == expected and cached.get("sha256") == digest(library):
            return library
    make()
    stamp.write_text(json.dumps({"inputs": expected, "sha256": digest(library)}, sort_keys=True) + "\n")
    return library


def host_flags(sanitize):
    flags = ["-std=c17", "-O2", "-fPIC", "-ffunction-sections", "-fdata-sections"]
    if sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
    return flags


def host(sanitize=False):
    """The native library for host tests: kernel ABI headers from the tree and
    the host model (CIUKI_GL_HOST_MODEL) in place of the syscall gate."""
    folder = ROOT / "build/host" / ("libgl-sanitize" if sanitize else "libgl")
    inputs = {**recipe(), "sanitize": sanitize, "model": "host",
              "compiler": subprocess.check_output(["clang", "--version"], text=True)}
    return _cached(folder, "libgl.a", inputs, lambda: build(
        folder, flags=[*host_flags(sanitize), "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                       "-I", str(ROOT / "src/kernel/include")],
        model=["-DCIUKI_GL_HOST_MODEL"]))


def host_tinygl(sanitize=False):
    folder = ROOT / "build/host" / ("tinygl-sanitize" if sanitize else "tinygl")
    inputs = {**recipe(), "sanitize": sanitize, "model": "tinygl",
              "compiler": subprocess.check_output(["clang", "--version"], text=True)}
    return _cached(folder, "libtinygl.a", inputs, lambda: build_tinygl(folder, flags=host_flags(sanitize)))
