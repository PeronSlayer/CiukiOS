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

COMDEMO_SRC="src/com/comdemo.asm"
COMDEMO_BIN="build/full/obj/comdemo.com"
MZDEMO_SRC="src/com/mzdemo.asm"
MZDEMO_BIN="build/full/obj/mzdemo.exe"
FILEIO_SRC="src/com/fileio.bin.asm"
FILEIO_BIN="build/full/obj/fileio.bin"
DELTEST_SRC="src/com/deltest.bin.asm"
DELTEST_BIN="build/full/obj/deltest.bin"

for f in "$BOOT_SRC" "$STAGE1_SRC" "$COMDEMO_SRC" "$MZDEMO_SRC" "$FILEIO_SRC" "$DELTEST_SRC"; do
	if [[ ! -f "$f" ]]; then
		echo "[build-full] ERROR: source not found: $f" >&2
		exit 1
	fi
done

echo "[build-full] assembling full stage0 boot sector"
nasm -f bin "$BOOT_SRC" -o "$BOOT_BIN"

BOOT_SIZE="$(stat -c%s "$BOOT_BIN")"
if [[ "$BOOT_SIZE" -ne 512 ]]; then
	echo "[build-full] ERROR: boot sector size is $BOOT_SIZE bytes (expected 512)" >&2
	exit 1
fi

echo "[build-full] assembling stage1 payload for full profile (FAT16)"
nasm -f bin "$STAGE1_SRC" \
	-D FAT_SPT="$FAT_SPT" \
	-D FAT_HEADS="$FAT_HEADS" \
	-D FAT_RESERVED_SECTORS="$FAT_RESERVED_SECTORS" \
	-D FAT_SECTORS_PER_FAT="$FAT_SECTORS_PER_FAT" \
	-D FAT_ROOT_DIR_SECTORS="$ROOT_DIR_SECTORS" \
	-D FAT_TYPE=16 \
	-D STAGE1_SELFTEST_AUTORUN=1 \
	-o "$STAGE1_BIN"

STAGE1_SIZE="$(stat -c%s "$STAGE1_BIN")"
if [[ "$STAGE1_SIZE" -gt "$STAGE1_SLOT_SIZE" ]]; then
	echo "[build-full] ERROR: stage1 payload is $STAGE1_SIZE bytes (max $STAGE1_SLOT_SIZE)" >&2
	exit 1
fi

echo "[build-full] preparing stage1 slot (${STAGE1_SECTORS} sectors)"
dd if=/dev/zero of="$STAGE1_SLOT_BIN" bs=512 count="$STAGE1_SECTORS" status=none
dd if="$STAGE1_BIN" of="$STAGE1_SLOT_BIN" conv=notrunc status=none

echo "[build-full] assembling application payloads"
nasm -f bin "$COMDEMO_SRC" -o "$COMDEMO_BIN"
nasm -f bin "$MZDEMO_SRC"  -o "$MZDEMO_BIN"
nasm -f bin "$FILEIO_SRC"  -o "$FILEIO_BIN"
nasm -f bin "$DELTEST_SRC" -o "$DELTEST_BIN"

COMDEMO_SIZE="$(stat -c%s "$COMDEMO_BIN")"
MZDEMO_SIZE="$(stat -c%s  "$MZDEMO_BIN")"
FILEIO_SIZE="$(stat -c%s  "$FILEIO_BIN")"
DELTEST_SIZE="$(stat -c%s "$DELTEST_BIN")"

if [[ "$FILEIO_SIZE" -le 512 ]]; then
	echo "[build-full] ERROR: FILEIO payload must span >1 cluster ($FILEIO_SIZE bytes)" >&2
	exit 1
fi

# FAT16 sector (little-endian 16-bit entries):
#  [0]=0xFFF8 media+reserved, [1]=0xFFFF, [2]=EOF(COMDEMO), [3]=EOF(MZDEMO),
#  [4]=0x0005 FILEIO chain->5, [5]=EOF(FILEIO), [6]=EOF(DELTEST)
FAT_SECTOR_BIN="build/full/obj/fat16_sector.bin"
printf 'F8FFFFFFFFFFFFFF0500FFFF FFFF' | tr -d ' ' | xxd -r -p > "$FAT_SECTOR_BIN"
dd if=/dev/zero bs=1 count=$((512 - 14)) status=none >> "$FAT_SECTOR_BIN"

# Helper: write a 32-byte FAT root directory entry
make_root_entry() {
	local out="$1" name="$2" cluster="$3" size="$4"
	dd if=/dev/zero of="$out" bs=1 count=32 status=none
	printf '%s' "$name" | dd of="$out" bs=1 seek=0 conv=notrunc status=none
	printf '\x20' | dd of="$out" bs=1 seek=11 conv=notrunc status=none
	printf "$(printf '\\x%02x\\x%02x' $((cluster & 0xFF)) $(((cluster >> 8) & 0xFF)))" \
		| dd of="$out" bs=1 seek=26 conv=notrunc status=none
	printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' \
		$((size & 0xFF)) $(((size >> 8) & 0xFF)) \
		$(((size >> 16) & 0xFF)) $(((size >> 24) & 0xFF)))" \
		| dd of="$out" bs=1 seek=28 conv=notrunc status=none
}

ROOT_ENTRY_COMDEMO="build/full/obj/root_comdemo.bin"
ROOT_ENTRY_MZDEMO="build/full/obj/root_mzdemo.bin"
ROOT_ENTRY_FILEIO="build/full/obj/root_fileio.bin"
ROOT_ENTRY_DELTEST="build/full/obj/root_deltest.bin"
make_root_entry "$ROOT_ENTRY_COMDEMO" 'COMDEMO COM' 2 "$COMDEMO_SIZE"
make_root_entry "$ROOT_ENTRY_MZDEMO"  'MZDEMO  EXE' 3 "$MZDEMO_SIZE"
make_root_entry "$ROOT_ENTRY_FILEIO"  'FILEIO  BIN' 4 "$FILEIO_SIZE"
make_root_entry "$ROOT_ENTRY_DELTEST" 'DELTEST BIN' 6 "$DELTEST_SIZE"

echo "[build-full] creating 128MB FAT16 image"
dd if=/dev/zero of="$IMG" bs=512 count="$TOTAL_SECTORS" status=none
dd if="$BOOT_BIN"           of="$IMG" bs=512 count=1                seek=0           conv=notrunc status=none
dd if="$STAGE1_SLOT_BIN"    of="$IMG" bs=512 count="$STAGE1_SECTORS" seek=1          conv=notrunc status=none
dd if="$FAT_SECTOR_BIN"     of="$IMG" bs=512 count=1                seek="$FAT1_LBA" conv=notrunc status=none
dd if="$FAT_SECTOR_BIN"     of="$IMG" bs=512 count=1                seek="$FAT2_LBA" conv=notrunc status=none
dd if="$ROOT_ENTRY_COMDEMO" of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 0))  conv=notrunc status=none
dd if="$ROOT_ENTRY_MZDEMO"  of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 32)) conv=notrunc status=none
dd if="$ROOT_ENTRY_FILEIO"  of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 64)) conv=notrunc status=none
dd if="$ROOT_ENTRY_DELTEST" of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 96)) conv=notrunc status=none
dd if="$COMDEMO_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + 0)) count=1 conv=notrunc status=none
dd if="$MZDEMO_BIN"  of="$IMG" bs=512 seek=$((DATA_LBA + 1)) count=1 conv=notrunc status=none
dd if="$FILEIO_BIN"  of="$IMG" bs=512 seek=$((DATA_LBA + 2)) count=2 conv=notrunc status=none
dd if="$DELTEST_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + 4)) count=1 conv=notrunc status=none

cat > build/full/README.txt << 'TXT'
CiukiOS Legacy v2 - Full profile (FAT16 baseline)

Image: ciukios-full.img (128MB)
State: BIOS stage0 -> stage1 with full DOS runtime and FAT16 file I/O
Filesystem: FAT16 (SPT=63 Heads=16 128MB) with COMDEMO/MZDEMO/FILEIO/DELTEST payloads
Boot path: stage0 at LBA0, stage1 payload in sectors 2-15
Data: cluster 2=COMDEMO, 3=MZDEMO, 4-5=FILEIO, 6=DELTEST
TXT

echo "[build-full] done: build/full/ciukios-full.img"
