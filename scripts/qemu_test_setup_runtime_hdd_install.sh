#!/usr/bin/env bash
set -euo pipefail

: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$CIUKIOS_ROOT"

OUT_DIR="build/full/setup-hdd"
TARGET_IMG="$OUT_DIR/runtime-hdd-install-target.img"
INSTALL_SERIAL_LOG="$OUT_DIR/runtime_hdd_install.serial.log"
INSTALL_STDERR_LOG="$OUT_DIR/runtime_hdd_install.stderr.log"
BOOT_SERIAL_LOG="$OUT_DIR/runtime_hdd_boot.serial.log"
BOOT_NORMALIZED_LOG="$OUT_DIR/runtime_hdd_boot.normalized.log"
BOOT_STDERR_LOG="$OUT_DIR/runtime_hdd_boot.stderr.log"
INTERRUPT_BOOT_SERIAL_LOG="$OUT_DIR/runtime_hdd_interrupted_boot.serial.log"
INTERRUPT_BOOT_STDERR_LOG="$OUT_DIR/runtime_hdd_interrupted_boot.stderr.log"
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
STAGE1_LST="build/full/obj/full_stage1.lst"
DIRECT_ISO="build/full/ciukios-full-cd-direct.iso"
INSTALL_ISO="${CIUKIOS_RUNTIME_HDD_INSTALL_ISO:-$DIRECT_ISO}"
if [[ "$INSTALL_ISO" == "$DIRECT_ISO" ]]; then
  DEFAULT_SOURCE_DISK_IMG="build/full/ciukios-full-cd-direct-disk.img"
  DEFAULT_EXPECTED_CD_MAP=T
else
  DEFAULT_SOURCE_DISK_IMG="build/full/ciukios-full-cd-disk.img"
  DEFAULT_EXPECTED_CD_MAP=R
fi
SOURCE_DISK_IMG="${CIUKIOS_RUNTIME_HDD_INSTALL_SOURCE_DISK_IMG:-$DEFAULT_SOURCE_DISK_IMG}"
SERIAL_NORMALIZER="$CIUKIOS_ROOT/scripts/serial_log_normalize.py"
TARGET_SECTORS="${CIUKIOS_RUNTIME_HDD_INSTALL_TARGET_SECTORS:-524288}"
INTERRUPT_AT_FIVE="${CIUKIOS_RUNTIME_HDD_INSTALL_INTERRUPT_AT_FIVE:-0}"
ATAPI_FAULT_ONCE="${CIUKIOS_RUNTIME_HDD_INSTALL_ATAPI_FAULT_ONCE:-0}"
ATAPI_BLKDEBUG_CONF="$CIUKIOS_ROOT/scripts/fixtures/blkdebug_atapi_transient.conf"
ATAPI_PERSISTENT_PRIMARY_FAULT="${CIUKIOS_RUNTIME_HDD_INSTALL_ATAPI_PERSISTENT_PRIMARY_FAULT:-0}"
ATAPI_PERSISTENT_BLKDEBUG_CONF="$CIUKIOS_ROOT/scripts/fixtures/blkdebug_atapi_persistent_primary.conf"
EXPECTED_CD_MAP="${CIUKIOS_RUNTIME_HDD_INSTALL_EXPECTED_CD_MAP:-$DEFAULT_EXPECTED_CD_MAP}"
QEMU_MEMORY_MB="${CIUKIOS_RUNTIME_HDD_INSTALL_MEMORY_MB:-128}"
NO_BUILD="${CIUKIOS_RUNTIME_HDD_INSTALL_NO_BUILD:-0}"
EJECT_CD_BEFORE_SETUP="${CIUKIOS_RUNTIME_HDD_INSTALL_EJECT_CD_BEFORE_SETUP:-0}"
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

for tool in dd sha256sum qemu-system-i386 grep socat timeout od mdir python3; do
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
    if [[ -f "$file" ]] && "$SERIAL_NORMALIZER" "$file" | grep -aEq "$pattern"; then
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

hmp_query() {
  local cmd="$1"
  printf "%s\n" "$cmd" >> "$CMD_LOG"
  printf "%s\n" "$cmd" | socat - "UNIX-CONNECT:$MON_SOCK" 2>>"$INSTALL_STDERR_LOG"
}

send_key() {
  hmp_cmd "sendkey $1"
  sleep 0.08
}

send_setup_command() {
  send_key shift-s
  send_key shift-e
  send_key shift-t
  send_key shift-u
  send_key shift-p
  send_key dot
  send_key shift-c
  send_key shift-o
  send_key shift-m
  send_key ret
}

fail_with_rc() {
  local message="$1"
  echo "[setup-runtime-hdd] ERROR: $message" >&2
  printf "%s\n" "1" > "$RC_LOG"
  exit 1
}

if [[ "$NO_BUILD" != "1" ]]; then
  echo "[setup-runtime-hdd] building full-CD source image"
  bash scripts/build_full_cd.sh
fi

if [[ ! -f "$INSTALL_ISO" ]]; then
  fail_with_rc "missing install ISO: $INSTALL_ISO"
fi
if [[ ! -f "$SOURCE_DISK_IMG" ]]; then
  fail_with_rc "missing El Torito source disk image: $SOURCE_DISK_IMG"
fi

mkdir -p "$OUT_DIR"
rm -f "$TARGET_IMG" "$INSTALL_SERIAL_LOG" "$INSTALL_STDERR_LOG" "$BOOT_SERIAL_LOG" "$BOOT_NORMALIZED_LOG" "$BOOT_STDERR_LOG" "$INTERRUPT_BOOT_SERIAL_LOG" "$INTERRUPT_BOOT_STDERR_LOG" "$CMD_LOG" "$RC_LOG" "$HASH_BEFORE" "$HASH_AFTER" "$MBR_SIG_LOG" "$PARTITION_LOG" "$MDIR_ROOT_LOG" "$MDIR_SYSTEM_LOG" "$MDIR_APPS_LOG" "$MON_SOCK"

echo "[setup-runtime-hdd] creating blank disposable target HDD: $TARGET_IMG"
dd if=/dev/zero of="$TARGET_IMG" bs=512 count="$TARGET_SECTORS" status=none
sha256sum "$TARGET_IMG" > "$HASH_BEFORE"

echo "[setup-runtime-hdd] booting install CD with blank HDD attached: $INSTALL_ISO"
cd_backend="$INSTALL_ISO"
if [[ "$ATAPI_FAULT_ONCE" == "1" && "$ATAPI_PERSISTENT_PRIMARY_FAULT" == "1" ]]; then
  fail_with_rc "transient and persistent optical fault modes are mutually exclusive"
elif [[ "$ATAPI_PERSISTENT_PRIMARY_FAULT" == "1" ]]; then
  [[ -f "$ATAPI_PERSISTENT_BLKDEBUG_CONF" ]] \
    || fail_with_rc "missing persistent ATAPI blkdebug configuration: $ATAPI_PERSISTENT_BLKDEBUG_CONF"
  cd_backend="blkdebug:$ATAPI_PERSISTENT_BLKDEBUG_CONF:$DIRECT_ISO"
  echo "[setup-runtime-hdd] injecting persistent optical EIO at virtual source sector 11600"
elif [[ "$ATAPI_FAULT_ONCE" == "1" ]]; then
  [[ -f "$ATAPI_BLKDEBUG_CONF" ]] \
    || fail_with_rc "missing ATAPI blkdebug configuration: $ATAPI_BLKDEBUG_CONF"
  cd_backend="blkdebug:$ATAPI_BLKDEBUG_CONF:$DIRECT_ISO"
  echo "[setup-runtime-hdd] injecting one optical EIO at virtual source sector 10048"
fi
qemu-system-i386 -machine pc,vmport=off -cpu pentium3 -m "$QEMU_MEMORY_MB" -drive file="$TARGET_IMG",format=raw,if=ide,index=0,media=disk -drive file="$cd_backend",format=raw,if=ide,index=2,media=cdrom,readonly=on -boot d -nographic -chardev file,id=ser0,path="$INSTALL_SERIAL_LOG" -serial chardev:ser0 -monitor "unix:$MON_SOCK,server,nowait" -no-reboot -no-shutdown >/dev/null 2>"$INSTALL_STDERR_LOG" &
qemu_pid=$!

if ! wait_for_socket "$MON_SOCK" 20; then
  fail_with_rc "monitor socket not ready"
fi

if ! wait_for_regex "$INSTALL_SERIAL_LOG" 'CiukiOS([[:space:]]+SHELL)?[[:space:]]+D:[\\]APPS>' 90; then
  fail_with_rc "shell prompt marker missing before setup"
fi

if [[ "$EJECT_CD_BEFORE_SETUP" == "1" ]]; then
  hmp_query "eject ide1-cd0" >/dev/null
  block_state="$(hmp_query "info block")"
  if ! grep -Eq 'ide1-cd0:.*\[not inserted\]' <<<"$block_state"; then
    fail_with_rc "QEMU did not confirm that the optical medium was ejected"
  fi
  echo "[setup-runtime-hdd] PASS: QEMU reports ide1-cd0 [not inserted] before SETUP"
fi

send_setup_command

if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-HDD-PROBE\] P=03 B=02 S=01" 45; then
  fail_with_rc "safe QEMU HDD probe marker missing"
fi

# The graphical wizard keeps every write behind the review checkbox.
if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-GUI\] PAGE 00" 45; then
  fail_with_rc "graphical setup did not open"
fi
send_key f
send_key ret
if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-GUI\] PAGE 01" 45; then
  fail_with_rc "destination page missing"
fi
send_key ret
if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-GUI\] PAGE 02" 45; then
  fail_with_rc "format options missing"
fi
send_key ret
if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-GUI\] PAGE 03" 45; then
  fail_with_rc "review page missing"
fi
send_key spc
send_key ret
if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-HDD-FORMAT\] DONE" 90; then
  fail_with_rc "runtime HDD format did not finish"
fi
if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-GUI\] PAGE 05" 45; then
  fail_with_rc "format completion page missing"
fi
send_key ret
sleep 1
send_setup_command
sleep 1
send_key ret
sleep 1
send_key ret
sleep 1
send_key ret
sleep 1
send_key spc
send_key ret

if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-HDD-INSTALL\] START" 300; then
  fail_with_rc "runtime HDD install start marker missing"
fi
if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-CD-MAP\] P=$EXPECTED_CD_MAP" 45; then
  fail_with_rc "expected CD source mapping P=$EXPECTED_CD_MAP missing"
fi
if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-HDD-INSTALL\] PROGRESS 05" 90; then
  fail_with_rc "runtime HDD install did not pass the former 5% reset point"
fi
if [[ "$ATAPI_FAULT_ONCE" == "1" ]]; then
  if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-CD-RETRY\].*L=0000:2740" 45; then
    fail_with_rc "injected optical error did not exercise the ATAPI retry path"
  fi
  echo "[setup-runtime-hdd] PASS: transient ATAPI read error recovered at source LBA 10048"
fi
if [[ "$ATAPI_PERSISTENT_PRIMARY_FAULT" == "1" ]]; then
  # QEMU versions translate a blkdebug EIO either to MEDIUM ERROR (03h) or
  # ILLEGAL REQUEST / LBA OUT OF RANGE (05h/21h).  Both prove REQUEST SENSE
  # was issued and decoded; physical media failures normally report 03h.
  if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-CD-RETRY\].*L=0000:2D50.*SK/ASC/Q=(03|05)/" 90; then
    fail_with_rc "persistent optical error did not produce decoded REQUEST SENSE data"
  fi
  if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-CD-MIRROR\] primary unreadable; using redundant extent" 90; then
    fail_with_rc "persistent primary error did not switch to the redundant optical extent"
  fi
  echo "[setup-runtime-hdd] PASS: permanent primary failure at LBA 11600 recovered from mirror"
fi
if od -An -tx1 -j 510 -N 2 "$TARGET_IMG" | grep -qi "55 aa"; then
  fail_with_rc "target became bootable before clone commit"
fi
echo "[setup-runtime-hdd] PASS: target MBR remains non-bootable during partial clone"
if [[ "$INTERRUPT_AT_FIVE" == "1" ]]; then
  cleanup_qemu
  qemu_pid=""
  echo "[setup-runtime-hdd] booting deliberately interrupted 5% target"
  set +e
  timeout 15 qemu-system-i386 -machine pc,vmport=off -cpu pentium3 -m 128 -drive file="$TARGET_IMG",format=raw,if=ide,index=0,media=disk -boot c -nographic -chardev file,id=ser0,path="$INTERRUPT_BOOT_SERIAL_LOG" -serial chardev:ser0 -monitor none -no-reboot -no-shutdown >/dev/null 2>"$INTERRUPT_BOOT_STDERR_LOG"
  interrupted_boot_rc=$?
  set -e
  if "$SERIAL_NORMALIZER" "$INTERRUPT_BOOT_SERIAL_LOG" 2>/dev/null | grep -aF "[BOOT0-FULL] CiukiOS full stage0 ready" >/dev/null; then
    fail_with_rc "interrupted target unexpectedly reached CiukiOS stage0"
  fi
  if [[ "$interrupted_boot_rc" -ne 0 && "$interrupted_boot_rc" -ne 124 ]]; then
    fail_with_rc "unexpected interrupted-target QEMU rc=$interrupted_boot_rc"
  fi
  echo "[setup-runtime-hdd] PASS: forced 5% interruption cannot boot partial CiukiOS"
  printf "%s\n" "0" > "$RC_LOG"
  exit 0
fi
if ! wait_for_regex "$INSTALL_SERIAL_LOG" "\[SETUP-HDD-INSTALL\] DONE" 300; then
  fail_with_rc "runtime HDD install done marker missing"
fi

cleanup_qemu
qemu_pid=""
sha256sum "$TARGET_IMG" > "$HASH_AFTER"

if cmp -s "$HASH_BEFORE" "$HASH_AFTER"; then
  echo "[setup-runtime-hdd] ERROR: target HDD remained blank after install" >&2
  exit 1
fi

# SETUP must turn the live D: default (index 3) into the installed C: default
# (index 2) in Stage1 before rebooting.  Check the exact instruction byte in
# addition to the end-to-end C:\APPS prompt asserted below.
default_drive_addr_hex="$(awk '/mov byte \[loader_default_drive\], DOS_DEFAULT_DRIVE_INDEX/ {print $2; exit}' "$STAGE1_LST")"
if [[ ! "$default_drive_addr_hex" =~ ^[0-9A-Fa-f]+$ ]]; then
  fail_with_rc "could not locate installed default-drive patch byte"
fi
default_drive_imm_off=$((16#$default_drive_addr_hex + 4))
default_drive_target_off=$((PARTITION_OFFSET_BYTES + 512 + default_drive_imm_off))
if [[ "$(od -An -tu1 -j "$default_drive_target_off" -N1 "$TARGET_IMG" | tr -d ' ')" != "2" ]]; then
  fail_with_rc "installed Stage1 default drive is not C: at byte $default_drive_target_off"
fi
echo "[setup-runtime-hdd] PASS: installed Stage1 default drive patched from D: to C:"

# Apart from the deliberate one-byte D:-to-C: patch, every byte cloned to the
# HDD must match the El Torito source image.  This detects truncated installs
# even when the early filesystem metadata happens to remain bootable.
clone_bytes="$(stat -c%s "$SOURCE_DISK_IMG")"
after_patch=$((default_drive_target_off + 1))
tail_bytes=$((clone_bytes - after_patch))
if ! cmp -n "$default_drive_target_off" "$SOURCE_DISK_IMG" "$TARGET_IMG" >/dev/null; then
  fail_with_rc "cloned prefix differs before default-drive patch"
fi
if ! cmp -i "$after_patch:$after_patch" -n "$tail_bytes" "$SOURCE_DISK_IMG" "$TARGET_IMG" >/dev/null; then
  fail_with_rc "cloned payload differs after default-drive patch"
fi
echo "[setup-runtime-hdd] PASS: every cloned payload byte matches the source image"

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
source_part_count_hex=$(od -An -tx1 -j 458 -N 4 "$SOURCE_DISK_IMG" | tr -d " \n")
if [[ "$part_count_hex" != "$source_part_count_hex" ]]; then
  echo "[setup-runtime-hdd] ERROR: target partition sector count mismatch: target=$part_count_hex source=$source_part_count_hex" >&2
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

if ! "$SERIAL_NORMALIZER" "$BOOT_SERIAL_LOG" > "$BOOT_NORMALIZED_LOG"; then
  echo "[setup-runtime-hdd] ERROR: could not normalize installed-HDD serial log" >&2
  exit 1
fi
if ! grep -aF "[BOOT0-FULL] CiukiOS full stage0 ready" "$BOOT_NORMALIZED_LOG" >/dev/null; then
  echo "[setup-runtime-hdd] ERROR: missing stage0 marker from installed HDD" >&2
  exit 1
fi
if ! grep -aEq 'CiukiOS([[:space:]]+SHELL)?[[:space:]]+C:[\\]APPS>' "$BOOT_NORMALIZED_LOG"; then
  echo "[setup-runtime-hdd] ERROR: missing C:\\APPS readiness prompt from installed HDD" >&2
  exit 1
fi

if [[ "$boot_rc" -ne 0 && "$boot_rc" -ne 124 ]]; then
  echo "[setup-runtime-hdd] ERROR: unexpected target boot QEMU rc=$boot_rc" >&2
  exit 1
fi

echo "[setup-runtime-hdd] PASS: runtime SETUP cloned install image to blank HDD and target boots alone"
echo "0" > "$RC_LOG"
echo "[setup-runtime-hdd] target=$TARGET_IMG"
echo "[setup-runtime-hdd] install_serial=$INSTALL_SERIAL_LOG"
echo "[setup-runtime-hdd] boot_serial=$BOOT_SERIAL_LOG"
