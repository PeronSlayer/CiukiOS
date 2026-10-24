"""DOS Box memory host qualification (DOSMEM). No QEMU, systemd scope or OS image.

Boots the kernel=unified guest exactly as the canonical builder installs it
(fork kernel, CIUKIDF.SYS, FDCONFIG.SYS with DOS=LOW, CIUKISH.COM, FDAUTO.BAT
with SHARE) on the production VM code (tests/host/dosvm_guest.c with
libx86emu, the f3-61 production path) and checks what a real-mode program
started at the DOS Box prompt sees (tests/host/dos/dosmem/dosmem.asm):

- XMS 3.0 through INT 2Fh AX=4300h/4310h, answered by the VMM in front of the
  guest's INT 2Fh chain (the FreeDOS kernel returns AH=43h unchained);
- the largest free conventional block against Wolfenstein 3-D 1.4's own
  memory check (WL_MAIN.C InitGame, ID_MM.C MM_Startup), with margin;
- FreeCOM's XMS swap (the resident COMMAND block while a program runs);
- HMA (XMS 00h), the UMB arena (XMS 10h largest-block query) and LIM EMS 4.0
  (INT 67h, EMMXXXX0 at the vector segment, page frame E000h);

then runs the pinned Wolfenstein 3-D 1.4 shareware from its directory and
requires it to stay in its game graphics (mode 13h unchained) instead of
returning to the prompt with "You do not have enough memory". The pristine
FreeDOS 1.3 reference base must see XMS too (f3-48 case "multiplex").
Host replay, not QEMU or hardware evidence. SPDX-License-Identifier: GPL-2.0-only
"""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "build/host/dos/dosmem"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


h56 = module("dosmem_h56", ROOT / "tests/host/dos/unified_binding.py")
fork, unified = h56.fork, h56.unified
games = module("dosmem_games", ROOT / "scripts/build_games_disk.py")
VM = ["src/kernel/vm/dos_unified.c", "src/kernel/vm/dos_unified_ifs.c", "src/kernel/vm/dos_file.c",
      "src/kernel/vm/dos_file_volume.c"]
failures = []

# Wolfenstein 3-D 1.4 (id Software source release, github.com/id-Software/wolf3d):
# WL_MAIN.C InitGame quits with ERRORSCREEN when mminfo.mainmem < 235000;
# ID_MM.C MM_Startup sets mainmem = nearheap + farheap: coreleft() of the
# near heap less SAVENEARHEAP (0x400) plus farcoreleft() less SAVEFARHEAP (0)
# (ID_MM.H). Only conventional memory counts (UMBs, XMS and EMS do not).
# The shipped WOLF3D.EXE is LZEXE 0.91; its Borland C++ c0 (medium model,
# _heaplen 0) keeps PSP, code and a 64 KiB DGROUP (the stack _stklen at its
# top) and the far heap is the rest of the program's DOS block, so
#   farheap  = (block - (10h + DGROUP + 1000h)) * 16 - 16
#   nearheap = 65536 - edata - _stklen - 400h - 16
# and mainmem >= 235000 needs block >= WOLF3D_MIN_BLOCK paragraphs.
WOLF3D_LAYOUT = dict(image=287040, dgroup=0x3783, stklen=0x1000, heaplen=0, edata=0xE881)
WOLF3D_MIN_BLOCK = 0x80BD   # 527312 bytes (515 KiB) for WOLF3D.EXE's own block
MARGIN = 0x800              # 32 KiB demanded above it
DOSMEM_BLOCK = 0x6B         # DOSMEM.COM's own MCB and block, free again when Wolf3D runs


def check(condition, message):
    if not condition:
        failures.append(message)
        print("DOSMEM CHECK FAILED: " + message, flush=True)


def unlzexe(data):
    """LZEXE 0.91 decompression (the published unlzexe algorithm); returns
    the load module and the original MZ fields at the decompressor header."""
    h = struct.unpack_from("<16H", data)
    head = (h[4] + h[11]) << 4
    info = struct.unpack_from("<8H", data, head)
    if data[28:32] != b"LZ91":
        raise ValueError("WOLF3D.EXE is not LZEXE 0.91")
    pos = (h[11] - info[4] + h[4]) << 4
    out = bytearray()
    word, left = 0, 0

    def bit():
        nonlocal word, left, pos
        if not left:
            word, left = data[pos] | data[pos + 1] << 8, 16
            pos += 2
        value, word, left = word & 1, word >> 1, left - 1
        if not left:
            word, left = data[pos] | data[pos + 1] << 8, 16
            pos += 2
        return value

    word, left = data[pos] | data[pos + 1] << 8, 16
    pos += 2
    while True:
        if bit():
            out.append(data[pos])
            pos += 1
            continue
        if not bit():
            length = ((bit() << 1) | bit()) + 2
            span = data[pos] - 256
            pos += 1
        else:
            low, high = data[pos], data[pos + 1]
            pos += 2
            span = (low | ((high & 0xF8) << 5) | 0xE000) - 0x10000
            length = (high & 7) + 2
            if length == 2:
                length = data[pos]
                pos += 1
                if length == 0:
                    break
                if length == 1:
                    continue
                length += 1
        for _ in range(length):
            out.append(out[len(out) + span])
        if len(out) > 1024 * 1024:
            raise ValueError("LZEXE stream exceeds bound")
    return bytes(out), info


def wolf3d_requirement(exe):
    """Recompute WOLF3D_MIN_BLOCK from the pinned executable's own startup."""
    image, info = unlzexe(exe)
    check(image[0] == 0xBA and image[3:8] == b"\x2e\x89\x16\xae\x02", "WOLF3D.EXE: Borland c0 entry (mov dx,DGROUP)")
    dgroup = struct.unpack_from("<H", image, 1)[0]
    # c0: "mov di,[_stklen]" ... "add di,offset edata" ... "add di,[_heaplen]"
    stack_at = struct.unpack_from("<H", image, 0x72)[0]
    edata = struct.unpack_from("<H", image, 0x83)[0]
    heap_at = struct.unpack_from("<H", image, 0x89)[0]
    stklen = struct.unpack_from("<H", image, dgroup * 16 + stack_at)[0]
    heaplen = struct.unpack_from("<H", image, dgroup * 16 + heap_at)[0]
    layout = dict(image=len(image), dgroup=dgroup, stklen=stklen, heaplen=heaplen, edata=edata)
    check(layout == WOLF3D_LAYOUT and info[:2] == (0, 0), f"WOLF3D.EXE layout {layout}")
    near = 65536 - edata - stklen - 0x400 - 16
    block = 0x10 + dgroup + 0x1000 + (235000 - near + 16 + 15) // 16
    check(block == WOLF3D_MIN_BLOCK, f"Wolf3D minimum block {block:#x} != {WOLF3D_MIN_BLOCK:#x}")
    return block


def guest(ref):
    """The kernel=unified guest files: the qualified installation record when
    run_unified_file.sh produced one, else one local fork build (same sources)."""
    if unified.FORK_QUALIFIED.is_file():
        return unified.guest_files()
    root = OUT / "fork"
    if not (root / "KERNEL.SYS").is_file():
        shutil.rmtree(root, ignore_errors=True)
        record = fork.build_fork(root)
        if record["status"] != "ok":
            raise SystemExit("DOSMEM fork build blocked: " + record.get("error", "?"))
    files, identity = ref.reference_files(h56.release_folder())
    files["/KERNEL.SYS"] = (root / "KERNEL.SYS").read_bytes()
    files["/CIUKIDF.SYS"] = (root / "CIUKIDF.SYS").read_bytes()
    files["/FDCONFIG.SYS"] = unified.fork_config(ref)
    files["/CIUKISH.COM"] = unified.session_shell()
    identity["kernel"] = dict(personality="ciukidos-fork", local_build=True)
    identity["profile"] = dict(kernel="unified", ifs=True)
    return files, identity


def compile_cpu():
    hook = module("dosmem_hook_cpu", ROOT / "tests/host/dos/hook_cpu.py")
    hook.prepare()
    cpu = OUT / "dosvm_vm_cpu"
    subprocess.run(["clang", "-DFS_HOST", "-DCIUKI_DOSVM_FILE_CPU", "-D_POSIX_C_SOURCE=200809L", "-std=c17", "-O1", "-g",
                    "-Wall", "-Wextra", "-Werror", "-Wno-pointer-to-int-cast", "-fsanitize=address,undefined", "-pthread",
                    "-Isrc/kernel/include", "-Ibuild/host", "tests/host/dosvm_guest.c", "tests/host/dosvm_guest_cpu.c",
                    "src/kernel/lib/fmt.c", "src/kernel/lib/sha256.c", "src/kernel/vm/virtual_vga.c",
                    "src/kernel/vm/virtual_vga_bios.c", "src/kernel/vm/vga_x86.c", "src/kernel/vm/vga_presenter.c",
                    *h56.FS, "src/kernel/fs/partition.c", "src/kernel/drivers/blkpart.c", *VM, "-lx86emu", "-o", str(cpu)],
                   cwd=ROOT, check=True)
    return cpu


def run(cpu, image, name, kernel, args, **extra):
    env = dict(os.environ, GUEST_KERNEL=kernel, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1", **extra)
    result = subprocess.run([str(cpu), str(image), *args], cwd=ROOT, env=env, capture_output=True, text=True,
                            timeout=1800)
    log = result.stdout + result.stderr
    (OUT / f"{name}.log").write_text(log)
    return result.returncode, log


def records(log):
    """The probe's record-port lines, reassembled from 48-byte parts."""
    text = "".join(re.findall(r"^  guest: (.*)$", log, re.M))
    return {m.group(1): m.group(2) for m in re.finditer(r"DOSMEM (CONV|MCB end|XMS|EMS) (.*?)(?=DOSMEM |$)", text)}, \
        re.findall(r"([0-9A-F]{4}):([0-9A-F]{4}):([0-9A-F]{4}):(.{8}) ", text)


def fields(line):
    return dict(item.split("=", 1) for item in line.split() if "=" in item)


def measure(cpu, image, name, kernel):
    code, log = run(cpu, image, name, kernel, ["\\DOSMEM.COM", "DOSMEM"])
    check(code == 0 and "-> DOSMEM PASS" in log and "guest runner: PASS" in log, f"{name}: DOSMEM run (exit {code})")
    check("H61 SHELL after DOSMEM exec_pending=0 overflow=0 shell_idle=1" in log,
          f"{name}: the program's return completed its EXEC frame and the shell is idle")
    lines, chain = records(log)
    conv, xms, ems = (fields(lines.get(key, "")) for key in ("CONV", "XMS", "EMS"))
    blocks = [(int(seg, 16), int(owner, 16), int(size, 16), label.rstrip(".")) for seg, owner, size, label in chain]
    print(f"DOSMEM {name}: int12={conv.get('k12')}KB largest={conv.get('largest')}h dos={conv.get('dos')} "
          f"flags={conv.get('flags')} link={conv.get('link')}; XMS 2F={xms.get('2F')} ver={xms.get('ver')} "
          f"hma={xms.get('hma')} freeKB={xms.get('freeKB')} umb={xms.get('umb')}; EMS {lines.get('EMS', '?')}", flush=True)
    for seg, owner, size, label in blocks:
        print(f"DOSMEM {name}: MCB {seg:04X} owner={owner:04X} paragraphs={size:04X} ({size * 16} bytes) {label}")
    return conv, xms, ems, blocks, log


def main():
    if not Path("/usr/include/x86emu.h").exists():
        print("DOSMEM: unavailable (libx86emu development headers missing)")
        return 1
    shutil.rmtree(OUT / "img", ignore_errors=True)
    OUT.mkdir(parents=True, exist_ok=True)
    _, wolf, _ = games.package_files("wolf3d-shareware")
    wolf = {path: data for path, data, _ in wolf}
    minimum = wolf3d_requirement(wolf["/WOLF3D/WOLF3D.EXE"])
    print(f"DOSMEM Wolfenstein 3-D 1.4: mainmem >= 235000 needs a {minimum:#x}-paragraph block "
          f"({minimum * 16} bytes); required with margin {minimum + MARGIN:#x}", flush=True)
    ref = unified.reference_module()
    files, identity = guest(ref)
    cpu = compile_cpu()
    folder = h56.release_folder()
    programs = {"/DOSREADY.COM": h56.assemble(ROOT / "src/probes/doscompat/dosready.asm", OUT / "programs/DOSREADY.COM"),
                "/DOSMEM.COM": h56.assemble(ROOT / "tests/host/dos/dosmem/dosmem.asm", OUT / "programs/DOSMEM.COM")}
    image = OUT / "img/unified/UNIFIED.IMG"
    image.parent.mkdir(parents=True)
    ref.build_reference(image, programs | wolf, ("D48", "WOLF3D"), None, folder, guest=(files, identity))

    conv, xms, ems, blocks, log = measure(cpu, image, "unified", "unified")
    largest = int(conv.get("largest", "0"), 16)
    check(conv.get("k12") == "0280" and conv.get("dos") == "070A", f"unified: 640 KiB, DOS 7.10 {conv}")
    check(xms.get("2F") == "80" and xms.get("ver") == "0300" and xms.get("hma") == "01",
          f"unified: XMS 3.00 visible through INT 2Fh/4300h in front of DOS, HMA present {xms}")
    check(xms.get("umb", "").startswith("0000/B0/") and int(xms["umb"][-4:], 16) == 0x1700,
          f"unified: XMS 10h reports the 92 KiB UMB arena C900h-DFFFh as the largest block {xms}")
    check(ems.get("st") == "00" and ems.get("frame") == "E000" and ems.get("ver") == "40" and
          lines_ems_present(log), f"unified: LIM EMS 4.0, page frame E000h {ems}")
    shell = [size for seg, owner, size, label in blocks if label.startswith("COMMAND") and owner == seg + 1]
    check(shell and all(size <= 0x100 for size in shell),
          f"unified: FreeCOM swapped to XMS while a program runs (resident COMMAND blocks {shell})")
    check(largest + DOSMEM_BLOCK >= minimum + MARGIN,
          f"unified: largest free block {largest:#x} (+{DOSMEM_BLOCK:#x}) below Wolf3D's {minimum:#x} + margin")
    print(f"DOSMEM unified: Wolf3D block at the prompt ~{largest + DOSMEM_BLOCK:#x} paragraphs "
          f"({(largest + DOSMEM_BLOCK) * 16} bytes), margin {(largest + DOSMEM_BLOCK - minimum) * 16} bytes", flush=True)

    # The game itself, started from its directory as the Games menu does. The
    # runner has no x87: GUEST_NO_X87 makes Borland's startup pick its emulator.
    code, log = run(cpu, image, "wolf3d", "unified", ["shell:CD \\WOLF3D", "-", "play:WOLF3D.EXE", "WOLF3D"],
                    GUEST_NO_X87="1", GUEST_PLAY_MS="8000")
    end = re.search(r"^H61 PLAY END WOLF3D (.*)$", log, re.M)
    play = fields(end.group(1)) if end else {}
    last = re.findall(r"^H61 PLAY t=(\d+) mode=(\w+) vbe=\w+ unchained=(\d)", log, re.M)
    check(code == 0 and play.get("returned") == "0" and play.get("stopped") == "0" and last and
          int(last[-1][0]) >= 8000 and last[-1][1] == "13" and last[-1][2] == "1",
          f"wolf3d: still in its game graphics after 8 s (play {play}, last sample {last[-1:] or None})")
    print(f"DOSMEM wolf3d: returned={play.get('returned')} stopped={play.get('stopped')} "
          f"last sample t={last[-1][0] if last else '?'} mode={last[-1][1] if last else '?'} "
          f"unchained={last[-1][2] if last else '?'}", flush=True)

    # The pristine FreeDOS 1.3 reference base: same VMM, XMS visible as the
    # f3-48 "multiplex" case expects.
    reference = OUT / "img/freedos/FREEDOS.IMG"
    reference.parent.mkdir(parents=True)
    ref.build_reference(reference, programs, ("D48",), None, folder)
    conv, xms, _, _, _ = measure(cpu, reference, "freedos-1.3", "freedos-1.3")
    check(xms.get("2F") == "80", f"freedos-1.3: XMS visible through INT 2Fh/4300h {xms}")

    shutil.rmtree(OUT / "img", ignore_errors=True)
    if failures:
        print(f"DOSMEM FAIL: {len(failures)} checks failed")
        return 1
    print("DOSMEM PASS: XMS visible in front of the guest INT 2Fh chain, FreeCOM swapped, "
          "Wolfenstein 3-D 1.4 memory check passed with margin and the game runs")
    return 0


def lines_ems_present(log):
    return re.search(r"DOSMEM EMS 1 ", "".join(re.findall(r"^  guest: (.*)$", log, re.M))) is not None


if __name__ == "__main__":
    sys.exit(main())
