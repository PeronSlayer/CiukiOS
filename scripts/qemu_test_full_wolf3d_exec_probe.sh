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
DEFAULT_PARENT_PSP_SEG="${DEFAULT_PARENT_PSP_SEG:-2000}"

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

extract_exit_reason() {
  local log_file="$1"
  local line
  line="$(strings -a "$log_file" | grep -Eo 'CHILD_EXIT[^[:cntrl:]]* reason=([0-9A-F]+|RETF)' | tail -n 1 || true)"
  echo "${line##*reason=}"
}

extract_int20_callsites() {
  local log_file="$1"
  strings -a "$log_file" | grep -Eo 'CH20IP [0-9A-F]{4}:[0-9A-F]{4}' || true
}

extract_exit_callsite() {
  local log_file="$1"
  local line
  line="$(strings -a "$log_file" | grep -Eo 'CHILD_EXIT[^[:cntrl:]]* CH4CIP [0-9A-F]{4}:[0-9A-F]{4}' | tail -n 1 || true)"
  echo "${line##*CH4CIP }"
}

hex16_sub2() {
  local value="$1"
  if [[ ! "$value" =~ ^[0-9A-Fa-f]{4}$ ]]; then
    echo NONE
    return 0
  fi
  printf '%04X\n' "$((((16#$value) - 2) & 0xFFFF))"
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

extract_last_child_psp() {
  local log_file="$1"
  local line

  line="$(strings -a "$log_file" | grep -Eo 'CHILD_EXIT[^[:cntrl:]]* psp=[0-9A-F]+' | tail -n 1 || true)"
  if [[ -n "$line" ]]; then
    echo "${line##*psp=}"
    return 0
  fi

  line="$(strings -a "$log_file" | grep -Eo 'CH4A[^[:cntrl:]]* psp=[0-9A-F]+' | tail -n 1 || true)"
  echo "${line##*psp=}"
}

extract_exec_req_psp() {
  local log_file="$1"
  local line

  line="$(strings -a "$log_file" | grep -Eo 'CHILD_EXEC_REQ[^[:cntrl:]]* psp=[0-9A-F]+' | tail -n 1 || true)"
  echo "${line##*psp=}"
}

extract_prejump_jft() {
  local log_file="$1"
  strings -a "$log_file" | awk '
    /JFTP / {
      line = $0
      sub(/^.*JFTP /, "", line)
      print line
      exit
    }
  '
}

extract_marker_sequence() {
  local log_file="$1"
  strings -a "$log_file" | awk '
    /CHILD_EXEC_REQ/ {seen=1}
    !seen {next}
    {
      line = $0
      while (match(line, /CHILD_EXEC_REQ[^[:cntrl:]]*|CHILD_PREJUMP|JFTP [0-9A-F ]+|CH4AR?[^[:cntrl:]]*|CH40R?[^[:cntrl:]]*|CH44[IO][^[:cntrl:]]*|I10[IO] [0-9A-F]{4}|I16[IO] [0-9A-F]{4}|CH4CIP [0-9A-F]{4}:[0-9A-F]{4}|CH20IP [0-9A-F]{4}:[0-9A-F]{4}|CHILD_EXIT[^[:cntrl:]]*/)) {
        print substr(line, RSTART, RLENGTH)
        line = substr(line, RSTART + RLENGTH)
      }
      if ($0 ~ /CHILD_TRACE_END/) {
        exit
      }
    }
  '
}

extract_int10_sequence() {
  local log_file="$1"
  strings -a "$log_file" | awk '
    /CHILD_EXEC_REQ/ {seen=1; next}
    !seen {next}
    {
      line = $0
      while (match(line, /I10[IO] [0-9A-F]{4}/)) {
        print substr(line, RSTART, RLENGTH)
        line = substr(line, RSTART + RLENGTH)
      }
      if ($0 ~ /CHILD_TRACE_END/) {
        exit
      }
    }
  '
}

extract_last_int21_before_exit() {
  local log_file="$1"
  strings -a "$log_file" | awk '
    /CHILD_EXEC_REQ/ {seen=1; next}
    !seen {next}
    /CHILD_EXIT/ {
      if (last != "") {
        print last
      }
      exit
    }
    {
      line = $0
      while (match(line, /CH4AR?[^[:cntrl:]]*|CH40R?[^[:cntrl:]]*|CH44[IO][^[:cntrl:]]*|CH35[^[:cntrl:]]*|CH25[^[:cntrl:]]*/)) {
        last = substr(line, RSTART, RLENGTH)
        line = substr(line, RSTART + RLENGTH)
      }
    }
  '
}

extract_last_marker_psp() {
  local log_file="$1"
  local marker_regex="$2"
  local line

  line="$(strings -a "$log_file" | grep -Eo "(${marker_regex})[^[:cntrl:]]* psp=[0-9A-F]+" | tail -n 1 || true)"
  if [[ -n "$line" && "$line" == *psp=* ]]; then
    echo "${line##*psp=}"
    return 0
  fi

  extract_last_child_psp "$log_file"
}

capture_physical_bytes() {
  local sock="$1"
  local cmd_log="$2"
  local phys_addr="$3"
  local byte_count="$4"
  local dump_file
  local phys_hex
  local count_dec

  dump_file="$(mktemp)"
  printf -v phys_hex '0x%X' "$phys_addr"
  count_dec=$((byte_count))
  if ! hmp_capture "$sock" "$cmd_log" "xp /${count_dec}bx ${phys_hex}" > "$dump_file"; then
    rm -f "$dump_file"
    return 1
  fi

  awk '
    /:/ {
      for (i = 2; i <= NF; i++) {
        token = $i
        gsub(/\r/, "", token)
        if (token ~ /^0x[0-9A-Fa-f][0-9A-Fa-f]$/) {
          bytes[++count] = toupper(substr(token, 3, 2))
        } else if (token ~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) {
          bytes[++count] = toupper(token)
        }
      }
    }
    END {
      for (i = 1; i <= count; i++) {
        if (i > 1) {
          printf " "
        }
        printf "%s", bytes[i]
      }
      printf "\n"
    }
  ' "$dump_file"

  rm -f "$dump_file"
}

read_le16_hex() {
  local -n bytes_ref="$1"
  local idx="$2"
  local lo="${bytes_ref[$idx]:-00}"
  local hi="${bytes_ref[$((idx + 1))]:-00}"
  printf '%04X' $((16#$lo + (16#$hi << 8)))
}

read_le16_dec() {
  local value
  value="$(read_le16_hex "$1" "$2")"
  printf '%d' $((16#$value))
}

read_le32_hex() {
  local -n bytes_ref="$1"
  local idx="$2"
  local b0="${bytes_ref[$idx]:-00}"
  local b1="${bytes_ref[$((idx + 1))]:-00}"
  local b2="${bytes_ref[$((idx + 2))]:-00}"
  local b3="${bytes_ref[$((idx + 3))]:-00}"
  printf '%08X' $((16#$b0 + (16#$b1 << 8) + (16#$b2 << 16) + (16#$b3 << 24)))
}

read_far_ptr() {
  local -n bytes_ref="$1"
  local idx="$2"
  local off seg
  local off_lo="${bytes_ref[$idx]:-00}"
  local off_hi="${bytes_ref[$((idx + 1))]:-00}"
  local seg_lo="${bytes_ref[$((idx + 2))]:-00}"
  local seg_hi="${bytes_ref[$((idx + 3))]:-00}"
  printf -v off '%04X' $((16#$off_lo + (16#$off_hi << 8)))
  printf -v seg '%04X' $((16#$seg_lo + (16#$seg_hi << 8)))
  printf '%s:%s' "$seg" "$off"
}

read_hex_range() {
  local -n bytes_ref="$1"
  local start="$2"
  local count="$3"
  local idx
  local out=""

  for ((idx = 0; idx < count; idx++)); do
    if (( idx > 0 )); then
      out+=" "
    fi
    out+="${bytes_ref[$((start + idx))]:-00}"
  done

  printf '%s' "$out"
}

decode_ascii_range() {
  local -n bytes_ref="$1"
  local start="$2"
  local count="$3"
  local idx value
  local out=""

  for ((idx = 0; idx < count; idx++)); do
    value=$((16#${bytes_ref[$((start + idx))]:-00}))
    if (( value >= 32 && value <= 126 )); then
      printf -v out '%s%b' "$out" "\\x$(printf '%02X' "$value")"
    else
      out+=" "
    fi
  done

  printf '%s' "$out" | sed -E 's/  +/ /g; s/^ +//; s/ +$//'
}

decode_env_strings() {
  local -n bytes_ref="$1"
  local idx value
  local current=""
  local out=""

  for ((idx = 0; idx < ${#bytes_ref[@]}; idx++)); do
    value=$((16#${bytes_ref[$idx]:-00}))
    if (( value == 0 )); then
      if [[ -z "$current" ]]; then
        break
      fi
      current="$(printf '%s' "$current" | sed -E 's/  +/ /g; s/^ +//; s/ +$//')"
      if [[ -n "$current" ]]; then
        if [[ -n "$out" ]]; then
          out+=$'\n'
        fi
        out+="$current"
      fi
      current=""
      continue
    fi

    if (( value >= 32 && value <= 126 )); then
      printf -v current '%s%b' "$current" "\\x$(printf '%02X' "$value")"
    else
      current+=" "
    fi
  done

  printf '%s' "$out"
}

append_reason() {
  local current="$1"
  local reason="$2"

  if [[ -z "$current" || "$current" == "NONE" ]]; then
    printf '%s' "$reason"
  else
    printf '%s, %s' "$current" "$reason"
  fi
}

capture_child_psp_snapshot() {
  local sock="$1"
  local cmd_log="$2"
  local psp_seg_hex="$3"
  local prefix="$4"
  local psp_phys
  local psp_bytes_raw
  local psp_jft
  local psp_jft_size
  local psp_jft_ptr
  local psp_env_seg
  local psp_field_003a
  local psp_term_vec
  local psp_ctrlc_vec
  local psp_crit_vec
  local psp_suspicious="NONE"
  local -a psp_bytes

  printf '%s_BEGIN\n' "$prefix"
  if [[ -z "$psp_seg_hex" || "$psp_seg_hex" == "NONE" ]]; then
    printf 'psp_seg=NONE\n'
    printf 'jft_20=NONE\n'
    printf 'jft_size=NONE\n'
    printf 'jft_ptr=NONE\n'
    printf 'env_seg=NONE\n'
    printf 'field_003A=NONE\n'
    printf 'term_vec=NONE\n'
    printf 'ctrlc_vec=NONE\n'
    printf 'crit_vec=NONE\n'
    printf 'suspicious=psp_unavailable\n'
    printf '%s_END\n' "$prefix"
    return 0
  fi

  psp_phys=$((16#$psp_seg_hex << 4))
  psp_bytes_raw="$(capture_physical_bytes "$sock" "$cmd_log" "$psp_phys" 0x200 || true)"
  read -r -a psp_bytes <<< "$psp_bytes_raw"
  if (( ${#psp_bytes[@]} < 0x3C )); then
    printf 'psp_seg=%s\n' "$psp_seg_hex"
    printf 'jft_20=NONE\n'
    printf 'jft_size=NONE\n'
    printf 'jft_ptr=NONE\n'
    printf 'env_seg=NONE\n'
    printf 'field_003A=NONE\n'
    printf 'term_vec=NONE\n'
    printf 'ctrlc_vec=NONE\n'
    printf 'crit_vec=NONE\n'
    printf 'suspicious=short_dump\n'
    printf '%s_END\n' "$prefix"
    return 0
  fi

  psp_jft="$(read_hex_range psp_bytes 0x18 20)"
  psp_jft_size="$(read_le16_dec psp_bytes 0x32)"
  psp_jft_ptr="$(read_far_ptr psp_bytes 0x34)"
  psp_env_seg="$(read_le16_hex psp_bytes 0x2C)"
  psp_field_003a="$(read_le16_hex psp_bytes 0x3A)"
  psp_term_vec="$(read_far_ptr psp_bytes 0x0A)"
  psp_ctrlc_vec="$(read_far_ptr psp_bytes 0x0E)"
  psp_crit_vec="$(read_far_ptr psp_bytes 0x12)"

  if [[ "$psp_jft" != 00\ 01\ 02\ 03\ 04* ]]; then
    psp_suspicious="$(append_reason "$psp_suspicious" "jft_layout_unexpected")"
  fi
  if [[ "$psp_jft_size" != "20" ]]; then
    psp_suspicious="$(append_reason "$psp_suspicious" "jft_size_${psp_jft_size}")"
  fi
  if [[ "$psp_jft_ptr" != "${psp_seg_hex}:0018" ]]; then
    psp_suspicious="$(append_reason "$psp_suspicious" "jft_ptr_${psp_jft_ptr}")"
  fi
  if [[ "$psp_env_seg" == "0000" || "$psp_env_seg" == "FFFF" ]]; then
    psp_suspicious="$(append_reason "$psp_suspicious" "env_seg_${psp_env_seg}")"
  fi
  if [[ "$psp_term_vec" == "0000:0000" || "$psp_term_vec" == "FFFF:FFFF" ]]; then
    psp_suspicious="$(append_reason "$psp_suspicious" "term_vec_${psp_term_vec}")"
  fi
  if [[ "$psp_ctrlc_vec" == "0000:0000" || "$psp_ctrlc_vec" == "FFFF:FFFF" ]]; then
    psp_suspicious="$(append_reason "$psp_suspicious" "ctrlc_vec_${psp_ctrlc_vec}")"
  fi
  if [[ "$psp_crit_vec" == "0000:0000" || "$psp_crit_vec" == "FFFF:FFFF" ]]; then
    psp_suspicious="$(append_reason "$psp_suspicious" "crit_vec_${psp_crit_vec}")"
  fi

  printf 'psp_seg=%s\n' "$psp_seg_hex"
  printf 'jft_20=%s\n' "$psp_jft"
  printf 'jft_size=%s\n' "$psp_jft_size"
  printf 'jft_ptr=%s\n' "$psp_jft_ptr"
  printf 'env_seg=%s\n' "$psp_env_seg"
  printf 'field_003A=%s\n' "$psp_field_003a"
  printf 'term_vec=%s\n' "$psp_term_vec"
  printf 'ctrlc_vec=%s\n' "$psp_ctrlc_vec"
  printf 'crit_vec=%s\n' "$psp_crit_vec"
  printf 'suspicious=%s\n' "$psp_suspicious"
  printf '%s_END\n' "$prefix"
}

capture_dgroup_guard_snapshot() {
  local sock="$1"
  local cmd_log="$2"
  local psp_seg_hex="$3"
  local prefix="$4"
  local psp_seg
  local entry_seg
  local dgroup_seg
  local dgroup_phys
  local dgroup_bytes_raw
  local dgroup_sum=0
  local dgroup_expected_sum="0D5C"
  local dgroup_bytes_2f="NONE"
  local dgroup_ascii="NONE"
  local dgroup_suspicious="NONE"
  local -a dgroup_bytes

  printf '%s_BEGIN\n' "$prefix"
  if [[ -z "$psp_seg_hex" || "$psp_seg_hex" == "NONE" ]]; then
    printf 'psp_seg=NONE\n'
    printf 'entry_seg=NONE\n'
    printf 'dgroup_seg=NONE\n'
    printf 'bytes_2f=NONE\n'
    printf 'ascii_2f=NONE\n'
    printf 'sum=NONE\n'
    printf 'expected_sum=%s\n' "$dgroup_expected_sum"
    printf 'suspicious=dgroup_unavailable\n'
    printf '%s_END\n' "$prefix"
    return 0
  fi

  psp_seg=$((16#$psp_seg_hex))
  entry_seg=$(((psp_seg + 0x0010) & 0xFFFF))
  dgroup_seg=$(((entry_seg + 0x3818) & 0xFFFF))
  dgroup_phys=$((dgroup_seg << 4))
  dgroup_bytes_raw="$(capture_physical_bytes "$sock" "$cmd_log" "$dgroup_phys" 0x2F || true)"
  read -r -a dgroup_bytes <<< "$dgroup_bytes_raw"
  if (( ${#dgroup_bytes[@]} < 0x2F )); then
    printf 'psp_seg=%s\n' "$psp_seg_hex"
    printf 'entry_seg=%04X\n' "$entry_seg"
    printf 'dgroup_seg=%04X\n' "$dgroup_seg"
    printf 'bytes_2f=NONE\n'
    printf 'ascii_2f=NONE\n'
    printf 'sum=NONE\n'
    printf 'expected_sum=%s\n' "$dgroup_expected_sum"
    printf 'suspicious=short_dump\n'
    printf '%s_END\n' "$prefix"
    return 0
  fi

  dgroup_bytes_2f="$(read_hex_range dgroup_bytes 0 0x2F)"
  dgroup_ascii="$(decode_ascii_range dgroup_bytes 0 0x2F)"
  for value_hex in "${dgroup_bytes[@]:0:0x2F}"; do
    dgroup_sum=$(((dgroup_sum + 16#$value_hex) & 0xFFFF))
  done
  printf -v dgroup_sum_hex '%04X' "$dgroup_sum"
  if [[ "$dgroup_sum_hex" != "$dgroup_expected_sum" ]]; then
    dgroup_suspicious="checksum_${dgroup_sum_hex}"
  fi

  printf 'psp_seg=%s\n' "$psp_seg_hex"
  printf 'entry_seg=%04X\n' "$entry_seg"
  printf 'dgroup_seg=%04X\n' "$dgroup_seg"
  printf 'bytes_2f=%s\n' "$dgroup_bytes_2f"
  printf 'ascii_2f=%s\n' "$dgroup_ascii"
  printf 'sum=%s\n' "$dgroup_sum_hex"
  printf 'expected_sum=%s\n' "$dgroup_expected_sum"
  printf 'suspicious=%s\n' "$dgroup_suspicious"
  printf '%s_END\n' "$prefix"
}

capture_marker_snapshot() {
  local sock="$1"
  local cmd_log="$2"
  local log_file="$3"
  local offset="$4"
  local wait_sec="$5"
  local marker_regex="$6"
  local marker_psp_regex="$7"
  local prefix="$8"
  local -n next_offset_ref="$9"
  local extra_prefix="${10:-}"
  local found_psp

  if ! wait_for_strings_regex_from_offset "$log_file" "$marker_regex" "$offset" "$wait_sec"; then
    next_offset_ref="$(file_size "$log_file")"
    if [[ -n "$extra_prefix" ]]; then
      capture_dgroup_guard_snapshot "$sock" "$cmd_log" "NONE" "$extra_prefix"
    fi
    capture_child_psp_snapshot "$sock" "$cmd_log" "NONE" "$prefix"
    return 1
  fi

  next_offset_ref="$(file_size "$log_file")"
  hmp "$sock" "$cmd_log" stop >/dev/null 2>&1 || true
  found_psp="$(extract_last_marker_psp "$log_file" "$marker_psp_regex")"
  if [[ -n "$extra_prefix" ]]; then
    capture_dgroup_guard_snapshot "$sock" "$cmd_log" "$found_psp" "$extra_prefix"
  fi
  capture_child_psp_snapshot "$sock" "$cmd_log" "$found_psp" "$prefix"
  hmp "$sock" "$cmd_log" cont >/dev/null 2>&1 || true
  return 0
}

capture_video_text() {
  local sock="$1"
  local cmd_log="$2"
  local byte_stream
  local dump_file
  local row_count="${VIDEO_TEXT_ROWS:-12}"
  local byte_count=$((80 * row_count * 2))

  if ! byte_stream="$(capture_physical_bytes "$sock" "$cmd_log" 0xB8000 "$byte_count")"; then
    return 1
  fi

  dump_file="$(mktemp)"
  printf '%s\n' "$byte_stream" > "$dump_file"

  awk -v max_rows="$row_count" '
    {
      for (i = 1; i <= NF; i++) {
        token = $i
        gsub(/\r/, "", token)
        if (token ~ /^0x[0-9A-Fa-f][0-9A-Fa-f]$/) {
          bytes[++count] = substr(token, 3, 2)
        } else if (token ~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) {
          bytes[++count] = token
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
bda_dump_available=no
bda_video_mode="NONE"
bda_columns="NONE"
bda_regen_size="NONE"
bda_active_page="NONE"
bda_crtc_base="NONE"
bda_rows_minus_one="NONE"
bda_char_height="NONE"
bda_equipment_word="NONE"
bda_memory_kb="NONE"
bda_timer_ticks="NONE"
bda_suspicious="NONE"
psp_dump_available=no
psp_seg="NONE"
psp_int20_sig="NONE"
psp_end_alloc_seg="NONE"
psp_parent_psp="NONE"
psp_env_seg="NONE"
psp_jft_size="NONE"
psp_jft_ptr="NONE"
psp_jft_20="NONE"
psp_cmd_tail_len="NONE"
psp_cmd_tail_ascii="NONE"
psp_term_vec="NONE"
psp_ctrlc_vec="NONE"
psp_crit_vec="NONE"
psp_suspicious="NONE"
parent_psp_dump_available=no
parent_psp_seg="$DEFAULT_PARENT_PSP_SEG"
parent_exec_req_psp="NONE"
parent_psp_int20_sig="NONE"
parent_psp_end_alloc_seg="NONE"
parent_psp_parent_psp="NONE"
parent_psp_env_seg="NONE"
parent_psp_jft_size="NONE"
parent_psp_jft_ptr="NONE"
parent_psp_jft_20="NONE"
parent_psp_cmd_tail_len="NONE"
parent_psp_cmd_tail_ascii="NONE"
parent_psp_term_vec="NONE"
parent_psp_ctrlc_vec="NONE"
parent_psp_crit_vec="NONE"
parent_psp_suspicious="NONE"
env_dump_available=no
env_strings=""
env_suspicious="NONE"
stack_dump_available=no
stack_expected_ss="NONE"
stack_sp="0080"
stack_first_words="NONE"
stack_ascii_snippet=""
stack_suspicious="NONE"
entry_dump_available=no
entry_expected_seg="NONE"
entry_expected_ip="0000"
entry_first_32_bytes="NONE"
entry_suspicious="NONE"
ivt_dump_available=no
ivt_int00="NONE"
ivt_int04="NONE"
ivt_int05="NONE"
ivt_int06="NONE"
ivt_int08="NONE"
ivt_int09="NONE"
ivt_int10="NONE"
ivt_int16="NONE"
ivt_int1A="NONE"
ivt_int20="NONE"
ivt_int21="NONE"
ivt_int23="NONE"
ivt_int24="NONE"
ivt_suspicious="NONE"
ivt_before_dump_available=no
ivt_before_int20="NONE"
ivt_before_int21="NONE"
ivt_before_int23="NONE"
ivt_before_int24="NONE"
ivt_before_suspicious="NONE"
prejump_jft_5="NONE"
marker_sequence=""
int10_sequence=""
first_int10_call="NONE"
last_int10_call="NONE"
final_int21_before_exit="NONE"
exit_reason="NONE"
int20_callsites=""
first_int20_callsite="NONE"
second_int20_callsite="NONE"
prompt_after_wolf3d=no
child_exit_seen=no
a000_dump_available=no
a000_nonzero="NONE"
a000_first_64="NONE"
exit_callsite_return="NONE"
exit_callsite_int21="NONE"
child_jft_at_4a_report=""
child_jft_after_40_1_report=""
child_jft_after_40_2_report=""
child_jft_after_exit_report=""
child_dgroup_after_exit_report=""

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

if [[ "$cd_wolf3d" == yes ]]; then
  parent_psp_phys=$((16#$parent_psp_seg << 4))
  if parent_psp_bytes_raw="$(capture_physical_bytes "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" "$parent_psp_phys" 0x200 || true)"; then
    read -r -a parent_psp_bytes <<< "$parent_psp_bytes_raw"
    if (( ${#parent_psp_bytes[@]} >= 0x100 )); then
      parent_psp_dump_available=yes
      parent_psp_int20_sig="${parent_psp_bytes[0]:-00}${parent_psp_bytes[1]:-00}"
      parent_psp_end_alloc_seg="$(read_le16_hex parent_psp_bytes 0x02)"
      parent_psp_term_vec="$(read_far_ptr parent_psp_bytes 0x0A)"
      parent_psp_ctrlc_vec="$(read_far_ptr parent_psp_bytes 0x0E)"
      parent_psp_crit_vec="$(read_far_ptr parent_psp_bytes 0x12)"
      parent_psp_parent_psp="$(read_le16_hex parent_psp_bytes 0x16)"
      parent_psp_jft_20="$(read_hex_range parent_psp_bytes 0x18 20)"
      parent_psp_env_seg="$(read_le16_hex parent_psp_bytes 0x2C)"
      parent_psp_jft_size="$(read_le16_dec parent_psp_bytes 0x32)"
      parent_psp_jft_ptr="$(read_far_ptr parent_psp_bytes 0x34)"
      parent_psp_cmd_tail_len="$((16#${parent_psp_bytes[0x80]:-00}))"
      if (( parent_psp_cmd_tail_len > 127 )); then
        parent_psp_cmd_tail_len=127
      fi
      parent_psp_cmd_tail_ascii="$(decode_ascii_range parent_psp_bytes 0x81 "$parent_psp_cmd_tail_len")"
      if [[ "$parent_psp_int20_sig" != "CD20" ]]; then
        parent_psp_suspicious="$(append_reason "$parent_psp_suspicious" "bad_int20_sig_${parent_psp_int20_sig}")"
      fi
      if (( 16#$parent_psp_end_alloc_seg <= 16#$parent_psp_seg )); then
        parent_psp_suspicious="$(append_reason "$parent_psp_suspicious" "end_alloc_seg_${parent_psp_end_alloc_seg}")"
      fi
      if [[ "$parent_psp_parent_psp" == "0000" || "$parent_psp_parent_psp" == "FFFF" ]]; then
        parent_psp_suspicious="$(append_reason "$parent_psp_suspicious" "parent_psp_${parent_psp_parent_psp}")"
      fi
      if [[ "$parent_psp_env_seg" == "0000" || "$parent_psp_env_seg" == "FFFF" ]]; then
        parent_psp_suspicious="$(append_reason "$parent_psp_suspicious" "env_seg_${parent_psp_env_seg}")"
      fi
      if [[ "$parent_psp_jft_20" != 00\ 01\ 02\ 03\ 04* ]]; then
        parent_psp_suspicious="$(append_reason "$parent_psp_suspicious" "jft_layout_unexpected")"
      fi
      if [[ "$parent_psp_term_vec" == "0000:0000" || "$parent_psp_term_vec" == "FFFF:FFFF" ]]; then
        parent_psp_suspicious="$(append_reason "$parent_psp_suspicious" "term_vec_${parent_psp_term_vec}")"
      fi
      if [[ "$parent_psp_ctrlc_vec" == "0000:0000" || "$parent_psp_ctrlc_vec" == "FFFF:FFFF" ]]; then
        parent_psp_suspicious="$(append_reason "$parent_psp_suspicious" "ctrlc_vec_${parent_psp_ctrlc_vec}")"
      fi
      if [[ "$parent_psp_crit_vec" == "0000:0000" || "$parent_psp_crit_vec" == "FFFF:FFFF" ]]; then
        parent_psp_suspicious="$(append_reason "$parent_psp_suspicious" "crit_vec_${parent_psp_crit_vec}")"
      fi
    fi
  fi

  if ivt_before_bytes_raw="$(capture_physical_bytes "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" 0x0000 0x100 || true)"; then
    read -r -a ivt_before_bytes <<< "$ivt_before_bytes_raw"
    if (( ${#ivt_before_bytes[@]} >= 0x94 )); then
      ivt_before_dump_available=yes
      ivt_before_int20="$(read_far_ptr ivt_before_bytes $((0x20 * 4)))"
      ivt_before_int21="$(read_far_ptr ivt_before_bytes $((0x21 * 4)))"
      ivt_before_int23="$(read_far_ptr ivt_before_bytes $((0x23 * 4)))"
      ivt_before_int24="$(read_far_ptr ivt_before_bytes $((0x24 * 4)))"
      for ivt_before_value in "$ivt_before_int20" "$ivt_before_int21" "$ivt_before_int23" "$ivt_before_int24"; do
        if [[ "$ivt_before_value" == "0000:0000" ]]; then
          ivt_before_suspicious="$(append_reason "$ivt_before_suspicious" "zero_vector")"
          break
        fi
        if [[ "$ivt_before_value" == "FFFF:FFFF" ]]; then
          ivt_before_suspicious="$(append_reason "$ivt_before_suspicious" "ffff_vector")"
          break
        fi
      done
    fi
  fi
fi

offset="$(file_size "$LOG_FILE")"
wolf_submit_offset="$offset"
send_text_and_enter "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" 'WOLF3D.EXE'
if wait_for_strings_regex_from_offset "$LOG_FILE" "$WOLF_CMD_PATTERN" "$offset" 10; then
  wolf3d_submitted=yes
fi

if [[ "$wolf3d_submitted" == yes ]]; then
  marker_offset="$offset"
  child_jft_at_4a_report="$(capture_marker_snapshot "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" "$LOG_FILE" "$marker_offset" "$OBSERVE_SEC" 'CH4A' 'CH4A' 'CHILD_JFT_AT_4A' marker_offset 'CHILD_DGROUP_AT_4A' || true)"
  child_jft_after_40_1_report="$(capture_marker_snapshot "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" "$LOG_FILE" "$marker_offset" "$OBSERVE_SEC" 'CH40' 'CH4A|CH40' 'CHILD_JFT_AFTER_40_1' marker_offset || true)"
  child_jft_after_40_2_report="$(capture_marker_snapshot "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" "$LOG_FILE" "$marker_offset" "$OBSERVE_SEC" 'CH40' 'CH4A|CH40' 'CHILD_JFT_AFTER_40_2' marker_offset || true)"
fi

start="$(date +%s)"
while kill -0 "$QEMU_PID" >/dev/null 2>&1; do
  now="$(date +%s)"
  if (( now - start >= OBSERVE_SEC )); then
    break
  fi
  sleep 1
done

if true; then
  if video_text_after_exit="$(capture_video_text "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" || true)"; then
    video_dump_available=yes
    if [[ -n "$video_text_after_exit" ]]; then
      video_text_available=yes
    fi
  fi

  child_psp_hex="$(extract_last_child_psp "$LOG_FILE")"
  if [[ -n "$child_psp_hex" ]]; then
    psp_seg="$child_psp_hex"

    if bda_bytes_raw="$(capture_physical_bytes "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" 0x400 0x100 || true)"; then
      read -r -a bda_bytes <<< "$bda_bytes_raw"
      if (( ${#bda_bytes[@]} >= 0x86 )); then
        bda_dump_available=yes
        bda_video_mode="${bda_bytes[73]:-00}"
        bda_columns="$(read_le16_dec bda_bytes 74)"
        bda_regen_size="$(read_le16_hex bda_bytes 76)"
        bda_active_page="${bda_bytes[98]:-00}"
        bda_crtc_base="$(read_le16_hex bda_bytes 99)"
        bda_rows_minus_one="$((16#${bda_bytes[132]:-00}))"
        bda_char_height="$((16#${bda_bytes[133]:-00}))"
        bda_equipment_word="$(read_le16_hex bda_bytes 16)"
        bda_memory_kb="$(read_le16_dec bda_bytes 19)"
        bda_timer_ticks="$(read_le32_hex bda_bytes 108)"
        if [[ "$bda_video_mode" != "03" ]]; then
          bda_suspicious="$(append_reason "$bda_suspicious" "video_mode_${bda_video_mode}")"
        fi
        if [[ "$bda_columns" != "80" ]]; then
          bda_suspicious="$(append_reason "$bda_suspicious" "columns_${bda_columns}")"
        fi
        if [[ "$bda_rows_minus_one" != "0" && "$bda_rows_minus_one" != "24" ]]; then
          bda_suspicious="$(append_reason "$bda_suspicious" "rows_minus_one_${bda_rows_minus_one}")"
        fi
        if [[ "$bda_crtc_base" != "03D4" && "$bda_crtc_base" != "03B4" ]]; then
          bda_suspicious="$(append_reason "$bda_suspicious" "crtc_base_${bda_crtc_base}")"
        fi
        if [[ "$bda_equipment_word" == "0000" ]]; then
          bda_suspicious="$(append_reason "$bda_suspicious" "equipment_word_zero")"
        fi
        if (( bda_memory_kb < 128 || bda_memory_kb > 640 )); then
          bda_suspicious="$(append_reason "$bda_suspicious" "memory_kb_${bda_memory_kb}")"
        fi
      fi
    fi

    if a000_bytes_raw="$(capture_physical_bytes "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" 0xA0000 0x100 || true)"; then
      read -r -a a000_bytes <<< "$a000_bytes_raw"
      if (( ${#a000_bytes[@]} >= 64 )); then
        a000_dump_available=yes
        a000_first_64="$(read_hex_range a000_bytes 0 64)"
        a000_nonzero=no
        for value_hex in "${a000_bytes[@]}"; do
          if [[ "$value_hex" != "00" ]]; then
            a000_nonzero=yes
            break
          fi
        done
      fi
    fi

    psp_phys=$((16#$child_psp_hex << 4))
    if psp_bytes_raw="$(capture_physical_bytes "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" "$psp_phys" 0x200 || true)"; then
      read -r -a psp_bytes <<< "$psp_bytes_raw"
      if (( ${#psp_bytes[@]} >= 0x100 )); then
        child_jft_after_exit_report="$(capture_child_psp_snapshot "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" "$child_psp_hex" 'CHILD_JFT_AFTER_EXIT')"
        child_dgroup_after_exit_report="$(capture_dgroup_guard_snapshot "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" "$child_psp_hex" 'CHILD_DGROUP_AFTER_EXIT')"
        psp_dump_available=yes
        psp_int20_sig="${psp_bytes[0]:-00}${psp_bytes[1]:-00}"
        psp_end_alloc_seg="$(read_le16_hex psp_bytes 0x02)"
        psp_term_vec="$(read_far_ptr psp_bytes 0x0A)"
        psp_ctrlc_vec="$(read_far_ptr psp_bytes 0x0E)"
        psp_crit_vec="$(read_far_ptr psp_bytes 0x12)"
        psp_parent_psp="$(read_le16_hex psp_bytes 0x16)"
        psp_jft_20="$(read_hex_range psp_bytes 0x18 20)"
        psp_env_seg="$(read_le16_hex psp_bytes 0x2C)"
        psp_jft_size="$(read_le16_dec psp_bytes 0x32)"
        psp_jft_ptr="$(read_far_ptr psp_bytes 0x34)"
        psp_cmd_tail_len="$((16#${psp_bytes[0x80]:-00}))"
        if (( psp_cmd_tail_len > 127 )); then
          psp_cmd_tail_len=127
        fi
        psp_cmd_tail_ascii="$(decode_ascii_range psp_bytes 0x81 "$psp_cmd_tail_len")"
        if [[ "$psp_int20_sig" != "CD20" ]]; then
          psp_suspicious="$(append_reason "$psp_suspicious" "bad_int20_sig_${psp_int20_sig}")"
        fi
        if (( 16#$psp_end_alloc_seg <= 16#$child_psp_hex )); then
          psp_suspicious="$(append_reason "$psp_suspicious" "end_alloc_seg_${psp_end_alloc_seg}")"
        fi
        if [[ "$psp_parent_psp" == "0000" || "$psp_parent_psp" == "FFFF" ]]; then
          psp_suspicious="$(append_reason "$psp_suspicious" "parent_psp_${psp_parent_psp}")"
        fi
        if [[ "$psp_env_seg" == "0000" || "$psp_env_seg" == "FFFF" ]]; then
          psp_suspicious="$(append_reason "$psp_suspicious" "env_seg_${psp_env_seg}")"
        fi
        if [[ "$psp_jft_20" != 00\ 01\ 02\ 03\ 04* ]]; then
          psp_suspicious="$(append_reason "$psp_suspicious" "jft_layout_unexpected")"
        fi
        if [[ "$psp_term_vec" == "0000:0000" || "$psp_term_vec" == "FFFF:FFFF" ]]; then
          psp_suspicious="$(append_reason "$psp_suspicious" "term_vec_${psp_term_vec}")"
        fi
        if [[ "$psp_ctrlc_vec" == "0000:0000" || "$psp_ctrlc_vec" == "FFFF:FFFF" ]]; then
          psp_suspicious="$(append_reason "$psp_suspicious" "ctrlc_vec_${psp_ctrlc_vec}")"
        fi
        if [[ "$psp_crit_vec" == "0000:0000" || "$psp_crit_vec" == "FFFF:FFFF" ]]; then
          psp_suspicious="$(append_reason "$psp_suspicious" "crit_vec_${psp_crit_vec}")"
        fi
      fi
    fi

    if [[ "$psp_env_seg" != "NONE" && "$psp_env_seg" != "0000" && "$psp_env_seg" != "FFFF" ]]; then
      env_phys=$((16#$psp_env_seg << 4))
      if env_bytes_raw="$(capture_physical_bytes "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" "$env_phys" 0x200 || true)"; then
        read -r -a env_bytes <<< "$env_bytes_raw"
        if (( ${#env_bytes[@]} > 0 )); then
          env_dump_available=yes
          env_strings="$(decode_env_strings env_bytes)"
          if [[ -z "$env_strings" ]]; then
            env_suspicious="$(append_reason "$env_suspicious" "env_not_decoded")"
          fi
          if (( 16#$psp_env_seg < 0x0100 || 16#$psp_env_seg >= 0xA000 )); then
            env_suspicious="$(append_reason "$env_suspicious" "env_seg_${psp_env_seg}")"
          fi
        fi
      fi
    fi

    reloc_base_seg=$((16#$child_psp_hex + 0x10))
    expected_ss=$((reloc_base_seg + 0x46AE))
    stack_expected_ss="$(printf '%04X' "$expected_ss")"
    stack_focus_phys=$(((expected_ss << 4) + 0x80 - 0x80))
    if stack_bytes_raw="$(capture_physical_bytes "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" "$stack_focus_phys" 0x100 || true)"; then
      read -r -a stack_bytes <<< "$stack_bytes_raw"
      if (( ${#stack_bytes[@]} >= 32 )); then
        stack_dump_available=yes
        stack_first_words="$(read_hex_range stack_bytes 0 32)"
        stack_ascii_snippet="$(decode_ascii_range stack_bytes 0 64)"
        if [[ "$stack_first_words" == "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00" ]]; then
          stack_suspicious="$(append_reason "$stack_suspicious" "stack_all_zero")"
        fi
        if [[ "$bda_memory_kb" != "NONE" ]] && (( expected_ss >= bda_memory_kb * 64 )); then
          stack_suspicious="$(append_reason "$stack_suspicious" "stack_above_conventional_top")"
        fi
      fi
    fi

    entry_expected_seg="$(printf '%04X' "$reloc_base_seg")"
    entry_phys=$((reloc_base_seg << 4))
    if entry_bytes_raw="$(capture_physical_bytes "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" "$entry_phys" 0x100 || true)"; then
      read -r -a entry_bytes <<< "$entry_bytes_raw"
      if (( ${#entry_bytes[@]} >= 32 )); then
        entry_dump_available=yes
        entry_first_32_bytes="$(read_hex_range entry_bytes 0 32)"
        if [[ "$entry_first_32_bytes" == "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00" ]]; then
          entry_suspicious="$(append_reason "$entry_suspicious" "entry_all_zero")"
        elif [[ "${entry_bytes[0]:-00}${entry_bytes[1]:-00}" == "4D5A" ]]; then
          entry_suspicious="$(append_reason "$entry_suspicious" "entry_starts_with_mz")"
        fi
      fi
    fi

    if ivt_bytes_raw="$(capture_physical_bytes "$QEMU_MON_SOCK" "$QEMU_CMD_LOG" 0x0000 0x100 || true)"; then
      read -r -a ivt_bytes <<< "$ivt_bytes_raw"
      if (( ${#ivt_bytes[@]} >= 0x98 )); then
        ivt_dump_available=yes
        ivt_int00="$(read_far_ptr ivt_bytes $((0x00 * 4)))"
        ivt_int04="$(read_far_ptr ivt_bytes $((0x04 * 4)))"
        ivt_int05="$(read_far_ptr ivt_bytes $((0x05 * 4)))"
        ivt_int06="$(read_far_ptr ivt_bytes $((0x06 * 4)))"
        ivt_int08="$(read_far_ptr ivt_bytes $((0x08 * 4)))"
        ivt_int09="$(read_far_ptr ivt_bytes $((0x09 * 4)))"
        ivt_int10="$(read_far_ptr ivt_bytes $((0x10 * 4)))"
        ivt_int16="$(read_far_ptr ivt_bytes $((0x16 * 4)))"
        ivt_int1A="$(read_far_ptr ivt_bytes $((0x1A * 4)))"
        ivt_int20="$(read_far_ptr ivt_bytes $((0x20 * 4)))"
        ivt_int21="$(read_far_ptr ivt_bytes $((0x21 * 4)))"
        ivt_int23="$(read_far_ptr ivt_bytes $((0x23 * 4)))"
        ivt_int24="$(read_far_ptr ivt_bytes $((0x24 * 4)))"
        for ivt_value in "$ivt_int00" "$ivt_int04" "$ivt_int05" "$ivt_int06" "$ivt_int08" "$ivt_int09" "$ivt_int10" "$ivt_int16" "$ivt_int1A" "$ivt_int20" "$ivt_int21" "$ivt_int23" "$ivt_int24"; do
          if [[ "$ivt_value" == "0000:0000" ]]; then
            ivt_suspicious="$(append_reason "$ivt_suspicious" "zero_vector")"
            break
          fi
          if [[ "$ivt_value" == "FFFF:FFFF" ]]; then
            ivt_suspicious="$(append_reason "$ivt_suspicious" "ffff_vector")"
            break
          fi
        done
        if [[ "$ivt_int23" == ${child_psp_hex}:* || "$ivt_int24" == ${child_psp_hex}:* ]]; then
          ivt_suspicious="$(append_reason "$ivt_suspicious" "child_psp_vector_not_restored")"
        fi
      fi
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
exit_reason="$(extract_exit_reason "$LOG_FILE")"
parent_exec_req_psp="$(extract_exec_req_psp "$LOG_FILE")"
prejump_jft_5="$(extract_prejump_jft "$LOG_FILE")"
marker_sequence="$(extract_marker_sequence "$LOG_FILE")"
int10_sequence="$(extract_int10_sequence "$LOG_FILE")"
if [[ -n "$int10_sequence" ]]; then
  first_int10_call="$(printf '%s\n' "$int10_sequence" | head -n 1)"
  last_int10_call="$(printf '%s\n' "$int10_sequence" | tail -n 1)"
fi
int20_callsites="$(extract_int20_callsites "$LOG_FILE")"
if [[ -n "$int20_callsites" ]]; then
  first_int20_callsite="$(printf '%s\n' "$int20_callsites" | sed -n '1s/^CH20IP //p')"
  second_int20_callsite="$(printf '%s\n' "$int20_callsites" | sed -n '2s/^CH20IP //p')"
  if [[ -z "$second_int20_callsite" ]]; then
    second_int20_callsite="NONE"
  fi
fi
final_int21_before_exit="$(extract_last_int21_before_exit "$LOG_FILE")"
exit_callsite_return="$(extract_exit_callsite "$LOG_FILE")"
if [[ "$exit_callsite_return" =~ ^([0-9A-F]{4}):([0-9A-F]{4})$ ]]; then
  exit_callsite_int21="${BASH_REMATCH[1]}:$(hex16_sub2 "${BASH_REMATCH[2]}")"
fi
if strings -a "$LOG_FILE" | grep -Eq 'CHILD_EXIT[^[:cntrl:]]*'; then
  child_exit_seen=yes
fi
if strings_from_offset "$LOG_FILE" "$wolf_submit_offset" | grep -Eiq "$WOLF_PROMPT_PATTERN|$APPS_PROMPT_PATTERN"; then
  prompt_after_wolf3d=yes
fi
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
printf 'shell prompt returned after WOLF3D: %s\n' "${prompt_after_wolf3d^^}"
printf 'child exit observed within window: %s\n' "${child_exit_seen^^}"
printf 'run window seconds: %s\n' "$OBSERVE_SEC"
printf 'MARKER_SEQUENCE_BEGIN\n'
if [[ -n "$marker_sequence" ]]; then
  printf '%s\n' "$marker_sequence"
fi
printf 'MARKER_SEQUENCE_END\n'
printf 'INT10_SEQUENCE_BEGIN\n'
if [[ -n "$int10_sequence" ]]; then
  printf '%s\n' "$int10_sequence"
fi
printf 'INT10_SEQUENCE_END\n'
printf 'first_int10=%s\n' "$first_int10_call"
printf 'last_int10=%s\n' "$last_int10_call"
printf 'first_int20_callsite=%s\n' "$first_int20_callsite"
printf 'second_int20_callsite=%s\n' "$second_int20_callsite"
printf 'final_int21_before_exit=%s\n' "$final_int21_before_exit"
printf 'video memory dump available: %s\n' "${video_dump_available^^}"
printf 'decoded B8000 text available: %s\n' "${video_text_available^^}"
printf 'VIDEO_TEXT_AFTER_EXIT_BEGIN\n'
if [[ -n "$video_text_after_exit" ]]; then
  printf '%s\n' "$video_text_after_exit"
fi
printf 'VIDEO_TEXT_AFTER_EXIT_END\n'
printf 'PARENT_PSP_BEFORE_EXEC_BEGIN\n'
printf 'snapshot_seg=%s\n' "$parent_psp_seg"
printf 'exec_req_psp=%s\n' "$parent_exec_req_psp"
printf 'int20_sig=%s\n' "$parent_psp_int20_sig"
printf 'end_alloc_seg=%s\n' "$parent_psp_end_alloc_seg"
printf 'parent_psp=%s\n' "$parent_psp_parent_psp"
printf 'env_seg=%s\n' "$parent_psp_env_seg"
printf 'jft_size=%s\n' "$parent_psp_jft_size"
printf 'jft_ptr=%s\n' "$parent_psp_jft_ptr"
printf 'jft_20=%s\n' "$parent_psp_jft_20"
printf 'cmd_tail_len=%s\n' "$parent_psp_cmd_tail_len"
printf 'cmd_tail_ascii=%s\n' "$parent_psp_cmd_tail_ascii"
printf 'term_vec=%s\n' "$parent_psp_term_vec"
printf 'ctrlc_vec=%s\n' "$parent_psp_ctrlc_vec"
printf 'crit_vec=%s\n' "$parent_psp_crit_vec"
printf 'suspicious=%s\n' "$parent_psp_suspicious"
printf 'PARENT_PSP_BEFORE_EXEC_END\n'
printf 'IVT_BEFORE_EXEC_BEGIN\n'
printf 'int20=%s\n' "$ivt_before_int20"
printf 'int21=%s\n' "$ivt_before_int21"
printf 'int23=%s\n' "$ivt_before_int23"
printf 'int24=%s\n' "$ivt_before_int24"
printf 'suspicious=%s\n' "$ivt_before_suspicious"
printf 'IVT_BEFORE_EXEC_END\n'
printf 'JFTP_PREJUMP_BEGIN\n'
printf 'jft_5=%s\n' "$prejump_jft_5"
printf 'JFTP_PREJUMP_END\n'
printf 'CH4CIP_EXIT_BEGIN\n'
printf 'reason=%s\n' "$exit_reason"
printf 'return_csip=%s\n' "$exit_callsite_return"
printf 'int21_csip=%s\n' "$exit_callsite_int21"
if [[ -n "$exit_callsite_int21" && "$exit_callsite_int21" != "NONE" ]]; then
  printf 'ax=%s\n' "4C${exit_code:-00}"
else
  printf 'ax=NONE\n'
fi
printf 'CH4CIP_EXIT_END\n'
printf '%s\n' "$child_jft_at_4a_report"
printf '%s\n' "$child_jft_after_40_1_report"
printf '%s\n' "$child_jft_after_40_2_report"
printf 'BDA_AFTER_EXIT_BEGIN\n'
printf 'video_mode=%s\n' "$bda_video_mode"
printf 'columns=%s\n' "$bda_columns"
printf 'regen_size=%s\n' "$bda_regen_size"
printf 'active_page=%s\n' "$bda_active_page"
printf 'crtc_base=%s\n' "$bda_crtc_base"
printf 'rows_minus_one=%s\n' "$bda_rows_minus_one"
printf 'char_height=%s\n' "$bda_char_height"
printf 'equipment_word=%s\n' "$bda_equipment_word"
printf 'memory_kb=%s\n' "$bda_memory_kb"
printf 'timer_ticks=%s\n' "$bda_timer_ticks"
printf 'suspicious=%s\n' "$bda_suspicious"
printf 'BDA_AFTER_EXIT_END\n'
printf 'A000_AFTER_RUN_BEGIN\n'
printf 'dump_available=%s\n' "${a000_dump_available^^}"
printf 'nonzero=%s\n' "$a000_nonzero"
printf 'first_64=%s\n' "$a000_first_64"
printf 'A000_AFTER_RUN_END\n'
printf 'PSP_AFTER_EXIT_BEGIN\n'
printf 'psp_seg=%s\n' "$psp_seg"
printf 'int20_sig=%s\n' "$psp_int20_sig"
printf 'end_alloc_seg=%s\n' "$psp_end_alloc_seg"
printf 'parent_psp=%s\n' "$psp_parent_psp"
printf 'env_seg=%s\n' "$psp_env_seg"
printf 'jft_size=%s\n' "$psp_jft_size"
printf 'jft_ptr=%s\n' "$psp_jft_ptr"
printf 'jft_20=%s\n' "$psp_jft_20"
printf 'cmd_tail_len=%s\n' "$psp_cmd_tail_len"
printf 'cmd_tail_ascii=%s\n' "$psp_cmd_tail_ascii"
printf 'term_vec=%s\n' "$psp_term_vec"
printf 'ctrlc_vec=%s\n' "$psp_ctrlc_vec"
printf 'crit_vec=%s\n' "$psp_crit_vec"
printf 'suspicious=%s\n' "$psp_suspicious"
printf 'PSP_AFTER_EXIT_END\n'
printf 'ENV_AFTER_EXIT_BEGIN\n'
if [[ -n "$env_strings" ]]; then
  printf '%s\n' "$env_strings"
fi
printf 'suspicious=%s\n' "$env_suspicious"
printf 'ENV_AFTER_EXIT_END\n'
printf 'STACK_AFTER_EXIT_BEGIN\n'
printf 'expected_ss=%s\n' "$stack_expected_ss"
printf 'sp=%s\n' "$stack_sp"
printf 'first_words=%s\n' "$stack_first_words"
printf 'ascii_snippet=%s\n' "$stack_ascii_snippet"
printf 'suspicious=%s\n' "$stack_suspicious"
printf 'STACK_AFTER_EXIT_END\n'
printf 'ENTRY_AFTER_EXIT_BEGIN\n'
printf 'expected_entry_seg=%s\n' "$entry_expected_seg"
printf 'expected_entry_ip=%s\n' "$entry_expected_ip"
printf 'first_32_bytes=%s\n' "$entry_first_32_bytes"
printf 'suspicious=%s\n' "$entry_suspicious"
printf 'ENTRY_AFTER_EXIT_END\n'
printf 'IVT_AFTER_EXIT_BEGIN\n'
printf 'int00=%s\n' "$ivt_int00"
printf 'int04=%s\n' "$ivt_int04"
printf 'int05=%s\n' "$ivt_int05"
printf 'int06=%s\n' "$ivt_int06"
printf 'int08=%s\n' "$ivt_int08"
printf 'int09=%s\n' "$ivt_int09"
printf 'int10=%s\n' "$ivt_int10"
printf 'int16=%s\n' "$ivt_int16"
printf 'int1A=%s\n' "$ivt_int1A"
printf 'int20=%s\n' "$ivt_int20"
printf 'int21=%s\n' "$ivt_int21"
printf 'int23=%s\n' "$ivt_int23"
printf 'int24=%s\n' "$ivt_int24"
printf 'suspicious=%s\n' "$ivt_suspicious"
printf 'IVT_AFTER_EXIT_END\n'
printf '%s\n' "$child_jft_after_exit_report"
printf '%s\n' "$child_dgroup_after_exit_report"
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
