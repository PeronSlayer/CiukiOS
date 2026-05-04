#!/usr/bin/env bash
set -euo pipefail

: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$CIUKIOS_ROOT"

OUT_DIR="build/full/setup-hdd"
TARGET_IMG="$OUT_DIR/cd-probe-blank-hdd.img"
SERIAL_LOG="$OUT_DIR/cd_probe.serial.log"
STDERR_LOG="$OUT_DIR/cd_probe.stderr.log"
CMD_LOG="$OUT_DIR/cd_probe.commands.log"
MON_SOCK="/tmp/ciukios-setup-cd-hdd-probe-$$.monitor.sock"
RC_LOG="$OUT_DIR/qemu_test_setup_cd_hdd_probe.rc"
HASH_BEFORE="$OUT_DIR/cd_probe_hdd_before.sha256"
HASH_AFTER="$OUT_DIR/cd_probe_hdd_after.sha256"
DIRECT_ISO="build/full/ciukios-full-cd-direct.iso"
TARGET_SECTORS="${CIUKIOS_CD_HDD_PROBE_TARGET_SECTORS:-524288}"
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
    echo "[setup-cd-hdd] ERROR: refusing unsafe target path: $TARGET_IMG" >&2
    exit 1
    ;;
esac

for tool in dd sha256sum qemu-system-i386 grep socat; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "[setup-cd-hdd] ERROR: required tool not found: $tool" >&2
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
  printf "%s\n" "$cmd" | socat - "UNIX-CONNECT:$MON_SOCK" >/dev/null 2>>"$STDERR_LOG"
}

send_key() {
  hmp_cmd "sendkey $1"
  sleep 0.05
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

echo "[setup-cd-hdd] building full-CD source image"
bash scripts/build_full_cd.sh

if [[ ! -f "$DIRECT_ISO" ]]; then
  echo "[setup-cd-hdd] ERROR: missing direct ISO: $DIRECT_ISO" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
rm -f "$TARGET_IMG" "$SERIAL_LOG" "$STDERR_LOG" "$CMD_LOG" "$RC_LOG" "$HASH_BEFORE" "$HASH_AFTER" "$MON_SOCK"

echo "[setup-cd-hdd] creating blank disposable target HDD: $TARGET_IMG"
dd if=/dev/zero of="$TARGET_IMG" bs=512 count="$TARGET_SECTORS" status=none
sha256sum "$TARGET_IMG" > "$HASH_BEFORE"

echo "[setup-cd-hdd] booting direct CD with blank HDD attached"
qemu-system-i386 -machine pc,vmport=off -cpu pentium3 -m 128 -drive file="$TARGET_IMG",format=raw,if=ide,index=0,media=disk -drive file="$DIRECT_ISO",format=raw,if=ide,index=2,media=cdrom,readonly=on -boot d -nographic -chardev file,id=ser0,path="$SERIAL_LOG" -serial chardev:ser0 -monitor "unix:$MON_SOCK,server,nowait" -no-reboot -no-shutdown >/dev/null 2>"$STDERR_LOG" &
qemu_pid=$!

if ! wait_for_socket "$MON_SOCK" 20; then
  echo "[setup-cd-hdd] ERROR: monitor socket not ready" >&2
  printf "%s\n" "1" > "$RC_LOG"
  exit 1
fi

if ! wait_for_regex "$SERIAL_LOG" "AAPPPPSS" 90; then
  echo "[setup-cd-hdd] ERROR: shell prompt marker missing before setup" >&2
  printf "%s\n" "1" > "$RC_LOG"
  exit 1
fi

send_setup_command

if ! wait_for_regex "$SERIAL_LOG" "\[SETUP-HDD-PROBE\] P=03 B=02 S=01" 45; then
  echo "[setup-cd-hdd] ERROR: setup BIOS HDD probe marker missing" >&2
  printf "%s\n" "1" > "$RC_LOG"
  exit 1
fi

cleanup_qemu
sha256sum "$TARGET_IMG" > "$HASH_AFTER"

if ! cmp -s "$HASH_BEFORE" "$HASH_AFTER"; then
  echo "[setup-cd-hdd] ERROR: blank target HDD changed during probe" >&2
  exit 1
fi

if ! grep -aF "Booting from DVD/CD" "$SERIAL_LOG" >/dev/null; then
  echo "[setup-cd-hdd] ERROR: CD boot marker missing" >&2
  exit 1
fi
if ! grep -aF "[STAGE1-SERIAL] READY" "$SERIAL_LOG" >/dev/null; then
  echo "[setup-cd-hdd] ERROR: Stage1 marker missing" >&2
  exit 1
fi
if ! grep -aF "[STAGE2] return to shell" "$SERIAL_LOG" >/dev/null; then
  echo "[setup-cd-hdd] ERROR: Stage2 return marker missing" >&2
  exit 1
fi
if ! grep -aF "PPAASSSS" "$SERIAL_LOG" >/dev/null; then
  echo "[setup-cd-hdd] ERROR: HW PASS marker missing" >&2
  exit 1
fi
if ! grep -aF "[SETUP-HDD-PROBE]" "$SERIAL_LOG" >/dev/null; then
  echo "[setup-cd-hdd] ERROR: setup probe marker missing" >&2
  exit 1
fi

echo "0" > "$RC_LOG"
echo "[setup-cd-hdd] PASS: direct CD boots, SETUP probes BIOS disks read-only, and blank HDD remains unchanged"
echo "[setup-cd-hdd] target=$TARGET_IMG"
echo "[setup-cd-hdd] serial=$SERIAL_LOG"
