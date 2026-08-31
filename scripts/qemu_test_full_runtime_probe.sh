#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

POSITIVE_LOG="build/full/qemu-full-shell-com.serial.log"
POSITIVE_NORMALIZED_LOG="${POSITIVE_LOG}.normalized"
NEGATIVE_LOG_PREFIX="build/full/qemu-full-runtime-probe-negative"
RUNTIME_BIN="build/full/obj/ciukidos.sys"
RUNTIME_IMAGE_PATH="::SYSTEM/CIUKIDOS.SYS"
BASE_IMG="build/full/ciukios-full.img"
SNAPSHOT_DIR=""
ORIGINAL_IMG=""
GOOD_IMG=""
CASE_BIN=""
ORIGINAL_PRESENT=0
ORIGINAL_HASH=""
GOOD_HASH=""
RESTORE_REQUIRED=0

CIUKIDOS_HEADER_SIZE=26
CIUKIDOS_ABI_VERSION=2
CIUKIDOS_SERVICE_COUNT=11
CIUKIDOS_DESCRIPTOR_SIZE=8
CIUKIDOS_TABLE_HEADER_SIZE=10
CIUKIDOS_MIN_SIZE=$((CIUKIDOS_HEADER_SIZE + CIUKIDOS_TABLE_HEADER_SIZE + CIUKIDOS_SERVICE_COUNT * CIUKIDOS_DESCRIPTOR_SIZE))
CIUKIDOS_LAST_DESCRIPTOR_OFFSET=$((CIUKIDOS_HEADER_SIZE + CIUKIDOS_TABLE_HEADER_SIZE + (CIUKIDOS_SERVICE_COUNT - 1) * CIUKIDOS_DESCRIPTOR_SIZE))

cleanup_snapshot_dir() {
  if [[ -z "$SNAPSHOT_DIR" ]]; then
    return 0
  fi
  case "$SNAPSHOT_DIR" in
    /tmp/ciukios-full-runtime-probe.*) ;;
    *)
      echo "[qemu-test-full-runtime-probe] ERROR: refusing unsafe snapshot cleanup: $SNAPSHOT_DIR" >&2
      return 1
      ;;
  esac
  if [[ ! -d "$SNAPSHOT_DIR" || -L "$SNAPSHOT_DIR" || "${SNAPSHOT_DIR#/tmp/}" == */* ]]; then
    echo "[qemu-test-full-runtime-probe] ERROR: refusing unsafe snapshot cleanup: $SNAPSHOT_DIR" >&2
    return 1
  fi
  if [[ -n "$ORIGINAL_IMG" ]]; then
    case "$ORIGINAL_IMG" in
      "$SNAPSHOT_DIR"/*) rm -f -- "$ORIGINAL_IMG" ;;
      *) return 1 ;;
    esac
  fi
  if [[ -n "$GOOD_IMG" ]]; then
    case "$GOOD_IMG" in
      "$SNAPSHOT_DIR"/*) rm -f -- "$GOOD_IMG" ;;
      *) return 1 ;;
    esac
  fi
  if [[ -n "$CASE_BIN" ]]; then
    case "$CASE_BIN" in
      "$SNAPSHOT_DIR"/*) rm -f -- "$CASE_BIN" ;;
      *) return 1 ;;
    esac
  fi
  if ! rmdir -- "$SNAPSHOT_DIR"; then
    return 1
  fi
  SNAPSHOT_DIR=""
  ORIGINAL_IMG=""
  GOOD_IMG=""
  CASE_BIN=""
  return 0
}

restore_original_image() {
  local current_hash

  (( RESTORE_REQUIRED )) || return 0
  case "$SNAPSHOT_DIR" in
    /tmp/ciukios-full-runtime-probe.*) ;;
    *) return 1 ;;
  esac
  if [[ ! -d "$SNAPSHOT_DIR" || -L "$SNAPSHOT_DIR" || "${SNAPSHOT_DIR#/tmp/}" == */* ]]; then
    return 1
  fi
  if (( ORIGINAL_PRESENT )); then
    if [[ "$ORIGINAL_IMG" != "$SNAPSHOT_DIR/ciukios-full-original.img" \
      || ! -f "$ORIGINAL_IMG" || -L "$ORIGINAL_IMG" ]]; then
      echo "[qemu-test-full-runtime-probe] ERROR: original image snapshot is missing" >&2
      return 1
    fi
    if [[ -e "$BASE_IMG" && ! -f "$BASE_IMG" && ! -L "$BASE_IMG" ]]; then
      echo "[qemu-test-full-runtime-probe] ERROR: canonical image path is not a regular file" >&2
      return 1
    fi
    if ! rm -f -- "$BASE_IMG" || [[ -e "$BASE_IMG" || -L "$BASE_IMG" ]]; then
      echo "[qemu-test-full-runtime-probe] ERROR: cannot clear canonical image before restore" >&2
      return 1
    fi
    if ! cp -a --reflink=auto --sparse=always "$ORIGINAL_IMG" "$BASE_IMG"; then
      echo "[qemu-test-full-runtime-probe] ERROR: cannot restore $BASE_IMG" >&2
      return 1
    fi
    current_hash="$(sha256sum "$BASE_IMG" | awk '{print $1}')"
    if [[ "$current_hash" != "$ORIGINAL_HASH" ]]; then
      echo "[qemu-test-full-runtime-probe] ERROR: restored image hash mismatch" >&2
      return 1
    fi
  else
    if [[ -e "$BASE_IMG" || -L "$BASE_IMG" ]]; then
      rm -f -- "$BASE_IMG"
    fi
    if [[ -e "$BASE_IMG" || -L "$BASE_IMG" ]]; then
      echo "[qemu-test-full-runtime-probe] ERROR: canonical image did not return to its pre-test absent state" >&2
      return 1
    fi
  fi
  RESTORE_REQUIRED=0
}

restore_good_image() {
  local current_hash

  case "$GOOD_IMG" in
    "$SNAPSHOT_DIR"/ciukios-full-good.img) ;;
    *) return 1 ;;
  esac
  if [[ ! -f "$GOOD_IMG" || -L "$GOOD_IMG" ]]; then
    echo "[qemu-test-full-runtime-probe] ERROR: good-image snapshot is missing" >&2
    return 1
  fi
  if ! rm -f -- "$BASE_IMG" || [[ -e "$BASE_IMG" || -L "$BASE_IMG" ]]; then
    echo "[qemu-test-full-runtime-probe] ERROR: cannot clear canonical image before case restore" >&2
    return 1
  fi
  if ! cp -a --reflink=auto --sparse=always -- "$GOOD_IMG" "$BASE_IMG"; then
    echo "[qemu-test-full-runtime-probe] ERROR: cannot restore good image" >&2
    return 1
  fi
  current_hash="$(sha256sum "$BASE_IMG" | awk '{print $1}')"
  if [[ "$current_hash" != "$GOOD_HASH" ]]; then
    echo "[qemu-test-full-runtime-probe] ERROR: good-image restore hash mismatch" >&2
    return 1
  fi
}

read_u16_le() {
  local file_path="$1"
  local byte_offset="$2"
  od -An -v -tu1 -j "$byte_offset" -N 2 -- "$file_path" \
    | awk '{ print $1 + (256 * $2) }'
}

patch_case_bytes() {
  local byte_offset="$1"
  local encoded_bytes="$2"

  printf '%b' "$encoded_bytes" \
    | dd of="$CASE_BIN" bs=1 seek="$byte_offset" conv=notrunc status=none
}

prepare_negative_case() {
  local case_name="$1"

  restore_good_image
  rm -f -- "$CASE_BIN"

  case "$case_name" in
    missing)
      mdel -i "$BASE_IMG" "$RUNTIME_IMAGE_PATH" >/dev/null
      if mdir -i "$BASE_IMG" "$RUNTIME_IMAGE_PATH" >/dev/null 2>&1; then
        echo "[qemu-test-full-runtime-probe] FAIL [$case_name]: runtime still present after delete" >&2
        return 1
      fi
      return 0
      ;;
    truncated)
      dd if="$RUNTIME_BIN" of="$CASE_BIN" bs=1 count=$((CIUKIDOS_MIN_SIZE - 1)) status=none
      ;;
    bad_signature)
      cp -- "$RUNTIME_BIN" "$CASE_BIN"
      patch_case_bytes 2 '\x58'
      ;;
    bad_header_size)
      cp -- "$RUNTIME_BIN" "$CASE_BIN"
      patch_case_bytes 10 '\x19\x00'
      ;;
    incompatible_abi)
      cp -- "$RUNTIME_BIN" "$CASE_BIN"
      patch_case_bytes 12 '\x03\x00'
      ;;
    missing_header_service_count)
      cp -- "$RUNTIME_BIN" "$CASE_BIN"
      patch_case_bytes 14 '\x00\x00'
      ;;
    missing_table_service_count)
      cp -- "$RUNTIME_BIN" "$CASE_BIN"
      patch_case_bytes $((CIUKIDOS_HEADER_SIZE + 6)) '\x00\x00'
      ;;
    missing_service_descriptor)
      cp -- "$RUNTIME_BIN" "$CASE_BIN"
      patch_case_bytes "$CIUKIDOS_LAST_DESCRIPTOR_OFFSET" '\x00\x00'
      ;;
    *)
      echo "[qemu-test-full-runtime-probe] FAIL: unknown negative case: $case_name" >&2
      return 1
      ;;
  esac

  if [[ ! -f "$CASE_BIN" || -L "$CASE_BIN" || ! -s "$CASE_BIN" ]]; then
    echo "[qemu-test-full-runtime-probe] FAIL [$case_name]: invalid case payload" >&2
    return 1
  fi
  if cmp -s -- "$RUNTIME_BIN" "$CASE_BIN"; then
    echo "[qemu-test-full-runtime-probe] FAIL [$case_name]: mutation did not change runtime" >&2
    return 1
  fi
  mcopy -o -i "$BASE_IMG" "$CASE_BIN" "$RUNTIME_IMAGE_PATH"
}

run_negative_case() {
  local case_name="$1"
  local negative_log="${NEGATIVE_LOG_PREFIX}-${case_name}.log"
  local normalized_log="${negative_log}.normalized"

  echo "[qemu-test-full-runtime-probe] running negative case: $case_name"
  prepare_negative_case "$case_name"
  if ! LOG_FILE="$negative_log" STAGE1_MARKER="- CIUKIDOS.SYS missing or invalid" \
      bash scripts/qemu_run_full.sh --test --no-build; then
    echo "[qemu-test-full-runtime-probe] FAIL [$case_name]: fatal-path runner failed" >&2
    return 1
  fi
  if ! grep -aFq "WOOF! CiukiOS ran into a problem." "$normalized_log"; then
    echo "[qemu-test-full-runtime-probe] FAIL [$case_name]: loader fatal header missing" >&2
    return 1
  fi
  if ! grep -aFq -- "- CIUKIDOS.SYS missing or invalid" "$normalized_log"; then
    echo "[qemu-test-full-runtime-probe] FAIL [$case_name]: loader fatal detail missing" >&2
    return 1
  fi
  if grep -aFq "[CIUKRTST] OWNER=CIUKIDOS" "$normalized_log"; then
    echo "[qemu-test-full-runtime-probe] FAIL [$case_name]: invalid runtime reached the black-box owner probe" >&2
    return 1
  fi
  if grep -aEiq 'CiukiOS([[:space:]]+SHELL)?[[:space:]]+[CD]:[\\]' "$normalized_log"; then
    echo "[qemu-test-full-runtime-probe] FAIL [$case_name]: Stage1/SHELL prompt appeared" >&2
    return 1
  fi
  restore_good_image
  echo "[qemu-test-full-runtime-probe] PASS negative case: $case_name"
}

on_exit() {
  local rc="$1"
  local restored=1
  trap - EXIT HUP INT TERM
  if ! restore_original_image; then
    rc=1
    restored=0
  fi
  if (( restored )) && ! cleanup_snapshot_dir; then
    rc=1
  fi
  if (( ! restored )); then
    echo "[qemu-test-full-runtime-probe] ERROR: recovery snapshot preserved at $SNAPSHOT_DIR" >&2
  fi
  exit "$rc"
}

trap 'on_exit "$?"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

for command_name in awk cmp cp dd grep mcopy mdel mdir mkdir mktemp od python3 rm rmdir sha256sum stat; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "[qemu-test-full-runtime-probe] FAIL: missing command: $command_name" >&2
    exit 1
  fi
done

mkdir -p build/full
if [[ -L "$BASE_IMG" ]]; then
  echo "[qemu-test-full-runtime-probe] FAIL: refusing symlink canonical image: $BASE_IMG" >&2
  exit 1
fi
if ! SNAPSHOT_DIR="$(mktemp -d /tmp/ciukios-full-runtime-probe.XXXXXX)"; then
  echo "[qemu-test-full-runtime-probe] FAIL: cannot create snapshot directory under /tmp" >&2
  exit 1
fi
case "$SNAPSHOT_DIR" in
  /tmp/ciukios-full-runtime-probe.*) ;;
  *)
    echo "[qemu-test-full-runtime-probe] FAIL: unsafe snapshot directory: $SNAPSHOT_DIR" >&2
    exit 1
    ;;
esac
if [[ ! -d "$SNAPSHOT_DIR" || -L "$SNAPSHOT_DIR" || "${SNAPSHOT_DIR#/tmp/}" == */* ]]; then
  echo "[qemu-test-full-runtime-probe] FAIL: snapshot directory failed path-safety validation: $SNAPSHOT_DIR" >&2
  exit 1
fi
ORIGINAL_IMG="$SNAPSHOT_DIR/ciukios-full-original.img"
GOOD_IMG="$SNAPSHOT_DIR/ciukios-full-good.img"
CASE_BIN="$SNAPSHOT_DIR/ciukidos-negative.sys"
if [[ -f "$BASE_IMG" ]]; then
  ORIGINAL_PRESENT=1
  ORIGINAL_HASH="$(sha256sum "$BASE_IMG" | awk '{print $1}')"
  if ! cp -a --reflink=auto --sparse=always "$BASE_IMG" "$ORIGINAL_IMG"; then
    echo "[qemu-test-full-runtime-probe] FAIL: cannot snapshot $BASE_IMG" >&2
    exit 1
  fi
  if [[ "$(sha256sum "$ORIGINAL_IMG" | awk '{print $1}')" != "$ORIGINAL_HASH" ]]; then
    echo "[qemu-test-full-runtime-probe] FAIL: canonical image snapshot hash mismatch" >&2
    exit 1
  fi
elif [[ -e "$BASE_IMG" ]]; then
  echo "[qemu-test-full-runtime-probe] FAIL: canonical image is not a regular file: $BASE_IMG" >&2
  exit 1
fi
RESTORE_REQUIRED=1

echo "[qemu-test-full-runtime-probe] running canonical full build and black-box ABI2 probe"
bash scripts/build_full.sh
bash scripts/verify_phase5_runtime_ownership.sh --no-build
bash scripts/qemu_test_full_shell_com.sh --no-build
python3 scripts/serial_log_normalize.py "$POSITIVE_LOG" > "$POSITIVE_NORMALIZED_LOG"

if ! grep -aEq '\[CIUKRTST\][[:space:]]+OWNER=CIUKIDOS[[:space:]]+ABI=2[[:space:]]+SERVICES=11[[:space:]]+CHAIN=0[[:space:]]+STATE=PASS' "$POSITIVE_NORMALIZED_LOG"; then
  echo "[qemu-test-full-runtime-probe] FAIL: canonical CIUKRTST ABI2 marker missing" >&2
  exit 1
fi

if [[ ! -s "$RUNTIME_BIN" ]]; then
  echo "[qemu-test-full-runtime-probe] FAIL: missing or empty runtime: $RUNTIME_BIN" >&2
  exit 1
fi
runtime_size="$(stat -c%s "$RUNTIME_BIN")"
echo "[qemu-test-full-runtime-probe] runtime artifact: $RUNTIME_BIN ($runtime_size bytes)"

if (( runtime_size < CIUKIDOS_MIN_SIZE )); then
  echo "[qemu-test-full-runtime-probe] FAIL: runtime is smaller than the ABI minimum ($CIUKIDOS_MIN_SIZE)" >&2
  exit 1
fi
if [[ "$(dd if="$RUNTIME_BIN" bs=1 skip=2 count=8 status=none)" != "CIUKIDOS" \
  || "$(read_u16_le "$RUNTIME_BIN" 10)" != "$CIUKIDOS_HEADER_SIZE" \
  || "$(read_u16_le "$RUNTIME_BIN" 12)" != "$CIUKIDOS_ABI_VERSION" \
  || "$(read_u16_le "$RUNTIME_BIN" 14)" != "$CIUKIDOS_SERVICE_COUNT" \
  || "$(read_u16_le "$RUNTIME_BIN" 16)" != "$CIUKIDOS_DESCRIPTOR_SIZE" \
  || "$(read_u16_le "$RUNTIME_BIN" 22)" != "$CIUKIDOS_HEADER_SIZE" ]]; then
  echo "[qemu-test-full-runtime-probe] FAIL: pristine runtime does not match the expected ABI2 header layout" >&2
  exit 1
fi

if ! cp -a --reflink=auto --sparse=always -- "$BASE_IMG" "$GOOD_IMG"; then
  echo "[qemu-test-full-runtime-probe] FAIL: cannot snapshot positive image" >&2
  exit 1
fi
GOOD_HASH="$(sha256sum "$GOOD_IMG" | awk '{print $1}')"
if [[ -z "$GOOD_HASH" || "$(sha256sum "$BASE_IMG" | awk '{print $1}')" != "$GOOD_HASH" ]]; then
  echo "[qemu-test-full-runtime-probe] FAIL: positive image snapshot hash mismatch" >&2
  exit 1
fi

negative_cases=(
  missing
  truncated
  bad_signature
  bad_header_size
  incompatible_abi
  missing_header_service_count
  missing_table_service_count
  missing_service_descriptor
)
for case_name in "${negative_cases[@]}"; do
  run_negative_case "$case_name"
done

if ! restore_original_image; then
  echo "[qemu-test-full-runtime-probe] FAIL: canonical image restoration failed" >&2
  exit 1
fi
if ! cleanup_snapshot_dir; then
  echo "[qemu-test-full-runtime-probe] FAIL: temporary snapshot cleanup failed: $SNAPSHOT_DIR" >&2
  exit 1
fi
echo "[qemu-test-full-runtime-probe] PASS canonical black-box ABI2 probe and ${#negative_cases[@]} fatal negative cases"
