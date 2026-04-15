#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/stage2-boot.log"

TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-45}"

mkdir -p "$LOG_DIR"
rm -f "$LOG_FILE"

if [[ ! -x "$RUN_SCRIPT" ]]; then
    echo "[FAIL] run script not found or not executable: $RUN_SCRIPT" >&2
    exit 1
fi

echo "[test-stage2] starting boot (timeout ${TIMEOUT_SECONDS}s)..."
set +e
timeout "${TIMEOUT_SECONDS}s" "$RUN_SCRIPT" > "$LOG_FILE" 2>&1
rc=$?
set -e

if [[ $rc -eq 124 ]]; then
    echo "[test-stage2] timeout reached (expected for QEMU halt loop)"
elif [[ $rc -ne 0 ]]; then
    echo "[FAIL] run_ciukios.sh exited with error (exit=$rc)" >&2
    tail -n 120 "$LOG_FILE" >&2 || true
    exit 1
fi

required_patterns=(
    "stage2.elf loaded into memory"
    "COM catalog ready:"
    "Stage2 ELF loaded, leaving Boot Services"
    "[ stage2 ] scaffolding started"
    "[ ok ] boot_info is valid"
    "[ ok ] handoff v0 is valid"
    "[ ok ] stage2 local gdt+tss is active"
    "[ ok ] stage2 local idt is active"
    "[ ok ] pic remapped and pit started"
    "[ ok ] keyboard ring buffer + set1 decoder ready"
    "[ ok ] interrupts enabled (timer irq0 + keyboard irq1)"
    "[ compat ] INT10h baseline path ready (stage2 video text/gfx)"
    "[ compat ] INT16h baseline path ready (irq1 + key buffer)"
    "[ compat ] INT1Ah baseline path ready (pit tick source)"
    "[ ok ] stage2 mini shell ready (help/pwd/cd/dir/type/copy/ren/move/mkdir/rmdir/attrib/del/ascii/cls/ver/echo/ticks/mem/run/shutdown/reboot)"
    "[ tick ] irq0 #0000000000000001"
    "[ ok ] splashscreen rendered src=0x"
    "[ ok ] disk cache layer is available"
    "[ ok ] FAT layer mounted (rw cache)"
    "[ shell ] mini command loop active"
    "[ stage2 ] next step: handoff to DOS-like runtime"
)

forbidden_patterns=(
    "Invalid Opcode"
    "#UD"
    "Can't find image information"
    "[ tick ] irq0 #0000000000000064"
    "[ panic ]"
)

for pattern in "${required_patterns[@]}"; do
    if ! grep -Fq "$pattern" "$LOG_FILE"; then
        echo "[FAIL] missing required pattern: $pattern" >&2
        tail -n 160 "$LOG_FILE" >&2 || true
        exit 1
    fi
    echo "[OK] found: $pattern"
done

for pattern in "${forbidden_patterns[@]}"; do
    if grep -Fq "$pattern" "$LOG_FILE"; then
        echo "[FAIL] forbidden pattern found: $pattern" >&2
        tail -n 160 "$LOG_FILE" >&2 || true
        exit 1
    fi
    echo "[OK] absent: $pattern"
done

echo "[PASS] stage2 boot test completed"
echo "[INFO] log: $LOG_FILE"
