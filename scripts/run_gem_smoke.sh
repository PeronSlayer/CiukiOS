#!/usr/bin/env bash
# run_gem_smoke.sh -- build, stage, boot CiukiOS in QEMU with the GEM
# desktop chain (gem -> GEMVDI.EXE -> GEM.EXE), then dump a diagnostic
# summary from the serial log.
#
# Usage:
#   scripts/run_gem_smoke.sh           # interactive QEMU window, no timeout
#   scripts/run_gem_smoke.sh --headless [--seconds N]
#                                      # no display, auto-kill after N seconds
#                                      # (default 45)
#   scripts/run_gem_smoke.sh --autoexec "gem vdi"
#                                      # override AUTOEXEC.BAT command
#
# Requires: make, mtools (mcopy), qemu-system-x86_64, OVMF (edk2-ovmf).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

HEADLESS=0
SECONDS_TIMEOUT=45
AUTOEXEC_CMD="gem"
OVMF_CODE="/usr/share/edk2/x64/OVMF_CODE.4m.fd"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --headless)   HEADLESS=1; shift ;;
        --seconds)    SECONDS_TIMEOUT="$2"; shift 2 ;;
        --autoexec)   AUTOEXEC_CMD="$2"; shift 2 ;;
        --ovmf-code)  OVMF_CODE="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,20p' "$0"; exit 0 ;;
        *)
            echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

SERIAL_LOG="build/serial-gem.log"
DEBUG_LOG="/tmp/qemu-debug.log"
IMG="build/ciukios.img"
OVMF_VARS="build/OVMF_VARS.4m.fd"

echo "[1/4] Building CiukiOS..."
make >/dev/null

if [[ ! -f "$OVMF_CODE" ]]; then
    echo "ERROR: OVMF firmware not found at $OVMF_CODE" >&2
    echo "Install edk2-ovmf or pass --ovmf-code <path>." >&2
    exit 3
fi

echo "[2/4] Staging stage2.elf and AUTOEXEC.BAT (cmd: '$AUTOEXEC_CMD')..."
mcopy -o -i "$IMG" build/stage2.elf ::EFI/CiukiOS/stage2.elf
TMP_AE="$(mktemp --suffix=.bat)"
trap 'rm -f "$TMP_AE"' EXIT
printf '%s\r\n' "$AUTOEXEC_CMD" > "$TMP_AE"
mcopy -o -i "$IMG" "$TMP_AE" ::AUTOEXEC.BAT

# Truncate previous logs for a clean diagnostic.
: > "$SERIAL_LOG"
: > "$DEBUG_LOG"

QEMU_ARGS=(
    -machine q35 -m 512M
    -device virtio-vga
    -serial "file:$SERIAL_LOG"
    -debugcon "file:$DEBUG_LOG"
    -no-reboot -no-shutdown
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,file=$OVMF_VARS"
    -drive "format=raw,file=$IMG"
    -boot order=c
)

if [[ "$HEADLESS" -eq 1 ]]; then
    echo "[3/4] Running QEMU headless for ${SECONDS_TIMEOUT}s..."
    QEMU_ARGS+=(-display none -monitor none)
    timeout --foreground "${SECONDS_TIMEOUT}s" \
        qemu-system-x86_64 "${QEMU_ARGS[@]}" || true
else
    echo "[3/4] Launching QEMU window."
    echo "      Watch the QEMU window. Move the mouse, press keys."
    echo "      Close the window when you're done to continue."
    qemu-system-x86_64 "${QEMU_ARGS[@]}" || true
fi

echo
echo "[4/4] Diagnostic summary"
echo "================================================================"
echo "serial log: $SERIAL_LOG ($(wc -l < "$SERIAL_LOG") lines)"
echo "debug  log: $DEBUG_LOG ($(wc -l < "$DEBUG_LOG") lines)"
echo
echo "--- top VDI opcodes (INT 0xEF) ---"
grep -oE "vec=EF op=0x[0-9A-F]+" "$SERIAL_LOG" 2>/dev/null \
    | sort | uniq -c | sort -rn | head -15 || echo "(none)"
echo
echo "--- top AES opcodes ---"
grep -oE "aes=0x[0-9A-F]+" "$SERIAL_LOG" 2>/dev/null \
    | sort | uniq -c | sort -rn | head -15 || echo "(none)"
echo
echo "--- unhandled INT 21h ---"
grep "int21 UNHANDLED" "$SERIAL_LOG" 2>/dev/null | sort -u | head -10 \
    || echo "(none)"
echo
echo "--- GEM chain markers ---"
grep -E "\[gem\]|chain|GEMVDI|GEM\.EXE" "$SERIAL_LOG" 2>/dev/null | head -20 \
    || echo "(none)"
echo
echo "--- last 40 lines of serial log ---"
tail -40 "$SERIAL_LOG" 2>/dev/null || true
echo "================================================================"
echo "Done. Full logs: $SERIAL_LOG  $DEBUG_LOG"
