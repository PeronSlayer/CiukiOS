"""CiukiDOS K2043 fork for the DFV2 binding (f3-56): pinned inputs and build.

The fork is patch 0001 (reproducible date) plus patch 0002 and the added
GPL-2.0-or-later fork files listed in third_party/freedos/unified/ciuki/
fork.json. Every input is hash-checked before extraction. The unmodified
f3-55 build in scripts/unified_dos.py is not changed by this module.
No downloads, emulator, image build or alternate compiler.
SPDX-License-Identifier: GPL-2.0-only
"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import unified_dos as u  # noqa: E402

FORK = u.VENDOR / "ciuki/fork.json"
IDENTITY = "CIUKIDOS K2043 3"
MODIFICATION = ("CiukiDOS fork: patch 0001 (reproducible date), patch 0002 "
                "(DFV2 binding, lifecycle, FCB and NLS) and added fork files")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def _safe(name):
    p = PurePosixPath(name)
    if (not name or "\\" in name or ":" in name or p.is_absolute() or
            any(part in ("", ".", "..") for part in name.split("/"))):
        raise ValueError("unsafe fork path: " + name)
    return name


def apply_patch(members, patch, postimage, label):
    """Strict unified-diff application: exact context, no offset or fuzz.

    Line endings of each target are preserved; hunks are LF-normalised.
    Every target must be named in `postimage` and match its SHA-256.
    """
    result = dict(members)
    lines = patch.splitlines(keepends=True)
    i = 0
    changed = set()
    while i < len(lines):
        if not lines[i].startswith(b"--- a/"):
            raise ValueError(label + ": invalid file header")
        path = _safe(lines[i][6:].rstrip(b"\n").decode("ascii"))
        if (path not in postimage or path in changed or i + 1 >= len(lines) or
                lines[i + 1] != b"+++ b/" + path.encode() + b"\n" or path not in members):
            raise ValueError(label + ": unexpected target " + path)
        original = members[path]
        newline = b"\r\n" if b"\r\n" in original else b"\n"
        before = original.replace(b"\r\n", b"\n").splitlines(keepends=True)
        after = []
        pos = 0
        i += 2
        while i < len(lines) and lines[i].startswith(b"@@ "):
            hunk = re.fullmatch(rb"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@[^\n]*\n", lines[i])
            if not hunk:
                raise ValueError(label + ": invalid hunk")
            start, old_count, new_start, new_count = (int(n) if n is not None else 1 for n in hunk.groups())
            offset = start if old_count == 0 else start - 1
            if offset < pos or offset > len(before):
                raise ValueError(label + ": invalid offset")
            after.extend(before[pos:offset])
            pos = offset
            if len(after) != (new_start if new_count == 0 else new_start - 1):
                raise ValueError(label + ": invalid new offset")
            removed = added = 0
            i += 1
            while i < len(lines) and not lines[i].startswith((b"@@ ", b"--- a/")):
                kind, data = lines[i][:1], lines[i][1:]
                if kind not in (b" ", b"-", b"+"):
                    raise ValueError(label + ": invalid line")
                if kind != b"+":
                    if pos >= len(before) or before[pos] != data:
                        raise ValueError(label + ": context mismatch in " + path)
                    pos += 1
                    removed += 1
                if kind != b"-":
                    after.append(data)
                    added += 1
                i += 1
            if (removed, added) != (old_count, new_count):
                raise ValueError(label + ": hunk length mismatch")
        after.extend(before[pos:])
        result[path] = b"".join(after).replace(b"\n", newline)
        if sha(result[path]) != postimage[path]:
            raise ValueError(label + ": postimage hash mismatch " + path)
        changed.add(path)
    if changed != set(postimage):
        raise ValueError(label + ": incomplete patch")
    return result


def manifest():
    raw = FORK.read_bytes()
    data = json.loads(raw)
    if (data.get("schema") != 1 or data.get("identity") != IDENTITY or
            data.get("modification") != MODIFICATION or data.get("license") != "GPL-2.0-or-later" or
            data.get("base") != u.MODIFICATION or
            data["patch"]["path"] != "ciuki/patches/0002-ciukidos-dfv2-binding.patch"):
        raise ValueError("invalid fork manifest")
    patch = (u.VENDOR / data["patch"]["path"]).read_bytes()
    if sha(patch) != data["patch"]["sha256"]:
        raise ValueError("patch 0002 SHA-256 mismatch")
    added = {}
    for dest, item in sorted(data["added"].items()):
        _safe(dest)
        source = _safe(item["source"])
        if not source.startswith("ciuki/fork/"):
            raise ValueError("fork file outside ciuki/fork: " + source)
        blob = (u.VENDOR / source).read_bytes()
        if sha(blob) != item["sha256"] or b"SPDX-License-Identifier: GPL-2.0-or-later" not in blob:
            raise ValueError("fork file hash/licence mismatch: " + source)
        added[dest] = blob
    return data, raw, patch, added


def fork_sources():
    data, _, patch, added = manifest()
    base = u.patched_sources(u.sources())
    result = apply_patch(base, patch, data["changed"], "patch 0002")
    for dest, blob in added.items():
        if dest in result:
            raise ValueError("fork file shadows upstream source: " + dest)
        result[dest] = blob
    return result


def source_hashes(work, members):
    return {name: u.digest(Path(work) / name) for name in members}


def build_fork(output, *, mtime=u.EPOCH):
    output = u.build_path(output)
    if output.exists():
        raise ValueError("fresh build root required")
    data, fork_raw, patch, _ = manifest()
    members = fork_sources()
    date_manifest, date_raw, _ = u.reproducible_inputs()
    config = u.build_config(date_manifest)
    header = u.date_header(date_manifest)
    output.mkdir(parents=True)
    work = u.extract(members, output / "source", mtime)
    (work / "config.mak").write_text(config)
    (work / "ciuki-build-date.h").write_text(header)
    expected = {n: sha(b) for n, b in members.items()}
    evidence = dict(schema=1, status="blocked", modification=MODIFICATION, identity=IDENTITY,
                    wall_date=datetime.now(timezone.utc).isoformat(), root=str(output),
                    input_mtime=mtime, source_hashes=expected, config=config, date_header=header,
                    fork_manifest_sha256=sha(fork_raw), patch_0002_sha256=sha(patch),
                    build_manifest_sha256=sha(date_raw))
    try:
        tools, pins = u.prepare_tools()
        evidence["host"] = u.host_environment()
        evidence["tools"] = pins["tools"]
        env = dict(PATH=str(tools / "binl") + ":/usr/bin:/bin", WATCOM=str(tools),
                   INCLUDE=str(tools / "h"),
                   LIB=str(tools / "lib286") + ":" + str(tools / "lib286/dos"),
                   LANG="C", LC_ALL="C", TZ="UTC", SOURCE_DATE_EPOCH=str(u.EPOCH),
                   TMPDIR=str(output), WLINK_LNK=str(tools / "binl/wlink.lnk"), WCC="")
        with (output / "make.log").open("w") as log:
            subprocess.run(["make", "all"], cwd=work, env=env, stdout=log, stdin=subprocess.DEVNULL,
                           stderr=subprocess.STDOUT, check=True, preexec_fn=u.no_core)
        if source_hashes(work, members) != expected:
            raise ValueError("source bytes changed during build")
        rows = u.link_audit(work, pinned=members)
        for row in rows:
            if row["source"] in data["added"] or row["source"] in data["changed"]:
                row.update(ledger=str(FORK.relative_to(ROOT)), reason="CiukiDOS fork source, hash-pinned in fork.json")
        evidence["linked_objects"] = rows
        kernel = (work / "bin/kernel.sys").read_bytes()
        if kernel != (work / "kernel/kernel.sys").read_bytes():
            raise ValueError("flattened/installed kernel bytes disagree")
        if not 1024 <= len(kernel) <= 134 * 1024 or IDENTITY.encode() not in kernel:
            raise ValueError("fork kernel extent/identity invalid")
        driver = output / "CIUKIDF.SYS"
        subprocess.run([evidence["host"]["nasm"]["path"], "-f", "bin", "-o", str(driver),
                        str(work / "ciuki/ciukidf.asm")], cwd=work, env=env, check=True,
                       capture_output=True, stdin=subprocess.DEVNULL, preexec_fn=u.no_core)
        driver_bytes = driver.read_bytes()
        if not driver_bytes.startswith(b"\xff\xff\xff\xff\x00\x80") or driver_bytes[10:18] != b"CIUKIDF$":
            raise ValueError("CIUKIDF.SYS header invalid")
        (output / "KERNEL.SYS").write_bytes(kernel)
        evidence.update(status="ok", kernel_sha256=sha(kernel), kernel_size=len(kernel),
                        driver_sha256=sha(driver_bytes), driver_size=len(driver_bytes),
                        map_sha256=u.digest(work / "kernel/kernel.map"))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        if isinstance(error, u.LinkAuditError):
            evidence["linked_objects"] = error.rows
        evidence.update(status="blocked", error=str(error))
    (output / "build.json").write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    return evidence


def refresh(fork_tree):
    """Developer step: regenerate patch 0002 and fork.json from an edited,
    LF-normalised copy of the patch-0001 sources (one directory level per
    upstream path). Never used by the build or tests."""
    import difflib
    fork_tree = Path(fork_tree)
    base = u.patched_sources(u.sources())
    chunks = []
    changed = {}
    for name in sorted(base):
        path = fork_tree / name
        if not path.is_file():
            continue
        old = base[name].replace(b"\r\n", b"\n")
        new = path.read_bytes().replace(b"\r\n", b"\n")
        if old == new:
            continue
        diff = difflib.unified_diff(old.decode("latin-1").splitlines(keepends=True),
                                    new.decode("latin-1").splitlines(keepends=True),
                                    "a/" + name, "b/" + name, n=3)
        chunks.append("".join(diff).encode("latin-1"))
        newline = b"\r\n" if b"\r\n" in base[name] else b"\n"
        changed[name] = sha(new.replace(b"\n", newline))
    patch = b"".join(chunks)
    target = u.VENDOR / "ciuki/patches/0002-ciukidos-dfv2-binding.patch"
    target.write_bytes(patch)
    # Fork-only files edited in the checkout go back to their tracked home.
    for dest, item in json.loads(FORK.read_text())["added"].items():
        if (fork_tree / dest).is_file():
            (u.VENDOR / item["source"]).write_bytes((fork_tree / dest).read_bytes())
    added = {}
    for source in sorted((u.VENDOR / "ciuki/fork").rglob("*")):
        if source.is_file():
            rel = source.relative_to(u.VENDOR / "ciuki/fork").as_posix()
            dest = "ciuki/" + source.name if rel.startswith("drivers/") else rel
            added[dest] = dict(source="ciuki/fork/" + rel, sha256=sha(source.read_bytes()))
    data = dict(schema=1, identity=IDENTITY, modification=MODIFICATION, license="GPL-2.0-or-later",
                base=u.MODIFICATION, patch=dict(path="ciuki/patches/0002-ciukidos-dfv2-binding.patch",
                                                sha256=sha(patch)),
                changed=changed, added=added)
    FORK.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    apply_patch(base, patch, changed, "patch 0002")
    ledger_rows(write=True)
    return data


LEDGER_PREFIX = "third_party/freedos/unified/"


def ledger_rows(write=False):
    """The licence-ledger rows of patch 0002 and every fork file (component
    `ciuki-fork`). With write=True the ledger is rewritten in place."""
    data = json.loads(FORK.read_text())
    evidence = "file notice; owner grant docs/licenses/CiukiDOS-unified-kernel-GRANT.md"
    rows = [("ciuki-fork", LEDGER_PREFIX + data["patch"]["path"], data["patch"]["sha256"], "GPL-2.0-or-later",
             evidence, "fork modification of the pinned K2043 sources (patch 0002)")]
    for dest, item in sorted(data["added"].items()):
        rows.append(("ciuki-fork", LEDGER_PREFIX + item["source"], item["sha256"], "GPL-2.0-or-later",
                     evidence, "fork addition installed as " + dest))
    lines = ["\t".join(r) for r in rows]
    if write:
        text = u.LEDGER.read_text().splitlines()
        kept = [line for line in text if not line.startswith("ciuki-fork\t")]
        u.LEDGER.write_text("\n".join(kept + lines) + "\n")
    return lines


def checkout(dest):
    """Developer step: an LF-normalised editable copy of the fork sources
    (the input format of refresh). Never used by the build or tests."""
    dest = u.build_path(dest)
    if dest.exists():
        raise ValueError("fresh checkout directory required")
    text = (".c", ".h", ".asm", ".inc", ".mak", "makefile")
    members = {n: (b.replace(b"\r\n", b"\n") if n.endswith(text) else b) for n, b in fork_sources().items()}
    return u.extract(members, dest)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--checkout":
        print(checkout(Path(sys.argv[2])))
    elif len(sys.argv) == 3 and sys.argv[1] == "--refresh":
        print(json.dumps(refresh(sys.argv[2]), indent=2, sort_keys=True))
    elif len(sys.argv) == 3 and sys.argv[1] == "--build":
        record = build_fork(Path(sys.argv[2]))
        print(json.dumps({k: record.get(k) for k in ("status", "error", "kernel_sha256", "kernel_size",
                                                      "driver_sha256", "driver_size")}, sort_keys=True))
        sys.exit(record["status"] != "ok")
    else:
        sys.exit("usage: unified_fork.py --build OUT | --checkout OUT | --refresh LF_FORK_TREE")
