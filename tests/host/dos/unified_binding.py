"""H56 unified binding host qualification (f3-56). No QEMU or OS image.

Builds the hash-pinned CiukiDOS fork and CIUKIDF.SYS twice (determinism)
and the pristine patch-0001 kernel with the pinned Open Watcom/NASM tools,
runs the sanitized native DFV2 matrix, then boots small private FAT16
fixture disks on a real x86 instruction emulator (libx86emu) with the
production adapter at the INT FE gates. f3-57 owns the live file backend:
here every file operation is FreeDOS's own local FAT path, and the native
backend is an explicit fixture (no runtime filesystem closure is claimed).
SPDX-License-Identifier: GPL-2.0-only
"""
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "build/host/dos/h56"
sys.path.insert(0, str(ROOT / "scripts"))


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


fork = module("unified_fork", ROOT / "scripts/unified_fork.py")
unified = fork.u
dos = module("h56_dos_image", ROOT / "scripts/build_dos_image.py")
conformance = module("h56_reference", ROOT / "scripts/dos_conformance.py")

CFLAGS = ["-DFS_HOST", "-D_POSIX_C_SOURCE=200809L", "-std=c17", "-O1", "-g", "-Wall", "-Wextra",
          "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
          "-fno-omit-frame-pointer", "-Isrc/kernel/include", "-Isrc/kernel/fs", "-Isrc/kernel"]
FS = ["src/kernel/fs/fs_port.c", "src/kernel/fs/blkreq.c", "src/kernel/fs/cache.c", "src/kernel/fs/path.c",
      "src/kernel/fs/fat.c", "src/kernel/fs/vfs.c", "src/kernel/fs/fs_ro.c"]
CONFIG = b"DOS=LOW\r\nFILES=40\r\nBUFFERS=10\r\nLASTDRIVE=Z\r\n%sSHELL=C:\\%s\r\n"
DEVICE = b"DEVICE=C:\\CIUKIDF.SYS\r\n"
COMMON = ("layout", "handles", "exec", "psp", "internals")
failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print("H56 CHECK FAILED: " + message, flush=True)


def assemble(source, out):
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["nasm", "-f", "bin", "-o", str(out), str(source)], check=True)
    return out.read_bytes()


def release_folder():
    """The ignored, hash-pinned FreeDOS 1.3 packages: this checkout, else the
    primary checkout of a linked worktree. UPSTREAM.json pins every byte."""
    folder = ROOT / "third_party/freedos/1.3"
    if (folder / "kernel.zip").is_file():
        return folder
    common = subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"], cwd=ROOT,
                            capture_output=True, text=True, check=True).stdout.strip()
    primary = Path(common).parent / "third_party/freedos/1.3"
    if (primary / "UPSTREAM.json").read_bytes() != (folder / "UPSTREAM.json").read_bytes():
        raise ValueError("FreeDOS 1.3 pins differ between checkouts")
    return primary


def disk(path, files, directories=()):
    """MBR + pinned FreeDOS FAT16 VBR + used clusters only (trailing zeros cut)."""
    boot, _ = conformance.installed_boot(release_folder())
    mbr = conformance.reference_mbr()
    volume = path.with_suffix(".vol")
    dos.write_fat16(volume, bytes(boot), b"", files, label="CIUKIH56", directories=directories,
                    sectors=dos.SECTORS - conformance.REFERENCE_START)
    used = 0
    with volume.open("rb") as stream:
        while block := stream.read(65536):
            if block.rstrip(b"\0"):
                used = stream.tell() - len(block) + len(block.rstrip(b"\0"))
    with volume.open("rb") as stream, path.open("wb") as image:
        image.write(mbr + bytes((conformance.REFERENCE_START - 1) * 512))
        image.write(stream.read((used + 511) // 512 * 512))
    volume.unlink()
    return path


def compile_all():
    cpu = OUT / "unified_cpu_test"
    native = OUT / "unified_binding_test"
    subprocess.run(["clang", *CFLAGS, "tests/host/dos/unified_cpu_test.c", "src/kernel/vm/dos_unified.c",
                    "-lx86emu", "-o", str(cpu)], cwd=ROOT, check=True)
    subprocess.run(["clang", *CFLAGS, "-pthread", "tests/host/dos/unified_binding_test.c",
                    "src/kernel/vm/dos_unified.c", "src/kernel/vm/dos_file.c", *FS, "-o", str(native)],
                   cwd=ROOT, check=True)
    return cpu, native


class Boot:
    def __init__(self, text):
        self.text = text
        e9 = re.search(r"E9<<(.*?)>>", text, re.S)
        con = re.search(r"CON<<(.*?)>>", text, re.S)
        self.e9 = (e9.group(1) if e9 else "").replace("\r", "")
        self.con = con.group(1) if con else ""

    def field(self, tag):
        match = re.search(rf"^H56 {tag} (.*)$", self.text, re.M)
        if not match:
            raise AssertionError("missing H56 " + tag)
        return dict(item.split("=", 1) for item in match.group(1).split() if "=" in item)

    def passed(self, name):
        return re.search(rf"^H56 {re.escape(name)} PASS$", self.e9, re.M) is not None

    def lines(self, prefix):
        return [line for line in self.e9.splitlines() if line.startswith(prefix)]


def boot(cpu, name, kernel, shell, files, device=True, args=(), directories=()):
    image = files | {"/KERNEL.SYS": kernel,
                     "/FDCONFIG.SYS": CONFIG % (DEVICE if device else b"", shell.encode())}
    path = disk(OUT / f"{name}.img", image, directories)
    result = subprocess.run([str(cpu), str(path), *args], cwd=ROOT, capture_output=True, text=True,
                            timeout=900)
    (OUT / f"{name}.log").write_text(result.stdout + result.stderr)
    path.unlink()
    run = Boot(result.stdout + result.stderr)
    cpu_line = run.field("CPU")
    print(f"H56 boot {name}: exit={result.returncode} instructions={cpu_line['instructions']} "
          f"stop={cpu_line['stop']} failure={cpu_line['failure']}", flush=True)
    check(result.returncode == 0, f"{name}: harness exit {result.returncode}")
    return run


def common_pass(run, name, extra=()):
    for test in (*COMMON, *extra):
        check(run.passed(test), f"{name}: guest test {test}")
    check("H56 END fails=0000" in run.e9, f"{name}: guest failures")


def native_balanced(run, name):
    writes = run.field("WRITES")
    check(writes["other"] == "0", f"{name}: native wrote outside the call record/claim output {writes}")
    destroy = run.field("DESTROY")
    backend = run.field("BACKEND")
    check(destroy["live"] == "0" and destroy["bad"] == "0" and destroy["prepared"] == "0" and
          destroy["late_completion"] == "refused", f"{name}: native ledger after VM stop {destroy}")
    check(backend["bad"] == "0", f"{name}: backend protocol violations {backend}")


def layout_matches(run, name):
    guest = re.search(r"H56 LAYOUT lol=([0-9A-F]{4}):([0-9A-F]{4}) sft=([0-9A-F]{4}) "
                      r"sda=([0-9A-F]{4}):([0-9A-F]{4}) cds=([0-9A-F]{4}):([0-9A-F]{4})/([0-9A-F]{4})", run.e9)
    native = run.field("LAYOUT")
    check(guest is not None, f"{name}: guest layout line")
    if guest:
        lol, sft, sda, cds = (f"{guest[1]}:{guest[2]}".lower(), int(guest[3], 16),
                              f"{guest[4]}:{guest[5]}".lower(), f"{guest[6]}:{guest[7]}".lower())
        check(native["lol"] == lol and native["sda"] == sda and int(native["sft"]) == sft and
              native["cds"].split("/")[0] == cds and int(native["cds"].split("/")[1]) == int(guest[8], 16),
              f"{name}: native registration differs from the guest's genuine layout {native} vs {guest.group(0)}")


def fcb_steps(run):
    steps = {}
    for line in run.lines("FCB "):
        m = re.match(r"FCB ([0-9A-F]{4}) (.*)$", line)
        if m and "=" in m.group(2) and not m.group(2).startswith("value"):
            steps.setdefault(int(m.group(1), 16), []).append(
                dict(item.split("=", 1) for item in m.group(2).split()))
    return steps


def assert_fcb(run, name):
    first = {k: v[0] for k, v in fcb_steps(run).items()}

    def expect(step, **fields):
        got = first.get(step, {})
        check(all(got.get(k) == v for k, v in fields.items()), f"{name}: FCB step {step} {fields} got {got}")
    expect(1, al="00", rec="07", rnd="12345678", rsz="0080")
    expect(2, al="00", blk="0001", rec="00", siz="00004000")             # 127 -> 128 rollover
    expect(3, al="00", cx="00FF", blk="0001", rec="7F", rnd="000000FF")
    expect(4, rnd="000000FF")
    expect(5, al="00", cx="0000", siz="00000005", rnd="00000005")         # zero-record truncate
    expect(6, al="03", rnd="5A000000", blk="0000", rec="00", e="0026")    # 24-bit, partial pad, 59h
    check("FCB DTA A74646464646000000 00A7A7" in run.e9, f"{name}: FCB zero padding/guards")
    expect(7, al="01", rec="01", rnd="00000001", e="0026")
    expect(8, al="03", rec="01", e="0026")
    expect(9, al="03", cx="0001", rnd="00000001", rec="01")
    expect(10, al="01", rec="01", rsz="0000", e="0057")                   # record size 0
    expect(11, al="01", rec="80", e="0057")                               # current record 128
    expect(12, al="02")                                                    # DTA segment wrap
    expect(13, al="02", cx="0000", rsz="FFFF")                            # > 64 KiB block
    expect(14, al="01", e="0027")                                          # 32-bit position overflow
    expect(15, al="00")
    expect(16, al="00")                                                    # close twice
    expect(17, al="00", rsz="0080", siz="00000005")
    expect(18, al="00", rnd="00000002")                                    # 23h, ceil(5/3)
    expect(19, al="FF", e="0006")                                          # forged SFT index
    expect(20, al="FF", e="0006")
    expect(35, al="FF", e="0006")                                          # FCB on a handle SFT
    expect(21, al="00")
    expect(22, al="FF", e="0006")                                          # renamed in place
    expect(23, al="00")
    expect(24, al="00")
    expect(25, al="00")
    check(re.search(r"^FCB FIND A7FF00000000000203464342202020202044415420.* A7$", run.e9, re.M) is not None,
          f"{name}: FCB find extended prefix/name/guards")
    check("FCB 001A value=0002" in run.e9, f"{name}: FCB wildcard search count")
    expect(27, al="00")
    expect(28, al="00", siz="00000005")
    expect(29, al="00")
    expect(30, al="FF")
    expect(31, al="FF", e="000F")                                          # invalid drive
    parse = run.lines("FCB PARSE")
    check(len(parse) == 3 and " al=01 " in parse[0] and "0341423F3F3F3F3F3F9A3F20" in parse[0] and
          parse[0].endswith(" A7"), f"{name}: 29h wildcard/CP437 parse {parse[:1]}")
    check(len(parse) == 3 and " al=00 " in parse[1] and "0341423F3F3F3F3F3F9A3F20" in parse[1],
          f"{name}: 29h keeps fields when the string has none {parse[1:2]}")
    check(len(parse) == 3 and " al=FF " in parse[2], f"{name}: 29h invalid drive")
    check("FCB SIZE 0024=00000005" in run.e9, f"{name}: FCB close publishes its size field")
    check("FCB SIZE 0025=0000000F" in run.e9 and run.e9.count("FCB SIZE 0025=00000005") == 1,
          f"{name}: FCB close after a concurrent extension")
    expect(38, al="00")
    check("FCB END" in run.e9, f"{name}: FCB probe completed")


def assert_fcb_pristine(run, name):
    """Upstream 2043 behaviour at the hardened steps (the fork differs)."""
    first = {k: v[0] for k, v in fcb_steps(run).items()}
    check(first.get(10, {}).get("al") == "00" and first[10].get("rec") == "02", f"{name}: upstream size-0 success")
    check(first.get(35, {}).get("al") == "00", f"{name}: upstream FCB I/O through a handle SFT")
    check(first.get(31, {}).get("e") == "0003", f"{name}: upstream invalid drive error 3")
    parse = run.lines("FCB PARSE")
    check(len(parse) == 3 and "0320202020202020209A3F20" in parse[1], f"{name}: upstream 29h blanks the name")
    check("FCB END" in run.e9, f"{name}: FCB probe completed")


def assert_nls(run, name, upcase):
    rows = [line for line in run.lines("NLS ") if re.match(r"NLS 0[1-7] ", line)]
    for sub in range(1, 8):
        mine = [line for line in rows if line.startswith(f"NLS 0{sub} ")]
        check(len(mine) == 5, f"{name}: 65h/{sub:02X} rows {len(mine)}")
        if len(mine) != 5:
            continue
        ok = mine[0]
        check(" cf=00 " in ok and f" cx={'0029' if sub == 1 else '0005'} " in ok, f"{name}: 65h/{sub:02X} {ok}")
        if sub == 1:
            check(" b=A70126000100B501 A7" in ok, f"{name}: 65h/01 record")
        else:
            table = ok.split()[-1]
            head = {2: "8000809A", 3: "00010001", 4: "8000809A", 5: "16008E00", 6: "00010001", 7: "00000000"}[sub]
            check(f" b=A70{sub}" in ok and table == head, f"{name}: 65h/{sub:02X} pointer/table {ok}")
        check(" cf=01 ax=0057 cx=0004" in mine[1], f"{name}: 65h/{sub:02X} CX<5")
        check(" cf=01 " in mine[2] and " cf=01 " in mine[3], f"{name}: 65h/{sub:02X} page/country")
        check(" cf=01 ax=0057 " in mine[4], f"{name}: 65h/{sub:02X} wrapping destination")
    lens = re.search(r"^NLS LEN (.*)$", run.e9, re.M)
    want = ["E", "E"] + [f"{n:02X}A7" for n in range(5, 41)] + ["29A7"] * 5
    check(lens is not None and lens.group(1).split() == want, f"{name}: 65h/01 lengths")
    up = re.search(r"^NLS UPPER ([0-9A-F]{256}) dh_bad=00$", run.e9, re.M)
    check(up is not None and bytes.fromhex(up.group(1)) == upcase, f"{name}: 65h/20 equals the pinned table 2")
    snap = run.field("SNAPSHOT")
    check(bytes.fromhex(snap["upcase"]) == upcase, f"{name}: native snapshot is the guest's table 2")
    check(snap["country"].startswith("0126000100B501"), f"{name}: native country snapshot")
    check("NLS AGREE 0101" in run.e9, f"{name}: 65h/21 and A1 agree with 20 and A0")
    check(re.search(r"^NLS 08 cf=00 ax=6521 cx=0000$", run.e9, re.M) is not None, f"{name}: 65h/21 CX=0")
    check(re.search(r"^NLS 08 cf=01 ax=0057 cx=0002$", run.e9, re.M) is not None, f"{name}: 65h/21 wrap")
    check(" s=41429A006166746572" in run.e9, f"{name}: 65h/22 counted ASCIIZ")
    check(len(re.findall(r"^NLS 08 cf=01 ax=0057 cx=0000$", run.e9, re.M)) == 2, f"{name}: 65h/22, A2 wrap")
    check("NLS YN 0001 0001 0000 0000 0002" in run.e9, f"{name}: 65h/23")
    check("bxdx=01B501B5" in run.e9, f"{name}: 66h/01")
    check(re.search(r"^NLS 08 cf=00 ax=6602", run.e9, re.M) is not None, f"{name}: 66h/02 437")
    check(re.search(r"^NLS 08 cf=01 ax=000D", run.e9, re.M) is not None, f"{name}: 66h/02 850 fails")
    country = re.search(r"^NLS 08 cf=00 ax=0001 cx=0000 b=0001 A7000024000000002C002E002D003A00000200"
                        r"[0-9A-F]{8}2C00A7", run.e9, re.M)
    check(country is not None, f"{name}: 38h country 1 (24 bytes)")
    check(re.search(r"^NLS 08 cf=01 ax=0001 cx=0000$", run.e9, re.M) is not None, f"{name}: 38h/02 fails")
    check("NLS PARSE 9A9A" in run.e9, f"{name}: 29h alias uses the same table")
    check("NLS END" in run.e9, f"{name}: NLS probe completed")


def assert_nls_pristine(run, name):
    check(" b=A7011C000100B501 A7" in run.e9, f"{name}: upstream 65h/01 is 28-byte DOS 3.3 data")
    check(re.search(r"^NLS 03 cf=01 ax=0001", run.e9, re.M) is not None, f"{name}: upstream lacks 65h/03")
    check(re.search(r"^NLS 08 cf=01 ax=0002", run.e9, re.M) is not None, f"{name}: upstream 66h/02 437 fails")
    check("NLS END" in run.e9, f"{name}: NLS probe completed")


def conformance_probes():
    """The unchanged f3-48 probe COMs (basic fixture) and a run list."""
    sys.path.insert(0, str(ROOT / "tests/host/dos"))
    import conformance as battery
    manifest = battery.build(OUT / "probes")
    rows = [r for r in manifest["cases"] if r["fixture"] == "basic"]
    files = {"/D48/" + r["binary"]: (OUT / "probes" / r["binary"]).read_bytes() for r in rows}
    files["/D48/D48CHILD.COM"] = (OUT / "probes/D48CHILD.COM").read_bytes()
    files["/D48/LIST.TXT"] = "".join(f"{r['tag']} {r['binary']}\r\n" for r in rows).encode()
    directories = ("D48", "D48TEST", *("D48TEST/" + r["tag"] for r in rows))
    return dict(files=files, directories=directories, tags=[r["tag"] for r in rows])


def table2():
    text = fork.fork_sources()["kernel/nls_hc.asm"].decode("latin-1")
    body = text[text.index("?table2:"):text.index("GLOBAL _nlsFUpcaseHardcoded")]
    values = [int(v, 16) for v in re.findall(r"0([0-9a-f]{2})h", body)]
    return bytes(values[2:130])


def builds():
    a, b = OUT / "fork-a", OUT / "fork-b"
    first = fork.build_fork(a, mtime=946684800)
    second = fork.build_fork(b, mtime=1609459200)
    for record in (first, second):
        if record["status"] != "ok":
            raise SystemExit("H56 fork build blocked: " + record.get("error", "?"))
    same = all(first[k] == second[k] for k in ("kernel_sha256", "driver_sha256", "source_hashes",
                                                 "linked_objects", "fork_manifest_sha256"))
    check(same and (a / "KERNEL.SYS").read_bytes() == (b / "KERNEL.SYS").read_bytes() and
          (a / "CIUKIDF.SYS").read_bytes() == (b / "CIUKIDF.SYS").read_bytes(),
          "fork build is not byte-identical across roots and input mtimes")
    rows = {r["object"]: r for r in first["linked_objects"]}
    check(rows["ciuki.obj"]["classification"] == "known" and rows["ciukinit.obj"]["classification"] == "known",
          "fork objects missing from the link audit")
    print(f"H56 fork: kernel {first['kernel_sha256']} {first['kernel_size']} bytes, CIUKIDF.SYS "
          f"{first['driver_sha256']} {first['driver_size']} bytes, map {first['map_sha256']}; "
          f"identical across two roots/mtimes; {len(rows)} linked objects audited", flush=True)
    pristine = unified.build(OUT / "pristine", mtime=946684800)
    if pristine["status"] != "ok":
        raise SystemExit("H56 pristine build blocked: " + pristine.get("error", "?"))
    print(f"H56 pristine patch-0001 kernel {pristine['kernel_sha256']} {pristine['size']} bytes", flush=True)
    shutil.rmtree(b)
    return a, OUT / "pristine"


def main():
    if not Path("/usr/include/x86emu.h").exists():
        print("H56: unavailable (libx86emu development headers missing)")
        return 1
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    fork_dir, pristine_dir = builds()
    kernel = (fork_dir / "KERNEL.SYS").read_bytes()
    driver = (fork_dir / "CIUKIDF.SYS").read_bytes()
    pristine = (pristine_dir / "KERNEL.SYS").read_bytes()
    cpu, native_test = compile_all()
    result = subprocess.run([str(native_test)], cwd=ROOT, capture_output=True, text=True, timeout=600)
    print(result.stdout + result.stderr, end="", flush=True)
    check(result.returncode == 0 and " 0 failures" in result.stdout, "native DFV2 matrix")

    guest = {f"/{name}": assemble(ROOT / f"tests/host/dos/h56/{source}.asm", OUT / "programs" / name)
             for name, source in (("H56.COM", "h56"), ("CHILD.COM", "child"), ("H56FCB.COM", "h56fcb"),
                                  ("H56NLS.COM", "h56nls"), ("RUNALL.COM", "runall"))}
    files = guest | {"/CIUKIDF.SYS": driver}

    # H56-01..04 on the real CPU: bound with the fixture's claims.
    run = boot(cpu, "bound-claims", kernel, "H56.COM B", files, args=("--claims",))
    common_pass(run, "bound-claims", ("direct", "pending", "spoof", "exec-after-spoof"))
    native = run.field("NATIVE")
    gates = run.field("GATES")
    check(native["bound"] == "1" and native["refused"] == "3" and native["suspended"] == "1" and
          native["completed"] == "1", f"bound-claims: native {native}")
    check(gates["critical"] == "1" and int(gates["local"]) > 0, f"bound-claims: gates {gates}")
    processes = re.findall(r"^H56 PROCESS psp=(\w+) parent=(\w+) kind=(\w+) provisional=(\d) resident=(\d)$",
                           run.text, re.M)
    check(sum(p[4] == "1" for p in processes) == 2 and sum(p[2] == "26" for p in processes) == 1 and
          sum(p[2] == "55" for p in processes) == 1 and not any(p[3] == "1" for p in processes),
          f"bound-claims: final process records {processes}")
    layout_matches(run, "bound-claims")
    native_balanced(run, "bound-claims")

    run = boot(cpu, "bound", kernel, "H56.COM N", files)
    common_pass(run, "bound", ("spoof", "exec-after-spoof"))
    check(run.field("BACKEND")["direct"] == "0" and run.field("NATIVE")["bound"] == "1", "bound: no claims")
    native_balanced(run, "bound")

    # Isolation: the fork without the driver, the pristine kernel, no host,
    # invalid descriptors/ROM roles and allocation failure at both binds.
    boots = 2
    for name, kern, device, args, message in (
            ("unbound-fork", kernel, False, (), None),
            ("pristine", pristine, False, (), None),
            ("pristine-with-driver", pristine, True, (), None),
            ("nohost", kernel, True, ("--nohost",), "CIUKIDF: no DFV2 host; driver not installed"),
            ("bad-version", kernel, True, ("--desc=4:3",), "CIUKIDF: DFV2 descriptor rejected"),
            ("bad-length", kernel, True, ("--desc=6:2f",), "CIUKIDF: DFV2 descriptor rejected"),
            ("bad-features", kernel, True, ("--desc=8:3e",), "CIUKIDF: DFV2 descriptor rejected"),
            ("bad-overlap", kernel, True, ("--desc=c:00", "--desc=d:18"), "CIUKIDF: DFV2 descriptor rejected"),
            ("bad-role", kernel, True, ("--rom=1c03:d1",), "CIUKIDF: DFV2 descriptor rejected"),
            ("bad-critical", kernel, True, ("--rom=1c0e:cb",), "CIUKIDF: DFV2 descriptor rejected"),
            ("bad-reserved", kernel, True, ("--desc=28:1",), "CIUKIDF: DFV2 descriptor rejected"),
            ("fail-register", kernel, True, ("--fail=0:1",), "CIUKIDF: DFV2 registration refused"),
            ("fail-final", kernel, True, ("--fail=0:2",), "CiukiDOS: DFV2 binding refused (error 8)")):
        run = boot(cpu, name, kern, "H56.COM U", files, device=device, args=args)
        boots += 1
        common_pass(run, name)
        native = run.field("NATIVE")
        if message:
            check(message in run.con, f"{name}: console {message!r}")
        registered = "1" if name in ("pristine-with-driver", "fail-final") else "0"
        check(native["bound"] == "0" and native["registered"] == registered, f"{name}: native {native}")
        if name in ("unbound-fork", "pristine", "nohost") or name.startswith("bad-"):
            check(run.field("GATES") == dict(direct="0", local="0", event="0", critical="0", refusals="0"),
                  f"{name}: a gate was entered {run.field('GATES')}")
        native_balanced(run, name)

    # H56-04 failure injection at the lifecycle publications.
    for name, args, tail, test in (("fail-exec-begin", ("--fail=3:2",), "F3", "fail-exec-begin"),
                                   ("fail-exec-commit", ("--fail=4:2",), "F4", "fail-exec-commit"),
                                   ("fail-psp55", ("--fail=1:1",), "F1", "fail-psp55")):
        run = boot(cpu, name, kernel, "H56.COM " + tail, files, args=args)
        boots += 1
        check(run.passed("layout") and run.passed(test) and "H56 END fails=0000" in run.e9, f"{name}: guest")
        native_balanced(run, name)

    # H56-05 FCB and H56-06 NLS on the upstream engine (bound) and pristine.
    run = boot(cpu, "fcb", kernel, "H56FCB.COM", files)
    assert_fcb(run, "fcb")
    check(run.field("NATIVE")["bound"] == "1", "fcb: bound")
    run = boot(cpu, "fcb-pristine", pristine, "H56FCB.COM", files, device=False)
    assert_fcb_pristine(run, "fcb-pristine")
    run = boot(cpu, "nls", kernel, "H56NLS.COM", files)
    assert_nls(run, "nls", table2())
    run = boot(cpu, "nls-pristine", pristine, "H56NLS.COM P", files, device=False)
    assert_nls_pristine(run, "nls-pristine")
    boots += 4

    # The unchanged f3-48 basic-fixture battery: the bound fork must report
    # exactly what the pristine kernel reports (QEMU/T23 stay the authority).
    probes = conformance_probes()
    results = {}
    for name, kern, device in (("battery-pristine", pristine, False), ("battery-fork", kernel, True)):
        run = boot(cpu, name, kern, "RUNALL.COM", files | probes["files"], device=device,
                   directories=probes["directories"])
        results[name] = dict(re.findall(r"^(D48\d{3}) (PASS|FAIL) [0-9A-F]{4}$", run.e9, re.M))
        check("RUNALL END" in run.e9 and "RUNALL EXECFAIL" not in run.e9 and "RUNALL CDFAIL" not in run.e9,
              f"{name}: runner")
        check(len(results[name]) == len(probes["tags"]), f"{name}: {len(results[name])} results")
        if device:
            check(run.field("NATIVE")["bound"] == "1", f"{name}: bound")
            native_balanced(run, name)
    check(results["battery-pristine"] == results["battery-fork"],
          f"battery differs: {set(results['battery-pristine'].items()) ^ set(results['battery-fork'].items())}")
    passed = sum(v == "PASS" for v in results["battery-fork"].values())
    print(f"H56 battery: {len(probes['tags'])} basic f3-48 probes, identical on pristine and bound fork "
          f"({passed} PASS; {sorted(k for k, v in results['battery-fork'].items() if v != 'PASS')} fail on both "
          "in this BIOS-only harness)", flush=True)
    boots += 2

    if failures:
        print(f"H56 FAIL: {len(failures)} checks failed")
        return 1
    print(f"H56 PASS: native matrix, {boots} CPU boots (bound/unbound/pristine/no host/descriptor and role "
          "faults/publication failures), FCB and NLS probes; fixture backend only, f3-57 owns the live VFS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
