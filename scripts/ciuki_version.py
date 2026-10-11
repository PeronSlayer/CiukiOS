#!/usr/bin/env python3
"""The CiukiOS version, read from config/version.json (single source).

build_kernel.py (kernel define and build record), build_image.py (loader
define and build manifest) and tests/host/test_version.py use this module;
nothing else may spell the version in code.
"""
# SPDX-License-Identifier: GPL-2.0-only
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
PATH = ROOT / "config" / "version.json"
NUMBER = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
STAGE = re.compile(r"[a-z][a-z0-9]*(-[a-z0-9]+)*")


def load(path=PATH):
    """Return {'number', 'stage', 'display'}; the characters are restricted
    so the display string can be passed as a compiler/assembler define."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    number, stage = data.get("version"), data.get("stage")
    if not (isinstance(number, str) and NUMBER.fullmatch(number)):
        raise SystemExit(f"{path}: 'version' must be MAJOR.MINOR.PATCH")
    if not (isinstance(stage, str) and STAGE.fullmatch(stage)):
        raise SystemExit(f"{path}: 'stage' must be lowercase words joined by '-'")
    return {"number": number, "stage": stage, "display": f"{number} {stage}"}


if __name__ == "__main__":
    print(load()["display"])
