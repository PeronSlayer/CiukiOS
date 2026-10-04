#!/usr/bin/env python3
"""Read the four raw CiukiOS boot records without modifying the disk."""

import argparse
from pathlib import Path
import subprocess


STAGES = {
    1: "MBR entered",
    2: "FAT16 boot sector entered",
    3: "Stage1 read",
    4: "Stage1 entered",
    5: "CiukiDOS loaded and validated",
    0xE0: "MBR could not read the FAT16 boot sector",
    0xE1: "FAT16 boot sector could not read Stage1",
    0xE2: "Stage1 could not load or validate CiukiDOS",
}

SHELL_STAGES = {
    0x00: "SHELL.COM entered",
    0x01: "display services returned",
    0x02: "input and startup drivers returned",
    0x03: "HARD startup choice returned",
    0x04: "long-name startup returned",
    0x05: "VM startup returned",
    0x06: "entering desktop",
    0x07: "desktop assets loaded",
    0x08: "desktop settings loaded",
    0x09: "sound startup returned",
    0x0A: "UI icons loaded",
    0x0B: "mouse reset returned",
    0x0C: "video profile resolved",
    0x0D: "VGA mode switch about to run",
    0x0E: "video initialization returned",
    0x0F: "applications and windows resumed",
    0x10: "first desktop frame painted",
    0x11: "first keyboard event read",
    0x12: "first mouse motion read",
    0x13: "first UI input poll about to run",
    0x14: "first UI input poll returned",
    0x15: "first idle HLT about to run",
    0x16: "INT 33h mouse reset succeeded",
    0x17: "INT 33h mouse reset unavailable",
    0x18: "first idle HLT woke on an interrupt",
    0x19: "display band allocated",
    0x1A: "display band allocation failed",
    0x1B: "legacy BIOS keyboard read used",
    0x1C: "master PIC IRQ1 unmasked",
    0x1D: "master PIC IRQ1 masked",
    0x1E: "slave PIC IRQ12 unmasked",
    0x1F: "slave PIC IRQ12 masked",
    0x20: "CiukiDOS owns INT 33h",
    0x21: "external driver owns INT 33h",
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=Path, help="whole-disk image or read-only block device")
    parser.add_argument("--shell-log", action="store_true",
                        help="also read SYSTEM/BOOT.LOG from the FAT16 partition")
    args = parser.parse_args()
    rows = []
    with args.disk.open("rb", buffering=0) as disk:
        for slot in range(1, 5):
            disk.seek(slot * 512)
            record = disk.read(512)
            if len(record) != 512 or record[:4] != b"CKLG":
                continue
            rows.append((int.from_bytes(record[4:6], "little"), slot,
                         record[6], record[8]))
    if not rows:
        print("No CiukiOS boot records. The BIOS may not have entered this MBR,")
        print("or its disk writes may have failed. This is not proof of either cause.")
    else:
        for sequence, slot, stage, drive in sorted(rows):
            print(f"boot {sequence:5}  slot {slot}  BIOS drive 0x{drive:02X}  "
                  f"stage 0x{stage:02X}: {STAGES.get(stage, 'unknown')}")
    if args.shell_log:
        result = subprocess.run(
            ["mtype", "-i", f"{args.disk}@@{63 * 512}",
             "::SYSTEM/BOOT.LOG"], capture_output=True, check=False)
        print("\nSYSTEM/BOOT.LOG:")
        if result.returncode:
            print(result.stderr.decode("utf-8", errors="replace").strip())
        else:
            data = result.stdout.decode("ascii", errors="replace")
            for line in data.splitlines():
                if len(line) == 3 and line[0] == "S":
                    try:
                        stage = int(line[1:], 16)
                    except ValueError:
                        pass
                    else:
                        print(f"{line}  {SHELL_STAGES.get(stage, 'unknown')}")
                        continue
                print(line)


if __name__ == "__main__":
    main()
