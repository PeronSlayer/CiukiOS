"""Pinned Tango icon store and Ciuki portrait for the native desktop harnesses.

libdeskui's icons.c reads /system/icons/tango.cis and the portrait conversion
at run time; host builds point it at copies under build/host/icons through
-DDU_ICON_STORE_PATH / -DDU_ICON_PORTRAIT_PATH, so harness frames contain the
same icons as the guest. Both files are rebuilt only when their pinned
SHA-256 (apps/desktop/tango-store.json, convert_portrait.ARRAY_SHA256) differs.
"""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
FOLDER = ROOT / "build/host/icons"
SOURCES = ["apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c"]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _fresh(path, expected, produce):
    try:
        if hashlib.sha256(path.read_bytes()).hexdigest() == expected:
            return path
    except OSError:
        pass
    data = produce()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f"{path.name}: produced bytes differ from the pinned SHA-256")
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(".tmp")
    temp.write_bytes(data)
    temp.replace(path)
    return path


def store():
    icons = load("host_icon_store", ROOT / "apps/desktop/convert_icons.py")
    return _fresh(FOLDER / "tango.cis", icons.store_record()["store_sha256"], lambda: icons.build_store()[0])


def portrait():
    c = load("host_icon_portrait", ROOT / "apps/desktop/convert_portrait.py")
    return _fresh(FOLDER / "ciuki-portrait.xrgb", c.ARRAY_SHA256, lambda: c.convert(ROOT / "assets/brand/ciuki-logo.png"))


def defines():
    """Compiler flags that make icons.c use the pinned host copies."""
    return [f'-DDU_ICON_STORE_PATH="{store()}"', f'-DDU_ICON_PORTRAIT_PATH="{portrait()}"']
