#!/usr/bin/env bash
set -euo pipefail

mkdir -p build/floppy
mkdir -p build/floppy/obj

BOOT_SRC="src/boot/floppy_boot.asm"
BOOT_BIN="build/floppy/obj/floppy_boot.bin"
STAGE1_SRC="src/boot/floppy_stage1.asm"
STAGE1_BIN="build/floppy/obj/floppy_stage1.bin"
STAGE1_SLOT_BIN="build/floppy/obj/floppy_stage1_slot.bin"
STAGE1_SECTORS=22
STAGE1_SLOT_SIZE=$((STAGE1_SECTORS * 512))
COMDEMO_SRC="src/com/comdemo.asm"
COMDEMO_BIN="build/floppy/obj/comdemo.com"
COMDEMO_MAX_SIZE=512
MZDEMO_SRC="src/com/mzdemo.asm"
MZDEMO_BIN="build/floppy/obj/mzdemo.exe"
MZDEMO_MAX_SIZE=512
FILEIO_SRC="src/com/fileio.bin.asm"
FILEIO_BIN="build/floppy/obj/fileio.bin"
FILEIO_MAX_SIZE=1024
DELTEST_SRC="src/com/deltest.bin.asm"
DELTEST_BIN="build/floppy/obj/deltest.bin"
DELTEST_MAX_SIZE=512

FAT_RESERVED_SECTORS=$((1 + STAGE1_SECTORS))
FAT_SECTORS_PER_FAT=9
FAT_COUNT=2
ROOT_ENTRIES=224
ROOT_DIR_SECTORS=$((ROOT_ENTRIES * 32 / 512))
FAT1_LBA=$FAT_RESERVED_SECTORS
FAT2_LBA=$((FAT1_LBA + FAT_SECTORS_PER_FAT))
ROOT_LBA=$((FAT2_LBA + FAT_SECTORS_PER_FAT))
DATA_LBA=$((ROOT_LBA + ROOT_DIR_SECTORS))
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
if [[ ! -f "$MZDEMO_SRC" ]]; then
  echo "[build-floppy] ERROR: MZ demo source not found: $MZDEMO_SRC" >&2
  exit 1
fi
if [[ ! -f "$FILEIO_SRC" ]]; then
  echo "[build-floppy] ERROR: fileio payload source not found: $FILEIO_SRC" >&2
  exit 1
fi
if [[ ! -f "$DELTEST_SRC" ]]; then
  echo "[build-floppy] ERROR: deltest payload source not found: $DELTEST_SRC" >&2
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

echo "[build-floppy] assembling MZ demo payload"
nasm -f bin "$MZDEMO_SRC" -o "$MZDEMO_BIN"

MZDEMO_SIZE="$(stat -c%s "$MZDEMO_BIN")"
if [[ "$MZDEMO_SIZE" -gt "$MZDEMO_MAX_SIZE" ]]; then
  echo "[build-floppy] ERROR: MZ demo payload is $MZDEMO_SIZE bytes (max $MZDEMO_MAX_SIZE)" >&2
  exit 1
fi

echo "[build-floppy] assembling file I/O payloads"
nasm -f bin "$FILEIO_SRC" -o "$FILEIO_BIN"
nasm -f bin "$DELTEST_SRC" -o "$DELTEST_BIN"

FILEIO_SIZE="$(stat -c%s "$FILEIO_BIN")"
if [[ "$FILEIO_SIZE" -gt "$FILEIO_MAX_SIZE" ]]; then
  echo "[build-floppy] ERROR: FILEIO payload is $FILEIO_SIZE bytes (max $FILEIO_MAX_SIZE)" >&2
  exit 1
fi
if [[ "$FILEIO_SIZE" -le 512 ]]; then
  echo "[build-floppy] ERROR: FILEIO payload must span >1 cluster (current $FILEIO_SIZE bytes)" >&2
  exit 1
fi

DELTEST_SIZE="$(stat -c%s "$DELTEST_BIN")"
if [[ "$DELTEST_SIZE" -gt "$DELTEST_MAX_SIZE" ]]; then
  echo "[build-floppy] ERROR: DELTEST payload is $DELTEST_SIZE bytes (max $DELTEST_MAX_SIZE)" >&2
  exit 1
fi

FAT_SECTOR_BIN="build/floppy/obj/fat_sector.bin"
ROOT_ENTRY_BIN="build/floppy/obj/root_comdemo_entry.bin"
ROOT_ENTRY_MZ_BIN="build/floppy/obj/root_mzdemo_entry.bin"
ROOT_ENTRY_FILEIO_BIN="build/floppy/obj/root_fileio_entry.bin"
ROOT_ENTRY_DELTEST_BIN="build/floppy/obj/root_deltest_entry.bin"

printf 'F0FFFFFFFFFF05F0FFFF0F00' | xxd -r -p > "$FAT_SECTOR_BIN"
dd if=/dev/zero bs=1 count=$((512 - 12)) status=none >> "$FAT_SECTOR_BIN"

dd if=/dev/zero of="$ROOT_ENTRY_BIN" bs=1 count=32 status=none
printf 'COMDEMO COM' | dd of="$ROOT_ENTRY_BIN" bs=1 seek=0 conv=notrunc status=none
printf '\x20' | dd of="$ROOT_ENTRY_BIN" bs=1 seek=11 conv=notrunc status=none
printf '\x02\x00' | dd of="$ROOT_ENTRY_BIN" bs=1 seek=26 conv=notrunc status=none
printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $((COMDEMO_SIZE & 0xFF)) $(((COMDEMO_SIZE >> 8) & 0xFF)) $(((COMDEMO_SIZE >> 16) & 0xFF)) $(((COMDEMO_SIZE >> 24) & 0xFF)))" | dd of="$ROOT_ENTRY_BIN" bs=1 seek=28 conv=notrunc status=none

dd if=/dev/zero of="$ROOT_ENTRY_MZ_BIN" bs=1 count=32 status=none
printf 'MZDEMO  EXE' | dd of="$ROOT_ENTRY_MZ_BIN" bs=1 seek=0 conv=notrunc status=none
printf '\x20' | dd of="$ROOT_ENTRY_MZ_BIN" bs=1 seek=11 conv=notrunc status=none
printf '\x03\x00' | dd of="$ROOT_ENTRY_MZ_BIN" bs=1 seek=26 conv=notrunc status=none
printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $((MZDEMO_SIZE & 0xFF)) $(((MZDEMO_SIZE >> 8) & 0xFF)) $(((MZDEMO_SIZE >> 16) & 0xFF)) $(((MZDEMO_SIZE >> 24) & 0xFF)))" | dd of="$ROOT_ENTRY_MZ_BIN" bs=1 seek=28 conv=notrunc status=none

dd if=/dev/zero of="$ROOT_ENTRY_FILEIO_BIN" bs=1 count=32 status=none
printf 'FILEIO  BIN' | dd of="$ROOT_ENTRY_FILEIO_BIN" bs=1 seek=0 conv=notrunc status=none
printf '\x20' | dd of="$ROOT_ENTRY_FILEIO_BIN" bs=1 seek=11 conv=notrunc status=none
printf '\x04\x00' | dd of="$ROOT_ENTRY_FILEIO_BIN" bs=1 seek=26 conv=notrunc status=none
printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $((FILEIO_SIZE & 0xFF)) $(((FILEIO_SIZE >> 8) & 0xFF)) $(((FILEIO_SIZE >> 16) & 0xFF)) $(((FILEIO_SIZE >> 24) & 0xFF)))" | dd of="$ROOT_ENTRY_FILEIO_BIN" bs=1 seek=28 conv=notrunc status=none

dd if=/dev/zero of="$ROOT_ENTRY_DELTEST_BIN" bs=1 count=32 status=none
printf 'DELTEST BIN' | dd of="$ROOT_ENTRY_DELTEST_BIN" bs=1 seek=0 conv=notrunc status=none
printf '\x20' | dd of="$ROOT_ENTRY_DELTEST_BIN" bs=1 seek=11 conv=notrunc status=none
printf '\x06\x00' | dd of="$ROOT_ENTRY_DELTEST_BIN" bs=1 seek=26 conv=notrunc status=none
printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $((DELTEST_SIZE & 0xFF)) $(((DELTEST_SIZE >> 8) & 0xFF)) $(((DELTEST_SIZE >> 16) & 0xFF)) $(((DELTEST_SIZE >> 24) & 0xFF)))" | dd of="$ROOT_ENTRY_DELTEST_BIN" bs=1 seek=28 conv=notrunc status=none

echo "[build-floppy] creating 1.44MB floppy image"
dd if=/dev/zero of=build/floppy/ciukios-floppy.img bs=512 count=2880 status=none
dd if="$BOOT_BIN" of="$IMG" bs=512 count=1 conv=notrunc status=none
dd if="$STAGE1_SLOT_BIN" of="$IMG" bs=512 seek=1 count="$STAGE1_SECTORS" conv=notrunc status=none
dd if="$FAT_SECTOR_BIN" of="$IMG" bs=512 seek="$FAT1_LBA" count=1 conv=notrunc status=none
dd if="$FAT_SECTOR_BIN" of="$IMG" bs=512 seek="$FAT2_LBA" count=1 conv=notrunc status=none
dd if="$ROOT_ENTRY_BIN" of="$IMG" bs=1 seek=$((ROOT_LBA * 512)) conv=notrunc status=none
dd if="$ROOT_ENTRY_MZ_BIN" of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 32)) conv=notrunc status=none
dd if="$ROOT_ENTRY_FILEIO_BIN" of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 64)) conv=notrunc status=none
dd if="$ROOT_ENTRY_DELTEST_BIN" of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 96)) conv=notrunc status=none
dd if="$COMDEMO_BIN" of="$IMG" bs=512 seek="$DATA_LBA" count=1 conv=notrunc status=none
dd if="$MZDEMO_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + 1)) count=1 conv=notrunc status=none
dd if="$FILEIO_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + 2)) count=2 conv=notrunc status=none
dd if="$DELTEST_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + 4)) count=1 conv=notrunc status=none

cat > build/floppy/README.txt << 'TXT'
CiukiOS Legacy v2 - Floppy profile

Image: ciukios-floppy.img (1.44MB)
State: BIOS stage0 -> stage1 chain-loader baseline
Boot path: stage0 at LBA0, stage1 payload in sectors 2-15
FAT layout: reserved sectors include stage1 area, FAT/root/data follow BPB geometry
COM demo payload: COMDEMO.COM root entry + first cluster at FAT data start
MZ demo payload: MZDEMO.EXE root entry + first cluster at FAT data start + 1
FILEIO payload: FILEIO.BIN root entry + cluster chain 4->5 for multi-cluster I/O tests
DELTEST payload: DELTEST.BIN root entry + cluster 6 for delete-path tests
TXT

echo "[build-floppy] done: $IMG"
