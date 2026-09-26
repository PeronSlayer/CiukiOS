#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
PREFIX="${DOOMVAN_AUDIO_PREFIX:-build/full/qemu-full-doomvan-audio}"
WAV_FILE="${PREFIX}.wav"
SCREENSHOT="${PREFIX}.ppm"
LOG_FILE="${PREFIX}.log"
STDERR_LOG="${PREFIX}.stderr.log"
CMD_LOG="${PREFIX}.commands.log"
MON_SOCK="${DOOMVAN_AUDIO_MON_SOCK:-/tmp/ciukios-doomvan-audio.$$.sock}"
OBSERVE_SEC="${DOOMVAN_AUDIO_OBSERVE_SEC:-45}"
QEMU_ACCEL_MODE="${QEMU_ACCEL_MODE:-kvm}"
SERIAL_NORMALIZER="$ROOT_DIR/scripts/serial_log_normalize.py"

usage() {
  cat <<'TXT'
Usage: scripts/qemu_test_full_doomvan_audio.sh [--no-build]

Runs the rebuildable doom-vanille DOS/4GW application through its transient
VSBHDA launcher.  The gate requires correct gameplay rendering, measures the
AC97 waveform and verifies that Quit returns to a working shell.
TXT
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build) DO_BUILD=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "[doomvan-audio] ERROR unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

for command_name in python3 socat mdir; do
  command -v "$command_name" >/dev/null 2>&1 \
    || { echo "[doomvan-audio] ERROR missing command: $command_name" >&2; exit 1; }
done
[[ -x "$SERIAL_NORMALIZER" ]] \
  || { echo "[doomvan-audio] ERROR missing serial normalizer" >&2; exit 1; }

mkdir -p "$(dirname "$PREFIX")"
rm -f -- "$WAV_FILE" "$SCREENSHOT" "$LOG_FILE" "$STDERR_LOG" "$CMD_LOG" "$MON_SOCK"

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
DOS_TAXONOMY_RUN_COMMAND='run PCDOOM.EXE -devparm -warp 1 1' \
DOS_TAXONOMY_POST_LAUNCH_KEYS='ctrl ctrl ctrl' \
DOS_TAXONOMY_POST_LAUNCH_KEY_DELAY_SEC=35 \
DOS_TAXONOMY_POST_LAUNCH_KEY_INTERVAL_SEC=1 \
DOS_TAXONOMY_SCREENSHOT="$SCREENSHOT" \
DOS_TAXONOMY_SCREENSHOT_DELAY_SEC=5 \
DOS_TAXONOMY_EXIT_KEYS='f10 y' \
DOS_TAXONOMY_EXIT_KEY_INTERVAL_SEC=2 \
DOS_TAXONOMY_AFTER_EXIT_DELAY_SEC=7 \
DOS_TAXONOMY_AFTER_EXIT_COMMAND='echo DOOMVAN EXIT RETURNED' \
DOS_TAXONOMY_ALLOW_SHELL_RETURN=1 \
DOS_TAXONOMY_OBSERVE_SEC="$OBSERVE_SEC" \
DOS_TAXONOMY_RUN_DRVLOAD=0 \
QEMU_AUDIO_MODE=on \
QEMU_AUDIO_DEVICES=ac97 \
QEMU_AUDIO_BACKEND=wav \
QEMU_AUDIO_WAV_PATH="$WAV_FILE" \
QEMU_ACCEL_MODE="$QEMU_ACCEL_MODE" \
QEMU_TIMEOUT_SEC=$((OBSERVE_SEC + 240)) \
LOG_FILE="$LOG_FILE" \
QEMU_STDERR="$STDERR_LOG" \
QEMU_CMD_LOG="$CMD_LOG" \
QEMU_MON_SOCK="$MON_SOCK" \
bash scripts/qemu_test_full_dos_taxonomy.sh

NORMALIZED_LOG="${PREFIX}.normalized.log"
"$SERIAL_NORMALIZER" "$LOG_FILE" > "$NORMALIZED_LOG"
grep -Eiq '\[DOOMVAN\][[:space:]]+LAUNCH[[:space:]]+STABLE[[:space:]]+PC-SPEAKER[[:space:]]+BUILD' "$NORMALIZED_LOG" \
  && { echo "[doomvan-audio] FAIL obsolete PC-speaker launcher was used" >&2; exit 1; }
for marker in \
  '\[DOOMVAN\][[:space:]]+LAUNCH[[:space:]]+AC97[[:space:]]+VSBHDA[[:space:]]+2[.]0[[:space:]]+TRANSIENT' \
  'Found[[:space:]]+sound[[:space:]]+card:[[:space:]]+82801AA' \
  '\[DOOMVAN\][[:space:]]+AUDIO[[:space:]]+CLEANUP[[:space:]]+COMPLETE' \
  'DOOMVAN[[:space:]]+EXIT[[:space:]]+RETURNED'; do
  grep -Eiq "$marker" "$NORMALIZED_LOG" \
    || { echo "[doomvan-audio] FAIL runtime marker missing: $marker" >&2; exit 1; }
done
if grep -Eiq 'DOS/32A fatal|Failed installing IO port trap|HDPMI32 is busy' "$NORMALIZED_LOG"; then
  echo "[doomvan-audio] FAIL transient AC97/DPMI path did not clean up" >&2
  exit 1
fi
python3 scripts/analyze_audio_wav.py "$WAV_FILE" --label doomvan-ac97 \
  --min-bytes 10000 --min-ac-rms 20 --min-unique 16 --min-changes 1000

echo "[doomvan-audio] PASS DOS/4GW gameplay produced native SB/AdLib audio through AC97 and returned cleanly"
