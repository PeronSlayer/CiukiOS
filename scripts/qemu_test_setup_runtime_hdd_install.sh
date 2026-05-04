#!/usr/bin/env bash
set -euo pipefail

: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$CIUKIOS_ROOT"

OUT_DIR="build/full/setup-hdd"
TARGET_IMG="$OUT_DIR/runtime-hdd-install-target.img"
INSTALL_SERIAL_LOG="$OUT_DIR/runtime_hdd_install.serial.log"
INSTALL_STDERR_LOG="$OUT_DIR/runtime_hdd_install.stderr.log"
BOOT_SERIAL_LOG="$OUT_DIR/runtime_hdd_boot.serial.log"
BOOT_STDERR_LOG="$OUT_DIR/runtime_hdd_boot.stderr.log"
CMD_LOG="$OUT_DIR/runtime_hdd_install.commands.log"
MON_SOCK="/tmp/ciukios-setup-runtime-hdd-install-$$.monitor.sock"
RC_LOG="$OUT_DIR/qemu_test_setup_runtime_hdd_install.rc"
HASH_BEFORE="$OUT_DIR/runtime_hdd_before.sha256"
HASH_AFTER="$OUT_DIR/runtime_hdd_after.sha256"
MBR_SIG_LOG="$OUT_DIR/runtime_hdd_mbr_sig.txt"
PARTITION_LOG="$OUT_DIR/runtime_hdd_partition_entry.hex"
MDIR_ROOT_LOG="$OUT_DIR/runtime_hdd_mdir_root.txt"
MDIR_SYSTEM_LOG="$OUT_DIR/runtime_hdd_mdir_system.txt"
MDIR_APPS_LOG="$OUT_DIR/runtime_hdd_mdir_apps.txt"
PARTITION_OFFSET_BYTES=32256
DIRECT_ISO="build/full/ciukios-full-cd-direct.iso"
TARGET_SECTORS="${CIUKIOS_RUNTIME_HDD_INSTALL_TARGET_SECTORS:-524288}"
qemu_pid=""

cleanup_qemu() {
  if [[ -n "${qemu_pid:-}" ]] && kill -0 "$qemu_pid" >/dev/null 2>&1; then
    kill "$qemu_pid" >/dev/null 2>&1 || true
    wait "$qemu_pid" >/dev/null 2>&1 || true
  fi
  rm -f "$MON_SOCK"
}
trap cleanup_qemu EXIT

case "$TARGET_IMG" in
  build/full/setup-hdd/*.img) ;;
  *)
    echo "[setup-runtime-hdd] ERROR: refusing unsafe target path: $TARGET_IMG" >&2
    exit 1
    ;;
esac

for tool in dd sha256sum qemu-system-i386 grep socat timeout od mdir; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "[setup-runtime-hdd] ERROR: required tool not found: $tool" >&2
    exit 1
  fi
done

wait_for_regex() {
  local file="$1"
  local pattern="$2"
  local timeout_sec="$3"
  local start=$SECONDS
  while (( SECONDS - start < timeout_sec )); do
    if [[ -f "$file" ]] && grep -aEq "$pattern" "$file"; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

wait_for_socket() {
  local sock="$1"
  local timeout_sec="$2"
  local start=$SECONDS
  while (( SECONDS - start < timeout_sec )); do
    if [[ -S "$sock" ]]; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

hmp_cmd() {
  local cmd="$1"
  printf "%s\n" "$cmd" >> "$CMD_LOG"
  printf "%s\n" "$cmd" | socat - "UNIX-CONNECT:$MON_SOCK" >/dev/null 2>>"$INSTALL_STDERR_LOG"
}

send_key() {
  hmp_cmd "sendkey $1"
  sleep 0.08
}

send_setup_command() {
  send_key s
  send_key e
  send_key t
  send_key u
  send_key p
  send_key dot
  send_key c
  send_key o
  send_key m
  send_key ret
}

fail_with_rc() {
  local message="$1"
  echo "[setup-runtime-hdd] ERROR: $message" >&2
  printf "%s\n" "1" > "$RC_LOG"
  exit 1
}

echo "[setup-runtime-hdd] building full-CD source image"
CIUKIOS_SETUP_RAW_HDD_INSTALL=1 bash scripts/build_full_cd.sh

if [[ ! -f "$DIRECT_ISO" ]]; then
  fail_with_rc "missing direct ISO: $DIRECT_ISO"
fi

mkdir -p "$OUT_DIR"
rm -f "$TARGET_IMG" "$INSTALL_SERIAL_LOG" "$INSTALL_STDERR_LOG" "$BOOT_SERIAL_LOG" "$BOOT_STDERR_LOG" "$CMD_LOG" "$RC_LOG" "$HASH_BEFORE" "$HASH_AFTER" "$MBR_SIG_LOG" "$PARTITION_LOG" "$MDIR_ROOT_LOG" "$MDIR_SYSTEM_LOG" "$MDIR_APPS_LOG" "$MON_SOCK"

echo "[setup-runtime-hdd] creating blank disposable target HDD: $TARGET_IMG"
dd if=/dev/zero of="$TARGET_IMG" bs=512 count="$TARGET_SECTORS" status=none
sha256sum "$TARGET_IMG" > "$HASH_BEFORE"

echo "[setup-runtime-hdd] booting direct CD with blank HDD attached"
qemu-system-i386 -machine pc,vmport=off -cpu pentium3 -m 128 -drive file="$TARGET_IMG",format=raw,if=ide,index=0,media=disk -drive file="$DIRECT_ISO",format=raw,if=ide,index=2,media=cdrom,readonly=on -boot d -nographic -chardev file,id=ser0,path="$INSTALL_SERIAL_LOG" -serial chardev:ser0 -monitor "unix:$MON_SOCK,server,nowait" -no-reboot -no-shutdown >/dev/null 2>"$INSTALL_STDERR_LOG" &
qemu_pid=$!

if ! wait_for_socket "$MON_SOCK" 20; then
  fail_with_rc "monitor socket not ready"
fi

if ! wait_for_regex "$INSTALL_SERIAL_LOG" "AAPPPPSS" 90; then
  fail_with_rc "shell prompt marker missing before setup"
fi

send_setup_command

if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-HDD-PROBE\] P=03 B=02 S=01" 45; then
  fail_with_rc "safe QEMU HDD probe marker missing"
fi

send_key ret
send_key ret
send_key d
send_key ret

if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-HDD-INSTALL\] START" 30; then
  fail_with_rc "runtime HDD install start marker missing"
fi
if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-HDD-INSTALL\] DONE" 180; then
  fail_with_rc "runtime HDD install done marker missing"
fi

cleanup_qemu
qemu_pid=""
sha256sum "$TARGET_IMG" > "$HASH_AFTER"

if cmp -s "$HASH_BEFORE" "$HASH_AFTER"; then
  echo "[setup-runtime-hdd] ERROR: target HDD remained blank after install" >&2
  exit 1
fi

dd if="$TARGET_IMG" bs=1 skip=510 count=2 status=none | od -An -tx1 > "$MBR_SIG_LOG"
if ! grep -qi "55 aa" "$MBR_SIG_LOG"; then
  echo "[setup-runtime-hdd] ERROR: invalid target MBR signature" >&2
  cat "$MBR_SIG_LOG" >&2
  exit 1
fi

dd if="$TARGET_IMG" bs=1 skip=446 count=16 status=none | od -An -tx1 > "$PARTITION_LOG"
if ! od -An -tx1 -j 450 -N 1 "$TARGET_IMG" | grep -qi "06"; then
  echo "[setup-runtime-hdd] ERROR: target partition type is not FAT16 0x06" >&2
  cat "$PARTITION_LOG" >&2
  exit 1
fi
part_lba_hex=$(od -An -tx1 -j 454 -N 4 "$TARGET_IMG" | tr -d " \n")
if [[ "$part_lba_hex" != "3f000000" ]]; then
  echo "[setup-runtime-hdd] ERROR: target partition start LBA mismatch: $part_lba_hex" >&2
  cat "$PARTITION_LOG" >&2
  exit 1
fi
part_count_hex=$(od -An -tx1 -j 458 -N 4 "$TARGET_IMG" | tr -d " \n")
if [[ "$part_count_hex" != "00000400" ]]; then
  echo "[setup-runtime-hdd] ERROR: target partition sector count mismatch: $part_count_hex" >&2
  cat "$PARTITION_LOG" >&2
  exit 1
fi

mdir -i "$TARGET_IMG@@$PARTITION_OFFSET_BYTES" :: > "$MDIR_ROOT_LOG"
mdir -i "$TARGET_IMG@@$PARTITION_OFFSET_BYTES" ::SYSTEM > "$MDIR_SYSTEM_LOG"
mdir -i "$TARGET_IMG@@$PARTITION_OFFSET_BYTES" ::APPS > "$MDIR_APPS_LOG"

echo "[setup-runtime-hdd] booting installed target HDD alone"
set +e
timeout 45 qemu-system-i386 -machine pc,vmport=off -cpu pentium3 -m 128 -drive file="$TARGET_IMG",format=raw,if=ide,index=0,media=disk -boot c -nographic -chardev file,id=ser0,path="$BOOT_SERIAL_LOG" -serial chardev:ser0 -monitor none -no-reboot -no-shutdown >/dev/null 2>"$BOOT_STDERR_LOG"
boot_rc=$?
set -e

if ! grep -aF "[BOOT0-FULL] CiukiOS full stage0 ready" "$BOOT_SERIAL_LOG" >/dev/null; then
  echo "[setup-runtime-hdd] ERROR: missing stage0 marker from installed HDD" >&2
  exit 1
fi
if ! grep -aF "[STAGE1-SERIAL] READY" "$BOOT_SERIAL_LOG" >/dev/null; then
  echo "[setup-runtime-hdd] ERROR: missing stage1 marker from installed HDD" >&2
  exit 1
fi
if ! grep -aF "[STAGE2] return to shell" "$BOOT_SERIAL_LOG" >/dev/null; then
  echo "[setup-runtime-hdd] ERROR: missing stage2 marker from installed HDD" >&2
  exit 1
fi
if ! grep -aF "AAPPPPSS" "$BOOT_SERIAL_LOG" >/dev/null; then
  echo "[setup-runtime-hdd] ERROR: missing shell prompt from installed HDD" >&2
  exit 1
fi

if [[ "$boot_rc" -ne 0 && "$boot_rc" -ne 124 ]]; then
  echo "[setup-runtime-hdd] ERROR: unexpected target boot QEMU rc=$boot_rc" >&2
  exit 1
fi

echo "[setup-runtime-hdd] PASS: runtime SETUP cloned direct CD image to blank HDD and target boots alone"
echo "0" > "$RC_LOG"
echo "[setup-runtime-hdd] target=$TARGET_IMG"
echo "[setup-runtime-hdd] install_serial=$INSTALL_SERIAL_LOG"
echo "[setup-runtime-hdd] boot_serial=$BOOT_SERIAL_LOG"
