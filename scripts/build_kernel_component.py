#!/usr/bin/env python3
"""Build or rebuild only the independently recorded CiukiDOS kernel component."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any

from ciukidos_image import inspect_kernel


ROOT = Path(__file__).resolve().parents[1]
EXPECTED_SOURCE = Path("src/runtime/ciukidos.asm")
RECORD_SCHEMA = 1
PRIVATE_SYMBOLS = {
    "dos_indos_flag": "KL_INDOS",
    "fat_cache_valid": "KL_FAT_VALID",
    "fat_cache_dirty": "KL_FAT_DIRTY",
    "fat_cache_sector": "KL_FAT_SECTOR",
    "dos_mem_last_mcb_seg": "KL_LAST_MCB",
    "int21_last_ah": "KL_LAST_AH",
    "dos_exec_identity_psp": "KL_EXEC_PSP",
    "current_psp_seg": "KL_CURRENT_PSP",
    "boot_drive": "KL_BOOT_DRIVE",
    "int_default_iret": "KL_DEFAULT_IRET",
}


def _argument_path(argv: list[str], option: str) -> Path:
    try:
        index = argv.index(option)
    except ValueError as exc:
        raise ValueError(f"NASM command is missing {option}") from exc
    if index + 1 >= len(argv):
        raise ValueError(f"NASM command has no value after {option}")
    return Path(argv[index + 1])


def validate_command(argv: Any) -> tuple[Path, Path]:
    if not isinstance(argv, list) or not argv or not all(isinstance(arg, str) for arg in argv):
        raise ValueError("recorded compiler_argv must be a nonempty string list")
    if Path(argv[0]).name != "nasm":
        raise ValueError("kernel component command must invoke nasm")
    if EXPECTED_SOURCE.as_posix() not in argv:
        raise ValueError(f"kernel component command must compile {EXPECTED_SOURCE}")
    output = _argument_path(argv, "-o")
    listing = _argument_path(argv, "-l")
    return output, listing


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(value, indent=2, sort_keys=True) + "\n"
    fd, tmp_name = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(payload)
        os.replace(tmp_name, path)
    except Exception:
        try:
            os.unlink(tmp_name)
        except FileNotFoundError:
            pass
        raise


def run_kernel(argv: list[str], record_path: Path | None) -> tuple[Path, Path, bytes]:
    output, listing = validate_command(argv)
    subprocess.run(argv, cwd=ROOT, check=True)
    binary_path = output if output.is_absolute() else ROOT / output
    listing_path = listing if listing.is_absolute() else ROOT / listing
    if not binary_path.is_file() or not listing_path.is_file():
        raise ValueError("NASM did not create both the kernel binary and listing")
    data = binary_path.read_bytes()
    layout = inspect_kernel(data)
    layout_path = binary_path.parent / "kernel-layout.json"
    write_json(layout_path, layout)
    if record_path is not None:
        record = {
            "schema": RECORD_SCHEMA,
            "compiler_argv": argv,
            "kernel_output": output.as_posix(),
            "listing_output": listing.as_posix(),
            "kernel_bytes": len(data),
            "kernel_sha256": digest(data),
        }
        write_json(record_path, record)
    return binary_path, listing_path, data


def load_record(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read build record {path}: {exc}") from exc
    if not isinstance(value, dict) or value.get("schema") != RECORD_SCHEMA:
        raise ValueError("unsupported kernel build record schema")
    validate_command(value.get("compiler_argv"))
    return value


def listing_symbols(path: Path) -> dict[str, int]:
    """Read the ten private offsets consumed by build_vm_session.sh."""
    source_to_symbol = {source: symbol for source, symbol in PRIVATE_SYMBOLS.items()}
    result: dict[str, int] = {}
    pending_default_iret = False
    for line in path.read_text(encoding="ascii", errors="replace").splitlines():
        if re.search(r"\bint_default_iret:\s*$", line):
            pending_default_iret = True
        elif pending_default_iret:
            code = re.match(r"\s*\d+\s+([0-9A-F]{8})\s+CF\b", line)
            if code:
                result["int_default_iret"] = int(code.group(1), 16)
                pending_default_iret = False
        label = re.match(
            r"\s*\d+\s+([0-9A-F]{8})\s+\S+\s+(?:<\d+>\s+)?(\w+)\s+db\b",
            line,
        )
        if not label:
            label = re.match(
                r"\s*\d+\s+([0-9A-F]{8})\s+\S+\s+(?:<\d+>\s+)?(\w+)\s+dw\b",
                line,
            )
        if label and label.group(2) in source_to_symbol:
            result[label.group(2)] = int(label.group(1), 16)
    missing = sorted(set(PRIVATE_SYMBOLS) - set(result))
    if missing:
        raise ValueError(f"kernel listing is missing private offsets: {', '.join(missing)}")
    return result


def parse_generated_layout(path: Path) -> dict[str, int]:
    result: dict[str, int] = {}
    for line in path.read_text(encoding="ascii", errors="replace").splitlines():
        match = re.match(r"\s*(KL_[A-Z0-9_]+)\s+equ\s+([0-9A-Fa-f]+)h?\s*$", line)
        if match:
            result[match.group(1)] = int(match.group(2), 16)
    if result.get("KL_PRESENT") != 1:
        raise ValueError("generated VM session kernel_layout.inc is incomplete")
    missing = sorted(set(PRIVATE_SYMBOLS.values()) - set(result))
    if missing:
        raise ValueError(f"generated VM session layout is missing: {', '.join(missing)}")
    return result


def patch_image(image: Path, kernel_path: Path, listing_path: Path, kernel_data: bytes) -> None:
    from qemu_test_installed_hdd import FAT16

    fs = FAT16(image)
    installed_kernel = fs.read("SYSTEM/CIUKIDOS.SYS")
    inspect_kernel(installed_kernel)

    session_dir = kernel_path.parent / "vm-window" / "session"
    generated_layout = parse_generated_layout(session_dir / "kernel_layout.inc")
    current_offsets = listing_symbols(listing_path)
    changed = []
    for source_name, layout_name in PRIVATE_SYMBOLS.items():
        actual = current_offsets[source_name]
        generated = generated_layout[layout_name]
        if actual != generated:
            changed.append(f"{layout_name}: generated 0x{generated:X}, rebuilt 0x{actual:X}")
    if changed:
        raise ValueError(
            "kernel private offsets changed; rebuild VM modules before image injection: "
            + "; ".join(changed)
        )

    lfn_artifact = kernel_path.parent / "lfn" / "LFN.COM"
    if not lfn_artifact.is_file():
        raise ValueError(f"matching LFN artifact is missing: {lfn_artifact}")
    expected_lfn = lfn_artifact.read_bytes()
    installed_lfn = fs.read("SYSTEM/LFN.COM")
    if installed_lfn != expected_lfn:
        raise ValueError("image SYSTEM/LFN.COM does not match the current kernel output artifact")

    module_artifacts = {
        "VM/CVSESS.DLL": session_dir / "CVSESSION.DLL",
        "VM/VMFORK.COM": kernel_path.parent / "vm-window" / "VMFORK.COM",
        "VM/VMFORK.EXE": kernel_path.parent / "vm-window" / "VMFORK.EXE",
    }
    for image_name, artifact in module_artifacts.items():
        if not artifact.is_file():
            raise ValueError(f"matching VM module artifact is missing: {artifact}")
        if fs.read(image_name) != artifact.read_bytes():
            raise ValueError(f"image {image_name} does not match current VM module artifact")

    volume = f"{image.resolve()}@@{fs.start}"
    subprocess.run([
        "mcopy", "-o", "-i", volume, str(kernel_path.resolve()), "::SYSTEM/CIUKIDOS.SYS"
    ], cwd=ROOT, check=True)
    updated = FAT16(image).read("SYSTEM/CIUKIDOS.SYS")
    if updated != kernel_data:
        raise ValueError("patched image kernel does not match rebuilt kernel bytes")
    inspect_kernel(updated)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--record", type=Path, help="compile after -- and write a command record")
    mode.add_argument("--rebuild", type=Path, help="replay a recorded kernel-only NASM command")
    parser.add_argument("--image", type=Path, help="patch an existing matching FAT16 image after rebuild")
    args, remainder = parser.parse_known_args(argv)
    try:
        if args.record:
            if args.image:
                raise ValueError("--image is supported only with --rebuild")
            if not remainder or remainder[0] != "--":
                raise ValueError("--record requires -- followed by the literal NASM argv")
            command = remainder[1:]
            if not command:
                raise ValueError("empty NASM argv")
            output, listing, data = run_kernel(command, args.record)
        else:
            if remainder:
                raise ValueError("--rebuild does not accept additional compiler arguments")
            record = load_record(args.rebuild)
            output, listing, data = run_kernel(record["compiler_argv"], None)
            record.update(kernel_bytes=len(data), kernel_sha256=digest(data))
            write_json(args.rebuild, record)
            if args.image:
                patch_image(args.image, output, listing, data)
        print(json.dumps({
            "kernel": str(output),
            "listing": str(listing),
            "bytes": len(data),
            "sha256": digest(data),
            "image_updated": str(args.image) if args.image else None,
        }, sort_keys=True))
        return 0
    except (OSError, subprocess.CalledProcessError, ValueError) as exc:
        print(f"build_kernel_component: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
