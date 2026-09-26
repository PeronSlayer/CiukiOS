#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
PREFIX="${WOLF3D_AUDIO_PREFIX:-build/full/qemu-full-wolf3d-audio}"
WAV_FILE="$PREFIX.wav"
SCREENSHOT="$PREFIX.ppm"
EXIT_SCREENSHOT="$PREFIX-exit.ppm"
LOG_FILE="$PREFIX.log"
NORMALIZED_LOG="$PREFIX.normalized.log"
STDERR_LOG="$PREFIX.stderr.log"
CMD_LOG="$PREFIX.commands.log"
MON_SOCK="${WOLF3D_AUDIO_MON_SOCK:-/tmp/ciukios-wolf3d-audio.$$.sock}"
QEMU_ACCEL_MODE="${QEMU_ACCEL_MODE:-auto}"

if [[ "${1:-}" == "--no-build" ]]; then
  DO_BUILD=0
fi

for command_name in python3 socat mdir; do
  command -v "$command_name" >/dev/null 2>&1 \
    || { echo "[wolf3d-audio] ERROR missing command: $command_name" >&2; exit 1; }
done

DO_BUILD="$DO_BUILD" \
DOS_TAXONOMY_USE_CASE=wolf3d \
DOS_TAXONOMY_PROFILE=dos_generic \
DOS_TAXONOMY_MIN_STAGE=visual_gameplay \
DOS_TAXONOMY_APP_DIR_IN_IMAGE=::APPS/WOLF3D \
DOS_TAXONOMY_APP_BINARY_NAME=WOLF3D.COM \
DOS_TAXONOMY_CWD='\APPS\WOLF3D' \
DOS_TAXONOMY_RUN_COMMAND='run WOLF3D nowait tedlevel 0 normal' \
DOS_TAXONOMY_APP_RUNTIME_MARKERS='\[WOLF3D\][[:space:]]+LAUNCH[[:space:]]+AC97[[:space:]]+VSBHDA[[:space:]]+2[.]0[[:space:]]+TRANSIENT' \
DOS_TAXONOMY_POST_LAUNCH_KEYS='ctrl ctrl ctrl' \
DOS_TAXONOMY_POST_LAUNCH_KEY_INTERVAL_SEC=1 \
DOS_TAXONOMY_POST_LAUNCH_KEY_DELAY_SEC=35 \
DOS_TAXONOMY_SCREENSHOT="$SCREENSHOT" \
DOS_TAXONOMY_SCREENSHOT_DELAY_SEC=5 \
DOS_TAXONOMY_EXIT_KEYS='f10 y' \
DOS_TAXONOMY_EXIT_KEY_DELAY_SEC=5 \
DOS_TAXONOMY_EXIT_KEY_INTERVAL_SEC=3 \
DOS_TAXONOMY_AFTER_EXIT_DELAY_SEC=7 \
DOS_TAXONOMY_AFTER_EXIT_SCREENSHOT="$EXIT_SCREENSHOT" \
DOS_TAXONOMY_AFTER_EXIT_COMMAND='echo WOLF3D EXIT RETURNED' \
DOS_TAXONOMY_ALLOW_SHELL_RETURN=1 \
DOS_TAXONOMY_OBSERVE_SEC=15 \
DOS_TAXONOMY_RUN_DRVLOAD=0 \
QEMU_AUDIO_MODE=on \
QEMU_AUDIO_DEVICES=ac97 \
QEMU_AUDIO_BACKEND=wav \
QEMU_AUDIO_WAV_PATH="$WAV_FILE" \
QEMU_ACCEL_MODE="$QEMU_ACCEL_MODE" \
QEMU_TIMEOUT_SEC=210 \
LOG_FILE="$LOG_FILE" \
QEMU_STDERR="$STDERR_LOG" \
QEMU_CMD_LOG="$CMD_LOG" \
QEMU_MON_SOCK="$MON_SOCK" \
bash scripts/qemu_test_full_dos_taxonomy.sh

scripts/serial_log_normalize.py "$LOG_FILE" > "$NORMALIZED_LOG"
for marker in \
  '\[WOLF3D\][[:space:]]+LAUNCH[[:space:]]+AC97[[:space:]]+VSBHDA[[:space:]]+2[.]0[[:space:]]+TRANSIENT' \
  'Found[[:space:]]+sound[[:space:]]+card:[[:space:]]+82801AA' \
  '\[WOLF3D\][[:space:]]+AUDIO[[:space:]]+CLEANUP[[:space:]]+COMPLETE'; do
  grep -Eiq "$marker" "$NORMALIZED_LOG" \
    || { echo "[wolf3d-audio] FAIL runtime marker missing: $marker" >&2; exit 1; }
done
grep -Eiq 'WOLF3D[[:space:]]+EXIT[[:space:]]+RETURNED' "$NORMALIZED_LOG" \
  || { echo "[wolf3d-audio] FAIL game did not exit cleanly back to the CiukiOS shell" >&2; exit 1; }
if grep -Eiq 'DOS/32A fatal|Failed installing IO port trap|HDPMI32 is busy' "$NORMALIZED_LOG"; then
  echo "[wolf3d-audio] FAIL transient AC97/DPMI path did not clean up" >&2
  exit 1
fi

python3 scripts/analyze_audio_wav.py "$WAV_FILE" \
  --label wolf3d-ac97 --min-bytes 10000 --min-ac-rms 20 --min-unique 16 --min-changes 1000
echo "[wolf3d-audio] PASS native game audio reached AC97 through transient VSBHDA, and Quit returned to a working CiukiOS shell"
