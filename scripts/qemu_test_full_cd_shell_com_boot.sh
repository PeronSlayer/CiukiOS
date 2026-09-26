#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
SERIAL_NORMALIZER="$ROOT_DIR/scripts/serial_log_normalize.py"
CIUKIOS_VERSION="${CIUKIOS_VERSION:-0.7.1}"
ISO_VERSION_TAG="${CIUKIOS_VERSION//./-}"
VERSIONED_ISO_IMG="build/full/CiukiOS_full_cd_${ISO_VERSION_TAG}.iso"

DO_BUILD=1
BOOT_EXPECT_FALLBACK="${FULL_CD_SHELL_COM_BOOT_EXPECT_FALLBACK:-0}"
IMG="build/full/ciukios-full-cd-direct.iso"
PREFIX="build/full/qemu-full-cd-shell-com-boot"
if (( BOOT_EXPECT_FALLBACK )); then
  PREFIX="build/full/qemu-full-cd-shell-com-boot-fallback"
fi
SERIAL_LOG="${PREFIX}.serial.log"
STRINGS_LOG="${PREFIX}.strings.log"
STDERR_LOG="${PREFIX}.stderr.log"
CMD_LOG="${PREFIX}.commands.log"
MON_SOCK="/tmp/ciukios-full-cd-shell-com-boot.$$.monitor.sock"

ACTIVE_QEMU_PID=0
ACTIVE_MON_SOCK=""
ACTIVE_CMD_LOG=""
CD_SNAPSHOT_DIR=""
CD_SNAPSHOT_ACTIVE=0
CD_ARTIFACTS=(
  "build/full/ciukios-full-cd-partition.img"
  "build/full/ciukios-full-cd-disk.img"
  "build/full/ciukios-full-cd-redundant.img"
  "build/full/obj/full_cd_mbr.bin"
  "build/full/cd-iso-root"
  "build/full/ciukios-full-cd.iso"
  "$VERSIONED_ISO_IMG"
  "build/full/ciukios-full-cd-direct.iso"
  "build/full/ciukios-full-cd-isolinux.iso"
  "build/full/ciukios-full-cd-lowmem.iso"
)
CD_ARTIFACT_PRESENT=()

usage() {
  cat <<'TXT'
Usage: scripts/qemu_test_full_cd_shell_com_boot.sh [--no-build]

Boots the direct full-CD ISO headlessly with default SHELL.COM boot enabled,
validates the SHELL.COM prompt on D:, reads the deterministic fixture beyond
LBA 65535, runs COM and MZ executables plus a focused command smoke, and
confirms that EXIT/QUIT remain disabled without transferring control back to
the loader.

Set FULL_CD_SHELL_COM_BOOT_EXPECT_FALLBACK=1 to remove \SYSTEM\SHELL.COM
from the test image, then verify the fatal loader message and no prompt.
TXT
}

mark_fail() {
  local marker="$1"
  local detail="$2"
  echo "[full-cd-shell-com-boot] FAIL ${marker}: ${detail}" >&2
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
  echo "[full-cd-shell-com-boot] PASS ${marker}"
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

remove_cd_artifact() {
  local path="$1"
  case "$path" in
    build/full/ciukios-full-cd-partition.img|\
    build/full/ciukios-full-cd-disk.img|\
    build/full/ciukios-full-cd-redundant.img|\
    build/full/obj/full_cd_mbr.bin|\
    build/full/cd-iso-root|\
    build/full/ciukios-full-cd.iso|\
    "$VERSIONED_ISO_IMG"|\
    build/full/ciukios-full-cd-direct.iso|\
    build/full/ciukios-full-cd-isolinux.iso|\
    build/full/ciukios-full-cd-lowmem.iso) ;;
    *)
      echo "[full-cd-shell-com-boot] ERROR: refusing unsafe artifact removal: $path" >&2
      return 1
      ;;
  esac

  if [[ -d "$path" && ! -L "$path" ]]; then
    find "$path" -depth -delete
  else
    rm -f -- "$path"
  fi
}

snapshot_cd_artifacts() {
  local index path snapshot

  if ! CD_SNAPSHOT_DIR="$(mktemp -d /tmp/ciukios-full-cd-fallback.XXXXXX)"; then
    return 1
  fi
  case "$CD_SNAPSHOT_DIR" in
    /tmp/ciukios-full-cd-fallback.*) ;;
    *) return 1 ;;
  esac
  if [[ ! -d "$CD_SNAPSHOT_DIR" || -L "$CD_SNAPSHOT_DIR" || "${CD_SNAPSHOT_DIR#/tmp/}" == */* ]]; then
    return 1
  fi

  for index in "${!CD_ARTIFACTS[@]}"; do
    path="${CD_ARTIFACTS[$index]}"
    snapshot="$CD_SNAPSHOT_DIR/artifact_$index"
    if [[ -e "$path" || -L "$path" ]]; then
      CD_ARTIFACT_PRESENT[$index]=1
      cp -a --reflink=auto --sparse=always -- "$path" "$snapshot" || return 1
      if [[ -d "$path" && ! -L "$path" ]]; then
        diff -qr -- "$path" "$snapshot" >/dev/null || return 1
      else
        cmp -s -- "$path" "$snapshot" || return 1
      fi
    else
      CD_ARTIFACT_PRESENT[$index]=0
    fi
  done
  CD_SNAPSHOT_ACTIVE=1
}

restore_cd_artifacts() {
  local index path snapshot restore_rc=0

  (( CD_SNAPSHOT_ACTIVE )) || return 0
  case "$CD_SNAPSHOT_DIR" in
    /tmp/ciukios-full-cd-fallback.*) ;;
    *) return 1 ;;
  esac
  if [[ ! -d "$CD_SNAPSHOT_DIR" || -L "$CD_SNAPSHOT_DIR" || "${CD_SNAPSHOT_DIR#/tmp/}" == */* ]]; then
    return 1
  fi

  for index in "${!CD_ARTIFACTS[@]}"; do
    path="${CD_ARTIFACTS[$index]}"
    snapshot="$CD_SNAPSHOT_DIR/artifact_$index"
    if ! remove_cd_artifact "$path"; then
      restore_rc=1
      continue
    fi
    if [[ "${CD_ARTIFACT_PRESENT[$index]:-0}" == "1" ]]; then
      mkdir -p -- "$(dirname "$path")"
      if ! cp -a --reflink=auto --sparse=always -- "$snapshot" "$path"; then
        restore_rc=1
        continue
      fi
      if [[ -d "$snapshot" && ! -L "$snapshot" ]]; then
        diff -qr -- "$snapshot" "$path" >/dev/null || restore_rc=1
      else
        cmp -s -- "$snapshot" "$path" || restore_rc=1
      fi
    elif [[ -e "$path" || -L "$path" ]]; then
      restore_rc=1
    fi
  done

  if (( restore_rc == 0 )); then
    CD_SNAPSHOT_ACTIVE=0
  fi
  return "$restore_rc"
}

cleanup_cd_snapshot() {
  [[ -n "$CD_SNAPSHOT_DIR" ]] || return 0
  case "$CD_SNAPSHOT_DIR" in
    /tmp/ciukios-full-cd-fallback.*) ;;
    *) return 1 ;;
  esac
  if [[ ! -d "$CD_SNAPSHOT_DIR" || -L "$CD_SNAPSHOT_DIR" || "${CD_SNAPSHOT_DIR#/tmp/}" == */* ]]; then
    return 1
  fi
  if ! find "$CD_SNAPSHOT_DIR" -depth -delete; then
    return 1
  fi
  CD_SNAPSHOT_DIR=""
}

on_exit() {
  local rc="$1"
  local restored=1
  trap - EXIT HUP INT TERM
  cleanup_active_qemu || rc=1
  if ! restore_cd_artifacts; then
    restored=0
    rc=1
  fi
  if (( restored )); then
    cleanup_cd_snapshot || rc=1
  elif [[ -n "$CD_SNAPSHOT_DIR" ]]; then
    echo "[full-cd-shell-com-boot] ERROR: recovery snapshot preserved at $CD_SNAPSHOT_DIR" >&2
  fi
  exit "$rc"
}

trap 'on_exit "$?"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

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
    sleep 0.05
  done
}

strings_from_offset() {
  local file="$1"
  local offset="$2"
  if [[ ! -f "$file" ]]; then
    return 0
  fi
  "$SERIAL_NORMALIZER" --offset "$offset" "$file" | strings -a
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
    sleep 0.05
  done
}

assert_no_strings_regex_from_offset() {
  local file="$1"
  local pattern="$2"
  local offset="$3"
  local timeout_sec="$4"
  local start now
  start="$(date +%s)"
  while true; do
    if strings_from_offset "$file" "$offset" | grep -Eiq "$pattern"; then
      return 1
    fi
    now="$(date +%s)"
    if (( now - start >= timeout_sec )); then
      return 0
    fi
    sleep 0.05
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
      '\\') key="backslash" ;;
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
      echo "[full-cd-shell-com-boot] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

need_cmd socat
need_cmd strings
need_cmd timeout
need_cmd python3
need_cmd cmp
need_cmd cp
need_cmd diff
need_cmd dirname
need_cmd find
need_cmd mkdir
need_cmd mktemp
need_cmd rm

if (( DO_BUILD )); then
  if (( BOOT_EXPECT_FALLBACK )); then
    echo "[full-cd-shell-com-boot] snapshotting canonical CD artifacts"
    snapshot_cd_artifacts \
      || mark_fail "CD_ARTIFACT_SNAPSHOT" "cannot snapshot canonical CD artifacts under /tmp"
  fi
  echo "[full-cd-shell-com-boot] build step"
  CIUKIOS_STAGE1_BOOT_EXTERNAL_SHELL=1 \
  CIUKIOS_FULL_CD_OMIT_SYSTEM_SHELL="$BOOT_EXPECT_FALLBACK" \
  bash scripts/build_full_cd.sh
fi

if [[ ! -f "$IMG" ]]; then
  mark_fail "IMAGE" "missing image: $IMG"
fi

QEMU_CMD="$(pick_qemu || true)"
if [[ -z "$QEMU_CMD" ]]; then
  mark_fail "QEMU" "qemu-system-i386/x86_64 not found"
fi

mkdir -p build/full
rm -f "$SERIAL_LOG" "$STRINGS_LOG" "$STDERR_LOG" "$CMD_LOG" "$MON_SOCK"

QEMU_TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-240}"
PROMPT_TIMEOUT_SEC="${FULL_CD_SHELL_COM_BOOT_PROMPT_TIMEOUT_SEC:-120}"
COMMAND_TIMEOUT_SEC="${FULL_CD_SHELL_COM_BOOT_COMMAND_TIMEOUT_SEC:-60}"

SHELL_PROMPT_PREFIX='C+I+U+K+I+O+S+[[:space:]]+S+H+E+L+L+[[:space:]]+'
CHILD_PROMPT_PATTERN="${SHELL_PROMPT_PREFIX}D+[:]+[\\]+A+P+P+S+>+"
STAGE1_PROMPT_PREFIX='C{1,2}i{1,2}u{1,2}k{1,2}i{1,2}O{1,2}S{1,2}[[:space:]]+'
STAGE1_APPS_PROMPT_PATTERN="${STAGE1_PROMPT_PREFIX}D{1,2}:{1,2}[\\]{1,2}A{1,2}P{2,4}S{1,2}[\\]{1,2}>{1,2}"
VER_PATTERN='C+I+U+K+I+O+S+[[:space:]]+P+R+E+[-[:space:]]*A+L+P+H+A+[[:space:]]+V+0+[.]+7+[.]+1+'
WHERE_SHELL_PATTERN='[CD]+[:]+[\\]+S+Y+S+T+E+M+[\\]+S+H+E+L+L+\.*C+O+M+'
WHERE_MOUSE_PATTERN='[CD]+[:]+[\\]+S+Y+S+T+E+M+[\\]+M+O+U+S+E+\.*C+O+M+'
COMDEMO_PASS_PATTERN='C+O+M+[[:space:]]+D+E+M+O+[[:space:]]+V+I+A+[[:space:]]+I+N+T+2+1+H+'
MZDEMO_PASS_PATTERN='M+Z+[[:space:]]+D+E+M+O+[[:space:]]+V+I+A+[[:space:]]+I+N+T+2+1+H+'
LBA32_READ_PATTERN='\[{1,2}L{1,2}B{1,2}A{1,2}3{1,2}2{1,2}\]{1,2}[[:space:]]+H{1,2}I{1,2}G{1,2}H{1,2}-+L{1,2}B{1,2}A{1,2}[[:space:]]+R{1,2}E{1,2}A{1,2}D{1,2}[[:space:]]+P{1,2}A{1,2}S{2,4}'
CIUKRTST_PASS_PATTERN='\[CIUKRTST\][[:space:]]+OWNER=CIUKIDOS[[:space:]]+ABI=2[[:space:]]+SERVICES=11[[:space:]]+CHAIN=0[[:space:]]+STATE=PASS'
PSTACK_CHILD_PASS_PATTERN='\[PSTACK:C\][[:space:]]+ALL=PASS[[:space:]]+TSR-EXEC-UNLOAD=PASS[[:space:]]+EXIT=5A'
PSTACK_ROOT_PASS_PATTERN='\[PSTACK:R\][[:space:]]+ALL=PASS'
WOOF_PATTERN='W+O+O+F+'
LOADER_FATAL_MISSING_PATTERN='S+H+E+L+L+\.*C+O+M+[[:space:]]+M+I+S+S+I+N+G+'
LOADER_FATAL_EXITED_PATTERN='S+H+E+L+L+\.*C+O+M+[[:space:]]+E+X+I+T+E+D+'
LOADER_FATAL_RETURN_PATTERN='S+H+E+L+L+\.*C+O+M+[[:space:]]+R+E+T+U+R+N+E+D+[[:space:]]+C+O+N+T+R+O+L+'
RESTART_PATTERN='P+L+E+A+S+E+[[:space:]]+R+E+S+T+A+R+T+'
EXIT_DISABLED_PATTERN='E+X+I+T+/+Q+U+I+T+[[:space:]]+I+S+[[:space:]]+N+O+T+[[:space:]]+A+V+A+I+L+A+B+L+E+'
EXIT_GUIDANCE_PATTERN='U+S+E+[[:space:]]+R+E+B+O+O+T+[[:space:]]+O+R+[[:space:]]+S+H+U+T+D+O+W+N+'
POWER_IDLE_PATTERN='S+H+U+T+D+O+W+N+[:]+[[:space:]]+I+D+L+E+'
POWER_QUEUE_REBOOT_PATTERN='R+E+B+O+O+T+[:]+[[:space:]]+Q+U+E+U+E+D+'
POWER_QUEUE_HALT_PATTERN='S+H+U+T+D+O+W+N+[:]+[[:space:]]+Q+U+E+U+E+D+'
POWER_STATUS_REBOOT_PATTERN='S+H+U+T+D+O+W+N+[:]+[[:space:]]+P+E+N+D+I+N+G+[[:space:]]+R+E+B+O+O+T+'
POWER_STATUS_HALT_PATTERN='S+H+U+T+D+O+W+N+[:]+[[:space:]]+P+E+N+D+I+N+G+[[:space:]]+H+A+L+T+'
POWER_CANCEL_PATTERN='S+H+U+T+D+O+W+N+[:]+[[:space:]]+C+A+N+C+E+L+E+D+'
POWER_BAD_TIMER_PATTERN='U+S+A+G+E+[:]+[[:space:]]+S+H+U+T+D+O+W+N+'
MOUSE_PATTERN='M+O+U+S+E+[:]+[[:space:]]+(I+N+T+3+3+H+[[:space:]]+N+O+T+[[:space:]]+I+N+S+T+A+L+L+E+D+|I+N+T+3+3+H+[[:space:]]+R+E+A+D+Y+)'
MOUSE_RUNTIME_PATTERN='M+O+U+S+E+[:]+[[:space:]]+R+U+N+T+I+M+E+[[:space:]]+B+A+C+K+E+D+[[:space:]]+S+E+R+V+I+C+E+[[:space:]]+A+C+T+I+V+E+'
MOUSE_INSTALL_PATTERN='M+O+U+S+E+[:]+[[:space:]]+R+U+N+T+I+M+E+[[:space:]]+B+A+C+K+E+D+[[:space:]]+S+E+R+V+I+C+E+[[:space:]]+A+L+R+E+A+D+Y+[[:space:]]+I+N+S+T+A+L+L+E+D+'
MOUSE_INFO_PATTERN='I+N+F+O+[[:space:]]+V+E+R+=+0+X+0+6+2+6+[[:space:]]+T+Y+P+E+=+0+X+0+0+0+4+'
MOUSE_PAGE1_PATTERN='P+A+G+E+=+0+X+'
MOUSE_DISABLE_PATTERN='M+O+U+S+E+[:]+[[:space:]]+D+R+I+V+E+R+[[:space:]]+D+I+S+A+B+L+E+D+'
MOUSE_ENABLE_PATTERN='M+O+U+S+E+[:]+[[:space:]]+D+R+I+V+E+R+[[:space:]]+E+N+A+B+L+E+D+'

QEMU_ARGS=(
  -machine pc,vmport=off
  -cpu pentium3
  -m 128
  -drive "file=$IMG,format=raw,if=ide,index=2,media=cdrom,readonly=on"
  -boot d
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

if (( BOOT_EXPECT_FALLBACK )); then
  if ! wait_for_strings_regex_from_offset "$SERIAL_LOG" "$WOOF_PATTERN" 0 "$PROMPT_TIMEOUT_SEC"; then
    mark_fail "LOADER_FATAL_WOOF" "WOOF fatal header not detected"
  fi
  mark_pass "LOADER_FATAL_WOOF"
  if ! wait_for_strings_regex_from_offset "$SERIAL_LOG" "$LOADER_FATAL_MISSING_PATTERN" 0 "$PROMPT_TIMEOUT_SEC"; then
    mark_fail "LOADER_FATAL_MISSING" "fatal missing-shell message not detected"
  fi
  mark_pass "LOADER_FATAL_MISSING"
  if ! wait_for_strings_regex_from_offset "$SERIAL_LOG" "$RESTART_PATTERN" 0 "$PROMPT_TIMEOUT_SEC"; then
    mark_fail "LOADER_FATAL_RESTART" "restart instruction not detected after missing shell"
  fi
  mark_pass "LOADER_FATAL_RESTART"
  if ! assert_no_strings_regex_from_offset "$SERIAL_LOG" "$CHILD_PROMPT_PATTERN" 0 5; then
    mark_fail "NO_SHELL_PROMPT" "shell prompt appeared after fatal missing-shell path"
  fi
  if ! assert_no_strings_regex_from_offset "$SERIAL_LOG" "$STAGE1_APPS_PROMPT_PATTERN" 0 5; then
    mark_fail "NO_STAGE1_PROMPT" "Stage1 prompt appeared after fatal missing-shell path"
  fi
  mark_pass "NO_FALLBACK_PROMPT"
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
  "$SERIAL_NORMALIZER" "$SERIAL_LOG" | strings -a > "$STRINGS_LOG" || true
  restore_cd_artifacts \
    || mark_fail "CD_ARTIFACT_RESTORE" "cannot restore canonical CD artifacts"
  cleanup_cd_snapshot \
    || mark_fail "CD_ARTIFACT_SNAPSHOT_CLEANUP" "cannot remove temporary CD snapshot"
  echo "[full-cd-shell-com-boot] PASS"
  exit 0
fi

if ! wait_for_strings_regex_from_offset "$SERIAL_LOG" "$CHILD_PROMPT_PATTERN" 0 "$PROMPT_TIMEOUT_SEC"; then
  mark_fail "INITIAL_SHELL_COM_PROMPT" "initial D:\\APPS SHELL.COM prompt not detected"
fi
mark_pass "INITIAL_SHELL_COM_PROMPT"

send_and_wait_for_pattern_and_prompt 'ver' "$VER_PATTERN" "$CHILD_PROMPT_PATTERN" "VER_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'where SHELL' "$WHERE_SHELL_PATTERN" "$CHILD_PROMPT_PATTERN" "WHERE_SHELL_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'where MOUSE' "$WHERE_MOUSE_PATTERN" "$CHILD_PROMPT_PATTERN" "WHERE_MOUSE_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'type D:/LBA32.TXT' "$LBA32_READ_PATTERN" "$CHILD_PROMPT_PATTERN" "CHS32_HIGH_LBA_READ_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'comdemo' "$COMDEMO_PASS_PATTERN" "$CHILD_PROMPT_PATTERN" "COMDEMO_D_APPS_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'mzdemo' "$MZDEMO_PASS_PATTERN" "$CHILD_PROMPT_PATTERN" "MZDEMO_D_APPS_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'ciukrtst' "$CIUKRTST_PASS_PATTERN" "$CHILD_PROMPT_PATTERN" "CIUKRTST_D_APPS_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'ciukpst' "$PSTACK_CHILD_PASS_PATTERN" "$CHILD_PROMPT_PATTERN" "PSTACK_NESTED_D_APPS_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'ciukpst /root' "$PSTACK_ROOT_PASS_PATTERN" "$CHILD_PROMPT_PATTERN" "PSTACK_ROOT_RESTORE_D_APPS_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'shutdown status' "$POWER_IDLE_PATTERN" "$CHILD_PROMPT_PATTERN" "POWER_IDLE_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'reboot /t 3' "$POWER_QUEUE_REBOOT_PATTERN" "$CHILD_PROMPT_PATTERN" "REBOOT_QUEUE_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'shutdown status' "$POWER_STATUS_REBOOT_PATTERN" "$CHILD_PROMPT_PATTERN" "POWER_REBOOT_STATUS_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'reboot cancel' "$POWER_CANCEL_PATTERN" "$CHILD_PROMPT_PATTERN" "REBOOT_CANCEL_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'shutdown /t 3' "$POWER_QUEUE_HALT_PATTERN" "$CHILD_PROMPT_PATTERN" "SHUTDOWN_QUEUE_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'shutdown status' "$POWER_STATUS_HALT_PATTERN" "$CHILD_PROMPT_PATTERN" "POWER_SHUTDOWN_STATUS_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'shutdown cancel' "$POWER_CANCEL_PATTERN" "$CHILD_PROMPT_PATTERN" "SHUTDOWN_CANCEL_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'shutdown /t nope' "$POWER_BAD_TIMER_PATTERN" "$CHILD_PROMPT_PATTERN" "POWER_BAD_TIMER_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'MOUSE' "$MOUSE_PATTERN" "$CHILD_PROMPT_PATTERN" "MOUSE_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'MOUSE STATUS' "$MOUSE_RUNTIME_PATTERN" "$CHILD_PROMPT_PATTERN" "MOUSE_STATUS_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'MOUSE INSTALL' "$MOUSE_INSTALL_PATTERN" "$CHILD_PROMPT_PATTERN" "MOUSE_INSTALL_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'MOUSE INFO' "$MOUSE_INFO_PATTERN" "$CHILD_PROMPT_PATTERN" "MOUSE_INFO_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'MOUSE PAGE 1' "$MOUSE_PAGE1_PATTERN" "$CHILD_PROMPT_PATTERN" "MOUSE_PAGE_SET_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'MOUSE GETPAGE' "$MOUSE_PAGE1_PATTERN" "$CHILD_PROMPT_PATTERN" "MOUSE_PAGE_GET_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'MOUSE DISABLE' "$MOUSE_DISABLE_PATTERN" "$CHILD_PROMPT_PATTERN" "MOUSE_DISABLE_OK" "$COMMAND_TIMEOUT_SEC"
send_and_wait_for_pattern_and_prompt 'MOUSE ENABLE' "$MOUSE_ENABLE_PATTERN" "$CHILD_PROMPT_PATTERN" "MOUSE_ENABLE_OK" "$COMMAND_TIMEOUT_SEC"
EXIT_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" 'exit' || mark_fail "SEND_EXIT_DISABLED_OK" "cannot send command: exit"
wait_for_strings_regex_from_offset "$SERIAL_LOG" "$EXIT_DISABLED_PATTERN" "$EXIT_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "EXIT_DISABLED_OK" "disabled exit message not detected after: exit"
wait_for_strings_regex_from_offset "$SERIAL_LOG" "$EXIT_GUIDANCE_PATTERN" "$EXIT_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "EXIT_DISABLED_OK" "exit guidance not detected after: exit"
wait_for_strings_regex_from_offset "$SERIAL_LOG" "$CHILD_PROMPT_PATTERN" "$EXIT_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "EXIT_DISABLED_OK" "shell prompt did not return after: exit"
if ! assert_no_strings_regex_from_offset "$SERIAL_LOG" "$WOOF_PATTERN" "$EXIT_OFFSET" 5; then
  mark_fail "EXIT_NO_FATAL" "fatal WOOF screen appeared after exit"
fi
if ! assert_no_strings_regex_from_offset "$SERIAL_LOG" "$LOADER_FATAL_RETURN_PATTERN" "$EXIT_OFFSET" 5; then
  mark_fail "EXIT_NO_FATAL" "loader fatal return text appeared after exit"
fi
mark_pass "EXIT_DISABLED_OK"
QUIT_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" 'quit' || mark_fail "SEND_QUIT_DISABLED_OK" "cannot send command: quit"
wait_for_strings_regex_from_offset "$SERIAL_LOG" "$EXIT_DISABLED_PATTERN" "$QUIT_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "QUIT_DISABLED_OK" "disabled quit message not detected after: quit"
wait_for_strings_regex_from_offset "$SERIAL_LOG" "$EXIT_GUIDANCE_PATTERN" "$QUIT_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "QUIT_DISABLED_OK" "quit guidance not detected after: quit"
wait_for_strings_regex_from_offset "$SERIAL_LOG" "$CHILD_PROMPT_PATTERN" "$QUIT_OFFSET" "$COMMAND_TIMEOUT_SEC" || mark_fail "QUIT_DISABLED_OK" "shell prompt did not return after: quit"
if ! assert_no_strings_regex_from_offset "$SERIAL_LOG" "$WOOF_PATTERN" "$QUIT_OFFSET" 5; then
  mark_fail "QUIT_NO_FATAL" "fatal WOOF screen appeared after quit"
fi
if ! assert_no_strings_regex_from_offset "$SERIAL_LOG" "$LOADER_FATAL_RETURN_PATTERN" "$QUIT_OFFSET" 5; then
  mark_fail "QUIT_NO_FATAL" "loader fatal return text appeared after quit"
fi
mark_pass "QUIT_DISABLED_OK"

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

"$SERIAL_NORMALIZER" "$SERIAL_LOG" | strings -a > "$STRINGS_LOG" || true

echo "[full-cd-shell-com-boot] PASS"
