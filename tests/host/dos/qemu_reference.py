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

from conformance import ROOT, compare, digest

sys.path.insert(0, str(ROOT / "scripts/test"))
import resources as res

SUITE = ROOT / "tests/suites/f3-dos-conformance.json"


def qualified(image, manifest, probes):
    from protocol import integration
    integration(image, manifest, probes)
    sys.path.insert(0,str(ROOT/"scripts"))
    import dos_conformance
    actual=dos_conformance.readback(image,probes)
    if actual!=manifest:
        raise ValueError("integration manifest differs from canonical image read-back")
    return actual


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
    from protocol import Transcript
    parser = Transcript(run_id, guest)
    pending = bytearray()
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    data = bytearray()
    poller = None
    try:
        end = time.monotonic() + timeout
        poller = selectors.DefaultSelector()
        poller.register(proc.stdout, selectors.EVENT_READ)
        while time.monotonic() < end:
            for key, _ in poller.select(min(0.5, max(0, end - time.monotonic()))):
                block = os.read(key.fileobj.fileno(), 8192)
                if not block:
                    raise ValueError("QEMU ended before the authenticated terminal record")
                data.extend(block)
                if len(data) > res.LOG_CAP:
                    raise ValueError("serial log cap exceeded")
                pending.extend(block)
                while b"\n" in pending:
                    raw, _, tail = pending.partition(b"\n")
                    pending = bytearray(tail)
                    parser.feed(raw.decode("ascii", errors="strict"))
                if parser.ended and not pending:
                    return parser.finish()
            if proc.poll() is not None:
                raise ValueError("QEMU stopped before completion")
        raise ValueError("DOS conformance timeout")
    finally:
        if poller is not None: poller.close()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        proc.wait()
        proc.stdout.close()
        log.write_bytes(data)


def reconcile_overlay(overlay, image, serial, disk, run_id, guest):
    """Read only bounded DOS logs after our process has been reaped."""
    from overlay import Overlay
    from trace import reconcile, calls
    sys.path.insert(0,str(ROOT/"scripts"))
    import dos_conformance
    chunks=[]
    with Overlay(overlay,image) as view:
        for name in ("BOOT2.LOG","BOOT1.LOG","BOOT.LOG"):
            try: chunks.append(dos_conformance.read_file(view,"/SYSTEM/LOGS/"+name,2048*512))
            except ValueError as error:
                if "missing/ambiguous FAT payload" not in str(error) or name=="BOOT.LOG": raise
    content=b"\n".join(chunks)
    disk.write_bytes(content)
    records=reconcile(serial.read_text(),content.decode("ascii",errors="strict"),run_id)
    # The bridge-disabled reference correctly has no candidate DOS trace.
    observed=calls(records) if guest=="ciukidos" else []
    return dict(records=len(records),calls=len(observed),serial_sha256=digest(serial),disk_sha256=digest(disk))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--image", type=Path, required=True)
    p.add_argument("--integration-manifest", type=Path, required=True)
    p.add_argument("--probes-manifest", type=Path, required=True)
    p.add_argument("--qemu", default="qemu-system-i386")
    p.add_argument("--qemu-img", default="qemu-img")
    args = p.parse_args()
    suite = json.loads(SUITE.read_text())
    identity=qualified(args.image, json.loads(args.integration_manifest.read_text()), args.probes_manifest)
    capped()
    profile = json.loads((ROOT / "tests/profiles/qemu-t23.json").read_text())
    run_id = secrets.token_hex(4)
    out = ROOT / "build/test-runs/f3-dos-conformance" / run_id
    out.mkdir(parents=True)
    sha = identity["image_sha256"]
    results = {}
    all_ok = True
    # Exclude the common launcher and every current runner slot, sequentially.
    with ExitStack() as locks:
        common = res.common_lock(ROOT)
        for path in [common, *res.slot_paths(common)]:
            locks.enter_context(res.ExclusiveLock(path))
        if digest(args.image)!=sha:
            raise ValueError("canonical image changed before suite lock")
        for repeat in range(2):
            results = {};ledgers={}
            current = out / str(repeat + 1)
            current.mkdir()
            for guest in suite["differential"]["guests"]:
                overlay = current / (guest + ".qcow2")
                try:
                    subprocess.run([args.qemu_img, "create", "-f", "qcow2", "-F", "raw", "-b",
                                    str(args.image.resolve()), str(overlay)], check=True, capture_output=True)
                    selector = suite["differential"]["selector"].format(run_id=run_id, guest=guest)
                    results[guest] = capture(command(args.qemu, overlay, selector, profile),
                                             current / (guest + ".serial.log"), run_id, guest,
                                             suite["differential"]["timeout_seconds"])
                    ledgers[guest]=reconcile_overlay(overlay,args.image,current/(guest+".serial.log"),
                        current/(guest+".disk.log"),run_id,guest)
                finally:
                    # Serial evidence survives; no owned overlay remains on
                    # timeout, failed create, parser error or normal completion.
                    overlay.unlink(missing_ok=True)
            if digest(args.image) != sha:
                raise ValueError("canonical image changed during suite")
            table, ok = compare(results["freedos"], results["ciukidos"])
            all_ok &= ok
            (current / "comparison.md").write_text(table)
            (current / "evidence.json").write_text(json.dumps(dict(image_sha256=sha, run_id=run_id,
                repeat=repeat+1, profile=profile, probes_sha256=digest(args.probes_manifest),
                integration=json.loads(args.integration_manifest.read_text()),
                trace_ledgers=ledgers,
                verdict="PASS" if ok else "FAIL"), indent=2) + "\n")
            # An independent second pair has a fresh nonce, same immutable base.
            run_id = secrets.token_hex(4)
    print(out)
    return 0 if all_ok else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.SubprocessError, res.Refusal) as error:
        print("dos-conformance: " + str(error), file=sys.stderr)
        sys.exit(2)
