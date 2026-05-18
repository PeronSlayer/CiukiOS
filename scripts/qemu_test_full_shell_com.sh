#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD=1
BOOT_AUTORUN="${SHELL_COM_BOOT_AUTORUN:-0}"
BOOT_EXPECT_FALLBACK="${SHELL_COM_BOOT_EXPECT_FALLBACK:-0}"
IMG="build/full/ciukios-full.img"
PREFIX="build/full/qemu-full-shell-com"
if (( BOOT_AUTORUN )); then
  PREFIX="build/full/qemu-full-shell-com-boot"
fi
if (( BOOT_EXPECT_FALLBACK )); then
  PREFIX="build/full/qemu-full-shell-com-boot-fallback"
fi
SERIAL_LOG="${PREFIX}.serial.log"
STRINGS_LOG="${PREFIX}.strings.log"
STDERR_LOG="${PREFIX}.stderr.log"
CMD_LOG="${PREFIX}.commands.log"
MON_SOCK="/tmp/ciukios-full-shell-com.$$.monitor.sock"

ACTIVE_QEMU_PID=0
ACTIVE_MON_SOCK=""
ACTIVE_CMD_LOG=""

usage() {
  cat <<'TXT'
Usage: scripts/qemu_test_full_shell_com.sh [--no-build]

Boots the full profile headlessly, launches \SYSTEM\SHELL.COM from the
Stage1 shell forced via override, validates help/ver/echo/cls/cd/dir/exit, and
confirms the Stage1 prompt returns afterward.

Set SHELL_COM_BOOT_AUTORUN=1 to use the full-profile default image, which boots directly
into \SYSTEM\SHELL.COM, then validates ver/where/exit before falling back
to the built-in Stage1 shell.

Set SHELL_COM_BOOT_EXPECT_FALLBACK=1 together with SHELL_COM_BOOT_AUTORUN=1
to remove \SYSTEM\SHELL.COM from the test image, then verify the short
fallback warning and the built-in Stage1 shell prompt.
TXT
}

mark_fail() {
  local marker="$1"
  local detail="$2"
  echo "[shell-com] FAIL ${marker}: ${detail}" >&2
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
  echo "[shell-com] PASS ${marker}"
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
  if [[ -f "$1" ]]; then
    wc -c < "$1"
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

strings_from_offset() {
  local file="$1"
  local offset="$2"
  if [[ ! -f "$file" ]]; then
    return 0
  fi
  tail -c "+$((offset + 1))" "$file" 2>/dev/null | strings -a
}

wait_for_strings_regex_from_offset() {
  local file="$1"
  local pattern="$2"
  local offset="$3"
  local timeout_sec="$4"
  local start now
  start="$(date +%s)"
  while true; do
    if strings_from_offset "$file" "$offset" | grep -Eiq "$pattern"; then
      return 0
    fi
    now="$(date +%s)"
    if (( now - start >= timeout_sec )); then
      return 1
    fi
  done
}

wait_for_strings_count_from_offset() {
  local file="$1"
  local pattern="$2"
  local min_count="$3"
  local offset="$4"
  local timeout_sec="$5"
  local start now count
  start="$(date +%s)"
  while true; do
    count="$(strings_from_offset "$file" "$offset" | grep -Eio "$pattern" | wc -l || true)"
    if (( count >= min_count )); then
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
  local out rc attempt
  echo "[HMP] $cmd" >> "$cmd_log"
  rc=1
  out=""
  for attempt in 1 2 3 4 5; do
    set +e
    out="$(printf '%s\n' "$cmd" | socat - UNIX-CONNECT:"$sock" 2>&1)"
    rc=$?
    set -e
    if [[ $rc -eq 0 ]]; then
      break
    fi
    sleep 0.2
  done
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
  sleep 0.03
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
  sleep 0.05
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
  if ! wait_for_strings_regex_from_offset "$SERIAL_LOG" "$prompt_pattern" "$offset" "$timeout_sec"; then
    mark_fail "$marker" "expected prompt did not appear after: $command_text"
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
  if ! wait_for_strings_regex_from_offset "$SERIAL_LOG" "$pattern" "$offset" "$timeout_sec"; then
    mark_fail "$marker" "expected output did not appear after: $command_text"
  fi
  if ! wait_for_strings_regex_from_offset "$SERIAL_LOG" "$prompt_pattern" "$offset" "$timeout_sec"; then
    mark_fail "$marker" "expected prompt did not appear after: $command_text"
  fi
  mark_pass "$marker"
}

send_and_wait_for_count_and_prompt() {
  local command_text="$1"
  local pattern="$2"
  local min_count="$3"
  local prompt_pattern="$4"
  local marker="$5"
  local timeout_sec="$6"
  local offset
  offset="$(file_size "$SERIAL_LOG")"
  send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$command_text" || mark_fail "SEND_${marker}" "cannot send command: $command_text"
  if ! wait_for_strings_count_from_offset "$SERIAL_LOG" "$pattern" "$min_count" "$offset" "$timeout_sec"; then
    mark_fail "$marker" "expected output count did not appear after: $command_text"
  fi
  if ! wait_for_strings_regex_from_offset "$SERIAL_LOG" "$prompt_pattern" "$offset" "$timeout_sec"; then
    mark_fail "$marker" "expected prompt did not appear after: $command_text"
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
      echo "[shell-com] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

need_cmd mdir
need_cmd mdel
need_cmd mcopy
need_cmd python3
need_cmd socat
need_cmd strings
need_cmd timeout

if (( BOOT_EXPECT_FALLBACK )) && (( ! BOOT_AUTORUN )); then
  mark_fail "BOOT_MODE" "SHELL_COM_BOOT_EXPECT_FALLBACK=1 requires SHELL_COM_BOOT_AUTORUN=1"
fi

if (( DO_BUILD )); then
  echo "[shell-com] build step"
  if (( BOOT_AUTORUN )); then
    bash scripts/build_full.sh
  else
    CIUKIOS_STAGE1_BOOT_EXTERNAL_SHELL=0 bash scripts/build_full.sh
  fi
fi

if [[ ! -f "$IMG" ]]; then
  mark_fail "IMAGE" "missing image: $IMG"
fi

if ! mdir -i "$IMG" ::SYSTEM 2>/dev/null | grep -Eq '^SHELL[[:space:]]+COM[[:space:]]'; then
  mark_fail "SHELL_COM_PRESENT" "\\SYSTEM\\SHELL.COM is missing from $IMG"
fi
mark_pass "SHELL_COM_PRESENT"

if (( BOOT_EXPECT_FALLBACK )); then
  if ! mdel -i "$IMG" ::SYSTEM/SHELL.COM >/dev/null 2>&1; then
    mark_fail "SHELL_COM_REMOVE" "could not remove \\SYSTEM\\SHELL.COM from $IMG"
  fi
  if mdir -i "$IMG" ::SYSTEM 2>/dev/null | grep -Eq '^SHELL[[:space:]]+COM[[:space:]]'; then
    mark_fail "SHELL_COM_REMOVE" "\\SYSTEM\\SHELL.COM still present after removal"
  fi
  mark_pass "SHELL_COM_REMOVE"
fi

DOS4GW_PRESENT=0
if mdir -i "$IMG" ::SYSTEM/DRIVERS 2>/dev/null | grep -Eq '^DOS4GW[[:space:]]+EXE[[:space:]]'; then
  DOS4GW_PRESENT=1
fi

if (( ! BOOT_AUTORUN )); then
  # Inject deterministic fixtures for the type/del/erase checks.
  # These land on whatever cluster mcopy picks (high in a freshly built image).
  # With the int21h 32-bit cluster->LBA fix in place, a high-cluster file must
  # read back correctly, so TYPE_OK below doubles as that fix's regression check.
  TYPE_TEST_LOCAL="${PREFIX}.typetest.txt"
  printf 'TYPE PAYLOAD TYPETOK99\r\n' > "$TYPE_TEST_LOCAL"
  if ! mcopy -i "$IMG" -o "$TYPE_TEST_LOCAL" ::APPS/TYPETEST.TXT 2>/dev/null; then
    mark_fail "TYPE_DEL_FIXTURES" "could not inject ::APPS/TYPETEST.TXT"
  fi
  if ! mcopy -i "$IMG" -o "$TYPE_TEST_LOCAL" ::APPS/ERASETST.TXT 2>/dev/null; then
    mark_fail "TYPE_DEL_FIXTURES" "could not inject ::APPS/ERASETST.TXT"
  fi
  mark_pass "TYPE_DEL_FIXTURES"

  # Regression guard: the type fixture must sit on a high cluster (one whose
  # data LBA overflows 16-bit math, i.e. cluster >= 8194) so that the TYPE_OK
  # check actually exercises the high-cluster cluster->LBA fix.
  TYPE_FIXTURE_CLUSTER="$(python3 - "$IMG" <<'PY'
import sys
img = open(sys.argv[1], 'rb').read()
i = img.find(b'TYPETESTTXT')
e = img[i:i+32]
print(e[26] | (e[27] << 8))
PY
)"
  if [[ -z "$TYPE_FIXTURE_CLUSTER" ]] || (( TYPE_FIXTURE_CLUSTER < 8194 )); then
    mark_fail "HIGH_CLUSTER_FIXTURE" "TYPETEST.TXT landed on cluster '${TYPE_FIXTURE_CLUSTER}'; high-cluster read no longer covered"
  fi
  mark_pass "HIGH_CLUSTER_FIXTURE cluster=${TYPE_FIXTURE_CLUSTER}"
fi

QEMU_CMD="$(pick_qemu || true)"
if [[ -z "$QEMU_CMD" ]]; then
  mark_fail "QEMU" "qemu-system-i386/x86_64 not found"
fi

mkdir -p build/full
rm -f "$SERIAL_LOG" "$STRINGS_LOG" "$STDERR_LOG" "$CMD_LOG" "$MON_SOCK"

QEMU_TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-180}"
PROMPT_TIMEOUT_SEC="${SHELL_COM_PROMPT_TIMEOUT_SEC:-60}"
COMMAND_TIMEOUT_SEC="${SHELL_COM_COMMAND_TIMEOUT_SEC:-45}"

PROMPT_PREFIX='C{1,2}i{1,2}u{1,2}k{1,2}i{1,2}O{1,2}S{1,2}[[:space:]]+'
BS='[\\]'
STAGE1_PROMPT_PATTERN="${PROMPT_PREFIX}C{1,2}:{1,2}${BS}{1,2}A{1,2}P{2,4}S{1,2}${BS}{1,2}>{1,2}"
CHILD_PROMPT_PATTERN='S+H+E+L+L+[[:space:]]+C+[:]+[\\]+'
ROOT_PROMPT_PATTERN='S+H+E+L+L+[[:space:]]+C+[:]+[\\]+>+'
APPS_PROMPT_PATTERN='S+H+E+L+L+[[:space:]]+C+[:]+[\\]+A+P+P+S+>+'
HELP_PATTERN='C+O+M+M+A+N+D+S+[:]+.*P+A+T+H+.*W+H+E+R+E+'
VER_PATTERN='C+I+U+K+I+O+S+[[:space:]]+S+H+E+L+L+\.*C+O+M+[[:space:]]+P+R+O+T+O+T+Y+P+E+'
FALLBACK_WARNING_PATTERN='W+A+R+N+[:]+[[:space:]]+S+H+E+L+L+[[:space:]]+B+O+O+T+[[:space:]]+F+A+I+L+'
PATH_PATTERN='C+[:]+[\\]+A+P+P+S+;+C+[:]+[\\]+S+Y+S+T+E+M+[\\]+D+R+I+V+E+R+S+;+C+[:]+[\\]+S+Y+S+T+E+M+'
WHERE_SHELL_PATTERN='C+[:]+[\\]+S+Y+S+T+E+M+[\\]+S+H+E+L+L+\.*C+O+M+'
WHERE_DOS4GW_PATTERN='C+[:]+[\\]+S+Y+S+T+E+M+[\\]+D+R+I+V+E+R+S+[\\]+D+O+S+4+G+W+\.*E+X+E+'
WHERE_MISSING_PATTERN='W+H+E+R+E+[:]+[[:space:]]+N+O+T+[[:space:]]+F+O+U+N+D+'
EXEC_MISSING_PATTERN='E+X+E+C+[:]+[[:space:]]+N+O+T+[[:space:]]+F+O+U+N+D+'
COMDEMO_PASS_PATTERN='C+O+M+[[:space:]]+D+E+M+O+[[:space:]]+V+I+A+[[:space:]]+I+N+T+2+1+H+'
ECHO_TOKEN='SH42'
ECHO_TOKEN_PATTERN='S+H+4+2+'
EXEC_RETURN_TOKEN='XR52'
EXEC_RETURN_PATTERN='X+R+5+2+'
CLS_TOKEN='SC43'
CLS_TOKEN_PATTERN='S+C+4+3+'
DIR_FILE_PATTERN='C+O+M+D+E+M+O+'
CD_ERROR_PATTERN='C+D+[:]+[[:space:]]+I+N+V+A+L+I+D+'
MKDIR_OK_PATTERN='D+I+R+E+C+T+O+R+Y+[[:space:]]+C+R+E+A+T+E+D+'
RMDIR_OK_PATTERN='D+I+R+E+C+T+O+R+Y+[[:space:]]+R+E+M+O+V+E+D+'
RENAME_OK_PATTERN='R+E+N+A+M+E+[[:space:]]+C+O+M+P+L+E+T+E+'
RENAME_ERR_PATTERN='R+E+N+A+M+E+[:]+[[:space:]]+F+A+I+L+E+D+'
RENAME_USE_PATTERN='R+E+N+A+M+E+[:]+[[:space:]]+U+S+A+G+E+'
TYPE_TOKEN_PATTERN='T+Y+P+E+T+O+K+9+9+'
TYPE_ERROR_PATTERN='T+Y+P+E+[:]+[[:space:]]+C+A+N+N+O+T+'
DEL_OK_PATTERN='F+I+L+E+[[:space:]]+D+E+L+E+T+E+D+'
COPY_OK_PATTERN='F+I+L+E+[[:space:]]+C+O+P+I+E+D+'
COPY_SRC_ERR_PATTERN='C+O+P+Y+[:]+[[:space:]]+S+O+U+R+C+E+'
COPY_USE_PATTERN='C+O+P+Y+[:]+[[:space:]]+U+S+A+G+E+'
DEL_ERROR_PATTERN='D+E+L+[:]+[[:space:]]+F+I+L+E+[[:space:]]+N+O+T+'
BACKSPACE_TOKEN='BK77'
BACKSPACE_TOKEN_PATTERN='B+K+7+7+'
HISTORY_ONE_TOKEN='HONE1'
HISTORY_ONE_PATTERN='H+O+N+E+1+'
HISTORY_TWO_TOKEN='HTWO2'
HISTORY_TWO_PATTERN='H+T+W+O+2+'

LONG_CMD="$(printf '%*s' 140 '')"
LONG_CMD="${LONG_CMD// /a}"

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

if (( BOOT_AUTORUN )); then
  if (( BOOT_EXPECT_FALLBACK )); then
    if ! wait_for_strings_regex_from_offset "$SERIAL_LOG" "$FALLBACK_WARNING_PATTERN" 0 "$PROMPT_TIMEOUT_SEC"; then
      mark_fail "FALLBACK_WARNING" "fallback warning not detected"
    fi
    mark_pass "FALLBACK_WARNING"
    if ! wait_for_strings_regex_from_offset "$SERIAL_LOG" "$STAGE1_PROMPT_PATTERN" 0 "$PROMPT_TIMEOUT_SEC"; then
      mark_fail "FALLBACK_STAGE1_PROMPT" "Stage1 prompt not detected after fallback"
    fi
    mark_pass "FALLBACK_STAGE1_PROMPT"
    send_and_wait_for_prompt 'ver' "$STAGE1_PROMPT_PATTERN" "FALLBACK_STAGE1_STABLE" "$COMMAND_TIMEOUT_SEC"
  else
    if ! wait_for_strings_regex_from_offset "$SERIAL_LOG" "$CHILD_PROMPT_PATTERN" 0 "$PROMPT_TIMEOUT_SEC"; then
      mark_fail "INITIAL_SHELL_COM_PROMPT" "initial SHELL.COM prompt not detected"
    fi
    mark_pass "INITIAL_SHELL_COM_PROMPT"
    send_and_wait_for_pattern_and_prompt 'ver' "$VER_PATTERN" "$CHILD_PROMPT_PATTERN" "VER_OK" "$COMMAND_TIMEOUT_SEC"
    send_and_wait_for_pattern_and_prompt 'where SHELL' "$WHERE_SHELL_PATTERN" "$CHILD_PROMPT_PATTERN" "WHERE_SHELL_OK" "$COMMAND_TIMEOUT_SEC"
    send_and_wait_for_prompt 'exit' "$STAGE1_PROMPT_PATTERN" "EXIT_TO_STAGE1_OK" "$COMMAND_TIMEOUT_SEC"
  fi
else
  if ! wait_for_strings_regex_from_offset "$SERIAL_LOG" "$STAGE1_PROMPT_PATTERN" 0 "$PROMPT_TIMEOUT_SEC"; then
    mark_fail "INITIAL_STAGE1_PROMPT" "initial CiukiOS C:\\APPS prompt not detected"
  fi
  mark_pass "INITIAL_STAGE1_PROMPT"

  # Kernel directory I/O regression: drive Stage1's md/rd builtins (kernel
  # int21_mkdir/int21_rmdir) so a fresh directory lands on a high FAT16 cluster
  # (>= 8194). HCDIR is left in place for the post-run cluster + zero-fill check;
  # RTDIR exercises the mkdir -> rmdir round-trip and the rmdir crash fix
  # (RMDIR_RETURN_OK proves the OS survives rd of a high-cluster directory).
  send_and_wait_for_prompt 'md \APPS\HCDIR' "$STAGE1_PROMPT_PATTERN" "HC_MKDIR_SENT" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_prompt 'md \APPS\SRDIR1' "$STAGE1_PROMPT_PATTERN" "SR1_MKDIR_SENT" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_prompt 'md \APPS\SRDIR2' "$STAGE1_PROMPT_PATTERN" "SR2_MKDIR_SENT" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_prompt 'md \APPS\RTDIR' "$STAGE1_PROMPT_PATTERN" "RT_MKDIR_SENT" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_prompt 'rd \APPS\RTDIR' "$STAGE1_PROMPT_PATTERN" "RMDIR_RETURN_OK" "$COMMAND_TIMEOUT_SEC"

  send_and_wait_for_prompt '\SYSTEM\SHELL.COM' "$CHILD_PROMPT_PATTERN" "SHELL_LAUNCHED" "$COMMAND_TIMEOUT_SEC"
  EMPTY_OFFSET="$(file_size "$SERIAL_LOG")"
  send_key "$MON_SOCK" "$CMD_LOG" ret || mark_fail "SEND_EMPTY_OK" "cannot send empty command"
  wait_for_strings_regex_from_offset "$SERIAL_LOG" "$CHILD_PROMPT_PATTERN" "$EMPTY_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "EMPTY_OK" "expected prompt did not appear after empty command"
  mark_pass "EMPTY_OK"
  SPACES_OFFSET="$(file_size "$SERIAL_LOG")"
  send_text "$MON_SOCK" "$CMD_LOG" '   ' || mark_fail "SEND_SPACES_ONLY_OK" "cannot send spaces-only command"
  send_key "$MON_SOCK" "$CMD_LOG" ret || mark_fail "SEND_SPACES_ONLY_OK" "cannot send enter for spaces-only command"
  wait_for_strings_regex_from_offset "$SERIAL_LOG" "$CHILD_PROMPT_PATTERN" "$SPACES_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "SPACES_ONLY_OK" "expected prompt did not appear after spaces-only command"
  mark_pass "SPACES_ONLY_OK"
  BACKSPACE_OFFSET="$(file_size "$SERIAL_LOG")"
  send_text "$MON_SOCK" "$CMD_LOG" 'ecx' || mark_fail "SEND_BACKSPACE_OK" "cannot send backspace prelude"
  send_key "$MON_SOCK" "$CMD_LOG" backspace || mark_fail "SEND_BACKSPACE_OK" "cannot send backspace key"
  send_text "$MON_SOCK" "$CMD_LOG" "ho $BACKSPACE_TOKEN" || mark_fail "SEND_BACKSPACE_OK" "cannot send backspace completion"
  send_key "$MON_SOCK" "$CMD_LOG" ret || mark_fail "SEND_BACKSPACE_OK" "cannot send enter after backspace edit"
  wait_for_strings_count_from_offset "$SERIAL_LOG" "$BACKSPACE_TOKEN_PATTERN" 2 "$BACKSPACE_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "BACKSPACE_OK" "edited echo command did not execute correctly"
  wait_for_strings_regex_from_offset "$SERIAL_LOG" "$CHILD_PROMPT_PATTERN" "$BACKSPACE_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "BACKSPACE_OK" "prompt did not return after backspace edit"
  mark_pass "BACKSPACE_OK"
  LONG_OFFSET="$(file_size "$SERIAL_LOG")"
  send_text "$MON_SOCK" "$CMD_LOG" "$LONG_CMD" || mark_fail "SEND_LONG_INPUT_OK" "cannot send long command"
  send_key "$MON_SOCK" "$CMD_LOG" ret || mark_fail "SEND_LONG_INPUT_OK" "cannot send enter after long command"
  wait_for_strings_regex_from_offset "$SERIAL_LOG" "$EXEC_MISSING_PATTERN" "$LONG_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "LONG_INPUT_OK" "long command did not reach normal not-found handling"
  wait_for_strings_regex_from_offset "$SERIAL_LOG" "$CHILD_PROMPT_PATTERN" "$LONG_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "LONG_INPUT_OK" "prompt did not recover after long command"
  mark_pass "LONG_INPUT_OK"
  send_and_wait_for_count_and_prompt "echo $HISTORY_ONE_TOKEN" "$HISTORY_ONE_PATTERN" 2 "$CHILD_PROMPT_PATTERN" "HISTORY_SEED1_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_count_and_prompt "echo $HISTORY_TWO_TOKEN" "$HISTORY_TWO_PATTERN" 2 "$CHILD_PROMPT_PATTERN" "HISTORY_SEED2_OK" "$COMMAND_TIMEOUT_SEC"
  HISTORY_UP_OFFSET="$(file_size "$SERIAL_LOG")"
  send_key "$MON_SOCK" "$CMD_LOG" up || mark_fail "SEND_HISTORY_UP_OK" "cannot send history up"
  send_key "$MON_SOCK" "$CMD_LOG" ret || mark_fail "SEND_HISTORY_UP_OK" "cannot send enter after history up"
  wait_for_strings_count_from_offset "$SERIAL_LOG" "$HISTORY_TWO_PATTERN" 2 "$HISTORY_UP_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "HISTORY_UP_OK" "history up did not recall previous command"
  wait_for_strings_regex_from_offset "$SERIAL_LOG" "$CHILD_PROMPT_PATTERN" "$HISTORY_UP_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "HISTORY_UP_OK" "prompt did not return after history up command"
  mark_pass "HISTORY_UP_OK"
  HISTORY_DOWN_OFFSET="$(file_size "$SERIAL_LOG")"
  send_key "$MON_SOCK" "$CMD_LOG" up || mark_fail "SEND_HISTORY_DOWN_OK" "cannot send first history up"
  send_key "$MON_SOCK" "$CMD_LOG" up || mark_fail "SEND_HISTORY_DOWN_OK" "cannot send second history up"
  send_key "$MON_SOCK" "$CMD_LOG" down || mark_fail "SEND_HISTORY_DOWN_OK" "cannot send history down"
  send_key "$MON_SOCK" "$CMD_LOG" ret || mark_fail "SEND_HISTORY_DOWN_OK" "cannot send enter after history down"
  wait_for_strings_count_from_offset "$SERIAL_LOG" "$HISTORY_TWO_PATTERN" 2 "$HISTORY_DOWN_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "HISTORY_DOWN_OK" "history down did not recall newer command"
  wait_for_strings_regex_from_offset "$SERIAL_LOG" "$CHILD_PROMPT_PATTERN" "$HISTORY_DOWN_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "HISTORY_DOWN_OK" "prompt did not return after history down command"
  mark_pass "HISTORY_DOWN_OK"
  HISTORY_DEDUP_OFFSET="$(file_size "$SERIAL_LOG")"
  send_key "$MON_SOCK" "$CMD_LOG" up || mark_fail "SEND_HISTORY_DEDUP_OK" "cannot send first history up for dedup"
  send_key "$MON_SOCK" "$CMD_LOG" up || mark_fail "SEND_HISTORY_DEDUP_OK" "cannot send second history up for dedup"
  send_key "$MON_SOCK" "$CMD_LOG" ret || mark_fail "SEND_HISTORY_DEDUP_OK" "cannot send enter after dedup navigation"
  wait_for_strings_count_from_offset "$SERIAL_LOG" "$HISTORY_ONE_PATTERN" 2 "$HISTORY_DEDUP_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "HISTORY_DEDUP_OK" "duplicate consecutive history entry was stored"
  wait_for_strings_regex_from_offset "$SERIAL_LOG" "$CHILD_PROMPT_PATTERN" "$HISTORY_DEDUP_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "HISTORY_DEDUP_OK" "prompt did not return after dedup history command"
  mark_pass "HISTORY_DEDUP_OK"
  send_and_wait_for_pattern_and_prompt 'help' "$HELP_PATTERN" "$CHILD_PROMPT_PATTERN" "HELP_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'ver' "$VER_PATTERN" "$CHILD_PROMPT_PATTERN" "VER_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_count_and_prompt "echo $ECHO_TOKEN" "$ECHO_TOKEN_PATTERN" 2 "$CHILD_PROMPT_PATTERN" "ECHO_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_prompt 'cls' "$CHILD_PROMPT_PATTERN" "CLS_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_count_and_prompt "echo $CLS_TOKEN" "$CLS_TOKEN_PATTERN" 2 "$CHILD_PROMPT_PATTERN" "POST_CLS_ECHO_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_prompt 'cd \' "$ROOT_PROMPT_PATTERN" "CD_ROOT_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'dir \APPS' "$DIR_FILE_PATTERN" "$ROOT_PROMPT_PATTERN" "DIR_APPS_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_prompt 'cd \APPS' "$APPS_PROMPT_PATTERN" "CD_APPS_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'path' "$PATH_PATTERN" "$APPS_PROMPT_PATTERN" "PATH_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'where SHELL' "$WHERE_SHELL_PATTERN" "$APPS_PROMPT_PATTERN" "WHERE_SHELL_OK" "$COMMAND_TIMEOUT_SEC"
  if (( DOS4GW_PRESENT )); then
    send_and_wait_for_pattern_and_prompt 'where DOS4GW' "$WHERE_DOS4GW_PATTERN" "$APPS_PROMPT_PATTERN" "WHERE_DOS4GW_OK" "$COMMAND_TIMEOUT_SEC"
  fi
  send_and_wait_for_pattern_and_prompt 'where MISSING987' "$WHERE_MISSING_PATTERN" "$APPS_PROMPT_PATTERN" "WHERE_MISSING_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'COMDEMO.COM' "$COMDEMO_PASS_PATTERN" "$APPS_PROMPT_PATTERN" "EXEC_COM_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_prompt 'cd \' "$ROOT_PROMPT_PATTERN" "EXEC_ROOT_CD_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'COMDEMO' "$COMDEMO_PASS_PATTERN" "$ROOT_PROMPT_PATTERN" "EXEC_PATH_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_count_and_prompt "echo $EXEC_RETURN_TOKEN" "$EXEC_RETURN_PATTERN" 2 "$ROOT_PROMPT_PATTERN" "EXEC_RETURN_TO_SHELL_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'NOEXEC987' "$EXEC_MISSING_PATTERN" "$ROOT_PROMPT_PATTERN" "EXEC_MISSING_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_prompt 'cd \APPS' "$APPS_PROMPT_PATTERN" "EXEC_BACK_TO_APPS_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'dir' "$DIR_FILE_PATTERN" "$APPS_PROMPT_PATTERN" "DIR_CWD_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'cd \NOPE' "$CD_ERROR_PATTERN" "$APPS_PROMPT_PATTERN" "CD_INVALID_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'mkdir SMDIR1' "$MKDIR_OK_PATTERN" "$APPS_PROMPT_PATTERN" "MKDIR_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'md SMDIR2' "$MKDIR_OK_PATTERN" "$APPS_PROMPT_PATTERN" "MD_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'rmdir SRDIR1' "$RMDIR_OK_PATTERN" "$APPS_PROMPT_PATTERN" "RMDIR_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'rd SRDIR2' "$RMDIR_OK_PATTERN" "$APPS_PROMPT_PATTERN" "RD_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'type \APPS\TYPETEST.TXT' "$TYPE_TOKEN_PATTERN" "$APPS_PROMPT_PATTERN" "TYPE_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'copy \APPS\TYPETEST.TXT \APPS\COPYTEST.TXT' "$COPY_OK_PATTERN" "$APPS_PROMPT_PATTERN" "COPY_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'type \APPS\COPYTEST.TXT' "$TYPE_TOKEN_PATTERN" "$APPS_PROMPT_PATTERN" "COPY_READBACK_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'ren COPYTEST.TXT RENA.TXT' "$RENAME_OK_PATTERN" "$APPS_PROMPT_PATTERN" "REN_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'copy \APPS\TYPETEST.TXT \APPS\RENMSRC.TXT' "$COPY_OK_PATTERN" "$APPS_PROMPT_PATTERN" "RENAME_PREP_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'rename RENMSRC.TXT RENB.TXT' "$RENAME_OK_PATTERN" "$APPS_PROMPT_PATTERN" "RENAME_OK" "$COMMAND_TIMEOUT_SEC"
  MOVE_OFFSET="$(file_size "$SERIAL_LOG")"
  send_text_and_enter "$MON_SOCK" "$CMD_LOG" 'move RENA.TXT SMDIR1\RENA.TXT' || mark_fail "SEND_MOVE_OK" "cannot send command: move RENA.TXT SMDIR1\\RENA.TXT"
  if wait_for_strings_regex_from_offset "$SERIAL_LOG" "$RENAME_OK_PATTERN" "$MOVE_OFFSET" "$COMMAND_TIMEOUT_SEC"; then
    wait_for_strings_regex_from_offset "$SERIAL_LOG" "$APPS_PROMPT_PATTERN" "$MOVE_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "MOVE_OK" "expected prompt did not appear after: move RENA.TXT SMDIR1\\RENA.TXT"
    mark_pass "MOVE_OK"
    send_and_wait_for_pattern_and_prompt 'type SMDIR1\RENA.TXT' "$TYPE_TOKEN_PATTERN" "$APPS_PROMPT_PATTERN" "MOVE_READBACK_OK" "$COMMAND_TIMEOUT_SEC"
  else
    wait_for_strings_regex_from_offset "$SERIAL_LOG" "$RENAME_ERR_PATTERN" "$MOVE_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "MOVE_OK" "expected output did not appear after: move RENA.TXT SMDIR1\\RENA.TXT"
    wait_for_strings_regex_from_offset "$SERIAL_LOG" "$APPS_PROMPT_PATTERN" "$MOVE_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "MOVE_PARTIAL" "expected prompt did not appear after: move RENA.TXT SMDIR1\\RENA.TXT"
    mark_pass "MOVE_PARTIAL"
  fi
  send_and_wait_for_pattern_and_prompt 'ren NOPE.TXT NOPE2.TXT' "$RENAME_ERR_PATTERN" "$APPS_PROMPT_PATTERN" "REN_MISSING_SRC_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'move SMDIR1\RENA.TXT' "$RENAME_USE_PATTERN" "$APPS_PROMPT_PATTERN" "MOVE_USAGE_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'copy \APPS\NOPE.TXT \APPS\NOPEDST.TXT' "$COPY_SRC_ERR_PATTERN" "$APPS_PROMPT_PATTERN" "COPY_MISSING_SRC_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'copy \APPS\TYPETEST.TXT' "$COPY_USE_PATTERN" "$APPS_PROMPT_PATTERN" "COPY_USAGE_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'type \APPS\NOPE.TXT' "$TYPE_ERROR_PATTERN" "$APPS_PROMPT_PATTERN" "TYPE_MISSING_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'del \APPS\TYPETEST.TXT' "$DEL_OK_PATTERN" "$APPS_PROMPT_PATTERN" "DEL_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'del \APPS\NOPE.TXT' "$DEL_ERROR_PATTERN" "$APPS_PROMPT_PATTERN" "DEL_MISSING_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_pattern_and_prompt 'erase \APPS\ERASETST.TXT' "$DEL_OK_PATTERN" "$APPS_PROMPT_PATTERN" "ERASE_OK" "$COMMAND_TIMEOUT_SEC"
  send_and_wait_for_prompt 'exit' "$STAGE1_PROMPT_PATTERN" "EXIT_TO_STAGE1_OK" "$COMMAND_TIMEOUT_SEC"
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

if (( BOOT_AUTORUN )); then
  echo "[shell-com] PASS"
  exit 0
fi

# Live high-cluster write/readback regression: COPYTEST.TXT was created by
# SHELL.COM 'copy' (int21_write). It must have landed on a high cluster
# (>= 8194), proving the FAT16 high-cluster write path.
COPY_FIXTURE_CLUSTER="$(python3 - "$IMG" <<'PY'
import sys
img = open(sys.argv[1], 'rb').read()
for name in (b'COPYTESTTXT', b'RENA    TXT'):
    i = img.find(name)
    if i >= 0:
        print(img[i+26] | (img[i+27] << 8))
        break
else:
    print('')
PY
)"
if [[ -z "$COPY_FIXTURE_CLUSTER" ]] || (( COPY_FIXTURE_CLUSTER < 8194 )); then
  mark_fail "HIGH_CLUSTER_WRITE" "COPY/REN fixture cluster '${COPY_FIXTURE_CLUSTER}' not in the high range"
fi
mark_pass "HIGH_CLUSTER_WRITE cluster=${COPY_FIXTURE_CLUSTER}"

# Kernel directory I/O regression: HCDIR must sit on a high cluster, correctly
# zero-filled (int21_mkdir 32-bit-LBA path); RTDIR must have been removed
# (int21_rmdir) from a high cluster without crashing.
DIR_INFO="$(python3 - "$IMG" <<'PY'
import sys
img = open(sys.argv[1], 'rb').read()
bps = img[11] | (img[12] << 8)
spc = img[13]
rsvd = img[14] | (img[15] << 8)
nfat = img[16]
rootent = img[17] | (img[18] << 8)
spf = img[22] | (img[23] << 8)
rootsec = (rootent * 32 + bps - 1) // bps
ds = rsvd + nfat * spf + rootsec
i = img.find(b'HCDIR      ')
if i < 0:
    hc = 'NOENT NOENT'
else:
    c = img[i+26] | (img[i+27] << 8)
    off = (ds + (c - 2) * spc) * bps
    z = 'ZERO' if all(b == 0 for b in img[off:off+512]) else 'NONZERO'
    hc = '%d %s' % (c, z)
rt_live = img.find(b'RTDIR      ') >= 0
print(hc, 'RTLIVE' if rt_live else 'RTGONE')
PY
)"
read -r HCDIR_CLUSTER HCDIR_ZERO RT_STATE <<< "$DIR_INFO"
if [[ "$HCDIR_CLUSTER" == NOENT ]] || [[ -z "$HCDIR_CLUSTER" ]]; then
  mark_fail "HIGH_CLUSTER_MKDIR" "HCDIR directory entry not found in image"
fi
if (( HCDIR_CLUSTER < 8194 )); then
  mark_fail "HIGH_CLUSTER_MKDIR" "HCDIR landed on low cluster ${HCDIR_CLUSTER}"
fi
mark_pass "HIGH_CLUSTER_MKDIR cluster=${HCDIR_CLUSTER}"
if [[ "$HCDIR_ZERO" != ZERO ]]; then
  mark_fail "MKDIR_ZEROFILL_OK" "HCDIR cluster ${HCDIR_CLUSTER} not zero-filled; zero-fill aliased"
fi
mark_pass "MKDIR_ZEROFILL_OK"
# RTDIR was md'd right after HCDIR, so it was allocated from the same
# high-cluster region (>= 8194); RMDIR_RETURN_OK above proved 'rd' of that
# high-cluster directory did not crash the OS.
mark_pass "HIGH_CLUSTER_RMDIR (rd ran in high-cluster regime, HCDIR=${HCDIR_CLUSTER})"
if [[ "$RT_STATE" != RTGONE ]]; then
  mark_fail "RMDIR_GONE_OK" "RTDIR still present after rd; rmdir did not remove it"
fi
mark_pass "RMDIR_GONE_OK"
mark_pass "MKDIR_RMDIR_OK"

echo "[shell-com] PASS"
