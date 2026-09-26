#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

usage() {
  cat <<'TXT'
Usage: scripts/build_run_full.sh [--tap] [QEMU options]

Builds the complete FAT16 full profile, verifies its loader/kernel boundary
and any local Windows 3.1 media, then launches the canonical image in QEMU.
The boot-resident INT 33h service and IBM INT 15h/C2 BIOS mouse interface are
shared by DOS/Windows clients; the optional GPL CTMOUSE.EXE replacement is
packaged under SYSTEM\DRIVERS.

Default mode opens the graphical QEMU window.

Useful QEMU options:
  --test               Run the short headless boot smoke instead of the GUI.
  --dry-run            Build and verify, then only print the QEMU command.
  --vga-fast           Use the faster legacy-VGA TCG profile. Do not use it
                       for the local original Doom binary on QEMU 11.1.
  --display <backend>  Select the visual backend (default: auto; SDL/X11 is
                       preferred for reliable DOS/Windows relative mouse input).
  --tap                Use the preconfigured ciukios0 TAP interface.

Environment overrides supported by the QEMU runner include QEMU_BIN,
QEMU_CPU_MODEL, QEMU_MEMORY_MB, QEMU_EXTRA_ARGS, QEMU_ACCEL_MODE (KVM by
default for the widest stable application set; `vga-fast` is opt-in),
QEMU_DISPLAY_TRANSPORT (verified X11/XWayland mouse transport by default),
QEMU_AUDIO_MODE, QEMU_AUDIO_BACKEND,
QEMU_NETWORK_MODE, and QEMU_NET_HOST_FTP_PORT. FTP passive data is forwarded
on 127.0.0.1:2048-2303 in user mode. For host-to-guest ping on Linux, run
scripts/ciukios_tap.sh up-nat and pass --tap.
TXT
}

RUNNER_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --tap)
      export QEMU_NETWORK_MODE=tap
      shift
      ;;
    --vga-fast)
      export QEMU_ACCEL_MODE=vga-fast
      RUNNER_ARGS+=(--vga-fast)
      shift
      ;;
    *)
      RUNNER_ARGS+=("$1")
      shift
      ;;
  esac
done

for command_name in cmp mdir mformat mcopy mmd nasm python3 stat; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "[build-run-full] ERROR: missing required command: $command_name" >&2
    exit 1
  fi
done

if [[ -n "${QEMU_BIN:-}" ]]; then
  if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
    echo "[build-run-full] ERROR: QEMU_BIN is not executable: $QEMU_BIN" >&2
    exit 1
  fi
elif ! command -v qemu-system-i386 >/dev/null 2>&1 \
  && ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
  echo "[build-run-full] ERROR: qemu-system-i386 or qemu-system-x86_64 is required" >&2
  exit 1
fi

if [[ "${CIUKIOS_FETCH_COSTA:-1}" == "1" ]]; then
  echo "[build-run-full] preparing the verified Costa release"
  bash scripts/fetch_costa.sh
fi

if [[ "${CIUKIOS_FETCH_NETWORK:-1}" == "1" ]]; then
  echo "[build-run-full] preparing the verified DOS network stack"
  bash scripts/fetch_network_stack.sh
fi

echo "[build-run-full] 1/4 building the complete FAT16 full profile"
bash scripts/build_full.sh

echo "[build-run-full] 2/4 verifying optional Windows 3.1 integration"
windows31_media_count=0
for disk_number in 01 02 03 04 05 06 07; do
  [[ -f "third_party/windows31/disk${disk_number}.img" ]] \
    && windows31_media_count=$((windows31_media_count + 1))
done
if (( windows31_media_count == 7 )); then
  bash scripts/qemu_test_full_windows31.sh --no-build --prepare-only
else
  echo "[build-run-full] Windows 3.1 local media absent (optional verification skipped)"
fi

echo "[build-run-full] 3/4 verifying the Phase 5 loader/kernel boundary"
bash scripts/verify_phase5_runtime_ownership.sh --no-build

echo "[build-run-full] 4/4 launching CiukiOS in QEMU"
exec bash scripts/qemu_run_full.sh --no-build "${RUNNER_ARGS[@]}"
