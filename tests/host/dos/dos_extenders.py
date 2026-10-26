"""DOS extenders in the DOS Box (DOSX). No QEMU, systemd scope or OS image.

Boots the kernel=unified guest exactly as the canonical builder installs it
(the same guest files as tests/host/dos/dos_memory.py) on the production VM
code (tests/host/dosvm_guest.c with libx86emu) and runs the DOS/4GW clients
of the VMM's DPMI host:

- DPMI4GW.EXE (src/probes/vm/dpmi_dos4gw.c, built with Open Watcom against
  its pinned binw/dos4gw.exe, DOS/4GW 1.97): DPMI services, IRQ0, virtual IF,
  INT 21h 2Ah/2Ch/2Ah and file I/O through the extender; it must report PASS.
- The Warcraft II demo v1.20 (third_party/games/war2-demo, WAR2.EXE bound to
  DOS/4GW Professional 1.97) started from \\WAR2 as the DOS Box types it, with
  and without the EMMXXXX0 device (CIUKIEMM.SYS): it must pass the extender
  start, its INT 21h 2Ch call, its timer setup (nested PUSHFD/CLI regions),
  find DRIVERS\\MDI.INI and DRIVERS\\DIG.INI and pass its VESA check on the
  VM's VBE BIOS with no native display provider (war2vbe, 2026-10-25: INT 10h
  4F00h, 4F01h, 4F02h 101h windowed, 4F0Ah, 4F05h bank switches; no
  UNIVBE.DRV), with no DOS/4GW error. With the EMMXXXX0 device the run goes
  on: Esc skips the Blizzard logo and the intro movie (mode 13h) and the
  window shows the 640x480x256 title screen (mode 101h, the presenter's
  frame, GUEST_PLAY_SHOT=<file.ppm> saves it).

- Duke Nukem 3D v1.3d shareware (fx-duke3d), through DUKE3D.BAT and its
  bound DOS/4GW Professional extender, with and without EMMXXXX0.

Regression cover for 2026-10-25 (docs/design/dos-dpmi-contract.md,
"Selector 40h" and "Nested saved flags images"): DOS/4GW's own INT 21h
2Ah/2Ch handlers load GDT selector 40h, which the VMM did not provide
("DOS/4GW Professional error (2001): exception 0Dh at C7:000014AF", ES=0),
and the demo's nested Watcom IF-restore idiom orphaned a planted TF
("exception 01h at 1F7:002427BA"). The runner has no x87: GUEST_NO_X87 makes
it a machine without a coprocessor, so Watcom's start-up finds no 8087.
For Duke, leave FPU detection enabled: its CRT's no-coprocessor path reads
a privileged control register, whereas every supported hardware profile has
an x87. The runner implements control/status detection only and stops at
the pinned client's first FLDZ (D9 EE), before reading DUKE3D.CFG. This
exact frontier qualifies extender startup only, never graphics or audio.
Host replay, not QEMU or hardware evidence. SPDX-License-Identifier: GPL-2.0-only
"""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "build/host/dos/extenders"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


memory = module("dosx_memory", ROOT / "tests/host/dos/dos_memory.py")
h56, unified, games = memory.h56, memory.unified, memory.games
dos = module("dosx_dos_build", ROOT / "scripts/build_dos_image.py")
failures = []
EMS_LINE = unified.EMS_DEVICE_LINE


def check(condition, message):
    if not condition:
        failures.append(message)
        print("DOSX CHECK FAILED: " + message, flush=True)


def screen(log):
    return [line[4:] for line in log.splitlines() if line.startswith("  | ")]


def run(cpu, image, name, args, *, no_x87=True, **extra):
    env = dict(os.environ, GUEST_KERNEL="unified", GUEST_NO_X87="1", ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1", **extra)
    if not no_x87:
        env.pop("GUEST_NO_X87", None)
    result = subprocess.run([str(cpu), str(image), *args], cwd=ROOT, env=env, capture_output=True, text=True,
                            timeout=1800)
    log = result.stdout + result.stderr
    (OUT / f"{name}.log").write_text(log)
    return result.returncode, log


def war2(cpu, image, name, menu):
    # INT 10h trace (dosvm_guest.c GUEST_INT10_TRACE); with menu, Esc at 12 s
    # and 14 s of simulated time skips the logo and intro movies.
    extra = dict(GUEST_PLAY_MS="24000" if menu else "10000", GUEST_INT10_TRACE="1")
    if menu:
        extra["GUEST_PLAY_KEYS"] = "01@12000,01@14000"
    code, log = run(cpu, image, name, ["shell:CD \\WAR2", "-", "play:WAR2.EXE", "WAR2"], **extra)
    text = "\n".join(screen(log))
    end = re.search(r"^H61 PLAY END WAR2 (.*)$", log, re.M)
    missing = re.findall(r"^H61 IFS play failed n=\d+ fn=\w+ error=\d+ name=(\S+)$", log, re.M)
    calls = re.findall(r"^I10 n=\d+ ax=(\w+) bx=(\w+) cx=\w+ dx=\w+ es:di=\S+ -> e=0 ax=(\w+) bx=\w+ cx=(\w+)", log, re.M)
    vbe = [(ax, bx, result, cx) for ax, bx, result, cx in calls if ax.startswith("4f")]
    banks = sum(ax == "4f05" and result == "004f" for ax, _, result, _ in vbe)
    frame = re.search(r"^H61 PLAY FRAME e=(-?\d+) mode=(\w+) (\d+)x(\d+) text=(\d) nonblack=(\d+)$", log, re.M)
    print(f"DOSX {name}: exit={code} end=[{end.group(1) if end else '?'}] failed opens={missing} "
          f"vbe calls={len(vbe)} bank switches={banks} frame=[{frame.group(0)[15:] if frame else '-'}]", flush=True)
    check(code == 0 and "guest runner: PASS" in log and "unexpected:" not in log, f"{name}: runner verdict (exit {code})")
    check("DOS/4GW Professional Protected Mode Run-time  Version 1.97" in text or
          'console="Copyright (c) Rational Systems, Inc. 1990-1994"' in log, f"{name}: the bound extender started")
    check("DOS/4GW Professional error" not in text and "DOS/4GW Professional error" not in log,
          f"{name}: no DOS/4GW error")
    check(not {"MDI.INI", "DIG.INI"} & set(missing), f"{name}: DRIVERS\\MDI.INI and DRIVERS\\DIG.INI found")
    # The VESA check passes on the VM's VBE BIOS: no message, no UNIVBE.DRV.
    check("Warcraft could not find a VESA video driver" not in text and "UNIVBE.DRV" not in missing,
          f"{name}: the demo passed its VESA check without UNIVBE.DRV")
    check(any(ax == "4f00" and result == "004f" for ax, _, result, _ in vbe), f"{name}: INT 10h 4F00h answered")
    check(any(ax == "4f01" and result == "004f" and cx == "0101" for ax, _, result, cx in vbe), f"{name}: 4F01h 101h")
    check(any(ax == "4f02" and bx == "0101" and result == "004f" for ax, bx, result, _ in vbe),
          f"{name}: 4F02h 101h set in the windowed model")
    check(any(ax == "4f0a" and result == "004f" for ax, _, result, _ in vbe), f"{name}: 4F0Ah protected-mode table")
    check(banks >= 5, f"{name}: 4F05h bank switches through the 4F0Ah SetWindow code ({banks})")
    if menu:
        check(frame is not None and frame.group(1) == "0" and frame.group(2) == "101" and frame.group(3) == "640" and
              frame.group(4) == "480" and frame.group(5) == "0" and int(frame.group(6)) > 150000,
              f"{name}: the window shows the 640x480x256 title screen")
    return log


def war2_perf(image):
    """t23-dosperf (docs/validation/t23-dos-window-lag-2026-10-25.md), only
    with DOSX_PERF=1: the demo for 30 simulated seconds under the fx-27 T23
    time model, one GUEST_PLAY_PERF line per second (exit causes, lean
    exits, CLI proof cache, seed demand, DAC bursts, armed flags images).
    The model charges host code time, so the runner is built as
    memory.compile_cpu builds it but with -O2 and no sanitizers."""
    cpu = OUT / "dosvm_vm_cpu_o2"
    subprocess.run(["clang", "-DFS_HOST", "-DCIUKI_DOSVM_FILE_CPU", "-D_POSIX_C_SOURCE=200809L", "-std=c17", "-O2", "-g",
                    "-Wall", "-Wextra", "-Werror", "-Wno-pointer-to-int-cast", "-pthread",
                    "-Isrc/kernel/include", "-Ibuild/host", "tests/host/dosvm_guest.c", "tests/host/dosvm_guest_cpu.c",
                    "src/kernel/lib/fmt.c", "src/kernel/lib/sha256.c", "src/kernel/vm/virtual_vga.c",
                    "src/kernel/vm/virtual_vga_bios.c", "src/kernel/vm/vga_x86.c", "src/kernel/vm/vga_presenter.c",
                    *h56.FS, "src/kernel/fs/partition.c", "src/kernel/drivers/blkpart.c", *memory.VM, "-lx86emu",
                    "-o", str(cpu)], cwd=ROOT, check=True)
    code, log = run(cpu, image, "war2-perf", ["shell:CD \\WAR2", "-", "play:WAR2.EXE", "WAR2"],
                    GUEST_PLAY_MS="30000", GUEST_PLAY_KEYS="01@12000,01@14000", GUEST_PLAY_PERF="1",
                    GUEST_PERF="1", GUEST_T23="1", GUEST_T23_MODEL="fx27", GUEST_SLICE="200000")
    rows = [dict(re.findall(r"(\w+)=([0-9.]+)", line)) for line in log.splitlines() if line.startswith("H61 PERF ")]
    check(code == 0 and "guest runner: PASS" in log and len(rows) > 12, f"war2-perf: runner verdict (exit {code})")
    if len(rows) > 12:
        a, b = rows[6], rows[11]           # the intro movie (mode 13h), simulated 6-11 s
        span = (float(b["t"]) - float(a["t"])) / 1000
        rate = {k: (float(b[k]) - float(a[k])) / span for k in ("traps", "lean", "missed", "unprovable", "dac")}
        print("DOSX war2-perf intro per second: " + " ".join(f"{k}={v:.0f}" for k, v in rate.items()), flush=True)


def duke3d(cpu, image, name):
    """Unmodified v1.3d, through its installed batch and bound DOS/4GW.
    This harness has no x87 arithmetic: assert extender startup, never claim
    title-screen/gameplay/audio qualification from a host replay."""
    code, log = run(cpu, image, name, ["shell:CD \\DUKE3D", "-", "play:DUKE3D.BAT", "DUKE3D"],
                    no_x87=False, GUEST_PLAY_MS="10000", GUEST_INT10_TRACE="1", GUEST_INT21_TRACE="1")
    text = "\n".join(screen(log))
    end = re.search(r"^H61 PLAY END DUKE3D (.*)$", log, re.M)
    print(f"DOSX {name}: exit={code} end=[{end.group(1) if end else '?'}]", flush=True)
    # Only this exact unimplemented instruction of the pinned executable is
    # an accepted host frontier. Any other exception or emulator error fails.
    frontier = "x87 instruction d9 ee at 01f7:00295687" in log
    unexpected = re.findall(r"^unexpected: (.*)$", log, re.M)
    limited = (code == 1 and frontier and len(unexpected) == 1 and
               unexpected[0].startswith("x87 instruction outside the runner's control/status subset at "))
    check(limited or (code == 0 and "guest runner: PASS" in log and not unexpected),
          f"{name}: runner verdict (exit {code})")
    check("DOS/4GW Professional Protected Mode Run-time" in text or
          'console="Copyright (c) Rational Systems, Inc. 1990-1994"' in log,
          f"{name}: the bound DOS/4GW extender started")
    check("DOS/4GW Professional error" not in log and "DOS/4GW error" not in log,
          f"{name}: no extender error")
    check(re.search(r"^H61 PLAY .*dpmi=1 clients=[1-9]", log, re.M) is not None,
          f"{name}: protected-mode client reached the production DPMI host")
    check(any(int(n) > 0 for n in re.findall(r"^H61 PLAY .*irq_pm=(\d+)", log, re.M)),
          f"{name}: protected-mode timer interrupts delivered")
    check(re.search(r'I21 .*path="[^"\n]*DUKESB\.COM".*cf=0', log, re.I) is not None,
          f"{name}: the installed sound helper was read")
    if limited:
        print(f"DOSX {name}: STARTUP ONLY; libx86emu stops at FLDZ 01f7:00295687, before CFG/video/audio", flush=True)
    else:
        check(re.search(r'I21 .*ax=3d[0-9a-f]{2} .*path="[^"\n]*DUKE3D\.CFG".*cf=0', log, re.I) is not None,
              f"{name}: the game configuration was read successfully")
    check("sb_irq=5 sb_dma8=1 sb_dma16=5" in log, f"{name}: helper selected IRQ 5 and DMA 1/5")
    if end and "returned=1" in end.group(1):
        progress = [line for line in log.splitlines() if line.startswith("H61 PLAY t=")]
        check(progress and "sb_irq=7 sb_dma8=1 sb_dma16=5" in progress[-1],
              f"{name}: launcher restored the VM's default IRQ after return")
    return log


def main():
    if not Path("/usr/include/x86emu.h").exists():
        print("DOSX: unavailable (libx86emu development headers missing)")
        return 1
    if not (dos.WATCOM / "binl64/wcl386").is_file():
        print(f"DOSX: unavailable (Open Watcom missing under {dos.WATCOM})")
        return 1
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    ref = unified.reference_module()
    files, identity = memory.guest(ref)
    cpu = memory.compile_cpu()
    folder = h56.release_folder()
    with tempfile.TemporaryDirectory(prefix="watcom-", dir=OUT) as work:
        client, extender = dos.watcom_build("src/probes/vm/dpmi_dos4gw.c", "DPMI4GW.EXE", Path(work))
    programs = {"/DOSREADY.COM": h56.assemble(ROOT / "src/probes/doscompat/dosready.asm", OUT / "DOSREADY.COM"),
                "/DPMI4GW.EXE": client, "/DOS4GW.EXE": extender}
    _, members, _ = games.package_files("war2-demo")
    demo = {path: data for path, data, _ in members}
    _, duke, _ = games.package_files("duke3d-shareware")
    demo.update({path: data for path, data, _ in duke})
    demo.update({path: data for path, data, _ in games.generated_files() if path.startswith(("/WAR2/", "/DUKE3D/"))})
    folders = sorted({"/".join(p.strip("/").split("/")[:k]) for p in demo for k in range(1, p.strip("/").count("/") + 1)},
                     key=lambda d: (d.count("/"), d))
    image = OUT / "img/UNIFIED.IMG"
    image.parent.mkdir(parents=True)
    ref.build_reference(image, programs | demo, ("D48", *folders), None, folder, guest=(files, identity))

    code, log = run(cpu, image, "dpmi4gw", ["\\DPMI4GW.EXE", "DPMI4GW"])
    verdict = re.search(r"^  -> (DPMI4GW \S+.*)$", log, re.M)
    print(f"DOSX DOS/4GW 1.97 client: {verdict.group(1) if verdict else 'no verdict'}", flush=True)
    check(code == 0 and verdict and verdict.group(1).startswith("DPMI4GW PASS") and "guest runner: PASS" in log,
          f"DPMI4GW.EXE: DPMI services, IRQ0, virtual IF, INT 21h 2Ah/2Ch and file I/O under DOS/4GW (exit {code})")
    check("DOS/4GW error" not in "\n".join(screen(log)), "DPMI4GW.EXE: no DOS/4GW error on the screen")

    war2(cpu, image, "war2-ems", True)
    duke3d(cpu, image, "duke3d-ems")
    if os.environ.get("DOSX_PERF"):
        war2_perf(image)
    # The same guest without the EMMXXXX0 device: DOS/4GW Professional probes
    # VCPI/EMS/XMS/DPMI itself; the VMM has EMS 4.0 but no VCPI (INT 67h
    # DE00h fails), so both profiles must take the DPMI path.
    plain = dict(files)
    plain["/FDCONFIG.SYS"] = files["/FDCONFIG.SYS"].replace(EMS_LINE, b"")
    plain.pop("/CIUKIEMM.SYS")
    check(plain["/FDCONFIG.SYS"] != files["/FDCONFIG.SYS"], "the EMMXXXX0 device line was removed")
    shutil.rmtree(image.parent)
    image.parent.mkdir(parents=True)
    ref.build_reference(image, programs | demo, ("D48", *folders), None, folder, guest=(plain, identity))
    war2(cpu, image, "war2-no-ems", False)
    duke3d(cpu, image, "duke3d-no-ems")

    shutil.rmtree(image.parent, ignore_errors=True)
    if failures:
        print(f"DOSX FAIL: {len(failures)} checks failed (logs in {OUT})")
        return 1
    print("DOSX PASS: DOS/4GW 1.97 client (INT 21h date/time through selector 40h) and the Warcraft II demo under "
          "DOS/4GW Professional 1.97, with and without the EMMXXXX0 device, pass the extender and VESA starts; "
          "the demo's 640x480x256 title screen reaches the window; Duke Nukem 3D v1.3d passes DOS/4GW startup")
    return 0


if __name__ == "__main__":
    sys.exit(main())
