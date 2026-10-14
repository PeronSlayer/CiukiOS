"""Offline, minimal FreeType recipe shared by target SDK and host checks."""
# SPDX-License-Identifier: MIT
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SOURCES = ("base/ftbase.c", "base/ftinit.c", "base/ftsystem.c", "base/ftdebug.c",
           "base/ftbitmap.c", "base/ftmm.c", "truetype/truetype.c", "sfnt/sfnt.c",
           "smooth/smooth.c", "autofit/autofit.c")
MODULES = ("truetype", "sfnt", "smooth", "autofit")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def pin():
    return json.loads((ROOT / "config/sdk-pins.json").read_text())["freetype"]


def source():
    record = pin()
    patches = sorted((HERE / "patches").glob("*.patch"))
    patch_hashes = {p.name: digest(p) for p in patches}
    if patch_hashes != record["patches"]:
        raise ValueError("unrecorded or changed FreeType patch")
    archive = ROOT / "build/downloads/freetype" / record["archive"]
    if not archive.is_file() or digest(archive) != record["sha256"]:
        raise ValueError(f"missing or changed pinned FreeType archive: {archive}; no download/fallback")
    parent = ROOT / "build/external"
    parent.mkdir(parents=True, exist_ok=True)
    folder = parent / record["directory"]
    stamp = folder / "ciuki-source-hashes.json"
    if stamp.exists():
        cached = json.loads(stamp.read_text())
        hashes = cached.get("files", {})
        expected = {str(p.relative_to(folder)) for p in folder.rglob("*") if p.is_file() and p != stamp}
        if cached.get("patches") == patch_hashes and expected == set(hashes) and all((folder / name).is_file() and digest(folder / name) == value for name, value in hashes.items()):
            return folder
    if folder.exists():
        shutil.rmtree(folder)
    with tarfile.open(archive) as stream:
        # The pinned archive has exactly one root; reject escaping paths even
        # before Python's data extraction filter runs.
        if any(Path(member.name).parts[0] != record["directory"] for member in stream.getmembers()):
            raise ValueError("unexpected FreeType archive root")
        stream.extractall(parent, filter="data")
    for patch in patches:
        subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch)], cwd=folder, check=True,
                       stdout=subprocess.DEVNULL)
    stamp.write_text(json.dumps({"patches": patch_hashes, "files": {
        str(p.relative_to(folder)): digest(p) for p in sorted(folder.rglob("*")) if p.is_file()}}, sort_keys=True) + "\n")
    return folder


def install_headers(destination, src):
    destination = Path(destination)
    shutil.copytree(src / "include/freetype", destination / "freetype", dirs_exist_ok=True)
    shutil.copy2(src / "include/ft2build.h", destination / "ft2build.h")
    # Export the actual configuration, including to callers using the ordinary
    # FT_FREETYPE_H include chain without our build-only command-line defines.
    overrides = (HERE / "ciuki_ftoption.h").read_text().split('#include <freetype/config/ftoption.h>', 1)[1]
    (destination / "freetype/config/ftoption.h").write_text(
        (src / "include/freetype/config/ftoption.h").read_text() + overrides)
    shutil.copy2(HERE / "ciuki_ftmodule.h", destination / "freetype/config/ftmodule.h")


def includes(src=None):
    return ["-I", str(HERE), "-I", str((src or source()) / "include"),
            '-DFT_CONFIG_OPTIONS_H="ciuki_ftoption.h"', '-DFT_CONFIG_MODULES_H="ciuki_ftmodule.h"']


def recipe():
    return {"pin": pin(), "modules": MODULES, "sources": SOURCES,
            "configuration": {p.name: digest(p) for p in (HERE / "ciuki_ftoption.h", HERE / "ciuki_ftmodule.h")},
            "hinting": "FT_LOAD_TARGET_NORMAL; interpreter-version=35",
            "license_choice": "FTL"}


def build(folder, cc="clang", ar="llvm-ar", flags=(), env=None):
    """One compiler at a time, deterministic archive, path-normalized objects."""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    src = source()
    environment = dict(os.environ if env is None else env)
    environment["SOURCE_DATE_EPOCH"] = str(pin()["source_date_epoch"])
    for key in ("CPATH", "C_INCLUDE_PATH", "LIBRARY_PATH"):
        environment.pop(key, None)
    objects = []
    for name in SOURCES:
        obj = folder / (Path(name).stem + ".o")
        subprocess.run([str(cc), *flags, *includes(src), "-DFT2_BUILD_LIBRARY", "-Wno-error",
                        f"-ffile-prefix-map={ROOT}=.", f"-fdebug-prefix-map={ROOT}=.",
                        f"-ffile-prefix-map={folder}=./freetype-build", f"-fdebug-prefix-map={folder}=./freetype-build",
                        "-fdebug-compilation-dir=.", "-c", str(src / "src" / name), "-o", str(obj)],
                       check=True, env=environment)
        objects.append(obj)
    library = folder / "libfreetype.a"
    library.unlink(missing_ok=True)
    subprocess.run([str(ar), "rcsD", str(library), *map(str, objects)], check=True, env=environment)
    (folder / "recipe.json").write_text(json.dumps(recipe(), indent=2, sort_keys=True) + "\n")
    return library


def host():
    folder = ROOT / "build/host/freetype"
    library = folder / "libfreetype.a"
    expected = {**recipe(), "compiler": subprocess.check_output(["clang", "--version"], text=True),
                "builder_sha256": digest(__file__)}
    stamp = folder / "host.json"
    if library.exists() and stamp.exists():
        record = json.loads(stamp.read_text())
        if record.get("inputs") == json.loads(json.dumps(expected)) and record.get("sha256") == digest(library):
            source()  # Verify the dependency even on a cache hit.
            return library
    build(folder, flags=("-std=c17", "-O2", "-fPIC", "-ffunction-sections", "-fdata-sections"))
    stamp.write_text(json.dumps({"inputs": expected, "sha256": digest(library)}, indent=2, sort_keys=True) + "\n")
    return library


def notices(destination):
    src = source()
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / "docs/FTL.TXT", destination / "FreeType-FTL.txt")
    shutil.copy2(src / "LICENSE.TXT", destination / "FreeType-LICENSE.txt")
    (destination / "FreeType-NOTICE.txt").write_text(
        "Parts of CiukiOS are based on the work of the FreeType team.\n"
        "FreeType 2.13.3 is distributed under the FreeType License (FTL).\n"
        "See FreeType-FTL.txt and FreeType-LICENSE.txt; upstream source notices are retained.\n")
