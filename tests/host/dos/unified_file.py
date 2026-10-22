"""H57 unified DOS local IFS host qualification (f3-57). No QEMU or OS image.

Builds the hash-pinned CiukiDOS fork and CIUKIDF.SYS twice (determinism) and
the pristine patch-0001 kernel, then boots private FAT16 fixture disks on a
real x86 instruction emulator (libx86emu) with the production DFV2 adapter
and the production live IFS backend (dos_unified_ifs.c) over the production
VFS/FAT: the fork's seven redirector callouts reach native files for C: (and a
read-only D:), critical errors are raised through the kernel's INT 2Fh/1206h,
and the activation barrier runs at the final bind. On success it writes
build/unified-dos/fork-qualified.json, which lets the canonical image builder
install the fork under kernel=unified. Not a VM boot and not hardware evidence.
SPDX-License-Identifier: GPL-2.0-only
"""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


h56 = module("h56_binding", ROOT / "tests/host/dos/unified_binding.py")
fork, unified, dos, conformance = h56.fork, h56.unified, h56.dos, h56.conformance
OUT = ROOT / "build/host/dos/h57"
h56.OUT = OUT
QUALIFIED = ROOT / "build/unified-dos/fork-qualified.json"
VM = ["src/kernel/vm/dos_unified.c", "src/kernel/vm/dos_unified_ifs.c", "src/kernel/vm/dos_file.c",
      "src/kernel/vm/dos_file_volume.c"]
failures = h56.failures
check = h56.check


def compile_ifs():
    cpu = OUT / "unified_ifs_cpu"
    subprocess.run(["clang", *h56.CFLAGS, "-DH57_IFS", "-pthread", "tests/host/dos/unified_cpu_test.c", *VM,
                    *h56.FS, "-lx86emu", "-o", str(cpu)], cwd=ROOT, check=True)
    return cpu


def builds():
    a, b = OUT / "fork-a", OUT / "fork-b"
    first = fork.build_fork(a, mtime=946684800)
    second = fork.build_fork(b, mtime=1609459200)
    for record in (first, second):
        if record["status"] != "ok":
            raise SystemExit("H57 fork build blocked: " + record.get("error", "?"))
    keys = ("kernel_sha256", "driver_sha256", "source_hashes", "linked_objects", "fork_manifest_sha256",
            "patch_0002_sha256", "identity")
    check(all(first[k] == second[k] for k in keys) and
          (a / "KERNEL.SYS").read_bytes() == (b / "KERNEL.SYS").read_bytes() and
          (a / "CIUKIDF.SYS").read_bytes() == (b / "CIUKIDF.SYS").read_bytes(),
          "fork build is not byte-identical across roots and input mtimes")
    print(f"H57 fork: kernel {first['kernel_sha256']} {first['kernel_size']} bytes, CIUKIDF.SYS "
          f"{first['driver_sha256']} {first['driver_size']} bytes; identical across two roots/mtimes", flush=True)
    pristine = unified.build(OUT / "pristine", mtime=946684800)
    if pristine["status"] != "ok":
        raise SystemExit("H57 pristine build blocked: " + pristine.get("error", "?"))
    shutil.rmtree(b / "source")                     # build.json stays for the record
    return a, b, OUT / "pristine"


def d_volume():
    boot, _ = conformance.installed_boot(h56.release_folder())
    path = OUT / "d.vol"
    dos.write_fat16(path, bytes(boot), b"", {"/RO.TXT": b"HELLO WORLD"}, label="CIUKIRO", sectors=65536)
    return path


def boot(cpu, name, kernel, shell, files, device=b"DEVICE=C:\\CIUKIDF.SYS\r\n", args=(), directories=(),
         lastdrive=b"Z", expect_exit=0):
    config = (b"DOS=LOW\r\nFILES=40\r\nBUFFERS=10\r\nLASTDRIVE=" + lastdrive + b"\r\n" + device +
              b"SHELL=C:\\" + shell.encode() + b"\r\n")
    image = files | {"/KERNEL.SYS": kernel, "/FDCONFIG.SYS": config}
    path = h56.disk(OUT / f"{name}.img", image, directories)
    result = subprocess.run([str(cpu), str(path), *args], cwd=ROOT, capture_output=True, text=True, timeout=900)
    (OUT / f"{name}.log").write_text(result.stdout + result.stderr)
    path.unlink()
    run = h56.Boot(result.stdout + result.stderr)
    cpu_line = run.field("CPU")
    print(f"H57 boot {name}: exit={result.returncode} instructions={cpu_line['instructions']} "
          f"stop={cpu_line['stop']} failure={cpu_line['failure']}", flush=True)
    check(result.returncode == expect_exit, f"{name}: harness exit {result.returncode}")
    return run


def field(run, tag):
    match = re.search(rf"^H57 {tag} (.*)$", run.text, re.M)
    if not match:
        raise AssertionError("missing H57 " + tag)
    return dict(item.split("=", 1) for item in match.group(1).split() if "=" in item)


def passed(run, test):
    return re.search(rf"^H57 {re.escape(test)} PASS$", run.e9, re.M) is not None


H57_TESTS = ("classify", "files", "dup", "find", "attr", "rename", "dirs", "space", "lock", "extopen", "time",
             "fcb-close-metadata", "readonly-d", "critical-retry", "critical-fail", "nocrit", "critical-abort",
             "after-abort")


def production_vm(installed, identity):
    """f3-61: the production VM path the H57 CPU harness bypasses. The real
    dosvm/BIOS/DPMI/dos_file_vm.c code on libx86emu (tests/host/dosvm_guest.c,
    the H50 hook CPU build) boots the kernel=unified base the canonical builder
    makes (MBR, FAT16 at LBA 63, .SHA sidecar) exactly as /Programs/dosbox
    creates it: config.kernel=2 with the v1 bridge disabled. Before f3-61 the
    DFV2 gates were refused with the v1 bridge, CIUKIDF.SYS found no host and
    the fork ran unbound on its local FAT (T23 image b453bc92); once served,
    the native mount took LBA 0 (the MBR) as the FAT volume. Also the
    freedos-1.3 base, which must stay unbound."""
    hook = module("h61_hook_cpu", ROOT / "tests/host/dos/hook_cpu.py")
    hook.prepare()
    cpu = OUT / "dosvm_vm_cpu"
    subprocess.run(["clang", "-DFS_HOST", "-DCIUKI_DOSVM_FILE_CPU", "-D_POSIX_C_SOURCE=200809L", "-std=c17", "-O1", "-g",
                    "-Wall", "-Wextra", "-Werror", "-Wno-pointer-to-int-cast", "-fsanitize=address,undefined", "-pthread",
                    "-Isrc/kernel/include", "-Ibuild/host", "tests/host/dosvm_guest.c", "tests/host/dosvm_guest_cpu.c",
                    "src/kernel/lib/fmt.c", "src/kernel/lib/sha256.c", "src/kernel/vm/virtual_vga.c",
                    "src/kernel/vm/virtual_vga_bios.c", "src/kernel/vm/vga_x86.c", "src/kernel/vm/vga_presenter.c",
                    *h56.FS, "src/kernel/fs/partition.c", "src/kernel/drivers/blkpart.c", *VM, "-lx86emu", "-o", str(cpu)],
                   cwd=ROOT, check=True)
    ready = dos.assemble("src/probes/doscompat/dosready.asm", OUT / "programs/DOSREADY.COM")
    folder = h56.release_folder()
    ref = unified.reference_module()
    pristine, pristine_identity = ref.reference_files(folder)
    results = {}
    for name, guest in (("UNIFIED", (installed, identity)), ("FREEDOS", (pristine, pristine_identity))):
        image = OUT / "h61" / name / f"{name}.IMG"
        image.parent.mkdir(parents=True, exist_ok=True)
        ref.build_reference(image, {"/DOSREADY.COM": ready}, ("D48",), None, folder, guest=guest)
        check(image.read_bytes()[510:512] == b"\x55\xaa" and image.read_bytes()[0x1c6:0x1ca] == (63).to_bytes(4, "little"),
              f"h61 {name}: partitioned base (MBR, FAT16 at LBA 63)")
        variants = [("", {})] if name == "FREEDOS" else [("", {}), ("-unbound", {"GUEST_UNIFIED_UNBOUND": "1"})]
        for suffix, extra in variants:
            env = dict(os.environ, GUEST_KERNEL="unified" if name == "UNIFIED" else "freedos-1.3",
                       ASAN_OPTIONS="detect_leaks=0:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1", **extra)
            run = subprocess.run([str(cpu), str(image), "shell:TYPE C:\\FDAUTO.BAT", "-",
                                  "shell:TYPE C:\\NOFILE.TXT", "-"], cwd=ROOT, env=env,
                                 capture_output=True, text=True, timeout=600)
            log = run.stdout + run.stderr
            (OUT / f"h61-{name.lower()}{suffix}.log").write_text(log)
            end = re.search(r"^H61 IFS end (.*)$", log, re.M)
            fields = dict(item.split("=", 1) for item in end.group(1).split() if "=" in item) if end else {}
            results[name + suffix] = fields
            check(run.returncode == 0 and "guest runner: PASS" in log, f"h61 {name}{suffix}: production VM boot")
            absent = "CIUKIDF: no DFV2 host; driver not installed" in log
            if name == "FREEDOS":
                check(fields.get("bound") == "0", f"h61 FREEDOS: never bound {fields}")
            elif suffix:
                check(absent and fields.get("bound") == "0", f"h61 UNIFIED unbound replay: pre-f3-61 state {fields}")
            else:
                check(not absent and fields.get("active") == "1" and int(fields.get("owned", "0"), 16) & 4 and
                      fields.get("forged") == "0" and fields.get("critical") == "0" and fields.get("held") == "0" and
                      int(fields.get("reads", "0")) > 0 and fields.get("opens") == fields.get("closes"),
                      f"h61 UNIFIED: DFV2 bound and C: IFS active on the partitioned base {fields}")
                # f3-61c: the failed-request list names what the guest asked for.
                failed = re.findall(r"^H61 IFS end failed n=(\d+) fn=(11[0-9a-f]{2}) error=(\d+) name=(\S+)$", log, re.M)
                check(int(fields.get("failed", "0")) >= 1 and
                      len(failed) == min(int(fields.get("failed", "0")), 8) and
                      any(name == "NOFILE.TXT" and error in ("2", "18") for _, _, error, name in failed),
                      f"h61 UNIFIED: failed requests listed by function and name {failed}")
                results["UNIFIED-failed"] = failed
    print(f"H61 production VM: unified base IFS active={results.get('UNIFIED', {}).get('active')} "
          f"owned={results.get('UNIFIED', {}).get('owned')} reads={results.get('UNIFIED', {}).get('reads')}; "
          f"unbound replay bound={results.get('UNIFIED-unbound', {}).get('bound')}; "
          f"freedos-1.3 bound={results.get('FREEDOS', {}).get('bound')}; failed requests "
          f"{['%s:%s:%s' % (fn, error, name) for _, fn, error, name in results.get('UNIFIED-failed', [])]}", flush=True)


def released(run, name):
    destroy = field(run, "DESTROY")
    check(destroy["held"] == "0" and destroy["mounted"] == "0" and destroy["active"] == "0",
          f"{name}: native state after VM stop {destroy}")


def main():
    if not Path("/usr/include/x86emu.h").exists():
        print("H57: unavailable (libx86emu development headers missing)")
        return 1
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    QUALIFIED.unlink(missing_ok=True)
    fork_a, fork_b, pristine_dir = builds()
    kernel = (fork_a / "KERNEL.SYS").read_bytes()
    driver = (fork_a / "CIUKIDF.SYS").read_bytes()
    pristine = (pristine_dir / "KERNEL.SYS").read_bytes()
    cpu = compile_ifs()
    dvol = d_volume()
    guest = {f"/{name}": h56.assemble(ROOT / f"tests/host/dos/{source}.asm", OUT / "programs" / name)
             for name, source in (("H57.COM", "h57/h57"), ("H56.COM", "h56/h56"), ("CHILD.COM", "h56/child"),
                                  ("RUNALL.COM", "h56/runall"))}
    files = guest | {"/CIUKIDF.SYS": driver}
    boots = 0

    # H57-01..05 on the real CPU: the live IFS for C: and a read-only D:.
    run = boot(cpu, "ifs", kernel, "H57.COM F", files, args=("--d=" + str(dvol),))
    boots += 1
    for test in H57_TESTS:
        check(passed(run, test), f"ifs: guest test {test}")
    check("H57 END fails=0000" in run.e9, "ifs: guest failures")
    ifs, io = field(run, "IFS"), field(run, "IO")
    check(ifs["active"] == "1" and ifs["owned"] == "c" and ifs["activations"] == "1" and ifs["refusals"] == "0",
          f"ifs: activation {ifs}")
    check(io["int13_after"] == "0" and int(io["int13_before"]) > 0, f"ifs: guest INT 13h after activation {io}")
    check(ifs["critical"] == "3" and ifs["retries"] == "1" and io["critical_status"] == "3" and
          io["adapter_retries"] == "1" and ifs["forged"] == "0", f"ifs: critical producers {ifs} {io}")
    check(ifs["opens"] == ifs["closes"], f"ifs: every native description closed by DOS {ifs}")
    held = field(run, "HELD")
    check(held["final"] == "1", f"ifs: one retained search (no file) before VM stop {held}")
    native = run.field("NATIVE")
    check(native["bound"] == "1" and native["refused"] == "0", f"ifs: binding {native}")
    released(run, "ifs")

    # The f3-56 guest suite over the live backend (EXEC, TSR, PSP, internals).
    run = boot(cpu, "ifs-h56", kernel, "H56.COM N", files)
    boots += 1
    h56.common_pass(run, "ifs-h56", ("spoof", "exec-after-spoof"))
    check(field(run, "IO")["int13_after"] == "0", "ifs-h56: guest INT 13h after activation")
    released(run, "ifs-h56")

    # Activation barrier (f3-57 Amendment 1): a refused activation stops the
    # VM at the lifecycle gate with one record, before INSTALL/shell and
    # without a halted guest; /LOCAL keeps the local FAT; a view beyond
    # LASTDRIVE is refused after the mount and unwound.
    run = boot(cpu, "strict-mount", kernel, "H57.COM F", files, args=("--ifs-fail=mount",))
    boots += 1
    check("H56 VMSTOP stopped=1 record=[dosvm] unified_activation_refused reason=native_mount_failed" in run.text and
          "failure=none" in run.text and "system stopped" not in run.con and not run.e9.strip(),
          "strict-mount: VM stopped at the gate with one record")
    ifs, io = field(run, "IFS"), field(run, "IO")
    check(ifs["active"] == "0" and ifs["refusals"] == "1" and io["mounts"] == "0" and
          run.field("GATES")["local"] == "0", f"strict-mount: nothing published {ifs} {io}")
    run = boot(cpu, "local-mount", kernel, "H57.COM U", files, args=("--ifs-fail=mount",),
               device=b"DEVICE=C:\\CIUKIDF.SYS /LOCAL\r\n")
    boots += 1
    check("H56 VMSTOP stopped=0" in run.text, "local-mount: /LOCAL does not stop the VM")
    check(passed(run, "local") and "H57 END fails=0000" in run.e9 and
          "CiukiDOS: DFV2 binding refused (error 31); continuing unbound" in run.con, "local-mount: local FAT")
    check(run.field("GATES")["local"] == "0" and field(run, "IFS")["active"] == "0", "local-mount: unbound")
    run = boot(cpu, "lastdrive", kernel, "H57.COM F", files, args=("--d=" + str(dvol), "--ifs-fail=lastdrive"),
               lastdrive=b"Y")
    boots += 1
    ifs, io = field(run, "IFS"), field(run, "IO")
    check("record=[dosvm] unified_activation_refused reason=owned_drive_beyond_lastdrive" in run.text and
          io["mounts"] == "1" and io["unmounts"] == "1" and ifs["active"] == "0" and
          "system stopped" not in run.con and not run.e9.strip(),
          f"lastdrive: refused after the mount, unwound, VM stopped {ifs} {io}")

    # The unchanged f3-48 basic-fixture battery: pristine kernel (local FAT,
    # no driver) against the fork with the live IFS.
    probes = h56.conformance_probes()
    results = {}
    for name, kern, device in (("battery-pristine", pristine, b""), ("battery-ifs", kernel, None)):
        args = {} if device is None else {"device": device}
        run = boot(cpu, name, kern, "RUNALL.COM", files | probes["files"], directories=probes["directories"], **args)
        boots += 1
        results[name] = dict(re.findall(r"^(D48\d{3}) (PASS|FAIL) [0-9A-F]{4}$", run.e9, re.M))
        check("RUNALL END" in run.e9 and "RUNALL EXECFAIL" not in run.e9 and "RUNALL CDFAIL" not in run.e9,
              f"{name}: runner")
        check(len(results[name]) == len(probes["tags"]), f"{name}: {len(results[name])} results")
        if device is None:
            check(field(run, "IFS")["active"] == "1" and field(run, "IO")["int13_after"] == "0",
                  f"{name}: live IFS without guest INT 13h")
    differ = sorted(k for k in results["battery-pristine"] if results["battery-pristine"][k] != results["battery-ifs"].get(k))
    check(not differ, f"battery differs on {differ}")
    passed_count = sum(v == "PASS" for v in results["battery-ifs"].values())
    print(f"H57 battery: {len(probes['tags'])} basic f3-48 probes; pristine vs live IFS differences: {differ or 'none'}; "
          f"{passed_count} PASS on the IFS", flush=True)

    if failures:
        print(f"H57 FAIL: {len(failures)} checks failed")
        return 1
    record = dict(first=str(fork_a / "build.json"), second=str(fork_b / "build.json"), cpu_boot="PASS",
                  ifs="PASS", boots=boots, runtime_acceptance="pending-lead-f3-48-qemu-and-t23")
    # Image installation (kernel=unified): the guest files the canonical
    # builder installs come only from this record; read them back here.
    for tamper in (dict(cpu_boot="FAIL"), dict(ifs=None), dict(second=str(OUT / "pristine/build.json"))):
        bad = OUT / "tampered.json"
        bad.write_text(json.dumps(record | tamper))
        try:
            unified.fork_guest_files(bad)
            check(False, f"install: tampered qualification accepted {tamper}")
        except (ValueError, KeyError):
            pass
        bad.unlink()
    QUALIFIED.parent.mkdir(parents=True, exist_ok=True)
    QUALIFIED.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    ref = unified.reference_module()
    installed, identity = unified.guest_files()
    config = unified.fork_config(ref)
    check(installed["/KERNEL.SYS"] == kernel and installed["/CIUKIDF.SYS"] == driver and
          installed["/FDCONFIG.SYS"] == config and b"DEVICE=C:\\CIUKIDF.SYS\r\nSHELL=" in config and
          identity["kernel"]["personality"] == "ciukidos-fork" and identity["profile"]["ifs"] is True,
          "install: kernel=unified guest files are the qualified fork, driver and CONFIG")
    image = dict(installed, **{"/FDAUTO.BAT": ref.AUTO,
                               "/D48/UNI.JSN": (json.dumps(identity, sort_keys=True, indent=2) + "\n").encode()})
    check(unified.readback(lambda name: image[name], identity, {}), "install: read-back")
    image["/CIUKIDF.SYS"] = bytes([driver[0] ^ 1]) + driver[1:]
    try:
        unified.readback(lambda name: image[name], identity, {})
        check(False, "install: a corrupted CIUKIDF.SYS passed read-back")
    except ValueError:
        pass
    if failures:
        QUALIFIED.unlink(missing_ok=True)
        print(f"H57 FAIL: {len(failures)} checks failed")
        return 1
    print("H57 install: kernel=unified guest = fork kernel + CIUKIDF.SYS + DEVICE line; tampered records and "
          "a corrupted driver refused", flush=True)
    production_vm(installed, identity)
    if failures:
        QUALIFIED.unlink(missing_ok=True)
        print(f"H57 FAIL: {len(failures)} checks failed")
        return 1
    print(f"H57 PASS: {boots} CPU boots (live IFS suite, f3-56 suite on the IFS, strict/LOCAL/LASTDRIVE "
          f"activation refusals, f3-48 basic battery pristine vs IFS); qualification {QUALIFIED.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
