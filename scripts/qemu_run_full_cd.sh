#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
SERIAL_NORMALIZER="$ROOT_DIR/scripts/serial_log_normalize.py"

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
  --vga-fast           Use the measured TCG planar-VGA fast path. QEMU 11.1
                       is not stable with the local original Doom binary.
  --display <backend>  QEMU display backend in visual mode (default: auto;
                       SDL/X11 is preferred, GTK is the fallback).

Environment:
  QEMU_BIN         Override QEMU binary.
  QEMU_CPU_MODEL   CPU model (default: pentium3, single vCPU for DOS/Win 3.x).
  QEMU_MEMORY_MB   VM RAM in MiB (default: 256; below 160 uses the lowmem ISO).
  QEMU_EXTRA_ARGS  Extra args appended to QEMU command.
  QEMU_ACCEL_MODE  Accelerator: kvm, vga-fast, auto, tcg, or tcg-safe
                    (default: kvm). vga-fast is an explicit JIT profile for
                    planar-VGA workloads that are verified TCG-safe.
  QEMU_DISPLAY_TRANSPORT  Pointer transport: auto, x11, or native (default: auto).
  QEMU_AUDIO_MODE  Audio mode: off, auto, on (default: on).
  QEMU_AUDIO_DEVICES Guest sound card: standard (SB16/AdLib, default) or ac97.
  QEMU_AUDIO_BACKEND  Force backend for -audiodev (pipewire,pa,pulse,alsa,sdl,none).
  QEMU_NETWORK_MODE  Network mode: auto/user (default, outbound NAT), tap or off.
  QEMU_TIMEOUT_SEC Timeout in test mode (default: 8).
  LOG_FILE         Test log path (default: build/full/qemu-full-cd.log).
  STAGE0_MARKER    Marker 1 for test validation.
  STAGE1_MARKER    Stage1 readiness marker (default: external-shell banner).
  CIUKIOS_STAGE2_AUTORUN  Set 1 to trigger stage2 automatically.
TXT
}

MODE="visual"
DO_BUILD=1
DRY_RUN=0
DISPLAY_BACKEND="${QEMU_DISPLAY:-auto}"
AUDIO_MODE="${QEMU_AUDIO_MODE:-on}"
QEMU_AUDIO_ARGS=()
QEMU_AUDIO_DETAIL="off"
QEMU_MACHINE_ARG="pc,vmport=off,i8042=on"
QEMU_CPU_MODEL="${QEMU_CPU_MODEL:-pentium3}"
QEMU_MEMORY_MB="${QEMU_MEMORY_MB:-256}"
QEMU_ACCEL_ARGS=()
QEMU_ACCEL_DETAIL="default TCG"
QEMU_DISPLAY_TRANSPORT_DETAIL="native"

if [[ -z "$QEMU_CPU_MODEL" ]]; then
  echo "[qemu-run-full-cd] ERROR: QEMU_CPU_MODEL cannot be empty" >&2
  exit 1
fi

qemu_kvm_available() {
  [[ -c /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] \
    && "$QEMU_CMD" -accel help 2>/dev/null | grep -Fxq kvm
}

configure_accel_args() {
  local mode="${QEMU_ACCEL_MODE:-kvm}"

  QEMU_ACCEL_ARGS=()
  QEMU_ACCEL_DETAIL="default TCG"

  if [[ " ${QEMU_EXTRA_ARGS:-} " == *" -accel "* \
    || " ${QEMU_EXTRA_ARGS:-} " == *" -accel="* ]]; then
    QEMU_ACCEL_DETAIL="provided by QEMU_EXTRA_ARGS"
    return 0
  fi

  case "$mode" in
    vga-fast)
      QEMU_ACCEL_ARGS=(-accel tcg)
      QEMU_ACCEL_DETAIL="tcg JIT (legacy VGA fast path)"
      ;;
    kvm)
      if ! qemu_kvm_available; then
        echo "[qemu-run-full-cd] ERROR: hardware acceleration is required but KVM is unavailable" >&2
        echo "[qemu-run-full-cd] enable virtualization and grant access to /dev/kvm, or explicitly use QEMU_ACCEL_MODE=auto for the safe software fallback" >&2
        exit 1
      fi
      QEMU_ACCEL_ARGS=(-accel kvm)
      QEMU_ACCEL_DETAIL="kvm (required default)"
      ;;
    auto)
      if qemu_kvm_available; then
        QEMU_ACCEL_ARGS=(-accel kvm)
        QEMU_ACCEL_DETAIL="kvm (auto)"
      else
        QEMU_ACCEL_ARGS=(-accel 'tcg,one-insn-per-tb=on')
        QEMU_ACCEL_DETAIL="tcg one-insn-per-tb (explicit automatic fallback)"
      fi
      ;;
    tcg)
      QEMU_ACCEL_ARGS=(-accel tcg)
      QEMU_ACCEL_DETAIL="tcg (explicit software mode)"
      ;;
    tcg-safe)
      QEMU_ACCEL_ARGS=(-accel 'tcg,one-insn-per-tb=on')
      QEMU_ACCEL_DETAIL="tcg one-insn-per-tb (explicit software mode)"
      ;;
    *)
      echo "[qemu-run-full-cd] ERROR: invalid QEMU_ACCEL_MODE=$mode (expected vga-fast, kvm, auto, tcg or tcg-safe)" >&2
      exit 1
      ;;
  esac
}

configure_display_args() {
  local transport="${QEMU_DISPLAY_TRANSPORT:-auto}"
  local display_number=""
  local x11_socket=""

  case "$transport" in
    auto|x11|native) ;;
    *) echo "[qemu-run-full-cd] ERROR: QEMU_DISPLAY_TRANSPORT must be auto, x11 or native" >&2; exit 1 ;;
  esac

  if [[ "$DISPLAY_BACKEND" == "auto" ]]; then
    if "$QEMU_CMD" -display help 2>/dev/null | grep -Eq '(^|[[:space:]])sdl([[:space:]]|$)'; then
      DISPLAY_BACKEND="sdl,window-close=off"
    elif "$QEMU_CMD" -display help 2>/dev/null | grep -Eq '(^|[[:space:]])gtk([[:space:]]|$)'; then
      DISPLAY_BACKEND="gtk,gl=off,window-close=off"
    else
      echo "[qemu-run-full-cd] ERROR: QEMU provides neither SDL nor GTK display support" >&2
      exit 1
    fi
  elif [[ "$DISPLAY_BACKEND" == "sdl" ]]; then
    DISPLAY_BACKEND="sdl,window-close=off"
  elif [[ "$DISPLAY_BACKEND" == "gtk" ]]; then
    DISPLAY_BACKEND="gtk,gl=off,window-close=off"
  fi
  if [[ "$transport" == "native" ]]; then
    QEMU_DISPLAY_TRANSPORT_DETAIL="native (explicit override)"
    return 0
  fi
  if [[ "${DISPLAY:-}" =~ ^:([0-9]+)(\.[0-9]+)?$ ]]; then
    display_number="${BASH_REMATCH[1]}"
    x11_socket="/tmp/.X11-unix/X${display_number}"
    if [[ -S "$x11_socket" ]]; then
      if [[ "$DISPLAY_BACKEND" == gtk* ]]; then
        export GDK_BACKEND=x11
        QEMU_DISPLAY_TRANSPORT_DETAIL="gtk/x11 verified socket=$x11_socket (relative PS/2 grab)"
        return 0
      fi
      if [[ "$DISPLAY_BACKEND" == sdl* ]]; then
        export SDL_VIDEODRIVER=x11
        QEMU_DISPLAY_TRANSPORT_DETAIL="sdl/x11 verified socket=$x11_socket (relative PS/2 grab)"
        return 0
      fi
    fi
  fi
  if [[ "$transport" == "x11" || "${XDG_SESSION_TYPE:-}" == "wayland" ]]; then
    echo "[qemu-run-full-cd] ERROR: reliable relative mouse input requires an accessible X11/XWayland socket" >&2
    exit 1
  fi
  QEMU_DISPLAY_TRANSPORT_DETAIL="native (X11 socket not required on this session)"
}
if [[ ! "$QEMU_MEMORY_MB" =~ ^[0-9]+$ ]] \
  || (( QEMU_MEMORY_MB < 128 || QEMU_MEMORY_MB > 4096 )); then
  echo "[qemu-run-full-cd] ERROR: QEMU_MEMORY_MB must be an integer from 128 to 4096" >&2
  exit 1
fi

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
    for candidate in pipewire pa alsa sdl; do
      if audio_backend_supported "$candidate"; then
        backend="$candidate"
        break
      fi
    done
    [[ -n "$backend" ]] || backend="none"
  fi

  QEMU_AUDIO_ARGS=(-audiodev "${backend},id=snd0")
  case "${QEMU_AUDIO_DEVICES:-standard}" in
    standard)
      QEMU_AUDIO_ARGS+=(
        -device "sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=snd0"
        -device "adlib,audiodev=snd0")
      QEMU_AUDIO_DETAIL="backend=${backend} pcspk=on sb16=iobase=0x220 irq=7 dma=1 hdma=5 adlib=opl2 ports=0x388"
      ;;
    ac97)
      QEMU_AUDIO_ARGS+=(-device "AC97,audiodev=snd0")
      QEMU_AUDIO_DETAIL="backend=${backend} pcspk=on ac97=8086:2415"
      ;;
    *)
      echo "[qemu-run-full-cd] ERROR: QEMU_AUDIO_DEVICES must be standard or ac97" >&2
      exit 1
      ;;
  esac
  QEMU_MACHINE_ARG="pc,vmport=off,i8042=on,pcspk-audiodev=snd0"
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
    --vga-fast)
      export QEMU_ACCEL_MODE=vga-fast
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

IMG="build/full/ciukios-full-cd.iso"
if (( QEMU_MEMORY_MB < 160 )); then
  IMG="build/full/ciukios-full-cd-lowmem.iso"
fi
if [[ ! -f "$IMG" ]]; then
  echo "[qemu-run-full-cd] ERROR: image not found: $IMG" >&2
  exit 1
fi

configure_accel_args

if [[ "$MODE" == "test" ]]; then
  configure_audio_args headless
else
  configure_audio_args visual
fi

case "${QEMU_NETWORK_MODE:-auto}" in
  auto|user) QEMU_NETWORK_ARGS=(-netdev user,id=ciuknet0 -device ne2k_isa,netdev=ciuknet0,irq=3,iobase=0x300,mac=52:54:00:12:34:56) ;;
  tap) QEMU_NETWORK_ARGS=(-netdev "tap,id=ciuknet0,ifname=${QEMU_NET_TAP_IF:-ciukios0},script=no,downscript=no" -device ne2k_isa,netdev=ciuknet0,irq=3,iobase=0x300,mac=52:54:00:12:34:56) ;;
  off) QEMU_NETWORK_ARGS=(-nic none) ;;
  *) echo "[qemu-run-full-cd] ERROR: invalid QEMU_NETWORK_MODE" >&2; exit 1 ;;
esac

BASE_ARGS=(
  "${QEMU_ACCEL_ARGS[@]}"
  -machine "$QEMU_MACHINE_ARG"
  -cpu "$QEMU_CPU_MODEL"
  -m "$QEMU_MEMORY_MB"
  -drive "file=$IMG,format=raw,if=ide,index=2,media=cdrom,readonly=on"
  -boot d
  "${QEMU_NETWORK_ARGS[@]}"
)

if [[ "$MODE" == "test" ]]; then
  TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-8}"
  STAGE0_MARKER="${STAGE0_MARKER:-[BOOT0-FULL] CiukiOS full stage0 ready}"
  STAGE1_MARKER="${STAGE1_MARKER:-CiukiOS SHELL}"
  LOG_FILE="${LOG_FILE:-build/full/qemu-full-cd.log}"
  NORMALIZED_LOG="${LOG_FILE}.normalized"
  STDERR_FILE="${STDERR_FILE:-build/full/qemu-full-cd.stderr.log}"
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
  echo "[qemu-run-full-cd] resources: 1 x $QEMU_CPU_MODEL, ${QEMU_MEMORY_MB} MiB RAM"
  echo "[qemu-run-full-cd] accelerator: $QEMU_ACCEL_DETAIL"
  echo "[qemu-run-full-cd] audio: $QEMU_AUDIO_DETAIL"

  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf '[qemu-run-full-cd] dry-run:'
    printf ' %q' timeout "$TIMEOUT_SEC" "$QEMU_CMD" "${QEMU_ARGS[@]}"
    printf ' >/dev/null 2>&1 (serial -> %q)\n' "$LOG_FILE"
    printf '\n'
    exit 0
  fi

  mkdir -p "$(dirname "$LOG_FILE")"
  rm -f "$LOG_FILE" "$NORMALIZED_LOG"
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

  if ! "$SERIAL_NORMALIZER" "$LOG_FILE" > "$NORMALIZED_LOG"; then
    echo "[qemu-run-full-cd] FAIL (cannot normalize serial log)" >&2
    exit 1
  fi

  if grep -aFq -- "$STAGE0_MARKER" "$NORMALIZED_LOG" \
    && grep -aFq -- "$STAGE1_MARKER" "$NORMALIZED_LOG" \
    && grep -Eaq "CiukiOS([[:space:]]+SHELL)?[[:space:]]+D:" "$NORMALIZED_LOG"; then
    echo "[qemu-run-full-cd] PASS (stage0/readiness markers detected; Live CD prompt is D:)"
    exit 0
  fi

  echo "[qemu-run-full-cd] FAIL (stage0/readiness marker or Live CD D: prompt not detected)" >&2
  echo "[qemu-run-full-cd] serial log size: $(wc -c < "$LOG_FILE" 2>/dev/null || echo 0) bytes" >&2
  tail -n 80 "$NORMALIZED_LOG" >&2 || true
  exit 1
fi

configure_display_args

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
echo "[qemu-run-full-cd] display transport: $QEMU_DISPLAY_TRANSPORT_DETAIL"
echo "[qemu-run-full-cd] resources: 1 x $QEMU_CPU_MODEL, ${QEMU_MEMORY_MB} MiB RAM"
echo "[qemu-run-full-cd] accelerator: $QEMU_ACCEL_DETAIL"
echo "[qemu-run-full-cd] audio: $QEMU_AUDIO_DETAIL"
echo "[qemu-run-full-cd] serial log: build/full/qemu-full-cd-visual.log"
echo "[qemu-run-full-cd] mouse: PS/2 i8042 enabled; entering the window captures it; Ctrl+Alt+G releases it"

if [[ "$DRY_RUN" -eq 1 ]]; then
  printf '[qemu-run-full-cd] dry-run:'
  printf ' %q' "$QEMU_CMD" "${QEMU_ARGS[@]}"
  printf '\n'
  exit 0
fi

exec "$QEMU_CMD" "${QEMU_ARGS[@]}"
