"""Real Quake EXEC/CRT regression on the production DOS VM and host CPU.

Creates only a temporary, sparse test FAT16 fixture; never builds or copies
the canonical image. No game data or altered executable is used.
"""
# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    runner = Path(sys.argv[1]).resolve()
    dos = load("exec_dos_fixture", "scripts/build_dos_image.py")
    games = load("exec_games_payload", "scripts/build_games_disk.py")
    record = games.upstream("quake-shareware")
    _, members = games.unpack(record, games.archive("quake-shareware", record))
    exe = members["QUAKE.EXE"]
    pin = next(p for p in record["install"]["files"] if p["name"] == "QUAKE.EXE")
    assert len(exe) == pin["size"] and games.sha256(exe) == pin["sha256"]
    print(f"f3-28 pinned QUAKE.EXE: {len(exe)} bytes sha256={pin['sha256']}", flush=True)
    scratch = ROOT / "build/host"
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="dos-exec-", dir=scratch) as folder:
        work = Path(folder)

        def asm(source, name, definitions=None):
            return dos.assemble(source, work / name, definitions)

        boot = asm("src/boot/full_boot.asm", "boot.bin", {"FAT_TOTAL_SECTORS": dos.SECTORS})
        loader = asm("src/boot/full_stage1_loader.asm", "loader.bin", dos.GEOMETRY)
        files = {
            "/SYSTEM/CIUKIDOS.SYS": asm("src/runtime/ciukidos.asm", "CIUKIDOS.SYS", {**dos.GEOMETRY, **dos.PERSONALITY}),
            "/SYSTEM/SHELL.COM": asm("src/com/dosvm_shell.asm", "SHELL.COM"),
            "/COMMAND.COM": asm("src/com/shell.asm", "COMMAND.COM", {"COMMAND_COMPAT": 1}),
            "/DOSREADY.COM": asm("src/probes/doscompat/dosready.asm", "DOSREADY.COM"),
            "/RUNREC.COM": asm("src/probes/dos/run_record.asm", "RUNREC.COM"),
            "/AUTOEXEC.BAT": b"\\DOSREADY.COM\r\n",
            "/QUAKE/QUAKE.EXE": exe,
        }
        for width in (16, 32):
            for source, name in (("dpmi_client", f"DPMI{width}.COM"),
                                 ("dpmi_child", f"DPMICH{width}.COM")):
                files["/" + name] = asm(f"src/probes/dos/{source}.asm", name, {"BITS32": int(width == 32)})
        fixture = work / "exec.fat16"
        dos.write_fat16(fixture, boot, loader, files, directories=("APPS", "QUAKE"))
        env = dict(os.environ, GUEST_BUDGET="10000000")
        # Exercise both corrected conformance probes and their nested EXEC
        # children without depending on a previously built DOSBOOT image.
        subprocess.run([str(runner), str(fixture), "\\DPMI16.COM", "DPMI16", "\\DPMI32.COM", "DPMI32"],
                       cwd=ROOT, env=env, timeout=60, check=True)
        result = subprocess.run([str(runner), str(fixture),
                                 "quake:\\RUNREC.COM \\QUAKE QUAKE.EXE +playdemo demo1", "RUNREC"],
                                cwd=ROOT, env=env, timeout=60)
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
