#!/usr/bin/env bash
set -euo pipefail

mkdir -p build/floppy
mkdir -p build/floppy/obj

BOOT_SRC="src/boot/floppy_boot.asm"
BOOT_BIN="build/floppy/obj/floppy_boot.bin"
STAGE1_SRC="src/boot/floppy_stage1.asm"
STAGE1_BIN="build/floppy/obj/floppy_stage1.bin"
STAGE1_SLOT_BIN="build/floppy/obj/floppy_stage1_slot.bin"
STAGE1_SECTORS=4
STAGE1_SLOT_SIZE=$((STAGE1_SECTORS * 512))
COMDEMO_SRC="src/com/comdemo.asm"
COMDEMO_BIN="build/floppy/obj/comdemo.com"
COMDEMO_MAX_SIZE=512
COMDEMO_SECTOR_LBA=5
IMG="build/floppy/ciukios-floppy.img"

if [[ ! -f "$BOOT_SRC" ]]; then
  echo "[build-floppy] ERROR: boot source not found: $BOOT_SRC" >&2
  exit 1
fi
if [[ ! -f "$STAGE1_SRC" ]]; then
  echo "[build-floppy] ERROR: stage1 source not found: $STAGE1_SRC" >&2
  exit 1
fi
if [[ ! -f "$COMDEMO_SRC" ]]; then
  echo "[build-floppy] ERROR: COM demo source not found: $COMDEMO_SRC" >&2
  exit 1
fi

echo "[build-floppy] assembling stage0 boot sector"
nasm -f bin "$BOOT_SRC" -o "$BOOT_BIN"

BOOT_SIZE="$(stat -c%s "$BOOT_BIN")"
if [[ "$BOOT_SIZE" -ne 512 ]]; then
  echo "[build-floppy] ERROR: boot sector size is $BOOT_SIZE bytes (expected 512)" >&2
  exit 1
fi

echo "[build-floppy] assembling stage1 payload"
nasm -f bin "$STAGE1_SRC" -o "$STAGE1_BIN"

STAGE1_SIZE="$(stat -c%s "$STAGE1_BIN")"
if [[ "$STAGE1_SIZE" -gt "$STAGE1_SLOT_SIZE" ]]; then
  echo "[build-floppy] ERROR: stage1 payload is $STAGE1_SIZE bytes (max $STAGE1_SLOT_SIZE)" >&2
  exit 1
fi

echo "[build-floppy] preparing stage1 slot (${STAGE1_SECTORS} sectors)"
dd if=/dev/zero of="$STAGE1_SLOT_BIN" bs=512 count="$STAGE1_SECTORS" status=none
dd if="$STAGE1_BIN" of="$STAGE1_SLOT_BIN" conv=notrunc status=none

echo "[build-floppy] assembling COM demo payload"
nasm -f bin "$COMDEMO_SRC" -o "$COMDEMO_BIN"

COMDEMO_SIZE="$(stat -c%s "$COMDEMO_BIN")"
if [[ "$COMDEMO_SIZE" -gt "$COMDEMO_MAX_SIZE" ]]; then
  echo "[build-floppy] ERROR: COM demo payload is $COMDEMO_SIZE bytes (max $COMDEMO_MAX_SIZE)" >&2
  exit 1
fi

echo "[build-floppy] creating 1.44MB floppy image"
dd if=/dev/zero of=build/floppy/ciukios-floppy.img bs=512 count=2880 status=none
dd if="$BOOT_BIN" of="$IMG" bs=512 count=1 conv=notrunc status=none
dd if="$STAGE1_SLOT_BIN" of="$IMG" bs=512 seek=1 count="$STAGE1_SECTORS" conv=notrunc status=none
dd if="$COMDEMO_BIN" of="$IMG" bs=512 seek="$COMDEMO_SECTOR_LBA" count=1 conv=notrunc status=none

cat > build/floppy/README.txt << 'TXT'
CiukiOS Legacy v2 - Floppy profile

Image: ciukios-floppy.img (1.44MB)
State: BIOS stage0 -> stage1 chain-loader baseline
Boot path: stage0 at LBA0, stage1 payload in sectors 2-5
COM demo payload: raw sector-backed binary at sector 6 (LBA5)
TXT

echo "[build-floppy] done: $IMG"
