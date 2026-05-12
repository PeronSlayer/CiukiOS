#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
IMG="build/full/ciukios-full.img"
SERIAL_LOG="build/full/qemu-full-drvload-smoke.serial.log"
STRINGS_LOG="build/full/qemu-full-drvload-smoke.strings.log"
STDERR_LOG="build/full/qemu-full-drvload-smoke.stderr.log"
CMD_LOG="build/full/qemu-full-drvload-smoke.commands.log"
META_LOG="build/full/qemu-full-drvload-smoke.meta"
MON_SOCK="/tmp/ciukios-drvload-smoke.monitor.sock"
QEMU_AUDIO_MODE="${QEMU_AUDIO_MODE:-on}"
QEMU_AUDIO_ARGS=()
QEMU_AUDIO_DETAIL="off"

ACTIVE_QEMU_PID=0
ACTIVE_MON_SOCK=""
ACTIVE_CMD_LOG=""

usage() {
  cat << 'TXT'
Usage: scripts/qemu_test_full_drvload_smoke.sh [--no-build]

Boots full profile, waits for shell prompt, runs:
  run \SYSTEM\DRIVERS\DRVLOAD.COM

Checks runtime markers in serial output:
  [DRVLOAD] BEGIN
  [DRVLOAD] DONE
  [DRVLOAD] TRY ... (at least one)

Artifacts:
  build/full/qemu-full-drvload-smoke.{serial.log,strings.log,stderr.log,commands.log,meta}
TXT
}

mark_fail() {
  local marker="$1"
  local detail="$2"
  echo "[drvload-smoke] FAIL ${marker}: ${detail}" >&2
  if [[ -f "$STDERR_LOG" ]]; then
    tail -n 40 "$STDERR_LOG" >&2 || true
  fi
  if [[ -f "$SERIAL_LOG" ]]; then
    tail -n 120 "$SERIAL_LOG" >&2 || true
  fi
  exit 1
}

mark_pass() {
  local marker="$1"
  echo "[drvload-smoke] PASS ${marker}"
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

normalize_audio_backend() {
  case "$1" in
    pulse|pulseaudio)
      echo "pa"
      ;;
    *)
      echo "$1"
      ;;
  esac
}

audio_backend_supported() {
  local qemu_cmd="$1"
  local backend="$2"

  case "$backend" in
    none|alsa|dbus|jack|oss|pa|pipewire|sdl|spice|wav) ;;
    *) return 1 ;;
  esac

  [[ "$backend" == "none" ]] && return 0
  "$qemu_cmd" -audiodev help 2>/dev/null | grep -Eq "^${backend}$"
}

configure_audio_args() {
  local qemu_cmd="$1"
  local context="$2"
  local requested_backend="${QEMU_AUDIO_BACKEND:-}"
  local backend=""
  local candidate

  QEMU_AUDIO_ARGS=()
  QEMU_AUDIO_DETAIL="off"

  case "$QEMU_AUDIO_MODE" in
    auto|on|off) ;;
    *)
      mark_fail "AUDIO_MODE" "invalid QEMU_AUDIO_MODE=$QEMU_AUDIO_MODE (expected auto, on or off)"
      ;;
  esac

  if [[ "$QEMU_AUDIO_MODE" == "off" ]]; then
    return 0
  fi

  if [[ -n "$requested_backend" ]]; then
    backend="$(normalize_audio_backend "$requested_backend")"
    if ! audio_backend_supported "$qemu_cmd" "$backend"; then
      mark_fail "AUDIO_BACKEND" "unsupported QEMU_AUDIO_BACKEND=$requested_backend for $qemu_cmd"
    fi
  elif [[ "$context" == "headless" && "$QEMU_AUDIO_MODE" != "on" ]]; then
    backend="none"
  else
    for candidate in pipewire pa alsa sdl; do
      if audio_backend_supported "$qemu_cmd" "$candidate"; then
        backend="$candidate"
        break
      fi
    done
    [[ -n "$backend" ]] || backend="none"
  fi

  QEMU_AUDIO_ARGS=(
    -audiodev "${backend},id=snd0"
    -device "sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=snd0"
  )
  QEMU_AUDIO_DETAIL="backend=${backend} sb16=iobase=0x220 irq=7 dma=1 hdma=5"
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

wait_for_regex() {
  local file="$1"
  local pattern="$2"
  local timeout_sec="$3"
  local start now
  start="$(date +%s)"
  while true; do
    if [[ -f "$file" ]] && grep -Eiq "$pattern" "$file"; then
      return 0
    fi
    now="$(date +%s)"
    if (( now - start >= timeout_sec )); then
      return 1
    fi
  done
}

wait_for_done_with_nudge() {
  local file="$1"
  local pattern="$2"
  local timeout_sec="$3"
  local sock="$4"
  local cmd_log="$5"
  local start now next_nudge

  start="$(date +%s)"
  next_nudge=$((start + 2))

  while true; do
    if [[ -f "$file" ]] && grep -Eiq "$pattern" "$file"; then
      return 0
    fi

    now="$(date +%s)"
    if (( now >= next_nudge )); then
      send_key "$sock" "$cmd_log" ret >/dev/null 2>&1 || true
      send_key "$sock" "$cmd_log" esc >/dev/null 2>&1 || true
      next_nudge=$((now + 2))
    fi

    if (( now - start >= timeout_sec )); then
      return 1
    fi
  done
}

shell_prompt_seen() {
  local file="$1"
  if [[ ! -f "$file" ]]; then
    return 1
  fi
  grep -Eiq 'CiukiOS C:\\|CCiiuukkiiOOSS' "$file"
}

wait_for_shell_prompt() {
  local file="$1"
  local timeout_sec="$2"
  local start now
  start="$(date +%s)"
  while true; do
    if shell_prompt_seen "$file"; then
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

send_text_and_enter() {
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
      [A-Z]) key="shift-$(printf '%s' "$ch" | tr 'A-Z' 'a-z')" ;;
      [a-z0-9]) key="$ch" ;;
      *) continue ;;
    esac
    send_key "$sock" "$cmd_log" "$key" || return 1
  done

  send_key "$sock" "$cmd_log" ret || return 1
  return 0
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
      echo "[drvload-smoke] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

need_cmd socat
need_cmd strings

if (( DO_BUILD )); then
  echo "[drvload-smoke] build step"
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

QEMU_TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-240}"
PROMPT_TIMEOUT_SEC="${DRVLOAD_PROMPT_TIMEOUT_SEC:-120}"
MARKER_TIMEOUT_SEC="${DRVLOAD_MARKER_TIMEOUT_SEC:-120}"
DRVLOAD_ARGS="${DRVLOAD_ARGS:-}"
DRVLOAD_COMMAND='run \SYSTEM\DRIVERS\DRVLOAD.COM'
if [[ -n "$DRVLOAD_ARGS" ]]; then
  DRVLOAD_COMMAND+=" $DRVLOAD_ARGS"
fi

DRVLOAD_BEGIN_PATTERN='\[DRVLOAD\][[:space:]]+BEGIN|\[{1,2}DDRRVVLLOOAADD\]\][[:space:]]+BBEEGGIIN'
DRVLOAD_TRY_PATTERN='\[DRVLOAD\][[:space:]]+TRY[[:space:]]+|\[{1,2}DDRRVVLLOOAADD\]\][[:space:]]+TTRRYY[[:space:]]+'
DRVLOAD_DONE_PATTERN='\[DRVLOAD\][[:space:]]+DONE|\[{1,2}DDRRVVLLOOAADD\]\][[:space:]]+DDOONNEE?'

configure_audio_args "$QEMU_CMD" headless

echo "[drvload-smoke] audio: $QEMU_AUDIO_DETAIL"

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
  "${QEMU_AUDIO_ARGS[@]}"
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
  mark_fail "MONITOR" "monitor socket not ready"
fi
mark_pass "MONITOR"

if ! kill -0 "$QEMU_PID" >/dev/null 2>&1; then
  mark_fail "QEMU_EARLY_EXIT" "qemu exited before shell prompt"
fi

if ! wait_for_shell_prompt "$SERIAL_LOG" "$PROMPT_TIMEOUT_SEC"; then
  mark_fail "PROMPT" "shell prompt not detected"
fi
mark_pass "PROMPT"

CD_CASE_FAIL_PATTERN='cd[[:space:]]+err=0x|ccdd.*00xx'
send_text_and_enter "$MON_SOCK" "$CMD_LOG" 'cd \Apps' || mark_fail "SEND_CASE_REJECT" "cannot send mixed-case cd command"
if ! wait_for_regex "$SERIAL_LOG" "$CD_CASE_FAIL_PATTERN" 20; then
  mark_fail "PATH_CASE_REJECT" "mixed-case path unexpectedly resolved or error marker missing"
fi
mark_pass "PATH_CASE_REJECT"
if ! wait_for_shell_prompt "$SERIAL_LOG" 30; then
  mark_fail "PROMPT_AFTER_CASE_REJECT" "shell prompt missing after mixed-case path rejection"
fi
mark_pass "PROMPT_AFTER_CASE_REJECT"

send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$DRVLOAD_COMMAND" || mark_fail "SEND_COMMAND" "cannot send DRVLOAD command"
mark_pass "SEND_COMMAND"

if ! wait_for_regex "$SERIAL_LOG" "$DRVLOAD_BEGIN_PATTERN" "$MARKER_TIMEOUT_SEC"; then
  mark_fail "DRVLOAD_BEGIN" "missing [DRVLOAD] BEGIN marker"
fi
mark_pass "DRVLOAD_BEGIN"

if ! wait_for_regex "$SERIAL_LOG" "$DRVLOAD_TRY_PATTERN" "$MARKER_TIMEOUT_SEC"; then
  mark_fail "DRVLOAD_TRY" "missing [DRVLOAD] TRY marker"
fi
mark_pass "DRVLOAD_TRY"

if ! wait_for_done_with_nudge "$SERIAL_LOG" "$DRVLOAD_DONE_PATTERN" "$MARKER_TIMEOUT_SEC" "$MON_SOCK" "$CMD_LOG"; then
  mark_fail "DRVLOAD_DONE" "missing [DRVLOAD] DONE marker"
fi
mark_pass "DRVLOAD_DONE"

if ! wait_for_shell_prompt "$SERIAL_LOG" 60; then
  mark_fail "PROMPT_RETURN" "shell prompt did not return after DRVLOAD"
fi
mark_pass "PROMPT_RETURN"

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

if ! grep -Eiq "$DRVLOAD_BEGIN_PATTERN" "$STRINGS_LOG"; then
  mark_fail "STRINGS_BEGIN" "BEGIN marker missing in strings log"
fi
if ! grep -Eiq "$DRVLOAD_DONE_PATTERN" "$STRINGS_LOG"; then
  mark_fail "STRINGS_DONE" "DONE marker missing in strings log"
fi
if ! grep -Eiq "$DRVLOAD_TRY_PATTERN" "$STRINGS_LOG"; then
  mark_fail "STRINGS_TRY" "TRY marker missing in strings log"
fi

if [[ " $DRVLOAD_ARGS " == *" /AUDIO "* ]]; then
  if ! grep -Eiq "\[SB16INIT\][[:space:]]+DSP[[:space:]]+OK|\[{1,2}SSBB1166IINNIITT\]\][[:space:]]+DDSSPP[[:space:]]+OOKK" "$STRINGS_LOG"; then
    mark_fail "STRINGS_AUDIO_DSP" "SB16 DSP OK marker missing in strings log"
  fi
  if ! grep -Eiq "\[SB16INIT\][[:space:]]+TONE[[:space:]]+DONE|\[{1,2}SSBB1166IINNIITT\]\][[:space:]]+TTOONNEE[[:space:]]+DDOONNE" "$STRINGS_LOG"; then
    mark_fail "STRINGS_AUDIO_TONE" "SB16 tone done marker missing in strings log"
  fi
  if ! grep -Eiq "\[DRVLOAD\][[:space:]]+OK[[:space:]]+AUDIO|\[{1,2}DDRRVVLLOOAADD\]\][[:space:]]+OOKK[[:space:]]+AAUUDDIIO" "$STRINGS_LOG"; then
    mark_fail "STRINGS_AUDIO_OK" "DRVLOAD OK AUDIO marker missing in strings log"
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
  echo "MARKER_TIMEOUT_SEC=$MARKER_TIMEOUT_SEC"
  echo "QEMU_TIMEOUT_SEC=$QEMU_TIMEOUT_SEC"
  echo "QEMU_AUDIO_MODE=$QEMU_AUDIO_MODE"
  echo "QEMU_AUDIO_DETAIL=$QEMU_AUDIO_DETAIL"
} > "$META_LOG"

echo "[drvload-smoke] PASS (BEGIN/TRY/DONE markers verified)"
