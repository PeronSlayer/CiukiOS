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

IMG="${CIUKIOS_FULL_IMG:-build/full/ciukios-full.img}"
TOTAL_SECTORS=262144
STAGE1_SECTORS=61
STAGE1_SLOT_SIZE=$((STAGE1_SECTORS * 512))
BOOT_LBA_OFFSET="${CIUKIOS_FULL_BOOT_LBA_OFFSET:-0}"
FAT_LBA_OFFSET="${CIUKIOS_FULL_FAT_LBA_OFFSET:-0}"

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
CIUKEDIT_SRC="src/com/ciukedit.asm"
CIUKEDIT_BIN="build/full/obj/ciukedit.com"
GFXRECT_SRC="src/com/gfxrect.asm"
GFXRECT_BIN="build/full/obj/gfxrect.com"
GFXRECT_MAX_SIZE=1024
GFXSTAR_SRC="src/com/gfxstar.asm"
GFXSTAR_BIN="build/full/obj/gfxstar.com"
GFXSTAR_MAX_SIZE=1024
SETUP_SRC="src/com/setup.asm"
SETUP_BIN="build/full/obj/setup.com"
SETUP_MAX_CLUSTERS=4
SETUP_MAX_SIZE=$((FAT_SECTORS_PER_CLUSTER * 512 * SETUP_MAX_CLUSTERS))
SETUP_MANIFEST_BIN="build/full/obj/setup.mft"
COMMAND_STUB_SRC="src/com/command_stub.asm"
COMMAND_STUB_BIN="build/full/obj/command.com"
COMMAND_COMPAT_IMAGE_PATH="${CIUKIOS_COMMAND_COM_IMAGE_PATH:-::COMMAND.COM}"
DRVLOAD_SRC="src/com/drvload.asm"
DRVLOAD_BIN="build/full/obj/drvload.com"
SPLASH_SRC="misc/CiukiOS_SplashScreen.png"
SPLASH_TOOL="scripts/generate_splash_asset.py"
SPLASH_BIN="build/full/obj/SPLASH.BIN"
SPLASH_MAX_SIZE=$((FAT_SECTORS_PER_CLUSTER * 512 * 6))
SPLASH_EXPECTED_SIZE=16768
DOOM_SRC_DIR="${CIUKIOS_DOOM_SRC_DIR:-$CIUKIOS_ROOT/third_party/Doom}"
DOOM_IMAGE_DIR="${CIUKIOS_DOOM_IMAGE_DIR:-::APPS/DOOM}"
DRIVERS_SRC_DIR="${CIUKIOS_DRIVERS_SRC_DIR:-$CIUKIOS_ROOT/third_party/drivers}"
DRIVERS_IMAGE_DIR="${CIUKIOS_DRIVERS_IMAGE_DIR:-::SYSTEM/DRIVERS}"
DRIVERS_VERIFY_SCRIPT="$CIUKIOS_ROOT/scripts/verify_full_drivers_payload.sh"
CIUKIOS_ALLOW_MISSING_DRIVERS="${CIUKIOS_ALLOW_MISSING_DRIVERS:-0}"
STAGE1_SELFTEST_AUTORUN="${CIUKIOS_STAGE1_SELFTEST_AUTORUN:-0}"
# Default to shell-first UX on full profile; enable autorun explicitly for desktop tests.
STAGE2_AUTORUN="${CIUKIOS_STAGE2_AUTORUN:-0}"
HARDWARE_VALIDATION_SCREEN="${CIUKIOS_HARDWARE_VALIDATION_SCREEN:-0}"
MTOOLS_TIMEOUT_SEC="${MTOOLS_TIMEOUT_SEC:-20}"
MTOOLS_KILL_AFTER_SEC="${MTOOLS_KILL_AFTER_SEC:-2}"

mtools_ensure_dir() {
	local image="$1"
	local dir_path="$2"
	local out rc

	if mdir -i "$image" "$dir_path" >/dev/null 2>&1; then
		echo "[build-full] mtools mkdir: already exists (verified): $dir_path"
		return 0
	fi

	set +e
	out="$(timeout --kill-after="${MTOOLS_KILL_AFTER_SEC}s" "${MTOOLS_TIMEOUT_SEC}s" mmd -i "$image" "$dir_path" 2>&1)"
	rc=$?
	set -e

	if [[ $rc -eq 0 ]]; then
		echo "[build-full] mtools mkdir: created: $dir_path"
		return 0
	fi

	# Benign case: command returned non-zero but directory is confirmed present.
	if mdir -i "$image" "$dir_path" >/dev/null 2>&1; then
		echo "[build-full] mtools mkdir: non-zero rc=$rc but directory is present (verified): $dir_path"
		if [[ -n "$out" ]]; then
			echo "[build-full] mtools mkdir detail: $out"
		fi
		return 0
	fi

	if [[ $rc -eq 124 ]]; then
		echo "[build-full] ERROR: mtools mkdir timed out creating $dir_path (timeout=${MTOOLS_TIMEOUT_SEC}s kill-after=${MTOOLS_KILL_AFTER_SEC}s)" >&2
	else
		echo "[build-full] ERROR: mtools mkdir failed creating $dir_path (rc=$rc)" >&2
	fi
	if [[ -n "$out" ]]; then
		echo "[build-full] mtools mkdir output: $out" >&2
	fi
	exit 1
}



for f in "$BOOT_SRC" "$STAGE1_SRC" "$STAGE2_SRC" "$COMDEMO_SRC" "$MZDEMO_SRC" "$FILEIO_SRC" "$DELTEST_SRC" "$CIUKEDIT_SRC" "$GFXRECT_SRC" "$GFXSTAR_SRC" "$SETUP_SRC" "$COMMAND_STUB_SRC" "$DRVLOAD_SRC"; do
	if [[ ! -f "$f" ]]; then
		echo "[build-full] ERROR: source not found: $f" >&2
		exit 1
	fi
done

if [[ ! -f "$SPLASH_SRC" ]]; then
	echo "[build-full] ERROR: source not found: $SPLASH_SRC" >&2
	exit 1
fi

if [[ ! -f "$SPLASH_TOOL" ]]; then
	echo "[build-full] ERROR: splash generator not found: $SPLASH_TOOL" >&2
	exit 1
fi

echo "[build-full] assembling full stage0 boot sector"
nasm -f bin "$BOOT_SRC" \
	-D BOOT_LBA_OFFSET="$BOOT_LBA_OFFSET" \
	-o "$BOOT_BIN"

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
	-D FAT_LBA_OFFSET="$FAT_LBA_OFFSET" \
	-D STAGE1_SELFTEST_AUTORUN="$STAGE1_SELFTEST_AUTORUN" \
	-D STAGE2_AUTORUN="$STAGE2_AUTORUN" \
	-D HARDWARE_VALIDATION_SCREEN="$HARDWARE_VALIDATION_SCREEN" \
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
nasm -f bin "$STAGE2_SRC" -o "$STAGE2_BIN"
nasm -f bin "$COMDEMO_SRC" -o "$COMDEMO_BIN"
nasm -f bin "$MZDEMO_SRC"  -o "$MZDEMO_BIN"
nasm -f bin "$FILEIO_SRC"  -o "$FILEIO_BIN"
nasm -f bin "$DELTEST_SRC" -o "$DELTEST_BIN"
nasm -f bin "$CIUKEDIT_SRC" -o "$CIUKEDIT_BIN"
nasm -f bin "$GFXRECT_SRC" -o "$GFXRECT_BIN"
nasm -f bin "$GFXSTAR_SRC" -o "$GFXSTAR_BIN"
nasm -f bin "$SETUP_SRC" -o "$SETUP_BIN"
nasm -f bin "$COMMAND_STUB_SRC" -o "$COMMAND_STUB_BIN"
nasm -f bin "$DRVLOAD_SRC" -o "$DRVLOAD_BIN"

echo "[build-full] generating splash asset"
if ! command -v python3 >/dev/null 2>&1; then
	echo "[build-full] ERROR: python3 is required to generate SPLASH.BIN" >&2
	exit 1
fi

if ! python3 "$SPLASH_TOOL" "$SPLASH_SRC" "$SPLASH_BIN"; then
	echo "[build-full] ERROR: failed to generate SPLASH.BIN (requires python3 + Pillow)" >&2
	exit 1
fi

STAGE2_SIZE="$(stat -c%s "$STAGE2_BIN")"
if [[ "$STAGE2_SIZE" -gt "$STAGE2_MAX_SIZE" ]]; then
	echo "[build-full] ERROR: stage2 payload is $STAGE2_SIZE bytes (max $STAGE2_MAX_SIZE)" >&2
	exit 1
fi

COMDEMO_SIZE="$(stat -c%s "$COMDEMO_BIN")"
MZDEMO_SIZE="$(stat -c%s  "$MZDEMO_BIN")"
FILEIO_SIZE="$(stat -c%s  "$FILEIO_BIN")"
DELTEST_SIZE="$(stat -c%s "$DELTEST_BIN")"
CIUKEDIT_SIZE="$(stat -c%s "$CIUKEDIT_BIN")"
GFXRECT_SIZE="$(stat -c%s "$GFXRECT_BIN")"
GFXSTAR_SIZE="$(stat -c%s "$GFXSTAR_BIN")"
SETUP_SIZE="$(stat -c%s "$SETUP_BIN")"
SPLASH_SIZE="$(stat -c%s "$SPLASH_BIN")"
SPLASH_SECTORS=$(((SPLASH_SIZE + 511) / 512))
CLUSTER_SIZE_BYTES=$((FAT_SECTORS_PER_CLUSTER * 512))
SPLASH_CLUSTERS=$(((SPLASH_SIZE + CLUSTER_SIZE_BYTES - 1) / CLUSTER_SIZE_BYTES))
STAGE2_SECTORS=$(((STAGE2_SIZE + 511) / 512))
COMDEMO_SECTORS=$(((COMDEMO_SIZE + 511) / 512))
MZDEMO_SECTORS=$(((MZDEMO_SIZE + 511) / 512))
FILEIO_SECTORS=$(((FILEIO_SIZE + 511) / 512))
DELTEST_SECTORS=$(((DELTEST_SIZE + 511) / 512))
CIUKEDIT_SECTORS=$(((CIUKEDIT_SIZE + 511) / 512))
GFXRECT_SECTORS=$(((GFXRECT_SIZE + 511) / 512))
GFXSTAR_SECTORS=$(((GFXSTAR_SIZE + 511) / 512))
SETUP_SECTORS=$(((SETUP_SIZE + 511) / 512))
SETUP_CLUSTERS=$(((SETUP_SIZE + CLUSTER_SIZE_BYTES - 1) / CLUSTER_SIZE_BYTES))

if [[ "$GFXRECT_SIZE" -gt "$GFXRECT_MAX_SIZE" ]]; then
	echo "[build-full] ERROR: GFXRECT payload is $GFXRECT_SIZE bytes (max $GFXRECT_MAX_SIZE)" >&2
	exit 1
fi

if [[ "$GFXSTAR_SIZE" -gt "$GFXSTAR_MAX_SIZE" ]]; then
	echo "[build-full] ERROR: GFXSTAR payload is $GFXSTAR_SIZE bytes (max $GFXSTAR_MAX_SIZE)" >&2
	exit 1
fi

if [[ "$SETUP_SIZE" -gt "$SETUP_MAX_SIZE" ]]; then
	echo "[build-full] ERROR: SETUP payload is $SETUP_SIZE bytes (max $SETUP_MAX_SIZE)" >&2
	exit 1
fi

if (( SETUP_CLUSTERS < 1 || SETUP_CLUSTERS > SETUP_MAX_CLUSTERS )); then
	echo "[build-full] ERROR: SETUP cluster span is invalid: $SETUP_CLUSTERS (max $SETUP_MAX_CLUSTERS)" >&2
	exit 1
fi

echo "[build-full] generating setup payload manifest"
printf 'SMF1' > "$SETUP_MANIFEST_BIN"
printf '\x09' >> "$SETUP_MANIFEST_BIN"
printf '\x01\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x01\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x02\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x02\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x02\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x03\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x03\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x03\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"
printf '\x03\x01\x00\x00' >> "$SETUP_MANIFEST_BIN"

SETUP_MANIFEST_SIZE="$(stat -c%s "$SETUP_MANIFEST_BIN")"
SETUP_MANIFEST_SECTORS=$(((SETUP_MANIFEST_SIZE + 511) / 512))
SETUP_MANIFEST_CLUSTERS=$(((SETUP_MANIFEST_SIZE + CLUSTER_SIZE_BYTES - 1) / CLUSTER_SIZE_BYTES))

if (( SETUP_MANIFEST_CLUSTERS != 1 )); then
	echo "[build-full] ERROR: setup manifest must fit in one cluster ($SETUP_MANIFEST_SIZE bytes)" >&2
	exit 1
fi

echo "[build-full] sector map: STAGE2=$STAGE2_SECTORS COMDEMO=$COMDEMO_SECTORS MZDEMO=$MZDEMO_SECTORS FILEIO=$FILEIO_SECTORS DELTEST=$DELTEST_SECTORS CIUKEDIT=$CIUKEDIT_SECTORS GFXRECT=$GFXRECT_SECTORS GFXSTAR=$GFXSTAR_SECTORS SETUP=$SETUP_SECTORS SETUP_MFT=$SETUP_MANIFEST_SECTORS SPLASH=$SPLASH_SECTORS"

if [[ "$SPLASH_SIZE" -ne "$SPLASH_EXPECTED_SIZE" ]]; then
	echo "[build-full] ERROR: SPLASH.BIN size is $SPLASH_SIZE bytes (expected $SPLASH_EXPECTED_SIZE)" >&2
	exit 1
fi

if [[ "$SPLASH_SIZE" -gt "$SPLASH_MAX_SIZE" ]]; then
	echo "[build-full] ERROR: SPLASH.BIN is $SPLASH_SIZE bytes (max $SPLASH_MAX_SIZE)" >&2
	exit 1
fi

if [[ "$FILEIO_SIZE" -le 512 ]]; then
	echo "[build-full] ERROR: FILEIO payload must span >1 cluster ($FILEIO_SIZE bytes)" >&2
	exit 1
fi

# FAT16 directory/cluster map with contiguous SYSTEM/SPLASH chain.
# App payloads remain single-cluster except SETUP.COM, which may span up to SETUP_MAX_CLUSTERS.
ROOT_SYSTEM_CLUSTER=2
ROOT_APPS_CLUSTER=3
SYSTEM_STAGE2_CLUSTER=4
SYSTEM_SPLASH_CLUSTER=5
SYSTEM_SPLASH_LAST_CLUSTER=$((SYSTEM_SPLASH_CLUSTER + SPLASH_CLUSTERS - 1))
APPS_COMDEMO_CLUSTER=$((SYSTEM_SPLASH_LAST_CLUSTER + 1))
APPS_MZDEMO_CLUSTER=$((APPS_COMDEMO_CLUSTER + 1))
APPS_FILEIO_CLUSTER=$((APPS_MZDEMO_CLUSTER + 1))
APPS_DELTEST_CLUSTER=$((APPS_FILEIO_CLUSTER + 1))
APPS_CIUKEDIT_CLUSTER=$((APPS_DELTEST_CLUSTER + 1))
APPS_GFXRECT_CLUSTER=$((APPS_CIUKEDIT_CLUSTER + 1))
APPS_GFXSTAR_CLUSTER=$((APPS_GFXRECT_CLUSTER + 1))
APPS_SETUP_CLUSTER=$((APPS_GFXSTAR_CLUSTER + 1))
APPS_SETUP_LAST_CLUSTER=$((APPS_SETUP_CLUSTER + SETUP_CLUSTERS - 1))
APPS_SETUP_MANIFEST_CLUSTER=$((APPS_SETUP_LAST_CLUSTER + 1))

if (( SPLASH_CLUSTERS < 1 )); then
	echo "[build-full] ERROR: SPLASH.BIN requires an invalid cluster count ($SPLASH_CLUSTERS)" >&2
	exit 1
fi

if (( APPS_SETUP_MANIFEST_CLUSTER >= 256 )); then
	echo "[build-full] ERROR: FAT16 layout exceeds first FAT sector entry range" >&2
	exit 1
fi

for sectors in \
	"$STAGE2_SECTORS" \
	"$COMDEMO_SECTORS" \
	"$MZDEMO_SECTORS" \
	"$FILEIO_SECTORS" \
	"$DELTEST_SECTORS" \
	"$CIUKEDIT_SECTORS" \
	"$GFXRECT_SECTORS" \
	"$GFXSTAR_SECTORS" \
	"$SETUP_MANIFEST_SECTORS"; do
	if (( sectors > FAT_SECTORS_PER_CLUSTER )); then
		echo "[build-full] ERROR: payload exceeds single FAT16 cluster (${FAT_SECTORS_PER_CLUSTER} sectors)" >&2
		exit 1
	fi
done

FAT_SECTOR_BIN="build/full/obj/fat16_sector.bin"
dd if=/dev/zero of="$FAT_SECTOR_BIN" bs=1 count=512 status=none

fat16_set_entry() {
	local index="$1" value="$2"
	local offset=$((index * 2))
	printf "$(printf '\\x%02x\\x%02x' $((value & 0xFF)) $(((value >> 8) & 0xFF)))" \
		| dd of="$FAT_SECTOR_BIN" bs=1 seek="$offset" conv=notrunc status=none
}

fat16_set_contiguous_chain() {
	local start="$1" count="$2"
	local cluster="$start"
	local remaining="$count"

	if (( remaining <= 0 )); then
		return
	fi

	while (( remaining > 1 )); do
		fat16_set_entry "$cluster" "$((cluster + 1))"
		cluster=$((cluster + 1))
		remaining=$((remaining - 1))
	done

	fat16_set_entry "$cluster" 0xFFFF
}

fat16_set_entry 0 0xFFF8
fat16_set_entry 1 0xFFFF
fat16_set_entry "$ROOT_SYSTEM_CLUSTER" 0xFFFF
fat16_set_entry "$ROOT_APPS_CLUSTER" 0xFFFF
fat16_set_entry "$SYSTEM_STAGE2_CLUSTER" 0xFFFF
fat16_set_contiguous_chain "$SYSTEM_SPLASH_CLUSTER" "$SPLASH_CLUSTERS"
fat16_set_entry "$APPS_COMDEMO_CLUSTER" 0xFFFF
fat16_set_entry "$APPS_MZDEMO_CLUSTER" 0xFFFF
fat16_set_entry "$APPS_FILEIO_CLUSTER" 0xFFFF
fat16_set_entry "$APPS_DELTEST_CLUSTER" 0xFFFF
fat16_set_entry "$APPS_CIUKEDIT_CLUSTER" 0xFFFF
fat16_set_entry "$APPS_GFXRECT_CLUSTER" 0xFFFF
fat16_set_entry "$APPS_GFXSTAR_CLUSTER" 0xFFFF
fat16_set_contiguous_chain "$APPS_SETUP_CLUSTER" "$SETUP_CLUSTERS"
fat16_set_entry "$APPS_SETUP_MANIFEST_CLUSTER" 0xFFFF

make_entry() {
	local out="$1" name="$2" attr="$3" cluster="$4" size="$5"
	dd if=/dev/zero of="$out" bs=1 count=32 status=none
	printf '%s' "$name" | dd of="$out" bs=1 seek=0 conv=notrunc status=none
	printf "$(printf '\\x%02x' "$attr")" | dd of="$out" bs=1 seek=11 conv=notrunc status=none
	printf "$(printf '\\x%02x\\x%02x' $((cluster & 0xFF)) $(((cluster >> 8) & 0xFF)))" \
		| dd of="$out" bs=1 seek=26 conv=notrunc status=none
	printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' \
		$((size & 0xFF)) $(((size >> 8) & 0xFF)) \
		$(((size >> 16) & 0xFF)) $(((size >> 24) & 0xFF)))" \
		| dd of="$out" bs=1 seek=28 conv=notrunc status=none
}

ROOT_ENTRY_SYSTEM="build/full/obj/root_system.bin"
ROOT_ENTRY_APPS="build/full/obj/root_apps.bin"
DIR_ENTRY_DOT_SYSTEM="build/full/obj/dir_dot_system.bin"
DIR_ENTRY_DOTDOT_ROOT="build/full/obj/dir_dotdot_root.bin"
DIR_ENTRY_DOT_APPS="build/full/obj/dir_dot_apps.bin"
DIR_ENTRY_STAGE2="build/full/obj/dir_stage2.bin"
DIR_ENTRY_SPLASH="build/full/obj/dir_splash.bin"
DIR_ENTRY_COMDEMO="build/full/obj/dir_comdemo.bin"
DIR_ENTRY_MZDEMO="build/full/obj/dir_mzdemo.bin"
DIR_ENTRY_FILEIO="build/full/obj/dir_fileio.bin"
DIR_ENTRY_DELTEST="build/full/obj/dir_deltest.bin"
DIR_ENTRY_CIUKEDIT="build/full/obj/dir_ciukedit.bin"
DIR_ENTRY_GFXRECT="build/full/obj/dir_gfxrect.bin"
DIR_ENTRY_GFXSTAR="build/full/obj/dir_gfxstar.bin"
DIR_ENTRY_SETUP="build/full/obj/dir_setup.bin"
DIR_ENTRY_SETUP_MFT="build/full/obj/dir_setup_mft.bin"
DIR_ENTRY_SETUP_MFT_ALT="build/full/obj/dir_setup_mft_alt.bin"
SYSTEM_DIR_CLUSTER_BIN="build/full/obj/system_dir_cluster.bin"
APPS_DIR_CLUSTER_BIN="build/full/obj/apps_dir_cluster.bin"

make_entry "$ROOT_ENTRY_SYSTEM" 'SYSTEM     ' 0x10 "$ROOT_SYSTEM_CLUSTER" 0
make_entry "$ROOT_ENTRY_APPS"   'APPS       ' 0x10 "$ROOT_APPS_CLUSTER" 0

make_entry "$DIR_ENTRY_DOT_SYSTEM" '.          ' 0x10 "$ROOT_SYSTEM_CLUSTER" 0
make_entry "$DIR_ENTRY_DOTDOT_ROOT" '..         ' 0x10 0 0
make_entry "$DIR_ENTRY_DOT_APPS" '.          ' 0x10 "$ROOT_APPS_CLUSTER" 0

make_entry "$DIR_ENTRY_STAGE2"   'STAGE2  BIN' 0x20 "$SYSTEM_STAGE2_CLUSTER" "$STAGE2_SIZE"
make_entry "$DIR_ENTRY_SPLASH"   'SPLASH  BIN' 0x20 "$SYSTEM_SPLASH_CLUSTER" "$SPLASH_SIZE"
make_entry "$DIR_ENTRY_COMDEMO"  'COMDEMO COM' 0x20 "$APPS_COMDEMO_CLUSTER" "$COMDEMO_SIZE"
make_entry "$DIR_ENTRY_MZDEMO"   'MZDEMO  EXE' 0x20 "$APPS_MZDEMO_CLUSTER" "$MZDEMO_SIZE"
make_entry "$DIR_ENTRY_FILEIO"   'FILEIO  BIN' 0x20 "$APPS_FILEIO_CLUSTER" "$FILEIO_SIZE"
make_entry "$DIR_ENTRY_DELTEST"  'DELTEST BIN' 0x20 "$APPS_DELTEST_CLUSTER" "$DELTEST_SIZE"
make_entry "$DIR_ENTRY_CIUKEDIT" 'CIUKEDITCOM' 0x20 "$APPS_CIUKEDIT_CLUSTER" "$CIUKEDIT_SIZE"
make_entry "$DIR_ENTRY_GFXRECT"  'GFXRECT COM' 0x20 "$APPS_GFXRECT_CLUSTER" "$GFXRECT_SIZE"
make_entry "$DIR_ENTRY_GFXSTAR"  'GFXSTAR COM' 0x20 "$APPS_GFXSTAR_CLUSTER" "$GFXSTAR_SIZE"
make_entry "$DIR_ENTRY_SETUP"    'SETUP   COM' 0x20 "$APPS_SETUP_CLUSTER" "$SETUP_SIZE"
make_entry "$DIR_ENTRY_SETUP_MFT" 'SETUPMFTBIN' 0x20 "$APPS_SETUP_MANIFEST_CLUSTER" "$SETUP_MANIFEST_SIZE"
make_entry "$DIR_ENTRY_SETUP_MFT_ALT" 'MANIFST BIN' 0x20 "$APPS_SETUP_MANIFEST_CLUSTER" "$SETUP_MANIFEST_SIZE"

dd if=/dev/zero of="$SYSTEM_DIR_CLUSTER_BIN" bs=512 count="$FAT_SECTORS_PER_CLUSTER" status=none
dd if="$DIR_ENTRY_DOT_SYSTEM" of="$SYSTEM_DIR_CLUSTER_BIN" bs=1 seek=0 conv=notrunc status=none
dd if="$DIR_ENTRY_DOTDOT_ROOT" of="$SYSTEM_DIR_CLUSTER_BIN" bs=1 seek=32 conv=notrunc status=none
dd if="$DIR_ENTRY_STAGE2" of="$SYSTEM_DIR_CLUSTER_BIN" bs=1 seek=64 conv=notrunc status=none
dd if="$DIR_ENTRY_SPLASH" of="$SYSTEM_DIR_CLUSTER_BIN" bs=1 seek=96 conv=notrunc status=none

dd if=/dev/zero of="$APPS_DIR_CLUSTER_BIN" bs=512 count="$FAT_SECTORS_PER_CLUSTER" status=none
dd if="$DIR_ENTRY_DOT_APPS" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=0 conv=notrunc status=none
dd if="$DIR_ENTRY_DOTDOT_ROOT" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=32 conv=notrunc status=none
dd if="$DIR_ENTRY_COMDEMO" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=64 conv=notrunc status=none
dd if="$DIR_ENTRY_MZDEMO" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=96 conv=notrunc status=none
dd if="$DIR_ENTRY_FILEIO" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=128 conv=notrunc status=none
dd if="$DIR_ENTRY_DELTEST" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=160 conv=notrunc status=none
dd if="$DIR_ENTRY_CIUKEDIT" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=192 conv=notrunc status=none
dd if="$DIR_ENTRY_GFXRECT" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=224 conv=notrunc status=none
dd if="$DIR_ENTRY_GFXSTAR" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=256 conv=notrunc status=none
dd if="$DIR_ENTRY_SETUP" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=288 conv=notrunc status=none
dd if="$DIR_ENTRY_SETUP_MFT" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=320 conv=notrunc status=none
dd if="$DIR_ENTRY_SETUP_MFT_ALT" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=352 conv=notrunc status=none

echo "[build-full] creating 128MB FAT16 image"
dd if=/dev/zero of="$IMG" bs=512 count="$TOTAL_SECTORS" status=none
dd if="$BOOT_BIN"           of="$IMG" bs=512 count=1                seek=0           conv=notrunc status=none
dd if="$STAGE1_SLOT_BIN"    of="$IMG" bs=512 count="$STAGE1_SECTORS" seek=1          conv=notrunc status=none
dd if="$FAT_SECTOR_BIN"     of="$IMG" bs=512 count=1                seek="$FAT1_LBA" conv=notrunc status=none
dd if="$FAT_SECTOR_BIN"     of="$IMG" bs=512 count=1                seek="$FAT2_LBA" conv=notrunc status=none
dd if="$ROOT_ENTRY_SYSTEM" of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 0)) conv=notrunc status=none
dd if="$ROOT_ENTRY_APPS" of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 32)) conv=notrunc status=none

dd if="$SYSTEM_DIR_CLUSTER_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((ROOT_SYSTEM_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$FAT_SECTORS_PER_CLUSTER" conv=notrunc status=none
dd if="$APPS_DIR_CLUSTER_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((ROOT_APPS_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$FAT_SECTORS_PER_CLUSTER" conv=notrunc status=none

dd if="$STAGE2_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((SYSTEM_STAGE2_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$STAGE2_SECTORS" conv=notrunc status=none
dd if="$SPLASH_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((SYSTEM_SPLASH_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$SPLASH_SECTORS" conv=notrunc status=none
dd if="$COMDEMO_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_COMDEMO_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$COMDEMO_SECTORS" conv=notrunc status=none
dd if="$MZDEMO_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_MZDEMO_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$MZDEMO_SECTORS" conv=notrunc status=none
dd if="$FILEIO_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_FILEIO_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$FILEIO_SECTORS" conv=notrunc status=none
dd if="$DELTEST_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_DELTEST_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$DELTEST_SECTORS" conv=notrunc status=none
dd if="$CIUKEDIT_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_CIUKEDIT_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$CIUKEDIT_SECTORS" conv=notrunc status=none
dd if="$GFXRECT_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_GFXRECT_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$GFXRECT_SECTORS" conv=notrunc status=none
dd if="$GFXSTAR_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_GFXSTAR_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$GFXSTAR_SECTORS" conv=notrunc status=none
dd if="$SETUP_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_SETUP_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$SETUP_SECTORS" conv=notrunc status=none
dd if="$SETUP_MANIFEST_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ((APPS_SETUP_MANIFEST_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER))) count="$SETUP_MANIFEST_SECTORS" conv=notrunc status=none

if ! command -v mcopy >/dev/null 2>&1; then
	echo "[build-full] ERROR: mcopy is required to inject COMMAND.COM compatibility stub" >&2
	exit 1
fi

if ! command -v mmd >/dev/null 2>&1 || ! command -v mdir >/dev/null 2>&1; then
	echo "[build-full] ERROR: mtools mmd/mdir are required to inject runtime driver helper" >&2
	exit 1
fi

echo "[build-full] injecting COMMAND.COM compatibility stub to $COMMAND_COMPAT_IMAGE_PATH"
mcopy -o -i "$IMG" "$COMMAND_STUB_BIN" "$COMMAND_COMPAT_IMAGE_PATH"

echo "[build-full] injecting DRVLOAD.COM helper to ${DRIVERS_IMAGE_DIR%/}/DRVLOAD.COM"
mtools_ensure_dir "$IMG" "$DRIVERS_IMAGE_DIR"
mcopy -o -i "$IMG" "$DRVLOAD_BIN" "${DRIVERS_IMAGE_DIR%/}/DRVLOAD.COM"

echo "[build-full] shell-only profile: external desktop payload injection disabled"
if [[ -d "$DOOM_SRC_DIR" ]]; then
    if ! command -v mmd >/dev/null 2>&1 || ! command -v mdir >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
	    echo "[build-full] ERROR: DOOM source present but mtools (mmd/mdir/mcopy) is missing" >&2
        exit 1
    fi

    echo "[build-full] injecting local Doom payload from $DOOM_SRC_DIR to $DOOM_IMAGE_DIR"
	mtools_ensure_dir "$IMG" "$DOOM_IMAGE_DIR"

    shopt -s nullglob dotglob
    doom_items=("$DOOM_SRC_DIR"/*)
    shopt -u nullglob dotglob
    if (( ${#doom_items[@]} > 0 )); then
        mcopy -s -o -i "$IMG" "${doom_items[@]}" "$DOOM_IMAGE_DIR/"
    else
        echo "[build-full] WARN: Doom source directory is empty: $DOOM_SRC_DIR" >&2
    fi
else
    echo "[build-full] doom payload not found at $DOOM_SRC_DIR (skipped)"
fi

if [[ ! -d "$DRIVERS_SRC_DIR" ]]; then
	if [[ "$CIUKIOS_ALLOW_MISSING_DRIVERS" == "1" ]]; then
		echo "[build-full] WARN: drivers payload not found at $DRIVERS_SRC_DIR (skipped by CIUKIOS_ALLOW_MISSING_DRIVERS=1)" >&2
	else
		echo "[build-full] ERROR: drivers source directory not found: $DRIVERS_SRC_DIR (set CIUKIOS_ALLOW_MISSING_DRIVERS=1 to skip intentionally)" >&2
		exit 1
	fi
else
	shopt -s nullglob dotglob
	drivers_items=("$DRIVERS_SRC_DIR"/*)
	shopt -u nullglob dotglob

	if (( ${#drivers_items[@]} == 0 )); then
		if [[ "$CIUKIOS_ALLOW_MISSING_DRIVERS" == "1" ]]; then
			echo "[build-full] WARN: drivers source directory is empty: $DRIVERS_SRC_DIR (skipped by CIUKIOS_ALLOW_MISSING_DRIVERS=1)" >&2
		else
			echo "[build-full] ERROR: drivers source directory is empty: $DRIVERS_SRC_DIR (set CIUKIOS_ALLOW_MISSING_DRIVERS=1 to skip intentionally)" >&2
			exit 1
		fi
	else
		if ! command -v mmd >/dev/null 2>&1 || ! command -v mdir >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
			echo "[build-full] ERROR: drivers source present but mtools (mmd/mdir/mcopy) is missing" >&2
			exit 1
		fi

		if [[ ! -f "$DRIVERS_VERIFY_SCRIPT" ]]; then
			echo "[build-full] ERROR: drivers verifier not found: $DRIVERS_VERIFY_SCRIPT" >&2
			exit 1
		fi

		echo "[build-full] injecting local drivers payload from $DRIVERS_SRC_DIR to $DRIVERS_IMAGE_DIR"
		mtools_ensure_dir "$IMG" "$DRIVERS_IMAGE_DIR"
		mcopy -s -o -i "$IMG" "${drivers_items[@]}" "$DRIVERS_IMAGE_DIR/"

		if ! IMG="$IMG" DRIVERS_SRC_DIR="$DRIVERS_SRC_DIR" DRIVERS_IMAGE_DIR="$DRIVERS_IMAGE_DIR" \
			bash "$DRIVERS_VERIFY_SCRIPT"; then
			echo "[build-full] ERROR: drivers payload verification failed" >&2
			exit 1
		fi
	fi
fi


cat > build/full/README.txt << TXT
CiukiOS Legacy v2 - Full profile (FAT16 baseline)

Image: ciukios-full.img (128MB)
State: BIOS stage0 -> stage1 with full DOS runtime and FAT16 file I/O
Filesystem: FAT16 (SPT=63 Heads=16 128MB) with root directories SYSTEM/APPS
Boot path: stage0 at LBA0, stage1 payload in sectors 2-$((STAGE1_SECTORS + 1))
Data: cluster 2=SYSTEM dir, 3=APPS dir, 4=SYSTEM/STAGE2, ${SYSTEM_SPLASH_CLUSTER}-${SYSTEM_SPLASH_LAST_CLUSTER}=SYSTEM/SPLASH
Data: cluster ${APPS_COMDEMO_CLUSTER}=APPS/COMDEMO, ${APPS_MZDEMO_CLUSTER}=APPS/MZDEMO, ${APPS_FILEIO_CLUSTER}=APPS/FILEIO, ${APPS_DELTEST_CLUSTER}=APPS/DELTEST, ${APPS_CIUKEDIT_CLUSTER}=APPS/CIUKEDIT, ${APPS_GFXRECT_CLUSTER}=APPS/GFXRECT, ${APPS_GFXSTAR_CLUSTER}=APPS/GFXSTAR, ${APPS_SETUP_CLUSTER}-${APPS_SETUP_LAST_CLUSTER}=APPS/SETUP, ${APPS_SETUP_MANIFEST_CLUSTER}=APPS/SETUPMFT.BIN
Optional payloads: third_party/drivers is injected under SYSTEM/DRIVERS when available at build time
TXT

echo "[build-full] done: $IMG"
