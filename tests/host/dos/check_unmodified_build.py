#!/usr/bin/env python3
"""H55-02/03 host-only checker. Never builds an OS image or launches a VM.

Build each root on a different wall-clock date, then compare/qualify:
 nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --out build/codex/h55-a --mtime 946684800
 nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --out build/codex/h55-b-longer --mtime 1609459200
 nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --compare build/codex/h55-a/build.json build/codex/h55-b-longer/build.json
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("unified_dos", ROOT / "scripts/unified_dos.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


def cpu_check(record, *, packed_reference=False):
    out = build.build_path(record["root"])
    ref = build.reference_module()
    (out / "mbr.bin").write_bytes(ref.reference_mbr())
    (out / "vbr.bin").write_bytes(ref.installed_boot()[0])
    program = out / "boot-cpu"
    subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(Path(__file__).with_name("unified_boot_test.c")),
                    "-lx86emu", "-o", str(program)], check=True)
    # Keep the caller/script's sanitizer settings, including leak detection.
    command = [str(program), str(out / "mbr.bin"), str(out / "vbr.bin"), str(out / "KERNEL.SYS")]
    if packed_reference:
        command.append("packed-reference")
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    (out / "boot-cpu.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        raise ValueError("CPU boot/relocation failed: " + str(result.returncode))
    print(result.stdout, end="")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--out", type=Path)
    mode.add_argument("--compare", nargs=2, type=Path)
    mode.add_argument("--cpu-reference", action="store_true", help="check the unchanged vendor kernel loader, not a rebuilt-kernel qualification")
    mode.add_argument("--source-out", type=Path, help="construct/read back source bundle without an image or a qualification claim")
    parser.add_argument("--mtime", type=int, default=build.EPOCH)
    args = parser.parse_args()
    if args.source_out:
        print(json.dumps(build.corresponding_source(args.source_out), sort_keys=True))
    elif args.cpu_reference:
        out = build.build_path(ROOT / "build/codex/h55-reference-cpu")
        out.mkdir(parents=True, exist_ok=True)
        files, _ = build.reference_module().reference_files()
        (out / "KERNEL.SYS").write_bytes(files["/KERNEL.SYS"])
        cpu_check(dict(root=str(out)), packed_reference=True)
    elif args.out:
        record = build.build(args.out, mtime=args.mtime)
        print(json.dumps({k: record[k] for k in ("status", "root")}, sort_keys=True))
        if record["status"] != "built-unqualified":
            print(record["error"], file=sys.stderr)
            return 1
        cpu_check(record)
    else:
        paths = [build.build_path(p) for p in args.compare]
        records = [json.loads(p.read_text()) for p in paths]
        build.compare(*records)
        for record in records:
            cpu_check(record)
        out = build.build_path(ROOT / "build/unified-dos")
        out.mkdir(exist_ok=True)
        (out / "qualified.json").write_text(json.dumps(dict(
            first=str(paths[0]), second=str(paths[1]), cpu_boot="PASS"), indent=2) + "\n")
        source = build.corresponding_source(out / "ciukidos-unified-source.zip")
        print("H55-02 PASS: different roots/mtimes/dates, raw kernel and source manifests identical")
        print(json.dumps(source, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print("H55 blocked: " + str(error), file=sys.stderr)
        sys.exit(1)
