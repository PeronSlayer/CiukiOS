#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
IMG="${IMG:-build/full/ciukios-full.img}"
DOOM_DIR_IN_IMAGE="${DOOM_DIR_IN_IMAGE:-::APPS/DOOM}"
DOOM_EXE_NAME="${DOOM_EXE_NAME:-DOOM.EXE}"
DOOM_WAD_PRIMARY="${DOOM_WAD_PRIMARY:-DOOM1.WAD}"
DOOM_WAD_ALIAS="${DOOM_WAD_ALIAS:-DOOM.WAD}"
LOG_FILE="${LOG_FILE:-build/full/qemu-full-doom-taxonomy.log}"
DOOM_RUNTIME_LOG="${DOOM_RUNTIME_LOG:-build/full/qemu-visual.log}"
QEMU_TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-45}"
DOOM_TAXONOMY_MIN_STAGE="${DOOM_TAXONOMY_MIN_STAGE:-wad_found}"
DOOM_TAXONOMY_PROFILE="${DOOM_TAXONOMY_PROFILE:-doom}"
DOOM_TAXONOMY_STRICT="${DOOM_TAXONOMY_STRICT:-0}"
DOOM_TAXONOMY_LAUNCH="${DOOM_TAXONOMY_LAUNCH:-1}"
DOOM_TAXONOMY_RUN_DRVLOAD="${DOOM_TAXONOMY_RUN_DRVLOAD:-1}"
DOOM_TAXONOMY_PROMPT_TIMEOUT_SEC="${DOOM_TAXONOMY_PROMPT_TIMEOUT_SEC:-120}"
DOOM_TAXONOMY_MARKER_TIMEOUT_SEC="${DOOM_TAXONOMY_MARKER_TIMEOUT_SEC:-45}"
DOOM_TAXONOMY_OBSERVE_SEC="${DOOM_TAXONOMY_OBSERVE_SEC:-15}"
DOOM_TAXONOMY_DOOM_CWD="${DOOM_TAXONOMY_DOOM_CWD:-\\APPS\\DOOM}"
DOOM_TAXONOMY_APP_DIR_IN_IMAGE="${DOOM_TAXONOMY_APP_DIR_IN_IMAGE:-$DOOM_DIR_IN_IMAGE}"
DOOM_TAXONOMY_APP_BINARY_NAME="${DOOM_TAXONOMY_APP_BINARY_NAME:-$DOOM_EXE_NAME}"
DOOM_TAXONOMY_RUN_COMMAND="${DOOM_TAXONOMY_RUN_COMMAND:-run ${DOOM_TAXONOMY_APP_BINARY_NAME}}"
DOOM_TAXONOMY_APP_RUNTIME_MARKERS="${DOOM_TAXONOMY_APP_RUNTIME_MARKERS:-}"
DOOM_TAXONOMY_DISPLAY_MODE="${DOOM_TAXONOMY_DISPLAY_MODE:-nographic}"
DOOM_TAXONOMY_SCREENSHOT="${DOOM_TAXONOMY_SCREENSHOT:-}"
DOOM_QEMU_STDERR="${DOOM_QEMU_STDERR:-build/full/qemu-full-doom-taxonomy.stderr.log}"
DOOM_QEMU_CMD_LOG="${DOOM_QEMU_CMD_LOG:-build/full/qemu-full-doom-taxonomy.commands.log}"
DOOM_MON_SOCK="${DOOM_MON_SOCK:-/tmp/ciukios-doom-taxonomy.monitor.sock}"
FUTURE_MTIME_TOLERANCE_SEC="${FUTURE_MTIME_TOLERANCE_SEC:-5}"

case "$DOOM_TAXONOMY_PROFILE" in
  doom)
    STAGES=(
      binary_found
      wad_found
      doom_exec_attempted
      mz_transfer
      extender_init
      video_init
      runtime_stable
      visual_gameplay
      menu_reached
    )
    ;;
  dos_generic)
    STAGES=(
      binary_found
      exec_attempted
      transfer_marker
      runtime_stable
    )
    if [[ "$DOOM_TAXONOMY_MIN_STAGE" == "wad_found" ]]; then
      DOOM_TAXONOMY_MIN_STAGE="transfer_marker"
    fi
    ;;
  *)
    echo "[doom-taxonomy] ERROR invalid profile: $DOOM_TAXONOMY_PROFILE (expected doom or dos_generic)" >&2
    exit 1
    ;;
esac

if [[ "$DOOM_TAXONOMY_PROFILE" == "dos_generic" && "$DOOM_TAXONOMY_DOOM_CWD" == "\\APPS\\DOOM" ]]; then
  DOOM_TAXONOMY_DOOM_CWD=""
fi

declare -A STAGE_STATUS
declare -A STAGE_DETAIL
declare -A PRE_LOG_EXISTS
declare -A PRE_LOG_MTIME
declare -A PRE_LOG_SIZE

LOG_FRESHNESS_REASON=""
ACTIVE_QEMU_PID=0
ACTIVE_MON_SOCK=""
ACTIVE_CMD_LOG=""

cleanup_active_qemu() {
  if [[ "$ACTIVE_QEMU_PID" -ne 0 ]] && kill -0 "$ACTIVE_QEMU_PID" >/dev/null 2>&1; then
    if [[ -n "$ACTIVE_MON_SOCK" && -S "$ACTIVE_MON_SOCK" && -n "$ACTIVE_CMD_LOG" ]]; then
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

normalize_detail() {
  local detail="$1"
  detail="${detail//$'\n'/ }"
  detail="${detail//$'\r'/ }"
  echo "$detail"
}

set_stage() {
  local stage="$1"
  local status="$2"
  local detail
  detail="$(normalize_detail "$3")"
  STAGE_STATUS["$stage"]="$status"
  STAGE_DETAIL["$stage"]="$detail"
}

emit_stage() {
  local stage="$1"
  echo "[doom-taxonomy] STAGE ${stage}=${STAGE_STATUS[$stage]} detail=${STAGE_DETAIL[$stage]}"
}

stage_index() {
  local target="$1"
  local i
  for i in "${!STAGES[@]}"; do
    if [[ "${STAGES[$i]}" == "$target" ]]; then
      echo "$i"
      return 0
    fi
  done
  return 1
}

command_exists() {
  command -v "$1" >/dev/null 2>&1
}

pick_qemu() {
  if [[ -n "${QEMU_BIN:-}" ]]; then
    echo "$QEMU_BIN"
    return 0
  fi

  if command_exists qemu-system-i386; then
    echo "qemu-system-i386"
    return 0
  fi

  if command_exists qemu-system-x86_64; then
    echo "qemu-system-x86_64"
    return 0
  fi

  return 1
}

qemu_available() {
  pick_qemu >/dev/null 2>&1
}

log_has_pattern() {
  local pattern="$1"
  shift

  local log_path
  for log_path in "$@"; do
    if [[ -s "$log_path" ]] && grep -Eiq "$pattern" "$log_path"; then
      return 0
    fi
  done

  return 1
}

log_has_fixed_marker() {
  local marker="$1"
  shift

  local log_path
  for log_path in "$@"; do
    if [[ -s "$log_path" ]] && grep -Fqi "$marker" "$log_path"; then
      return 0
    fi
  done

  return 1
}

get_file_mtime_epoch() {
  stat -c %Y "$1" 2>/dev/null || echo 0
}

get_file_size_bytes() {
  stat -c %s "$1" 2>/dev/null || echo 0
}

capture_log_metadata() {
  local log_path="$1"

  if [[ -e "$log_path" ]]; then
    PRE_LOG_EXISTS["$log_path"]=1
    PRE_LOG_MTIME["$log_path"]="$(get_file_mtime_epoch "$log_path")"
    PRE_LOG_SIZE["$log_path"]="$(get_file_size_bytes "$log_path")"
  else
    PRE_LOG_EXISTS["$log_path"]=0
    PRE_LOG_MTIME["$log_path"]=0
    PRE_LOG_SIZE["$log_path"]=0
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

  return "$rc"
}

send_key() {
  local sock="$1"
  local cmd_log="$2"
  local key="$3"
  hmp "$sock" "$cmd_log" "sendkey $key" >/dev/null 2>&1 || return 1
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
}

observe_runtime_window() {
  local pid="$1"
  local observe_sec="$2"
  local start now
  start="$(date +%s)"

  while kill -0 "$pid" >/dev/null 2>&1; do
    now="$(date +%s)"
    if (( now - start >= observe_sec )); then
      return 0
    fi
    sleep 1
  done

  return 0
}

log_is_fresh_for_run() {
  local log_path="$1"
  local run_start_epoch="$2"
  local now_epoch future_limit
  local post_mtime post_size
  local pre_exists pre_mtime pre_size

  LOG_FRESHNESS_REASON=""

  if [[ ! -e "$log_path" ]]; then
    LOG_FRESHNESS_REASON="missing"
    return 1
  fi

  post_size="$(get_file_size_bytes "$log_path")"
  if [[ "$post_size" -le 0 ]]; then
    LOG_FRESHNESS_REASON="empty"
    return 1
  fi

  post_mtime="$(get_file_mtime_epoch "$log_path")"
  now_epoch="$(date +%s)"
  future_limit=$((now_epoch + FUTURE_MTIME_TOLERANCE_SEC))
  if [[ "$post_mtime" -gt "$future_limit" ]]; then
    LOG_FRESHNESS_REASON="future-mtime-rejected (mtime=$post_mtime, now=$now_epoch, tolerance=${FUTURE_MTIME_TOLERANCE_SEC}s)"
    return 1
  fi

  pre_exists="${PRE_LOG_EXISTS[$log_path]:-0}"
  pre_mtime="${PRE_LOG_MTIME[$log_path]:-0}"
  pre_size="${PRE_LOG_SIZE[$log_path]:-0}"

  if [[ "$pre_exists" -eq 0 ]]; then
    if [[ "$post_mtime" -ge "$run_start_epoch" ]]; then
      LOG_FRESHNESS_REASON="created-during-run"
      return 0
    fi

    LOG_FRESHNESS_REASON="new-file-but-mtime-before-run (mtime=$post_mtime, run_start=$run_start_epoch)"
    return 1
  fi

  if [[ "$post_mtime" != "$pre_mtime" || "$post_size" != "$pre_size" ]]; then
    LOG_FRESHNESS_REASON="updated-during-run (mtime $pre_mtime->$post_mtime, size $pre_size->$post_size)"
    return 0
  fi

  LOG_FRESHNESS_REASON="unchanged-from-pre-run (mtime=$post_mtime, size=$post_size)"
  return 1
}

ppm_visual_diversity() {
  local ppm_path="$1"

  perl -0777ne '
    my $data = $_;
    my $pos = 0;
    my @tokens;
    while (@tokens < 4) {
      $data =~ /\G\s*/gc or last;
      if ($data =~ /\G#[^\n]*(?:\n|$)/gc) {
        next;
      }
      if ($data =~ /\G(\S+)/gc) {
        push @tokens, $1;
        next;
      }
      last;
    }
    die "invalid ppm header\n" unless @tokens == 4 && $tokens[0] eq "P6" && $tokens[1] =~ /^\d+$/ && $tokens[2] =~ /^\d+$/ && $tokens[3] =~ /^\d+$/;
    die "unsupported ppm maxval\n" unless $tokens[3] == 255;
    $pos = pos($data);
    die "invalid ppm raster separator\n" unless defined $pos && $pos < length($data) && substr($data, $pos, 1) =~ /\s/;
    $pos++;
    my ($width, $height) = ($tokens[1], $tokens[2]);
    my $expected = $width * $height * 3;
    my $pixels = substr($data, $pos, $expected);
    die "truncated ppm raster\n" unless length($pixels) == $expected;
    my $nonblank = 0;
    my %colors;
    my $sample_step = 3;
    my $max_samples = 20000;
    if ($width * $height > $max_samples) {
      $sample_step = int(($width * $height) / $max_samples) * 3;
      $sample_step = 3 if $sample_step < 3;
    }
    for (my $i = 0; $i + 2 < length($pixels); $i += $sample_step) {
      my ($r, $g, $b) = unpack("CCC", substr($pixels, $i, 3));
      $nonblank++ if $r || $g || $b;
      $colors{pack("CCC", $r, $g, $b)} = 1;
    }
    print "$width $height $nonblank ", scalar(keys %colors), "\n";
  ' "$ppm_path"
}

classify_visual_gameplay() {
  local run_start_epoch="$1"
  local smoke_detail="$2"
  local screenshot_mtime
  local ppm_stats
  local width height nonblank unique_colors

  if [[ "${STAGE_STATUS[runtime_stable]}" != "PASS" ]]; then
    set_stage "visual_gameplay" "DEFERRED" "$smoke_detail; runtime_stable gate not passed, visual screenshot not evaluated"
    return
  fi

  if [[ -z "$DOOM_TAXONOMY_SCREENSHOT" ]]; then
    set_stage "visual_gameplay" "DEFERRED" "$smoke_detail; screenshot not requested (DOOM_TAXONOMY_SCREENSHOT empty)"
    return
  fi

  if [[ ! -s "$DOOM_TAXONOMY_SCREENSHOT" ]]; then
    set_stage "visual_gameplay" "DEFERRED" "$smoke_detail; screenshot missing or empty: $DOOM_TAXONOMY_SCREENSHOT"
    return
  fi

  screenshot_mtime="$(get_file_mtime_epoch "$DOOM_TAXONOMY_SCREENSHOT")"
  if [[ "$screenshot_mtime" -lt "$run_start_epoch" ]]; then
    set_stage "visual_gameplay" "DEFERRED" "$smoke_detail; screenshot stale for current run: $DOOM_TAXONOMY_SCREENSHOT (mtime=$screenshot_mtime, run_start=$run_start_epoch)"
    return
  fi

  if ! ppm_stats="$(ppm_visual_diversity "$DOOM_TAXONOMY_SCREENSHOT" 2>&1)"; then
    set_stage "visual_gameplay" "FAIL" "$smoke_detail; screenshot PPM validation failed for $DOOM_TAXONOMY_SCREENSHOT: $ppm_stats"
    return
  fi

  read -r width height nonblank unique_colors <<<"$ppm_stats"
  if [[ "$nonblank" -ge 1000 && "$unique_colors" -ge 16 ]]; then
    set_stage "visual_gameplay" "PASS" "$smoke_detail; screenshot fresh P6 PPM has visual diversity: ${width}x${height}, nonblank_samples=$nonblank, unique_sampled_colors=$unique_colors"
  else
    set_stage "visual_gameplay" "FAIL" "$smoke_detail; screenshot blank or low diversity: ${width}x${height}, nonblank_samples=$nonblank, unique_sampled_colors=$unique_colors"
  fi
}

classify_static_with_mdir() {
  local app_dir="${DOOM_TAXONOMY_APP_DIR_IN_IMAGE%/}"

  if ! mdir -i "$IMG" "$app_dir" >/dev/null 2>&1; then
    set_stage "binary_found" "FAIL" "directory $app_dir missing in image (mdir)"
    if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
      set_stage "wad_found" "FAIL" "directory $app_dir missing in image (mdir)"
    fi
    return
  fi

  if mdir -i "$IMG" "$app_dir/$DOOM_TAXONOMY_APP_BINARY_NAME" >/dev/null 2>&1; then
    set_stage "binary_found" "PASS" "found $DOOM_TAXONOMY_APP_BINARY_NAME in $app_dir via mdir"
  else
    set_stage "binary_found" "FAIL" "missing $DOOM_TAXONOMY_APP_BINARY_NAME in $app_dir (mdir)"
  fi

  if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
    if mdir -i "$IMG" "$app_dir/$DOOM_WAD_PRIMARY" >/dev/null 2>&1; then
      set_stage "wad_found" "PASS" "found $DOOM_WAD_PRIMARY in $app_dir via mdir"
    elif mdir -i "$IMG" "$app_dir/$DOOM_WAD_ALIAS" >/dev/null 2>&1; then
      set_stage "wad_found" "PASS" "found $DOOM_WAD_ALIAS in $app_dir via mdir"
    else
      set_stage "wad_found" "FAIL" "missing $DOOM_WAD_PRIMARY/$DOOM_WAD_ALIAS in $app_dir (mdir)"
    fi
  fi
}

classify_static_with_strings() {
  local dump_file
  dump_file="$(mktemp)"

  if ! strings -a "$IMG" >"$dump_file" 2>/dev/null; then
    rm -f "$dump_file"
    set_stage "binary_found" "FAIL" "strings fallback failed for image scan"
    set_stage "wad_found" "FAIL" "strings fallback failed for image scan"
    return
  fi

  if grep -Fqi "$DOOM_TAXONOMY_APP_BINARY_NAME" "$dump_file"; then
    set_stage "binary_found" "PASS" "found $DOOM_TAXONOMY_APP_BINARY_NAME via strings fallback"
  else
    set_stage "binary_found" "FAIL" "missing $DOOM_TAXONOMY_APP_BINARY_NAME via strings fallback"
  fi

  if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
    if grep -Fqi "$DOOM_WAD_PRIMARY" "$dump_file"; then
      set_stage "wad_found" "PASS" "found $DOOM_WAD_PRIMARY via strings fallback"
    elif grep -Fqi "$DOOM_WAD_ALIAS" "$dump_file"; then
      set_stage "wad_found" "PASS" "found $DOOM_WAD_ALIAS via strings fallback"
    else
      set_stage "wad_found" "FAIL" "missing $DOOM_WAD_PRIMARY/$DOOM_WAD_ALIAS via strings fallback"
    fi
  fi

  rm -f "$dump_file"
}

classify_runtime() {
  local smoke_rc=0
  local smoke_detail
  local runtime_run_start_epoch
  local runtime_logs=()
  local stale_runtime_logs=()
  local stale_runtime_log_details=()
  local runtime_log_sources=""
  local stale_log_sources=""
  local stale_detail=""
  local qemu_cmd
  local qemu_pid
  local qemu_rc=0
  local -a qemu_display_args
  local drvload_done_pattern='\[DRVLOAD\][[:space:]]+DONE|\[\[DDRRVVLLOOAADD\]\][[:space:]]+DDOONNEE?'
  local doom_cmd="$DOOM_TAXONOMY_RUN_COMMAND"
  local launch_detail="$doom_cmd"
  local exec_stage_name="doom_exec_attempted"

  if [[ "$DOOM_TAXONOMY_PROFILE" == "dos_generic" ]]; then
    exec_stage_name="exec_attempted"
  fi

  if ! qemu_available; then
    set_stage "$exec_stage_name" "DEFERRED" "qemu unavailable; runtime probe skipped"
    if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
      set_stage "mz_transfer" "DEFERRED" "qemu unavailable; runtime probe skipped"
      set_stage "extender_init" "DEFERRED" "qemu unavailable; runtime probe skipped"
      set_stage "video_init" "DEFERRED" "qemu unavailable; runtime probe skipped"
      set_stage "visual_gameplay" "DEFERRED" "qemu unavailable; visual screenshot probe skipped"
      set_stage "menu_reached" "DEFERRED" "qemu unavailable; runtime probe skipped"
    else
      set_stage "transfer_marker" "DEFERRED" "qemu unavailable; runtime probe skipped"
    fi
    set_stage "runtime_stable" "DEFERRED" "qemu unavailable; runtime probe skipped"
    return
  fi

  if [[ "$DOOM_TAXONOMY_LAUNCH" != "0" && "$DOOM_TAXONOMY_LAUNCH" != "1" ]]; then
    set_stage "$exec_stage_name" "FAIL" "invalid DOOM_TAXONOMY_LAUNCH=$DOOM_TAXONOMY_LAUNCH (expected 0 or 1)"
    if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
      set_stage "mz_transfer" "DEFERRED" "runtime launch skipped due invalid launch mode"
      set_stage "extender_init" "DEFERRED" "runtime launch skipped due invalid launch mode"
      set_stage "video_init" "DEFERRED" "runtime launch skipped due invalid launch mode"
      set_stage "visual_gameplay" "DEFERRED" "runtime launch skipped due invalid launch mode"
      set_stage "menu_reached" "DEFERRED" "runtime launch skipped due invalid launch mode"
    else
      set_stage "transfer_marker" "DEFERRED" "runtime launch skipped due invalid launch mode"
    fi
    set_stage "runtime_stable" "DEFERRED" "runtime launch skipped due invalid launch mode"
    return
  fi

  mkdir -p "$(dirname "$LOG_FILE")"
  capture_log_metadata "$LOG_FILE"
  if [[ "$DOOM_RUNTIME_LOG" != "$LOG_FILE" ]]; then
    capture_log_metadata "$DOOM_RUNTIME_LOG"
  fi

  runtime_run_start_epoch="$(date +%s)"
  rm -f "$LOG_FILE" "$DOOM_QEMU_STDERR" "$DOOM_QEMU_CMD_LOG" "$DOOM_MON_SOCK"
  if [[ -n "$DOOM_TAXONOMY_SCREENSHOT" ]]; then
    mkdir -p "$(dirname "$DOOM_TAXONOMY_SCREENSHOT")"
    rm -f "$DOOM_TAXONOMY_SCREENSHOT"
  fi

  if [[ "$DOOM_TAXONOMY_LAUNCH" == "1" ]]; then
    if ! command_exists socat; then
      set_stage "$exec_stage_name" "DEFERRED" "socat unavailable; interactive app launch skipped"
      if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
        set_stage "mz_transfer" "DEFERRED" "socat unavailable; interactive app launch skipped"
        set_stage "extender_init" "DEFERRED" "socat unavailable; interactive app launch skipped"
        set_stage "video_init" "DEFERRED" "socat unavailable; interactive app launch skipped"
        set_stage "visual_gameplay" "DEFERRED" "socat unavailable; visual screenshot probe skipped"
        set_stage "menu_reached" "DEFERRED" "socat unavailable; interactive app launch skipped"
      else
        set_stage "transfer_marker" "DEFERRED" "socat unavailable; interactive app launch skipped"
      fi
      set_stage "runtime_stable" "DEFERRED" "socat unavailable; interactive DOOM launch skipped"
      return
    fi

    qemu_cmd="$(pick_qemu)"
    case "$DOOM_TAXONOMY_DISPLAY_MODE" in
      nographic) qemu_display_args=(-nographic) ;;
      none) qemu_display_args=(-display none) ;;
      *)
        set_stage "$exec_stage_name" "FAIL" "invalid DOOM_TAXONOMY_DISPLAY_MODE=$DOOM_TAXONOMY_DISPLAY_MODE (expected nographic or none)"
        if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
          set_stage "mz_transfer" "DEFERRED" "runtime launch skipped due invalid display mode"
          set_stage "extender_init" "DEFERRED" "runtime launch skipped due invalid display mode"
          set_stage "video_init" "DEFERRED" "runtime launch skipped due invalid display mode"
          set_stage "visual_gameplay" "DEFERRED" "runtime launch skipped due invalid display mode"
          set_stage "menu_reached" "DEFERRED" "runtime launch skipped due invalid display mode"
        else
          set_stage "transfer_marker" "DEFERRED" "runtime launch skipped due invalid display mode"
        fi
        set_stage "runtime_stable" "DEFERRED" "runtime launch skipped due invalid display mode"
        return
        ;;
    esac

    QEMU_ARGS=(
      -machine pc,vmport=off
      -cpu pentium3
      -m 128
      -drive "file=$IMG,format=raw,if=ide"
      -boot c
      "${qemu_display_args[@]}"
      -chardev "file,id=ser0,path=$LOG_FILE"
      -serial chardev:ser0
      -monitor "unix:$DOOM_MON_SOCK,server,nowait"
      -no-reboot
      -no-shutdown
    )

    if [[ -n "${QEMU_EXTRA_ARGS:-}" ]]; then
      # shellcheck disable=SC2206
      EXTRA_ARGS=(${QEMU_EXTRA_ARGS})
      QEMU_ARGS+=("${EXTRA_ARGS[@]}")
    fi

    set +e
    timeout "$QEMU_TIMEOUT_SEC" "$qemu_cmd" "${QEMU_ARGS[@]}" >/dev/null 2>"$DOOM_QEMU_STDERR" &
    qemu_pid=$!
    set -e

    ACTIVE_QEMU_PID="$qemu_pid"
    ACTIVE_MON_SOCK="$DOOM_MON_SOCK"
    ACTIVE_CMD_LOG="$DOOM_QEMU_CMD_LOG"

    if ! wait_for_socket "$DOOM_MON_SOCK" 20; then
      set_stage "$exec_stage_name" "FAIL" "monitor socket not ready"
    elif ! wait_for_shell_prompt "$LOG_FILE" "$DOOM_TAXONOMY_PROMPT_TIMEOUT_SEC"; then
      set_stage "$exec_stage_name" "FAIL" "shell prompt not detected before app launch"
    else
      if [[ "$DOOM_TAXONOMY_RUN_DRVLOAD" == "1" ]]; then
        if send_text_and_enter "$DOOM_MON_SOCK" "$DOOM_QEMU_CMD_LOG" 'run \SYSTEM\DRIVERS\DRVLOAD.COM'; then
          wait_for_regex "$LOG_FILE" "$drvload_done_pattern" "$DOOM_TAXONOMY_MARKER_TIMEOUT_SEC" || true
          wait_for_shell_prompt "$LOG_FILE" 30 || true
        fi
      fi

      if [[ -n "$DOOM_TAXONOMY_DOOM_CWD" ]]; then
        if send_text_and_enter "$DOOM_MON_SOCK" "$DOOM_QEMU_CMD_LOG" "cd $DOOM_TAXONOMY_DOOM_CWD"; then
          wait_for_shell_prompt "$LOG_FILE" 30 || true
          launch_detail="cd $DOOM_TAXONOMY_DOOM_CWD; $doom_cmd"
        fi
      fi

      if send_text_and_enter "$DOOM_MON_SOCK" "$DOOM_QEMU_CMD_LOG" "$doom_cmd"; then
        set_stage "$exec_stage_name" "PASS" "sent shell command: $launch_detail"
        observe_runtime_window "$qemu_pid" "$DOOM_TAXONOMY_OBSERVE_SEC"
        if [[ -n "$DOOM_TAXONOMY_SCREENSHOT" ]]; then
          hmp "$DOOM_MON_SOCK" "$DOOM_QEMU_CMD_LOG" "screendump $DOOM_TAXONOMY_SCREENSHOT" >/dev/null 2>&1 || true
        fi
      else
        set_stage "$exec_stage_name" "FAIL" "failed to send shell command: $doom_cmd"
      fi
    fi

    hmp "$DOOM_MON_SOCK" "$DOOM_QEMU_CMD_LOG" "quit" >/dev/null 2>&1 || true
    set +e
    wait "$qemu_pid"
    qemu_rc=$?
    set -e
    ACTIVE_QEMU_PID=0
    ACTIVE_MON_SOCK=""
    ACTIVE_CMD_LOG=""
    rm -f "$DOOM_MON_SOCK"
    smoke_rc="$qemu_rc"
  else
    set +e
    LOG_FILE="$LOG_FILE" QEMU_TIMEOUT_SEC="$QEMU_TIMEOUT_SEC" \
      bash scripts/qemu_run_full.sh --test --no-build >/dev/null 2>&1
    smoke_rc=$?
    set -e

    set_stage "$exec_stage_name" "DEFERRED" "interactive app launch disabled (DOOM_TAXONOMY_LAUNCH=0)"
    if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
      set_stage "mz_transfer" "DEFERRED" "interactive app launch disabled (DOOM_TAXONOMY_LAUNCH=0)"
    else
      set_stage "transfer_marker" "DEFERRED" "interactive app launch disabled (DOOM_TAXONOMY_LAUNCH=0)"
    fi
  fi

  if [[ $smoke_rc -eq 0 ]]; then
    smoke_detail="qemu runtime rc=0"
  elif [[ $smoke_rc -eq 124 ]]; then
    smoke_detail="qemu runtime rc=124 (timeout)"
  else
    smoke_detail="qemu runtime rc=$smoke_rc"
  fi

  if [[ -s "$LOG_FILE" ]]; then
    if log_is_fresh_for_run "$LOG_FILE" "$runtime_run_start_epoch"; then
      runtime_logs+=("$LOG_FILE")
    else
      stale_runtime_logs+=("$LOG_FILE")
      stale_runtime_log_details+=("$LOG_FILE ($LOG_FRESHNESS_REASON)")
    fi
  fi
  if [[ -s "$DOOM_RUNTIME_LOG" && "$DOOM_RUNTIME_LOG" != "$LOG_FILE" ]]; then
    if log_is_fresh_for_run "$DOOM_RUNTIME_LOG" "$runtime_run_start_epoch"; then
      runtime_logs+=("$DOOM_RUNTIME_LOG")
    else
      stale_runtime_logs+=("$DOOM_RUNTIME_LOG")
      stale_runtime_log_details+=("$DOOM_RUNTIME_LOG ($LOG_FRESHNESS_REASON)")
    fi
  fi

  if (( ${#runtime_logs[@]} > 0 )); then
    runtime_log_sources="$(IFS=','; echo "${runtime_logs[*]}")"
  fi
  if (( ${#stale_runtime_logs[@]} > 0 )); then
    stale_log_sources="$(IFS=','; echo "${stale_runtime_logs[*]}")"
    stale_detail="; stale logs ignored: $stale_log_sources"
    if (( ${#stale_runtime_log_details[@]} > 0 )); then
      stale_detail="$stale_detail; stale reasons: $(IFS=' | '; echo "${stale_runtime_log_details[*]}")"
    fi
  fi

  if (( ${#runtime_logs[@]} == 0 )); then
    if [[ "${STAGE_STATUS[$exec_stage_name]}" == "DEFERRED" ]]; then
      set_stage "$exec_stage_name" "DEFERRED" "$smoke_detail; no fresh runtime logs available$stale_detail"
    fi
    if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
      set_stage "mz_transfer" "DEFERRED" "$smoke_detail; no fresh runtime logs available$stale_detail"
      set_stage "extender_init" "DEFERRED" "$smoke_detail; no fresh runtime logs available$stale_detail"
      set_stage "video_init" "DEFERRED" "$smoke_detail; no fresh runtime logs available$stale_detail"
      set_stage "visual_gameplay" "DEFERRED" "$smoke_detail; no fresh runtime logs available; visual screenshot not evaluated$stale_detail"
      set_stage "menu_reached" "DEFERRED" "$smoke_detail; no fresh runtime logs available$stale_detail"
    else
      set_stage "transfer_marker" "DEFERRED" "$smoke_detail; no fresh runtime logs available$stale_detail"
    fi
    set_stage "runtime_stable" "DEFERRED" "$smoke_detail; no fresh runtime logs available$stale_detail"
    return
  fi

  if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
    if log_has_fixed_marker '[MZ] run' "${runtime_logs[@]}" \
      || log_has_pattern '\\[\\[MMZZ\\]\\][[:space:]]+rruunn' "${runtime_logs[@]}"; then
      set_stage "mz_transfer" "PASS" "$smoke_detail; MZ transfer marker observed in fresh logs: $runtime_log_sources"
    else
      set_stage "mz_transfer" "DEFERRED" "$smoke_detail; MZ transfer marker not observed in fresh logs: $runtime_log_sources"
    fi
  else
    if log_has_fixed_marker '[MZ] run' "${runtime_logs[@]}" \
      || log_has_fixed_marker '[COM] run' "${runtime_logs[@]}" \
      || log_has_pattern '\\[\\[(MMZZ|CCOOMM)\\]\\][[:space:]]+rruunn' "${runtime_logs[@]}"; then
      set_stage "transfer_marker" "PASS" "$smoke_detail; COM/MZ transfer marker observed in fresh logs: $runtime_log_sources"
    elif [[ -n "$DOOM_TAXONOMY_APP_RUNTIME_MARKERS" ]] && \
      log_has_pattern "$DOOM_TAXONOMY_APP_RUNTIME_MARKERS" "${runtime_logs[@]}"; then
      set_stage "transfer_marker" "PASS" "$smoke_detail; inferred transfer from app-specific runtime markers (DOOM_TAXONOMY_APP_RUNTIME_MARKERS) in fresh logs: $runtime_log_sources"
    else
      set_stage "transfer_marker" "DEFERRED" "$smoke_detail; COM/MZ transfer marker not observed in fresh logs: $runtime_log_sources"
    fi
  fi

  if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
    if log_has_pattern 'cannot allocate tstack|ccaannnnoott[[:space:]]+aallllooccaattee[[:space:]]+ttssttaacck' "${runtime_logs[@]}"; then
      set_stage "extender_init" "FAIL" "$smoke_detail; DOS/16M tstack allocation failed in fresh logs: $runtime_log_sources"
    elif log_has_pattern "not a DOS/16M executable|nnoott[[:space:]]+aa[[:space:]]+DDOOSS//1166MM[[:space:]]+eexxeeccuuttaabbllee" "${runtime_logs[@]}"; then
      set_stage "extender_init" "FAIL" "$smoke_detail; DOS/16M executable validation failed in fresh logs: $runtime_log_sources"
    elif log_has_fixed_marker '[ doom ] stage reached: extender_init' "${runtime_logs[@]}" \
      || log_has_pattern '\\[ *doom *\\].*stage reached: *(extender_init|extender)|OpenGEM: extender (probe complete|mode=dpmi-stub)|DOS/?4G|DOS/?16M|DDOOSS//44GGWW|DDOOSS//1166MM|DPMI' "${runtime_logs[@]}"; then
      set_stage "extender_init" "PASS" "$smoke_detail; extender marker observed in fresh logs: $runtime_log_sources"
    else
      set_stage "extender_init" "DEFERRED" "$smoke_detail; extender marker not observed in fresh logs: $runtime_log_sources"
    fi
  fi

  if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
    if log_has_pattern 'Game mode indeterminate|GGaammee[[:space:]]+mmooddee[[:space:]]+iinnddeetteerrmmiinnaattee' "${runtime_logs[@]}"; then
      set_stage "video_init" "FAIL" "$smoke_detail; DOOM exited before video because IWAD/game mode was not resolved in fresh logs: $runtime_log_sources"
    elif log_has_fixed_marker '[ doom ] stage reached: video' "${runtime_logs[@]}" \
      || log_has_fixed_marker '[ doom ] stage reached: video/menu' "${runtime_logs[@]}" \
      || log_has_pattern '\\[ *doom *\\].*stage reached: *(video|video/menu|gfx)|I_InitGraphics|I__Init|II__IInniitt|V_Init|VV__IInniitt|DOOM System Startup|DDOOOOMM[[:space:]]+SSyysstteemm[[:space:]]+SSttaarrttuupp|video init' "${runtime_logs[@]}"; then
      set_stage "video_init" "PASS" "$smoke_detail; video marker observed in fresh logs: $runtime_log_sources"
    else
      set_stage "video_init" "DEFERRED" "$smoke_detail; video marker not observed in fresh logs: $runtime_log_sources"
    fi
  fi

  if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" \
    && "${STAGE_STATUS[extender_init]}" == "DEFERRED" \
    && "${STAGE_STATUS[mz_transfer]}" == "PASS" \
    && "${STAGE_STATUS[video_init]}" == "PASS" ]]; then
    set_stage "extender_init" "PASS" "$smoke_detail; inferred extender init from strong evidence chain (mz_transfer=PASS and video_init=PASS) in fresh logs: $runtime_log_sources"
  fi

  if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" && "${STAGE_STATUS[video_init]}" != "PASS" ]]; then
    set_stage "runtime_stable" "DEFERRED" "$smoke_detail; video gate not passed, stability window not evaluated"
  elif [[ $smoke_rc -eq 0 ]]; then
    set_stage "runtime_stable" "PASS" "$smoke_detail; QEMU remained controllable through the observation window"
  elif [[ $smoke_rc -eq 139 ]]; then
    set_stage "runtime_stable" "FAIL" "$smoke_detail; QEMU terminated with SIGSEGV during the post-video observation window"
  elif [[ $smoke_rc -eq 124 ]]; then
    set_stage "runtime_stable" "FAIL" "$smoke_detail; QEMU timed out before controlled shutdown during the post-video observation window"
  else
    set_stage "runtime_stable" "FAIL" "$smoke_detail; QEMU exited unexpectedly during the post-video observation window"
  fi

  if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
    classify_visual_gameplay "$runtime_run_start_epoch" "$smoke_detail"

    if log_has_fixed_marker '[ doom ] stage reached: menu' "${runtime_logs[@]}" \
      || log_has_fixed_marker '[ doom ] stage reached: video/menu' "${runtime_logs[@]}" \
      || log_has_pattern '\\[ *doom *\\].*stage reached: *(menu|video/menu)|D_DoomMain|TITLEPIC|new game' "${runtime_logs[@]}"; then
      set_stage "menu_reached" "PASS" "$smoke_detail; menu marker observed in fresh logs: $runtime_log_sources"
    else
      set_stage "menu_reached" "DEFERRED" "$smoke_detail; menu marker not observed in fresh logs: $runtime_log_sources"
    fi
  fi
}

if [[ "$DOOM_TAXONOMY_STRICT" != "0" && "$DOOM_TAXONOMY_STRICT" != "1" ]]; then
  echo "[doom-taxonomy] ERROR invalid strict mode: $DOOM_TAXONOMY_STRICT (expected 0 or 1)" >&2
  exit 1
fi

if ! MIN_STAGE_INDEX="$(stage_index "$DOOM_TAXONOMY_MIN_STAGE")"; then
  echo "[doom-taxonomy] ERROR invalid min stage: $DOOM_TAXONOMY_MIN_STAGE" >&2
  exit 1
fi

for stage in "${STAGES[@]}"; do
  set_stage "$stage" "DEFERRED" "not evaluated"
done

if [[ "$DO_BUILD" == "1" ]]; then
  set +e
  bash scripts/build_full.sh
  build_rc=$?
  set -e
  if [[ $build_rc -ne 0 ]]; then
    echo "[doom-taxonomy] ERROR build failed rc=$build_rc (DO_BUILD=1)" >&2
    exit "$build_rc"
  fi
fi

if [[ ! -f "$IMG" ]]; then
  set_stage "binary_found" "FAIL" "image not found: $IMG"
  if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
    set_stage "wad_found" "FAIL" "image not found: $IMG"
    set_stage "mz_transfer" "DEFERRED" "runtime probe skipped (image missing)"
    set_stage "extender_init" "DEFERRED" "runtime probe skipped (image missing)"
    set_stage "video_init" "DEFERRED" "runtime probe skipped (image missing)"
    set_stage "visual_gameplay" "DEFERRED" "runtime probe skipped (image missing)"
    set_stage "menu_reached" "DEFERRED" "runtime probe skipped (image missing)"
  else
    set_stage "exec_attempted" "DEFERRED" "runtime probe skipped (image missing)"
    set_stage "transfer_marker" "DEFERRED" "runtime probe skipped (image missing)"
  fi
  set_stage "runtime_stable" "DEFERRED" "runtime probe skipped (image missing)"
else
  if command_exists mdir; then
    classify_static_with_mdir
  elif command_exists strings; then
    classify_static_with_strings
  else
    set_stage "binary_found" "FAIL" "missing mdir and strings tooling"
    if [[ "$DOOM_TAXONOMY_PROFILE" == "doom" ]]; then
      set_stage "wad_found" "FAIL" "missing mdir and strings tooling"
    fi
  fi

  classify_runtime
fi

REACHED_STAGE="none"
for stage in "${STAGES[@]}"; do
  if [[ "${STAGE_STATUS[$stage]}" == "PASS" ]]; then
    REACHED_STAGE="$stage"
  else
    break
  fi
done

RESULT="PASS"
for ((i=0; i<=MIN_STAGE_INDEX; i++)); do
  stage="${STAGES[$i]}"
  if [[ "${STAGE_STATUS[$stage]}" != "PASS" ]]; then
    RESULT="FAIL"
    break
  fi
done

if [[ "$RESULT" == "PASS" && "$DOOM_TAXONOMY_STRICT" == "1" ]]; then
  for stage in "${STAGES[@]}"; do
    if [[ "${STAGE_STATUS[$stage]}" != "PASS" ]]; then
      RESULT="FAIL"
      break
    fi
  done
fi

for stage in "${STAGES[@]}"; do
  emit_stage "$stage"
done

echo "[doom-taxonomy] SUMMARY min_stage=$DOOM_TAXONOMY_MIN_STAGE reached=$REACHED_STAGE result=$RESULT"

if [[ "$RESULT" != "PASS" ]]; then
  exit 1
fi
