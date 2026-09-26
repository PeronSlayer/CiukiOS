#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
BASE_IMG="${IMG:-build/full/ciukios-full.img}"
PREFIX="${DOOMVAN_PERF_PREFIX:-build/full/qemu-full-doomvan-performance}"
TICS="${DOOMVAN_PERF_TICS:-350}"
MAX_REALTICS="${DOOMVAN_PERF_MAX_REALTICS:-350}"
PERF_ACCEL="${QEMU_ACCEL_MODE:-vga-fast}"
SERIAL_NORMALIZER="$ROOT_DIR/scripts/serial_log_normalize.py"
TEMP_DIR=""

usage() {
  cat <<'TXT'
Usage: scripts/qemu_test_full_doomvan_performance.sh [--no-build]

Extracts a short timedemo from the packaged IWAD, runs it on an isolated image
with the explicit VGA-fast accelerator, and requires at least real-time
rendering. The canonical image and copyrighted IWAD are never modified.
TXT
}

cleanup() {
  if [[ -n "$TEMP_DIR" && -d "$TEMP_DIR" && "$TEMP_DIR" == /tmp/ciukios-doomvan-perf.* ]]; then
    rm -rf -- "$TEMP_DIR"
  fi
}
trap cleanup EXIT

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build) DO_BUILD=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "[doomvan-performance] ERROR unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$TICS" =~ ^[0-9]+$ && "$MAX_REALTICS" =~ ^[0-9]+$ ]] \
  || { echo '[doomvan-performance] ERROR tic limits must be positive integers' >&2; exit 1; }
for command_name in cp mcopy mdir python3 socat; do
  command -v "$command_name" >/dev/null 2>&1 \
    || { echo "[doomvan-performance] ERROR missing command: $command_name" >&2; exit 1; }
done

if [[ "$DO_BUILD" == "1" ]]; then
  bash scripts/build_full.sh
fi
[[ -f "$BASE_IMG" ]] || { echo "[doomvan-performance] ERROR missing image: $BASE_IMG" >&2; exit 1; }

TEMP_DIR="$(mktemp -d /tmp/ciukios-doomvan-perf.XXXXXX)"
TEST_IMG="$TEMP_DIR/ciukios-full.img"
WAD_FILE="$TEMP_DIR/doom.wad"
DEMO_FILE="$TEMP_DIR/short.lmp"
cp --reflink=auto -- "$BASE_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" ::APPS/DOOMVAN/DOOM.WAD "$WAD_FILE"
python3 scripts/extract_doom_demo.py "$WAD_FILE" "$DEMO_FILE" --tics "$TICS"
mcopy -o -i "$TEST_IMG" "$DEMO_FILE" ::APPS/DOOMVAN/SHORT.LMP

LOG_FILE="${PREFIX}.log"
STDERR_LOG="${PREFIX}.stderr.log"
CMD_LOG="${PREFIX}.commands.log"
NORMALIZED_LOG="${PREFIX}.normalized.log"
MON_SOCK="/tmp/ciukios-doomvan-performance.$$.sock"

# TCG boots DOS/4GW more slowly than KVM but renders legacy planar VGA much
# faster once gameplay starts. Keep enough startup headroom before evaluating
# Doom's own realtic counter. Returning to the shell is the expected successful
# end of a timedemo, so completion is validated from Doom's marker below.
DO_BUILD=0 \
IMG="$TEST_IMG" \
DOS_TAXONOMY_USE_CASE=generic \
DOS_TAXONOMY_PROFILE=dosapp \
DOS_TAXONOMY_MIN_STAGE=video_init \
DOS_TAXONOMY_DISPLAY_MODE=nographic \
DOS_TAXONOMY_APP_DIR_IN_IMAGE=::APPS/DOOMVAN \
DOS_TAXONOMY_APP_BINARY_NAME=PCDMCORE.EXE \
DOS_APP_AUX_PRIMARY=DOOM.WAD \
DOS_APP_AUX_ALIAS=DOOM.WAD \
DOS_TAXONOMY_CWD='\APPS\DOOMVAN' \
DOS_TAXONOMY_RUN_COMMAND='run PCDMCORE.EXE -nosound -timedemo short' \
DOS_TAXONOMY_OBSERVE_SEC=55 \
DOS_TAXONOMY_RUN_DRVLOAD=0 \
QEMU_AUDIO_MODE=off \
QEMU_ACCEL_MODE="$PERF_ACCEL" \
QEMU_TIMEOUT_SEC=180 \
LOG_FILE="$LOG_FILE" \
QEMU_STDERR="$STDERR_LOG" \
QEMU_CMD_LOG="$CMD_LOG" \
QEMU_MON_SOCK="$MON_SOCK" \
bash scripts/qemu_test_full_dos_taxonomy.sh

"$SERIAL_NORMALIZER" "$LOG_FILE" > "$NORMALIZED_LOG"
timing="$(grep -Eo "timed[[:space:]]+${TICS}[[:space:]]+gametics[[:space:]]+in[[:space:]]+[0-9]+[[:space:]]+realtics" "$NORMALIZED_LOG" | tail -n 1 || true)"
[[ -n "$timing" ]] \
  || { echo '[doomvan-performance] FAIL timedemo completion marker missing' >&2; exit 1; }
realtics="$(awk '{print $5}' <<<"$timing")"
if (( realtics > MAX_REALTICS )); then
  echo "[doomvan-performance] FAIL $timing limit=$MAX_REALTICS" >&2
  exit 1
fi

echo "[doomvan-performance] PASS $timing limit=$MAX_REALTICS (>=35 rendered fps)"
