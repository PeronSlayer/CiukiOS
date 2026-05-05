#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD=1
IMG="build/full/ciukios-full.img"
PREFIX="build/full/qemu-full-dos-compat-smoke"
SERIAL_LOG="${PREFIX}.serial.log"
STRINGS_LOG="${PREFIX}.strings.log"
STDERR_LOG="${PREFIX}.stderr.log"
CMD_LOG="${PREFIX}.commands.log"
META_LOG="${PREFIX}.meta"
MON_SOCK="/tmp/ciukios-full-dos-compat-smoke.monitor.sock"

ACTIVE_QEMU_PID=0
ACTIVE_MON_SOCK=""
ACTIVE_CMD_LOG=""

usage() {
  cat << 'TXT'
Usage: scripts/qemu_test_full_dos_compat_smoke.sh [--no-build]

Boots the full profile headlessly and validates DOS compatibility smoke flow:
  run \APPS\CIUKEDIT.COM MATRIX.TXT
  wait for [CIUKEDIT:BOOT]
  wait for Enter line>
  submit one input line
  wait for [CIUKEDIT:OK]
  verify prompt returns
  run built-in gfxstar command
  wait for [GFXSTAR-SERIAL] PASS
  verify prompt returns
  if third_party/DOSNavigator/DN.COM is present:
    cd \APPS\DOSNAV
    verify prompt changes to C:\APPS\DOSNAV>
    run DN.COM
    wait for a DOSNavigator startup banner marker
  otherwise: print skip/pass note and keep the lane green

Artifacts:
  build/full/qemu-full-dos-compat-smoke.{serial.log,strings.log,stderr.log,commands.log,meta}
TXT
}

mark_fail() {
  local marker="$1"
  local detail="$2"
  echo "[dos-compat-smoke] FAIL ${marker}: ${detail}" >&2
  if [[ -f "$STDERR_LOG" ]]; then
    tail -n 40 "$STDERR_LOG" >&2 || true
  fi
  if [[ -f "$SERIAL_LOG" ]]; then
    tail -n 200 "$SERIAL_LOG" >&2 || true
  fi
  exit 1
}

mark_pass() {
  local marker="$1"
  echo "[dos-compat-smoke] PASS ${marker}"
}

cleanup_active_qemu() {
  if [[ "$ACTIVE_QEMU_PID" -ne 0 ]] && kill -0 "$ACTIVE_QEMU_PID" >/dev/null 2>&1; then
    if [[ -n "$ACTIVE_MON_SOCK" ]] && [[ -S "$ACTIVE_MON_SOCK" ]] && [[ -n "$ACTIVE_CMD_LOG" ]]; then
      hmp "$ACTIVE_MON_SOCK" "$ACTIVE_CMD_LOG" "quit" >/dev/null 2>&1 || true
    fi
    kill "$ACTIVE_QEMU_PID" >/dev/null 2>&1 || true
    wait "$ACTIVE_QEMU_PID" >/dev/null 2>&1 || true
  fi
  if [[ -n "$ACTIVE_MON_SOCK" ]]; then
    rm -f "$ACTIVE_MON_SOCK"
  fi
}

trap cleanup_active_qemu EXIT

need_cmd() {
  local c="$1"
  if ! command -v "$c" >/dev/null 2>&1; then
    mark_fail "CMD_${c}" "missing command: $c"
  fi
}

pick_qemu() {
  if [[ -n "${QEMU_BIN:-}" ]]; then
    echo "$QEMU_BIN"
    return 0
  fi
  if command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "qemu-system-i386"
    return 0
  fi
  if command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "qemu-system-x86_64"
    return 0
  fi
  return 1
}

file_size() {
  local file="$1"
  if [[ -f "$file" ]]; then
    wc -c < "$file"
  else
    echo 0
  fi
}

wait_for_socket() {
  local sock="$1"
  local timeout_sec="$2"
  local start now
  start="$(date +%s)"
  while true; do
    if [[ -S "$sock" ]]; then
      return 0
    fi
    now="$(date +%s)"
    if (( now - start >= timeout_sec )); then
      return 1
    fi
  done
}

wait_for_regex_from_offset() {
  local file="$1"
  local pattern="$2"
  local offset="$3"
  local timeout_sec="$4"
  local start now
  start="$(date +%s)"
  while true; do
    if [[ -f "$file" ]] && tail -c "+$((offset + 1))" "$file" 2>/dev/null | grep -Eiq "$pattern"; then
      return 0
    fi
    now="$(date +%s)"
    if (( now - start >= timeout_sec )); then
      return 1
    fi
  done
}

hmp() {
  local sock="$1"
  local cmd_log="$2"
  local cmd="$3"
  local out rc
  echo "[HMP] $cmd" >> "$cmd_log"
  set +e
  out="$(printf '%s\n' "$cmd" | socat - UNIX-CONNECT:"$sock" 2>&1)"
  rc=$?
  set -e
  if [[ -n "$out" ]]; then
    printf '%s\n' "$out" >> "$cmd_log"
  fi
  echo "[HMP_RC] $cmd => $rc" >> "$cmd_log"
  return $rc
}

send_key() {
  local sock="$1"
  local cmd_log="$2"
  local key="$3"
  hmp "$sock" "$cmd_log" "sendkey $key" >/dev/null 2>&1 || return 1
  return 0
}

send_text() {
  local sock="$1"
  local cmd_log="$2"
  local txt="$3"
  local i ch key

  for ((i=0; i<${#txt}; i++)); do
    ch="${txt:i:1}"
    case "$ch" in
      ' ') key="spc" ;;
      '.') key="dot" ;;
      '/') key="slash" ;;
      '\') key="backslash" ;;
      '-') key="minus" ;;
      ':') key="shift-semicolon" ;;
      [A-Z]) key="shift-$(printf '%s' "$ch" | tr 'A-Z' 'a-z')" ;;
      [a-z0-9]) key="$ch" ;;
      *) continue ;;
    esac
    send_key "$sock" "$cmd_log" "$key" || return 1
  done

  return 0
}

send_text_and_enter() {
  local sock="$1"
  local cmd_log="$2"
  local txt="$3"

  send_text "$sock" "$cmd_log" "$txt" || return 1
  send_key "$sock" "$cmd_log" ret || return 1
  return 0
}

wait_for_prompt_from_offset() {
  local offset="$1"
  local timeout_sec="$2"
  local marker="$3"
  if ! wait_for_regex_from_offset "$SERIAL_LOG" "$APPS_PROMPT_PATTERN" "$offset" "$timeout_sec"; then
    mark_fail "$marker" "C:\\APPS prompt did not appear"
  fi
  mark_pass "$marker"
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
      echo "[dos-compat-smoke] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

need_cmd socat
need_cmd strings
need_cmd timeout

if (( DO_BUILD )); then
  echo "[dos-compat-smoke] build step"
  bash scripts/build_full.sh
fi

if [[ ! -f "$IMG" ]]; then
  mark_fail "IMAGE" "missing image: $IMG"
fi

QEMU_CMD="$(pick_qemu || true)"
if [[ -z "$QEMU_CMD" ]]; then
  mark_fail "QEMU" "qemu-system-i386/x86_64 not found"
fi

mkdir -p build/full
rm -f "$SERIAL_LOG" "$STRINGS_LOG" "$STDERR_LOG" "$CMD_LOG" "$META_LOG" "$MON_SOCK"

QEMU_TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-300}"
PROMPT_TIMEOUT_SEC="${DOS_COMPAT_PROMPT_TIMEOUT_SEC:-120}"
APP_TIMEOUT_SEC="${DOS_COMPAT_APP_TIMEOUT_SEC:-120}"
EDITOR_INPUT_LINE="${DOS_COMPAT_EDITOR_INPUT_LINE:-compat smoke}"
CIUKEDIT_COMMAND='run \APPS\CIUKEDIT.COM MATRIX.TXT'
GFXSTAR_COMMAND='gfxstar'
DOSNAV_PAYLOAD='third_party/DOSNavigator/DN.COM'
DOSNAV_DIR_COMMAND='cd \APPS\DOSNAV'
DOSNAV_COMMAND='run DN.COM'
DOSNAV_PRESENT=0
if [[ -f "$DOSNAV_PAYLOAD" ]]; then
  DOSNAV_PRESENT=1
fi

PROMPT_PREFIX='C{1,2}i{1,2}u{1,2}k{1,2}i{1,2}O{1,2}S{1,2}[[:space:]]+'
BS='[\\]'
APPS_PROMPT_PATTERN="${PROMPT_PREFIX}C{1,2}:{1,2}${BS}{1,2}A{1,2}P{2,4}S{1,2}${BS}{1,2}>{1,2}"
DOSNAV_PROMPT_PATTERN="${PROMPT_PREFIX}C{1,2}:{1,2}${BS}{1,2}A{1,2}P{2,4}S{1,2}${BS}{1,2}D{1,2}O{1,2}S{1,2}N{1,2}A{1,2}V{1,2}${BS}{1,2}>{1,2}"
CIUKEDIT_BOOT_PATTERN='\[CIUKEDIT:BOOT\]|\[{1,2}C{1,2}I{1,2}U{1,2}K{1,2}E{1,2}D{1,2}I{1,2}T{1,2}:{1,2}B{1,2}O{2,4}T{1,2}\]{1,2}'
CIUKEDIT_INPUT_PATTERN='Enter[[:space:]]+line>|E{1,2}n{1,2}t{1,2}e{1,2}r{1,2}[[:space:]]+l{1,2}i{1,2}n{1,2}e{1,2}>'
CIUKEDIT_OK_PATTERN='\[CIUKEDIT:OK\]|\[{1,2}C{1,2}I{1,2}U{1,2}K{1,2}E{1,2}D{1,2}I{1,2}T{1,2}:{1,2}O{1,2}K{1,2}\]{1,2}'
GFXSTAR_PASS_PATTERN='\[GFXSTAR-SERIAL\][[:space:]]+PASS|\[{1,2}G{1,2}F{1,2}X{1,2}S{1,2}T{1,2}A{1,2}R{1,2}-{1,2}S{1,2}E{1,2}R{1,2}I{1,2}A{1,2}L{1,2}\]{1,2}[[:space:]]+P{1,2}A{1,2}S{2,4}'
DOSNAV_START_PATTERN='Dos[[:space:]]+Navigator|D{1,2}o{1,2}s{1,2}[[:space:]]+N{1,2}a{1,2}v{1,2}i{1,2}g{1,2}a{1,2}t{1,2}o{1,2}r|RIT[[:space:]]+Research[[:space:]]+Labs|R{1,2}I{1,2}T{1,2}[[:space:]]+R{1,2}e{1,2}s{1,2}e{1,2}a{1,2}r{1,2}c{1,2}h{1,2}[[:space:]]+L{1,2}a{1,2}b{1,2}s{1,2}'

QEMU_ARGS=(
  -machine pc,vmport=off
  -cpu pentium3
  -m 128
  -drive "file=$IMG,format=raw,if=ide"
  -boot c
  -nographic
  -chardev "file,id=ser0,path=$SERIAL_LOG"
  -serial chardev:ser0
  -monitor "unix:$MON_SOCK,server,nowait"
  -no-reboot
  -no-shutdown
)

set +e
timeout "$QEMU_TIMEOUT_SEC" "$QEMU_CMD" "${QEMU_ARGS[@]}" >/dev/null 2>"$STDERR_LOG" &
QEMU_PID=$!
set -e

ACTIVE_QEMU_PID=$QEMU_PID
ACTIVE_MON_SOCK="$MON_SOCK"
ACTIVE_CMD_LOG="$CMD_LOG"

if ! wait_for_socket "$MON_SOCK" 20; then
  mark_fail "MONITOR_SOCKET_READY" "monitor socket not ready"
fi
mark_pass "MONITOR_SOCKET_READY"

if ! kill -0 "$QEMU_PID" >/dev/null 2>&1; then
  mark_fail "QEMU_EARLY_EXIT" "qemu exited before shell prompt"
fi

wait_for_prompt_from_offset 0 "$PROMPT_TIMEOUT_SEC" "INITIAL_PROMPT_DETECTED"

CIUKEDIT_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$CIUKEDIT_COMMAND" || mark_fail "SEND_CIUKEDIT_COMMAND" "cannot send CIUKEDIT command"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$CIUKEDIT_BOOT_PATTERN" "$CIUKEDIT_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "CIUKEDIT_BOOT" "missing [CIUKEDIT:BOOT] marker"
fi
mark_pass "CIUKEDIT_BOOT"

if ! wait_for_regex_from_offset "$SERIAL_LOG" "$CIUKEDIT_INPUT_PATTERN" "$CIUKEDIT_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "CIUKEDIT_INPUT_PROMPT" "missing Enter line> prompt"
fi
mark_pass "CIUKEDIT_INPUT_PROMPT"

CIUKEDIT_INPUT_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$EDITOR_INPUT_LINE" || mark_fail "SEND_CIUKEDIT_INPUT" "cannot send CIUKEDIT input line"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$CIUKEDIT_OK_PATTERN" "$CIUKEDIT_INPUT_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "CIUKEDIT_OK" "missing [CIUKEDIT:OK] marker"
fi
mark_pass "CIUKEDIT_OK"

wait_for_prompt_from_offset "$CIUKEDIT_INPUT_OFFSET" "$APP_TIMEOUT_SEC" "CIUKEDIT_PROMPT_RETURNED"

GFXSTAR_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$GFXSTAR_COMMAND" || mark_fail "SEND_GFXSTAR_BUILTIN_COMMAND" "cannot send built-in GFXSTAR command"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$GFXSTAR_PASS_PATTERN" "$GFXSTAR_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "GFXSTAR_PASS" "missing [GFXSTAR-SERIAL] PASS marker"
fi
mark_pass "GFXSTAR_PASS"

wait_for_prompt_from_offset "$GFXSTAR_OFFSET" "$APP_TIMEOUT_SEC" "GFXSTAR_PROMPT_RETURNED"

if (( DOSNAV_PRESENT )); then
  DOSNAV_CD_OFFSET="$(file_size "$SERIAL_LOG")"
  send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$DOSNAV_DIR_COMMAND" || mark_fail "SEND_DOSNAV_CD_COMMAND" "cannot send DOSNavigator directory change command"
  if ! wait_for_regex_from_offset "$SERIAL_LOG" "$DOSNAV_PROMPT_PATTERN" "$DOSNAV_CD_OFFSET" "$APP_TIMEOUT_SEC"; then
    mark_fail "DOSNAV_PROMPT_CHANGED" "C:\APPS\DOSNAV prompt did not appear"
  fi
  mark_pass "DOSNAV_PROMPT_CHANGED"

  DOSNAV_RUN_OFFSET="$(file_size "$SERIAL_LOG")"
  send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$DOSNAV_COMMAND" || mark_fail "SEND_DOSNAV_COMMAND" "cannot send DOSNavigator launch command"
  if ! wait_for_regex_from_offset "$SERIAL_LOG" "$DOSNAV_START_PATTERN" "$DOSNAV_RUN_OFFSET" "$APP_TIMEOUT_SEC"; then
    mark_fail "DOSNAV_START" "missing DOSNavigator startup banner marker"
  fi
  mark_pass "DOSNAV_START"
else
  echo "[dos-compat-smoke] PASS DOSNAV_SKIP: payload not present at $DOSNAV_PAYLOAD"
fi

hmp "$MON_SOCK" "$CMD_LOG" "quit" >/dev/null 2>&1 || true
set +e
wait "$QEMU_PID"
QEMU_RC=$?
set -e
if [[ "$QEMU_RC" -ne 0 ]]; then
  mark_fail "QEMU_EXIT" "unexpected qemu exit code: $QEMU_RC"
fi
ACTIVE_QEMU_PID=0
ACTIVE_MON_SOCK=""
ACTIVE_CMD_LOG=""
rm -f "$MON_SOCK"

strings -a "$SERIAL_LOG" > "$STRINGS_LOG" || true

if ! grep -Eiq "$CIUKEDIT_BOOT_PATTERN" "$STRINGS_LOG"; then
  mark_fail "STRINGS_CIUKEDIT_BOOT" "CIUKEDIT boot marker missing in strings log"
fi
if ! grep -Eiq "$CIUKEDIT_OK_PATTERN" "$STRINGS_LOG"; then
  mark_fail "STRINGS_CIUKEDIT_OK" "CIUKEDIT ok marker missing in strings log"
fi
if ! grep -Eiq "$GFXSTAR_PASS_PATTERN" "$STRINGS_LOG"; then
  mark_fail "STRINGS_GFXSTAR_PASS" "GFXSTAR pass marker missing in strings log"
fi
if (( DOSNAV_PRESENT )); then
  if ! grep -Eiq "$DOSNAV_START_PATTERN" "$STRINGS_LOG"; then
    mark_fail "STRINGS_DOSNAV_START" "DOSNavigator startup marker missing in strings log"
  fi
fi

{
  echo "QEMU_CMD=$QEMU_CMD"
  echo "QEMU_RC=$QEMU_RC"
  echo "IMAGE=$IMG"
  echo "SERIAL_LOG=$SERIAL_LOG"
  echo "STRINGS_LOG=$STRINGS_LOG"
  echo "STDERR_LOG=$STDERR_LOG"
  echo "COMMAND_LOG=$CMD_LOG"
  echo "PROMPT_TIMEOUT_SEC=$PROMPT_TIMEOUT_SEC"
  echo "APP_TIMEOUT_SEC=$APP_TIMEOUT_SEC"
  echo "QEMU_TIMEOUT_SEC=$QEMU_TIMEOUT_SEC"
  echo "EDITOR_INPUT_LINE=$EDITOR_INPUT_LINE"
  echo "CIUKEDIT_COMMAND=$CIUKEDIT_COMMAND"
  echo "GFXSTAR_COMMAND=$GFXSTAR_COMMAND"
  echo "DOSNAV_PAYLOAD=$DOSNAV_PAYLOAD"
  echo "DOSNAV_PRESENT=$DOSNAV_PRESENT"
  echo "DOSNAV_DIR_COMMAND=$DOSNAV_DIR_COMMAND"
  echo "DOSNAV_COMMAND=$DOSNAV_COMMAND"
  echo "VALIDATION_FLOW=ciukedit_run_with_args_then_gfxstar_builtin_then_dosnavigator_launch_if_present"
} > "$META_LOG"

if (( DOSNAV_PRESENT )); then
  echo "[dos-compat-smoke] PASS (CIUKEDIT via run-with-args, GFXSTAR via built-in command, and DOSNavigator launch verified)"
else
  echo "[dos-compat-smoke] PASS (CIUKEDIT via run-with-args and GFXSTAR via built-in command verified; DOSNavigator skipped because payload is absent)"
fi