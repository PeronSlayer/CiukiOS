#!/usr/bin/env python3
"""Build Ciuki VMM (build/f0/VMM.ELF) with the pinned toolchain.

Checks config/toolchain.json, compiles with the flags of
docs/design/execution-abi.md, links with src/kernel/linker.ld, fails on
undefined symbols, and audits the kernel for x87/MMX/SSE instructions
outside the FPU state-management routines.
"""
from __future__ import annotations

import json
import os
import re
import runpy
import subprocess
import sys
import time
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "kernel"
OUT = ROOT / "build" / "f0"
OBJ = OUT / "obj"

CFLAGS = [
    "--target=i686-unknown-elf", "-march=pentiumpro", "-ffreestanding", "-fno-pic",
    "-fno-pie", "-fno-builtin", "-nostdlib", "-mno-sse", "-mno-sse2", "-mno-mmx",
    "-mno-80387", "-mno-red-zone", "-fno-omit-frame-pointer", "-fno-strict-aliasing",
    "-fstack-protector-strong", "-mstack-protector-guard=global",
    "-mstack-alignment=4", "-std=c17", "-O2", "-g",
    "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
]
# Only these routines may contain FPU/SIMD instructions.
FPU_ALLOWED = {"fpu_fxsave", "fpu_fxrstor", "fpu_fnsave", "fpu_frstor", "fpu_reset_state", "trap_dispatch"}
FPU_ALLOWED_MNEMONICS = {"fxsave", "fxrstor", "fnsave", "frstor", "fninit", "ldmxcsr", "xorps", "fnclex"}


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    print("+", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def major(cmd: list[str], pattern: str) -> int:
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    m = re.search(pattern, out)
    if not m:
        raise SystemExit(f"cannot read version from {' '.join(cmd)}")
    return int(m.group(1))


def check_toolchain() -> None:
    pins = json.loads((ROOT / "config" / "toolchain.json").read_text())
    found = {
        "clang_major": major(["clang", "--version"], r"clang version (\d+)"),
        "ld_lld_major": major(["ld.lld", "--version"], r"LLD (\d+)"),
        "nasm_major": major(["nasm", "-v"], r"NASM version (\d+)"),
    }
    for key, value in found.items():
        if pins[key] != value:
            raise SystemExit(f"toolchain mismatch: {key} is {value}, pinned {pins[key]}")


def build_id() -> tuple[str, str, int]:
    """(human id, first 8 hex digits of the commit, dirty flag)."""
    try:
        rev = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short=12", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
        # Untracked sources count as dirty: the build must not look clean
        # while new files are not yet committed.
        dirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=all",
                                "--", "src", "scripts", "config", "tests", "Makefile"],
                               capture_output=True, text=True, check=True).stdout.strip()
        return rev + ("-dirty" if dirty else ""), rev[:8], 1 if dirty else 0
    except subprocess.CalledProcessError:
        return "unknown", "00000000", 1


FPU_SIMD_PREFIXES = ("f", "cvt", "ucomis", "comis", "sqrt", "rsqrt", "rcp", "shuf", "unpck",
                     "movap", "movup", "movhp", "movlp", "movhlp", "movlhp", "movmsk", "movnt",
                     "ldmxcsr", "stmxcsr", "emms", "movd", "movq", "maskmov", "pshuf", "clflush",
                     "sfence", "lfence", "mfence", "prefetch")
INTEGER_P_MNEMONICS = ("push", "pop", "pause", "popcnt")


def is_fpu_or_simd(mnemonic: str) -> bool:
    """Classify an AT&T mnemonic as x87/MMX/SSE. Conservative: any unknown
    'f'/'p' family or ps/pd/ss/sd suffix counts, so an omission fails the
    build instead of passing silently."""
    if mnemonic.startswith(INTEGER_P_MNEMONICS):
        return False
    if mnemonic.startswith("p") and len(mnemonic) > 2:
        return True                     # packed integer (MMX/SSE2): pxor, paddd, pand, ...
    if mnemonic.startswith(FPU_SIMD_PREFIXES):
        return True
    string_ops = {f"{op}{sz}" for op in ("lods", "stos", "movs", "cmps", "scas", "ins", "outs") for sz in "bwlq"}
    if mnemonic in string_ops:
        return False
    if mnemonic.endswith(("ps", "pd", "ss", "sd")) and len(mnemonic) > 3:
        return True                     # movss, cmpsd, addps, ... (AT&T string ops end in b/w/l/q)
    return mnemonic == "wait"


def audit_disassembly(dis: str, allowed: dict[str, set[str]]) -> list[str]:
    func = None
    bad = []
    insn = re.compile(r"^\s*[0-9a-f]+:\s+([a-z][a-z0-9]*)")
    for line in dis.splitlines():
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if m:
            func = m.group(1)
            continue
        m = insn.match(line)
        if not m:
            continue
        mnem = m.group(1)
        if is_fpu_or_simd(mnem) and mnem not in allowed.get(func, set()):
            bad.append(f"{func}: {line.strip()}")
    return bad


def audit(elf: Path) -> None:
    dis = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn", str(elf)],
                         capture_output=True, text=True, check=True).stdout
    allowed = {f: FPU_ALLOWED_MNEMONICS for f in FPU_ALLOWED}
    bad = audit_disassembly(dis, allowed)
    if bad:
        raise SystemExit("FPU/SIMD audit failed:\n" + "\n".join(bad[:20]))
    print("[build-kernel] FPU/SIMD audit passed")


def main() -> int:
    check_toolchain()
    OBJ.mkdir(parents=True, exist_ok=True)
    os.environ["TMPDIR"] = str(OUT)
    bid, bhex, bdirty = build_id()
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", str(int(time.time()))))
    if epoch < 0 or epoch > 0x7fffffffffffffff:
        raise SystemExit("invalid build epoch")
    payload = OUT / "payload.bin"
    run(["nasm", "-f", "bin", str(SRC / "probes" / "payload.asm"), "-o", str(payload)])
    # The interim native fixture is a NASM ELF file, with every public value
    # extracted from abi.h by the existing target-layout compiler boundary.
    sys.dont_write_bytecode = True
    layout = runpy.run_path(str(ROOT / "scripts/test/abi_layout_dump.py"))["dump"]()
    definitions = []
    for key, value in layout.items():
        if key.startswith("constant."):
            name = key.removeprefix("constant.")
        elif key.startswith(("sizeof.ciuki_", "offsetof.ciuki_")):
            name = "ABI_" + re.sub(r"[^A-Za-z0-9_]", "_", key).upper()
        else:
            continue
        definitions.append(f"%define {name} {value}\n")
    (OUT / "proc_abi.inc").write_text("".join(definitions))
    proc_payload = OUT / "proc-payload.elf"
    run(["nasm", "-f", "bin", "-I", str(OUT) + "/",
         str(ROOT / "tests/host/proc/payload.asm"), "-o", str(proc_payload)])
    signal_payload = OUT / "signal-payload.elf"
    run(["nasm", "-f", "bin", "-I", str(OUT) + "/",
         str(ROOT / "tests/host/proc/signal_payload.asm"), "-o", str(signal_payload)])
    desktop_payload = OUT / "desktop-payload.elf"
    run(["nasm", "-f", "bin", "-I", str(OUT) + "/",
         str(ROOT / "tests/host/proc/desktop_payload.asm"), "-o", str(desktop_payload)])
    files_payload = OUT / "files-payload.elf"
    run(["nasm", "-f", "bin", "-I", str(OUT) + "/",
         str(ROOT / "tests/host/proc/files_payload.asm"), "-o", str(files_payload)])
    objs = []
    for asm in sorted((SRC / "arch").glob("*.asm")) + sorted((SRC / "vm").glob("*.asm")) + [SRC / "probes" / "payload_blob.asm"]:
        o = OBJ / (asm.stem + ".o")
        run(["nasm", "-f", "elf32", "-I", str(ROOT) + "/", f"-DPAYLOAD_BIN='{payload}'",
             str(asm), "-o", str(o)], cwd=ROOT)
        objs.append(o)
    for c in sorted(list((SRC / "core").glob("*.c")) + list((SRC / "lib").glob("*.c")) +
                    list((SRC / "arch").glob("*.c")) + list((SRC / "probes").glob("*.c")) +
                    list((SRC / "drivers").glob("*.c")) + list((SRC / "fs").glob("*.c")) +
                    list((SRC / "proc").glob("*.c")) + list((SRC / "vm").glob("*.c"))):
        o = OBJ / (c.parent.name + "_" + c.stem + ".o")
        run(["clang", *CFLAGS, f'-DCIUKI_BUILD_ID="{bid}"', f'-DCIUKI_BUILD_HEX8="{bhex}"',
             f'-DCIUKI_PROC_PAYLOAD_BIN="{proc_payload}"',
             f'-DCIUKI_SIGNAL_PAYLOAD_BIN="{signal_payload}"',
             f'-DCIUKI_DESKTOP_PAYLOAD_BIN="{desktop_payload}"',
             f'-DCIUKI_FILES_PAYLOAD_BIN="{files_payload}"',
             f"-DCIUKI_BUILD_EPOCH={epoch}LL",
             f"-DCIUKI_BUILD_DIRTY={bdirty}", "-I", str(SRC / "include"),
             "-c", str(c), "-o", str(o)])
        objs.append(o)
    elf = OUT / "VMM.ELF"
    # .f1probes is append-only in the linker script. Its objects must follow
    # the acceptance table, rather than the source filenames. Keep the
    # dispatcher unchanged; each registration remains in its owning file.
    f1_order = {
        name: i for i, name in enumerate((
            "probes_registry_probe", "drivers_i8042_probe", "drivers_fbdev_probe",
            "drivers_ata_probe", "probes_fat_probes", "probes_safe_probe",
        ))
    }
    objs.sort(key=lambda obj: f1_order.get(obj.stem, -1))
    # The append-only F2 table follows its contract order. app-gate owns its
    # registration in its own translation unit and is deliberately last.
    f2_order = {
        name: i for i, name in enumerate((
            "probes_f2_probes_process", "probes_f2_probes_desktop",
            "probes_f2_probes_files", "probes_f2_probes_signals",
            "probes_f2_probes_app",
        ))
    }
    objs.sort(key=lambda obj: f2_order.get(obj.stem, -1))
    run(["ld.lld", "-m", "elf_i386", "-T", str(SRC / "linker.ld"), "--no-undefined",
         "-Map", str(OUT / "VMM.map"), "-o", str(elf), *map(str, objs)])
    audit(elf)
    clock_record = {"utc_epoch": epoch, "kernel_sha256": hashlib.sha256(elf.read_bytes()).hexdigest()}
    (OUT / "build-clock.json").write_text(json.dumps(clock_record, indent=2) + "\n")
    size = elf.stat().st_size
    print(f"[build-kernel] {elf.relative_to(ROOT)} ({size} bytes), build {bid}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
