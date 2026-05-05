#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD=1
IMG="build/full/ciukios-full.img"
PREFIX="build/full/qemu-full-shell-stability"
SERIAL_LOG="${PREFIX}.serial.log"
STRINGS_LOG="${PREFIX}.strings.log"
STDERR_LOG="${PREFIX}.stderr.log"
CMD_LOG="${PREFIX}.commands.log"
META_LOG="${PREFIX}.meta"
MON_SOCK="/tmp/ciukios-full-shell-stability.monitor.sock"

ACTIVE_QEMU_PID=0
ACTIVE_MON_SOCK=""
ACTIVE_CMD_LOG=""

usage() {
  cat << 'TXT'
Usage: scripts/qemu_test_full_shell_stability.sh [--no-build]

Boots the full profile headlessly and validates shell prompt stability:
  empty command recovery
  invalid command recovery
  long invalid command recovery within the input buffer
  backspace correction via HMP sendkey
  tab key recovery via HMP sendkey
  pwd
  woof \APPS
  woof DOOM
  cd..
  cd \Apps
  run \SYSTEM\DRIVERS\DRVLOAD.COM
  repeated COM/EXE execution loop

Artifacts:
  build/full/qemu-full-shell-stability.{serial.log,strings.log,stderr.log,commands.log,meta}
TXT
}

mark_fail() {
  local marker="$1"
  local detail="$2"
  echo "[shell-stability] FAIL ${marker}: ${detail}" >&2
  if [[ -f "$STDERR_LOG" ]]; then
    tail -n 40 "$STDERR_LOG" >&2 || true
  fi
  if [[ -f "$SERIAL_LOG" ]]; then
    tail -n 160 "$SERIAL_LOG" >&2 || true
  fi
  exit 1
}

mark_pass() {
  local marker="$1"
  echo "[shell-stability] PASS ${marker}"
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
    if [[ -f "$file" ]] && tail -c +"$((offset + 1))" "$file" 2>/dev/null | grep -Eiq "$pattern"; then
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

send_and_wait_for_prompt() {
  local command_text="$1"
  local prompt_pattern="$2"
  local marker="$3"
  local timeout_sec="$4"
  local offset
  offset="$(file_size "$SERIAL_LOG")"
  send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$command_text" || mark_fail "SEND_${marker}" "cannot send command: $command_text"
  if ! wait_for_regex_from_offset "$SERIAL_LOG" "$prompt_pattern" "$offset" "$timeout_sec"; then
    mark_fail "$marker" "expected prompt did not appear after: $command_text"
  fi
  mark_pass "$marker"
}

send_keys_and_wait_for_prompt() {
  local prompt_pattern="$1"
  local marker="$2"
  local timeout_sec="$3"
  shift 3
  local offset key
  offset="$(file_size "$SERIAL_LOG")"
  for key in "$@"; do
    send_key "$MON_SOCK" "$CMD_LOG" "$key" || mark_fail "SEND_${marker}" "cannot send key: $key"
  done
  if ! wait_for_regex_from_offset "$SERIAL_LOG" "$prompt_pattern" "$offset" "$timeout_sec"; then
    mark_fail "$marker" "expected prompt did not appear after key sequence: $*"
  fi
  mark_pass "$marker"
}

send_and_wait_for_pattern_and_prompt() {
  local command_text="$1"
  local pattern="$2"
  local prompt_pattern="$3"
  local marker="$4"
  local timeout_sec="$5"
  local offset
  offset="$(file_size "$SERIAL_LOG")"
  send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$command_text" || mark_fail "SEND_${marker}" "cannot send command: $command_text"
  if ! wait_for_regex_from_offset "$SERIAL_LOG" "$pattern" "$offset" "$timeout_sec"; then
    mark_fail "$marker" "expected marker did not appear after: $command_text"
  fi
  if ! wait_for_regex_from_offset "$SERIAL_LOG" "$prompt_pattern" "$offset" "$timeout_sec"; then
    mark_fail "$marker" "expected prompt did not appear after: $command_text"
  fi
  mark_pass "$marker"
}

send_text_keys_and_wait_for_pattern_and_prompt() {
  local text_before_keys="$1"
  local text_after_keys="$2"
  local pattern="$3"
  local prompt_pattern="$4"
  local marker="$5"
  local timeout_sec="$6"
  shift 6
  local offset key
  offset="$(file_size "$SERIAL_LOG")"
  send_text "$MON_SOCK" "$CMD_LOG" "$text_before_keys" || mark_fail "SEND_${marker}" "cannot send text: $text_before_keys"
  for key in "$@"; do
    send_key "$MON_SOCK" "$CMD_LOG" "$key" || mark_fail "SEND_${marker}" "cannot send key: $key"
  done
  send_text "$MON_SOCK" "$CMD_LOG" "$text_after_keys" || mark_fail "SEND_${marker}" "cannot send text: $text_after_keys"
  send_key "$MON_SOCK" "$CMD_LOG" ret || mark_fail "SEND_${marker}" "cannot send ret"
  if ! wait_for_regex_from_offset "$SERIAL_LOG" "$pattern" "$offset" "$timeout_sec"; then
    mark_fail "$marker" "expected marker did not appear after edited input"
  fi
  if ! wait_for_regex_from_offset "$SERIAL_LOG" "$prompt_pattern" "$offset" "$timeout_sec"; then
    mark_fail "$marker" "expected prompt did not appear after edited input"
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
      echo "[shell-stability] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

need_cmd socat
need_cmd strings
need_cmd timeout

if (( DO_BUILD )); then
  echo "[shell-stability] build step"
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
PROMPT_TIMEOUT_SEC="${SHELL_STABILITY_PROMPT_TIMEOUT_SEC:-120}"
COMMAND_TIMEOUT_SEC="${SHELL_STABILITY_COMMAND_TIMEOUT_SEC:-60}"
DRVLOAD_TIMEOUT_SEC="${SHELL_STABILITY_DRVLOAD_TIMEOUT_SEC:-120}"
STRESS_LOOPS="${SHELL_STABILITY_STRESS_LOOPS:-4}"

PROMPT_PREFIX='C{1,2}i{1,2}u{1,2}k{1,2}i{1,2}O{1,2}S{1,2}[[:space:]]+'
BS='[\\]'
ROOT_PROMPT_PATTERN="${PROMPT_PREFIX}C{1,2}:{1,2}${BS}{1,2}>{1,2}"
ANY_PROMPT_PATTERN="${PROMPT_PREFIX}C{1,2}:{1,2}"
APPS_PROMPT_PATTERN="${PROMPT_PREFIX}C{1,2}:{1,2}${BS}{1,2}A{1,2}P{2,4}S{1,2}${BS}{1,2}>{1,2}"
DOOM_PROMPT_PATTERN="${PROMPT_PREFIX}C{1,2}:{1,2}${BS}{1,2}A{1,2}P{2,4}S{1,2}${BS}{1,2}D{1,2}O{2,4}M{1,2}${BS}{1,2}>{1,2}"
CD_CASE_FAIL_PATTERN='cd[[:space:]]+err=0x|c{1,2}d{1,2}.*e{1,2}r{2,4}={1,2}0{1,2}x{1,2}'
CWD_APPS_PATTERN='cwd=.*APPS|c{1,2}w{1,2}d{1,2}={1,2}.*A{1,2}P{2,4}S{1,2}'
UNKNOWN_COMMAND_PATTERN='Unknown[[:space:]]+command|U{1,2}n{1,2}k{1,2}n{1,2}o{1,2}w{1,2}n{1,2}[[:space:]]+c{1,2}o{1,2}m{2,4}a{1,2}n{1,2}d{1,2}'
COMDEMO_PASS_PATTERN='\[COMDEMO-SERIAL\][[:space:]]+PASS|\[{1,2}C{1,2}O{1,2}M{1,2}D{1,2}E{1,2}M{1,2}O{1,2}-{1,2}S{1,2}E{1,2}R{1,2}I{1,2}A{1,2}L{1,2}\]{1,2}[[:space:]]+P{1,2}A{1,2}S{2,4}'
MZDEMO_PASS_PATTERN='\[MZDEMO-SERIAL\][[:space:]]+PASS|\[{1,2}M{1,2}Z{1,2}D{1,2}E{1,2}M{1,2}O{1,2}-{1,2}S{1,2}E{1,2}R{1,2}I{1,2}A{1,2}L{1,2}\]{1,2}[[:space:]]+P{1,2}A{1,2}S{2,4}'
DRVLOAD_BEGIN_PATTERN='\[DRVLOAD\][[:space:]]+BEGIN|\[\[DDRRVVLLOOAADD\]\][[:space:]]+BBEEGGIIN'
DRVLOAD_DONE_PATTERN='\[DRVLOAD\][[:space:]]+DONE|\[\[DDRRVVLLOOAADD\]\][[:space:]]+DDOONNEE?'
# TODO: Assert FREE footer presence here after the VGA-only footer renderer gains a serial marker suitable for this headless serial harness.

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

if ! wait_for_regex_from_offset "$SERIAL_LOG" "$APPS_PROMPT_PATTERN" 0 "$PROMPT_TIMEOUT_SEC"; then
  mark_fail "INITIAL_PROMPT_DETECTED" "initial C:\APPS shell prompt not detected"
fi
mark_pass "INITIAL_PROMPT_DETECTED"

send_keys_and_wait_for_prompt "$APPS_PROMPT_PATTERN" "EMPTY_COMMAND_PROMPT_RETURNED" "$COMMAND_TIMEOUT_SEC" ret
send_and_wait_for_pattern_and_prompt 'notacommand' "$UNKNOWN_COMMAND_PATTERN" "$APPS_PROMPT_PATTERN" "INVALID_COMMAND_RECOVERY" "$COMMAND_TIMEOUT_SEC"
for ((stress_i=1; stress_i<=STRESS_LOOPS; stress_i++)); do
  send_and_wait_for_pattern_and_prompt "badcmd${stress_i}" "$UNKNOWN_COMMAND_PATTERN" "$APPS_PROMPT_PATTERN" "INVALID_COMMAND_STRESS_${stress_i}" "$COMMAND_TIMEOUT_SEC"
done
send_and_wait_for_pattern_and_prompt 'zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz' "$UNKNOWN_COMMAND_PATTERN" "$APPS_PROMPT_PATTERN" "LONG_INVALID_COMMAND_RECOVERY" "$COMMAND_TIMEOUT_SEC"
send_text_keys_and_wait_for_pattern_and_prompt 'pwz' 'd' "$CWD_APPS_PATTERN" "$APPS_PROMPT_PATTERN" "BACKSPACE_CORRECTION_PWD" "$COMMAND_TIMEOUT_SEC" backspace
send_keys_and_wait_for_prompt "$APPS_PROMPT_PATTERN" "TAB_KEY_RECOVERY" "$COMMAND_TIMEOUT_SEC" tab ret
send_and_wait_for_prompt 'pwd' "$APPS_PROMPT_PATTERN" "PWD_APPS_PROMPT_RETURNED" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_prompt 'woof \APPS' "$APPS_PROMPT_PATTERN" "WOOF_APPS_PROMPT" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_prompt 'woof DOOM' "$DOOM_PROMPT_PATTERN" "WOOF_DOOM_PROMPT" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_prompt 'cd..' "$APPS_PROMPT_PATTERN" "CDDOTDOT_APPS_PROMPT" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_prompt 'cd D:\' "$APPS_PROMPT_PATTERN" "CD_D_ROOT_DEFAULT_C_APPS" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_prompt 'cd D:APPS' "$APPS_PROMPT_PATTERN" "CD_D_REL_DEFAULT_C_APPS" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_prompt 'cd C:\APPS\..' "$ROOT_PROMPT_PATTERN" "CD_C_PARENT_ROOT" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_prompt 'cd C:\APPS' "$APPS_PROMPT_PATTERN" "CD_C_ABS_APPS" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'cd Z:\' "$CD_CASE_FAIL_PATTERN" "$APPS_PROMPT_PATTERN" "CD_BAD_DRIVE_REJECT" "$COMMAND_TIMEOUT_SEC"
for ((stress_i=1; stress_i<=STRESS_LOOPS; stress_i++)); do
  send_and_wait_for_pattern_and_prompt 'comdemo' "$COMDEMO_PASS_PATTERN" "$APPS_PROMPT_PATTERN" "COMDEMO_LOOP_${stress_i}" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'mzdemo' "$MZDEMO_PASS_PATTERN" "$APPS_PROMPT_PATTERN" "MZDEMO_LOOP_${stress_i}" "$COMMAND_TIMEOUT_SEC"
done

CASE_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" 'cd \Apps' || mark_fail "SEND_MIXED_CASE_PATH_REJECT" "cannot send mixed-case cd command"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$CD_CASE_FAIL_PATTERN" "$CASE_OFFSET" "$COMMAND_TIMEOUT_SEC"; then
  mark_fail "MIXED_CASE_PATH_REJECT" "mixed-case path unexpectedly resolved or error marker missing"
fi
mark_pass "MIXED_CASE_PATH_REJECT"

DRVLOAD_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" 'run \SYSTEM\DRIVERS\DRVLOAD.COM' || mark_fail "SEND_DRVLOAD_RETURNED" "cannot send DRVLOAD command"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$DRVLOAD_BEGIN_PATTERN" "$DRVLOAD_OFFSET" "$DRVLOAD_TIMEOUT_SEC"; then
  mark_fail "DRVLOAD_RETURNED" "missing [DRVLOAD] BEGIN marker"
fi
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$DRVLOAD_DONE_PATTERN" "$DRVLOAD_OFFSET" "$DRVLOAD_TIMEOUT_SEC"; then
  mark_fail "DRVLOAD_RETURNED" "missing [DRVLOAD] DONE marker"
fi
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$APPS_PROMPT_PATTERN" "$DRVLOAD_OFFSET" "$DRVLOAD_TIMEOUT_SEC"; then
  mark_fail "DRVLOAD_CWD_RESTORED" "C:\\APPS prompt missing after DRVLOAD returned"
fi
mark_pass "DRVLOAD_RETURNED"
mark_pass "DRVLOAD_CWD_RESTORED"

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

{
  echo "QEMU_CMD=$QEMU_CMD"
  echo "QEMU_RC=$QEMU_RC"
  echo "IMAGE=$IMG"
  echo "SERIAL_LOG=$SERIAL_LOG"
  echo "STRINGS_LOG=$STRINGS_LOG"
  echo "STDERR_LOG=$STDERR_LOG"
  echo "COMMAND_LOG=$CMD_LOG"
  echo "PROMPT_TIMEOUT_SEC=$PROMPT_TIMEOUT_SEC"
  echo "COMMAND_TIMEOUT_SEC=$COMMAND_TIMEOUT_SEC"
  echo "DRVLOAD_TIMEOUT_SEC=$DRVLOAD_TIMEOUT_SEC"
  echo "QEMU_TIMEOUT_SEC=$QEMU_TIMEOUT_SEC"
} > "$META_LOG"

echo "[shell-stability] PASS (shell stability sequence verified)"