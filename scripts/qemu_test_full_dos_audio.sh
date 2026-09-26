#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
PREFIX="${DOS_AUDIO_PREFIX:-build/full/qemu-full-dos-audio}"
REALMODE_WAV="${PREFIX}-realmode.wav"
PM_WAV="${PREFIX}-protected.wav"
PM_LOG="${PREFIX}-protected.log"
PM_STDERR="${PREFIX}-protected.stderr.log"
PM_COMMANDS="${PREFIX}-protected.commands.log"
PM_MON_SOCK="${DOS_AUDIO_PM_MON_SOCK:-/tmp/ciukios-dos-audio-pm.$$.sock}"
QEMU_ACCEL_MODE="${QEMU_ACCEL_MODE:-kvm}"

usage() {
  cat <<'TXT'
Usage: scripts/qemu_test_full_dos_audio.sh [--no-build]

Runs the DOS audio compatibility stack on one full image:
  1. real-mode SB16 DSP + DMA1 + IRQ7 playback and shell return;
  2. DOS/4GW protected-mode timer + DMA + IRQ reflection and shell return;
  3. external DOS/4GW AdLib/OPL2 music with SFX disabled;
  4. rebuildable doom-vanille gameplay with OPL2 music and SB16 SFX.

Every layer records a WAV file and rejects silent or constant output.
TXT
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build) DO_BUILD=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "[dos-audio] ERROR unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

for command_name in python3 socat mdir; do
  command -v "$command_name" >/dev/null 2>&1 \
    || { echo "[dos-audio] ERROR missing command: $command_name" >&2; exit 1; }
done

if (( DO_BUILD )); then
  bash scripts/build_full.sh
fi

IMG="build/full/ciukios-full.img"
[[ -f "$IMG" ]] || { echo "[dos-audio] ERROR missing image: $IMG" >&2; exit 1; }
for image_path in \
  ::SYSTEM/DRIVERS/SB16INIT.COM \
  ::SYSTEM/DRIVERS/PMIRQSB.LE \
  ::SYSTEM/DRIVERS/DOS4GW.EXE \
  ::APPS/DOOMVAN/PCDOOM.EXE \
  ::APPS/DOOMVAN/DOOM.WAD; do
  mdir -i "$IMG" "$image_path" >/dev/null 2>&1 \
    || { echo "[dos-audio] ERROR missing required payload: $image_path" >&2; exit 1; }
done

mkdir -p "$(dirname "$PREFIX")"
rm -f -- "$REALMODE_WAV" "$PM_WAV" "$PM_LOG" "$PM_STDERR" "$PM_COMMANDS" "$PM_MON_SOCK"

echo "[dos-audio] layer 1/4: real-mode SB16 DMA/IRQ"
DO_BUILD=0 \
DRVLOAD_ARGS=/AUDIO \
QEMU_AUDIO_MODE=on \
QEMU_AUDIO_BACKEND=wav \
QEMU_AUDIO_WAV_PATH="$REALMODE_WAV" \
bash scripts/qemu_test_full_drvload_smoke.sh --no-build
python3 scripts/analyze_audio_wav.py "$REALMODE_WAV" \
  --label dos-audio-realmode --min-bytes 5000 --min-unique 4 --min-changes 20

echo "[dos-audio] layer 2/4: DOS/4GW protected-mode SB16 DMA/IRQ"
DO_BUILD=0 \
DOS_TAXONOMY_USE_CASE=generic \
DOS_TAXONOMY_PROFILE=dos_generic \
DOS_TAXONOMY_MIN_STAGE=transfer_marker \
DOS_TAXONOMY_APP_DIR_IN_IMAGE=::SYSTEM/DRIVERS \
DOS_TAXONOMY_APP_BINARY_NAME=PMIRQSB.LE \
DOS_TAXONOMY_CWD='\SYSTEM\DRIVERS' \
DOS_TAXONOMY_RUN_COMMAND='run DOS4GW.EXE PMIRQSB.LE TASK' \
DOS_TAXONOMY_APP_RUNTIME_MARKERS='\[PMIRQSB\][[:space:]]+TASK[[:space:]]+PASS.*\[PMIRQSB\][[:space:]]+PASS|\[PMIRQSB\][[:space:]]+PASS' \
DOS_TAXONOMY_OBSERVE_SEC=5 \
DOS_TAXONOMY_RUN_DRVLOAD=0 \
QEMU_AUDIO_MODE=on \
QEMU_AUDIO_BACKEND=wav \
QEMU_AUDIO_WAV_PATH="$PM_WAV" \
QEMU_ACCEL_MODE="$QEMU_ACCEL_MODE" \
QEMU_TIMEOUT_SEC=220 \
LOG_FILE="$PM_LOG" \
QEMU_STDERR="$PM_STDERR" \
QEMU_CMD_LOG="$PM_COMMANDS" \
QEMU_MON_SOCK="$PM_MON_SOCK" \
bash scripts/qemu_test_full_dos_taxonomy.sh
scripts/serial_log_normalize.py "$PM_LOG" | grep -Eiq '\[PMIRQSB\][[:space:]]+TASK[[:space:]]+PASS' \
  || { echo "[dos-audio] FAIL protected-mode TASK PASS marker missing" >&2; exit 1; }
python3 scripts/analyze_audio_wav.py "$PM_WAV" \
  --label dos-audio-protected --min-bytes 20000 --min-unique 4 --min-changes 20

echo "[dos-audio] layer 3/4: external DOS/4GW AdLib/OPL2 music"
DO_BUILD=0 QEMU_ACCEL_MODE="$QEMU_ACCEL_MODE" \
  bash scripts/qemu_test_full_opl_audio.sh --no-build

echo "[dos-audio] layer 4/4: external DOS/4GW OPL2 + SB16 mixer"
DO_BUILD=0 QEMU_ACCEL_MODE="$QEMU_ACCEL_MODE" \
  bash scripts/qemu_test_full_doomvan_audio.sh --no-build

echo "[dos-audio] PASS real-mode, protected-mode, OPL2 music, and external mixed audio layers"
