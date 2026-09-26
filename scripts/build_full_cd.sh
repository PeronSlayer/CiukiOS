#!/usr/bin/env bash
set -euo pipefail

: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$CIUKIOS_ROOT"

PARTITION_LBA="${CIUKIOS_FULL_CD_PARTITION_LBA:-63}"
PARTITION_SECTORS="${CIUKIOS_FULL_CD_PARTITION_SECTORS:-196608}"
DISK_SECTORS=$((PARTITION_LBA + PARTITION_SECTORS))
MIRROR_PAD_SECTORS=$(((4 - (DISK_SECTORS % 4)) % 4))
MIRROR_START_SECTOR=$((DISK_SECTORS + MIRROR_PAD_SECTORS))
MIRROR_BLOCKS=$((MIRROR_START_SECTOR / 4))
REDUNDANT_SECTORS=$((MIRROR_START_SECTOR + DISK_SECTORS))

PART_IMG="build/full/ciukios-full-cd-partition.img"
DISK_IMG="build/full/ciukios-full-cd-disk.img"
DIRECT_PART_IMG="build/full/ciukios-full-cd-direct-partition.img"
DIRECT_DISK_IMG="build/full/ciukios-full-cd-direct-disk.img"
REDUNDANT_DISK_IMG="build/full/ciukios-full-cd-redundant.img"
MBR_BIN="build/full/obj/full_cd_mbr.bin"
ISO_ROOT="build/full/cd-iso-root"
CIUKIOS_VERSION="${CIUKIOS_VERSION:-0.7.1}"
if [[ ! "$CIUKIOS_VERSION" =~ ^[0-9]+([.][0-9]+)*$ ]]; then
	echo "[build-full-cd] ERROR: invalid CIUKIOS_VERSION=$CIUKIOS_VERSION" >&2
	exit 2
fi
ISO_VERSION_TAG="${CIUKIOS_VERSION//./-}"
ISO_IMG="build/full/ciukios-full-cd.iso"
VERSIONED_ISO_IMG="build/full/CiukiOS_full_cd_${ISO_VERSION_TAG}.iso"
DIRECT_ISO_IMG="build/full/ciukios-full-cd-direct.iso"
ISOLINUX_FALLBACK_IMG="build/full/ciukios-full-cd-isolinux.iso"
LOWMEM_ISO_IMG="build/full/ciukios-full-cd-lowmem.iso"
# GRUB4DOS uses a different decompressor and RAM mapping path. The original
# uncompressed ISOLINUX/MEMDISK ISO remains available, including on the menu.
BOOTLOADER="${CIUKIOS_FULL_CD_BOOTLOADER:-grub4dos}"
case "$BOOTLOADER" in
    grub4dos) default_compress=1 ;;
    isolinux) default_compress=0 ;;
    *) echo "[build-full-cd] ERROR: bootloader must be grub4dos or isolinux" >&2; exit 2 ;;
esac
COMPRESS_RAM_IMAGE="${CIUKIOS_FULL_CD_COMPRESS:-$default_compress}"
if [[ "$COMPRESS_RAM_IMAGE" != 0 && "$COMPRESS_RAM_IMAGE" != 1 ]]; then
	echo "[build-full-cd] ERROR: CIUKIOS_FULL_CD_COMPRESS must be 0 or 1" >&2
	exit 2
fi
OMIT_SYSTEM_SHELL="${CIUKIOS_FULL_CD_OMIT_SYSTEM_SHELL:-0}"
ISOLINUX_BIN="${ISOLINUX_BIN:-/usr/lib/syslinux/bios/isolinux.bin}"
MEMDISK_BIN="${MEMDISK_BIN:-/usr/lib/syslinux/bios/memdisk}"
LDLINUX_C32="${LDLINUX_C32:-/usr/lib/syslinux/bios/ldlinux.c32}"

mkdir -p build/full/obj

echo "[build-full-cd] hardware profile: stage2 autorun is opt-in (set CIUKIOS_STAGE2_AUTORUN=1 to enable)"
echo "[build-full-cd] shell profile: native desktop, F4 for DOS (external=0 exercises loader-fatal test path)"
export CIUKIOS_SETUP_RAW_HDD_INSTALL="${CIUKIOS_SETUP_RAW_HDD_INSTALL:-1}"
export CIUKIOS_SETUP_RAW_HDD_DESTRUCTIVE="${CIUKIOS_SETUP_RAW_HDD_DESTRUCTIVE:-1}"
export CIUKIOS_SETUP_LIVE_CD_MODE="${CIUKIOS_SETUP_LIVE_CD_MODE:-1}"

build_cd_partition() {
	local output_img="$1"
	local force_ram_source="$2"
	CIUKIOS_FULL_IMG="$output_img" \
	CIUKIOS_FULL_TOTAL_SECTORS="$PARTITION_SECTORS" \
	CIUKIOS_FULL_BOOT_LBA_OFFSET="$PARTITION_LBA" \
	CIUKIOS_FULL_FAT_LBA_OFFSET="$PARTITION_LBA" \
	CIUKIOS_STAGE1_BOOT_EXTERNAL_SHELL="${CIUKIOS_STAGE1_BOOT_EXTERNAL_SHELL:-1}" \
	CIUKIOS_STAGE2_AUTORUN="${CIUKIOS_STAGE2_AUTORUN:-0}" \
	CIUKIOS_HARDWARE_VALIDATION_SCREEN="${CIUKIOS_HARDWARE_VALIDATION_SCREEN:-0}" \
	CIUKIOS_DOS_DEFAULT_DRIVE_INDEX="${CIUKIOS_DOS_DEFAULT_DRIVE_INDEX:-3}" \
	CIUKIOS_ENABLE_PS2_MOUSE_INIT="${CIUKIOS_ENABLE_PS2_MOUSE_INIT:-1}" \
	CIUKIOS_SETUP_FORCE_MEMDISK_SOURCE="$force_ram_source" \
	CIUKIOS_SETUP_ATAPI_MIRROR_BLOCKS="$MIRROR_BLOCKS" \
	MTOOLS_TIMEOUT_SEC="${MTOOLS_TIMEOUT_SEC:-5}" \
	MTOOLS_KILL_AFTER_SEC="${MTOOLS_KILL_AFTER_SEC:-1}" \
	bash scripts/build_full.sh

	if (( OMIT_SYSTEM_SHELL )); then
		echo "[build-full-cd] test-only: removing ::SYSTEM/SHELL.COM from $output_img"
		mdel -i "$output_img" ::SYSTEM/SHELL.COM >/dev/null 2>&1 || {
			echo "[build-full-cd] ERROR: could not remove ::SYSTEM/SHELL.COM from $output_img" >&2
			exit 1
		}
		if mdir -i "$output_img" ::SYSTEM 2>/dev/null | grep -Eq '^SHELL[[:space:]]+COM[[:space:]]'; then
			echo "[build-full-cd] ERROR: ::SYSTEM/SHELL.COM still present in $output_img" >&2
			exit 1
		fi
	fi
}

# Build the physical-ATAPI fallback first.  Build the RAM release last so the
# shared object/listing artifacts describe the image that users actually burn.
echo "[build-full-cd] building direct-ATAPI fallback partition"
build_cd_partition "$DIRECT_PART_IMG" 0
echo "[build-full-cd] building RAM-resident release partition"
build_cd_partition "$PART_IMG" 1

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

echo "[build-full-cd] creating direct-ATAPI hard-disk image"
dd if=/dev/zero of="$DIRECT_DISK_IMG" bs=512 count="$DISK_SECTORS" status=none
dd if="$MBR_BIN" of="$DIRECT_DISK_IMG" bs=512 count=1 conv=notrunc status=none
dd if="$DIRECT_PART_IMG" of="$DIRECT_DISK_IMG" bs=512 seek="$PARTITION_LBA" conv=notrunc status=none

echo "[build-full-cd] creating redundant optical image (mirror starts at 2048-byte block $MIRROR_BLOCKS)"
dd if=/dev/zero of="$REDUNDANT_DISK_IMG" bs=512 count="$REDUNDANT_SECTORS" status=none
dd if="$DIRECT_DISK_IMG" of="$REDUNDANT_DISK_IMG" bs=512 conv=notrunc status=none
dd if="$DIRECT_DISK_IMG" of="$REDUNDANT_DISK_IMG" bs=512 seek="$MIRROR_START_SECTOR" conv=notrunc status=none
primary_bytes="$(stat -c%s "$DIRECT_DISK_IMG")"
mirror_offset_bytes=$((MIRROR_START_SECTOR * 512))
cmp -n "$primary_bytes" "$DIRECT_DISK_IMG" "$REDUNDANT_DISK_IMG"
cmp -i "0:$mirror_offset_bytes" -n "$primary_bytes" "$DIRECT_DISK_IMG" "$REDUNDANT_DISK_IMG"

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
cp "$REDUNDANT_DISK_IMG" "$ISO_ROOT/ciukios-full-cd-disk.img"

echo "[build-full-cd] creating direct El Torito fallback ISO"
# This fast-boot fallback keeps direct ATAPI recovery and a second copy of the
# system image.  It is not the release default because some real notebook
# mechanisms can remain BSY indefinitely during many small packet reads.
xorriso -as mkisofs -quiet -V CIUKIOS_DIR \
	-o "$DIRECT_ISO_IMG" \
	-b ciukios-full-cd-disk.img \
	-c boot.cat \
	-hard-disk-boot \
	"$ISO_ROOT"

echo "[build-full-cd] creating uncompressed low-memory ISO"
# Load the single 96 MiB system disk once, before CiukiOS starts.  SETUP then
# clones through MEMDISK's RAM-backed INT 13h service and never performs
# packet reads from the optical mechanism at 4/5 percent or later. This
# uncompressed recovery also works at 128 MiB. The release below uses the
# independent GRUB4DOS decompressor; MEMDISK gzip failed on the physical T23.
rm -f "$ISO_ROOT/ciukios-full-cd-disk.img"
cp "$DISK_IMG" "$ISO_ROOT/ciukios-full-cd-disk.img"
cat > "$ISO_ROOT/boot/isolinux/isolinux.cfg" <<'TXT'
CONSOLE 1
PROMPT 0
TIMEOUT 10
DEFAULT ciukios

LABEL ciukios
  KERNEL /boot/memdisk
  INITRD /ciukios-full-cd-disk.img
  APPEND harddisk raw
TXT
xorriso -as mkisofs -quiet -V CIUKIOS_FULL \
	-o "$LOWMEM_ISO_IMG" \
	-b boot/isolinux/isolinux.bin \
	-c boot/isolinux/boot.cat \
	-no-emul-boot \
	-boot-load-size 4 \
	-boot-info-table \
	"$ISO_ROOT"

# Keep the original independent recovery ISO, before adding GRUB4DOS assets.
cp -f "$LOWMEM_ISO_IMG" "$ISOLINUX_FALLBACK_IMG"
if [[ "$BOOTLOADER" == grub4dos ]]; then
    bash scripts/fetch_grub4dos.sh
    cp build/tools/grub4dos/grldr-ciukios "$ISO_ROOT/grldr"
    mkdir -p "$ISO_ROOT/boot/grub4dos"
    cp build/tools/grub4dos/release/grub4dos-0.4.6a/COPYING "$ISO_ROOT/boot/grub4dos/COPYING"
    cp build/tools/grub4dos/source-2020-08-09.tar.gz "$ISO_ROOT/boot/grub4dos/source.tar.gz"
    cp patches/grub4dos-int13-irq-gdt.patch "$ISO_ROOT/boot/grub4dos/"
    cp scripts/patch_grub4dos_irq.py "$ISO_ROOT/boot/grub4dos/"
    ram_image=/ciukios-full-cd-disk.img
    if [[ "$COMPRESS_RAM_IMAGE" == 1 ]]; then
        gzip -n -6 -c "$DISK_IMG" > "$ISO_ROOT/ciukios-full-cd-disk.img.gz"
        ram_image=/ciukios-full-cd-disk.img.gz
    fi
    # Reserve hd0 for the RAM image, shifting actual BIOS disks up by one.
    # This is essential for SETUP: hd1 must still identify the physical disk.
    # Keep compressed optical loading, but let BIOS INT 15h/87h perform
    # runtime memory transfers. The raw CR0/GDT/A20 path is an explicit
    # alternate for firmware with a broken BIOS mover, not the default.
    cat > "$ISO_ROOT/menu.lst" <<'TXT'
timeout 6
default 0
fallback 5
color light-gray/blue white/blue
set /a ORIGHD=*0x475 & 0xff
TXT

    # Append diagnostics so existing entry indices and fallback=5 stay stable.
    for session in live setup safe dos alternate recovery muted; do
        session_image="$ram_image"
        memory_access=0
        case "$session" in
            live) printf '\ntitle CiukiOS - Live CD\\nOpen the desktop. Automatic display with a safe VGA fallback.\n' >> "$ISO_ROOT/menu.lst" ;;
            setup) printf '\ntitle CiukiOS - Setup\\nStart the graphical installer directly. No desktop or startup sound.\n' >> "$ISO_ROOT/menu.lst" ;;
            safe) printf '\ntitle CiukiOS - Live CD (safe graphics)\\nOpen the desktop in VGA 640x480 with startup sound disabled.\n' >> "$ISO_ROOT/menu.lst" ;;
            dos) printf '\ntitle CiukiOS - DOS console\\nStart in text mode. Type EXIT to open the desktop.\n' >> "$ISO_ROOT/menu.lst" ;;
            alternate)
                printf '\ntitle CiukiOS - alternate disk access\\nUse direct memory access if the normal BIOS disk access fails.\n' >> "$ISO_ROOT/menu.lst"
                memory_access=1
                ;;
            recovery)
                printf '\ntitle CiukiOS - recovery\\nLoad the uncompressed image if compressed loading fails.\n' >> "$ISO_ROOT/menu.lst"
                session_image=/ciukios-full-cd-disk.img
                ;;
            muted)
                printf '\ntitle CiukiOS - Live CD (no startup sound)\\nOpen the normal desktop without running the startup audio player.\n' >> "$ISO_ROOT/menu.lst"
                ;;
        esac
    cat >> "$ISO_ROOT/menu.lst" <<'TXT'
map --unhook
map --unmap=0:0xff
map --harddrives=%ORIGHD%
TXT
    printf 'find --set-root %s\n' "$session_image" >> "$ISO_ROOT/menu.lst"
    # Missing drives are normal. The geometry guard also supports a CD-only VM.
    for ((drive=6; drive>=0; drive--)); do
        printf 'geometry (hd%d) > nul && map (hd%d) (hd%d) || echo
' \
            "$drive" "$drive" "$((drive + 1))" >> "$ISO_ROOT/menu.lst"
    done
    printf 'map --memdisk-raw=%s\nmap --mem %s (hd0)\n' \
        "$memory_access" "$session_image" >> "$ISO_ROOT/menu.lst"
    cat >> "$ISO_ROOT/menu.lst" <<'TXT'
set /a RAMHD=%ORIGHD% + 1
map --harddrives=%RAMHD%
map --hook
TXT
    # hd0 is now exclusively the new RAM image. Never write a physical disk.
    case "$session" in
        setup) echo 'write (hd0,0)/SYSTEM/STARTUP.CFG SETU' >> "$ISO_ROOT/menu.lst" ;;
        safe) echo 'write (hd0,0)/SYSTEM/STARTUP.CFG SAFE' >> "$ISO_ROOT/menu.lst" ;;
        dos) echo 'write (hd0,0)/SYSTEM/STARTUP.CFG DOS!' >> "$ISO_ROOT/menu.lst" ;;
        muted) echo 'write (hd0,0)/SYSTEM/STARTUP.CFG MUTE' >> "$ISO_ROOT/menu.lst" ;;
    esac
    cat >> "$ISO_ROOT/menu.lst" <<'TXT'
rootnoverify (hd0)
chainloader (hd0)+1
TXT
    done
    raw_bytes="$(stat -c%s "$DISK_IMG")"
    packed_bytes="$(stat -c%s "$ISO_ROOT$ram_image")"
    echo "[build-full-cd] GRUB4DOS optical read: $raw_bytes -> $packed_bytes bytes"
    xorriso -as mkisofs -quiet -V CIUKIOS_FULL -o "$ISO_IMG" \
        -b grldr -c boot.cat -no-emul-boot -boot-load-size 4 "$ISO_ROOT"
elif [[ "$COMPRESS_RAM_IMAGE" == 1 ]]; then
    # Retained for explicit MEMDISK decompression diagnostics only.
    gzip -n -6 -c "$DISK_IMG" > "$ISO_ROOT/ciukios-full-cd-disk.img.gz"
    rm -f "$ISO_ROOT/ciukios-full-cd-disk.img"
    sed -i 's@INITRD /ciukios-full-cd-disk.img$@INITRD /ciukios-full-cd-disk.img.gz@' \
        "$ISO_ROOT/boot/isolinux/isolinux.cfg"
    xorriso -as mkisofs -quiet -V CIUKIOS_FULL -o "$ISO_IMG" \
        -b boot/isolinux/isolinux.bin -c boot/isolinux/boot.cat \
        -no-emul-boot -boot-load-size 4 -boot-info-table "$ISO_ROOT"
else
    cp -f "$LOWMEM_ISO_IMG" "$ISO_IMG"
fi
cp -f "$ISO_IMG" "$VERSIONED_ISO_IMG"
echo "[build-full-cd] done: $VERSIONED_ISO_IMG ($BOOTLOADER RAM boot)"
echo "[build-full-cd] done: $ISOLINUX_FALLBACK_IMG (original uncompressed recovery)"
echo "[build-full-cd] done: $DIRECT_ISO_IMG (direct ATAPI diagnostic)"
