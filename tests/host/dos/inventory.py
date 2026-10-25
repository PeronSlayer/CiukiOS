#!/usr/bin/env python3
"""Reproducible static candidates from pinned public shareware, not a trace.

Unpacks members in memory using the canonical, hash-checking package reader.
No disk image or copied game payload is produced. Immediate-register patterns
are candidate sites only: disassembly/reachability and runtime proof are separate.
SPDX-License-Identifier: GPL-2.0-only
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[3]
CORPUS = ("wolf3d-shareware", "doom-shareware", "quake-shareware")


def sites(data):
    """Report only adjacent MOV AH/AX,imm; INT21/2F byte sequences.

    Exact bytes/offsets are reproducible. No claim that embedded data or
    compressed bytes are executed instructions, nor that this is exhaustive.
    """
    result = []
    for match in re.finditer(rb"(?:\xb4(.)|\xb8(..))\xcd([\x21\x2f])", data, re.S):
        ah, ax, vector = match.groups()
        value = ah[0] << 8 if ah else struct.unpack("<H", ax)[0]
        result.append(dict(offset=match.start(), vector=f"{vector[0]:02X}",
            ax=f"{value:04X}", al_known=ax is not None,
            bytes=match.group().hex(), evidence="unvalidated-static-candidate"))
    return result


def collect():
    spec = importlib.util.spec_from_file_location("d48_games", ROOT / "scripts/build_games_disk.py")
    games = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(games)
    result = dict(schema_version=1, runtime_trace="not-collected", files=[])
    # The f3-48 audit is pinned to the three shareware games; packages added
    # later (the Warcraft II demo) are outside its recorded corpus.
    for package in CORPUS:
        _, files, _ = games.package_files(package)
        for path, data, _ in files:
            if not path.upper().endswith((".EXE", ".COM")):
                continue
            signatures = [s for s in ("DOS/4GW", "DOS/32A", "DOS/32", "go32stub", "CWSDPMI", "LZ91", "LZ09")
                          if s.encode() in data]
            entry = None
            if data[:2] == b"MZ" and len(data) >= 28:
                header = struct.unpack_from("<H", data, 8)[0] * 16
                ip, cs = struct.unpack_from("<HH", data, 20)
                entry = header + cs * 16 + ip
            result["files"].append(dict(package=package, path=path, size=len(data),
                sha256=hashlib.sha256(data).hexdigest(), entry_offset=entry,
                signatures=signatures, candidates=sites(data)))
    return result


if __name__ == "__main__":
    json.dump(collect(), sys.stdout, indent=2)
    print()
