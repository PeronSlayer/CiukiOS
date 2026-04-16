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
INCLUDE_FREEDOS="${CIUKIOS_INCLUDE_FREEDOS:-1}"
INCLUDE_OZONE="${CIUKIOS_INCLUDE_OZONE:-auto}"
QEMU_NO_REBOOT="${CIUKIOS_QEMU_NO_REBOOT:-1}"
QEMU_NO_SHUTDOWN="${CIUKIOS_QEMU_NO_SHUTDOWN:-1}"
FREEDOS_RUNTIME_DIR="$PROJECT_DIR/third_party/freedos/runtime"
OZONE_RUNTIME_DIR="$PROJECT_DIR/third_party/freedos/runtime/OZONE"

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

copy_freedos_file_if_present() {
    local src="$1"
    local dst="$2"
    if [[ -f "$src" ]]; then
        mcopy -o -i "$IMAGE" "$src" "$dst"
        return 0
    fi
    return 1
}

if [[ "$INCLUDE_FREEDOS" == "1" ]]; then
    echo "[CiukiOS] FreeDOS symbiotic integration enabled (CIUKIOS_INCLUDE_FREEDOS=1)"
    if [[ -d "$FREEDOS_RUNTIME_DIR" ]] && find "$FREEDOS_RUNTIME_DIR" -maxdepth 1 -type f | grep -q .; then
        mmd -i "$IMAGE" ::FREEDOS || true
        while IFS= read -r -d '' file; do
            mcopy -o -i "$IMAGE" "$file" ::FREEDOS/
        done < <(find "$FREEDOS_RUNTIME_DIR" -maxdepth 1 -type f -print0 | sort -z)

        copy_freedos_file_if_present "$FREEDOS_RUNTIME_DIR/COMMAND.COM" ::COMMAND.COM || true
        copy_freedos_file_if_present "$FREEDOS_RUNTIME_DIR/KERNEL.SYS" ::KERNEL.SYS || true
        copy_freedos_file_if_present "$FREEDOS_RUNTIME_DIR/FDCONFIG.SYS" ::FDCONFIG.SYS || true
        copy_freedos_file_if_present "$FREEDOS_RUNTIME_DIR/FDAUTO.BAT" ::AUTOEXEC.BAT || true

        echo "[CiukiOS] FreeDOS bundle copied from: $FREEDOS_RUNTIME_DIR"
    else
        echo "[CiukiOS] FreeDOS runtime not found (optional): $FREEDOS_RUNTIME_DIR"
    fi
else
    echo "[CiukiOS] FreeDOS symbiotic integration disabled (CIUKIOS_INCLUDE_FREEDOS=0)"
fi

# === oZone GUI optional payload ===
OZONE_INCLUDED=0
if [[ "$INCLUDE_OZONE" == "auto" ]]; then
    # Auto-detect: include only if files exist
    if [[ -d "$OZONE_RUNTIME_DIR" ]] && find "$OZONE_RUNTIME_DIR" -maxdepth 1 -type f | grep -q .; then
        INCLUDE_OZONE=1
    else
        INCLUDE_OZONE=0
    fi
fi

if [[ "$INCLUDE_OZONE" == "1" ]]; then
    if [[ -d "$OZONE_RUNTIME_DIR" ]] && find "$OZONE_RUNTIME_DIR" -maxdepth 1 -type f | grep -q .; then
        mmd -i "$IMAGE" ::FREEDOS/OZONE 2>/dev/null || true
        while IFS= read -r -d '' file; do
            mcopy -o -i "$IMAGE" "$file" ::FREEDOS/OZONE/
        done < <(find "$OZONE_RUNTIME_DIR" -maxdepth 1 -type f -print0 | sort -z)
        OZONE_INCLUDED=1
        echo "[CiukiOS] oZone GUI payload copied from: $OZONE_RUNTIME_DIR"
    else
        echo "[CiukiOS] oZone GUI files not found (optional): $OZONE_RUNTIME_DIR"
    fi
else
    echo "[CiukiOS] oZone GUI integration disabled (CIUKIOS_INCLUDE_OZONE=0)"
fi
echo "[CiukiOS] oZone inclusion status: $( [[ $OZONE_INCLUDED -eq 1 ]] && echo INCLUDED || echo SKIPPED )"

echo "[CiukiOS] Preparing OVMF_VARS..."
cp "$OVMF_VARS_SRC" "$OVMF_VARS_DST"

echo "[CiukiOS] Starting QEMU..."
QEMU_ARGS=(
  -machine q35
  -m 512M
  -serial stdio
  -debugcon file:"$BUILD_DIR/debugcon.log"
  -global isa-debugcon.iobase=0xe9
)

if [[ "$QEMU_NO_REBOOT" == "1" ]]; then
  QEMU_ARGS+=(-no-reboot)
fi
if [[ "$QEMU_NO_SHUTDOWN" == "1" ]]; then
  QEMU_ARGS+=(-no-shutdown)
fi

if [[ "$TRACE_INT" == "1" ]]; then
  echo "[CiukiOS] QEMU interrupt trace enabled (CIUKIOS_TRACE_INT=1)"
  QEMU_ARGS+=(-d int)
fi

exec qemu-system-x86_64 \
  "${QEMU_ARGS[@]}" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_DST" \
  -drive format=raw,file="$IMAGE"
