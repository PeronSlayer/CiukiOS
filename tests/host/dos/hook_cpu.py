"""H50 production hook CPU fixture. No canonical/floppy image or QEMU build."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def prepare():
    out = ROOT / "build/host"
    out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "src/kernel/core/vmm.c").read_text()
    aspace = source.split("/* ---- user address spaces ---- */", 1)[1]
    (out / "dosvm_aspace.inc").write_text("#define AS_AUDIT_CHARGE(ns) ((void)0)\n" + aspace + "\n#undef AS_AUDIT_CHARGE\n")
    font = (ROOT / "assets/fonts/native/INCONSOL.CFN").read_bytes()[64:64 + 95 + 95 * 32]
    assert len(font) == 95 + 95 * 32
    (out / "dosvm_font.inc").write_text("const uint8_t font_cfn_regular[] = {" + ",".join(map(str, font)) + "};\n")


def main():
    raise SystemExit("Classic CiukiDOS CPU harness retired by f3-63; use run_unified_file.sh. "
                     "--prepare remains available for the unified harness.")


if __name__ == "__main__":
    prepare() if "--prepare" in sys.argv else main()
