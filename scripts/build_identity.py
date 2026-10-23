#!/usr/bin/env python3
"""Stage guest BUILD.TXT without a recursive image/manifest self-hash.

Called by the image producer after its complete source mapping is assembled,
before payload_records/populate_payloads. Never builds an image itself.
"""
# SPDX-License-Identifier: MIT
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def stage_identity(kernel, directory, sources, metadata, revision, dirty, version, installed_kernel=None):
    """`kernel` is the complete build/f0/VMM.ELF (bound to build-clock.json);
    `installed_kernel` is the file the image installs as /system/VMM.ELF (the
    same kernel without debug sections; default: `kernel` itself)."""
    kernel = Path(kernel)
    clock = json.loads((kernel.parent / "build-clock.json").read_text())
    full_sha = digest(kernel)
    if clock["kernel_sha256"] != full_sha:
        raise ValueError("kernel build-clock hash")
    kernel_sha = digest(installed_kernel) if installed_kernel is not None else full_sha
    # A source tree without git reports "unknown" for both; anything else
    # must be a full commit id and a boolean.
    unknown = revision == "unknown" and dirty == "unknown"
    if not unknown and (not isinstance(revision, str) or len(revision) != 40 or
            any(c not in "0123456789abcdef" for c in revision) or type(dirty) is not bool):
        raise ValueError("source identity unavailable")
    # Identity of the source/payload manifest before BUILD.TXT is added. The
    # full manifest and image both include this id; neither claims a self-hash.
    record = {"schema": 1, "git_revision": revision, "git_dirty": dirty,
              "kernel_sha256": kernel_sha, "kernel_full_sha256": full_sha, "build_epoch": clock["utc_epoch"],
              "version": version,
              "payloads": [{"path": key, "sha256": digest(path), "size": Path(path).stat().st_size}
                           for key, path in sorted(sources.items()) if key.casefold() != "/system/build.txt"]}
    manifest_id = hashlib.sha256(json.dumps(record, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    date = datetime.fromtimestamp(clock["utc_epoch"], timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    text = (f"schema=1\nsource_commit={revision}\nsource_dirty={dirty if unknown else int(dirty)}\n"
            f"kernel_elf_sha256={kernel_sha}\nkernel_full_elf_sha256={full_sha}\nbuild_manifest_id={manifest_id}\nbuild_date={date}\n"
            "image_sha256=see release page\n")
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / "BUILD.TXT"
    target.write_text(text, encoding="ascii")
    sources["/System/BUILD.TXT"] = target
    metadata["build_manifest_id"] = manifest_id
    metadata["build_identity"] = record
    return manifest_id
