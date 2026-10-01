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
Usage: scripts/qemu_run_full.sh [--test] [--no-build] [--dry-run] [--display <backend>]

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
  QEMU_MEMORY_MB   VM RAM in MiB (default: 256; minimum: 64).
  CIUKIOS_FULL_IMG Disk image (default: build/full/ciukios-full.img).
  QEMU_EXTRA_ARGS  Extra args appended to QEMU command.
  QEMU_ACCEL_MODE  Accelerator: kvm, vga-fast, auto, tcg, or tcg-safe
                    (default: kvm). vga-fast is an explicit JIT profile for
                    planar-VGA workloads that are verified TCG-safe.
  QEMU_DISPLAY_TRANSPORT  Pointer transport: auto, x11, or native (default: auto).
                    auto forces verified X11/XWayland for reliable PS/2 grabs.
  QEMU_VIDEO_SIZE  Optional preferred VGA EDID size, WIDTHxHEIGHT. The guest
                   still chooses its own mode for DOS games.
  QEMU_AUDIO_MODE  Audio mode: off, auto, on (default: on).
  QEMU_AUDIO_DEVICES Guest sound card: standard (SB16/AdLib, default) or ac97.
  QEMU_AUDIO_BACKEND  Force backend for -audiodev (pipewire,pa,pulse,alsa,sdl,none).
  QEMU_NETWORK_MODE  Network mode: auto, user, tap, off (default: auto; user in GUI).
  QEMU_NET_HOST_FTP_PORT  Host port forwarded to CiukiOS FTP/21 (default: 8021).
                         Passive FTP data uses 127.0.0.1:2048.
  QEMU_NET_TAP_IF    Preconfigured TAP interface for tap mode (default: ciukios0).
                     Tap mode makes 10.0.2.15 directly reachable, including ICMP.
  QEMU_LEGACY_NAV_KEYS  Diagnostic-only remap of dedicated navigation keys to
                    keypad scan codes (default: 0; normal DOS input stays native).
  QEMU_KEYMAP_LAYOUT  Base QEMU keymap used for visual input (default: en-us).
  QEMU_TIMEOUT_SEC Timeout in test mode (default: 8).
  LOG_FILE         Test log path (default: build/full/qemu-full.log).
  QEMU_VISUAL_LOG  Visual run serial log (default: build/full/qemu-visual.log).
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
QEMU_NETWORK_ARGS=()
QEMU_NETWORK_DETAIL="off"
QEMU_ACCEL_ARGS=()
QEMU_ACCEL_DETAIL="default TCG"
QEMU_MACHINE_ARG="pc,vmport=off,i8042=on"
QEMU_KEYBOARD_ARGS=()
QEMU_KEYBOARD_DETAIL="native"
QEMU_DISPLAY_TRANSPORT_DETAIL="native"
QEMU_MOUSE_INPUT_DETAIL="display backend default"
QEMU_CPU_MODEL="${QEMU_CPU_MODEL:-pentium3}"
QEMU_MEMORY_MB="${QEMU_MEMORY_MB:-256}"
VISUAL_LOG="${QEMU_VISUAL_LOG:-build/full/qemu-visual.log}"
QEMU_VIDEO_ARGS=(-vga std)
QEMU_VIDEO_DETAIL="std (automatic EDID)"

if [[ -z "$QEMU_CPU_MODEL" ]]; then
  echo "[qemu-run-full] ERROR: QEMU_CPU_MODEL cannot be empty" >&2
  exit 1
fi
if [[ ! "$QEMU_MEMORY_MB" =~ ^[0-9]+$ ]] \
  || (( QEMU_MEMORY_MB < 64 || QEMU_MEMORY_MB > 4096 )); then
  echo "[qemu-run-full] ERROR: QEMU_MEMORY_MB must be an integer from 64 to 4096" >&2
  exit 1
fi

if [[ -n "${QEMU_VIDEO_SIZE:-}" ]]; then
  if [[ ! "$QEMU_VIDEO_SIZE" =~ ^([0-9]{3,4})x([0-9]{3,4})$ ]]; then
    echo "[qemu-run-full] ERROR: QEMU_VIDEO_SIZE must be WIDTHxHEIGHT" >&2
    exit 1
  fi
  QEMU_VIDEO_ARGS=(-vga none -device "VGA,xres=${BASH_REMATCH[1]},yres=${BASH_REMATCH[2]}")
  QEMU_VIDEO_DETAIL="std VGA, preferred EDID ${QEMU_VIDEO_SIZE}"
fi

prepare_legacy_navigation_keymap() {
  local enabled="${QEMU_LEGACY_NAV_KEYS:-0}"
  local layout="${QEMU_KEYMAP_LAYOUT:-en-us}"
  local source="${QEMU_KEYMAP_SOURCE:-/usr/share/qemu/keymaps/$layout}"
  local output="$ROOT_DIR/build/full/qemu-keymap-${layout}-legacy-nav"
  local output_tmp="${output}.tmp"

  case "$enabled" in
    0)
      return 0
      ;;
    1) ;;
    *)
      echo "[qemu-run-full] ERROR: QEMU_LEGACY_NAV_KEYS must be 0 or 1" >&2
      exit 1
      ;;
  esac

  if [[ ! -f "$source" ]]; then
    echo "[qemu-run-full] ERROR: QEMU keymap source not found: $source" >&2
    echo "[qemu-run-full] set QEMU_KEYMAP_SOURCE or QEMU_LEGACY_NAV_KEYS=0" >&2
    exit 1
  fi

  mkdir -p "$ROOT_DIR/build/full"
  sed -E \
    -e 's/^Home 0xc7$/Home 0x47/' \
    -e 's/^Up 0xc8$/Up 0x48/' \
    -e 's/^Prior 0xc9$/Prior 0x49/' \
    -e 's/^Left 0xcb$/Left 0x4b/' \
    -e 's/^Right 0xcd$/Right 0x4d/' \
    -e 's/^End 0xcf$/End 0x4f/' \
    -e 's/^Down 0xd0$/Down 0x50/' \
    -e 's/^Next 0xd1$/Next 0x51/' \
    -e 's/^Insert 0xd2$/Insert 0x52/' \
    -e 's/^Delete 0xd3$/Delete 0x53/' \
    "$source" > "$output_tmp"
  mv -f -- "$output_tmp" "$output"

  for mapping in \
    'Home 0x47' 'Up 0x48' 'Prior 0x49' 'Left 0x4b' 'Right 0x4d' \
    'End 0x4f' 'Down 0x50' 'Next 0x51' 'Insert 0x52' 'Delete 0x53'; do
    grep -Fxq -- "$mapping" "$output" \
      || { echo "[qemu-run-full] ERROR: generated keymap lacks: $mapping" >&2; exit 1; }
  done

  QEMU_KEYBOARD_ARGS=(-k "$output")
  QEMU_KEYBOARD_DETAIL="legacy-nav keymap=$layout"
}

resolve_display_backend() {
  local backend="$1"

  # SDL/X11 uses QEMU's established relative-mouse grab and remains stable
  # across DOS/Windows video-mode switches on XWayland.  GTK's grab-on-hover
  # path can display the guest while silently losing relative PS/2 movement,
  # so keep GTK as a fallback or an explicit override rather than the default.
  if [[ "$backend" == "auto" ]]; then
    if "$QEMU_CMD" -display help 2>/dev/null | grep -Eq "(^|[[:space:]])sdl([[:space:]]|$)"; then
      echo "sdl,window-close=off"
      return
    fi
    if "$QEMU_CMD" -display help 2>/dev/null | grep -Eq "(^|[[:space:]])gtk([[:space:]]|$)"; then
      echo "gtk,gl=off,window-close=off"
      return
    fi
    echo "[qemu-run-full] ERROR: QEMU provides neither SDL nor GTK display support" >&2
    exit 1
  fi

  if [[ "$backend" == "sdl" ]]; then
    echo "sdl,window-close=off"
    return
  fi

  if [[ "$backend" == "gtk" ]]; then
    echo "gtk,gl=off,window-close=off"
    return
  fi

  echo "$backend"
}

configure_display_environment() {
  local backend="$1"
  local transport="${QEMU_DISPLAY_TRANSPORT:-auto}"
  local display_number=""
  local x11_socket=""

  QEMU_DISPLAY_TRANSPORT_DETAIL="native"
  QEMU_MOUSE_INPUT_DETAIL="${backend%%,*} display relative input"

  case "$transport" in
    auto|x11|native) ;;
    *)
      echo "[qemu-run-full] ERROR: QEMU_DISPLAY_TRANSPORT must be auto, x11 or native" >&2
      exit 1
      ;;
  esac

  # Keep the canonical DOS path on SDL raw-relative input.  Warp-relative mode
  # generates synthetic recentering motion that breaks Costa and DOSNavigator,
  # so override any inherited host hint for every canonical visual run.
  if [[ "$backend" == sdl* ]]; then
    export SDL_MOUSE_RELATIVE_MODE_WARP=0
    QEMU_MOUSE_INPUT_DETAIL="SDL raw-relative input (canonical DOS path)"
  fi

  if [[ "$transport" == "native" ]]; then
    QEMU_DISPLAY_TRANSPORT_DETAIL="native (explicit override)"
    return 0
  fi

  # DOS and Windows 3.x consume relative PS/2 packets. GTK's native Wayland
  # backend can display the guest while failing to deliver a stable relative
  # grab across VGA mode switches. Prefer the verified X11 path even when an
  # inherited desktop variable says GDK_BACKEND=wayland; callers that truly
  # want that path can request QEMU_DISPLAY_TRANSPORT=native explicitly.
  if [[ "${DISPLAY:-}" =~ ^:([0-9]+)(\.[0-9]+)?$ ]]; then
    display_number="${BASH_REMATCH[1]}"
    x11_socket="/tmp/.X11-unix/X${display_number}"
    if [[ -S "$x11_socket" ]]; then
      if [[ "$backend" == gtk* ]]; then
        export GDK_BACKEND=x11
        QEMU_DISPLAY_TRANSPORT_DETAIL="gtk/x11 verified socket=$x11_socket (relative PS/2 grab)"
        return 0
      fi
      if [[ "$backend" == sdl* ]]; then
        export SDL_VIDEODRIVER=x11
        QEMU_DISPLAY_TRANSPORT_DETAIL="sdl/x11 verified socket=$x11_socket (relative PS/2 grab)"
        return 0
      fi
    fi
  fi

  if [[ "$transport" == "x11" || "${XDG_SESSION_TYPE:-}" == "wayland" ]]; then
    echo "[qemu-run-full] ERROR: reliable relative mouse input requires an accessible X11/XWayland socket" >&2
    echo "[qemu-run-full] DISPLAY=${DISPLAY:-unset}; expected /tmp/.X11-unix/X<N>, or explicitly set QEMU_DISPLAY_TRANSPORT=native" >&2
    exit 1
  fi

  QEMU_DISPLAY_TRANSPORT_DETAIL="native (X11 socket not required on this session)"
}

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
    auto)
      if qemu_kvm_available; then
        QEMU_ACCEL_ARGS=(-accel kvm)
        QEMU_ACCEL_DETAIL="kvm (auto)"
      else
        QEMU_ACCEL_ARGS=(-accel 'tcg,one-insn-per-tb=on')
        QEMU_ACCEL_DETAIL="tcg one-insn-per-tb (automatic Wolf3D-safe fallback)"
      fi
      ;;
    kvm)
      if ! qemu_kvm_available; then
        echo "[qemu-run-full] ERROR: hardware acceleration is required but KVM is unavailable" >&2
        echo "[qemu-run-full] enable virtualization and grant access to /dev/kvm, or explicitly use QEMU_ACCEL_MODE=auto for the safe software fallback" >&2
        exit 1
      fi
      QEMU_ACCEL_ARGS=(-accel kvm)
      QEMU_ACCEL_DETAIL="kvm"
      ;;
    tcg)
      QEMU_ACCEL_ARGS=(-accel tcg)
      QEMU_ACCEL_DETAIL="tcg"
      ;;
    tcg-safe)
      QEMU_ACCEL_ARGS=(-accel 'tcg,one-insn-per-tb=on')
      QEMU_ACCEL_DETAIL="tcg one-insn-per-tb (Wolf3D-safe)"
      ;;
    *)
      echo "[qemu-run-full] ERROR: invalid QEMU_ACCEL_MODE=$mode (expected vga-fast, auto, kvm, tcg or tcg-safe)" >&2
      exit 1
      ;;
  esac
}

configure_network_args() {
  local context="$1"
  local mode="${QEMU_NETWORK_MODE:-auto}"
  local host_ftp_port="${QEMU_NET_HOST_FTP_PORT:-8021}"
  local tap_if="${QEMU_NET_TAP_IF:-ciukios0}"
  local netdev_spec="user,id=ciuknet0"
  local passive_port

  QEMU_NETWORK_ARGS=()
  QEMU_NETWORK_DETAIL="off"

  if [[ "$mode" == "auto" ]]; then
    if [[ "$context" == "visual" ]]; then
      mode="user"
    else
      mode="off"
    fi
  fi

  case "$mode" in
    off)
      return 0
      ;;
    user)
      if [[ ! "$host_ftp_port" =~ ^[0-9]+$ ]] || (( host_ftp_port < 0 || host_ftp_port > 65535 )); then
        echo "[qemu-run-full] ERROR: invalid QEMU_NET_HOST_FTP_PORT=$host_ftp_port" >&2
        exit 1
      fi
      if (( host_ftp_port >= 2048 && host_ftp_port <= 2303 )); then
        echo "[qemu-run-full] ERROR: QEMU_NET_HOST_FTP_PORT overlaps passive FTP ports 2048-2303" >&2
        exit 1
      fi
      if (( host_ftp_port > 0 )); then
        netdev_spec+=",hostfwd=tcp:127.0.0.1:${host_ftp_port}-:21"
        for ((passive_port=2048; passive_port<=2303; passive_port++)); do
          netdev_spec+=",hostfwd=tcp:127.0.0.1:${passive_port}-:${passive_port}"
        done
      fi
      QEMU_NETWORK_ARGS=(
        -netdev "$netdev_spec"
        -device "ne2k_isa,netdev=ciuknet0,irq=3,iobase=0x300,mac=52:54:00:12:34:56"
      )
      QEMU_NETWORK_DETAIL="user NAT guest=10.0.2.15 ne2000=irq3/io300"
      if (( host_ftp_port > 0 )); then
        QEMU_NETWORK_DETAIL+=" host-ftp=127.0.0.1:${host_ftp_port}->guest:21 passive=127.0.0.1:2048-2303"
      fi
      ;;
    tap)
      if [[ ! "$tap_if" =~ ^[[:alnum:]_.:-]{1,15}$ ]]; then
        echo "[qemu-run-full] ERROR: invalid QEMU_NET_TAP_IF=$tap_if" >&2
        exit 1
      fi
      if [[ ! -e "/sys/class/net/$tap_if" ]]; then
        echo "[qemu-run-full] ERROR: TAP interface does not exist: $tap_if" >&2
        echo "[qemu-run-full] create it with: scripts/ciukios_tap.sh up" >&2
        exit 1
      fi
      if ! command -v ip >/dev/null 2>&1 \
        || ! ip -d link show dev "$tap_if" 2>/dev/null | grep -Eq 'tun[[:space:]]+type[[:space:]]+tap'; then
        echo "[qemu-run-full] ERROR: interface exists but is not a TAP device: $tap_if" >&2
        exit 1
      fi
      QEMU_NETWORK_ARGS=(
        -netdev "tap,id=ciuknet0,ifname=${tap_if},script=no,downscript=no"
        -device "ne2k_isa,netdev=ciuknet0,irq=3,iobase=0x300,mac=52:54:00:12:34:56"
      )
      QEMU_NETWORK_DETAIL="tap=$tap_if host=${CIUKIOS_TAP_HOST_CIDR:-10.0.2.2/24} guest=configured-by-NETCFG ne2000=irq3/io300 ICMP=inbound"
      ;;
    *)
      echo "[qemu-run-full] ERROR: invalid QEMU_NETWORK_MODE=$mode (expected auto, user, tap or off)" >&2
      exit 1
      ;;
  esac
}

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

  case "$AUDIO_MODE" in
    auto|on|off) ;;
    *)
      echo "[qemu-run-full] ERROR: invalid QEMU_AUDIO_MODE=$AUDIO_MODE (expected auto, on or off)" >&2
      exit 1
      ;;
  esac

  if [[ "$AUDIO_MODE" == "off" ]]; then
    return 0
  fi

  if [[ -n "$requested_backend" ]]; then
    backend="$(normalize_audio_backend "$requested_backend")"
    if ! audio_backend_supported "$backend"; then
      echo "[qemu-run-full] ERROR: unsupported QEMU_AUDIO_BACKEND=$requested_backend for $QEMU_CMD" >&2
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
      echo "[qemu-run-full] ERROR: QEMU_AUDIO_DEVICES must be standard or ac97" >&2
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
        echo "[qemu-run-full] ERROR: missing value for --display" >&2
        exit 1
      fi
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[qemu-run-full] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if ! QEMU_CMD="$(pick_qemu)"; then
  echo "[qemu-run-full] ERROR: QEMU not found (set QEMU_BIN to override)." >&2
  exit 1
fi

if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "[qemu-run-full] build step"
  bash scripts/build_full.sh
fi

IMG="${CIUKIOS_FULL_IMG:-build/full/ciukios-full.img}"
if [[ ! -f "$IMG" ]]; then
  echo "[qemu-run-full] ERROR: image not found: $IMG" >&2
  exit 1
fi

configure_accel_args

if [[ "$MODE" == "test" ]]; then
  configure_audio_args headless
  configure_network_args headless
else
  configure_audio_args visual
  configure_network_args visual
fi

BASE_ARGS=(
  "${QEMU_ACCEL_ARGS[@]}"
  -machine "$QEMU_MACHINE_ARG"
  -cpu "$QEMU_CPU_MODEL"
  -m "$QEMU_MEMORY_MB"
  -drive "file=$IMG,format=raw,if=ide"
  -boot c
)

if [[ "$MODE" == "test" ]]; then
  TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-8}"
  STAGE0_MARKER="${STAGE0_MARKER:-[BOOT0-FULL] CiukiOS full stage0 ready}"
  STAGE1_MARKER="${STAGE1_MARKER:-CiukiOS SHELL}"
  LOG_FILE="${LOG_FILE:-build/full/qemu-full.log}"
  NORMALIZED_LOG="${LOG_FILE}.normalized"
  STDERR_FILE="${STDERR_FILE:-build/full/qemu-full.stderr.log}"
  QEMU_ARGS=(
    "${BASE_ARGS[@]}"
    -nographic
    -chardev "file,id=ser0,path=$LOG_FILE"
    -serial chardev:ser0
    -monitor none
    -no-reboot
    -no-shutdown
    "${QEMU_AUDIO_ARGS[@]}"
    "${QEMU_NETWORK_ARGS[@]}"
  )

  if [[ -n "${QEMU_EXTRA_ARGS:-}" ]]; then
    # shellcheck disable=SC2206
    EXTRA_ARGS=(${QEMU_EXTRA_ARGS})
    QEMU_ARGS+=("${EXTRA_ARGS[@]}")
  fi

  echo "[qemu-run-full] running smoke test with $QEMU_CMD (timeout=${TIMEOUT_SEC}s)"
  echo "[qemu-run-full] resources: 1 x $QEMU_CPU_MODEL, ${QEMU_MEMORY_MB} MiB RAM"
  echo "[qemu-run-full] accelerator: $QEMU_ACCEL_DETAIL"
  echo "[qemu-run-full] audio: $QEMU_AUDIO_DETAIL"
  echo "[qemu-run-full] network: $QEMU_NETWORK_DETAIL"

  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf '[qemu-run-full] dry-run:'
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
    echo "[qemu-run-full] FAIL (qemu exit code: $RC)" >&2
    if [[ -s "$STDERR_FILE" ]]; then
      echo "[qemu-run-full] qemu stderr:" >&2
      tail -n 40 "$STDERR_FILE" >&2 || true
    fi
    tail -n 80 "$LOG_FILE" >&2 || true
    exit "$RC"
  fi

  if ! "$SERIAL_NORMALIZER" "$LOG_FILE" > "$NORMALIZED_LOG"; then
    echo "[qemu-run-full] FAIL (cannot normalize serial log)" >&2
    exit 1
  fi

  if grep -aFq -- "$STAGE0_MARKER" "$NORMALIZED_LOG" && grep -aFq -- "$STAGE1_MARKER" "$NORMALIZED_LOG"; then
    echo "[qemu-run-full] PASS (stage0 and Stage1 readiness markers detected)"
    exit 0
  fi

  echo "[qemu-run-full] FAIL (stage0/Stage1 readiness marker not detected)" >&2
  echo "[qemu-run-full] serial log size: $(wc -c < "$LOG_FILE" 2>/dev/null || echo 0) bytes" >&2
  tail -n 80 "$NORMALIZED_LOG" >&2 || true
  exit 1
fi

prepare_legacy_navigation_keymap
RESOLVED_DISPLAY_BACKEND="$(resolve_display_backend "$DISPLAY_BACKEND")"
configure_display_environment "$RESOLVED_DISPLAY_BACKEND"

QEMU_ARGS=(
  "${BASE_ARGS[@]}"
  "${QEMU_VIDEO_ARGS[@]}"
  -display "$RESOLVED_DISPLAY_BACKEND"
  -name CiukiOS
  -chardev "file,id=ser0,path=$VISUAL_LOG"
  -serial chardev:ser0
  "${QEMU_KEYBOARD_ARGS[@]}"
  "${QEMU_AUDIO_ARGS[@]}"
  "${QEMU_NETWORK_ARGS[@]}"
)

if [[ -n "${QEMU_EXTRA_ARGS:-}" ]]; then
  # shellcheck disable=SC2206
  EXTRA_ARGS=(${QEMU_EXTRA_ARGS})
  QEMU_ARGS+=("${EXTRA_ARGS[@]}")
fi

echo "[qemu-run-full] starting visual QEMU session"
echo "[qemu-run-full] full profile FAT16 baseline boot"
echo "[qemu-run-full] display backend: $RESOLVED_DISPLAY_BACKEND"
echo "[qemu-run-full] display transport: $QEMU_DISPLAY_TRANSPORT_DETAIL"
echo "[qemu-run-full] mouse input path: $QEMU_MOUSE_INPUT_DETAIL"
echo "[qemu-run-full] vga device: $QEMU_VIDEO_DETAIL"
echo "[qemu-run-full] keyboard: $QEMU_KEYBOARD_DETAIL"
echo "[qemu-run-full] resources: 1 x $QEMU_CPU_MODEL, ${QEMU_MEMORY_MB} MiB RAM"
echo "[qemu-run-full] accelerator: $QEMU_ACCEL_DETAIL"
echo "[qemu-run-full] audio: $QEMU_AUDIO_DETAIL"
echo "[qemu-run-full] network: $QEMU_NETWORK_DETAIL"
echo "[qemu-run-full] serial log: $VISUAL_LOG"
echo "[qemu-run-full] guest mouse: standard PS/2 i8042; entering the window captures it; Ctrl+Alt+G releases it"

if [[ "$DRY_RUN" -eq 1 ]]; then
  printf '[qemu-run-full] dry-run:'
  printf ' %q' "$QEMU_CMD" "${QEMU_ARGS[@]}"
  printf '\n'
  exit 0
fi

mkdir -p "$(dirname "$VISUAL_LOG")"
exec "$QEMU_CMD" "${QEMU_ARGS[@]}"
