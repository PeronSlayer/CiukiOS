#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
LOADER_DIR="$PROJECT_DIR/boot/uefi-loader"

OVMF_CODE="/usr/share/edk2/x64/OVMF_CODE.4m.fd"
OVMF_VARS_SRC="/usr/share/edk2/x64/OVMF_VARS.4m.fd"
OVMF_VARS_DST="$BUILD_DIR/OVMF_VARS.4m.fd"
IMAGE="$BUILD_DIR/ciukios.img"
SKIP_STAGE2="${CIUKIOS_SKIP_STAGE2:-0}"
TRACE_INT="${CIUKIOS_TRACE_INT:-0}"

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Error: missing command: $1" >&2
        exit 1
    }
}

echo "[CiukiOS] Checking dependencies..."
require_cmd make
require_cmd xxd
require_cmd truncate
require_cmd mkfs.vfat
require_cmd mmd
require_cmd mcopy
require_cmd qemu-system-x86_64

if [[ ! -f "$OVMF_CODE" ]]; then
    echo "Error: OVMF firmware not found: $OVMF_CODE" >&2
    exit 1
fi

if [[ ! -f "$OVMF_VARS_SRC" ]]; then
    echo "Error: OVMF_VARS file not found: $OVMF_VARS_SRC" >&2
    exit 1
fi

echo "[CiukiOS] Build kernel + stage2..."
cd "$PROJECT_DIR"
make clean
make

echo "[CiukiOS] Build loader UEFI..."
cd "$LOADER_DIR"
make clean
make

echo "[CiukiOS] Preparing FAT image..."
mkdir -p "$BUILD_DIR"
rm -f "$IMAGE"
truncate -s 64M "$IMAGE"
mkfs.vfat -F 32 "$IMAGE"

mmd -i "$IMAGE" ::EFI
mmd -i "$IMAGE" ::EFI/BOOT
mmd -i "$IMAGE" ::EFI/CiukiOS

mcopy -i "$IMAGE" "$LOADER_DIR/build/BOOTX64.EFI" ::EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMAGE" "$BUILD_DIR/kernel.elf" ::EFI/CiukiOS/kernel.elf
if [[ "$SKIP_STAGE2" == "1" ]]; then
    echo "[CiukiOS] stage2.elf not copied (CIUKIOS_SKIP_STAGE2=1)"
else
    mcopy -i "$IMAGE" "$BUILD_DIR/stage2.elf" ::EFI/CiukiOS/stage2.elf
fi

if [[ -f "$BUILD_DIR/INIT.COM" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/INIT.COM" ::EFI/CiukiOS/INIT.COM
    echo "[CiukiOS] INIT.COM copied to image"
fi

echo "[CiukiOS] Preparing OVMF_VARS..."
cp "$OVMF_VARS_SRC" "$OVMF_VARS_DST"

echo "[CiukiOS] Starting QEMU..."
QEMU_ARGS=(
  -machine q35
  -m 512M
  -serial stdio
  -debugcon file:"$BUILD_DIR/debugcon.log"
  -global isa-debugcon.iobase=0xe9
  -no-reboot
  -no-shutdown
)

if [[ "$TRACE_INT" == "1" ]]; then
  echo "[CiukiOS] QEMU interrupt trace enabled (CIUKIOS_TRACE_INT=1)"
  QEMU_ARGS+=(-d int)
fi

exec qemu-system-x86_64 \
  "${QEMU_ARGS[@]}" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_DST" \
  -drive format=raw,file="$IMAGE"
