#!/usr/bin/env bash
set -euo pipefail

: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$CIUKIOS_ROOT"

PARTITION_LBA="${CIUKIOS_FULL_CD_PARTITION_LBA:-63}"
PARTITION_SECTORS=262144
DISK_SECTORS=$((PARTITION_LBA + PARTITION_SECTORS))

PART_IMG="build/full/ciukios-full-cd-partition.img"
DISK_IMG="build/full/ciukios-full-cd-disk.img"
MBR_BIN="build/full/obj/full_cd_mbr.bin"
ISO_ROOT="build/full/cd-iso-root"
ISO_IMG="build/full/ciukios-full-cd.iso"
DIRECT_ISO_IMG="build/full/ciukios-full-cd-direct.iso"
ISOLINUX_FALLBACK_IMG="build/full/ciukios-full-cd-isolinux.iso"
ISOLINUX_BIN="${ISOLINUX_BIN:-/usr/lib/syslinux/bios/isolinux.bin}"
MEMDISK_BIN="${MEMDISK_BIN:-/usr/lib/syslinux/bios/memdisk}"
LDLINUX_C32="${LDLINUX_C32:-/usr/lib/syslinux/bios/ldlinux.c32}"

mkdir -p build/full/obj

echo "[build-full-cd] building CD partition image"
echo "[build-full-cd] hardware profile: forcing stage2 autorun (CIUKIOS_STAGE2_AUTORUN=1)"
export CIUKIOS_SETUP_RAW_HDD_INSTALL="${CIUKIOS_SETUP_RAW_HDD_INSTALL:-1}"
export CIUKIOS_SETUP_RAW_HDD_DESTRUCTIVE="${CIUKIOS_SETUP_RAW_HDD_DESTRUCTIVE:-1}"
export CIUKIOS_SETUP_LIVE_CD_MODE="${CIUKIOS_SETUP_LIVE_CD_MODE:-1}"
CIUKIOS_FULL_IMG="$PART_IMG" \
CIUKIOS_FULL_BOOT_LBA_OFFSET="$PARTITION_LBA" \
CIUKIOS_FULL_FAT_LBA_OFFSET="$PARTITION_LBA" \
CIUKIOS_STAGE2_AUTORUN=1 \
CIUKIOS_HARDWARE_VALIDATION_SCREEN="${CIUKIOS_HARDWARE_VALIDATION_SCREEN:-1}" \
CIUKIOS_DOS_DEFAULT_DRIVE_INDEX="${CIUKIOS_DOS_DEFAULT_DRIVE_INDEX:-3}" \
CIUKIOS_ENABLE_PS2_MOUSE_INIT="${CIUKIOS_ENABLE_PS2_MOUSE_INIT:-0}" \
MTOOLS_TIMEOUT_SEC="${MTOOLS_TIMEOUT_SEC:-5}" \
MTOOLS_KILL_AFTER_SEC="${MTOOLS_KILL_AFTER_SEC:-1}" \
bash scripts/build_full.sh

echo "[build-full-cd] assembling CD MBR"
nasm -f bin src/boot/full_cd_mbr.asm \
	-D PARTITION_LBA="$PARTITION_LBA" \
	-D PARTITION_SECTORS="$PARTITION_SECTORS" \
	-o "$MBR_BIN"

MBR_SIZE="$(stat -c%s "$MBR_BIN")"
if [[ "$MBR_SIZE" -ne 512 ]]; then
	echo "[build-full-cd] ERROR: MBR size is $MBR_SIZE bytes (expected 512)" >&2
	exit 1
fi

echo "[build-full-cd] creating El Torito hard-disk image"
dd if=/dev/zero of="$DISK_IMG" bs=512 count="$DISK_SECTORS" status=none
dd if="$MBR_BIN" of="$DISK_IMG" bs=512 count=1 conv=notrunc status=none
dd if="$PART_IMG" of="$DISK_IMG" bs=512 seek="$PARTITION_LBA" conv=notrunc status=none

if [[ ! -f "$ISOLINUX_BIN" ]]; then
	echo "[build-full-cd] ERROR: isolinux.bin not found (set ISOLINUX_BIN=...)" >&2
	exit 1
fi
if [[ ! -f "$MEMDISK_BIN" ]]; then
	echo "[build-full-cd] ERROR: memdisk not found (set MEMDISK_BIN=...)" >&2
	exit 1
fi
if [[ ! -f "$LDLINUX_C32" ]]; then
	echo "[build-full-cd] ERROR: ldlinux.c32 not found (set LDLINUX_C32=...)" >&2
	exit 1
fi

rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot/isolinux"
cp "$ISOLINUX_BIN" "$ISO_ROOT/boot/isolinux/isolinux.bin"
cp "$LDLINUX_C32" "$ISO_ROOT/boot/isolinux/ldlinux.c32"
cp "$MEMDISK_BIN" "$ISO_ROOT/boot/memdisk"
cp "$DISK_IMG" "$ISO_ROOT/ciukios-full-cd-disk.img"
cat > "$ISO_ROOT/boot/isolinux/isolinux.cfg" <<'TXT'
CONSOLE 1
PROMPT 1
TIMEOUT 50
DEFAULT ciukios

LABEL ciukios
  KERNEL /boot/memdisk
  INITRD /ciukios-full-cd-disk.img
  APPEND harddisk raw
TXT

echo "[build-full-cd] creating primary ISO (direct El Torito hard-disk emulation)"
# Real-hardware boot path: BIOS handles the El Torito hard-disk-boot record
# directly, so the kernel/MBR runs in seconds with no ISOLINUX/memdisk
# 128 MB load delay. This is the ISO end-users should burn to a CD/CD-RW.
xorriso -as mkisofs -quiet -V CIUKIOS_FULL \
	-o "$ISO_IMG" \
	-b ciukios-full-cd-disk.img \
	-c boot.cat \
	-hard-disk-boot \
	"$ISO_ROOT"

# Maintain the legacy file name "ciukios-full-cd-direct.iso" so existing
# QEMU test lanes (qemu_test_setup_runtime_hdd_install.sh,
# qemu_test_setup_cd_hdd_probe.sh, qemu_run_full_cd.sh) keep working.
cp -f "$ISO_IMG" "$DIRECT_ISO_IMG"

echo "[build-full-cd] creating ISOLINUX fallback ISO (slow on real HW; use only if direct does not boot)"
xorriso -as mkisofs -quiet -V CIUKIOS_FB \
	-o "$ISOLINUX_FALLBACK_IMG" \
	-b boot/isolinux/isolinux.bin \
	-c boot/isolinux/boot.cat \
	-no-emul-boot \
	-boot-load-size 4 \
	-boot-info-table \
	"$ISO_ROOT"

echo "[build-full-cd] done: $ISO_IMG"
echo "[build-full-cd] done: $DIRECT_ISO_IMG (alias of primary ISO)"
echo "[build-full-cd] done: $ISOLINUX_FALLBACK_IMG (fallback)"
