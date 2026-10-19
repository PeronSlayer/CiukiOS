#!/usr/bin/env python3
"""Lead-only differential runner, written at f3-48 and deliberately unrun.

The f3-51 runtime probe boots the two DOS personalities from the SAME canonical
image and executes the embedded COM manifest. This runner never installs files,
builds an image, or boots a floppy. It refuses pre-f3-51 images explicitly.
Run inside the lead's capped scope. See README.md for the integration manifest.
SPDX-License-Identifier: GPL-2.0-only
"""
import argparse
from contextlib import ExitStack
import json
import os
from pathlib import Path
import re
import secrets
import selectors
import subprocess
import sys
import time

from conformance import ROOT, compare, digest, records

sys.path.insert(0, str(ROOT / "scripts/test"))
import resources as res

SUITE = ROOT / "tests/suites/f3-dos-conformance.json"


def qualified(image, manifest, probes):
    if manifest.get("dos_conformance_adapter") != 1:
        raise ValueError("f3-51 runtime probe is required; this image is not qualified")
    if manifest.get("image_sha256") != digest(image):
        raise ValueError("integration manifest/image mismatch")
    if manifest.get("probes_sha256") != digest(probes):
        raise ValueError("embedded COM manifest differs")
    if manifest.get("freedos_release") != "1.3" or not re.fullmatch("[0-9a-f]{64}", manifest.get("freedos_kernel_sha256", "")):
        raise ValueError("pinned FreeDOS 1.3 kernel identity required")


def capped():
    # cgroup v2: check actual limits, including a cap inherited from ancestors.
    cgroup = next((line.split(":", 2)[2] for line in Path("/proc/self/cgroup").read_text().splitlines()
                   if line.startswith("0::")), None)
    if cgroup is None:
        raise ValueError("lead must run in a cgroup v2 memory-capped scope")
    base = Path("/sys/fs/cgroup")
    at = base / cgroup.lstrip("/")
    memory, swap = [], []
    while at == base or base in at.parents:
        for name, values in (("memory.max", memory), ("memory.swap.max", swap)):
            path = at / name
            if path.exists() and path.read_text().strip() != "max":
                values.append(int(path.read_text().strip()))
        if at == base:
            break
        at = at.parent
    if not memory or min(memory) > 1500 * 1024**2 or not swap or min(swap) != 0:
        raise ValueError("requires MemoryMax<=1500M and MemorySwapMax=0")
    available = int(re.search(r"MemAvailable:\s+(\d+)", Path("/proc/meminfo").read_text())[1]) * 1024
    if available < 2 * 1024**3 or res.qemu_count() or res.heavy_builds():
        raise ValueError("insufficient available memory, active QEMU, or active heavy build")


def command(qemu, overlay, selector, profile):
    return [qemu, "-machine", profile["machine"], "-cpu", profile["cpu"],
            "-accel", profile["accelerator"], "-m", str(profile["ram_mib"]),
            "-icount", profile["icount"], "-vga", profile["vga"],
            "-drive", f"file={overlay},format=qcow2,if=ide,index=0", "-boot", "c",
            "-display", "none", "-monitor", "none", "-serial", "stdio", "-parallel", "none",
            "-nic", "none", "-audiodev", "driver=none,id=silent", "-device", "AC97,audiodev=silent",
            "-fw_cfg", "name=opt/it.alcybercloud.ciukios/test,string=" + selector,
            "-no-reboot"]


def capture(argv, log, run_id, guest, timeout):
    """Bound time/log memory; terminate and reap our process on every exit."""
    terminal = f"D48 END run={run_id} guest={guest} count=".encode()
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    data = bytearray()
    end = time.monotonic() + timeout
    poller = selectors.DefaultSelector()
    poller.register(proc.stdout, selectors.EVENT_READ)
    try:
        while time.monotonic() < end:
            for key, _ in poller.select(min(0.5, max(0, end - time.monotonic()))):
                block = os.read(key.fileobj.fileno(), 8192)
                if not block:
                    raise ValueError("QEMU ended before the authenticated terminal record")
                data.extend(block)
                if len(data) > res.LOG_CAP:
                    raise ValueError("serial log cap exceeded")
                if terminal in data and data.endswith(b"\n"):
                    text = data.decode("ascii", errors="strict")
                    from conformance import tags
                    start = f"D48 BEGIN run={run_id} guest={guest}\n"
                    stop = f"D48 END run={run_id} guest={guest} count={len(tags())}\n"
                    normalized = text.replace("\r\n", "\n")
                    if normalized.count(start) != 1 or normalized.count(stop) != 1:
                        raise ValueError("missing/duplicate run markers or wrong case count")
                    between = normalized.split(start, 1)[1].split(stop, 1)[0]
                    return records(between)
            if proc.poll() is not None:
                raise ValueError("QEMU stopped before completion")
        raise ValueError("DOS conformance timeout")
    finally:
        poller.close()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        proc.wait()
        proc.stdout.close()
        log.write_bytes(data)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--image", type=Path, required=True)
    p.add_argument("--integration-manifest", type=Path, required=True)
    p.add_argument("--probes-manifest", type=Path, required=True)
    p.add_argument("--qemu", default="qemu-system-i386")
    p.add_argument("--qemu-img", default="qemu-img")
    args = p.parse_args()
    suite = json.loads(SUITE.read_text())
    qualified(args.image, json.loads(args.integration_manifest.read_text()), args.probes_manifest)
    capped()
    profile = json.loads((ROOT / "tests/profiles/qemu-t23.json").read_text())
    run_id = secrets.token_hex(4)
    out = ROOT / "build/test-runs/f3-dos-conformance" / run_id
    out.mkdir(parents=True)
    sha = digest(args.image)
    results = {}
    # Exclude the common launcher and every current runner slot, sequentially.
    with ExitStack() as locks:
        common = res.common_lock(ROOT)
        for path in [common, *res.slot_paths(common)]:
            locks.enter_context(res.ExclusiveLock(path))
        for guest in suite["differential"]["guests"]:
            overlay = out / (guest + ".qcow2")
            subprocess.run([args.qemu_img, "create", "-f", "qcow2", "-F", "raw", "-b",
                            str(args.image.resolve()), str(overlay)], check=True, capture_output=True)
            selector = suite["differential"]["selector"].format(run_id=run_id, guest=guest)
            results[guest] = capture(command(args.qemu, overlay, selector, profile),
                                     out / (guest + ".serial.log"), run_id, guest,
                                     suite["differential"]["timeout_seconds"])
        if digest(args.image) != sha:
            raise ValueError("canonical image changed during suite")
        table, ok = compare(results["freedos"], results["ciukidos"])
        (out / "comparison.md").write_text(table)
        (out / "evidence.json").write_text(json.dumps(dict(image_sha256=sha, run_id=run_id,
            profile=profile, probes_sha256=digest(args.probes_manifest),
            integration=json.loads(args.integration_manifest.read_text()),
            verdict="PASS" if ok else "FAIL"), indent=2) + "\n")
        if ok:
            for overlay in out.glob("*.qcow2"):
                overlay.unlink()
    print(out)
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.SubprocessError, res.Refusal) as error:
        print("dos-conformance: " + str(error), file=sys.stderr)
        sys.exit(2)
