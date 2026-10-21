"""Pinned, unmodified K2043 guest build and distribution inputs (f3-55).

No downloads, emulator, image build or alternate compiler. All extraction and
generated files belong below this checkout's build/. SPDX-License-Identifier: GPL-2.0-only
"""
import csv
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import resource
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
EPOCH = 1792540800  # 2026-10-21 00:00:00 UTC, external build input
DATE_HEADER = b'#undef __DATE__\n#undef __TIME__\n#define __DATE__ "Oct 21 2026"\n#define __TIME__ "00:00:00"\n'
CONFIG = ("BUILDENV=linux\nXCPU=386\nXFAT=32\nLOADSEG=0x60\n"
          "XUPX=\nUPXOPT=\nXNASM=nasm\n"
          "ALLCFLAGS=-fi=../ciuki-build-date.h\n")


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
    members = zip_members(archive.read_bytes(), max_total=256 * 1024 * 1024,
                          select=lambda n: n.startswith("binl/") or n == "license.txt")
    for name, pin in pins["tools"].items():
        data = members["binl/" + name]
        if len(data) != pin["size"] or sha(data) != pin["sha256"]:
            raise ValueError("extracted tool hash mismatch: " + name)
    if sha(members["license.txt"]) != pins["license_sha256"]:
        raise ValueError("tool license hash mismatch")
    output = build_path(output or ROOT / "build/tools/openwatcom-1.9")
    if not output.exists():
        extract(members, output)
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
    for name in ("wmake", "wcc", "wlink", "wlib"):
        p = subprocess.run([str(tools / "binl" / name), "-?"], capture_output=True,
                           timeout=20, preexec_fn=no_core)
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


def link_audit(work):
    """Fail closed on unknown modules/libraries, retain full map as evidence."""
    mapping = {}; required = set()
    for directory in ("kernel", "drivers"):
        text = (work / directory / "makefile").read_text().replace("\\\n", " ")
        objs = set(re.findall(r"\b([A-Za-z0-9_]+)\.obj\b", text))
        if directory == "kernel":
            required = objs
        for obj in objs:
            src = "asmsupt" if obj == "iasmsupt" else obj
            choices = [f"{directory}/{src}{ext}" for ext in (".c", ".asm")]
            choices = [p for p in choices if p in ledger("kernel")]
            if len(choices) != 1:
                raise ValueError("unresolved source for guest object: " + obj)
            mapping[obj] = choices[0]
    text = (work / "kernel/kernel.map").read_text(errors="replace")
    # Wlink's verbose map names the contributing object modules and libraries.
    objects = set(re.findall(r"\b([A-Za-z0-9_]+)\.obj\b", text, re.I))
    libraries = set(re.findall(r"\b([A-Za-z0-9_]+)\.lib\b", text, re.I))
    if not required <= objects or objects - mapping.keys() or libraries - {"device"}:
        raise ValueError("unknown/opaque guest module in link map")
    if (work / "lib/libm.lib").stat().st_size:
        raise ValueError("Watcom CRT/math library is forbidden in guest")
    rows = []
    for obj in sorted(objects):
        src = mapping[obj]
        rows.append(dict(object=obj + ".obj", source=src, sha256=digest(work / src),
                         license="GPL-2.0-or-later", ledger=str(LEDGER.relative_to(ROOT))))
    return rows


def build(output, *, mtime=EPOCH):
    output = build_path(output)
    if output.exists():
        raise ValueError("fresh build root required")
    members = sources(folder=ROOT / "third_party/freedos/1.3")
    output.mkdir(parents=True)
    work = extract(members, output / "source", mtime)
    (work / "config.mak").write_text(CONFIG)
    (work / "ciuki-build-date.h").write_bytes(DATE_HEADER)
    evidence = dict(schema=1, status="blocked", unmodified=True,
                    wall_date=__import__("datetime").datetime.now(__import__("datetime").timezone.utc).isoformat(),
                    root=str(output), input_mtime=mtime, source_hashes=source_hashes(work),
                    source_archive_sha256=SOURCE_PINS["kernel"][2], config=CONFIG,
                    forced_header_sha256=sha(DATE_HEADER))
    try:
        tools, pins = prepare_tools()
        evidence["host"] = host_environment()
        evidence["tools"] = pins["tools"]
        evidence["execution"] = tool_probe(tools)
        if any(r["returncode"] < 0 or not r["output"] for r in evidence["execution"].values()):
            raise ValueError("native OW1.9 Linux execution unavailable; see execution return codes")
        env = dict(PATH=str(tools / "binl") + ":/usr/bin:/bin", WATCOM=str(tools),
                   LANG="C", LC_ALL="C", TZ="UTC", SOURCE_DATE_EPOCH=str(EPOCH),
                   TMPDIR=str(output), WLINK_LNK="", WCC="")
        # A forced include is an external compiler input, not an upstream edit.
        # Compile the same macro probe before trusting date overrides.
        probe = output / "date-probe.c"
        probe.write_text('const char date_probe[] = __DATE__ "|" __TIME__;\n')
        subprocess.run([str(tools / "binl/wcc"), "-zq", "-bt=dos", "-fi=" + str(work / "ciuki-build-date.h"),
                        "-fo=" + str(output / "date-probe.obj"), str(probe)],
                       env=env, cwd=output, check=True)
        if b"Oct 21 2026|00:00:00" not in (output / "date-probe.obj").read_bytes():
            raise ValueError("compiler timestamp freeze failed; source patch requires lead review")
        with (output / "make.log").open("w") as log:
            subprocess.run(["make", "all"], cwd=work, env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
        if source_hashes(work) != evidence["source_hashes"]:
            raise ValueError("upstream source bytes changed during build")
        evidence["linked_objects"] = link_audit(work)
        kernel = (work / "bin/kernel.sys").read_bytes()
        if kernel != (work / "kernel/kernel.sys").read_bytes():
            raise ValueError("flattened/installed kernel bytes disagree")
        if not 1024 <= len(kernel) <= 134 * 1024 or kernel.startswith(b"CIUKIDOS"):
            raise ValueError("FreeDOS kernel extent/header invalid")
        (output / "KERNEL.SYS").write_bytes(kernel)
        evidence.update(status="built-unqualified", kernel_sha256=sha(kernel), size=len(kernel),
                        map_sha256=digest(work / "kernel/kernel.map"))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        evidence["error"] = str(error)
    (output / "build.json").write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    return evidence


def compare(first, second, *, require_dates=True):
    if any(r.get("status") != "built-unqualified" for r in (first, second)):
        raise ValueError("two successful unmodified builds required")
    if first["root"] == second["root"] or first["input_mtime"] == second["input_mtime"]:
        raise ValueError("distinct build roots and input mtimes required")
    if require_dates and first["wall_date"][:10] == second["wall_date"][:10]:
        raise ValueError("different wall-clock dates required by f3-54 amendment 1")
    for name in ("kernel_sha256", "size", "source_hashes", "source_archive_sha256", "config",
                 "forced_header_sha256", "host", "tools", "linked_objects"):
        if first[name] != second[name]:
            raise ValueError("reproducibility mismatch: " + name)
    if (Path(first["root"]) / "KERNEL.SYS").read_bytes() != (Path(second["root"]) / "KERNEL.SYS").read_bytes():
        raise ValueError("raw kernel byte mismatch")
    return True


def reference_module():
    import importlib.util
    spec = importlib.util.spec_from_file_location("unified_reference", ROOT / "scripts/dos_conformance.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def guest_files(qualified=None):
    """Never accept arbitrary prebuilt binaries or a failed checker output."""
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
                              source_archive_sha256=SOURCE_PINS["kernel"][2], binding=False)
    identity["profile"] = dict(kernel="unified", schema_value=2, bridge=False, share=True,
                               config_sha256=sha(ref.CONFIG), auto_sha256=sha(ref.AUTO))
    return files, identity


def corresponding_source(output):
    """Deterministic source bundle; no tool binaries, private media or images."""
    output = build_path(output)
    files = {}
    for component, (name, _, _) in SOURCE_PINS.items():
        sources(component, ROOT / "third_party/freedos/1.3")
        files["upstream/" + name] = (VENDOR / name).read_bytes()
    paths = ["scripts/unified_dos.py", "scripts/dos_conformance.py", "scripts/build_dos_image.py",
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
    files["build-inputs/config.mak"] = CONFIG.encode()
    files["build-inputs/ciuki-build-date.h"] = DATE_HEADER
    files["README.txt"] = ("CiukiDOS unified unmodified K2043 corresponding source.\n"
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
    credits = ("CiukiDOS unified: unmodified FreeDOS kernel 2043 (GPL-2.0-or-later).\n"
               "Copyright Pat Villani and the FreeDOS contributors; original notices retained.\n"
               "FreeCOM 0.85a, GPL-2.0; bundled SUPPL has separate LGPL notices.\n"
               "SHARE 08/2006a: original package GPL notice, version not expanded.\n"
               "Binding disabled; DOS=LOW; original FreeCOM and SHARE release binaries.\n"
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
    if read("/FDCONFIG.SYS") != ref.CONFIG or read("/FDAUTO.BAT") != ref.AUTO:
        raise ValueError("unified profile read-back mismatch")
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
