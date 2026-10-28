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
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "kernel"
OUT = ROOT / "build" / "f0"
OBJ = OUT / "obj"

# Keep the 4 KiB single-frame bound after f2-20's increase to 16 KiB task
# stacks. The app controller and supervisor keep a stricter 1 KiB bound for
# the existing storage frames, formatters and interrupt frames.
# Clang measures optimized frames, including inlined callees:
# https://clang.llvm.org/docs/DiagnosticsReference.html#wframe-larger-than
KERNEL_FRAME_LIMIT = 4096
APP_GATE_FRAME_LIMIT = 1024
CFLAGS = [
    "--target=i686-unknown-elf", "-march=pentiumpro", "-ffreestanding", "-fno-pic",
    "-fno-pie", "-fno-builtin", "-nostdlib", "-mno-sse", "-mno-sse2", "-mno-mmx",
    "-mno-80387", "-mno-red-zone", "-fno-omit-frame-pointer", "-fno-strict-aliasing",
    "-fstack-protector-strong", "-mstack-protector-guard=global",
    "-mstack-alignment=4", "-std=c17", "-O2", "-g",
    "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
    f"-Wframe-larger-than={KERNEL_FRAME_LIMIT}",
]
# Only these routines may contain FPU/SIMD instructions.
FPU_ALLOWED = {"fpu_fxsave", "fpu_fxrstor", "fpu_fnsave", "fpu_frstor", "fpu_reset_state", "trap_dispatch"}
FPU_ALLOWED_MNEMONICS = {"fxsave", "fxrstor", "fnsave", "frstor", "fninit", "ldmxcsr", "xorps", "fnclex"}
# Per-function admissions outside the FPU context switch. intel2d flushes CPU
# cache lines only after its probe saw CPUID.1:EDX[19] (CLFSH); no other
# function may contain CLFLUSH.
FPU_ALLOWED_PER_FUNCTION = {"intel2d_clflush_range": {"clflush"}}


def frame_flags(source: Path) -> list[str]:
    if source in (SRC / "probes" / "f2_probes_app.c", SRC / "proc" / "supervisor.c"):
        return [f"-Wframe-larger-than={APP_GATE_FRAME_LIMIT}"]
    return []


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


def read_version() -> dict:
    """{'number', 'stage', 'display'} from config/version.json (the single
    source; scripts/ciuki_version.py validates it)."""
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("ciuki_version", ROOT / "scripts" / "ciuki_version.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.load()


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
    allowed.update(FPU_ALLOWED_PER_FUNCTION)
    bad = audit_disassembly(dis, allowed)
    if bad:
        raise SystemExit("FPU/SIMD audit failed:\n" + "\n".join(bad[:20]))
    print("[build-kernel] FPU/SIMD audit passed")


def main() -> int:
    check_toolchain()
    OBJ.mkdir(parents=True, exist_ok=True)
    os.environ["TMPDIR"] = str(OUT)
    bid, bhex, bdirty = build_id()
    version = read_version()["display"]
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
    audio_payload = OUT / "audio-payload.elf"
    run(["nasm", "-f", "bin", "-I", str(OUT) + "/",
         str(ROOT / "tests/host/audio/payload.asm"), "-o", str(audio_payload)])
    w32_core = runpy.run_path(str(ROOT / "scripts/win32/core_fixtures.py"))["build"]()
    # Quiet-boot splash: the original Ciuki photograph (docs/boot.md).
    splash = runpy.run_path(str(ROOT / "scripts/convert_splash.py"))["build"](OUT / "splash.rgb565")
    # f4-02 optical registration is collected from probes/f4_probes_media.c
    # below and retained by .f4probes in the canonical linker script.
    if not (SRC / "probes/f4_probes_media.c").is_file():
        raise SystemExit("missing F4 optical probe")
    lwip_recipe = runpy.run_path(str(SRC / "net/lwip/build.py"))
    lwip_source = lwip_recipe["source"]()
    lwip_includes = lwip_recipe["includes"](lwip_source)
    objs = []
    acpi_recipe = runpy.run_path(str(SRC / "acpi/build.py"))
    acpi_out = OBJ / "acpica"
    acpi_sources = acpi_recipe["prepare"](acpi_out)
    for c in [*acpi_sources, *sorted((SRC / "acpi").glob("*.c"))]:
        o = acpi_out / (c.stem + ".o")
        run(["clang", *CFLAGS, *acpi_recipe["flags"](acpi_out),
             "-c", str(c), "-o", str(o)])
        objs.append(o)
    jent_recipe = runpy.run_path(str(SRC / "lib/jitterentropy/build.py"))
    jent_source = jent_recipe["source"]()
    for c in jent_recipe["sources"](jent_source):
        o = OBJ / ("jent_" + c.stem + ".o")
        # Upstream deliberately rejects optimized noise/core builds.
        run(["clang", *CFLAGS, "-O0", *jent_recipe["includes"](jent_source),
             "-I", str(SRC / "include"), "-c", str(c), "-o", str(o)])
        objs.append(o)
    for name in lwip_recipe["SOURCES"]:
        c = lwip_source / "src" / name
        o = OBJ / ("lwip_" + name.replace("/", "_") + ".o")
        run(["clang", *CFLAGS, *lwip_includes, "-I", str(SRC / "net/lwip/libc"),
             "-I", str(SRC / "include"), "-c", str(c), "-o", str(o)])
        objs.append(o)

    for asm in sorted((SRC / "arch").glob("*.asm")) + sorted((SRC / "vm").glob("*.asm")) + [SRC / "probes" / "payload_blob.asm"]:
        o = OBJ / (asm.stem + ".o")
        run(["nasm", "-f", "elf32", "-I", str(ROOT) + "/", f"-DPAYLOAD_BIN='{payload}'",
             str(asm), "-o", str(o)], cwd=ROOT)
        objs.append(o)
    for c in sorted(list((SRC / "core").glob("*.c")) + list((SRC / "lib").glob("*.c")) +
                    list((SRC / "arch").glob("*.c")) + list((SRC / "probes").glob("*.c")) +
                    list((SRC / "drivers").glob("*.c")) +
                    [SRC / "drivers/video" / name for name in ("registry.c", "attempt.c", "swdib.c", "native.c", "batch.c",
                        "native_hw.c", "render3d.c", "render3d_scalar.c", "render3d_native.c", "render3d_transport.c", "intel2d/engine.c", "intel2d/driver.c", "unichrome/engine.c",
                        "unichrome/driver.c", "sis/engine.c", "sis/cursor.c", "sis/register.c",
                        "vmwaresvga/engine.c", "vmwaresvga/driver.c", "vmwaresvga/io.c", "mach64/engine.c",
                        "mach64/driver.c", "rage128/driver.c", "rage128/engine.c", "cirrus/engine.c",
                        "cirrus/driver.c", "tdfx/driver.c", "tdfx/engine.c", "s3legacy/engine.c",
                        "s3legacy/driver.c", "s3legacy/platform.c", "s3trio64/engine.c", "s3virge/engine.c",
                        "i810/driver.c", "i810/memory.c", "i740/driver.c")] +
                    [SRC / "drivers/video/s3savage" / name for name in ("s3savage_register.c", "savage2d.c", "savage3d.c", "savage3d_owner.c", "supersavage_display.c", "supersavage_mode.c")] +
                    [SRC / "drivers/video/radeon2d" / name for name in ("engine.c", "cursor.c", "driver.c")] +
                    [SRC / "drivers/video/nv2d" / name for name in ("adapter.c", "engine.c", "hardware.c", "dma_hw.c")] +
                    [SRC / "drivers/video/riva128" / name for name in ("driver.c", "engine.c")] +
                    [SRC / "drivers/video/matrox" / name for name in ("engine.c", "driver.c", "matrox3d.c", "matrox3d_owner.c")] +
                    [SRC / "drivers/video/trident" / name for name in ("engine.c", "driver.c")] +
                    [SRC / "drivers/video/neomagic" / name for name in ("driver.c", "engine.c", "cursor.c")] +
                    [SRC / "drivers/video/chipshiqv" / name for name in ("hiqv_engine.c", "chipshiqv.c")] +
                    list((SRC / "fs").glob("*.c")) +
                    list((SRC / "usb").glob("*.c")) + list((SRC / "audio").glob("*.c")) + list((SRC / "proc").glob("*.c")) + list((SRC / "proc/win32").glob("*.c")) + list((SRC / "ndis").glob("*.c")) + list((SRC / "net").glob("*.c")) + list((SRC / "net/lwip").glob("*.c")) + list((SRC / "vm").glob("*.c"))):
        o = OBJ / (c.parent.name + "_" + c.stem + ".o")
        run(["clang", *CFLAGS, *lwip_includes, "-I", str(SRC / "net/lwip/libc"), *frame_flags(c), f'-DCIUKI_BUILD_ID="{bid}"', f'-DCIUKI_BUILD_HEX8="{bhex}"',
             f'-DCIUKI_VERSION="{version}"',
             f'-DCIUKI_PROC_PAYLOAD_BIN="{proc_payload}"',
             f'-DCIUKI_SIGNAL_PAYLOAD_BIN="{signal_payload}"',
             f'-DCIUKI_DESKTOP_PAYLOAD_BIN="{desktop_payload}"',
             f'-DCIUKI_FILES_PAYLOAD_BIN="{files_payload}"',
             f'-DCIUKI_AUDIO_PAYLOAD_BIN="{audio_payload}"',
             f'-DCIUKI_W32_EXE_BIN="{w32_core["exe"]}"',
             f'-DCIUKI_W32_LOADER_BIN="{w32_core["loader"]}"',
             f'-DCIUKI_W32_CORE_BASE={w32_core["base"]}u',
             f'-DCIUKI_W32_CORE_RESULT_RVA={w32_core["CoreResult"]}u',
             f'-DCIUKI_W32_CORE_RELEASE_RVA={w32_core["CoreRelease"]}u',
             f'-DCIUKI_SPLASH_BIN="{splash}"',
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
    # f3-00 owns the first append-only F3 registration; f3-01 devices,
    # f3-02 video (vm-video, vm-window), f3-05 audio state, f3-04 DPMI and the f3-06
    # workloads (dos4gw, doom, doom-sound, wolf3d, vm-starve, quake) follow in the sweep order.
    objs.sort(key=lambda obj: {"probes_f3_probes_vm": 0, "probes_f3_probes_devices": 1,
                               "probes_f3_probes_video": 2, "probes_f3_probes_audio_state": 3,
                               "probes_f3_probes_dpmi": 4,
                               "probes_f3_probes_workloads": 5}.get(obj.stem, -1))
    run(["ld.lld", "-m", "elf_i386", "-T", str(SRC / "linker.ld"), "--no-undefined",
         "-Map", str(OUT / "VMM.map"), "-o", str(elf), *map(str, objs)])
    audit(elf)
    clock_record = {"utc_epoch": epoch, "kernel_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
                    "version": version}
    (OUT / "build-clock.json").write_text(json.dumps(clock_record, indent=2) + "\n")
    size = elf.stat().st_size
    print(f"[build-kernel] {elf.relative_to(ROOT)} ({size} bytes), CiukiOS {version}, build {bid}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
