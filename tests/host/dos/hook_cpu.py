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
    # Byte for byte what tests/host/vm/prepare_lifecycle.py writes to this
    # path (its F0-44 teardown audit points, the guard for dosvm_test's own
    # AS_AUDIT_CHARGE): either generator may run last, also concurrently
    # (fx-doswin3: the guest runner's build had replaced the audited copy
    # under a running build-dos-host).
    aspace = aspace.replace("uint32_t old = pt[j];", "AS_AUDIT_CHARGE(1000); uint32_t old = pt[j];")
    aspace = aspace.replace("if (pd[i] & PTE_P) {", "AS_AUDIT_CHARGE(1000); if (pd[i] & PTE_P) {")
    (out / "dosvm_aspace.inc").write_text(
        "#ifndef AS_AUDIT_CHARGE\n#define AS_AUDIT_CHARGE(ns) ((void)0)\n#endif\n" + aspace + "\n#undef AS_AUDIT_CHARGE\n")
    font = (ROOT / "assets/fonts/native/INCONSOL.CFN").read_bytes()[64:64 + 95 + 95 * 32]
    assert len(font) == 95 + 95 * 32
    (out / "dosvm_font.inc").write_text("const uint8_t font_cfn_regular[] = {" + ",".join(map(str, font)) + "};\n")


def main():
    raise SystemExit("Classic CiukiDOS CPU harness retired by f3-63; use run_unified_file.sh. "
                     "--prepare remains available for the unified harness.")


if __name__ == "__main__":
    prepare() if "--prepare" in sys.argv else main()
