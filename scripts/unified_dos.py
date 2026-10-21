"""Pinned K2043 guest, unmodified except patch 0001 (reproducible date).

No downloads, emulator, image build or alternate compiler. All extraction and
generated files belong below this checkout's build/. SPDX-License-Identifier: GPL-2.0-only
"""
import csv
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import resource
import shlex
import stat
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "third_party/freedos/unified"
LEDGER = ROOT / "docs/design/unified-dos-licenses.tsv"
SOURCE_PINS = {
    "kernel": ("kernel-2043-sources.zip", "SOURCE/KERNEL/SOURCES.ZIP",
               "a977497b7d1b2990dc50542cf1a8da2965079f74c37508d5894ecc02a4c8a9d5"),
    "shell": ("freecom-0.85a-sources.zip", "SOURCE/FREECOM/SOURCES.ZIP",
              "cdd1702c137037393f14adad64d4c3fe5e87d7381d1260c1be12b74f65d8e087"),
    "share": ("share-2006a-sources.zip", "SOURCE/SHARE/SOURCES.ZIP",
              "2e28bf858e38086764d4b237ffd0273d687fcb6f8d970c1887559b63511ce7a3"),
}
EPOCH = 1620950400  # K2043 release date: 2021-05-14 00:00:00 UTC
MODIFICATION = "unmodified except patch 0001 (reproducible date)"
BUILD_MANIFEST = VENDOR / "ciuki/build.json"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def build_path(path):
    path = Path(path).resolve()
    if not path.is_relative_to((ROOT / "build").resolve()) or path == (ROOT / "build").resolve():
        raise ValueError("extraction/output must be inside this worktree's build/")
    return path


def zip_members(data, *, expected=None, max_member=16 * 1024 * 1024,
                max_total=64 * 1024 * 1024, max_count=4096, select=None):
    """Validate the entire directory before reading any selected member.

    Never use extractall. Reject Windows paths too, even on a Linux host.
    Return bytes only; callers cannot leave a partial import on failure.
    """
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        infos = archive.infolist()
        if len(infos) > max_count:
            raise ValueError("archive member count exceeds bound")
        seen = {}; total = 0
        for info in infos:
            name = info.filename
            p = PurePosixPath(name)
            if (not name or "\\" in name or ":" in name or "\0" in name or
                    p.is_absolute() or any(part in ("", ".", "..") for part in name.rstrip("/").split("/"))):
                raise ValueError("unsafe archive name: " + repr(name))
            key = name.rstrip("/").casefold()
            mode = info.external_attr >> 16
            if key in seen or (stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR)):
                raise ValueError("duplicate/symlink/special archive member: " + name)
            if info.flag_bits & 1:
                raise ValueError("encrypted archive member")
            seen[key] = info.is_dir()
            total += info.file_size
            if (info.file_size > max_member or total > max_total or
                    info.file_size > max(1024 * 1024, info.compress_size * 2000)):
                raise ValueError("archive expanded size exceeds bound")
        for name in seen:
            parts = name.split("/")
            if any(seen.get("/".join(parts[:i])) is False for i in range(1, len(parts))):
                raise ValueError("archive file/directory collision")
        names = {i.filename for i in infos if not i.is_dir()}
        if expected is not None and names != set(expected):
            raise ValueError("archive member list differs from pinned ledger")
        result = {}
        for info in infos:
            if info.is_dir() or (select is not None and not select(info.filename)):
                continue
            raw = archive.read(info)
            if len(raw) != info.file_size or (expected is not None and sha(raw) != expected[info.filename]):
                raise ValueError("archive source hash mismatch: " + info.filename)
            result[info.filename] = raw
        return result


def ledger(component):
    outer = {"kernel": "kernel.zip", "shell": "freecom.zip", "share": "share.zip"}[component]
    prefix = outer + "::" + SOURCE_PINS[component][1] + "::"
    with LEDGER.open(newline="") as stream:
        rows = [r for r in csv.DictReader(stream, delimiter="\t") if r["origin"].startswith(prefix)]
    hashes = {r["origin"][len(prefix):]: r["sha256"] for r in rows}
    if not hashes or len(hashes) != len(rows):
        raise ValueError("missing/duplicate source ledger")
    return hashes


def sources(component="kernel", folder=None):
    name, nested, wanted = SOURCE_PINS[component]
    raw = (VENDOR / name).read_bytes()
    if sha(raw) != wanted:
        raise ValueError("imported upstream source archive pin mismatch")
    members = zip_members(raw, expected=ledger(component))
    if folder is not None:
        folder = Path(folder)
        pins = json.loads((folder / "UPSTREAM.json").read_text())[component]
        if not re.fullmatch(r"[A-Za-z0-9_.-]+\.zip", pins["archive"]):
            raise ValueError("unsafe outer package name")
        package = (folder / pins["archive"]).read_bytes()
        if sha(package) != pins["package_sha256"]:
            raise ValueError("outer package hash mismatch")
        outer = zip_members(package)
        if outer.get(nested) != raw:
            raise ValueError("nested archive pin mismatch")
    return members


def extract(members, output, mtime=EPOCH):
    """Atomic import; prevalidate bytes before calling. Destination is fresh."""
    output = build_path(output)
    if output.exists():
        raise ValueError("fresh extraction directory required")
    for name, data in members.items():
        if (not isinstance(data, bytes) or not name or "\\" in name or ":" in name or
                PurePosixPath(name).is_absolute() or "\0" in name or
                any(part in ("", ".", "..") for part in name.split("/"))):
            raise ValueError("unsafe extraction member")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".extract-", dir=output.parent) as name:
        stage = Path(name) / "source"
        stage.mkdir()
        for name, data in members.items():
            p = stage / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
            os.utime(p, (mtime, mtime))
        stage.rename(output)
    return output


def prepare_tools(output=None):
    pins = json.loads((ROOT / "config/openwatcom.json").read_text())
    archive = ROOT / pins["installer"]["path"]
    if archive.stat().st_size != pins["installer"]["size"] or digest(archive) != pins["installer"]["sha256"]:
        raise ValueError("Open Watcom 1.9 installer pin mismatch")
    source = pins["source"]
    if source.get("sha256"):
        path = ROOT / source["path"]
        if path.stat().st_size != source["size"] or digest(path) != source["sha256"]:
            raise ValueError("Open Watcom source archive pin mismatch")
    support = pins["support_files"]
    if not {"binl/wlink.lnk", "binl/wlsystem.lnk",
            "h/stdlib.h", "h/dos.h", "h/sys/stat.h", "lib286/dos/clibs.lib",
            "lib286/dos/cstart_t.obj", "lib286/dos/doslfns.lib"} <= support.keys():
        raise ValueError("incomplete Open Watcom SYS dependency pins")
    members = zip_members(archive.read_bytes(), max_total=256 * 1024 * 1024,
                          select=lambda n: n.startswith("binl/") or n == "license.txt" or n in support)
    for name, pin in pins["tools"].items():
        data = members["binl/" + name]
        if len(data) != pin["size"] or sha(data) != pin["sha256"]:
            raise ValueError("extracted tool hash mismatch: " + name)
    if sha(members["license.txt"]) != pins["license_sha256"]:
        raise ValueError("tool license hash mismatch")
    for name, pin in support.items():
        data = members.get(name)
        if data is None or len(data) != pin["size"] or sha(data) != pin["sha256"]:
            raise ValueError("Open Watcom SYS dependency hash mismatch: " + name)
    output = build_path(output or ROOT / "build/tools/openwatcom-1.9")
    if not output.exists():
        extract(members, output)
    else:
        # Upgrade the former binl-only extraction without overwriting any
        # installed byte. Incomplete/corrupt existing trees still fail closed.
        missing = [d for d in ("h", "lib286") if not (output / d).exists()]
        for name, data in members.items():
            if name.split("/")[0] not in missing:
                p = output / name
                if p.is_symlink() or p.read_bytes() != data:
                    raise ValueError("installed Open Watcom file modified: " + name)
        for directory in missing:
            extract({n[len(directory) + 1:]: b for n, b in members.items()
                     if n.startswith(directory + "/")}, output / directory)
    # Verify all extracted bytes on every invocation, including message files.
    for name, data in members.items():
        p = output / name
        if p.is_symlink() or p.read_bytes() != data:
            raise ValueError("installed Open Watcom file modified: " + name)
        if name.startswith("binl/"):
            p.chmod(0o755)
    return output, pins


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def tool_probe(tools):
    results = {}
    for name in ("wmake", "wcc", "wlink", "wlib", "wcl"):
        # stdin is /dev/null: wlink's help pager otherwise waits on an
        # inherited pipe until the timeout (observed in f3-56).
        p = subprocess.run([str(tools / "binl" / name), "-?"], capture_output=True,
                           stdin=subprocess.DEVNULL, timeout=20, preexec_fn=no_core)
        results[name] = dict(returncode=p.returncode,
                             output=(p.stdout + p.stderr).decode(errors="replace"))
    return results


def host_environment():
    pins = json.loads((ROOT / "config/openwatcom.json").read_text())["host"]
    actual = {}
    for name, pin in pins.items():
        path = Path(pin["path"])
        if digest(path) != pin["sha256"]:
            raise ValueError("host tool/environment pin mismatch: " + name)
        actual[name] = pin
    version = subprocess.check_output([pins["nasm"]["path"], "-v"], text=True).strip()
    if not version.startswith("NASM version 3.02 "):
        raise ValueError("NASM 3.02 required")
    return actual


def source_hashes(work):
    return {name: digest(work / name) for name in ledger("kernel")}


def reproducible_inputs():
    raw = BUILD_MANIFEST.read_bytes()
    manifest = json.loads(raw)
    fixed = datetime.fromtimestamp(EPOCH, timezone.utc)
    date = ("Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split()[fixed.month - 1] +
            f" {fixed.day:2d} {fixed.year:04d}")
    if (manifest["schema"] != 1 or manifest["modification"] != MODIFICATION or
            manifest["epoch"] != EPOCH or manifest["date"] != date or
            manifest["time"] != "00:00:00" or
            manifest["patch"]["path"] != "ciuki/patches/0001-reproducible-date.patch" or
            set(manifest["source_hashes"]) != {"hdr/version.h", "sys/sys.c"}):
        raise ValueError("invalid pinned reproducible-date manifest")
    patch = (VENDOR / manifest["patch"]["path"]).read_bytes()
    if sha(patch) != manifest["patch"]["sha256"]:
        raise ValueError("patch 0001 SHA-256 mismatch")
    return manifest, raw, patch


def patched_sources(members):
    """Apply only pinned patch 0001, with exact context and no offset/fuzz."""
    manifest, _, patch = reproducible_inputs()
    if {n: sha(b) for n, b in members.items()} != ledger("kernel"):
        raise ValueError("patch 0001 requires pristine pinned source bytes")
    result = dict(members)
    lines = patch.splitlines(keepends=True)
    i = 0; changed = set()
    while i < len(lines):
        if not lines[i].startswith(b"--- a/"):
            raise ValueError("invalid patch 0001 file header")
        path = lines[i][6:].rstrip(b"\n").decode("ascii")
        if (path not in manifest["source_hashes"] or path in changed or i + 1 >= len(lines) or
                lines[i + 1] != b"+++ b/" + path.encode() + b"\n"):
            raise ValueError("unexpected patch 0001 target")
        original = members[path]
        newline = b"\r\n" if b"\r\n" in original else b"\n"
        before = original.replace(b"\r\n", b"\n").splitlines(keepends=True)
        after = []; pos = 0; i += 2
        while i < len(lines) and lines[i].startswith(b"@@ "):
            hunk = re.fullmatch(rb"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@\n", lines[i])
            if not hunk:
                raise ValueError("invalid patch 0001 hunk")
            start, old_count, new_start, new_count = (int(n) if n is not None else 1 for n in hunk.groups())
            offset = start if old_count == 0 else start - 1
            if offset < pos or offset > len(before):
                raise ValueError("invalid patch 0001 offset")
            after.extend(before[pos:offset]); pos = offset
            if len(after) != new_start - 1:
                raise ValueError("invalid patch 0001 new offset")
            removed = added = 0; i += 1
            while i < len(lines) and not lines[i].startswith((b"@@ ", b"--- a/")):
                kind, data = lines[i][:1], lines[i][1:]
                if kind not in (b" ", b"-", b"+"):
                    raise ValueError("invalid patch 0001 line")
                if kind != b"+":
                    if pos >= len(before) or before[pos] != data:
                        raise ValueError("patch 0001 context mismatch")
                    pos += 1; removed += 1
                if kind != b"-":
                    after.append(data); added += 1
                i += 1
            if (removed, added) != (old_count, new_count):
                raise ValueError("patch 0001 hunk length mismatch")
        after.extend(before[pos:])
        result[path] = b"".join(after).replace(b"\n", newline)
        if sha(result[path]) != manifest["source_hashes"][path]:
            raise ValueError("patch 0001 postimage hash mismatch")
        changed.add(path)
    if changed != set(manifest["source_hashes"]):
        raise ValueError("incomplete patch 0001")
    return result


def date_header(manifest):
    # External generated input, never a change to a ledger-pinned source file.
    return (f'#define CIUKI_BUILD_DATE "{manifest["date"]}"\n'
            f'#define CIUKI_BUILD_TIME "{manifest["time"]}"\n')


def date_flags():
    # All WCC compilations run in a source subdirectory. Keep this token free
    # of spaces and absolute roots: wmake/wcc split -d values containing spaces.
    return ["-fi=../ciuki-build-date.h"]


def build_config(manifest):
    flags = " ".join(date_flags())
    # SYS uses CFLAGST instead of ALLCFLAGS. A command-line wmake macro has
    # precedence over watcom.mak; preserve its flags and owlinux's .obj suffix.
    sys_flags = "CFLAGST=-fo=.obj -zq-zp1-os-s-we-e3-wx-bt=DOS " + flags
    # Retain a separator before watcom.mak appends further compiler switches.
    return ("BUILDENV=linux\nXCPU=386\nXFAT=32\nLOADSEG=0x60\n"
            "XUPX=\nUPXOPT=\nXNASM=nasm\nMAKE=wmake -ms -h " + shlex.quote(sys_flags) +
            "\nALLCFLAGS=" + flags + " \n")


def kernel_provenance(work, kernel):
    """Record the raw rebuild's section table beside the unchanged packed pin.

    A SYS file is flat, so its sections come from the Wlink map, not a forged
    executable header. Keep the table's names/classes/groups/addresses/sizes;
    exclude the map's host timestamp and absolute module paths.
    """
    text = (work / "kernel/kernel.map").read_text(errors="replace")
    header = re.search(r"(?m)^Segment\s+Class\s+Group\s+Address\s+Size\s*$", text)
    if not header:
        raise ValueError("kernel section layout missing from Wlink map")
    table = text[header.end():].split("+", 1)[0]
    sections = [" ".join(line.split()) for line in table.splitlines()
                if re.search(r"\b[0-9a-fA-F]{4}:[0-9a-fA-F]{4,8}\s+[0-9a-fA-F]{8}\b", line)]
    if not sections:
        raise ValueError("empty kernel section layout in Wlink map")
    files, identity = reference_module().reference_files()
    vendor = files["/KERNEL.SYS"]
    return dict(rebuilt=dict(representation="raw uncompressed kernel, patch 0001",
                             sha256=sha(kernel), size=len(kernel), sections=sections),
                reference=dict(representation="original packed BIN/KERNL386.SYS",
                               sha256=identity["kernel"]["sha256"], size=len(vendor)),
                vendor_byte_comparison=dict(status="replaced-by-amendment-3",
                    reason="UPX 5.2.1 rejects the flattened vendor SYS wrapper with NotPackedException"),
                behavioural_equivalence=dict(status="pending-lead-f3-48", required="100% identical results"))


class LinkAuditError(ValueError):
    """Carry the complete named classification into blocked build evidence."""
    def __init__(self, rows):
        self.rows = rows
        super().__init__("guest link audit failed: " + "; ".join(
            f"{r['object']} ({r['module']}), source={r['source']}, "
            f"origin={r['origin']}: {r['reason']}"
            for r in rows if r["classification"] == "opaque"))


def link_audit(work, pinned=None):
    """Resolve Wlink OMF names against pinned recipes, not .obj substrings.

    The public-symbol map omits objects without public symbols. Retain all
    direct OBJS inputs from the pinned link recipe and audit library members
    and the verbose module-segment table separately. Never trust a basename
    from an external object/library/source path. `pinned` lets the f3-56
    fork audit its own hash-checked inputs; the default is patch 0001 only.
    """
    work = Path(work).resolve()
    pinned = patched_sources(sources()) if pinned is None else pinned
    rows = {}; catalog = {}
    for directory in ("kernel", "drivers"):
        makefile = directory + "/makefile"
        text = pinned[makefile].decode().replace("\r\n", "\n").replace("\\\n", " ")
        # Only declared OBJS lists, not all .obj references/dependencies.
        objects = set(re.findall(r"\b([A-Za-z0-9_]+)\.obj\b", " ".join(
            re.findall(r"(?m)^OBJS\d*\s*=([^\n]*)", text))))
        for obj in sorted(objects):
            stem = "asmsupt" if obj == "iasmsupt" else obj
            choices = [f"{directory}/{stem}{ext}" for ext in (".c", ".asm")
                       if f"{directory}/{stem}{ext}" in pinned]
            if len(choices) != 1:
                raise ValueError("unresolved pinned build rule for guest object: " + obj)
            src = choices[0]
            assembler = src.endswith(".asm")
            special = obj == "iasmsupt" or (not assembler and
                re.search(rf"(?m)^{obj}\.obj:[^\n]*\n\s+\$\(CC\) \$\(INITCFLAGS\)", text))
            origin = dict(tool="nasm 3.02" if assembler else "Open Watcom 1.9 wcc",
                          directory=directory,
                          rule=makefile + ":" + obj + ".obj" if special else
                               "mkfiles/generic.mak:" + (".asm.obj" if assembler else ".c.obj"),
                          output=f"{directory}/{obj}.obj")
            if directory == "drivers":
                origin["library"] = "drivers/makefile:device.lib (Open Watcom 1.9 wlib), copied to lib/device.lib"
            catalog[f"{directory}/{obj}.obj"] = dict(object=obj + ".obj", module=Path(src).name,
                source=src, sha256=sha(pinned[src]), license="GPL-2.0-or-later",
                ledger=str(LEDGER.relative_to(ROOT)), origin=origin, classification="known",
                reason="pinned kernel source and toolchain build rule",
                map_visibility="link-input-only")

    def relative(name, directory):
        path = (work / directory / name).resolve()
        return path.relative_to(work).as_posix() if path.is_relative_to(work) else str(path)

    def opaque(obj, module, source, reason, candidate=None):
        row = dict(candidate) if candidate else dict(object=obj, module=module, source=source,
                    origin="no producer in pinned kernel build rules", license="unknown")
        row.update(object=obj, module=module, classification="opaque", reason=reason)
        rows[(obj, module, reason)] = row

    # Verify recipe/source bytes before assigning any known provenance.
    for name in ("kernel/makefile", "drivers/makefile", "mkfiles/generic.mak",
                 "mkfiles/watcom.mak", "mkfiles/owlinux.mak"):
        if (work / name).read_bytes() != pinned[name]:
            opaque(name, name, name, "build rule differs from pinned inputs")
    for key, row in catalog.items():
        if (work / row["source"]).read_bytes() != pinned[row["source"]]:
            row.update(classification="opaque", reason="source differs from pinned inputs")
        if key.startswith("kernel/"):
            rows[key] = dict(row)

    text = (work / "kernel/kernel.map").read_text(errors="replace")
    modules = re.findall(r"(?m)^Module:\s*(.+)$", text)
    if not modules:
        opaque("kernel/kernel.map", "<none>", "unknown", "no Wlink module records")
    for record in modules:
        match = re.fullmatch(r"(.+)\(([^()]+)\)\s*", record)
        if not match:
            opaque(record, record, "unknown", "malformed Wlink module record")
            continue
        container, module = match.groups()
        path = relative(container, "kernel")
        if path == "lib/device.lib":
            matches = [(key, row) for key, row in catalog.items() if key.startswith("drivers/")
                       and relative(module, "drivers") == row["source"]]
        else:
            matches = [(path, catalog[path])] if path in catalog and path.startswith("kernel/") else []
        if len(matches) != 1:
            opaque(container, module, relative(module, "kernel"),
                   "object/library member has no producer in pinned kernel build rules")
            continue
        key, candidate = matches[0]
        row = dict(candidate)
        if relative(module, row["origin"]["directory"]) != row["source"]:
            opaque(container, module, relative(module, "kernel"),
                   "map source does not match pinned object producer", row)
            continue
        row["map_visibility"] = "public-symbols"
        rows[key] = row

    # Modules with no public symbols still occur in the verbose segment table.
    segment_header = re.search(r"(?m)^Module\s+Segment\s+Class\s+Address\s+Size\s*$", text)
    if not segment_header:
        opaque("kernel/kernel.map", "<segments>", "unknown", "missing verbose module-segment table")
    else:
        table = text[segment_header.end():].split("+", 1)[0]
        for line in table.splitlines():
            if not line or line[0].isspace() or set(line) <= {"=", " "}:
                continue
            # Long OMF source names occupy a line by themselves. Short names
            # precede the padded segment columns; unknown names need no suffix.
            module = re.split(r"\s{2,}", line.rstrip(), maxsplit=1)[0]
            matches = [(key, row) for key, row in catalog.items()
                       if relative(module, row["origin"]["directory"]) == row["source"]]
            if not matches:
                opaque("<segment module>", module, relative(module, "kernel"),
                       "segment module has no producer in pinned kernel build rules")
            for key, row in matches:
                if key not in rows:
                    rows[key] = dict(row)
                if rows[key]["map_visibility"] != "public-symbols":
                    rows[key]["map_visibility"] = "module-segments"

    # Also inspect Libraries Used entries, including libraries with no publics.
    libraries = set(re.findall(r"[^\s()]+\.lib\b", text, re.I))
    for library in sorted(libraries):
        if relative(library, "kernel") != "lib/device.lib":
            opaque(library, "<library>", "unknown", "library not built from pinned kernel sources")
    if (work / "lib/libm.lib").stat().st_size:
        opaque("lib/libm.lib", "<library>", "makefile",
               "nonempty libm.lib: Watcom CRT/math library is forbidden in guest")
    result = sorted(rows.values(), key=lambda r: (r["object"], r["module"], r["reason"]))
    if any(r["classification"] == "opaque" for r in result):
        raise LinkAuditError(result)
    return result


def build(output, *, mtime=EPOCH):
    output = build_path(output)
    if output.exists():
        raise ValueError("fresh build root required")
    members = sources(folder=ROOT / "third_party/freedos/1.3")
    manifest, manifest_bytes, _ = reproducible_inputs()
    patched = patched_sources(members)
    config = build_config(manifest)
    output.mkdir(parents=True)
    work = extract(patched, output / "source", mtime)
    (work / "config.mak").write_text(config)
    header = date_header(manifest)
    (work / "ciuki-build-date.h").write_text(header)
    evidence = dict(schema=3, status="blocked", unmodified=False, modification=MODIFICATION,
                    wall_date=datetime.now(timezone.utc).isoformat(),
                    root=str(output), input_mtime=mtime, source_hashes=source_hashes(work),
                    pristine_source_hashes={n: sha(b) for n, b in members.items()},
                    source_archive_sha256=SOURCE_PINS["kernel"][2], config=config, date_header=header,
                    build_manifest_sha256=sha(manifest_bytes), build_inputs=manifest)
    try:
        tools, pins = prepare_tools()
        evidence["host"] = host_environment()
        evidence["tools"] = pins["tools"]
        evidence["support_files"] = pins["support_files"]
        evidence["execution"] = tool_probe(tools)
        if any(r["returncode"] < 0 or not r["output"] for r in evidence["execution"].values()):
            raise ValueError("native OW1.9 Linux execution unavailable; see execution return codes")
        env = dict(PATH=str(tools / "binl") + ":/usr/bin:/bin", WATCOM=str(tools),
                   INCLUDE=str(tools / "h"),
                   LIB=str(tools / "lib286") + ":" + str(tools / "lib286/dos"),
                   LANG="C", LC_ALL="C", TZ="UTC", SOURCE_DATE_EPOCH=str(EPOCH),
                   TMPDIR=str(output), WLINK_LNK=str(tools / "binl/wlink.lnk"), WCC="")
        # Probe with the same space-free forced-include option and cwd as SYS.
        probe = work / "sys/ciuki-date-probe.c"
        probe.write_text('const char date_probe[] = CIUKI_BUILD_DATE "|" CIUKI_BUILD_TIME;\n')
        subprocess.run([str(tools / "binl/wcc"), "-zq", "-bt=dos", *date_flags(),
                        "-fo=" + str(output / "date-probe.obj"), str(probe)],
                       env=env, cwd=work / "sys", check=True)
        if (manifest["date"] + "|" + manifest["time"]).encode() not in (output / "date-probe.obj").read_bytes():
            raise ValueError("patch 0001 compiler build-date macro probe failed")
        with (output / "make.log").open("w") as log:
            subprocess.run(["make", "all"], cwd=work, env=env, stdout=log, stdin=subprocess.DEVNULL,
                           stderr=subprocess.STDOUT, check=True)
        if source_hashes(work) != evidence["source_hashes"]:
            raise ValueError("source bytes changed during build beyond patch 0001")
        if ((work / "config.mak").read_text() != config or
                (work / "ciuki-build-date.h").read_text() != header):
            raise ValueError("external build inputs changed during build")
        evidence["linked_objects"] = link_audit(work)
        kernel = (work / "bin/kernel.sys").read_bytes()
        if kernel != (work / "kernel/kernel.sys").read_bytes():
            raise ValueError("flattened/installed kernel bytes disagree")
        if not 1024 <= len(kernel) <= 134 * 1024 or kernel.startswith(b"CIUKIDOS"):
            raise ValueError("FreeDOS kernel extent/header invalid")
        evidence["kernel_provenance"] = kernel_provenance(work, kernel)
        (output / "KERNEL.SYS").write_bytes(kernel)
        evidence.update(status="ok", kernel_sha256=sha(kernel), size=len(kernel),
                        map_sha256=digest(work / "kernel/kernel.map"))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        if isinstance(error, LinkAuditError):
            evidence["linked_objects"] = error.rows
        evidence.update(status="blocked", error=str(error))
    (output / "build.json").write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    return evidence


def compare_rebuilds(first, second):
    if any(r.get("status") != "ok" for r in (first, second)):
        raise ValueError("two successful patch 0001 builds required")
    if first["root"] == second["root"] or first["input_mtime"] == second["input_mtime"]:
        raise ValueError("distinct build roots and input mtimes required")
    if datetime.fromtimestamp(first["input_mtime"], timezone.utc).date() == datetime.fromtimestamp(second["input_mtime"], timezone.utc).date():
        raise ValueError("different input dates required by f3-55")
    if datetime.fromisoformat(first["wall_date"]).date() == datetime.fromisoformat(second["wall_date"]).date():
        raise ValueError("different host calendar dates required by f3-55 amendment 3")
    manifest, manifest_bytes, _ = reproducible_inputs()
    expected_hashes = {n: sha(b) for n, b in patched_sources(sources()).items()}
    for record in (first, second):
        if (record.get("schema") != 3 or record.get("unmodified") is not False or
                record.get("modification") != MODIFICATION or record.get("build_inputs") != manifest or
                record.get("build_manifest_sha256") != sha(manifest_bytes) or
                record.get("config") != build_config(manifest) or
                record.get("date_header") != date_header(manifest) or
                record.get("source_archive_sha256") != SOURCE_PINS["kernel"][2] or
                record.get("pristine_source_hashes") != ledger("kernel") or
                record.get("source_hashes") != expected_hashes):
            raise ValueError("patch 0001 build/source provenance mismatch")
        kernel = (build_path(record["root"]) / "KERNEL.SYS").read_bytes()
        if sha(kernel) != record["kernel_sha256"] or len(kernel) != record["size"]:
            raise ValueError("recorded kernel hash/size differs from raw bytes")
        if record.get("kernel_provenance") != kernel_provenance(build_path(record["root"]) / "source", kernel):
            raise ValueError("kernel layout/reference provenance differs from current bytes")
    for name in ("kernel_sha256", "size", "source_hashes", "source_archive_sha256", "config",
                 "build_inputs", "build_manifest_sha256", "date_header", "kernel_provenance",
                 "host", "tools", "support_files", "linked_objects"):
        if name not in first or name not in second or first[name] != second[name]:
            raise ValueError("reproducibility mismatch: " + name)
    if (Path(first["root"]) / "KERNEL.SYS").read_bytes() != (Path(second["root"]) / "KERNEL.SYS").read_bytes():
        raise ValueError("raw kernel byte mismatch")
    return True


def compare(first, second):
    # Host qualification enables an opt-in image for the lead's battery.
    # It does not claim the separately required f3-48 behavioural equivalence.
    return compare_rebuilds(first, second)


def reference_module():
    import importlib.util
    spec = importlib.util.spec_from_file_location("unified_reference", ROOT / "scripts/dos_conformance.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


FORK_QUALIFIED = ROOT / "build/unified-dos/fork-qualified.json"


def fork_config(ref):
    """kernel=unified CONFIG for the fork: the reference profile plus the
    DFV2 control driver (f3-57). DOS=LOW stays required (f3-56 Amendment 1)."""
    marker = b"SHELL="
    if ref.CONFIG.count(marker) != 1 or not ref.CONFIG.startswith(b"DOS=LOW\r\n"):
        raise ValueError("reference CONFIG layout changed")
    return ref.CONFIG.replace(marker, b"DEVICE=C:\\CIUKIDF.SYS\r\n" + marker)


def fork_guest_files(qualified=FORK_QUALIFIED):
    """f3-57: kernel=unified installs the CiukiDOS fork kernel and CIUKIDF.SYS,
    only from the host qualification record of two identical fork builds and
    passing CPU boots with the live IFS (tests/host/dos/unified_file.py)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("unified_fork_install", ROOT / "scripts/unified_fork.py")
    fork = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fork)
    record = json.loads(build_path(qualified).read_text())
    first = json.loads(build_path(record["first"]).read_text())
    second = json.loads(build_path(record["second"]).read_text())
    for key in ("status", "kernel_sha256", "driver_sha256", "source_hashes", "fork_manifest_sha256",
                "patch_0002_sha256", "identity", "modification"):
        if first.get(key) != second.get(key):
            raise ValueError("fork rebuilds differ: " + key)
    data, raw, patch, _ = fork.manifest()
    if (first["status"] != "ok" or first["identity"] != fork.IDENTITY or
            first["fork_manifest_sha256"] != sha(raw) or first["patch_0002_sha256"] != sha(patch)):
        raise ValueError("fork qualification does not match the tracked fork")
    if record.get("cpu_boot") != "PASS" or record.get("ifs") != "PASS":
        raise ValueError("fork CPU boot/IFS evidence missing")
    kernel = (build_path(first["root"]) / "KERNEL.SYS").read_bytes()
    driver = (build_path(first["root"]) / "CIUKIDF.SYS").read_bytes()
    if sha(kernel) != first["kernel_sha256"] or sha(driver) != first["driver_sha256"]:
        raise ValueError("rebuilt fork kernel/driver hash mismatch")
    ref = reference_module()
    files, identity = ref.reference_files()
    config = fork_config(ref)
    files["/KERNEL.SYS"] = kernel
    files["/CIUKIDF.SYS"] = driver
    files["/FDCONFIG.SYS"] = config
    files["/D48/KERNEL.LIC"] = sources()["COPYING"]
    identity["kernel"] = dict(version="2043", personality="ciukidos-fork", sha256=sha(kernel),
                              license="GPL-2.0-or-later", license_sha256=sha(files["/D48/KERNEL.LIC"]),
                              source_archive_sha256=SOURCE_PINS["kernel"][2], binding=True,
                              identity=first["identity"], modification=first["modification"],
                              fork_manifest_sha256=first["fork_manifest_sha256"],
                              driver=dict(name="CIUKIDF.SYS", sha256=sha(driver), size=len(driver)))
    identity["profile"] = dict(kernel="unified", schema_value=2, bridge=False, share=True, ifs=True,
                               config_sha256=sha(config), auto_sha256=sha(ref.AUTO))
    return files, identity


def guest_files(qualified=None):
    """Never accept arbitrary prebuilt binaries or a failed checker output."""
    if qualified is None and FORK_QUALIFIED.is_file():
        return fork_guest_files()
    qualified = build_path(qualified or ROOT / "build/unified-dos/qualified.json")
    record = json.loads(qualified.read_text())
    first = json.loads(build_path(record["first"]).read_text())
    second = json.loads(build_path(record["second"]).read_text())
    compare(first, second)
    if record.get("cpu_boot") != "PASS":
        raise ValueError("rebuilt guest CPU boot/relocation evidence missing")
    ref = reference_module()
    files, identity = ref.reference_files()
    kernel = (build_path(first["root"]) / "KERNEL.SYS").read_bytes()
    if sha(kernel) != first["kernel_sha256"]:
        raise ValueError("rebuilt guest kernel hash mismatch")
    files["/KERNEL.SYS"] = kernel
    files["/D48/KERNEL.LIC"] = sources()["COPYING"]
    identity["kernel"] = dict(version="2043", personality="unified-unmodified", sha256=sha(kernel),
                              license="GPL-2.0-or-later", license_sha256=sha(files["/D48/KERNEL.LIC"]),
                              source_archive_sha256=SOURCE_PINS["kernel"][2], binding=False,
                              modification=MODIFICATION, build_inputs=first["build_inputs"])
    identity["profile"] = dict(kernel="unified", schema_value=2, bridge=False, share=True,
                               config_sha256=sha(ref.CONFIG), auto_sha256=sha(ref.AUTO))
    return files, identity


def fork_source_files(vendor=VENDOR):
    """CiukiDOS fork sources for the corresponding-source bundle (f3-56
    Amendment 1): fork.json, patch 0002 and every file under ciuki/fork/.
    A fork file absent from fork.json, a listed file absent from disk or any
    hash mismatch fails, so the bundle can never silently omit a fork file."""
    vendor = Path(vendor)
    raw = (vendor / "ciuki/fork.json").read_bytes()
    data = json.loads(raw)
    files = {"ciuki/fork.json": raw}
    patch = (vendor / data["patch"]["path"]).read_bytes()
    if sha(patch) != data["patch"]["sha256"]:
        raise ValueError("patch 0002 hash differs from fork.json")
    files[data["patch"]["path"]] = patch
    listed = {item["source"]: item["sha256"] for item in data["added"].values()}
    present = {"ciuki/fork/" + p.relative_to(vendor / "ciuki/fork").as_posix()
               for p in (vendor / "ciuki/fork").rglob("*") if p.is_file()}
    if present - set(listed):
        raise ValueError("fork file missing from fork.json: " + ", ".join(sorted(present - set(listed))))
    if set(listed) - present:
        raise ValueError("fork.json lists an absent fork file: " + ", ".join(sorted(set(listed) - present)))
    for name, pin in sorted(listed.items()):
        blob = (vendor / name).read_bytes()
        if sha(blob) != pin:
            raise ValueError("fork file hash differs from fork.json: " + name)
        files[name] = blob
    return files


def corresponding_source(output):
    """Deterministic source bundle; no tool binaries, private media or images."""
    output = build_path(output)
    files = {}
    for component, (name, _, _) in SOURCE_PINS.items():
        sources(component, ROOT / "third_party/freedos/1.3")
        files["upstream/" + name] = (VENDOR / name).read_bytes()
    manifest, manifest_bytes, patch = reproducible_inputs()
    patched_sources(sources())  # Validate the distributed patch and postimages too.
    files["ciukios/third_party/freedos/unified/ciuki/build.json"] = manifest_bytes
    files["ciukios/third_party/freedos/unified/" + manifest["patch"]["path"]] = patch
    files["ciukios/third_party/freedos/unified/ciuki/patches/README.md"] = (VENDOR / "ciuki/patches/README.md").read_bytes()
    for name, data in fork_source_files().items():
        files["ciukios/third_party/freedos/unified/" + name] = data
    paths = ["scripts/unified_dos.py", "scripts/unified_fork.py", "docs/licenses/CiukiDOS-unified-kernel-GRANT.md",
             "scripts/dos_conformance.py", "scripts/build_dos_image.py",
             "scripts/ciukidos_image.py", "src/boot/full_boot.asm", "src/boot/full_stage1_loader.asm",
             "config/openwatcom.json", "docs/design/unified-dos-licenses.tsv",
             "third_party/freedos/1.3/UPSTREAM.json", "third_party/freedos/1.3/boot16.bin",
             "third_party/freedos/1.3/COPYING", "tests/host/dos/reference_mbr.asm",
             "tests/host/dos/check_unmodified_build.py", "tests/host/dos/unified_boot_test.c"]
    # The canonical guest also carries Ciuki probe/wrapper binaries. Include
    # their full narrowly scoped source trees and build configuration.
    paths.extend(("src/com/shell.asm", "src/com/dosvm_shell.asm"))
    for folder in ("src/runtime", "src/probes/dos", "src/probes/doscompat"):
        paths.extend(str(p.relative_to(ROOT)) for p in (ROOT / folder).rglob("*") if p.is_file())
    paths.append("src/boot/floppy_stage1.asm")
    paths.append("src/probes/vm/dpmi_dos4gw.c")
    paths.extend(str(p.relative_to(ROOT)) for p in (ROOT / "tests/host/dos").rglob("*")
                 if p.is_file() and p.suffix in (".py", ".c", ".asm", ".inc", ".sh"))
    # Include literal assembly dependencies of the shipped wrappers/probes and
    # old guest, including files outside the initial scoped directories.
    seen = set()
    while paths:
        path = paths.pop()
        if path in seen:
            continue
        seen.add(path)
        data = (ROOT / path).read_bytes()
        for dep in re.findall(rb'^\s*%include\s+"([^"\r\n]+)"', data, re.M):
            candidate = dep.decode("ascii")
            resolved = (ROOT / candidate).resolve()
            if not resolved.is_relative_to(ROOT):
                raise ValueError("source include escapes repository")
            paths.append(str(resolved.relative_to(ROOT)))
    for path in sorted(seen):
        files["ciukios/" + path] = (ROOT / path).read_bytes()
    # Release URL/hash are separate metadata: including an archive's own hash
    # in an archived config would create a circular hash dependency.
    toolchain = json.loads(files["ciukios/config/openwatcom.json"])
    toolchain["source_offer"] = dict(url="", sha256="", status="assigned separately in release metadata")
    files["ciukios/config/openwatcom.json"] = (json.dumps(toolchain, indent=2) + "\n").encode()
    files["build-inputs/config.mak"] = build_config(manifest).encode()
    files["build-inputs/ciuki-build-date.h"] = date_header(manifest).encode()
    files["README.txt"] = ("CiukiDOS unified K2043: " + MODIFICATION + ".\n"
        "Upstream archives are literal FreeDOS 1.3 nested SOURCES.ZIP files.\n"
        "See ciukios/config/openwatcom.json for build-time tool URLs and SHA-256 pins.\n"
        "Restore ciukios/ files at repository root and upstream/ archives under\n"
        "third_party/freedos/unified/; run the host build checker documented there.\n"
        "No Watcom runtime is permitted in the kernel. FreeCOM/SHARE are pinned\n"
        "upstream release binaries, not claimed as reproducibly rebuilt here.\n").encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(".new")
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, data in sorted(files.items()):
                info = zipfile.ZipInfo(name, (2026, 10, 21, 0, 0, 0))
                info.external_attr = 0o100644 << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, data)
        checked = zip_members(temporary.read_bytes(), expected={n: sha(b) for n, b in files.items()})
        if checked != files:
            raise ValueError("corresponding-source read-back mismatch")
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return dict(archive=output.name, sha256=digest(output), size=output.stat().st_size)


def notices(identity, source):
    ref = reference_module()
    _, pinned = ref.reference_files()
    shell_sources = sources("shell")
    suppl = {n: b for n, b in shell_sources.items() if n.lower() in ("suppl/src/copying", "suppl/src/copying.lb")}
    if len(suppl) != 2:
        raise ValueError("SUPPL notices missing")
    offer = json.loads((ROOT / "config/openwatcom.json").read_text())["source_offer"]
    if offer.get("sha256") and offer["sha256"] != source["sha256"]:
        raise ValueError("lead source-offer hash differs from corresponding source")
    # Publication status is explicit. A URL is not claimed live or immutable
    # until the lead supplies it and verifies the actual downloadable bytes.
    metadata = dict(schema=1, guest=identity, corresponding_source=source,
                    source_url=offer["url"], publication="pending-lead-verification")
    fork = identity["kernel"].get("personality") == "ciukidos-fork"
    modification = identity["kernel"]["modification"] if fork else MODIFICATION
    credits = ("CiukiDOS unified: FreeDOS kernel 2043, " + modification + " (GPL-2.0-or-later).\n"
               "Copyright Pat Villani and the FreeDOS contributors; original notices retained.\n"
               + ("CIUKIDF.SYS: CiukiOS contributors, GPL-2.0-or-later; grant in the source bundle.\n" if fork else "") +
               "FreeCOM 0.85a, GPL-2.0; bundled SUPPL has separate LGPL notices.\n"
               "SHARE 08/2006a: original package GPL notice, version not expanded.\n"
               + ("DFV2 binding and native file system on; DOS=LOW; original FreeCOM and SHARE release binaries.\n"
                  if fork else "Binding disabled; DOS=LOW; original FreeCOM and SHARE release binaries.\n") +
               "Corresponding source: " + source["archive"] + "\nSHA-256: " + source["sha256"] +
               "\nSource URL: " + (offer["url"] or "not assigned; release publication blocked") + "\n")
    files = {"CIUKIDOS.TXT": credits.encode(), "FDK-GPL2.TXT": sources()["COPYING"],
             "FREECOM.TXT": ref.reference_files()[0]["/D48/SHELL.LIC"],
             "SUPPL.TXT": b"\n".join(suppl[n] for n in sorted(suppl)),
             "SHARE.TXT": ref.reference_files()[0]["/D48/SHARE.LIC"],
             "CIUKIDOS.JSN": (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode()}
    for role in ("shell", "share"):
        if identity[role] != pinned[role]:
            raise ValueError("shell/SHARE identity mismatch")
    return files, metadata


def readback(read, identity, license_files):
    """Independent parser is supplied by canonical image builder or host tests."""
    ref = reference_module()
    for role, name in (("kernel", "/KERNEL.SYS"), ("shell", "/COMMAND.COM"), ("share", "/SHARE.COM")):
        if sha(read(name)) != identity[role]["sha256"]:
            raise ValueError("unified guest read-back mismatch: " + role)
        if sha(read("/D48/" + role.upper() + ".LIC")) != identity[role]["license_sha256"]:
            raise ValueError("unified license read-back mismatch: " + role)
    config = fork_config(ref) if identity.get("profile", {}).get("ifs") else ref.CONFIG
    if read("/FDCONFIG.SYS") != config or read("/FDAUTO.BAT") != ref.AUTO:
        raise ValueError("unified profile read-back mismatch")
    driver = identity["kernel"].get("driver")
    if driver and sha(read("/" + driver["name"])) != driver["sha256"]:
        raise ValueError("unified read-back mismatch: CIUKIDF.SYS")
    if json.loads(read("/D48/UNI.JSN")) != identity:
        raise ValueError("unified identity read-back mismatch")
    for name, expected in license_files.items():
        if read("/system/licenses/" + name) != expected:
            raise ValueError("installed notice read-back mismatch: " + name)
    return True


def install(out, system_files, directories, games_files, *, source_output):
    """Called only by the canonical image builder when qualification exists."""
    ref = reference_module()
    files, identity = guest_files()
    identity = ref.build_reference(out, system_files, directories, games_files, guest=(files, identity))
    source = corresponding_source(source_output)
    license_files, metadata = notices(identity, source)
    license_dir = build_path(Path(out).parent / "unified-licenses")
    license_dir.mkdir(exist_ok=True)
    for name, data in license_files.items():
        (license_dir / name).write_bytes(data)
    return identity, license_dir, metadata
