#!/usr/bin/env python3
"""Real HDD boot, floppy, SeaBIOS USB-storage and ATAPI CD file reads/copies."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess

from qemu_test_full_display_profile import VM


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--disk", type=Path, default=Path("build/full/t23-runtime-repair-2026-09-06/final-install/target.img"))
    parser.add_argument("--program", type=Path, default=Path("build/full/media-work/media.com"))
    parser.add_argument("--use-installed-program", action="store_true", help="Test the existing HDD payload without replacing it")
    parser.add_argument("--inspect-elf", type=Path, help="Matching MEDIA.ELF, used only to inspect the actual selected backend")
    parser.add_argument("--fixtures", type=Path, default=Path("build/full/media-work/tests/fixtures"))
    parser.add_argument("--output", type=Path, default=Path("build/full/media-work/qemu"))
    parser.add_argument("--interactive-only", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    disk = args.output.resolve() / "installed.img"
    shutil.copyfile(args.disk, disk)
    frozen_program = args.output.resolve() / "media.com"
    volume = str(disk) + "@@32256"
    if args.use_installed_program:
        frozen_program.write_bytes(subprocess.check_output(["mtype", "-i", volume, "::APPS/MEDIA.COM"]))
    else:
        shutil.copyfile(args.program, frozen_program)
        subprocess.run(["mcopy", "-o", "-i", volume, str(frozen_program), "::APPS/MEDIA.COM"], check=True)
    symbols = {}
    if args.inspect_elf:
        elf_binary = args.output.resolve() / "elf-reference.com"
        subprocess.run(["ia16-elf-objcopy", "-O", "binary", str(args.inspect_elf), str(elf_binary)], check=True)
        assert elf_binary.read_bytes() == frozen_program.read_bytes(), "ELF does not describe the installed binary"
        nm = subprocess.check_output(["ia16-elf-nm", "-n", str(args.inspect_elf)], text=True)
        symbols = {name: int(address, 16) for address, _, name in (line.split() for line in nm.splitlines())}
    inputs = {name: args.fixtures.resolve()/name for name in ["fat12.img", "fat16.img", "data.iso"]}
    hashes = {name: digest(path) for name, path in inputs.items()}
    vm = VM(disk, args.output.resolve(), [
        "-drive", f"file={inputs['fat12.img']},format=raw,if=floppy,index=0,readonly=on",
        "-drive", f"file={inputs['data.iso']},format=raw,if=ide,index=2,media=cdrom,readonly=on",
        "-usb", "-drive", f"file={inputs['fat16.img']},format=raw,if=none,id=usbmedia,readonly=on",
        "-device", "usb-storage,drive=usbmedia",
    ])
    checks = []
    backends = []
    try:
        vm.wait("CiukiOS SHELL C:\\", timeout=90)
        offset = vm.offset(); vm.text("cd \\APPS"); vm.wait("CiukiOS SHELL C:\\APPS>", offset, timeout=30)
        cases = [] if args.interactive_only else [("FLOPPY", "FLOP.BIN"), ("USB", "USB.BIN"), ("CD", "CD.BIN"), ("CD:IDE", "ATAPI.BIN")]
        for device, destination in cases:
            if symbols:
                offset = vm.offset(); vm.text(f"media {device}"); vm.wait("Media />", offset, timeout=60)
                # MEDIA's private stack shares its COM segment. Freeze the VM
                # and verify its actual code before interpreting live globals.
                vm.hmp("stop")
                try:
                    registers = vm.hmp("info registers").decode(errors="replace")
                    segment = int(re.search(r"SS\s*=([0-9a-fA-F]+)", registers)[1], 16)
                    snapshot = args.output.resolve() / f"backend-{len(backends)}.bin"
                    vm.hmp(f'pmemsave {segment * 16} 65536 "{snapshot}"')
                    state = snapshot.read_bytes()
                    assert state[0x100:0x120] == frozen_program.read_bytes()[:32], "SS is not the MEDIA program segment"
                    base = int.from_bytes(state[symbols["atapi_base"]:symbols["atapi_base"]+2], "little")
                    drive = state[symbols["drive"]]
                    if device == "CD":
                        assert base == 0 and 0xe0 <= drive < 0xe8, "CD silently fell back to ATAPI"
                    elif device == "CD:IDE":
                        assert base in (0x1f0, 0x170), "ATAPI was not selected"
                    elif device == "FLOPPY":
                        assert base == 0 and drive == 0
                    elif device == "USB":
                        assert base == 0 and drive == 0x81, "SeaBIOS did not expose the attached USB disk"
                    backends.append({"request": device, "bios_drive": hex(drive), "atapi_port": hex(base)})
                finally:
                    vm.hmp("cont")
                offset = vm.offset(); vm.text("exit"); vm.wait("CiukiOS SHELL C:\\APPS>", offset, timeout=60)
            vm.command(f"media {device} dir /nested/deep", "README.TXT", timeout=60)
            vm.command(f"media {device} copy /payload.bin C:\\{destination}", "Copied 8965 bytes", timeout=60)
            checks.append({"device": device, "destination": destination})
        if not args.interactive_only:
            vm.command("media FLOPPY copy /payload.bin C:\\FLOP.BIN", "Destination already exists", timeout=60)
        offset = vm.offset(); vm.text("media FLOPPY"); vm.wait("Media />", offset, timeout=60)
        offset = vm.offset(); vm.text("cd nested"); vm.wait("Media /nested>", offset)
        offset = vm.offset(); vm.text("cd deep"); vm.wait("Media /nested/deep>", offset)
        offset = vm.offset(); vm.text("dir"); vm.wait("README.TXT", offset)
        vm.wait("Media /nested/deep>", offset)
        offset = vm.offset(); vm.text("exit"); vm.wait("CiukiOS SHELL C:\\APPS>", offset, timeout=60)
        vm.hmp("eject -f floppy0")
        offset = vm.offset(); vm.text("media FLOPPY")
        vm.wait("Press any key to return to CiukiOS.", offset, timeout=60)
        log = subprocess.check_output(["scripts/serial_log_normalize.py", "--offset", str(offset), str(vm.serial)])
        assert b"CiukiOS SHELL C:\\APPS>" not in log, "Media error vanished before dismissal"
        vm.shot("missing-media")
        vm.key("esc"); vm.wait("CiukiOS SHELL C:\\APPS>", offset, timeout=60)
        vm.command("echo MEDIA-RETURN-OK", "MEDIA-RETURN-OK")
        vm.shot("after-media")
    except BaseException:
        vm.shot("failure")
        raise
    finally:
        vm.close()
    expected = (args.fixtures / "PAYLOAD.BIN").read_bytes()
    for check in checks:
        content = subprocess.check_output(["mtype", "-i", volume, "::" + check["destination"]])
        assert content == expected, check
        check["sha256"] = hashlib.sha256(content).hexdigest()
        check["status"] = "PASS"
    assert {name: digest(path) for name, path in inputs.items()} == hashes, "Source media changed"
    installed_program = subprocess.check_output(["mtype", "-i", volume, "::APPS/MEDIA.COM"])
    assert installed_program == frozen_program.read_bytes(), "The installed test program changed"
    report = {"installed_disk_input_sha256": digest(args.disk), "program_sha256": digest(frozen_program),
              "program_replaced": not args.use_installed_program, "observed_backends": backends,
              "copies": checks, "interactive_browser": "PASS", "missing_media_dismissal": "PASS", "source_media_unchanged": hashes}
    (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
