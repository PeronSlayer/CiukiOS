#!/usr/bin/env bash
# run_ciukios_macos.sh — CiukiOS launcher per macOS Intel (Homebrew)
# Testato su macOS 15+ (Sequoia/Tahoe) Intel x86_64
# Equivalente a run_ciukios.sh ma con path OVMF, display e toolchain adattati a macOS.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
LOADER_DIR="$PROJECT_DIR/boot/uefi-loader"

# ---------------------------------------------------------------------------
# OVMF — Homebrew qemu include i firmware EDK2 in bundle.
# Prova la variante 4M (più recente) e poi la 2M come fallback.
# ---------------------------------------------------------------------------
BREW_PREFIX="${HOMEBREW_PREFIX:-/usr/local}"   # /usr/local su Intel, /opt/homebrew su Apple Silicon
QEMU_SHARE="$BREW_PREFIX/share/qemu"

# Su macOS molti tool Homebrew (es. mkfs.fat) stanno in sbin,
# che spesso non e' nel PATH delle shell non interattive.
export PATH="$BREW_PREFIX/sbin:$BREW_PREFIX/opt/dosfstools/sbin:$PATH"

find_ovmf_code() {
    for f in \
        "$QEMU_SHARE/edk2-x86_64-code.4m.fd" \
        "$QEMU_SHARE/edk2-x86_64-code.fd" \
        "$BREW_PREFIX/opt/qemu/share/qemu/edk2-x86_64-code.4m.fd" \
        "$BREW_PREFIX/opt/qemu/share/qemu/edk2-x86_64-code.fd"; do
        [[ -f "$f" ]] && echo "$f" && return
    done
}

find_ovmf_vars() {
    for f in \
        "$QEMU_SHARE/edk2-x86_64-vars.4m.fd" \
        "$QEMU_SHARE/edk2-x86_64-vars.fd" \
        "$QEMU_SHARE/edk2-i386-vars.fd" \
        "$BREW_PREFIX/opt/qemu/share/qemu/edk2-x86_64-vars.4m.fd" \
        "$BREW_PREFIX/opt/qemu/share/qemu/edk2-x86_64-vars.fd" \
        "$BREW_PREFIX/opt/qemu/share/qemu/edk2-i386-vars.fd"; do
        [[ -f "$f" ]] && echo "$f" && return
    done
}

OVMF_CODE="${CIUKIOS_OVMF_CODE:-$(find_ovmf_code 2>/dev/null || true)}"
OVMF_VARS_SRC="${CIUKIOS_OVMF_VARS:-$(find_ovmf_vars 2>/dev/null || true)}"
OVMF_VARS_DST="$BUILD_DIR/OVMF_VARS.4m.fd"
IMAGE="$BUILD_DIR/ciukios.img"

# ---------------------------------------------------------------------------
# Variabili di controllo (stesse interfacce di run_ciukios.sh)
# ---------------------------------------------------------------------------
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
# Su macOS il backend nativo è "cocoa"; SDL è disponibile ma richiede brew install sdl2
QEMU_DISPLAY_BACKEND="${CIUKIOS_QEMU_DISPLAY_BACKEND:-cocoa}"
QEMU_GOP_XRES="${CIUKIOS_QEMU_GOP_XRES:-1920}"
QEMU_GOP_YRES="${CIUKIOS_QEMU_GOP_YRES:-1080}"
QEMU_WINDOW_CENTERED="${CIUKIOS_QEMU_WINDOW_CENTERED:-1}"
FREEDOS_RUNTIME_DIR="$PROJECT_DIR/third_party/freedos/runtime"
OPENGEM_RUNTIME_DIR="$PROJECT_DIR/third_party/freedos/runtime/OPENGEM"

# ---------------------------------------------------------------------------
# Helper
# ---------------------------------------------------------------------------
require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Error: missing command: $1" >&2
        echo "  → Installa con: $2" >&2
        exit 1
    }
}

echo "[CiukiOS] Checking dependencies..."

# Tool runtime obbligatori
require_cmd make          "xcode-select --install"
require_cmd xxd           "xcode-select --install  (incluso in macOS)"
require_cmd truncate      "(incluso in macOS)"
require_cmd mkfs.fat      "brew install dosfstools"
require_cmd mmd           "brew install mtools"
require_cmd mcopy         "brew install mtools"
require_cmd qemu-system-x86_64 "brew install qemu"

# mkfs.vfat è un alias di mkfs.fat su macOS/Homebrew; lo creiamo dinamicamente
# se non esiste (alcuni sistemi lo linkano, altri no)
if ! command -v mkfs.vfat >/dev/null 2>&1; then
    MKFS_VFAT="mkfs.fat"
    echo "[CiukiOS] mkfs.vfat non trovato, uso mkfs.fat (dosfstools)"
else
    MKFS_VFAT="mkfs.vfat"
fi

# Tool di build — clang/lld/llvm-objcopy vengono da Homebrew llvm
# su macOS il clang di sistema NON include ld.lld né llvm-objcopy.
# Aggiungi /usr/local/opt/llvm/bin al PATH se necessario.
LLVM_BIN="$BREW_PREFIX/opt/llvm/bin"
if [[ -d "$LLVM_BIN" ]]; then
    export PATH="$LLVM_BIN:$PATH"
fi

# Per il loader UEFI servono ld.bfd e objcopy (binutils GNU).
# Homebrew: brew install x86_64-elf-binutils  oppure  brew install binutils
BINUTILS_BIN="$BREW_PREFIX/opt/x86_64-elf-binutils/bin"
if [[ -d "$BINUTILS_BIN" ]]; then
    export PATH="$BINUTILS_BIN:$PATH"
fi
# Prova anche binutils generico
BINUTILS_GENERIC="$BREW_PREFIX/opt/binutils/bin"
if [[ -d "$BINUTILS_GENERIC" ]]; then
    export PATH="$BINUTILS_GENERIC:$PATH"
fi

# Verifica OVMF
if [[ -z "$OVMF_CODE" ]] || [[ ! -f "$OVMF_CODE" ]]; then
    echo "Error: OVMF firmware (code) non trovato." >&2
    echo "  Assicurati di avere: brew install qemu" >&2
    echo "  Oppure imposta: export CIUKIOS_OVMF_CODE=/path/to/edk2-x86_64-code.fd" >&2
    exit 1
fi
if [[ -z "$OVMF_VARS_SRC" ]] || [[ ! -f "$OVMF_VARS_SRC" ]]; then
    echo "Error: OVMF firmware (vars) non trovato." >&2
    echo "  Oppure imposta: export CIUKIOS_OVMF_VARS=/path/to/edk2-x86_64-vars.fd" >&2
    exit 1
fi

echo "[CiukiOS] OVMF CODE: $OVMF_CODE"
echo "[CiukiOS] OVMF VARS: $OVMF_VARS_SRC"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if [[ "$SKIP_BUILD" == "1" ]]; then
    echo "[CiukiOS] Build steps skipped (CIUKIOS_SKIP_BUILD=1)"
else
    echo "[CiukiOS] Build kernel + stage2..."
    cd "$PROJECT_DIR"
    make clean
    make \
        CC='clang -target x86_64-unknown-none-elf' \
        AS='clang -target x86_64-unknown-none-elf' \
        LD=ld.lld

    echo "[CiukiOS] Build loader UEFI..."
    cd "$LOADER_DIR"
    make clean
    make \
        CC=x86_64-elf-gcc \
        EFI_INC=/usr/local/include/efi \
        EFI_ARCH_INC=/usr/local/include/efi/x86_64 \
        EFI_LIB=/usr/local/lib \
        EFI_GNUEFI_LIB=/usr/local/lib \
        LD=x86_64-elf-ld \
        OBJCOPY=x86_64-elf-objcopy
fi

# ---------------------------------------------------------------------------
# Preparazione immagine FAT
# ---------------------------------------------------------------------------
echo "[CiukiOS] Preparing FAT image..."
mkdir -p "$BUILD_DIR"
rm -f "$IMAGE"
# truncate su macOS (BSD) supporta -s con suffissi numerici ma NON "64M" — usa byte espliciti
truncate -s $((64 * 1024 * 1024)) "$IMAGE"
$MKFS_VFAT -F 32 "$IMAGE"

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

# COM/EXE opzionali
for bin in \
    INIT.COM CIUKEDIT.COM GFXSMK.COM DOSMD13.COM FADEDMO.COM \
    CIUKDEMO.COM GFXDOOM.COM WADVIEW.COM CIUKSMK.COM CIUKMSE.COM \
    CIUKMZ.EXE CIUKPM.EXE CIUK4GW.EXE CIUKDPM.EXE \
    CIUK31.EXE CIUK306.EXE CIUKLDT.EXE CIUKMEM.EXE \
    CIUKREL.EXE CIUKRMI.EXE; do
    if [[ -f "$BUILD_DIR/$bin" ]]; then
        mcopy -i "$IMAGE" "$BUILD_DIR/$bin" "::EFI/CiukiOS/$bin"
        echo "[CiukiOS] $bin copied to image"
    fi
done

# File demo Dante
if [[ -f "$PROJECT_DIR/assets/DANTE.TXT" ]]; then
    mcopy -o -i "$IMAGE" "$PROJECT_DIR/assets/DANTE.TXT" ::DANTE.TXT
    mcopy -o -i "$IMAGE" "$PROJECT_DIR/assets/DANTE.TXT" ::EFI/CiukiOS/DANTE.TXT
    echo "[CiukiOS] DANTE.TXT demo copied to image (root + EFI/CiukiOS)"
fi

# ---------------------------------------------------------------------------
# FreeDOS
# ---------------------------------------------------------------------------
copy_freedos_file_if_present() {
    local src="$1" dst="$2"
    if [[ -f "$src" ]]; then mcopy -o -i "$IMAGE" "$src" "$dst"; return 0; fi
    return 1
}
copy_optional_file() {
    local src="$1" dst="$2"
    if [[ -f "$src" ]]; then mcopy -o -i "$IMAGE" "$src" "$dst"; return 0; fi
    return 1
}

if [[ "$INCLUDE_FREEDOS" == "1" ]]; then
    echo "[CiukiOS] FreeDOS symbiotic integration enabled"
    if [[ -d "$FREEDOS_RUNTIME_DIR" ]] && find "$FREEDOS_RUNTIME_DIR" -maxdepth 1 -type f | grep -q .; then
        mmd -i "$IMAGE" ::FREEDOS || true
        while IFS= read -r -d '' file; do
            mcopy -o -i "$IMAGE" "$file" ::FREEDOS/
        done < <(find "$FREEDOS_RUNTIME_DIR" -maxdepth 1 -type f -print0 | sort -z)

        copy_freedos_file_if_present "$FREEDOS_RUNTIME_DIR/COMMAND.COM" ::COMMAND.COM  || true
        copy_freedos_file_if_present "$FREEDOS_RUNTIME_DIR/KERNEL.SYS"  ::KERNEL.SYS   || true
        copy_freedos_file_if_present "$FREEDOS_RUNTIME_DIR/FDCONFIG.SYS" ::FDCONFIG.SYS || true
        copy_freedos_file_if_present "$FREEDOS_RUNTIME_DIR/FDAUTO.BAT"  ::AUTOEXEC.BAT || true

        echo "[CiukiOS] FreeDOS bundle copied from: $FREEDOS_RUNTIME_DIR"
    else
        echo "[CiukiOS] FreeDOS runtime not found (optional): $FREEDOS_RUNTIME_DIR"
    fi
else
    echo "[CiukiOS] FreeDOS symbiotic integration disabled"
fi

# ---------------------------------------------------------------------------
# DOOM (opzionale)
# ---------------------------------------------------------------------------
if [[ "$INCLUDE_DOOM" == "1" ]]; then
    if [[ ! -f "$DOOM_EXE_PATH" ]]; then
        echo "Error: DOOM packaging abilitato ma EXE non trovato: $DOOM_EXE_PATH" >&2; exit 1
    fi
    if [[ ! -f "$DOOM_WAD_PATH" ]]; then
        echo "Error: DOOM packaging abilitato ma WAD non trovato: $DOOM_WAD_PATH" >&2; exit 1
    fi
    mcopy -o -i "$IMAGE" "$DOOM_EXE_PATH" ::EFI/CiukiOS/DOOM.EXE
    echo "[CiukiOS] DOOM.EXE copied to image"
    DOOM_WAD_NAME="$(basename "$DOOM_WAD_PATH" | tr '[:lower:]' '[:upper:]')"
    [[ "$DOOM_WAD_NAME" == "DOOM.WAD" ]] && echo "[CiukiOS] DOOM.WAD alias → DOOM1.WAD"
    mcopy -o -i "$IMAGE" "$DOOM_WAD_PATH" ::EFI/CiukiOS/DOOM1.WAD
    echo "[CiukiOS] DOOM1.WAD copied to image"
    if [[ -n "$DOOM_CFG_PATH" ]]; then
        copy_optional_file "$DOOM_CFG_PATH" ::EFI/CiukiOS/DEFAULT.CFG && echo "[CiukiOS] DEFAULT.CFG copied"
    fi
    printf 'run DOOM.EXE\n' > "$BUILD_DIR/DOOM.BAT"
    mcopy -o -i "$IMAGE" "$BUILD_DIR/DOOM.BAT" ::EFI/CiukiOS/DOOM.BAT
    echo "[CiukiOS] DOOM.BAT generated in image"
fi

# ---------------------------------------------------------------------------
# OpenGEM (opzionale)
# ---------------------------------------------------------------------------
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
        mmd -i "$IMAGE" ::FREEDOS/OPENGEM 2>/dev/null || true
        mcopy -s -o -i "$IMAGE" "$OPENGEM_RUNTIME_DIR"/* ::FREEDOS/OPENGEM/ 2>/dev/null || true
        OPENGEM_INCLUDED=1
        echo "[CiukiOS] OpenGEM payload copied from: $OPENGEM_RUNTIME_DIR"
    else
        echo "[CiukiOS] OpenGEM files not found (optional)"
    fi
else
    echo "[CiukiOS] OpenGEM integration disabled"
fi
echo "[CiukiOS] OpenGEM: $( [[ $OPENGEM_INCLUDED -eq 1 ]] && echo INCLUDED || echo SKIPPED )"

# ---------------------------------------------------------------------------
# OVMF vars
# ---------------------------------------------------------------------------
echo "[CiukiOS] Preparing OVMF_VARS..."
cp "$OVMF_VARS_SRC" "$OVMF_VARS_DST"

if [[ "$QEMU_SKIP_RUN" == "1" ]]; then
    echo "[CiukiOS] QEMU launch skipped (CIUKIOS_QEMU_SKIP_RUN=1)"
    exit 0
fi

# ---------------------------------------------------------------------------
# QEMU
# ---------------------------------------------------------------------------
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
        QEMU_SERIAL_CAPTURE_VIA_STDIO=1
        QEMU_ARGS+=( -serial stdio )
    else
        QEMU_ARGS+=( -serial "file:$QEMU_SERIAL_FILE" )
    fi
else
    QEMU_ARGS+=( -serial stdio )
fi

[[ "$QEMU_NO_REBOOT"   == "1" ]] && QEMU_ARGS+=(-no-reboot)
[[ "$QEMU_NO_SHUTDOWN" == "1" ]] && QEMU_ARGS+=(-no-shutdown)

if [[ "$QEMU_HEADLESS" == "1" ]]; then
    QEMU_ARGS+=(-display none -monitor none)
else
    case "$QEMU_DISPLAY_BACKEND" in
        cocoa)
            # Backend nativo macOS — non richiede dipendenze extra
            if [[ "$QEMU_WINDOW_CENTERED" == "1" ]]; then
                # cocoa non supporta SDL_VIDEO_CENTERED; la finestra appare centrata di default
                : # noop
            fi
            QEMU_ARGS+=(-display cocoa,show-cursor=on,zoom-to-fit=off)
            ;;
        sdl)
            # Richiede: brew install sdl2
            [[ "$QEMU_WINDOW_CENTERED" == "1" ]] && export SDL_VIDEO_CENTERED=1
            QEMU_ARGS+=(-display sdl,gl=on,window-close=on,show-cursor=on)
            ;;
        gtk)
            # Richiede: brew install gtk+3  e  XQuartz per l'accelerazione GL
            QEMU_ARGS+=(-display gtk,gl=off,show-tabs=off,show-menubar=off,zoom-to-fit=off,window-close=on)
            ;;
        *)
            echo "Error: backend display non supportato: $QEMU_DISPLAY_BACKEND" >&2
            exit 1
            ;;
    esac
    echo "[CiukiOS] QEMU display backend: $QEMU_DISPLAY_BACKEND"
    echo "[CiukiOS] QEMU GOP target: ${QEMU_GOP_XRES}x${QEMU_GOP_YRES}"
fi

[[ -n "$QEMU_BOOT_ORDER" ]] && QEMU_ARGS+=(-boot "order=${QEMU_BOOT_ORDER}")

if [[ "$TRACE_INT" == "1" ]]; then
    echo "[CiukiOS] QEMU interrupt trace enabled"
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
