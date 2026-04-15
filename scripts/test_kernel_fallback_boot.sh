#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/fallback-boot.log"

TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-45}"

mkdir -p "$LOG_DIR"
rm -f "$LOG_FILE"

if [[ ! -x "$RUN_SCRIPT" ]]; then
    echo "[FAIL] run script not found or not executable: $RUN_SCRIPT" >&2
    exit 1
fi

echo "[test-fallback] starting fallback boot (timeout ${TIMEOUT_SECONDS}s)..."
set +e
CIUKIOS_SKIP_STAGE2=1 timeout "${TIMEOUT_SECONDS}s" "$RUN_SCRIPT" > "$LOG_FILE" 2>&1
rc=$?
set -e

if [[ $rc -eq 124 ]]; then
    echo "[test-fallback] timeout reached (expected for QEMU halt loop)"
elif [[ $rc -ne 0 ]]; then
    echo "[FAIL] run_ciukios.sh exited with error (exit=$rc)" >&2
    tail -n 120 "$LOG_FILE" >&2 || true
    exit 1
fi

required_patterns=(
    "stage2.elf not found, falling back to kernel.elf"
    "kernel.elf loaded into memory"
    "Kernel ELF loaded, leaving Boot Services"
    "[ CiukiOS ] kernel started"
    "[ ok ] boot_info is valid"
    "[ ok ] checks passed from custom UEFI loader"
)

forbidden_patterns=(
    "[ stage2 ] scaffolding started"
    "Invalid Opcode"
    "#UD"
    "Can't find image information"
    "[ panic ]"
)

for pattern in "${required_patterns[@]}"; do
    if ! grep -Fq "$pattern" "$LOG_FILE"; then
        echo "[FAIL] missing required pattern: $pattern" >&2
        tail -n 180 "$LOG_FILE" >&2 || true
        exit 1
    fi
    echo "[OK] found: $pattern"
done

for pattern in "${forbidden_patterns[@]}"; do
    if grep -Fq "$pattern" "$LOG_FILE"; then
        echo "[FAIL] forbidden pattern found: $pattern" >&2
        tail -n 180 "$LOG_FILE" >&2 || true
        exit 1
    fi
    echo "[OK] absent: $pattern"
done

echo "[PASS] kernel fallback boot test completed"
echo "[INFO] log: $LOG_FILE"
