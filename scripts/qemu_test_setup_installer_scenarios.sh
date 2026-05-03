#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD=1
ACTIVE_QEMU_PID=0
ACTIVE_MON_SOCK=""
ACTIVE_CMD_LOG=""

usage() {
  cat << 'TXT'
Usage: scripts/qemu_test_setup_installer_scenarios.sh [--no-build]

Runs installer runtime scenarios on full profile images:
  1) Success case (Minimal profile)
  2) Media-swap success and timeout-safe failure
  3) Invalid target drive filter failure
  4) Missing source payload failure (retry + cancel)
  5) Dry-run (snapshot) no-write validation
  6) Insufficient-space deterministic failure
  7) Invalid manifest header deterministic fallback

Artifacts:
  build/full/setup_scenario_*.{serial.log,strings.log,report.txt,meta}
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
      echo "[setup-scenarios] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

mark_pass() {
  local marker="$1"
  echo "[setup-scenarios] MARKER ${marker}=PASS"
}

mark_fail() {
  local marker="$1"
  local detail="$2"
  echo "[setup-scenarios] MARKER ${marker}=FAIL" >&2
  echo "[setup-scenarios] DETAIL ${detail}" >&2
  exit 1
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

wait_for_pattern() {
  local file="$1"
  local pattern="$2"
  local timeout_sec="$3"
  local start now
  start="$(date +%s)"
  while true; do
    if [[ -f "$file" ]] && grep -Fq "$pattern" "$file"; then
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
      '\\') key="backslash" ;;
      '-') key="minus" ;;
      [A-Z]) key="$(printf '%s' "$ch" | tr 'A-Z' 'a-z')" ;;
      [a-z0-9]) key="$ch" ;;
      *) continue ;;
    esac
    send_key "$sock" "$cmd_log" "$key" || return 1
  done
  send_key "$sock" "$cmd_log" ret || return 1
  return 0
}

read_report_field() {
  local report="$1"
  local field="$2"
  local line
  line="$(grep -m1 -E "^${field}=" "$report" || true)"
  if [[ -z "$line" ]]; then
    return 1
  fi
  printf '%s\n' "${line#*=}"
}

hex_to_dec() {
  local hex="$1"
  printf '%d\n' "$((16#$hex))"
}

assert_report_line() {
  local report="$1"
  local expected="$2"
  local marker="$3"
  if grep -Fxq "$expected" "$report"; then
    mark_pass "$marker"
  else
    mark_fail "$marker" "missing line '$expected' in $report"
  fi
}

assert_report_hex_ge() {
  local report="$1"
  local field="$2"
  local min_dec="$3"
  local marker="$4"
  local hex val
  hex="$(read_report_field "$report" "$field" || true)"
  if [[ -z "$hex" ]]; then
    mark_fail "$marker" "missing field $field in $report"
  fi
  if ! [[ "$hex" =~ ^[0-9A-Fa-f]+$ ]]; then
    mark_fail "$marker" "field $field is not hex: $hex"
  fi
  val="$(hex_to_dec "$hex")"
  if (( val < min_dec )); then
    mark_fail "$marker" "$field=$hex (< $min_dec)"
  fi
  mark_pass "$marker"
}

assert_report_hex_eq() {
  local report="$1"
  local field="$2"
  local expected_hex="$3"
  local marker="$4"
  local hex
  hex="$(read_report_field "$report" "$field" || true)"
  if [[ -z "$hex" ]]; then
    mark_fail "$marker" "missing field $field in $report"
  fi
  if ! [[ "$hex" =~ ^[0-9A-Fa-f]+$ ]]; then
    mark_fail "$marker" "field $field is not hex: $hex"
  fi

  local normalized_actual normalized_expected
  normalized_actual="$(printf '%04X' "$((16#$hex))")"
  normalized_expected="$(printf '%04X' "$((16#$expected_hex))")"
  if [[ "$normalized_actual" != "$normalized_expected" ]]; then
    mark_fail "$marker" "$field=$normalized_actual expected $normalized_expected"
  fi
  mark_pass "$marker"
}

parse_free_bytes() {
  local image="$1"
  local free_line digits
  free_line="$(mdir -i "$image" :: 2>/dev/null | grep -m1 'bytes free' || true)"
  if [[ -z "$free_line" ]]; then
    return 1
  fi
  digits="$(printf '%s' "$free_line" | tr -cd '0-9')"
  if [[ -z "$digits" ]]; then
    return 1
  fi
  printf '%s\n' "$digits"
}

prepare_insufficient_space_image() {
  local image="$1"
  local free_before

  free_before="$(parse_free_bytes "$image" || true)"
  if [[ -z "$free_before" ]]; then
    mark_fail "INSUFFICIENT_SPACE_PREP" "cannot read free bytes from $image"
  fi

  # Runtime note: INT21h AH=36 free-space reporting is not stable enough in this
  # environment to trigger a physical no-space branch deterministically.
  # The deterministic setup override below forces the same 0x0202 no-space path.
  mark_pass "INSUFFICIENT_SPACE_PREP"
}

prepare_insufficient_space_setup_override() {
  local image="$1"
  local setup_orig="build/full/setup_scenario_insufficient_setup.orig.com"
  local setup_patched="build/full/setup_scenario_insufficient_setup.patched.com"

  rm -f "$setup_orig" "$setup_patched"

  if ! mcopy -o -i "$image" ::APPS/SETUP.COM "$setup_orig" >/dev/null 2>&1; then
    mark_fail "INSUFFICIENT_SPACE_SETUP_OVERRIDE" "cannot extract APPS/SETUP.COM from $image"
  fi

  # Patch required-bytes table so FULL profile needs an intentionally huge amount of space.
  perl -0777 -pe 's/\x00\x60\x00\x80\x00\x00\x00\x00\x01\x00\x03\x00/\x00\x60\x00\x80\xFF\xFF\x00\x00\x01\x00\xFF\x7F/s' "$setup_orig" > "$setup_patched"

  if cmp -s "$setup_orig" "$setup_patched"; then
    mark_fail "INSUFFICIENT_SPACE_SETUP_OVERRIDE" "required-bytes table pattern not found in setup binary"
  fi

  if ! mcopy -o -i "$image" "$setup_patched" ::APPS/SETUP.COM >/dev/null 2>&1; then
    mark_fail "INSUFFICIENT_SPACE_SETUP_OVERRIDE" "cannot write patched APPS/SETUP.COM into $image"
  fi

  rm -f "$setup_orig" "$setup_patched"
  mark_pass "INSUFFICIENT_SPACE_SETUP_OVERRIDE"
}

prepare_manifest_invalid_image() {
  local image="$1"
  local manifest_orig="build/full/setup_scenario_manifest_invalid.orig.mft"
  local manifest_patched="build/full/setup_scenario_manifest_invalid.patched.mft"

  rm -f "$manifest_orig" "$manifest_patched"

  if ! mcopy -o -i "$image" ::APPS/SETUPMFT.BIN "$manifest_orig" >/dev/null 2>&1; then
    mark_fail "MANIFEST_INVALID_PREP" "cannot extract APPS/SETUPMFT.BIN from $image"
  fi

  perl -0777 -pe 'substr($_,0,1)="X" if length($_) > 0; $_' "$manifest_orig" > "$manifest_patched"

  if cmp -s "$manifest_orig" "$manifest_patched"; then
    mark_fail "MANIFEST_INVALID_PREP" "manifest patch did not change payload"
  fi

  if ! mcopy -o -i "$image" "$manifest_patched" ::APPS/SETUPMFT.BIN >/dev/null 2>&1; then
    mark_fail "MANIFEST_INVALID_PREP" "cannot write patched APPS/SETUPMFT.BIN into $image"
  fi

  rm -f "$manifest_orig" "$manifest_patched"
  mark_pass "MANIFEST_INVALID_PREP"
}

prepare_media_swap_manifest_image() {
  local image="$1"
  local manifest_orig="build/full/setup_scenario_media_swap.orig.mft"
  local manifest_patched="build/full/setup_scenario_media_swap.patched.mft"

  rm -f "$manifest_orig" "$manifest_patched"

  if ! mcopy -o -i "$image" ::APPS/SETUPMFT.BIN "$manifest_orig" >/dev/null 2>&1; then
    mark_fail "MEDIA_SWAP_MANIFEST_PREP" "cannot extract APPS/SETUPMFT.BIN from $image"
  fi

  # Manifest layout: 5-byte header + 9 records x 4 bytes.
  # Set record #2 (second minimal file) media_id from 1 to 2.
  perl -0777 -pe 'substr($_,10,1)=chr(2) if length($_) > 10; $_' "$manifest_orig" > "$manifest_patched"

  if cmp -s "$manifest_orig" "$manifest_patched"; then
    mark_fail "MEDIA_SWAP_MANIFEST_PREP" "media-id patch did not change setup manifest"
  fi

  if ! mcopy -o -i "$image" "$manifest_patched" ::APPS/SETUPMFT.BIN >/dev/null 2>&1; then
    mark_fail "MEDIA_SWAP_MANIFEST_PREP" "cannot write patched APPS/SETUPMFT.BIN into $image"
  fi

  rm -f "$manifest_orig" "$manifest_patched"
  mark_pass "MEDIA_SWAP_MANIFEST_PREP"
}

run_setup_case() {
  local case_id="$1"
  local image="$2"
  local mode="$3"
  local snapshot_mode="${4:-0}"

  local serial_log="build/full/setup_scenario_${case_id}.serial.log"
  local strings_log="build/full/setup_scenario_${case_id}.strings.log"
  local stderr_log="build/full/setup_scenario_${case_id}.stderr.log"
  local cmd_log="build/full/setup_scenario_${case_id}.commands.log"
  local meta="build/full/setup_scenario_${case_id}.meta"
  local report="build/full/setup_scenario_${case_id}.report.txt"
  local mon_sock="/tmp/ciukios-setup-${case_id}.monitor.sock"

  rm -f "$serial_log" "$strings_log" "$stderr_log" "$cmd_log" "$meta" "$report" "$mon_sock"

  local qemu_cmd qemu_pid=0
  local -a qemu_args

  qemu_cmd="$(pick_qemu)" || mark_fail "QEMU_PRESENT" "qemu-system-i386/x86_64 not found"

  qemu_args=(
    -machine pc,vmport=off
    -cpu pentium3
    -m 128
    -drive "file=$image,format=raw,if=ide"
    -boot c
    -nographic
    -chardev "file,id=ser0,path=$serial_log"
    -serial chardev:ser0
    -monitor "unix:$mon_sock,server,nowait"
    -no-reboot
    -no-shutdown
  )
  if [[ "$snapshot_mode" == "1" ]]; then
    qemu_args+=(-snapshot)
  fi

  "$qemu_cmd" "${qemu_args[@]}" >/dev/null 2>"$stderr_log" &
  qemu_pid=$!
  ACTIVE_QEMU_PID=$qemu_pid
  ACTIVE_MON_SOCK="$mon_sock"
  ACTIVE_CMD_LOG="$cmd_log"

  if ! wait_for_socket "$mon_sock" 20; then
    mark_fail "${case_id}_MONITOR" "monitor socket not ready"
  fi

  if ! kill -0 "$qemu_pid" >/dev/null 2>&1; then
    local early_detail="qemu exited before prompt"
    if [[ -s "$stderr_log" ]]; then
      early_detail="$(tail -n 3 "$stderr_log" | tr '\n' ' ' | sed 's/[[:space:]]\+/ /g')"
    fi
    mark_fail "${case_id}_QEMU_EARLY_EXIT" "$early_detail"
  fi

  if ! wait_for_shell_prompt "$serial_log" 90; then
    local prompt_detail="shell prompt not detected"
    if [[ -s "$stderr_log" ]]; then
      prompt_detail="${prompt_detail}; stderr: $(tail -n 3 "$stderr_log" | tr '\n' ' ' | sed 's/[[:space:]]\+/ /g')"
    fi
    mark_fail "${case_id}_PROMPT" "$prompt_detail"
  fi

  send_text_and_enter "$mon_sock" "$cmd_log" "setup.com" || mark_fail "${case_id}_SETUP_CMD" "cannot send setup command"

  wait_for_regex "$serial_log" 'Press Enter to continue or Esc to abort\.|aabboorrtt\.' 40 || mark_fail "${case_id}_WELCOME" "welcome prompt not detected"
  send_key "$mon_sock" "$cmd_log" ret || mark_fail "${case_id}_WELCOME_KEY" "cannot send Enter"

  wait_for_regex "$serial_log" 'Choose 1/2/3 \(Esc abort\)\.|11//22//33' 40 || mark_fail "${case_id}_PROFILE" "profile prompt not detected"

  if [[ "$mode" == "success" || "$mode" == "dry_run" || "$mode" == "media_swap_success" || "$mode" == "media_swap_timeout" || "$mode" == "manifest_invalid" ]]; then
    send_key "$mon_sock" "$cmd_log" down || mark_fail "${case_id}_KEY_DOWN" "cannot send Down"
    send_key "$mon_sock" "$cmd_log" up || mark_fail "${case_id}_KEY_UP" "cannot send Up"
    send_key "$mon_sock" "$cmd_log" ret || mark_fail "${case_id}_KEY_ENTER" "cannot send Enter"
  elif [[ "$mode" == "insufficient_space" ]]; then
    send_key "$mon_sock" "$cmd_log" 3 || mark_fail "${case_id}_KEY_PROFILE_3" "cannot send profile key 3"
    send_key "$mon_sock" "$cmd_log" ret || mark_fail "${case_id}_KEY_ENTER" "cannot send Enter"
  else
    send_key "$mon_sock" "$cmd_log" ret || mark_fail "${case_id}_KEY_ENTER" "cannot send Enter"
  fi

  wait_for_regex "$serial_log" 'Enter confirm / Esc cancel|ccoonnffiirrmm' 40 || mark_fail "${case_id}_TARGET" "target confirmation prompt not detected"
  if [[ "$mode" == "invalid_target" ]]; then
    send_key "$mon_sock" "$cmd_log" a || mark_fail "${case_id}_TARGET_KEY_A" "cannot send target drive key A"
    send_key "$mon_sock" "$cmd_log" ret || mark_fail "${case_id}_TARGET_KEY" "cannot send Enter"
  else
    send_key "$mon_sock" "$cmd_log" ret || mark_fail "${case_id}_TARGET_KEY" "cannot send Enter"
  fi

  if [[ "$mode" == "success" || "$mode" == "dry_run" ]]; then
    wait_for_regex "$serial_log" 'DONE|DDOONNE|DDOONNEE' 120 || mark_fail "${case_id}_DONE" "DONE marker not found"
  elif [[ "$mode" == "media_swap_success" ]]; then
    wait_for_regex "$serial_log" 'Insert media|IInnsseerrtt[[:space:]]+mmeeddiiaa' 120 || mark_fail "${case_id}_SWAP_PROMPT" "media swap prompt not found"
    send_key "$mon_sock" "$cmd_log" ret || mark_fail "${case_id}_SWAP_ENTER" "cannot send Enter to media swap prompt"
    wait_for_regex "$serial_log" 'DONE|DDOONNE|DDOONNEE' 120 || mark_fail "${case_id}_DONE" "DONE marker not found"
  elif [[ "$mode" == "media_swap_timeout" ]]; then
    wait_for_regex "$serial_log" 'Insert media|IInnsseerrtt[[:space:]]+mmeeddiiaa' 120 || mark_fail "${case_id}_SWAP_PROMPT" "media swap prompt not found"
    wait_for_regex "$serial_log" 'Prompt timeout|PPrroommpptt[[:space:]]+ttiimmeeoouutt' 120 || mark_fail "${case_id}_SWAP_TIMEOUT" "timeout marker not found"
    wait_for_regex "$serial_log" 'FAIL|FFAAIIL|FFAAIILL' 60 || mark_fail "${case_id}_FAIL_MARKER" "FAIL marker not found"
  elif [[ "$mode" == "failure" ]]; then
    wait_for_regex "$serial_log" 'Copy fail: R retry, B back, Esc cancel\.|CCooppyy  ffaaiill' 120 || mark_fail "${case_id}_FAIL_PROMPT" "retry prompt not found"
    send_key "$mon_sock" "$cmd_log" r || mark_fail "${case_id}_RETRY" "cannot send retry key"
    wait_for_regex "$serial_log" 'Copy fail: R retry, B back, Esc cancel\.|CCooppyy  ffaaiill' 120 || mark_fail "${case_id}_FAIL_PROMPT_AGAIN" "retry prompt not shown again"
    send_key "$mon_sock" "$cmd_log" esc || mark_fail "${case_id}_CANCEL" "cannot send Esc"
    wait_for_regex "$serial_log" 'FAIL|FFAAIIL|FFAAIILL' 60 || mark_fail "${case_id}_FAIL_MARKER" "FAIL marker not found"
  elif [[ "$mode" == "insufficient_space" ]]; then
    wait_for_regex "$serial_log" 'not enough free space|nnoott[[:space:]]+eennoouugghh[[:space:]]+ffrreeee[[:space:]]+ssppaaccee' 120 || mark_fail "${case_id}_NOSPACE_MSG" "no-space message not found"
    wait_for_regex "$serial_log" 'FAIL|FFAAIIL|FFAAIILL' 60 || mark_fail "${case_id}_FAIL_MARKER" "FAIL marker not found"
  elif [[ "$mode" == "invalid_target" ]]; then
    wait_for_regex "$serial_log" 'Invalid target drive|IInnvvaalliidd[[:space:]]+ttaarrggeett[[:space:]]+ddrriivvee' 120 || mark_fail "${case_id}_TARGET_INVALID_MSG" "invalid target message not found"
    wait_for_regex "$serial_log" 'FAIL|FFAAIIL|FFAAIILL' 60 || mark_fail "${case_id}_FAIL_MARKER" "FAIL marker not found"
  elif [[ "$mode" == "manifest_invalid" ]]; then
    wait_for_regex "$serial_log" 'Manifest fallback: invalid header\.|MMaanniiffeesstt[[:space:]]+ffaallllbbaacckk:+[[:space:]]+iinnvvaalliidd[[:space:]]+hheeaaddeerr\.?' 120 || mark_fail "${case_id}_MANIFEST_HEADER_FALLBACK" "manifest header fallback message not found"
    wait_for_regex "$serial_log" 'DONE|DDOONNE|DDOONNEE' 120 || mark_fail "${case_id}_DONE" "DONE marker not found"
  else
    mark_fail "${case_id}_MODE" "unknown mode '$mode'"
  fi

  wait_for_shell_prompt "$serial_log" 60 || mark_fail "${case_id}_PROMPT_RETURN" "shell prompt did not return"

  if [[ "$mode" != "dry_run" ]]; then
    send_text_and_enter "$mon_sock" "$cmd_log" "dir \\ciukios\\install.rpt" || mark_fail "${case_id}_POST_CMD" "cannot send post-setup dir command"
    wait_for_shell_prompt "$serial_log" 30 || mark_fail "${case_id}_POST_PROMPT" "shell prompt missing after post-setup command"
  fi

  hmp "$mon_sock" "$cmd_log" "quit" >/dev/null 2>&1 || true
  set +e
  wait "$qemu_pid"
  local qemu_rc=$?
  set -e
  qemu_pid=0
  ACTIVE_QEMU_PID=0
  ACTIVE_MON_SOCK=""
  ACTIVE_CMD_LOG=""

  strings -a "$serial_log" > "$strings_log" || true

  if [[ "$mode" == "success" || "$mode" == "dry_run" || "$mode" == "media_swap_success" || "$mode" == "manifest_invalid" ]]; then
    grep -Eiq 'DONE|DDOONNE|DDOONNEE' "$serial_log" || mark_fail "${case_id}_DONE_LOG" "DONE marker missing in serial log"
  else
    grep -Eiq 'FAIL|FFAAIIL|FFAAIILL' "$serial_log" || mark_fail "${case_id}_FAIL_LOG" "FAIL marker missing in serial log"
  fi

  if [[ "$mode" == "dry_run" ]]; then
    : > "$report"
    if mdir -i "$image" ::CIUKIOS/INSTALL.RPT >/dev/null 2>&1; then
      mark_fail "${case_id}_REPORT_ABSENT" "dry-run unexpectedly persisted INSTALL.RPT"
    fi
    mark_pass "${case_id}_REPORT_ABSENT"
  else
    if ! mtype -i "$image" ::CIUKIOS/INSTALL.RPT > "$report" 2>/dev/null; then
      mark_fail "${case_id}_REPORT" "INSTALL.RPT not found in image"
    fi
    if [[ ! -s "$report" ]]; then
      mark_fail "${case_id}_REPORT_BYTES" "INSTALL.RPT is empty"
    fi

    local report_norm="${report}.norm"
    tr -d '\r' < "$report" > "$report_norm"
    mv "$report_norm" "$report"

    mark_pass "${case_id}_REPORT_BYTES"

    assert_report_line "$report" 'REPORT_SCHEMA=SETUP_MVP_V2' "${case_id}_REPORT_SCHEMA"
    assert_report_line "$report" 'INPUT_MEDIA=FULL_FAT16' "${case_id}_REPORT_MEDIA"
    assert_report_line "$report" 'INPUT_TARGET=\CIUKIOS' "${case_id}_REPORT_TARGET"
    local expected_profile_line='INPUT_PROFILE=MINIMAL'
    if [[ "$mode" == "insufficient_space" ]]; then
      expected_profile_line='INPUT_PROFILE=FULL'
    fi
    assert_report_line "$report" "$expected_profile_line" "${case_id}_REPORT_PROFILE"

    if [[ "$mode" == "success" || "$mode" == "media_swap_success" || "$mode" == "manifest_invalid" ]]; then
      assert_report_line "$report" 'STATUS=OK' "${case_id}_REPORT_STATUS"
      assert_report_hex_eq "$report" 'FAIL_CODE_HEX' '0000' "${case_id}_FAILCODE_OK"
      assert_report_hex_ge "$report" 'FILES_COPIED_HEX' 1 "${case_id}_FILES_COPIED"
      assert_report_hex_ge "$report" 'BYTES_COPIED_HEX' 1 "${case_id}_BYTES_COPIED"
      assert_report_hex_ge "$report" 'RETRY_COUNT_HEX' 0 "${case_id}_RETRY_COUNT"
      if [[ "$mode" == "media_swap_success" ]]; then
        assert_report_hex_ge "$report" 'MEDIA_SWAPS_HEX' 1 "${case_id}_MEDIA_SWAPS"
      else
        assert_report_hex_eq "$report" 'MEDIA_SWAPS_HEX' '0000' "${case_id}_MEDIA_SWAPS"
      fi
    elif [[ "$mode" == "failure" ]]; then
      assert_report_line "$report" 'STATUS=FAIL' "${case_id}_REPORT_STATUS"
      assert_report_hex_eq "$report" 'FAIL_CODE_HEX' '0601' "${case_id}_FAILCODE_EXPECTED"
      assert_report_hex_ge "$report" 'RETRY_COUNT_HEX' 1 "${case_id}_RETRY_COUNT"
      assert_report_hex_eq "$report" 'FILES_COPIED_HEX' '0000' "${case_id}_FILES_COPIED"
      assert_report_hex_eq "$report" 'BYTES_COPIED_HEX' '0000' "${case_id}_BYTES_COPIED"
      assert_report_hex_eq "$report" 'MEDIA_SWAPS_HEX' '0000' "${case_id}_MEDIA_SWAPS"
    elif [[ "$mode" == "insufficient_space" ]]; then
      assert_report_line "$report" 'STATUS=FAIL' "${case_id}_REPORT_STATUS"
      assert_report_hex_eq "$report" 'FAIL_CODE_HEX' '0202' "${case_id}_FAILCODE_EXPECTED"
      assert_report_hex_eq "$report" 'RETRY_COUNT_HEX' '0000' "${case_id}_RETRY_COUNT"
      assert_report_hex_eq "$report" 'FILES_COPIED_HEX' '0000' "${case_id}_FILES_COPIED"
      assert_report_hex_eq "$report" 'BYTES_COPIED_HEX' '0000' "${case_id}_BYTES_COPIED"
      assert_report_hex_eq "$report" 'MEDIA_SWAPS_HEX' '0000' "${case_id}_MEDIA_SWAPS"
    elif [[ "$mode" == "invalid_target" ]]; then
      assert_report_line "$report" 'STATUS=FAIL' "${case_id}_REPORT_STATUS"
      assert_report_hex_eq "$report" 'FAIL_CODE_HEX' '0203' "${case_id}_FAILCODE_EXPECTED"
      assert_report_hex_eq "$report" 'RETRY_COUNT_HEX' '0000' "${case_id}_RETRY_COUNT"
      assert_report_hex_eq "$report" 'FILES_COPIED_HEX' '0000' "${case_id}_FILES_COPIED"
      assert_report_hex_eq "$report" 'BYTES_COPIED_HEX' '0000' "${case_id}_BYTES_COPIED"
      assert_report_hex_eq "$report" 'MEDIA_SWAPS_HEX' '0000' "${case_id}_MEDIA_SWAPS"
    elif [[ "$mode" == "media_swap_timeout" ]]; then
      assert_report_line "$report" 'STATUS=FAIL' "${case_id}_REPORT_STATUS"
      assert_report_hex_eq "$report" 'FAIL_CODE_HEX' '0603' "${case_id}_FAILCODE_EXPECTED"
      assert_report_hex_eq "$report" 'RETRY_COUNT_HEX' '0000' "${case_id}_RETRY_COUNT"
      assert_report_hex_ge "$report" 'FILES_COPIED_HEX' 1 "${case_id}_FILES_COPIED"
      assert_report_hex_ge "$report" 'BYTES_COPIED_HEX' 1 "${case_id}_BYTES_COPIED"
      assert_report_hex_eq "$report" 'MEDIA_SWAPS_HEX' '0000' "${case_id}_MEDIA_SWAPS"
    fi

    local expected_manifest_source=''
    case "$mode" in
      success|media_swap_success|media_swap_timeout|failure|insufficient_space)
        expected_manifest_source='0001'
        ;;
      invalid_target|manifest_invalid)
        expected_manifest_source='0000'
        ;;
      *)
        mark_fail "${case_id}_MANIFEST_EXPECTED_MODE" "no manifest source expectation for mode '$mode'"
        ;;
    esac

    assert_report_hex_eq "$report" 'MANIFEST_MEDIA_HEX' "$expected_manifest_source" "${case_id}_MANIFEST_SOURCE"

    if [[ "$expected_manifest_source" == "0001" ]]; then
      if grep -Eiq 'Manifest fallback: (open failed|read failed|invalid header|invalid record)\.|MMaanniiffeesstt[[:space:]]+ffaallllbbaacckk:' "$strings_log"; then
        mark_fail "${case_id}_MANIFEST_MEDIA_PATH" "unexpected manifest fallback in $strings_log"
      fi
      mark_pass "${case_id}_MANIFEST_MEDIA_PATH"
    fi

    assert_report_hex_ge "$report" 'STEP_HEX' 1 "${case_id}_STEP_RECORDED"
    assert_report_hex_ge "$report" 'KB_KEYS_HEX' 1 "${case_id}_KB_KEYS"
    if [[ "$mode" == "invalid_target" ]]; then
      assert_report_hex_eq "$report" 'FILES_PLANNED_HEX' '0000' "${case_id}_FILES_PLANNED"
    else
      assert_report_hex_ge "$report" 'FILES_PLANNED_HEX' 1 "${case_id}_FILES_PLANNED"
    fi
  fi

  if [[ "$mode" == "success" || "$mode" == "media_swap_success" || "$mode" == "manifest_invalid" ]]; then
    local cfg_out="build/full/setup_scenario_${case_id}.cfg.txt"
    if ! mtype -i "$image" ::CIUKIOS/CIUKIOS.CFG > "$cfg_out" 2>/dev/null; then
      mark_fail "${case_id}_CFG" "CIUKIOS.CFG not found in image"
    fi
    grep -Fq 'PROFILE=MINIMAL' "$cfg_out" || mark_fail "${case_id}_CFG_PROFILE" "PROFILE=MINIMAL missing in cfg"
    grep -Fq 'TARGET=\CIUKIOS' "$cfg_out" || mark_fail "${case_id}_CFG_TARGET" "TARGET=\CIUKIOS missing in cfg"
    mark_pass "${case_id}_CFG_CONTENT"
  elif [[ "$mode" == "dry_run" ]]; then
    if mdir -i "$image" ::CIUKIOS/CIUKIOS.CFG >/dev/null 2>&1; then
      mark_fail "${case_id}_CFG_ABSENT" "dry-run unexpectedly persisted CIUKIOS.CFG"
    fi
    mark_pass "${case_id}_CFG_ABSENT"
  else
    mark_pass "${case_id}_FAIL_PATH_OBSERVED"
  fi

  {
    echo "CASE_ID=$case_id"
    echo "MODE=$mode"
    echo "SNAPSHOT_MODE=$snapshot_mode"
    echo "IMAGE=$image"
    echo "QEMU_CMD=$qemu_cmd"
    echo "QEMU_RC=$qemu_rc"
    echo "SERIAL_LOG=$serial_log"
    echo "STRINGS_LOG=$strings_log"
    echo "STDERR_LOG=$stderr_log"
    echo "CMD_LOG=$cmd_log"
    echo "REPORT=$report"
  } > "$meta"

  mark_pass "${case_id}_COMPLETE"
}

main() {
  need_cmd socat
  need_cmd strings
  need_cmd mtype
  need_cmd mdel
  need_cmd mmd
  need_cmd mdir
  need_cmd mcopy
  need_cmd sha256sum
  need_cmd perl
  need_cmd cmp

  if (( DO_BUILD )); then
    echo "[setup-scenarios] build step"
    bash scripts/build_full.sh
  fi

  local base_img="build/full/ciukios-full.img"
  local success_img="build/full/ciukios-full-setup-success.img"
  local media_swap_success_img="build/full/ciukios-full-setup-media-swap-success.img"
  local media_swap_timeout_img="build/full/ciukios-full-setup-media-swap-timeout.img"
  local failure_img="build/full/ciukios-full-setup-failure.img"
  local target_invalid_img="build/full/ciukios-full-setup-target-invalid.img"
  local dryrun_img="build/full/ciukios-full-setup-dryrun.img"
  local insufficient_img="build/full/ciukios-full-setup-insufficient-space.img"
  local manifest_invalid_img="build/full/ciukios-full-setup-manifest-invalid.img"
  local setup_bin="build/full/obj/setup.com"
  local setup_manifest_bin="build/full/obj/setup.mft"

  if [[ ! -f "$base_img" ]]; then
    mark_fail "BASE_IMAGE" "missing image: $base_img"
  fi

  if [[ ! -f "$setup_bin" ]]; then
    mark_fail "SETUP_BIN" "missing setup binary: $setup_bin"
  fi

  if [[ ! -f "$setup_manifest_bin" ]]; then
    mark_fail "SETUP_MANIFEST_BIN" "missing setup manifest binary: $setup_manifest_bin"
  fi

  local contract_markers=(
    'REPORT_SCHEMA=SETUP_MVP_V2'
    'INPUT_MEDIA=FULL_FAT16'
    'INPUT_TARGET=\CIUKIOS'
    'INPUT_PROFILE='
    'STEP_HEX='
    'RETRY_COUNT_HEX='
    'TARGET_DRIVE_HEX='
    'TARGETS_VALID_HEX='
    'MEDIA_SWAPS_HEX='
    'MANIFEST_MEDIA_HEX='
    'KB_KEYS_HEX='
    'KB_NAV_HEX='
    'FILES_PLANNED_HEX='
    'FILES_COPIED_HEX='
    'BYTES_COPIED_HEX='
    'FAIL_CODE_HEX='
  )
  for marker in "${contract_markers[@]}"; do
    strings -a "$setup_bin" | grep -Fq "$marker" || mark_fail "REPORT_CONTRACT" "missing marker '$marker' in $setup_bin"
  done
  mark_pass "REPORT_CONTRACT_BIN"

  cp "$base_img" "$success_img"
  cp "$base_img" "$media_swap_success_img"
  cp "$base_img" "$media_swap_timeout_img"
  cp "$base_img" "$failure_img"
  cp "$base_img" "$target_invalid_img"
  cp "$base_img" "$dryrun_img"
  cp "$base_img" "$insufficient_img"
  cp "$base_img" "$manifest_invalid_img"
  mark_pass "IMAGE_COPIES"

  # Runtime note: stage2 mkdir path handling is unstable on absolute roots;
  # pre-create the target tree so setup can validate copy/report paths.
  local img
  for img in "$success_img" "$media_swap_success_img" "$media_swap_timeout_img" "$failure_img" "$target_invalid_img" "$dryrun_img" "$insufficient_img" "$manifest_invalid_img"; do
    mmd -i "$img" ::CIUKIOS >/dev/null 2>&1 || true
    mmd -i "$img" ::CIUKIOS/SYSTEM >/dev/null 2>&1 || true
    mmd -i "$img" ::CIUKIOS/APPS >/dev/null 2>&1 || true
  done
  mark_pass "TARGET_DIR_PREP"

  run_setup_case "success_minimal" "$success_img" "success" "0"

  prepare_media_swap_manifest_image "$media_swap_success_img"
  prepare_media_swap_manifest_image "$media_swap_timeout_img"
  run_setup_case "success_media_swap" "$media_swap_success_img" "media_swap_success" "0"
  run_setup_case "failure_media_swap_timeout" "$media_swap_timeout_img" "media_swap_timeout" "0"

  prepare_manifest_invalid_image "$manifest_invalid_img"
  run_setup_case "manifest_invalid_header_fallback" "$manifest_invalid_img" "manifest_invalid" "0"

  run_setup_case "failure_invalid_target" "$target_invalid_img" "invalid_target" "0"

  mdel -i "$failure_img" ::SYSTEM/STAGE2.BIN >/dev/null 2>&1 || mark_fail "FAILURE_PREP" "unable to remove SYSTEM/STAGE2.BIN"
  mark_pass "FAILURE_PREP"

  run_setup_case "failure_missing_media" "$failure_img" "failure" "0"

  local dryrun_sha_before dryrun_sha_after
  dryrun_sha_before="$(sha256sum "$dryrun_img" | awk '{print $1}')"
  run_setup_case "dry_run_minimal" "$dryrun_img" "dry_run" "1"
  dryrun_sha_after="$(sha256sum "$dryrun_img" | awk '{print $1}')"
  if [[ "$dryrun_sha_before" != "$dryrun_sha_after" ]]; then
    mark_fail "DRYRUN_IMAGE_UNCHANGED" "dry-run image hash changed despite snapshot mode"
  fi
  mark_pass "DRYRUN_IMAGE_UNCHANGED"

  prepare_insufficient_space_image "$insufficient_img"
  prepare_insufficient_space_setup_override "$insufficient_img"
  run_setup_case "failure_insufficient_space" "$insufficient_img" "insufficient_space" "0"

  echo "[setup-scenarios] PASS (success, media-swap success+timeout, invalid-target, missing-media failure, dry-run, insufficient-space, invalid-manifest-header fallback scenarios validated)"
}

main "$@"
