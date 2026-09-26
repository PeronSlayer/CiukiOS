#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
PREFIX="${DOOM_AUDIO_PREFIX:-build/full/qemu-full-doom-audio}"
WAV_FILE="${PREFIX}.wav"
SCREENSHOT="${PREFIX}.ppm"
LOG_FILE="${PREFIX}.log"
STDERR_LOG="${PREFIX}.stderr.log"
CMD_LOG="${PREFIX}.commands.log"
MON_SOCK="${DOOM_AUDIO_MON_SOCK:-/tmp/ciukios-doom-audio.$$.sock}"
OBSERVE_SEC="${DOOM_AUDIO_OBSERVE_SEC:-45}"
QEMU_ACCEL_MODE="${QEMU_ACCEL_MODE:-kvm}"

usage() {
  cat <<'TXT'
Usage: scripts/qemu_test_full_doom_audio.sh [--no-build]

Runs bundled Doom through its transient VSBHDA launcher and QEMU's AC97 WAV
backend.  The gate requires DPMI/video stability, a real gameplay frame,
non-silent Sound Blaster/AdLib output and a clean return to the shell.
TXT
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build) DO_BUILD=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "[doom-audio] ERROR unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

for command_name in python3 socat mdir mtype; do
  command -v "$command_name" >/dev/null 2>&1 \
    || { echo "[doom-audio] ERROR missing command: $command_name" >&2; exit 1; }
done

mkdir -p "$(dirname "$PREFIX")"
rm -f -- "$WAV_FILE" "$SCREENSHOT" "$LOG_FILE" "$STDERR_LOG" "$CMD_LOG" "$MON_SOCK"

IMG="${IMG:-build/full/ciukios-full.img}"
mtype -i "$IMG" ::APPS/DOOM/DEFAULT.CFG | grep -Eq '^snd_musicdevice[[:space:]]+3[[:space:]]*$' \
  || { echo '[doom-audio] FAIL packaged Doom profile does not select AdLib music' >&2; exit 1; }
mtype -i "$IMG" ::APPS/DOOM/DEFAULT.CFG | grep -Eq '^snd_sfxdevice[[:space:]]+3[[:space:]]*$' \
  || { echo '[doom-audio] FAIL packaged Doom profile does not select Sound Blaster SFX' >&2; exit 1; }

DO_BUILD="$DO_BUILD" \
DOS_TAXONOMY_USE_CASE=doom \
DOS_TAXONOMY_PROFILE=dosapp \
DOS_TAXONOMY_MIN_STAGE=visual_gameplay \
DOS_TAXONOMY_VISUAL_PROFILE=doom_vga_gameplay \
DOS_TAXONOMY_DISPLAY_MODE=nographic \
DOS_TAXONOMY_RUN_COMMAND='run DOOM.EXE -warp 1 1' \
DOS_TAXONOMY_POST_LAUNCH_KEYS='ctrl ctrl ctrl' \
DOS_TAXONOMY_POST_LAUNCH_KEY_DELAY_SEC=35 \
DOS_TAXONOMY_POST_LAUNCH_KEY_INTERVAL_SEC=1 \
DOS_TAXONOMY_SCREENSHOT="$SCREENSHOT" \
DOS_TAXONOMY_SCREENSHOT_DELAY_SEC=5 \
DOS_TAXONOMY_EXIT_KEYS='f10 y' \
DOS_TAXONOMY_EXIT_KEY_INTERVAL_SEC=2 \
DOS_TAXONOMY_AFTER_EXIT_DELAY_SEC=7 \
DOS_TAXONOMY_AFTER_EXIT_COMMAND='echo DOOM EXIT RETURNED' \
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
scripts/serial_log_normalize.py "$LOG_FILE" > "$NORMALIZED_LOG"
for marker in \
  '\[DOOM\][[:space:]]+LAUNCH[[:space:]]+AC97[[:space:]]+VSBHDA[[:space:]]+2[.]0[[:space:]]+TRANSIENT' \
  'Found[[:space:]]+sound[[:space:]]+card:[[:space:]]+82801AA' \
  '\[DOOM\][[:space:]]+AUDIO[[:space:]]+CLEANUP[[:space:]]+COMPLETE' \
  'DOOM[[:space:]]+EXIT[[:space:]]+RETURNED'; do
  grep -Eiq "$marker" "$NORMALIZED_LOG" \
    || { echo "[doom-audio] FAIL runtime marker missing: $marker" >&2; exit 1; }
done
grep -Eiq 'DOS/32A fatal|Failed installing IO port trap|HDPMI32 is busy' "$NORMALIZED_LOG" \
  && { echo '[doom-audio] FAIL transient AC97/DPMI path did not clean up' >&2; exit 1; }

python3 scripts/analyze_audio_wav.py "$WAV_FILE" --label doom-ac97 \
  --min-bytes 10000 --min-ac-rms 20 --min-unique 16 --min-changes 1000

echo "[doom-audio] PASS Doom gameplay, native SB SFX/AdLib music and clean shell return remained stable"
