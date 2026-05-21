#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
IMG="${IMG:-build/full/ciukios-full.img}"
LOG_FILE="${LOG_FILE:-build/full/qemu-full-wolf3d-exec-probe.log}"
QEMU_STDERR="${QEMU_STDERR:-build/full/qemu-full-wolf3d-exec-probe.stderr.log}"
QEMU_CMD_LOG="${QEMU_CMD_LOG:-build/full/qemu-full-wolf3d-exec-probe.commands.log}"
QEMU_MON_SOCK="${QEMU_MON_SOCK:-/tmp/ciukios-wolf3d-exec-probe.monitor.sock}"
QEMU_TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-90}"
PROMPT_TIMEOUT_SEC="${PROMPT_TIMEOUT_SEC:-120}"
OBSERVE_SEC="${OBSERVE_SEC:-12}"
KEY_DELAY_SEC="${KEY_DELAY_SEC:-0.12}"
PRE_ENTER_DELAY_SEC="${PRE_ENTER_DELAY_SEC:-0.35}"

command_exists() {
  command -v "$1" >/dev/null 2>&1
}

pick_qemu() {
  if [[ -n "${QEMU_BIN:-}" ]]; then
    echo "$QEMU_BIN"
    return 0
  fi
  if command_exists qemu-system-i386; then
    echo qemu-system-i386
    return 0
  fi
  if command_exists qemu-system-x86_64; then
    echo qemu-system-x86_64
    return 0
  fi
  return 1
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

file_size() {
  if [[ -f "$1" ]]; then
    wc -c < "$1"
  else
    echo 0
  fi
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
  return "$rc"
}

hmp_capture() {
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
    printf '%s\n' "$out"
  fi
  echo "[HMP_RC] $cmd => $rc" >> "$cmd_log"
  return "$rc"
}

send_key() {
  local sock="$1"
  local cmd_log="$2"
  local key="$3"
  hmp "$sock" "$cmd_log" "sendkey $key" >/dev/null 2>&1 || return 1
  if [[ "$KEY_DELAY_SEC" != "0" ]]; then
    sleep "$KEY_DELAY_SEC"
  fi
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
      '\\') key="backslash" ;;
      '-') key="minus" ;;
      ':') key="shift-semicolon" ;;
      [A-Z]) key="shift-$(printf '%s' "$ch" | tr 'A-Z' 'a-z')" ;;
      [a-z0-9]) key="$ch" ;;
      *) continue ;;
    esac
    send_key "$sock" "$cmd_log" "$key" || return 1
  done

  if [[ "$PRE_ENTER_DELAY_SEC" != "0" ]]; then
    sleep "$PRE_ENTER_DELAY_SEC"
  fi

  send_key "$sock" "$cmd_log" ret || return 1
}

extract_exit_code() {
  local log_file="$1"
  local line
  line="$(strings -a "$log_file" | grep -Eo 'CHILD_EXIT[^[:cntrl:]]* code=[0-9A-F]+' | tail -n 1 || true)"
  echo "${line##*code=}"
}

extract_first_post_exec_marker() {
  local log_file="$1"
  strings -a "$log_file" | awk '
    /CHILD_EXEC_REQ/ {seen=1; next}
    !seen {next}
    {
      if (match($0, /CHILD_PREJUMP|CHILD_EXIT|I16I|I10I|CH44I|CH44O|CH4A|CH40|CH4C/)) {
        print substr($0, RSTART)
        exit
      }
    }
  '
}

extract_first_post_exec_bios() {
  local log_file="$1"
  strings -a "$log_file" | awk '
    /CHILD_EXEC_REQ/ {seen=1; next}
    !seen {next}
    {
      if (match($0, /I16I|I10I/)) {
        print substr($0, RSTART)
        exit
      }
    }
  '
}

extract_first_post_exec_int21() {
  local log_file="$1"
  strings -a "$log_file" | awk '
    /CHILD_EXEC_REQ/ {seen=1; next}
    !seen {next}
    {
      if (match($0, /CH44I|CH44O|CH4A|CH40|CH4C/)) {
        print substr($0, RSTART)
        exit
      }
    }
  '
}

capture_video_text() {
  local sock="$1"
  local cmd_log="$2"
  local dump_file
  local row_count="${VIDEO_TEXT_ROWS:-12}"
  local byte_count=$((80 * row_count * 2))

  dump_file="$(mktemp)"
  if ! hmp_capture "$sock" "$cmd_log" "xp /${byte_count}bx 0xb8000" > "$dump_file"; then
    rm -f "$dump_file"
    return 1
  fi

  awk -v max_rows="$row_count" '
    /:/ {
      for (i = 2; i <= NF; i++) {
        if ($i ~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) {
          bytes[++count] = $i
        }
      }
    }
    END {
      row = 0
      col = 0
      line = ""
      for (i = 1; i <= count && row < max_rows; i += 2) {
        value = strtonum("0x" bytes[i])
        ch = (value >= 32 && value <= 126) ? sprintf("%c", value) : " "
        line = line ch
        col++
        if (col == 80) {
          gsub(/  +/, " ", line)
          sub(/^ +/, "", line)
          sub(/ +$/, "", line)
          print line
          row++
          line = ""
          col = 0
        }
      }
    }
  ' "$dump_file"

  rm -f "$dump_file"
}

format_child_marker() {
  local line="$1"
  if [[ -z "$line" ]]; then
    echo NONE
    return 0
  fi
  case "$line" in
    I16I*)
      printf 'INT16 %s\n' "$line"
      ;;
    I10I*)
      printf 'INT10 %s\n' "$line"
      ;;
    CHILD_PREJUMP*)
      echo CHILD_PREJUMP
      ;;
    CHILD_EXIT*)
      if [[ "$line" =~ reason=([0-9A-F]{2}) ]]; then
        printf 'AH=%s\n' "${BASH_REMATCH[1]}"
      else
        echo CHILD_EXIT
      fi
      ;;
    CH[0-9A-F][0-9A-F]*)
      printf 'AH=%s\n' "${line:2:2}"
      ;;
    *)
      echo "$line"
      ;;
  esac
}

if [[ "$DO_BUILD" == "1" ]]; then
  CIUKIOS_TRACE_CHILD_INT21=1 make build-full
fi

if [[ ! -f "$IMG" ]]; then
  echo "[wolf-probe] ERROR missing image: $IMG" >&2
  exit 1
fi
if ! command_exists socat; then
  echo "[wolf-probe] ERROR socat not found" >&2
  exit 1
fi
QEMU_CMD="$(pick_qemu)" || {
  echo "[wolf-probe] ERROR qemu-system-i386/x86_64 not found" >&2
  exit 1
}

mkdir -p "$(dirname "$LOG_FILE")"
rm -f "$LOG_FILE" "$QEMU_STDERR" "$QEMU_CMD_LOG" "$QEMU_MON_SOCK"

cleanup() {
  local rc=$?
  if [[ -S "$QEMU_MON_SOCK" ]]; then
    hmp "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" quit >/dev/null 2>&1 || true
  fi
  rm -f "$QEMU_MON_SOCK"
  exit $rc
}
trap cleanup EXIT

set +e
timeout "$QEMU_TIMEOUT_SEC" "$QEMU_CMD" \
  -machine pc,vmport=off \
  -cpu pentium3 \
  -m 128 \
  -drive "file=$IMG,format=raw,if=ide" \
  -boot c \
  -nographic \
  -chardev "file,id=ser0,path=$LOG_FILE" \
  -serial chardev:ser0 \
  -monitor "unix:$QEMU_MON_SOCK,server,nowait" \
  -no-reboot \
  -no-shutdown >/dev/null 2>"$QEMU_STDERR" &
QEMU_PID=$!
set -e

wait_for_socket "$QEMU_MON_SOCK" 20 || {
  echo "[wolf-probe] ERROR monitor socket not ready" >&2
  exit 1
}

SHELL_PROMPT_PREFIX='C+I+U+K+I+O+S+[[:space:]]+S+H+E+L+L+[[:space:]]+'
APPS_PROMPT_PATTERN="${SHELL_PROMPT_PREFIX}C+[:]+[\\]+A+P+P+S+>+"
WOLF_PROMPT_PATTERN="${SHELL_PROMPT_PREFIX}C+[:]+[\\]+A+P+P+S+[\\]+W+O+L+F+3+D+>+"
WOLF_CWD_PATTERN='C+U+R+R+E+N+T+[[:space:]]+D+I+R+E+C+T+O+R+Y+[:]+[[:space:]]+C+[:]+[\\]+A+P+P+S+[\\]+W+O+L+F+3+D+'
WOLF_CMD_PATTERN='W+O+L+F+3+D+\.*E+X+E+'
COMMAND_NOT_FOUND_PATTERN='C+O+M+M+A+N+D+[:]+[[:space:]]+N+O+T+[[:space:]]+F+O+U+N+D+'
LOADER_RETURN_PATTERN='S+H+E+L+L+\.*C+O+M+[[:space:]]+R+E+T+U+R+N+E+D+[[:space:]]+C+O+N+T+R+O+L+'
TRACE_MARKER_PATTERN='CHILD_EXEC_REQ|CHILD_EXEC_RET|EXRT ax=|EXVL ok|CHILD_PREJUMP|CHILD_EXIT|I16I|I10I|CH44I|CH44O|CH4A|CH4AR|CH40|CH40R|CH35|CH25'

shell_prompt_reached=no
cd_wolf3d=no
wolf3d_submitted=no
external_lookup=no
ah4b_reached=no
child_exec_req=no
child_prejump=no
post_transfer_child_activity=no
child_transfer=no
wolf3d_exit_03=no
trace_markers_seen=no
cmd_not_found=no
loader_return=no
video_dump_available=no
video_text_available=no
video_text_after_exit=""

offset=0

wait_for_strings_regex_from_offset "$LOG_FILE" "$APPS_PROMPT_PATTERN" "$offset" "$PROMPT_TIMEOUT_SEC" || {
  echo "[wolf-probe] ERROR shell prompt not detected" >&2
  exit 1
}
shell_prompt_reached=yes

offset="$(file_size "$LOG_FILE")"
send_text_and_enter "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" 'cd WOLF3D'
if wait_for_strings_regex_from_offset "$LOG_FILE" "$WOLF_CWD_PATTERN" "$offset" 30 && \
   wait_for_strings_regex_from_offset "$LOG_FILE" "$WOLF_PROMPT_PATTERN" "$offset" 30; then
  cd_wolf3d=yes
fi

offset="$(file_size "$LOG_FILE")"
send_text_and_enter "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" 'WOLF3D.EXE'
if wait_for_strings_regex_from_offset "$LOG_FILE" "$WOLF_CMD_PATTERN" "$offset" 10; then
  wolf3d_submitted=yes
fi

start="$(date +%s)"
while kill -0 "$QEMU_PID" >/dev/null 2>&1; do
  now="$(date +%s)"
  if (( now - start >= OBSERVE_SEC )); then
    break
  fi
  sleep 1
done

if strings -a "$LOG_FILE" | grep -Eq 'CHILD_EXIT[^[:cntrl:]]* reason=4C code=03'; then
  if video_text_after_exit="$(capture_video_text "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" || true)"; then
    video_dump_available=yes
    if [[ -n "$video_text_after_exit" ]]; then
      video_text_available=yes
    fi
  fi
fi

hmp "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" quit >/dev/null 2>&1 || true
set +e
wait "$QEMU_PID"
QEMU_RC=$?
set -e

exec_return_error=no
child_int21=no
exit_code=""
first_blocker="none"
first_post_transfer_marker="NONE"
first_child_int21="NONE"
first_bios_marker="NONE"
int16_seen=no
int10_seen=no

if strings -a "$LOG_FILE" | grep -Eq "$TRACE_MARKER_PATTERN"; then
  trace_markers_seen=yes
fi
if strings -a "$LOG_FILE" | grep -Eq 'CHILD_EXEC_REQ.*WOLF3D(\.EXE)?|CHILD_EXEC_REQ.*path=.*WOLF3D'; then
  child_exec_req=yes
  external_lookup=yes
  ah4b_reached=yes
fi
if strings -a "$LOG_FILE" | grep -Eq 'CHILD_EXEC_RET cf=01|CHILD_EXEC_RET cf=1|EXRT ax='; then
  ah4b_reached=yes
  external_lookup=yes
fi
if strings -a "$LOG_FILE" | grep -Eq 'CHILD_EXEC_RET cf=01|CHILD_EXEC_RET cf=1'; then
  exec_return_error=yes
fi
if strings -a "$LOG_FILE" | grep -Eq "$COMMAND_NOT_FOUND_PATTERN"; then
  cmd_not_found=yes
fi
if strings -a "$LOG_FILE" | grep -Eq "$LOADER_RETURN_PATTERN"; then
  loader_return=yes
fi
if strings -a "$LOG_FILE" | grep -Eq 'CHILD_PREJUMP'; then
  child_prejump=yes
fi
first_post_transfer_marker="$(extract_first_post_exec_marker "$LOG_FILE")"
first_child_int21="$(extract_first_post_exec_int21 "$LOG_FILE")"
first_bios_marker="$(extract_first_post_exec_bios "$LOG_FILE")"
if [[ -n "$first_post_transfer_marker" ]]; then
  post_transfer_child_activity=yes
fi
if [[ -n "$first_child_int21" ]]; then
  child_int21=yes
fi
if strings -a "$LOG_FILE" | grep -Eq 'I16I'; then
  int16_seen=yes
fi
if strings -a "$LOG_FILE" | grep -Eq 'I10I'; then
  int10_seen=yes
fi
if [[ "$child_prejump" == yes || "$post_transfer_child_activity" == yes ]]; then
  child_transfer=yes
fi
exit_code="$(extract_exit_code "$LOG_FILE")"
if [[ "$exit_code" == 03 ]]; then
  wolf3d_exit_03=yes
fi

if [[ "$shell_prompt_reached" != yes ]]; then
  first_blocker='shell prompt not reached'
elif [[ "$cd_wolf3d" != yes ]]; then
  first_blocker='cd WOLF3D failed or prompt not observed'
elif [[ "$wolf3d_submitted" != yes ]]; then
  first_blocker='WOLF3D.EXE command not submitted'
elif [[ "$cmd_not_found" == yes ]]; then
  first_blocker='shell rejected WOLF3D.EXE command'
elif [[ "$trace_markers_seen" != yes ]]; then
  first_blocker='trace markers absent; probe requires traced image'
elif [[ "$external_lookup" != yes ]]; then
  first_blocker='external lookup not observed'
elif [[ "$ah4b_reached" != yes ]]; then
  first_blocker='AH=4Bh not observed'
elif [[ "$child_exec_req" != yes ]]; then
  first_blocker='CHILD_EXEC_REQ not observed'
elif [[ "$exec_return_error" == yes ]]; then
  first_blocker='INT21 4Bh returned error'
elif [[ "$child_transfer" != yes && "$loader_return" == yes ]]; then
  first_blocker='SHELL.COM returned control to loader before child transfer'
elif [[ "$child_transfer" != yes ]]; then
  first_blocker='child transfer not observed'
elif [[ -n "$exit_code" && "$exit_code" != 00 ]]; then
  first_blocker="child exited with code $exit_code"
elif [[ "$child_int21" != yes ]]; then
  first_blocker='child made no traced INT21 calls'
fi

printf 'shell prompt reached: %s\n' "${shell_prompt_reached^^}"
printf 'cd WOLF3D succeeded: %s\n' "${cd_wolf3d^^}"
printf 'WOLF3D.EXE submitted: %s\n' "${wolf3d_submitted^^}"
printf 'external lookup reached: %s\n' "${external_lookup^^}"
printf 'AH=4Bh reached: %s\n' "${ah4b_reached^^}"
printf 'CHILD_EXEC_REQ observed: %s\n' "${child_exec_req^^}"
printf 'CHILD_PREJUMP observed: %s\n' "${child_prejump^^}"
printf 'post-transfer child activity observed: %s\n' "${post_transfer_child_activity^^}"
printf 'first post-transfer marker: %s\n' "$(format_child_marker "$first_post_transfer_marker")"
printf 'first child INT21 observed: %s\n' "$(format_child_marker "$first_child_int21")"
printf 'INT16 observed: %s\n' "${int16_seen^^}"
printf 'INT10 observed: %s\n' "${int10_seen^^}"
printf 'first BIOS marker: %s\n' "$(format_child_marker "$first_bios_marker")"
printf 'video memory dump available: %s\n' "${video_dump_available^^}"
printf 'decoded B8000 text available: %s\n' "${video_text_available^^}"
printf 'VIDEO_TEXT_AFTER_EXIT_BEGIN\n'
if [[ -n "$video_text_after_exit" ]]; then
  printf '%s\n' "$video_text_after_exit"
fi
printf 'VIDEO_TEXT_AFTER_EXIT_END\n'
printf 'child transfer reached: %s\n' "${child_transfer^^}"
printf 'WOLF3D exits code 03: %s\n' "${wolf3d_exit_03^^}"
printf 'next blocker: %s\n' "$first_blocker"
printf 'child INT21 observed: %s\n' "${child_int21^^}"
printf 'exit code: %s\n' "${exit_code:-NONE}"
printf 'qemu rc: %s\n' "$QEMU_RC"
printf 'serial log: %s\n' "$LOG_FILE"

if [[ "$first_blocker" != none ]]; then
  exit 1
fi
