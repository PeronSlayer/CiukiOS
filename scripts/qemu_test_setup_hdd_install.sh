#!/usr/bin/env bash
set -euo pipefail

: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$CIUKIOS_ROOT"

OUT_DIR="build/full/setup-hdd"
TARGET_IMG="$OUT_DIR/target-hdd.img"
SERIAL_LOG="$OUT_DIR/target_boot.serial.log"
STDERR_LOG="$OUT_DIR/target_boot.stderr.log"
MBR_SIG_LOG="$OUT_DIR/mbr_sig.txt"
PARTITION_LOG="$OUT_DIR/partition_entry.hex"
MDIR_ROOT_LOG="$OUT_DIR/mdir_root.txt"
MDIR_SYSTEM_LOG="$OUT_DIR/mdir_system.txt"
MDIR_APPS_LOG="$OUT_DIR/mdir_apps.txt"
RC_LOG="$OUT_DIR/qemu_test_setup_hdd_install.rc"

PARTITION_LBA="${CIUKIOS_HDD_INSTALL_PARTITION_LBA:-63}"
PARTITION_SECTORS="${CIUKIOS_HDD_INSTALL_PARTITION_SECTORS:-262144}"
TARGET_SECTORS="${CIUKIOS_HDD_INSTALL_TARGET_SECTORS:-524288}"
PARTITION_OFFSET_BYTES=$((PARTITION_LBA * 512))
REQUIRED_TARGET_SECTORS=$((PARTITION_LBA + PARTITION_SECTORS))
if (( TARGET_SECTORS < REQUIRED_TARGET_SECTORS )); then
  echo "[setup-hdd] ERROR: target sectors $TARGET_SECTORS smaller than required $REQUIRED_TARGET_SECTORS" >&2
  exit 1
fi

case "$TARGET_IMG" in
  build/full/setup-hdd/*.img) ;;
  *)
    echo "[setup-hdd] ERROR: refusing unsafe target path: $TARGET_IMG" >&2
    exit 1
    ;;
esac

for tool in dd od mdir qemu-system-i386 timeout; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "[setup-hdd] ERROR: required tool not found: $tool" >&2
    exit 1
  fi
done

echo "[setup-hdd] building full-CD source image"
bash scripts/build_full_cd.sh

MBR_BIN="build/full/obj/full_cd_mbr.bin"
PART_IMG="build/full/ciukios-full-cd-partition.img"
if [[ ! -f "$MBR_BIN" || ! -f "$PART_IMG" ]]; then
  echo "[setup-hdd] ERROR: missing source artifacts: $MBR_BIN or $PART_IMG" >&2
  exit 1
fi

part_size=$(stat -c%s "$PART_IMG")
expected_part_size=$((PARTITION_SECTORS * 512))
if [[ "$part_size" -ne "$expected_part_size" ]]; then
  echo "[setup-hdd] ERROR: partition image size $part_size != expected $expected_part_size" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
rm -f "$TARGET_IMG" "$SERIAL_LOG" "$STDERR_LOG" "$MBR_SIG_LOG" "$PARTITION_LOG" "$MDIR_ROOT_LOG" "$MDIR_SYSTEM_LOG" "$MDIR_APPS_LOG" "$RC_LOG"

echo "[setup-hdd] creating disposable HDD image: $TARGET_IMG"
dd if=/dev/zero of="$TARGET_IMG" bs=512 count="$TARGET_SECTORS" status=none

echo "[setup-hdd] writing MBR and FAT16 full partition"
dd if="$MBR_BIN" of="$TARGET_IMG" bs=512 count=1 conv=notrunc status=none
dd if="$PART_IMG" of="$TARGET_IMG" bs=512 seek="$PARTITION_LBA" conv=notrunc status=none

dd if="$TARGET_IMG" bs=1 skip=510 count=2 status=none | od -An -tx1 > "$MBR_SIG_LOG"
if ! grep -qi "55 aa" "$MBR_SIG_LOG"; then
  echo "[setup-hdd] ERROR: invalid MBR signature" >&2
  cat "$MBR_SIG_LOG" >&2
  exit 1
fi

dd if="$TARGET_IMG" bs=1 skip=446 count=16 status=none | od -An -tx1 > "$PARTITION_LOG"
if ! od -An -tx1 -j 446 -N 1 "$TARGET_IMG" | grep -qi "80"; then
  echo "[setup-hdd] ERROR: partition is not active" >&2
  cat "$PARTITION_LOG" >&2
  exit 1
fi
if ! od -An -tx1 -j 450 -N 1 "$TARGET_IMG" | grep -qi "06"; then
  echo "[setup-hdd] ERROR: partition type is not FAT16 0x06" >&2
  cat "$PARTITION_LOG" >&2
  exit 1
fi
part_lba_hex=$(od -An -tx1 -j 454 -N 4 "$TARGET_IMG" | tr -d " \n")
if [[ "$part_lba_hex" != "3f000000" ]]; then
  echo "[setup-hdd] ERROR: partition start LBA mismatch: $part_lba_hex" >&2
  cat "$PARTITION_LOG" >&2
  exit 1
fi
part_count_hex=$(od -An -tx1 -j 458 -N 4 "$TARGET_IMG" | tr -d " \n")
if [[ "$part_count_hex" != "00000400" ]]; then
  echo "[setup-hdd] ERROR: partition sector count mismatch: $part_count_hex" >&2
  cat "$PARTITION_LOG" >&2
  exit 1
fi

mdir -i "$TARGET_IMG@@$PARTITION_OFFSET_BYTES" :: > "$MDIR_ROOT_LOG"
mdir -i "$TARGET_IMG@@$PARTITION_OFFSET_BYTES" ::SYSTEM > "$MDIR_SYSTEM_LOG"
mdir -i "$TARGET_IMG@@$PARTITION_OFFSET_BYTES" ::APPS > "$MDIR_APPS_LOG"

echo "[setup-hdd] booting disposable HDD image in QEMU"
set +e
timeout 45 qemu-system-i386 -machine pc,vmport=off -cpu pentium3 -m 128 -drive file="$TARGET_IMG",format=raw,if=ide,index=0,media=disk -boot c -nographic -chardev file,id=ser0,path="$SERIAL_LOG" -serial chardev:ser0 -monitor none -no-reboot -no-shutdown >/dev/null 2>"$STDERR_LOG"
qemu_rc=$?
set -e
printf "%s\n" "$qemu_rc" > "$RC_LOG"

if ! grep -aF "[BOOT0-FULL] CiukiOS full stage0 ready" "$SERIAL_LOG" >/dev/null; then
  echo "[setup-hdd] ERROR: missing stage0 marker" >&2
  exit 1
fi
if ! grep -aF "[STAGE1-SERIAL] READY" "$SERIAL_LOG" >/dev/null; then
  echo "[setup-hdd] ERROR: missing stage1 marker" >&2
  exit 1
fi
if ! grep -aF "[STAGE2] return to shell" "$SERIAL_LOG" >/dev/null; then
  echo "[setup-hdd] ERROR: missing Stage2 return marker" >&2
  exit 1
fi
if ! grep -aF "AAPPPPSS" "$SERIAL_LOG" >/dev/null; then
  echo "[setup-hdd] ERROR: missing shell prompt" >&2
  exit 1
fi

if [[ "$qemu_rc" -ne 0 && "$qemu_rc" -ne 124 ]]; then
  echo "[setup-hdd] ERROR: unexpected QEMU rc=$qemu_rc" >&2
  exit 1
fi

echo "[setup-hdd] PASS: disposable HDD image is partitioned, FAT16-readable, and boots to shell"
echo "[setup-hdd] target=$TARGET_IMG"
echo "[setup-hdd] serial=$SERIAL_LOG"
