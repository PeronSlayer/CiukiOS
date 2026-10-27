"""fx-doswin3: DOS window/full-screen parity replay at the T23 model (WPAR).

No QEMU, systemd scope or OS image. Opt-in measurement tool, not a suite.
Boots the kernel=unified guest (tests/host/dos/dos_memory.py) with the
pinned DOOM 1.9 shareware, Wolfenstein 3-D 1.4 shareware and Warcraft II
demo packages on the production VM code (tests/host/dosvm_guest.c with
libx86emu, built -O2 without sanitizers as dos_extenders.war2_perf builds
it) and runs each title twice under the fx-27 T23 time model
(GUEST_T23_MODEL=fx27):

- window: every monitor exit costs the T23's price for its cause, and the
  DOS Box presenter runs at the production cadence; each changed frame
  charges the T23 presentation price of its written rows
  (GUEST_T23_PRESENT, dosvm_guest.c);
- full screen: GUEST_T23_FULLSCREEN, the VGA ports and aperture are the
  hardware's (VGA exits charged 1 us, no presentation).

Per steady interval it reports rendered frames (CRTC start changes) per
simulated second, guest instructions per simulated second (the game's CPU),
presented frames and the charged monitor/presentation time, and the
window/full-screen factor. The instruction clock is calibrated to the
trial's DOOM window bank (guest time per page flip, WPAR_SLICE). Host replay:
not QEMU or hardware evidence. Usage:

  python3 -B tests/host/dos/window_parity.py [--games doom,wolf3d,war2]
      [--modes window,full] [--seconds 30] [--present K,B,D,F] [--tag NAME]

SPDX-License-Identifier: GPL-2.0-only
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "build/host/dos/window-parity"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


memory = module("wpar_memory", ROOT / "tests/host/dos/dos_memory.py")
h56, unified, games = memory.h56, memory.unified, memory.games

# The T23 presentation price of one written 640-pixel XRGB row of the DOS
# Box's 640x480 VM surface, in ns, and of one changed frame, in us
# (trial 5bb73dba, docs/validation/fx-doswin3.md, round 2): kernel
# conversion and surface write (vm_perf_present present_us / rows: Wolf
# 176473/12716, DOOM 102724/7372 = 13.9 us; Warcraft II VBE8 231547/9659 =
# 24.0 us), the DOS Box viewport copy (dosbox user time per frame) and the
# desktop (desktop CPU above its full-screen level per frame). Overridden
# per run with --present KERNEL,DOSBOX,DESKTOP,FRAME.
PRESENT_TRIAL = "13900,7000,50000,250"
# Round 3 (fx-doswin3): an unobscured DOS window's damage presented straight
# from its surface (compositor.c direct_present): the desktop stage is the
# upload alone. The host replay of the production pipeline
# (tests/host/desktop/dos_window_replay.c, dos-window-direct) presents
# 241,280 pixels in one request for a full Mode Y Fit frame of 400 VM rows:
# 965,120 bytes at the T23's 134 MB/s upload rate (f4-20) = 7.20 ms, 18.0 us
# per VM row, against the composed path's 241,280 composed pixels, 512,000
# upload bytes and 354 requests (50 us per row, trial-derived).
PRESENT_DIRECT = "13900,7000,18000,250"
# Guest instructions per simulated ms (GUEST_SLICE), calibrated in round 2:
# DOOM's attract demo executes about 1.07 M runner instructions per frame;
# the trial's DOOM window bank (n=4) spent 1,038 ms of guest time for 87
# flips, of which about 1 us per exit for its 174 k exits is the hardware
# trap round trip outside the monitor's stamps: 864 ms / 87 = 9.9 ms of
# guest execution per frame, 108 instructions per us.
SLICE = 108000
# T23 price of an exit's hardware round trip and of one INVLPG, ns
# (model assumptions, stated in docs/validation/fx-doswin3.md round 2).
TRAP_NS = 1000.0
INVLPG_NS = 100.0
# Calibration of the window's VGA prices to trial 5bb73dba (round 2): the
# fx-27 lean-exit model charged DOOM's map-mask OUTs 9.07 us plus 1.38 us
# of INVLPGs against the trial's 7.24 us per VGA I/O exit (bank n=4):
# (7.24 - 1.38) / 9.07 = 0.65. PF_SCALE from the Wolfenstein 3-D window
# replay over the sources the trial ran (be7852468) against its 233 us per
# aperture fault (bank n=6).
VGA_SCALE = 0.65            # fx27 model only; the fixed model's VGA price is the trial's (scale 1)
PF_SCALE = 1.0
# A locked XCHG on the P6 family, ns (Agner Fog's tables: about 20-25 clocks
# for XCHG r,m); only for sources whose remap exchanges each changed PTE
# (this round). Pass --xchg-ns 0 for earlier sources.
XCHG_NS = 25.0
# Title runs: arguments, extra environment, steady interval (simulated s).
TITLES = {
    # DOOM's attract loop: graphics from about 8 s, TITLEPIC, then demo1
    # from about 13 s of the play run (host replay, 2026-10-27).
    "doom": (["shell:CD \\DOOM", "-", "play:DOOM.EXE -config CIUKI.CFG", "DOOM"], {}, (25, None)),
    # TEDLEVEL 0 NOWAIT (ID_US_1.C US_Startup) starts E1M1 at once; the
    # player turns right (keypad 6) so every frame differs.
    "wolf3d": (["shell:CD \\WOLF3D", "-", "play:WOLF3D.EXE TEDLEVEL 0 NOWAIT", "WOLF3D"],
               {"GUEST_NO_X87": "1", "GUEST_PLAY_HOLD": "4d@12500-600000"}, (14, None)),
    # Esc at 12 s and 14 s skips the logo and intro movies (dos_extenders);
    # the 640x480x256 title screen animates its palette every retrace.
    "war2": (["shell:CD \\WAR2", "-", "play:WAR2.EXE", "WAR2"],
             {"GUEST_NO_X87": "1", "GUEST_PLAY_KEYS": "01@12000,01@14000"}, (17, None)),
}


def compile_runner():
    module("wpar_hook_cpu", ROOT / "tests/host/dos/hook_cpu.py").prepare()
    cpu = OUT / "dosvm_vm_cpu_o2"
    subprocess.run(["clang", "-DFS_HOST", "-DCIUKI_DOSVM_FILE_CPU", "-D_POSIX_C_SOURCE=200809L", "-std=c17", "-O2", "-g",
                    "-Wall", "-Wextra", "-Werror", "-Wno-pointer-to-int-cast", "-pthread",
                    "-Isrc/kernel/include", "-Ibuild/host", "tests/host/dosvm_guest.c", "tests/host/dosvm_guest_cpu.c",
                    "src/kernel/lib/fmt.c", "src/kernel/lib/sha256.c", "src/kernel/vm/virtual_vga.c",
                    "src/kernel/vm/virtual_vga_bios.c", "src/kernel/vm/vga_x86.c", "src/kernel/vm/vga_presenter.c",
                    *h56.FS, "src/kernel/fs/partition.c", "src/kernel/drivers/blkpart.c", *memory.VM, "-lx86emu",
                    "-o", str(cpu)], cwd=ROOT, check=True)
    return cpu


def build_image():
    image = OUT / "img/UNIFIED.IMG"
    if image.is_file():
        return image
    ref = unified.reference_module()
    files, identity = memory.guest(ref)
    folder = h56.release_folder()
    content = {}
    for package in ("doom-shareware", "wolf3d-shareware", "war2-demo"):
        _, members, _ = games.package_files(package)
        content.update({path: data for path, data, _ in members})
    content.update({path: data for path, data, _ in games.generated_files()})
    folders = sorted({"/".join(p.strip("/").split("/")[:k]) for p in content
                      for k in range(1, p.strip("/").count("/") + 1)}, key=lambda d: (d.count("/"), d))
    programs = {"/DOSREADY.COM": h56.assemble(ROOT / "src/probes/doscompat/dosready.asm", OUT / "DOSREADY.COM")}
    image.parent.mkdir(parents=True, exist_ok=True)
    ref.build_reference(image, programs | content, ("D48", *folders), None, folder, guest=(files, identity))
    return image


def run(cpu, image, title, mode, seconds, slice_, present, tag, more=(), delay=0, trap_ns=0, invlpg_ns=0,
        vga_scale=1.0, pf_scale=1.0, share=0.2, model="fixed", xchg_ns=0.0, desktop_share=0.25, ack=0,
        desktop_hz=60.0):
    args, extra, _ = TITLES[title]
    extra = dict(extra, **dict(item.split("=", 1) for item in more))
    env = dict(os.environ, GUEST_KERNEL="unified", ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1", GUEST_PLAY_MS=str(seconds * 1000), GUEST_PLAY_PERF="1",
               GUEST_PERF="1", GUEST_T23="1", GUEST_T23_MODEL=model, GUEST_SLICE=str(slice_),
               GUEST_T23_PRESENT=present, GUEST_T23_PRESENT_DELAY=str(delay), GUEST_T23_TRAP_NS=str(trap_ns),
               GUEST_T23_INVLPG_NS=str(invlpg_ns), GUEST_T23_VGA_SCALE=str(vga_scale),
               GUEST_T23_PF_SCALE=str(pf_scale), GUEST_T23_PRESENT_SHARE=str(share), GUEST_T23_XCHG_NS=str(xchg_ns),
               GUEST_T23_DESKTOP_SHARE=str(desktop_share), GUEST_T23_STRETCH_SPLIT="1",
               GUEST_T23_ACK=str(ack), GUEST_T23_DESKTOP_HZ=str(desktop_hz),
               **extra)
    if mode == "full":
        env["GUEST_T23_FULLSCREEN"] = "1"
    result = subprocess.run([str(cpu), str(image), *args], cwd=ROOT, env=env, capture_output=True, text=True,
                            timeout=7200)
    log = result.stdout + result.stderr
    (OUT / f"{tag}-{title}-{mode}.log").write_text(log)
    return result.returncode, log


def rows(log, kind):
    return [{k: int(v, 16) if k == "mode" else float(v) for k, v in re.findall(r"(\w+)=(-?[0-9a-f.]+)\b", line)
             if k == "mode" or re.fullmatch(r"-?[0-9.]+", v)}
            for line in log.splitlines() if line.startswith(f"H61 {kind} t=")]


def rates(log, interval):
    play, perf = rows(log, "PLAY"), rows(log, "PERF")
    if len(play) < 3 or len(perf) < 3:
        return None
    first, last = interval
    pick = [i for i, p in enumerate(play) if p["t"] >= first * 1000 and (last is None or p["t"] <= last * 1000)]
    if len(pick) < 2:
        return None
    a, b = pick[0], pick[-1]
    span = (play[b]["t"] - play[a]["t"]) / 1000.0
    out = {"from_s": play[a]["t"] / 1000, "to_s": play[b]["t"] / 1000,
           "flips_s": (play[b]["flips"] - play[a]["flips"]) / span,
           "minstr_s": (play[b]["instr"] - play[a]["instr"]) / span / 1e6,
           "mode": "%02x" % int(play[b]["mode"]), "vbe": int(play[b]["vbe"])}
    for key in ("t23_ms", "t23_vga_ms", "t23_db_ms", "present_frames", "present_rows", "present_ms", "off_ms",
                "traps", "lean", "remaps", "db", "vga_io", "ret", "present_wakes", "invlpg", "trap_ms", "invlpg_ms",
                "shown_frames", "shown_rows", "ack_timeouts"):
        if key in perf[a] and key in perf[b]:
            out[key + "_s"] = (perf[b][key] - perf[a][key]) / span
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--games", default="doom,wolf3d,war2")
    p.add_argument("--modes", default="window,full")
    p.add_argument("--seconds", type=int, default=30)
    p.add_argument("--slice", type=int, default=int(os.environ.get("WPAR_SLICE", str(SLICE))))
    p.add_argument("--delay", type=int, default=0, help="presenter dispatch delay after its wake, ms")
    p.add_argument("--trap-ns", type=float, default=TRAP_NS)
    p.add_argument("--invlpg-ns", type=float, default=INVLPG_NS)
    p.add_argument("--vga-scale", type=float, default=None,
                   help="VGA port price scale (fx27: calibration, default 0.65; fixed: what-if, default 1)")
    p.add_argument("--pf-scale", type=float, default=PF_SCALE)
    p.add_argument("--share", type=float, default=0.2, help="DOS Box presenter share of a ms beside the VM")
    p.add_argument("--desktop-share", type=float, default=0.25, help="desktop compositor share of a ms")
    p.add_argument("--model", default="fixed", choices=("fixed", "fx27"),
                   help="fixed: the trial's per-cause prices (deterministic); fx27: host-time scaled")
    p.add_argument("--xchg-ns", type=float, default=XCHG_NS,
                   help="T23 price of one locked PTE exchange per remapped entry (this round's sources)")
    p.add_argument("--present", default=PRESENT_TRIAL,
                   help="KERNEL,DOSBOX,DESKTOP,FRAME prices; 'direct' for the round-3 direct present")
    p.add_argument("--ack", type=int, default=0, choices=(0, 1, 2),
                   help="the DOS Box's composed acknowledgement (round 3): 1 serial, 2 one frame ahead (production)")
    p.add_argument("--desktop-hz", type=float, default=60.0, help="desktop frame cadence (0: none)")
    p.add_argument("--tag", default="run")
    p.add_argument("--runner", help="a prebuilt runner (e.g. over earlier sources); default: build this tree's")
    p.add_argument("--env", action="append", default=[], help="extra runner environment NAME=VALUE")
    a = p.parse_args()
    if not Path("/usr/include/x86emu.h").exists():
        print("WPAR: unavailable (libx86emu development headers missing)")
        return 1
    OUT.mkdir(parents=True, exist_ok=True)
    cpu = Path(a.runner) if a.runner else compile_runner()
    image = build_image()
    results = {}
    for title in a.games.split(","):
        for mode in a.modes.split(","):
            present = PRESENT_DIRECT if a.present == "direct" else a.present
            code, log = run(cpu, image, title, mode, a.seconds, a.slice, present, a.tag, a.env, a.delay, a.trap_ns,
                            a.invlpg_ns, a.vga_scale if a.vga_scale is not None else (VGA_SCALE if a.model == "fx27" else 1.0),
                            a.pf_scale, a.share, a.model, a.xchg_ns, a.desktop_share, a.ack, a.desktop_hz)
            r = rates(log, TITLES[title][2])
            ok = code == 0 and "guest runner: PASS" in log
            results[f"{title}/{mode}"] = dict(r or {}, exit=code, runner_pass=ok)
            print(f"WPAR {a.tag} {title} {mode}: exit={code} pass={ok} " +
                  (" ".join(f"{k}={v:.2f}" if isinstance(v, float) else f"{k}={v}" for k, v in r.items()) if r else
                   "no steady interval"), flush=True)
    for title in a.games.split(","):
        w, f = results.get(f"{title}/window"), results.get(f"{title}/full")
        if not w or not f or "flips_s" not in w or "flips_s" not in f:
            continue
        metric = "minstr_s" if title == "war2" else "flips_s"
        if w.get(metric):
            print(f"WPAR {a.tag} {title}: full/window {metric} factor = {f[metric] / w[metric]:.3f} "
                  f"(window {w[metric]:.2f}, full screen {f[metric]:.2f}; window frames shown/s "
                  f"{w.get('shown_frames_s', 0):.2f})", flush=True)
    (OUT / f"{a.tag}.json").write_text(json.dumps(results, indent=1, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
