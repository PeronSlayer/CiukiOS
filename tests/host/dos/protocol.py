"""Fail-closed f3-51 wire protocol. No guest execution or result synthesis.
SPDX-License-Identifier: GPL-2.0-only
"""
import json
import re
from conformance import records, tags, digest

HASH = re.compile(r"[0-9a-f]{64}")
SELECTOR = re.compile(r"f3:dos-conformance run=([0-9a-fA-F]{8}) guest=(freedos|ciukidos)( kernel=unified)?")


def selector(value):
    match = SELECTOR.fullmatch(value)
    if not match or (match[3] and match[2] != "ciukidos"):
        raise ValueError("expected f3:dos-conformance run=<8 hex> guest=freedos|ciukidos [kernel=unified for ciukidos]")
    return match[1], match[2]


def probe_manifest(manifest, read_binary=None):
    if manifest.get("schema_version") != 1:
        raise ValueError("unknown probe manifest schema")
    rows = manifest.get("cases", [])
    expected = list(tags())
    if [r.get("tag") for r in rows] != expected:
        raise ValueError("missing, reordered or unknown COM cases")
    helpers = manifest.get("helpers", [])
    if [r.get("binary") for r in helpers] != ["D48CHILD.COM"]:
        raise ValueError("missing/unknown EXEC helper")
    for row in rows + helpers:
        name = row.get("binary", "")
        if not re.fullmatch(r"D48(?:[0-9]{3}|CHILD)\.COM", name) or not HASH.fullmatch(row.get("sha256", "")):
            raise ValueError("invalid COM/helper identity")
        if read_binary:
            import hashlib
            data = read_binary(name)
            if not 0 < len(data) < 0xff00 or hashlib.sha256(data).hexdigest() != row["sha256"]:
                raise ValueError("COM/helper read-back mismatch: " + name)
    for row, case in zip(rows, tags().values()):
        if row.get("id") != case.id or row.get("fixture") != case.fixture or tuple(row.get("functions", ())) != case.functions:
            raise ValueError("case identity/fixture/coverage mismatch")
    return manifest


def integration(image, manifest, probes):
    if manifest.get("dos_conformance_adapter") != 1 or manifest.get("schema_version") != 1:
        raise ValueError("f3-51 runtime adapter/read-back manifest required")
    if manifest.get("image_sha256") != digest(image) or manifest.get("probes_sha256") != digest(probes):
        raise ValueError("integration image/probes hash mismatch")
    probe_manifest(json.loads(probes.read_text()))
    if manifest.get("freedos_release") != "1.3":
        raise ValueError("official FreeDOS 1.3 required")
    for role in ("kernel", "shell", "share", "profile"):
        if not HASH.fullmatch(manifest.get("freedos_" + role + "_sha256", "")):
            raise ValueError("missing FreeDOS " + role + " identity")
    profile = manifest.get("profile", {})
    required = dict(share=True, ansi=False, redirector=False, dpmi=True, xms=True,
                    freedos_bridge=False, ciukidos_bridge=True, hardware="qemu-t23")
    if profile != required:
        raise ValueError("reference profile or bridge policy mismatch")
    for role in ("kernel", "shell", "share"):
        identity = manifest.get("freedos_" + role, {})
        if not identity.get("version") or identity.get("sha256") != manifest["freedos_" + role + "_sha256"]:
            raise ValueError("missing actual binary version/hash: " + role)
    if manifest.get("read_back") != "canonical-image" or not manifest.get("payloads"):
        raise ValueError("built-image read-back evidence required")
    return manifest


class Transcript:
    """One ordered, run-bound stream, including teardown before END.

    VMM hex records stay intact. A supplied run envelope is checked as well as
    BEGIN/END: stale guest rows cannot be laundered between current markers.
    """
    def __init__(self, run, guest):
        selector(f"f3:dos-conformance run={run} guest={guest}")
        self.run, self.guest = run, guest
        self.started = self.ended = self.cleaned = False
        self.results = {}
        self.order = list(tags())
        self.sequence = None

    def feed(self, raw):
        line = raw.rstrip("\r\n")
        if "CIUKI_TEST " in line:
            pairs = re.findall(r"([a-zA-Z_]+)=([^ ]+)", line)
            fields = dict(pairs)
            if len(fields) != len(pairs):
                raise ValueError("duplicate envelope fields")
            if fields.get("guest",self.guest)!=self.guest:
                raise ValueError("wrong serial guest")
            if fields.get("run") != self.run:
                raise ValueError("stale serial run")
            if "seq" not in fields or not fields["seq"].isdigit():
                raise ValueError("missing serial sequence")
            sequence = int(fields["seq"])
            # Disk-only diagnostics allocate from the same global counter.
            # Gaps therefore need the lead's disk log, not guessed UART loss.
            if self.sequence is not None and sequence <= self.sequence:
                raise ValueError("reordered/duplicate serial records")
            self.sequence = sequence
            for key in ("dropped", "lost", "trace_dropped", "cleanup_error"):
                if key in fields and fields[key] != "0":
                    raise ValueError("nonzero " + key)
            if fields.get("status") in ("FAIL", "not_run"):
                raise ValueError("controller failed")
            if "group=guest " not in line:
                return
        if line.startswith("D48 BEGIN "):
            if self.started or self.ended or line != f"D48 BEGIN run={self.run} guest={self.guest}":
                raise ValueError("wrong/duplicate BEGIN")
            self.started = True
            return
        if line.startswith("D48 CLEAN "):
            expect = f"D48 CLEAN run={self.run} guest={self.guest} handles=0 searches=0 locks=0 vms=0 dropped=0"
            if not self.started or self.ended or self.cleaned or line != expect or len(self.results) != len(self.order):
                raise ValueError("invalid teardown ledger")
            self.cleaned = True
            return
        if line.startswith("D48 END "):
            expect = f"D48 END run={self.run} guest={self.guest} count={len(self.order)}"
            if not self.started or self.ended or not self.cleaned or line != expect:
                raise ValueError("wrong/premature/duplicate END")
            self.ended = True
            return
        found = records(line)
        if not found:
            return
        if not self.started or self.ended or self.cleaned:
            raise ValueError("result outside active run")
        tag = next(iter(found))
        if len(self.results) >= len(self.order) or tag != self.order[len(self.results)]:
            raise ValueError("reordered, duplicate or unknown case result")
        self.results.update(found)

    def finish(self):
        if not self.ended or set(self.results) != set(self.order):
            raise ValueError("partial conformance transcript")
        return self.results
