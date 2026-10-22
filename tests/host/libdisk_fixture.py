#!/usr/bin/env python3
"""Apply one f1-22 corruption fixture (scripts/test/fat_fixtures.py) to a FAT
volume file in place, for the f9-02 consistency-check test."""
# SPDX-License-Identifier: GPL-2.0-only
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts/test"))
import fat_fixtures  # noqa: E402

result = fat_fixtures.corrupt(Path(sys.argv[1]), sys.argv[2])
print(json.dumps({"defect": result["defect"], "patched_sectors": result["patched_sectors"]}))
