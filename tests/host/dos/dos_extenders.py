"""DOS extenders in the DOS Box (DOSX). No QEMU, systemd scope or OS image.

Boots the kernel=unified guest exactly as the canonical builder installs it
(the same guest files as tests/host/dos/dos_memory.py) on the production VM
code (tests/host/dosvm_guest.c with libx86emu) and runs two DOS/4GW clients
of the VMM's DPMI host:

- DPMI4GW.EXE (src/probes/vm/dpmi_dos4gw.c, built with Open Watcom against
  its pinned binw/dos4gw.exe, DOS/4GW 1.97): DPMI services, IRQ0, virtual IF,
  INT 21h 2Ah/2Ch/2Ah and file I/O through the extender; it must report PASS.
- The Warcraft II demo v1.20 (third_party/games/war2-demo, WAR2.EXE bound to
  DOS/4GW Professional 1.97) started from \\WAR2 as the DOS Box types it, with
  and without the EMMXXXX0 device (CIUKIEMM.SYS): it must pass the extender
  start, its INT 21h 2Ch call, its timer setup (nested PUSHFD/CLI regions)
  and find DRIVERS\\MDI.INI and DRIVERS\\DIG.INI, then stop at its own video
  check ("could not find a VESA video driver", UNIVBE.DRV is written by
  UVCONFIG on the user's machine), with no DOS/4GW error.

Regression cover for 2026-10-25 (docs/design/dos-dpmi-contract.md,
"Selector 40h" and "Nested saved flags images"): DOS/4GW's own INT 21h
2Ah/2Ch handlers load GDT selector 40h, which the VMM did not provide
("DOS/4GW Professional error (2001): exception 0Dh at C7:000014AF", ES=0),
and the demo's nested Watcom IF-restore idiom orphaned a planted TF
("exception 01h at 1F7:002427BA"). The runner has no x87: GUEST_NO_X87 makes
it a machine without a coprocessor, so Watcom's start-up finds no 8087.
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


def run(cpu, image, name, args, **extra):
    env = dict(os.environ, GUEST_KERNEL="unified", GUEST_NO_X87="1", ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1", **extra)
    result = subprocess.run([str(cpu), str(image), *args], cwd=ROOT, env=env, capture_output=True, text=True,
                            timeout=1800)
    log = result.stdout + result.stderr
    (OUT / f"{name}.log").write_text(log)
    return result.returncode, log


def war2(cpu, image, name):
    code, log = run(cpu, image, name, ["shell:CD \\WAR2", "-", "play:WAR2.EXE", "WAR2"], GUEST_PLAY_MS="8000")
    text = "\n".join(screen(log))
    end = re.search(r"^H61 PLAY END WAR2 (.*)$", log, re.M)
    missing = re.findall(r"^H61 IFS play failed n=\d+ fn=\w+ error=\d+ name=(\S+)$", log, re.M)
    print(f"DOSX {name}: exit={code} end=[{end.group(1) if end else '?'}] failed opens={missing}", flush=True)
    check(code == 0 and "guest runner: PASS" in log and "unexpected:" not in log, f"{name}: runner verdict (exit {code})")
    check("DOS/4GW Professional Protected Mode Run-time  Version 1.97" in text, f"{name}: the bound extender started")
    check("DOS/4GW Professional error" not in text, f"{name}: no DOS/4GW error on the screen")
    check("Warcraft could not find a VESA video driver" in text,
          f"{name}: the demo reached its own video check (no UNIVBE.DRV yet)")
    check(not {"MDI.INI", "DIG.INI"} & set(missing), f"{name}: DRIVERS\\MDI.INI and DRIVERS\\DIG.INI found")
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
    demo.update({path: data for path, data, _ in games.generated_files() if path.startswith("/WAR2/")})
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

    war2(cpu, image, "war2-ems")
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
    war2(cpu, image, "war2-no-ems")

    shutil.rmtree(image.parent, ignore_errors=True)
    if failures:
        print(f"DOSX FAIL: {len(failures)} checks failed (logs in {OUT})")
        return 1
    print("DOSX PASS: DOS/4GW 1.97 client (INT 21h date/time through selector 40h) and the Warcraft II demo under "
          "DOS/4GW Professional 1.97, with and without the EMMXXXX0 device, reach their own code with no extender error")
    return 0


if __name__ == "__main__":
    sys.exit(main())
