#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

pick_qemu() {
  if [[ -n "${QEMU_BIN:-}" ]]; then
    echo "$QEMU_BIN"
    return
  fi
  if command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "qemu-system-i386"
    return
  fi
  if command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "qemu-system-x86_64"
    return
  fi
  return 1
}

usage() {
  cat << 'TXT'
Usage: scripts/qemu_run_full_cd.sh [--test] [--no-build] [--dry-run] [--display <backend>]

Modes:
  default            Visual run mode (GUI window).
  --test             Smoke-test mode (headless with timeout).

Options:
  --no-build           Skip image build step.
  --dry-run            Print the QEMU command without running it.
  --display <backend>  QEMU display backend in visual mode (default: gtk).

Environment:
  QEMU_BIN         Override QEMU binary.
  QEMU_EXTRA_ARGS  Extra args appended to QEMU command.
  QEMU_AUDIO_MODE  Audio mode: off, auto, on (default: on).
  QEMU_AUDIO_BACKEND  Force backend for -audiodev (pipewire,pa,pulse,alsa,sdl,none).
  QEMU_TIMEOUT_SEC Timeout in test mode (default: 8).
  LOG_FILE         Test log path (default: build/full/qemu-full-cd.log).
  STAGE0_MARKER    Marker 1 for test validation.
  STAGE1_MARKER    Marker 2 for test validation.
  CIUKIOS_STAGE2_AUTORUN  Set 1 to trigger stage2 automatically.
TXT
}

MODE="visual"
DO_BUILD=1
DRY_RUN=0
DISPLAY_BACKEND="${QEMU_DISPLAY:-gtk}"
AUDIO_MODE="${QEMU_AUDIO_MODE:-on}"
QEMU_AUDIO_ARGS=()
QEMU_AUDIO_DETAIL="off"
QEMU_MACHINE_ARG="pc,vmport=off"

normalize_audio_backend() {
  case "$1" in
    pulse|pulseaudio)
      echo "pa"
      ;;
    *)
      echo "$1"
      ;;
  esac
}

audio_backend_supported() {
  local backend="$1"

  case "$backend" in
    none|alsa|dbus|jack|oss|pa|pipewire|sdl|spice|wav) ;;
    *) return 1 ;;
  esac

  [[ "$backend" == "none" ]] && return 0
  "$QEMU_CMD" -audiodev help 2>/dev/null | grep -Eq "^${backend}$"
}

configure_audio_args() {
  local context="$1"
  local requested_backend="${QEMU_AUDIO_BACKEND:-}"
  local backend=""
  local candidate

  QEMU_AUDIO_ARGS=()
  QEMU_AUDIO_DETAIL="off"

  case "$AUDIO_MODE" in
    auto|on|off) ;;
    *)
      echo "[qemu-run-full-cd] ERROR: invalid QEMU_AUDIO_MODE=$AUDIO_MODE (expected auto, on or off)" >&2
      exit 1
      ;;
  esac

  if [[ "$AUDIO_MODE" == "off" ]]; then
    return 0
  fi

  if [[ -n "$requested_backend" ]]; then
    backend="$(normalize_audio_backend "$requested_backend")"
    if ! audio_backend_supported "$backend"; then
      echo "[qemu-run-full-cd] ERROR: unsupported QEMU_AUDIO_BACKEND=$requested_backend for $QEMU_CMD" >&2
      exit 1
    fi
  elif [[ "$context" == "headless" && "$AUDIO_MODE" != "on" ]]; then
    backend="none"
  else
    for candidate in alsa pipewire pa sdl; do
      if audio_backend_supported "$candidate"; then
        backend="$candidate"
        break
      fi
    done
    [[ -n "$backend" ]] || backend="none"
  fi

  QEMU_AUDIO_ARGS=(
    -audiodev "${backend},id=snd0"
    -device "sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=snd0"
  )
  QEMU_MACHINE_ARG="pc,vmport=off,pcspk-audiodev=snd0"
  QEMU_AUDIO_DETAIL="backend=${backend} pcspk=on sb16=iobase=0x220 irq=7 dma=1 hdma=5"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --test)
      MODE="test"
      shift
      ;;
    --no-build)
      DO_BUILD=0
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --display)
      DISPLAY_BACKEND="${2:-}"
      if [[ -z "$DISPLAY_BACKEND" ]]; then
        echo "[qemu-run-full-cd] ERROR: missing value for --display" >&2
        exit 1
      fi
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[qemu-run-full-cd] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if ! QEMU_CMD="$(pick_qemu)"; then
  echo "[qemu-run-full-cd] ERROR: QEMU not found (set QEMU_BIN to override)." >&2
  exit 1
fi

if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "[qemu-run-full-cd] build step"
  bash scripts/build_full_cd.sh
fi

IMG="build/full/ciukios-full-cd-direct.iso"
if [[ ! -f "$IMG" ]]; then
  echo "[qemu-run-full-cd] ERROR: image not found: $IMG" >&2
  exit 1
fi

BASE_ARGS=(
  -machine "$QEMU_MACHINE_ARG"
  -cpu pentium3
  -m 128
  -drive "file=$IMG,format=raw,if=ide,index=2,media=cdrom,readonly=on"
  -boot d
)

if [[ "$MODE" == "test" ]]; then
  TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-8}"
  STAGE0_MARKER="${STAGE0_MARKER:-[BOOT0-FULL] CiukiOS full stage0 ready}"
  STAGE1_MARKER="${STAGE1_MARKER:-[STAGE1-SERIAL] READY}"
  LOG_FILE="${LOG_FILE:-build/full/qemu-full-cd.log}"
  STDERR_FILE="${STDERR_FILE:-build/full/qemu-full-cd.stderr.log}"
  configure_audio_args headless

  QEMU_ARGS=(
    "${BASE_ARGS[@]}"
    -nographic
    -chardev "file,id=ser0,path=$LOG_FILE"
    -serial chardev:ser0
    -monitor none
    "${QEMU_AUDIO_ARGS[@]}"
    -no-reboot
    -no-shutdown
  )

  if [[ -n "${QEMU_EXTRA_ARGS:-}" ]]; then
    # shellcheck disable=SC2206
    EXTRA_ARGS=(${QEMU_EXTRA_ARGS})
    QEMU_ARGS+=("${EXTRA_ARGS[@]}")
  fi

  echo "[qemu-run-full-cd] running smoke test with $QEMU_CMD (timeout=${TIMEOUT_SEC}s)"
  echo "[qemu-run-full-cd] audio: $QEMU_AUDIO_DETAIL"

  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf '[qemu-run-full-cd] dry-run:'
    printf ' %q' timeout "$TIMEOUT_SEC" "$QEMU_CMD" "${QEMU_ARGS[@]}"
    printf ' >/dev/null 2>&1 (serial -> %q)\n' "$LOG_FILE"
    printf '\n'
    exit 0
  fi

  mkdir -p "$(dirname "$LOG_FILE")"
  rm -f "$LOG_FILE"
  rm -f "$STDERR_FILE"

  set +e
  timeout "$TIMEOUT_SEC" "$QEMU_CMD" "${QEMU_ARGS[@]}" >/dev/null 2>"$STDERR_FILE"
  RC=$?
  set -e

  if [[ $RC -ne 0 && $RC -ne 124 ]]; then
    echo "[qemu-run-full-cd] FAIL (qemu exit code: $RC)" >&2
    if [[ -s "$STDERR_FILE" ]]; then
      echo "[qemu-run-full-cd] qemu stderr:" >&2
      tail -n 40 "$STDERR_FILE" >&2 || true
    fi
    tail -n 80 "$LOG_FILE" >&2 || true
    exit "$RC"
  fi

  if grep -Fq "$STAGE0_MARKER" "$LOG_FILE" && grep -Eaq "CiukiOS[[:space:]]+D:|CCiiuukkiiOOSS[[:space:]]+DD::" "$LOG_FILE"; then
    echo "[qemu-run-full-cd] PASS (Live CD prompt is D:)"
    exit 0
  fi

  echo "[qemu-run-full-cd] FAIL (Live CD D: prompt not detected)" >&2
  echo "[qemu-run-full-cd] serial log size: $(wc -c < "$LOG_FILE" 2>/dev/null || echo 0) bytes" >&2
  tail -n 80 "$LOG_FILE" >&2 || true
  exit 1
fi

configure_audio_args visual

QEMU_ARGS=(
  "${BASE_ARGS[@]}"
  -display "$DISPLAY_BACKEND"
  -chardev "file,id=ser0,path=build/full/qemu-full-cd-visual.log"
  -serial chardev:ser0
  "${QEMU_AUDIO_ARGS[@]}"
)

if [[ -n "${QEMU_EXTRA_ARGS:-}" ]]; then
  # shellcheck disable=SC2206
  EXTRA_ARGS=(${QEMU_EXTRA_ARGS})
  QEMU_ARGS+=("${EXTRA_ARGS[@]}")
fi

echo "[qemu-run-full-cd] starting visual Live/install CD QEMU session"
echo "[qemu-run-full-cd] Live/install CD boot"
echo "[qemu-run-full-cd] audio: $QEMU_AUDIO_DETAIL"
echo "[qemu-run-full-cd] serial log: build/full/qemu-full-cd-visual.log"

if [[ "$DRY_RUN" -eq 1 ]]; then
  printf '[qemu-run-full-cd] dry-run:'
  printf ' %q' "$QEMU_CMD" "${QEMU_ARGS[@]}"
  printf '\n'
  exit 0
fi

exec "$QEMU_CMD" "${QEMU_ARGS[@]}"
