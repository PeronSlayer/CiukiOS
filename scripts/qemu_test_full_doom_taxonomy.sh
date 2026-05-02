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
DOOM_TAXONOMY_STRICT="${DOOM_TAXONOMY_STRICT:-0}"
FUTURE_MTIME_TOLERANCE_SEC="${FUTURE_MTIME_TOLERANCE_SEC:-5}"

STAGES=(
  binary_found
  wad_found
  extender_init
  video_init
  menu_reached
)

declare -A STAGE_STATUS
declare -A STAGE_DETAIL
declare -A PRE_LOG_EXISTS
declare -A PRE_LOG_MTIME
declare -A PRE_LOG_SIZE

LOG_FRESHNESS_REASON=""

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

qemu_available() {
  if [[ -n "${QEMU_BIN:-}" ]]; then
    if command -v "$QEMU_BIN" >/dev/null 2>&1 || [[ -x "$QEMU_BIN" ]]; then
      return 0
    fi
  fi

  if command_exists qemu-system-i386 || command_exists qemu-system-x86_64; then
    return 0
  fi

  return 1
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

classify_static_with_mdir() {
  local doom_dir="${DOOM_DIR_IN_IMAGE%/}"

  if ! mdir -i "$IMG" "$doom_dir" >/dev/null 2>&1; then
    set_stage "binary_found" "FAIL" "directory $doom_dir missing in image (mdir)"
    set_stage "wad_found" "FAIL" "directory $doom_dir missing in image (mdir)"
    return
  fi

  if mdir -i "$IMG" "$doom_dir/$DOOM_EXE_NAME" >/dev/null 2>&1; then
    set_stage "binary_found" "PASS" "found $DOOM_EXE_NAME in $doom_dir via mdir"
  else
    set_stage "binary_found" "FAIL" "missing $DOOM_EXE_NAME in $doom_dir (mdir)"
  fi

  if mdir -i "$IMG" "$doom_dir/$DOOM_WAD_PRIMARY" >/dev/null 2>&1; then
    set_stage "wad_found" "PASS" "found $DOOM_WAD_PRIMARY in $doom_dir via mdir"
  elif mdir -i "$IMG" "$doom_dir/$DOOM_WAD_ALIAS" >/dev/null 2>&1; then
    set_stage "wad_found" "PASS" "found $DOOM_WAD_ALIAS in $doom_dir via mdir"
  else
    set_stage "wad_found" "FAIL" "missing $DOOM_WAD_PRIMARY/$DOOM_WAD_ALIAS in $doom_dir (mdir)"
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

  if grep -Fqi "$DOOM_EXE_NAME" "$dump_file"; then
    set_stage "binary_found" "PASS" "found $DOOM_EXE_NAME via strings fallback"
  else
    set_stage "binary_found" "FAIL" "missing $DOOM_EXE_NAME via strings fallback"
  fi

  if grep -Fqi "$DOOM_WAD_PRIMARY" "$dump_file"; then
    set_stage "wad_found" "PASS" "found $DOOM_WAD_PRIMARY via strings fallback"
  elif grep -Fqi "$DOOM_WAD_ALIAS" "$dump_file"; then
    set_stage "wad_found" "PASS" "found $DOOM_WAD_ALIAS via strings fallback"
  else
    set_stage "wad_found" "FAIL" "missing $DOOM_WAD_PRIMARY/$DOOM_WAD_ALIAS via strings fallback"
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

  if ! qemu_available; then
    set_stage "extender_init" "DEFERRED" "qemu unavailable; runtime probe skipped"
    set_stage "video_init" "DEFERRED" "qemu unavailable; runtime probe skipped"
    set_stage "menu_reached" "DEFERRED" "qemu unavailable; runtime probe skipped"
    return
  fi

  mkdir -p "$(dirname "$LOG_FILE")"
  capture_log_metadata "$LOG_FILE"
  if [[ "$DOOM_RUNTIME_LOG" != "$LOG_FILE" ]]; then
    capture_log_metadata "$DOOM_RUNTIME_LOG"
  fi

  runtime_run_start_epoch="$(date +%s)"
  rm -f "$LOG_FILE"

  set +e
  LOG_FILE="$LOG_FILE" QEMU_TIMEOUT_SEC="$QEMU_TIMEOUT_SEC" \
    bash scripts/qemu_run_full.sh --test --no-build >/dev/null 2>&1
  smoke_rc=$?
  set -e

  if [[ $smoke_rc -eq 0 ]]; then
    smoke_detail="qemu smoke rc=0"
  else
    smoke_detail="qemu smoke rc=$smoke_rc"
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
    set_stage "extender_init" "DEFERRED" "$smoke_detail; no fresh runtime logs available$stale_detail"
    set_stage "video_init" "DEFERRED" "$smoke_detail; no fresh runtime logs available$stale_detail"
    set_stage "menu_reached" "DEFERRED" "$smoke_detail; no fresh runtime logs available$stale_detail"
    return
  fi

  if [[ $smoke_rc -ne 0 ]]; then
    set_stage "extender_init" "DEFERRED" "$smoke_detail; runtime markers ignored because smoke failed; fresh logs seen: $runtime_log_sources$stale_detail"
    set_stage "video_init" "DEFERRED" "$smoke_detail; runtime markers ignored because smoke failed; fresh logs seen: $runtime_log_sources$stale_detail"
    set_stage "menu_reached" "DEFERRED" "$smoke_detail; runtime markers ignored because smoke failed; fresh logs seen: $runtime_log_sources$stale_detail"
    return
  fi

  if log_has_fixed_marker '[ doom ] stage reached: extender_init' "${runtime_logs[@]}" \
    || log_has_pattern '\\[ *doom *\\].*stage reached: *(extender_init|extender)|OpenGEM: extender (probe complete|mode=dpmi-stub)|DOS/?4GW|DPMI' "${runtime_logs[@]}"; then
    set_stage "extender_init" "PASS" "$smoke_detail; extender marker observed in fresh logs: $runtime_log_sources"
  else
    set_stage "extender_init" "DEFERRED" "$smoke_detail; extender marker not observed in fresh logs: $runtime_log_sources"
  fi

  if log_has_fixed_marker '[ doom ] stage reached: video' "${runtime_logs[@]}" \
    || log_has_fixed_marker '[ doom ] stage reached: video/menu' "${runtime_logs[@]}" \
    || log_has_pattern '\\[ *doom *\\].*stage reached: *(video|video/menu|gfx)|I_InitGraphics|V_Init|video init' "${runtime_logs[@]}"; then
    set_stage "video_init" "PASS" "$smoke_detail; video marker observed in fresh logs: $runtime_log_sources"
  else
    set_stage "video_init" "DEFERRED" "$smoke_detail; video marker not observed in fresh logs: $runtime_log_sources"
  fi

  if log_has_fixed_marker '[ doom ] stage reached: menu' "${runtime_logs[@]}" \
    || log_has_fixed_marker '[ doom ] stage reached: video/menu' "${runtime_logs[@]}" \
    || log_has_pattern '\\[ *doom *\\].*stage reached: *(menu|video/menu)|M_Init|D_DoomMain|TITLEPIC|new game' "${runtime_logs[@]}"; then
    set_stage "menu_reached" "PASS" "$smoke_detail; menu marker observed in fresh logs: $runtime_log_sources"
  else
    set_stage "menu_reached" "DEFERRED" "$smoke_detail; menu marker not observed in fresh logs: $runtime_log_sources"
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
  set_stage "wad_found" "FAIL" "image not found: $IMG"
  set_stage "extender_init" "DEFERRED" "runtime probe skipped (image missing)"
  set_stage "video_init" "DEFERRED" "runtime probe skipped (image missing)"
  set_stage "menu_reached" "DEFERRED" "runtime probe skipped (image missing)"
else
  if command_exists mdir; then
    classify_static_with_mdir
  elif command_exists strings; then
    classify_static_with_strings
  else
    set_stage "binary_found" "FAIL" "missing mdir and strings tooling"
    set_stage "wad_found" "FAIL" "missing mdir and strings tooling"
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
