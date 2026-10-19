#!/usr/bin/env python3
"""Build COM probes, replay a supplied host fixture, compare strict transcripts.

Does not build disk images or simulate a DOS filesystem. The production bridge
does not exist at f3-48: a model run is evidence only for the supplied executable.
SPDX-License-Identifier: GPL-2.0-only
"""
import argparse
from dataclasses import asdict
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from cases import CASES, source

ROOT = Path(__file__).resolve().parents[3]
LINE = re.compile(r"^(D48[0-9]{3}) (PASS|FAIL) ([0-9A-F]{4})$")


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def build(out):
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    for i, item in enumerate(CASES, 1):
        tag = f"D48{i:03d}"
        asm, binary = out / f"{tag}.asm", out / f"{tag}.COM"
        asm.write_text(source(i, item))
        # NASM 3 warns on normal ORG-based 16-bit absolute COM addresses.
        subprocess.run(["nasm", "-f", "bin", "-Wall", "-Werror", "-w-reloc-abs-word",
                        str(asm), "-o", str(binary)], check=True)
        if not 0 < binary.stat().st_size < 0xff00:
            raise ValueError("invalid COM size: " + tag)
        rows.append(dict(asdict(item), tag=tag, binary=binary.name, sha256=digest(binary)))
    manifest = dict(schema_version=1, source="f3-48", cases=rows)
    helper = Path(__file__).with_name("child.asm")
    binary = out / "D48CHILD.COM"
    subprocess.run(["nasm", "-f", "bin", "-Wall", "-Werror", "-w-reloc-abs-word",
                    str(helper), "-o", str(binary)], check=True)
    manifest["helpers"] = [{"binary": binary.name, "sha256": digest(binary)}]
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    # The lead's controller runs each case in a fresh writable directory;
    # SHARE must already be loaded. No mutation outside these directories.
    batch = ["@echo off", "C:", "cd \\", "md D48TEST", "cd D48TEST"]
    for row in rows:
        folder = row["tag"]
        batch += ["md " + folder, "cd " + folder, "\\D48\\" + row["binary"], "cd .."]
    (out / "RUN.BAT").write_bytes(("\r\n".join(batch) + "\r\n").encode("ascii"))
    return manifest


def tags():
    return {f"D48{i:03d}": item for i, item in enumerate(CASES, 1)}


def records(text):
    """Accept raw UART, existing host 'guest:' lines, or VMM DATA hex records.

    Each probe emits fewer than 48 bytes, so multipart records are rejected.
    Unknown, duplicate, malformed and absent result lines fail closed.
    """
    found = {}
    for raw in text.splitlines():
        line = raw.strip()
        if "group=guest " in line and " data=" in line:
            fields = dict(re.findall(r"(\w+)=([^ ]+)", line))
            if fields.get("part") != "1" or fields.get("parts") != "1":
                raise ValueError("multipart guest record in conformance transcript")
            if not re.fullmatch(r"(?:[0-9a-fA-F]{2}){1,48}", fields.get("data", "")):
                raise ValueError("malformed guest hex record")
            line = bytes.fromhex(fields["data"]).decode("ascii").strip()
        elif line.startswith("guest: "):
            line = line[7:]
        if not line.startswith("D48"):
            continue
        match = LINE.fullmatch(line)
        if not match:
            raise ValueError("malformed result: " + line)
        tag, verdict, detail = match.groups()
        if tag not in tags() or tag in found:
            raise ValueError("unknown or duplicate result: " + tag)
        if (verdict == "PASS") != (detail == "0000"):
            raise ValueError("contradictory result: " + line)
        found[tag] = (verdict, detail)
    return found


def compare(reference, candidate):
    expected = tags()
    if set(reference) != set(expected) or set(candidate) != set(expected):
        raise ValueError("incomplete battery: missing or additional case results")
    rows, success = [], True
    for tag, item in expected.items():
        ref, got = reference[tag], candidate[tag]
        good = ref == got == ("PASS", "0000")
        success &= good
        rows.append(f"| {', '.join(item.functions)} / {item.id} | {' '.join(ref)} | {' '.join(got)} | {'PASS' if good else 'FAIL'} |")
    return "\n".join(["| Function / case | FreeDOS reference | CiukiDOS + VMM | Verdict |", "| --- | --- | --- | --- |"] + rows) + "\n", success


def host(model, volume, out, selected=None):
    """Use the existing dosvm_guest CLI, never a Python imitation of DOS.

    volume is a lead-prepared expendable fixture with RUN.BAT's directories
    and these exact probes installed at C:\\D48. No image is made/copied here.
    """
    out.mkdir(parents=True, exist_ok=True)
    results = {}
    volume_hash = digest(volume)
    for i, item in enumerate(CASES, 1):
        if selected and item.id not in selected:
            continue
        tag = f"D48{i:03d}"
        command = f"rc:\\D48\\{tag}.COM"
        # A fresh host VM/overlay per probe; the fixture's AUTOEXEC must select
        # its scratch cwd. Fixture keyboard/media hooks belong to f3-51.
        proc = subprocess.run([str(model.resolve()), str(volume.resolve()), command, tag],
                              capture_output=True, text=True, timeout=120)
        (out / f"{tag}.log").write_text(proc.stdout + proc.stderr)
        observed = records(proc.stdout)
        if set(observed) != {tag}:
            raise ValueError(f"host model did not emit exactly {tag}; see {out}")
        results.update(observed)
        if proc.returncode:
            raise ValueError(f"host replay exited {proc.returncode}: {tag}")
    if digest(volume) != volume_hash:
        raise ValueError("host runner changed fixture base")
    (out / "evidence.json").write_text(json.dumps(dict(model_sha256=digest(model),
        volume_sha256=volume_hash, coverage="subset" if selected else "complete",
        results=results), indent=2) + "\n")
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    p = sub.add_parser("build")
    p.add_argument("--out", type=Path, default=ROOT / "build/codex/dos-conformance")
    p = sub.add_parser("compare")
    p.add_argument("reference", type=Path)
    p.add_argument("candidate", type=Path)
    p.add_argument("--out", type=Path, required=True)
    p = sub.add_parser("host")
    p.add_argument("--model", type=Path, required=True)
    p.add_argument("--volume", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--case", action="append", choices=[c.id for c in CASES])
    args = parser.parse_args()
    if args.operation == "build":
        m = build(args.out)
        print(f"assembled {len(m['cases'])} COM probes: {args.out}")
    elif args.operation == "compare":
        table, success = compare(records(args.reference.read_text()), records(args.candidate.read_text()))
        args.out.write_text(table)
        return 0 if success else 1
    else:
        observed = host(args.model, args.volume, args.out, args.case)
        return 0 if all(row == ("PASS", "0000") for row in observed.values()) else 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print("dos-conformance: " + str(error), file=sys.stderr)
        sys.exit(2)
