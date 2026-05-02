#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" == "Darwin" ]]; then
  # Allow direct invocation on macOS without going through the wrapper entrypoint.
  source "$(cd "$(dirname "${BASH_SOURCE[0]}")/macos" && pwd)/common.sh"
  ciuk_macos_prepare_tools
  ciuk_macos_check_required
  cd "$CIUKIOS_ROOT"
fi

mkdir -p build/floppy
mkdir -p build/floppy/obj

BOOT_SRC="src/boot/floppy_boot.asm"
BOOT_BIN="build/floppy/obj/floppy_boot.bin"
STAGE1_SRC="src/boot/floppy_stage1.asm"
STAGE1_BIN="build/floppy/obj/floppy_stage1.bin"
STAGE1_SLOT_BIN="build/floppy/obj/floppy_stage1_slot.bin"
STAGE1_SECTORS=50
STAGE1_SLOT_SIZE=$((STAGE1_SECTORS * 512))
STAGE2_SRC="src/boot/floppy_stage2.asm"
STAGE2_BIN="build/floppy/obj/floppy_stage2.bin"
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
CIUKEDIT_SRC="src/com/ciukedit.asm"
CIUKEDIT_BIN="build/floppy/obj/ciukedit.com"
CIUKEDIT_MAX_SIZE=1024
GFXRECT_SRC="src/com/gfxrect.asm"
GFXRECT_BIN="build/floppy/obj/gfxrect.com"
GFXRECT_MAX_SIZE=1024
GFXSTAR_SRC="src/com/gfxstar.asm"
GFXSTAR_BIN="build/floppy/obj/gfxstar.com"
GFXSTAR_MAX_SIZE=1024
STAGE1_SELFTEST_AUTORUN="${CIUKIOS_STAGE1_SELFTEST_AUTORUN:-0}"

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
if [[ ! -f "$STAGE2_SRC" ]]; then
  echo "[build-floppy] ERROR: stage2 source not found: $STAGE2_SRC" >&2
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
if [[ ! -f "$CIUKEDIT_SRC" ]]; then
  echo "[build-floppy] ERROR: ciukedit source not found: $CIUKEDIT_SRC" >&2
  exit 1
fi
if [[ ! -f "$GFXRECT_SRC" ]]; then
  echo "[build-floppy] ERROR: gfxrect source not found: $GFXRECT_SRC" >&2
  exit 1
fi
if [[ ! -f "$GFXSTAR_SRC" ]]; then
  echo "[build-floppy] ERROR: gfxstar source not found: $GFXSTAR_SRC" >&2
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
nasm -f bin "$STAGE1_SRC" \
  -D FAT_SPT=18 \
  -D FAT_HEADS=2 \
  -D FAT_RESERVED_SECTORS="$FAT_RESERVED_SECTORS" \
  -D FAT_SECTORS_PER_CLUSTER=1 \
  -D FAT_SECTORS_PER_FAT="$FAT_SECTORS_PER_FAT" \
  -D FAT_ROOT_DIR_SECTORS="$ROOT_DIR_SECTORS" \
  -D FAT_TYPE=12 \
  -D FAT_LBA_OFFSET=0 \
  -D STAGE1_SELFTEST_AUTORUN="$STAGE1_SELFTEST_AUTORUN" \
  -o "$STAGE1_BIN"
STAGE1_SIZE="$(stat -c%s "$STAGE1_BIN")"
if [[ "$STAGE1_SIZE" -gt "$STAGE1_SLOT_SIZE" ]]; then
  echo "[build-floppy] ERROR: stage1 payload is $STAGE1_SIZE bytes (max $STAGE1_SLOT_SIZE)" >&2
  exit 1
fi

echo "[build-floppy] preparing stage1 slot (${STAGE1_SECTORS} sectors)"
dd if=/dev/zero of="$STAGE1_SLOT_BIN" bs=512 count="$STAGE1_SECTORS" status=none
dd if="$STAGE1_BIN" of="$STAGE1_SLOT_BIN" conv=notrunc status=none

echo "[build-floppy] assembling stage2 payload"
nasm -f bin "$STAGE2_SRC" -o "$STAGE2_BIN"

STAGE2_SIZE="$(stat -c%s "$STAGE2_BIN")"
if [[ "$STAGE2_SIZE" -gt 4096 ]]; then
  echo "[build-floppy] WARNING: stage2 payload is $STAGE2_SIZE bytes (soft limit 4096)" >&2
fi

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

echo "[build-floppy] assembling CIUKEDIT editor payload"
nasm -f bin "$CIUKEDIT_SRC" -o "$CIUKEDIT_BIN"

CIUKEDIT_SIZE="$(stat -c%s "$CIUKEDIT_BIN")"
if [[ "$CIUKEDIT_SIZE" -gt "$CIUKEDIT_MAX_SIZE" ]]; then
  echo "[build-floppy] ERROR: CIUKEDIT payload is $CIUKEDIT_SIZE bytes (max $CIUKEDIT_MAX_SIZE)" >&2
  exit 1
fi

echo "[build-floppy] assembling GFX demo payloads"
nasm -f bin "$GFXRECT_SRC" -o "$GFXRECT_BIN"
nasm -f bin "$GFXSTAR_SRC" -o "$GFXSTAR_BIN"

GFXRECT_SIZE="$(stat -c%s "$GFXRECT_BIN")"
if [[ "$GFXRECT_SIZE" -gt "$GFXRECT_MAX_SIZE" ]]; then
  echo "[build-floppy] ERROR: GFXRECT payload is $GFXRECT_SIZE bytes (max $GFXRECT_MAX_SIZE)" >&2
  exit 1
fi

GFXSTAR_SIZE="$(stat -c%s "$GFXSTAR_BIN")"
if [[ "$GFXSTAR_SIZE" -gt "$GFXSTAR_MAX_SIZE" ]]; then
  echo "[build-floppy] ERROR: GFXSTAR payload is $GFXSTAR_SIZE bytes (max $GFXSTAR_MAX_SIZE)" >&2
  exit 1
fi

STAGE2_SECTORS=$(((STAGE2_SIZE + 511) / 512))
COMDEMO_SECTORS=$(((COMDEMO_SIZE + 511) / 512))
MZDEMO_SECTORS=$(((MZDEMO_SIZE + 511) / 512))
FILEIO_SECTORS=$(((FILEIO_SIZE + 511) / 512))
DELTEST_SECTORS=$(((DELTEST_SIZE + 511) / 512))
CIUKEDIT_SECTORS=$(((CIUKEDIT_SIZE + 511) / 512))
GFXRECT_SECTORS=$(((GFXRECT_SIZE + 511) / 512))
GFXSTAR_SECTORS=$(((GFXSTAR_SIZE + 511) / 512))

echo "[build-floppy] sector map: STAGE2=$STAGE2_SECTORS COMDEMO=$COMDEMO_SECTORS MZDEMO=$MZDEMO_SECTORS FILEIO=$FILEIO_SECTORS DELTEST=$DELTEST_SECTORS CIUKEDIT=$CIUKEDIT_SECTORS GFXRECT=$GFXRECT_SECTORS GFXSTAR=$GFXSTAR_SECTORS"

ROOT_SYSTEM_CLUSTER=2
ROOT_APPS_CLUSTER=3
NEXT_CLUSTER=4
SYSTEM_STAGE2_CLUSTER=$NEXT_CLUSTER
NEXT_CLUSTER=$((NEXT_CLUSTER + STAGE2_SECTORS))
APPS_COMDEMO_CLUSTER=$NEXT_CLUSTER
NEXT_CLUSTER=$((NEXT_CLUSTER + COMDEMO_SECTORS))
APPS_MZDEMO_CLUSTER=$NEXT_CLUSTER
NEXT_CLUSTER=$((NEXT_CLUSTER + MZDEMO_SECTORS))
APPS_FILEIO_CLUSTER=$NEXT_CLUSTER
NEXT_CLUSTER=$((NEXT_CLUSTER + FILEIO_SECTORS))
APPS_DELTEST_CLUSTER=$NEXT_CLUSTER
NEXT_CLUSTER=$((NEXT_CLUSTER + DELTEST_SECTORS))
APPS_CIUKEDIT_CLUSTER=$NEXT_CLUSTER
NEXT_CLUSTER=$((NEXT_CLUSTER + CIUKEDIT_SECTORS))
APPS_GFXRECT_CLUSTER=$NEXT_CLUSTER
NEXT_CLUSTER=$((NEXT_CLUSTER + GFXRECT_SECTORS))
APPS_GFXSTAR_CLUSTER=$NEXT_CLUSTER
NEXT_CLUSTER=$((NEXT_CLUSTER + GFXSTAR_SECTORS))

TOTAL_DATA_CLUSTERS=$((2880 - DATA_LBA))
if (( NEXT_CLUSTER - 2 > TOTAL_DATA_CLUSTERS )); then
  echo "[build-floppy] ERROR: payload layout exceeds FAT12 data area" >&2
  exit 1
fi

FAT_SECTOR_BIN="build/floppy/obj/fat_sector.bin"
ROOT_ENTRY_SYSTEM_BIN="build/floppy/obj/root_system_entry.bin"
ROOT_ENTRY_APPS_BIN="build/floppy/obj/root_apps_entry.bin"
DIR_ENTRY_DOT_SYSTEM="build/floppy/obj/dir_dot_system_entry.bin"
DIR_ENTRY_DOTDOT_ROOT="build/floppy/obj/dir_dotdot_root_entry.bin"
DIR_ENTRY_DOT_APPS="build/floppy/obj/dir_dot_apps_entry.bin"
DIR_ENTRY_STAGE2_BIN="build/floppy/obj/dir_stage2_entry.bin"
DIR_ENTRY_COMDEMO_BIN="build/floppy/obj/dir_comdemo_entry.bin"
DIR_ENTRY_MZ_BIN="build/floppy/obj/dir_mzdemo_entry.bin"
DIR_ENTRY_FILEIO_BIN="build/floppy/obj/dir_fileio_entry.bin"
DIR_ENTRY_DELTEST_BIN="build/floppy/obj/dir_deltest_entry.bin"
DIR_ENTRY_CIUKEDIT_BIN="build/floppy/obj/dir_ciukedit_entry.bin"
DIR_ENTRY_GFXRECT_BIN="build/floppy/obj/dir_gfxrect_entry.bin"
DIR_ENTRY_GFXSTAR_BIN="build/floppy/obj/dir_gfxstar_entry.bin"
SYSTEM_DIR_CLUSTER_BIN="build/floppy/obj/system_dir_cluster.bin"
APPS_DIR_CLUSTER_BIN="build/floppy/obj/apps_dir_cluster.bin"

dd if=/dev/zero of="$FAT_SECTOR_BIN" bs=1 count=512 status=none

fat12_get_byte() {
  local offset="$1"
  local hex
  hex=$(xxd -p -l 1 -s "$offset" "$FAT_SECTOR_BIN")
  if [[ -z "$hex" ]]; then
    echo 0
    return
  fi
  echo $((16#$hex))
}

fat12_set_byte() {
  local offset="$1" value="$2"
  printf "$(printf '\\x%02x' "$value")" | dd of="$FAT_SECTOR_BIN" bs=1 seek="$offset" conv=notrunc status=none
}

fat12_set_entry() {
  local index="$1" value="$2"
  local offset=$((index + index / 2))

  if (( index % 2 == 0 )); then
    local b1
    b1=$(fat12_get_byte $((offset + 1)))
    fat12_set_byte "$offset" $((value & 0xFF))
    fat12_set_byte $((offset + 1)) $(((b1 & 0xF0) | ((value >> 8) & 0x0F)))
  else
    local b0
    b0=$(fat12_get_byte "$offset")
    fat12_set_byte "$offset" $(((b0 & 0x0F) | ((value << 4) & 0xF0)))
    fat12_set_byte $((offset + 1)) $(((value >> 4) & 0xFF))
  fi
}

fat12_chain_span() {
  local start="$1" count="$2"
  local i cluster
  for ((i = 0; i < count; i++)); do
    cluster=$((start + i))
    if (( i + 1 < count )); then
      fat12_set_entry "$cluster" $((cluster + 1))
    else
      fat12_set_entry "$cluster" 0xFFF
    fi
  done
}

fat12_set_entry 0 0xFF0
fat12_set_entry 1 0xFFF
fat12_chain_span "$ROOT_SYSTEM_CLUSTER" 1
fat12_chain_span "$ROOT_APPS_CLUSTER" 1
fat12_chain_span "$SYSTEM_STAGE2_CLUSTER" "$STAGE2_SECTORS"
fat12_chain_span "$APPS_COMDEMO_CLUSTER" "$COMDEMO_SECTORS"
fat12_chain_span "$APPS_MZDEMO_CLUSTER" "$MZDEMO_SECTORS"
fat12_chain_span "$APPS_FILEIO_CLUSTER" "$FILEIO_SECTORS"
fat12_chain_span "$APPS_DELTEST_CLUSTER" "$DELTEST_SECTORS"
fat12_chain_span "$APPS_CIUKEDIT_CLUSTER" "$CIUKEDIT_SECTORS"
fat12_chain_span "$APPS_GFXRECT_CLUSTER" "$GFXRECT_SECTORS"
fat12_chain_span "$APPS_GFXSTAR_CLUSTER" "$GFXSTAR_SECTORS"

make_entry() {
  local out="$1" name="$2" attr="$3" cluster="$4" size="$5"
  dd if=/dev/zero of="$out" bs=1 count=32 status=none
  printf '%s' "$name" | dd of="$out" bs=1 seek=0 conv=notrunc status=none
  printf "$(printf '\\x%02x' "$attr")" | dd of="$out" bs=1 seek=11 conv=notrunc status=none
  printf "$(printf '\\x%02x\\x%02x' $((cluster & 0xFF)) $(((cluster >> 8) & 0xFF)))" \
    | dd of="$out" bs=1 seek=26 conv=notrunc status=none
  printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $((size & 0xFF)) $(((size >> 8) & 0xFF)) $(((size >> 16) & 0xFF)) $(((size >> 24) & 0xFF)))" \
    | dd of="$out" bs=1 seek=28 conv=notrunc status=none
}

make_entry "$ROOT_ENTRY_SYSTEM_BIN" 'SYSTEM     ' 0x10 "$ROOT_SYSTEM_CLUSTER" 0
make_entry "$ROOT_ENTRY_APPS_BIN"   'APPS       ' 0x10 "$ROOT_APPS_CLUSTER" 0

make_entry "$DIR_ENTRY_DOT_SYSTEM" '.          ' 0x10 "$ROOT_SYSTEM_CLUSTER" 0
make_entry "$DIR_ENTRY_DOTDOT_ROOT" '..         ' 0x10 0 0
make_entry "$DIR_ENTRY_DOT_APPS" '.          ' 0x10 "$ROOT_APPS_CLUSTER" 0

make_entry "$DIR_ENTRY_STAGE2_BIN" 'STAGE2  BIN' 0x20 "$SYSTEM_STAGE2_CLUSTER" "$STAGE2_SIZE"
make_entry "$DIR_ENTRY_COMDEMO_BIN" 'COMDEMO COM' 0x20 "$APPS_COMDEMO_CLUSTER" "$COMDEMO_SIZE"
make_entry "$DIR_ENTRY_MZ_BIN" 'MZDEMO  EXE' 0x20 "$APPS_MZDEMO_CLUSTER" "$MZDEMO_SIZE"
make_entry "$DIR_ENTRY_FILEIO_BIN" 'FILEIO  BIN' 0x20 "$APPS_FILEIO_CLUSTER" "$FILEIO_SIZE"
make_entry "$DIR_ENTRY_DELTEST_BIN" 'DELTEST BIN' 0x20 "$APPS_DELTEST_CLUSTER" "$DELTEST_SIZE"
make_entry "$DIR_ENTRY_CIUKEDIT_BIN" 'CIUKEDITCOM' 0x20 "$APPS_CIUKEDIT_CLUSTER" "$CIUKEDIT_SIZE"
make_entry "$DIR_ENTRY_GFXRECT_BIN" 'GFXRECT COM' 0x20 "$APPS_GFXRECT_CLUSTER" "$GFXRECT_SIZE"
make_entry "$DIR_ENTRY_GFXSTAR_BIN" 'GFXSTAR COM' 0x20 "$APPS_GFXSTAR_CLUSTER" "$GFXSTAR_SIZE"

dd if=/dev/zero of="$SYSTEM_DIR_CLUSTER_BIN" bs=512 count=1 status=none
dd if="$DIR_ENTRY_DOT_SYSTEM" of="$SYSTEM_DIR_CLUSTER_BIN" bs=1 seek=0 conv=notrunc status=none
dd if="$DIR_ENTRY_DOTDOT_ROOT" of="$SYSTEM_DIR_CLUSTER_BIN" bs=1 seek=32 conv=notrunc status=none
dd if="$DIR_ENTRY_STAGE2_BIN" of="$SYSTEM_DIR_CLUSTER_BIN" bs=1 seek=64 conv=notrunc status=none

dd if=/dev/zero of="$APPS_DIR_CLUSTER_BIN" bs=512 count=1 status=none
dd if="$DIR_ENTRY_DOT_APPS" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=0 conv=notrunc status=none
dd if="$DIR_ENTRY_DOTDOT_ROOT" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=32 conv=notrunc status=none
dd if="$DIR_ENTRY_COMDEMO_BIN" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=64 conv=notrunc status=none
dd if="$DIR_ENTRY_MZ_BIN" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=96 conv=notrunc status=none
dd if="$DIR_ENTRY_FILEIO_BIN" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=128 conv=notrunc status=none
dd if="$DIR_ENTRY_DELTEST_BIN" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=160 conv=notrunc status=none
dd if="$DIR_ENTRY_CIUKEDIT_BIN" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=192 conv=notrunc status=none
dd if="$DIR_ENTRY_GFXRECT_BIN" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=224 conv=notrunc status=none
dd if="$DIR_ENTRY_GFXSTAR_BIN" of="$APPS_DIR_CLUSTER_BIN" bs=1 seek=256 conv=notrunc status=none

echo "[build-floppy] creating 1.44MB floppy image"
dd if=/dev/zero of=build/floppy/ciukios-floppy.img bs=512 count=2880 status=none
dd if="$BOOT_BIN" of="$IMG" bs=512 count=1 conv=notrunc status=none
dd if="$STAGE1_SLOT_BIN" of="$IMG" bs=512 seek=1 count="$STAGE1_SECTORS" conv=notrunc status=none
dd if="$FAT_SECTOR_BIN" of="$IMG" bs=512 seek="$FAT1_LBA" count=1 conv=notrunc status=none
dd if="$FAT_SECTOR_BIN" of="$IMG" bs=512 seek="$FAT2_LBA" count=1 conv=notrunc status=none
dd if="$ROOT_ENTRY_SYSTEM_BIN" of="$IMG" bs=1 seek=$((ROOT_LBA * 512)) conv=notrunc status=none
dd if="$ROOT_ENTRY_APPS_BIN" of="$IMG" bs=1 seek=$((ROOT_LBA * 512 + 32)) conv=notrunc status=none

dd if="$SYSTEM_DIR_CLUSTER_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ROOT_SYSTEM_CLUSTER - 2)) count=1 conv=notrunc status=none
dd if="$APPS_DIR_CLUSTER_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + ROOT_APPS_CLUSTER - 2)) count=1 conv=notrunc status=none

dd if="$STAGE2_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + SYSTEM_STAGE2_CLUSTER - 2)) count="$STAGE2_SECTORS" conv=notrunc status=none
dd if="$COMDEMO_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + APPS_COMDEMO_CLUSTER - 2)) count="$COMDEMO_SECTORS" conv=notrunc status=none
dd if="$MZDEMO_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + APPS_MZDEMO_CLUSTER - 2)) count="$MZDEMO_SECTORS" conv=notrunc status=none
dd if="$FILEIO_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + APPS_FILEIO_CLUSTER - 2)) count="$FILEIO_SECTORS" conv=notrunc status=none
dd if="$DELTEST_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + APPS_DELTEST_CLUSTER - 2)) count="$DELTEST_SECTORS" conv=notrunc status=none
dd if="$CIUKEDIT_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + APPS_CIUKEDIT_CLUSTER - 2)) count="$CIUKEDIT_SECTORS" conv=notrunc status=none
dd if="$GFXRECT_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + APPS_GFXRECT_CLUSTER - 2)) count="$GFXRECT_SECTORS" conv=notrunc status=none
dd if="$GFXSTAR_BIN" of="$IMG" bs=512 seek=$((DATA_LBA + APPS_GFXSTAR_CLUSTER - 2)) count="$GFXSTAR_SECTORS" conv=notrunc status=none

cat > build/floppy/README.txt <<TXT
CiukiOS Legacy v2 - Floppy profile

Image: ciukios-floppy.img (1.44MB)
State: BIOS stage0 -> stage1 -> stage2 runtime
Boot path: stage0 at LBA0, stage1 payload in sectors 2-23, stage2 in FAT data area
FAT layout: reserved sectors include stage1, FAT/root/data follow BPB geometry
Root directories: SYSTEM cluster ${ROOT_SYSTEM_CLUSTER}, APPS cluster ${ROOT_APPS_CLUSTER}
SYSTEM: STAGE2.BIN clusters ${SYSTEM_STAGE2_CLUSTER}-$((SYSTEM_STAGE2_CLUSTER + STAGE2_SECTORS - 1))
APPS: COMDEMO.COM clusters ${APPS_COMDEMO_CLUSTER}-$((APPS_COMDEMO_CLUSTER + COMDEMO_SECTORS - 1))
APPS: MZDEMO.EXE clusters ${APPS_MZDEMO_CLUSTER}-$((APPS_MZDEMO_CLUSTER + MZDEMO_SECTORS - 1))
APPS: FILEIO.BIN clusters ${APPS_FILEIO_CLUSTER}-$((APPS_FILEIO_CLUSTER + FILEIO_SECTORS - 1))
APPS: DELTEST.BIN clusters ${APPS_DELTEST_CLUSTER}-$((APPS_DELTEST_CLUSTER + DELTEST_SECTORS - 1))
APPS: CIUKEDIT.COM clusters ${APPS_CIUKEDIT_CLUSTER}-$((APPS_CIUKEDIT_CLUSTER + CIUKEDIT_SECTORS - 1))
APPS: GFXRECT.COM clusters ${APPS_GFXRECT_CLUSTER}-$((APPS_GFXRECT_CLUSTER + GFXRECT_SECTORS - 1))
APPS: GFXSTAR.COM clusters ${APPS_GFXSTAR_CLUSTER}-$((APPS_GFXSTAR_CLUSTER + GFXSTAR_SECTORS - 1))
TXT

echo "[build-floppy] done: $IMG"
