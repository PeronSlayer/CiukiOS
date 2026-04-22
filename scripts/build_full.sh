#!/usr/bin/env bash
set -euo pipefail

mkdir -p build/full
mkdir -p build/full/obj

BOOT_SRC="src/boot/full_boot.asm"
BOOT_BIN="build/full/obj/full_boot.bin"
STAGE1_SRC="src/boot/floppy_stage1.asm"
STAGE1_BIN="build/full/obj/full_stage1.bin"
STAGE1_SLOT_BIN="build/full/obj/full_stage1_slot.bin"

IMG="build/full/ciukios-full.img"
TOTAL_SECTORS=262144
STAGE1_SECTORS=14
STAGE1_SLOT_SIZE=$((STAGE1_SECTORS * 512))

FAT_SPT=63
FAT_HEADS=16
FAT_RESERVED_SECTORS=$((1 + STAGE1_SECTORS))
FAT_SECTORS_PER_FAT=128
FAT_COUNT=2
ROOT_ENTRIES=512
ROOT_DIR_SECTORS=$((ROOT_ENTRIES * 32 / 512))
FAT1_LBA=$FAT_RESERVED_SECTORS
FAT2_LBA=$((FAT1_LBA + FAT_SECTORS_PER_FAT))
ROOT_LBA=$((FAT2_LBA + FAT_SECTORS_PER_FAT))
DATA_LBA=$((ROOT_LBA + ROOT_DIR_SECTORS))

if [[ ! -f "$BOOT_SRC" ]]; then
	echo "[build-full] ERROR: boot source not found: $BOOT_SRC" >&2
	exit 1
fi
if [[ ! -f "$STAGE1_SRC" ]]; then
	echo "[build-full] ERROR: stage1 source not found: $STAGE1_SRC" >&2
	exit 1
fi

echo "[build-full] assembling full stage0 boot sector"
nasm -f bin "$BOOT_SRC" -o "$BOOT_BIN"

BOOT_SIZE="$(stat -c%s "$BOOT_BIN")"
if [[ "$BOOT_SIZE" -ne 512 ]]; then
	echo "[build-full] ERROR: boot sector size is $BOOT_SIZE bytes (expected 512)" >&2
	exit 1
fi

echo "[build-full] assembling stage1 payload for full profile"
nasm -f bin "$STAGE1_SRC" \
	-D FAT_SPT="$FAT_SPT" \
	-D FAT_HEADS="$FAT_HEADS" \
	-D FAT_RESERVED_SECTORS="$FAT_RESERVED_SECTORS" \
	-D FAT_SECTORS_PER_FAT="$FAT_SECTORS_PER_FAT" \
	-D FAT_ROOT_DIR_SECTORS="$ROOT_DIR_SECTORS" \
	-D STAGE1_SELFTEST_AUTORUN=0 \
	-o "$STAGE1_BIN"

STAGE1_SIZE="$(stat -c%s "$STAGE1_BIN")"
if [[ "$STAGE1_SIZE" -gt "$STAGE1_SLOT_SIZE" ]]; then
	echo "[build-full] ERROR: stage1 payload is $STAGE1_SIZE bytes (max $STAGE1_SLOT_SIZE)" >&2
	exit 1
fi

echo "[build-full] preparing stage1 slot (${STAGE1_SECTORS} sectors)"
dd if=/dev/zero of="$STAGE1_SLOT_BIN" bs=512 count="$STAGE1_SECTORS" status=none
dd if="$STAGE1_BIN" of="$STAGE1_SLOT_BIN" conv=notrunc status=none

FAT_SECTOR_BIN="build/full/obj/fat16_sector.bin"
printf 'F8FFFFFF' | xxd -r -p > "$FAT_SECTOR_BIN"
dd if=/dev/zero bs=1 count=$((512 - 4)) status=none >> "$FAT_SECTOR_BIN"

echo "[build-full] creating 128MB FAT16 image"
dd if=/dev/zero of="$IMG" bs=512 count="$TOTAL_SECTORS" status=none
dd if="$BOOT_BIN" of="$IMG" bs=512 count=1 conv=notrunc status=none
dd if="$STAGE1_SLOT_BIN" of="$IMG" bs=512 seek=1 count="$STAGE1_SECTORS" conv=notrunc status=none
dd if="$FAT_SECTOR_BIN" of="$IMG" bs=512 seek="$FAT1_LBA" count=1 conv=notrunc status=none
dd if="$FAT_SECTOR_BIN" of="$IMG" bs=512 seek="$FAT2_LBA" count=1 conv=notrunc status=none

cat > build/full/README.txt << 'TXT'
CiukiOS Legacy v2 - Full profile (FAT16 baseline)

Image: ciukios-full.img (128MB)
State: BIOS stage0 -> stage1 bootable baseline
Filesystem: FAT16 baseline geometry (BPB + FAT copies + root/data layout)
Boot path: stage0 at LBA0, stage1 payload in sectors 2-15
TXT

echo "[build-full] done: build/full/ciukios-full.img"
