#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
BASE_IMG="${CUTEMOUSE_BASE_IMG:-build/full/ciukios-full.img}"
TEST_IMG="${CUTEMOUSE_TEST_IMG:-build/full/ciukios-full-cutemouse-test.img}"
CTMOUSE_BIN="${CUTEMOUSE_BIN:-third_party/ctmouse/ctmouse.exe}"
CTMOUSE_LICENSE="${CUTEMOUSE_LICENSE:-third_party/ctmouse/copying}"
SERIAL_NORMALIZER="$ROOT_DIR/scripts/serial_log_normalize.py"
PREFIX="${CUTEMOUSE_TEST_PREFIX:-build/full/qemu-full-cutemouse}"
SERIAL_LOG="${PREFIX}.serial.log"
NORMALIZED_LOG="${PREFIX}.normalized.log"
STRINGS_LOG="${PREFIX}.strings.log"
STDERR_LOG="${PREFIX}.stderr.log"
CMD_LOG="${PREFIX}.commands.log"
META_LOG="${PREFIX}.meta"
INJECTED_COPY="${PREFIX}.injected.exe"
MON_SOCK="${CUTEMOUSE_MON_SOCK:-/tmp/ciukios-full-cutemouse.$$.monitor.sock}"
QEMU_TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-180}"
PROMPT_TIMEOUT_SEC="${CUTEMOUSE_PROMPT_TIMEOUT_SEC:-90}"
COMMAND_TIMEOUT_SEC="${CUTEMOUSE_COMMAND_TIMEOUT_SEC:-45}"
KEY_DELAY_SEC="${CUTEMOUSE_KEY_DELAY_SEC:-0.12}"

ACTIVE_QEMU_PID=0
ACTIVE_MON_SOCK=""
ACTIVE_CMD_LOG=""
BASE_HASH_BEFORE=""

usage() {
  cat <<'TXT'
Usage: scripts/qemu_test_full_cutemouse.sh [--no-build]

Creates an isolated copy of the full image, injects the locally vendored
GPL CuteMouse binary, then validates this external DOS workflow:
  1. CTMOUSE.EXE /N/P installs a new TSR and returns to the shell.
  2. MOUSE.COM calls INT 33h and observes the CuteMouse 7.05 signature.
  3. CTMOUSE.EXE /U unloads the TSR and returns to the shell.
  4. MOUSE.COM confirms that the previous INT 33h service was restored.

The base image is hashed before and after the lane and is never attached to
QEMU. Raw serial evidence is preserved; all marker matching uses
scripts/serial_log_normalize.py.
TXT
}

base_image_unchanged() {
  local current_hash

  [[ -n "$BASE_HASH_BEFORE" ]] || return 0
  [[ -f "$BASE_IMG" && ! -L "$BASE_IMG" ]] || return 1
  current_hash="$(sha256sum "$BASE_IMG" | awk '{print $1}')"
  [[ "$current_hash" == "$BASE_HASH_BEFORE" ]]
}

mark_fail() {
  local marker="$1"
  local detail="$2"

  echo "[cutemouse] FAIL ${marker}: ${detail}" >&2
  if ! base_image_unchanged; then
    echo "[cutemouse] FAIL BASE_IMAGE_MUTATED: $BASE_IMG changed during the lane" >&2
  fi
  if [[ -s "$STDERR_LOG" ]]; then
    tail -n 40 "$STDERR_LOG" >&2 || true
  fi
  if [[ -s "$SERIAL_LOG" ]]; then
    if [[ -x "$SERIAL_NORMALIZER" ]]; then
      "$SERIAL_NORMALIZER" "$SERIAL_LOG" | tail -n 160 >&2 || true
    else
      tail -n 160 "$SERIAL_LOG" >&2 || true
    fi
  fi
  exit 1
}

mark_pass() {
  echo "[cutemouse] PASS $1"
}

cleanup_active_qemu() {
  if [[ "$ACTIVE_QEMU_PID" -ne 0 ]] && kill -0 "$ACTIVE_QEMU_PID" >/dev/null 2>&1; then
    if [[ -n "$ACTIVE_MON_SOCK" && -S "$ACTIVE_MON_SOCK" && -n "$ACTIVE_CMD_LOG" ]]; then
      hmp "$ACTIVE_MON_SOCK" "$ACTIVE_CMD_LOG" quit >/dev/null 2>&1 || true
    fi
    kill "$ACTIVE_QEMU_PID" >/dev/null 2>&1 || true
    wait "$ACTIVE_QEMU_PID" >/dev/null 2>&1 || true
  fi
  [[ -n "$ACTIVE_MON_SOCK" ]] && rm -f "$ACTIVE_MON_SOCK"
  return 0
}

trap cleanup_active_qemu EXIT

need_cmd() {
  local command_name="$1"
  if ! command -v "$command_name" >/dev/null 2>&1; then
    mark_fail "COMMAND_${command_name}" "missing command: $command_name"
  fi
}

pick_qemu() {
  if [[ -n "${QEMU_BIN:-}" ]]; then
    echo "$QEMU_BIN"
    return 0
  fi
  if command -v qemu-system-i386 >/dev/null 2>&1; then
    echo qemu-system-i386
    return 0
  fi
  if command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo qemu-system-x86_64
    return 0
  fi
  return 1
}

file_size() {
  if [[ -f "$1" ]]; then
    wc -c < "$1"
  else
    echo 0
  fi
}

normalized_from_offset() {
  local file="$1"
  local offset="$2"

  [[ -f "$file" ]] || return 0
  "$SERIAL_NORMALIZER" --offset "$offset" "$file"
}

wait_for_regex_from_offset() {
  local file="$1"
  local pattern="$2"
  local offset="$3"
  local timeout_sec="$4"
  local start now

  start="$(date +%s)"
  while true; do
    if normalized_from_offset "$file" "$offset" | grep -aEiq -- "$pattern"; then
      return 0
    fi
    now="$(date +%s)"
    if (( now - start >= timeout_sec )); then
      return 1
    fi
    sleep 0.05
  done
}

wait_for_socket() {
  local socket_path="$1"
  local timeout_sec="$2"
  local start now

  start="$(date +%s)"
  while true; do
    [[ -S "$socket_path" ]] && return 0
    now="$(date +%s)"
    if (( now - start >= timeout_sec )); then
      return 1
    fi
    sleep 0.05
  done
}

hmp() {
  local socket_path="$1"
  local command_log="$2"
  local command_text="$3"
  local output rc

  echo "[HMP] $command_text" >> "$command_log"
  set +e
  output="$(printf '%s\n' "$command_text" | socat - UNIX-CONNECT:"$socket_path" 2>&1)"
  rc=$?
  set -e
  [[ -n "$output" ]] && printf '%s\n' "$output" >> "$command_log"
  echo "[HMP_RC] $command_text => $rc" >> "$command_log"
  return "$rc"
}

send_key() {
  local key="$1"

  hmp "$MON_SOCK" "$CMD_LOG" "sendkey $key" >/dev/null 2>&1 || return 1
  if [[ "$KEY_DELAY_SEC" != "0" ]]; then
    sleep "$KEY_DELAY_SEC"
  fi
}

send_text() {
  local text="$1"
  local index character key

  for ((index=0; index<${#text}; index++)); do
    character="${text:index:1}"
    case "$character" in
      ' ') key=spc ;;
      '.') key=dot ;;
      '/') key=slash ;;
      '\') key=backslash ;;
      '-') key=minus ;;
      ':') key=shift-semicolon ;;
      [A-Z]) key="shift-$(printf '%s' "$character" | tr 'A-Z' 'a-z')" ;;
      [a-z0-9]) key="$character" ;;
      *) return 1 ;;
    esac
    send_key "$key" || return 1
  done
}

send_text_and_enter() {
  send_text "$1" || return 1
  send_key ret
}

send_and_wait_for_prompt() {
  local command_text="$1"
  local marker="$2"
  local offset

  offset="$(file_size "$SERIAL_LOG")"
  send_text_and_enter "$command_text" \
    || mark_fail "SEND_${marker}" "cannot send command: $command_text"
  wait_for_regex_from_offset "$SERIAL_LOG" "$APPS_PROMPT_PATTERN" "$offset" "$COMMAND_TIMEOUT_SEC" \
    || mark_fail "$marker" "shell prompt did not return after: $command_text"
  mark_pass "$marker"
}

send_and_wait_for_pattern_and_prompt() {
  local command_text="$1"
  local output_pattern="$2"
  local marker="$3"
  local offset

  offset="$(file_size "$SERIAL_LOG")"
  send_text_and_enter "$command_text" \
    || mark_fail "SEND_${marker}" "cannot send command: $command_text"
  wait_for_regex_from_offset "$SERIAL_LOG" "$output_pattern" "$offset" "$COMMAND_TIMEOUT_SEC" \
    || mark_fail "$marker" "expected output missing after: $command_text"
  wait_for_regex_from_offset "$SERIAL_LOG" "$APPS_PROMPT_PATTERN" "$offset" "$COMMAND_TIMEOUT_SEC" \
    || mark_fail "$marker" "shell prompt did not return after: $command_text"
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
      echo "[cutemouse] ERROR unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

case "$TEST_IMG" in
  build/full/*cutemouse*.img)
    if [[ "${TEST_IMG#build/full/}" == */* ]]; then
      echo "[cutemouse] ERROR unsafe nested CUTEMOUSE_TEST_IMG: $TEST_IMG" >&2
      exit 1
    fi
    ;;
  *)
    echo "[cutemouse] ERROR unsafe CUTEMOUSE_TEST_IMG: $TEST_IMG" >&2
    exit 1
    ;;
esac
if [[ "$TEST_IMG" == "$BASE_IMG" ]]; then
  echo "[cutemouse] ERROR fixture image must differ from base image" >&2
  exit 1
fi
case "$PREFIX" in
  build/full/*cutemouse*)
    if [[ "${PREFIX#build/full/}" == */* ]]; then
      echo "[cutemouse] ERROR unsafe nested CUTEMOUSE_TEST_PREFIX: $PREFIX" >&2
      exit 1
    fi
    ;;
  *)
    echo "[cutemouse] ERROR unsafe CUTEMOUSE_TEST_PREFIX: $PREFIX" >&2
    exit 1
    ;;
esac
case "$MON_SOCK" in
  /tmp/ciukios-full-cutemouse.*.monitor.sock)
    if [[ "${MON_SOCK#/tmp/}" == */* ]]; then
      echo "[cutemouse] ERROR unsafe nested CUTEMOUSE_MON_SOCK: $MON_SOCK" >&2
      exit 1
    fi
    ;;
  *)
    echo "[cutemouse] ERROR unsafe CUTEMOUSE_MON_SOCK: $MON_SOCK" >&2
    exit 1
    ;;
esac

for command_name in awk cmp cp grep mcopy mdir sha256sum socat strings timeout tr wc; do
  need_cmd "$command_name"
done
[[ -x "$SERIAL_NORMALIZER" ]] \
  || mark_fail "SERIAL_NORMALIZER" "missing executable: $SERIAL_NORMALIZER"
[[ -f "$CTMOUSE_BIN" ]] \
  || mark_fail "CTMOUSE_BINARY" "missing local payload: $CTMOUSE_BIN"
[[ -f "$CTMOUSE_LICENSE" ]] \
  || mark_fail "CTMOUSE_LICENSE" "missing local license: $CTMOUSE_LICENSE"
grep -F 'GNU GENERAL PUBLIC LICENSE' "$CTMOUSE_LICENSE" >/dev/null \
  || mark_fail "CTMOUSE_LICENSE" "expected GPL text missing from: $CTMOUSE_LICENSE"
strings -a "$CTMOUSE_BIN" | grep -F '  /N' >/dev/null \
  || mark_fail "CTMOUSE_INSTALL_SUPPORT" "local binary does not advertise /N"
strings -a "$CTMOUSE_BIN" | grep -F '  /P' >/dev/null \
  || mark_fail "CTMOUSE_PS2_SUPPORT" "local binary does not advertise /P"
strings -a "$CTMOUSE_BIN" | grep -F '  /U' >/dev/null \
  || mark_fail "CTMOUSE_UNLOAD_SUPPORT" "local binary does not advertise /U"

echo "[cutemouse] payload=$CTMOUSE_BIN"
echo "[cutemouse] license=$CTMOUSE_LICENSE (GPL-2.0-or-later)"

if (( DO_BUILD )); then
  echo "[cutemouse] build step"
  bash scripts/build_full.sh
fi
[[ -f "$BASE_IMG" ]] || mark_fail "BASE_IMAGE" "missing full image: $BASE_IMG"

mkdir -p "$(dirname "$TEST_IMG")" "$(dirname "$PREFIX")"
BASE_HASH_BEFORE="$(sha256sum "$BASE_IMG" | awk '{print $1}')"
rm -f "$TEST_IMG" "$SERIAL_LOG" "$NORMALIZED_LOG" "$STRINGS_LOG" \
  "$STDERR_LOG" "$CMD_LOG" "$META_LOG" "$INJECTED_COPY" "$MON_SOCK"

cp --reflink=auto --sparse=always "$BASE_IMG" "$TEST_IMG"
mcopy -o -i "$TEST_IMG" "$CTMOUSE_BIN" ::APPS/CTMOUSE.EXE \
  || mark_fail "PAYLOAD_INJECT" "cannot inject CTMOUSE.EXE into fixture"
mdir -i "$TEST_IMG" ::APPS/CTMOUSE.EXE >/dev/null 2>&1 \
  || mark_fail "PAYLOAD_DIRECTORY" "CTMOUSE.EXE missing from fixture directory"
mdir -i "$TEST_IMG" ::SYSTEM/MOUSE.COM >/dev/null 2>&1 \
  || mark_fail "INT33_PROBE" "SYSTEM/MOUSE.COM missing from full-image fixture"
mcopy -o -i "$TEST_IMG" ::APPS/CTMOUSE.EXE "$INJECTED_COPY" \
  || mark_fail "PAYLOAD_READBACK" "cannot extract injected CTMOUSE.EXE"
cmp -s "$CTMOUSE_BIN" "$INJECTED_COPY" \
  || mark_fail "PAYLOAD_READBACK" "injected binary differs from local source"
base_image_unchanged \
  || mark_fail "BASE_IMAGE_MUTATED" "base image changed during fixture injection"
mark_pass "FIXTURE_ISOLATED"
mark_pass "PAYLOAD_INJECTED"

QEMU_CMD="$(pick_qemu || true)"
[[ -n "$QEMU_CMD" ]] || mark_fail "QEMU" "qemu-system-i386/x86_64 not found"

INITIAL_PROMPT_PATTERN='CiukiOS[[:space:]]+SHELL[[:space:]]+C:'
APPS_PROMPT_PATTERN='CiukiOS[[:space:]]+SHELL[[:space:]]+C:\\APPS>'
INSTALL_PATTERN='Installed[[:space:]]+at[[:space:]]+PS/2[[:space:]]+port'
INT33_READY_PATTERN='mouse:[[:space:]]+int33h[[:space:]]+ready'
# CuteMouse advertises Microsoft-driver version 7.05, PS/2 type 4 and IRQ 0
# through the standard INT 33h/AX=0024h register contract.
CUTEMOUSE_INFO_PATTERN='info[[:space:]]+ver=0x0705[[:space:]]+type=0x0004[[:space:]]+irq=0x0000[[:space:]]+status=0x0024'
UNLOAD_PATTERN='Driver[[:space:]]+successfully[[:space:]]+unloaded'
FULL_INFO_PATTERN='info[[:space:]]+ver=0x[0-9A-Fa-f]{4}[[:space:]]+type=0x[0-9A-Fa-f]{4}[[:space:]]+irq=0x[0-9A-Fa-f]{4}[[:space:]]+status=0x[0-9A-Fa-f]{4}'
CTMOUSE_FAILURE_PATTERN='Mouse services already present|Error:[[:space:]]+device not found|CuteMouse driver is not installed|Driver unload failed|Enter /\? on command line for help'

QEMU_ARGS=(
  -machine pc,vmport=off
  -cpu pentium3
  -m 128
  -drive "file=$TEST_IMG,format=raw,if=ide"
  -boot c
  -nographic
  -chardev "file,id=ser0,path=$SERIAL_LOG"
  -serial chardev:ser0
  -monitor "unix:$MON_SOCK,server,nowait"
  -no-reboot
  -no-shutdown
)

set +e
timeout "$QEMU_TIMEOUT_SEC" "$QEMU_CMD" "${QEMU_ARGS[@]}" \
  >/dev/null 2>"$STDERR_LOG" &
QEMU_PID=$!
set -e
ACTIVE_QEMU_PID="$QEMU_PID"
ACTIVE_MON_SOCK="$MON_SOCK"
ACTIVE_CMD_LOG="$CMD_LOG"

wait_for_socket "$MON_SOCK" 20 \
  || mark_fail "MONITOR_SOCKET" "QEMU monitor socket did not appear"
mark_pass "MONITOR_SOCKET"
kill -0 "$QEMU_PID" >/dev/null 2>&1 \
  || mark_fail "QEMU_EARLY_EXIT" "QEMU exited before the shell prompt"
wait_for_regex_from_offset "$SERIAL_LOG" "$INITIAL_PROMPT_PATTERN" 0 "$PROMPT_TIMEOUT_SEC" \
  || mark_fail "INITIAL_PROMPT" "external shell prompt did not appear"
mark_pass "INITIAL_PROMPT"

send_and_wait_for_prompt 'cd \APPS' "CD_APPS"

send_and_wait_for_pattern_and_prompt 'MOUSE' "$INT33_READY_PATTERN" "BASELINE_INT33_READY"
BASELINE_INFO_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter 'MOUSE INFO' \
  || mark_fail "SEND_BASELINE_INFO" "cannot send baseline MOUSE INFO"
wait_for_regex_from_offset "$SERIAL_LOG" "$FULL_INFO_PATTERN" "$BASELINE_INFO_OFFSET" "$COMMAND_TIMEOUT_SEC" \
  || mark_fail "BASELINE_INFO" "runtime INT 33h service did not emit a complete identity tuple"
wait_for_regex_from_offset "$SERIAL_LOG" "$APPS_PROMPT_PATTERN" "$BASELINE_INFO_OFFSET" "$COMMAND_TIMEOUT_SEC" \
  || mark_fail "BASELINE_PROMPT" "shell prompt did not return after baseline MOUSE INFO"
BASELINE_INFO_LINE="$(normalized_from_offset "$SERIAL_LOG" "$BASELINE_INFO_OFFSET" \
  | grep -aEio -- "$FULL_INFO_PATTERN" | tail -n 1 | tr '[:lower:]' '[:upper:]' || true)"
[[ -n "$BASELINE_INFO_LINE" ]] \
  || mark_fail "BASELINE_CAPTURE" "cannot capture baseline INT 33h identity tuple"
mark_pass "BASELINE_HANDLER_CAPTURED"

send_and_wait_for_pattern_and_prompt 'run CTMOUSE.EXE /N/P' "$INSTALL_PATTERN" "INSTALL_N_P"
send_and_wait_for_pattern_and_prompt 'MOUSE' "$INT33_READY_PATTERN" "INT33_READY"
send_and_wait_for_pattern_and_prompt 'MOUSE INFO' "$CUTEMOUSE_INFO_PATTERN" "INT33_CUTEMOUSE_705"
send_and_wait_for_pattern_and_prompt 'run CTMOUSE.EXE /U' "$UNLOAD_PATTERN" "UNLOAD_U"
send_and_wait_for_pattern_and_prompt 'MOUSE' "$INT33_READY_PATTERN" "INT33_RESTORED"

POST_UNLOAD_INFO_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter 'MOUSE INFO' \
  || mark_fail "SEND_POST_UNLOAD_INFO" "cannot send MOUSE INFO after unload"
wait_for_regex_from_offset "$SERIAL_LOG" "$FULL_INFO_PATTERN" "$POST_UNLOAD_INFO_OFFSET" "$COMMAND_TIMEOUT_SEC" \
  || mark_fail "POST_UNLOAD_INFO" "restored INT 33h service did not emit a complete identity tuple"
wait_for_regex_from_offset "$SERIAL_LOG" "$APPS_PROMPT_PATTERN" "$POST_UNLOAD_INFO_OFFSET" "$COMMAND_TIMEOUT_SEC" \
  || mark_fail "POST_UNLOAD_PROMPT" "shell prompt did not return after unload verification"
POST_UNLOAD_INFO_LINE="$(normalized_from_offset "$SERIAL_LOG" "$POST_UNLOAD_INFO_OFFSET" \
  | grep -aEio -- "$FULL_INFO_PATTERN" | tail -n 1 | tr '[:lower:]' '[:upper:]' || true)"
[[ -n "$POST_UNLOAD_INFO_LINE" ]] \
  || mark_fail "POST_UNLOAD_CAPTURE" "cannot capture restored INT 33h identity tuple"
[[ "$POST_UNLOAD_INFO_LINE" == "$BASELINE_INFO_LINE" ]] \
  || mark_fail "POST_UNLOAD_HANDLER" "restored INT 33h tuple differs from baseline: baseline='$BASELINE_INFO_LINE' restored='$POST_UNLOAD_INFO_LINE'"
mark_pass "POST_UNLOAD_HANDLER_RESTORED"
mark_pass "PROMPT_RETURN"

if normalized_from_offset "$SERIAL_LOG" 0 | grep -aEiq -- "$CTMOUSE_FAILURE_PATTERN"; then
  mark_fail "CTMOUSE_FAILURE_OUTPUT" "CuteMouse emitted a failure diagnostic"
fi

hmp "$MON_SOCK" "$CMD_LOG" quit >/dev/null 2>&1 \
  || mark_fail "QEMU_QUIT" "cannot request controlled QEMU shutdown"
set +e
wait "$QEMU_PID"
QEMU_RC=$?
set -e
ACTIVE_QEMU_PID=0
ACTIVE_MON_SOCK=""
ACTIVE_CMD_LOG=""
rm -f "$MON_SOCK"
[[ "$QEMU_RC" -eq 0 ]] \
  || mark_fail "QEMU_EXIT" "unexpected QEMU exit code: $QEMU_RC"

"$SERIAL_NORMALIZER" "$SERIAL_LOG" > "$NORMALIZED_LOG"
strings -a "$NORMALIZED_LOG" > "$STRINGS_LOG" || true
base_image_unchanged \
  || mark_fail "BASE_IMAGE_MUTATED" "base image changed while QEMU used the fixture"
mark_pass "BASE_IMAGE_UNCHANGED"

{
  echo "PAYLOAD=$CTMOUSE_BIN"
  echo "PAYLOAD_SHA256=$(sha256sum "$CTMOUSE_BIN" | awk '{print $1}')"
  echo "LICENSE=$CTMOUSE_LICENSE"
  echo "LICENSE_ID=GPL-2.0-or-later"
  echo "BASE_IMAGE=$BASE_IMG"
  echo "BASE_IMAGE_SHA256=$BASE_HASH_BEFORE"
  echo "TEST_IMAGE=$TEST_IMG"
  echo "SERIAL_LOG=$SERIAL_LOG"
  echo "NORMALIZED_LOG=$NORMALIZED_LOG"
  echo "COMMAND_LOG=$CMD_LOG"
  echo "QEMU=$QEMU_CMD"
  echo "QEMU_RC=$QEMU_RC"
  echo "WORKFLOW=baseline_capture,install_/N_/P,int33_reset,int33_version_7.05,unload_/U,baseline_restore,prompt_return"
} > "$META_LOG"

echo "[cutemouse] PASS external_install_int33_unload"
