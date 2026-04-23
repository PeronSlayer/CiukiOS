#!/usr/bin/env bash
set -euo pipefail

# Default CIUKIOS_ROOT to the repository root (parent of scripts/) when not set externally.
: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

if [[ "$(uname -s)" == "Darwin" ]]; then
	# Allow direct invocation on macOS without going through the wrapper entrypoint.
	source "$(cd "$(dirname "${BASH_SOURCE[0]}")/macos" && pwd)/common.sh"
	ciuk_macos_prepare_tools
	ciuk_macos_check_required
	cd "$CIUKIOS_ROOT"
fi

mkdir -p build/full
mkdir -p build/full/obj

BOOT_SRC="src/boot/full_boot.asm"
BOOT_BIN="build/full/obj/full_boot.bin"
STAGE1_SRC="src/boot/floppy_stage1.asm"
STAGE1_BIN="build/full/obj/full_stage1.bin"
STAGE1_SLOT_BIN="build/full/obj/full_stage1_slot.bin"
STAGE2_SRC="src/boot/full_stage2.asm"
STAGE2_BIN="build/full/obj/full_stage2.bin"
STAGE2_MAX_SIZE=512
OPENGEM_TRY_EXEC="${CIUKIOS_OPENGEM_TRY_EXEC:-0}"

IMG="build/full/ciukios-full.img"
TOTAL_SECTORS=262144
STAGE1_SECTORS=24
STAGE1_SLOT_SIZE=$((STAGE1_SECTORS * 512))

FAT_SPT=63
FAT_HEADS=16
FAT_SECTORS_PER_CLUSTER=8
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
OPENGEM_PAYLOAD_DIR="assets/full/opengem"
OPENGEM_UPSTREAM_DIR="$OPENGEM_PAYLOAD_DIR/upstream/OPENGEM7-RC3"
INCLUDE_OPENGEM_PAYLOAD="${CIUKIOS_INCLUDE_OPENGEM:-1}"
STAGE1_SELFTEST_AUTORUN="${CIUKIOS_STAGE1_SELFTEST_AUTORUN:-0}"
STAGE2_AUTORUN="${CIUKIOS_STAGE2_AUTORUN:-0}"

for f in "$BOOT_SRC" "$STAGE1_SRC" "$STAGE2_SRC" "$COMDEMO_SRC" "$MZDEMO_SRC" "$FILEIO_SRC" "$DELTEST_SRC"; do
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
	-D FAT_SECTORS_PER_CLUSTER="$FAT_SECTORS_PER_CLUSTER" \
	-D FAT_SECTORS_PER_FAT="$FAT_SECTORS_PER_FAT" \
	-D FAT_ROOT_DIR_SECTORS="$ROOT_DIR_SECTORS" \
	-D FAT_TYPE=16 \
	-D STAGE1_SELFTEST_AUTORUN="$STAGE1_SELFTEST_AUTORUN" \
	-D STAGE2_AUTORUN="$STAGE2_AUTORUN" \
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
nasm -f bin "$STAGE2_SRC" -D OPENGEM_TRY_EXEC="$OPENGEM_TRY_EXEC" -o "$STAGE2_BIN"
nasm -f bin "$COMDEMO_SRC" -o "$COMDEMO_BIN"
nasm -f bin "$MZDEMO_SRC"  -o "$MZDEMO_BIN"
nasm -f bin "$FILEIO_SRC"  -o "$FILEIO_BIN"
nasm -f bin "$DELTEST_SRC" -o "$DELTEST_BIN"

STAGE2_SIZE="$(stat -c%s "$STAGE2_BIN")"
if [[ "$STAGE2_SIZE" -gt "$STAGE2_MAX_SIZE" ]]; then
	echo "[build-full] ERROR: stage2 payload is $STAGE2_SIZE bytes (max $STAGE2_MAX_SIZE)" >&2
	exit 1
fi

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
#  [4]=EOF(FILEIO), [5]=free, [6]=EOF(DELTEST), [7]=EOF(STAGE2)
FAT_SECTOR_BIN="build/full/obj/fat16_sector.bin"
printf 'F8FFFFFFFFFFFFFFFFFF0000FFFFFFFF' | tr -d ' ' | xxd -r -p > "$FAT_SECTOR_BIN"
dd if=/dev/zero bs=1 count=$((512 - 16)) status=none >> "$FAT_SECTOR_BIN"

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
ROOT_ENTRY_STAGE2="build/full/obj/root_stage2.bin"
make_root_entry "$ROOT_ENTRY_COMDEMO" 'COMDEMO COM' 2 "$COMDEMO_SIZE"
make_root_entry "$ROOT_ENTRY_MZDEMO"  'MZDEMO  EXE' 3 "$MZDEMO_SIZE"
make_root_entry "$ROOT_ENTRY_FILEIO"  'FILEIO  BIN' 4 "$FILEIO_SIZE"
make_root_entry "$ROOT_ENTRY_DELTEST" 'DELTEST BIN' 6 "$DELTEST_SIZE"
make_root_entry "$ROOT_ENTRY_STAGE2"  'STAGE2  BIN' 7 "$STAGE2_SIZE"

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
dd if="$ROOT_ENTRY_STAGE2"  of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 128)) conv=notrunc status=none
dd if="$COMDEMO_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((2 - 2) * FAT_SECTORS_PER_CLUSTER))) count=1 conv=notrunc status=none
dd if="$MZDEMO_BIN"  of="$IMG" bs=512 seek=$((DATA_LBA + ((3 - 2) * FAT_SECTORS_PER_CLUSTER))) count=1 conv=notrunc status=none
dd if="$FILEIO_BIN"  of="$IMG" bs=512 seek=$((DATA_LBA + ((4 - 2) * FAT_SECTORS_PER_CLUSTER))) count=2 conv=notrunc status=none
dd if="$DELTEST_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((6 - 2) * FAT_SECTORS_PER_CLUSTER))) count=1 conv=notrunc status=none
dd if="$STAGE2_BIN"  of="$IMG" bs=512 seek=$((DATA_LBA + ((7 - 2) * FAT_SECTORS_PER_CLUSTER))) count=1 conv=notrunc status=none

OPENGEM_STAGE_DIR="build/full/obj/opengem_stage"
mkdir -p "$OPENGEM_STAGE_DIR"
rm -f "$OPENGEM_STAGE_DIR"/*

stage_regular_files() {
	local src_dir="$1"
	if [[ ! -d "$src_dir" ]]; then
		return
	fi
	while IFS= read -r -d '' f; do
		local base
		base="$(basename "$f")"
		if [[ "$base" == "README.md" || "$base" == "_DS_STOR" ]]; then
			continue
		fi
		cp "$f" "$OPENGEM_STAGE_DIR/$base"
	done < <(find "$src_dir" -maxdepth 1 -type f -print0)
}

stage_regular_files "$OPENGEM_PAYLOAD_DIR"
stage_regular_files "$OPENGEM_UPSTREAM_DIR/GEMAPPS/GEMSYS"
stage_regular_files "$CIUKIOS_ROOT/OLD/archive-2026-04-22/third_party/freedos/runtime/OPENGEM/GEMAPPS/FONTS"
stage_regular_files "$CIUKIOS_ROOT/OLD/archive-2026-04-22/third_party/freedos/runtime/OPENGEM/GEMAPPS"
stage_regular_files "$OPENGEM_UPSTREAM_DIR"

if [[ -f "$OPENGEM_STAGE_DIR/VGAFSTR.INF" ]]; then
	cp "$OPENGEM_STAGE_DIR/VGAFSTR.INF" "$OPENGEM_STAGE_DIR/FSTR.INF"
	cp "$OPENGEM_STAGE_DIR/VGAFSTR.INF" "$OPENGEM_STAGE_DIR/FHDR.INF"
fi

if [[ -f "$OPENGEM_STAGE_DIR/STANDARD.PSF" ]]; then
	cp "$OPENGEM_STAGE_DIR/STANDARD.PSF" "$OPENGEM_STAGE_DIR/STANDARD.FNT"
fi

if [[ -f "$OPENGEM_STAGE_DIR/ROMAN.PSF" ]]; then
	cp "$OPENGEM_STAGE_DIR/ROMAN.PSF" "$OPENGEM_STAGE_DIR/ROMAN.FNT"
fi

if [[ -f "$OPENGEM_STAGE_DIR/SANSERIF.PSF" ]]; then
	cp "$OPENGEM_STAGE_DIR/SANSERIF.PSF" "$OPENGEM_STAGE_DIR/SANSERIF.FNT"
fi

if [[ "$INCLUDE_OPENGEM_PAYLOAD" != "1" ]]; then
	echo "[build-full] OpenGEM payload injection disabled (CIUKIOS_INCLUDE_OPENGEM=$INCLUDE_OPENGEM_PAYLOAD)"
elif command -v mcopy >/dev/null 2>&1; then
	if compgen -G "$OPENGEM_STAGE_DIR/*" >/dev/null; then
		echo "[build-full] injecting OpenGEM payload files (root + GEMAPPS/GEMSYS)"
		if command -v mmd >/dev/null 2>&1; then
			mmd -i "$IMG" ::GEMAPPS >/dev/null 2>&1 || true
			mmd -i "$IMG" ::GEMAPPS/GEMSYS >/dev/null 2>&1 || true
			mmd -i "$IMG" ::GEMAPPS/FONTS >/dev/null 2>&1 || true
			mmd -i "$IMG" ::GEMAPPS/GEMBOOT >/dev/null 2>&1 || true
			mmd -i "$IMG" ::GEMBOOT >/dev/null 2>&1 || true
		fi
		for f in "$OPENGEM_STAGE_DIR"/*; do
			base="$(basename "$f")"
			mcopy -o -i "$IMG" "$f" "::$base" >/dev/null
			mcopy -o -i "$IMG" "$f" "::GEMAPPS/GEMSYS/$base" >/dev/null || true
			case "$base" in
				*.PSF|*.AFF|*.B30|*.CGA|*.ELQ|*.EPS|*.HPH|*.VGA|*.X20|FSTR.INF|FHDR.INF|VGAFSTR.INF|*.FNT)
					mcopy -o -i "$IMG" "$f" "::GEMAPPS/FONTS/$base" >/dev/null || true
					;;
			esac
			if [[ "$base" == "GEM.EXE" ]]; then
				mcopy -o -i "$IMG" "$f" "::GEMBOOT/$base" >/dev/null || true
				mcopy -o -i "$IMG" "$f" "::GEMAPPS/GEMBOOT/$base" >/dev/null || true
			fi
		done
	else
		echo "[build-full] OpenGEM payload not found at $OPENGEM_PAYLOAD_DIR (skipping)"
	fi
else
	echo "[build-full] WARNING: mcopy not found; OpenGEM payload injection skipped"
fi

cat > build/full/README.txt << 'TXT'
CiukiOS Legacy v2 - Full profile (FAT16 baseline)

Image: ciukios-full.img (128MB)
State: BIOS stage0 -> stage1 with full DOS runtime and FAT16 file I/O
Filesystem: FAT16 (SPT=63 Heads=16 128MB) with COMDEMO/MZDEMO/FILEIO/DELTEST/STAGE2 payloads
Boot path: stage0 at LBA0, stage1 payload in sectors 2-15
Data: cluster 2=COMDEMO, 3=MZDEMO, 4-5=FILEIO, 6=DELTEST, 7=STAGE2
TXT

echo "[build-full] done: build/full/ciukios-full.img"
