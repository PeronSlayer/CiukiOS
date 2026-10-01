#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
QEMU_TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-120}"
MCAST_GROUP="${CIUKIOS_ICMP_TEST_GROUP:-230.0.0.42}"
MCAST_PORT="${CIUKIOS_ICMP_TEST_PORT:-12342}"
IMG="build/full/ciukios-full.img"
PREFIX="build/full/qemu-network-icmp"
REPORT="${PREFIX}.report.txt"
SERIAL_ARTIFACT="${PREFIX}.serial.log"
STDERR_ARTIFACT="${PREFIX}.stderr.log"
COMMAND_ARTIFACT="${PREFIX}.commands.log"
PEER_ARTIFACT="${PREFIX}.peer.log"

usage() {
  cat <<'TXT'
Usage: scripts/qemu_test_full_network_icmp.sh [--no-build]

Boots a disposable full image on an isolated QEMU multicast LAN, runs NETSTART,
injects an Ethernet ICMP Echo Request while FTPSRV is not running, and requires
a checksum-valid resident Echo Reply from CiukiOS.
TXT
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build) DO_BUILD=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *)
      echo "[network-icmp] ERROR: unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ ! "$MCAST_PORT" =~ ^[0-9]+$ ]] \
  || (( MCAST_PORT < 1 || MCAST_PORT > 65535 )); then
  echo "[network-icmp] ERROR: invalid CIUKIOS_ICMP_TEST_PORT=$MCAST_PORT" >&2
  exit 1
fi

for command_name in cp date grep mktemp python3 sha256sum socat strings timeout; do
  command -v "$command_name" >/dev/null 2>&1 \
    || { echo "[network-icmp] ERROR: missing command: $command_name" >&2; exit 1; }
done

if [[ -n "${QEMU_BIN:-}" ]]; then
  QEMU_CMD="$QEMU_BIN"
elif command -v qemu-system-i386 >/dev/null 2>&1; then
  QEMU_CMD=qemu-system-i386
elif command -v qemu-system-x86_64 >/dev/null 2>&1; then
  QEMU_CMD=qemu-system-x86_64
else
  echo "[network-icmp] ERROR: QEMU is required" >&2
  exit 1
fi

mkdir -p build/full
: > "$REPORT"
: > "$SERIAL_ARTIFACT"
: > "$STDERR_ARTIFACT"
: > "$COMMAND_ARTIFACT"
: > "$PEER_ARTIFACT"

TEST_DIR="$(mktemp -d /tmp/ciukios-network-icmp.XXXXXX)"
case "$TEST_DIR" in
  /tmp/ciukios-network-icmp.*) ;;
  *) echo "[network-icmp] ERROR: unsafe temporary directory: $TEST_DIR" >&2; exit 1 ;;
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
    kill "$QEMU_PID" >/dev/null 2>&1 || true
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
    echo "[network-icmp] FAIL; report: $REPORT" >&2
    tail -n 80 "$STDERR_ARTIFACT" >&2 || true
    strings -a "$SERIAL_ARTIFACT" | tail -n 100 >&2 || true
  fi
  case "$TEST_DIR" in
    /tmp/ciukios-network-icmp.*) rm -rf -- "$TEST_DIR" ;;
  esac
  exit "$rc"
}

trap on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

pass() {
  echo "[network-icmp] PASS $1" | tee -a "$REPORT"
}

fail() {
  echo "[network-icmp] FAIL $1: $2" | tee -a "$REPORT" >&2
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
  local literal="$1" timeout_sec="$2"
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
  echo "[HMP] $1" >> "$COMMAND_LOG"
  printf '%s\n' "$1" | socat - UNIX-CONNECT:"$MON_SOCK" >> "$COMMAND_LOG" 2>&1
}

send_key() {
  hmp "sendkey $1" >/dev/null
}

send_text() {
  local input="$1" index char key
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

if (( DO_BUILD )); then
  echo "[network-icmp] preparing payload and building full image"
  bash scripts/fetch_network_stack.sh
  bash scripts/build_full.sh
fi

[[ -f "$IMG" ]] || fail IMAGE "missing $IMG"
CANONICAL_HASH_BEFORE="$(sha256sum "$IMG" | awk '{print $1}')"
cp "$IMG" "$TEST_IMG"
pass DISPOSABLE_IMAGE_READY

QEMU_ACCEL_ARGS=(-accel 'tcg,one-insn-per-tb=on')
if [[ -r /dev/kvm && -w /dev/kvm ]]; then
  QEMU_ACCEL_ARGS=(-accel kvm)
fi

QEMU_ARGS=(
  "${QEMU_ACCEL_ARGS[@]}"
  -machine pc,vmport=off
  -cpu pentium3
  -m 128
  -drive "file=$TEST_IMG,format=raw,if=ide"
  -boot c
  -display none
  -serial "file:$SERIAL_LOG"
  -monitor "unix:$MON_SOCK,server,nowait"
  -netdev "socket,id=ciuknet0,mcast=${MCAST_GROUP}:${MCAST_PORT}"
  -device "ne2k_isa,netdev=ciuknet0,irq=3,iobase=0x300,mac=52:54:00:12:34:56"
  -no-reboot
  -no-shutdown
)

timeout "$QEMU_TIMEOUT_SEC" "$QEMU_CMD" "${QEMU_ARGS[@]}" \
  >/dev/null 2>"$STDERR_LOG" &
QEMU_PID=$!

wait_for_socket 20 || fail MONITOR_SOCKET "QEMU monitor did not become ready"
pass MONITOR_SOCKET_READY
wait_for_serial_literal '[DESKTOP] READY' 90 \
  || fail DESKTOP_READY "graphical desktop did not become ready"
pass DESKTOP_READY
send_key f4
wait_for_serial_literal '[DESKTOP] DOS' 20 \
  || fail DOS_HANDOFF "F4 did not leave the desktop for DOS"
wait_for_serial_literal 'CiukiOS SHELL C:\APPS>' 20 \
  || fail SHELL_READY "DOS shell prompt not detected after F4"
pass SHELL_READY

send_command 'netstart'
wait_for_serial_literal 'My Ethernet address is 52:54:00:12:34:56' 20 \
  || fail PACKET_DRIVER "NE2000 packet driver did not initialize"
pass PACKET_DRIVER
wait_for_serial_literal 'NETSTART: ICMP resident active; mTCP Packet Driver is INT 61h' 20 \
  || fail RESIDENT_ICMP "resident ICMP service did not install"
pass RESIDENT_ICMP_WITHOUT_FTPSRV
send_command 'netcfg static 10.0.2.25 255.255.255.0 10.0.2.2 1.1.1.1'
wait_for_serial_literal 'NETCFG: static IPv4 configuration saved' 20 \
  || fail NETCFG_STATIC "NETCFG did not persist IP, subnet, gateway and DNS"
wait_for_serial_literal 'NETCFG: resident ICMP address updated immediately' 20 \
  || fail NETCFG_APPLY "NETCFG did not update the active resident service"
pass NETCFG_STATIC_PERSISTENCE_AND_LIVE_APPLY
send_command 'ipconfig'
wait_for_serial_literal 'IPv4 address : 10.0.2.25' 20 \
  || fail IPCONFIG_STATIC "IPCONFIG did not show the saved IPv4 value"
wait_for_serial_literal 'DNS server   : 1.1.1.1' 20 \
  || fail IPCONFIG_STATIC "IPCONFIG did not show the saved DNS value"
wait_for_serial_literal 'ICMP receive : ACTIVE (resident, independent of FTPSRV)' 20 \
  || fail IPCONFIG_ICMP "IPCONFIG did not report resident ICMP active"
pass IPCONFIG_STATIC_AND_ICMP_STATUS
sleep 1
python3 scripts/icmp_echo_peer.py \
  --group "$MCAST_GROUP" \
  --port "$MCAST_PORT" \
  --guest-ip 10.0.2.25 \
  --timeout 20 | tee "$PEER_ARTIFACT" \
  || fail ICMP_ECHO_REPLY "no valid inbound Echo Reply"
grep -Fq 'ICMP_PEER_ECHO_REPLY' "$PEER_ARTIFACT" \
  || fail ICMP_ECHO_REPLY "peer success marker missing"
pass ICMP_ECHO_REPLY

stop_qemu
CANONICAL_HASH_AFTER="$(sha256sum "$IMG" | awk '{print $1}')"
[[ "$CANONICAL_HASH_AFTER" == "$CANONICAL_HASH_BEFORE" ]] \
  || fail CANONICAL_IMAGE_IMMUTABLE "canonical image changed during gate"
pass CANONICAL_IMAGE_IMMUTABLE

echo "[network-icmp] PASS inbound resident ICMP without FTPSRV"
echo "[network-icmp] report: $REPORT"
