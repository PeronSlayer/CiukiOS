#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
PREFIX="${VIDEO_RESTORE_PREFIX:-build/full/qemu-full-video-restore}"
SCREENSHOT="${PREFIX}.ppm"
LOG_FILE="${PREFIX}.log"
STDERR_LOG="${PREFIX}.stderr.log"
CMD_LOG="${PREFIX}.commands.log"
MON_SOCK="${VIDEO_RESTORE_MON_SOCK:-/tmp/ciukios-video-restore.$$.sock}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build) DO_BUILD=0; shift ;;
    -h|--help)
      echo 'Usage: scripts/qemu_test_full_video_restore.sh [--no-build]'
      exit 0
      ;;
    *) echo "[video-restore] ERROR unknown option: $1" >&2; exit 2 ;;
  esac
done

DO_BUILD="$DO_BUILD" \
DOS_TAXONOMY_USE_CASE=generic \
DOS_TAXONOMY_PROFILE=dos_generic \
DOS_TAXONOMY_MIN_STAGE=transfer_marker \
DOS_TAXONOMY_DISPLAY_MODE=nographic \
DOS_TAXONOMY_APP_DIR_IN_IMAGE=::SYSTEM/DRIVERS \
DOS_TAXONOMY_APP_BINARY_NAME=VIDLEAVE.COM \
DOS_TAXONOMY_CWD='\SYSTEM\DRIVERS' \
DOS_TAXONOMY_RUN_COMMAND='run VIDLEAVE.COM' \
DOS_TAXONOMY_APP_RUNTIME_MARKERS='\[VIDLEAVE\][[:space:]]+MODE13' \
DOS_TAXONOMY_SCREENSHOT="$SCREENSHOT" \
DOS_TAXONOMY_SCREENSHOT_DELAY_SEC=3 \
DOS_TAXONOMY_OBSERVE_SEC=4 \
DOS_TAXONOMY_RUN_DRVLOAD=0 \
QEMU_AUDIO_MODE=off \
QEMU_ACCEL_MODE=kvm \
QEMU_TIMEOUT_SEC=180 \
LOG_FILE="$LOG_FILE" \
QEMU_STDERR="$STDERR_LOG" \
QEMU_CMD_LOG="$CMD_LOG" \
QEMU_MON_SOCK="$MON_SOCK" \
bash scripts/qemu_test_full_dos_taxonomy.sh

python3 scripts/analyze_textmode_screen.py "$SCREENSHOT" --label video-restore
scripts/serial_log_normalize.py "$LOG_FILE" \
  | grep -Eiq 'CiukiOS([[:space:]]+SHELL)?[[:space:]]+C:\\SYSTEM\\DRIVERS>' \
  || { echo '[video-restore] FAIL shell prompt did not return' >&2; exit 1; }

echo '[video-restore] PASS child graphics state was replaced by a clean shell text mode'
