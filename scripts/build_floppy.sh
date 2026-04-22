#!/usr/bin/env bash
set -euo pipefail

mkdir -p build/floppy
mkdir -p build/floppy/obj

BOOT_SRC="src/boot/floppy_boot.asm"
BOOT_BIN="build/floppy/obj/floppy_boot.bin"
IMG="build/floppy/ciukios-floppy.img"

if [[ ! -f "$BOOT_SRC" ]]; then
  echo "[build-floppy] ERROR: boot source not found: $BOOT_SRC" >&2
  exit 1
fi

echo "[build-floppy] assembling stage0 boot sector"
nasm -f bin "$BOOT_SRC" -o "$BOOT_BIN"

BOOT_SIZE="$(stat -c%s "$BOOT_BIN")"
if [[ "$BOOT_SIZE" -ne 512 ]]; then
  echo "[build-floppy] ERROR: boot sector size is $BOOT_SIZE bytes (expected 512)" >&2
  exit 1
fi

echo "[build-floppy] creating 1.44MB floppy image"
dd if=/dev/zero of=build/floppy/ciukios-floppy.img bs=512 count=2880 status=none
dd if="$BOOT_BIN" of="$IMG" bs=512 count=1 conv=notrunc status=none

cat > build/floppy/README.txt << 'TXT'
CiukiOS Legacy v2 - Floppy profile

Image: ciukios-floppy.img (1.44MB)
State: BIOS-bootable stage0 baseline
Boot path: 16-bit boot sector at LBA0
TXT

echo "[build-floppy] done: $IMG"
