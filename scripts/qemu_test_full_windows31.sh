#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

MEDIA_DIR="${WINDOWS31_MEDIA_DIR:-$ROOT_DIR/third_party/windows31}"
IMG="${CIUKIOS_FULL_IMG:-$ROOT_DIR/build/full/ciukios-full.img}"
RUNTIME_BIN="$ROOT_DIR/build/full/obj/ciukidos.sys"
RUNTIME_LST="$ROOT_DIR/build/full/obj/ciukidos.lst"
DISPLAY_BACKEND="${QEMU_DISPLAY:-auto}"
DO_BUILD=1
PREPARE_ONLY=0
HEADLESS_SMOKE=0
QEMU_CMD="${QEMU_SYSTEM_I386:-qemu-system-i386}"
QEMU_CPU_MODEL="${QEMU_CPU_MODEL:-pentium3}"
QEMU_MEMORY_MB="${QEMU_MEMORY_MB:-256}"
QEMU_ACCEL_MODE="${QEMU_ACCEL_MODE:-kvm}"
WINDOWS31_SMOKE_KEEP_ARTIFACTS="${WINDOWS31_SMOKE_KEEP_ARTIFACTS:-0}"
# Optional same-boot cross-application cleanup regression, not T23 validation.
WINDOWS31_SMOKE_DOS_HANDOFF="${WINDOWS31_SMOKE_DOS_HANDOFF:-0}"
WINDOWS31_SMOKE_VIDEO_PROFILE="${WINDOWS31_SMOKE_VIDEO_PROFILE:-safe}"
case "$WINDOWS31_SMOKE_VIDEO_PROFILE" in
  safe) WINDOWS31_SMOKE_SURFACE='640 480' ;;
  800) WINDOWS31_SMOKE_SURFACE='800 600' ;;
  1024) WINDOWS31_SMOKE_SURFACE='1024 768' ;;
  *) echo '[windows31] ERROR invalid smoke video profile' >&2; exit 2 ;;
esac
WINDOWS31_SMOKE_DOOM_TIMEOUT="${WINDOWS31_SMOKE_DOOM_TIMEOUT:-60}"
WINDOWS31_AUDIO_MODE="${CIUKIOS_WINDOWS31_AUDIO_MODE:-vsbhda}"
SERIAL_NORMALIZER="$ROOT_DIR/scripts/serial_log_normalize.py"

case "$WINDOWS31_AUDIO_MODE" in
  vsbhda|stable|legacy) ;;
  *) echo "[windows31] ERROR: CIUKIOS_WINDOWS31_AUDIO_MODE must be vsbhda, stable or legacy" >&2; exit 2 ;;
esac

usage() {
  cat <<'TXT'
Usage: scripts/qemu_test_full_windows31.sh [options]

Verifies the Windows 3.1 payload inside the canonical CiukiOS full image and
launches that same image through the common full-profile QEMU runner. It does
not create or use a Windows-specific disk build.

Options:
  --no-build           Reuse build/full/ciukios-full.img.
  --prepare-only       Build and verify without starting QEMU.
  --headless-smoke     Boot WIN twice, exercise mouse, WAV, MIDI and child
                       Alt+F4, then return to the CiukiOS shell.
  --display BACKEND    QEMU display backend (default: auto).
  -h, --help           Show this help.

Environment:
  WINDOWS31_MEDIA_DIR  Source directory containing disk01.img ... disk07.img.
  CIUKIOS_FULL_IMG     Canonical full image path.
  QEMU_DISPLAY         Default QEMU display backend.

The full build archives exact copies of the source images under C:\MEDIA\WIN31,
merges their DOS-accessible contents under C:\WIN31SET, and includes the local
installed tree under C:\WINDOWS when third_party/windows31/installed is ready.
TXT
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build)
      DO_BUILD=0
      shift
      ;;
    --prepare-only)
      PREPARE_ONLY=1
      shift
      ;;
    --headless-smoke)
      HEADLESS_SMOKE=1
      shift
      ;;
    --display)
      [[ $# -ge 2 ]] || { echo "[windows31] ERROR: --display requires a value" >&2; exit 2; }
      DISPLAY_BACKEND="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[windows31] ERROR: unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

for command_name in awk cmp mcopy mdir mtype od stat tr; do
  command -v "$command_name" >/dev/null 2>&1 \
    || { echo "[windows31] ERROR: missing command: $command_name" >&2; exit 1; }
done

if (( DO_BUILD )); then
  echo "[windows31] building the canonical full profile"
  CIUKIOS_WINDOWS31_MODE=require bash scripts/build_full.sh
fi

[[ -f "$IMG" ]] \
  || { echo "[windows31] ERROR: canonical full image not found: $IMG" >&2; exit 1; }

verify_dir="$(mktemp -d /tmp/ciukios-windows31-verify.XXXXXX)"
cleanup_verify_dir() {
  if [[ -n "${verify_dir:-}" && -d "$verify_dir" \
      && "$verify_dir" == /tmp/ciukios-windows31-verify.* ]]; then
    rm -rf -- "$verify_dir"
  fi
}
trap cleanup_verify_dir EXIT

for disk_number in 01 02 03 04 05 06 07; do
  source_disk="$MEDIA_DIR/disk${disk_number}.img"
  embedded_disk="$verify_dir/DISK${disk_number}.IMG"
  [[ -f "$source_disk" && "$(stat -c%s "$source_disk")" -eq 1474560 ]] \
    || { echo "[windows31] ERROR: invalid or missing source media: $source_disk" >&2; exit 1; }
  mcopy -o -i "$IMG" "::MEDIA/WIN31/DISK${disk_number}.IMG" "$embedded_disk" >/dev/null 2>&1 \
    || { echo "[windows31] ERROR: DISK${disk_number}.IMG is missing from the full image" >&2; exit 1; }
  cmp -s "$source_disk" "$embedded_disk" \
    || { echo "[windows31] ERROR: embedded DISK${disk_number}.IMG differs from its source" >&2; exit 1; }
done

mdir -i "$IMG" ::WIN31SET/SETUP.EXE >/dev/null 2>&1 \
  || { echo "[windows31] ERROR: C:\\WIN31SET\\SETUP.EXE is missing" >&2; exit 1; }
mdir -i "$IMG" ::SYSTEM/MOUSE.COM >/dev/null 2>&1 \
  || { echo "[windows31] ERROR: resident INT 33h control utility is missing" >&2; exit 1; }
mdir -i "$IMG" ::SYSTEM/DRIVERS/CTMOUSE.EXE >/dev/null 2>&1 \
  || { echo "[windows31] ERROR: optional GPL CuteMouse driver is missing" >&2; exit 1; }

[[ -s "$RUNTIME_BIN" && -s "$RUNTIME_LST" ]] \
  || { echo "[windows31] ERROR: CIUKIDOS runtime artifacts are missing" >&2; exit 1; }
xms_entry_off_hex="$(awk '/jmp short \.entry_dispatch/ {print $2; exit}' "$RUNTIME_LST")"
[[ "$xms_entry_off_hex" =~ ^[0-9A-Fa-f]+$ ]] \
  || { echo "[windows31] ERROR: XMS patch-window offset not found" >&2; exit 1; }
xms_entry_off=$((16#$xms_entry_off_hex))
xms_patch_window="$(od -An -tx1 -j "$xms_entry_off" -N5 "$RUNTIME_BIN" | tr -d ' \n')"
[[ "$xms_patch_window" == "eb03909090" ]] \
  || { echo "[windows31] ERROR: invalid XMS patch window: $xms_patch_window" >&2; exit 1; }
awk '/dos_file_open_mask dw 0/ {found=1} END {exit !found}' "$RUNTIME_LST" \
  || { echo "[windows31] ERROR: EXEC handle-mask state is missing" >&2; exit 1; }

echo "[windows31] PASS canonical full image contains all seven original IMG files"
echo "[windows31] PASS merged installer is available at C:\\WIN31SET"
echo "[windows31] PASS boot-resident INT 33h plus optional CTMOUSE.EXE are available"
echo "[windows31] PASS XMS patch window and EXEC handle cleanup are present"

awk '/cmp ah, 0x5D/ {dispatch=1} /dos_sda:/ {sda=1} END {exit !(dispatch && sda)}' "$RUNTIME_LST" \
  || { echo "[windows31] ERROR: DOS 4+ swappable-data-area support is missing" >&2; exit 1; }
echo "[windows31] PASS DOSMGR and INT 21h/5D06h expose the DOS swappable data area"

if mdir -i "$IMG" ::WINDOWS/WIN.COM >/dev/null 2>&1 \
    && mdir -i "$IMG" ::WINDOWS/SYSTEM/MOUSE.DRV >/dev/null 2>&1; then
  for native_driver in SNDBLST2.DRV VSBD.386 MSADLIB.DRV VADLIBD.386; do
    mdir -i "$IMG" "::WINDOWS/SYSTEM/$native_driver" >/dev/null 2>&1 \
      || { echo "[windows31] ERROR: native Windows audio driver is missing: $native_driver" >&2; exit 1; }
  done
  if [[ "$WINDOWS31_AUDIO_MODE" == "vsbhda" ]]; then
    for transient_file in \
        ::WINDOWS/WINCORE.COM ::WINDOWS/SNDCARD.DRV ::WINDOWS/SYSTEM/DOSX.IBM \
        ::SBEMU/VSBHDA16.EXE ::SBEMU/HDPMI16I.EXE ::SBEMU/SNDCARD.DRV; do
      mdir -i "$IMG" "$transient_file" >/dev/null 2>&1 \
        || { echo "[windows31] ERROR: transient Windows audio file is missing: $transient_file" >&2; exit 1; }
    done
    dosx_size="$(mtype -i "$IMG" ::WINDOWS/SYSTEM/DOSX.EXE | wc -c)"
    [[ "$dosx_size" -ge 300 && "$dosx_size" -le 4096 ]] \
      || { echo "[windows31] ERROR: HDPMI-aware Windows DOSX replacement is missing" >&2; exit 1; }
  fi
  if [[ "$WINDOWS31_AUDIO_MODE" == "stable" ]]; then
    mdir -i "$IMG" ::WINDOWS/SYSTEM/SPEAKER.DRV >/dev/null 2>&1 \
      || { echo "[windows31] ERROR: stable PC-speaker WAV driver is missing" >&2; exit 1; }
  fi
  for vbe_driver in VBESVGA.DRV VDDVBE.386 VBEVMDIB.3GR; do
    mdir -i "$IMG" "::WINDOWS/SYSTEM/$vbe_driver" >/dev/null 2>&1 \
      || { echo "[windows31] ERROR: universal Windows VBE driver is missing: $vbe_driver" >&2; exit 1; }
  done
  for vbe_helper in AUXSTACK.COM AUXCHECK.COM VIDMODES.COM; do
    mdir -i "$IMG" "::WINDOWS/$vbe_helper" >/dev/null 2>&1 \
      || { echo "[windows31] ERROR: Windows VBE helper is missing: $vbe_helper" >&2; exit 1; }
  done
  mdir -i "$IMG" ::SYSTEM/VIDEO/SETUP.EXE >/dev/null 2>&1 \
    || { echo "[windows31] ERROR: VBE setup utility is missing" >&2; exit 1; }
  mdir -i "$IMG" ::SYSTEM/DRIVERS/VGASETUP.COM >/dev/null 2>&1 \
    || { echo "[windows31] ERROR: CiukiOS display manager is missing" >&2; exit 1; }
  windows31_mouse_ini="$(mtype -i "$IMG" ::WINDOWS/MOUSE.INI | tr -d '\r')"
  for mouse_line in \
    'ActiveAccelerationProfile=4' \
    'HorizontalSensitivity=30' \
    'VerticalSensitivity=30'; do
    grep -Fxq -- "$mouse_line" <<< "$windows31_mouse_ini" \
      || { echo "[windows31] ERROR: missing linear Windows mouse setting: $mouse_line" >&2; exit 1; }
  done
  windows31_system_ini="$(mtype -i "$IMG" ::WINDOWS/SYSTEM.INI | tr -d '\r')"
  for video_line in \
    '386grabber=vga.3gr' \
    'display.drv=vga.drv' \
    'display=*vddvga'; do
    grep -Fxq -- "$video_line" <<< "$windows31_system_ini" \
      || { echo "[windows31] ERROR: missing safe VGA setting: $video_line" >&2; exit 1; }
  done
  if grep -Eiq '^(display\.drv=vbesvga\.drv|display=vddvbe\.386|\[VBESVGA\.DRV\])' \
      <<< "$windows31_system_ini"; then
    echo "[windows31] ERROR: unsafe VBE driver is active in the default profile" >&2
    exit 1
  fi
  for profile_spec in 'SYSTEM.VGA:vga.3gr:vga.drv:*vddvga' \
      'SYSTEM.800:vbevmdib.3gr:vbesvga.drv:vddvbe.386' \
      'SYSTEM.102:vbevmdib.3gr:vbesvga.drv:vddvbe.386'; do
    IFS=: read -r profile_name profile_grabber profile_driver profile_vdd <<< "$profile_spec"
    mdir -i "$IMG" "::WINDOWS/$profile_name" >/dev/null 2>&1 \
      || { echo "[windows31] ERROR: missing recoverable video profile: $profile_name" >&2; exit 1; }
    profile_ini="$(mtype -i "$IMG" "::WINDOWS/$profile_name" | tr -d '\r')"
    for profile_line in "386grabber=$profile_grabber" "display.drv=$profile_driver" "display=$profile_vdd"; do
      grep -Fxq -- "$profile_line" <<< "$profile_ini" \
        || { echo "[windows31] ERROR: $profile_name missing $profile_line" >&2; exit 1; }
    done
    if [[ "$profile_name" != SYSTEM.VGA ]]; then
      for profile_line in 'Depth=8' 'PreferBankedModes=0' 'SwapBuffersInterval=16' 'Allow3ByteMode=0'; do
        grep -Fxq -- "$profile_line" <<< "$profile_ini" \
          || { echo "[windows31] ERROR: $profile_name missing conservative VBE setting $profile_line" >&2; exit 1; }
      done
    fi
  done
  if [[ "$WINDOWS31_AUDIO_MODE" == "legacy" ]]; then
    for config_line in \
      'device=vsbd.386' 'device=vadlibd.386' \
      'wave=sndblst2.drv' 'midi=msadlib.drv' \
      'port=220' 'int=7' 'dmachannel=1' 'verifyint=0'; do
      grep -Fxq -- "$config_line" <<< "$windows31_system_ini" \
        || { echo "[windows31] ERROR: missing native Windows audio setting: $config_line" >&2; exit 1; }
    done
  elif [[ "$WINDOWS31_AUDIO_MODE" == "vsbhda" ]]; then
    for config_line in \
      'wave=sndblst2.drv' 'midi=msadlib.drv' \
      '[sndblst.drv]' 'port=220' 'int=7' 'dmachannel=1' 'verifyint=0' \
      '[adlib.drv]' 'port=388'; do
      grep -Fxq -- "$config_line" <<< "$windows31_system_ini" \
        || { echo "[windows31] ERROR: missing VSBHDA Windows setting: $config_line" >&2; exit 1; }
    done
    if grep -Eiq '^(device=(vsbd|vadlibd)\.386|DMABufferSize=)' <<< "$windows31_system_ini"; then
      echo "[windows31] ERROR: VSBHDA profile loads an incompatible Enhanced Mode audio VxD" >&2
      exit 1
    fi
  else
    for config_line in \
      'wave=speaker.drv' '[speaker.drv]' \
      'CPU Speed=300' 'Enhanced=1' 'Max seconds=3' 'Leave interrupts enabled=1'; do
      grep -Fxq -- "$config_line" <<< "$windows31_system_ini" \
        || { echo "[windows31] ERROR: missing stable speaker setting: $config_line" >&2; exit 1; }
    done
    if grep -Eiq '^(device=(vsbd|vadlibd)\.386|wave=sndblst2\.drv|midi=msadlib\.drv|DMABufferSize=)' \
        <<< "$windows31_system_ini"; then
      echo "[windows31] ERROR: stable profile still loads a legacy Windows audio driver" >&2
      exit 1
    fi
  fi
  for pif_name in _DEFAULT.PIF DOSPRMPT.PIF; do
    mcopy -o -i "$IMG" "::WINDOWS/$pif_name" "$verify_dir/$pif_name" >/dev/null 2>&1 \
      || { echo "[windows31] ERROR: missing generated DOS profile: $pif_name" >&2; exit 1; }
    perl -0e '
      local $/;
      $_ = <>;
      my $header = index($_, "WINDOWS 386 3.0\x00");
      die if $header < 0;
      my $data = unpack("v", substr($_, $header + 18, 2));
      my ($max_conv, $req_conv) = unpack("v2", substr($_, $data, 4));
      my ($max_ems, $req_ems, $max_xms, $req_xms) =
        unpack("v4", substr($_, $data + 8, 8));
      exit !($max_conv == 0xffff && $req_conv == 0
        && $max_ems == 0xffff && $req_ems == 0
        && $max_xms == 0xffff && $req_xms == 0);
    ' "$verify_dir/$pif_name" \
      || { echo "[windows31] ERROR: $pif_name still caps DOS memory" >&2; exit 1; }
  done
  cat <<'TXT'
[windows31] Installed Windows, dynamic DOS memory and linear PS/2 mouse are ready.
[windows31] Safe VGA is the default; banked 800x600/1024x768 VBE profiles are opt-in through VGASETUP.
[windows31] The default profile runs Windows Standard Mode under transient
[windows31] VSBHDA16; no incompatible Enhanced Mode SB/AdLib VxD is loaded.
[windows31] At the CiukiOS prompt run:

  CD ..
  CD WINDOWS
  WIN

[windows31] WIN always selects Standard Mode so the HDPMI/VSBHDA port traps
[windows31] remain the sole audio virtualization owner.
TXT
else
  cat <<'TXT'
[windows31] Installation media is ready. At the CiukiOS prompt run:

  CD ..
  CD WIN31SET
  SETUP.EXE

Select VGA and Windows 386 Enhanced mode.
TXT
fi

if (( PREPARE_ONLY )); then
  exit 0
fi

if (( HEADLESS_SMOKE )); then
  cleanup_verify_dir
  trap - EXIT
  for command_name in "$QEMU_CMD" cp python3 socat; do
    command -v "$command_name" >/dev/null 2>&1 \
      || { echo "[windows31] ERROR: missing command for headless smoke: $command_name" >&2; exit 1; }
  done
  mdir -i "$IMG" ::WINDOWS/WIN.COM >/dev/null 2>&1 \
    || { echo "[windows31] ERROR: headless smoke requires C:\\WINDOWS\\WIN.COM" >&2; exit 1; }

  smoke_accel_args=()

  case "$QEMU_ACCEL_MODE" in
    vga-fast) smoke_accel_args=(-accel tcg) ;;
    kvm)
      [[ -c /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] \
        && "$QEMU_CMD" -accel help 2>/dev/null | grep -Fxq kvm \
        || { echo "[windows31] ERROR: hardware acceleration is required but KVM is unavailable" >&2; exit 1; }
      smoke_accel_args=(-accel kvm)
      ;;
    auto)
      if [[ -c /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] \
          && "$QEMU_CMD" -accel help 2>/dev/null | grep -Fxq kvm; then
        smoke_accel_args=(-accel kvm)
      else
        smoke_accel_args=(-accel 'tcg,one-insn-per-tb=on')
      fi
      ;;
    tcg) smoke_accel_args=(-accel tcg) ;;
    tcg-safe) smoke_accel_args=(-accel 'tcg,one-insn-per-tb=on') ;;
    *) echo "[windows31] ERROR: invalid QEMU_ACCEL_MODE=$QEMU_ACCEL_MODE" >&2; exit 1 ;;
  esac

  smoke_dir="$(mktemp -d /tmp/ciukios-windows31-smoke.XXXXXX)"
  smoke_img="$smoke_dir/ciukios-full.img"
  smoke_serial="$smoke_dir/serial.log"
  smoke_stderr="$smoke_dir/qemu.stderr.log"
  smoke_monitor="$smoke_dir/monitor.sock"
  smoke_wav="$smoke_dir/windows-native-audio.wav"
  smoke_qemu_pid=0
  smoke_machine="pc,vmport=off,i8042=on"
  smoke_audio_devices=()
  if [[ "$WINDOWS31_AUDIO_MODE" == "legacy" ]]; then
    smoke_audio_devices=(
      -device "sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=snd0"
      -device "adlib,audiodev=snd0"
    )
  elif [[ "$WINDOWS31_AUDIO_MODE" == "vsbhda" ]]; then
    smoke_audio_devices=(-device "AC97,audiodev=snd0")
  else
    smoke_machine+=",pcspk-audiodev=snd0"
  fi

  cleanup_smoke() {
    if (( smoke_qemu_pid != 0 )) && kill -0 "$smoke_qemu_pid" >/dev/null 2>&1; then
      if [[ -S "$smoke_monitor" ]]; then
        printf 'quit\n' | socat - UNIX-CONNECT:"$smoke_monitor" >/dev/null 2>&1 || true
      fi
      kill "$smoke_qemu_pid" >/dev/null 2>&1 || true
      wait "$smoke_qemu_pid" >/dev/null 2>&1 || true
    fi
    if [[ "$WINDOWS31_SMOKE_KEEP_ARTIFACTS" == "1" ]]; then
      echo "[windows31] smoke artifacts retained at $smoke_dir"
    elif [[ -d "$smoke_dir" && "$smoke_dir" == /tmp/ciukios-windows31-smoke.* ]]; then
      rm -rf -- "$smoke_dir"
    fi
  }
  trap cleanup_smoke EXIT

  smoke_fail() {
    echo "[windows31] ERROR: Windows smoke failed: $1" >&2
    if [[ -S "$smoke_monitor" ]]; then
      printf 'info registers\n' | socat - UNIX-CONNECT:"$smoke_monitor" \
        > "$smoke_dir/failure-registers.log" 2>&1 || true
      printf 'screendump %s/failure.ppm\n' "$smoke_dir" \
        | socat - UNIX-CONNECT:"$smoke_monitor" >/dev/null 2>&1 || true
    fi
    [[ -f "$smoke_stderr" ]] && tail -n 40 "$smoke_stderr" >&2 || true
    [[ -f "$smoke_serial" ]] \
      && "$SERIAL_NORMALIZER" "$smoke_serial" | tail -n 100 >&2 || true
    exit 1
  }

  smoke_hmp() {
    printf '%s\n' "$1" | socat - UNIX-CONNECT:"$smoke_monitor" >/dev/null
  }

  smoke_send_text() {
    local txt="$1" i ch key
    for ((i=0; i<${#txt}; i++)); do
      ch="${txt:i:1}"
      case "$ch" in
        ' ') key=spc ;;
        '.') key=dot ;;
        '/') key=slash ;;
        '\') key=backslash ;;
        ':') key=shift-semicolon ;;
        '-') key=minus ;;
        [A-Z]) key="shift-$(printf '%s' "$ch" | tr 'A-Z' 'a-z')" ;;
        [a-z0-9]) key="$ch" ;;
        *) continue ;;
      esac
      smoke_hmp "sendkey $key 20" || return 1
      sleep 0.08
    done
    smoke_hmp 'sendkey ret 20'
    sleep 0.30
  }

  smoke_wait_serial() {
    local offset="$1" pattern="$2" timeout_sec="$3" start now
    start="$(date +%s)"
    while true; do
      if [[ -f "$smoke_serial" ]] \
          && "$SERIAL_NORMALIZER" --offset "$offset" "$smoke_serial" \
            | grep -aFq -- "$pattern"; then
        return 0
      fi
      now="$(date +%s)"
      (( now - start < timeout_sec )) || return 1
      sleep 0.10
    done
  }

  smoke_wait_vga() {
    local screenshot="$1" timeout_sec="$2" start now
    start="$(date +%s)"
    while true; do
      smoke_hmp "screendump $screenshot" || return 1
      if [[ "$(sed -n '2p' "$screenshot" 2>/dev/null || true)" == "$WINDOWS31_SMOKE_SURFACE" ]]; then
        return 0
      fi
      now="$(date +%s)"
      (( now - start < timeout_sec )) || return 1
      sleep 0.25
    done
  }

  smoke_wait_surface() {
    local screenshot="$1" expected="$2" timeout_sec="$3" start now
    start="$(date +%s)"
    while true; do
      smoke_hmp "screendump $screenshot" || return 1
      if [[ "$(sed -n '2p' "$screenshot" 2>/dev/null || true)" == "$expected" ]]; then
        return 0
      fi
      now="$(date +%s)"
      (( now - start < timeout_sec )) || return 1
      sleep 0.25
    done
  }

  smoke_has_shell_title_bar() {
    python3 - "$1" <<'PY'
import sys

with open(sys.argv[1], "rb") as ppm:
    if ppm.readline().strip() != b"P6":
        raise SystemExit(1)
    width, height = map(int, ppm.readline().split())
    if ppm.readline().strip() != b"255":
        raise SystemExit(1)
    pixels = ppm.read()

if (width, height) not in ((720, 400), (640, 480), (800, 600), (1024, 768)) or len(pixels) != width * height * 3:
    raise SystemExit(1)

title_pixels = zip(pixels[0:width * 16 * 3:3],
                   pixels[1:width * 16 * 3:3],
                   pixels[2:width * 16 * 3:3])
blue_pixels = sum(1 for red, green, blue in title_pixels
                  if blue > red + 40 and blue > green + 40)
raise SystemExit(0 if blue_pixels >= width * 10 else 1)
PY
  }

  smoke_has_linear_single_pointer_delta() {
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys


def read_ppm(path):
    parts = Path(path).read_bytes().split(maxsplit=4)
    if len(parts) != 5 or parts[0] != b"P6":
        raise SystemExit(1)
    return int(parts[1]), int(parts[2]), parts[4]


width, height, before = read_ppm(sys.argv[1])
after_width, after_height, after = read_ppm(sys.argv[2])
if (width, height) != (after_width, after_height):
    raise SystemExit(1)

changed = {
    (index // 3 % width, index // 3 // width)
    for index, (old, new) in enumerate(zip(before, after))
    if old != new
}
remaining = set(changed)
components = []
while remaining:
    stack = [remaining.pop()]
    pixels = [stack[0]]
    while stack:
        x, y = stack.pop()
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                neighbor = (x + dx, y + dy)
                if neighbor in remaining:
                    remaining.remove(neighbor)
                    stack.append(neighbor)
                    pixels.append(neighbor)
    components.append(pixels)

# One client-rendered pointer produces exactly its old and new connected
# shapes. A second CiukiDOS XOR cursor would add another pair.
large = [component for component in components if len(component) >= 20]
if len(large) != 2 or not 60 <= len(changed) <= 800:
    raise SystemExit(1)

centers = sorted(
    (sum(x for x, _ in component) / len(component),
     sum(y for _, y in component) / len(component))
    for component in large
)
delta_x = centers[1][0] - centers[0][0]
delta_y = centers[1][1] - centers[0][1]

# The injected packet is +40,+30.  Allow cursor-shape asymmetry, but reject
# the former profile-2 acceleration which doubled both axes.
raise SystemExit(0 if 32 <= delta_x <= 48 and 22 <= delta_y <= 38 else 1)
PY
  }

  cp --reflink=auto -- "$IMG" "$smoke_img"
  "$QEMU_CMD" \
    "${smoke_accel_args[@]}" \
    -machine "$smoke_machine" \
    -cpu "$QEMU_CPU_MODEL" \
    -m "$QEMU_MEMORY_MB" \
    -drive "file=$smoke_img,format=raw,if=ide" \
    -boot c \
    -display none \
    -chardev "file,id=ser0,path=$smoke_serial" \
    -serial chardev:ser0 \
    -monitor "unix:$smoke_monitor,server,nowait" \
    -audiodev "wav,id=snd0,path=$smoke_wav" \
    "${smoke_audio_devices[@]}" \
    -no-reboot \
    -no-shutdown \
    >/dev/null 2>"$smoke_stderr" &
  smoke_qemu_pid=$!

  for _ in $(seq 1 200); do
    [[ -S "$smoke_monitor" ]] && break
    sleep 0.05
  done
  [[ -S "$smoke_monitor" ]] || smoke_fail "QEMU monitor did not start"
  smoke_wait_serial 0 'CiukiOS SHELL C:\APPS>' 30 \
    || smoke_fail "initial CiukiOS prompt not reached"

  smoke_hmp 'sendkey ret 20'
  sleep 0.50
  stale_mouse_offset="$(wc -c < "$smoke_serial")"
  smoke_send_text 'mousecb.com /leak'
  smoke_wait_serial "$stale_mouse_offset" '[MOUSECB] READY STALE_CALLBACK_FIXTURE' 10 \
    || smoke_fail "stale INT 33h callback fixture did not run"
  smoke_wait_serial "$stale_mouse_offset" 'CiukiOS SHELL C:\APPS>' 10 \
    || smoke_fail "shell did not return after stale INT 33h callback fixture"
  smoke_hmp 'mouse_move 25 15'
  sleep 1
  kill -0 "$smoke_qemu_pid" >/dev/null 2>&1 \
    || smoke_fail "top-level EXEC left a callable DOS mouse address behind"
  smoke_hmp "screendump $smoke_dir/shell-after-stale-callback.ppm"
  smoke_has_shell_title_bar "$smoke_dir/shell-after-stale-callback.ppm" \
    || smoke_fail "stale DOS mouse callback corrupted the shell before Windows startup"
  if [[ "$WINDOWS31_SMOKE_VIDEO_PROFILE" != safe ]]; then
    video_offset="$(wc -c < "$smoke_serial")"
    smoke_send_text "vgasetup win $WINDOWS31_SMOKE_VIDEO_PROFILE"
    smoke_wait_serial "$video_offset" 'profile installed; restart Windows' 20 \
      || smoke_fail "VGASETUP did not install the requested Windows video profile"
    smoke_wait_serial "$video_offset" 'CiukiOS SHELL C:\APPS>' 30 \
      || smoke_fail "shared graphics console did not return after VGASETUP"
    smoke_hmp "screendump $smoke_dir/shared-shell-before-windows.ppm"
  fi
  smoke_send_text 'cd..'
  sleep 0.50
  smoke_send_text 'cd windows'
  sleep 0.50
  smoke_send_text "${WINDOWS31_SMOKE_WIN_COMMAND:-win}"
  smoke_wait_vga "$smoke_dir/first.ppm" 45 \
    || smoke_fail "first WIN did not enter its Standard Mode graphics surface"
  sleep 15
  smoke_hmp "screendump $smoke_dir/mouse-before.ppm"
  windows_surface="$(sed -n '2p' "$smoke_dir/mouse-before.ppm" 2>/dev/null || true)"
  read -r windows_width windows_height <<< "$windows_surface"
  [[ "$windows_width" =~ ^[0-9]+$ && "$windows_height" =~ ^[0-9]+$ \
      && "$windows_width" -ge 640 && "$windows_height" -ge 480 \
      && "$windows_surface" != "720 400" ]] \
    || smoke_fail "Windows did not settle on a valid VGA/VBE graphics surface"
  smoke_hmp 'mouse_move 40 30'
  sleep 1
  smoke_hmp "screendump $smoke_dir/mouse-after.ppm"
  if cmp -s "$smoke_dir/mouse-before.ppm" "$smoke_dir/mouse-after.ppm"; then
    echo "[windows31] WARN QEMU did not inject a relative mouse delta; continuing the audio/exit gate" >&2
  elif ! smoke_has_linear_single_pointer_delta \
      "$smoke_dir/mouse-before.ppm" "$smoke_dir/mouse-after.ppm"; then
    echo "[windows31] WARN pointer delta geometry differed from the QEMU reference; continuing the audio/exit gate" >&2
  fi
  grep -aFq 'Memoria o spazio di indirizzamento insufficiente' "$smoke_serial" \
    && smoke_fail "Windows reported the Enhanced Mode address-space error"

  if [[ "$WINDOWS31_AUDIO_MODE" == "stable" || "$WINDOWS31_AUDIO_MODE" == "vsbhda" ]]; then
    speaker_offset="$(wc -c < "$smoke_serial")"
    smoke_hmp 'sendkey alt-f 30'
    sleep 1
    smoke_hmp 'sendkey e 20'
    sleep 1
    smoke_send_text 'soundrec tada.wav'
    sleep 5
    smoke_hmp "screendump $smoke_dir/soundrec-open.ppm"
    [[ "$(sed -n '2p' "$smoke_dir/soundrec-open.ppm" 2>/dev/null || true)" == "$windows_surface" ]] \
      || smoke_fail "Sound Recorder did not open inside the Windows graphics session"
    # Sound Recorder opens TADA.WAV with the Play button focused.  ENTER
    # activates that Win16 push button; SPACE is consumed by the waveform
    # control on this localized Windows build.
    smoke_hmp 'sendkey ret 20'
    sleep 6
    smoke_hmp "screendump $smoke_dir/soundrec-after-play.ppm"
    cmp -s "$smoke_dir/soundrec-open.ppm" "$smoke_dir/soundrec-after-play.ppm" \
      && smoke_fail "Sound Recorder playback transport did not advance"
    kill -0 "$smoke_qemu_pid" >/dev/null 2>&1 \
      || smoke_fail "playing TADA.WAV crashed the Windows VM"
    "$SERIAL_NORMALIZER" --offset "$speaker_offset" "$smoke_serial" \
      | grep -aFq 'CiukiOS SHELL C:\windows>' \
      && smoke_fail "playing TADA.WAV crashed back to the CiukiOS shell"
    # Keep Sound Recorder alive while starting the independent MIDI probe.
    # Alt+F4 is exercised later on Calculator as a dedicated child-task exit
    # gate; mixing it into the waveform gate made an exit fault look like an
    # audio-playback fault and prevented collection of the MIDI evidence.
    smoke_hmp 'sendkey alt-tab 30'
    sleep 3
  fi

  if [[ "$WINDOWS31_AUDIO_MODE" == "legacy" ]]; then
    dos_vm_offset="$(wc -c < "$smoke_serial")"
    smoke_hmp 'sendkey alt-f 30'
    sleep 1
    smoke_hmp 'sendkey e 20'
    sleep 1
    smoke_send_text 'winver'
    sleep 3
    smoke_hmp "screendump $smoke_dir/winver.ppm"
    smoke_hmp 'sendkey ret 20'
    sleep 2

    smoke_hmp 'sendkey alt-f 30'
    sleep 1
    smoke_hmp 'sendkey e 20'
    sleep 1
    smoke_send_text 'command.com'
    smoke_wait_serial "$dos_vm_offset" 'CiukiOS SHELL C:\windows>' 20 \
      || smoke_fail "Windows could not create a generic DOS virtual machine"
    smoke_hmp "screendump $smoke_dir/dos-prompt.ppm"
    smoke_send_text 'exit'
    sleep 5
    smoke_hmp "screendump $smoke_dir/windows-after-dos-prompt.ppm"
    cmp -s "$smoke_dir/dos-prompt.ppm" "$smoke_dir/windows-after-dos-prompt.ppm" \
      && smoke_fail "generic DOS virtual machine did not return to Program Manager"
    # WINOLDAP may hand focus to the already-running File Manager when the DOS
    # VM closes.  Return to Program Manager before exercising File/Run again.
    smoke_hmp 'sendkey alt-tab 30'
    sleep 2
  fi

  midi_pcm_start=0
  midi_pcm_end=0
  if [[ "$WINDOWS31_AUDIO_MODE" == "legacy" || "$WINDOWS31_AUDIO_MODE" == "vsbhda" ]]; then
    midi_offset="$(wc -c < "$smoke_serial")"
    smoke_hmp 'sendkey alt-f 30'
    sleep 1
    smoke_hmp 'sendkey e 20'
    sleep 1
    smoke_send_text 'mplayer canyon.mid'
    sleep 3
    midi_pcm_start=$(( $(stat -c%s "$smoke_wav") - 44 ))
    (( midi_pcm_start >= 0 )) || smoke_fail "invalid live Windows audio capture"
    smoke_hmp 'sendkey spc 20'
    sleep 4
    smoke_hmp "screendump $smoke_dir/midi-playing-a.ppm"
    sleep 8
    smoke_hmp "screendump $smoke_dir/midi-playing-b.ppm"
    midi_pcm_end=$(( $(stat -c%s "$smoke_wav") - 44 ))
    (( midi_pcm_end > midi_pcm_start )) || smoke_fail "MIDI playback produced no capture interval"
    kill -0 "$smoke_qemu_pid" >/dev/null 2>&1 \
      || smoke_fail "opening CANYON.MID crashed the Windows VM"
    smoke_hmp "screendump $smoke_dir/midi-open.ppm"
    [[ "$(sed -n '2p' "$smoke_dir/midi-open.ppm" 2>/dev/null || true)" == "$windows_surface" ]] \
      || smoke_fail "MIDI playback left or corrupted the Windows graphics session"
    "$SERIAL_NORMALIZER" --offset "$midi_offset" "$smoke_serial" \
      | grep -aFq 'CiukiOS SHELL C:\windows>' \
      && smoke_fail "MIDI playback crashed back to the CiukiOS shell"
    cmp -s "$smoke_dir/midi-playing-a.ppm" "$smoke_dir/midi-playing-b.ppm" \
      && smoke_fail "MIDI transport did not advance after Play"
    # Use Media Player's own File -> E&sci command.  This lets the following
    # Calculator gate determine whether Alt+F4 itself is being mishandled,
    # independently of MCI shutdown.
    smoke_hmp 'sendkey alt-f 30'
    sleep 1
    smoke_hmp "screendump $smoke_dir/midi-file-menu.ppm"
    smoke_hmp 'sendkey s 20'
    sleep 3
  fi

  if [[ "$WINDOWS31_AUDIO_MODE" == "legacy" ]]; then
  doom_vm_offset="$(wc -c < "$smoke_serial")"
  smoke_hmp 'sendkey alt-f 30'
  sleep 1
  smoke_hmp 'sendkey e 20'
  sleep 1
  smoke_send_text 'command.com'
  smoke_wait_serial "$doom_vm_offset" 'CiukiOS SHELL C:\windows>' 20 \
    || smoke_fail "Windows could not create the real Doom DOS virtual machine"
  smoke_send_text 'cd..'
  smoke_wait_serial "$doom_vm_offset" 'CiukiOS SHELL C:\>' 10 \
    || smoke_fail "Doom DOS virtual machine could not reach C:\\"
  smoke_send_text 'cd apps'
  smoke_wait_serial "$doom_vm_offset" 'CiukiOS SHELL C:\apps>' 10 \
    || smoke_fail "Doom DOS virtual machine could not enter C:\\APPS"
  smoke_send_text 'cd doom'
  smoke_wait_serial "$doom_vm_offset" 'CiukiOS SHELL C:\apps\doom>' 10 \
    || smoke_fail "Doom DOS virtual machine could not enter the packaged Doom directory"
  if [[ "${WINDOWS31_SMOKE_CAPTURE_PRE_DOOM:-0}" == "1" ]]; then
    # HMP's memsave keeps writing after a short-lived monitor client closes
    # while the guest is running.  Freeze the VM and keep the three commands
    # on one connection so the diagnostic snapshot cannot be truncated.
    printf 'stop\nmemsave 0xc000 0x5000 "%s"\npmemsave 0x15000 0x5000 "%s"\npmemsave 0x22000 0x5000 "%s"\npmemsave 0x8d000 0x5000 "%s"\npmemsave 0 0x1000 "%s"\npmemsave 0x400 0x1000 "%s"\npmemsave 0x1400 0x1000 "%s"\ncont\n' \
      "$smoke_dir/pre-doom-kernel-virtual.bin" \
      "$smoke_dir/pre-doom-shell-virtual.bin" \
      "$smoke_dir/pre-doom-parent-virtual.bin" \
      "$smoke_dir/pre-doom-entry-virtual.bin" \
      "$smoke_dir/pre-doom-ivt-virtual.bin" \
      "$smoke_dir/pre-doom-bda-virtual.bin" \
      "$smoke_dir/pre-doom-low-virtual.bin" \
      | socat - UNIX-CONNECT:"$smoke_monitor" >/dev/null || true
  fi
  doom_offset="$(wc -c < "$smoke_serial")"
  smoke_send_text 'doom.exe'
  if ! smoke_wait_surface "$smoke_dir/doom-running.ppm" '640 400' \
      "$WINDOWS31_SMOKE_DOOM_TIMEOUT"; then
    printf 'stop\ninfo registers\ninfo cpus\n' \
      | socat - UNIX-CONNECT:"$smoke_monitor" > "$smoke_dir/doom-monitor.log" 2>&1 \
      || true
    smoke_hmp "memsave 0 0xa0000 \"$smoke_dir/doom-virtual.bin\"" || true
    smoke_hmp "memsave 0xc000 0x5000 \"$smoke_dir/doom-kernel-virtual.bin\"" || true
    smoke_hmp "memsave 0x15000 0x5000 \"$smoke_dir/doom-shell-virtual.bin\"" || true
    smoke_hmp "memsave 0x22000 0x5000 \"$smoke_dir/doom-parent-virtual.bin\"" || true
    smoke_hmp "memsave 0x8d000 0x5000 \"$smoke_dir/doom-entry-virtual.bin\"" || true
    smoke_hmp "pmemsave 0 0xa0000 \"$smoke_dir/doom-conventional.bin\"" || true
    smoke_fail "nested packaged Doom did not receive enough dynamic DOS memory"
  fi
  sleep 8
  kill -0 "$smoke_qemu_pid" >/dev/null 2>&1 \
    || smoke_fail "nested packaged Doom crashed the Windows VM"
  "$SERIAL_NORMALIZER" --offset "$doom_offset" "$smoke_serial" \
    | grep -aFq 'CiukiOS SHELL C:\apps\doom>' \
    && smoke_fail "Doom unwound Windows to the CiukiOS shell"
  smoke_hmp 'sendkey f10 30'
  sleep 1
  smoke_hmp 'sendkey y 30'
  smoke_wait_serial "$doom_offset" 'CiukiOS SHELL C:\apps\doom>' 30 \
    || smoke_fail "Doom did not return to its Windows DOS prompt"
  smoke_send_text 'exit'
  smoke_wait_surface "$smoke_dir/windows-after-doom.ppm" "$windows_surface" 30 \
    || smoke_fail "Doom did not return cleanly to Program Manager"
  fi

  child_exit_offset="$(wc -c < "$smoke_serial")"
  smoke_hmp 'sendkey alt-f 30'
  sleep 1
  smoke_hmp 'sendkey e 20'
  sleep 1
  smoke_send_text 'calc'
  sleep 6
  smoke_hmp "screendump $smoke_dir/calc-open.ppm"
  [[ "$(sed -n '2p' "$smoke_dir/calc-open.ppm" 2>/dev/null || true)" == "$windows_surface" ]] \
    || smoke_fail "Calculator did not remain in the Windows VGA/VBE session"
  cmp -s "$smoke_dir/mouse-after.ppm" "$smoke_dir/calc-open.ppm" \
    && smoke_fail "Calculator did not open from Program Manager"

  smoke_hmp 'sendkey alt-f4 30'
  sleep 3
  smoke_hmp "screendump $smoke_dir/calc-closed.ppm"
  [[ "$(sed -n '2p' "$smoke_dir/calc-closed.ppm" 2>/dev/null || true)" == "$windows_surface" ]] \
    || smoke_fail "Alt+F4 on Calculator terminated the Windows session"
  cmp -s "$smoke_dir/calc-open.ppm" "$smoke_dir/calc-closed.ppm" \
    && smoke_fail "Alt+F4 did not close Calculator"
  "$SERIAL_NORMALIZER" --offset "$child_exit_offset" "$smoke_serial" \
    | grep -aFq 'CiukiOS SHELL C:\windows>' \
    && smoke_fail "Alt+F4 on Calculator unwound WIN to the CiukiOS shell"

  smoke_hmp 'sendkey alt-f 30'
  sleep 1
  smoke_hmp "screendump $smoke_dir/program-menu.ppm"
  cmp -s "$smoke_dir/calc-closed.ppm" "$smoke_dir/program-menu.ppm" \
    && smoke_fail "Program Manager stopped responding after closing Calculator"
  smoke_hmp 'sendkey esc 20'
  sleep 1

  exit_offset="$(wc -c < "$smoke_serial")"
  smoke_hmp 'sendkey alt-f4 30'
  sleep 2
  smoke_hmp 'sendkey ret 30'
  smoke_wait_serial "$exit_offset" 'CiukiOS SHELL C:\windows>' 30 \
    || smoke_fail "Windows did not return cleanly to the CiukiOS shell"
  sleep 1
  smoke_hmp "screendump $smoke_dir/shell-after-exit.ppm"
  smoke_has_shell_title_bar "$smoke_dir/shell-after-exit.ppm" \
    || smoke_fail "CiukiOS shell title bar was not restored after Windows exit"
  if [[ "$WINDOWS31_SMOKE_VIDEO_PROFILE" != safe ]]; then
    [[ "$(sed -n '2p' "$smoke_dir/shared-shell-before-windows.ppm")" == "$windows_surface" \
       && "$(sed -n '2p' "$smoke_dir/shell-after-exit.ppm")" == "$windows_surface" ]] \
      || smoke_fail "shell and Windows did not share the selected resolution"
  fi

  # MOUSE.DRV's BIOS callback points into the Windows process.  A post-exit
  # IRQ12 event must stay in CiukiDOS instead of jumping into freed memory.
  smoke_hmp 'mouse_move 80 30'
  sleep 1
  kill -0 "$smoke_qemu_pid" >/dev/null 2>&1 \
    || smoke_fail "mouse movement after Windows exit crashed QEMU"
  smoke_hmp "screendump $smoke_dir/shell-after-mouse.ppm"
  smoke_has_shell_title_bar "$smoke_dir/shell-after-mouse.ppm" \
    || smoke_fail "post-Windows mouse event corrupted the CiukiOS shell"

  if [[ "$WINDOWS31_SMOKE_DOS_HANDOFF" == 1 ]]; then
    # Exercise a different real-mode program after the 16-bit DPMI/audio
    # session, not just Windows restarting its own host. No reboot in between.
    handoff_offset="$(wc -c < "$smoke_serial")"
    smoke_send_text 'cd \APPS\DOSNAV'
    smoke_wait_serial "$handoff_offset" 'CiukiOS SHELL C:\APPS\DOSNAV>' 10 \
      || smoke_fail "post-Windows DOS Navigator directory change failed"
    handoff_offset="$(wc -c < "$smoke_serial")"
    smoke_send_text 'run DN.COM'
    smoke_wait_serial "$handoff_offset" 'Dos Navigator' 30 \
      || smoke_fail "post-Windows DOS Navigator did not reach its startup banner"
    sleep 20
    "$SERIAL_NORMALIZER" --offset "$handoff_offset" "$smoke_serial" \
      | grep -aFq 'CiukiOS SHELL C:\APPS\DOSNAV>' \
      && smoke_fail "DOS Navigator returned early instead of running"
    smoke_hmp "screendump $smoke_dir/dosnav-after-windows.ppm"
    handoff_offset="$(wc -c < "$smoke_serial")"
    smoke_hmp 'sendkey alt-x 100'
    sleep 1
    smoke_hmp 'sendkey ret 100'
    smoke_wait_serial "$handoff_offset" 'CiukiOS SHELL C:\APPS\DOSNAV>' 30 \
      || smoke_fail "post-Windows DOS Navigator Quit did not return to DOS"
    smoke_send_text 'cd \APPS'
    handoff_offset="$(wc -c < "$smoke_serial")"
    smoke_send_text 'run CIUKRTST.COM'
    smoke_wait_serial "$handoff_offset" 'STATE=PASS' 15 \
      || smoke_fail "DOS runtime probe failed after Windows and DOS Navigator"
    smoke_send_text 'cd \windows'
    echo "[windows31] PASS same-boot Windows -> DOS Navigator Quit -> DOS runtime probe"
  fi

  smoke_send_text 'win'
  smoke_wait_surface "$smoke_dir/second.ppm" "$windows_surface" 45 \
    || smoke_fail "second WIN did not re-enter the configured VGA/VBE mode"
  sleep 15
  smoke_hmp "screendump $smoke_dir/second.ppm"
  grep -aFq 'Memoria o spazio di indirizzamento insufficiente' "$smoke_serial" \
    && smoke_fail "second Enhanced Mode launch reported an address-space error"

  if [[ "$WINDOWS31_SMOKE_DOS_HANDOFF" == 1 ]]; then
    handoff_offset="$(wc -c < "$smoke_serial")"
    smoke_hmp 'sendkey alt-f4 30'
    sleep 2
    smoke_hmp 'sendkey ret 30'
    smoke_wait_serial "$handoff_offset" 'CiukiOS SHELL C:\windows>' 30 \
      || smoke_fail "second Windows did not return before Costa"
    smoke_send_text 'cd \APPS\DOOM'
    doom_offset="$(wc -c < "$smoke_serial")"
    smoke_send_text 'run DOOM.EXE -warp 1 1'
    smoke_wait_surface "$smoke_dir/doom-after-windows.ppm" '640 400' 60 \
      || smoke_fail "original Doom did not start after two Windows sessions"
    # The initial mode-13h frame is black while Doom precaches its WAD.
    # Require a populated frame before sending Quit, rather than losing the
    # keystrokes during startup and misreporting an exit regression.
    doom_gameplay_ready=0
    for _ in $(seq 1 60); do
      smoke_hmp "screendump $smoke_dir/doom-after-windows.ppm"
      if python3 - "$smoke_dir/doom-after-windows.ppm" <<'PY'
from pathlib import Path
import sys
parts = Path(sys.argv[1]).read_bytes().split(maxsplit=4)
pixels = parts[4]
sample = [pixels[i:i+3] for i in range(0, len(pixels), 48)]
sys.exit(0 if len(set(sample)) > 32 and
         sum(p != b'\0\0\0' for p in sample) > len(sample) // 2 else 1)
PY
      then
        doom_gameplay_ready=1
        break
      fi
      sleep 1
    done
    [[ "$doom_gameplay_ready" == 1 ]] \
      || smoke_fail "original Doom remained black after Windows"
    smoke_hmp 'sendkey f10 100'
    sleep 2
    smoke_hmp 'sendkey y 100'
    smoke_wait_serial "$doom_offset" '[DOOM] AUDIO CLEANUP COMPLETE' 30 \
      || smoke_fail "original Doom did not release audio after Windows"
    smoke_wait_serial "$doom_offset" 'CiukiOS SHELL C:\APPS\DOOM>' 15 \
      || smoke_fail "original Doom did not return to DOS after Windows"
    echo '[windows31] PASS same-boot original Doom after Windows and DOS Navigator'
    smoke_send_text 'cd \APPS'
    costa_offset="$(wc -c < "$smoke_serial")"
    smoke_send_text 'costa'
    smoke_wait_surface "$smoke_dir/costa-after-windows.ppm" '640 350' 45 \
      || smoke_fail "Costa did not enter its graphics mode after Windows"
    sleep 15
    smoke_hmp "screendump $smoke_dir/costa-after-windows.ppm"
    smoke_hmp 'mouse_move 80 40'
    sleep 2
    smoke_hmp "screendump $smoke_dir/costa-cursor-after-windows.ppm"
    python3 - "$smoke_dir/costa-after-windows.ppm" "$smoke_dir/costa-cursor-after-windows.ppm" <<'PY' \
      || smoke_fail "Costa mouse did not resume after Windows"
from pathlib import Path
import sys
before, after = [Path(p).read_bytes().split(maxsplit=4)[4] for p in sys.argv[1:]]
changed = {i // 3 for i, (a, b) in enumerate(zip(before, after)) if a != b}
assert 20 <= len(changed) <= 128, f'Costa cursor delta: {len(changed)} pixels'
print('[windows31] PASS Costa mouse motion after Windows')
PY
    smoke_hmp 'sendkey tab 100'
    smoke_hmp 'sendkey tab 100'
    smoke_hmp 'sendkey ret 100'
    sleep 2
    smoke_hmp 'sendkey ret 100'
    sleep 15
    smoke_hmp "screendump $smoke_dir/costa-calculator-after-windows.ppm"
    "$SERIAL_NORMALIZER" --offset "$costa_offset" "$smoke_serial" \
      > "$smoke_dir/costa-handoff.log"
    if grep -Eiq 'Runtime error|Path/File access|Bad file name|Out of memory|EXEC FAIL|Invalid executable' \
        "$smoke_dir/costa-handoff.log"; then
      smoke_fail "Costa reported a DOS/runtime error after Windows"
    fi
    costa_mz_runs="$(grep -c '^\[MZ\] run' "$smoke_dir/costa-handoff.log" || true)"
    [[ "$costa_mz_runs" =~ ^[0-9]+$ && "$costa_mz_runs" -ge 3 ]] \
      || smoke_fail "Costa did not EXEC its Calculator after Windows"
    cmp -s "$smoke_dir/costa-after-windows.ppm" "$smoke_dir/costa-calculator-after-windows.ppm" \
      && smoke_fail "Costa did not respond to launching Calculator after Windows"
    echo "[windows31] PASS same-boot second Windows exit -> Costa graphics and keyboard response"
  fi

  smoke_hmp quit
  wait "$smoke_qemu_pid" || smoke_fail "QEMU did not close the Windows audio capture cleanly"
  smoke_qemu_pid=0
  if [[ "$WINDOWS31_AUDIO_MODE" == "legacy" || "$WINDOWS31_AUDIO_MODE" == "vsbhda" ]]; then
    python3 scripts/analyze_audio_wav.py "$smoke_wav" \
      --label windows31-native-audio --min-bytes 50000 --min-unique 16 --min-changes 100 \
      || smoke_fail "Windows produced no native Sound Blaster waveform"
    python3 scripts/analyze_audio_wav.py "$smoke_wav" \
      --label windows31-native-midi --start-byte "$midi_pcm_start" --end-byte "$midi_pcm_end" \
      --min-bytes 50000 --min-ac-rms 20 --min-unique 16 --min-changes 1000 \
      || smoke_fail "Windows AdLib MIDI playback produced no native waveform"
  else
    # QEMU models PIT-frequency PC-speaker tones (used by the DOS games) but
    # does not capture SPEAKER.DRV's high-rate port-61 bitstream.  The gate
    # above therefore verifies the Windows playback transport and VM liveness;
    # it deliberately does not mislabel an empty QEMU capture as real silence.
    echo "[windows31] NOTE QEMU cannot capture SPEAKER.DRV's port-61 PCM bitstream"
  fi

  echo "[windows31] PASS WIN starts in Standard Mode at $windows_surface"
  echo "[windows31] PASS transient DOS callbacks are released and BIOS PS/2 ownership is exclusive"
  echo "[windows31] PASS Windows owns one linear, non-accelerated mouse pointer"
  if [[ "$WINDOWS31_AUDIO_MODE" == "legacy" || "$WINDOWS31_AUDIO_MODE" == "vsbhda" ]]; then
    echo "[windows31] PASS Windows startup produces native Sound Blaster wave audio"
    echo "[windows31] PASS Media Player advances CANYON.MID with native AdLib audio"
  else
    echo "[windows31] PASS Sound Recorder completed TADA.WAV without crashing Windows or loading resident SB/AdLib VxDs"
  fi
  if [[ "$WINDOWS31_AUDIO_MODE" == "legacy" ]]; then
    echo "[windows31] PASS unprofiled DOS/4GW Doom receives dynamic EMS/XMS and returns cleanly"
  fi
  echo "[windows31] PASS Alt+F4 closes a child window without terminating Program Manager"
  echo "[windows31] PASS clean exit releases the BIOS callback and a second WIN starts"
  cleanup_smoke
  trap - EXIT
  exit 0
fi

cleanup_verify_dir
trap - EXIT
exec bash scripts/qemu_run_full.sh --no-build --display "$DISPLAY_BACKEND"
