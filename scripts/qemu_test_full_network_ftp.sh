#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
HOST_FTP_PORT="${CIUKIOS_NET_TEST_HOST_FTP_PORT:-8021}"
HOST_PASV_BASE=2048
HOST_PASV_LAST=2303
QEMU_TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-180}"
IMG="build/full/ciukios-full.img"
PREFIX="build/full/qemu-network-ftp"
REPORT="${PREFIX}.report.txt"
SERIAL_ARTIFACT="${PREFIX}.serial.log"
STDERR_ARTIFACT="${PREFIX}.stderr.log"
COMMAND_ARTIFACT="${PREFIX}.commands.log"
SCREENSHOT="${PREFIX}.server.ppm"
UPLOAD_SOURCE="config/network/README.TXT"

usage() {
  cat <<'TXT'
Usage: scripts/qemu_test_full_network_ftp.sh [--no-build]

Boots a disposable copy of the full image and validates the complete network
file-sharing path: NE2000 -> resident bridge -> mTCP IPv4/FTP -> QEMU NAT.
It downloads README.TXT, uploads HOSTPUT.TXT, verifies both hashes, and confirms
that the uploaded bytes were committed to C:\SHARE in the disposable image.

Environment:
  CIUKIOS_NET_TEST_HOST_FTP_PORT  FTP control port on localhost (default: 8021)
  QEMU_TIMEOUT_SEC                QEMU safety timeout (default: 180)
  QEMU_BIN                        qemu-system-i386/x86_64 override
TXT
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build)
      DO_BUILD=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[network-ftp] ERROR: unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ ! "$HOST_FTP_PORT" =~ ^[0-9]+$ ]] \
  || (( HOST_FTP_PORT < 1 || HOST_FTP_PORT > 65535 )); then
  echo "[network-ftp] ERROR: invalid CIUKIOS_NET_TEST_HOST_FTP_PORT=$HOST_FTP_PORT" >&2
  exit 1
fi
if (( HOST_FTP_PORT >= HOST_PASV_BASE && HOST_FTP_PORT <= HOST_PASV_LAST )); then
  echo "[network-ftp] ERROR: host FTP port overlaps passive range 2048-2303" >&2
  exit 1
fi

for command_name in awk cp curl date grep mcopy mkdir mktemp qemu-system-i386 \
  sha256sum socat strings; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    if [[ "$command_name" == qemu-system-i386 ]] \
      && command -v qemu-system-x86_64 >/dev/null 2>&1; then
      continue
    fi
    echo "[network-ftp] ERROR: missing command: $command_name" >&2
    exit 1
  fi
done

if [[ -n "${QEMU_BIN:-}" ]]; then
  QEMU_CMD="$QEMU_BIN"
elif command -v qemu-system-i386 >/dev/null 2>&1; then
  QEMU_CMD=qemu-system-i386
else
  QEMU_CMD=qemu-system-x86_64
fi

mkdir -p build/full
: > "$REPORT"
: > "$SERIAL_ARTIFACT"
: > "$STDERR_ARTIFACT"
: > "$COMMAND_ARTIFACT"

TEST_DIR="$(mktemp -d /tmp/ciukios-network-ftp.XXXXXX)"
case "$TEST_DIR" in
  /tmp/ciukios-network-ftp.*) ;;
  *)
    echo "[network-ftp] ERROR: unsafe temporary directory: $TEST_DIR" >&2
    exit 1
    ;;
esac

TEST_IMG="$TEST_DIR/ciukios-network-test.img"
SERIAL_LOG="$TEST_DIR/serial.log"
STDERR_LOG="$TEST_DIR/qemu.stderr.log"
COMMAND_LOG="$TEST_DIR/commands.log"
MON_SOCK="$TEST_DIR/monitor.sock"
QEMU_PID=0

save_artifacts() {
  [[ -f "$SERIAL_LOG" ]] && cp "$SERIAL_LOG" "$SERIAL_ARTIFACT"
  [[ -f "$STDERR_LOG" ]] && cp "$STDERR_LOG" "$STDERR_ARTIFACT"
  [[ -f "$COMMAND_LOG" ]] && cp "$COMMAND_LOG" "$COMMAND_ARTIFACT"
}

stop_qemu() {
  if (( QEMU_PID > 0 )) && kill -0 "$QEMU_PID" >/dev/null 2>&1; then
    if [[ -S "$MON_SOCK" ]]; then
      printf 'quit\n' | socat - UNIX-CONNECT:"$MON_SOCK" >/dev/null 2>&1 || true
    fi
    for _ in $(seq 1 100); do
      kill -0 "$QEMU_PID" >/dev/null 2>&1 || break
      sleep 0.05
    done
    if kill -0 "$QEMU_PID" >/dev/null 2>&1; then
      kill "$QEMU_PID" >/dev/null 2>&1 || true
    fi
    wait "$QEMU_PID" >/dev/null 2>&1 || true
  fi
  QEMU_PID=0
}

on_exit() {
  local rc="$?"
  trap - EXIT HUP INT TERM
  stop_qemu
  save_artifacts
  if (( rc != 0 )); then
    echo "[network-ftp] FAIL; report: $REPORT" >&2
    tail -n 80 "$STDERR_ARTIFACT" >&2 || true
    strings -a "$SERIAL_ARTIFACT" | tail -n 100 >&2 || true
  fi
  case "$TEST_DIR" in
    /tmp/ciukios-network-ftp.*)
      rm -rf -- "$TEST_DIR"
      ;;
  esac
  exit "$rc"
}

trap on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

pass() {
  local marker="$1"
  echo "[network-ftp] PASS $marker" | tee -a "$REPORT"
}

fail() {
  local marker="$1"
  local detail="$2"
  echo "[network-ftp] FAIL $marker: $detail" | tee -a "$REPORT" >&2
  exit 1
}

wait_for_socket() {
  local deadline=$(( $(date +%s) + $1 ))
  while (( $(date +%s) <= deadline )); do
    [[ -S "$MON_SOCK" ]] && return 0
    kill -0 "$QEMU_PID" >/dev/null 2>&1 || return 1
    sleep 0.05
  done
  return 1
}

wait_for_serial_literal() {
  local literal="$1"
  local timeout_sec="$2"
  local deadline=$(( $(date +%s) + timeout_sec ))
  while (( $(date +%s) <= deadline )); do
    if [[ -f "$SERIAL_LOG" ]] && strings -a "$SERIAL_LOG" | grep -Fq -- "$literal"; then
      return 0
    fi
    kill -0 "$QEMU_PID" >/dev/null 2>&1 || return 1
    sleep 0.05
  done
  return 1
}

hmp() {
  local command="$1"
  echo "[HMP] $command" >> "$COMMAND_LOG"
  printf '%s\n' "$command" | socat - UNIX-CONNECT:"$MON_SOCK" >> "$COMMAND_LOG" 2>&1
}

send_key() {
  hmp "sendkey $1" >/dev/null
}

send_text() {
  local input="$1"
  local index char key
  for ((index=0; index<${#input}; index++)); do
    char="${input:index:1}"
    case "$char" in
      ' ') key=spc ;;
      '.') key=dot ;;
      '/') key=slash ;;
      '\') key=backslash ;;
      '-') key=minus ;;
      ':') key=shift-semicolon ;;
      [A-Z]) key="shift-$(printf '%s' "$char" | tr 'A-Z' 'a-z')" ;;
      [a-z0-9]) key="$char" ;;
      *) continue ;;
    esac
    send_key "$key"
  done
}

send_command() {
  send_text "$1"
  send_key ret
}

echo "[network-ftp] preparing verified mTCP/Crynwr payload"
bash scripts/fetch_network_stack.sh

if (( DO_BUILD )); then
  echo "[network-ftp] building full image"
  bash scripts/build_full.sh
fi

[[ -f "$IMG" ]] || fail IMAGE "missing $IMG"
[[ -f "$UPLOAD_SOURCE" ]] || fail UPLOAD_SOURCE "missing $UPLOAD_SOURCE"

CANONICAL_HASH_BEFORE="$(sha256sum "$IMG" | awk '{print $1}')"
cp "$IMG" "$TEST_IMG"
pass DISPOSABLE_IMAGE_READY

QEMU_ACCEL_ARGS=()
if [[ -r /dev/kvm && -w /dev/kvm ]]; then
  QEMU_ACCEL_ARGS=(-accel kvm)
fi

NETDEV_SPEC="user,id=ciuknet0,hostfwd=tcp:127.0.0.1:${HOST_FTP_PORT}-:21"
for ((passive_port=HOST_PASV_BASE; passive_port<=HOST_PASV_LAST; passive_port++)); do
  NETDEV_SPEC+=",hostfwd=tcp:127.0.0.1:${passive_port}-:${passive_port}"
done

QEMU_ARGS=(
  "${QEMU_ACCEL_ARGS[@]}"
  -machine pc,vmport=off
  -cpu pentium3
  -m 128
  -drive "file=$TEST_IMG,format=raw,if=ide"
  -boot c
  -vga std
  -display none
  -serial "file:$SERIAL_LOG"
  -monitor "unix:$MON_SOCK,server,nowait"
  -netdev "$NETDEV_SPEC"
  -device "ne2k_isa,netdev=ciuknet0,irq=3,iobase=0x300,mac=52:54:00:12:34:56"
  -no-reboot
  -no-shutdown
)

timeout "$QEMU_TIMEOUT_SEC" "$QEMU_CMD" "${QEMU_ARGS[@]}" \
  >/dev/null 2>"$STDERR_LOG" &
QEMU_PID=$!

wait_for_socket 20 || fail MONITOR_SOCKET "QEMU monitor did not become ready"
pass MONITOR_SOCKET

INITIAL_PROMPT='CCiiuukkiiOOSS  SSHHEELLLL  CC::\\AAPPPPSS>>'
wait_for_serial_literal "$INITIAL_PROMPT" 90 \
  || fail SHELL_READY "initial C:\\APPS prompt not detected"
pass SHELL_READY

send_command 'cd C:\NET'
sleep 1
send_command 'run netstart.com'
wait_for_serial_literal 'My Ethernet address is 52:54:00:12:34:56' 20 \
  || fail PACKET_DRIVER "NE2000 packet driver did not initialize"
pass PACKET_DRIVER
wait_for_serial_literal 'NETSTART: ICMP resident active; mTCP Packet Driver is INT 61h' 20 \
  || fail RESIDENT_BRIDGE "NETSTART did not install the resident network bridge"
pass RESIDENT_BRIDGE

send_command 'run ping.exe -count 1 -timeout 5 1.1.1.1'
wait_for_serial_literal 'Replies received: 1' 20 \
  || fail OUTBOUND_INTERNET_ICMP "mTCP did not receive an Internet Echo Reply from 1.1.1.1"
pass OUTBOUND_INTERNET_ICMP

send_command 'run ftpsrv.exe'

CURL_COMMON=(
  --noproxy '*'
  --fail
  --silent
  --show-error
  --connect-timeout 2
  --max-time 8
  --ftp-skip-pasv-ip
  --user ciukios:ciukios
)
FTP_URL="ftp://127.0.0.1:${HOST_FTP_PORT}"
LISTING=""
for _ in $(seq 1 60); do
  if LISTING="$(curl "${CURL_COMMON[@]}" --list-only "$FTP_URL/" 2>/dev/null)"; then
    break
  fi
  kill -0 "$QEMU_PID" >/dev/null 2>&1 \
    || fail FTP_SERVER "QEMU exited while waiting for mTCP FTP"
  sleep 0.25
done
[[ "$LISTING" == *README.TXT* ]] \
  || fail FTP_LIST "C:\\SHARE/README.TXT was not listed"
pass FTP_LOGIN_AND_LIST

hmp "screendump $ROOT_DIR/$SCREENSHOT" >/dev/null || true

EXPECTED_HASH="$(sha256sum "$UPLOAD_SOURCE" | awk '{print $1}')"
DOWNLOAD_HASH="$(curl "${CURL_COMMON[@]}" "$FTP_URL/README.TXT" \
  | sha256sum | awk '{print $1}')"
[[ "$DOWNLOAD_HASH" == "$EXPECTED_HASH" ]] \
  || fail FTP_DOWNLOAD_HASH "expected $EXPECTED_HASH, got $DOWNLOAD_HASH"
pass FTP_DOWNLOAD

curl "${CURL_COMMON[@]}" --upload-file "$UPLOAD_SOURCE" \
  "$FTP_URL/HOSTPUT.TXT"
ROUNDTRIP_HASH="$(curl "${CURL_COMMON[@]}" "$FTP_URL/HOSTPUT.TXT" \
  | sha256sum | awk '{print $1}')"
[[ "$ROUNDTRIP_HASH" == "$EXPECTED_HASH" ]] \
  || fail FTP_UPLOAD_HASH "expected $EXPECTED_HASH, got $ROUNDTRIP_HASH"
pass FTP_UPLOAD_ROUNDTRIP

send_key ctrl-c || true
sleep 1
stop_qemu

IMAGE_HASH="$(mcopy -i "$TEST_IMG" ::SHARE/HOSTPUT.TXT - \
  | sha256sum | awk '{print $1}')"
[[ "$IMAGE_HASH" == "$EXPECTED_HASH" ]] \
  || fail IMAGE_COMMIT_HASH "expected $EXPECTED_HASH, got $IMAGE_HASH"
pass IMAGE_COMMIT

CANONICAL_HASH_AFTER="$(sha256sum "$IMG" | awk '{print $1}')"
[[ "$CANONICAL_HASH_AFTER" == "$CANONICAL_HASH_BEFORE" ]] \
  || fail CANONICAL_IMAGE_IMMUTABLE "the canonical full image changed during the test"
pass CANONICAL_IMAGE_IMMUTABLE

{
  printf 'ftp_url=%s\n' "$FTP_URL/"
  printf 'guest_ip=10.0.2.15\n'
  printf 'packet_driver=NE2000 INT60 IRQ3 IO300; resident bridge INT61\n'
  printf 'download_sha256=%s\n' "$DOWNLOAD_HASH"
  printf 'upload_sha256=%s\n' "$ROUNDTRIP_HASH"
  printf 'image_sha256=%s\n' "$IMAGE_HASH"
  printf 'result=PASS\n'
} >> "$REPORT"

echo "[network-ftp] PASS end-to-end FTP file sharing"
echo "[network-ftp] report: $REPORT"
