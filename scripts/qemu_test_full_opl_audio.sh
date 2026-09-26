#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
PREFIX="${OPL_AUDIO_PREFIX:-build/full/qemu-full-opl-audio}"
WAV_FILE="${PREFIX}.wav"
SCREENSHOT="${PREFIX}.ppm"
LOG_FILE="${PREFIX}.log"
STDERR_LOG="${PREFIX}.stderr.log"
CMD_LOG="${PREFIX}.commands.log"
MON_SOCK="${OPL_AUDIO_MON_SOCK:-/tmp/ciukios-opl-audio.$$.sock}"
SERIAL_NORMALIZER="$ROOT_DIR/scripts/serial_log_normalize.py"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build) DO_BUILD=0; shift ;;
    -h|--help)
      echo 'Usage: scripts/qemu_test_full_opl_audio.sh [--no-build]'
      exit 0
      ;;
    *) echo "[opl-audio] ERROR unknown option: $1" >&2; exit 2 ;;
  esac
done

DO_BUILD="$DO_BUILD" \
DOS_TAXONOMY_USE_CASE=generic \
DOS_TAXONOMY_PROFILE=dosapp \
DOS_TAXONOMY_MIN_STAGE=visual_gameplay \
DOS_TAXONOMY_VISUAL_PROFILE=doom_vga_gameplay \
DOS_TAXONOMY_DISPLAY_MODE=nographic \
DOS_TAXONOMY_APP_DIR_IN_IMAGE=::APPS/DOOMVAN \
DOS_TAXONOMY_APP_BINARY_NAME=PCDOOM.EXE \
DOS_APP_AUX_PRIMARY=DOOM.WAD \
DOS_APP_AUX_ALIAS=DOOM.WAD \
DOS_TAXONOMY_CWD='\APPS\DOOMVAN' \
DOS_TAXONOMY_RUN_COMMAND='run PCDOOM.EXE -nosfx -devparm' \
DOS_TAXONOMY_POST_LAUNCH_KEYS='esc ret ret ret' \
DOS_TAXONOMY_POST_LAUNCH_KEY_DELAY_SEC=18 \
DOS_TAXONOMY_POST_LAUNCH_KEY_INTERVAL_SEC=2 \
DOS_TAXONOMY_SCREENSHOT="$SCREENSHOT" \
DOS_TAXONOMY_SCREENSHOT_DELAY_SEC=10 \
DOS_TAXONOMY_OBSERVE_SEC=32 \
DOS_TAXONOMY_RUN_DRVLOAD=0 \
QEMU_AUDIO_MODE=on \
QEMU_AUDIO_BACKEND=wav \
QEMU_AUDIO_WAV_PATH="$WAV_FILE" \
QEMU_ACCEL_MODE=kvm \
QEMU_TIMEOUT_SEC=260 \
LOG_FILE="$LOG_FILE" \
QEMU_STDERR="$STDERR_LOG" \
QEMU_CMD_LOG="$CMD_LOG" \
QEMU_MON_SOCK="$MON_SOCK" \
bash scripts/qemu_test_full_dos_taxonomy.sh

NORMALIZED_LOG="${PREFIX}.normalized.log"
"$SERIAL_NORMALIZER" "$LOG_FILE" > "$NORMALIZED_LOG"
grep -Eiq 'Music[[:space:]]+device[[:space:]]+#2[[:space:]]+&[[:space:]]+dmxCode=2' "$NORMALIZED_LOG" \
  || { echo '[opl-audio] FAIL AdLib music selection marker missing' >&2; exit 1; }
grep -Eiq 'Sfx[[:space:]]+device[[:space:]]+#0[[:space:]]+&[[:space:]]+dmxCode=0' "$NORMALIZED_LOG" \
  || { echo '[opl-audio] FAIL music-only SFX-disable marker missing' >&2; exit 1; }
grep -Eiq 'DMX_Init\(\)[[:space:]]+returned[[:space:]]+2' "$NORMALIZED_LOG" \
  || { echo '[opl-audio] FAIL AdLib DMX initialization marker missing' >&2; exit 1; }

python3 scripts/analyze_audio_wav.py "$WAV_FILE" --label opl-audio \
  --min-bytes 100000 --min-unique 64 --min-changes 1000

echo '[opl-audio] PASS external DOS/4GW AdLib/OPL2 music remained audible and stable'
