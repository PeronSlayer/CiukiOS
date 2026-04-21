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
SKIP_BUILD="${CIUKIOS_SKIP_BUILD:-0}"
TRACE_INT="${CIUKIOS_TRACE_INT:-0}"
INCLUDE_FREEDOS="${CIUKIOS_INCLUDE_FREEDOS:-1}"
INCLUDE_OPENGEM="${CIUKIOS_INCLUDE_OPENGEM:-auto}"
INCLUDE_DOOM="${CIUKIOS_INCLUDE_DOOM:-0}"
DOOM_EXE_PATH="${CIUKIOS_DOOM_EXE_PATH:-}"
DOOM_WAD_PATH="${CIUKIOS_DOOM_WAD_PATH:-}"
DOOM_CFG_PATH="${CIUKIOS_DOOM_CFG_PATH:-}"
QEMU_SKIP_RUN="${CIUKIOS_QEMU_SKIP_RUN:-0}"
QEMU_NO_REBOOT="${CIUKIOS_QEMU_NO_REBOOT:-0}"
QEMU_NO_SHUTDOWN="${CIUKIOS_QEMU_NO_SHUTDOWN:-0}"
QEMU_HEADLESS="${CIUKIOS_QEMU_HEADLESS:-0}"
QEMU_BOOT_ORDER="${CIUKIOS_QEMU_BOOT_ORDER:-c}"
QEMU_SERIAL_FILE="${CIUKIOS_QEMU_SERIAL_FILE:-}"
QEMU_DISPLAY_BACKEND="${CIUKIOS_QEMU_DISPLAY_BACKEND:-sdl}"
QEMU_GOP_XRES="${CIUKIOS_QEMU_GOP_XRES:-1920}"
QEMU_GOP_YRES="${CIUKIOS_QEMU_GOP_YRES:-1080}"
QEMU_WINDOW_CENTERED="${CIUKIOS_QEMU_WINDOW_CENTERED:-1}"
FREEDOS_RUNTIME_DIR="$PROJECT_DIR/third_party/freedos/runtime"
OPENGEM_RUNTIME_DIR="$PROJECT_DIR/third_party/freedos/runtime/OPENGEM"

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

if [[ "$SKIP_BUILD" == "1" ]]; then
    echo "[CiukiOS] Build steps skipped (CIUKIOS_SKIP_BUILD=1)"
else
    echo "[CiukiOS] Build kernel + stage2..."
    cd "$PROJECT_DIR"
    make clean
    make

    echo "[CiukiOS] Build loader UEFI..."
    cd "$LOADER_DIR"
    make clean
    make
fi

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

if [[ -f "$BUILD_DIR/CIUKEDIT.COM" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/CIUKEDIT.COM" ::EFI/CiukiOS/CIUKEDIT.COM
    echo "[CiukiOS] CIUKEDIT.COM copied to image"
fi

# Demo text file for CIUKEDIT (Dante, Inferno Canto I). Copied to both
# the FAT root (::DANTE.TXT) and next to the COM binaries
# (::EFI/CiukiOS/DANTE.TXT) so it is reachable regardless of the
# current working directory when CIUKEDIT is launched.
if [[ -f "$PROJECT_DIR/assets/DANTE.TXT" ]]; then
    mcopy -o -i "$IMAGE" "$PROJECT_DIR/assets/DANTE.TXT" ::DANTE.TXT
    mcopy -o -i "$IMAGE" "$PROJECT_DIR/assets/DANTE.TXT" ::EFI/CiukiOS/DANTE.TXT
    echo "[CiukiOS] DANTE.TXT demo copied to image (root + EFI/CiukiOS)"
fi

if [[ -f "$BUILD_DIR/GFXSMK.COM" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/GFXSMK.COM" ::EFI/CiukiOS/GFXSMK.COM
    echo "[CiukiOS] GFXSMK.COM copied to image"
fi

if [[ -f "$BUILD_DIR/DOSMD13.COM" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/DOSMD13.COM" ::EFI/CiukiOS/DOSMD13.COM
    echo "[CiukiOS] DOSMD13.COM copied to image"
fi

if [[ -f "$BUILD_DIR/FADEDMO.COM" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/FADEDMO.COM" ::EFI/CiukiOS/FADEDMO.COM
    echo "[CiukiOS] FADEDMO.COM copied to image"
fi

if [[ -f "$BUILD_DIR/CIUKDEMO.COM" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/CIUKDEMO.COM" ::EFI/CiukiOS/CIUKDEMO.COM
    echo "[CiukiOS] CIUKDEMO.COM copied to image"
fi

if [[ -f "$BUILD_DIR/GFXDOOM.COM" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/GFXDOOM.COM" ::EFI/CiukiOS/GFXDOOM.COM
    echo "[CiukiOS] GFXDOOM.COM copied to image"
fi

if [[ -f "$BUILD_DIR/WADVIEW.COM" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/WADVIEW.COM" ::EFI/CiukiOS/WADVIEW.COM
    echo "[CiukiOS] WADVIEW.COM copied to image"
fi

if [[ -f "$BUILD_DIR/CIUKSMK.COM" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/CIUKSMK.COM" ::EFI/CiukiOS/CIUKSMK.COM
    echo "[CiukiOS] CIUKSMK.COM copied to image"
fi

if [[ -f "$BUILD_DIR/CIUKMSE.COM" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/CIUKMSE.COM" ::EFI/CiukiOS/CIUKMSE.COM
    echo "[CiukiOS] CIUKMSE.COM copied to image"
fi

if [[ -f "$BUILD_DIR/CIUKMZ.EXE" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/CIUKMZ.EXE" ::EFI/CiukiOS/CIUKMZ.EXE
    echo "[CiukiOS] CIUKMZ.EXE copied to image"
fi

if [[ -f "$BUILD_DIR/CIUKPM.EXE" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/CIUKPM.EXE" ::EFI/CiukiOS/CIUKPM.EXE
    echo "[CiukiOS] CIUKPM.EXE copied to image"
fi

if [[ -f "$BUILD_DIR/CIUK4GW.EXE" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/CIUK4GW.EXE" ::EFI/CiukiOS/CIUK4GW.EXE
    echo "[CiukiOS] CIUK4GW.EXE copied to image"
fi

if [[ -f "$BUILD_DIR/CIUKDPM.EXE" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/CIUKDPM.EXE" ::EFI/CiukiOS/CIUKDPM.EXE
    echo "[CiukiOS] CIUKDPM.EXE copied to image"
fi

if [[ -f "$BUILD_DIR/CIUK31.EXE" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/CIUK31.EXE" ::EFI/CiukiOS/CIUK31.EXE
    echo "[CiukiOS] CIUK31.EXE copied to image"
fi

if [[ -f "$BUILD_DIR/CIUK306.EXE" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/CIUK306.EXE" ::EFI/CiukiOS/CIUK306.EXE
    echo "[CiukiOS] CIUK306.EXE copied to image"
fi

if [[ -f "$BUILD_DIR/CIUKLDT.EXE" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/CIUKLDT.EXE" ::EFI/CiukiOS/CIUKLDT.EXE
    echo "[CiukiOS] CIUKLDT.EXE copied to image"
fi

if [[ -f "$BUILD_DIR/CIUKMEM.EXE" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/CIUKMEM.EXE" ::EFI/CiukiOS/CIUKMEM.EXE
    echo "[CiukiOS] CIUKMEM.EXE copied to image"
fi
if [[ -f "$BUILD_DIR/CIUKREL.EXE" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/CIUKREL.EXE" ::EFI/CiukiOS/CIUKREL.EXE
    echo "[CiukiOS] CIUKREL.EXE copied to image"
fi
if [[ -f "$BUILD_DIR/CIUKRMI.EXE" ]]; then
    mcopy -i "$IMAGE" "$BUILD_DIR/CIUKRMI.EXE" ::EFI/CiukiOS/CIUKRMI.EXE
    echo "[CiukiOS] CIUKRMI.EXE copied to image"
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

copy_optional_file() {
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

if [[ "$INCLUDE_DOOM" == "1" ]]; then
    if [[ ! -f "$DOOM_EXE_PATH" ]]; then
        echo "Error: DOOM packaging enabled but executable not found: $DOOM_EXE_PATH" >&2
        exit 1
    fi
    if [[ ! -f "$DOOM_WAD_PATH" ]]; then
        echo "Error: DOOM packaging enabled but WAD not found: $DOOM_WAD_PATH" >&2
        exit 1
    fi

    mcopy -o -i "$IMAGE" "$DOOM_EXE_PATH" ::EFI/CiukiOS/DOOM.EXE
    echo "[CiukiOS] DOOM.EXE copied to image"

    DOOM_WAD_NAME="$(basename "$DOOM_WAD_PATH" | tr '[:lower:]' '[:upper:]')"
    if [[ "$DOOM_WAD_NAME" == "DOOM.WAD" ]]; then
        echo "[CiukiOS] DOOM.WAD alias mapped to DOOM1.WAD"
    fi
    mcopy -o -i "$IMAGE" "$DOOM_WAD_PATH" ::EFI/CiukiOS/DOOM1.WAD
    echo "[CiukiOS] DOOM1.WAD copied to image"

    if [[ -n "$DOOM_CFG_PATH" ]]; then
        if copy_optional_file "$DOOM_CFG_PATH" ::EFI/CiukiOS/DEFAULT.CFG; then
            echo "[CiukiOS] DEFAULT.CFG copied to image"
        fi
    fi

    cat > "$BUILD_DIR/DOOM.BAT" <<'EOF'
run DOOM.EXE
EOF
    mcopy -o -i "$IMAGE" "$BUILD_DIR/DOOM.BAT" ::EFI/CiukiOS/DOOM.BAT
    echo "[CiukiOS] DOOM.BAT generated in image"
fi

# === OpenGEM GUI optional payload ===
OPENGEM_INCLUDED=0
if [[ "$INCLUDE_OPENGEM" == "auto" ]]; then
    if [[ -d "$OPENGEM_RUNTIME_DIR" ]] && find "$OPENGEM_RUNTIME_DIR" -maxdepth 1 -type f | grep -q .; then
        INCLUDE_OPENGEM=1
    else
        INCLUDE_OPENGEM=0
    fi
fi

if [[ "$INCLUDE_OPENGEM" == "1" ]]; then
    if [[ -d "$OPENGEM_RUNTIME_DIR" ]]; then
        # OpenGEM has a deep directory tree — use recursive mcopy
        mmd -i "$IMAGE" ::FREEDOS/OPENGEM 2>/dev/null || true
        mcopy -s -o -i "$IMAGE" "$OPENGEM_RUNTIME_DIR"/* ::FREEDOS/OPENGEM/ 2>/dev/null || true

        # GEMVDI expects the selected SD*.* screen driver at the drive root.
        if [[ -f "$OPENGEM_RUNTIME_DIR/GEMAPPS/GEMSYS/SDPSC9.VGA" ]]; then
            mcopy -o -i "$IMAGE" "$OPENGEM_RUNTIME_DIR/GEMAPPS/GEMSYS/SDPSC9.VGA" ::SDPSC9.VGA
            echo "[CiukiOS] OpenGEM root driver staged: ::SDPSC9.VGA"
        fi

        # GEMVDI also probes ..\GEMBOOT\GEM.EXE before chaining GEM proper.
        if [[ -f "$OPENGEM_RUNTIME_DIR/GEMAPPS/GEMSYS/GEM.EXE" ]]; then
            mmd -i "$IMAGE" ::GEMBOOT 2>/dev/null || true
            mcopy -o -i "$IMAGE" "$OPENGEM_RUNTIME_DIR/GEMAPPS/GEMSYS/GEM.EXE" ::GEMBOOT/GEM.EXE
            echo "[CiukiOS] OpenGEM chain target staged: ::GEMBOOT/GEM.EXE"
        fi

        # GEM.EXE probes C:\GEMBOOT\GEM.RSC (observed via INT21h AH=4E trace).
        # Stage the GEMSYS .RSC payload under ::GEMBOOT to satisfy that path.
        # NOTE: mtools' mmd hangs when re-creating an already-existing directory
        # (no prompt visible but blocks with stdin/stderr redirected). Since the
        # ::GEMBOOT directory was already created in the GEM.EXE staging block
        # above, we skip the redundant mmd here.
        if [[ -f "$OPENGEM_RUNTIME_DIR/GEMAPPS/GEMSYS/GEM.RSC" ]]; then
            mcopy -o -i "$IMAGE" "$OPENGEM_RUNTIME_DIR/GEMAPPS/GEMSYS/GEM.RSC" ::GEMBOOT/GEM.RSC 2>/dev/null || true
            mcopy -o -i "$IMAGE" "$OPENGEM_RUNTIME_DIR/GEMAPPS/GEMSYS/DESKTOP.RSC" ::GEMBOOT/DESKTOP.RSC 2>/dev/null || true
            echo "[CiukiOS] OpenGEM resource staged: ::GEMBOOT/GEM.RSC"
        fi

        OPENGEM_INCLUDED=1
        echo "[CiukiOS] OpenGEM GUI payload copied from: $OPENGEM_RUNTIME_DIR"
    else
        echo "[CiukiOS] OpenGEM GUI files not found (optional): $OPENGEM_RUNTIME_DIR"
    fi
else
    echo "[CiukiOS] OpenGEM GUI integration disabled (CIUKIOS_INCLUDE_OPENGEM=0)"
fi
echo "[CiukiOS] OpenGEM inclusion status: $( [[ $OPENGEM_INCLUDED -eq 1 ]] && echo INCLUDED || echo SKIPPED )"

echo "[CiukiOS] Preparing OVMF_VARS..."
cp "$OVMF_VARS_SRC" "$OVMF_VARS_DST"

if [[ "$QEMU_SKIP_RUN" == "1" ]]; then
    echo "[CiukiOS] QEMU launch skipped (CIUKIOS_QEMU_SKIP_RUN=1)"
    exit 0
fi

echo "[CiukiOS] Starting QEMU..."
QEMU_ARGS=(
  -machine q35
  -m 512M
    -device "virtio-vga,xres=${QEMU_GOP_XRES},yres=${QEMU_GOP_YRES},edid=on"
  -debugcon file:"$BUILD_DIR/debugcon.log"
  -global isa-debugcon.iobase=0xe9
)

QEMU_SERIAL_CAPTURE_VIA_STDIO=0

if [[ -n "$QEMU_SERIAL_FILE" ]]; then
    rm -f "$QEMU_SERIAL_FILE"
    echo "[CiukiOS] QEMU serial sink: file:$QEMU_SERIAL_FILE"
    if [[ "$QEMU_HEADLESS" == "1" ]]; then
        # Keep GOP/VGA available in headless test runs while still capturing
        # serial deterministically through stdout and a tee'd file sink.
        QEMU_SERIAL_CAPTURE_VIA_STDIO=1
        QEMU_ARGS+=( -serial stdio )
    else
        QEMU_ARGS+=( -serial "file:$QEMU_SERIAL_FILE" )
    fi
else
    QEMU_ARGS+=( -serial stdio )
fi

if [[ "$QEMU_NO_REBOOT" == "1" ]]; then
  QEMU_ARGS+=(-no-reboot)
fi
if [[ "$QEMU_NO_SHUTDOWN" == "1" ]]; then
  QEMU_ARGS+=(-no-shutdown)
fi

if [[ "$QEMU_HEADLESS" == "1" ]]; then
    QEMU_ARGS+=(-display none -monitor none)
else
    case "$QEMU_DISPLAY_BACKEND" in
        gtk)
            QEMU_ARGS+=(-display gtk,gl=on,show-tabs=off,show-menubar=off,zoom-to-fit=off,window-close=on)
            ;;
        sdl)
            if [[ "$QEMU_WINDOW_CENTERED" == "1" ]]; then
                export SDL_VIDEO_CENTERED=1
            fi
            QEMU_ARGS+=(-display sdl,gl=on,window-close=on,show-cursor=on)
            ;;
        *)
            echo "Error: unsupported QEMU display backend: $QEMU_DISPLAY_BACKEND" >&2
            exit 1
            ;;
    esac
fi

if [[ "$QEMU_HEADLESS" != "1" ]]; then
    echo "[CiukiOS] QEMU display backend: $QEMU_DISPLAY_BACKEND"
    echo "[CiukiOS] QEMU GOP target: ${QEMU_GOP_XRES}x${QEMU_GOP_YRES}"
    if [[ "$QEMU_DISPLAY_BACKEND" == "sdl" && "$QEMU_WINDOW_CENTERED" == "1" ]]; then
        echo "[CiukiOS] QEMU window centering: enabled"
    fi
fi

if [[ -n "$QEMU_BOOT_ORDER" ]]; then
    QEMU_ARGS+=(-boot "order=${QEMU_BOOT_ORDER}")
fi

if [[ "$TRACE_INT" == "1" ]]; then
  echo "[CiukiOS] QEMU interrupt trace enabled (CIUKIOS_TRACE_INT=1)"
  QEMU_ARGS+=(-d int)
fi

QEMU_CMD=(
    qemu-system-x86_64
    "${QEMU_ARGS[@]}"
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE"
    -drive if=pflash,format=raw,file="$OVMF_VARS_DST"
    -drive format=raw,file="$IMAGE"
)

if [[ "$QEMU_SERIAL_CAPTURE_VIA_STDIO" == "1" ]]; then
        "${QEMU_CMD[@]}" | tee "$QEMU_SERIAL_FILE"
    exit ${PIPESTATUS[0]}
fi

exec "${QEMU_CMD[@]}"
